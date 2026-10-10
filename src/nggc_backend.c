#include "nggc_backend.h"

#include "uhm/uhm_platform.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct nggc_backend {
    uhm_platform_heap owner;
    uhm_heap *heap;
    const uhm_api *api;
};

#if NGGC_ENABLE_FAULT_INJECTION
static atomic_uint nggc_backend_fail_next_create;
static atomic_uint nggc_backend_fail_next_map;
static atomic_uint nggc_backend_fail_next_unmap;
static atomic_uint nggc_backend_fail_next_decommit;
static atomic_uint nggc_backend_fail_next_recommit;
static atomic_uint nggc_backend_fail_next_stats;
static atomic_uint nggc_backend_fail_next_check;

static int nggc_backend_fault_consume(atomic_uint *flag) {
    return atomic_exchange_explicit(flag, 0u, memory_order_acq_rel) != 0u;
}

void nggc_backend_fault_reset(void) {
    atomic_store_explicit(&nggc_backend_fail_next_create, 0u,
                          memory_order_release);
    atomic_store_explicit(&nggc_backend_fail_next_map, 0u,
                          memory_order_release);
    atomic_store_explicit(&nggc_backend_fail_next_unmap, 0u,
                          memory_order_release);
    atomic_store_explicit(&nggc_backend_fail_next_decommit, 0u,
                          memory_order_release);
    atomic_store_explicit(&nggc_backend_fail_next_recommit, 0u,
                          memory_order_release);
    atomic_store_explicit(&nggc_backend_fail_next_stats, 0u,
                          memory_order_release);
    atomic_store_explicit(&nggc_backend_fail_next_check, 0u,
                          memory_order_release);
}

void nggc_backend_fault_fail_next_create(void) {
    atomic_store_explicit(&nggc_backend_fail_next_create, 1u,
                          memory_order_release);
}

void nggc_backend_fault_fail_next_map(void) {
    atomic_store_explicit(&nggc_backend_fail_next_map, 1u,
                          memory_order_release);
}

void nggc_backend_fault_fail_next_unmap(void) {
    atomic_store_explicit(&nggc_backend_fail_next_unmap, 1u,
                          memory_order_release);
}

void nggc_backend_fault_fail_next_decommit(void) {
    atomic_store_explicit(&nggc_backend_fail_next_decommit, 1u,
                          memory_order_release);
}

void nggc_backend_fault_fail_next_recommit(void) {
    atomic_store_explicit(&nggc_backend_fail_next_recommit, 1u,
                          memory_order_release);
}

void nggc_backend_fault_fail_next_stats(void) {
    atomic_store_explicit(&nggc_backend_fail_next_stats, 1u,
                          memory_order_release);
}

void nggc_backend_fault_fail_next_check(void) {
    atomic_store_explicit(&nggc_backend_fail_next_check, 1u,
                          memory_order_release);
}
#endif

nggc_result nggc_backend_create(size_t uhm_arena_size,
                                nggc_backend **out_backend) {
    nggc_backend *backend;
    uhm_platform_heap_config config;
    uhm_result result;

    if (!out_backend || uhm_arena_size == 0) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_backend = 0;
#if NGGC_ENABLE_FAULT_INJECTION
    if (nggc_backend_fault_consume(&nggc_backend_fail_next_create)) {
        return NGGC_ERR_BACKEND;
    }
#endif
    backend = (nggc_backend *)calloc(1u, sizeof(*backend));
    if (!backend) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = uhm_arena_size;

    result = uhm_platform_heap_open(&config, &backend->owner, &backend->heap);
    if (result != UHM_OK) {
        free(backend);
        return NGGC_ERR_BACKEND;
    }
    backend->api = uhm_get_api(backend->heap);
    if (!backend->api || !backend->api->mmap || !backend->api->munmap ||
        !backend->api->get_stats || !backend->api->check) {
        (void)uhm_platform_heap_close(&backend->owner);
        free(backend);
        return NGGC_ERR_BACKEND;
    }

    *out_backend = backend;
    return NGGC_OK;
}

nggc_result nggc_backend_destroy(nggc_backend *backend) {
    nggc_result result = NGGC_OK;
    if (!backend) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (backend->heap && uhm_platform_heap_close(&backend->owner) != UHM_OK) {
        result = NGGC_ERR_BACKEND;
    }
    free(backend);
    return result;
}

