#ifndef UHM_UHM_H
#define UHM_UHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define UHM_CALL __cdecl
#if defined(UHMAS_BUILD_SHARED)
#if defined(UHMAS_BUILDING_LIBRARY)
#define UHM_PUBLIC __declspec(dllexport)
#else
#define UHM_PUBLIC __declspec(dllimport)
#endif
#else
#define UHM_PUBLIC
#endif
#else
#define UHM_CALL
#if defined(__GNUC__) || defined(__clang__)
#define UHM_PUBLIC __attribute__((visibility("default")))
#else
#define UHM_PUBLIC
#endif
#endif

#define UHM_ABI_MAJOR 0u
#define UHM_ABI_MINOR 17u

typedef struct uhm_heap uhm_heap;
typedef struct uhm_thread_ctx uhm_thread_ctx;

typedef int32_t uhm_result;

#define UHM_OK                         ((uhm_result)0)
#define UHM_ERR_INVALID_ARGUMENT       ((uhm_result)-1)
#define UHM_ERR_OUT_OF_MEMORY          ((uhm_result)-2)
#define UHM_ERR_OUT_OF_ADDRESS_SPACE   ((uhm_result)-3)
#define UHM_ERR_ALREADY_MAPPED         ((uhm_result)-4)
#define UHM_ERR_NOT_MAPPED             ((uhm_result)-5)
#define UHM_ERR_PROTECTION             ((uhm_result)-6)
#define UHM_ERR_UNSUPPORTED            ((uhm_result)-7)
#define UHM_ERR_PLATFORM               ((uhm_result)-8)
#define UHM_ERR_CORRUPTION             ((uhm_result)-9)
#define UHM_ERR_BUSY                   ((uhm_result)-10)

#define UHM_PROT_NONE                  (0u)
#define UHM_PROT_READ                  (1u << 0)
#define UHM_PROT_WRITE                 (1u << 1)
#define UHM_PROT_EXEC                  (1u << 2)

#define UHM_MAP_PRIVATE                (1u << 0)
#define UHM_MAP_FIXED                  (1u << 1)
#define UHM_MAP_FIXED_NOREPLACE        (1u << 2)
#define UHM_MAP_NO_COMMIT              (1u << 3)
#define UHM_MAP_PAGE_PRIVATE           (1u << 4)

#define UHM_REMAP_MAYMOVE              (1u << 0)

#define UHM_ARENA_PRE_RESERVED         (1u << 0)

#define UHM_CONFIG_LOCKLESS_REMOTE_FREE  (1u << 0)
#define UHM_CONFIG_SPINLOCK_BURN         (1u << 1)
#define UHM_CONFIG_SPINLOCK_YIELD        (1u << 2)
#define UHM_CONFIG_FAIL_ON_TREAP_HEALTH  (1u << 3)
#define UHM_CONFIG_REPAIR_TREAP_HEALTH   (1u << 4)

#define UHM_CHECK_FAIL_ON_TREAP_HEALTH   (1u << 0)

#define UHM_VM_CAN_RESERVE                 (1ull << 0)
#define UHM_VM_CAN_COMMIT                  (1ull << 1)
#define UHM_VM_CAN_DECOMMIT                (1ull << 2)
#define UHM_VM_CAN_RELEASE                 (1ull << 3)
#define UHM_VM_CAN_PROTECT                 (1ull << 4)
#define UHM_VM_CAN_FIXED_REMAP             (1ull << 5)
#define UHM_VM_FIXED_NOREPLACE_ATOMIC      (1ull << 6)
#define UHM_VM_DECOMMIT_MAKES_NOACCESS     (1ull << 7)
#define UHM_VM_DECOMMIT_DROPS_PHYSICAL     (1ull << 8)
#define UHM_VM_DECOMMIT_ZERO_FILLS         (1ull << 9)
#define UHM_VM_CAN_RECOMMIT_SAME_ADDRESS   (1ull << 10)
#define UHM_VM_COMMIT_IS_PROTECT_ONLY      (1ull << 11)
#define UHM_VM_USERMODE_LOGICAL_VM         (1ull << 12)
#define UHM_VM_PROTECT_EXEC_SUPPORTED      (1ull << 13)
#define UHM_VM_WX_POLICY_RESTRICTED        (1ull << 14)
#define UHM_VM_CAN_NUMA_FIRST_TOUCH        (1ull << 15)
#define UHM_VM_CAN_NUMA_PREFERRED          (1ull << 16)
#define UHM_VM_CAN_NUMA_BIND               (1ull << 17)

