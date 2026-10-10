#include "nggc/nggc.h"

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct scanner_ex_node {
    struct scanner_ex_node *edge;
    uint32_t value;
} scanner_ex_node;

static atomic_uint scan_ex_calls;
static atomic_uint scan_ex_poll_calls;
static int scan_ex_cancel;

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

static void scan_legacy_node(void *object, nggc_visit_fn visit, void *ctx) {
    scanner_ex_node *node = (scanner_ex_node *)object;
    visit(ctx, (void **)&node->edge);
}

static nggc_result scan_ex_node(void *object, nggc_visit_fn visit, void *ctx,
                                const nggc_scan_control *control) {
    scanner_ex_node *node = (scanner_ex_node *)object;
    nggc_result result;

    if (!control || control->size < sizeof(*control) || !control->poll) {
        return NGGC_ERR_CORRUPTION;
    }
    atomic_fetch_add_explicit(&scan_ex_calls, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&scan_ex_poll_calls, 1u, memory_order_relaxed);
    result = control->poll(control->ctx);
    if (result != NGGC_OK) {
        return result;
    }
    if (scan_ex_cancel) {
        return NGGC_ERR_BUSY;
    }
    visit(ctx, (void **)&node->edge);
    return NGGC_OK;
}

int main(void) {
    nggc_config config;
    nggc_type_desc legacy_desc;
    nggc_type_desc extended_desc;
    nggc_heap *heap = 0;
    scanner_ex_node *legacy_node;
    scanner_ex_node *extended_node;
    void *legacy_root;
    void *extended_root;
    uint16_t legacy_type = NGGC_TYPE_NONE;
    uint16_t extended_type = NGGC_TYPE_NONE;

    atomic_init(&scan_ex_calls, 0u);
    atomic_init(&scan_ex_poll_calls, 0u);
    scan_ex_cancel = 0;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open scanner ex heap");

    memset(&legacy_desc, 0, sizeof(legacy_desc));
    legacy_desc.size = (uint32_t)offsetof(nggc_type_desc, scan_ex);
    legacy_desc.object_size = sizeof(scanner_ex_node);
    legacy_desc.scan = scan_legacy_node;
    legacy_desc.name = "legacy-scanner-node";
    expect_ok(nggc_register_type(heap, &legacy_desc, &legacy_type),
              "register legacy-sized scanner descriptor");

    memset(&extended_desc, 0, sizeof(extended_desc));
    extended_desc.size = sizeof(extended_desc);
    extended_desc.object_size = sizeof(scanner_ex_node);
    extended_desc.scan_ex = scan_ex_node;
    extended_desc.name = "extended-scanner-node";
    expect_ok(nggc_register_type(heap, &extended_desc, &extended_type),
              "register extended scanner descriptor");

    legacy_node = (scanner_ex_node *)nggc_alloc(heap, legacy_type);
    extended_node = (scanner_ex_node *)nggc_alloc(heap, extended_type);
    expect_true(legacy_node != 0 && extended_node != 0,
                "allocate scanner nodes");
    memset(legacy_node, 0, sizeof(*legacy_node));
    memset(extended_node, 0, sizeof(*extended_node));
    legacy_node->edge = legacy_node;
    extended_node->edge = extended_node;
    legacy_node->value = 7u;
    extended_node->value = 11u;
    legacy_root = legacy_node;
    extended_root = extended_node;
    expect_ok(nggc_root_push(heap, &legacy_root), "root legacy scanner node");
    expect_ok(nggc_root_push(heap, &extended_root),
              "root extended scanner node");

    expect_ok(nggc_check(heap), "check extended scanner heap");
    expect_true(atomic_load_explicit(&scan_ex_calls, memory_order_relaxed) !=
                    0u &&
                    atomic_load_explicit(&scan_ex_poll_calls,
                                         memory_order_relaxed) != 0u,
                "extended scanner poll was called");
    expect_ok(nggc_collect(heap), "collect extended scanner heap");
    expect_true(legacy_root != 0 && extended_root != 0 &&
                    legacy_root == legacy_node &&
                    extended_root == extended_node,
                "legacy and extended scanner roots survive");

    scan_ex_cancel = 1;
    expect_true(nggc_check(heap) == NGGC_ERR_BUSY,
                "extended scanner cancellation reaches check");
    expect_true(nggc_collect(heap) == NGGC_ERR_BUSY,
                "extended scanner cancellation reaches collection");
    scan_ex_cancel = 0;
    expect_ok(nggc_check(heap), "heap recovers after scanner cancellation");

    expect_ok(nggc_root_pop(heap, &extended_root),
              "unroot extended scanner node");
    expect_ok(nggc_root_pop(heap, &legacy_root),
              "unroot legacy scanner node");
    expect_ok(nggc_collect(heap), "collect scanner test garbage");
    expect_ok(nggc_close(heap), "close scanner ex heap");
    return 0;
}
