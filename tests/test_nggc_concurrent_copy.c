#include "nggc/nggc.h"
#include "nggc_internal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

enum { MUTATORS = 4, NODES = 8, ROUNDS = 3, PRESSURE_WRITES = 256 };

typedef struct copy_node {
    struct copy_node *edges[MUTATORS];
    unsigned char bytes[1024];
} copy_node;

typedef struct copy_shared {
    nggc_heap *heap;
    copy_node *pinned;
    uint16_t type_id;
    atomic_uint round;
    atomic_uint stop;
} copy_shared;

typedef struct copy_mutator {
    copy_shared *shared;
    size_t index;
    copy_node *nodes[NODES];
    copy_node *contended_source;
    copy_node *contended_referent;
    atomic_uint ready;
    atomic_uint resumed;
    atomic_uint command;
    atomic_uint stored;
    atomic_uint pressured;
    atomic_uint released;
} copy_mutator;

typedef struct copy_collector {
    nggc_heap *heap;
    nggc_result result;
} copy_collector;

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

static void await_value(atomic_uint *value, unsigned expected,
                        const char *message) {
    for (size_t i = 0u; i < 2000000u; ++i) {
        if (atomic_load_explicit(value, memory_order_acquire) == expected) {
            return;
        }
        thrd_yield();
    }
    require(0, message);
}

static void await_gate(nggc_heap *heap, uint32_t stage) {
    for (size_t i = 0u; i < 2000000u; ++i) {
        nggc_result result = stage == 0u
            ? nggc_internal_test_concurrent_window_wait(heap)
            : nggc_internal_test_concurrent_copy_wait(heap, stage);
        if (result == NGGC_OK) {
            return;
        }
        require(result == NGGC_ERR_BUSY, "valid copy gate wait");
        thrd_yield();
    }
    fprintf(stderr, "concurrent copy did not reach gate %u\n", stage);
    require(0, "concurrent copy reaches requested gate");
}

static nggc_stats read_stats(nggc_heap *heap) {
    nggc_stats stats;
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    require_ok(nggc_get_stats(heap, &stats), "read concurrent copy stats");
    return stats;
}

static void scan_node(void *object, nggc_visit_fn visit, void *context) {
    copy_node *node = (copy_node *)object;
    for (size_t i = 0u; i < MUTATORS; ++i) {
        visit(context, (void **)&node->edges[i]);
    }
}

static int collect_main(void *context) {
    copy_collector *collector = (copy_collector *)context;
    do {
        collector->result = nggc_collect_young(collector->heap);
        if (collector->result == NGGC_ERR_BUSY) {
            thrd_yield();
        }
    } while (collector->result == NGGC_ERR_BUSY);
    if (collector->result != NGGC_OK) {
        fprintf(stderr, "concurrent collector failed: %d\n",
                (int)collector->result);
    }
    return 0;
}

