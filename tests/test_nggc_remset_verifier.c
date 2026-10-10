#include "nggc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NGGC_HAVE_C11_THREADS
#define NGGC_HAVE_C11_THREADS 0
#endif

#if NGGC_HAVE_C11_THREADS
#include <stdatomic.h>
#include <threads.h>
#endif

typedef struct remset_node {
    struct remset_node *edge;
    unsigned char padding[600];
} remset_node;

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void expect_ok(nggc_result result, const char *message) {
    if (result != NGGC_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static void scan_node(void *object, nggc_visit_fn visit, void *ctx) {
    remset_node *node = (remset_node *)object;
    visit(ctx, (void **)&node->edge);
}

#if NGGC_HAVE_C11_THREADS
typedef struct remset_publish_worker_args {
    nggc_heap *heap;
    uint16_t type_id;
    remset_node *shared_owner;
    remset_node *shared_value;
    atomic_int ready;
    atomic_int start;
    uint32_t prefill_same_card;
    uint32_t store_null;
    nggc_result store_result;
    nggc_result park_result;
    nggc_result unregister_result;
} remset_publish_worker_args;

static int remset_publish_worker_main(void *arg) {
    remset_publish_worker_args *args =
        (remset_publish_worker_args *)arg;
    nggc_thread *thread = 0;
    remset_node *owner;
    remset_node *value;
    if (nggc_thread_register(args->heap, &thread) != NGGC_OK) {
        return 1;
    }
    owner = args->shared_owner
                ? args->shared_owner
                : (remset_node *)nggc_thread_alloc(
                      args->heap, thread, args->type_id);
    value = args->shared_value
                ? args->shared_value
                : (remset_node *)nggc_thread_alloc(
                      args->heap, thread, args->type_id);
    if (!owner || !value) {
        (void)nggc_thread_unregister(args->heap, thread);
        return 2;
    }
    if (!args->shared_owner) {
        memset(owner, 0, sizeof(*owner));
    }
    if (!args->shared_value) {
        memset(value, 0, sizeof(*value));
    }
    if (args->prefill_same_card != 0u &&
        NGGC_STORE(args->heap, owner, &owner->edge, value) != NGGC_OK) {
        (void)nggc_thread_unregister(args->heap, thread);
        return 3;
    }
    atomic_store_explicit(&args->ready, 1, memory_order_release);
    while (atomic_load_explicit(&args->start,
                                memory_order_acquire) == 0) {
        thrd_yield();
    }
    args->store_result =
        NGGC_STORE(args->heap, owner, &owner->edge,
                   args->store_null != 0u ? 0 : value);
    args->park_result = args->store_result == NGGC_OK
                            ? nggc_safepoint_park(args->heap, thread)
                            : NGGC_ERR_BUSY;
    args->unregister_result =
        nggc_thread_unregister(args->heap, thread);
    return args->store_result == NGGC_OK &&
                   args->park_result == NGGC_OK &&
                   args->unregister_result == NGGC_OK
               ? 0
               : 3;
}

static void run_deferred_publication_window_case(
    uint32_t prefill_same_card, uint32_t store_null) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    remset_publish_worker_args args;
    thrd_t worker;
    uint16_t type_id = NGGC_TYPE_NONE;
    int worker_result = -1;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)256u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = (size_t)24u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open deferred RemSet publication heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(remset_node);
    desc.scan = scan_node;
    desc.name = "deferred-remset-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register deferred RemSet type");

    memset(&args, 0, sizeof(args));
    args.heap = heap;
    args.type_id = type_id;
    args.prefill_same_card = prefill_same_card;
    args.store_null = store_null;
    args.store_result = NGGC_ERR_BUSY;
    args.park_result = NGGC_ERR_BUSY;
    args.unregister_result = NGGC_ERR_BUSY;
    atomic_init(&args.ready, 0);
    atomic_init(&args.start, 0);
    if (thrd_create(&worker, remset_publish_worker_main, &args) !=
        thrd_success) {
        fail("create deferred RemSet publication worker");
    }
    while (atomic_load_explicit(&args.ready,
                                memory_order_acquire) == 0) {
        thrd_yield();
    }
    expect_ok(nggc_internal_test_remset_publish_pause_arm(heap),
              "arm deferred RemSet publication pause");
    atomic_store_explicit(&args.start, 1, memory_order_release);
    while (nggc_internal_test_remset_publish_pause_wait(heap) ==
           NGGC_ERR_BUSY) {
        thrd_yield();
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "observe reserved RemSet publication");
    if (stats.remset_buffered_card_count != 1u ||
        stats.remset_buffered_thread_count != 1u ||
        stats.remset_buffer_inflight_card_count != 1u ||
        stats.remset_buffer_inflight_thread_count != 1u ||
        stats.remset_buffer_record_count !=
            (prefill_same_card != 0u ? 2u : 1u) ||
        stats.remset_buffer_deferred_publish_count !=
            (prefill_same_card != 0u ? 1u : 0u) ||
        stats.remset_buffer_deduplicated_count != 0u ||
        stats.remset_buffer_deferred_deduplicated_count != 0u ||
        stats.remset_classification_pending_thread_count != 1u ||
        stats.remset_record_count !=
            (prefill_same_card != 0u ? 2u : 1u) ||
        stats.remset_cross_region_record_count != 0u ||
        stats.remset_old_to_young_record_count != 0u ||
        stats.remset_buffer_deferred_classification_count !=
            (prefill_same_card != 0u ? 2u : 1u) ||
        stats.remset_direct_record_count != 0u ||
        stats.dirty_card_count != 0u) {
        fail("reserved RemSet card is observable before array publication");
    }
    expect_ok(nggc_internal_test_remset_rebuild_check(heap),
              "rebuild coverage includes reserved publication descriptor");
    expect_ok(nggc_check(heap),
              "full verifier accepts reserved RemSet publication");
    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_INFLIGHT_CARD) !=
        NGGC_ERR_CORRUPTION) {
        fail("out-of-range in-flight RemSet card rejected");
    }

    expect_ok(nggc_request_safepoint(heap),
              "request safepoint across deferred RemSet publication");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "observe in-flight RemSet card during safepoint request");
    if (stats.safepoint_active == 0u ||
        stats.safepoint_pending_threads != 1u ||
        stats.remset_buffered_card_count != 1u ||
        stats.remset_buffer_inflight_card_count != 1u) {
        fail("safepoint keeps in-flight RemSet publication in closure");
    }
    expect_ok(nggc_internal_test_remset_publish_pause_release(heap),
              "release deferred RemSet publication");
    for (;;) {
        size_t pending_threads = SIZE_MAX;
        nggc_result wait_result =
            nggc_safepoint_wait(heap, &pending_threads);
        if (wait_result == NGGC_OK) {
            if (pending_threads != 0u) {
                fail("deferred RemSet safepoint wait pending count");
            }
            break;
        }
        if (wait_result != NGGC_ERR_BUSY) {
            fail("wait deferred RemSet safepoint");
        }
        thrd_yield();
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read parked deferred RemSet publication stats");
    if (stats.remset_buffered_card_count != 0u ||
        stats.remset_buffer_inflight_card_count != 0u ||
        stats.remset_buffer_flush_count != 1u ||
        stats.remset_buffer_flushed_card_count != 1u ||
        stats.remset_buffer_deferred_publish_count != 1u ||
        stats.remset_buffer_deduplicated_count !=
            (prefill_same_card != 0u ? 1u : 0u) ||
        stats.remset_buffer_deferred_deduplicated_count !=
            (prefill_same_card != 0u ? 1u : 0u) ||
        stats.remset_classification_pending_thread_count != 0u ||
        stats.remset_buffer_deferred_classification_count !=
            (prefill_same_card != 0u ? 2u : 1u) ||
        stats.dirty_card_count != 1u ||
        stats.safepoint_parked_threads != 1u) {
        fail("safepoint ack flushes deferred RemSet publication");
    }
    expect_ok(nggc_safepoint_release(heap),
              "release deferred RemSet safepoint");
    if (thrd_join(worker, &worker_result) != thrd_success ||
        worker_result != 0) {
        fail("join deferred RemSet publication worker");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read completed deferred RemSet publication stats");
    if (stats.registered_thread_count != 0u ||
        stats.remset_buffered_card_count != 0u ||
        stats.remset_buffered_thread_count != 0u ||
        stats.remset_buffer_inflight_card_count != 0u ||
        stats.remset_buffer_inflight_thread_count != 0u ||
        stats.remset_buffer_flush_count != 1u ||
        stats.remset_buffer_flushed_card_count != 1u ||
        stats.remset_buffer_deferred_publish_count != 1u ||
        stats.remset_buffer_deduplicated_count !=
            (prefill_same_card != 0u ? 1u : 0u) ||
        stats.remset_buffer_deferred_deduplicated_count !=
            (prefill_same_card != 0u ? 1u : 0u) ||
        stats.remset_classification_pending_thread_count != 0u ||
        stats.remset_buffer_deferred_classification_count !=
            (prefill_same_card != 0u ? 2u : 1u) ||
        stats.dirty_card_count != 1u) {
        fail("unregister flushes completed deferred RemSet publication");
    }
    expect_ok(nggc_check(heap),
              "check completed deferred RemSet publication heap");
    expect_ok(nggc_close(heap),
              "close deferred RemSet publication heap");
}

static void run_deferred_classification_outcome_window_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    remset_publish_worker_args args;
    thrd_t worker;
    uint16_t type_id = NGGC_TYPE_NONE;
    int worker_result = -1;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)256u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = (size_t)24u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open deferred classification outcome heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(remset_node);
    desc.scan = scan_node;
    desc.name = "deferred-classification-outcome-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register deferred classification outcome type");

    memset(&args, 0, sizeof(args));
    args.heap = heap;
    args.type_id = type_id;
    args.store_result = NGGC_ERR_BUSY;
    args.park_result = NGGC_ERR_BUSY;
    args.unregister_result = NGGC_ERR_BUSY;
    atomic_init(&args.ready, 0);
    atomic_init(&args.start, 0);
    if (thrd_create(&worker, remset_publish_worker_main, &args) !=
        thrd_success) {
        fail("create deferred classification outcome worker");
    }
    while (atomic_load_explicit(&args.ready,
                                memory_order_acquire) == 0) {
        thrd_yield();
    }
    expect_ok(nggc_internal_test_remset_classification_pause_arm(heap),
              "arm deferred classification outcome pause");
    atomic_store_explicit(&args.start, 1, memory_order_release);
    while (nggc_internal_test_remset_classification_pause_wait(heap) ==
           NGGC_ERR_BUSY) {
        thrd_yield();
    }

    for (unsigned snapshot = 0u; snapshot < 2u; ++snapshot) {
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(heap, &stats),
                  "observe published classification outcome");
        if (stats.region_size != config.region_size ||
            stats.remset_classification_pending_thread_count != 1u ||
            stats.remset_record_count != 1u ||
            stats.remset_cross_region_record_count != 0u ||
            stats.remset_old_to_young_record_count != 0u ||
            stats.remset_buffer_record_count != 1u ||
            stats.remset_buffer_deferred_classification_count != 1u ||
            stats.remset_buffered_card_count != 1u ||
            stats.remset_buffer_inflight_card_count != 0u ||
            stats.remset_buffer_deferred_publish_count != 1u) {
            fail("published classification outcome counts exactly once");
        }
    }
    expect_ok(nggc_check(heap),
              "check published classification outcome window");
    expect_ok(nggc_request_safepoint(heap),
              "request classification outcome safepoint");
    expect_ok(
        nggc_internal_test_remset_classification_pause_release(heap),
        "release deferred classification outcome pause");
    for (;;) {
        size_t pending_threads = SIZE_MAX;
        nggc_result wait_result =
            nggc_safepoint_wait(heap, &pending_threads);
        if (wait_result == NGGC_OK) {
            if (pending_threads != 0u) {
                fail("classification outcome safepoint pending count");
            }
            break;
        }
        if (wait_result != NGGC_ERR_BUSY) {
            fail("wait classification outcome safepoint");
        }
        thrd_yield();
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read folded classification outcome stats");
    if (stats.remset_classification_pending_thread_count != 0u ||
        stats.remset_record_count != 1u ||
        stats.remset_buffer_record_count != 1u ||
        stats.remset_buffer_deferred_classification_count != 1u ||
        stats.remset_buffer_deferred_publish_count != 1u ||
        stats.remset_buffer_flush_count != 1u ||
        stats.safepoint_parked_threads != 1u) {
        fail("folded classification outcome remains single-counted");
    }
    expect_ok(nggc_safepoint_release(heap),
              "release classification outcome safepoint");
    if (thrd_join(worker, &worker_result) != thrd_success ||
        worker_result != 0) {
        fail("join deferred classification outcome worker");
    }
    expect_ok(nggc_check(heap),
              "check completed classification outcome heap");
    expect_ok(nggc_close(heap),
              "close deferred classification outcome heap");
}

