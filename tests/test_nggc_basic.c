#include "nggc/nggc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define TEST_OBJECT_HEADER_BYTES 64u
#define TEST_OBJECT_MAGIC 0x4e474f42u

typedef struct test_node {
    struct test_node *left;
    struct test_node *right;
    int value;
} test_node;

typedef struct test_object_header {
    uint32_t magic;
    atomic_uint state;
    uint16_t type_id;
    uint16_t size_class;
    uint32_t flags;
    _Atomic(void *) forwarding;
    atomic_uint forwarding_epoch;
} test_object_header;

static void test_forwarding_set(test_object_header *header,
                                void *target) {
    if (target) {
        atomic_store_explicit(&header->forwarding_epoch, 1u,
                              memory_order_relaxed);
        atomic_store_explicit(&header->forwarding, target,
                              memory_order_release);
    } else {
        atomic_store_explicit(&header->forwarding, 0,
                              memory_order_release);
        atomic_store_explicit(&header->forwarding_epoch, 0u,
                              memory_order_relaxed);
    }
}

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

static void scan_test_node(void *object, nggc_visit_fn visit, void *ctx) {
    test_node *node = (test_node *)object;
    visit(ctx, (void **)&node->left);
    visit(ctx, (void **)&node->right);
}

int main(void) {
    nggc_heap *heap = 0;
    nggc_heap *small_heap = 0;
    nggc_heap *type_heap = 0;
    nggc_heap *corrupt_heap = 0;
    nggc_heap *auto_heap = 0;
    nggc_heap *owned_heap = 0;
    nggc_heap *split_heap = 0;
    nggc_heap *flag_heap = 0;
    nggc_heap *weak_heap = 0;
    nggc_heap *soft_heap = 0;
    nggc_heap *soft_clear_heap = 0;
    nggc_heap *soft_retain_heap = 0;
    nggc_heap *phantom_heap = 0;
    nggc_heap *reaper_heap = 0;
    nggc_heap *assist_heap = 0;
    nggc_thread *thread = 0;
    nggc_thread *thread2 = 0;
    nggc_thread *auto_thread = 0;
    nggc_thread *auto_thread2 = 0;
    nggc_thread *owned_thread = 0;
    nggc_thread *owned_peer = 0;
    nggc_config config;
    nggc_config small_config;
    nggc_config type_config;
    nggc_config corrupt_config;
    nggc_config auto_config;
    nggc_config split_config;
    nggc_config flag_config;
    nggc_config weak_config;
    nggc_config soft_config;
    nggc_config soft_clear_config;
    nggc_config soft_retain_config;
    nggc_config phantom_config;
    nggc_config reaper_config;
    nggc_config assist_config;
    nggc_type_desc node_desc;
    nggc_type_desc boundary_desc;
    nggc_type_desc flag_desc;
    nggc_stats stats;
    uint16_t node_type = NGGC_TYPE_NONE;
    uint16_t fixed_type = 123u;
    uint16_t max_type = NGGC_TYPE_NONE;
    uint16_t reusable_type = NGGC_TYPE_NONE;
    size_t pending_threads = 0;
    test_node *root;
    test_node *child;
    test_node *replacement;
    test_node *corrupt_node;
    test_node *frozen_node;
    test_node *reusable_node;
    test_node *large_reused;
    test_node *small_reuse;
    test_node *flagged_node;
    test_node *large_node;
    test_node *public_weak_referent;
    test_node *public_strong_referent;
    void *public_weak_ref;
    void *public_rooted_weak;
    void *public_weak_value;
    test_node *public_soft_referent;
    test_node *public_soft_pressure_referent;
    void *public_soft_ref;
    void *public_soft_value;
    test_node *public_phantom_referent;
    void *public_phantom_queue;
    void *public_phantom_ref;
    void *public_popped_phantom;
    size_t public_phantom_pending = 0;
    test_node *reaper_referent;
    void *reaper_empty_queue;
    void *reaper_queue;
    void *reaper_phantom_ref;
    void *reaper_seen_queue;
    void *reaper_popped_phantom;
    size_t reaper_pending = 0;
    test_node *assist_head;
    test_node *assist_tail;
    test_node *assist_next;
    test_node *assist_replacement;
    size_t assist_count = 0;
    size_t assist_retry_count = 0;
    test_node *forwarded_root_alias;
    test_object_header *child_header;
    uint32_t *corrupt_magic;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)2u * 1024u * 1024u;
    config.uhm_arena_size = (size_t)8u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open heap");
    expect_ok(nggc_thread_register(heap, &thread), "thread register");
    expect_ok(nggc_thread_register(heap, &thread2), "thread2 register");
    expect_true(nggc_close(heap) == NGGC_ERR_BUSY,
                "close rejects registered threads");
    expect_ok(nggc_safepoint_park(heap, thread),
              "park poll without request returns immediately");
    expect_ok(nggc_request_safepoint(heap), "request safepoint");
    expect_ok(nggc_safepoint(heap, thread), "thread safepoint");
    expect_true(nggc_safepoint_wait(heap, &pending_threads) == NGGC_ERR_BUSY,
                "safepoint wait busy");
    expect_true(pending_threads == 1u, "one pending thread");
    expect_true(nggc_safepoint_release(heap) == NGGC_ERR_BUSY,
                "cannot release partial safepoint");
    expect_true(nggc_thread_unregister(heap, thread) == NGGC_ERR_BUSY,
                "parked thread cannot unregister");
    expect_true(nggc_collect(heap) == NGGC_ERR_BUSY,
                "collect waits for safepoint completion");
    expect_ok(nggc_safepoint(heap, thread2), "thread2 safepoint");
    expect_ok(nggc_safepoint_wait(heap, &pending_threads),
              "safepoint wait complete");
    expect_true(pending_threads == 0u, "no pending threads");
    expect_true(nggc_root_push(heap, (void **)&root) == NGGC_ERR_BUSY,
                "global root mutation blocked while parked");
    expect_true(nggc_thread_root_push(heap, thread, (void **)&root) ==
                    NGGC_ERR_BUSY,
                "thread root mutation blocked while parked");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "parked safepoint stats");
    expect_true(stats.safepoint_active == 1u &&
                    stats.safepoint_parked_threads == 2u,
                "all threads remain parked before release");
    expect_ok(nggc_safepoint_release(heap), "release safepoint");
    expect_true(nggc_safepoint_release(heap) == NGGC_ERR_INVALID_ARGUMENT,
                "duplicate safepoint release rejected");

    memset(&node_desc, 0, sizeof(node_desc));
    node_desc.size = sizeof(node_desc);
    node_desc.object_size = sizeof(test_node);
    node_desc.alignment = 0;
    node_desc.scan = scan_test_node;
    node_desc.name = "test_node";
    expect_ok(nggc_register_type(heap, &node_desc, &node_type),
              "register node type");
    expect_true(node_type != NGGC_TYPE_NONE, "node type assigned");
    node_desc.type_id = fixed_type;
    expect_ok(nggc_register_type(heap, &node_desc, 0), "register fixed type");
    expect_true(nggc_register_type(heap, &node_desc, 0) ==
                    NGGC_ERR_TYPE_EXISTS,
                "duplicate fixed type rejected");

    root = 0;
    expect_ok(nggc_root_push(heap, (void **)&root), "push root");
    root = (test_node *)nggc_alloc(heap, node_type);
    child = (test_node *)nggc_alloc(heap, node_type);
    expect_true(root != 0 && child != 0, "allocate nodes");
    expect_ok(NGGC_STORE(heap, root, &root->left, child),
              "store root left");
    expect_ok(NGGC_STORE(heap, root, &root->right, 0),
              "store root right null");
    root->value = 7;
    child->left = 0;
    child->right = 0;
    child->value = 11;
    expect_true(NGGC_LOAD(heap, &root->left) == child,
                "load root left");

    expect_true(nggc_object_type(root) == node_type, "object type");
    expect_true(nggc_object_size(root) == sizeof(test_node), "object size");
    expect_ok(nggc_check(heap), "heap check");
    expect_ok(nggc_collect(heap), "collector keeps rooted graph");

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "stats");
    expect_true(stats.registered_type_count == 2u, "stats type count");
    expect_true(stats.live_object_count == 2u, "stats object count");
    expect_true(stats.live_payload_bytes == sizeof(test_node) * 2u,
                "stats live bytes");
    expect_true(stats.collection_count == 1u, "stats collection count");
    expect_true(stats.reference_process_count == 1u,
                "stats reference process count");
    expect_true(stats.mutator_assist_charged_bytes != 0u,
                "assist charged allocation bytes");
    expect_true(stats.mutator_assist_debt_bytes == 0u,
                "assist debt repaid after collect");
    expect_true(stats.mutator_assist_repaid_bytes ==
                    stats.mutator_assist_charged_bytes,
                "assist repaid charged bytes");
    expect_true(stats.mutator_assist_repay_count == 1u,
                "assist repay count");
    expect_true(stats.root_count == 1u, "stats root count");
    expect_true(stats.card_size == 512u, "stats card size");
    expect_true(stats.card_count != 0u, "stats card count");
    expect_true(stats.dirty_card_count == 1u, "stats dirty card count");
    expect_true(stats.registered_thread_count == 2u,
                "stats registered thread count");
    expect_true(stats.load_barrier_count == 1u, "stats load barrier count");
    expect_true(stats.store_barrier_count == 2u,
                "stats store barrier count");
    expect_true(stats.remset_record_count == 2u,
                "stats remset record count");
    expect_true(stats.safepoint_epoch == 1u, "stats safepoint epoch");
    expect_true(stats.safepoint_request_count == 1u,
                "stats safepoint request count");
    expect_true(stats.safepoint_poll_count == 3u,
                "stats safepoint poll count");
    expect_true(stats.safepoint_ack_count == 2u,
                "stats safepoint ack count");
    expect_true(stats.safepoint_wait_count == 2u,
                "stats safepoint wait count");
    expect_true(stats.safepoint_pending_threads == 0u,
                "stats safepoint pending count");
    expect_true(stats.safepoint_parked_threads == 0u &&
                    stats.safepoint_active == 0u &&
                    stats.safepoint_release_count == 1u &&
                    stats.safepoint_release_epoch == stats.safepoint_epoch,
                "stats safepoint released state");
    expect_true(stats.gc_phase == NGGC_PHASE_IDLE, "stats idle phase");
    expect_true(stats.read_barrier_active == 0u,
                "stats read barrier initially inactive");
    expect_ok(nggc_phase_begin(heap, NGGC_PHASE_ROOT_SNAPSHOT),
              "begin root snapshot phase");
    expect_true(nggc_phase_begin(heap, NGGC_PHASE_MARK) == NGGC_ERR_BUSY,
                "nested phase rejected");
    expect_ok(nggc_phase_end(heap, NGGC_PHASE_ROOT_SNAPSHOT),
              "end root snapshot phase");
    expect_ok(nggc_phase_begin(heap, NGGC_PHASE_REFERENCE_PROCESS),
              "begin reference process phase");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "stats during reference process");
    expect_true(stats.gc_phase == NGGC_PHASE_REFERENCE_PROCESS,
                "stats reference process phase");
    expect_true(stats.read_barrier_active == 0u,
                "reference process keeps read barrier inactive");
    expect_ok(nggc_phase_end(heap, NGGC_PHASE_REFERENCE_PROCESS),
              "end reference process phase");
    expect_ok(nggc_phase_begin(heap, NGGC_PHASE_REMAP),
              "begin remap phase");
    child_header =
        (test_object_header *)((uintptr_t)child - TEST_OBJECT_HEADER_BYTES);
    expect_true(child_header->magic == TEST_OBJECT_MAGIC,
                "child header magic before forwarding");
    forwarded_root_alias = child;
    expect_ok(nggc_root_push(heap, (void **)&forwarded_root_alias),
              "push forwarding alias root");
    expect_true(NGGC_LOAD(heap, &root->left) == child,
                "load through active read barrier");
    test_forwarding_set(child_header, root);
    expect_true(NGGC_LOAD(heap, &root->left) == root,
                "active read barrier follows forwarding");
    atomic_store_explicit(&child_header->forwarding_epoch, 0u,
                          memory_order_relaxed);
    expect_true(NGGC_LOAD(heap, &root->left) == 0 &&
                    nggc_get_last_error(heap) == NGGC_ERR_CORRUPTION,
                "read barrier rejects forwarding without epoch");
    atomic_store_explicit(&child_header->forwarding_epoch, 1u,
                          memory_order_relaxed);
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "stats during remap");
    expect_true(stats.gc_phase == NGGC_PHASE_REMAP, "stats remap phase");
    expect_true(stats.read_barrier_active == 1u,
                "stats read barrier active");
    expect_true(stats.read_barrier_active_load_count == 3u,
                "stats active read barrier load count");
    expect_ok(nggc_phase_end(heap, NGGC_PHASE_REMAP), "end remap phase");
    expect_true(root->left == root,
                "remap phase fixes forwarded object slot");
    expect_true(forwarded_root_alias == root,
                "remap phase fixes forwarded root slot");
    expect_ok(nggc_root_pop(heap, (void **)&forwarded_root_alias),
              "pop forwarding alias root");
    expect_ok(nggc_phase_begin(heap, NGGC_PHASE_REMAP),
              "begin residual forwarding verification phase");
    test_forwarding_set(child_header, root);
    forwarded_root_alias = child;
    expect_ok(nggc_root_push(heap, (void **)&forwarded_root_alias),
              "push stale forwarded root");
    expect_true(nggc_check(heap) == NGGC_ERR_CORRUPTION,
                "forwarded root slot detected");
    expect_ok(nggc_root_pop(heap, (void **)&forwarded_root_alias),
              "pop stale forwarded root");
    root->right = child;
    expect_true(nggc_check(heap) == NGGC_ERR_CORRUPTION,
                "forwarded strong slot detected");
    root->right = 0;
    expect_ok(nggc_check(heap), "heap check after forwarded slot repair");
    test_forwarding_set(child_header, (void *)0x1234);
    expect_true(nggc_check(heap) == NGGC_ERR_CORRUPTION,
                "bad forwarding target detected");
    test_forwarding_set(child_header, 0);
    atomic_store_explicit(&child_header->forwarding_epoch, 1u,
                          memory_order_relaxed);
    expect_true(nggc_check(heap) == NGGC_ERR_CORRUPTION,
                "stale forwarding epoch detected");
    atomic_store_explicit(&child_header->forwarding_epoch, 0u,
                          memory_order_relaxed);
    expect_ok(nggc_phase_end(heap, NGGC_PHASE_REMAP),
              "end residual forwarding verification phase");
    expect_true(stats.uhm_mmap_live_bytes + stats.uhm_decommitted_bytes >=
                    stats.gc_arena_size,
                "stats backed by UHMAS mmap/decommit");

    expect_true(NGGC_STORE(heap, child, &child->left, (void *)0x1234) ==
                    NGGC_ERR_CORRUPTION,
                "bad store rejected");
    child->left = (test_node *)0x1234;
    expect_true(nggc_check(heap) == NGGC_ERR_CORRUPTION,
                "bad reference detected");
    expect_ok(NGGC_STORE(heap, child, &child->left, 0),
              "repair child left");
    expect_ok(nggc_check(heap), "heap check after repair");

    expect_ok(NGGC_STORE(heap, root, &root->left, 0),
              "clear root left");
    expect_ok(nggc_collect(heap), "collector reclaims unreachable child");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "stats after reclaim");
    expect_true(stats.live_object_count == 1u, "one live after reclaim");
    expect_true(stats.free_object_count == 1u, "one free after reclaim");
    expect_true(stats.reclaimed_object_count == 1u, "one reclaimed object");
    expect_true(stats.dirty_card_count >= 1u,
                "dirty card retained after reclaim");

    replacement = (test_node *)nggc_alloc(heap, node_type);
    expect_true(replacement == child, "free block reused");
    replacement->left = 0;
    replacement->right = 0;
    replacement->value = 19;
    expect_ok(NGGC_STORE(heap, root, &root->right, replacement),
              "store replacement");
    expect_ok(nggc_check(heap), "heap check after reuse");

    expect_ok(nggc_root_pop(heap, (void **)&root), "pop root");
    expect_ok(nggc_collect(heap), "collector reclaims unrooted graph");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "stats after unrooted collect");
    expect_true(stats.live_object_count == 0u, "no live objects after pop");
    expect_true(stats.mutator_assist_debt_bytes == 0u,
                "assist debt remains repaid after later collect");
    expect_true(stats.mutator_assist_repaid_bytes ==
                    stats.mutator_assist_charged_bytes,
                "all assist debt repaid after later collect");
    expect_true(stats.root_count == 0u, "no roots after pop");
    expect_true(stats.remset_record_count == 5u,
                "all valid object stores recorded");
    expect_true(stats.phase_transition_count == 32u,
                "phase transitions recorded");
    expect_true(stats.read_barrier_active == 0u,
                "read barrier inactive after phases");
    expect_true(stats.read_barrier_active_load_count == 3u,
                "read barrier active load retained");

    expect_ok(nggc_thread_unregister(heap, thread), "thread unregister");
    thread = 0;
    expect_ok(nggc_thread_unregister(heap, thread2), "thread2 unregister");
    thread2 = 0;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "stats after unregister");
    expect_true(stats.registered_thread_count == 0u,
                "no registered threads after unregister");
    expect_ok(nggc_close(heap), "close heap");

    memset(&type_config, 0, sizeof(type_config));
    type_config.size = sizeof(type_config);
    type_config.abi_major = NGGC_ABI_MAJOR;
    type_config.abi_minor = NGGC_ABI_MINOR;
    type_config.gc_arena_size = (size_t)512u * 1024u;
    type_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&type_config, &type_heap), "type heap open");
    expect_true(nggc_register_type(type_heap, 0, 0) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "null type desc rejected");
    boundary_desc = node_desc;
    boundary_desc.alignment = 3u;
    expect_true(nggc_register_type(type_heap, &boundary_desc, 0) ==
                    NGGC_ERR_UNSUPPORTED,
                "non power-of-two alignment rejected");
    boundary_desc = node_desc;
    boundary_desc.flags = (uint16_t)(1u << 15);
    expect_true(nggc_register_type(type_heap, &boundary_desc, 0) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "unknown type flags rejected");
    boundary_desc = node_desc;
    boundary_desc.type_id = NGGC_TYPE_MAX;
    expect_ok(nggc_register_type(type_heap, &boundary_desc, &max_type),
              "max type id registered");
    expect_true(max_type == NGGC_TYPE_MAX, "max type id returned");
    expect_true(nggc_register_type(type_heap, &boundary_desc, 0) ==
                    NGGC_ERR_TYPE_EXISTS,
                "duplicate max type rejected");
    expect_true(nggc_unregister_type(type_heap, NGGC_TYPE_NONE) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "none type unregister rejected");
    expect_true(nggc_unregister_type(type_heap, 1u) ==
                    NGGC_ERR_TYPE_NOT_FOUND,
                "unregistered type unregister rejected");
    boundary_desc = node_desc;
    boundary_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(type_heap, &boundary_desc,
                                 &reusable_type),
              "reusable auto type registered");
    reusable_node = (test_node *)nggc_alloc(type_heap, reusable_type);
    expect_true(reusable_node != 0, "reusable type allocation");
    expect_true(nggc_unregister_type(type_heap, reusable_type) ==
                    NGGC_ERR_BUSY,
                "live type unregister rejected");
    expect_ok(nggc_collect(type_heap), "collect reusable type object");
    expect_ok(nggc_unregister_type(type_heap, reusable_type),
              "unused type unregistered");
    expect_true(nggc_alloc(type_heap, reusable_type) == 0,
                "unregistered reusable type allocation rejected");
    expect_true(nggc_get_last_error(type_heap) == NGGC_ERR_TYPE_NOT_FOUND,
                "unregistered reusable type allocation error");
    boundary_desc = node_desc;
    boundary_desc.type_id = NGGC_TYPE_NONE;
    max_type = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(type_heap, &boundary_desc, &max_type),
              "auto type id reused after unregister");
    expect_true(max_type == reusable_type, "reused type id matches");
    expect_ok(nggc_unregister_type(type_heap, reusable_type),
              "reused type unregistered");
    max_type = NGGC_TYPE_MAX;
    expect_true(nggc_alloc(type_heap, NGGC_TYPE_NONE) == 0,
                "none type allocation rejected");
    expect_true(nggc_get_last_error(type_heap) == NGGC_ERR_INVALID_ARGUMENT,
                "none type allocation error");
    expect_true(nggc_alloc(type_heap, 1u) == 0,
                "unregistered type allocation rejected");
    expect_true(nggc_get_last_error(type_heap) == NGGC_ERR_TYPE_NOT_FOUND,
                "unregistered type allocation error");
    expect_true(nggc_alloc_size(type_heap,
                                NGGC_TYPE_MAX,
                                sizeof(test_node) - 1u) == 0,
                "undersized payload rejected");
    expect_true(nggc_get_last_error(type_heap) == NGGC_ERR_INVALID_ARGUMENT,
                "undersized payload error");
    expect_ok(nggc_type_table_freeze(type_heap), "type table freeze");
    expect_ok(nggc_type_table_freeze(type_heap), "type table freeze idempotent");
    boundary_desc = node_desc;
    boundary_desc.type_id = 321u;
    expect_true(nggc_register_type(type_heap, &boundary_desc, 0) ==
                    NGGC_ERR_FROZEN,
                "frozen type table rejects registration");
    expect_true(nggc_get_last_error(type_heap) == NGGC_ERR_FROZEN,
                "frozen type table error");
    expect_true(nggc_unregister_type(type_heap, NGGC_TYPE_MAX) ==
                    NGGC_ERR_FROZEN,
                "frozen type table rejects unregister");
    frozen_node = (test_node *)nggc_alloc(type_heap, NGGC_TYPE_MAX);
    expect_true(frozen_node != 0,
                "frozen type table allows existing type allocation");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(type_heap, &stats), "type heap frozen stats");
    expect_true(stats.type_table_frozen == 1u,
                "type heap frozen stats flag");
    expect_true(stats.registered_type_count == 1u,
                "type heap frozen type count");
    expect_ok(nggc_check(type_heap), "type heap check");
    expect_ok(nggc_close(type_heap), "type heap close");

    memset(&flag_config, 0, sizeof(flag_config));
    flag_config.size = sizeof(flag_config);
    flag_config.abi_major = NGGC_ABI_MAJOR;
    flag_config.abi_minor = NGGC_ABI_MINOR;
    flag_config.gc_arena_size = (size_t)512u * 1024u;
    flag_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&flag_config, &flag_heap), "flag heap open");
    flag_desc = node_desc;
    flag_desc.type_id = NGGC_TYPE_NONE;
    flag_desc.flags = NGGC_TYPE_FLAG_PINNABLE | NGGC_TYPE_FLAG_NON_MOVABLE;
    expect_ok(nggc_register_type(flag_heap, &flag_desc, &node_type),
              "flag heap type");
    flagged_node = (test_node *)nggc_alloc(flag_heap, node_type);
    large_node = (test_node *)nggc_alloc_size(flag_heap,
                                              node_type,
                                              (size_t)96u * 1024u);
    expect_true(flagged_node != 0 && large_node != 0,
                "flag heap allocations");
    expect_true((nggc_object_flags(flagged_node) &
                 NGGC_OBJECT_FLAG_PINNABLE) != 0u,
                "pinnable object flag");
    expect_true((nggc_object_flags(flagged_node) &
                 NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u,
                "non-movable object flag");
    expect_true((nggc_object_flags(flagged_node) &
                 NGGC_OBJECT_FLAG_LARGE) == 0u,
                "small object not large");
    expect_true((nggc_object_flags(large_node) &
                 NGGC_OBJECT_FLAG_LARGE) != 0u,
                "large object flag");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(flag_heap, &stats), "flag heap stats");
    expect_true(stats.live_object_count == 2u,
                "flag heap live count");
    expect_true(stats.large_object_count == 1u,
                "flag heap large count");
    expect_true(stats.pinnable_object_count == 2u,
                "flag heap pinnable count");
    expect_true(stats.non_movable_object_count == 2u,
                "flag heap non-movable count");
    expect_ok(nggc_collect(flag_heap), "flag heap collect");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(flag_heap, &stats),
              "flag heap stats after collect");
    expect_true(stats.live_object_count == 0u,
                "flag heap live count after collect");
    expect_true(stats.large_object_count == 0u,
                "flag heap large count after collect");
    expect_true(stats.pinnable_object_count == 0u,
                "flag heap pinnable count after collect");
    expect_true(stats.non_movable_object_count == 0u,
                "flag heap non-movable count after collect");
    expect_ok(nggc_check(flag_heap), "flag heap check");
    expect_ok(nggc_close(flag_heap), "flag heap close");

    memset(&weak_config, 0, sizeof(weak_config));
    weak_config.size = sizeof(weak_config);
    weak_config.abi_major = NGGC_ABI_MAJOR;
    weak_config.abi_minor = NGGC_ABI_MINOR;
    weak_config.gc_arena_size = (size_t)512u * 1024u;
    weak_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&weak_config, &weak_heap), "weak heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(weak_heap, &node_desc, &node_type),
              "weak heap type");
    expect_true(nggc_weak_ref_create(weak_heap, 0, 0) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "weak ref create requires output");
    public_weak_ref = (void *)1;
    expect_true(nggc_weak_ref_create(weak_heap, (void *)0x1234,
                                     &public_weak_ref) ==
                    NGGC_ERR_CORRUPTION,
                "weak ref rejects invalid referent");
    expect_true(public_weak_ref == 0, "weak ref output cleared on failure");
    public_weak_value = (void *)1;
    expect_true(nggc_weak_ref_get(weak_heap, 0, &public_weak_value) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "weak ref get rejects null weak ref");
    expect_true(public_weak_value == 0,
                "weak ref get output cleared on failure");
    expect_ok(nggc_weak_ref_create(weak_heap, 0, &public_weak_ref),
              "weak ref allows null referent");
    public_weak_value = (void *)1;
    expect_ok(nggc_weak_ref_get(weak_heap, public_weak_ref,
                                &public_weak_value),
              "weak ref reads null referent");
    expect_true(public_weak_value == 0, "weak null referent value");

    public_weak_referent = (test_node *)nggc_alloc(weak_heap, node_type);
    expect_true(public_weak_referent != 0, "weak referent allocation");
    expect_ok(nggc_weak_ref_create(weak_heap, public_weak_referent,
                                   &public_weak_ref),
              "public weak ref create");
    expect_ok(nggc_weak_ref_get(weak_heap, public_weak_ref,
                                &public_weak_value),
              "public weak ref get before collect");
    expect_true(public_weak_value == public_weak_referent,
                "public weak referent before collect");
    expect_ok(nggc_root_push(weak_heap, &public_weak_ref),
              "public weak ref root");
    expect_ok(nggc_collect(weak_heap), "public weak clears referent");
    expect_ok(nggc_weak_ref_get(weak_heap, public_weak_ref,
                                &public_weak_value),
              "public weak ref get after clear");
    expect_true(public_weak_value == 0,
                "public weak referent cleared after collect");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(weak_heap, &stats),
              "weak heap stats after clear");
    expect_true(stats.live_object_count == 1u,
                "public weak ref remains live");
    expect_true(stats.weak_reference_discovered_count == 1u,
                "public weak discovered");
    expect_true(stats.weak_reference_cleared_count == 1u,
                "public weak cleared count");
    expect_ok(nggc_root_pop(weak_heap, &public_weak_ref),
              "public weak ref root pop");
    expect_ok(nggc_collect(weak_heap), "public weak object collect");

    public_strong_referent = (test_node *)nggc_alloc(weak_heap, node_type);
    expect_true(public_strong_referent != 0,
                "weak strong referent allocation");
    expect_ok(nggc_weak_ref_create(weak_heap, public_strong_referent,
                                   &public_rooted_weak),
              "public rooted weak create");
    expect_ok(nggc_root_push(weak_heap,
                             (void **)&public_strong_referent),
              "public strong referent root");
    expect_ok(nggc_root_push(weak_heap, &public_rooted_weak),
              "public rooted weak root");
    expect_ok(nggc_collect(weak_heap),
              "public weak keeps strongly rooted referent");
    expect_ok(nggc_weak_ref_get(weak_heap, public_rooted_weak,
                                &public_weak_value),
              "public rooted weak get");
    expect_true(public_weak_value == public_strong_referent,
                "public weak referent retained by strong root");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(weak_heap, &stats),
              "weak heap stats after retained weak");
    expect_true(stats.live_object_count == 2u,
                "public rooted weak and referent live");
    expect_true(stats.weak_reference_discovered_count == 2u,
                "public retained weak discovered");
    expect_true(stats.weak_reference_cleared_count == 1u,
                "public retained weak not cleared");
    expect_ok(nggc_root_pop(weak_heap, &public_rooted_weak),
              "public rooted weak pop");
    expect_ok(nggc_root_pop(weak_heap,
                            (void **)&public_strong_referent),
              "public strong referent pop");
    expect_ok(nggc_collect(weak_heap), "public weak heap final collect");
    expect_ok(nggc_check(weak_heap), "weak heap check");
    expect_ok(nggc_close(weak_heap), "weak heap close");

    memset(&soft_config, 0, sizeof(soft_config));
    soft_config.size = sizeof(soft_config);
    soft_config.abi_major = NGGC_ABI_MAJOR;
    soft_config.abi_minor = NGGC_ABI_MINOR;
    soft_config.soft_reference_policy = 99u;
    soft_config.gc_arena_size = (size_t)512u * 1024u;
    soft_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_true(nggc_open(&soft_config, &soft_heap) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "invalid soft policy rejected");
    soft_config.soft_reference_policy =
        NGGC_SOFT_REFERENCE_POLICY_HEAP_PRESSURE;
    soft_config.soft_reference_retain_percent = 101u;
    expect_true(nggc_open(&soft_config, &soft_heap) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "invalid soft retain percent rejected");
    soft_config.soft_reference_retain_percent = 0u;
    expect_ok(nggc_open(&soft_config, &soft_heap), "soft heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(soft_heap, &node_desc, &node_type),
              "soft heap type");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(soft_heap, &stats),
              "soft heap initial stats");
    expect_true(stats.soft_reference_policy ==
                    NGGC_SOFT_REFERENCE_POLICY_HEAP_PRESSURE,
                "soft heap default policy");
    expect_true(stats.soft_reference_retain_percent ==
                    NGGC_SOFT_REFERENCE_RETAIN_PERCENT_DEFAULT,
                "soft heap default retain percent");
    expect_true(nggc_soft_ref_create(soft_heap, 0, 0) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "soft ref create requires output");
    public_soft_ref = (void *)1;
    expect_true(nggc_soft_ref_create(soft_heap, (void *)0x1234,
                                     &public_soft_ref) ==
                    NGGC_ERR_CORRUPTION,
                "soft ref rejects invalid referent");
    expect_true(public_soft_ref == 0, "soft ref output cleared on failure");
    public_soft_value = (void *)1;
    expect_true(nggc_soft_ref_get(soft_heap, 0, &public_soft_value) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "soft ref get rejects null soft ref");
    expect_true(public_soft_value == 0,
                "soft ref get output cleared on failure");
    expect_ok(nggc_soft_ref_create(soft_heap, 0, &public_soft_ref),
              "soft ref allows null referent");
    public_soft_value = (void *)1;
    expect_ok(nggc_soft_ref_get(soft_heap, public_soft_ref,
                                &public_soft_value),
              "soft ref reads null referent");
    expect_true(public_soft_value == 0, "soft null referent value");

    public_soft_referent = (test_node *)nggc_alloc(soft_heap, node_type);
    expect_true(public_soft_referent != 0, "soft referent allocation");
    public_soft_referent->left = 0;
    public_soft_referent->right = 0;
    expect_ok(nggc_soft_ref_create(soft_heap, public_soft_referent,
                                   &public_soft_ref),
              "public soft ref create");
    expect_ok(nggc_soft_ref_get(soft_heap, public_soft_ref,
                                &public_soft_value),
              "public soft ref get before collect");
    expect_true(public_soft_value == public_soft_referent,
                "public soft referent before collect");
    expect_ok(nggc_root_push(soft_heap, &public_soft_ref),
              "public soft ref root");
    expect_ok(nggc_collect(soft_heap),
              "public soft retains low-pressure referent");
    expect_ok(nggc_soft_ref_get(soft_heap, public_soft_ref,
                                &public_soft_value),
              "public soft ref get after retain");
    expect_true(public_soft_value == public_soft_referent,
                "public soft referent retained");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(soft_heap, &stats),
              "soft heap stats after retain");
    expect_true(stats.live_object_count == 2u,
                "public soft ref and referent live");
    expect_true(stats.soft_reference_discovered_count == 1u,
                "public soft discovered");
    expect_true(stats.soft_reference_retained_count == 1u,
                "public soft retained count");
    expect_true(stats.soft_reference_cleared_count == 0u,
                "public soft not cleared");
    expect_ok(nggc_root_pop(soft_heap, &public_soft_ref),
              "public soft ref root pop");
    expect_ok(nggc_collect(soft_heap), "public soft heap final collect");
    expect_ok(nggc_check(soft_heap), "soft heap check");
    expect_ok(nggc_close(soft_heap), "soft heap close");

    memset(&soft_clear_config, 0, sizeof(soft_clear_config));
    soft_clear_config.size = sizeof(soft_clear_config);
    soft_clear_config.abi_major = NGGC_ABI_MAJOR;
    soft_clear_config.abi_minor = NGGC_ABI_MINOR;
    soft_clear_config.soft_reference_policy =
        NGGC_SOFT_REFERENCE_POLICY_ALWAYS_CLEAR;
    soft_clear_config.gc_arena_size = (size_t)512u * 1024u;
    soft_clear_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&soft_clear_config, &soft_clear_heap),
              "soft clear heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(soft_clear_heap, &node_desc, &node_type),
              "soft clear heap type");
    public_soft_referent =
        (test_node *)nggc_alloc(soft_clear_heap, node_type);
    expect_true(public_soft_referent != 0,
                "soft clear referent allocation");
    public_soft_referent->left = 0;
    public_soft_referent->right = 0;
    expect_ok(nggc_soft_ref_create(soft_clear_heap, public_soft_referent,
                                   &public_soft_ref),
              "public soft clear ref create");
    expect_ok(nggc_root_push(soft_clear_heap, &public_soft_ref),
              "public soft clear ref root");
    expect_ok(nggc_collect(soft_clear_heap),
              "public soft always-clear collect");
    expect_ok(nggc_soft_ref_get(soft_clear_heap, public_soft_ref,
                                &public_soft_value),
              "public soft clear get");
    expect_true(public_soft_value == 0,
                "public soft always-clear cleared referent");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(soft_clear_heap, &stats),
              "soft clear heap stats");
    expect_true(stats.soft_reference_policy ==
                    NGGC_SOFT_REFERENCE_POLICY_ALWAYS_CLEAR,
                "soft clear stats policy");
    expect_true(stats.live_object_count == 1u,
                "public soft clear ref remains live");
    expect_true(stats.soft_reference_discovered_count == 1u,
                "public soft clear discovered");
    expect_true(stats.soft_reference_retained_count == 0u,
                "public soft clear not retained");
    expect_true(stats.soft_reference_cleared_count == 1u,
                "public soft clear count");
    expect_ok(nggc_close(soft_clear_heap), "soft clear heap close");

    memset(&soft_retain_config, 0, sizeof(soft_retain_config));
    soft_retain_config.size = sizeof(soft_retain_config);
    soft_retain_config.abi_major = NGGC_ABI_MAJOR;
    soft_retain_config.abi_minor = NGGC_ABI_MINOR;
    soft_retain_config.soft_reference_policy =
        NGGC_SOFT_REFERENCE_POLICY_HEAP_PRESSURE;
    soft_retain_config.soft_reference_retain_percent = 100u;
    soft_retain_config.gc_arena_size = 4096u;
    soft_retain_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&soft_retain_config, &soft_retain_heap),
              "soft retain heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(soft_retain_heap, &node_desc, &node_type),
              "soft retain heap type");
    public_soft_pressure_referent =
        (test_node *)nggc_alloc_size(soft_retain_heap, node_type, 3072u);
    expect_true(public_soft_pressure_referent != 0,
                "soft retain pressure referent allocation");
    public_soft_pressure_referent->left = 0;
    public_soft_pressure_referent->right = 0;
    expect_ok(nggc_soft_ref_create(soft_retain_heap,
                                   public_soft_pressure_referent,
                                   &public_soft_ref),
              "public pressure soft ref create");
    expect_ok(nggc_root_push(soft_retain_heap, &public_soft_ref),
              "public pressure soft ref root");
    expect_ok(nggc_collect(soft_retain_heap),
              "public soft threshold retains under pressure");
    expect_ok(nggc_soft_ref_get(soft_retain_heap, public_soft_ref,
                                &public_soft_value),
              "public pressure soft get");
    expect_true(public_soft_value == public_soft_pressure_referent,
                "public soft retained by configured threshold");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(soft_retain_heap, &stats),
              "soft retain heap stats");
    expect_true(stats.soft_reference_retain_percent == 100u,
                "soft retain stats percent");
    expect_true(stats.live_object_count == 2u,
                "public pressure soft and referent live");
    expect_true(stats.soft_reference_retained_count == 1u,
                "public pressure soft retained count");
    expect_true(stats.soft_reference_cleared_count == 0u,
                "public pressure soft not cleared");
    expect_ok(nggc_close(soft_retain_heap), "soft retain heap close");

    memset(&phantom_config, 0, sizeof(phantom_config));
    phantom_config.size = sizeof(phantom_config);
    phantom_config.abi_major = NGGC_ABI_MAJOR;
    phantom_config.abi_minor = NGGC_ABI_MINOR;
    phantom_config.gc_arena_size = (size_t)512u * 1024u;
    phantom_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&phantom_config, &phantom_heap),
              "phantom heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(phantom_heap, &node_desc, &node_type),
              "phantom heap type");
    expect_true(nggc_phantom_queue_create(phantom_heap, 0) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "phantom queue create requires output");
    expect_ok(nggc_phantom_queue_create(phantom_heap,
                                        &public_phantom_queue),
              "public phantom queue create");
    expect_true(public_phantom_queue != 0, "public phantom queue pointer");
    expect_true(nggc_phantom_queue_count(phantom_heap,
                                         public_phantom_queue,
                                         0) == NGGC_ERR_INVALID_ARGUMENT,
                "phantom queue count requires output");
    public_phantom_pending = 42u;
    expect_ok(nggc_phantom_queue_count(phantom_heap,
                                       public_phantom_queue,
                                       &public_phantom_pending),
              "public phantom queue initial count");
    expect_true(public_phantom_pending == 0u,
                "public phantom queue initially empty");
    public_popped_phantom = (void *)1;
    expect_true(nggc_phantom_queue_pop(phantom_heap,
                                       public_phantom_queue,
                                       &public_popped_phantom) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "public phantom pop rejects empty queue");
    expect_true(public_popped_phantom == 0,
                "public phantom pop clears output on empty queue");
    expect_true(nggc_phantom_ref_create(phantom_heap, 0,
                                        public_phantom_queue, 0) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "phantom ref create requires output");
    public_phantom_ref = (void *)1;
    expect_true(nggc_phantom_ref_create(phantom_heap, 0,
                                        (void *)0x1234,
                                        &public_phantom_ref) ==
                    NGGC_ERR_CORRUPTION,
                "phantom ref rejects invalid queue");
    expect_true(public_phantom_ref == 0,
                "phantom ref output cleared on invalid queue");
    public_phantom_referent =
        (test_node *)nggc_alloc(phantom_heap, node_type);
    expect_true(public_phantom_referent != 0,
                "phantom referent allocation");
    public_phantom_referent->left = 0;
    public_phantom_referent->right = 0;
    expect_ok(nggc_phantom_ref_create(phantom_heap,
                                      public_phantom_referent,
                                      public_phantom_queue,
                                      &public_phantom_ref),
              "public phantom ref create");
    expect_true(public_phantom_ref != 0, "public phantom ref pointer");
    expect_ok(nggc_root_push(phantom_heap, &public_phantom_ref),
              "public phantom ref root");
    expect_ok(nggc_collect(phantom_heap),
              "public phantom enqueue after referent death");
    expect_ok(nggc_phantom_queue_count(phantom_heap,
                                       public_phantom_queue,
                                       &public_phantom_pending),
              "public phantom queue count after enqueue");
    expect_true(public_phantom_pending == 1u,
                "public phantom queue has one pending item");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(phantom_heap, &stats),
              "phantom heap stats after enqueue");
    expect_true(stats.live_object_count == 2u,
                "public phantom queue and ref live");
    expect_true(stats.phantom_reference_discovered_count == 1u,
                "public phantom discovered");
    expect_true(stats.phantom_reference_enqueued_count == 1u,
                "public phantom enqueued count");
    expect_true(stats.phantom_queue_promoted_count == 1u,
                "public phantom queue promoted");
    expect_ok(nggc_root_pop(phantom_heap, &public_phantom_ref),
              "public phantom ref root pop");
    expect_ok(nggc_collect(phantom_heap),
              "public phantom self-retains non-empty queue");
    expect_ok(nggc_phantom_queue_count(phantom_heap,
                                       public_phantom_queue,
                                       &public_phantom_pending),
              "public phantom queue count after self-retain");
    expect_true(public_phantom_pending == 1u,
                "public phantom queue still pending after self-retain");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(phantom_heap, &stats),
              "phantom heap stats after self-retain");
    expect_true(stats.live_object_count == 2u,
                "public phantom self-retained queue and ref live");
    expect_true(stats.phantom_queue_promoted_count == 2u,
                "public phantom queue promoted again");
    expect_ok(nggc_phantom_queue_pop(phantom_heap,
                                     public_phantom_queue,
                                     &public_popped_phantom),
              "public phantom queue pop");
    expect_true(public_popped_phantom == public_phantom_ref,
                "public phantom pop returns reference");
    expect_ok(nggc_phantom_queue_count(phantom_heap,
                                       public_phantom_queue,
                                       &public_phantom_pending),
              "public phantom queue count after pop");
    expect_true(public_phantom_pending == 0u,
                "public phantom queue drained");
    expect_ok(nggc_collect(phantom_heap),
              "public phantom collect drained queue");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(phantom_heap, &stats),
              "phantom heap stats after drained collect");
    expect_true(stats.live_object_count == 0u,
                "public phantom drained queue reclaimed");
    expect_ok(nggc_check(phantom_heap), "phantom heap check");
    expect_ok(nggc_close(phantom_heap), "phantom heap close");

    memset(&reaper_config, 0, sizeof(reaper_config));
    reaper_config.size = sizeof(reaper_config);
    reaper_config.abi_major = NGGC_ABI_MAJOR;
    reaper_config.abi_minor = NGGC_ABI_MINOR;
    reaper_config.gc_arena_size = (size_t)512u * 1024u;
    reaper_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&reaper_config, &reaper_heap),
              "reaper heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(reaper_heap, &node_desc, &node_type),
              "reaper heap type");
    expect_true(nggc_phantom_reaper_watch(reaper_heap, 0) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "reaper watch rejects null queue");
    expect_true(nggc_phantom_reaper_poll(reaper_heap, 0,
                                         &reaper_pending) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "reaper poll requires queue output");
    expect_true(nggc_phantom_reaper_wait(reaper_heap, 0u, 0,
                                         &reaper_pending) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "reaper wait requires queue output");
    expect_true(nggc_phantom_reaper_pop(reaper_heap, &reaper_seen_queue,
                                        0) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "reaper pop requires phantom output");
    expect_ok(nggc_phantom_queue_create(reaper_heap,
                                        &reaper_empty_queue),
              "reaper empty queue create");
    expect_ok(nggc_phantom_reaper_watch(reaper_heap, reaper_empty_queue),
              "reaper watch empty queue");
    expect_ok(nggc_phantom_reaper_watch(reaper_heap, reaper_empty_queue),
              "reaper duplicate watch idempotent");
    reaper_seen_queue = (void *)1;
    reaper_pending = 99u;
    expect_true(nggc_phantom_reaper_wait(reaper_heap, 0u,
                                         &reaper_seen_queue,
                                         &reaper_pending) ==
                    NGGC_ERR_BUSY,
                "reaper bounded wait reports no pending queue");
    expect_true(reaper_seen_queue == 0 && reaper_pending == 0u,
                "reaper bounded wait clears empty outputs");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(reaper_heap, &stats),
              "reaper stats after watch");
    expect_true(stats.phantom_reaper_watched_queue_count == 1u,
                "reaper watched queue count");
    expect_ok(nggc_phantom_reaper_unwatch(reaper_heap,
                                          reaper_empty_queue),
              "reaper unwatch empty queue");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(reaper_heap, &stats),
              "reaper stats after unwatch");
    expect_true(stats.phantom_reaper_watched_queue_count == 0u,
                "reaper unwatch removes queue");
    expect_ok(nggc_phantom_reaper_watch(reaper_heap, reaper_empty_queue),
              "reaper watch empty queue again");
    expect_ok(nggc_collect(reaper_heap),
              "reaper collect empty watched queue");
    reaper_seen_queue = (void *)1;
    reaper_pending = 99u;
    expect_ok(nggc_phantom_reaper_poll(reaper_heap, &reaper_seen_queue,
                                       &reaper_pending),
              "reaper poll drops stale empty queue");
    expect_true(reaper_seen_queue == 0 && reaper_pending == 0u,
                "reaper poll no pending after stale drop");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(reaper_heap, &stats),
              "reaper stats after stale drop");
    expect_true(stats.live_object_count == 0u,
                "reaper weak watch did not retain empty queue");
    expect_true(stats.phantom_reaper_watched_queue_count == 0u,
                "reaper stale queue removed");
    expect_true(stats.phantom_reaper_dropped_queue_count == 1u,
                "reaper stale queue drop counted");

    expect_ok(nggc_phantom_queue_create(reaper_heap, &reaper_queue),
              "reaper queue create");
    reaper_referent = (test_node *)nggc_alloc(reaper_heap, node_type);
    expect_true(reaper_referent != 0, "reaper referent allocation");
    reaper_referent->left = 0;
    reaper_referent->right = 0;
    expect_ok(nggc_phantom_ref_create(reaper_heap, reaper_referent,
                                      reaper_queue,
                                      &reaper_phantom_ref),
              "reaper phantom ref create");
    expect_ok(nggc_phantom_reaper_watch(reaper_heap, reaper_queue),
              "reaper watch queue");
    expect_ok(nggc_root_push(reaper_heap, &reaper_phantom_ref),
              "reaper phantom ref root");
    reaper_seen_queue = 0;
    reaper_pending = 0u;
    expect_true(nggc_phantom_reaper_wait(reaper_heap, 0u,
                                         &reaper_seen_queue,
                                         &reaper_pending) ==
                    NGGC_ERR_BUSY,
                "reaper zero-budget wait does not collect");
    expect_true(reaper_seen_queue == 0 && reaper_pending == 0u,
                "reaper zero-budget wait leaves queue empty");
    expect_ok(nggc_phantom_reaper_wait(reaper_heap, 1u,
                                       &reaper_seen_queue,
                                       &reaper_pending),
              "reaper bounded wait collects phantom referent");
    expect_true(reaper_seen_queue == reaper_queue &&
                    reaper_pending == 1u,
                "reaper bounded wait returns pending queue");
    expect_ok(nggc_root_pop(reaper_heap, &reaper_phantom_ref),
              "reaper phantom ref root pop");
    reaper_seen_queue = 0;
    reaper_popped_phantom = 0;
    expect_ok(nggc_phantom_reaper_pop(reaper_heap, &reaper_seen_queue,
                                      &reaper_popped_phantom),
              "reaper pop pending phantom");
    expect_true(reaper_seen_queue == reaper_queue &&
                    reaper_popped_phantom == reaper_phantom_ref,
                "reaper pop returns queue and phantom");
    expect_ok(nggc_phantom_queue_count(reaper_heap, reaper_queue,
                                       &reaper_pending),
              "reaper queue count after pop");
    expect_true(reaper_pending == 0u, "reaper queue drained");
    expect_ok(nggc_collect(reaper_heap),
              "reaper collect drained observed queue");
    reaper_seen_queue = (void *)1;
    reaper_popped_phantom = (void *)1;
    expect_ok(nggc_phantom_reaper_pop(reaper_heap, &reaper_seen_queue,
                                      &reaper_popped_phantom),
              "reaper pop drops stale drained queue");
    expect_true(reaper_seen_queue == 0 && reaper_popped_phantom == 0,
                "reaper pop no pending after stale drained queue");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(reaper_heap, &stats),
              "reaper stats after drained stale drop");
    expect_true(stats.live_object_count == 0u,
                "reaper drained queue reclaimed");
    expect_true(stats.phantom_reaper_watched_queue_count == 0u,
                "reaper watched queues empty after stale drop");
    expect_true(stats.phantom_reaper_dropped_queue_count == 2u,
                "reaper dropped both stale queues");
    expect_ok(nggc_check(reaper_heap), "reaper heap check");
    expect_ok(nggc_close(reaper_heap), "reaper heap close");

    memset(&corrupt_config, 0, sizeof(corrupt_config));
    corrupt_config.size = sizeof(corrupt_config);
    corrupt_config.abi_major = NGGC_ABI_MAJOR;
    corrupt_config.abi_minor = NGGC_ABI_MINOR;
    corrupt_config.gc_arena_size = (size_t)512u * 1024u;
    corrupt_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&corrupt_config, &corrupt_heap),
              "corrupt heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(corrupt_heap, &node_desc, &node_type),
              "corrupt heap type");
    corrupt_node = (test_node *)nggc_alloc(corrupt_heap, node_type);
    expect_true(corrupt_node != 0, "corrupt heap allocation");
    corrupt_magic = (uint32_t *)((uintptr_t)corrupt_node -
                                 TEST_OBJECT_HEADER_BYTES);
    expect_true(*corrupt_magic == TEST_OBJECT_MAGIC,
                "test object header magic");
    *corrupt_magic = 0u;
    expect_true(nggc_check(corrupt_heap) == NGGC_ERR_CORRUPTION,
                "corrupted header detected");
    expect_ok(nggc_close(corrupt_heap), "corrupt heap close");

    memset(&auto_config, 0, sizeof(auto_config));
    auto_config.size = sizeof(auto_config);
    auto_config.abi_major = NGGC_ABI_MAJOR;
    auto_config.abi_minor = NGGC_ABI_MINOR;
    auto_config.flags = NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT;
    auto_config.gc_arena_size = (size_t)512u * 1024u;
    auto_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&auto_config, &auto_heap), "auto heap open");
    expect_ok(nggc_thread_register(auto_heap, &auto_thread),
              "auto thread register");
    expect_ok(nggc_thread_register(auto_heap, &auto_thread2),
              "auto thread2 register");
    expect_true(nggc_collect(auto_heap) == NGGC_ERR_BUSY,
                "auto collect requests safepoint");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(auto_heap, &stats),
              "auto stats after request");
    expect_true(stats.flags == NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT,
                "auto stats flags");
    expect_true(stats.safepoint_epoch == 1u,
                "auto stats safepoint epoch");
    expect_true(stats.safepoint_request_count == 1u,
                "auto stats safepoint request count");
    expect_true(stats.safepoint_wait_count == 1u,
                "auto stats safepoint wait count");
    expect_true(stats.safepoint_pending_threads == 2u,
                "auto stats pending threads");
    expect_true(stats.phase_transition_count == 0u,
                "auto stats no phase before safepoint");
    expect_true(nggc_collect(auto_heap) == NGGC_ERR_BUSY,
                "auto collect waits without new request");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(auto_heap, &stats),
              "auto stats after second busy");
    expect_true(stats.safepoint_request_count == 1u,
                "auto stats request not repeated");
    expect_true(stats.safepoint_wait_count == 2u,
                "auto stats wait repeated");
    expect_ok(nggc_safepoint(auto_heap, auto_thread),
              "auto thread safepoint");
    expect_true(nggc_collect(auto_heap) == NGGC_ERR_BUSY,
                "auto collect waits for second thread");
    expect_ok(nggc_safepoint(auto_heap, auto_thread2),
              "auto thread2 safepoint");
    expect_ok(nggc_collect(auto_heap),
              "auto collect after safepoint complete");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(auto_heap, &stats),
              "auto stats after collect");
    expect_true(stats.collection_count == 1u,
                "auto stats collection count");
    expect_true(stats.reference_process_count == 1u,
                "auto stats reference process count");
    expect_true(stats.phase_transition_count == 8u,
                "auto stats phase transitions");
    expect_true(stats.safepoint_ack_count == 2u,
                "auto stats safepoint ack count");
    expect_true(stats.safepoint_pending_threads == 0u,
                "auto stats no pending threads");
    expect_true(stats.safepoint_parked_threads == 0u &&
                    stats.safepoint_active == 0u &&
                    stats.safepoint_release_count == 1u &&
                    stats.safepoint_release_epoch == stats.safepoint_epoch,
                "auto collect released parked threads");
    expect_ok(nggc_thread_unregister(auto_heap, auto_thread),
              "auto thread unregister");
    auto_thread = 0;
    expect_ok(nggc_thread_unregister(auto_heap, auto_thread2),
              "auto thread2 unregister");
    auto_thread2 = 0;
    expect_ok(nggc_close(auto_heap), "auto heap close");

    expect_ok(nggc_open(&auto_config, &owned_heap),
              "collector-owned heap open");
    expect_ok(nggc_thread_register(owned_heap, &owned_thread),
              "collector owner register");
    expect_ok(nggc_thread_register(owned_heap, &owned_peer),
              "collector peer register");
    expect_true(nggc_thread_collect(owned_heap, owned_thread) ==
                    NGGC_ERR_BUSY,
                "collector owner waits for peer");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(owned_heap, &stats),
              "collector-owned pending stats");
    expect_true(stats.safepoint_active == 1u &&
                    stats.safepoint_pending_threads == 1u &&
                    stats.safepoint_parked_threads == 1u &&
                    stats.safepoint_collector_owned_threads == 1u,
                "collector owner excludes itself from pending");
    expect_true(nggc_collect(owned_heap) == NGGC_ERR_BUSY,
                "ordinary collector cannot steal owner cycle");
    expect_true(nggc_thread_collect(owned_heap, owned_peer) ==
                    NGGC_ERR_BUSY,
                "peer cannot steal owner cycle");
    expect_true(nggc_safepoint(owned_heap, owned_thread) == NGGC_ERR_BUSY,
                "collector owner cannot downgrade to parked mutator");
    expect_ok(nggc_safepoint(owned_heap, owned_peer),
              "collector peer parks");
    expect_ok(nggc_thread_collect(owned_heap, owned_thread),
              "collector owner completes collection");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(owned_heap, &stats),
              "collector-owned completion stats");
    expect_true(stats.collection_count == 1u &&
                    stats.safepoint_ack_count == 2u &&
                    stats.safepoint_release_count == 1u &&
                    stats.safepoint_active == 0u &&
                    stats.safepoint_collector_owned_threads == 0u,
                "collector-owned cycle releases all threads");
    expect_ok(nggc_thread_unregister(owned_heap, owned_peer),
              "collector peer unregister");
    expect_ok(nggc_thread_unregister(owned_heap, owned_thread),
              "collector owner unregister");
    expect_ok(nggc_close(owned_heap), "collector-owned heap close");

    memset(&split_config, 0, sizeof(split_config));
    split_config.size = sizeof(split_config);
    split_config.abi_major = NGGC_ABI_MAJOR;
    split_config.abi_minor = NGGC_ABI_MINOR;
    split_config.gc_arena_size = (size_t)512u * 1024u;
    split_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    split_config.region_size = 1536u;
    expect_ok(nggc_open(&split_config, &split_heap), "split heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(split_heap, &node_desc, &node_type),
              "split heap type");
    large_reused = (test_node *)nggc_alloc_size(split_heap,
                                                node_type,
                                                1024u);
    expect_true(large_reused != 0, "split heap large allocation");
    expect_ok(nggc_collect(split_heap), "split heap reclaim large block");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(split_heap, &stats),
              "split stats after reclaim");
    expect_true(stats.free_object_count == 1u,
                "split stats one free block");
    small_reuse = (test_node *)nggc_alloc(split_heap, node_type);
    expect_true(small_reuse == large_reused,
                "split heap reuses large block prefix");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(split_heap, &stats),
              "split stats after reuse");
    expect_true(stats.live_object_count == 1u,
                "split stats live small object");
    expect_true(stats.free_object_count == 1u,
                "split stats tail free block remains");
    expect_true(stats.free_bytes != 0u,
                "split stats tail free bytes remain");
    expect_ok(nggc_check(split_heap), "split heap check");
    expect_ok(nggc_collect(split_heap), "split heap coalesce free blocks");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(split_heap, &stats),
              "split stats after coalesce");
    expect_true(stats.live_object_count == 0u,
                "split stats no live after coalesce");
    expect_true(stats.free_object_count == 1u,
                "split stats one coalesced free block");
    large_reused = (test_node *)nggc_alloc_size(split_heap,
                                                node_type,
                                                1024u);
    expect_true(large_reused == small_reuse,
                "split heap reuses coalesced block");
    expect_ok(nggc_check(split_heap), "split heap check after coalesce");
    expect_ok(nggc_close(split_heap), "split heap close");

    memset(&small_config, 0, sizeof(small_config));
    small_config.size = sizeof(small_config);
    small_config.abi_major = NGGC_ABI_MAJOR;
    small_config.abi_minor = NGGC_ABI_MINOR;
    small_config.gc_arena_size = 4096u;
    small_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&small_config, &small_heap), "small heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(small_heap, &node_desc, &node_type),
              "small heap type");
    while (nggc_alloc(small_heap, node_type) != 0) {
    }
    expect_true(nggc_get_last_error(small_heap) == NGGC_ERR_OUT_OF_MEMORY,
                "small heap OOM");
    expect_true(nggc_root_pop(small_heap, (void **)&root) ==
                    NGGC_ERR_INVALID_ARGUMENT,
                "root pop underflow rejected");
    expect_ok(nggc_close(small_heap), "small heap close");

    memset(&assist_config, 0, sizeof(assist_config));
    assist_config.size = sizeof(assist_config);
    assist_config.abi_major = NGGC_ABI_MAJOR;
    assist_config.abi_minor = NGGC_ABI_MINOR;
    assist_config.flags = NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM;
    assist_config.gc_arena_size = 4096u;
    assist_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&assist_config, &assist_heap),
              "assist heap open");
    node_desc.type_id = NGGC_TYPE_NONE;
    expect_ok(nggc_register_type(assist_heap, &node_desc, &node_type),
              "assist heap type");
    assist_head = (test_node *)nggc_alloc(assist_heap, node_type);
    expect_true(assist_head != 0, "assist head allocation");
    assist_head->left = 0;
    assist_head->right = 0;
    expect_ok(nggc_root_push(assist_heap, (void **)&assist_head),
              "assist head root");
    assist_tail = assist_head;
    assist_count = 1u;
    while (assist_count < 64u) {
        assist_next = (test_node *)nggc_alloc(assist_heap, node_type);
        if (!assist_next) {
            break;
        }
        assist_next->left = 0;
        assist_next->right = 0;
        expect_ok(NGGC_STORE(assist_heap, assist_tail, &assist_tail->left,
                             assist_next),
                  "assist link node");
        assist_tail = assist_next;
        ++assist_count;
    }
    expect_true(assist_count > 1u, "assist heap filled with reachable chain");
    expect_true(nggc_get_last_error(assist_heap) == NGGC_ERR_OUT_OF_MEMORY,
                "assist heap initial OOM with reachable chain");
    expect_ok(NGGC_STORE(assist_heap, assist_head, &assist_head->left, 0),
              "assist unlink tail");
    assist_replacement = 0;
    while (!assist_replacement && assist_retry_count < 128u) {
        assist_replacement =
            (test_node *)nggc_alloc(assist_heap, node_type);
        ++assist_retry_count;
    }
    expect_true(assist_replacement != 0,
                "assist reclaim makes allocation succeed");
    expect_true(assist_retry_count > 1u,
                "bounded assist requires multiple allocation retries");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(assist_heap, &stats),
              "assist heap stats after reclaim");
    expect_true(stats.reclaimed_object_count >= 1u,
                "assist reclaim reclaimed object");
    expect_true(stats.collection_count >= 1u,
                "assist reclaim counted collection work");
    expect_true(stats.mutator_assist_repay_count >= 1u,
                "assist reclaim repaid debt");
    expect_true(stats.mutator_assist_packet_count >= assist_retry_count,
                "assist counted bounded packets");
    expect_true(stats.mutator_assist_cycle_count >= 1u,
                "assist counted cycle start");
    expect_true(stats.mutator_assist_mark_items_processed <=
                    stats.mutator_assist_packet_count * 8u,
                "assist mark work stays within packet budget");
    expect_true(stats.mutator_assist_progress_count >= 1u,
                "assist reports packet progress");
    expect_ok(nggc_check(assist_heap), "assist heap check");
    expect_ok(nggc_close(assist_heap), "assist heap close");
    return 0;
}
