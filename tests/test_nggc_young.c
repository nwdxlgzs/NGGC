#include "nggc/nggc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NGGC_HAVE_C11_THREADS
#define NGGC_HAVE_C11_THREADS 0
#endif

typedef struct young_node {
    struct young_node *edge;
    int value;
} young_node;

typedef struct barrier_large_node {
    young_node *edge;
    size_t tail_offset;
} barrier_large_node;

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

static void scan_young_node(void *object, nggc_visit_fn visit, void *ctx) {
    young_node *node = (young_node *)object;
    visit(ctx, (void **)&node->edge);
}

static void scan_barrier_large_node(void *object, nggc_visit_fn visit,
                                    void *ctx) {
    barrier_large_node *node = (barrier_large_node *)object;
    visit(ctx, (void **)&node->edge);
    visit(ctx, (void **)((unsigned char *)node + node->tail_offset));
}

static void run_read_barrier_slot_case(size_t region_size) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_heap *heap = 0;
    barrier_large_node *large;
    young_node *child;
    young_node *child_source;
    void **tail;
    void *alias;
    size_t payload_size = region_size * 3u + sizeof(*large);
    uint16_t node_type = NGGC_TYPE_NONE;
    uint16_t large_type = NGGC_TYPE_NONE;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.region_size = region_size;
    config.gc_arena_size = region_size * 16u;
    config.uhm_arena_size = (size_t)8u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open read-barrier slot heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(*child);
    desc.scan = scan_young_node;
    expect_ok(nggc_register_type(heap, &desc, &node_type),
              "register read-barrier young node");
    desc.object_size = sizeof(*large);
    desc.scan = scan_barrier_large_node;
    expect_ok(nggc_register_type(heap, &desc, &large_type),
              "register read-barrier variable large layout");
    large = (barrier_large_node *)nggc_alloc_size(heap, large_type, payload_size);
    child = (young_node *)nggc_alloc(heap, node_type);
    if (!large || !child) {
        fail("allocate read-barrier slot graph");
    }
    memset(large, 0, payload_size);
    memset(child, 0, sizeof(*child));
    large->tail_offset = payload_size - sizeof(void *);
    tail = (void **)((unsigned char *)large + large->tail_offset);
    child_source = child;
    expect_ok(NGGC_STORE(heap, large, &large->edge, child),
              "publish large start-region edge");
    expect_ok(nggc_store(heap, large, tail, child),
              "publish large continuation-region edge");
    expect_ok(nggc_root_push(heap, (void **)&large), "root read-barrier large owner");
    expect_ok(nggc_root_push(heap, (void **)&child), "root read-barrier young child");
    expect_ok(nggc_collect_young(heap), "activate read barrier with retained source");
    if (child == child_source || NGGC_LOAD(heap, &large->edge) != child ||
        nggc_load(heap, tail) != child) {
        fail("read barrier finds payload slots in large start and continuation regions");
    }
    alias = child_source;
    if (nggc_load(heap, &alias) != child || alias != child) {
        fail("external slot remains distinct from managed payload lookup");
    }
    if (nggc_load(heap, (void **)((unsigned char *)child_source - sizeof(void *))) != 0 ||
        nggc_get_last_error(heap) != NGGC_ERR_CORRUPTION) {
        fail("active read barrier rejects an aligned header slot");
    }
    if (nggc_load(heap, (void **)((unsigned char *)large + payload_size)) != 0 ||
        nggc_get_last_error(heap) != NGGC_ERR_CORRUPTION) {
        fail("active read barrier rejects aligned payload padding");
    }
    if (nggc_load(heap, (void **)((unsigned char *)tail + 1u)) != 0 ||
        nggc_get_last_error(heap) != NGGC_ERR_INVALID_ARGUMENT) {
        fail("read barrier rejects unaligned slot before dereferencing it");
    }
    expect_ok(nggc_check(heap), "slot rejection preserves graph and metadata");
    expect_ok(nggc_root_pop(heap, (void **)&child), "unroot read-barrier child");
    expect_ok(nggc_root_pop(heap, (void **)&large), "unroot read-barrier large owner");
    expect_ok(nggc_collect(heap), "collect read-barrier slot graph");
    expect_ok(nggc_close(heap), "close read-barrier slot heap");
}

