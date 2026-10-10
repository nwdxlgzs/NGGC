#include "nggc/nggc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define THREAD_COUNT 4u
#define THREAD_ITERATIONS 64u

typedef struct thread_node {
    struct thread_node *left;
    struct thread_node *right;
    int value;
} thread_node;

typedef struct worker_args {
    nggc_heap *heap;
    nggc_thread *foreign_thread;
    uint16_t type_id;
    atomic_int *ready_count;
    atomic_int *start_flag;
    atomic_int *failure_count;
} worker_args;

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

static void scan_thread_node(void *object, nggc_visit_fn visit, void *ctx) {
    thread_node *node = (thread_node *)object;
    visit(ctx, (void **)&node->left);
    visit(ctx, (void **)&node->right);
}

static void open_tss_test_heap(nggc_heap **out_heap, uint16_t *out_type_id,
                               const char *message) {
    nggc_config config;
    nggc_type_desc desc;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)256u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = (size_t)24u * 1024u;
    expect_ok(nggc_open(&config, out_heap), message);

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(thread_node);
    desc.scan = scan_thread_node;
    desc.name = "tss-thread-node";
    expect_ok(nggc_register_type(*out_heap, &desc, out_type_id),
              "register TSS test type");
}

static void run_tss_ambiguity_case(void) {
    nggc_heap *heap = 0;
    nggc_thread *thread_a = 0;
    nggc_thread *thread_b = 0;
    nggc_stats stats;
    thread_node *owner;
    thread_node *value;
    uint16_t type_id = NGGC_TYPE_NONE;

    open_tss_test_heap(&heap, &type_id, "open TSS ambiguity heap");
    expect_ok(nggc_thread_register(heap, &thread_a),
              "register first same-thread handle");
    expect_ok(nggc_thread_register(heap, &thread_b),
              "register second same-thread handle");
    owner = (thread_node *)nggc_thread_alloc(heap, thread_a, type_id);
    value = (thread_node *)nggc_thread_alloc(heap, thread_a, type_id);
    expect_true(owner != 0 && value != 0,
                "allocate TSS ambiguity objects");
    memset(owner, 0, sizeof(*owner));
    memset(value, 0, sizeof(*value));
    expect_ok(NGGC_STORE(heap, owner, &owner->left, value),
              "ambiguous barrier uses direct publication");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read ambiguous barrier stats");
    expect_true(stats.remset_barrier_tss_hit_count == 0u &&
                    stats.remset_barrier_tss_fallback_scan_count == 0u &&
                    stats.remset_barrier_tss_ambiguous_count == 1u &&
                    stats.remset_direct_record_count == 1u &&
                    stats.remset_buffer_record_count == 0u,
                "same native thread handles remain explicitly ambiguous");

    expect_ok(nggc_thread_unregister(heap, thread_b),
              "remove second same-thread handle");
    thread_b = 0;
    expect_ok(NGGC_STORE(heap, owner, &owner->right, value),
              "single remaining handle restores TSS hit");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read restored unique barrier stats");
    expect_true(stats.remset_barrier_tss_hit_count == 1u &&
                    stats.remset_barrier_tss_ambiguous_count == 1u &&
                    stats.remset_buffer_record_count == 1u &&
                    stats.remset_buffer_deferred_publish_count == 0u &&
                    stats.remset_barrier_tss_update_failure_count == 0u,
                "unregister rebuilds a unique per-heap TSS context");
    expect_ok(nggc_check(heap), "check TSS ambiguity heap");
    expect_ok(nggc_thread_unregister(heap, thread_a),
              "unregister first same-thread handle");
    thread_a = 0;
    expect_ok(NGGC_STORE(heap, owner, &owner->left, value),
              "unregistered barrier uses no-owner direct path");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read no-owner barrier stats");
    expect_true(stats.remset_barrier_tss_fallback_scan_count == 1u &&
                    stats.remset_barrier_no_owner_direct_count == 1u &&
                    stats.remset_direct_record_count == 2u,
                "no registered owner is observable and remains correct");
    expect_ok(nggc_check(heap), "check no-owner direct heap");
    expect_ok(nggc_close(heap), "close TSS ambiguity heap");
}

