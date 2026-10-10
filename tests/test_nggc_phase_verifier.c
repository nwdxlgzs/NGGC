#include "nggc/nggc.h"
#include "nggc_internal.h"

#include <stdint.h>
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

static void expect_true(int value, const char *message) {
    if (!value) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void expect_ok(nggc_result result, const char *message) {
    if (result != NGGC_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static void run_adaptive_tlab_sizing_case(void) {
    const size_t region_size = (size_t)24u * 1024u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    nggc_thread *thread = 0;
    uint16_t type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)12u * region_size;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = region_size;
    config.tlab_sizing_policy = NGGC_TLAB_SIZING_POLICY_SMART;
    config.tlab_size = (size_t)4u * 1024u;
    config.tlab_min_size = (size_t)2u * 1024u;
    config.tlab_max_size = (size_t)16u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open adaptive TLAB heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = 512u;
    desc.name = "adaptive-tlab-leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register adaptive TLAB type");
    expect_ok(nggc_type_table_freeze(heap),
              "freeze adaptive TLAB type table");
    expect_ok(nggc_thread_register(heap, &thread),
              "register adaptive TLAB thread");

    for (size_t i = 0u; i < 15u; ++i) {
        expect_true(nggc_thread_alloc(heap, thread, type_id) != 0,
                    "fill adaptive TLAB high-utilization spans");
    }
    expect_ok(nggc_internal_tlab_retire(heap, thread),
              "retire first low-utilization adaptive span");
    for (size_t i = 0u; i < 2u; ++i) {
        expect_ok(nggc_internal_tlab_reserve(
                      heap, thread, (size_t)8u * 1024u),
                  "reserve low-utilization adaptive span");
        expect_true(nggc_internal_tlab_alloc_size(
                        heap, thread, type_id, 512u) != 0,
                    "allocate low-utilization adaptive object");
        expect_ok(nggc_internal_tlab_retire(heap, thread),
                  "retire low-utilization adaptive span");
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read adaptive TLAB statistics");
    expect_true(
        stats.region_size == region_size &&
            stats.tlab_sizing_policy == NGGC_TLAB_SIZING_POLICY_SMART &&
            stats.tlab_size == (size_t)4u * 1024u &&
            stats.tlab_min_size == (size_t)2u * 1024u &&
            stats.tlab_max_size == (size_t)16u * 1024u &&
            stats.tlab_target_min_size == (size_t)4u * 1024u &&
            stats.tlab_target_max_size == (size_t)4u * 1024u &&
            stats.tlab_sizing_sample_count == 5u &&
            stats.tlab_sizing_grow_count == 1u &&
            stats.tlab_sizing_shrink_count == 1u &&
            stats.tlab_sizing_hold_count == 3u &&
            stats.active_tlab_count == 0u,
        "adaptive TLAB hysteresis is deterministic");
    expect_ok(nggc_check(heap), "verify adaptive TLAB heap");
    expect_ok(nggc_thread_unregister(heap, thread),
              "unregister adaptive TLAB thread");
    expect_ok(nggc_collect(heap), "collect adaptive TLAB heap");
    expect_ok(nggc_close(heap), "close adaptive TLAB heap");
}

static void run_large_free_bin_fault_case(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *large;
    void *middle_guard;
    void *second_large;
    void *guard;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)16u * region_size;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = region_size;
    expect_ok(nggc_open(&config, &heap),
              "large free bin fault heap open");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "large-free-bin-fault-leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register large free bin fault type");
    prefix = nggc_alloc_size(heap, type_id, 512u);
    large = nggc_alloc_size(heap, type_id,
                            (size_t)3u * region_size);
    middle_guard = nggc_alloc_size(heap, type_id, region_size);
    second_large = nggc_alloc_size(heap, type_id,
                                   (size_t)2u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    expect_true(prefix != 0 && large != 0 && middle_guard != 0 &&
                    second_large != 0 && guard != 0,
                "allocate large free bin fault objects");
    expect_ok(nggc_root_push(heap, &prefix),
              "root large free bin fault prefix");
    expect_ok(nggc_root_push(heap, &middle_guard),
              "root large free bin fault middle guard");
    expect_ok(nggc_root_push(heap, &guard),
              "root large free bin fault guard");
    expect_ok(nggc_collect(heap),
              "create large free run for bin faults");
    expect_true(nggc_internal_test_large_free_bin_fault_check(
                    heap, NGGC_TEST_LARGE_BIN_FAULT_MASK) ==
                    NGGC_ERR_CORRUPTION,
                "large free bin mask corruption detected");
    expect_ok(nggc_check(heap),
              "large free bin mask restored");
    expect_true(nggc_internal_test_large_free_bin_fault_check(
                    heap, NGGC_TEST_LARGE_BIN_FAULT_WRONG_BIN) ==
                    NGGC_ERR_CORRUPTION,
                "wrong large free bin membership detected");
    expect_ok(nggc_check(heap),
              "large free bin membership restored");
    expect_true(nggc_internal_test_large_free_bin_fault_check(
                    heap, NGGC_TEST_LARGE_BIN_FAULT_ORDER) ==
                    NGGC_ERR_CORRUPTION,
                "large free bin ordering corruption detected");
    expect_ok(nggc_check(heap),
              "large free bin ordering restored");
    expect_ok(nggc_root_pop(heap, &guard),
              "unroot large free bin fault guard");
    expect_ok(nggc_root_pop(heap, &middle_guard),
              "unroot large free bin fault middle guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot large free bin fault prefix");
    expect_ok(nggc_close(heap),
              "large free bin fault heap close");
}

static void run_decommitted_run_fault_case(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *large;
    void *middle_guard;
    void *second_large;
    void *guard;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)32u * region_size;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = region_size;
    config.interior_decommit_min_regions = 8u;
    expect_ok(nggc_open(&config, &heap),
              "decommitted run fault heap open");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "decommitted-run-fault-leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register decommitted run fault type");
    prefix = nggc_alloc_size(heap, type_id, 512u);
    large = nggc_alloc_size(heap, type_id,
                            (size_t)8u * region_size);
    middle_guard = nggc_alloc_size(heap, type_id, region_size);
    second_large = nggc_alloc_size(heap, type_id,
                                   (size_t)9u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    expect_true(prefix != 0 && large != 0 && middle_guard != 0 &&
                    second_large != 0 && guard != 0,
                "allocate decommitted run fault objects");
    expect_ok(nggc_root_push(heap, &prefix),
              "root decommitted run fault prefix");
    expect_ok(nggc_root_push(heap, &middle_guard),
              "root decommitted run fault middle guard");
    expect_ok(nggc_root_push(heap, &guard),
              "root decommitted run fault guard");
    expect_ok(nggc_collect(heap),
              "create decommitted run for faults");
    expect_true(nggc_internal_test_decommitted_run_fault_check(
                    heap, NGGC_TEST_DECOMMITTED_BIN_FAULT_MASK) ==
                    NGGC_ERR_CORRUPTION,
                "decommitted run bin mask corruption detected");
    expect_ok(nggc_check(heap),
              "decommitted run bin mask restored");
    expect_true(nggc_internal_test_decommitted_run_fault_check(
                    heap, NGGC_TEST_DECOMMITTED_RUN_FAULT_OFFSET) ==
                    NGGC_ERR_CORRUPTION,
                "decommitted continuation offset corruption detected");
    expect_ok(nggc_check(heap),
              "decommitted continuation offset restored");
    expect_true(nggc_internal_test_decommitted_run_fault_check(
                    heap, NGGC_TEST_DECOMMITTED_BIN_FAULT_ORDER) ==
                    NGGC_ERR_CORRUPTION,
                "decommitted run ordering corruption detected");
    expect_ok(nggc_check(heap),
              "decommitted run ordering restored");
    expect_ok(nggc_root_pop(heap, &guard),
              "unroot decommitted run fault guard");
    expect_ok(nggc_root_pop(heap, &middle_guard),
              "unroot decommitted run fault middle guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot decommitted run fault prefix");
    expect_ok(nggc_close(heap),
              "decommitted run fault heap close");
}

typedef struct evacuation_fault_node {
    struct evacuation_fault_node *edge;
    int value;
} evacuation_fault_node;

static void scan_evacuation_fault_node(void *object,
                                       nggc_visit_fn visit,
                                       void *ctx) {
    evacuation_fault_node *node =
        (evacuation_fault_node *)object;
    visit(ctx, (void **)&node->edge);
}

#if NGGC_HAVE_C11_THREADS
typedef struct concurrent_window_node {
    struct concurrent_window_node *edge;
    unsigned char padding[2048];
} concurrent_window_node;

typedef struct concurrent_window_collect_args {
    nggc_heap *heap;
    nggc_result result;
} concurrent_window_collect_args;