#define UHM_NUMA_POLICY_DEFAULT            0u
#define UHM_NUMA_POLICY_FIRST_TOUCH        1u
#define UHM_NUMA_POLICY_PREFERRED          2u
#define UHM_NUMA_POLICY_BIND               3u

#define UHM_LOCKLESS_REMOTE_MODE_NONE                    0u
#define UHM_LOCKLESS_REMOTE_MODE_POINTER_TAGGED_LOCKFREE 1u
#define UHM_LOCKLESS_REMOTE_MODE_32BIT_NATIVE            2u
#define UHM_LOCKLESS_REMOTE_MODE_32BIT_LOW_LOCK          3u

#define UHM_CALL_ABI_C       0u
#define UHM_CALL_ABI_CDECL   1u

typedef struct uhm_abi_info {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    uint32_t pointer_size;
    uint32_t size_t_size;
    uint32_t atomic_pointer_lock_free;
    uint32_t reserved;
    uint32_t atomic_u64_lock_free;
    uint32_t tagged_freelist_supported;
    uint32_t tagged_freelist_tag_bits;
    uint32_t lockless_remote_primitives_supported;
    uint32_t lockless_remote_config_supported;
    uint32_t reserved2;
    uint32_t atomic_u32_lock_free;
    uint32_t malloc_min_alignment;
    uint32_t cacheline_size;
    uint32_t call_abi;
} uhm_abi_info;

#define UHM_ABI_INFO_SIZE_ABI9 \
    ((uint32_t)offsetof(uhm_abi_info, atomic_u64_lock_free))
#define UHM_ABI_INFO_SIZE_ABI17 ((uint32_t)sizeof(uhm_abi_info))

typedef struct uhm_storage_info {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    size_t storage_size;
    size_t storage_alignment;
} uhm_storage_info;

typedef struct uhm_vm_ops {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    void *user;
    size_t page_size;
    size_t allocation_granularity;
    uint64_t capabilities;

    uhm_result (UHM_CALL *reserve)(void *user, void **addr, size_t size, size_t alignment);
    uhm_result (UHM_CALL *commit)(void *user, void *addr, size_t size, uint32_t prot);
    uhm_result (UHM_CALL *decommit)(void *user, void *addr, size_t size);
    uhm_result (UHM_CALL *release)(void *user, void *addr, size_t size);
    uhm_result (UHM_CALL *protect)(void *user, void *addr, size_t size, uint32_t prot);
    uhm_result (UHM_CALL *fixed_remap)(void *user, void *addr, size_t size, uint32_t prot);
    uhm_result (UHM_CALL *numa_hint)(void *user, void *addr, size_t size,
                                     uint32_t policy, int32_t node,
                                     uint32_t flags);
} uhm_vm_ops;

typedef struct uhm_mmap_options {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    void *addr;
    size_t map_size;
    uint32_t prot;
    uint32_t map_flags;

    uint32_t numa_policy;
    int32_t numa_node;
} uhm_mmap_options;

typedef struct uhm_config {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    void *heap_storage;
    size_t heap_storage_size;

    void *arena_base;
    size_t arena_size;
    uint32_t arena_flags;

    uhm_vm_ops vm;

    size_t control_band_min_size;
    size_t metadata_commit_granularity;
    size_t mmap_reuse_cache_limit;
    size_t thread_registry_slots;

    size_t tls_cache_min_pages;
    size_t tls_cache_max_pages;
    size_t tls_cache_default_pages;

    uint32_t numa_policy;
    int32_t preferred_numa_node;

    size_t malloc_free_decommit_retention_pages;

    uint32_t spinlock_yield_after_spins;
} uhm_config;

#define UHM_CONFIG_SIZE_ABI7 \
    ((uint32_t)offsetof(uhm_config, malloc_free_decommit_retention_pages))
