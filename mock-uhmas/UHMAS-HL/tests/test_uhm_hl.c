#include "uhm/uhm_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_true(int value, const char *message) {
    if (!value) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void expect_ok(uhm_result result, const char *message) {
    if (result != UHM_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

typedef struct leak_count {
    unsigned malloc_count;
    unsigned mmap_count;
} leak_count;

static uhm_result UHM_CALL count_leak(void *user,
                                      const uhm_leak_record *record) {
    leak_count *count = (leak_count *)user;
    if (record->kind == UHM_LEAK_KIND_MALLOC) {
        ++count->malloc_count;
    } else if (record->kind == UHM_LEAK_KIND_MMAP) {
        ++count->mmap_count;
    }
    return UHM_OK;
}

int main(void) {
    uhm_abi_info abi;
    uhm_storage_info heap_info;
    uhm_storage_info ctx_info;
    uhm_platform_heap owner;
    uhm_platform_heap compat_owner;
    uhm_platform_heap_config platform_config;
    uhm_heap *heap = 0;
    uhm_heap *compat_heap = 0;
    const uhm_api *api;
    const uhm_api *compat_api;
    uhm_stats stats;
    uhm_stats compat_stats;
    uhm_thread_config ctx_config;
    uhm_thread_ctx *ctx = 0;
    void *ctx_storage;
    void *ptr;
    void *grown;
    void *aligned;
    void *map = 0;
    void *remapped;
    void *fixed;
    uhm_mmap_options options;
    leak_count leaks = {0, 0};

    memset(&abi, 0, sizeof(abi));
    abi.size = sizeof(abi);
    abi.abi_major = 0xffffu;
    expect_ok(uhm_abi_query(&abi), "abi query");
    expect_true(abi.lockless_remote_primitives_supported == 0u,
                "HL reports no lockless remote primitives");
    expect_true(abi.lockless_remote_config_supported == 0u,
                "HL reports no lockless remote config");

    memset(&heap_info, 0, sizeof(heap_info));
    memset(&ctx_info, 0, sizeof(ctx_info));
    heap_info.size = sizeof(heap_info);
    ctx_info.size = sizeof(ctx_info);
    expect_ok(uhm_heap_storage_query(&heap_info, &ctx_info),
              "storage query");
    expect_ok(uhm_heap_storage_query(0, 0), "storage query null outputs");
    expect_true(heap_info.storage_size > 0 &&
                    ctx_info.storage_size >= sizeof(void *),
                "storage sizes");

    memset(&platform_config, 0, sizeof(platform_config));
    platform_config.size = sizeof(platform_config);
    platform_config.abi_major = UHM_ABI_MAJOR;
    platform_config.abi_minor = UHM_ABI_MINOR;
    platform_config.heap_flags = UHM_CONFIG_LOCKLESS_REMOTE_FREE |
                                 UHM_CONFIG_SPINLOCK_YIELD |
                                 UHM_CONFIG_REPAIR_TREAP_HEALTH;
    platform_config.spinlock_yield_after_spins = 1u;
    expect_ok(uhm_platform_heap_open(&platform_config, &compat_owner,
                                     &compat_heap),
              "compat flags heap open");
    compat_api = uhm_get_api(compat_heap);
    expect_true(compat_api != 0, "compat api");
    memset(&compat_stats, 0, sizeof(compat_stats));
    compat_stats.size = sizeof(compat_stats);
    expect_ok(compat_api->get_stats(compat_heap, &compat_stats),
              "compat stats");
    expect_true(compat_stats.config_flags_enabled == 0u,
                "unsupported flags are not enabled");
    expect_true(compat_stats.lockless_remote_mode ==
                    UHM_LOCKLESS_REMOTE_MODE_NONE,
                "compat lockless none");
    expect_ok(uhm_platform_heap_close(&compat_owner), "compat close");

    platform_config.heap_flags = UHM_CONFIG_FAIL_ON_TREAP_HEALTH |
                                 UHM_CONFIG_REPAIR_TREAP_HEALTH;
    expect_true(uhm_platform_heap_open(&platform_config, &compat_owner,
                                       &compat_heap) ==
                    UHM_ERR_INVALID_ARGUMENT,
                "mutually exclusive treap flags rejected");

    expect_ok(uhm_platform_heap_open_default(&owner, &heap),
              "platform heap open");
    api = uhm_get_api(heap);
    expect_true(api && api->malloc && api->mmap && api->close, "api table");

    ptr = api->malloc(heap, 123);
    expect_true(ptr != 0, "malloc");
    memset(ptr, 0x5a, 123);
    grown = api->realloc(heap, ptr, 4096);
    expect_true(grown != 0, "realloc grow");
    expect_true(((unsigned char *)grown)[0] == 0x5a, "realloc preserves data");
    api->free(heap, grown);

    aligned = api->aligned_alloc(heap, 4096, 64);
    expect_true(aligned != 0 && ((uintptr_t)aligned % 4096u) == 0,
                "aligned alloc");
    api->free(heap, aligned);

    ctx_storage = malloc(ctx_info.storage_size + ctx_info.storage_alignment);
    expect_true(ctx_storage != 0, "ctx storage alloc");
    memset(&ctx_config, 0, sizeof(ctx_config));
    ctx_config.size = sizeof(ctx_config);
    ctx_config.abi_major = UHM_ABI_MAJOR;
    ctx_config.abi_minor = UHM_ABI_MINOR;
    ctx_config.storage = ctx_storage;
    ctx_config.storage_size = ctx_info.storage_size;
    expect_ok(api->thread_ctx_open(heap, &ctx_config, &ctx), "ctx open");
    ptr = api->malloc_ctx(heap, ctx, 77);
    expect_true(ptr != 0, "ctx malloc");
    api->free_ctx(heap, ctx, ptr);
    expect_ok(api->thread_ctx_flush(heap, ctx), "ctx flush");
    api->thread_ctx_close(heap, ctx);
    free(ctx_storage);

    expect_ok(api->mmap(heap, &map, 1234, UHM_PROT_READ | UHM_PROT_WRITE,
                        UHM_MAP_PRIVATE),
              "mmap");
    expect_true(map != 0, "mmap addr");
    memset(map, 0x33, 1234);
    remapped = map;
    expect_ok(api->mremap(heap, &remapped, 1234,
                          owner.vm.page_size * 2u + 17u, UHM_REMAP_MAYMOVE),
              "mremap grow");
    expect_true(remapped != 0 && ((unsigned char *)remapped)[0] == 0x33,
                "mremap grow preserves data");
    map = remapped;
    expect_ok(api->mremap(heap, &map, owner.vm.page_size * 2u + 17u,
                          owner.vm.page_size, 0),
              "mremap shrink");
    expect_true(map == remapped, "mremap shrink stays in place");
    expect_ok(api->mprotect(heap, map, owner.vm.page_size, UHM_PROT_READ),
              "mprotect");
    expect_ok(api->munmap(heap, map, owner.vm.page_size), "munmap");

    fixed = map;
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.abi_major = UHM_ABI_MAJOR;
    options.abi_minor = UHM_ABI_MINOR;
    options.addr = fixed;
    options.map_size = owner.vm.page_size - 1u;
    options.prot = UHM_PROT_READ | UHM_PROT_WRITE;
    options.map_flags = UHM_MAP_PRIVATE | UHM_MAP_FIXED_NOREPLACE;
    expect_ok(api->mmap_ex(heap, &options), "fixed mmap_ex");
    expect_true(options.addr == fixed &&
                    options.map_size == owner.vm.page_size,
                "mmap_ex canonical size");
    expect_ok(api->check(heap, 0), "check fixed mmap");

    ptr = api->malloc(heap, 32);
    expect_true(ptr != 0, "leak malloc seed");
    expect_ok(api->enumerate_leaks(heap, count_leak, &leaks),
              "enumerate leaks");
    expect_true(leaks.malloc_count == 1u && leaks.mmap_count == 1u,
                "leak counts");
    api->free(heap, ptr);
    expect_ok(api->munmap(heap, options.addr, options.map_size),
              "fixed munmap");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(api->get_stats(heap, &stats), "stats");
    expect_true(stats.lockless_remote_mode == UHM_LOCKLESS_REMOTE_MODE_NONE,
                "stats lockless none");
    expect_true(stats.malloc_live_bytes == 0 && stats.mmap_live_bytes == 0,
                "stats no live bytes");
    expect_ok(api->check(heap, 0), "check");
    expect_ok(uhm_platform_heap_close(&owner), "platform heap close");
    return 0;
}
