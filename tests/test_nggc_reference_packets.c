#include "nggc/nggc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct packet_node {
    void *left;
    void *right;
} packet_node;

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

static void scan_packet_node(void *object, nggc_visit_fn visit, void *ctx) {
    packet_node *node = (packet_node *)object;
    visit(ctx, &node->left);
    visit(ctx, &node->right);
}

static void read_stats(nggc_heap *heap, nggc_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->size = sizeof(*stats);
    expect_ok(nggc_get_stats(heap, stats), "read reference packet stats");
}

static void run_reference_packet_takeover_case(void) {
    enum { reference_count = 96 };
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    void *references[reference_count];
    void *object;
    uint16_t type_id = NGGC_TYPE_NONE;
    size_t filled = 0u;
    uint64_t soft_discovered_before_takeover = 0u;
    uint64_t reference_processes_before_takeover = 0u;
    int saw_reference_process = 0;

    memset(references, 0, sizeof(references));
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM;
    config.soft_reference_policy = NGGC_SOFT_REFERENCE_POLICY_ALWAYS_CLEAR;
    config.gc_arena_size = (size_t)64u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open reference packet takeover heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(packet_node);
    desc.scan = scan_packet_node;
    desc.name = "reference-packet-takeover-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register reference packet takeover type");

    for (size_t i = 0u; i < reference_count; ++i) {
        expect_ok(nggc_soft_ref_create(heap, 0, &references[i]),
                  "create takeover soft reference");
        expect_ok(nggc_root_push(heap, &references[i]),
                  "root takeover soft reference");
    }
    while ((object = nggc_alloc(heap, type_id)) != 0) {
        memset(object, 0, sizeof(packet_node));
        ++filled;
    }
    expect_true(filled > 32u, "takeover heap did not fill");

    for (size_t attempts = 0u; attempts < 512u; ++attempts) {
        object = nggc_alloc(heap, type_id);
        if (object) {
            memset(object, 0, sizeof(packet_node));
        }
        read_stats(heap, &stats);
        if (stats.mutator_assist_state ==
            NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) {
            saw_reference_process = 1;
            expect_true(stats.reference_discovered_state ==
                            NGGC_REFERENCE_DISCOVERY_DRAINING,
                        "takeover did not observe draining discovery state");
            expect_true(stats.reference_process_pending_items != 0u,
                        "takeover had no pending reference work");
            expect_ok(nggc_check(heap),
                      "check before reference packet takeover");
            soft_discovered_before_takeover =
                stats.soft_reference_discovered_count;
            reference_processes_before_takeover =
                stats.reference_process_count;

            /* Full collection must discard the partial packet and restart. */
            expect_ok(nggc_collect(heap),
                      "full collection takes over reference packet");
            read_stats(heap, &stats);
            expect_true(stats.mutator_assist_state ==
                            NGGC_MUTATOR_ASSIST_STATE_IDLE,
                        "reference packet takeover left Assist active");
            expect_true(stats.reference_discovered_state ==
                            NGGC_REFERENCE_DISCOVERY_IDLE &&
                            stats.reference_process_pending_items == 0u,
                        "reference packet takeover left cursor state");
            expect_true(stats.soft_reference_discovered_count >=
                            soft_discovered_before_takeover +
                                (uint64_t)reference_count &&
                            stats.reference_process_count >
                                reference_processes_before_takeover,
                        "takeover did not restart reference discovery");
            expect_ok(nggc_check(heap),
                      "check after reference packet takeover");
            break;
        }
    }
    expect_true(saw_reference_process,
                "takeover case never exposed reference processing");

    for (size_t i = reference_count; i != 0u; --i) {
        expect_ok(nggc_root_pop(heap, &references[i - 1u]),
                  "unroot takeover soft reference");
    }
    expect_ok(nggc_collect(heap), "final takeover collection");
    expect_ok(nggc_check(heap), "final takeover heap check");
    expect_ok(nggc_close(heap), "close reference packet takeover heap");
}