static void run_evacuation_worker_case(const nggc_type_desc *base_desc) {
    enum { OBJECT_COUNT = 64 };
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    young_node *objects[OBJECT_COUNT];
    uint16_t type_id = NGGC_TYPE_NONE;
#if NGGC_HAVE_C11_THREADS
    uint32_t pool_size_after_first;
    uint64_t created_after_first;
    uint64_t dispatch_after_first;
    uint64_t remap_dispatch_after_first;
#else
    uint64_t fallback_after_first;
    uint64_t remap_fallback_after_first;
#endif

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)1024u * 1024u;
    config.uhm_arena_size = (size_t)4u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open worker evacuation heap");
    desc = *base_desc;
    desc.type_id = NGGC_TYPE_NONE;
    desc.name = "worker-young-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register worker evacuation node");
    memset(objects, 0, sizeof(objects));
    for (size_t i = 0u; i < OBJECT_COUNT; ++i) {
        objects[i] = (young_node *)nggc_alloc(heap, type_id);
        if (!objects[i]) {
            fail("allocate worker evacuation objects");
        }
        memset(objects[i], 0, sizeof(*objects[i]));
        objects[i]->value = (int)i + 1;
        expect_ok(nggc_root_push(heap, (void **)&objects[i]),
                  "root worker evacuation object");
    }
    expect_ok(nggc_collect_young(heap),
              "parallel worker young evacuation");
    expect_ok(nggc_check(heap), "verify worker evacuation heap");
    for (size_t i = 0u; i < OBJECT_COUNT; ++i) {
        if (!objects[i] || objects[i]->value != (int)i + 1 ||
            nggc_object_type(objects[i]) != type_id) {
            fail("worker evacuation preserves rooted objects");
        }
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read worker evacuation stats");
    if (stats.evacuation_worker_task_count != OBJECT_COUNT ||
        stats.forwarding_cas_success_count != OBJECT_COUNT ||
        stats.forwarding_cas_failure_count != 0u ||
        stats.evacuation_loser_reclaimed_count != 0u ||
        stats.evacuation_workers_active != 0u ||
        stats.evacuation_workers_quiescent != 0u ||
        stats.evacuation_worker_epoch != 0u ||
        stats.remap_workers_active != 0u ||
        stats.remap_workers_quiescent != 0u ||
        stats.remap_worker_epoch != 0u ||
        stats.from_space_reclaim_pending != 0u) {
        fail("worker evacuation quiesces before collection returns");
    }
#if NGGC_HAVE_C11_THREADS
    if (stats.evacuation_parallel_cycle_count != 1u ||
        stats.evacuation_worker_pool_size == 0u ||
        stats.evacuation_worker_created_count == 0u ||
        stats.evacuation_worker_created_count !=
            stats.evacuation_worker_pool_size ||
        stats.evacuation_worker_dispatch_count == 0u ||
        stats.evacuation_workers_peak_active < 1u ||
        stats.remap_parallel_cycle_count != 1u ||
        stats.remap_worker_dispatch_count == 0u ||
        stats.remap_worker_block_count == 0u ||
        stats.remap_scanned_slot_count < OBJECT_COUNT ||
        stats.remap_rewritten_slot_count < OBJECT_COUNT ||
        stats.remap_workers_peak_active < 1u ||
        stats.remap_worker_epoch != 0u ||
        stats.remap_workers_active != 0u ||
        stats.remap_workers_quiescent != 0u) {
        fail("C11 evacuation creates persistent background workers");
    }
    pool_size_after_first = stats.evacuation_worker_pool_size;
    created_after_first = stats.evacuation_worker_created_count;
    dispatch_after_first = stats.evacuation_worker_dispatch_count;
    remap_dispatch_after_first = stats.remap_worker_dispatch_count;
#else
    if (stats.evacuation_parallel_cycle_count != 0u ||
        stats.evacuation_worker_pool_size != 0u ||
        stats.evacuation_worker_created_count != 0u ||
        stats.evacuation_worker_dispatch_count != 0u ||
        stats.evacuation_worker_fallback_count == 0u ||
        stats.remap_parallel_cycle_count != 0u ||
        stats.remap_worker_dispatch_count != 0u ||
        stats.remap_worker_fallback_count == 0u ||
        stats.remap_worker_block_count == 0u ||
        stats.remap_scanned_slot_count < OBJECT_COUNT ||
        stats.remap_rewritten_slot_count < OBJECT_COUNT) {
        fail("threadless evacuation uses synchronous fallback");
    }
    fallback_after_first = stats.evacuation_worker_fallback_count;
    remap_fallback_after_first = stats.remap_worker_fallback_count;
#endif

    expect_ok(nggc_collect_young(heap),
              "reuse persistent workers for threshold evacuation");
    expect_ok(nggc_check(heap),
              "verify persistent worker reuse heap");
    for (size_t i = 0u; i < OBJECT_COUNT; ++i) {
        if (!objects[i] || objects[i]->value != (int)i + 1 ||
            nggc_object_type(objects[i]) != type_id) {
            fail("persistent worker reuse preserves rooted objects");
        }
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read persistent worker reuse stats");
    if (stats.young_collection_count != 2u ||
        stats.young_object_count != 0u ||
        stats.old_object_count != OBJECT_COUNT ||
        stats.young_promoted_object_count != OBJECT_COUNT ||
        stats.evacuation_worker_task_count != OBJECT_COUNT * 2u ||
        stats.evacuation_workers_active != 0u ||
        stats.evacuation_workers_quiescent != 0u ||
        stats.evacuation_worker_epoch != 0u ||
        stats.remap_workers_active != 0u ||
        stats.remap_workers_quiescent != 0u ||
        stats.remap_worker_epoch != 0u ||
        stats.from_space_reclaim_pending != 0u) {
        fail("persistent worker threshold collection invariants");
    }
#if NGGC_HAVE_C11_THREADS
    if (stats.evacuation_parallel_cycle_count != 2u ||
        stats.evacuation_worker_pool_size != pool_size_after_first ||
        stats.evacuation_worker_created_count != created_after_first ||
        stats.evacuation_worker_dispatch_count <= dispatch_after_first ||
        stats.remap_parallel_cycle_count != 2u ||
        stats.remap_worker_dispatch_count <= remap_dispatch_after_first) {
        fail("persistent evacuation workers are reused across collections");
    }
#else
    if (stats.evacuation_parallel_cycle_count != 0u ||
        stats.evacuation_worker_pool_size != 0u ||
        stats.evacuation_worker_created_count != 0u ||
        stats.evacuation_worker_dispatch_count != 0u ||
        stats.evacuation_worker_fallback_count <= fallback_after_first ||
        stats.remap_parallel_cycle_count != 0u ||
        stats.remap_worker_dispatch_count != 0u ||
        stats.remap_worker_fallback_count <=
            remap_fallback_after_first) {
        fail("threadless repeated evacuation remains synchronous");
    }
#endif
    for (size_t i = OBJECT_COUNT; i != 0u; --i) {
        expect_ok(nggc_root_pop(heap, (void **)&objects[i - 1u]),
                  "unroot worker evacuation object");
    }
    expect_ok(nggc_collect(heap), "collect worker evacuation heap");
    expect_ok(nggc_close(heap), "close worker evacuation heap");
}

static void run_delayed_from_space_reclaim_case(
    const nggc_type_desc *base_desc) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats before_reclaim;
    nggc_stats after_reclaim;
    nggc_heap *heap = 0;
    young_node *owner;
    young_node *child;
    young_node *owner_source;
    young_node *child_source;
    young_node *stale_alias;
    uint16_t type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open delayed from-space heap");
    expect_ok(nggc_collect_young(heap),
              "collect empty delayed from-space heap");
    expect_ok(nggc_check(heap),
              "verify empty young collection");
    desc = *base_desc;
    desc.type_id = NGGC_TYPE_NONE;
    desc.name = "delayed-from-space-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register delayed from-space node");

    owner = (young_node *)nggc_alloc(heap, type_id);
    child = (young_node *)nggc_alloc(heap, type_id);
    if (!owner || !child) {
        fail("allocate delayed from-space graph");
    }
    memset(owner, 0, sizeof(*owner));
    memset(child, 0, sizeof(*child));
    owner->value = 71;
    child->value = 72;
    owner->edge = child;
    owner_source = owner;
    child_source = child;
    expect_ok(nggc_root_push(heap, (void **)&owner),
              "root delayed from-space owner");
    expect_ok(nggc_collect_young(heap),
              "create delayed from-space quarantine");
    if (owner == owner_source || owner->edge == child_source ||
        owner->value != 71 || owner->edge->value != 72 ||
        nggc_object_type(owner_source) != NGGC_TYPE_NONE ||
        nggc_object_type(child_source) != NGGC_TYPE_NONE) {
        fail("moving collection preserves forwarding shells");
    }

    stale_alias = child_source;
    if (NGGC_LOAD(heap, &stale_alias) != owner->edge ||
        stale_alias != owner->edge) {
        fail("read barrier canonicalizes and heals stale alias");
    }
    expect_ok(NGGC_STORE(heap, owner_source, &owner_source->edge,
                         child_source),
              "store barrier canonicalizes stale owner and value");
    if (owner->edge != stale_alias || owner_source->edge != stale_alias) {
        fail("stale-owner store updates source shadow and target");
    }

    expect_ok(NGGC_STORE(heap, owner, &owner->edge, 0),
              "clear canonical owner without updating its old shadow");
    if (owner_source->edge != stale_alias) {
        fail("canonical write leaves an intentionally stale source field");
    }
    if (NGGC_LOAD(heap, &owner_source->edge) != 0 ||
        nggc_get_last_error(heap) != NGGC_OK) {
        fail("stale-owner load reads null from canonical field");
    }
    expect_ok(NGGC_STORE(heap, owner, &owner->edge, owner),
              "replace canonical owner edge with another referent");
    if (NGGC_LOAD(heap, &owner_source->edge) != owner) {
        fail("stale-owner load reads replacement from canonical field");
    }
    expect_ok(NGGC_STORE(heap, owner_source, &owner_source->edge,
                         child_source),
              "restore graph through stale owner after canonical reads");

    memset(&before_reclaim, 0, sizeof(before_reclaim));
    before_reclaim.size = sizeof(before_reclaim);
    expect_ok(nggc_get_stats(heap, &before_reclaim),
              "read active from-space quarantine stats");
    if (before_reclaim.gc_phase != NGGC_PHASE_IDLE ||
        before_reclaim.read_barrier_active != 1u ||
        before_reclaim.from_space_reclaim_pending != 0u ||
        before_reclaim.from_space_quarantine_active != 1u ||
        before_reclaim.from_space_quarantine_epoch == 0u ||
        before_reclaim.from_space_quarantine_object_count != 2u ||
        before_reclaim.from_space_quarantine_bytes == 0u ||
        before_reclaim.quarantined_region_count == 0u ||
        before_reclaim.from_space_quarantine_cycle_count != 1u ||
        before_reclaim.from_space_delayed_reclaim_count != 0u) {
        fail("from-space quarantine remains barrier protected");
    }
    expect_ok(nggc_check(heap),
              "verify active delayed from-space quarantine");

    expect_ok(nggc_request_safepoint(heap),
              "request later quarantine reclaim safepoint");
    expect_ok(nggc_safepoint_release(heap),
              "later safepoint reclaims forwarding shells");
    memset(&after_reclaim, 0, sizeof(after_reclaim));
    after_reclaim.size = sizeof(after_reclaim);
    expect_ok(nggc_get_stats(heap, &after_reclaim),
              "read delayed reclaim stats");
    if (after_reclaim.read_barrier_active != 0u ||
        after_reclaim.from_space_quarantine_active != 0u ||
        after_reclaim.from_space_quarantine_epoch != 0u ||
        after_reclaim.from_space_quarantine_object_count != 0u ||
        after_reclaim.from_space_quarantine_bytes != 0u ||
        after_reclaim.quarantined_region_count != 0u ||
        after_reclaim.from_space_delayed_reclaim_count != 1u ||
        after_reclaim.from_space_delayed_reclaimed_object_count != 2u ||
        after_reclaim.from_space_delayed_reclaimed_bytes !=
            before_reclaim.from_space_quarantine_bytes) {
        fail("later safepoint publishes delayed reclaim completion");
    }
    stale_alias = owner_source;
    if (NGGC_LOAD(heap, &stale_alias) != 0 ||
        nggc_get_last_error(heap) != NGGC_ERR_CORRUPTION) {
        fail("reclaimed forwarding shell is no longer readable");
    }
    expect_ok(nggc_check(heap),
              "verify heap after delayed from-space reclaim");
    expect_ok(nggc_root_pop(heap, (void **)&owner),
              "unroot delayed from-space owner");
    expect_ok(nggc_collect(heap),
              "collect delayed from-space heap");
    expect_ok(nggc_close(heap), "close delayed from-space heap");
}