static void run_tss_multi_heap_case(void) {
    nggc_heap *heap_a = 0;
    nggc_heap *heap_b = 0;
    nggc_thread *thread_a = 0;
    nggc_thread *thread_b = 0;
    nggc_stats stats_a;
    nggc_stats stats_b;
    thread_node *owner_a;
    thread_node *value_a;
    thread_node *owner_b;
    thread_node *value_b;
    uint16_t type_a = NGGC_TYPE_NONE;
    uint16_t type_b = NGGC_TYPE_NONE;

    open_tss_test_heap(&heap_a, &type_a, "open first TSS heap");
    open_tss_test_heap(&heap_b, &type_b, "open second TSS heap");
    expect_ok(nggc_thread_register(heap_a, &thread_a),
              "register first heap TSS handle");
    expect_ok(nggc_thread_register(heap_b, &thread_b),
              "register second heap TSS handle");
    owner_a = (thread_node *)nggc_thread_alloc(heap_a, thread_a, type_a);
    value_a = (thread_node *)nggc_thread_alloc(heap_a, thread_a, type_a);
    owner_b = (thread_node *)nggc_thread_alloc(heap_b, thread_b, type_b);
    value_b = (thread_node *)nggc_thread_alloc(heap_b, thread_b, type_b);
    expect_true(owner_a && value_a && owner_b && value_b,
                "allocate multi-heap TSS objects");
    memset(owner_a, 0, sizeof(*owner_a));
    memset(value_a, 0, sizeof(*value_a));
    memset(owner_b, 0, sizeof(*owner_b));
    memset(value_b, 0, sizeof(*value_b));
    expect_ok(NGGC_STORE(heap_a, owner_a, &owner_a->left, value_a),
              "first heap TSS barrier");
    expect_ok(NGGC_STORE(heap_b, owner_b, &owner_b->left, value_b),
              "second heap TSS barrier");

    memset(&stats_a, 0, sizeof(stats_a));
    stats_a.size = sizeof(stats_a);
    memset(&stats_b, 0, sizeof(stats_b));
    stats_b.size = sizeof(stats_b);
    expect_ok(nggc_get_stats(heap_a, &stats_a),
              "read first heap TSS stats");
    expect_ok(nggc_get_stats(heap_b, &stats_b),
              "read second heap TSS stats");
    expect_true(stats_a.remset_barrier_tss_hit_count == 1u &&
                    stats_b.remset_barrier_tss_hit_count == 1u &&
                    stats_a.remset_buffer_deferred_publish_count == 1u &&
                    stats_b.remset_buffer_deferred_publish_count == 1u &&
                    stats_a.remset_barrier_tss_fallback_scan_count == 0u &&
                    stats_b.remset_barrier_tss_fallback_scan_count == 0u,
                "one native thread keeps independent per-heap contexts");
    expect_ok(nggc_check(heap_a), "check first TSS heap");
    expect_ok(nggc_check(heap_b), "check second TSS heap");
    expect_ok(nggc_thread_unregister(heap_b, thread_b),
              "unregister second heap TSS handle");
    expect_ok(nggc_thread_unregister(heap_a, thread_a),
              "unregister first heap TSS handle");
    expect_ok(nggc_close(heap_b), "close second TSS heap");
    expect_ok(nggc_close(heap_a), "close first TSS heap");
}

static int worker_main(void *arg) {
    worker_args *args = (worker_args *)arg;
    nggc_thread *thread = 0;

    if (nggc_thread_register(args->heap, &thread) != NGGC_OK) {
        atomic_fetch_add_explicit(args->failure_count, 1,
                                  memory_order_relaxed);
        return 1;
    }

    atomic_fetch_add_explicit(args->ready_count, 1, memory_order_release);
    while (atomic_load_explicit(args->start_flag, memory_order_acquire) == 0) {
        thrd_yield();
    }

    if (nggc_safepoint_park(args->heap, thread) != NGGC_OK) {
        fprintf(stderr, "worker safepoint park failed\n");
        atomic_fetch_add_explicit(args->failure_count, 1,
                                  memory_order_relaxed);
    }

    if (nggc_thread_alloc(args->heap, args->foreign_thread,
                          args->type_id) != 0) {
        fprintf(stderr, "foreign thread handle allocation succeeded\n");
        atomic_fetch_add_explicit(args->failure_count, 1,
                                  memory_order_relaxed);
    }
    if (nggc_thread_unregister(args->heap, args->foreign_thread) !=
        NGGC_ERR_INVALID_ARGUMENT) {
        fprintf(stderr, "foreign thread handle unregister succeeded\n");
        atomic_fetch_add_explicit(args->failure_count, 1,
                                  memory_order_relaxed);
    }
    if (nggc_thread_collect(args->heap, args->foreign_thread) !=
        NGGC_ERR_INVALID_ARGUMENT) {
        fprintf(stderr, "foreign thread handle collection succeeded\n");
        atomic_fetch_add_explicit(args->failure_count, 1,
                                  memory_order_relaxed);
    }

    for (unsigned i = 0; i < THREAD_ITERATIONS; ++i) {
        thread_node *parent;
        thread_node *child;

        parent = (thread_node *)nggc_thread_alloc(
            args->heap, thread, args->type_id);
        child = (thread_node *)nggc_thread_alloc(
            args->heap, thread, args->type_id);
        if (!parent || !child) {
            fprintf(stderr, "worker allocation failed: %d\n",
                    (int)nggc_get_last_error(args->heap));
            atomic_fetch_add_explicit(args->failure_count, 1,
                                      memory_order_relaxed);
            break;
        }

        parent->value = (int)i;
        child->value = (int)(i + 1u);
        if (NGGC_STORE(args->heap, parent, &parent->left, child) != NGGC_OK ||
            NGGC_LOAD(args->heap, &parent->left) != child) {
            fprintf(stderr, "worker barrier failed: %d\n",
                    (int)nggc_get_last_error(args->heap));
            atomic_fetch_add_explicit(args->failure_count, 1,
                                      memory_order_relaxed);
            break;
        }
    }

    if (nggc_thread_unregister(args->heap, thread) != NGGC_OK) {
        atomic_fetch_add_explicit(args->failure_count, 1,
                                  memory_order_relaxed);
        return 1;
    }

    return 0;
}

