/*
 * UHMAS-HL: compact HeapLock implementation.
 *
 * This file intentionally keeps the public UHMAS ABI shape while replacing the
 * original fine-grained/lock-free internals with one heap-wide lock.
 */

#include "uhm/uhm.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif

#define UHM_HL_MAGIC 0x55484c48u
#define UHM_HL_CLOSED_MAGIC 0x55484c58u
#define UHM_HL_THREAD_MAGIC 0x55484c54u
#define UHM_HL_MIN_ALIGNMENT 16u
#define UHM_HL_DEFAULT_PAGE_SIZE 4096u
#define UHM_HL_MAX_REGIONS 8192u
#define UHM_HL_SPINLOCK_MODE_BURN 0u
#define UHM_HL_SPINLOCK_MODE_YIELD 1u
#define UHM_HL_SPINLOCK_DEFAULT_YIELD_AFTER 256u

typedef struct uhm_hl_lock {
    atomic_flag flag;
    uint32_t mode;
    uint32_t yield_after_spins;
} uhm_hl_lock;

typedef enum uhm_hl_region_kind {
    UHM_HL_REGION_UNUSED = 0,
    UHM_HL_REGION_MALLOC = 1,
    UHM_HL_REGION_MALLOC_FREE = 2,
    UHM_HL_REGION_MMAP = 3,
    UHM_HL_REGION_MMAP_FREE = 4
} uhm_hl_region_kind;

typedef struct uhm_hl_region {
    uint32_t kind;
    uint32_t prot;
    uint32_t flags;
    uint32_t committed;
    uintptr_t addr;
    size_t size;
    size_t requested_size;
    uint64_t generation;
} uhm_hl_region;

struct uhm_heap {
    uint32_t magic;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t state_flags;
    uhm_hl_lock lock;
    uhm_api api;
    uhm_vm_ops vm;
    uintptr_t arena_base;
    uintptr_t arena_end;
    uintptr_t malloc_hi;
    uintptr_t mmap_lo;
    size_t arena_size;
    int owns_arena;
    int closing;
    uint64_t generation;
    uhm_result last_error;
    uint32_t last_platform_error;
    size_t committed_bytes;
    size_t decommitted_bytes;
    size_t malloc_live_bytes;
    size_t mmap_live_bytes;
    uint64_t vm_reserve_calls;
    uint64_t vm_reserve_failures;
    uint64_t vm_commit_calls;
    uint64_t vm_commit_failures;
    uint64_t vm_decommit_calls;
    uint64_t vm_decommit_failures;
    uint64_t vm_release_calls;
    uint64_t vm_release_failures;
    uint64_t vm_protect_calls;
    uint64_t vm_protect_failures;
    uhm_hl_region regions[UHM_HL_MAX_REGIONS];
};

struct uhm_thread_ctx {
    uint32_t magic;
    uhm_heap *heap;
};

