#ifndef NGGC_BACKEND_H
#define NGGC_BACKEND_H

#include "nggc/nggc.h"

#include <stddef.h>

typedef struct nggc_backend nggc_backend;

typedef struct nggc_backend_stats {
    size_t arena_size;
    size_t committed_bytes;
    size_t decommitted_bytes;
    size_t mmap_live_bytes;
    size_t page_size;
    size_t allocation_granularity;
} nggc_backend_stats;

nggc_result nggc_backend_create(size_t uhm_arena_size,
                                nggc_backend **out_backend);
nggc_result nggc_backend_destroy(nggc_backend *backend);
nggc_result nggc_backend_map(nggc_backend *backend, size_t size,
                             void **out_addr);
nggc_result nggc_backend_unmap(nggc_backend *backend, void *addr,
                               size_t size);
nggc_result nggc_backend_decommit(nggc_backend *backend, void *addr,
                                  size_t size);
nggc_result nggc_backend_recommit(nggc_backend *backend, void *addr,
                                  size_t size);
nggc_result nggc_backend_get_stats(nggc_backend *backend,
                                   nggc_backend_stats *out_stats);
nggc_result nggc_backend_check(nggc_backend *backend);

#if NGGC_ENABLE_FAULT_INJECTION
void nggc_backend_fault_reset(void);
void nggc_backend_fault_fail_next_create(void);
void nggc_backend_fault_fail_next_map(void);
void nggc_backend_fault_fail_next_unmap(void);
void nggc_backend_fault_fail_next_decommit(void);
void nggc_backend_fault_fail_next_recommit(void);
void nggc_backend_fault_fail_next_stats(void);
void nggc_backend_fault_fail_next_check(void);
#endif

#endif
