#ifndef FLAME_INCLUDE_META_H
#define FLAME_INCLUDE_META_H

#include <cstdint>

namespace flame {

/**
 * Cluster MetaData
 */
struct cluster_meta_t {
    std::string name    {};
    uint32_t    mgrs    {0};
    uint32_t    csds    {0};
    uint64_t    size    {0};
    uint64_t    alloced {0};
    uint64_t    used    {0};
    uint64_t    ctime   {0};
}; // struct cluster_meta_t

/**
 * Volume Group MetaData
 */
struct volume_group_meta_t {
    uint64_t    vg_id   {0};
    std::string name    {};
    uint64_t    ctime   {0};    // (ms)
    uint32_t    volumes {0};
    uint64_t    size    {0};    // total visible size
    uint64_t    alloced {0};    // total logical size
    uint64_t    used    {0};    // total physical size
}; // struct group_mate_t

/**
 * Volume MetaData
 */
struct volume_meta_t {
    uint64_t    vol_id  {0};
    uint64_t    vg_id   {0};
    std::string name    {};
    uint64_t    ctime   {0};    // (ms)
    uint64_t    chk_sz  {0};    // chunk size
    uint64_t    size    {0};
    uint64_t    alloced {0};
    uint64_t    used    {0};
    uint32_t    flags   {0};
    uint32_t    spolicy {0};    // store policy
    uint32_t    chunks  {0};    // chunk number
}; // struct volume_meta_t

enum VolumeFlags {
    VOL_FLG_PREALLOC = 0x1
};

/**
 * chunk_id_t
 * 
 * |------- volume id -------|---- index ---|--- sub id ---|
 * | 63                   20 | 19         4 | 3          0 |
 */
struct chunk_id_t {
    uint64_t val;

    chunk_id_t(uint64_t v) : val(v) {}
    chunk_id_t(uint64_t vol_id, uint64_t index, uint64_t sub_id) {
        val = (vol_id << 20) | ((index & 0xFFFFULL) << 4) | (sub_id & 0xFULL);
    }

    uint64_t get_vol_id() const { return val >> 20; }
    void set_vol_id(uint64_t v) { val = (v << 20) | (val & 0xFFFFFULL); }

    uint16_t get_index() const { return (val >> 4) & 0xFFFFULL; }
    void set_index(uint64_t v) { val = (val & ~0xFFFFFULL) | ((v & 0xFFFFULL) << 4) | (val & 0xFULL); }

    uint16_t get_sub_id() const { return val & 0xFULL; }
    void set_sub_id(uint64_t v) { val = (val & ~0xFULL) | (v & 0xFULL); }

    operator uint64_t () const { return val; }
    chunk_id_t& operator = (uint64_t v) { val = v; return *this; }
}; // struct chunk_id_t

/**
 * Chunk MetaData
 */
struct chunk_meta_t {
    uint64_t    chk_id  {0};
    uint64_t    vol_id  {0};
    uint32_t    index   {0};    // Thera is a same index in a chunk group
    uint32_t    stat    {0};    // chunk status
    uint32_t    spolicy {0};
    uint64_t    ctime   {0};
    uint64_t    primary {0};    // this chunk is the primary chunk when primary == chk_id
    uint64_t    size    {0};
    uint64_t    csd_id  {0};
    uint64_t    csd_mtime {0};  // csd map time  
    uint64_t    dst_id  {0};
    uint64_t    dst_ctime {0};  // dst start time
}; // struct chunk_meta_t

enum ChunkFlags {
    CHK_FLG_PREALLOC = 0x1
};

/**
 * Chunk Health MetaData
 */
struct chunk_health_meta_t {
    uint64_t    chk_id  {0};
    uint32_t    stat    {0};
    uint64_t    size    {0};
    uint64_t    used    {0};
    uint64_t    csd_used    {0};
    uint64_t    dst_used    {0};
    uint64_t    write_count {0};
    uint64_t    read_count  {0};
    uint64_t    last_time   {0};
    uint64_t    last_write  {0};
    uint64_t    last_read   {0};
    uint64_t    last_latency{0};    // (ns)
    uint64_t    last_alloc  {0};
    double      load_weight {0};
    double      wear_weight {0};
    double      total_weight{0};
}; // struct chunk_health_meta_t

/**
 * Node Addr 
 */
struct node_addr_t {
    uint64_t val;

    node_addr_t(uint64_t v) : val(v) {}
    node_addr_t(uint32_t ip, uint16_t port) {
        val = ((uint64_t)ip << 16) | (uint64_t)port;
    }

    uint32_t get_ip() const { return val >> 16; }
    void set_ip(uint32_t v) { val = ((uint64_t)v << 16) | (val & 0xFFFFULL); }

    uint16_t get_port() const { return val & 0xFFFFULL; }
    void set_port(uint16_t v) { val  = (val & ~0XFFFFULL) | (uint64_t)v; }

    operator uint64_t () const { return val; }
    node_addr_t& operator = (uint64_t v) { val = v; return *this; }
};

/**
 * CSD MetaData
 */
struct csd_meta_t {
    uint64_t    csd_id  {0};
    std::string name    {0};
    uint64_t    size    {0};
    uint64_t    ctime   {0};    // (ms)
    uint64_t    io_addr {0};
    uint64_t    admin_addr {0}; 
    uint32_t    stat    {0};
    uint64_t    latime   {0};    // last active time
}; // struct csd_meta_t

/**
 * CSD Health MetaData
 */
struct csd_health_meta_t {
    uint64_t    csd_id  {0};
    uint32_t    stat    {0};
    uint64_t    size    {0};
    uint64_t    alloced {0};
    uint64_t    used    {0};
    uint64_t    write_count {0};
    uint64_t    read_count  {0};
    uint64_t    last_time   {0};
    uint64_t    last_write  {0};
    uint64_t    last_read   {0};
    uint64_t    last_latency{0};
    uint64_t    last_alloc  {0};
    double      load_weight {0};
    double      wear_weight {0};
    double      total_weight{0};
}; // struct csd_health_meta_t

/**
 * Gateway MetaData
 */
struct gateway_meta_t {
    uint64_t    gw_id   {0};
    uint64_t    admin_addr  {0};
    uint64_t    ltime   {0};    // connect time (ms)
    uint64_t    atime   {0};    // active time (ms)
}; // struct gateway_meta_t

} // namespace flame

#endif // FLAME_INCLUDE_META_H