static void uhm_hl_yield(void) {
#if defined(_WIN32)
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

static void uhm_hl_lock_init(uhm_hl_lock *lock, uint32_t mode,
                             uint32_t yield_after_spins) {
    lock->mode = mode;
    lock->yield_after_spins =
        yield_after_spins != 0u ? yield_after_spins
                                : UHM_HL_SPINLOCK_DEFAULT_YIELD_AFTER;
    atomic_flag_clear(&lock->flag);
}

static void uhm_hl_lock_acquire(uhm_hl_lock *lock) {
    uint32_t spins = 0u;
    uint32_t yield_after = lock->yield_after_spins != 0u
                               ? lock->yield_after_spins
                               : UHM_HL_SPINLOCK_DEFAULT_YIELD_AFTER;
    while (atomic_flag_test_and_set_explicit(&lock->flag,
                                             memory_order_acquire)) {
        if (lock->mode == UHM_HL_SPINLOCK_MODE_YIELD &&
            ++spins >= yield_after) {
            spins = 0u;
            uhm_hl_yield();
        }
    }
}

static void uhm_hl_lock_release(uhm_hl_lock *lock) {
    atomic_flag_clear_explicit(&lock->flag, memory_order_release);
}

static int uhm_hl_is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static int uhm_hl_align_up_size(size_t value, size_t alignment, size_t *out) {
    size_t mask;
    if (!out || !uhm_hl_is_power_of_two(alignment)) {
        return 1;
    }
    mask = alignment - 1u;
    if (value > SIZE_MAX - mask) {
        return 1;
    }
    *out = (value + mask) & ~mask;
    return 0;
}

static int uhm_hl_align_up_uintptr(uintptr_t value, size_t alignment,
                                   uintptr_t *out) {
    uintptr_t mask;
    if (!out || !uhm_hl_is_power_of_two(alignment) ||
        alignment > (size_t)UINTPTR_MAX) {
        return 1;
    }
    mask = (uintptr_t)alignment - 1u;
    if (value > UINTPTR_MAX - mask) {
        return 1;
    }
    *out = (value + mask) & ~mask;
    return 0;
}

static uintptr_t uhm_hl_align_down_uintptr(uintptr_t value, size_t alignment) {
    return value & ~((uintptr_t)alignment - 1u);
}

static int uhm_hl_range_end(uintptr_t addr, size_t size, uintptr_t *end) {
    if (!end || size > (size_t)(UINTPTR_MAX - addr)) {
        return 1;
    }
    *end = addr + size;
    return 0;
}

static void uhm_hl_set_error(uhm_heap *heap, uhm_result error) {
    if (heap) {
        heap->last_error = error;
    }
}

static int uhm_hl_heap_live(const uhm_heap *heap) {
    return heap && heap->magic == UHM_HL_MAGIC && !heap->closing;
}

static size_t uhm_hl_page_size(const uhm_heap *heap) {
    return heap && heap->vm.page_size != 0 ? heap->vm.page_size
                                           : (size_t)UHM_HL_DEFAULT_PAGE_SIZE;
}

static uhm_result uhm_hl_vm_reserve(uhm_heap *heap, void **addr, size_t size,
                                    size_t alignment) {
    uhm_result result;
    if (!heap || !heap->vm.reserve) {
        return UHM_ERR_UNSUPPORTED;
    }
    ++heap->vm_reserve_calls;
    result = heap->vm.reserve(heap->vm.user, addr, size, alignment);
    if (result != UHM_OK) {
        ++heap->vm_reserve_failures;
        uhm_hl_set_error(heap, result);
    }
    return result;
}

static uhm_result uhm_hl_vm_commit(uhm_heap *heap, void *addr, size_t size,
                                   uint32_t prot) {
    uhm_result result;
    if (size == 0) {
        return UHM_OK;
    }
    if (!heap || !heap->vm.commit) {
        return UHM_ERR_UNSUPPORTED;
    }
    ++heap->vm_commit_calls;
    result = heap->vm.commit(heap->vm.user, addr, size, prot);
    if (result != UHM_OK) {
        ++heap->vm_commit_failures;
        uhm_hl_set_error(heap, result);
    }
    return result;
}

static uhm_result uhm_hl_vm_decommit(uhm_heap *heap, void *addr, size_t size) {
    uhm_result result;
    if (size == 0 || !heap->vm.decommit) {
        return UHM_OK;
    }
    ++heap->vm_decommit_calls;
    result = heap->vm.decommit(heap->vm.user, addr, size);
    if (result != UHM_OK) {
        ++heap->vm_decommit_failures;
        uhm_hl_set_error(heap, result);
    }
    return result;
}

static uhm_result uhm_hl_vm_release(uhm_heap *heap, void *addr, size_t size) {
    uhm_result result;
    if (!heap || !heap->vm.release) {
        return UHM_ERR_UNSUPPORTED;
    }
    ++heap->vm_release_calls;
    result = heap->vm.release(heap->vm.user, addr, size);
    if (result != UHM_OK) {
        ++heap->vm_release_failures;
        uhm_hl_set_error(heap, result);
    }
    return result;
}

static uhm_result uhm_hl_vm_protect(uhm_heap *heap, void *addr, size_t size,
                                    uint32_t prot) {
    uhm_result result;
    if (!heap || !heap->vm.protect) {
        return UHM_ERR_UNSUPPORTED;
    }
    ++heap->vm_protect_calls;
    result = heap->vm.protect(heap->vm.user, addr, size, prot);
    if (result != UHM_OK) {
        ++heap->vm_protect_failures;
        uhm_hl_set_error(heap, result);
    }
    return result;
}

static uhm_hl_region *uhm_hl_region_new(uhm_heap *heap) {
    size_t i;
    for (i = 0; i < UHM_HL_MAX_REGIONS; ++i) {
        if (heap->regions[i].kind == UHM_HL_REGION_UNUSED) {
            memset(&heap->regions[i], 0, sizeof(heap->regions[i]));
            return &heap->regions[i];
        }
    }
    return 0;
}

static int uhm_hl_region_live(const uhm_hl_region *region) {
    return region && (region->kind == UHM_HL_REGION_MALLOC ||
                      region->kind == UHM_HL_REGION_MMAP);
}

static int uhm_hl_region_overlaps(const uhm_hl_region *region, uintptr_t addr,
                                  size_t size) {
    uintptr_t end;
    uintptr_t region_end;
    if (!uhm_hl_region_live(region) ||
        uhm_hl_range_end(addr, size, &end) ||
        uhm_hl_range_end(region->addr, region->size, &region_end)) {
        return 0;
    }
    return addr < region_end && region->addr < end;
}

static int uhm_hl_any_live_overlap(uhm_heap *heap, uintptr_t addr, size_t size) {
    size_t i;
    for (i = 0; i < UHM_HL_MAX_REGIONS; ++i) {
        if (uhm_hl_region_overlaps(&heap->regions[i], addr, size)) {
            return 1;
        }
    }
    return 0;
}

static uhm_hl_region *uhm_hl_find_malloc(uhm_heap *heap, const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    size_t i;
    for (i = 0; i < UHM_HL_MAX_REGIONS; ++i) {
        if (heap->regions[i].kind == UHM_HL_REGION_MALLOC &&
            heap->regions[i].addr == addr) {
            return &heap->regions[i];
        }
    }
    return 0;
}

static uhm_hl_region *uhm_hl_find_mmap_covering(uhm_heap *heap, uintptr_t addr,
                                                size_t size) {
    uintptr_t end;
    size_t i;
    if (uhm_hl_range_end(addr, size, &end)) {
        return 0;
    }
    for (i = 0; i < UHM_HL_MAX_REGIONS; ++i) {
        uhm_hl_region *region = &heap->regions[i];
        uintptr_t region_end;
        if (region->kind != UHM_HL_REGION_MMAP ||
            uhm_hl_range_end(region->addr, region->size, &region_end)) {
            continue;
        }
        if (region->addr <= addr && end <= region_end) {
            return region;
        }
    }
    return 0;
}

static uhm_result uhm_hl_region_commit(uhm_heap *heap, uhm_hl_region *region,
                                       uint32_t prot) {
    uhm_result result;
    if (!region->committed) {
        result = uhm_hl_vm_commit(heap, (void *)region->addr, region->size,
                                  prot);
        if (result != UHM_OK) {
            return result;
        }
        region->committed = 1u;
        heap->committed_bytes += region->size;
    }
    return UHM_OK;
}

static void uhm_hl_region_decommit_best_effort(uhm_heap *heap,
                                               uhm_hl_region *region) {
    if (region->committed) {
        if (uhm_hl_vm_decommit(heap, (void *)region->addr, region->size) ==
            UHM_OK) {
            region->committed = 0u;
            heap->decommitted_bytes += region->size;
            if (heap->committed_bytes >= region->size) {
                heap->committed_bytes -= region->size;
            } else {
                heap->committed_bytes = 0;
            }
        }
    }
}

static void uhm_hl_fill_common_prefix(uint32_t requested_size,
                                      uint16_t abi_minor, void *out) {
    uint32_t *size_field = (uint32_t *)out;
    uint16_t *major_field = (uint16_t *)((unsigned char *)out + 4u);
    uint16_t *minor_field = (uint16_t *)((unsigned char *)out + 6u);
    uint32_t *flags_field = (uint32_t *)((unsigned char *)out + 8u);
    (void)requested_size;
    *size_field = requested_size;
    *major_field = (uint16_t)UHM_ABI_MAJOR;
    *minor_field = abi_minor;
    *flags_field = 0u;
}

static uhm_result UHM_CALL uhm_api_get_stats(uhm_heap *heap,
                                             uhm_stats *out_stats);
static uhm_result UHM_CALL uhm_api_check(uhm_heap *heap, uint32_t check_flags);
static uhm_result UHM_CALL uhm_api_close(uhm_heap *heap);
static uhm_result UHM_CALL uhm_api_enumerate_leaks(uhm_heap *heap,
                                                   uhm_leak_callback callback,
                                                   void *user);
static uhm_result UHM_CALL uhm_api_get_last_error(uhm_heap *heap,
                                                  uhm_result *out_error,
                                                  uint32_t *out_platform_error);
static void *UHM_CALL uhm_api_malloc(uhm_heap *heap, size_t size);
static void *UHM_CALL uhm_api_calloc(uhm_heap *heap, size_t count,
                                     size_t size);
static void *UHM_CALL uhm_api_realloc(uhm_heap *heap, void *ptr,
                                      size_t new_size);
static void *UHM_CALL uhm_api_aligned_alloc(uhm_heap *heap, size_t alignment,
                                            size_t size);
static void UHM_CALL uhm_api_free(uhm_heap *heap, void *ptr);
static size_t UHM_CALL uhm_api_malloc_usable_size(uhm_heap *heap,
                                                  const void *ptr);
static uhm_result UHM_CALL uhm_api_mmap(uhm_heap *heap, void **addr_inout,
                                        size_t size, uint32_t prot,
                                        uint32_t map_flags);
static uhm_result UHM_CALL uhm_api_mmap_ex(uhm_heap *heap,
                                           uhm_mmap_options *options);
static uhm_result UHM_CALL uhm_api_munmap(uhm_heap *heap, void *addr,
                                          size_t size);
static uhm_result UHM_CALL uhm_api_mprotect(uhm_heap *heap, void *addr,
                                            size_t size, uint32_t prot);
static uhm_result UHM_CALL uhm_api_mremap(uhm_heap *heap, void **addr_inout,
                                          size_t old_size, size_t new_size,
                                          uint32_t flags);
static uhm_result UHM_CALL uhm_api_thread_ctx_open(
    uhm_heap *heap, const uhm_thread_config *config, uhm_thread_ctx **out_ctx);
static void UHM_CALL uhm_api_thread_ctx_close(uhm_heap *heap,
                                              uhm_thread_ctx *ctx);
static uhm_result UHM_CALL uhm_api_thread_ctx_flush(uhm_heap *heap,
                                                    uhm_thread_ctx *ctx);
static void *UHM_CALL uhm_api_malloc_ctx(uhm_heap *heap, uhm_thread_ctx *ctx,
                                         size_t size);
static void UHM_CALL uhm_api_free_ctx(uhm_heap *heap, uhm_thread_ctx *ctx,
                                      void *ptr);
static uhm_result UHM_CALL uhm_api_free_deferred_ctx(uhm_heap *heap,
                                                     uhm_thread_ctx *ctx,
                                                     void *ptr);

static void uhm_hl_fill_api(uhm_api *api) {
    memset(api, 0, sizeof(*api));
    api->size = sizeof(*api);
    api->abi_major = UHM_ABI_MAJOR;
    api->abi_minor = UHM_ABI_MINOR;
    api->malloc = uhm_api_malloc;
    api->calloc = uhm_api_calloc;
    api->realloc = uhm_api_realloc;
    api->aligned_alloc = uhm_api_aligned_alloc;
    api->free = uhm_api_free;
    api->malloc_usable_size = uhm_api_malloc_usable_size;
    api->mmap = uhm_api_mmap;
    api->munmap = uhm_api_munmap;
    api->mprotect = uhm_api_mprotect;
    api->thread_ctx_open = uhm_api_thread_ctx_open;
    api->thread_ctx_close = uhm_api_thread_ctx_close;
    api->malloc_ctx = uhm_api_malloc_ctx;
    api->free_ctx = uhm_api_free_ctx;
    api->get_stats = uhm_api_get_stats;
    api->check = uhm_api_check;
    api->close = uhm_api_close;
    api->enumerate_leaks = uhm_api_enumerate_leaks;
    api->get_last_error = uhm_api_get_last_error;
    api->mmap_ex = uhm_api_mmap_ex;
    api->thread_ctx_flush = uhm_api_thread_ctx_flush;
    api->free_deferred_ctx = uhm_api_free_deferred_ctx;
    api->mremap = uhm_api_mremap;
}

uhm_result UHM_CALL uhm_abi_query(uhm_abi_info *out) {
    uhm_abi_info temp;
    uint32_t requested_size;
    if (!out || out->size < UHM_ABI_INFO_SIZE_ABI9) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    requested_size = out->size;
    memset(&temp, 0, sizeof(temp));
    temp.size = sizeof(temp);
    temp.abi_major = UHM_ABI_MAJOR;
    temp.abi_minor = UHM_ABI_MINOR;
    temp.pointer_size = (uint32_t)sizeof(void *);
    temp.size_t_size = (uint32_t)sizeof(size_t);
    temp.atomic_pointer_lock_free = ATOMIC_POINTER_LOCK_FREE;
    temp.atomic_u64_lock_free = 0u;
    temp.tagged_freelist_supported = 0u;
    temp.tagged_freelist_tag_bits = 0u;
    temp.lockless_remote_primitives_supported = 0u;
    temp.lockless_remote_config_supported = 0u;
    temp.atomic_u32_lock_free = ATOMIC_INT_LOCK_FREE == 2 ? 1u : 0u;
    temp.malloc_min_alignment = UHM_HL_MIN_ALIGNMENT;
    temp.cacheline_size = 64u;
#if defined(_WIN32)
    temp.call_abi = UHM_CALL_ABI_CDECL;
#else
    temp.call_abi = UHM_CALL_ABI_C;
#endif
    memcpy(out, &temp, requested_size < sizeof(temp) ? requested_size
                                                     : sizeof(temp));
    return UHM_OK;
}

uhm_result UHM_CALL uhm_heap_storage_query(uhm_storage_info *out_heap,
                                           uhm_storage_info *out_thread_ctx) {
    if ((out_heap && out_heap->size < sizeof(*out_heap)) ||
        (out_thread_ctx && out_thread_ctx->size < sizeof(*out_thread_ctx))) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (out_heap) {
        uhm_hl_fill_common_prefix(sizeof(*out_heap), UHM_ABI_MINOR, out_heap);
        out_heap->storage_size = sizeof(uhm_heap);
        out_heap->storage_alignment = alignof(uhm_heap);
    }
    if (out_thread_ctx) {
        uhm_hl_fill_common_prefix(sizeof(*out_thread_ctx), UHM_ABI_MINOR,
                                  out_thread_ctx);
        out_thread_ctx->storage_size = sizeof(uhm_thread_ctx);
        out_thread_ctx->storage_alignment = alignof(uhm_thread_ctx);
    }
    return UHM_OK;
}

uhm_result UHM_CALL uhm_open(const uhm_config *config, uhm_heap **out_heap) {
    uhm_heap *heap;
    uintptr_t arena_base;
    uintptr_t arena_end;
    size_t arena_size;
    size_t page_size;
    uint32_t spinlock_mode;
    uint32_t spinlock_yield_after_spins;
    uint32_t known_flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE |
                           UHM_CONFIG_SPINLOCK_BURN |
                           UHM_CONFIG_SPINLOCK_YIELD |
                           UHM_CONFIG_FAIL_ON_TREAP_HEALTH |
                           UHM_CONFIG_REPAIR_TREAP_HEALTH;
    uhm_result result;

    if (!config || !out_heap || config->size < UHM_CONFIG_SIZE_ABI7 ||
        config->abi_major != UHM_ABI_MAJOR || !config->heap_storage ||
        config->heap_storage_size < sizeof(uhm_heap) ||
        ((uintptr_t)config->heap_storage % alignof(uhm_heap)) != 0 ||
        (config->flags & ~known_flags) != 0 ||
        ((config->flags & UHM_CONFIG_SPINLOCK_BURN) != 0 &&
         (config->flags & UHM_CONFIG_SPINLOCK_YIELD) != 0) ||
        ((config->flags & UHM_CONFIG_FAIL_ON_TREAP_HEALTH) != 0 &&
         (config->flags & UHM_CONFIG_REPAIR_TREAP_HEALTH) != 0)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    *out_heap = 0;
    if (config->arena_size == 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    spinlock_mode = (config->flags & UHM_CONFIG_SPINLOCK_BURN) != 0
                        ? UHM_HL_SPINLOCK_MODE_BURN
                        : UHM_HL_SPINLOCK_MODE_YIELD;
    spinlock_yield_after_spins =
        config->size >= sizeof(*config) &&
                config->spinlock_yield_after_spins != 0u
            ? config->spinlock_yield_after_spins
            : UHM_HL_SPINLOCK_DEFAULT_YIELD_AFTER;

    heap = (uhm_heap *)config->heap_storage;
    memset(heap, 0, sizeof(*heap));
    heap->magic = UHM_HL_MAGIC;
    heap->abi_major = UHM_ABI_MAJOR;
    heap->abi_minor = UHM_ABI_MINOR;
    heap->state_flags = 0u;
    heap->vm = config->vm;
    page_size = heap->vm.page_size != 0 ? heap->vm.page_size
                                        : UHM_HL_DEFAULT_PAGE_SIZE;
    if (!uhm_hl_is_power_of_two(page_size)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    heap->vm.page_size = page_size;
    if (heap->vm.allocation_granularity == 0) {
        heap->vm.allocation_granularity = page_size;
    }
    if (!heap->vm.commit) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (uhm_hl_align_up_size(config->arena_size, page_size, &arena_size)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    arena_base = (uintptr_t)config->arena_base;
    if (arena_base == 0) {
        void *reserved = 0;
        result = uhm_hl_vm_reserve(heap, &reserved, arena_size,
                                   heap->vm.allocation_granularity);
        if (result != UHM_OK) {
            heap->magic = 0;
            return result;
        }
        arena_base = (uintptr_t)reserved;
        heap->owns_arena = 1;
    }
    if ((arena_base % page_size) != 0 ||
        uhm_hl_range_end(arena_base, arena_size, &arena_end)) {
        if (heap->owns_arena) {
            (void)uhm_hl_vm_release(heap, (void *)arena_base, arena_size);
        }
        heap->magic = 0;
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_init(&heap->lock, spinlock_mode, spinlock_yield_after_spins);
    uhm_hl_fill_api(&heap->api);
    heap->arena_base = arena_base;
    heap->arena_end = arena_end;
    heap->arena_size = arena_size;
    heap->malloc_hi = arena_base;
    heap->mmap_lo = arena_end;
    *out_heap = heap;
    return UHM_OK;
}

const uhm_api *UHM_CALL uhm_get_api(uhm_heap *heap) {
    return uhm_hl_heap_live(heap) ? &heap->api : 0;
}

static void *UHM_CALL uhm_api_aligned_alloc(uhm_heap *heap, size_t alignment,
                                            size_t size) {
    size_t page_size;
    size_t alloc_size;
    uintptr_t addr;
    uintptr_t end;
    uhm_hl_region *region;
    uhm_result result;

    if (!uhm_hl_heap_live(heap)) {
        return 0;
    }
    if (size == 0) {
        size = 1u;
    }
    if (alignment < UHM_HL_MIN_ALIGNMENT) {
        alignment = UHM_HL_MIN_ALIGNMENT;
    }
    if (!uhm_hl_is_power_of_two(alignment)) {
        uhm_hl_set_error(heap, UHM_ERR_INVALID_ARGUMENT);
        return 0;
    }
    uhm_hl_lock_acquire(&heap->lock);
    page_size = uhm_hl_page_size(heap);
    if (alignment < page_size) {
        alignment = page_size;
    }
    if (uhm_hl_align_up_size(size, page_size, &alloc_size)) {
        uhm_hl_set_error(heap, UHM_ERR_OUT_OF_MEMORY);
        uhm_hl_lock_release(&heap->lock);
        return 0;
    }
    for (region = heap->regions; region != heap->regions + UHM_HL_MAX_REGIONS;
         ++region) {
        if (region->kind == UHM_HL_REGION_MALLOC_FREE &&
            region->size >= alloc_size &&
            (region->addr % alignment) == 0) {
            result = uhm_hl_region_commit(heap, region,
                                          UHM_PROT_READ | UHM_PROT_WRITE);
            if (result != UHM_OK) {
                uhm_hl_lock_release(&heap->lock);
                return 0;
            }
            region->kind = UHM_HL_REGION_MALLOC;
            region->requested_size = size;
            region->generation = ++heap->generation;
            heap->malloc_live_bytes += size;
            addr = region->addr;
            uhm_hl_lock_release(&heap->lock);
            return (void *)addr;
        }
    }
    if (uhm_hl_align_up_uintptr(heap->malloc_hi, alignment, &addr) ||
        uhm_hl_range_end(addr, alloc_size, &end)) {
        uhm_hl_set_error(heap, UHM_ERR_OUT_OF_ADDRESS_SPACE);
        uhm_hl_lock_release(&heap->lock);
        return 0;
    }
    if (end > heap->mmap_lo) {
        uhm_hl_set_error(heap, UHM_ERR_OUT_OF_MEMORY);
        uhm_hl_lock_release(&heap->lock);
        return 0;
    }
    region = uhm_hl_region_new(heap);
    if (!region) {
        uhm_hl_set_error(heap, UHM_ERR_OUT_OF_MEMORY);
        uhm_hl_lock_release(&heap->lock);
        return 0;
    }
    region->kind = UHM_HL_REGION_MALLOC;
    region->addr = addr;
    region->size = alloc_size;
    region->requested_size = size;
    region->prot = UHM_PROT_READ | UHM_PROT_WRITE;
    region->generation = ++heap->generation;
    result = uhm_hl_region_commit(heap, region, region->prot);
    if (result != UHM_OK) {
        memset(region, 0, sizeof(*region));
        uhm_hl_lock_release(&heap->lock);
        return 0;
    }
    heap->malloc_hi = end;
    heap->malloc_live_bytes += size;
    uhm_hl_lock_release(&heap->lock);
    return (void *)addr;
}

static void *UHM_CALL uhm_api_malloc(uhm_heap *heap, size_t size) {
    return uhm_api_aligned_alloc(heap, UHM_HL_MIN_ALIGNMENT, size);
}

static void *UHM_CALL uhm_api_calloc(uhm_heap *heap, size_t count,
                                     size_t size) {
    size_t total;
    void *ptr;
    if (count != 0 && size > SIZE_MAX / count) {
        uhm_hl_set_error(heap, UHM_ERR_OUT_OF_MEMORY);
        return 0;
    }
    total = count * size;
    ptr = uhm_api_malloc(heap, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

static void UHM_CALL uhm_api_free(uhm_heap *heap, void *ptr) {
    uhm_hl_region *region;
    if (!ptr || !uhm_hl_heap_live(heap)) {
        return;
    }
    uhm_hl_lock_acquire(&heap->lock);
    region = uhm_hl_find_malloc(heap, ptr);
    if (!region) {
        uhm_hl_set_error(heap, UHM_ERR_INVALID_ARGUMENT);
        uhm_hl_lock_release(&heap->lock);
        return;
    }
    if (heap->malloc_live_bytes >= region->requested_size) {
        heap->malloc_live_bytes -= region->requested_size;
    } else {
        heap->malloc_live_bytes = 0;
    }
    uhm_hl_region_decommit_best_effort(heap, region);
    region->kind = UHM_HL_REGION_MALLOC_FREE;
    region->requested_size = 0;
    uhm_hl_lock_release(&heap->lock);
}

static size_t UHM_CALL uhm_api_malloc_usable_size(uhm_heap *heap,
                                                  const void *ptr) {
    uhm_hl_region *region;
    size_t usable = 0;
    if (!ptr || !uhm_hl_heap_live(heap)) {
        return 0;
    }
    uhm_hl_lock_acquire(&heap->lock);
    region = uhm_hl_find_malloc(heap, ptr);
    if (region) {
        usable = region->size;
    }
    uhm_hl_lock_release(&heap->lock);
    return usable;
}

static void *UHM_CALL uhm_api_realloc(uhm_heap *heap, void *ptr,
                                      size_t new_size) {
    uhm_hl_region snapshot;
    void *new_ptr;
    size_t copy_size;
    if (!ptr) {
        return uhm_api_malloc(heap, new_size);
    }
    if (new_size == 0) {
        uhm_api_free(heap, ptr);
        return 0;
    }
    if (!uhm_hl_heap_live(heap)) {
        return 0;
    }
    uhm_hl_lock_acquire(&heap->lock);
    {
        uhm_hl_region *region = uhm_hl_find_malloc(heap, ptr);
        if (!region) {
            uhm_hl_set_error(heap, UHM_ERR_INVALID_ARGUMENT);
            uhm_hl_lock_release(&heap->lock);
            return 0;
        }
        if (new_size <= region->size) {
            if (heap->malloc_live_bytes >= region->requested_size) {
                heap->malloc_live_bytes -= region->requested_size;
            }
            region->requested_size = new_size;
            heap->malloc_live_bytes += new_size;
            uhm_hl_lock_release(&heap->lock);
            return ptr;
        }
        snapshot = *region;
    }
    uhm_hl_lock_release(&heap->lock);
    new_ptr = uhm_api_malloc(heap, new_size);
    if (!new_ptr) {
        return 0;
    }
    copy_size = snapshot.requested_size < new_size ? snapshot.requested_size
                                                   : new_size;
    memcpy(new_ptr, ptr, copy_size);
    uhm_api_free(heap, ptr);
    return new_ptr;
}

static uhm_result uhm_hl_mmap_take_free(uhm_heap *heap, size_t size,
                                        uintptr_t *addr_out) {
    uhm_hl_region *region;
    for (region = heap->regions; region != heap->regions + UHM_HL_MAX_REGIONS;
         ++region) {
        if (region->kind == UHM_HL_REGION_MMAP_FREE && region->size >= size) {
            *addr_out = region->addr;
            if (region->size == size) {
                memset(region, 0, sizeof(*region));
            } else {
                region->addr += size;
                region->size -= size;
            }
            return UHM_OK;
        }
    }
    return UHM_ERR_NOT_MAPPED;
}

static void uhm_hl_mmap_forget_free_range(uhm_heap *heap, uintptr_t addr,
                                          size_t size) {
    uintptr_t end;
    size_t i;
    if (uhm_hl_range_end(addr, size, &end)) {
        return;
    }
    for (i = 0; i < UHM_HL_MAX_REGIONS; ++i) {
        uhm_hl_region *region = &heap->regions[i];
        uintptr_t region_end;
        if (region->kind != UHM_HL_REGION_MMAP_FREE ||
            uhm_hl_range_end(region->addr, region->size, &region_end) ||
            end <= region->addr || addr >= region_end) {
            continue;
        }
        if (addr <= region->addr && end >= region_end) {
            memset(region, 0, sizeof(*region));
        } else if (addr <= region->addr) {
            region->addr = end;
            region->size = (size_t)(region_end - end);
        } else if (end >= region_end) {
            region->size = (size_t)(addr - region->addr);
        } else {
            uhm_hl_region *tail = uhm_hl_region_new(heap);
            if (tail) {
                tail->kind = UHM_HL_REGION_MMAP_FREE;
                tail->addr = end;
                tail->size = (size_t)(region_end - end);
            }
            region->size = (size_t)(addr - region->addr);
        }
    }
}

static uhm_result uhm_hl_mmap_publish(uhm_heap *heap, uintptr_t addr,
                                      size_t size, uint32_t prot,
                                      uint32_t map_flags) {
    uhm_hl_region *region = uhm_hl_region_new(heap);
    uhm_result result = UHM_OK;
    if (!region) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    region->kind = UHM_HL_REGION_MMAP;
    region->addr = addr;
    region->size = size;
    region->requested_size = size;
    region->prot = prot;
    region->flags = map_flags;
    region->generation = ++heap->generation;
    if ((map_flags & UHM_MAP_NO_COMMIT) == 0) {
        result = uhm_hl_region_commit(heap, region, prot);
        if (result != UHM_OK) {
            memset(region, 0, sizeof(*region));
            return result;
        }
    }
    heap->mmap_live_bytes += size;
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_mmap(uhm_heap *heap, void **addr_inout,
                                        size_t size, uint32_t prot,
                                        uint32_t map_flags) {
    size_t rounded;
    size_t page_size;
    uintptr_t addr = 0;
    uintptr_t end = 0;
    uintptr_t old_mmap_lo;
    uhm_result result;
    uint32_t known_flags = UHM_MAP_PRIVATE | UHM_MAP_FIXED |
                           UHM_MAP_FIXED_NOREPLACE | UHM_MAP_NO_COMMIT |
                           UHM_MAP_PAGE_PRIVATE;

    if (!uhm_hl_heap_live(heap) || !addr_inout ||
        (map_flags & ~known_flags) != 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_acquire(&heap->lock);
    page_size = uhm_hl_page_size(heap);
    if (uhm_hl_align_up_size(size, page_size, &rounded) || rounded == 0) {
        uhm_hl_lock_release(&heap->lock);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if ((map_flags & (UHM_MAP_FIXED | UHM_MAP_FIXED_NOREPLACE)) != 0) {
        addr = (uintptr_t)*addr_inout;
        if (addr == 0 || (addr % page_size) != 0 ||
            uhm_hl_range_end(addr, rounded, &end) ||
            addr < heap->malloc_hi || end > heap->arena_end) {
            uhm_hl_lock_release(&heap->lock);
            return (map_flags & UHM_MAP_FIXED_NOREPLACE)
                       ? UHM_ERR_ALREADY_MAPPED
                       : UHM_ERR_PROTECTION;
        }
        if (uhm_hl_any_live_overlap(heap, addr, rounded)) {
            uhm_hl_region *covering;
            if ((map_flags & UHM_MAP_FIXED_NOREPLACE) != 0) {
                uhm_hl_lock_release(&heap->lock);
                return UHM_ERR_ALREADY_MAPPED;
            }
            covering = uhm_hl_find_mmap_covering(heap, addr, rounded);
            if (!covering) {
                uhm_hl_lock_release(&heap->lock);
                return UHM_ERR_PROTECTION;
            }
            if (covering->committed) {
                result = uhm_hl_vm_decommit(heap, (void *)addr, rounded);
                if (result != UHM_OK) {
                    uhm_hl_lock_release(&heap->lock);
                    return result;
                }
                result = uhm_hl_vm_commit(heap, (void *)addr, rounded, prot);
                if (result != UHM_OK) {
                    uhm_hl_lock_release(&heap->lock);
                    return result;
                }
            }
            covering->prot = prot;
            covering->flags = map_flags;
            *addr_inout = (void *)addr;
            uhm_hl_lock_release(&heap->lock);
            return UHM_OK;
        }
        if (addr < heap->mmap_lo) {
            heap->mmap_lo = addr;
        }
        uhm_hl_mmap_forget_free_range(heap, addr, rounded);
    } else {
        result = uhm_hl_mmap_take_free(heap, rounded, &addr);
        if (result != UHM_OK) {
            old_mmap_lo = heap->mmap_lo;
            addr = uhm_hl_align_down_uintptr(old_mmap_lo - rounded, page_size);
            if (addr > old_mmap_lo || addr < heap->malloc_hi) {
                uhm_hl_lock_release(&heap->lock);
                return UHM_ERR_OUT_OF_MEMORY;
            }
            heap->mmap_lo = addr;
        }
    }
    result = uhm_hl_mmap_publish(heap, addr, rounded, prot, map_flags);
    if (result != UHM_OK) {
        uhm_hl_set_error(heap, result);
        uhm_hl_lock_release(&heap->lock);
        return result;
    }
    *addr_inout = (void *)addr;
    uhm_hl_lock_release(&heap->lock);
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_mmap_ex(uhm_heap *heap,
                                           uhm_mmap_options *options) {
    void *addr;
    size_t rounded;
    uhm_result result;
    if (!options || options->size < sizeof(*options) ||
        options->abi_major != UHM_ABI_MAJOR) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    addr = options->addr;
    result = uhm_api_mmap(heap, &addr, options->map_size, options->prot,
                          options->map_flags);
    if (result != UHM_OK) {
        return result;
    }
    if (uhm_hl_align_up_size(options->map_size, uhm_hl_page_size(heap),
                             &rounded)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    options->addr = addr;
    options->map_size = rounded;
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_munmap(uhm_heap *heap, void *addr_ptr,
                                          size_t size) {
    uintptr_t addr = (uintptr_t)addr_ptr;
    uintptr_t end;
    uintptr_t region_end;
    size_t rounded;
    size_t page_size;
    uhm_hl_region *region;
    uhm_result result;
    int add_free_region = 1;

    if (!uhm_hl_heap_live(heap) || !addr_ptr) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_acquire(&heap->lock);
    page_size = uhm_hl_page_size(heap);
    if ((addr % page_size) != 0 ||
        uhm_hl_align_up_size(size, page_size, &rounded) || rounded == 0 ||
        uhm_hl_range_end(addr, rounded, &end)) {
        uhm_hl_lock_release(&heap->lock);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    region = uhm_hl_find_mmap_covering(heap, addr, rounded);
    if (!region || uhm_hl_range_end(region->addr, region->size, &region_end)) {
        uhm_hl_lock_release(&heap->lock);
        return UHM_ERR_NOT_MAPPED;
    }
    if (region->committed) {
        result = uhm_hl_vm_decommit(heap, (void *)addr, rounded);
        if (result != UHM_OK) {
            uhm_hl_lock_release(&heap->lock);
            return result;
        }
        heap->decommitted_bytes += rounded;
        if (heap->committed_bytes >= rounded) {
            heap->committed_bytes -= rounded;
        }
    }
    if (heap->mmap_live_bytes >= rounded) {
        heap->mmap_live_bytes -= rounded;
    } else {
        heap->mmap_live_bytes = 0;
    }
    if (addr == region->addr && end == region_end) {
        region->kind = UHM_HL_REGION_MMAP_FREE;
        region->committed = 0u;
        add_free_region = 0;
    } else if (addr == region->addr) {
        region->addr = end;
        region->size = (size_t)(region_end - end);
    } else if (end == region_end) {
        region->size = (size_t)(addr - region->addr);
    } else {
        uhm_hl_region *tail = uhm_hl_region_new(heap);
        if (!tail) {
            uhm_hl_lock_release(&heap->lock);
            return UHM_ERR_OUT_OF_MEMORY;
        }
        *tail = *region;
        tail->addr = end;
        tail->size = (size_t)(region_end - end);
        region->size = (size_t)(addr - region->addr);
    }
    if (add_free_region) {
        uhm_hl_region *free_region = uhm_hl_region_new(heap);
        if (free_region) {
            free_region->kind = UHM_HL_REGION_MMAP_FREE;
            free_region->addr = addr;
            free_region->size = rounded;
        }
    }
    uhm_hl_lock_release(&heap->lock);
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_mprotect(uhm_heap *heap, void *addr_ptr,
                                            size_t size, uint32_t prot) {
    uintptr_t addr = (uintptr_t)addr_ptr;
    size_t rounded;
    size_t page_size;
    uhm_hl_region *region;
    uhm_result result;
    if (!uhm_hl_heap_live(heap) || !addr_ptr) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_acquire(&heap->lock);
    page_size = uhm_hl_page_size(heap);
    if ((addr % page_size) != 0 ||
        uhm_hl_align_up_size(size, page_size, &rounded) || rounded == 0) {
        uhm_hl_lock_release(&heap->lock);
        return UHM_ERR_INVALID_ARGUMENT;
    }
    region = uhm_hl_find_mmap_covering(heap, addr, rounded);
    if (!region) {
        uhm_hl_lock_release(&heap->lock);
        return UHM_ERR_NOT_MAPPED;
    }
    result = region->committed
                 ? uhm_hl_vm_protect(heap, (void *)addr, rounded, prot)
                 : UHM_OK;
    if (result == UHM_OK) {
        region->prot = prot;
    }
    uhm_hl_lock_release(&heap->lock);
    return result;
}

static uhm_result UHM_CALL uhm_api_mremap(uhm_heap *heap, void **addr_inout,
                                          size_t old_size, size_t new_size,
                                          uint32_t flags) {
    uintptr_t addr;
    uhm_hl_region snapshot;
    uhm_hl_region *region;
    size_t old_rounded;
    size_t new_rounded;
    size_t page_size;
    void *new_addr = 0;
    uhm_result result;
    if (!uhm_hl_heap_live(heap) || !addr_inout || !*addr_inout ||
        (flags & ~UHM_REMAP_MAYMOVE) != 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    addr = (uintptr_t)*addr_inout;
    page_size = uhm_hl_page_size(heap);
    if (uhm_hl_align_up_size(old_size, page_size, &old_rounded) ||
        uhm_hl_align_up_size(new_size, page_size, &new_rounded) ||
        old_rounded == 0 || new_rounded == 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_acquire(&heap->lock);
    region = uhm_hl_find_mmap_covering(heap, addr, old_rounded);
    if (!region || region->addr != addr || region->size != old_rounded) {
        uhm_hl_lock_release(&heap->lock);
        return UHM_ERR_NOT_MAPPED;
    }
    if (new_rounded == old_rounded) {
        uhm_hl_lock_release(&heap->lock);
        *addr_inout = (void *)addr;
        return UHM_OK;
    }
    if (new_rounded < old_rounded) {
        uhm_hl_lock_release(&heap->lock);
        result = uhm_api_munmap(heap, (void *)(addr + new_rounded),
                                old_rounded - new_rounded);
        if (result == UHM_OK) {
            *addr_inout = (void *)addr;
        }
        return result;
    }
    snapshot = *region;
    uhm_hl_lock_release(&heap->lock);
    if ((flags & UHM_REMAP_MAYMOVE) == 0) {
        return UHM_ERR_UNSUPPORTED;
    }
    result = uhm_api_mmap(heap, &new_addr, new_rounded, snapshot.prot,
                          snapshot.flags & ~UHM_MAP_FIXED_NOREPLACE);
    if (result != UHM_OK) {
        return result;
    }
    memcpy(new_addr, (void *)addr, old_rounded);
    result = uhm_api_munmap(heap, (void *)addr, old_rounded);
    if (result != UHM_OK) {
        (void)uhm_api_munmap(heap, new_addr, new_rounded);
        return result;
    }
    *addr_inout = new_addr;
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_thread_ctx_open(
    uhm_heap *heap, const uhm_thread_config *config, uhm_thread_ctx **out_ctx) {
    uhm_thread_ctx *ctx;
    if (!uhm_hl_heap_live(heap) || !config || !out_ctx ||
        config->size < sizeof(*config) || config->abi_major != UHM_ABI_MAJOR ||
        !config->storage || config->storage_size < sizeof(*ctx) ||
        ((uintptr_t)config->storage % alignof(uhm_thread_ctx)) != 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    ctx = (uhm_thread_ctx *)config->storage;
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic = UHM_HL_THREAD_MAGIC;
    ctx->heap = heap;
    *out_ctx = ctx;
    return UHM_OK;
}

static void UHM_CALL uhm_api_thread_ctx_close(uhm_heap *heap,
                                              uhm_thread_ctx *ctx) {
    (void)heap;
    if (ctx && ctx->magic == UHM_HL_THREAD_MAGIC) {
        ctx->magic = 0;
        ctx->heap = 0;
    }
}

static uhm_result UHM_CALL uhm_api_thread_ctx_flush(uhm_heap *heap,
                                                    uhm_thread_ctx *ctx) {
    return uhm_hl_heap_live(heap) && ctx && ctx->magic == UHM_HL_THREAD_MAGIC
               ? UHM_OK
               : UHM_ERR_INVALID_ARGUMENT;
}

static void *UHM_CALL uhm_api_malloc_ctx(uhm_heap *heap, uhm_thread_ctx *ctx,
                                         size_t size) {
    if (!ctx || ctx->magic != UHM_HL_THREAD_MAGIC || ctx->heap != heap) {
        uhm_hl_set_error(heap, UHM_ERR_INVALID_ARGUMENT);
        return 0;
    }
    return uhm_api_malloc(heap, size);
}

static void UHM_CALL uhm_api_free_ctx(uhm_heap *heap, uhm_thread_ctx *ctx,
                                      void *ptr) {
    if (ctx && ctx->magic == UHM_HL_THREAD_MAGIC && ctx->heap == heap) {
        uhm_api_free(heap, ptr);
    } else {
        uhm_hl_set_error(heap, UHM_ERR_INVALID_ARGUMENT);
    }
}

static uhm_result UHM_CALL uhm_api_free_deferred_ctx(uhm_heap *heap,
                                                     uhm_thread_ctx *ctx,
                                                     void *ptr) {
    if (!ctx || ctx->magic != UHM_HL_THREAD_MAGIC || ctx->heap != heap) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_api_free(heap, ptr);
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_get_stats(uhm_heap *heap,
                                             uhm_stats *out_stats) {
    uhm_stats temp;
    uint32_t requested_size;
    if (!uhm_hl_heap_live(heap) || !out_stats ||
        out_stats->size < UHM_STATS_SIZE_ABI10) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    requested_size = out_stats->size;
    memset(&temp, 0, sizeof(temp));
    temp.size = sizeof(temp);
    temp.abi_major = UHM_ABI_MAJOR;
    temp.abi_minor = UHM_ABI_MINOR;
    uhm_hl_lock_acquire(&heap->lock);
    temp.arena_size = heap->arena_size;
    temp.committed_bytes = heap->committed_bytes;
    temp.decommitted_bytes = heap->decommitted_bytes;
    temp.logical_decommitted_bytes = heap->decommitted_bytes;
    temp.physical_released_bytes = heap->decommitted_bytes;
    temp.malloc_live_bytes = heap->malloc_live_bytes;
    temp.mmap_live_bytes = heap->mmap_live_bytes;
    temp.collision_free_bytes = heap->mmap_lo > heap->malloc_hi
                                    ? (size_t)(heap->mmap_lo - heap->malloc_hi)
                                    : 0u;
    temp.config_flags_enabled = heap->state_flags;
    temp.lockless_remote_mode = UHM_LOCKLESS_REMOTE_MODE_NONE;
    temp.vm_reserve_calls = heap->vm_reserve_calls;
    temp.vm_reserve_failures = heap->vm_reserve_failures;
    temp.vm_commit_calls = heap->vm_commit_calls;
    temp.vm_commit_failures = heap->vm_commit_failures;
    temp.vm_decommit_calls = heap->vm_decommit_calls;
    temp.vm_decommit_failures = heap->vm_decommit_failures;
    temp.vm_release_calls = heap->vm_release_calls;
    temp.vm_release_failures = heap->vm_release_failures;
    temp.vm_protect_calls = heap->vm_protect_calls;
    temp.vm_protect_failures = heap->vm_protect_failures;
    temp.page_lock_range_calls = 1u;
    temp.page_lock_range_shards = 1u;
    temp.page_lock_range_max_shards = 1u;
    uhm_hl_lock_release(&heap->lock);
    memcpy(out_stats, &temp, requested_size < sizeof(temp) ? requested_size
                                                           : sizeof(temp));
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_check(uhm_heap *heap, uint32_t check_flags) {
    size_t i;
    size_t j;
    (void)check_flags;
    if (!uhm_hl_heap_live(heap)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_acquire(&heap->lock);
    if (heap->arena_base >= heap->arena_end || heap->malloc_hi > heap->mmap_lo) {
        uhm_hl_lock_release(&heap->lock);
        return UHM_ERR_CORRUPTION;
    }
    for (i = 0; i < UHM_HL_MAX_REGIONS; ++i) {
        uhm_hl_region *a = &heap->regions[i];
        uintptr_t a_end;
        if (!uhm_hl_region_live(a)) {
            continue;
        }
        if (uhm_hl_range_end(a->addr, a->size, &a_end) ||
            a->addr < heap->arena_base || a_end > heap->arena_end) {
            uhm_hl_lock_release(&heap->lock);
            return UHM_ERR_CORRUPTION;
        }
        for (j = i + 1u; j < UHM_HL_MAX_REGIONS; ++j) {
            if (uhm_hl_region_overlaps(&heap->regions[j], a->addr, a->size)) {
                uhm_hl_lock_release(&heap->lock);
                return UHM_ERR_CORRUPTION;
            }
        }
    }
    uhm_hl_lock_release(&heap->lock);
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_enumerate_leaks(uhm_heap *heap,
                                                   uhm_leak_callback callback,
                                                   void *user) {
    size_t i;
    if (!uhm_hl_heap_live(heap) || !callback) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_acquire(&heap->lock);
    for (i = 0; i < UHM_HL_MAX_REGIONS; ++i) {
        uhm_hl_region *region = &heap->regions[i];
        uhm_leak_record record;
        uhm_result result;
        if (!uhm_hl_region_live(region)) {
            continue;
        }
        memset(&record, 0, sizeof(record));
        record.size = sizeof(record);
        record.abi_major = UHM_ABI_MAJOR;
        record.abi_minor = UHM_ABI_MINOR;
        record.kind = region->kind == UHM_HL_REGION_MALLOC
                          ? UHM_LEAK_KIND_MALLOC
                          : UHM_LEAK_KIND_MMAP;
        record.prot = region->prot;
        record.addr = (void *)region->addr;
        record.requested_size = region->requested_size;
        record.allocated_size = region->size;
        record.generation = region->generation;
        uhm_hl_lock_release(&heap->lock);
        result = callback(user, &record);
        if (result != UHM_OK) {
            return result;
        }
        uhm_hl_lock_acquire(&heap->lock);
    }
    uhm_hl_lock_release(&heap->lock);
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_get_last_error(uhm_heap *heap,
                                                  uhm_result *out_error,
                                                  uint32_t *out_platform_error) {
    if (!uhm_hl_heap_live(heap) || !out_error) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_acquire(&heap->lock);
    *out_error = heap->last_error;
    if (out_platform_error) {
        *out_platform_error = heap->last_platform_error;
    }
    uhm_hl_lock_release(&heap->lock);
    return UHM_OK;
}

static uhm_result UHM_CALL uhm_api_close(uhm_heap *heap) {
    size_t i;
    uhm_result result = UHM_OK;
    if (!uhm_hl_heap_live(heap)) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    uhm_hl_lock_acquire(&heap->lock);
    heap->closing = 1;
    for (i = 0; i < UHM_HL_MAX_REGIONS; ++i) {
        if (uhm_hl_region_live(&heap->regions[i])) {
            uhm_hl_region_decommit_best_effort(heap, &heap->regions[i]);
            heap->regions[i].kind = UHM_HL_REGION_UNUSED;
        }
    }
    if (heap->owns_arena) {
        result = uhm_hl_vm_release(heap, (void *)heap->arena_base,
                                   heap->arena_size);
    }
    heap->magic = UHM_HL_CLOSED_MAGIC;
    uhm_hl_lock_release(&heap->lock);
    return result;
}
