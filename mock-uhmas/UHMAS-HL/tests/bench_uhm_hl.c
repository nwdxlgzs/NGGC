#include "uhm/uhm_platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#if defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#elif defined(__linux__)
#define MAP_ANONYMOUS 0x20
#endif
#endif
#endif

typedef struct bench_heap_env {
    uhm_platform_heap owner;
    uhm_heap *heap;
    const uhm_api *api;
} bench_heap_env;

typedef struct bench_result {
    const char *name;
    double seconds;
    size_t operations;
} bench_result;

typedef struct bench_thread_args {
    bench_heap_env *env;
    size_t iterations;
    int use_system;
    int ok;
} bench_thread_args;

typedef struct bench_options {
    int stress;
    size_t arena_size;
    size_t small_iters;
    size_t thread_iters;
    size_t large_iters;
    size_t mmap_iters;
    size_t fixed_iters;
    size_t release_iters;
} bench_options;

static volatile uintptr_t bench_sink;

static void bench_fail(const char *message) {
    fprintf(stderr, "benchmark failure: %s\n", message);
    exit(1);
}

static void bench_expect(int value, const char *message) {
    if (!value) {
        bench_fail(message);
    }
}

static void bench_expect_ok(uhm_result result, const char *message) {
    if (result != UHM_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static uint64_t bench_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000ull) /
                      frequency.QuadPart);
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static bench_result bench_result_make(const char *name, uint64_t begin,
                                      uint64_t end, size_t operations) {
    bench_result result;
    result.name = name;
    result.seconds = (double)(end - begin) / 1000000000.0;
    result.operations = operations;
    return result;
}

static void bench_print(const bench_result *result) {
    double ns_per_op = 0.0;
    if (result->operations != 0 && result->seconds > 0.0) {
        ns_per_op = (result->seconds * 1000000000.0) /
                    (double)result->operations;
    }
    printf("%-38s %10.6f s  %10zu ops  %10.1f ns/op\n", result->name,
           result->seconds, result->operations, ns_per_op);
}

static int bench_parse_size(const char *text, size_t *out) {
    char *end = 0;
    unsigned long long value;
    if (!text || !out || text[0] == '\0') {
        return 0;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0ull) {
        return 0;
    }
    *out = (size_t)value;
    return 1;
}

static void bench_usage(const char *argv0) {
    printf("Usage: %s [options]\n", argv0);
    printf("  --stress              use UHMAS stress iteration counts\n");
    printf("  --arena-mib N         override arena size in MiB\n");
    printf("  --small-iters N       override small malloc iterations\n");
    printf("  --thread-iters N      override per-thread small iterations\n");
    printf("  --large-iters N       override large malloc iterations\n");
    printf("  --mmap-iters N        override mmap iterations\n");
    printf("  --fixed-iters N       override fixed/mprotect iterations\n");
    printf("  --release-iters N     override mixed pressure iterations\n");
    printf("Compatibility aliases: --malloc-iterations, --vm-iterations, "
           "--protect-iterations; --mremap-iterations is accepted and "
           "ignored.\n");
}

static void bench_options_init(bench_options *options, int stress) {
    options->stress = stress;
    options->arena_size = stress ? (size_t)256u * 1024u * 1024u
                                 : (size_t)96u * 1024u * 1024u;
    options->small_iters = stress ? 240000u : 24000u;
    options->thread_iters = stress ? 70000u : 7000u;
    options->large_iters = stress ? 9000u : 900u;
    options->mmap_iters = stress ? 5000u : 500u;
    options->fixed_iters = stress ? 2500u : 250u;
    options->release_iters = stress ? 90000u : 9000u;
}

