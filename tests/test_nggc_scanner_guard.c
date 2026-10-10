#include "nggc/nggc.h"
#include "nggc_internal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NGGC_HAVE_C11_THREADS
#define NGGC_HAVE_C11_THREADS 0
#endif
#if NGGC_HAVE_C11_THREADS
#include <threads.h>
#endif

enum { OBJECTS = 64, PUBLIC_APIS = 54, CROSS_HEAP_STATS = 55,
       CROSS_HEAP_CLOSE = 56, BAD_POLL_CONTEXT = 57 };

typedef struct guard_fixture guard_fixture;
typedef struct guard_node {
    struct guard_node *edge;
    guard_fixture *fixture;
    unsigned kind;
    unsigned value;
} guard_node;

struct guard_fixture {
    nggc_heap *heap;
    nggc_heap *other_heap;
    nggc_thread *thread;
    guard_node *nodes[OBJECTS];
    nggc_type_desc desc;
    uint16_t types[2];
    unsigned target_kind;
    uint32_t target_phase;
    unsigned only_background;
    unsigned return_error;
    atomic_uint action;
    atomic_uint nested_ok;
    atomic_uint triggered;
#if NGGC_HAVE_C11_THREADS
    thrd_t coordinator;
    atomic_uint *overlap;
    unsigned overlap_bit;
    atomic_uint overlapped;
#endif
};

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

static void require_ok(nggc_result result, const char *message) {
    if (result != NGGC_OK) {
        fprintf(stderr, "%s: %d\n", message, (int)result);
        exit(1);
    }
}

