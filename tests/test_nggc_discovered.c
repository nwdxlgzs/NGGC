#include "nggc/nggc.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct discovered_node {
    void *left;
    void *right;
} discovered_node;

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

static void scan_discovered_node(void *object, nggc_visit_fn visit,
                                 void *ctx) {
    discovered_node *node = (discovered_node *)object;
    visit(ctx, &node->left);
    visit(ctx, &node->right);
}

int main(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    uint16_t type_id = NGGC_TYPE_NONE;
    discovered_node *first_target;
    discovered_node *nested_target;
    discovered_node *carrier;
    discovered_node *dead_weak_target;
    discovered_node *dead_phantom_target;
    void *root_soft = 0;
    void *nested_soft = 0;
    void *weak = 0;
    void *queue = 0;
    void *phantom = 0;
    void *value = 0;
    void *popped = 0;
    size_t pending = 0u;
    uint32_t state = UINT32_MAX;
    nggc_result fail_stop_reason = NGGC_OK;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.soft_reference_policy =
        NGGC_SOFT_REFERENCE_POLICY_ALWAYS_RETAIN;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open discovered heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(discovered_node);
    desc.scan = scan_discovered_node;
    desc.name = "discovered-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register discovered type");

    first_target = (discovered_node *)nggc_alloc(heap, type_id);
    nested_target = (discovered_node *)nggc_alloc(heap, type_id);
    carrier = (discovered_node *)nggc_alloc(heap, type_id);
    dead_weak_target = (discovered_node *)nggc_alloc(heap, type_id);
    dead_phantom_target = (discovered_node *)nggc_alloc(heap, type_id);
    expect_true(first_target && nested_target && carrier &&
                    dead_weak_target && dead_phantom_target,
                "allocate discovered graph");
    memset(first_target, 0, sizeof(*first_target));
    memset(nested_target, 0, sizeof(*nested_target));
    memset(carrier, 0, sizeof(*carrier));
    memset(dead_weak_target, 0, sizeof(*dead_weak_target));
    memset(dead_phantom_target, 0, sizeof(*dead_phantom_target));

    expect_ok(nggc_weak_ref_create(heap, dead_weak_target, &weak),
              "create discovered weak reference");
    expect_ok(nggc_phantom_queue_create(heap, &queue),
              "create discovered phantom queue");
    expect_ok(nggc_phantom_ref_create(heap, dead_phantom_target, queue,
                                      &phantom),
              "create discovered phantom reference");
    carrier->left = weak;
    carrier->right = phantom;

    expect_ok(nggc_soft_ref_create(heap, carrier, &nested_soft),
              "create nested soft reference");
    first_target->left = nested_soft;
    expect_ok(nggc_soft_ref_create(heap, first_target, &root_soft),
              "create root soft reference");
    expect_ok(nggc_root_push(heap, &root_soft), "root first soft reference");

    expect_ok(nggc_collect(heap), "collect discovered graph");
    expect_ok(nggc_check(heap), "check discovered graph");
    expect_ok(nggc_soft_ref_get(heap, root_soft, &value),
              "read first retained soft referent");
    expect_true(value == first_target, "first soft referent retained");
    expect_ok(nggc_soft_ref_get(heap, nested_soft, &value),
              "read nested retained soft referent");
    expect_true(value == carrier, "nested soft referent retained");
    expect_ok(nggc_weak_ref_get(heap, weak, &value),
              "read discovered weak referent");
    expect_true(value == 0, "unreachable weak referent cleared");
    expect_ok(nggc_phantom_queue_count(heap, queue, &pending),
              "count discovered phantom queue");
    expect_true(pending == 1u, "unreachable phantom reference enqueued");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read discovered stats");
    expect_true(stats.reference_discovered_state ==
                    NGGC_REFERENCE_DISCOVERY_IDLE &&
                    stats.reference_discovered_epoch == 0u &&
                    stats.reference_discovered_soft_count == 0u &&
                    stats.reference_discovered_weak_count == 0u &&
                    stats.reference_discovered_phantom_count == 0u,
                "discovered lists drained after collection");
    expect_true(stats.soft_reference_discovered_count == 2u &&
                    stats.soft_reference_retained_count == 2u &&
                    stats.weak_reference_discovered_count == 1u &&
                    stats.weak_reference_cleared_count == 1u &&
                    stats.phantom_reference_discovered_count == 1u &&
                    stats.phantom_reference_enqueued_count == 1u &&
                    stats.phantom_queue_promoted_count == 1u,
                "nested discovered counters");

    expect_ok(nggc_phantom_queue_pop(heap, queue, &popped),
              "pop discovered phantom reference");
    expect_true(popped == phantom, "pop expected phantom reference");
    expect_ok(nggc_root_pop(heap, &root_soft), "unroot first soft reference");
    expect_ok(nggc_collect(heap), "collect discovered graph after release");
    expect_ok(nggc_get_heap_state(heap, &state, &fail_stop_reason),
              "read discovered heap state");
    expect_true(state == NGGC_HEAP_STATE_RUNNING,
                "discovered heap remains running");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read final discovered stats");
    expect_true(stats.live_object_count == 0u,
                "discovered graph reclaimed after root release");
    expect_ok(nggc_close(heap), "close discovered heap");
    return 0;
}
