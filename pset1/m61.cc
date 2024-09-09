#include "m61.hh"
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <sys/mman.h>
#include <unordered_map>


struct m61_memory_buffer {
    char* buffer;
    size_t pos = 0;
    size_t size = 8 << 20; /* 8 MiB */

    m61_memory_buffer();
    ~m61_memory_buffer();
};

static m61_memory_buffer default_buffer;


m61_memory_buffer::m61_memory_buffer() {
    void* buf = mmap(nullptr,    // Place the buffer at a random address
        this->size,              // Buffer should be 8 MiB big
        PROT_WRITE,              // We want to read and write the buffer
        MAP_ANON | MAP_PRIVATE, -1, 0);
                                 // We want memory freshly allocated by the OS
    assert(buf != MAP_FAILED);
    this->buffer = (char*) buf;
}

m61_memory_buffer::~m61_memory_buffer() {
    munmap(this->buffer, this->size);
}

/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.

static m61_statistics gstats = {
    .nactive = 0,
    .active_size = 0,
    .ntotal = 0,
    .total_size = 0,
    .nfail = 0,
    .fail_size = 0,
    .heap_min = UINTPTR_MAX,
    .heap_max = 0
};

std::unordered_map<void*, size_t> allocation_map;

void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings

    if (default_buffer.pos + sz > default_buffer.size || default_buffer.pos + sz < default_buffer.pos) {
        // Not enough space left in default buffer for allocation
        // Check for overflow
        ++ gstats.nfail;
        gstats.fail_size += sz;
        return nullptr;
    }

    // Otherwise there is enough space; claim the next `sz` bytes
    void* ptr = &default_buffer.buffer[default_buffer.pos];

    if ((uintptr_t) ptr < gstats.heap_min){
        gstats.heap_min = (uintptr_t) ptr;
    }

    if ((uintptr_t) ptr > gstats.heap_max){
        gstats.heap_max = (uintptr_t) (ptr) + sz - 1;;
    }


    allocation_map[ptr] = sz;

    default_buffer.pos += sz;
    gstats.total_size += sz;
    gstats.active_size += sz;
    ++gstats.nactive;
    ++gstats.ntotal;

    return ptr;

}


/// m61_free(ptr, file, line)
///    Frees the memory allocation pointed to by `ptr`. If `ptr == nullptr`,
///    does nothing. Otherwise, `ptr` must point to a currently active
///    allocation returned by `m61_malloc`. The free was called at location
///    `file`:`line`.

void m61_free(void* ptr, const char* file, int line) {
    // avoid uninitialized variable warnings
    (void) ptr, (void) file, (void) line;

    if (ptr == nullptr ){
        return;
    }

    //make sure ptr is at an active allocation
    if(ptr < default_buffer.buffer || ptr > (default_buffer.buffer + default_buffer.pos)){
        fprintf (stdout, "Invalid pointer");
        return; 
    }

    auto it = allocation_map.find(ptr);
    if (it == allocation_map.end()) {
        fprintf(stdout, "Invalid free at %s:%d\n", file, line);
        return;
    }

    size_t ptr_size = it->second;

    //free space at pointer
    memset(ptr, 0, ptr_size);
    --gstats.nactive;
    gstats.active_size -= ptr_size;

    allocation_map.erase(ptr);

}


/// m61_calloc(count, sz, file, line)
///    Returns a pointer a fresh dynamic memory allocation big enough to
///    hold an array of `count` elements of `sz` bytes each. Returned
///    memory is initialized to zero. The allocation request was at
///    location `file`:`line`. Returns `nullptr` if out of memory; may
///    also return `nullptr` if `count == 0` or `size == 0`.

void* m61_calloc(size_t count, size_t sz, const char* file, int line) {
    if (count != 0 && sz > SIZE_MAX/count){
        //sz * count would cause an overflow
        ++ gstats.nfail;
        gstats.fail_size += SIZE_MAX;
        return nullptr;
    }

    void* ptr = m61_malloc(count * sz, file, line);
    if (ptr) {
        memset(ptr, 0, count * sz);
    }
    return ptr;
}


/// m61_get_statistics()
///    Return the current memory statistics.

m61_statistics m61_get_statistics() {
    // The handout code sets all statistics to enormous numbers.
    // m61_statistics stats;
    // memset(&stats, 0, sizeof(m61_statistics));
    // stats.ntotal = gstats.ntotal;
    return gstats;
}


/// m61_print_statistics()
///    Prints the current memory statistics.

void m61_print_statistics() {
    m61_statistics stats = m61_get_statistics();
    printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}


/// m61_print_leak_report()
///    Prints a report of all currently-active allocated blocks of dynamic
///    memory.

void m61_print_leak_report() {
    // Your code here.
}
