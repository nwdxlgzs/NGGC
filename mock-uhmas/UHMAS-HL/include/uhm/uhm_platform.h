#ifndef UHM_UHM_PLATFORM_H
#define UHM_UHM_PLATFORM_H

#include "uhm/uhm.h"

#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#if defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#elif defined(__linux__)
#define MAP_ANONYMOUS 0x20
#endif
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline size_t uhm_platform_system_page_size(void) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    return info.dwPageSize != 0 ? (size_t)info.dwPageSize : (size_t)0u;
#else
    long page_size = sysconf(_SC_PAGESIZE);
    return page_size > 0 ? (size_t)page_size : (size_t)0u;
#endif
}

static inline size_t uhm_platform_system_allocation_granularity(void) {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    return info.dwAllocationGranularity != 0
               ? (size_t)info.dwAllocationGranularity
               : uhm_platform_system_page_size();
#else
    return uhm_platform_system_page_size();
#endif
}

static inline uhm_result UHM_CALL uhm_platform_usermode_noop_commit(void *user, void *addr,
                                                                    size_t size, uint32_t prot) {
    (void)user;
    (void)addr;
    (void)size;
    (void)prot;
    return UHM_OK;
}

static inline uhm_result UHM_CALL uhm_platform_usermode_noop_decommit(void *user, void *addr,
                                                                      size_t size) {
    (void)user;
    (void)addr;
    (void)size;
    return UHM_OK;
}

static inline uhm_result UHM_CALL uhm_platform_usermode_noop_protect(void *user, void *addr,
                                                                     size_t size, uint32_t prot) {
    (void)user;
    (void)addr;
    (void)size;
    (void)prot;
    return UHM_OK;
}

static inline uhm_result UHM_CALL uhm_platform_usermode_noop_fixed_remap(void *user, void *addr,
                                                                         size_t size, uint32_t prot) {
    (void)user;
    (void)addr;
    (void)size;
    (void)prot;
    return UHM_OK;
}

static inline uhm_result UHM_CALL uhm_platform_usermode_numa_hint(
    void *user, void *addr, size_t size, uint32_t policy, int32_t node,
    uint32_t flags) {
    (void)user;
    (void)addr;
    (void)size;
    (void)policy;
    (void)node;
    (void)flags;
    return UHM_OK;
}

static inline uhm_result uhm_platform_usermode_vm_ops_init(uhm_vm_ops *out,
                                                           size_t page_size,
                                                           size_t allocation_granularity,
                                                           uint64_t extra_capabilities) {
    if (!out) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (page_size == 0) {
        page_size = uhm_platform_system_page_size();
        if (page_size == 0) {
            return UHM_ERR_PLATFORM;
        }
    }
    if (allocation_granularity == 0) {
        allocation_granularity = uhm_platform_system_allocation_granularity();
        if (allocation_granularity < page_size) {
            allocation_granularity = page_size;
        }
    }
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    out->abi_major = UHM_ABI_MAJOR;
    out->abi_minor = UHM_ABI_MINOR;
    out->page_size = page_size;
    out->allocation_granularity = allocation_granularity;
    out->capabilities = UHM_VM_CAN_COMMIT |
                        UHM_VM_CAN_DECOMMIT |
                        UHM_VM_CAN_PROTECT |
                        UHM_VM_CAN_FIXED_REMAP |
                        UHM_VM_CAN_RECOMMIT_SAME_ADDRESS |
                        UHM_VM_USERMODE_LOGICAL_VM |
                        UHM_VM_CAN_NUMA_FIRST_TOUCH |
                        extra_capabilities;
    out->commit = uhm_platform_usermode_noop_commit;
    out->decommit = uhm_platform_usermode_noop_decommit;
    out->protect = uhm_platform_usermode_noop_protect;
    out->fixed_remap = uhm_platform_usermode_noop_fixed_remap;
    out->numa_hint = uhm_platform_usermode_numa_hint;
    return UHM_OK;
}