typedef struct concurrent_window_mutator_args {
    nggc_heap *heap;
    nggc_thread *thread;
    concurrent_window_node **nodes;
    atomic_uint started;
    atomic_uint stop;
    atomic_uint window_operations;
    nggc_result result;
} concurrent_window_mutator_args;

static void scan_concurrent_window_node(void *object,
                                        nggc_visit_fn visit,
                                        void *ctx) {
    concurrent_window_node *node =
        (concurrent_window_node *)object;
    visit(ctx, (void **)&node->edge);
}

static int concurrent_window_collect_main(void *arg) {
    concurrent_window_collect_args *args =
        (concurrent_window_collect_args *)arg;
    do {
        args->result = nggc_collect_young(args->heap);
        if (args->result == NGGC_ERR_BUSY) {
            thrd_yield();
        }
    } while (args->result == NGGC_ERR_BUSY);
    return 0;
}

static int concurrent_window_mutator_main(void *arg) {
    concurrent_window_mutator_args *args =
        (concurrent_window_mutator_args *)arg;
    nggc_stats stats;
    atomic_store_explicit(&args->started, 1u, memory_order_release);
    args->result = NGGC_OK;
    while (atomic_load_explicit(&args->stop, memory_order_acquire) == 0u) {
        nggc_result poll_result =
            nggc_safepoint_park(args->heap, args->thread);
        if (poll_result != NGGC_OK && poll_result != NGGC_ERR_BUSY) {
            args->result = poll_result;
            break;
        }
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        if (nggc_get_stats(args->heap, &stats) != NGGC_OK) {
            args->result = NGGC_ERR_CORRUPTION;
            break;
        }
        if (stats.mutator_concurrent_active != 0u) {
            void *loaded = nggc_load(args->heap,
                                     (void **)&args->nodes[0]->edge);
            (void)nggc_store(args->heap, args->nodes[0],
                             (void **)&args->nodes[0]->edge, loaded);
            atomic_fetch_add_explicit(&args->window_operations, 1u,
                                      memory_order_relaxed);
        }
        thrd_yield();
    }
    return 0;
}

typedef struct tlab_fast_quiescence_args {
    nggc_heap *heap;
    uint16_t type_id;
    atomic_uint ready;
    atomic_uint start;
    nggc_result result;
} tlab_fast_quiescence_args;

typedef struct tlab_fast_request_args {
    nggc_heap *heap;
    nggc_result result;
} tlab_fast_request_args;

typedef struct tlab_fast_late_enrollment_args {
    nggc_heap *heap;
    uint16_t type_id;
    atomic_uint ready;
    atomic_uint start;
    nggc_result result;
} tlab_fast_late_enrollment_args;

typedef struct tlab_fast_stats_args {
    nggc_heap *heap;
    atomic_uint done;
    nggc_result result;
} tlab_fast_stats_args;

static int tlab_fast_quiescence_mutator_main(void *arg) {
    tlab_fast_quiescence_args *args =
        (tlab_fast_quiescence_args *)arg;
    nggc_thread *thread = 0;
    args->result = nggc_thread_register(args->heap, &thread);
    if (args->result != NGGC_OK ||
        !nggc_thread_alloc(args->heap, thread, args->type_id)) {
        args->result = NGGC_ERR_CORRUPTION;
        atomic_store_explicit(&args->ready, 1u, memory_order_release);
        return 1;
    }
    atomic_store_explicit(&args->ready, 1u, memory_order_release);
    while (atomic_load_explicit(&args->start, memory_order_acquire) == 0u) {
        thrd_yield();
    }
    if (!nggc_thread_alloc(args->heap, thread, args->type_id)) {
        args->result = NGGC_ERR_CORRUPTION;
        return 1;
    }
    args->result = nggc_safepoint_park(args->heap, thread);
    if (args->result == NGGC_OK) {
        args->result = nggc_thread_unregister(args->heap, thread);
    }
    return args->result == NGGC_OK ? 0 : 1;
}

static int tlab_fast_request_main(void *arg) {
    tlab_fast_request_args *args = (tlab_fast_request_args *)arg;
    args->result = nggc_request_safepoint(args->heap);
    return args->result == NGGC_OK ? 0 : 1;
}

static int tlab_fast_late_enrollment_mutator_main(void *arg) {
    tlab_fast_late_enrollment_args *args =
        (tlab_fast_late_enrollment_args *)arg;
    nggc_thread *thread = 0;
    args->result = nggc_thread_register(args->heap, &thread);
    if (args->result != NGGC_OK ||
        !nggc_thread_alloc(args->heap, thread, args->type_id)) {
        args->result = NGGC_ERR_CORRUPTION;
        atomic_store_explicit(&args->ready, 1u, memory_order_release);
        return 1;
    }
    atomic_store_explicit(&args->ready, 1u, memory_order_release);
    while (atomic_load_explicit(&args->start, memory_order_acquire) == 0u) {
        thrd_yield();
    }
    if (!nggc_thread_alloc(args->heap, thread, args->type_id)) {
        args->result = NGGC_ERR_CORRUPTION;
        return 1;
    }
    args->result = nggc_thread_unregister(args->heap, thread);
    return args->result == NGGC_OK ? 0 : 1;
}

static int tlab_fast_stats_main(void *arg) {
    tlab_fast_stats_args *args = (tlab_fast_stats_args *)arg;
    nggc_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    args->result = nggc_get_stats(args->heap, &stats);
    atomic_store_explicit(&args->done, 1u, memory_order_release);
    return args->result == NGGC_OK ? 0 : 1;
}

