#include "nggc/nggc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct assist_node {
    struct assist_node *next;
    struct assist_node *side;
    size_t value;
} assist_node;

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
    assist_node *node = (assist_node *)object;
    visit(ctx, (void **)&node->next);
    visit(ctx, (void **)&node->side);
}

static void read_stats(nggc_heap *heap, nggc_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->size = sizeof(*stats);
    expect_ok(nggc_get_stats(heap, stats), "read assist stats");
}

static void run_thread_assist_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_thread *thread_a = 0;
    nggc_thread *thread_b = 0;
    nggc_stats stats;
    uint16_t type_id = NGGC_TYPE_NONE;
    size_t b_allocations = 0u;
    size_t a_attempts;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM;
    config.gc_arena_size = (size_t)128u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config.region_size = (size_t)16u * 1024u;
    config.tlab_size = (size_t)4u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open threaded assist heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(assist_node);
    desc.scan = scan_node;
    desc.name = "threaded_assist_node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register threaded assist node");
    expect_ok(nggc_thread_register(heap, &thread_a),
              "register first assist actor");
    expect_ok(nggc_thread_register(heap, &thread_b),
              "register second assist actor");

    while (b_allocations < 320u) {
        assist_node *node =
            (assist_node *)nggc_thread_alloc(heap, thread_b, type_id);
        if (!node) {
            fail("build second-thread assist debt");
        }
        memset(node, 0, sizeof(*node));
        ++b_allocations;
    }
    read_stats(heap, &stats);
    if (stats.mutator_assist_pending_thread_count == 0u ||
        stats.mutator_assist_max_thread_debt_bytes == 0u) {
        fail("per-thread assist debt was not published");
    }

    for (a_attempts = 0u; a_attempts < 1024u; ++a_attempts) {
        assist_node *node =
            (assist_node *)nggc_thread_alloc(heap, thread_a, type_id);
        if (node) {
            memset(node, 0, sizeof(*node));
        }
        read_stats(heap, &stats);
        if (stats.mutator_assist_fairness_cap_count != 0u) {
            break;
        }
    }
    if (stats.mutator_assist_fairness_cap_count == 0u ||
        stats.mutator_assist_dynamic_budget_count == 0u ||
        stats.mutator_assist_current_budget_items > 32u ||
        stats.mutator_assist_current_budget_bytes > 8192u ||
        stats.mutator_assist_thread_packet_count == 0u ||
        stats.mutator_assist_thread_progress_count == 0u) {
        fail("threaded assist fairness or budget accounting failed");
    }
    expect_ok(nggc_check(heap), "check threaded assist heap");
    expect_ok(nggc_thread_unregister(heap, thread_b),
              "unregister second assist actor");
    thread_b = 0;
    expect_ok(nggc_thread_unregister(heap, thread_a),
              "unregister first assist actor");
    thread_a = 0;
    expect_ok(nggc_close(heap), "close threaded assist heap");
}

int main(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    assist_node *root;
    assist_node *tail;
    assist_node *orphan;
    assist_node *node;
    uint16_t type_id = NGGC_TYPE_NONE;
    size_t count = 0u;
    size_t attempts = 0u;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM;
    config.gc_arena_size = 8192u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open assist heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(assist_node);
    desc.scan = scan_node;
    desc.name = "assist_node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register assist node");

    orphan = (assist_node *)nggc_alloc(heap, type_id);
    root = (assist_node *)nggc_alloc(heap, type_id);
    if (!orphan || !root) {
        fail("initial assist allocations");
    }
    memset(orphan, 0, sizeof(*orphan));
    memset(root, 0, sizeof(*root));
    orphan->value = 0x51a7u;
    expect_ok(nggc_root_push(heap, (void **)&root), "root assist graph");
    tail = root;
    count = 2u;

    for (;;) {
        node = (assist_node *)nggc_alloc(heap, type_id);
        if (!node) {
            break;
        }
        memset(node, 0, sizeof(*node));
        expect_ok(NGGC_STORE(heap, tail, &tail->next, node),
                  "extend assist graph");
        tail = node;
        ++count;
    }
    if (count <= 16u) {
        fail("assist heap did not build a useful mark backlog");
    }

    read_stats(heap, &stats);
    if (stats.mutator_assist_state != NGGC_MUTATOR_ASSIST_STATE_MARK ||
        stats.mutator_assist_mark_backlog_items == 0u ||
        stats.mutator_assist_current_budget_items < 8u ||
        stats.mutator_assist_current_budget_items > 32u ||
        stats.mutator_assist_mark_items_processed >
            stats.mutator_assist_packet_count * 32u) {
        fail("first assist packet was not bounded MARK work");
    }

    expect_ok(NGGC_STORE(heap, root, &root->side, orphan),
              "incremental-update barrier marks new edge");
    expect_ok(NGGC_STORE(heap, root, &root->next, 0),
              "make the old tail reclaimable");

    while (attempts++ < 512u) {
        node = (assist_node *)nggc_alloc(heap, type_id);
        read_stats(heap, &stats);
        if (node) {
            memset(node, 0, sizeof(*node));
            node->value = attempts;
            expect_ok(NGGC_STORE(heap, root, &root->next, node),
                      "retain allocation during assist");
        }
        if (stats.mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_IDLE &&
            stats.collection_count != 0u) {
            break;
        }
    }
    if (stats.mutator_assist_state != NGGC_MUTATOR_ASSIST_STATE_IDLE) {
        fail("assist cycle did not return to IDLE");
    }
    if (stats.mutator_assist_packet_count < 3u ||
        stats.mutator_assist_cycle_count == 0u ||
        stats.mutator_assist_progress_count == 0u ||
        stats.mutator_assist_sweep_bytes_processed == 0u) {
        fail("assist progress statistics incomplete");
    }
    if (stats.mutator_assist_mark_items_processed >
        stats.mutator_assist_packet_count * 8u) {
        fail("assist MARK packet exceeded item budget");
    }
    if (root->side != orphan || orphan->value != 0x51a7u) {
        fail("MARK write barrier failed to preserve new edge");
    }
    expect_ok(nggc_check(heap), "verify completed assist heap");

    node = (assist_node *)nggc_alloc(heap, type_id);
    while (node) {
        memset(node, 0, sizeof(*node));
        node = (assist_node *)nggc_alloc(heap, type_id);
    }
    read_stats(heap, &stats);
    if (stats.mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_IDLE) {
        fail("second assist cycle did not start");
    }
    expect_ok(nggc_collect(heap), "full collection takes over assist");
    read_stats(heap, &stats);
    if (stats.mutator_assist_state != NGGC_MUTATOR_ASSIST_STATE_IDLE ||
        stats.mutator_assist_mark_backlog_items != 0u) {
        fail("full collection left assist state behind");
    }

    expect_ok(nggc_close(heap), "close assist heap");
    run_thread_assist_case();
    return 0;
}