static void run_external_collection_set_edge_case(
    const nggc_type_desc *base_desc) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_type_desc special_desc;
    nggc_stats base_stats;
    nggc_stats stats;
    nggc_heap *heap = 0;
    young_node *special_owner;
    young_node *young_target;
    young_node *young_garbage;
    young_node *target_source;
    uint16_t type_id = NGGC_TYPE_NONE;
    uint16_t special_type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open external collection-set edge heap");

    desc = *base_desc;
    desc.type_id = NGGC_TYPE_NONE;
    desc.name = "external-cset-young-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register external collection-set young node");
    special_desc = desc;
    special_desc.type_id = NGGC_TYPE_NONE;
    special_desc.flags = NGGC_TYPE_FLAG_PINNABLE |
                         NGGC_TYPE_FLAG_NON_MOVABLE;
    special_desc.name = "external-cset-special-owner";
    expect_ok(nggc_register_type(heap, &special_desc, &special_type_id),
              "register external collection-set special owner");

    special_owner = (young_node *)nggc_alloc(heap, special_type_id);
    young_target = (young_node *)nggc_alloc(heap, type_id);
    young_garbage = (young_node *)nggc_alloc(heap, type_id);
    if (!special_owner || !young_target || !young_garbage) {
        fail("allocate external collection-set graph");
    }
    memset(special_owner, 0, sizeof(*special_owner));
    memset(young_target, 0, sizeof(*young_target));
    memset(young_garbage, 0, sizeof(*young_garbage));
    special_owner->value = 81;
    young_target->value = 82;
    young_garbage->value = 83;
    expect_ok(nggc_root_push(heap, (void **)&special_owner),
              "root external collection-set special owner");
    expect_ok(NGGC_STORE(heap, special_owner, &special_owner->edge,
                         young_target),
              "store external collection-set edge");

    target_source = young_target;
    memset(&base_stats, 0, sizeof(base_stats));
    base_stats.size = sizeof(base_stats);
    expect_ok(nggc_get_stats(heap, &base_stats),
              "read external collection-set base stats");

    expect_ok(nggc_collect_young(heap),
              "young collection follows external collection-set edge");
    expect_ok(nggc_check(heap),
              "verify external collection-set edge heap");
    if (!special_owner->edge || special_owner->edge == target_source ||
        special_owner->edge->value != 82 ||
        nggc_object_type(special_owner->edge) != type_id ||
        nggc_object_type(target_source) != NGGC_TYPE_NONE ||
        nggc_object_type(young_garbage) != NGGC_TYPE_NONE ||
        nggc_object_type(special_owner) != special_type_id) {
        fail("external collection-set edge preserves only reachable target");
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read external collection-set edge stats");
    if (stats.collection_count != base_stats.collection_count + 1u ||
        stats.young_collection_count !=
            base_stats.young_collection_count + 1u ||
        stats.young_reclaimed_object_count !=
            base_stats.young_reclaimed_object_count + 1u ||
        stats.young_evacuated_object_count !=
            base_stats.young_evacuated_object_count + 1u ||
        stats.young_mark_dirty_slot_count <=
            base_stats.young_mark_dirty_slot_count ||
        stats.collection_set_region_count != 0u) {
        fail("external collection-set edge statistics");
    }

    expect_ok(nggc_root_pop(heap, (void **)&special_owner),
              "unroot external collection-set special owner");
    expect_ok(nggc_collect(heap),
              "collect external collection-set edge heap");
    expect_ok(nggc_close(heap), "close external collection-set edge heap");
}