static void run_tlab_fast_quiescence_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    tlab_fast_quiescence_args mutator_args;
    tlab_fast_request_args request_args;
    thrd_t mutator;
    thrd_t requester;
    uint16_t type_id = NGGC_TYPE_NONE;
    size_t pending = 1u;
    int observed = 0;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = (size_t)24u * 1024u;
    config.tlab_size = (size_t)4u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open TLAB fast quiescence heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = 32u;
    desc.name = "tlab-fast-quiescence";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register TLAB fast quiescence type");
    expect_ok(nggc_type_table_freeze(heap),
              "freeze TLAB fast quiescence types");

    memset(&mutator_args, 0, sizeof(mutator_args));
    mutator_args.heap = heap;
    mutator_args.type_id = type_id;
    atomic_init(&mutator_args.ready, 0u);
    atomic_init(&mutator_args.start, 0u);
    mutator_args.result = NGGC_ERR_BUSY;
    expect_true(thrd_create(&mutator, tlab_fast_quiescence_mutator_main,
                            &mutator_args) == thrd_success,
                "start paused TLAB fast mutator");
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (atomic_load_explicit(&mutator_args.ready,
                                 memory_order_acquire) != 0u) {
            observed = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(observed && mutator_args.result == NGGC_OK,
                "TLAB fast mutator primes its private span");
    expect_ok(nggc_internal_test_tlab_fast_pause_arm(heap),
              "arm TLAB fast inflight pause");
    atomic_store_explicit(&mutator_args.start, 1u, memory_order_release);
    observed = 0;
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (nggc_internal_test_tlab_fast_pause_wait(heap) == NGGC_OK) {
            observed = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(observed, "TLAB fast allocation reaches inflight window");

    request_args.heap = heap;
    request_args.result = NGGC_ERR_BUSY;
    expect_true(thrd_create(&requester, tlab_fast_request_main,
                            &request_args) == thrd_success,
                "start safepoint against inflight TLAB allocation");
    observed = 0;
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (nggc_internal_test_tlab_fast_gate_closed(heap) == NGGC_OK) {
            observed = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(observed,
                "safepoint closes gate before inflight publication drains");
    expect_ok(nggc_internal_test_tlab_fast_pause_release(heap),
              "release inflight TLAB fast allocation");
    expect_true(thrd_join(requester, 0) == thrd_success &&
                    request_args.result == NGGC_OK,
                "safepoint waits for inflight TLAB publication");
    for (size_t i = 0u; i < 1000000u; ++i) {
        nggc_result wait_result = nggc_safepoint_wait(heap, &pending);
        if (wait_result == NGGC_OK) {
            break;
        }
        expect_true(wait_result == NGGC_ERR_BUSY,
                    "TLAB quiescence safepoint wait result");
        thrd_yield();
    }
    expect_true(pending == 0u,
                "TLAB mutator retires only after fast publication flush");
    expect_ok(nggc_safepoint_release(heap),
              "release TLAB fast quiescence safepoint");
    expect_true(thrd_join(mutator, 0) == thrd_success &&
                    mutator_args.result == NGGC_OK,
                "join TLAB fast quiescence mutator");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read TLAB fast quiescence stats");
    expect_true(stats.region_size == (size_t)24u * 1024u &&
                    stats.allocation_count == 2u &&
                    stats.tlab_fast_allocation_count == 1u &&
                    stats.tlab_fast_flushed_object_count == 1u &&
                    stats.active_tlab_count == 0u,
                "inflight fast allocation is flushed exactly once");
    expect_ok(nggc_check(heap), "verify TLAB fast quiescence heap");
    expect_ok(nggc_collect(heap), "reclaim TLAB fast quiescence heap");
    expect_ok(nggc_close(heap), "close TLAB fast quiescence heap");
}

static void run_tlab_fast_late_enrollment_case(void) {
    const size_t region_size = (size_t)24u * 1024u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    tlab_fast_late_enrollment_args mutator_args;
    tlab_fast_stats_args stats_args;
    thrd_t mutator;
    thrd_t observer;
    uint16_t type_id = NGGC_TYPE_NONE;
    int observed = 0;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = region_size;
    config.tlab_size = (size_t)4u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open TLAB late-enrollment heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = 32u;
    desc.name = "tlab-fast-late-enrollment";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register TLAB late-enrollment type");
    expect_ok(nggc_type_table_freeze(heap),
              "freeze TLAB late-enrollment types");

    memset(&mutator_args, 0, sizeof(mutator_args));
    mutator_args.heap = heap;
    mutator_args.type_id = type_id;
    atomic_init(&mutator_args.ready, 0u);
    atomic_init(&mutator_args.start, 0u);
    mutator_args.result = NGGC_ERR_BUSY;
    expect_true(thrd_create(&mutator,
                            tlab_fast_late_enrollment_mutator_main,
                            &mutator_args) == thrd_success,
                "start TLAB late-enrollment mutator");
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (atomic_load_explicit(&mutator_args.ready,
                                 memory_order_acquire) != 0u) {
            observed = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(observed && mutator_args.result == NGGC_OK,
                "late-enrollment mutator primes its private span");
    expect_ok(nggc_internal_test_tlab_fast_enrollment_pause_arm(heap),
              "arm TLAB pre-enrollment pause");
    expect_ok(nggc_internal_test_tlab_fast_flush_pause_arm(heap),
              "arm TLAB quiescent-to-flush pause");
    atomic_store_explicit(&mutator_args.start, 1u, memory_order_release);
    observed = 0;
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (nggc_internal_test_tlab_fast_enrollment_pause_wait(
                heap, 1u) == NGGC_OK) {
            observed = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(observed,
                "mutator pauses after observing the open fast gate");

    memset(&stats_args, 0, sizeof(stats_args));
    stats_args.heap = heap;
    atomic_init(&stats_args.done, 0u);
    stats_args.result = NGGC_ERR_BUSY;
    expect_true(thrd_create(&observer, tlab_fast_stats_main,
                            &stats_args) == thrd_success,
                "start stats observer against late enrollment");
    observed = 0;
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (nggc_internal_test_tlab_fast_flush_pause_wait(heap) ==
            NGGC_OK) {
            observed = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(observed,
                "observer scans zero inflight before flushing TLABs");
    expect_ok(nggc_internal_test_tlab_fast_enrollment_pause_advance(
                  heap, 1u),
              "release pre-enrollment mutator stage");
    observed = 0;
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (nggc_internal_test_tlab_fast_enrollment_pause_wait(
                heap, 2u) == NGGC_OK) {
            observed = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(observed,
                "late mutator enrolls while the fast gate is closed");
    expect_ok(nggc_internal_test_tlab_fast_flush_pause_release(heap),
              "release observer into TLAB flush");
    observed = 0;
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (nggc_internal_test_tlab_fast_flush_busy_wait(heap) ==
            NGGC_OK) {
            observed = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(observed &&
                    atomic_load_explicit(&stats_args.done,
                                         memory_order_acquire) == 0u,
                "observer retries a flush that sees late inflight work");
    expect_ok(nggc_internal_test_tlab_fast_enrollment_pause_advance(
                  heap, 2u),
              "release late enrolled mutator");
    expect_true(thrd_join(observer, 0) == thrd_success &&
                    stats_args.result == NGGC_OK,
                "stats observer completes after re-quiescing TLABs");
    expect_true(thrd_join(mutator, 0) == thrd_success &&
                    mutator_args.result == NGGC_OK,
                "late-enrollment allocation falls back and completes");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read TLAB late-enrollment statistics");
    expect_true(stats.region_size == region_size &&
                    stats.allocation_count == 2u &&
                    stats.tlab_fast_allocation_count == 0u &&
                    stats.tlab_fast_slow_path_count >= 2u &&
                    stats.active_tlab_count == 0u,
                "late enrollment neither leaks BUSY nor publishes twice");
    expect_ok(nggc_check(heap), "verify TLAB late-enrollment heap");
    expect_ok(nggc_collect(heap), "collect TLAB late-enrollment heap");
    expect_ok(nggc_close(heap), "close TLAB late-enrollment heap");
}

static void run_mutator_concurrent_window_case(void) {
    enum { OBJECT_COUNT = 32 };
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    nggc_thread *mutator_thread = 0;
    concurrent_window_node *nodes[OBJECT_COUNT];
    concurrent_window_collect_args args;
    concurrent_window_mutator_args mutator_args;
    thrd_t collector;
    thrd_t mutator;
    uint16_t type_id = NGGC_TYPE_NONE;
    int entered = 0;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)1024u * 1024u;
    config.uhm_arena_size = (size_t)4u * 1024u * 1024u;
    config.flags = NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT;
    expect_ok(nggc_open(&config, &heap),
              "open mutator-concurrent window heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(concurrent_window_node);
    desc.scan = scan_concurrent_window_node;
    desc.name = "mutator-concurrent-window-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register mutator-concurrent window node");
    expect_ok(nggc_thread_register(heap, &mutator_thread),
              "register mutator-concurrent window thread");
    memset(nodes, 0, sizeof(nodes));
    for (size_t i = 0u; i < OBJECT_COUNT; ++i) {
        nodes[i] = (concurrent_window_node *)nggc_alloc(heap, type_id);
        expect_true(nodes[i] != 0,
                    "allocate mutator-concurrent window objects");
        memset(nodes[i], 0, sizeof(*nodes[i]));
        nodes[i]->edge = nodes[i];
        expect_ok(nggc_root_push(heap, (void **)&nodes[i]),
                  "root mutator-concurrent window object");
    }
    memset(&mutator_args, 0, sizeof(mutator_args));
    mutator_args.heap = heap;
    mutator_args.thread = mutator_thread;
    mutator_args.nodes = nodes;
    atomic_init(&mutator_args.started, 0u);
    atomic_init(&mutator_args.stop, 0u);
    atomic_init(&mutator_args.window_operations, 0u);
    mutator_args.result = NGGC_ERR_BUSY;
    expect_true(thrd_create(&mutator, concurrent_window_mutator_main,
                            &mutator_args) == thrd_success,
                "start registered mutator thread");
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (atomic_load_explicit(&mutator_args.started,
                                 memory_order_acquire) != 0u) {
            break;
        }
        thrd_yield();
    }
    expect_true(atomic_load_explicit(&mutator_args.started,
                                     memory_order_acquire) != 0u,
                "registered mutator thread starts");
    expect_ok(nggc_internal_test_collect_fault_set(
                  heap, NGGC_TEST_COLLECT_ACTION_CONCURRENT_WINDOW),
              "arm mutator-concurrent window gate");
    args.heap = heap;
    args.result = NGGC_ERR_BUSY;
    expect_true(thrd_create(&collector, concurrent_window_collect_main,
                            &args) == thrd_success,
                "start mutator-concurrent collection");
    for (size_t i = 0u; i < 1000000u; ++i) {
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(heap, &stats),
                  "read mutator-concurrent safepoint stats");
        if (stats.safepoint_active != 0u &&
            stats.safepoint_pending_threads == 0u) {
            break;
        }
        thrd_yield();
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "confirm registered mutator parked");
    expect_true(stats.safepoint_active != 0u &&
                    stats.safepoint_pending_threads == 0u,
                "registered mutator reaches collection safepoint");
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (nggc_internal_test_concurrent_window_wait(heap) == NGGC_OK) {
            entered = 1;
            break;
        }
        thrd_yield();
    }
    expect_true(entered, "collector entered concurrent moving window");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read active concurrent mutator stats");
    expect_true(stats.mutator_concurrent_running_threads == 1u &&
                    stats.mutator_concurrent_repark_pending_threads == 0u &&
                    stats.safepoint_parked_threads == 0u,
                "registered mutator resumes only for concurrent barrier window");
    for (size_t i = 0u; i < 1000000u; ++i) {
        if (atomic_load_explicit(&mutator_args.window_operations,
                                 memory_order_relaxed) != 0u) {
            break;
        }
        thrd_yield();
    }
    expect_true(atomic_load_explicit(&mutator_args.window_operations,
                                     memory_order_relaxed) != 0u,
                "registered mutator executes barrier operations");
    expect_true(nggc_store(heap, 0, (void **)&nodes[0], nodes[0]) ==
                    NGGC_ERR_BUSY,
                "concurrent mutator cannot mutate root slot during window");
    expect_true(nggc_thread_alloc(heap, mutator_thread, type_id) == 0,
                "concurrent mutator cannot allocate during window");
    {
        void *candidate = nodes[0];
        expect_true(nggc_root_push(heap, &candidate) == NGGC_ERR_BUSY,
                    "concurrent mutator cannot publish a global root");
        expect_true(nggc_thread_root_push(heap, mutator_thread, &candidate) ==
                        NGGC_ERR_BUSY,
                    "concurrent mutator cannot publish a thread root");
    }
    expect_true(nggc_load(heap, (void **)&nodes[0]->edge) == nodes[0],
                "load barrier runs during concurrent moving window");
    expect_ok(nggc_store(heap, nodes[0], (void **)&nodes[0]->edge,
                         nodes[0]),
              "store barrier runs during concurrent moving window");
    expect_ok(nggc_internal_test_concurrent_window_release(heap),
              "release mutator-concurrent window gate");
    expect_true(thrd_join(collector, 0) == thrd_success,
                "join mutator-concurrent collection");
    atomic_store_explicit(&mutator_args.stop, 1u, memory_order_release);
    expect_true(thrd_join(mutator, 0) == thrd_success,
                "join registered mutator thread");
    expect_ok(mutator_args.result, "registered mutator thread completes");
    expect_ok(args.result, "mutator-concurrent collection completes");
    expect_ok(nggc_check(heap), "verify mutator-concurrent collection");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read mutator-concurrent window stats");
    expect_true(stats.mutator_concurrent_window_count == 1u &&
                    stats.mutator_concurrent_load_count != 0u &&
                    stats.mutator_concurrent_store_count != 0u &&
                    stats.mutator_concurrent_active == 0u &&
                    stats.mutator_concurrent_epoch == 0u &&
                    stats.mutator_concurrent_running_threads == 0u &&
                    stats.mutator_concurrent_repark_pending_threads == 0u &&
                    stats.mutator_concurrent_resume_count == 1u &&
                    stats.mutator_concurrent_repark_count == 1u,
                "mutator-concurrent window statistics and quiescence");
    for (size_t i = OBJECT_COUNT; i != 0u; --i) {
        expect_ok(nggc_root_pop(heap, (void **)&nodes[i - 1u]),
                  "unroot mutator-concurrent window object");
    }
    expect_ok(nggc_thread_unregister(heap, mutator_thread),
              "unregister mutator-concurrent window thread");
    expect_ok(nggc_collect(heap),
              "reclaim mutator-concurrent window heap");
    expect_ok(nggc_close(heap), "close mutator-concurrent window heap");
}
#else
static void run_tlab_fast_quiescence_case(void) {}
static void run_tlab_fast_late_enrollment_case(void) {}
static void run_mutator_concurrent_window_case(void) {}
#endif

static nggc_result auto_collect_young_with_fault(
    nggc_heap *heap, nggc_thread *thread, uint32_t fault) {
    if (fault != 0u) {
        expect_ok(nggc_internal_test_collect_fault_set(heap, fault),
                  "arm moving collector fault");
    }
    return nggc_thread_collect_young(heap, thread);
}

static void run_evacuation_fault_case(uint32_t fault,
                                      int after_forwarding) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    nggc_thread *thread = 0;
    evacuation_fault_node *parent;
    evacuation_fault_node *child;
    evacuation_fault_node *parent_source;
    evacuation_fault_node *child_source;
    evacuation_fault_node *young_child_source;
    uint16_t type_id = NGGC_TYPE_NONE;
    uint32_t generation = NGGC_REGION_GENERATION_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open moving fault heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(evacuation_fault_node);
    desc.scan = scan_evacuation_fault_node;
    desc.name = "evacuation-fault-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register moving fault node");
    expect_ok(nggc_thread_register(heap, &thread),
              "register moving fault mutator");

    parent = (evacuation_fault_node *)nggc_alloc(heap, type_id);
    expect_true(parent != 0, "allocate moving fault parent");
    memset(parent, 0, sizeof(*parent));
    parent->value = 101;
    expect_ok(nggc_root_push(heap, (void **)&parent),
              "root moving fault parent");
    expect_ok(auto_collect_young_with_fault(heap, thread, 0u),
              "age moving fault parent once");

    child = (evacuation_fault_node *)nggc_alloc(heap, type_id);
    expect_true(child != 0, "allocate moving fault child");
    memset(child, 0, sizeof(*child));
    child->value = 202;
    expect_ok(NGGC_STORE(heap, parent, &parent->edge, child),
              "store moving fault parent edge");
    parent_source = parent;
    child_source = child;

    expect_true(auto_collect_young_with_fault(heap, thread, fault) ==
                    NGGC_ERR_CORRUPTION,
                "moving collector fault returns original error");
    expect_ok(nggc_check(heap),
              "moving collector fault leaves valid heap");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read moving collector fault stats");
    expect_true(stats.gc_phase == NGGC_PHASE_IDLE &&
                    stats.read_barrier_active ==
                        (after_forwarding ? 1u : 0u) &&
                    stats.safepoint_active == 0u &&
                    stats.safepoint_pending_threads == 0u &&
                    stats.safepoint_parked_threads == 0u &&
                    stats.collection_set_region_count == 0u &&
                    stats.from_space_quarantine_active ==
                        (after_forwarding ? 1u : 0u) &&
                    stats.from_space_quarantine_object_count ==
                        (after_forwarding ? 2u : 0u) &&
                    stats.from_space_quarantine_release_epoch ==
                        (after_forwarding ? stats.safepoint_epoch : 0u) &&
                    (after_forwarding
                         ? stats.from_space_quarantine_bytes != 0u
                         : stats.from_space_quarantine_bytes == 0u) &&
                    stats.young_evacuation_emergency_failure_count == 0u &&
                    stats.evacuation_epoch == 2u &&
                    stats.forwarding_cas_success_count ==
                        (after_forwarding ? 3u : 1u) &&
                    stats.forwarding_cas_failure_count == 0u,
                "moving fault restores collector and safepoint state");

    if (after_forwarding) {
        expect_true(parent != parent_source && parent->edge != child_source &&
                        parent->value == 101 && parent->edge->value == 202 &&
                        nggc_object_type(parent_source) == NGGC_TYPE_NONE &&
                        nggc_object_type(child_source) == NGGC_TYPE_NONE &&
                        stats.collection_count == 2u &&
                        stats.young_collection_count == 2u &&
                        stats.young_evacuation_emergency_completion_count ==
                            1u,
                    "post-forwarding fault completes moving collection");
    } else {
        expect_true(parent == parent_source && parent->edge == child_source &&
                        nggc_object_type(parent_source) == type_id &&
                        nggc_object_type(child_source) == type_id &&
                        stats.collection_count == 1u &&
                        stats.young_collection_count == 1u &&
                        stats.young_evacuation_emergency_completion_count ==
                            0u,
                    "pre-forwarding fault aborts without aging objects");
        expect_ok(auto_collect_young_with_fault(heap, thread, 0u),
                  "retry pre-forwarding moving collection");
        expect_true(parent != parent_source && parent->edge != child_source &&
                        nggc_object_type(parent_source) == NGGC_TYPE_NONE &&
                        nggc_object_type(child_source) == NGGC_TYPE_NONE,
                    "retry remaps pre-forwarding fault graph");
    }

    expect_ok(nggc_internal_test_object_generation(
                  heap, parent, &generation),
              "query recovered parent generation");
    expect_true(generation == NGGC_REGION_GENERATION_OLD,
                "recovered parent promotes at threshold");
    expect_ok(nggc_internal_test_object_generation(
                  heap, parent->edge, &generation),
              "query recovered child generation");
    expect_true(generation == NGGC_REGION_GENERATION_YOUNG,
                "recovered younger child stays young");

    young_child_source = parent->edge;
    expect_ok(auto_collect_young_with_fault(heap, thread, 0u),
              "collect after moving emergency completion");
    expect_true(parent->edge != young_child_source &&
                    parent->edge->value == 202 &&
                    nggc_object_type(young_child_source) == NGGC_TYPE_NONE,
                "recovered remset preserves next young collection");
    expect_ok(nggc_internal_test_object_generation(
                  heap, parent->edge, &generation),
              "query post-recovery child generation");
    expect_true(generation == NGGC_REGION_GENERATION_OLD,
                "post-recovery child later promotes normally");
    expect_ok(nggc_check(heap),
              "post-recovery young collection verifies");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read post-recovery moving stats");
    expect_true(stats.collection_count == 3u &&
                    stats.young_collection_count == 3u &&
                    stats.young_object_count == 0u &&
                    stats.old_object_count == 2u &&
                    stats.young_evacuation_emergency_completion_count ==
                        (after_forwarding ? 1u : 0u) &&
                    stats.young_evacuation_emergency_failure_count == 0u &&
                    stats.evacuation_epoch ==
                        (after_forwarding ? 3u : 4u) &&
                    stats.forwarding_cas_success_count ==
                        stats.young_evacuated_object_count &&
                    stats.forwarding_cas_success_count == 4u &&
                    stats.forwarding_cas_failure_count == 0u,
                "moving recovery statistics remain consistent");

    expect_ok(nggc_root_pop(heap, (void **)&parent),
              "unroot moving fault parent");
    expect_ok(nggc_thread_unregister(heap, thread),
              "unregister moving fault mutator");
    expect_ok(nggc_collect(heap), "collect moving fault heap");
    expect_ok(nggc_close(heap), "close moving fault heap");
}

static void run_forwarding_cas_protocol_case(void) {
    nggc_config config;
    nggc_stats stats;
    nggc_heap *heap = 0;
    void *source;
    void *winner;
    void *loser;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)256u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open forwarding CAS heap");
    source = nggc_internal_alloc_none(heap, 16u);
    winner = nggc_internal_alloc_none(heap, 16u);
    loser = nggc_internal_alloc_none(heap, 16u);
    expect_true(source && winner && loser,
                "allocate forwarding CAS objects");
    expect_ok(nggc_internal_test_forwarding_cas_check(
                  heap, source, winner, loser),
              "forwarding CAS winner and loser protocol");
    expect_ok(nggc_check(heap), "forwarding CAS cleanup verifies");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read forwarding CAS stats");
    expect_true(stats.evacuation_epoch == 0u &&
                    stats.forwarding_cas_success_count == 1u &&
                    stats.forwarding_cas_failure_count == 2u,
                "forwarding CAS statistics distinguish outcomes");

    expect_ok(nggc_collect(heap), "collect forwarding CAS heap");
    expect_ok(nggc_close(heap), "close forwarding CAS heap");
}

