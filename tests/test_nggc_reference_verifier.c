#include "nggc/nggc.h"
#include "nggc_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct reference_fault_case {
    uint32_t fault;
    void *target;
    void *auxiliary;
} reference_fault_case;

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
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    void *soft_referent;
    void *soft_ref;
    void *weak_referent;
    void *weak_ref;
    void *phantom_referent;
    void *phantom_queue;
    void *phantom_ref;
    void *shared_referent;
    void *shared_soft;
    void *shared_weak;
    void *shared_queue;
    void *shared_phantom;
    void *dead_referent;
    void *dead_queue;
    void *dead_phantom;
    void *value = 0;
    void *popped = 0;
    size_t pending = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    reference_fault_case faults[10];

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.soft_reference_policy =
        NGGC_SOFT_REFERENCE_POLICY_ALWAYS_RETAIN;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "reference verifier heap open");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(void *);
    desc.name = "reference_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "reference verifier type register");

    soft_referent = nggc_alloc(heap, type_id);
    expect_true(soft_referent != 0, "soft-only referent allocation");
    expect_ok(nggc_soft_ref_create(heap, soft_referent, &soft_ref),
              "soft-only reference creation");
    expect_ok(nggc_root_push(heap, &soft_ref), "root soft-only reference");

    weak_referent = nggc_alloc(heap, type_id);
    expect_true(weak_referent != 0, "weak-only referent allocation");
    expect_ok(nggc_weak_ref_create(heap, weak_referent, &weak_ref),
              "weak-only reference creation");
    expect_ok(nggc_root_push(heap, &weak_referent),
              "root weak-only referent");
    expect_ok(nggc_root_push(heap, &weak_ref), "root weak-only reference");

    phantom_referent = nggc_alloc(heap, type_id);
    expect_true(phantom_referent != 0,
                "live phantom referent allocation");
    expect_ok(nggc_phantom_queue_create(heap, &phantom_queue),
              "live phantom queue creation");
    expect_ok(nggc_phantom_ref_create(heap, phantom_referent,
                                      phantom_queue, &phantom_ref),
              "live phantom reference creation");
    expect_ok(nggc_root_push(heap, &phantom_referent),
              "root live phantom referent");
    expect_ok(nggc_root_push(heap, &phantom_ref),
              "root live phantom reference");

    shared_referent = nggc_alloc(heap, type_id);
    expect_true(shared_referent != 0, "shared referent allocation");
    expect_ok(nggc_soft_ref_create(heap, shared_referent, &shared_soft),
              "shared soft creation");
    expect_ok(nggc_weak_ref_create(heap, shared_referent, &shared_weak),
              "shared weak creation");
    expect_ok(nggc_phantom_queue_create(heap, &shared_queue),
              "shared phantom queue creation");
    expect_ok(nggc_phantom_ref_create(heap, shared_referent, shared_queue,
                                      &shared_phantom),
              "shared phantom creation");
    expect_ok(nggc_root_push(heap, &shared_soft), "root shared soft");
    expect_ok(nggc_root_push(heap, &shared_weak), "root shared weak");
    expect_ok(nggc_root_push(heap, &shared_phantom),
              "root shared phantom");

    dead_referent = nggc_alloc(heap, type_id);
    expect_true(dead_referent != 0, "dead phantom referent allocation");
    expect_ok(nggc_phantom_queue_create(heap, &dead_queue),
              "dead phantom queue creation");
    expect_ok(nggc_phantom_ref_create(heap, dead_referent, dead_queue,
                                      &dead_phantom),
              "dead phantom creation");
    expect_ok(nggc_root_push(heap, &dead_phantom), "root dead phantom");

    expect_ok(nggc_collect(heap), "reference verifier collection");
    expect_ok(nggc_check(heap), "reference processing verified");
    expect_ok(nggc_soft_ref_get(heap, shared_soft, &value),
              "read shared soft after processing");
    expect_true(value == shared_referent,
                "soft stage retained shared referent");
    expect_ok(nggc_weak_ref_get(heap, shared_weak, &value),
              "read shared weak after processing");
    expect_true(value == shared_referent,
                "weak stage observed soft-retained referent");
    expect_ok(nggc_phantom_queue_count(heap, shared_queue, &pending),
              "shared phantom queue count");
    expect_true(pending == 0u,
                "phantom stage did not enqueue soft-retained referent");
    expect_ok(nggc_phantom_queue_count(heap, phantom_queue, &pending),
              "live phantom queue survived while empty");
    expect_true(pending == 0u, "live phantom queue remains empty");
    expect_ok(nggc_phantom_queue_count(heap, dead_queue, &pending),
              "dead phantom queue count");
    expect_true(pending == 1u, "dead phantom enqueued once");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "reference verifier stats");
    expect_true(stats.soft_reference_discovered_count == 2u &&
                    stats.soft_reference_retained_count == 2u &&
                    stats.weak_reference_discovered_count == 2u &&
                    stats.weak_reference_cleared_count == 0u &&
                    stats.phantom_reference_discovered_count == 3u &&
                    stats.phantom_reference_enqueued_count == 1u &&
                    stats.phantom_queue_promoted_count == 1u,
                "reference processing order counters");

    faults[0] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_SOFT_REFERENT_MARK, soft_ref, 0};
    faults[1] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_WEAK_REFERENT_MARK, weak_ref, 0};
    faults[2] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_PHANTOM_REFERENT_MARK, phantom_ref, 0};
    faults[3] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_PHANTOM_QUEUE_MARK, phantom_ref, 0};
    faults[4] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_ENQUEUED_REFERENT, dead_phantom,
        shared_referent};
    faults[5] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_ENQUEUED_MARK, dead_phantom, 0};
    faults[6] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_ORPHAN_ENQUEUED, phantom_ref, 0};
    faults[7] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_QUEUE_OWNER, dead_phantom,
        phantom_queue};
    faults[8] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_QUEUE_COUNT, dead_queue, 0};
    faults[9] = (reference_fault_case){
        NGGC_TEST_REFERENCE_FAULT_ENQUEUED_FLAG, dead_phantom, 0};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        expect_true(nggc_internal_test_reference_fault_check(
                        heap, faults[i].fault, faults[i].target,
                        faults[i].auxiliary) == NGGC_ERR_CORRUPTION,
                    "reference processing corruption detected");
        expect_ok(nggc_check(heap), "reference graph restored after fault");
    }

    expect_ok(nggc_phantom_queue_pop(heap, dead_queue, &popped),
              "drain dead phantom queue");
    expect_true(popped == dead_phantom, "drained expected phantom");
    expect_ok(nggc_root_pop(heap, &dead_phantom), "pop dead phantom root");
    expect_ok(nggc_root_pop(heap, &shared_phantom),
              "pop shared phantom root");
    expect_ok(nggc_root_pop(heap, &shared_weak), "pop shared weak root");
    expect_ok(nggc_root_pop(heap, &shared_soft), "pop shared soft root");
    expect_ok(nggc_root_pop(heap, &phantom_ref),
              "pop live phantom root");
    expect_ok(nggc_root_pop(heap, &phantom_referent),
              "pop live phantom referent root");
    expect_ok(nggc_root_pop(heap, &weak_ref), "pop weak-only root");
    expect_ok(nggc_root_pop(heap, &weak_referent),
              "pop weak-only referent root");
    expect_ok(nggc_root_pop(heap, &soft_ref), "pop soft-only root");
    expect_ok(nggc_collect(heap), "reference verifier final collection");
    expect_ok(nggc_check(heap), "reference verifier final check");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "reference final stats");
    expect_true(stats.live_object_count == 0u,
                "reference verifier reclaimed all objects");
    expect_ok(nggc_close(heap), "reference verifier heap close");
    return 0;
}