#if defined(_WIN32)

static inline DWORD uhm_platform_windows_protect(uint32_t prot) {
    if (prot == UHM_PROT_NONE) {
        return PAGE_NOACCESS;
    }
    if ((prot & UHM_PROT_EXEC) != 0) {
        if ((prot & UHM_PROT_WRITE) != 0) {
            return PAGE_EXECUTE_READWRITE;
        }
        if ((prot & UHM_PROT_READ) != 0) {
            return PAGE_EXECUTE_READ;
        }
        return PAGE_EXECUTE;
    }
    if ((prot & UHM_PROT_WRITE) != 0) {
        return PAGE_READWRITE;
    }
    if ((prot & UHM_PROT_READ) != 0) {
        return PAGE_READONLY;
    }
    return PAGE_NOACCESS;
}

static inline uhm_result UHM_CALL uhm_platform_windows_reserve(void *user, void **addr,
                                                               size_t size, size_t alignment) {
    void *requested = addr ? *addr : 0;
    void *result;
    (void)user;
    (void)alignment;
    result = VirtualAlloc(requested, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!result) {
        return UHM_ERR_PLATFORM;
    }
    *addr = result;
    return UHM_OK;
}

static inline uhm_result UHM_CALL uhm_platform_windows_commit(void *user, void *addr,
                                                              size_t size, uint32_t prot) {
    (void)user;
    return VirtualAlloc(addr, size, MEM_COMMIT, uhm_platform_windows_protect(prot)) != 0
        ? UHM_OK
        : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_windows_decommit(void *user, void *addr,
                                                                size_t size) {
    (void)user;
    return VirtualFree(addr, size, MEM_DECOMMIT) != 0 ? UHM_OK : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_windows_release(void *user, void *addr,
                                                               size_t size) {
    (void)user;
    (void)size;
    return VirtualFree(addr, 0, MEM_RELEASE) != 0 ? UHM_OK : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_windows_protect_cb(void *user, void *addr,
                                                                  size_t size, uint32_t prot) {
    DWORD old_protect = 0;
    (void)user;
    return VirtualProtect(addr, size, uhm_platform_windows_protect(prot), &old_protect) != 0
        ? UHM_OK
        : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_windows_fixed_remap(void *user, void *addr,
                                                                   size_t size, uint32_t prot) {
    uhm_result result = uhm_platform_windows_decommit(user, addr, size);
    if (result != UHM_OK) {
        return result;
    }
    return uhm_platform_windows_commit(user, addr, size, prot);
}

static inline uhm_result UHM_CALL uhm_platform_windows_numa_hint(
    void *user, void *addr, size_t size, uint32_t policy, int32_t node,
    uint32_t flags) {
    (void)user;
    (void)addr;
    (void)size;
    (void)node;
    (void)flags;
    return policy == UHM_NUMA_POLICY_DEFAULT ||
                   policy == UHM_NUMA_POLICY_FIRST_TOUCH
               ? UHM_OK
               : UHM_ERR_UNSUPPORTED;
}

static inline uhm_result uhm_platform_vm_ops_init(uhm_vm_ops *out) {
    SYSTEM_INFO info;
    if (!out) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    GetNativeSystemInfo(&info);
    if (info.dwPageSize == 0 || info.dwAllocationGranularity == 0) {
        return UHM_ERR_PLATFORM;
    }
    out->size = sizeof(*out);
    out->abi_major = UHM_ABI_MAJOR;
    out->abi_minor = UHM_ABI_MINOR;
    out->page_size = (size_t)info.dwPageSize;
    out->allocation_granularity = (size_t)info.dwAllocationGranularity;
    out->capabilities = UHM_VM_CAN_RESERVE |
                        UHM_VM_CAN_COMMIT |
                        UHM_VM_CAN_DECOMMIT |
                        UHM_VM_CAN_RELEASE |
                        UHM_VM_CAN_PROTECT |
                        UHM_VM_CAN_FIXED_REMAP |
                        UHM_VM_DECOMMIT_MAKES_NOACCESS |
                        UHM_VM_DECOMMIT_DROPS_PHYSICAL |
                        UHM_VM_DECOMMIT_ZERO_FILLS |
                        UHM_VM_CAN_RECOMMIT_SAME_ADDRESS |
                        UHM_VM_PROTECT_EXEC_SUPPORTED |
                        UHM_VM_CAN_NUMA_FIRST_TOUCH;
    out->reserve = uhm_platform_windows_reserve;
    out->commit = uhm_platform_windows_commit;
    out->decommit = uhm_platform_windows_decommit;
    out->release = uhm_platform_windows_release;
    out->protect = uhm_platform_windows_protect_cb;
    out->fixed_remap = uhm_platform_windows_fixed_remap;
    out->numa_hint = uhm_platform_windows_numa_hint;
    return UHM_OK;
}

#else

static inline int uhm_platform_posix_protect(uint32_t prot) {
    int out = 0;
    if ((prot & UHM_PROT_READ) != 0) {
        out |= PROT_READ;
    }
    if ((prot & UHM_PROT_WRITE) != 0) {
        out |= PROT_WRITE;
    }
    if ((prot & UHM_PROT_EXEC) != 0) {
        out |= PROT_EXEC;
    }
    return out;
}

static inline uhm_result UHM_CALL uhm_platform_posix_reserve(void *user, void **addr,
                                                             size_t size, size_t alignment) {
    void *requested = addr ? *addr : 0;
    void *result;
    size_t page_size;
    size_t effective_alignment;
    (void)user;
    if (!addr || size == 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    page_size = uhm_platform_system_page_size();
    effective_alignment = alignment != 0 ? alignment : page_size;
    if (effective_alignment < page_size) {
        effective_alignment = page_size;
    }
    if ((effective_alignment & (effective_alignment - 1u)) != 0) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    if (!requested && effective_alignment > page_size) {
        size_t reserve_size;
        uintptr_t base;
        uintptr_t aligned;
        size_t prefix;
        size_t suffix;

        if (size > SIZE_MAX - effective_alignment) {
            return UHM_ERR_OUT_OF_MEMORY;
        }
        reserve_size = size + effective_alignment;
        result = mmap(0, reserve_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (result == MAP_FAILED) {
            return UHM_ERR_PLATFORM;
        }
        base = (uintptr_t)result;
        aligned = (base + effective_alignment - 1u) &
                  ~((uintptr_t)effective_alignment - 1u);
        prefix = (size_t)(aligned - base);
        suffix = reserve_size - prefix - size;
        if (prefix != 0 && munmap((void *)base, prefix) != 0) {
            (void)munmap(result, reserve_size);
            return UHM_ERR_PLATFORM;
        }
        if (suffix != 0 && munmap((void *)(aligned + size), suffix) != 0) {
            (void)munmap((void *)aligned, size);
            return UHM_ERR_PLATFORM;
        }
        *addr = (void *)aligned;
        return UHM_OK;
    }
    result = mmap(requested, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        return UHM_ERR_PLATFORM;
    }
    if (((uintptr_t)result % effective_alignment) != 0) {
        (void)munmap(result, size);
        return UHM_ERR_UNSUPPORTED;
    }
    *addr = result;
    return UHM_OK;
}

static inline uhm_result UHM_CALL uhm_platform_posix_commit(void *user, void *addr,
                                                            size_t size, uint32_t prot) {
    (void)user;
    return mprotect(addr, size, uhm_platform_posix_protect(prot)) == 0 ? UHM_OK : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_posix_decommit(void *user, void *addr,
                                                              size_t size) {
    (void)user;
#if defined(MADV_DONTNEED)
    (void)madvise(addr, size, MADV_DONTNEED);
#endif
    return mprotect(addr, size, PROT_NONE) == 0 ? UHM_OK : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_posix_release(void *user, void *addr,
                                                             size_t size) {
    (void)user;
    return munmap(addr, size) == 0 ? UHM_OK : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_posix_protect_cb(void *user, void *addr,
                                                                size_t size, uint32_t prot) {
    (void)user;
    return mprotect(addr, size, uhm_platform_posix_protect(prot)) == 0 ? UHM_OK : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_posix_fixed_remap(void *user, void *addr,
                                                                 size_t size, uint32_t prot) {
    void *result;
    (void)user;
    result = mmap(addr, size, uhm_platform_posix_protect(prot),
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return result == addr ? UHM_OK : UHM_ERR_PLATFORM;
}

static inline uhm_result UHM_CALL uhm_platform_posix_numa_hint(
    void *user, void *addr, size_t size, uint32_t policy, int32_t node,
    uint32_t flags) {
    (void)user;
    (void)addr;
    (void)size;
    (void)node;
    (void)flags;
    return policy == UHM_NUMA_POLICY_DEFAULT ||
                   policy == UHM_NUMA_POLICY_FIRST_TOUCH
               ? UHM_OK
               : UHM_ERR_UNSUPPORTED;
}

static inline uint64_t uhm_platform_posix_probe_fixed_noreplace(size_t page_size) {
#if defined(MAP_FIXED_NOREPLACE)
    void *base = mmap(0, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *probe;
    if (base == MAP_FAILED) {
        return 0;
    }
    errno = 0;
    probe = mmap(base, page_size, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (probe == MAP_FAILED && errno == EEXIST) {
        (void)munmap(base, page_size);
        return UHM_VM_FIXED_NOREPLACE_ATOMIC;
    }
    if (probe != MAP_FAILED && probe != base) {
        (void)munmap(probe, page_size);
    }
    (void)munmap(base, page_size);
#else
    (void)page_size;
#endif
    return 0;
}

static inline uhm_result uhm_platform_vm_ops_init(uhm_vm_ops *out) {
    long page_size;
    if (!out) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return UHM_ERR_PLATFORM;
    }
    out->size = sizeof(*out);
    out->abi_major = UHM_ABI_MAJOR;
    out->abi_minor = UHM_ABI_MINOR;
    out->page_size = (size_t)page_size;
    out->allocation_granularity = (size_t)page_size;
    out->capabilities = UHM_VM_CAN_RESERVE |
                        UHM_VM_CAN_COMMIT |
                        UHM_VM_CAN_DECOMMIT |
                        UHM_VM_CAN_RELEASE |
                        UHM_VM_CAN_PROTECT |
                        UHM_VM_CAN_FIXED_REMAP |
                        UHM_VM_DECOMMIT_MAKES_NOACCESS |
                        UHM_VM_DECOMMIT_DROPS_PHYSICAL |
                        UHM_VM_CAN_RECOMMIT_SAME_ADDRESS |
                        UHM_VM_COMMIT_IS_PROTECT_ONLY |
                        UHM_VM_PROTECT_EXEC_SUPPORTED |
                        UHM_VM_CAN_NUMA_FIRST_TOUCH |
                        uhm_platform_posix_probe_fixed_noreplace((size_t)page_size);
    out->reserve = uhm_platform_posix_reserve;
    out->commit = uhm_platform_posix_commit;
    out->decommit = uhm_platform_posix_decommit;
    out->release = uhm_platform_posix_release;
    out->protect = uhm_platform_posix_protect_cb;
    out->fixed_remap = uhm_platform_posix_fixed_remap;
    out->numa_hint = uhm_platform_posix_numa_hint;
    return UHM_OK;
}

#endif

typedef struct uhm_platform_heap_config {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    size_t arena_size;
    uint32_t heap_flags;
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
} uhm_platform_heap_config;

#define UHM_PLATFORM_HEAP_AUTO_TREAP_REPAIR (1u << 0)

#define UHM_PLATFORM_HEAP_KNOWN_FLAGS \
    (UHM_PLATFORM_HEAP_AUTO_TREAP_REPAIR)

#define UHM_PLATFORM_HEAP_CONFIG_SIZE_ABI7 \
    ((uint32_t)offsetof(uhm_platform_heap_config, \
                        malloc_free_decommit_retention_pages))
#define UHM_PLATFORM_HEAP_CONFIG_SIZE_ABI8 \
    ((uint32_t)offsetof(uhm_platform_heap_config, \
                        spinlock_yield_after_spins))

typedef struct uhm_platform_heap {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    uhm_heap *heap;
    uhm_vm_ops vm;
    void *heap_storage;
    size_t heap_storage_size;
    size_t heap_storage_reserved_size;
} uhm_platform_heap;

static inline int uhm_platform_size_align_up(size_t value, size_t alignment,
                                             size_t *out) {
    size_t mask;
    if (!out || alignment == 0 || (alignment & (alignment - 1u)) != 0) {
        return 0;
    }
    mask = alignment - 1u;
    if (value > (SIZE_MAX - mask)) {
        return 0;
    }
    *out = (value + mask) & ~mask;
    return 1;
}

static inline size_t uhm_platform_default_arena_size(void) {
    return sizeof(void *) >= 8u ? (size_t)64u * 1024u * 1024u
                               : (size_t)16u * 1024u * 1024u;
}

static inline void uhm_platform_heap_zero(uhm_platform_heap *owner) {
    if (owner) {
        memset(owner, 0, sizeof(*owner));
        owner->size = sizeof(*owner);
        owner->abi_major = UHM_ABI_MAJOR;
        owner->abi_minor = UHM_ABI_MINOR;
    }
}

static inline uhm_result
uhm_platform_heap_open(const uhm_platform_heap_config *config,
                       uhm_platform_heap *owner,
                       uhm_heap **out_heap) {
    uhm_storage_info heap_info;
    uhm_config heap_config;
    uhm_vm_ops vm;
    void *storage = 0;
    size_t storage_reserved_size;
    size_t arena_size;
    uhm_result result;

    if (!owner || !out_heap) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    *out_heap = 0;
    if (config && (config->size < UHM_PLATFORM_HEAP_CONFIG_SIZE_ABI7 ||
                   config->abi_major != UHM_ABI_MAJOR ||
                   (config->flags & ~UHM_PLATFORM_HEAP_KNOWN_FLAGS) != 0u ||
                   ((config->flags & UHM_PLATFORM_HEAP_AUTO_TREAP_REPAIR) != 0u &&
                    (config->heap_flags & UHM_CONFIG_FAIL_ON_TREAP_HEALTH) != 0u))) {
        return UHM_ERR_INVALID_ARGUMENT;
    }

    memset(&heap_info, 0, sizeof(heap_info));
    heap_info.size = sizeof(heap_info);
    result = uhm_heap_storage_query(&heap_info, 0);
    if (result != UHM_OK) {
        return result;
    }
    result = uhm_platform_vm_ops_init(&vm);
    if (result != UHM_OK) {
        return result;
    }
    if ((vm.capabilities & (UHM_VM_CAN_RESERVE | UHM_VM_CAN_COMMIT |
                            UHM_VM_CAN_RELEASE)) !=
            (UHM_VM_CAN_RESERVE | UHM_VM_CAN_COMMIT | UHM_VM_CAN_RELEASE) ||
        !vm.reserve || !vm.commit || !vm.release) {
        return UHM_ERR_UNSUPPORTED;
    }
    if (!uhm_platform_size_align_up(heap_info.storage_size, vm.page_size,
                                    &storage_reserved_size)) {
        return UHM_ERR_OUT_OF_MEMORY;
    }
    result = vm.reserve(vm.user, &storage, storage_reserved_size,
                        vm.allocation_granularity);
    if (result != UHM_OK) {
        return result;
    }
    result = vm.commit(vm.user, storage, storage_reserved_size,
                       UHM_PROT_READ | UHM_PROT_WRITE);
    if (result != UHM_OK) {
        (void)vm.release(vm.user, storage, storage_reserved_size);
        return result;
    }

    uhm_platform_heap_zero(owner);
    owner->flags = config ? config->flags : 0u;
    owner->vm = vm;
    owner->heap_storage = storage;
    owner->heap_storage_size = heap_info.storage_size;
    owner->heap_storage_reserved_size = storage_reserved_size;

    arena_size = config && config->arena_size != 0
                     ? config->arena_size
                     : uhm_platform_default_arena_size();
    memset(&heap_config, 0, sizeof(heap_config));
    heap_config.size = sizeof(heap_config);
    heap_config.abi_major = UHM_ABI_MAJOR;
    heap_config.abi_minor = UHM_ABI_MINOR;
    heap_config.flags = config ? config->heap_flags : 0u;
    if (config &&
        (config->flags & UHM_PLATFORM_HEAP_AUTO_TREAP_REPAIR) != 0u) {
        heap_config.flags |= UHM_CONFIG_REPAIR_TREAP_HEALTH;
    }
    heap_config.heap_storage = storage;
    heap_config.heap_storage_size = heap_info.storage_size;
    heap_config.arena_size = arena_size;
    heap_config.vm = vm;
    if (config) {
        heap_config.control_band_min_size = config->control_band_min_size;
        heap_config.metadata_commit_granularity =
            config->metadata_commit_granularity;
        heap_config.mmap_reuse_cache_limit = config->mmap_reuse_cache_limit;
        heap_config.thread_registry_slots = config->thread_registry_slots;
        heap_config.tls_cache_min_pages = config->tls_cache_min_pages;
        heap_config.tls_cache_max_pages = config->tls_cache_max_pages;
        heap_config.tls_cache_default_pages = config->tls_cache_default_pages;
        heap_config.numa_policy = config->numa_policy;
        heap_config.preferred_numa_node = config->preferred_numa_node;
        if (config->size >= UHM_PLATFORM_HEAP_CONFIG_SIZE_ABI8) {
            heap_config.malloc_free_decommit_retention_pages =
                config->malloc_free_decommit_retention_pages;
        }
        if (config->size >= sizeof(*config)) {
            heap_config.spinlock_yield_after_spins =
                config->spinlock_yield_after_spins;
        }
    }

    result = uhm_open(&heap_config, &owner->heap);
    if (result != UHM_OK) {
        (void)vm.release(vm.user, storage, storage_reserved_size);
        uhm_platform_heap_zero(owner);
        return result;
    }
    *out_heap = owner->heap;
    return UHM_OK;
}

static inline uhm_result
uhm_platform_heap_open_default(uhm_platform_heap *owner,
                               uhm_heap **out_heap) {
    return uhm_platform_heap_open(0, owner, out_heap);
}

static inline uhm_result uhm_platform_heap_close(uhm_platform_heap *owner) {
    const uhm_api *api;
    uhm_result result;
    void *storage;
    size_t storage_reserved_size;
    uhm_vm_ops vm;

    if (!owner || !owner->heap || !owner->heap_storage) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    api = uhm_get_api(owner->heap);
    if (!api || !api->close) {
        return UHM_ERR_INVALID_ARGUMENT;
    }
    result = api->close(owner->heap);
    if (result != UHM_OK) {
        return result;
    }
    vm = owner->vm;
    storage = owner->heap_storage;
    storage_reserved_size = owner->heap_storage_reserved_size;
    result = vm.release && storage && storage_reserved_size != 0
                 ? vm.release(vm.user, storage, storage_reserved_size)
                 : UHM_ERR_INVALID_ARGUMENT;
    uhm_platform_heap_zero(owner);
    return result;
}

#ifdef __cplusplus
}
#endif

#endif