static int mutator_main(void *context) {
    copy_mutator *mutator = (copy_mutator *)context;
    copy_shared *shared = mutator->shared;
    nggc_thread *thread = 0;
    require_ok(nggc_thread_register(shared->heap, &thread),
               "mutator registers on its native owner");
    for (size_t i = 0u; i < NODES; ++i) {
        mutator->nodes[i] = (copy_node *)nggc_thread_alloc(
            shared->heap, thread, shared->type_id);
        require(mutator->nodes[i] != 0, "allocate concurrent copy node");
        memset(mutator->nodes[i], 0, sizeof(copy_node));
        memset(mutator->nodes[i]->bytes,
               (int)(1u + mutator->index * NODES + i),
               sizeof(mutator->nodes[i]->bytes));
        mutator->nodes[i]->edges[0] = mutator->nodes[i];
        require_ok(nggc_store(shared->heap, mutator->nodes[i],
                              (void **)&mutator->nodes[i]->edges[2], shared->pinned),
                   "initialize cross-region pinned edge through write barrier");
        require_ok(nggc_thread_root_push(
                       shared->heap, thread, (void **)&mutator->nodes[i]),
                   "publish owner thread root");
    }
    atomic_store_explicit(&mutator->ready, 1u, memory_order_release);

    while (atomic_load_explicit(&shared->stop, memory_order_acquire) == 0u) {
        unsigned round;
        nggc_stats stats;
        require_ok(nggc_safepoint_park(shared->heap, thread),
                   "mutator cooperates with park/resume/repark");
        if (atomic_load_explicit(&shared->stop, memory_order_acquire) != 0u) {
            break;
        }
        round = atomic_load_explicit(&shared->round, memory_order_acquire);
        stats = read_stats(shared->heap);
        if (stats.mutator_concurrent_active != 0u &&
            atomic_load_explicit(&mutator->resumed, memory_order_acquire) !=
                round) {
            require_ok(nggc_store(shared->heap, mutator->nodes[0],
                                  (void **)&mutator->nodes[0]->edges[1],
                                  mutator->nodes[1]),
                       "write source payload before copying begins");
            require_ok(nggc_store(shared->heap, shared->pinned,
                                  (void **)&shared->pinned->edges[mutator->index],
                                  mutator->nodes[0]),
                       "pinned owner records edge to a movable young object");
            atomic_store_explicit(&mutator->resumed, round,
                                  memory_order_release);
            /* The test coordinator holds the copy window until these
               scripted barrier operations finish; no safepoint is missed. */
            while (atomic_load_explicit(&mutator->command,
                                        memory_order_acquire) == 0u) {
                thrd_yield();
            }
            if (atomic_load_explicit(&mutator->command,
                                     memory_order_acquire) != 2u) {
                void *owner_alias = mutator->contended_source;
                copy_node *canonical_owner;
                if (atomic_load_explicit(&mutator->command,
                                         memory_order_acquire) == 3u) {
                    void *expected = mutator->contended_referent;
                    void *loaded = nggc_load(
                        shared->heap,
                        (void **)&mutator->contended_source->edges[0]);
                    require(loaded == nggc_load(shared->heap, &expected) &&
                                nggc_load(shared->heap, &owner_alias) !=
                                    mutator->contended_source,
                            "load racing copy waits and follows published forwarding");
                }
                require_ok(nggc_store(
                               shared->heap, mutator->contended_source,
                               (void **)&mutator->contended_source->edges[0],
                               shared->pinned),
                           "store racing copy rechecks forwarding after locking");
                require(nggc_load(
                            shared->heap,
                            (void **)&mutator->contended_source->edges[0]) ==
                            shared->pinned,
                        "post-forwarding load observes the contending store");
                canonical_owner = (copy_node *)nggc_load(
                    shared->heap, &owner_alias);
                require(canonical_owner != mutator->contended_source,
                        "selected owner has published a canonical destination");
                require_ok(nggc_store(
                               shared->heap, canonical_owner,
                               (void **)&canonical_owner->edges[2], 0),
                           "canonical write clears only destination field");
                {
                    nggc_stats before = read_stats(shared->heap);
                    void *loaded = nggc_load(shared->heap,
                        (void **)&mutator->contended_source->edges[2]);
                    nggc_stats after = read_stats(shared->heap);
                    /* Other mutators are gated here. The count proves the
                       null read passed owner/slot validation; last_error is
                       a sticky heap-wide value from earlier BUSY attempts. */
                    require(mutator->contended_source->edges[2] == shared->pinned &&
                                loaded == 0 &&
                                after.mutator_concurrent_load_count ==
                                    before.mutator_concurrent_load_count + 1u,
                            "concurrent stale-owner load reads canonical null field");
                }
                require_ok(nggc_store(
                               shared->heap, canonical_owner,
                               (void **)&canonical_owner->edges[2], canonical_owner),
                           "canonical write replaces destination referent");
                require(nggc_load(shared->heap,
                            (void **)&mutator->contended_source->edges[2]) ==
                                canonical_owner,
                        "concurrent stale-owner load reads canonical replacement");
                require_ok(nggc_store(
                               shared->heap, canonical_owner,
                               (void **)&canonical_owner->edges[2], shared->pinned),
                           "restore canonical field after stale-owner reads");
                atomic_store_explicit(&mutator->stored, round,
                                      memory_order_release);
                while (atomic_load_explicit(&mutator->command,
                                            memory_order_acquire) != 2u) {
                    thrd_yield();
                }
            }
            for (size_t i = 0u; i < PRESSURE_WRITES; ++i) {
                copy_node *owner = mutator->nodes[i % NODES];
                void *target = mutator->nodes[NODES - 1u];
                require_ok(nggc_store(shared->heap, owner,
                                      (void **)&owner->edges[1], target),
                           "store through potentially forwarded owner");
                {
                    void *loaded = nggc_load(shared->heap,
                                             (void **)&owner->edges[1]);
                    void *canonical = nggc_load(shared->heap, &target);
                    /* Forwarding may publish between the two loads.  An
                       earlier load may legally return the original source;
                       the final parked graph check requires canonical slots. */
                    require(loaded != 0 && (loaded == target || loaded == canonical),
                            "load preserves referent identity across forwarding");
                }
            }
            require_ok(nggc_store(shared->heap, shared->pinned,
                                  (void **)&shared->pinned->edges[mutator->index],
                                  mutator->nodes[0]),
                       "pinned owner records edge after forwarding publication");
            atomic_store_explicit(&mutator->pressured, round,
                                  memory_order_release);
        } else if (round != 0u && stats.safepoint_active == 0u &&
                   atomic_load_explicit(&mutator->pressured,
                                        memory_order_acquire) == round) {
            atomic_store_explicit(&mutator->released, round,
                                  memory_order_release);
        }
        thrd_yield();
    }

    for (size_t i = NODES; i != 0u; --i) {
        require_ok(nggc_thread_root_pop(
                       shared->heap, thread, (void **)&mutator->nodes[i - 1u]),
                   "remove owner thread root");
    }
    require_ok(nggc_thread_unregister(shared->heap, thread),
               "mutator unregisters on its native owner");
    return 0;
}