static int call_api(guard_fixture *fixture, guard_node *node,
                    const nggc_scan_control *control, unsigned action) {
    nggc_heap *heap = fixture->heap;
    nggc_thread *thread = fixture->thread;
    nggc_heap *out_heap = fixture->other_heap;
    nggc_thread *out_thread = thread;
    void *out_object = node;
    size_t out_count = SIZE_MAX;
    uint16_t out_type = NGGC_TYPE_NONE;
    uint32_t out_state = UINT32_MAX;
    nggc_result out_reason = NGGC_ERR_BACKEND;
    nggc_stats stats;
    nggc_result result = NGGC_OK;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    switch (action) {
        case 1: result = nggc_open(0, &out_heap); break;
        case 2: result = nggc_close(heap); break;
        case 3: result = nggc_get_heap_state(heap, &out_state, &out_reason); break;
        case 4: result = nggc_force_destroy(heap); break;
        case 5: result = nggc_register_type(heap, &fixture->desc, &out_type); break;
        case 6: result = nggc_unregister_type(heap, fixture->types[0]); break;
        case 7: result = nggc_type_table_freeze(heap); break;
        case 8: return nggc_alloc(heap, fixture->types[0]) == 0;
        case 9: return nggc_alloc_size(heap, fixture->types[0], sizeof(*node)) == 0;
        case 10: return nggc_thread_alloc(heap, thread, fixture->types[0]) == 0;
        case 11: return nggc_thread_alloc_size(heap, thread, fixture->types[0], sizeof(*node)) == 0;
        case 12: result = nggc_weak_ref_create(heap, node, &out_object); break;
        case 13: result = nggc_weak_ref_get(heap, node, &out_object); break;
        case 14: result = nggc_soft_ref_create(heap, node, &out_object); break;
        case 15: result = nggc_soft_ref_get(heap, node, &out_object); break;
        case 16: result = nggc_phantom_queue_create(heap, &out_object); break;
        case 17: result = nggc_phantom_ref_create(heap, node, node, &out_object); break;
        case 18: result = nggc_phantom_queue_count(heap, node, &out_count); break;
        case 19: result = nggc_phantom_queue_pop(heap, node, &out_object); break;
        case 20: result = nggc_phantom_reaper_watch(heap, node); break;
        case 21: result = nggc_phantom_reaper_unwatch(heap, node); break;
        case 22: result = nggc_phantom_reaper_poll(heap, &out_object, &out_count); break;
        case 23: result = nggc_phantom_reaper_wait(heap, 1u, &out_object, &out_count); break;
        case 24: result = nggc_phantom_reaper_wait_blocking(heap, &out_object, &out_count); break;
        case 25: result = nggc_phantom_reaper_pop(heap, &out_object, &out_object); break;
        case 26: result = nggc_root_push(heap, (void **)&fixture->nodes[0]); break;
        case 27: result = nggc_root_pop(heap, (void **)&fixture->nodes[0]); break;
        case 28: result = nggc_thread_register(heap, &out_thread); break;
        case 29: result = nggc_thread_unregister(heap, thread); break;
        case 30: result = nggc_thread_root_push(heap, thread, (void **)&fixture->nodes[0]); break;
        case 31: result = nggc_thread_root_pop(heap, thread, (void **)&fixture->nodes[0]); break;
        case 32: result = nggc_request_safepoint(heap); break;
        case 33: result = nggc_safepoint(heap, thread); break;
        case 34: result = nggc_safepoint_park(heap, thread); break;
        case 35: result = nggc_safepoint_wait(heap, &out_count); break;
        case 36: result = nggc_safepoint_release(heap); break;
        case 37: result = nggc_phase_begin(heap, NGGC_PHASE_MARK); break;
        case 38: result = nggc_phase_end(heap, NGGC_PHASE_MARK); break;
        case 39: return nggc_load(heap, (void **)&node->edge) == 0;
        case 40: result = nggc_store(heap, node, (void **)&node->edge, node); break;
        case 41: return nggc_object_type(node) == NGGC_TYPE_NONE;
        case 42: return nggc_object_size(node) == 0u;
        case 43: return nggc_object_flags(node) == 0u;
        case 44: return nggc_object_pin_count(node) == 0u;
        case 45: result = nggc_pin(heap, node); break;
        case 46: result = nggc_unpin(heap, node); break;
        case 47: result = nggc_collect(heap); break;
        case 48: result = nggc_collect_if_needed(heap, 1u); break;
        case 49: result = nggc_thread_collect(heap, thread); break;
        case 50: result = nggc_collect_young(heap); break;
        case 51: result = nggc_thread_collect_young(heap, thread); break;
        case 52: result = nggc_check(heap); break;
        case 53: result = nggc_get_stats(heap, &stats); break;
        case 54: result = nggc_get_last_error(heap); break;
        case CROSS_HEAP_STATS: result = nggc_get_stats(fixture->other_heap, &stats); break;
        case CROSS_HEAP_CLOSE: result = nggc_close(fixture->other_heap); break;
        case BAD_POLL_CONTEXT:
            return control && control->poll(fixture) == NGGC_ERR_CORRUPTION;
        default: require(0, "valid scanner reentry test action"); break;
    }
    return result == NGGC_ERR_BUSY && out_heap == fixture->other_heap &&
           out_thread == thread && out_object == node && out_count == SIZE_MAX &&
           out_type == NGGC_TYPE_NONE && out_state == UINT32_MAX &&
           out_reason == NGGC_ERR_BACKEND && stats.live_object_count == 0u;
}