#define UHM_CONFIG_SIZE_ABI8 \
    ((uint32_t)offsetof(uhm_config, spinlock_yield_after_spins))

typedef struct uhm_thread_config {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    void *storage;
    size_t storage_size;
    size_t local_cache_bytes;
} uhm_thread_config;

typedef struct uhm_stats {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    size_t arena_size;
    size_t committed_bytes;
    size_t decommitted_bytes;
    size_t logical_decommitted_bytes;
    size_t physical_released_bytes;
    size_t malloc_live_bytes;
    size_t mmap_live_bytes;
    size_t control_bytes;
    size_t collision_free_bytes;
    uint32_t config_flags_enabled;
    uint32_t lockless_remote_free_enabled;
    uint32_t lockless_remote_malloc_enabled;
    uint32_t lockless_remote_mode;
    uint32_t reserved;
    uint64_t decommit_events;
    uint64_t decommit_bytes;
    uint64_t recommit_events;
    uint64_t recommit_bytes;
    uint64_t malloc_free_decommit_deferred_events;
    uint64_t malloc_free_decommit_deferred_bytes;
    uint64_t decommit_recommit_same_addr_events;
    uint64_t decommit_recommit_same_addr_bytes;
    uint64_t page_span_calls;
    uint64_t page_span_pages;
    uint64_t page_mark_calls;
    uint64_t page_mark_pages;
    uint64_t page_mark_max_pages;
    uint64_t page_mark_budget_exceeded;
    uint64_t page_mark_leaf_spans;
    uint64_t page_mark_max_leaf_spans;
    uint64_t page_range_query_calls;
    uint64_t page_range_query_pages;
    uint64_t page_range_query_max_pages;
    uint64_t page_range_query_budget_exceeded;
    uint64_t page_range_query_leaf_spans;
    uint64_t page_range_query_max_leaf_spans;
    uint64_t vm_reserve_calls;
    uint64_t vm_reserve_failures;
    uint64_t vm_reserve_ns;
    uint64_t vm_commit_calls;
    uint64_t vm_commit_failures;
    uint64_t vm_commit_ns;
    uint64_t vm_decommit_calls;
    uint64_t vm_decommit_failures;
    uint64_t vm_decommit_ns;
    uint64_t vm_fixed_remap_calls;
    uint64_t vm_fixed_remap_failures;
    uint64_t vm_fixed_remap_ns;
    uint64_t vm_protect_calls;
    uint64_t vm_protect_failures;
    uint64_t vm_protect_ns;
    uint64_t vm_release_calls;
    uint64_t vm_release_failures;
    uint64_t vm_release_ns;
    uint64_t vm_numa_hint_calls;
    uint64_t vm_numa_hint_failures;
    uint64_t vm_numa_hint_ns;
    uint64_t metadata_retired_vma_count;
    uint64_t metadata_retired_free_extent_count;
    uint64_t metadata_hot_vma_count;
    uint64_t metadata_hot_free_extent_count;
    uint64_t metadata_hot_vma_hits;
    uint64_t metadata_hot_vma_misses;
    uint64_t metadata_hot_vma_reclaimed;
    uint64_t metadata_hot_free_extent_hits;
    uint64_t metadata_hot_free_extent_misses;
    uint64_t metadata_hot_free_extent_reclaimed;
    uint64_t metadata_quarantine_drain_calls;
    uint64_t metadata_quarantine_drain_scanned;
    uint64_t metadata_quarantine_drain_reclaimed;
    uint64_t metadata_quarantine_drain_budget_exhausted;
    uint64_t page_lock_range_calls;
    uint64_t page_lock_range_shards;
    uint64_t page_lock_range_max_shards;
    uint64_t page_lock_range_budget_exceeded;
} uhm_stats;

#define UHM_STATS_SIZE_ABI10 \
    ((uint32_t)offsetof(uhm_stats, config_flags_enabled))
#define UHM_STATS_SIZE_ABI11 \
    ((uint32_t)offsetof(uhm_stats, decommit_events))
#define UHM_STATS_SIZE_ABI12 \
    ((uint32_t)offsetof(uhm_stats, page_span_calls))
#define UHM_STATS_SIZE_ABI13 \
    ((uint32_t)offsetof(uhm_stats, vm_reserve_calls))
