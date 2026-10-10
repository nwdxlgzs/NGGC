#include "nggc/nggc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

typedef struct reaper_wait_args {
    nggc_heap *heap;
    atomic_int started;
    atomic_int finished;
    nggc_result result;
    void *queue;
    size_t pending_count;
} reaper_wait_args;

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void expect_true(int value, const char *message) {
    if (!value) {
        fail(message);
    }
}

static void expect_ok(nggc_result result, const char *message) {
    if (result != NGGC_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static int reaper_wait_worker(void *arg) {
    reaper_wait_args *args = (reaper_wait_args *)arg;

    atomic_store_explicit(&args->started, 1, memory_order_release);
    args->result = nggc_phantom_reaper_wait_blocking(
        args->heap, &args->queue, &args->pending_count);
    atomic_store_explicit(&args->finished, 1, memory_order_release);
    return args->result == NGGC_OK ? 0 : 1;
}

int main(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *queue = 0;
    void *referent = 0;
    void *phantom_ref = 0;
    void *root = 0;
    void *seen_queue = 0;
    void *seen_ref = 0;
    size_t pending_count = 0u;
    reaper_wait_args args;
    thrd_t worker;
    int worker_result = 0;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open reaper wait heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(void *);
    desc.name = "reaper_wait_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register reaper wait type");

    expect_ok(nggc_phantom_queue_create(heap, &queue),
              "create reaper wait queue");
    referent = nggc_alloc(heap, type_id);
    if (!referent) {
        fail("allocate reaper wait referent");
    }
    expect_ok(nggc_phantom_ref_create(heap, referent, queue, &phantom_ref),
              "create reaper wait phantom");
    root = phantom_ref;
    expect_ok(nggc_phantom_reaper_watch(heap, queue),
              "watch reaper wait queue");
    expect_ok(nggc_root_push(heap, &root), "root reaper wait phantom");

    memset(&args, 0, sizeof(args));
    args.heap = heap;
    atomic_init(&args.started, 0);
    atomic_init(&args.finished, 0);
    if (thrd_create(&worker, reaper_wait_worker, &args) != thrd_success) {
        fail("create reaper wait worker");
    }
    while (atomic_load_explicit(&args.started, memory_order_acquire) == 0) {
        thrd_yield();
    }
    for (unsigned i = 0u; i < 10000u; ++i) {
        if (atomic_load_explicit(&args.finished, memory_order_acquire) != 0) {
            fail("reaper wait returned before queue notification");
        }
        thrd_yield();
    }

    expect_ok(nggc_phantom_reaper_unwatch(heap, queue),
              "unwatch wakes reaper wait");
    if (thrd_join(worker, &worker_result) != thrd_success ||
        worker_result != 1 || args.result != NGGC_ERR_BUSY ||
        args.queue != 0 || args.pending_count != 0u) {
        fail("reaper wait unwatch result");
    }
    expect_ok(nggc_phantom_reaper_watch(heap, queue),
              "rewatch reaper wait queue");

    atomic_store_explicit(&args.started, 0, memory_order_relaxed);
    atomic_store_explicit(&args.finished, 0, memory_order_relaxed);
    args.result = NGGC_ERR_BUSY;
    args.queue = 0;
    args.pending_count = 0u;
    worker_result = 0;
    if (thrd_create(&worker, reaper_wait_worker, &args) != thrd_success) {
        fail("create second reaper wait worker");
    }
    while (atomic_load_explicit(&args.started, memory_order_acquire) == 0) {
        thrd_yield();
    }
    for (unsigned i = 0u; i < 10000u; ++i) {
        if (atomic_load_explicit(&args.finished, memory_order_acquire) != 0) {
            fail("second reaper wait returned before queue notification");
        }
        thrd_yield();
    }

    expect_ok(nggc_collect(heap), "collect and notify reaper wait");
    if (thrd_join(worker, &worker_result) != thrd_success ||
        worker_result != 0 || args.result != NGGC_OK ||
        args.queue != queue || args.pending_count != 1u) {
        fail("reaper wait worker result");
    }

    expect_ok(nggc_phantom_reaper_pop(heap, &seen_queue, &seen_ref),
              "pop notified phantom");
    if (seen_queue != queue || seen_ref != phantom_ref) {
        fail("pop returned the wrong notified phantom");
    }
    expect_ok(nggc_root_pop(heap, &root), "unroot reaper wait phantom");
    expect_ok(nggc_collect(heap), "collect drained reaper wait queue");

    seen_queue = (void *)1;
    pending_count = 99u;
    expect_true(nggc_phantom_reaper_wait_blocking(
                    heap, &seen_queue, &pending_count) == NGGC_ERR_BUSY,
                "stale reaper wait returns busy");
    if (seen_queue != 0 || pending_count != 0u) {
        fail("stale reaper wait outputs");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read reaper wait stats");
    if (stats.phantom_reaper_watched_queue_count != 0u ||
        stats.phantom_reaper_dropped_queue_count != 1u) {
        fail("reaper wait stale queue accounting");
    }
    expect_ok(nggc_check(heap), "check reaper wait heap");
    expect_ok(nggc_close(heap), "close reaper wait heap");
    return 0;
}