static int bench_parse_options(int argc, char **argv,
                               bench_options *options) {
    int i;
    int stress = 0;
    const char *stress_env = getenv("UHMAS_BENCH_STRESS");
    if (stress_env && stress_env[0] != '\0' && stress_env[0] != '0') {
        stress = 1;
    }
    bench_options_init(options, stress);
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        size_t *slot = 0;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            bench_usage(argv[0]);
            return 1;
        } else if (strcmp(arg, "--stress") == 0) {
            bench_options_init(options, 1);
            continue;
        } else if (strcmp(arg, "--arena-mib") == 0) {
            size_t mib;
            if (i + 1 >= argc || !bench_parse_size(argv[++i], &mib)) {
                fprintf(stderr, "invalid value for %s\n", arg);
                return -1;
            }
            options->arena_size = mib * (size_t)1024u * (size_t)1024u;
            continue;
        } else if (strcmp(arg, "--small-iters") == 0 ||
                   strcmp(arg, "--malloc-iterations") == 0) {
            slot = &options->small_iters;
        } else if (strcmp(arg, "--thread-iters") == 0) {
            slot = &options->thread_iters;
        } else if (strcmp(arg, "--large-iters") == 0) {
            slot = &options->large_iters;
        } else if (strcmp(arg, "--mmap-iters") == 0 ||
                   strcmp(arg, "--vm-iterations") == 0) {
            slot = &options->mmap_iters;
        } else if (strcmp(arg, "--fixed-iters") == 0 ||
                   strcmp(arg, "--protect-iterations") == 0) {
            slot = &options->fixed_iters;
        } else if (strcmp(arg, "--release-iters") == 0) {
            slot = &options->release_iters;
        } else if (strcmp(arg, "--mremap-iterations") == 0) {
            size_t ignored;
            if (i + 1 >= argc ||
                !bench_parse_size(argv[++i], &ignored)) {
                fprintf(stderr, "invalid value for %s\n", arg);
                return -1;
            }
            continue;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            return -1;
        }
        if (!slot || i + 1 >= argc || !bench_parse_size(argv[++i], slot)) {
            fprintf(stderr, "invalid value for %s\n", arg);
            return -1;
        }
    }
    return 0;
}

static void bench_print_abi_capabilities(void) {
    uhm_abi_info abi;
    memset(&abi, 0, sizeof(abi));
    abi.size = sizeof(abi);
    bench_expect_ok(uhm_abi_query(&abi), "abi query");
    printf("abi capabilities: pointer_size=%u size_t_size=%u "
           "atomic_pointer_lock_free=%u atomic_u32_lock_free=%u "
           "atomic_u64_lock_free=%u tagged_freelist_supported=%u "
           "tagged_freelist_tag_bits=%u "
           "lockless_remote_primitives_supported=%u "
           "lockless_remote_config_supported=%u malloc_min_alignment=%u "
           "cacheline_size=%u call_abi=%u\n",
           abi.pointer_size, abi.size_t_size, abi.atomic_pointer_lock_free,
           abi.atomic_u32_lock_free, abi.atomic_u64_lock_free,
           abi.tagged_freelist_supported, abi.tagged_freelist_tag_bits,
           abi.lockless_remote_primitives_supported,
           abi.lockless_remote_config_supported, abi.malloc_min_alignment,
           abi.cacheline_size, abi.call_abi);
}

static void bench_platform_named_comparisons(void) {
#if defined(_WIN32)
    printf("platform comparison: Windows HeapAlloc and VirtualAlloc covered\n");
#else
    printf("platform comparison: system malloc and mmap covered\n");
#endif
}

static void bench_heap_open(bench_heap_env *env, size_t arena_size) {
    uhm_platform_heap_config config;
    memset(env, 0, sizeof(*env));
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = UHM_ABI_MAJOR;
    config.abi_minor = UHM_ABI_MINOR;
    config.arena_size = arena_size;
    config.heap_flags = UHM_CONFIG_SPINLOCK_YIELD;
    config.spinlock_yield_after_spins = 256u;
    bench_expect_ok(uhm_platform_heap_open(&config, &env->owner, &env->heap),
                    "benchmark heap open");
    env->api = uhm_get_api(env->heap);
    bench_expect(env->api != 0, "benchmark api");
}

static void bench_heap_close(bench_heap_env *env) {
    bench_expect_ok(uhm_platform_heap_close(&env->owner),
                    "benchmark heap close");
}