int main(void) {
    const size_t region_size = (size_t)24u * 1024u;
    copy_shared shared;
    copy_mutator mutators[MUTATORS];
    copy_collector collector;
    thrd_t threads[MUTATORS];
    thrd_t collector_thread;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    copy_node *sources[MUTATORS][NODES];
    unsigned selected[MUTATORS][NODES] = {{0}};
    uint16_t pinned_type = NGGC_TYPE_NONE;
    memset(&shared, 0, sizeof(shared));
    atomic_init(&shared.round, 0u);
    atomic_init(&shared.stop, 0u);
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.flags = NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT;
    config.region_size = region_size;
    config.gc_arena_size = region_size * 64u;
    config.uhm_arena_size = (size_t)8u * 1024u * 1024u;
    config.young_tenuring_threshold = ROUNDS;
    require_ok(nggc_open(&config, &shared.heap), "open concurrent copy heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = sizeof(copy_node);
    desc.scan = scan_node;
    desc.name = "concurrent-copy-node";
    require_ok(nggc_register_type(shared.heap, &desc, &shared.type_id),
               "register concurrent copy type");
    desc.flags = NGGC_TYPE_FLAG_PINNABLE;
    desc.name = "concurrent-copy-pinned-node";
    require_ok(nggc_register_type(shared.heap, &desc, &pinned_type),
               "register concurrent copy pinned type");
    require_ok(nggc_type_table_freeze(shared.heap), "freeze copy type table");
    shared.pinned = (copy_node *)nggc_alloc(shared.heap, pinned_type);
    require(shared.pinned != 0, "allocate pinned barrier owner");
    memset(shared.pinned, 0, sizeof(copy_node));
    require_ok(nggc_root_push(shared.heap, (void **)&shared.pinned),
               "root pinned barrier owner");
    require_ok(nggc_pin(shared.heap, shared.pinned), "pin barrier owner");
    for (size_t i = 0u; i < MUTATORS; ++i) {
        memset(&mutators[i], 0, sizeof(mutators[i]));
        mutators[i].shared = &shared;
        mutators[i].index = i;
        atomic_init(&mutators[i].ready, 0u);
        atomic_init(&mutators[i].resumed, 0u);
        atomic_init(&mutators[i].command, 0u);
        atomic_init(&mutators[i].stored, 0u);
        atomic_init(&mutators[i].pressured, 0u);
        atomic_init(&mutators[i].released, 0u);
        require(thrd_create(&threads[i], mutator_main, &mutators[i]) ==
                    thrd_success,
                "start native owner mutator");
    }
    for (size_t i = 0u; i < MUTATORS; ++i) {
        await_value(&mutators[i].ready, 1u, "mutator initialization completes");
    }

    for (unsigned round = 1u; round <= ROUNDS; ++round) {
        void *source = 0;
        size_t selected_mutator = MUTATORS;
        copy_node *pinned_address = shared.pinned;
        for (size_t i = 0u; i < MUTATORS; ++i) {
            memcpy(sources[i], mutators[i].nodes, sizeof(sources[i]));
            atomic_store_explicit(&mutators[i].command, 0u,
                                  memory_order_release);
        }
        atomic_store_explicit(&shared.round, round, memory_order_release);
        require_ok(nggc_internal_test_collect_fault_set(
                       shared.heap, NGGC_TEST_COLLECT_ACTION_CONCURRENT_COPY),
                   "arm concurrent copy interleaving");
        collector.heap = shared.heap;
        collector.result = NGGC_ERR_BUSY;
        require(thrd_create(&collector_thread, collect_main, &collector) ==
                    thrd_success,
                "start concurrent collector");
        await_gate(shared.heap, 0u);
        for (size_t i = 0u; i < MUTATORS; ++i) {
            await_value(&mutators[i].resumed, round,
                        "all native mutators really resume before copying");
        }
        require_ok(nggc_internal_test_concurrent_window_release(shared.heap),
                   "start real parallel payload copying");
        await_gate(shared.heap, 1u);
        require_ok(nggc_internal_test_concurrent_copy_source(shared.heap, &source),
                   "observe source held by an active copy worker");
        for (size_t i = 0u; i < MUTATORS; ++i) {
            for (size_t j = 0u; j < NODES; ++j) {
                if (sources[i][j] == source) {
                    selected_mutator = i;
                    selected[i][j] = 1u;
                    mutators[i].contended_referent = sources[i][j]->edges[0];
                }
            }
        }
        require(selected_mutator < MUTATORS, "paused source belongs to a mutator");
        mutators[selected_mutator].contended_source = (copy_node *)source;
        atomic_store_explicit(&mutators[selected_mutator].command,
                              round % 2u == 0u ? 3u : 1u,
                              memory_order_release);
        {
            int entered = 0;
            for (size_t i = 0u; i < 2000000u; ++i) {
                nggc_result result = round % 2u == 0u
                    ? nggc_internal_test_concurrent_copy_load_wait(shared.heap)
                    : nggc_internal_test_concurrent_copy_store_wait(shared.heap);
                if (result ==
                        NGGC_OK) {
                    entered = 1;
                    break;
                }
                thrd_yield();
            }
            require(entered, "barrier reaches a payload lock held by a copy worker");
        }
        require(atomic_load_explicit(&mutators[selected_mutator].stored,
                                     memory_order_acquire) != round,
                "contending barrier cannot finish before source copy");
        require_ok(nggc_internal_test_concurrent_copy_release(shared.heap, 1u),
                   "copy and publish forwarding while barrier is waiting");
        await_gate(shared.heap, 2u);
        await_value(&mutators[selected_mutator].stored, round,
                    "contending barrier follows and updates canonical destination");
        for (size_t i = 0u; i < MUTATORS; ++i) {
            atomic_store_explicit(&mutators[i].command, 2u,
                                  memory_order_release);
        }
        for (size_t i = 0u; i < MUTATORS; ++i) {
            await_value(&mutators[i].pressured, round,
                        "all resumed mutators complete concurrent barrier pressure");
        }
        require_ok(nggc_internal_test_concurrent_copy_release(shared.heap, 2u),
                   "quiesce workers and request cooperative repark");
        await_gate(shared.heap, 3u);
        {
            int blocked = 0;
            for (size_t i = 0u; i < 2000000u; ++i) {
                if (nggc_internal_test_safepoint_blocked_threads(shared.heap) ==
                        MUTATORS) {
                    blocked = 1;
                    break;
                }
                thrd_yield();
            }
            require(blocked, "reparked mutators block instead of reusing resume epoch");
        }
        stats = read_stats(shared.heap);
        require(stats.gc_phase == NGGC_PHASE_EVACUATE &&
                    stats.safepoint_parked_threads == MUTATORS &&
                    stats.mutator_concurrent_active == 0u &&
                    stats.mutator_concurrent_running_threads == 0u &&
                    stats.remset_buffered_card_count == 0u &&
                    stats.remset_buffer_inflight_thread_count == 0u &&
                    stats.remset_classification_pending_thread_count == 0u,
                "repark closes thread and remembered-set publication before REMAP");
        require_ok(nggc_internal_test_concurrent_copy_release(shared.heap, 3u),
                   "enter parked parallel REMAP and final release");
        require(thrd_join(collector_thread, 0) == thrd_success,
                "join concurrent collector");
        require_ok(collector.result, "concurrent copy collection succeeds");
        for (size_t i = 0u; i < MUTATORS; ++i) {
            await_value(&mutators[i].released, round,
                        "mutator observes final safepoint release");
            require(shared.pinned->edges[i] == mutators[i].nodes[0],
                    "pinned owner edge remaps to canonical thread-root object");
            for (size_t j = 0u; j < NODES; ++j) {
                copy_node *node = mutators[i].nodes[j];
                uint32_t generation = NGGC_REGION_GENERATION_NONE;
                require(node != sources[i][j] &&
                            node->edges[0] == (selected[i][j] ? shared.pinned : node) &&
                            node->edges[1] == mutators[i].nodes[NODES - 1u] &&
                            node->edges[2] == shared.pinned,
                        "thread roots and racing stores survive parallel REMAP");
                for (size_t k = 0u; k < sizeof(node->bytes); ++k) {
                    require(node->bytes[k] == (unsigned char)(1u + i * NODES + j),
                            "parallel copying preserves every payload byte");
                }
                require_ok(nggc_internal_test_object_generation(
                               shared.heap, node, &generation),
                           "observe age-based copy promotion");
                require(generation == (round < ROUNDS
                            ? NGGC_REGION_GENERATION_YOUNG
                            : NGGC_REGION_GENERATION_OLD),
                        "objects become OLD only at the configured survival age");
            }
        }
        require(shared.pinned == pinned_address, "pinned object never moves");
        require_ok(nggc_check(shared.heap), "verify concurrent copy closure");
        stats = read_stats(shared.heap);
        require(stats.region_size == region_size &&
                    stats.mutator_concurrent_resume_count == round * MUTATORS &&
                    stats.mutator_concurrent_repark_count == round * MUTATORS &&
                    stats.remap_parallel_cycle_count == round &&
                    stats.safepoint_active == 0u &&
                    stats.evacuation_workers_active == 0u &&
                    stats.remap_workers_active == 0u &&
                    stats.pin_lease_count == 1u,
                "copy/repark/REMAP and pin accounting remain balanced");
    }

    atomic_store_explicit(&shared.stop, 1u, memory_order_release);
    for (size_t i = 0u; i < MUTATORS; ++i) {
        require(thrd_join(threads[i], 0) == thrd_success, "join native mutator");
    }
    require_ok(nggc_collect(shared.heap),
               "close delayed from-space quarantine before changing pin lease");
    require_ok(nggc_unpin(shared.heap, shared.pinned), "release pinned owner lease");
    require_ok(nggc_root_pop(shared.heap, (void **)&shared.pinned),
               "remove pinned owner root");
    require_ok(nggc_collect(shared.heap), "reclaim concurrent copy graph");
    require_ok(nggc_check(shared.heap), "verify concurrent copy cleanup");
    require_ok(nggc_close(shared.heap), "close concurrent copy heap");
    return 0;
}
