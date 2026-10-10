#include "nggc/nggc.h"
#include "nggc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef NGGC_HAVE_C11_THREADS
#define NGGC_HAVE_C11_THREADS 0
#endif

typedef struct gc_pool_leaf {
    struct gc_pool_leaf *edge;
    unsigned char bytes[64];
} gc_pool_leaf;

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

#if NGGC_HAVE_C11_THREADS
#include <threads.h>
enum { GC_POOL_OBJECT_COUNT = 64 };

static void scan_pool_leaf(void *object, nggc_visit_fn visit, void *ctx) {
    visit(ctx, (void **)&((gc_pool_leaf *)object)->edge);
}

static void check_objects(void **objects) {
    for (size_t i = 0u; i < GC_POOL_OBJECT_COUNT; ++i) {
        gc_pool_leaf *object = (gc_pool_leaf *)objects[i];
        expect_true(object != 0, "GC pool root survives worker failure");
        expect_true(object->edge == objects[(i + 1u) % GC_POOL_OBJECT_COUNT],
                    "GC pool ring edges are remapped after worker failure");
        for (size_t j = 0u; j < sizeof(object->bytes); ++j) {
            expect_true(object->bytes[j] == (unsigned char)(i + 1u),
                        "GC pool payload survives worker failure");
        }
    }
}

static void wait_for_worker_failure(nggc_heap *heap) {
    struct timespec started;
    expect_true(timespec_get(&started, TIME_UTC) == TIME_UTC,
                "read worker failure deadline");
    for (;;) {
        struct timespec now;
        nggc_result result =
            nggc_internal_test_gc_pool_worker_failure_wait(heap);
        if (result == NGGC_OK) {
            return;
        }
        expect_true(result == NGGC_ERR_BUSY, "poll worker failure");
        expect_true(timespec_get(&now, TIME_UTC) == TIME_UTC &&
                        difftime(now.tv_sec, started.tv_sec) < 10.0,
                    "worker failure is observed within deadline");
        thrd_yield();
    }
}

static nggc_heap *open_populated_heap(void **objects) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)1024u * 1024u;
    config.uhm_arena_size = (size_t)4u * 1024u * 1024u;
    config.young_tenuring_threshold = 8u;
    expect_ok(nggc_open(&config, &heap), "open GC pool heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(gc_pool_leaf);
    desc.scan = scan_pool_leaf;
    desc.name = "gc-pool-leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register GC pool type");

    for (size_t i = 0u; i < GC_POOL_OBJECT_COUNT; ++i) {
        void *object = nggc_alloc(heap, type_id);
        expect_true(object != 0, "allocate GC pool object");
        memset(((gc_pool_leaf *)object)->bytes, (int)(i + 1u),
               sizeof(((gc_pool_leaf *)object)->bytes));
        objects[i] = object;
        expect_ok(nggc_root_push(heap, &objects[i]),
                  "root GC pool object");
    }
    for (size_t i = 0u; i < GC_POOL_OBJECT_COUNT; ++i) {
        gc_pool_leaf *object = (gc_pool_leaf *)objects[i];
        expect_ok(nggc_store(heap, object, (void **)&object->edge,
                            objects[(i + 1u) % GC_POOL_OBJECT_COUNT]),
                  "connect GC pool object ring");
    }

    expect_ok(nggc_collect_young(heap),
              "prime GC pool with parallel young collection");
    check_objects(objects);
    {
        nggc_stats stats;
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(heap, &stats),
                  "read primed GC pool stats");
        expect_true(stats.evacuation_worker_pool_size != 0u,
                    "parallel GC pool was created");
        expect_true(stats.evacuation_workers_active == 0u &&
                        stats.evacuation_workers_quiescent == 0u,
                    "primed GC pool is quiescent");
    }
    return heap;
}

static void unroot_objects(nggc_heap *heap, void **objects) {
    for (size_t i = GC_POOL_OBJECT_COUNT; i != 0u; --i) {
        expect_ok(nggc_root_pop(heap, (void **)&objects[i - 1u]),
                  "unroot GC pool object");
    }
}

