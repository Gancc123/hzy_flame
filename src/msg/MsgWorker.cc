#include "MsgWorker.h"
#include "internal/errno.h"

#include <string>
#include <chrono>
#include <cassert>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>


namespace flame{

void MsgWorkerThread::entry(){
    worker->process();
    worker->drain();
}


void HandleNotifyCallBack::read_cb(){
    char c[256];
    int r = 0;
    do {
        r = read(fd, c, sizeof(c));
        if (r < 0) {
            if (errno != EAGAIN){
                ML(fct, error, "read notify pipe failed: {}",
                                                        cpp_strerror(errno));
            }
        }
    } while (r > 0);
}


int MsgWorker::set_nonblock(int sd){
    int flags;
    int r = 0;

    /* Set the socket nonblocking.
    * Note that fcntl(2) for F_GETFL and F_SETFL can't be
    * interrupted by a signal. */
    if ((flags = fcntl(sd, F_GETFL)) < 0 ) {
        r = errno;
        ML(fct, error, "{} fcntl(F_GETFL) failed: {}", this->name, 
                                                        cpp_strerror(r));
        return -r;
    }
    if (fcntl(sd, F_SETFL, flags | O_NONBLOCK) < 0) {
        r = errno;
        ML(fct,  error, "{} cntl(F_SETFL,O_NONBLOCK): {}", this->name,
                                                        cpp_strerror(r));
        return -r;
    }

    return 0;
}

MsgWorker::MsgWorker(FlameContext *c, int i)
: fct(c), index(i), event_poller(c, 128), worker_thread(this), 
    is_running(false), extra_job_num(0), external_mutex(), external_num(0),
    time_work_next_id(1){
    int r;
    name = "MsgWorker" + std::to_string(i);

    int fds[2];
    if (pipe(fds) < 0) {
        ML(fct, error, "{} can't create notify pipe: {}", this->name,
                                                        cpp_strerror(errno));
        throw ErrnoException(errno);
    }

    notify_receive_fd = fds[0];
    notify_send_fd = fds[1];
    r = set_nonblock(notify_receive_fd);
    if (r < 0) {
        throw ErrnoException(-r);
    }
    r = set_nonblock(notify_send_fd);
    if (r < 0) {
        throw ErrnoException(-r);
    }

    auto notify_cb = new HandleNotifyCallBack(fct);
    notify_cb->fd = notify_receive_fd;
    add_event(notify_cb);
    notify_cb->put();
}

MsgWorker::~MsgWorker(){
    if (notify_receive_fd >= 0)
        ::close(notify_receive_fd);
    if (notify_send_fd >= 0)
        ::close(notify_send_fd);
}

int MsgWorker::get_job_num(){
    return event_poller.get_event_num() + extra_job_num;
}

void MsgWorker::update_job_num(int v){
    extra_job_num += v;
    if(extra_job_num < 0)
        extra_job_num = 0;
}

int MsgWorker::get_event_num() {
    return event_poller.get_event_num();
}

int MsgWorker::add_event(EventCallBack *ecb){
    return event_poller.set_event(ecb->fd, ecb);
}

int MsgWorker::del_event(int fd){
    return event_poller.del_event(fd);
}


void MsgWorker::wakeup(){
    char buf = 'c';
    // wake up "event_wait"
    int n = write(notify_send_fd, &buf, sizeof(buf));
    if (n < 0) {
        if (errno != EAGAIN) {
            ML(fct, error, "{} write notify pipe failed: {}", this->name,
                                                        cpp_strerror(errno));
        }
    }
}

void MsgWorker::add_time_work(time_point expire, work_fn_t work_fn, 
                                                                uint64_t id){
    assert(worker_thread.am_self());
    std::multimap<time_point, std::pair<uint64_t, work_fn_t>>::value_type 
                            val(expire, std::make_pair(id, work_fn));
    auto it = time_works.insert(std::move(val));
    tw_map[id] = it;
}

void MsgWorker::del_time_work(uint64_t time_work_id){
    assert(worker_thread.am_self());
    ML(fct, trace, "id={}", time_work_id);
    auto it = tw_map.find(time_work_id);
    if(it == tw_map.end()){
        ML(fct, debug, "id={} not found", time_work_id);
        return;
    }
    time_works.erase(it->second);
    tw_map.erase(it);
}

uint64_t MsgWorker::post_time_work(uint64_t microseconds, work_fn_t work_fn){
    uint64_t id = time_work_next_id++;
    auto now = clock_type::now();
    ML(fct, trace, "id={} trigger after {} us", id, microseconds);
    time_point expire = now + std::chrono::microseconds(microseconds);
    if(worker_thread.am_self()){
        add_time_work(expire, work_fn, id);
    }else{
        post_work([expire, work_fn, id, this](){
            this->add_time_work(expire, work_fn, id);
        });
    }
    return id;
}

void MsgWorker::cancel_time_work(uint64_t id){
    if(worker_thread.am_self()){
        del_time_work(id);
    }else{
        post_work([id, this](){
            this->del_time_work(id);
        });
    }
}

void MsgWorker::post_work(work_fn_t work_fn){
    bool wake = false;
    uint64_t num = 0;
    {
        MutexLocker l(external_mutex);
        external_queue.push_back(work_fn);
        wake = !external_num.load();
        num = ++external_num;
    }
    if (!worker_thread.am_self() && wake)
        wakeup();
    ML(fct, debug, "{} pending {}", this->name, num);
}

void MsgWorker::start(){
    is_running = true;
    worker_thread.create(name.c_str());
    ML(fct, debug, "{} worker thread created", this->name);
}

void MsgWorker::stop(){
    is_running = false;
    wakeup();
    worker_thread.join();
    
    MutexLocker l(external_mutex);
    external_queue.clear();
    external_num = 0;
}

int MsgWorker::process_time_works(){
    int processed = 0;
    time_point now = clock_type::now();
    // ML(fct, trace, "cur time is {} us", 
    //         std::chrono::duration_cast
    //         <std::chrono::microseconds>(now.time_since_epoch()).count());

    while(!time_works.empty()){
        auto it = time_works.begin();
        if(now >= it->first){
            auto pair = it->second;
            uint64_t id = pair.first;
            work_fn_t work_fn = pair.second;
            time_works.erase(it);
            tw_map.erase(id);
            ML(fct, trace, "process time work: id={}", id);
            ++processed;
            work_fn();
        }else{
            break;
        }
    }

    return processed;
}

void MsgWorker::process(){
    ML(fct, debug, "{} start", this->name);
    std::vector<FiredEvent> fevents;
    struct timeval timeout;
    int numevents;

    while(is_running){

        bool trigger_time = false;
        auto now = clock_type::now();
        auto tw_it = time_works.begin();
        bool blocking = !external_num.load();
        if(!blocking){
            if(tw_it != time_works.end() && now >= tw_it->first){
                trigger_time = true;
            }
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;
        }else{
            time_point shortest;
            shortest = now + std::chrono::seconds(30);
            if(tw_it != time_works.end() && shortest >= tw_it->first){
                shortest = tw_it->first;
                trigger_time = true;
            }
            if(shortest > now){
                auto dur =  std::chrono::duration_cast
                                <std::chrono::microseconds>(shortest - now)
                                .count();
                timeout.tv_sec = dur / 1000000;
                timeout.tv_usec = dur % 1000000;
            }else{
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
            }

        }

        numevents = event_poller.process_events(fevents, &timeout);
        if(!is_running) break;

        for(int i = 0;i < numevents;++i){
            //* 确保ecb不会被释放，直到该ecb处理完成
            ML(fct, trace, "{} process fd:{}, mask:{}", this->name,
                                        fevents[i].ecb->fd, fevents[i].mask);
            fevents[i].ecb->get();
            if(FLAME_EVENT_ERROR & fevents[i].mask){
                fevents[i].ecb->error_cb();
                fevents[i].ecb->put();
                continue;
            }
            if(FLAME_EVENT_READABLE & fevents[i].mask & fevents[i].ecb->mask){
                fevents[i].ecb->read_cb();
            }
            if(FLAME_EVENT_WRITABLE & fevents[i].mask & fevents[i].ecb->mask){
                fevents[i].ecb->write_cb();
            }
            fevents[i].ecb->put();
        }

        if(trigger_time){
            numevents += this->process_time_works();
        }

        if (external_num.load()) {
            std::deque<work_fn_t> cur_process;
            {
                MutexLocker l(external_mutex);
                cur_process.swap(external_queue);
                external_num.store(0);
            }
            numevents += cur_process.size();
            while (!cur_process.empty()) {
                work_fn_t cb = cur_process.front();
                ML(fct, trace, "{} do func {:p}", this->name,
                                            (void *)cb.target<void(void)>());
                cb();
                cur_process.pop_front();
            }
        }

        if(numevents > 0){
            ML(fct, trace, "{} process {} event and work", this->name, 
                                                                numevents);
        }
    }

    ML(fct, debug, "{} exit", this->name);

}

void MsgWorker::drain(){
    // only executed when stop.
    if(is_running) return;
    int total = 0;
    ML(fct, trace, "{} drain start", this->name);
    while(external_num.load() > 0){
        std::deque<work_fn_t> cur_process;
        {
            MutexLocker l(external_mutex);
            cur_process.swap(external_queue);
            external_num.store(0);
        }
        total += cur_process.size();
        while (!cur_process.empty()) {
            work_fn_t cb = cur_process.front();
            ML(fct, trace, "{} do func {:p}", this->name,
                                        (void *)cb.target<void()>());
            cb();
            cur_process.pop_front();
        }
    }

    total += this->process_time_works();
    if(time_works.size() > 0){
        ML(fct, trace, "{}: {} time_works not done", 
                                    this->name, time_works.size());
    }

    ML(fct, trace, "{} drain done. total: {}", this->name, total);
}


}