#define UHM_STATS_SIZE_ABI14 \
    ((uint32_t)offsetof(uhm_stats, metadata_retired_vma_count))
#define UHM_STATS_SIZE_ABI15 \
    ((uint32_t)offsetof(uhm_stats, page_lock_range_calls))

#define UHM_LEAK_KIND_MALLOC           1u
#define UHM_LEAK_KIND_MMAP             2u

#define UHM_LEAK_FLAG_PAGE_PRIVATE     (1u << 0)

typedef struct uhm_leak_record {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    uint32_t kind;
    uint32_t prot;
    void *addr;
    size_t requested_size;
    size_t allocated_size;
    uint64_t generation;
} uhm_leak_record;

typedef uhm_result (UHM_CALL *uhm_leak_callback)(void *user,
                                                 const uhm_leak_record *record);

typedef struct uhm_api {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;
    uint64_t capabilities;

    void *(UHM_CALL *malloc)(uhm_heap *heap, size_t size);
    void *(UHM_CALL *calloc)(uhm_heap *heap, size_t count, size_t size);
    void *(UHM_CALL *realloc)(uhm_heap *heap, void *ptr, size_t new_size);
    void *(UHM_CALL *aligned_alloc)(uhm_heap *heap, size_t alignment, size_t size);
    void  (UHM_CALL *free)(uhm_heap *heap, void *ptr);
    size_t (UHM_CALL *malloc_usable_size)(uhm_heap *heap, const void *ptr);

    uhm_result (UHM_CALL *mmap)(uhm_heap *heap, void **addr_inout, size_t size,
                                uint32_t prot, uint32_t map_flags);
    uhm_result (UHM_CALL *munmap)(uhm_heap *heap, void *addr, size_t size);
    uhm_result (UHM_CALL *mprotect)(uhm_heap *heap, void *addr, size_t size,
                                    uint32_t prot);

    uhm_result (UHM_CALL *thread_ctx_open)(uhm_heap *heap,
                                           const uhm_thread_config *config,
                                           uhm_thread_ctx **out_ctx);
    void (UHM_CALL *thread_ctx_close)(uhm_heap *heap, uhm_thread_ctx *ctx);
    void *(UHM_CALL *malloc_ctx)(uhm_heap *heap, uhm_thread_ctx *ctx, size_t size);
    void (UHM_CALL *free_ctx)(uhm_heap *heap, uhm_thread_ctx *ctx, void *ptr);

    uhm_result (UHM_CALL *get_stats)(uhm_heap *heap, uhm_stats *out_stats);
    uhm_result (UHM_CALL *check)(uhm_heap *heap, uint32_t check_flags);
    uhm_result (UHM_CALL *close)(uhm_heap *heap);
    uhm_result (UHM_CALL *enumerate_leaks)(uhm_heap *heap,
                                           uhm_leak_callback callback,
                                           void *user);
    uhm_result (UHM_CALL *get_last_error)(uhm_heap *heap,
                                          uhm_result *out_error,
                                          uint32_t *out_platform_error);
    uhm_result (UHM_CALL *mmap_ex)(uhm_heap *heap, uhm_mmap_options *options);
    uhm_result (UHM_CALL *thread_ctx_flush)(uhm_heap *heap,
                                            uhm_thread_ctx *ctx);
    uhm_result (UHM_CALL *free_deferred_ctx)(uhm_heap *heap,
                                             uhm_thread_ctx *ctx,
                                             void *ptr);
    uhm_result (UHM_CALL *mremap)(uhm_heap *heap, void **addr_inout,
                                  size_t old_size, size_t new_size,
                                  uint32_t flags);
} uhm_api;

UHM_PUBLIC uhm_result UHM_CALL uhm_abi_query(uhm_abi_info *out);
UHM_PUBLIC uhm_result UHM_CALL uhm_heap_storage_query(uhm_storage_info *out_heap,
                                                      uhm_storage_info *out_thread_ctx);
UHM_PUBLIC uhm_result UHM_CALL uhm_open(const uhm_config *config, uhm_heap **out_heap);
UHM_PUBLIC const uhm_api *UHM_CALL uhm_get_api(uhm_heap *heap);