static int scan_common(void *object, nggc_visit_fn visit, void *context,
                       const nggc_scan_control *control) {
    guard_node *node = (guard_node *)object;
    guard_fixture *fixture = node->fixture;
    uint32_t phase = NGGC_PHASE_IDLE;
    unsigned action;
    int return_error = 0;
    require_ok(nggc_internal_test_scanner_phase(&phase),
               "scanner diagnostic observes its own stable phase");
    if (control) {
        require_ok(control->poll(control->ctx), "normal scanner poll is permitted");
    }
#if NGGC_HAVE_C11_THREADS
    if (fixture->overlap && phase == NGGC_PHASE_MARK &&
        atomic_exchange_explicit(&fixture->overlapped, 1u,
                                 memory_order_acq_rel) == 0u) {
        atomic_fetch_or_explicit(fixture->overlap, fixture->overlap_bit,
                                 memory_order_release);
        while (atomic_load_explicit(fixture->overlap,
                                    memory_order_acquire) != 3u) {
            thrd_yield();
        }
        if (control) {
            require_ok(control->poll(control->ctx),
                       "overlapping heaps keep distinct scanner poll contexts");
        }
    }
#endif
    action = atomic_load_explicit(&fixture->action, memory_order_acquire);
    if (action != 0u && node->kind == fixture->target_kind &&
        (fixture->target_phase == UINT32_MAX || phase == fixture->target_phase)) {
#if NGGC_HAVE_C11_THREADS
        if (fixture->only_background &&
            thrd_equal(thrd_current(), fixture->coordinator)) {
            while (atomic_load_explicit(&fixture->triggered,
                                        memory_order_acquire) == 0u) {
                thrd_yield();
            }
        } else
#endif
        if (atomic_compare_exchange_strong_explicit(
                &fixture->action, &action, 0u,
                memory_order_acq_rel, memory_order_acquire)) {
            int rejected = call_api(fixture, node, control, action);
            if (control && action != BAD_POLL_CONTEXT) {
                rejected = rejected &&
                    control->poll(control->ctx) == NGGC_ERR_CORRUPTION;
            }
            atomic_store_explicit(&fixture->nested_ok, rejected ? 1u : 0u,
                                  memory_order_release);
            atomic_store_explicit(&fixture->triggered, 1u,
                                  memory_order_release);
            return_error = fixture->return_error != 0u;
        }
    }
    visit(context, (void **)&node->edge);
    return return_error;
}

static void scan_legacy(void *object, nggc_visit_fn visit, void *context) {
    (void)scan_common(object, visit, context, 0);
}

static nggc_result scan_extended(void *object, nggc_visit_fn visit,
                                 void *context,
                                 const nggc_scan_control *control) {
    return scan_common(object, visit, context, control)
               ? NGGC_ERR_OUT_OF_MEMORY : NGGC_OK;
}

static nggc_stats stats_of(nggc_heap *heap) {
    nggc_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    require_ok(nggc_get_stats(heap, &stats), "read scanner guard stats outside callback");
    return stats;
}

static void fixture_open(guard_fixture *fixture) {
    nggc_config config;
    memset(fixture, 0, sizeof(*fixture));
    atomic_init(&fixture->action, 0u);
    atomic_init(&fixture->nested_ok, 0u);
    atomic_init(&fixture->triggered, 0u);
#if NGGC_HAVE_C11_THREADS
    fixture->coordinator = thrd_current();
    atomic_init(&fixture->overlapped, 0u);
#endif
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT;
    config.region_size = (size_t)24u * 1024u;
    config.gc_arena_size = config.region_size * 16u;
    config.uhm_arena_size = (size_t)4u * 1024u * 1024u;
    require_ok(nggc_open(&config, &fixture->heap), "open scanner guard heap");
    require_ok(nggc_open(&config, &fixture->other_heap), "open scanner cross-call heap");
    fixture->desc.size = sizeof(fixture->desc);
    fixture->desc.object_size = sizeof(guard_node);
    fixture->desc.scan = scan_legacy;
    fixture->desc.name = "guard-legacy-node";
    require_ok(nggc_register_type(fixture->heap, &fixture->desc, &fixture->types[0]),
               "register guarded legacy scanner");
    fixture->desc.scan = 0;
    fixture->desc.scan_ex = scan_extended;
    fixture->desc.name = "guard-extended-node";
    require_ok(nggc_register_type(fixture->heap, &fixture->desc, &fixture->types[1]),
               "register guarded extended scanner");
    require_ok(nggc_type_table_freeze(fixture->heap), "freeze scanner guard types");
    require_ok(nggc_thread_register(fixture->heap, &fixture->thread),
               "register scanner guard owner");
    for (size_t i = 0u; i < OBJECTS; ++i) {
        fixture->nodes[i] = (guard_node *)nggc_thread_alloc(
            fixture->heap, fixture->thread, fixture->types[i % 2u]);
        require(fixture->nodes[i] != 0, "allocate scanner guard object");
        fixture->nodes[i]->edge = fixture->nodes[i];
        fixture->nodes[i]->fixture = fixture;
        fixture->nodes[i]->kind = (unsigned)(i % 2u + 1u);
        fixture->nodes[i]->value = (unsigned)(i + 71u);
        require_ok(nggc_thread_root_push(
                       fixture->heap, fixture->thread, (void **)&fixture->nodes[i]),
                   "root scanner guard object");
    }
}

