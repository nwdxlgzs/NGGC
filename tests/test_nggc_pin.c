#include "nggc/nggc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NGGC_HAVE_C11_THREADS) && NGGC_HAVE_C11_THREADS
#include <stdatomic.h>
#include <threads.h>
#endif

typedef struct pin_value {
    uint32_t value;
    uint32_t padding;
} pin_value;

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

static void expect_result(nggc_result result, nggc_result expected,
                          const char *message) {
    if (result != expected) {
        fprintf(stderr, "%s: got %d expected %d\n", message, (int)result,
                (int)expected);
        exit(1);
    }
}

static void read_stats(nggc_heap *heap, nggc_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->size = sizeof(*stats);
    expect_ok(nggc_get_stats(heap, stats), "read pin stats");
}

#if defined(NGGC_HAVE_C11_THREADS) && NGGC_HAVE_C11_THREADS
static void expect_true(int value, const char *message) {
    if (!value) {
        fail(message);
    }
}

typedef struct pin_conflict_worker_args {
    nggc_heap *heap;
    void *object;
    atomic_int ready;
    atomic_int release;
    nggc_result register_result;
    nggc_result pin_result;
    nggc_result unpin_result;
    nggc_result unregister_result;
} pin_conflict_worker_args;

static int pin_conflict_worker(void *context) {
    pin_conflict_worker_args *args = (pin_conflict_worker_args *)context;
    nggc_thread *thread = 0;

    args->register_result = nggc_thread_register(args->heap, &thread);
    if (args->register_result == NGGC_OK) {
        args->pin_result = nggc_pin(args->heap, args->object);
    } else {
        args->pin_result = args->register_result;
    }
    atomic_store_explicit(&args->ready, 1, memory_order_release);
    while (atomic_load_explicit(&args->release, memory_order_acquire) == 0) {
        thrd_yield();
    }
    if (args->pin_result == NGGC_OK) {
        args->unpin_result = nggc_unpin(args->heap, args->object);
    } else {
        args->unpin_result = args->pin_result;
    }
    if (thread) {
        args->unregister_result = nggc_thread_unregister(args->heap, thread);
    } else {
        args->unregister_result = args->register_result;
    }
    return 0;
}
#endif