static inline void *uhm_malloc(uhm_heap *heap, size_t size) {
    const uhm_api *api = uhm_get_api(heap);
    return api ? api->malloc(heap, size) : (void *)0;
}

static inline void *uhm_calloc(uhm_heap *heap, size_t count, size_t size) {
    const uhm_api *api = uhm_get_api(heap);
    return api ? api->calloc(heap, count, size) : (void *)0;
}

static inline void *uhm_realloc(uhm_heap *heap, void *ptr, size_t new_size) {
    const uhm_api *api = uhm_get_api(heap);
    return api ? api->realloc(heap, ptr, new_size) : (void *)0;
}

static inline void *uhm_aligned_alloc(uhm_heap *heap, size_t alignment, size_t size) {
    const uhm_api *api = uhm_get_api(heap);
    return api ? api->aligned_alloc(heap, alignment, size) : (void *)0;
}

static inline void uhm_free(uhm_heap *heap, void *ptr) {
    const uhm_api *api = uhm_get_api(heap);
    if (api) {
        api->free(heap, ptr);
    }
}

static inline uhm_result uhm_mmap(uhm_heap *heap, void **addr_inout, size_t size,
                                  uint32_t prot, uint32_t map_flags) {
    const uhm_api *api = uhm_get_api(heap);
    return api ? api->mmap(heap, addr_inout, size, prot, map_flags) : UHM_ERR_INVALID_ARGUMENT;
}

static inline uhm_result uhm_mmap_ex(uhm_heap *heap,
                                     uhm_mmap_options *options) {
    const uhm_api *api = uhm_get_api(heap);
    return api && api->mmap_ex ? api->mmap_ex(heap, options)
                               : UHM_ERR_INVALID_ARGUMENT;
}

static inline uhm_result uhm_munmap(uhm_heap *heap, void *addr, size_t size) {
    const uhm_api *api = uhm_get_api(heap);
    return api ? api->munmap(heap, addr, size) : UHM_ERR_INVALID_ARGUMENT;
}

static inline uhm_result uhm_mremap(uhm_heap *heap, void **addr_inout,
                                    size_t old_size, size_t new_size,
                                    uint32_t flags) {
    const uhm_api *api = uhm_get_api(heap);
    return api && api->mremap
               ? api->mremap(heap, addr_inout, old_size, new_size, flags)
               : UHM_ERR_INVALID_ARGUMENT;
}

static inline uhm_result uhm_mprotect(uhm_heap *heap, void *addr, size_t size,
                                      uint32_t prot) {
    const uhm_api *api = uhm_get_api(heap);
    return api ? api->mprotect(heap, addr, size, prot) : UHM_ERR_INVALID_ARGUMENT;
}

static inline uhm_result uhm_thread_ctx_flush(uhm_heap *heap,
                                              uhm_thread_ctx *ctx) {
    const uhm_api *api = uhm_get_api(heap);
    return api && api->thread_ctx_flush
               ? api->thread_ctx_flush(heap, ctx)
               : UHM_ERR_INVALID_ARGUMENT;
}

static inline uhm_result uhm_free_deferred_ctx(uhm_heap *heap,
                                               uhm_thread_ctx *ctx,
                                               void *ptr) {
    const uhm_api *api = uhm_get_api(heap);
    return api && api->free_deferred_ctx
               ? api->free_deferred_ctx(heap, ctx, ptr)
               : UHM_ERR_INVALID_ARGUMENT;
}

static inline uhm_result uhm_enumerate_leaks(uhm_heap *heap,
                                             uhm_leak_callback callback,
                                             void *user) {
    const uhm_api *api = uhm_get_api(heap);
    return api && api->enumerate_leaks
               ? api->enumerate_leaks(heap, callback, user)
               : UHM_ERR_INVALID_ARGUMENT;
}

static inline uhm_result uhm_get_last_error_info(uhm_heap *heap,
                                                 uhm_result *out_error,
                                                 uint32_t *out_platform_error) {
    const uhm_api *api = uhm_get_api(heap);
    return api && api->get_last_error
               ? api->get_last_error(heap, out_error, out_platform_error)
               : UHM_ERR_INVALID_ARGUMENT;
}

#ifdef __cplusplus
}
#endif

#endif