static void run_deferred_global_coverage_window_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    remset_publish_worker_args args;
    remset_node *owner;
    remset_node *value;
    thrd_t worker;
    uint16_t type_id = NGGC_TYPE_NONE;
    int worker_result = -1;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)256u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = (size_t)24u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open deferred global-card coverage heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(remset_node);
    desc.scan = scan_node;
    desc.name = "deferred-global-card-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register deferred global-card type");
    owner = (remset_node *)nggc_alloc(heap, type_id);
    value = (remset_node *)nggc_alloc_size(
        heap, type_id, config.region_size);
    if (!owner || !value) {
        fail("allocate deferred global-card objects");
    }
    memset(owner, 0, sizeof(*owner));
    memset(value, 0, sizeof(*value));
    expect_ok(NGGC_STORE(heap, owner, &owner->edge, value),
              "publish global card before owner-buffer store");

    memset(&args, 0, sizeof(args));
    args.heap = heap;
    args.type_id = type_id;
    args.shared_owner = owner;
    args.shared_value = value;
    args.store_result = NGGC_ERR_BUSY;
    args.park_result = NGGC_ERR_BUSY;
    args.unregister_result = NGGC_ERR_BUSY;
    atomic_init(&args.ready, 0);
    atomic_init(&args.start, 0);
    if (thrd_create(&worker, remset_publish_worker_main, &args) !=
        thrd_success) {
        fail("create deferred global-card worker");
    }
    while (atomic_load_explicit(&args.ready,
                                memory_order_acquire) == 0) {
        thrd_yield();
    }
    expect_ok(nggc_internal_test_remset_publish_pause_arm(heap),
              "arm deferred global-card pause");
    atomic_store_explicit(&args.start, 1, memory_order_release);
    while (nggc_internal_test_remset_publish_pause_wait(heap) ==
           NGGC_ERR_BUSY) {
        thrd_yield();
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "observe deferred global-card descriptor");
    if (stats.remset_buffered_card_count != 0u ||
        stats.remset_buffered_thread_count != 0u ||
        stats.remset_buffer_inflight_card_count != 1u ||
        stats.remset_buffer_inflight_thread_count != 1u ||
        stats.remset_buffer_record_count != 1u ||
        stats.remset_buffer_deferred_publish_count != 0u ||
        stats.remset_buffer_deferred_deduplicated_count != 0u ||
        stats.remset_buffer_deferred_covered_count != 0u ||
        stats.remset_classification_pending_thread_count != 1u ||
        stats.remset_record_count != 2u ||
        stats.remset_cross_region_record_count != 2u ||
        stats.remset_old_to_young_record_count != 0u ||
        stats.remset_buffer_deferred_classification_count != 1u ||
        stats.remset_card_atomic_lock_free > 1u ||
        stats.remset_direct_record_count != 1u ||
        stats.dirty_card_count != 1u) {
        fail("global card absorbs effective in-flight descriptor");
    }
    expect_ok(nggc_internal_test_remset_rebuild_check(heap),
              "global card covers paused descriptor edge");
    expect_ok(nggc_check(heap),
              "check paused global-card coverage descriptor");
    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_CARD_VALUE) !=
        NGGC_ERR_CORRUPTION) {
        fail("non-binary atomic card value rejected");
    }

    expect_ok(nggc_request_safepoint(heap),
              "request safepoint across global-card coverage");
    expect_ok(nggc_internal_test_remset_publish_pause_release(heap),
              "release deferred global-card pause");
    for (;;) {
        size_t pending_threads = SIZE_MAX;
        nggc_result wait_result =
            nggc_safepoint_wait(heap, &pending_threads);
        if (wait_result == NGGC_OK) {
            if (pending_threads != 0u) {
                fail("global-card safepoint pending count");
            }
            break;
        }
        if (wait_result != NGGC_ERR_BUSY) {
            fail("wait deferred global-card safepoint");
        }
        thrd_yield();
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read parked global-card coverage stats");
    if (stats.remset_buffered_card_count != 0u ||
        stats.remset_buffer_inflight_card_count != 0u ||
        stats.remset_buffer_flush_count != 0u ||
        stats.remset_buffer_flushed_card_count != 0u ||
        stats.remset_buffer_deferred_publish_count != 0u ||
        stats.remset_buffer_deferred_deduplicated_count != 0u ||
        stats.remset_buffer_deferred_covered_count != 1u ||
        stats.remset_classification_pending_thread_count != 0u ||
        stats.remset_buffer_deferred_classification_count != 1u ||
        stats.dirty_card_count != 1u ||
        stats.safepoint_parked_threads != 1u) {
        fail("deferred global coverage avoids redundant buffer append");
    }
    expect_ok(nggc_safepoint_release(heap),
              "release global-card coverage safepoint");
    if (thrd_join(worker, &worker_result) != thrd_success ||
        worker_result != 0) {
        fail("join deferred global-card worker");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read completed global-card coverage stats");
    if (stats.registered_thread_count != 0u ||
        stats.remset_buffered_card_count != 0u ||
        stats.remset_buffer_inflight_card_count != 0u ||
        stats.remset_buffer_deferred_covered_count != 1u ||
        stats.remset_classification_pending_thread_count != 0u ||
        stats.remset_buffer_deferred_classification_count != 1u ||
        stats.dirty_card_count != 1u) {
        fail("completed global-card coverage lifecycle");
    }
    expect_ok(nggc_check(heap),
              "check completed deferred global-card heap");
    expect_ok(nggc_close(heap),
              "close deferred global-card coverage heap");
}

static void run_deferred_old_to_young_classification_window_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    remset_publish_worker_args args;
    remset_node *owner;
    remset_node *value;
    thrd_t worker;
    uint16_t type_id = NGGC_TYPE_NONE;
    uint32_t owner_generation = NGGC_REGION_GENERATION_NONE;
    uint32_t value_generation = NGGC_REGION_GENERATION_NONE;
    int worker_result = -1;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)256u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = (size_t)24u * 1024u;
    config.young_tenuring_threshold = 1u;
    expect_ok(nggc_open(&config, &heap),
              "open deferred OLD-to-YOUNG classification heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(remset_node);
    desc.scan = scan_node;
    desc.name = "deferred-old-young-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register deferred OLD-to-YOUNG type");
    owner = (remset_node *)nggc_alloc(heap, type_id);
    if (!owner) {
        fail("allocate deferred OLD owner");
    }
    memset(owner, 0, sizeof(*owner));
    expect_ok(nggc_root_push(heap, (void **)&owner),
              "root deferred OLD owner");
    expect_ok(nggc_collect_young(heap),
              "age deferred classification owner to OLD");
    expect_ok(nggc_internal_test_object_generation(
                  heap, owner, &owner_generation),
              "query deferred classification owner generation");
    if (owner_generation != NGGC_REGION_GENERATION_OLD) {
        fail("deferred classification owner reaches OLD by age");
    }
    value = (remset_node *)nggc_alloc(heap, type_id);
    if (!value) {
        fail("allocate deferred YOUNG value");
    }
    memset(value, 0, sizeof(*value));
    expect_ok(nggc_root_push(heap, (void **)&value),
              "root deferred YOUNG value");
    expect_ok(nggc_internal_test_object_generation(
                  heap, value, &value_generation),
              "query deferred classification value generation");
    if (value_generation != NGGC_REGION_GENERATION_YOUNG) {
        fail("deferred classification value remains YOUNG");
    }

    memset(&args, 0, sizeof(args));
    args.heap = heap;
    args.type_id = type_id;
    args.shared_owner = owner;
    args.shared_value = value;
    args.store_result = NGGC_ERR_BUSY;
    args.park_result = NGGC_ERR_BUSY;
    args.unregister_result = NGGC_ERR_BUSY;
    atomic_init(&args.ready, 0);
    atomic_init(&args.start, 0);
    if (thrd_create(&worker, remset_publish_worker_main, &args) !=
        thrd_success) {
        fail("create deferred OLD-to-YOUNG worker");
    }
    while (atomic_load_explicit(&args.ready,
                                memory_order_acquire) == 0) {
        thrd_yield();
    }
    expect_ok(nggc_internal_test_remset_publish_pause_arm(heap),
              "arm deferred OLD-to-YOUNG pause");
    atomic_store_explicit(&args.start, 1, memory_order_release);
    while (nggc_internal_test_remset_publish_pause_wait(heap) ==
           NGGC_ERR_BUSY) {
        thrd_yield();
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "observe deferred OLD-to-YOUNG classification");
    if (stats.region_size != config.region_size ||
        stats.remset_classification_pending_thread_count != 1u ||
        stats.remset_record_count != 1u ||
        stats.remset_cross_region_record_count != 1u ||
        stats.remset_old_to_young_record_count != 1u ||
        stats.remset_buffer_record_count != 1u ||
        stats.remset_buffer_deferred_classification_count != 1u ||
        stats.remset_buffer_inflight_card_count != 1u) {
        fail("OLD-to-YOUNG descriptor is effective before folding");
    }
    expect_ok(nggc_check(heap),
              "check deferred OLD-to-YOUNG descriptor");
    expect_ok(nggc_request_safepoint(heap),
              "request OLD-to-YOUNG descriptor safepoint");
    expect_ok(nggc_internal_test_remset_publish_pause_release(heap),
              "release deferred OLD-to-YOUNG pause");
    for (;;) {
        size_t pending_threads = SIZE_MAX;
        nggc_result wait_result =
            nggc_safepoint_wait(heap, &pending_threads);
        if (wait_result == NGGC_OK) {
            if (pending_threads != 0u) {
                fail("OLD-to-YOUNG safepoint pending count");
            }
            break;
        }
        if (wait_result != NGGC_ERR_BUSY) {
            fail("wait deferred OLD-to-YOUNG safepoint");
        }
        thrd_yield();
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read folded OLD-to-YOUNG classification");
    if (stats.remset_classification_pending_thread_count != 0u ||
        stats.remset_record_count != 1u ||
        stats.remset_cross_region_record_count != 1u ||
        stats.remset_old_to_young_record_count != 1u ||
        stats.remset_buffer_deferred_classification_count != 1u ||
        stats.safepoint_parked_threads != 1u) {
        fail("OLD-to-YOUNG classification folds exactly once");
    }
    expect_ok(nggc_safepoint_release(heap),
              "release OLD-to-YOUNG classification safepoint");
    if (thrd_join(worker, &worker_result) != thrd_success ||
        worker_result != 0) {
        fail("join deferred OLD-to-YOUNG worker");
    }
    expect_ok(nggc_root_pop(heap, (void **)&value),
              "pop deferred YOUNG value root");
    expect_ok(nggc_root_pop(heap, (void **)&owner),
              "pop deferred OLD owner root");
    expect_ok(nggc_check(heap),
              "check completed OLD-to-YOUNG classification heap");
    expect_ok(nggc_close(heap),
              "close deferred OLD-to-YOUNG classification heap");
}