static void bench_check_heap(bench_heap_env *env, const char *message) {
    bench_expect_ok(env->api->check(env->heap, 0), message);
}

static void *bench_os_map(size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *map = mmap(0, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return map == MAP_FAILED ? 0 : map;
#endif
}

static void bench_os_unmap(void *addr, size_t size) {
#if defined(_WIN32)
    (void)size;
    bench_expect(VirtualFree(addr, 0, MEM_RELEASE) != 0, "system unmap");
#else
    bench_expect(munmap(addr, size) == 0, "system unmap");
#endif
}

static bench_result bench_uhm_small(bench_heap_env *env, size_t iterations) {
    enum { slot_count = 128 };
    void *slots[slot_count];
    size_t i;
    uint64_t begin;
    uint64_t end;
    memset(slots, 0, sizeof(slots));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 17u) % slot_count;
        size_t size = 16u + ((i * 13u) % 240u);
        if (slots[slot]) {
            env->api->free(env->heap, slots[slot]);
        }
        slots[slot] = env->api->malloc(env->heap, size);
        bench_expect(slots[slot] != 0, "uhm small malloc");
        memset(slots[slot], (int)(i & 0xffu), size);
        bench_sink ^= (uintptr_t)slots[slot];
    }
    for (i = 0; i < slot_count; ++i) {
        if (slots[i]) {
            env->api->free(env->heap, slots[i]);
        }
    }
    end = bench_now_ns();
    return bench_result_make("uhm small malloc/free", begin, end,
                             iterations + slot_count);
}

static bench_result bench_system_small(size_t iterations) {
    enum { slot_count = 128 };
    void *slots[slot_count];
    size_t i;
    uint64_t begin;
    uint64_t end;
    memset(slots, 0, sizeof(slots));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 17u) % slot_count;
        size_t size = 16u + ((i * 13u) % 240u);
        free(slots[slot]);
        slots[slot] = malloc(size);
        bench_expect(slots[slot] != 0, "system small malloc");
        memset(slots[slot], (int)(i & 0xffu), size);
        bench_sink ^= (uintptr_t)slots[slot];
    }
    for (i = 0; i < slot_count; ++i) {
        free(slots[i]);
    }
    end = bench_now_ns();
    return bench_result_make("system malloc small", begin, end,
                             iterations + slot_count);
}

static int bench_thread_run(bench_thread_args *args) {
    enum { slot_count = 64 };
    void *slots[slot_count];
    size_t i;
    memset(slots, 0, sizeof(slots));
    args->ok = 0;
    for (i = 0; i < args->iterations; ++i) {
        size_t slot = (i * 7u) % slot_count;
        size_t size = 24u + ((i * 19u) % 384u);
        if (args->use_system) {
            free(slots[slot]);
            slots[slot] = malloc(size);
        } else {
            args->env->api->free(args->env->heap, slots[slot]);
            slots[slot] = args->env->api->malloc(args->env->heap, size);
        }
        if (!slots[slot]) {
            goto fail;
        }
        memset(slots[slot], (int)(size & 0xffu), size);
        bench_sink ^= (uintptr_t)slots[slot];
    }
    args->ok = 1;
fail:
    for (i = 0; i < slot_count; ++i) {
        if (args->use_system) {
            free(slots[i]);
        } else {
            args->env->api->free(args->env->heap, slots[i]);
        }
    }
    return args->ok;
}

#if defined(_WIN32)
static DWORD WINAPI bench_thread_entry(LPVOID user) {
    return bench_thread_run((bench_thread_args *)user) ? 0u : 1u;
}
#else
static void *bench_thread_entry(void *user) {
    (void)bench_thread_run((bench_thread_args *)user);
    return 0;
}
#endif