static void run_internal_reference_evacuation_case(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats base_stats;
    nggc_stats stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *weak_referent = 0;
    void *weak_reference = 0;
    void *soft_referent = 0;
    void *soft_reference = 0;
    void *phantom_queue = 0;
    void *phantom_referent = 0;
    void *phantom_reference = 0;
    void *old_weak_referent;
    void *old_weak_reference;
    void *old_soft_referent;
    void *old_soft_reference;
    void *old_phantom_queue;
    void *old_phantom_reference;
    void *referent = 0;
    void *reaper_queue = 0;
    void *popped_reference = 0;
    size_t pending_count = 0u;
    size_t queue_count = 0u;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.region_size = 8192u;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open internal reference evacuation heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(young_node);
    desc.scan = scan_young_node;
    desc.name = "internal-reference-evacuation-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register internal reference evacuation node");

    weak_referent = nggc_alloc(heap, type_id);
    expect_ok(nggc_weak_ref_create(heap, weak_referent, &weak_reference),
              "create weak reference for evacuation");
    soft_referent = nggc_alloc(heap, type_id);
    expect_ok(nggc_soft_ref_create(heap, soft_referent, &soft_reference),
              "create soft reference for evacuation");
    expect_ok(nggc_phantom_queue_create(heap, &phantom_queue),
              "create phantom queue for evacuation");
    phantom_referent = nggc_alloc(heap, type_id);
    expect_ok(nggc_phantom_ref_create(heap, phantom_referent, phantom_queue,
                                      &phantom_reference),
              "create phantom reference for evacuation");
    if (!weak_referent || !weak_reference || !soft_referent ||
        !soft_reference || !phantom_queue || !phantom_referent ||
        !phantom_reference) {
        fail("allocate internal reference evacuation graph");
    }
    expect_ok(nggc_root_push(heap, &weak_referent),
              "root weak referent for evacuation");
    expect_ok(nggc_root_push(heap, &weak_reference),
              "root weak reference for evacuation");
    expect_ok(nggc_root_push(heap, &soft_referent),
              "root soft referent for evacuation");
    expect_ok(nggc_root_push(heap, &soft_reference),
              "root soft reference for evacuation");
    expect_ok(nggc_root_push(heap, &phantom_queue),
              "root phantom queue for evacuation");
    expect_ok(nggc_root_push(heap, &phantom_reference),
              "root phantom reference for evacuation");
    expect_ok(nggc_phantom_reaper_watch(heap, phantom_queue),
              "watch phantom queue for evacuation");

    old_weak_referent = weak_referent;
    old_weak_reference = weak_reference;
    old_soft_referent = soft_referent;
    old_soft_reference = soft_reference;
    old_phantom_queue = phantom_queue;
    old_phantom_reference = phantom_reference;
    memset(&base_stats, 0, sizeof(base_stats));
    base_stats.size = sizeof(base_stats);
    expect_ok(nggc_get_stats(heap, &base_stats),
              "read internal reference evacuation base stats");

    expect_ok(nggc_collect_young(heap),
              "evacuate internal weak soft phantom objects");
    expect_ok(nggc_check(heap),
              "verify internal reference evacuation heap");
    if (weak_referent == old_weak_referent ||
        weak_reference == old_weak_reference ||
        soft_referent == old_soft_referent ||
        soft_reference == old_soft_reference ||
        phantom_queue == old_phantom_queue ||
        phantom_reference == old_phantom_reference ||
        nggc_object_type(phantom_referent) != NGGC_TYPE_NONE) {
        fail("internal references evacuate and dead phantom referent reclaims");
    }
    expect_ok(nggc_weak_ref_get(heap, weak_reference, &referent),
              "get evacuated weak referent");
    if (referent != weak_referent) {
        fail("weak referent follows evacuation");
    }
    expect_ok(nggc_soft_ref_get(heap, soft_reference, &referent),
              "get evacuated soft referent");
    if (referent != soft_referent) {
        fail("soft referent follows evacuation");
    }
    expect_ok(nggc_phantom_reaper_poll(heap, &reaper_queue, &pending_count),
              "poll evacuated phantom queue");
    expect_ok(nggc_phantom_queue_count(heap, phantom_queue, &queue_count),
              "count evacuated phantom queue");
    if (reaper_queue != phantom_queue || pending_count != 1u ||
        queue_count != 1u) {
        fail("phantom reaper follows evacuated queue");
    }
    expect_ok(nggc_phantom_reaper_pop(heap, &reaper_queue,
                                      &popped_reference),
              "pop evacuated phantom reference");
    if (reaper_queue != phantom_queue ||
        popped_reference != phantom_reference) {
        fail("phantom queue link follows evacuation");
    }
    expect_ok(nggc_phantom_queue_count(heap, phantom_queue, &queue_count),
              "count drained evacuated phantom queue");
    if (queue_count != 0u) {
        fail("evacuated phantom queue drains correctly");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read internal reference evacuation stats");
    if (stats.young_evacuation_capacity_fallback_count !=
            base_stats.young_evacuation_capacity_fallback_count ||
        stats.young_evacuated_object_count <
            base_stats.young_evacuated_object_count + 6u ||
        stats.young_reclaimed_object_count <
            base_stats.young_reclaimed_object_count + 1u) {
        fail("internal reference evacuation statistics");
    }

    expect_ok(nggc_root_pop(heap, &phantom_reference),
              "unroot evacuated phantom reference");
    expect_ok(nggc_root_pop(heap, &phantom_queue),
              "unroot evacuated phantom queue");
    expect_ok(nggc_root_pop(heap, &soft_reference),
              "unroot evacuated soft reference");
    expect_ok(nggc_root_pop(heap, &soft_referent),
              "unroot evacuated soft referent");
    expect_ok(nggc_root_pop(heap, &weak_reference),
              "unroot evacuated weak reference");
    expect_ok(nggc_root_pop(heap, &weak_referent),
              "unroot evacuated weak referent");
    expect_ok(nggc_collect(heap), "collect evacuated internal references");
    expect_ok(nggc_check(heap), "verify reclaimed internal references");
    expect_ok(nggc_close(heap), "close internal reference evacuation heap");
}

