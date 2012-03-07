#include <stdlib.h>
#include <stdio.h>  // for printf
#include <stdint.h> // for uint32_t etc
#include <string.h> // for memcopy
#include <assert.h> // for assert

#include "config.h"

#ifdef HAVE_NUMA_H
#include <numa.h>
#endif

#include "llsimplecache.h"

#ifndef LINE_SIZE
    #define LINE_SIZE 64 // default cache line
#endif

#define barrier    asm volatile("": : :"memory")
#define cpu_relax  asm volatile("pause\n": : :"memory")
#define cas(a,b,c) __sync_bool_compare_and_swap((a),(b),(c))

struct llsimplecache
{
    size_t                 cache_size;  
    uint32_t               mask;         // size-1
    uint32_t               *table;       // table with data
    llsimplecache_delete_f cb_delete;    // delete function (callback pre-delete)
    void                   *cb_data;
};

// 64-bit hashing function, http://www.locklessinc.com (hash_mul.s)
unsigned long long hash_mul(const void* data, unsigned long long len);

static const int       HASH_PER_CL = ((LINE_SIZE) / 4);
static const uint32_t  CL_MASK     = ~(((LINE_SIZE) / 4) - 1); 
static const uint32_t  CL_MASK_R   = ((LINE_SIZE) / 4) - 1;

static const uint32_t  EMPTY       = 0;

/* Example values with a LINE_SIZE of 64
 * HASH_PER_CL = 16
 * CL_MASK     = 0xFFFFFFF0
 * CL_MASK_R   = 0x0000000F
 */

// Calculate next index on a cache line walk
static inline int next(uint32_t *cur, uint32_t last) 
{
    return (*cur = (*cur & CL_MASK) | ((*cur + 1) & CL_MASK_R)) != last;
}

int llsimplecache_put(const llsimplecache_t dbs, uint32_t *data, uint32_t hash) 
{
    if (hash == 0) hash = (uint32_t)hash_mul(data, sizeof(uint32_t));
    if (hash == 0) hash++; // blah. Just avoid 0, that's all.

    uint32_t f_idx = hash & dbs->mask;
    uint32_t idx = f_idx;
    uint32_t v;

    register const uint32_t d = *data;

    do {
        register volatile uint32_t *bucket = &dbs->table[idx];

        register uint32_t v;
restart_bucket:
        v = *bucket;

        // Check empty
        if (v == EMPTY) {
            if (cas(bucket, EMPTY, d)) return 1;
            cpu_relax;
            goto restart_bucket;
        }

        // Check existing
        if (v == d) return 0;                    
    } while (next(&idx, f_idx));

    // If we are here, the cache line is full.
    // Claim first bucket
    while (1) {
        register volatile uint32_t *bucket = &dbs->table[f_idx];
        register const uint32_t v = *bucket;
        if (v == d) return 0;
        if (cas(bucket, v, d)) {
            *data = v;
            return 2;
        }
        cpu_relax;
    }
}

static inline unsigned next_pow2(unsigned x)
{
    if (x <= 2) return x;
    return (1ULL << 32) >> __builtin_clz(x - 1);
}

llsimplecache_t llsimplecache_create(size_t cache_size, llsimplecache_delete_f cb_delete, void *cb_data)
{
    llsimplecache_t dbs;
    posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llsimplecache));
    
    if (cache_size < HASH_PER_CL) cache_size = HASH_PER_CL;
    assert(next_pow2(cache_size) == cache_size);
    dbs->cache_size = cache_size;
    dbs->mask = dbs->cache_size - 1;

#ifdef HAVE_NUMA_H
    if (numa_available >= 0) {
        dbs->table = (uint32_t*)numa_alloc_interleaved(dbs->cache_size * sizeof(uint32_t));
    } else {
#endif
    posix_memalign((void**)&dbs->table, LINE_SIZE, dbs->cache_size * sizeof(uint32_t));
#ifdef HAVE_NUMA_H
    }
#endif

    memset(dbs->table, 0, sizeof(uint32_t) * dbs->cache_size);

    dbs->cb_delete = cb_delete; // can be NULL
    dbs->cb_data   = cb_data;

    return dbs;
}

inline void llsimplecache_clear(llsimplecache_t dbs)
{
    llsimplecache_clear_partial(dbs, 0, dbs->cache_size);
}

inline void llsimplecache_clear_partial(llsimplecache_t dbs, size_t first, size_t count)
{
    register volatile uint32_t *bucket = &dbs->table[first];
    register uint32_t *end = &dbs->table[first+count];

    if (dbs->cb_delete == NULL) {
        while (bucket < end) *bucket++ = 0;        
        return;
    }

    while (bucket < end) {
        while(1) {
            register uint32_t data = *bucket;
            if (data == 0) break;
            if (cas(bucket, data, 0)) {
                dbs->cb_delete(dbs->cb_data, data);
                break;
            } 
        }
        bucket++; // next!
    }
}

void llsimplecache_free(llsimplecache_t dbs)
{
#ifdef HAVE_NUMA_H
    if (numa_available() >= 0) {
        numa_free(dbs->table, dbs->cache_size * sizeof(uint32_t));
    } else {
#endif
    free(dbs->table);
#ifdef HAVE_NUMA_H
    }
#endif
    free(dbs);
}

void llsimplecache_print_size(llsimplecache_t dbs, FILE *f)
{
    fprintf(f, "4 * %ld = %ld bytes", dbs->cache_size, dbs->cache_size * sizeof(uint32_t));
}