static bench_result bench_multithread_small(bench_heap_env *env,
                                            size_t iterations,
                                            int use_system) {
    enum { thread_count = 4 };
    bench_thread_args args[thread_count];
    size_t i;
    uint64_t begin;
    uint64_t end;
#if defined(_WIN32)
    HANDLE threads[thread_count];
#else
    pthread_t threads[thread_count];
#endif
    memset(args, 0, sizeof(args));
    begin = bench_now_ns();
    for (i = 0; i < thread_count; ++i) {
        args[i].env = env;
        args[i].iterations = iterations;
        args[i].use_system = use_system;
#if defined(_WIN32)
        threads[i] = CreateThread(0, 0, bench_thread_entry, &args[i], 0, 0);
        bench_expect(threads[i] != 0, "benchmark thread create");
#else
        bench_expect(pthread_create(&threads[i], 0, bench_thread_entry,
                                    &args[i]) == 0,
                     "benchmark thread create");
#endif
    }
    for (i = 0; i < thread_count; ++i) {
#if defined(_WIN32)
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], 0);
#endif
        bench_expect(args[i].ok, "benchmark thread result");
    }
    end = bench_now_ns();
    return bench_result_make(use_system ? "system malloc multithread"
                                        : "uhm small malloc/free multithread",
                             begin, end, iterations * thread_count);
}

static bench_result bench_large_malloc(bench_heap_env *env, size_t iterations) {
    size_t i;
    size_t page_size = env->owner.vm.page_size;
    uint64_t begin;
    uint64_t end;
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = 2u * page_size + ((i % 8u) * page_size);
        void *ptr = env->api->malloc(env->heap, size);
        bench_expect(ptr != 0, "uhm large malloc");
        memset(ptr, (int)i, size);
        ptr = env->api->realloc(env->heap, ptr, size + page_size);
        bench_expect(ptr != 0, "uhm large realloc");
        memset(ptr, (int)(i + 1u), size + page_size);
        bench_sink ^= (uintptr_t)ptr;
        env->api->free(env->heap, ptr);
    }
    end = bench_now_ns();
    return bench_result_make("uhm large malloc/realloc/free", begin, end,
                             iterations * 3u);
}

static bench_result bench_system_large_malloc(size_t iterations,
                                              size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = 2u * page_size + ((i % 8u) * page_size);
        void *ptr = malloc(size);
        bench_expect(ptr != 0, "system large malloc");
        memset(ptr, (int)i, size);
        ptr = realloc(ptr, size + page_size);
        bench_expect(ptr != 0, "system large realloc");
        memset(ptr, (int)(i + 1u), size + page_size);
        bench_sink ^= (uintptr_t)ptr;
        free(ptr);
    }
    end = bench_now_ns();
    return bench_result_make("system malloc large/realloc/free", begin, end,
                             iterations * 3u);
}

static bench_result bench_uhm_mmap(bench_heap_env *env, size_t iterations,
                                   size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = page_size * (4u + (i % 4u));
        void *map = 0;
        bench_expect_ok(env->api->mmap(env->heap, &map, size,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "uhm mmap benchmark");
        memset(map, (int)i, size);
        bench_sink ^= (uintptr_t)map;
        bench_expect_ok(env->api->munmap(env->heap, map, size),
                        "uhm munmap benchmark");
    }
    end = bench_now_ns();
    return bench_result_make("uhm mmap/munmap large", begin, end,
                             iterations * 2u);
}

static bench_result bench_system_mmap(size_t iterations, size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t size = page_size * (4u + (i % 4u));
        void *map = bench_os_map(size);
        bench_expect(map != 0, "system map benchmark");
        memset(map, (int)i, size);
        bench_sink ^= (uintptr_t)map;
        bench_os_unmap(map, size);
    }
    end = bench_now_ns();
    return bench_result_make("system mmap/VirtualAlloc large", begin, end,
                             iterations * 2u);
}

static bench_result bench_fixed_mix(bench_heap_env *env, size_t iterations,
                                    size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *fixed;
        bench_expect_ok(env->api->mmap(env->heap, &map, page_size * 8u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "fixed mix source mmap");
        fixed = (void *)((uintptr_t)map + page_size * 2u);
        bench_expect_ok(env->api->mmap(env->heap, &fixed, page_size * 2u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE | UHM_MAP_FIXED),
                        "fixed mix replace");
        bench_expect_ok(env->api->munmap(env->heap, map, page_size * 8u),
                        "fixed mix cleanup");
    }
    end = bench_now_ns();
    return bench_result_make("uhm mmap fixed/auto mix", begin, end,
                             iterations * 3u);
}