int main(void) {
    enum { reference_count = 80, phantom_count = 40 };
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    void *references[reference_count];
    void *phantom_references[phantom_count];
    void *phantom_targets[phantom_count];
    void *phantom_queue = 0;
    void *object;
    uint16_t type_id = NGGC_TYPE_NONE;
    size_t filled = 0u;
    size_t attempts;
    int saw_reference_packet = 0;
    int saw_pending_packet = 0;

    memset(references, 0, sizeof(references));
    memset(phantom_references, 0, sizeof(phantom_references));
    memset(phantom_targets, 0, sizeof(phantom_targets));
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM;
    config.soft_reference_policy = NGGC_SOFT_REFERENCE_POLICY_ALWAYS_CLEAR;
    config.gc_arena_size = (size_t)64u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open reference packet heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(packet_node);
    desc.scan = scan_packet_node;
    desc.name = "reference-packet-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register reference packet type");

    expect_ok(nggc_phantom_queue_create(heap, &phantom_queue),
              "create reference packet phantom queue");
    for (size_t i = 0u; i < phantom_count; ++i) {
        phantom_targets[i] = nggc_alloc(heap, type_id);
        expect_true(phantom_targets[i] != 0,
                    "allocate reference packet phantom target");
        memset(phantom_targets[i], 0, sizeof(packet_node));
        expect_ok(nggc_phantom_ref_create(
                      heap, phantom_targets[i], phantom_queue,
                      &phantom_references[i]),
                  "create reference packet phantom reference");
        expect_ok(nggc_root_push(heap, &phantom_references[i]),
                  "root reference packet phantom reference");
    }
    for (size_t i = 0u; i < reference_count; ++i) {
        expect_ok(nggc_soft_ref_create(heap, 0, &references[i]),
                  "create reference packet soft reference");
        expect_ok(nggc_root_push(heap, &references[i]),
                  "root reference packet soft reference");
    }
    while ((object = nggc_alloc(heap, type_id)) != 0) {
        memset(object, 0, sizeof(packet_node));
        ++filled;
    }
    expect_true(filled > 32u, "reference packet heap did not fill");

    for (attempts = 0u; attempts < 512u; ++attempts) {
        object = nggc_alloc(heap, type_id);
        if (object) {
            memset(object, 0, sizeof(packet_node));
        }
        read_stats(heap, &stats);
        if (stats.mutator_assist_state ==
                NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) {
            saw_reference_packet = 1;
            if (stats.reference_process_pending_items != 0u) {
                saw_pending_packet = 1;
            }
            expect_true(stats.reference_discovered_state ==
                            NGGC_REFERENCE_DISCOVERY_DRAINING,
                        "reference packet did not retain draining state");
            expect_true(stats.reference_process_pending_items <=
                            stats.reference_process_items_processed +
                                reference_count + 1u,
                        "reference packet pending work overflowed bounds");
            expect_ok(nggc_check(heap),
                      "check partial reference processing state");
        }
        if (stats.mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_IDLE &&
            stats.collection_count != 0u) {
            break;
        }
    }

    expect_true(saw_reference_packet,
                "assist never exposed reference processing state");
    expect_true(saw_pending_packet,
                "reference processing packet had no pending work");
    expect_true(stats.mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_IDLE,
                "reference packet assist did not complete");
    expect_true(stats.reference_process_packet_count > 1u,
                "reference processing was not split into packets");
    expect_true(stats.reference_process_items_processed >= reference_count,
                "reference packet item accounting incomplete");
    expect_true(stats.soft_reference_discovered_count == reference_count,
                "soft references were not all processed");
    expect_true(stats.phantom_reference_discovered_count == phantom_count &&
                    stats.phantom_reference_enqueued_count == phantom_count,
                "phantom references were not all processed");
    expect_true(stats.reference_discovered_state ==
                    NGGC_REFERENCE_DISCOVERY_IDLE &&
                    stats.reference_process_pending_items == 0u,
                "reference packet state was not finalized");

    {
        size_t pending = 0u;
        expect_ok(nggc_phantom_queue_count(heap, phantom_queue, &pending),
                  "count reference packet phantom queue");
        expect_true(pending == phantom_count,
                    "phantom queue was not retained and filled");
        for (size_t i = 0u; i < phantom_count; ++i) {
            void *popped = 0;
            expect_ok(nggc_phantom_queue_pop(heap, phantom_queue, &popped),
                      "drain reference packet phantom queue");
            expect_true(popped == phantom_references[i],
                        "phantom queue order changed during packets");
        }
    }
    for (size_t i = reference_count; i != 0u; --i) {
        expect_ok(nggc_root_pop(heap, &references[i - 1u]),
                  "unroot reference packet soft reference");
    }
    for (size_t i = phantom_count; i != 0u; --i) {
        expect_ok(nggc_root_pop(heap, &phantom_references[i - 1u]),
                  "unroot reference packet phantom reference");
    }
    expect_ok(nggc_collect(heap), "collect after reference packet test");
    expect_ok(nggc_check(heap), "final reference packet heap check");
    expect_ok(nggc_close(heap), "close reference packet heap");
    run_reference_packet_takeover_case();
    return 0;
}