static void run_coordinator_wait_failure_case(void) {
    void *objects[GC_POOL_OBJECT_COUNT];
    nggc_heap *heap = open_populated_heap(objects);
    nggc_stats stats;

    expect_ok(nggc_internal_test_gc_pool_fault_set(
                  heap, NGGC_TEST_GC_POOL_FAULT_WAIT),
              "arm GC pool coordinator wait fault");
    expect_true(nggc_collect_young(heap) == NGGC_ERR_BACKEND,
                "coordinator wait failure is propagated");
    expect_ok(nggc_check(heap),
              "heap verifies after coordinator wait failure");
    check_objects(objects);
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read coordinator wait failure stats");
    expect_true(stats.evacuation_workers_active == 0u &&
                    stats.evacuation_workers_quiescent == 0u &&
                    stats.gc_phase == NGGC_PHASE_IDLE,
                "coordinator wait failure quiesces all workers");

    unroot_objects(heap, objects);
    expect_ok(nggc_collect(heap),
              "reclaim coordinator wait failure heap");
    expect_ok(nggc_close(heap),
              "close after coordinator wait failure");
}

static void run_dispatch_broadcast_failure_case(void) {
    void *objects[GC_POOL_OBJECT_COUNT];
    nggc_heap *heap = open_populated_heap(objects);
    nggc_stats stats;

    expect_ok(nggc_internal_test_gc_pool_fault_set(
                  heap, NGGC_TEST_GC_POOL_FAULT_BROADCAST),
              "arm GC pool dispatch broadcast fault");
    expect_ok(nggc_collect_young(heap),
              "dispatch broadcast failure falls back to collector worker");
    expect_ok(nggc_check(heap),
              "heap verifies after dispatch broadcast failure");
    check_objects(objects);
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read dispatch broadcast failure stats");
    expect_true(stats.evacuation_workers_active == 0u &&
                    stats.evacuation_workers_quiescent == 0u &&
                    stats.evacuation_worker_fallback_count != 0u &&
                    stats.gc_phase == NGGC_PHASE_IDLE,
                "dispatch broadcast failure leaves a quiescent fallback");

    unroot_objects(heap, objects);
    expect_ok(nggc_collect(heap),
              "reclaim dispatch broadcast failure heap");
    expect_ok(nggc_close(heap),
              "close after dispatch broadcast failure");
}

static void run_close_broadcast_failure_case(void) {
    void *objects[GC_POOL_OBJECT_COUNT];
    nggc_heap *heap = open_populated_heap(objects);
    uint32_t state = 0u;
    nggc_result reason = NGGC_OK;

    unroot_objects(heap, objects);
    expect_ok(nggc_internal_test_gc_pool_fault_set(
                  heap, NGGC_TEST_GC_POOL_FAULT_BROADCAST),
              "arm GC pool close broadcast fault");
    expect_true(nggc_close(heap) == NGGC_ERR_BACKEND,
                "close broadcast failure is reported");
    expect_ok(nggc_get_heap_state(heap, &state, &reason),
              "read heap state after close broadcast failure");
    expect_true(state == NGGC_HEAP_STATE_RUNNING && reason == NGGC_OK,
                "failed close keeps a live running heap handle");
    expect_ok(nggc_check(heap),
              "heap verifies during close broadcast retry state");
    expect_ok(nggc_close(heap),
              "retry close after broadcast failure");
}

static void run_join_failure_retry_case(void) {
    void *objects[GC_POOL_OBJECT_COUNT];
    nggc_heap *heap = open_populated_heap(objects);
    uint32_t state = 0u;
    nggc_result reason = NGGC_OK;

    unroot_objects(heap, objects);
    expect_ok(nggc_internal_test_gc_pool_fault_set(
                  heap, NGGC_TEST_GC_POOL_FAULT_JOIN),
              "arm GC pool join fault");
    expect_true(nggc_close(heap) == NGGC_ERR_BACKEND,
                "join failure is reported");
    expect_ok(nggc_get_heap_state(heap, &state, &reason),
              "read heap state after join failure");
    expect_true(state == NGGC_HEAP_STATE_RUNNING && reason == NGGC_OK,
                "failed join keeps a live running heap handle");
    expect_ok(nggc_check(heap),
              "heap verifies during join retry state");
    expect_ok(nggc_close(heap),
              "retry close after join failure");
}