static bench_result bench_mprotect_split(bench_heap_env *env, size_t iterations,
                                         size_t page_size) {
    size_t i;
    uint64_t begin;
    uint64_t end;
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        void *map = 0;
        void *middle;
        bench_expect_ok(env->api->mmap(env->heap, &map, page_size * 6u,
                                       UHM_PROT_READ | UHM_PROT_WRITE,
                                       UHM_MAP_PRIVATE),
                        "mprotect split mmap");
        middle = (void *)((uintptr_t)map + page_size * 2u);
        bench_expect_ok(env->api->mprotect(env->heap, middle, page_size * 2u,
                                           UHM_PROT_READ),
                        "mprotect split read");
        bench_expect_ok(env->api->mprotect(env->heap, middle, page_size * 2u,
                                           UHM_PROT_READ | UHM_PROT_WRITE),
                        "mprotect split write");
        bench_expect_ok(env->api->munmap(env->heap, map, page_size * 6u),
                        "mprotect split cleanup");
    }
    end = bench_now_ns();
    return bench_result_make("uhm mprotect split/merge cost", begin, end,
                             iterations * 4u);
}

static bench_result bench_collision_pressure(bench_heap_env *env,
                                             size_t page_size) {
    enum { max_mallocs = 4096, max_maps = 4096 };
    void *mallocs[max_mallocs];
    void *maps[max_maps];
    size_t malloc_count = 0;
    size_t map_count = 0;
    size_t step = 0;
    uint64_t begin;
    uint64_t end;
    memset(mallocs, 0, sizeof(mallocs));
    memset(maps, 0, sizeof(maps));
    begin = bench_now_ns();
    while (malloc_count < max_mallocs && map_count < max_maps) {
        void *ptr = env->api->malloc(env->heap, 2048u + ((step % 8u) * 512u));
        void *map = 0;
        if (!ptr) {
            break;
        }
        mallocs[malloc_count++] = ptr;
        if (env->api->mmap(env->heap, &map, page_size,
                           UHM_PROT_READ | UHM_PROT_WRITE,
                           UHM_MAP_PRIVATE) != UHM_OK) {
            break;
        }
        maps[map_count++] = map;
        ++step;
    }
    bench_check_heap(env, "collision pressure check after allocation phase");
    while (map_count != 0) {
        --map_count;
        (void)env->api->munmap(env->heap, maps[map_count], page_size);
    }
    bench_check_heap(env, "collision pressure check after mmap cleanup");
    while (malloc_count != 0) {
        --malloc_count;
        env->api->free(env->heap, mallocs[malloc_count]);
    }
    bench_check_heap(env, "collision pressure check after malloc cleanup");
    end = bench_now_ns();
    return bench_result_make("uhm malloc/mmap collision pressure", begin, end,
                             step * 2u);
}