static void run_barrier_tss_update_failure_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    nggc_thread *thread = 0;
    nggc_thread *thread2 = 0;
    remset_node *owner;
    remset_node *value;
    uint16_t type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)256u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open barrier TSS fault heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(remset_node);
    desc.scan = scan_node;
    desc.name = "barrier-tss-fault-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register barrier TSS fault type");
    expect_ok(nggc_internal_test_barrier_tss_fail_next_update(heap),
              "arm barrier TSS update fault");
    expect_ok(nggc_thread_register(heap, &thread),
              "registration survives barrier TSS update fault");
    owner = (remset_node *)nggc_thread_alloc(heap, thread, type_id);
    value = (remset_node *)nggc_thread_alloc(heap, thread, type_id);
    if (!owner || !value) {
        fail("allocate barrier TSS fault objects");
    }
    memset(owner, 0, sizeof(*owner));
    memset(value, 0, sizeof(*value));
    expect_ok(NGGC_STORE(heap, owner, &owner->edge, value),
              "barrier TSS fault falls back to registry scan");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read barrier TSS fault stats");
    if (stats.remset_barrier_tss_update_failure_count != 1u ||
        stats.remset_barrier_tss_fallback_scan_count != 1u ||
        stats.remset_barrier_tss_hit_count != 0u ||
        stats.remset_barrier_tss_ambiguous_count != 0u ||
        stats.remset_barrier_no_owner_direct_count != 0u ||
        stats.remset_buffer_record_count != 1u ||
        stats.remset_buffer_deferred_publish_count != 1u ||
        stats.remset_direct_record_count != 0u) {
        fail("TSS update failure preserves buffered barrier semantics");
    }
    expect_ok(nggc_check(heap), "check barrier TSS fallback heap");

    expect_ok(nggc_thread_register(heap, &thread2),
              "create ambiguous context after missing-entry fallback");
    expect_ok(nggc_thread_unregister(heap, thread2),
              "restore unique context before stale-cache fault");
    thread2 = 0;
    expect_ok(nggc_internal_test_barrier_tss_fail_next_update(heap),
              "arm existing barrier TSS update fault");
    expect_ok(nggc_thread_register(heap, &thread2),
              "second registration survives existing-cache fault");
    expect_ok(NGGC_STORE(heap, owner, &owner->edge, value),
              "invalidated stale cache falls back to ambiguous direct path");
    expect_ok(nggc_thread_unregister(heap, thread2),
              "unregister ambiguous fault handle");
    thread2 = 0;
    expect_ok(NGGC_STORE(heap, owner, &owner->edge, value),
              "post-fault unregister restores unique TSS context");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read existing-cache TSS fault stats");
    if (stats.remset_barrier_tss_update_failure_count != 2u ||
        stats.remset_barrier_tss_fallback_scan_count != 2u ||
        stats.remset_barrier_tss_hit_count != 1u ||
        stats.remset_barrier_tss_ambiguous_count != 1u ||
        stats.remset_barrier_no_owner_direct_count != 0u ||
        stats.remset_buffer_record_count != 2u ||
        stats.remset_buffer_deferred_publish_count != 1u ||
        stats.remset_direct_record_count != 1u) {
        fail("failed existing-cache update cannot expose stale unique owner");
    }
    expect_ok(nggc_check(heap), "check restored barrier TSS fault heap");
    expect_ok(nggc_thread_unregister(heap, thread),
              "unregister barrier TSS fallback thread");
    expect_ok(nggc_close(heap), "close barrier TSS fault heap");
}
#endif