static void run_fail_stop_management_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_thread *thread = 0;
    evacuation_fault_node *root;
    uint16_t type_id = NGGC_TYPE_NONE;
    uint32_t state = NGGC_HEAP_STATE_RUNNING;
    nggc_result reason = NGGC_OK;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open fail-stop heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(evacuation_fault_node);
    desc.scan = scan_evacuation_fault_node;
    desc.name = "fail-stop-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register fail-stop type");
    expect_ok(nggc_thread_register(heap, &thread),
              "register fail-stop thread");
    root = (evacuation_fault_node *)nggc_alloc(heap, type_id);
    expect_true(root != 0, "allocate fail-stop root");
    memset(root, 0, sizeof(*root));
    expect_ok(nggc_root_push(heap, (void **)&root),
              "root fail-stop object");
    expect_ok(nggc_internal_test_force_emergency_failure(heap),
              "arm fail-stop emergency failure");
    expect_true(nggc_thread_collect_young(heap, thread) ==
                    NGGC_ERR_CORRUPTION,
                "emergency failure enters fail-stop");

    expect_ok(nggc_get_heap_state(heap, &state, &reason),
              "query fail-stop state");
    expect_true(state == NGGC_HEAP_STATE_FAIL_STOP &&
                    reason == NGGC_ERR_CORRUPTION,
                "fail-stop state preserves reason");
    expect_true(nggc_alloc(heap, type_id) == 0,
                "fail-stop rejects allocation");
    expect_true(nggc_close(heap) == NGGC_ERR_BUSY,
                "normal close remains gated in fail-stop");
    expect_ok(nggc_force_destroy(heap),
              "force destroy fail-stop heap");
}