static bench_result bench_release_pressure(bench_heap_env *env,
                                           size_t iterations,
                                           size_t page_size) {
    enum { slot_count = 96 };
    void *mallocs[slot_count];
    void *maps[slot_count];
    size_t map_sizes[slot_count];
    size_t i;
    uint64_t begin;
    uint64_t end;
    memset(mallocs, 0, sizeof(mallocs));
    memset(maps, 0, sizeof(maps));
    memset(map_sizes, 0, sizeof(map_sizes));
    begin = bench_now_ns();
    for (i = 0; i < iterations; ++i) {
        size_t slot = (i * 13u) % slot_count;
        if (mallocs[slot]) {
            env->api->free(env->heap, mallocs[slot]);
            mallocs[slot] = 0;
        }
        if (maps[slot]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[slot],
                                             map_sizes[slot]),
                            "release pressure munmap");
            maps[slot] = 0;
            map_sizes[slot] = 0;
        }
        mallocs[slot] = env->api->malloc(env->heap,
                                         64u + ((i * 37u) % 8192u));
        bench_expect(mallocs[slot] != 0, "release pressure malloc");
        memset(mallocs[slot], (int)i, 64u);
        bench_sink ^= (uintptr_t)mallocs[slot];
        if ((i % 3u) == 0) {
            map_sizes[slot] = page_size * (1u + (i % 4u));
            bench_expect_ok(env->api->mmap(env->heap, &maps[slot],
                                           map_sizes[slot],
                                           UHM_PROT_READ | UHM_PROT_WRITE,
                                           UHM_MAP_PRIVATE),
                            "release pressure mmap");
            if ((i % 5u) == 0) {
                bench_expect_ok(env->api->mprotect(env->heap, maps[slot],
                                                   page_size, UHM_PROT_READ),
                                "release pressure mprotect");
            }
        }
    }
    for (i = 0; i < slot_count; ++i) {
        if (maps[i]) {
            bench_expect_ok(env->api->munmap(env->heap, maps[i], map_sizes[i]),
                            "release pressure cleanup map");
        }
        env->api->free(env->heap, mallocs[i]);
    }
    end = bench_now_ns();
    return bench_result_make("uhm release mixed long pressure", begin, end,
                             iterations);
}

int main(int argc, char **argv) {
    bench_options options;
    bench_heap_env env;
    uhm_vm_ops vm;
    size_t page_size;
    bench_result result;
    int parse_result;
#define BENCH_RUN_UHM_CASE(expr, check_message)                                  \
    do {                                                                         \
        bench_heap_open(&env, options.arena_size);                               \
        result = (expr);                                                         \
        bench_check_heap(&env, (check_message));                                 \
        bench_print(&result);                                                    \
        bench_heap_close(&env);                                                  \
    } while (0)

    parse_result = bench_parse_options(argc, argv, &options);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        bench_usage(argv[0]);
        return 2;
    }

    bench_expect_ok(uhm_platform_vm_ops_init(&vm), "benchmark vm init");
    page_size = vm.page_size ? vm.page_size : uhm_platform_system_page_size();

    printf("UHMAS benchmark mode: %s\n", options.stress ? "stress" : "quick");
    printf("arena_size=%zu page_size=%zu\n", options.arena_size, page_size);
    bench_print_abi_capabilities();
    bench_platform_named_comparisons();

    BENCH_RUN_UHM_CASE(bench_uhm_small(&env, options.small_iters),
                       "check after uhm small benchmark");
    result = bench_system_small(options.small_iters);
    bench_print(&result);

    BENCH_RUN_UHM_CASE(bench_multithread_small(&env, options.thread_iters, 0),
                       "check after uhm multithread small benchmark");
    result = bench_multithread_small(&env, options.thread_iters, 1);
    bench_print(&result);

    BENCH_RUN_UHM_CASE(bench_large_malloc(&env, options.large_iters),
                       "check after uhm large benchmark");
    result = bench_system_large_malloc(options.large_iters, page_size);
    bench_print(&result);

    BENCH_RUN_UHM_CASE(bench_uhm_mmap(&env, options.mmap_iters, page_size),
                       "check after uhm mmap benchmark");
    result = bench_system_mmap(options.mmap_iters, page_size);
    bench_print(&result);

    BENCH_RUN_UHM_CASE(bench_fixed_mix(&env, options.fixed_iters, page_size),
                       "check after uhm fixed mix benchmark");

    BENCH_RUN_UHM_CASE(bench_mprotect_split(&env, options.fixed_iters,
                                            page_size),
                       "check after uhm mprotect benchmark");

    BENCH_RUN_UHM_CASE(bench_collision_pressure(&env, page_size),
                       "check after uhm collision benchmark");

    BENCH_RUN_UHM_CASE(bench_release_pressure(&env, options.release_iters,
                                              page_size),
                       "check after uhm release pressure benchmark");

#undef BENCH_RUN_UHM_CASE
    return 0;
}