int main(void) {
    nggc_config config;
    nggc_type_desc normal_desc;
    nggc_type_desc pinnable_desc;
    nggc_heap *heap = 0;
    uint16_t normal_type = NGGC_TYPE_NONE;
    uint16_t pinnable_type = NGGC_TYPE_NONE;
    pin_value *object;
    pin_value *dead;
    pin_value *owner_object;
    void *root;
    void *old_address;
    nggc_stats stats;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = (size_t)24u * 1024u;
    config.young_tenuring_threshold = 2u;
    expect_ok(nggc_open(&config, &heap), "open pin heap");

    memset(&normal_desc, 0, sizeof(normal_desc));
    normal_desc.size = sizeof(normal_desc);
    normal_desc.object_size = sizeof(pin_value);
    normal_desc.name = "normal-pin-value";
    expect_ok(nggc_register_type(heap, &normal_desc, &normal_type),
              "register normal pin type");

    pinnable_desc = normal_desc;
    pinnable_desc.flags = NGGC_TYPE_FLAG_PINNABLE;
    pinnable_desc.name = "pinnable-pin-value";
    expect_ok(nggc_register_type(heap, &pinnable_desc, &pinnable_type),
              "register pinnable type");
    expect_ok(nggc_type_table_freeze(heap), "freeze pin types");

    object = (pin_value *)nggc_alloc(heap, pinnable_type);
    if (!object) {
        fail("allocate pinnable object");
    }
    object->value = UINT32_C(0x12345678);
    root = object;
    expect_ok(nggc_root_push(heap, &root), "root pinnable object");
    old_address = object;

    read_stats(heap, &stats);
    if (stats.pinnable_object_count != 1u ||
        stats.pinned_region_count != 0u) {
        fail("unpinned pinnable Young object is not a pinned Region");
    }

    expect_result(nggc_pin(heap, nggc_alloc(heap, normal_type)),
                  NGGC_ERR_UNSUPPORTED, "reject non-pinnable object");
    expect_ok(nggc_pin(heap, object), "first pin");
    expect_ok(nggc_pin(heap, object), "nested pin");
    if (nggc_object_pin_count(object) != 2u) {
        fail("nested pin count is two");
    }
    read_stats(heap, &stats);
    if (stats.pinned_object_count != 1u || stats.pin_lease_count != 2u ||
        stats.pin_acquire_count != 2u || stats.pinned_region_count == 0u) {
        fail("pin counters after nested pin");
    }

    {
        nggc_thread *owner_thread = 0;
        owner_object = (pin_value *)nggc_alloc(heap, pinnable_type);
        if (!owner_object) {
            fail("allocate owner pin object");
        }
        expect_ok(nggc_thread_register(heap, &owner_thread),
                  "register pin owner thread");
        expect_ok(nggc_pin(heap, owner_object), "owner first pin");
        expect_ok(nggc_pin(heap, owner_object), "owner nested pin");
        expect_result(nggc_thread_unregister(heap, owner_thread),
                      NGGC_ERR_BUSY,
                      "reject owner thread exit with pin lease");
        read_stats(heap, &stats);
        if (stats.pin_lease_record_count != 2u ||
            stats.pin_thread_exit_busy_count != 1u) {
            fail("pin owner exit statistics");
        }
        expect_ok(nggc_unpin(heap, owner_object), "owner release nested pin");
        expect_ok(nggc_unpin(heap, owner_object), "owner release final pin");
        expect_ok(nggc_thread_unregister(heap, owner_thread),
                  "unregister released pin owner");
        expect_ok(nggc_check(heap), "check released pin owner ledger");
    }

#if defined(NGGC_HAVE_C11_THREADS) && NGGC_HAVE_C11_THREADS
    {
        pin_conflict_worker_args args;
        thrd_t worker;
        int worker_result = 0;
        pin_value *conflict_object =
            (pin_value *)nggc_alloc(heap, pinnable_type);
        if (!conflict_object) {
            fail("allocate conflict pin object");
        }
        memset(&args, 0, sizeof(args));
        args.heap = heap;
        args.object = conflict_object;
        atomic_init(&args.ready, 0);
        atomic_init(&args.release, 0);
        if (thrd_create(&worker, pin_conflict_worker, &args) != thrd_success) {
            fail("create pin conflict worker");
        }
        while (atomic_load_explicit(&args.ready, memory_order_acquire) == 0) {
            thrd_yield();
        }
        expect_ok(args.register_result, "register conflict pin owner");
        expect_ok(args.pin_result, "pin conflict object in worker");
        expect_result(nggc_unpin(heap, conflict_object),
                      NGGC_ERR_INVALID_ARGUMENT,
                      "reject cross-owner unpin");
        atomic_store_explicit(&args.release, 1, memory_order_release);
        expect_true(thrd_join(worker, &worker_result) == thrd_success &&
                        worker_result == 0,
                    "join pin conflict worker");
        expect_ok(args.unpin_result, "worker releases conflict pin");
        expect_ok(args.unregister_result,
                  "worker unregisters released conflict pin");
        read_stats(heap, &stats);
        if (stats.pin_owner_conflict_count == 0u ||
            stats.pin_lease_record_count != 1u) {
            fail("pin owner conflict statistics");
        }
        expect_ok(nggc_check(heap), "check cross-owner pin ledger");
    }
#endif

    expect_result(nggc_phase_begin(heap, NGGC_PHASE_MARK), NGGC_OK,
                  "begin standalone phase");
    expect_result(nggc_pin(heap, object), NGGC_ERR_BUSY,
                  "reject pin during phase");
    expect_result(nggc_unpin(heap, object), NGGC_ERR_BUSY,
                  "reject unpin during phase");
    expect_ok(nggc_phase_end(heap, NGGC_PHASE_MARK), "end standalone phase");
    expect_ok(nggc_request_safepoint(heap), "request empty safepoint");
    expect_result(nggc_pin(heap, object), NGGC_ERR_BUSY,
                  "reject pin during safepoint");
    expect_ok(nggc_safepoint_release(heap), "release empty safepoint");

    expect_ok(nggc_unpin(heap, object), "release nested pin");
    expect_ok(nggc_unpin(heap, object), "release final pin");
    expect_result(nggc_unpin(heap, object), NGGC_ERR_INVALID_ARGUMENT,
                  "reject pin underflow");
    if (nggc_object_pin_count(object) != 0u) {
        fail("pin count returns to zero");
    }
    read_stats(heap, &stats);
    if (stats.pinned_region_count != 0u) {
        fail("final unpin clears pinned Region");
    }

    expect_ok(nggc_pin(heap, object), "pin before young collection");
    old_address = object;
    expect_ok(nggc_collect_young(heap), "young collection with pinned object");
    if (root != object || object->value != UINT32_C(0x12345678) ||
        nggc_object_pin_count(object) != 1u) {
        fail("pinned object remains in place after first young collection");
    }
    read_stats(heap, &stats);
    if (stats.young_object_count != 1u || stats.old_object_count != 0u ||
        stats.young_tenuring_candidate_count != 0u ||
        stats.pinned_object_count != 1u || stats.pin_lease_count != 1u) {
        fail("first pinned young collection state");
    }

    expect_ok(nggc_collect_young(heap),
              "second young collection with pinned object");
    if (root != object || nggc_object_pin_count(object) != 1u) {
        fail("pinned object remains in place at tenuring age");
    }
    read_stats(heap, &stats);
    if (stats.young_object_count != 1u || stats.old_object_count != 0u ||
        stats.young_tenuring_candidate_count != 1u) {
        fail("pinned object becomes a young tenuring candidate");
    }

    expect_ok(nggc_unpin(heap, object), "unpin tenured candidate");
    expect_ok(nggc_collect_young(heap), "evacuate after unpin");
    object = (pin_value *)root;
    if (!object || object == (pin_value *)old_address ||
        object->value != UINT32_C(0x12345678) ||
        nggc_object_pin_count(object) != 0u) {
        fail("unpinned candidate evacuates and preserves payload");
    }
    read_stats(heap, &stats);
    if (stats.young_object_count != 0u || stats.old_object_count != 1u ||
        stats.pinned_object_count != 0u || stats.pin_lease_count != 0u) {
        fail("unpinned candidate is promoted to old");
    }
    expect_ok(nggc_check(heap), "check promoted pin heap");
    expect_ok(nggc_collect(heap), "finish from-space quarantine");

    dead = (pin_value *)nggc_alloc(heap, pinnable_type);
    if (!dead) {
        fail("allocate unreachable pinned object");
    }
    expect_ok(nggc_pin(heap, dead), "pin unreachable object");
    read_stats(heap, &stats);
    if (stats.pinned_object_count != 1u || stats.pin_lease_count != 1u) {
        fail("unreachable pin is accounted");
    }
    expect_ok(nggc_collect(heap), "collect unreachable pinned object");
    read_stats(heap, &stats);
    if (stats.pinned_object_count != 0u || stats.pin_lease_count != 0u) {
        fail("unreachable pin is reclaimed");
    }
    if (nggc_object_pin_count(dead) != 0u) {
        fail("reclaimed object has no stale pin count");
    }
    expect_result(nggc_unpin(heap, dead), NGGC_ERR_INVALID_ARGUMENT,
                  "reject unpin of reclaimed object");

    expect_ok(nggc_root_pop(heap, &root), "unroot promoted object");
    expect_ok(nggc_collect(heap), "collect final pin heap");
    expect_ok(nggc_check(heap), "check final pin heap");
    read_stats(heap, &stats);
    if (stats.pinned_object_count != 0u || stats.pin_lease_count != 0u ||
        stats.pin_busy_count < 3u || stats.pin_unpin_error_count < 2u) {
        fail("final pin statistics");
    }
    expect_ok(nggc_close(heap), "close pin heap");
    return 0;
}
