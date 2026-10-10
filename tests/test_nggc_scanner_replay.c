#include "nggc/nggc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct replay_node {
    struct replay_node *left;
    struct replay_node *right;
    uint32_t value;
} replay_node;

enum {
    REPLAY_SCAN_VALID = 0u,
    REPLAY_SCAN_REORDER = 1u,
    REPLAY_SCAN_DUPLICATE = 2u,
    REPLAY_SCAN_UNSTABLE = 3u,
    REPLAY_SCAN_MUTATE = 4u,
    REPLAY_SCAN_OUTSIDE = 5u,
    REPLAY_SCAN_FOREIGN = 6u,
    REPLAY_SCAN_UNALIGNED = 7u
};

static uint32_t replay_scan_mode;
static size_t replay_scan_invocation;
static void *replay_outside_slot;
static void **replay_foreign_slot;

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

static void scan_replay_node(void *object, nggc_visit_fn visit, void *ctx) {
    replay_node *node = (replay_node *)object;
    size_t invocation = replay_scan_invocation++;
    switch (replay_scan_mode) {
        case REPLAY_SCAN_VALID:
            visit(ctx, (void **)&node->left);
            visit(ctx, (void **)&node->right);
            break;
        case REPLAY_SCAN_REORDER:
            if ((invocation & 1u) == 0u) {
                visit(ctx, (void **)&node->left);
                visit(ctx, (void **)&node->right);
            } else {
                visit(ctx, (void **)&node->right);
                visit(ctx, (void **)&node->left);
            }
            break;
        case REPLAY_SCAN_DUPLICATE:
            visit(ctx, (void **)&node->left);
            visit(ctx, (void **)&node->left);
            break;
        case REPLAY_SCAN_UNSTABLE:
            visit(ctx, (invocation & 1u) == 0u
                           ? (void **)&node->left
                           : (void **)&node->right);
            break;
        case REPLAY_SCAN_MUTATE:
            node->left = node->right;
            visit(ctx, (void **)&node->left);
            break;
        case REPLAY_SCAN_OUTSIDE:
            visit(ctx, &replay_outside_slot);
            break;
        case REPLAY_SCAN_FOREIGN:
            visit(ctx, replay_foreign_slot);
            break;
        case REPLAY_SCAN_UNALIGNED:
            visit(ctx, (void **)((unsigned char *)node + 1u));
            break;
        default:
            break;
    }
}

static nggc_result run_check(nggc_heap *heap, uint32_t mode) {
    replay_scan_mode = mode;
    replay_scan_invocation = 0u;
    return nggc_check(heap);
}

int main(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    replay_node *node;
    void *weak_ref = 0;
    uint16_t type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "scanner replay heap open");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(replay_node);
    desc.scan = scan_replay_node;
    desc.name = "replay_node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "scanner replay type register");
    node = (replay_node *)nggc_alloc(heap, type_id);
    expect_true(node != 0, "scanner replay node allocation");
    node->left = node;
    node->right = 0;
    node->value = 7u;
    expect_ok(nggc_weak_ref_create(heap, node, &weak_ref),
              "scanner replay weak reference creation");

    expect_ok(run_check(heap, REPLAY_SCAN_VALID),
              "valid scanner accepted");
    expect_true(run_check(heap, REPLAY_SCAN_OUTSIDE) ==
                    NGGC_ERR_CORRUPTION,
                "scanner slot outside owner rejected");
    replay_foreign_slot = (void **)weak_ref;
    expect_true(run_check(heap, REPLAY_SCAN_FOREIGN) ==
                    NGGC_ERR_CORRUPTION,
                "scanner special-reference slot rejected");
    expect_true(run_check(heap, REPLAY_SCAN_UNALIGNED) ==
                    NGGC_ERR_CORRUPTION,
                "unaligned scanner slot rejected");

#if NGGC_ENABLE_VERIFY
    expect_ok(run_check(heap, REPLAY_SCAN_REORDER),
              "stable scanner set accepted in different order");
    expect_true(run_check(heap, REPLAY_SCAN_DUPLICATE) ==
                    NGGC_ERR_CORRUPTION,
                "duplicate scanner slot rejected");
    expect_true(run_check(heap, REPLAY_SCAN_UNSTABLE) ==
                    NGGC_ERR_CORRUPTION,
                "unstable scanner slot set rejected");
    node->left = node;
    expect_true(run_check(heap, REPLAY_SCAN_MUTATE) ==
                    NGGC_ERR_CORRUPTION,
                "scanner payload mutation rejected");
    node->left = node;
#endif

    expect_ok(run_check(heap, REPLAY_SCAN_VALID),
              "scanner replay heap repaired");
    expect_ok(nggc_close(heap), "scanner replay heap close");
    return 0;
}