int main(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    nggc_thread *thread = 0;
    remset_node *owner_a;
    remset_node *owner_b;
    remset_node *value;
    uint32_t owner_generation = NGGC_REGION_GENERATION_NONE;
    uint64_t rebuild_count_before_edges = 0u;
    uint64_t expected_tss_hits = NGGC_HAVE_C11_THREADS ? 3u : 0u;
    uint64_t expected_tss_fallback_scans =
        NGGC_HAVE_C11_THREADS ? 0u : 3u;
    uint64_t expected_deferred_publications =
        NGGC_HAVE_C11_THREADS ? 2u : 0u;
    uint16_t type_id = NGGC_TYPE_NONE;

#if NGGC_HAVE_C11_THREADS
    run_deferred_publication_window_case(0u, 0u);
    run_deferred_publication_window_case(1u, 0u);
    run_deferred_publication_window_case(0u, 1u);
    run_deferred_classification_outcome_window_case();
    run_deferred_global_coverage_window_case();
    run_deferred_old_to_young_classification_window_case();
    run_barrier_tss_update_failure_case();
#endif

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)256u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.young_tenuring_threshold = 1u;
    config.remset_buffer_capacity = 1u;
    expect_ok(nggc_open(&config, &heap), "open remset heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(remset_node);
    desc.scan = scan_node;
    desc.name = "remset_node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register remset node");

    owner_a = (remset_node *)nggc_alloc(heap, type_id);
    owner_b = (remset_node *)nggc_alloc(heap, type_id);
    expect_ok(nggc_thread_register(heap, &thread),
              "register young allocator thread");
    if (!owner_a || !owner_b) {
        fail("allocate remset nodes");
    }
    memset(owner_a, 0, sizeof(*owner_a));
    memset(owner_b, 0, sizeof(*owner_b));
    expect_ok(nggc_root_push(heap, (void **)&owner_a), "root owner a");
    expect_ok(nggc_root_push(heap, (void **)&owner_b), "root owner b");
    expect_ok(nggc_thread_collect_young(heap, thread),
              "promote remembered-set owners by age");
    expect_ok(nggc_internal_test_object_generation(
                  heap, owner_a, &owner_generation),
              "query promoted owner generation");
    if (owner_generation != NGGC_REGION_GENERATION_OLD) {
        fail("remembered-set owner reaches old by threshold");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read remset promotion baseline");
    rebuild_count_before_edges = stats.remset_rebuild_count;
    value = (remset_node *)nggc_thread_alloc(heap, thread, type_id);
    if (!value) {
        fail("allocate young remembered-set value");
    }
    memset(value, 0, sizeof(*value));
    expect_ok(nggc_root_push(heap, (void **)&value), "root value");

    expect_ok(nggc_internal_test_remset_set_limit(heap, 1u),
              "limit precise remset");
    expect_ok(NGGC_STORE(heap, owner_a, &owner_a->edge, value),
              "buffer first precise card");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read first buffered card stats");
    if (stats.remset_buffer_capacity != 1u ||
        stats.remset_buffered_card_count != 1u ||
        stats.remset_buffered_thread_count != 1u ||
        stats.dirty_card_count != 0u ||
        stats.remset_mode != NGGC_REMSET_MODE_PRECISE_CARD ||
        stats.remset_buffer_record_count != 1u ||
        stats.remset_buffer_flush_count != 0u) {
        fail("first remembered-set card remains thread-local");
    }
    expect_ok(nggc_internal_test_remset_rebuild_check(heap),
              "rebuild verifier accepts buffered coverage");
    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_BUFFER_CARD) !=
        NGGC_ERR_CORRUPTION) {
        fail("out-of-range buffered card rejected");
    }