static void run_evacuation_worker_action_case(
    uint32_t action, size_t object_count,
    uint64_t expected_task_count, uint64_t expected_cas_failures,
    uint64_t expected_loser_reclaims, int expect_fallback) {
    enum { MAX_OBJECTS = 4 };
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    evacuation_fault_node *objects[MAX_OBJECTS];
    evacuation_fault_node *sources[MAX_OBJECTS];
    uint16_t type_id = NGGC_TYPE_NONE;
    uint32_t generation = NGGC_REGION_GENERATION_NONE;

    expect_true(object_count != 0u && object_count <= MAX_OBJECTS,
                "valid evacuation worker action object count");
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.young_tenuring_threshold = 2u;
    expect_ok(nggc_open(&config, &heap),
              "open evacuation worker action heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(evacuation_fault_node);
    desc.scan = scan_evacuation_fault_node;
    desc.name = "evacuation-worker-action-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register evacuation worker action node");
    memset(objects, 0, sizeof(objects));
    memset(sources, 0, sizeof(sources));
    for (size_t i = 0u; i < object_count; ++i) {
        objects[i] = (evacuation_fault_node *)nggc_alloc(heap, type_id);
        expect_true(objects[i] != 0,
                    "allocate evacuation worker action object");
        memset(objects[i], 0, sizeof(*objects[i]));
        objects[i]->value = (int)i + 301;
        sources[i] = objects[i];
        expect_ok(nggc_root_push(heap, (void **)&objects[i]),
                  "root evacuation worker action object");
    }

    expect_ok(nggc_internal_test_collect_fault_set(heap, action),
              "arm evacuation worker action");
    expect_ok(nggc_collect_young(heap),
              "run evacuation worker action collection");
    expect_ok(nggc_check(heap),
              "verify evacuation worker action collection");
    for (size_t i = 0u; i < object_count; ++i) {
        expect_true(objects[i] != sources[i] &&
                        objects[i]->value == (int)i + 301 &&
                        nggc_object_type(sources[i]) == NGGC_TYPE_NONE &&
                        nggc_object_type(objects[i]) == type_id,
                    "worker action preserves and remaps object");
        expect_ok(nggc_internal_test_object_generation(
                      heap, objects[i], &generation),
                  "query first-survival worker action generation");
        expect_true(generation == NGGC_REGION_GENERATION_YOUNG,
                    "first survival remains young below threshold");
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read evacuation worker action stats");
    expect_true(stats.collection_count == 1u &&
                    stats.young_collection_count == 1u &&
                    stats.young_object_count == object_count &&
                    stats.old_object_count == 0u &&
                    stats.young_evacuated_object_count == object_count &&
                    stats.young_promoted_object_count == 0u &&
                    stats.evacuation_worker_task_count ==
                        expected_task_count &&
                    stats.forwarding_cas_success_count == object_count &&
                    stats.forwarding_cas_failure_count ==
                        expected_cas_failures &&
                    stats.evacuation_loser_reclaimed_count ==
                        expected_loser_reclaims &&
                    stats.evacuation_workers_active == 0u &&
                    stats.evacuation_workers_quiescent == 0u &&
                    stats.evacuation_worker_epoch == 0u &&
                    stats.from_space_reclaim_pending == 0u &&
                    (!expect_fallback ||
                     stats.evacuation_worker_fallback_count != 0u),
                "evacuation worker action statistics");
#if NGGC_HAVE_C11_THREADS
    expect_true(stats.evacuation_worker_pool_size != 0u &&
                    stats.evacuation_worker_pool_size <= 3u &&
                    stats.evacuation_worker_created_count ==
                        stats.evacuation_worker_pool_size &&
                    stats.remap_parallel_cycle_count == 1u &&
                    stats.remap_worker_dispatch_count != 0u &&
                    stats.remap_worker_block_count != 0u &&
                    stats.remap_scanned_slot_count >= object_count &&
                    stats.remap_rewritten_slot_count >= object_count &&
                    stats.remap_worker_epoch == 0u &&
                    stats.remap_workers_active == 0u &&
                    stats.remap_workers_quiescent == 0u,
                "moving remap uses and quiesces shared worker pool");
    if (action == NGGC_TEST_COLLECT_ACTION_DUPLICATE_EVACUATION_TASK) {
        expect_true(stats.evacuation_parallel_cycle_count == 1u &&
                        stats.evacuation_worker_create_failure_count == 0u &&
                        stats.evacuation_worker_dispatch_count == 1u,
                    "duplicate task action uses persistent worker pool");
    } else if (action ==
               NGGC_TEST_COLLECT_ACTION_WORKER_TASK_RETRY) {
        expect_true(stats.evacuation_parallel_cycle_count == 0u &&
                        stats.evacuation_worker_create_failure_count == 0u &&
                        stats.evacuation_worker_dispatch_count == 0u,
                    "single task retry stays on coordinator");
    } else {
        expect_true(action ==
                            NGGC_TEST_COLLECT_ACTION_WORKER_CREATE_FALLBACK &&
                        stats.evacuation_parallel_cycle_count == 0u &&
                        stats.evacuation_worker_create_failure_count == 1u &&
                        stats.evacuation_worker_dispatch_count == 0u,
                    "injected worker create failure falls back cleanly");
    }
#else
    expect_true(stats.evacuation_parallel_cycle_count == 0u &&
                    stats.evacuation_worker_pool_size == 0u &&
                    stats.evacuation_worker_created_count == 0u &&
                    stats.evacuation_worker_create_failure_count == 0u &&
                    stats.evacuation_worker_dispatch_count == 0u &&
                    stats.remap_parallel_cycle_count == 0u &&
                    stats.remap_worker_dispatch_count == 0u &&
                    stats.remap_worker_fallback_count != 0u &&
                    stats.remap_worker_block_count != 0u &&
                    stats.remap_scanned_slot_count >= object_count &&
                    stats.remap_rewritten_slot_count >= object_count,
                "threadless worker actions stay synchronous");
#endif

    for (size_t i = 0u; i < object_count; ++i) {
        sources[i] = objects[i];
    }
    expect_ok(nggc_collect_young(heap),
              "promote worker action survivors at threshold");
    expect_ok(nggc_check(heap),
              "verify worker action threshold promotion");
    for (size_t i = 0u; i < object_count; ++i) {
        expect_true(objects[i] != sources[i] &&
                        objects[i]->value == (int)i + 301 &&
                        nggc_object_type(sources[i]) == NGGC_TYPE_NONE,
                    "threshold collection remaps worker action survivor");
        expect_ok(nggc_internal_test_object_generation(
                      heap, objects[i], &generation),
                  "query threshold worker action generation");
        expect_true(generation == NGGC_REGION_GENERATION_OLD,
                    "worker action object becomes old only at threshold");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read worker action promotion stats");
    expect_true(stats.young_object_count == 0u &&
                    stats.old_object_count == object_count &&
                    stats.young_promoted_object_count == object_count &&
                    stats.remap_worker_epoch == 0u &&
                    stats.remap_workers_active == 0u &&
                    stats.remap_workers_quiescent == 0u,
                "worker action survivors promote at configured age");
#if NGGC_HAVE_C11_THREADS
    expect_true(stats.evacuation_worker_pool_size != 0u &&
                    stats.evacuation_worker_pool_size <= 3u &&
                    stats.evacuation_worker_created_count ==
                        stats.evacuation_worker_pool_size &&
                    stats.remap_parallel_cycle_count == 2u &&
                    stats.remap_worker_dispatch_count != 0u,
                "second moving remap reuses shared worker pool");
    if (action == NGGC_TEST_COLLECT_ACTION_WORKER_CREATE_FALLBACK) {
        expect_true(stats.evacuation_worker_create_failure_count == 1u &&
                        stats.evacuation_worker_pool_size == 3u &&
                        stats.evacuation_worker_created_count == 3u &&
                        stats.evacuation_parallel_cycle_count == 1u &&
                        stats.evacuation_worker_dispatch_count == 3u,
                    "worker pool recovers after transient create failure");
    }
#endif

    for (size_t i = object_count; i != 0u; --i) {
        expect_ok(nggc_root_pop(heap, (void **)&objects[i - 1u]),
                  "unroot evacuation worker action object");
    }
    expect_ok(nggc_collect(heap),
              "collect evacuation worker action heap");
    expect_ok(nggc_close(heap),
              "close evacuation worker action heap");
}

int main(void) {
    static const uint32_t phases[] = {
        NGGC_PHASE_ROOT_SNAPSHOT,
        NGGC_PHASE_MARK,
        NGGC_PHASE_REFERENCE_PROCESS,
        NGGC_PHASE_SWEEP,
        NGGC_PHASE_EVACUATE,
        NGGC_PHASE_REMAP
    };
    static const uint32_t faults[] = {
        NGGC_TEST_PHASE_FAULT_INVALID_PHASE,
        NGGC_TEST_PHASE_FAULT_READ_BARRIER,
        NGGC_TEST_PHASE_FAULT_TRANSITION_PARITY,
        NGGC_TEST_PHASE_FAULT_CYCLE_FLAG,
        NGGC_TEST_PHASE_FAULT_INACTIVE_EXPECTED,
        NGGC_TEST_PHASE_FAULT_CYCLE_MISMATCH,
        NGGC_TEST_PHASE_FAULT_ACTIVE_PENDING,
        NGGC_TEST_PHASE_FAULT_ACTIVE_BARRIER,
        NGGC_TEST_PHASE_FAULT_CYCLE_EPOCH,
        NGGC_TEST_PHASE_FAULT_COLLECT_EPOCH,
        NGGC_TEST_PHASE_FAULT_SAFEPOINT_ACTIVE,
        NGGC_TEST_PHASE_FAULT_PARKED_COUNT,
        NGGC_TEST_PHASE_FAULT_COLLECT_OWNER,
        NGGC_TEST_PHASE_FAULT_EVACUATION_EPOCH,
        NGGC_TEST_PHASE_FAULT_EVACUATION_WORKER_ACTIVE,
        NGGC_TEST_PHASE_FAULT_FROM_SPACE_RECLAIM_GATE,
        NGGC_TEST_PHASE_FAULT_REMAP_WORKER_ACTIVE,
        NGGC_TEST_PHASE_FAULT_REMAP_RECLAIM_GATE,
        NGGC_TEST_PHASE_FAULT_QUARANTINE_BARRIER,
        NGGC_TEST_PHASE_FAULT_QUARANTINE_EPOCH,
        NGGC_TEST_PHASE_FAULT_QUARANTINE_RECLAIM_PROOF,
        NGGC_TEST_PHASE_FAULT_MUTATOR_CONCURRENT
    };
    static const uint32_t collect_faults[] = {
        NGGC_TEST_COLLECT_FAULT_AFTER_CYCLE_BEGIN,
        NGGC_TEST_COLLECT_FAULT_ROOT_OOM,
        NGGC_TEST_COLLECT_FAULT_REFERENCE_PROCESS,
        NGGC_TEST_COLLECT_FAULT_SWEEP
    };
    nggc_config config;
    nggc_heap *heap = 0;
    nggc_heap *auto_heap = 0;
    nggc_thread *auto_thread = 0;
    nggc_thread *tlab_thread = 0;
    nggc_stats stats;
    nggc_type_desc tlab_type;
    uint16_t tlab_type_id = 0u;
    uint16_t pinned_type_id = 0u;
    void *standard_young_object = 0;
    uint32_t standard_generation = NGGC_REGION_GENERATION_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.tlab_size = (size_t)8u * 1024u;
    {
        nggc_config bad_config = config;
        nggc_heap *bad_heap = 0;
        bad_config.tlab_size = 1000u;
        expect_true(nggc_open(&bad_config, &bad_heap) ==
                        NGGC_ERR_INVALID_ARGUMENT && bad_heap == 0,
                    "unaligned TLAB size rejected");
        bad_config = config;
        bad_config.young_tenuring_threshold = 256u;
        expect_true(nggc_open(&bad_config, &bad_heap) ==
                        NGGC_ERR_INVALID_ARGUMENT && bad_heap == 0,
                    "oversized young tenuring threshold rejected");
    }
    {
        nggc_heap *placement_heap = 0;
        nggc_type_desc placement_type;
        uint16_t placement_type_id = 0u;
        void *placement_object;
        expect_ok(nggc_open(&config, &placement_heap),
                  "Region placement fault heap open");
        memset(&placement_type, 0, sizeof(placement_type));
        placement_type.size = sizeof(placement_type);
        placement_type.flags = NGGC_TYPE_FLAG_PINNABLE |
                               NGGC_TYPE_FLAG_NON_MOVABLE;
        placement_type.object_size = (size_t)64u * 1024u;
        placement_type.name = "Region placement fault object";
        expect_ok(nggc_register_type(placement_heap, &placement_type,
                                     &placement_type_id),
                  "register Region placement fault type");
        placement_object = nggc_alloc(placement_heap, placement_type_id);
        expect_true(placement_object != 0,
                    "allocate Region placement fault object");
        expect_true(nggc_internal_test_region_fault_check(
                        placement_heap,
                        NGGC_TEST_REGION_FAULT_PLACEMENT_MISMATCH) ==
                        NGGC_ERR_CORRUPTION,
                    "Region placement mismatch detected");
        expect_ok(nggc_check(placement_heap),
                  "Region placement fault heap restored");
        expect_true(nggc_internal_test_region_fault_check(
                        placement_heap,
                        NGGC_TEST_REGION_FAULT_HUMONGOUS_ROLE_INVALID) ==
                        NGGC_ERR_CORRUPTION,
                    "humongous Region role corruption detected");
        expect_ok(nggc_check(placement_heap),
                  "humongous Region role restored");
        expect_true(nggc_internal_test_region_fault_check(
                        placement_heap,
                        NGGC_TEST_REGION_FAULT_HUMONGOUS_OFFSET) ==
                        NGGC_ERR_CORRUPTION,
                    "humongous continuation offset corruption detected");
        expect_ok(nggc_check(placement_heap),
                  "humongous continuation offset restored");
        expect_ok(nggc_close(placement_heap),
                  "Region placement fault heap close");
    }
    expect_ok(nggc_open(&config, &heap), "phase verifier heap open");
    memset(&tlab_type, 0, sizeof(tlab_type));
    tlab_type.size = sizeof(tlab_type);
    tlab_type.object_size = 64u;
    tlab_type.name = "tlab-test-object";
    expect_ok(nggc_register_type(heap, &tlab_type, &tlab_type_id),
              "register TLAB object type");
    tlab_type.flags = NGGC_TYPE_FLAG_PINNABLE;
    tlab_type.name = "tlab-pinned-test-object";
    expect_ok(nggc_register_type(heap, &tlab_type, &pinned_type_id),
              "register pinned fallback type");
    expect_ok(nggc_internal_test_phase_fault_check(
                  heap, NGGC_TEST_PHASE_CHECK),
              "initial phase state valid");
    {
        static const uint32_t region_faults[] = {
            NGGC_TEST_REGION_FAULT_CURSOR,
            NGGC_TEST_REGION_FAULT_OWNER,
            NGGC_TEST_REGION_FAULT_LEASE_PENDING,
            NGGC_TEST_REGION_FAULT_OWNER_DUPLICATE,
            NGGC_TEST_REGION_FAULT_GENERATION_INVALID,
            NGGC_TEST_REGION_FAULT_PLACEMENT_INVALID,
            NGGC_TEST_REGION_FAULT_PLACEMENT_NON_SPECIAL,
            NGGC_TEST_REGION_FAULT_SMART_PRESSURE_SOURCE,
            NGGC_TEST_REGION_FAULT_SMART_SLOW_VM
        };
        for (size_t i = 0;
             i < sizeof(region_faults) / sizeof(region_faults[0]); ++i) {
            expect_true(nggc_internal_test_region_fault_check(
                            heap, region_faults[i]) == NGGC_ERR_CORRUPTION,
                        "region allocation corruption detected");
            expect_ok(nggc_check(heap),
                      "region allocation state restored after fault");
        }
    }
    expect_ok(nggc_thread_register(heap, &tlab_thread),
              "register TLAB verifier thread");
    expect_true(nggc_thread_alloc_size(
                    heap, tlab_thread, NGGC_TYPE_NONE, 64u) == 0,
                "thread allocation rejects type none");
    expect_ok(nggc_internal_test_tlab_reserved_span_check(heap, tlab_thread),
              "valid active TLAB reserved span accepted");
    expect_ok(nggc_check(heap),
              "TLAB reserved span test restored heap state");
    expect_ok(nggc_internal_tlab_reserve(heap, tlab_thread, 1024u),
              "reserve real TLAB span");
    expect_true(nggc_internal_tlab_alloc_size(
                    heap, tlab_thread, tlab_type_id, 64u) != 0,
                "publish first object from TLAB");
    expect_true(nggc_internal_tlab_alloc_size(
                    heap, tlab_thread, tlab_type_id, 64u) != 0,
                "publish second object from TLAB");
    expect_ok(nggc_check(heap), "reserved real TLAB span verified");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "reserved TLAB stats");
    expect_true(stats.active_tlab_count == 1u &&
                    stats.young_region_count == 1u &&
                    stats.old_region_count == 0u &&
                    stats.tlab_reserved_block_count == 1u &&
                    stats.tlab_reserved_bytes == 768u &&
                    stats.live_object_count == 2u,
                "real TLAB reservation counted");
    expect_ok(nggc_internal_tlab_retire(heap, tlab_thread),
              "retire real TLAB span");
    expect_ok(nggc_check(heap), "retired TLAB span verified");
    standard_young_object = nggc_alloc(heap, tlab_type_id);
    expect_true(standard_young_object != 0,
                "standard allocation after young TLAB retire");
    expect_ok(nggc_internal_test_object_generation(
                  heap, standard_young_object, &standard_generation),
              "query standard allocation generation");
    expect_true(standard_generation == NGGC_REGION_GENERATION_YOUNG,
                "standard allocator preserves newborn generation");
    expect_ok(nggc_internal_tlab_reserve(heap, tlab_thread, 256u),
              "reserve exactly consumed TLAB span");
    expect_true(nggc_internal_tlab_alloc_size(
                    heap, tlab_thread, tlab_type_id, 64u) != 0 &&
                    nggc_internal_tlab_alloc_size(
                        heap, tlab_thread, tlab_type_id, 64u) != 0,
                "consume complete TLAB span");
    expect_ok(nggc_check(heap), "fully published TLAB span verified");
    expect_ok(nggc_internal_tlab_retire(heap, tlab_thread),
              "retire fully consumed TLAB span");
    expect_ok(nggc_internal_tlab_reserve(heap, tlab_thread, 1024u),
              "reserve TLAB before safepoint");
    expect_true(nggc_internal_tlab_alloc_size(
                    heap, tlab_thread, tlab_type_id, 64u) != 0,
                "publish TLAB object before safepoint");
    expect_ok(nggc_request_safepoint(heap),
              "request safepoint with active TLAB");
    expect_ok(nggc_safepoint(heap, tlab_thread),
              "safepoint retires active TLAB");
    expect_ok(nggc_check(heap), "safepoint TLAB retirement verified");
    expect_ok(nggc_safepoint_release(heap),
              "release safepoint after TLAB retirement");
    for (size_t i = 0; i < 140u; ++i) {
        expect_true(nggc_thread_alloc_size(
                        heap, tlab_thread, tlab_type_id, 64u) != 0,
                    "thread allocation refills TLAB");
    }
    expect_ok(nggc_check(heap), "automatic TLAB refill verified");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "automatic TLAB refill stats");
    expect_true(stats.active_tlab_count == 1u &&
                    stats.live_object_count == 146u &&
                    stats.tlab_size == (size_t)8u * 1024u &&
                    stats.tlab_sizing_policy ==
                        NGGC_TLAB_SIZING_POLICY_FIXED &&
                    stats.tlab_min_size == (size_t)8u * 1024u &&
                    stats.tlab_max_size == (size_t)8u * 1024u &&
                    stats.tlab_target_min_size ==
                        (size_t)8u * 1024u &&
                    stats.tlab_target_max_size ==
                        (size_t)8u * 1024u &&
                    stats.tlab_sizing_sample_count == 0u &&
                    stats.tlab_refill_count >= 2u &&
                    stats.tlab_retire_count >= 1u &&
                    stats.tlab_allocation_count >= 145u &&
                    stats.tlab_fast_allocation_count == 0u &&
                    stats.tlab_fast_flushed_object_count == 0u &&
                    stats.tlab_refill_bytes != 0u &&
                    stats.tlab_retired_unused_bytes != 0u,
                "automatic refill published all objects");
    expect_ok(nggc_request_safepoint(heap),
              "request safepoint after automatic refill");
    expect_ok(nggc_safepoint(heap, tlab_thread),
              "retire automatically refilled TLAB");
    expect_ok(nggc_safepoint_release(heap),
              "release safepoint before refill");
    expect_true(nggc_thread_alloc(
                    heap, tlab_thread, tlab_type_id) != 0,
                "thread allocation refills after safepoint");
    expect_true(nggc_thread_alloc_size(
                    heap, tlab_thread, tlab_type_id,
                    (size_t)64u * 1024u + 1u) != 0,
                "thread allocation routes large object to slow path");
    expect_true(nggc_thread_alloc(
                    heap, tlab_thread, pinned_type_id) != 0,
                "thread allocation routes pinned object to slow path");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "TLAB fallback stats");
    expect_true(stats.tlab_fallback_allocation_count >= 2u,
                "TLAB fallback allocations counted");
    expect_true(stats.young_region_count >= 1u &&
                    stats.old_region_count == 0u &&
                    stats.special_region_count >= 2u,
                "TLAB and special fallback regions stay distinct");
    expect_ok(nggc_check(heap), "post-safepoint refill verified");
    {
        static const uint32_t tlab_faults[] = {
            NGGC_TEST_TLAB_FAULT_REGION_ORPHAN,
            NGGC_TEST_TLAB_FAULT_THREAD_ORPHAN,
            NGGC_TEST_TLAB_FAULT_TOP_BOUNDS,
            NGGC_TEST_TLAB_FAULT_EPOCH_MISMATCH,
            NGGC_TEST_TLAB_FAULT_ACK_NOT_RETIRED,
            NGGC_TEST_TLAB_FAULT_RESERVED_IN_FREE_LIST,
            NGGC_TEST_TLAB_FAULT_INTERVAL_OVERLAP,
            NGGC_TEST_TLAB_FAULT_FAST_TOP_BOUNDS,
            NGGC_TEST_TLAB_FAULT_FAST_INFLIGHT,
            NGGC_TEST_TLAB_FAULT_OBJECT_ID_LEASE,
            NGGC_TEST_TLAB_FAULT_FAST_GATE,
            NGGC_TEST_TLAB_FAULT_ADAPTIVE_TARGET
        };
        for (size_t i = 0;
             i < sizeof(tlab_faults) / sizeof(tlab_faults[0]); ++i) {
            expect_true(nggc_internal_test_tlab_fault_check(
                            heap, tlab_thread, tlab_faults[i]) ==
                            NGGC_ERR_CORRUPTION,
                        "TLAB shadow corruption detected");
            expect_ok(nggc_check(heap),
                      "TLAB shadow state restored after fault");
        }
    }
    expect_ok(nggc_internal_test_collect_fault_set(
                  heap, NGGC_TEST_COLLECT_FAULT_SWEEP),
              "arm young sweep fault");
    expect_true(nggc_thread_collect_young(heap, tlab_thread) ==
                    NGGC_ERR_CORRUPTION,
                "young sweep fault aborts collection");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "young sweep fault stats");
    expect_true(stats.collection_set_region_count == 0u &&
                    stats.safepoint_active == 0u &&
                    stats.young_collection_count == 0u,
                "young sweep fault clears collection set");
    expect_ok(nggc_check(heap), "young sweep fault heap restored");
    expect_ok(nggc_thread_unregister(heap, tlab_thread),
              "unregister retires TLAB verifier thread");
    expect_ok(nggc_check(heap), "unregister TLAB retirement verified");

    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        expect_true(nggc_internal_test_phase_fault_check(heap, faults[i]) ==
                        NGGC_ERR_CORRUPTION,
                    "phase corruption detected");
        expect_ok(nggc_internal_test_phase_fault_check(
                      heap, NGGC_TEST_PHASE_CHECK),
                  "phase state restored after fault");
    }
    expect_true(nggc_internal_test_phase_fault_check(
                    heap, NGGC_TEST_PHASE_ACTION_OUT_OF_ORDER) ==
                    NGGC_ERR_CORRUPTION,
                "collector out-of-order phase rejected");
    expect_ok(nggc_internal_test_phase_fault_check(
                  heap, NGGC_TEST_PHASE_ACTION_ABORT_CYCLE),
              "collector cycle abort returns to idle protocol");
    expect_ok(nggc_internal_test_phase_fault_check(
                  heap, NGGC_TEST_PHASE_ACTION_MOVING_CYCLE),
              "moving collector cycle follows evacuation protocol");

    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        expect_ok(nggc_phase_begin(heap, phases[i]),
                  "standalone phase begin");
        expect_ok(nggc_check(heap), "active standalone phase verified");
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(heap, &stats),
                  "standalone phase stats");
        expect_true(stats.gc_phase == phases[i],
                    "standalone phase published");
        expect_true(stats.read_barrier_active ==
                        ((phases[i] == NGGC_PHASE_EVACUATE ||
                          phases[i] == NGGC_PHASE_REMAP)
                             ? 1u
                             : 0u),
                    "standalone phase barrier matches matrix");
        expect_ok(nggc_phase_end(heap, phases[i]),
                  "standalone phase end");
    }

    expect_ok(nggc_phase_begin(heap, NGGC_PHASE_MARK),
              "begin phase for safepoint exclusion");
    expect_true(nggc_request_safepoint(heap) == NGGC_ERR_BUSY,
                "safepoint request rejected during active phase");
    expect_ok(nggc_phase_end(heap, NGGC_PHASE_MARK),
              "end phase for safepoint exclusion");

    expect_ok(nggc_collect(heap), "collector phase cycle");
    expect_ok(nggc_internal_test_phase_fault_check(
                  heap, NGGC_TEST_PHASE_CHECK),
              "completed collector cycle verified");
    expect_ok(nggc_check(heap), "phase verifier heap check");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "phase verifier stats");
    expect_true(stats.gc_phase == NGGC_PHASE_IDLE &&
                    stats.read_barrier_active == 0u,
                "collector returned to idle barrier state");
    expect_true(stats.phase_transition_count == 34u,
                "standalone and collector transitions counted");
    expect_true(stats.collection_count == 1u,
                "collector cycle completed once");

    expect_ok(nggc_close(heap), "phase verifier heap close");

    config.flags = NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT;
    expect_ok(nggc_open(&config, &auto_heap),
              "phase verifier auto heap open");
    expect_ok(nggc_thread_register(auto_heap, &auto_thread),
              "phase verifier auto thread register");
    expect_ok(nggc_phase_begin(auto_heap, NGGC_PHASE_MARK),
              "begin auto standalone phase");
    expect_true(nggc_collect(auto_heap) == NGGC_ERR_BUSY,
                "auto collect rejected during standalone phase");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(auto_heap, &stats),
              "auto standalone phase stats");
    expect_true(stats.safepoint_request_count == 0u &&
                    stats.safepoint_pending_threads == 0u,
                "rejected collect did not start safepoint handshake");
    expect_ok(nggc_phase_end(auto_heap, NGGC_PHASE_MARK),
              "end auto standalone phase");
    for (size_t i = 0;
         i < sizeof(collect_faults) / sizeof(collect_faults[0]); ++i) {
        nggc_result expected =
            collect_faults[i] == NGGC_TEST_COLLECT_FAULT_ROOT_OOM
                ? NGGC_ERR_OUT_OF_MEMORY
                : NGGC_ERR_CORRUPTION;
        expect_true(nggc_collect(auto_heap) == NGGC_ERR_BUSY,
                    "faulted auto collect requests safepoint");
        expect_ok(nggc_safepoint(auto_heap, auto_thread),
                  "faulted auto collector thread parks");
        expect_ok(nggc_internal_test_collect_fault_set(
                      auto_heap, collect_faults[i]),
                  "arm collector fault");
        expect_true(nggc_collect(auto_heap) == expected,
                    "collector fault returned expected error");
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(auto_heap, &stats),
                  "collector fault release stats");
        expect_true(stats.gc_phase == NGGC_PHASE_IDLE &&
                        stats.safepoint_active == 0u &&
                        stats.safepoint_pending_threads == 0u &&
                        stats.safepoint_parked_threads == 0u &&
                        stats.safepoint_release_count == i + 1u,
                    "collector fault abort released mutator");
        expect_ok(nggc_internal_test_phase_fault_check(
                      auto_heap, NGGC_TEST_PHASE_CHECK),
                  "collector fault restored phase state");
    }
    expect_true(nggc_collect(auto_heap) == NGGC_ERR_BUSY,
                "post-fault auto collect requests safepoint");
    expect_ok(nggc_safepoint(auto_heap, auto_thread),
              "post-fault auto thread parks");
    expect_ok(nggc_collect(auto_heap),
              "post-fault auto collection succeeds");
    expect_ok(nggc_thread_unregister(auto_heap, auto_thread),
              "phase verifier auto thread unregister");
    expect_ok(nggc_close(auto_heap), "phase verifier auto heap close");

    run_evacuation_fault_case(
        NGGC_TEST_COLLECT_FAULT_BEFORE_FORWARDING, 0);
    run_evacuation_fault_case(
        NGGC_TEST_COLLECT_FAULT_AFTER_FORWARDING, 1);
    run_evacuation_fault_case(
        NGGC_TEST_COLLECT_FAULT_AFTER_EVACUATE, 1);
    run_evacuation_fault_case(
        NGGC_TEST_COLLECT_FAULT_DURING_REMAP, 1);
    run_evacuation_fault_case(
        NGGC_TEST_COLLECT_FAULT_DURING_SOURCE_RELEASE, 1);
    run_fail_stop_management_case();
    run_forwarding_cas_protocol_case();
    run_large_free_bin_fault_case();
    run_decommitted_run_fault_case();
    run_evacuation_worker_action_case(
        NGGC_TEST_COLLECT_ACTION_DUPLICATE_EVACUATION_TASK,
        1u, 2u, 1u, 1u, 0);
    run_evacuation_worker_action_case(
        NGGC_TEST_COLLECT_ACTION_WORKER_TASK_RETRY,
        1u, 1u, 0u, 0u, 1);
    run_evacuation_worker_action_case(
        NGGC_TEST_COLLECT_ACTION_WORKER_CREATE_FALLBACK,
        4u, 4u, 0u, 0u, 1);
    run_tlab_fast_quiescence_case();
    run_tlab_fast_late_enrollment_case();
    run_adaptive_tlab_sizing_case();
    run_mutator_concurrent_window_case();
    return 0;
}