static void run_in_place_region_promotion_case(
    const nggc_type_desc *base_desc) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats before;
    nggc_stats after;
    nggc_heap *heap = 0;
    young_node *object;
    void *root;
    void *first_source;
    uint16_t type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_YOUNG_IN_PLACE_PROMOTION;
    config.gc_arena_size = (size_t)128u * 1024u;
    config.uhm_arena_size = (size_t)512u * 1024u;
    config.region_size = (size_t)8u * 1024u;
    config.young_tenuring_threshold = 2u;
    expect_ok(nggc_open(&config, &heap),
              "open in-place promotion heap");
    desc = *base_desc;
    desc.type_id = NGGC_TYPE_NONE;
    desc.name = "in-place-promotion-node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register in-place promotion node");

    object = (young_node *)nggc_alloc(heap, type_id);
    if (!object) {
        fail("allocate in-place promotion object");
    }
    memset(object, 0, sizeof(*object));
    object->value = 808;
    root = object;
    expect_ok(nggc_root_push(heap, &root),
              "root in-place promotion object");
    first_source = object;

    expect_ok(nggc_collect_young(heap),
              "first in-place promotion collection");
    object = (young_node *)root;
    if (!object || object == (young_node *)first_source ||
        object->value != 808 || nggc_object_type(object) != type_id) {
        fail("object below threshold still evacuates");
    }
    memset(&before, 0, sizeof(before));
    before.size = sizeof(before);
    expect_ok(nggc_get_stats(heap, &before),
              "read pre-promotion in-place stats");
    if (before.young_object_count != 1u || before.old_object_count != 0u ||
        before.young_in_place_promoted_object_count != 0u ||
        before.young_promoted_object_count != 0u) {
        fail("object below threshold remains young");
    }

    first_source = object;
    expect_ok(nggc_collect_young(heap),
              "whole-region in-place promotion collection");
    object = (young_node *)root;
    if (!object || object != (young_node *)first_source ||
        object->value != 808 || nggc_object_type(object) != type_id) {
        fail("eligible whole-region object moves during in-place promotion");
    }
    memset(&after, 0, sizeof(after));
    after.size = sizeof(after);
    expect_ok(nggc_get_stats(heap, &after),
              "read in-place promotion stats");
    if (after.young_object_count != 0u || after.old_object_count != 1u ||
        after.young_promoted_object_count !=
            before.young_promoted_object_count + 1u ||
        after.young_in_place_promoted_object_count !=
            before.young_in_place_promoted_object_count + 1u ||
        after.young_evacuated_object_count !=
            before.young_evacuated_object_count ||
        after.evacuation_worker_task_count !=
            before.evacuation_worker_task_count) {
        fail("whole-region in-place promotion statistics");
    }
    expect_ok(nggc_check(heap), "verify in-place promoted region");
    expect_ok(nggc_root_pop(heap, &root),
              "unroot in-place promotion object");
    expect_ok(nggc_collect(heap), "reclaim in-place promotion heap");
    expect_ok(nggc_close(heap), "close in-place promotion heap");
}