nggc_result nggc_backend_map(nggc_backend *backend, size_t size,
                             void **out_addr) {
    void *addr = 0;
    if (!backend || !backend->api || !backend->api->mmap || !out_addr ||
        size == 0) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_addr = 0;
#if NGGC_ENABLE_FAULT_INJECTION
    if (nggc_backend_fault_consume(&nggc_backend_fail_next_map)) {
        return NGGC_ERR_BACKEND;
    }
#endif
    if (backend->api->mmap(backend->heap, &addr, size,
                           UHM_PROT_READ | UHM_PROT_WRITE,
                           UHM_MAP_PRIVATE) != UHM_OK) {
        return NGGC_ERR_BACKEND;
    }
    *out_addr = addr;
    return NGGC_OK;
}

nggc_result nggc_backend_unmap(nggc_backend *backend, void *addr,
                               size_t size) {
    if (!backend || !backend->api || !backend->api->munmap || !addr ||
        size == 0) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if NGGC_ENABLE_FAULT_INJECTION
    if (nggc_backend_fault_consume(&nggc_backend_fail_next_unmap)) {
        return NGGC_ERR_BACKEND;
    }
#endif
    return backend->api->munmap(backend->heap, addr, size) == UHM_OK
               ? NGGC_OK
               : NGGC_ERR_BACKEND;
}

nggc_result nggc_backend_decommit(nggc_backend *backend, void *addr,
                                  size_t size) {
    if (!backend || !backend->api || !backend->api->munmap || !addr ||
        size == 0) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if NGGC_ENABLE_FAULT_INJECTION
    if (nggc_backend_fault_consume(&nggc_backend_fail_next_decommit)) {
        return NGGC_ERR_BACKEND;
    }
#endif
    return backend->api->munmap(backend->heap, addr, size) == UHM_OK
               ? NGGC_OK
               : NGGC_ERR_BACKEND;
}

nggc_result nggc_backend_recommit(nggc_backend *backend, void *addr,
                                  size_t size) {
    void *fixed = addr;
    if (!backend || !backend->api || !backend->api->mmap || !addr ||
        size == 0) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if NGGC_ENABLE_FAULT_INJECTION
    if (nggc_backend_fault_consume(&nggc_backend_fail_next_recommit)) {
        return NGGC_ERR_BACKEND;
    }
#endif
    if (backend->api->mmap(backend->heap, &fixed, size,
                           UHM_PROT_READ | UHM_PROT_WRITE,
                           UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE) !=
            UHM_OK ||
        fixed != addr) {
        return NGGC_ERR_BACKEND;
    }
    return NGGC_OK;
}

nggc_result nggc_backend_get_stats(nggc_backend *backend,
                                   nggc_backend_stats *out_stats) {
    uhm_stats stats;
    if (!backend || !backend->api || !backend->api->get_stats ||
        !out_stats) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if NGGC_ENABLE_FAULT_INJECTION
    if (nggc_backend_fault_consume(&nggc_backend_fail_next_stats)) {
        return NGGC_ERR_BACKEND;
    }
#endif
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    if (backend->api->get_stats(backend->heap, &stats) != UHM_OK) {
        return NGGC_ERR_BACKEND;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    out_stats->arena_size = stats.arena_size;
    out_stats->committed_bytes = stats.committed_bytes;
    out_stats->decommitted_bytes = stats.decommitted_bytes;
    out_stats->mmap_live_bytes = stats.mmap_live_bytes;
    out_stats->page_size = backend->owner.vm.page_size;
    out_stats->allocation_granularity =
        backend->owner.vm.allocation_granularity;
    return NGGC_OK;
}

nggc_result nggc_backend_check(nggc_backend *backend) {
    if (!backend || !backend->api || !backend->api->check) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if NGGC_ENABLE_FAULT_INJECTION
    if (nggc_backend_fault_consume(&nggc_backend_fail_next_check)) {
        return NGGC_ERR_BACKEND;
    }
#endif
    return backend->api->check(backend->heap, 0u) == UHM_OK
               ? NGGC_OK
               : NGGC_ERR_BACKEND;
}