static void fixture_close(guard_fixture *fixture) {
    for (size_t i = OBJECTS; i != 0u; --i) {
        require_ok(nggc_thread_root_pop(
                       fixture->heap, fixture->thread, (void **)&fixture->nodes[i - 1u]),
                   "unroot scanner guard object");
    }
    require_ok(nggc_thread_unregister(fixture->heap, fixture->thread),
               "unregister scanner guard owner");
    require_ok(nggc_collect(fixture->heap), "reclaim scanner guard objects");
    require_ok(nggc_close(fixture->heap), "close scanner guard heap");
    require_ok(nggc_close(fixture->other_heap), "close scanner cross-call heap");
}

static void arm(guard_fixture *fixture, unsigned action, unsigned kind,
                 uint32_t phase, unsigned only_background) {
    fixture->target_kind = kind;
    fixture->target_phase = phase;
    fixture->only_background = only_background;
    atomic_store_explicit(&fixture->nested_ok, 0u, memory_order_release);
    atomic_store_explicit(&fixture->triggered, 0u, memory_order_release);
    atomic_store_explicit(&fixture->action, action, memory_order_release);
}

static void verify_triggered(guard_fixture *fixture) {
    require(atomic_load_explicit(&fixture->triggered, memory_order_acquire) == 1u &&
                atomic_load_explicit(&fixture->nested_ok, memory_order_acquire) == 1u,
            "scanner API is rejected before outputs or heap state change");
    require_ok(nggc_check(fixture->heap), "scanner TLS clears after a failed callback");
    require_ok(nggc_check(fixture->other_heap), "cross-heap rejection preserves other heap");
}

static void run_api_matrix(void) {
    guard_fixture fixture;
    fixture_open(&fixture);
    for (unsigned kind = 1u; kind <= 2u; ++kind) {
        for (unsigned action = 1u; action <= CROSS_HEAP_CLOSE; ++action) {
            nggc_result result;
            nggc_stats before = stats_of(fixture.heap);
            arm(&fixture, action, kind, UINT32_MAX, 0u);
            result = nggc_check(fixture.heap);
            if (result != NGGC_ERR_CORRUPTION) {
                fprintf(stderr, "scanner kind %u API case %u result %d\n",
                        kind, action, (int)result);
                exit(1);
            }
            verify_triggered(&fixture);
            {
                nggc_stats after = stats_of(fixture.heap);
                require(before.allocation_count == after.allocation_count &&
                            before.registered_thread_count == after.registered_thread_count &&
                            before.root_count == after.root_count &&
                            after.gc_phase == NGGC_PHASE_IDLE &&
                            after.safepoint_active == 0u,
                        "rejected scanner APIs do not mutate runtime ownership");
            }
        }
    }
    arm(&fixture, BAD_POLL_CONTEXT, 2u, UINT32_MAX, 0u);
    require(nggc_check(fixture.heap) == NGGC_ERR_CORRUPTION,
            "wrong poll context is rejected without dereferencing it");
    verify_triggered(&fixture);
    fixture.return_error = 1u;
    arm(&fixture, PUBLIC_APIS, 2u, UINT32_MAX, 0u);
    require(nggc_check(fixture.heap) == NGGC_ERR_CORRUPTION,
            "callback error cannot hide a latched protocol violation");
    verify_triggered(&fixture);
    fixture.return_error = 0u;

    for (unsigned kind = 1u; kind <= 2u; ++kind) {
        arm(&fixture, 53u, kind, NGGC_PHASE_MARK, 0u);
        require(nggc_thread_collect(fixture.heap, fixture.thread) == NGGC_ERR_CORRUPTION,
                "mark scanner reentry aborts collection without self-deadlock");
        verify_triggered(&fixture);
        require(stats_of(fixture.heap).safepoint_active == 0u,
                "mark scanner violation releases the held safepoint");
    }
    fixture_close(&fixture);
}

