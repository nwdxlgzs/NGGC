#include "nggc/nggc.h"
#include "nggc_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NGGC_HAVE_C11_THREADS
#define NGGC_HAVE_C11_THREADS 0
#endif
#if NGGC_HAVE_C11_THREADS
#include <threads.h>
#endif

typedef struct create_args {
    nggc_heap *heap;
    void *queue;
    void *object;
    unsigned kind;
    nggc_result result;
} create_args;

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void require_ok(nggc_result result, const char *message) {
    if (result != NGGC_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static nggc_stats stats_of(nggc_heap *heap) {
    nggc_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    require_ok(nggc_get_stats(heap, &stats), "read error-result heap stats");
    return stats;
}

static int create_main(void *ctx) {
    create_args *args = (create_args *)ctx;
    switch (args->kind) {
        case 0u:
            args->result = nggc_weak_ref_create(args->heap, 0, &args->object);
            break;
        case 1u:
            args->result = nggc_soft_ref_create(args->heap, 0, &args->object);
            break;
        case 2u:
            args->result = nggc_phantom_queue_create(args->heap, &args->object);
            break;
        case 3u:
            args->result = nggc_phantom_ref_create(
                args->heap, 0, args->queue, &args->object);
            break;
        default: require(0, "valid reference creator kind"); break;
    }
    return 0;
}

#if NGGC_HAVE_C11_THREADS
typedef struct diagnostic_args {
    nggc_heap *heap;
    atomic_uint *started;
    atomic_uint *finished;
    unsigned kind;
} diagnostic_args;

static int diagnostic_main(void *ctx) {
    diagnostic_args *args = (diagnostic_args *)ctx;
    int invalid_referent = 0;
    atomic_fetch_add_explicit(args->started, 1u, memory_order_release);
    while (atomic_load_explicit(args->started, memory_order_acquire) != 3u) {
        thrd_yield();
    }
    for (size_t i = 0u; i < 4000u; ++i) {
        if (args->kind == 0u) {
            require(nggc_pin(args->heap, 0) == NGGC_ERR_INVALID_ARGUMENT,
                    "invalid pin returns its own error during other publication");
        } else if (args->kind == 1u) {
            void *output = &invalid_referent;
            require(nggc_weak_ref_create(args->heap, &invalid_referent, &output) ==
                        NGGC_ERR_CORRUPTION && output == 0,
                    "invalid referent creation returns its own error concurrently");
        } else {
            nggc_result diagnostic = nggc_get_last_error(args->heap);
            require(diagnostic == NGGC_ERR_INVALID_ARGUMENT ||
                        diagnostic == NGGC_ERR_CORRUPTION,
                    "concurrent diagnostic reads observe a published result");
        }
    }
    atomic_fetch_add_explicit(args->finished, 1u, memory_order_release);
    return 0;
}

static void run_diagnostic_overlap(nggc_heap *heap) {
    thrd_t threads[3];
    diagnostic_args args[3];
    atomic_uint started;
    atomic_uint finished;
    atomic_init(&started, 0u);
    atomic_init(&finished, 0u);
    require(nggc_pin(heap, 0) == NGGC_ERR_INVALID_ARGUMENT,
            "prime diagnostic before overlapping publishers");
    for (unsigned i = 0u; i < 3u; ++i) {
        args[i].heap = heap;
        args[i].started = &started;
        args[i].finished = &finished;
        args[i].kind = i;
        require(thrd_create(&threads[i], diagnostic_main, &args[i]) == thrd_success,
                "start concurrent error publisher or observer");
    }
    for (size_t i = 0u; i < 3u; ++i) {
        require(thrd_join(threads[i], 0) == thrd_success,
                "join concurrent error publisher or observer");
    }
    require(atomic_load_explicit(&finished, memory_order_acquire) == 3u,
            "all diagnostic participants complete");
}
#endif

int main(void) {
    nggc_config config;
    nggc_heap *heap = 0;
    void *queue = 0;
    nggc_stats before;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_trigger_policy = NGGC_TRIGGER_MANUAL;
    config.region_size = (size_t)24u * 1024u;
    config.gc_arena_size = config.region_size * 16u;
    config.uhm_arena_size = (size_t)4u * 1024u * 1024u;
    require_ok(nggc_open(&config, &heap), "open error-result heap");
    require(nggc_get_last_error(heap) == NGGC_OK, "initialize diagnostic atomically");
    require_ok(nggc_phantom_queue_create(heap, &queue), "create error-result queue");
    require_ok(nggc_root_push(heap, &queue), "root error-result queue");
    before = stats_of(heap);
    require_ok(nggc_request_safepoint(heap), "hold allocation-rejecting safepoint");
    for (unsigned kind = 0u; kind < 4u; ++kind) {
        create_args args;
        memset(&args, 0, sizeof(args));
        args.heap = heap;
        args.queue = queue;
        args.object = queue;
        args.kind = kind;
        args.result = NGGC_OK;
#if NGGC_HAVE_C11_THREADS
        thrd_t creator;
        require_ok(nggc_internal_test_reference_create_pause_arm(heap),
                   "pause reference creator after it captures its failure");
        require(thrd_create(&creator, create_main, &args) == thrd_success,
                "start failing reference creator");
        while (nggc_internal_test_reference_create_pause_wait(heap) != NGGC_OK) {
            thrd_yield();
        }
        require(nggc_pin(heap, 0) == NGGC_ERR_INVALID_ARGUMENT &&
                    nggc_get_last_error(heap) == NGGC_ERR_INVALID_ARGUMENT,
                "another thread replaces shared diagnostic before creator returns");
        require_ok(nggc_internal_test_reference_create_pause_release(heap),
                   "release paused reference creator");
        require(thrd_join(creator, 0) == thrd_success, "join failing reference creator");
#else
        (void)create_main(&args);
#endif
        require(args.result == NGGC_ERR_BUSY && args.object == 0,
                "reference creator preserves its own failure and null output");
    }
    require_ok(nggc_safepoint_release(heap), "release error-result safepoint");
#if NGGC_HAVE_C11_THREADS
    run_diagnostic_overlap(heap);
#endif
    {
        nggc_stats after = stats_of(heap);
        require(after.allocation_count == before.allocation_count &&
                    after.live_object_count == before.live_object_count &&
                    after.root_count == before.root_count &&
                    after.safepoint_active == 0u,
                "failed creators and diagnostics preserve heap ownership");
    }
    require_ok(nggc_check(heap), "verify error-result heap");
    require_ok(nggc_root_pop(heap, &queue), "unroot error-result queue");
    require_ok(nggc_collect(heap), "reclaim error-result queue");
    require_ok(nggc_close(heap), "close error-result heap");
    return 0;
}
