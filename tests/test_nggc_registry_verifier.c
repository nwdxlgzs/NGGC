#include "nggc/nggc.h"
#include "nggc_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void) {
    static const uint32_t faults[] = {
        NGGC_TEST_REGISTRY_FAULT_ROOT_COUNT,
        NGGC_TEST_REGISTRY_FAULT_ROOT_DUPLICATE,
        NGGC_TEST_REGISTRY_FAULT_ROOT_UNALIGNED,
        NGGC_TEST_REGISTRY_FAULT_THREAD_COUNT,
        NGGC_TEST_REGISTRY_FAULT_THREAD_CYCLE,
        NGGC_TEST_REGISTRY_FAULT_THREAD_MAGIC,
        NGGC_TEST_REGISTRY_FAULT_THREAD_OWNER,
        NGGC_TEST_REGISTRY_FAULT_THREAD_STATE,
        NGGC_TEST_REGISTRY_FAULT_PENDING_COUNT,
        NGGC_TEST_REGISTRY_FAULT_OBSERVED_EPOCH,
        NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_COUNT,
        NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_DUPLICATE,
        NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_UNALIGNED
    };
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_thread *thread_a = 0;
    nggc_thread *thread_b = 0;
    void *object;
    void *root_a;
    void *root_b;
    void *thread_root;
    void *thread_fault_root_a;
    void *thread_fault_root_b;
    nggc_stats stats;
    size_t pending = 0;
    uint16_t type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "registry verifier heap open");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(void *);
    desc.name = "registry_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "registry verifier type register");
    object = nggc_alloc(heap, type_id);
    expect_true(object != 0, "registry verifier object allocation");
    root_a = object;
    root_b = object;
    expect_ok(nggc_root_push(heap, &root_a), "push registry root a");
    expect_ok(nggc_root_push(heap, &root_b), "push registry root b");
    expect_true(nggc_root_push(heap, &root_a) == NGGC_ERR_INVALID_ARGUMENT,
                "duplicate public root registration rejected");

    expect_ok(nggc_thread_register(heap, &thread_a),
              "register verifier thread a");
    expect_ok(nggc_thread_register(heap, &thread_b),
              "register verifier thread b");
    thread_root = nggc_thread_alloc(heap, thread_a, type_id);
    expect_true(thread_root != 0, "allocate thread-rooted object");
    expect_ok(nggc_thread_root_push(heap, thread_a, &thread_root),
              "push thread-local root");
    expect_true(nggc_thread_root_push(heap, thread_b, &thread_root) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "cross-thread duplicate root rejected");
    expect_true(nggc_root_push(heap, &thread_root) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "global duplicate of thread root rejected");
    expect_true(nggc_thread_unregister(heap, thread_a) == NGGC_ERR_BUSY,
                "thread with live roots cannot unregister");
    expect_ok(nggc_request_safepoint(heap),
              "request verifier safepoint baseline");
    expect_ok(nggc_safepoint(heap, thread_a),
              "ack verifier safepoint a");
    expect_ok(nggc_safepoint(heap, thread_b),
              "ack verifier safepoint b");
    expect_ok(nggc_check(heap), "valid registries accepted");
    expect_ok(nggc_collect(heap), "collect with thread-local root");
    expect_ok(nggc_safepoint_release(heap),
              "release verifier safepoint baseline");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "thread-local root stats");
    expect_true(stats.root_count == 3u && stats.live_object_count == 2u,
                "thread-local root participates in marking");
    expect_ok(nggc_thread_root_pop(heap, thread_a, &thread_root),
              "pop thread-local root");
    expect_ok(nggc_collect(heap), "collect after thread-local root pop");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "post thread-root stats");
    expect_true(stats.root_count == 2u && stats.live_object_count == 1u,
                "popped thread-local root becomes reclaimable");
    thread_fault_root_a = object;
    thread_fault_root_b = object;
    expect_ok(nggc_thread_root_push(
                  heap, thread_b, &thread_fault_root_a),
              "push thread fault root a");
    expect_ok(nggc_thread_root_push(
                  heap, thread_b, &thread_fault_root_b),
              "push thread fault root b");

    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        expect_true(nggc_internal_test_registry_fault_check(heap,
                                                             faults[i]) ==
                        NGGC_ERR_CORRUPTION,
                    "registry corruption detected");
        expect_ok(nggc_check(heap), "registry restored after fault");
    }
    expect_ok(nggc_thread_root_pop(
                  heap, thread_b, &thread_fault_root_b),
              "pop thread fault root b");
    expect_ok(nggc_thread_root_pop(
                  heap, thread_b, &thread_fault_root_a),
              "pop thread fault root a");

    expect_ok(nggc_request_safepoint(heap),
              "request partial verifier safepoint");
    expect_ok(nggc_safepoint(heap, thread_a),
              "ack partial verifier safepoint a");
    expect_true(nggc_safepoint_wait(heap, &pending) == NGGC_ERR_BUSY &&
                    pending == 1u,
                "partial verifier safepoint remains pending");
    expect_ok(nggc_check(heap), "partial safepoint registry accepted");
    expect_ok(nggc_safepoint(heap, thread_b),
              "ack partial verifier safepoint b");
    expect_ok(nggc_check(heap), "completed safepoint registry accepted");
    expect_ok(nggc_safepoint_release(heap),
              "release completed verifier safepoint");

    expect_ok(nggc_thread_unregister(heap, thread_b),
              "unregister verifier thread b");
    expect_ok(nggc_thread_unregister(heap, thread_a),
              "unregister verifier thread a");
    expect_ok(nggc_root_pop(heap, &root_b), "pop registry root b");
    expect_ok(nggc_root_pop(heap, &root_a), "pop registry root a");
    expect_ok(nggc_check(heap), "empty registries accepted");
    expect_ok(nggc_close(heap), "registry verifier heap close");
    return 0;
}