int main(void) {
    nggc_heap *heap = 0;
    nggc_thread *collector_thread = 0;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    uint16_t type_id = NGGC_TYPE_NONE;
    thrd_t threads[THREAD_COUNT];
    worker_args args[THREAD_COUNT];
    atomic_int ready_count;
    atomic_int start_flag;
    atomic_int failure_count;
    size_t pending_threads = 0;
    uint64_t expected_allocations =
        (uint64_t)THREAD_COUNT * THREAD_ITERATIONS * 2u;

    run_tss_ambiguity_case();
    run_tss_multi_heap_case();

    atomic_init(&ready_count, 0);
    atomic_init(&start_flag, 0);
    atomic_init(&failure_count, 0);

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)2u * 1024u * 1024u;
    config.uhm_arena_size = (size_t)8u * 1024u * 1024u;
    config.region_size = (size_t)24u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open thread heap");
    expect_ok(nggc_thread_register(heap, &collector_thread),
              "register collector owner");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(thread_node);
    desc.scan = scan_thread_node;
    desc.name = "thread_node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register thread node type");
    expect_ok(nggc_type_table_freeze(heap),
              "freeze thread allocation type table");

    for (unsigned i = 0; i < THREAD_COUNT; ++i) {
        args[i].heap = heap;
        args[i].foreign_thread = collector_thread;
        args[i].type_id = type_id;
        args[i].ready_count = &ready_count;
        args[i].start_flag = &start_flag;
        args[i].failure_count = &failure_count;
        expect_true(thrd_create(&threads[i], worker_main, &args[i]) ==
                        thrd_success,
                    "thread create");
    }

    while (atomic_load_explicit(&ready_count, memory_order_acquire) !=
           (int)THREAD_COUNT) {
        thrd_yield();
    }

    expect_true(nggc_thread_collect(heap, collector_thread) ==
                    NGGC_ERR_BUSY,
                "collector owner requests threaded safepoint");
    atomic_store_explicit(&start_flag, 1, memory_order_release);

    for (;;) {
        nggc_result wait_result =
            nggc_safepoint_wait(heap, &pending_threads);
        if (wait_result == NGGC_OK) {
            break;
        }
        expect_true(wait_result == NGGC_ERR_BUSY, "thread safepoint wait");
        thrd_yield();
    }
    expect_true(pending_threads == 0u, "thread safepoint completed");
    expect_ok(nggc_thread_collect(heap, collector_thread),
              "collector owner completes threaded collection");

    for (unsigned i = 0; i < THREAD_COUNT; ++i) {
        int thread_result = 0;
        expect_true(thrd_join(threads[i], &thread_result) == thrd_success,
                    "thread join");
        expect_true(thread_result == 0, "thread result");
    }

    expect_true(atomic_load_explicit(&failure_count, memory_order_acquire) ==
                    0,
                "worker failures");

    expect_true(nggc_thread_alloc(heap, collector_thread, type_id) != 0 &&
                    nggc_thread_alloc(heap, collector_thread, type_id) != 0,
                "collector owner primes a fast TLAB");
    expected_allocations += 2u;
    expect_ok(nggc_request_safepoint(heap),
              "close fast allocation gate for safepoint");
    expect_true(nggc_thread_alloc(heap, collector_thread, type_id) == 0,
                "closed safepoint gate rejects further allocation");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read closed fast allocation gate stats");
    expect_true(stats.allocation_count == expected_allocations,
                "closed gate publishes no additional object");
    expect_ok(nggc_safepoint(heap, collector_thread),
              "collector owner acknowledges closed gate safepoint");
    expect_ok(nggc_safepoint_release(heap),
              "release closed gate safepoint");

    expect_ok(nggc_thread_unregister(heap, collector_thread),
              "unregister collector owner");
    collector_thread = 0;

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "thread stats");
    expect_true(stats.registered_thread_count == 0u,
                "thread stats no registered threads");
    expect_true(stats.safepoint_request_count == 2u,
                "thread stats safepoint request count");
    expect_true(stats.safepoint_ack_count == THREAD_COUNT + 2u,
                "thread stats safepoint ack count");
    expect_true(stats.safepoint_release_count == 2u &&
                    stats.safepoint_parked_threads == 0u &&
                    stats.safepoint_active == 0u &&
                    stats.safepoint_release_epoch == stats.safepoint_epoch,
                "thread stats safepoint release");
    expect_true(stats.allocation_count == expected_allocations,
                "thread stats allocation count");
    expect_true(stats.tlab_allocation_count != 0u &&
                    stats.tlab_allocation_count <= expected_allocations,
                 "thread stats TLAB allocation count");
    expect_true(stats.region_size == (size_t)24u * 1024u,
                "thread heap uses configurable non-power-of-two Region");
    expect_true(stats.tlab_fast_allocation_count != 0u &&
                    stats.tlab_fast_allocation_count ==
                        stats.tlab_fast_flushed_object_count &&
                    stats.tlab_fast_flush_count != 0u &&
                    stats.tlab_fast_slow_path_count >= THREAD_COUNT,
                "per-thread TLAB fast allocations flush exactly once");
    expect_true(stats.tlab_fast_wrong_owner_count == THREAD_COUNT,
                "foreign TLAB owners are rejected and counted");
    expect_true(stats.tlab_refill_count >= THREAD_COUNT,
                "thread stats TLAB refill count");
    expect_true(stats.tlab_retire_count >= THREAD_COUNT &&
                    stats.active_tlab_count == 0u,
                "thread stats TLABs retired on unregister");
    expect_true(stats.mutator_assist_charged_bytes != 0u,
                "thread stats assist charged bytes");
    expect_true(stats.mutator_assist_debt_bytes != 0u,
                "thread stats assist debt before collect");
    expect_true(stats.store_barrier_count == THREAD_COUNT * THREAD_ITERATIONS,
                "thread stats store barrier count");
    expect_true(stats.load_barrier_count == THREAD_COUNT * THREAD_ITERATIONS,
                "thread stats load barrier count");
    expect_true(stats.remset_buffer_capacity == 64u &&
                    stats.remset_buffer_record_count ==
                        THREAD_COUNT * THREAD_ITERATIONS &&
                    stats.remset_buffer_flush_count >= THREAD_COUNT &&
                    stats.remset_buffer_flushed_card_count <=
                        stats.remset_buffer_record_count &&
                    stats.remset_buffer_deferred_publish_count != 0u &&
                    stats.remset_buffer_deferred_publish_count <=
                        stats.remset_buffer_record_count &&
                    stats.remset_buffered_card_count == 0u &&
                    stats.remset_buffered_thread_count == 0u &&
                    stats.remset_buffer_inflight_card_count == 0u &&
                    stats.remset_buffer_inflight_thread_count == 0u,
                "per-thread remembered-set buffers flush on unregister");
    expect_true(stats.remset_barrier_tss_hit_count ==
                        THREAD_COUNT * THREAD_ITERATIONS &&
                    stats.remset_barrier_tss_fallback_scan_count == 0u &&
                    stats.remset_barrier_tss_ambiguous_count == 0u &&
                    stats.remset_barrier_tss_update_failure_count == 0u &&
                    stats.remset_barrier_no_owner_direct_count == 0u,
                "registered workers use O(1) per-heap barrier contexts");

    expect_ok(nggc_check(heap), "thread heap check after owner collect");
    expect_ok(nggc_collect(heap), "thread heap final collect");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "thread stats after collect");
    expect_true(stats.live_object_count == 0u,
                "thread heap reclaimed all objects");
    expect_true(stats.reclaimed_object_count == expected_allocations,
                "thread heap reclaimed allocation count");
    expect_true(stats.collection_count == 2u &&
                    stats.reference_process_count == 2u,
                "thread stats reference process count");
    expect_true(stats.mutator_assist_debt_bytes == 0u,
                "thread stats assist debt repaid after collect");
    expect_true(stats.mutator_assist_repaid_bytes ==
                    stats.mutator_assist_charged_bytes,
                "thread stats assist charged bytes repaid");
    expect_true(stats.mutator_assist_repay_count == 1u,
                "thread stats assist repay count");

    expect_ok(nggc_close(heap), "close thread heap");
    return 0;
}