static void run_worker_failure_case(uint32_t fault) {
    void *objects[GC_POOL_OBJECT_COUNT];
    nggc_heap *heap = open_populated_heap(objects);
    nggc_stats before;
    nggc_stats after;
    uint32_t state = 0u;
    nggc_result reason = NGGC_OK;

    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(nggc_get_stats(heap, &before), "read worker failure baseline");
    expect_ok(nggc_internal_test_gc_pool_fault_set(heap, fault),
              "arm worker failure");
    if (fault == NGGC_TEST_GC_POOL_FAULT_WORKER_WAIT) {
        wait_for_worker_failure(heap);
        expect_ok(nggc_check(heap), "verify idle failed worker state");
        expect_ok(nggc_collect_young(heap),
                  "idle failed worker causes synchronous collection fallback");
    } else {
        expect_true(nggc_collect_young(heap) == NGGC_ERR_BACKEND,
                    "dispatched worker failure preserves backend error");
        wait_for_worker_failure(heap);
    }
    expect_ok(nggc_check(heap), "verify quiescent failed worker state");
    check_objects(objects);

    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(nggc_get_stats(heap, &after), "read failed worker stats");
    expect_true(after.gc_phase == NGGC_PHASE_IDLE &&
                    after.evacuation_workers_active == 0u &&
                    after.remap_workers_active == 0u &&
                    after.from_space_reclaim_pending == 0u,
                "failed worker is drained before collection returns");
    if (fault == NGGC_TEST_GC_POOL_FAULT_WORKER_WAIT) {
        expect_true(after.evacuation_worker_dispatch_count ==
                        before.evacuation_worker_dispatch_count &&
                        after.remap_worker_dispatch_count ==
                            before.remap_worker_dispatch_count,
                    "idle dead worker pool receives no further dispatch");
    }

    before = after;
    expect_ok(nggc_collect_young(heap),
              "subsequent young collection uses synchronous fallback");
    check_objects(objects);
    after.size = sizeof(after);
    expect_ok(nggc_get_stats(heap, &after), "read fallback worker stats");
    expect_true(after.evacuation_worker_dispatch_count ==
                    before.evacuation_worker_dispatch_count &&
                    after.remap_worker_dispatch_count ==
                        before.remap_worker_dispatch_count &&
                    after.evacuation_worker_fallback_count >
                        before.evacuation_worker_fallback_count &&
                    after.remap_worker_fallback_count >
                        before.remap_worker_fallback_count,
                "broken pool is excluded from copy and REMAP dispatch");

    unroot_objects(heap, objects);
    expect_ok(nggc_collect(heap), "reclaim failed worker heap");
    expect_ok(nggc_check(heap), "verify reclaimed failed worker heap");
    expect_true(nggc_close(heap) == NGGC_ERR_BACKEND,
                "successful join preserves nonzero worker result");
    expect_ok(nggc_get_heap_state(heap, &state, &reason),
              "failed worker close keeps heap handle live");
    expect_true(state == NGGC_HEAP_STATE_RUNNING && reason == NGGC_OK,
                "failed worker close exposes running heap state");
    expect_ok(nggc_check(heap), "verify joined worker during close retry");
    expect_ok(nggc_close(heap), "retry close never joins failed worker twice");
}
#endif

int main(void) {
#if NGGC_HAVE_C11_THREADS
    run_coordinator_wait_failure_case();
    run_dispatch_broadcast_failure_case();
    run_close_broadcast_failure_case();
    run_join_failure_retry_case();
    run_worker_failure_case(NGGC_TEST_GC_POOL_FAULT_WORKER_WAIT);
    run_worker_failure_case(NGGC_TEST_GC_POOL_FAULT_WORKER_BEFORE_JOB);
    run_worker_failure_case(NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_JOB);
    run_worker_failure_case(NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_COMPLETE);
    run_worker_failure_case(NGGC_TEST_GC_POOL_FAULT_REMAP_BEFORE_JOB);
    run_worker_failure_case(NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_JOB);
    run_worker_failure_case(NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_COMPLETE);
#endif
    return 0;
}