#if NGGC_HAVE_C11_THREADS
static void run_background_remap_case(unsigned kind) {
    guard_fixture fixture;
    guard_node *sources[OBJECTS];
    nggc_stats stats;
    fixture_open(&fixture);
    memcpy(sources, fixture.nodes, sizeof(sources));
    arm(&fixture, 53u, kind, NGGC_PHASE_REMAP, 1u);
    require(nggc_thread_collect_young(fixture.heap, fixture.thread) == NGGC_ERR_CORRUPTION,
            "background REMAP scanner rejects API reentry before coordinator lock");
    verify_triggered(&fixture);
    for (size_t i = 0u; i < OBJECTS; ++i) {
        require(fixture.nodes[i] != sources[i] &&
                    fixture.nodes[i]->edge == fixture.nodes[i] &&
                    fixture.nodes[i]->value == i + 71u,
                "post-forwarding scanner violation completes canonical object graph");
    }
    stats = stats_of(fixture.heap);
    require(stats.gc_phase == NGGC_PHASE_IDLE && stats.safepoint_active == 0u &&
                stats.remap_workers_active == 0u &&
                stats.evacuation_workers_active == 0u &&
                stats.young_evacuation_emergency_completion_count == 1u &&
                stats.remap_worker_dispatch_count != 0u,
            "worker-side scanner violation quiesces and completes emergency recovery");
    require_ok(nggc_thread_collect_young(fixture.heap, fixture.thread),
               "same persistent workers scan normally on the next collection");
    fixture_close(&fixture);
}

typedef struct overlap_args {
    atomic_uint *overlap;
    unsigned bit;
} overlap_args;

static int overlap_main(void *context) {
    overlap_args *args = (overlap_args *)context;
    guard_fixture fixture;
    fixture_open(&fixture);
    fixture.overlap = args->overlap;
    fixture.overlap_bit = args->bit;
    require_ok(nggc_thread_collect(fixture.heap, fixture.thread),
               "two heap scanners overlap with independent thread-local guards");
    require(atomic_load_explicit(&fixture.overlapped, memory_order_acquire) == 1u,
            "overlapping scanner actually ran");
    fixture.overlap = 0;
    fixture_close(&fixture);
    return 0;
}

static void run_overlap_case(void) {
    atomic_uint overlap;
    overlap_args args[2];
    thrd_t threads[2];
    atomic_init(&overlap, 0u);
    for (size_t i = 0u; i < 2u; ++i) {
        args[i].overlap = &overlap;
        args[i].bit = (unsigned)(1u << i);
        require(thrd_create(&threads[i], overlap_main, &args[i]) == thrd_success,
                "start independent heap scanner");
    }
    for (size_t i = 0u; i < 2u; ++i) {
        require(thrd_join(threads[i], 0) == thrd_success, "join independent heap scanner");
    }
    require(atomic_load_explicit(&overlap, memory_order_acquire) == 3u,
            "both scanner contexts overlapped");
}
#endif

int main(void) {
    uint32_t phase = NGGC_PHASE_IDLE;
    require(nggc_internal_test_scanner_phase(&phase) == NGGC_ERR_INVALID_ARGUMENT,
            "scanner-only diagnostic is unavailable outside a callback");
    run_api_matrix();
#if NGGC_HAVE_C11_THREADS
    run_background_remap_case(1u);
    run_background_remap_case(2u);
    run_overlap_case();
#endif
    return 0;
}
