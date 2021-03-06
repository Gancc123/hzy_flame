#include "MemoryManager.h"
#include "RdmaMem.h"
#include "acconfig.h"

#include <cstring>
#include <cstdlib>
#include <sys/mman.h>

#define ALIGN_TO_PAGE_SIZE(x, page_size) \
  (((x) + (page_size) -1) / (page_size) * (page_size))

namespace flame{
namespace msg{
namespace ib{

Chunk::Chunk(uint32_t len)
:bytes(len), offset(0){
}

Chunk::~Chunk(){
}

void Chunk::set_offset(uint32_t o){
    offset = o;
}

uint32_t Chunk::get_offset(){
    return offset;
}

void Chunk::set_bound(uint32_t b){
    bound = b;
}

void Chunk::prepare_read(uint32_t b){
    offset = 0;
    bound = b;
}

uint32_t Chunk::get_bound(){
    return bound;
}

uint32_t Chunk::read(char* buf, uint32_t len){
    uint32_t left = bound - offset;
    if (left >= len) {
        ::memcpy(buf, data+offset, len);
        offset += len;
        return len;
    } else {
        ::memcpy(buf, data+offset, left);
        offset = 0;
        bound = 0;
        return left;
    }
}

uint32_t Chunk::write(char* buf, uint32_t len){
    uint32_t left = bytes - offset;
    if (left >= len) {
        ::memcpy(data+offset, buf, len);
        offset += len;
        return len;
    } else {
        ::memcpy(data+offset, buf, left);
        offset = bytes;
        return left;
    }
}

bool Chunk::full(){
    return offset == bytes;
}

bool Chunk::over(){
    return offset == bound;
}

void Chunk::clear(){
    offset = 0;
    bound = 0;
}

void *MemPool::slow_malloc(){
    void *p;

    MutexLocker l(PoolAllocator::m_lock);
    PoolAllocator::mmgr = mmgr;
    // this will trigger pool expansion via PoolAllocator::malloc()
    p = boost::pool<PoolAllocator>::malloc();
    PoolAllocator::mmgr = nullptr;
    return p;
}

Mutex PoolAllocator::m_lock{};
MemoryManager *PoolAllocator::mmgr = nullptr;

// lock is taken by mem_pool::slow_malloc()
char *PoolAllocator::malloc(const size_type bytes){
    mem_info *m;
    Chunk *ch;
    size_t buf_size;
    unsigned nbufs;
    MsgContext *mct;

    assert(mmgr);
    mct      = mmgr->mct;
    buf_size = sizeof(Chunk) + mmgr->get_buffer_size();
    nbufs    = bytes/buf_size;

    if (!mmgr->can_alloc(nbufs))
        return NULL;

    size_t real_size;
    if(mct->config->rdma_enable_hugepage){
        uint32_t page_size = mmgr->get_hugepage_size();
        real_size = ALIGN_TO_PAGE_SIZE(bytes + sizeof(*m), page_size);
    }else{
        real_size = bytes + sizeof(*m);
    }

    m = static_cast<mem_info *>(mmgr->malloc(real_size));
    if (!m) {
        ML(mct, error, "failed to allocate {} + {} bytes of memory for {}",
                                                    bytes, sizeof(*m), nbufs);
        return NULL;
    }

    // Here we register m with real_size. 
    // When using huge_page, ibv_reg_mr m(at least hugepage_size) with
    // s (s < hugepage_size) will fail. Don't know reason.
    // So s will be the allocated memory's real_size.
    m->mr = ibv_reg_mr(mmgr->get_pd()->pd, m, real_size, 
                            IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
    if (m->mr == NULL) {
        ML(mct, error, "failed to register {} + {} ({}) bytes of memory for "
                    "{} bufs: {}", bytes, sizeof(*m), real_size,
                    nbufs, cpp_strerror(errno));
        mmgr->free(m);
        return NULL;
    }else{
        ML(mct, info, "{:p} register {} + {} ({}) bytes of memory for {} bufs.",
            (void *)m, bytes, sizeof(*m), real_size, nbufs);
    }

    m->nbufs = nbufs;
    // save this chunk context
    m->mmgr   = mmgr;

    mmgr->update_stats(nbufs);

    /* initialize chunks */
    ch = m->chunks;
    for (unsigned i = 0; i < nbufs; i++) {
        ch->lkey   = m->mr->lkey;
        ch->bytes  = mmgr->get_buffer_size();
        ch->offset = 0;
        ch = reinterpret_cast<Chunk *>(reinterpret_cast<char *>(ch) + buf_size);
    }

    return reinterpret_cast<char *>(m->chunks);
}


void PoolAllocator::free(char * const block){
    mem_info *m;

    MutexLocker l(m_lock);
    m = reinterpret_cast<mem_info *>(block) - 1;
    m->mmgr->update_stats(-m->nbufs);
    ibv_dereg_mr(m->mr);
    m->mmgr->free(m);
}

MemoryManager::MemoryManager(MsgContext *c, ProtectionDomain *p)
: mct(c), pd(p), buffer_size(c->config->rdma_buffer_size),
    hugepage_size(c->config->rdma_hugepage_size),
    lock(MUTEX_TYPE_ADAPTIVE_NP), huge_page_map_lock(MUTEX_TYPE_ADAPTIVE_NP),
    mem_pool(this, sizeof(Chunk) + mct->config->rdma_buffer_size,
                                                        calcu_init_space(c)){
    rdma_buffer_allocator = new RdmaBufferAllocator(this);
    assert(rdma_buffer_allocator);
    int r = rdma_buffer_allocator->init();
    assert(r == 0);
    ML(mct, info, "hugepage size: {}", size_str_from_uint64(hugepage_size));
}

MemoryManager::~MemoryManager(){
    rdma_buffer_allocator->fin();
    delete rdma_buffer_allocator;
    mem_pool.purge_memory();
}

void* MemoryManager::huge_pages_malloc(size_t size){
    size_t real_size = ALIGN_TO_PAGE_SIZE(size, hugepage_size);
    char *ptr = (char *)mmap(nullptr, real_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB,
                    -1, 0);
    if (ptr == MAP_FAILED) {
        ML(mct, warn, "Alloc huge page failed. Use malloc. {} ({}) B",
            size, real_size);
        ptr = (char *)std::malloc(real_size);
        if (ptr == nullptr) return nullptr;
        real_size = 0;
    }else{
        MutexLocker l(huge_page_map_lock);
        assert(huge_page_map[ptr] == 0);
        huge_page_map[ptr] = real_size;
    }
    return ptr;
}

void MemoryManager::huge_pages_free(void *ptr){
    if (ptr == nullptr) return;
    MutexLocker l(huge_page_map_lock);
    auto it = huge_page_map.find(ptr);
    if(it != huge_page_map.end()){
        size_t real_size = it->second;
        assert(real_size % hugepage_size == 0);
        munmap(ptr, real_size);
        huge_page_map.erase(it);
    }else{
        std::free(ptr);
    }
}

void* MemoryManager::malloc(size_t size){
    if(mct->config->rdma_enable_hugepage)
        return huge_pages_malloc(size);
    else
        return std::malloc(size);
}

void MemoryManager::free(void *ptr){
    if (mct->config->rdma_enable_hugepage)
        huge_pages_free(ptr);
    else
        std::free(ptr);
}

int MemoryManager::get_buffers(size_t bytes, std::vector<Chunk*> &c){
    uint32_t num = bytes / buffer_size + 1;
    if(bytes % buffer_size == 0){
        --num;
    }
    int r = 0;
    c.reserve(num);
    MutexLocker l(lock);
    for(r = 0;r < num;++r){
        auto ch = reinterpret_cast<Chunk *>(mem_pool.malloc());
        if(ch == nullptr) {
            break;
        }
        ch->clear(); // !!!clear the mem_pool's pointer.
        c.push_back(ch);
    }
    return r;
}

void MemoryManager::release_buffers(std::vector<Chunk*> &c){
    MutexLocker l(lock);
    for(auto ch : c){
        mem_pool.free(ch);
    }
}

uint64_t MemoryManager::calcu_init_space(MsgContext *mct){
    auto config = mct->config;

    uint64_t default_init_space = 
            64 * (config->rdma_recv_queue_len + config->rdma_send_queue_len);

    size_t chunk_bytes = config->rdma_buffer_size + sizeof(Chunk);
    
    // if(default_init_space 
    //                 < (hugepage_size - 16 - sizeof(mem_info)) / chunk_bytes){
    //     // use a huge page for init: 
    //     // mem_info(4K) + chunks(4K * n) == huge_page_size
    //     default_init_space = 
    //                     (hugepage_size - 16 - sizeof(mem_info)) / chunk_bytes;
    // }

    if(config->rdma_buffer_num > 0 
        && config->rdma_buffer_num < default_init_space){
        return config->rdma_buffer_num;
    }

    return default_init_space;
}


} //namespace ib
} //namespace msg
} //namespace flame