#if NGGC_HAVE_C11_THREADS
    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_DEFERRED_OUTCOME) !=
        NGGC_ERR_CORRUPTION) {
        fail("semantic deferred RemSet outcome mismatch rejected");
    }
    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_CLASSIFICATION_OUTCOME) !=
        NGGC_ERR_CORRUPTION) {
        fail("semantic deferred classification mismatch rejected");
    }
#endif
    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_CARD_VALUE) !=
        NGGC_ERR_CORRUPTION) {
        fail("non-binary remembered-set card rejected");
    }
    expect_ok(NGGC_STORE(heap, owner_a, &owner_a->edge, value),
              "deduplicate repeated buffered card");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read deduplicated card stats");
    if (stats.remset_buffered_card_count != 1u ||
        stats.dirty_card_count != 0u ||
        stats.remset_buffer_record_count != 2u ||
        stats.remset_buffer_deduplicated_count != 1u ||
        stats.remset_buffer_deferred_deduplicated_count != 0u) {
        fail("same-card writes deduplicate in the thread buffer");
    }
    expect_ok(NGGC_STORE(heap, owner_b, &owner_b->edge, value),
              "overflow only the current thread buffer");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read overflow flush stats");
    if (stats.remset_mode != NGGC_REMSET_MODE_PRECISE_CARD ||
        stats.dirty_card_count != 1u ||
        stats.remset_buffered_card_count != 1u ||
        stats.remset_buffered_thread_count != 1u ||
        stats.remset_buffer_overflow_flush_count != 1u ||
        stats.remset_buffer_deferred_publish_count !=
            expected_deferred_publications ||
        stats.remset_buffer_flush_count != 1u ||
        stats.remset_buffer_flushed_card_count != 1u) {
        fail("buffer overflow flushes one thread and keeps the new card");
    }
    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_CROSS_REGION_CARD) !=
        NGGC_ERR_CORRUPTION) {
        fail("missing cross-region owner card rejected");
    }
    expect_ok(nggc_request_safepoint(heap),
              "request remset flush safepoint");
    expect_ok(nggc_safepoint(heap, thread),
              "flush remset buffer before safepoint acknowledgement");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read remset stats");
    if (stats.remset_mode != NGGC_REMSET_MODE_COARSE_HEAP ||
        stats.dirty_card_count != stats.card_count ||
        stats.remset_overflow_count != 1u ||
        stats.remset_max_dirty_cards != 1u ||
        stats.remset_cross_region_record_count != 3u ||
        stats.remset_old_to_young_record_count != 3u ||
        stats.remset_buffered_card_count != 0u ||
        stats.remset_buffered_thread_count != 0u ||
        stats.remset_buffer_record_count != 3u ||
        stats.remset_buffer_deduplicated_count != 1u ||
        stats.remset_buffer_deferred_deduplicated_count != 0u ||
        stats.remset_buffer_overflow_flush_count != 1u ||
        stats.remset_buffer_flush_count != 2u ||
        stats.remset_buffer_flushed_card_count != 2u ||
        stats.remset_buffer_deferred_publish_count !=
            expected_deferred_publications ||
        stats.remset_direct_record_count != 0u ||
        stats.remset_barrier_tss_hit_count != expected_tss_hits ||
        stats.remset_barrier_tss_fallback_scan_count !=
            expected_tss_fallback_scans ||
        stats.remset_barrier_tss_ambiguous_count != 0u ||
        stats.remset_barrier_tss_update_failure_count != 0u ||
        stats.remset_barrier_no_owner_direct_count != 0u) {
        fail("coarse remset fallback statistics");
    }
    expect_ok(nggc_safepoint_release(heap),
              "release remset flush safepoint");
    expect_ok(nggc_internal_test_remset_rebuild_check(heap),
              "rebuild verifier accepts coarse coverage");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read rebuild stats");
    if (stats.remset_rebuild_count != rebuild_count_before_edges + 3u ||
        stats.remset_rebuild_cross_region_edge_count != 2u ||
        stats.remset_rebuild_old_to_young_edge_count != 2u) {
        fail("rebuild verifier count");
    }

    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_CARD_COUNT) !=
        NGGC_ERR_CORRUPTION) {
        fail("dirty card count corruption rejected");
    }
    if (nggc_internal_test_remset_fault_check(
            heap, NGGC_TEST_REMSET_FAULT_COARSE_CARD) !=
        NGGC_ERR_CORRUPTION) {
        fail("coarse card coverage corruption rejected");
    }
    expect_ok(nggc_check(heap), "remset heap remains valid");
    expect_ok(nggc_thread_unregister(heap, thread),
              "unregister young allocator thread");
    expect_ok(nggc_close(heap), "close remset heap");
    return 0;
}
