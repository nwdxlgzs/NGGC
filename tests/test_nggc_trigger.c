#include "nggc/nggc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct trigger_node {
    struct trigger_node *next;
    size_t value;
} trigger_node;

typedef struct trigger_clock_ctx {
    uint64_t now_ns;
    uint64_t step_ns;
    uint32_t fail;
} trigger_clock_ctx;

static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void expect_true(int condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

static void expect_ok(nggc_result result, const char *message) {
    if (result != NGGC_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static void scan_trigger_node(void *object, nggc_visit_fn visit, void *ctx) {
    trigger_node *node = (trigger_node *)object;
    visit(ctx, (void **)&node->next);
}

static nggc_result trigger_clock_ns(void *context, uint64_t *out_ns) {
    trigger_clock_ctx *ctx = (trigger_clock_ctx *)context;
    if (!ctx || !out_ns || ctx->fail ||
        ctx->now_ns > UINT64_MAX - ctx->step_ns) {
        return NGGC_ERR_BACKEND;
    }
    ctx->now_ns += ctx->step_ns;
    *out_ns = ctx->now_ns;
    return NGGC_OK;
}

static void init_config(nggc_config *config, uint32_t policy) {
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->abi_major = NGGC_ABI_MAJOR;
    config->abi_minor = NGGC_ABI_MINOR;
    config->flags = NGGC_CONFIG_COLLECT_AUTO_TRIGGER;
    config->gc_arena_size = (size_t)128u * 1024u;
    config->uhm_arena_size = (size_t)2u * 1024u * 1024u;
    config->region_size = (size_t)8u * 1024u;
    config->gc_trigger_policy = policy;
    config->gc_trigger_soft_limit = (size_t)16u * 1024u;
    config->gc_trigger_quantum = (size_t)4u * 1024u;
}

static void run_smart_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    trigger_node *root;
    uint16_t type_id = NGGC_TYPE_NONE;
    size_t allocations = 0u;

    memset(&stats, 0, sizeof(stats));

    init_config(&config, NGGC_TRIGGER_SMART);
    expect_ok(nggc_open(&config, &heap), "open SMART trigger heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(trigger_node);
    desc.scan = scan_trigger_node;
    desc.name = "trigger_node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register trigger type");

    root = (trigger_node *)nggc_alloc(heap, type_id);
    expect_true(root != 0, "allocate trigger root");
    memset(root, 0, sizeof(*root));
    expect_ok(nggc_root_push(heap, (void **)&root), "root trigger node");

    while (allocations < 256u) {
        trigger_node *orphan = (trigger_node *)nggc_alloc(heap, type_id);
        if (!orphan) {
            fail("SMART trigger allocation failed");
        }
        memset(orphan, 0, sizeof(*orphan));
        orphan->value = allocations;
        ++allocations;
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(heap, &stats), "read SMART trigger stats");
        if (stats.gc_trigger_collection_count != 0u) {
            break;
        }
    }
    expect_true(stats.gc_trigger_collection_count == 1u,
                "SMART trigger did not request exactly one collection");
    expect_true(stats.gc_trigger_request_count == 1u &&
                    stats.gc_trigger_last_action == NGGC_TRIGGER_ACTION_FULL &&
                    stats.gc_trigger_last_reason !=
                        NGGC_TRIGGER_REASON_NONE,
                "SMART trigger decision telemetry is incomplete");
    expect_true(stats.live_object_count < allocations + 1u,
                "SMART trigger did not reclaim orphan objects");
    expect_true(root->next == 0, "SMART trigger damaged rooted object");
    expect_ok(nggc_check(heap), "check SMART trigger heap");

    expect_ok(nggc_root_pop(heap, (void **)&root), "unroot trigger node");
    expect_ok(nggc_close(heap), "close SMART trigger heap");
}

static void run_manual_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    uint16_t type_id = NGGC_TYPE_NONE;

    init_config(&config, NGGC_TRIGGER_MANUAL);
    expect_ok(nggc_open(&config, &heap), "open MANUAL trigger heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(trigger_node);
    desc.scan = scan_trigger_node;
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register MANUAL trigger type");
    expect_true(nggc_alloc(heap, type_id) != 0,
                "allocate MANUAL trigger object");
    expect_ok(nggc_collect_if_needed(heap, (size_t)64u * 1024u),
              "manual trigger evaluation");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read MANUAL trigger stats");
    expect_true(stats.gc_trigger_policy == NGGC_TRIGGER_MANUAL &&
                    stats.gc_trigger_collection_count == 0u &&
                    stats.gc_trigger_request_count == 0u &&
                    stats.gc_trigger_noop_count != 0u,
                "MANUAL trigger was not a no-op");
    expect_ok(nggc_close(heap), "close MANUAL trigger heap");
}

static void run_latency_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    nggc_stats stats;
    trigger_clock_ctx clock_ctx;
    trigger_node *root;
    uint16_t type_id = NGGC_TYPE_NONE;

    memset(&clock_ctx, 0, sizeof(clock_ctx));
    clock_ctx.step_ns = 10u;
    init_config(&config, (size_t)128u * 1024u);
    config.gc_trigger_policy = NGGC_TRIGGER_LATENCY_TARGET;
    config.gc_trigger_latency_target_ns = 1u;
    config.clock_ns = trigger_clock_ns;
    config.clock_ns_ctx = &clock_ctx;
    expect_ok(nggc_open(&config, &heap), "open LATENCY trigger heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(trigger_node);
    desc.scan = scan_trigger_node;
    desc.name = "latency_trigger_node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register LATENCY trigger type");
    root = (trigger_node *)nggc_alloc(heap, type_id);
    expect_true(root != 0, "allocate LATENCY trigger root");
    memset(root, 0, sizeof(*root));
    expect_ok(nggc_root_push(heap, (void **)&root),
              "root LATENCY trigger node");
    expect_true(nggc_alloc_size(heap, type_id, (size_t)32u * 1024u) != 0,
                "allocate LATENCY trigger pressure");
    expect_ok(nggc_collect_if_needed(heap, (size_t)64u * 1024u),
              "LATENCY trigger collection");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read LATENCY trigger stats");
    expect_true(stats.gc_trigger_policy == NGGC_TRIGGER_LATENCY_TARGET &&
                    stats.gc_trigger_collection_count != 0u &&
                    stats.gc_trigger_pause_sample_count ==
                        stats.gc_trigger_collection_count &&
                    stats.gc_trigger_last_pause_ns != 0u &&
                    stats.gc_trigger_pause_ewma_ns != 0u &&
                    stats.gc_trigger_last_reason == NGGC_TRIGGER_REASON_LATENCY,
                "LATENCY trigger pause feedback is incomplete");

    clock_ctx.fail = 1u;
    expect_ok(nggc_collect(heap),
              "clock failure must not block collection");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read LATENCY clock failure stats");
    expect_true(stats.gc_trigger_clock_failure_count != 0u,
                "LATENCY clock failure was not recorded");

    expect_ok(nggc_root_pop(heap, (void **)&root),
              "unroot LATENCY trigger node");
    expect_ok(nggc_close(heap), "close LATENCY trigger heap");
}

static void run_config_validation_case(void) {
    nggc_config config;
    nggc_heap *heap = 0;

    init_config(&config, 99u);
    expect_true(nggc_open(&config, &heap) == NGGC_ERR_INVALID_ARGUMENT,
                "invalid trigger policy was accepted");
    init_config(&config, NGGC_TRIGGER_SMART);
    config.gc_trigger_soft_limit = config.gc_arena_size + 1u;
    expect_true(nggc_open(&config, &heap) == NGGC_ERR_INVALID_ARGUMENT,
                "oversized trigger soft limit was accepted");
}

int main(void) {
    run_smart_case();
    run_manual_case();
    run_latency_case();
    run_config_validation_case();
    return 0;
}