int main(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_type_desc special_desc;
    nggc_stats stats;
    nggc_stats base_stats;
    nggc_heap *heap = 0;
    nggc_thread *thread = 0;
    young_node *old_owner;
    young_node *old_garbage;
    young_node *young_live;
    young_node *young_garbage;
    young_node *young_dead;
    young_node *young_source;
    young_node *survivor_copy;
    young_node *survivor_child;
    young_node *fresh_child;
    uint16_t type_id = NGGC_TYPE_NONE;
    uint16_t special_type_id = NGGC_TYPE_NONE;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open young heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(young_node);
    desc.scan = scan_young_node;
    desc.name = "young_node";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register young node");

    old_owner = (young_node *)nggc_alloc(heap, type_id);
    old_garbage = (young_node *)nggc_alloc(heap, type_id);
    expect_ok(nggc_thread_register(heap, &thread),
              "register young mutator");
    if (!old_owner || !old_garbage) {
        fail("allocate future old objects");
    }
    memset(old_owner, 0, sizeof(*old_owner));
    memset(old_garbage, 0, sizeof(*old_garbage));
    expect_ok(nggc_root_push(heap, (void **)&old_owner),
              "root future old owner");
    expect_ok(nggc_root_push(heap, (void **)&old_garbage),
              "root future old garbage");
    expect_ok(nggc_thread_collect_young(heap, thread),
              "age future old objects once");
    expect_ok(nggc_thread_collect_young(heap, thread),
              "promote future old objects at threshold");
    expect_ok(nggc_root_pop(heap, (void **)&old_garbage),
              "unroot promoted old garbage");
    memset(&base_stats, 0, sizeof(base_stats));
    base_stats.size = sizeof(base_stats);
    expect_ok(nggc_get_stats(heap, &base_stats),
              "read promotion bootstrap stats");
    if (base_stats.young_object_count != 0u ||
        base_stats.old_object_count != 2u ||
        base_stats.young_promoted_object_count != 2u ||
        base_stats.young_in_place_promoted_object_count != 0u) {
        fail("ordinary objects become old only at threshold");
    }
    young_live = (young_node *)nggc_thread_alloc(heap, thread, type_id);
    young_garbage = (young_node *)nggc_thread_alloc(heap, thread, type_id);
    young_dead = (young_node *)nggc_thread_alloc(heap, thread, type_id);
    if (!young_live || !young_garbage || !young_dead) {
        fail("allocate young collection graph");
    }
    memset(young_live, 0, sizeof(*young_live));
    memset(young_garbage, 0, sizeof(*young_garbage));
    memset(young_dead, 0, sizeof(*young_dead));
    young_live->value = 11;
    young_garbage->value = 22;
    expect_ok(NGGC_STORE(heap, old_owner, &old_owner->edge, young_live),
              "store old to young edge");
    expect_ok(NGGC_STORE(heap, young_live, &young_live->edge,
                         young_garbage),
              "store young to young edge");

    if (nggc_collect_young(heap) != NGGC_ERR_BUSY) {
        fail("young collection requires retired active TLABs");
    }
    expect_ok(nggc_thread_collect_young(heap, thread),
              "collector-owned young collection");
    expect_ok(nggc_check(heap), "verify young collection");
    young_source = young_live;
    survivor_copy = old_owner->edge;
    survivor_child = survivor_copy ? survivor_copy->edge : 0;
    if (!survivor_copy || survivor_copy == young_source ||
        nggc_object_type(survivor_copy) != type_id ||
        !survivor_child || survivor_child == young_garbage ||
        survivor_copy->value != 11 || survivor_child->value != 22 ||
        nggc_object_type(young_source) != NGGC_TYPE_NONE ||
        nggc_object_type(young_garbage) != NGGC_TYPE_NONE ||
        nggc_object_type(young_dead) != NGGC_TYPE_NONE ||
        nggc_object_type(old_garbage) != type_id) {
        fail("young collection generation filtering");
    }

    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read young stats");
    if (stats.collection_count != base_stats.collection_count + 1u ||
        stats.young_collection_count !=
            base_stats.young_collection_count + 1u ||
        stats.young_reclaimed_object_count !=
            base_stats.young_reclaimed_object_count + 1u ||
        stats.young_mark_old_object_count !=
            base_stats.young_mark_old_object_count + 2u ||
        stats.young_mark_dirty_slot_count == 0u ||
        stats.young_object_count != 2u || stats.old_object_count != 2u ||
        stats.young_survivor_object_count != 2u ||
        stats.young_tenuring_candidate_count != 0u ||
        stats.young_tenuring_threshold !=
            NGGC_YOUNG_TENURING_THRESHOLD_DEFAULT ||
        stats.young_evacuation_preflight_count !=
            base_stats.young_evacuation_preflight_count + 1u ||
        stats.young_evacuation_capacity_fallback_count !=
            base_stats.young_evacuation_capacity_fallback_count ||
        stats.young_evacuation_survivor_bytes == 0u ||
        stats.young_evacuation_promotion_bytes != 0u ||
        stats.young_evacuated_object_count !=
            base_stats.young_evacuated_object_count + 2u ||
        stats.young_promoted_object_count !=
            base_stats.young_promoted_object_count ||
        stats.reclaimed_object_count !=
            base_stats.reclaimed_object_count + 1u ||
        stats.live_object_count != 4u ||
        stats.collection_set_region_count != 0u ||
        stats.remset_old_to_young_record_count < 2u) {
        fail("young collection statistics");
    }

    fresh_child = (young_node *)nggc_thread_alloc(heap, thread, type_id);
    if (!fresh_child) {
        fail("allocate younger survivor child");
    }
    memset(fresh_child, 0, sizeof(*fresh_child));
    fresh_child->value = 33;
    expect_ok(NGGC_STORE(heap, survivor_copy, &survivor_copy->edge,
                         fresh_child),
              "replace survivor edge with younger child");

    expect_ok(nggc_thread_collect_young(heap, thread),
              "second young collection ages survivor");
    young_live = old_owner->edge;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read survivor age stats");
    if (stats.collection_count != base_stats.collection_count + 2u ||
        stats.young_collection_count !=
            base_stats.young_collection_count + 2u ||
        stats.young_object_count != 1u || stats.old_object_count != 3u ||
        stats.young_survivor_object_count != 1u ||
        stats.young_tenuring_candidate_count != 0u ||
        stats.young_evacuation_preflight_count !=
            base_stats.young_evacuation_preflight_count + 2u ||
        stats.young_evacuation_capacity_fallback_count !=
            base_stats.young_evacuation_capacity_fallback_count ||
        stats.young_evacuation_survivor_bytes == 0u ||
        stats.young_evacuation_promotion_bytes == 0u ||
        stats.young_evacuated_object_count !=
            base_stats.young_evacuated_object_count + 4u ||
        stats.young_promoted_object_count !=
            base_stats.young_promoted_object_count + 1u ||
        stats.young_region_count == 0u || stats.old_region_count < 1u ||
        stats.live_object_count != 4u || young_live == survivor_copy ||
        !young_live->edge || young_live->edge == fresh_child ||
        young_live->value != 11 || young_live->edge->value != 33 ||
        nggc_object_type(survivor_copy) != NGGC_TYPE_NONE ||
        nggc_object_type(survivor_child) != NGGC_TYPE_NONE ||
        nggc_object_type(fresh_child) != NGGC_TYPE_NONE ||
        nggc_object_type(young_live) != type_id ||
        stats.remset_old_to_young_record_count < 2u) {
        fail("young object age reaches tenuring threshold");
    }

    fresh_child = young_live->edge;
    expect_ok(nggc_thread_collect_young(heap, thread),
              "third young collection follows remapped remembered edge");
    if (!old_owner->edge || old_owner->edge != young_live ||
        !young_live->edge || young_live->edge == fresh_child ||
        young_live->edge->value != 33 ||
        nggc_object_type(fresh_child) != NGGC_TYPE_NONE ||
        nggc_object_type(young_live->edge) != type_id) {
        fail("remapped old to young edge preserves younger child");
    }
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read remembered evacuation stats");
    if (stats.collection_count != base_stats.collection_count + 3u ||
        stats.young_collection_count !=
            base_stats.young_collection_count + 3u ||
        stats.young_object_count != 0u || stats.old_object_count != 4u ||
        stats.young_evacuated_object_count !=
            base_stats.young_evacuated_object_count + 5u ||
        stats.young_promoted_object_count !=
            base_stats.young_promoted_object_count + 2u ||
        stats.young_reclaimed_object_count !=
            base_stats.young_reclaimed_object_count + 2u ||
        stats.young_mark_dirty_slot_count < 2u ||
        stats.live_object_count != 4u) {
        fail("remembered edge young collection statistics");
    }

    expect_ok(nggc_thread_unregister(heap, thread),
              "unregister young mutator");
    thread = 0;
    expect_ok(nggc_root_pop(heap, (void **)&old_owner),
              "unroot old owner");
    old_owner = 0;
    expect_ok(nggc_collect(heap), "full collect after young collection");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats), "read final young stats");
    if (stats.collection_count != base_stats.collection_count + 4u ||
        stats.young_collection_count !=
            base_stats.young_collection_count + 3u ||
        stats.live_object_count != 0u ||
        stats.reclaimed_object_count !=
            base_stats.reclaimed_object_count + 6u) {
        fail("full collection after young collection");
    }
    expect_ok(nggc_check(heap), "verify final young heap");
    expect_ok(nggc_close(heap), "close young heap");

    heap = 0;
    thread = 0;
    old_owner = 0;
    young_live = 0;
    type_id = NGGC_TYPE_NONE;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)128u * 1024u;
    config.uhm_arena_size = (size_t)512u * 1024u;
    config.tlab_size = (size_t)64u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open evacuation fallback heap");
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register fallback young node");
    special_desc = desc;
    special_desc.type_id = NGGC_TYPE_NONE;
    special_desc.flags = NGGC_TYPE_FLAG_PINNABLE |
                         NGGC_TYPE_FLAG_NON_MOVABLE;
    special_desc.name = "special_young_owner";
    expect_ok(nggc_register_type(heap, &special_desc, &special_type_id),
              "register fallback special owner");
    old_owner = (young_node *)nggc_alloc(heap, special_type_id);
    expect_ok(nggc_thread_register(heap, &thread),
              "register fallback mutator");
    young_live = (young_node *)nggc_thread_alloc(heap, thread, type_id);
    if (!old_owner || !young_live) {
        fail("allocate evacuation fallback graph");
    }
    memset(old_owner, 0, sizeof(*old_owner));
    memset(young_live, 0, sizeof(*young_live));
    expect_ok(nggc_root_push(heap, (void **)&old_owner),
              "root fallback special owner");
    expect_ok(NGGC_STORE(heap, old_owner, &old_owner->edge, young_live),
              "store fallback special to young edge");
    expect_ok(nggc_thread_collect_young(heap, thread),
              "young collection falls back before evacuation");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read evacuation fallback stats");
    if (stats.young_evacuation_preflight_count != 1u ||
        stats.young_evacuation_capacity_fallback_count != 1u ||
        stats.young_evacuation_survivor_bytes != 0u ||
        stats.young_evacuation_promotion_bytes != 0u ||
        nggc_object_type(young_live) != type_id) {
        fail("capacity fallback preserves young survivor");
    }
    expect_ok(nggc_check(heap), "verify evacuation capacity fallback");
    expect_ok(nggc_thread_unregister(heap, thread),
              "unregister fallback mutator");
    expect_ok(nggc_root_pop(heap, (void **)&old_owner),
              "unroot fallback special owner");
    expect_ok(nggc_collect(heap), "collect fallback heap");
    expect_ok(nggc_close(heap), "close evacuation fallback heap");

    heap = 0;
    old_owner = 0;
    young_live = 0;
    type_id = NGGC_TYPE_NONE;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap),
              "open evacuation boundary heap");
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register evacuation boundary node");
    old_owner = (young_node *)nggc_alloc(heap, type_id);
    if (!old_owner) {
        fail("allocate boundary promotion candidate");
    }
    memset(old_owner, 0, sizeof(*old_owner));
    old_owner->value = 44;
    expect_ok(nggc_root_push(heap, (void **)&old_owner),
              "root boundary promotion candidate");
    expect_ok(nggc_collect_young(heap),
              "age boundary promotion candidate once");

    young_live = (young_node *)nggc_alloc_size(
        heap, type_id, (size_t)64u * 1024u - 64u);
    if (!young_live) {
        fail("allocate exact-region survivor");
    }
    memset(young_live, 0, sizeof(*young_live));
    young_live->value = 55;
    expect_ok(nggc_root_push(heap, (void **)&young_live),
              "root exact-region survivor");
    survivor_copy = old_owner;
    young_source = young_live;
    expect_ok(nggc_collect_young(heap),
              "evacuate exact-region survivor with promotion");
    expect_ok(nggc_check(heap),
              "verify exact-region survivor evacuation");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "read exact-region evacuation stats");
    if (!old_owner || old_owner == survivor_copy ||
        !young_live || young_live == young_source ||
        old_owner->value != 44 || young_live->value != 55 ||
        nggc_object_type(old_owner) != type_id ||
        nggc_object_type(young_live) != type_id ||
        stats.young_object_count != 1u ||
        stats.old_object_count != 1u ||
        stats.young_evacuation_survivor_bytes !=
            (size_t)64u * 1024u ||
        stats.young_evacuation_promotion_bytes == 0u ||
        stats.young_evacuation_capacity_fallback_count != 0u) {
        fail("exact-region survivor does not skip an extra region");
    }
    expect_ok(nggc_root_pop(heap, (void **)&young_live),
              "unroot exact-region survivor");
    expect_ok(nggc_root_pop(heap, (void **)&old_owner),
              "unroot boundary promoted object");
    expect_ok(nggc_collect(heap), "collect evacuation boundary heap");
    expect_ok(nggc_close(heap), "close evacuation boundary heap");
    run_external_collection_set_edge_case(&desc);
    run_delayed_from_space_reclaim_case(&desc);
    run_read_barrier_slot_case((size_t)512u);
    run_read_barrier_slot_case((size_t)24u * 1024u);
    run_read_barrier_slot_case((size_t)128u * 1024u);
    run_evacuation_worker_case(&desc);
    run_internal_reference_evacuation_case();
    run_in_place_region_promotion_case(&desc);
    return 0;
}
