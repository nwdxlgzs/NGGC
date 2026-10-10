#ifndef NGGC_NGGC_H
#define NGGC_NGGC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NGGC_ABI_MAJOR 0u
#define NGGC_ABI_MINOR 88u

#define NGGC_TYPE_NONE ((uint16_t)0u)
#define NGGC_TYPE_MAX  ((uint16_t)0xffffu)

#ifndef NGGC_ENABLE_VERIFY
#define NGGC_ENABLE_VERIFY 0
#endif

#ifndef NGGC_ENABLE_ASAN_HOOKS
#define NGGC_ENABLE_ASAN_HOOKS 0
#endif

#ifndef NGGC_ENABLE_TSAN_HOOKS
#define NGGC_ENABLE_TSAN_HOOKS 0
#endif

#ifndef NGGC_ENABLE_FAULT_INJECTION
#define NGGC_ENABLE_FAULT_INJECTION 0
#endif

#ifndef NGGC_ENABLE_PROFILING
#define NGGC_ENABLE_PROFILING 0
#endif

#ifndef NGGC_ENABLE_TRACE_LOG
#define NGGC_ENABLE_TRACE_LOG 0
#endif

#if (NGGC_ENABLE_VERIFY != 0 && NGGC_ENABLE_VERIFY != 1)
#error NGGC_ENABLE_VERIFY must be 0 or 1
#endif

#if (NGGC_ENABLE_ASAN_HOOKS != 0 && NGGC_ENABLE_ASAN_HOOKS != 1)
#error NGGC_ENABLE_ASAN_HOOKS must be 0 or 1
#endif

#if (NGGC_ENABLE_TSAN_HOOKS != 0 && NGGC_ENABLE_TSAN_HOOKS != 1)
#error NGGC_ENABLE_TSAN_HOOKS must be 0 or 1
#endif

#if (NGGC_ENABLE_FAULT_INJECTION != 0 && NGGC_ENABLE_FAULT_INJECTION != 1)
#error NGGC_ENABLE_FAULT_INJECTION must be 0 or 1
#endif

#if (NGGC_ENABLE_PROFILING != 0 && NGGC_ENABLE_PROFILING != 1)
#error NGGC_ENABLE_PROFILING must be 0 or 1
#endif

#if (NGGC_ENABLE_TRACE_LOG != 0 && NGGC_ENABLE_TRACE_LOG != 1)
#error NGGC_ENABLE_TRACE_LOG must be 0 or 1
#endif

typedef struct nggc_heap nggc_heap;
typedef struct nggc_thread nggc_thread;

typedef int32_t nggc_result;

#define NGGC_OK                    ((nggc_result)0)
#define NGGC_ERR_INVALID_ARGUMENT  ((nggc_result)-1)
#define NGGC_ERR_OUT_OF_MEMORY     ((nggc_result)-2)
#define NGGC_ERR_BACKEND           ((nggc_result)-3)
#define NGGC_ERR_TYPE_FULL         ((nggc_result)-4)
#define NGGC_ERR_TYPE_NOT_FOUND    ((nggc_result)-5)
#define NGGC_ERR_TYPE_EXISTS       ((nggc_result)-6)
#define NGGC_ERR_CORRUPTION        ((nggc_result)-7)
#define NGGC_ERR_UNSUPPORTED       ((nggc_result)-8)
#define NGGC_ERR_BUSY              ((nggc_result)-9)
#define NGGC_ERR_FROZEN            ((nggc_result)-10)

#define NGGC_HEAP_STATE_RUNNING    0u
#define NGGC_HEAP_STATE_FAIL_STOP  1u

#define NGGC_TYPE_FLAG_INTERNAL    (1u << 0)
#define NGGC_TYPE_FLAG_PINNABLE    (1u << 1)
#define NGGC_TYPE_FLAG_NON_MOVABLE (1u << 2)

#define NGGC_OBJECT_FLAG_INTERNAL    (1u << 0)
#define NGGC_OBJECT_FLAG_PINNABLE    (1u << 1)
#define NGGC_OBJECT_FLAG_NON_MOVABLE (1u << 2)
#define NGGC_OBJECT_FLAG_LARGE       (1u << 3)

#define NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT   (1u << 0)
#define NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM   (1u << 1)
#define NGGC_CONFIG_COLLECT_AUTO_TRIGGER    (1u << 2)
/* Enable whole-Region in-place promotion after every live object reaches age. */
#define NGGC_CONFIG_YOUNG_IN_PLACE_PROMOTION (1u << 3)

/* Zero-initialized configuration selects the explainable SMART policy. */
#define NGGC_TRIGGER_SMART          0u
#define NGGC_TRIGGER_HEAP_LIMIT     1u
#define NGGC_TRIGGER_ALLOCATION_RATE 2u
#define NGGC_TRIGGER_LATENCY_TARGET 3u
#define NGGC_TRIGGER_THROUGHPUT     4u
#define NGGC_TRIGGER_MEMORY_SAVING  5u
#define NGGC_TRIGGER_MANUAL         6u

#define NGGC_TRIGGER_ACTION_NONE  0u
#define NGGC_TRIGGER_ACTION_YOUNG 1u
#define NGGC_TRIGGER_ACTION_FULL  2u

#define NGGC_TRIGGER_REASON_NONE          0u
#define NGGC_TRIGGER_REASON_HEAP_LIMIT    1u
#define NGGC_TRIGGER_REASON_ALLOCATION_RATE 2u
#define NGGC_TRIGGER_REASON_ASSIST_DEBT   3u
#define NGGC_TRIGGER_REASON_LATENCY       4u
#define NGGC_TRIGGER_REASON_MEMORY_PRESSURE 5u

#define NGGC_SOFT_REFERENCE_POLICY_HEAP_PRESSURE 0u
#define NGGC_SOFT_REFERENCE_POLICY_ALWAYS_RETAIN 1u
#define NGGC_SOFT_REFERENCE_POLICY_ALWAYS_CLEAR  2u
#define NGGC_SOFT_REFERENCE_RETAIN_PERCENT_DEFAULT 75u
#define NGGC_YOUNG_TENURING_THRESHOLD_DEFAULT 2u
#define NGGC_INTERIOR_DECOMMIT_MIN_REGIONS_DEFAULT ((size_t)8u)
#define NGGC_INTERIOR_DECOMMIT_DISABLED ((size_t)-1)

#define NGGC_INTERIOR_DECOMMIT_POLICY_SMART    0u
#define NGGC_INTERIOR_DECOMMIT_POLICY_MANUAL   1u
#define NGGC_INTERIOR_DECOMMIT_POLICY_DISABLED 2u

#define NGGC_INTERIOR_DECOMMIT_SMART_ACTION_HOLD     0u
#define NGGC_INTERIOR_DECOMMIT_SMART_ACTION_LOWER    1u
#define NGGC_INTERIOR_DECOMMIT_SMART_ACTION_RAISE    2u
#define NGGC_INTERIOR_DECOMMIT_SMART_ACTION_COOLDOWN 3u

#define NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_PORTABLE 0u
#define NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_CALLBACK 1u

/*
 * AUTO preserves the pre-ABI-72 source contract: an explicit tlab_size is
 * fixed, while a zero tlab_size selects the adaptive SMART default.
 * SMART treats tlab_size as the initial per-thread target. FIXED keeps every
 * registered thread at tlab_size and requires tlab_min_size/tlab_max_size
 * to be zero.
 */
#define NGGC_TLAB_SIZING_POLICY_AUTO  0u
#define NGGC_TLAB_SIZING_POLICY_SMART 1u
#define NGGC_TLAB_SIZING_POLICY_FIXED 2u

#define NGGC_MEMORY_PRESSURE_RESIDENT_VALID (1u << 0)
#define NGGC_MEMORY_PRESSURE_LIMIT_VALID    (1u << 1)
#define NGGC_MEMORY_PRESSURE_DIRECT_VALID   (1u << 2)
#define NGGC_MEMORY_PRESSURE_KNOWN_FLAGS \
    (NGGC_MEMORY_PRESSURE_RESIDENT_VALID | \
     NGGC_MEMORY_PRESSURE_LIMIT_VALID | \
     NGGC_MEMORY_PRESSURE_DIRECT_VALID)

#define NGGC_PHASE_IDLE            0u
#define NGGC_PHASE_ROOT_SNAPSHOT   1u
#define NGGC_PHASE_MARK            2u
#define NGGC_PHASE_SWEEP           3u
#define NGGC_PHASE_EVACUATE        4u
#define NGGC_PHASE_REMAP           5u
#define NGGC_PHASE_REFERENCE_PROCESS 6u

#define NGGC_REFERENCE_DISCOVERY_IDLE    0u
#define NGGC_REFERENCE_DISCOVERY_ACTIVE  1u
#define NGGC_REFERENCE_DISCOVERY_DRAINING 2u

#define NGGC_MUTATOR_ASSIST_STATE_IDLE  0u
#define NGGC_MUTATOR_ASSIST_STATE_MARK  1u
#define NGGC_MUTATOR_ASSIST_STATE_SWEEP 2u
#define NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS 3u

#define NGGC_REMSET_MODE_PRECISE_CARD 0u
#define NGGC_REMSET_MODE_COARSE_HEAP  1u

#define NGGC_REGION_STATE_INACTIVE 0u
#define NGGC_REGION_STATE_ACTIVE   1u

#define NGGC_REGION_GENERATION_NONE  0u
#define NGGC_REGION_GENERATION_YOUNG 1u
#define NGGC_REGION_GENERATION_OLD   2u
#define NGGC_REGION_GENERATION_SPECIAL 3u

#define NGGC_REGION_FLAG_HAS_LIVE        UINT32_C(0x00000001)
#define NGGC_REGION_FLAG_HAS_FREE        UINT32_C(0x00000002)
#define NGGC_REGION_FLAG_HAS_LARGE       UINT32_C(0x00000004)
#define NGGC_REGION_FLAG_HAS_PINNED      UINT32_C(0x00000008)
#define NGGC_REGION_FLAG_HAS_NON_MOVABLE UINT32_C(0x00000010)
#define NGGC_REGION_FLAG_HAS_QUARANTINE  UINT32_C(0x00000020)

typedef void (*nggc_visit_fn)(void *ctx, void **slot);

/*
 * A scan function may run concurrently for different objects. It must be
 * reentrant, synchronously enumerate only aligned strong slots in the given
 * object's payload, and must not retain visit/ctx or call back into NGGC.
 * Slot enumeration must depend on object layout, not current referent values.
 * Debug/Verify rejects same-thread public API reentry, including calls to a
 * different heap: result APIs return NGGC_ERR_BUSY, pointer APIs return NULL,
 * and object queries return zero/NGGC_TYPE_NONE without changing outputs.
 * The enclosing scan returns NGGC_ERR_CORRUPTION even if the callback ignores
 * the rejection. These diagnostics are absent when NGGC_ENABLE_VERIFY is 0.
 */
typedef void (*nggc_scan_fn)(void *object, nggc_visit_fn visit, void *ctx);

/*
 * An extended scanner may cooperatively stop by returning any result other
 * than NGGC_OK. It may call control->poll between bounded pieces of work;
 * the poll result is propagated in the same way. The control object and the
 * visitor are valid only for the duration of this call on the current thread.
 * control->poll(control->ctx) is permitted during the callback; Debug/Verify
 * also reports any previously rejected API reentry through this poll.
 */
typedef nggc_result (*nggc_scan_poll_fn)(void *ctx);

typedef struct nggc_scan_control {
    uint32_t size;
    nggc_scan_poll_fn poll;
    void *ctx;
} nggc_scan_control;

typedef nggc_result (*nggc_scan_fn_ex)(void *object,
                                       nggc_visit_fn visit,
                                       void *ctx,
                                       const nggc_scan_control *control);

typedef struct nggc_memory_pressure_sample {
    uint32_t size;
    uint32_t flags;
    size_t resident_bytes;
    size_t limit_bytes;
    uint32_t pressure_basis_points;
} nggc_memory_pressure_sample;

/*
 * The pressure callback runs synchronously during a full-GC safepoint while
 * the heap lock is held. It must not retain pointers, block indefinitely, or
 * call back into NGGC. A non-NGGC_OK result selects the portable fallback and
 * does not fail the collection.
 */
typedef nggc_result (*nggc_memory_pressure_fn)(
    void *ctx, nggc_memory_pressure_sample *sample);
/*
 * The optional clock runs under the heap lock around Region VM operations and
 * enabled collection-pause samples. It has the same non-blocking/non-reentrant
 * rules, must be monotonic, and has no wall-clock fallback.
 */
typedef nggc_result (*nggc_clock_ns_fn)(void *ctx, uint64_t *out_ns);

typedef struct nggc_config {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;

    size_t uhm_arena_size;
    size_t gc_arena_size;
    size_t tlab_size;
    uint32_t young_tenuring_threshold;

    uint32_t soft_reference_policy;
    uint32_t soft_reference_retain_percent;
    /* Zero selects 64 KiB; explicit sizes need not be powers of two. */
    size_t region_size;
    size_t interior_decommit_min_regions;
    nggc_memory_pressure_fn memory_pressure;
    void *memory_pressure_ctx;
    nggc_clock_ns_fn clock_ns;
    void *clock_ns_ctx;
    /* Nonzero enables latency policy feedback and requires clock_ns. */
    uint64_t interior_decommit_slow_ns_per_region;

    uint32_t tlab_sizing_policy;
    size_t tlab_min_size;
    size_t tlab_max_size;
    /* Zero selects min(64, card_count) entries per registered thread. */
    size_t remset_buffer_capacity;
    /* Zero selects the SMART policy, a 75% arena soft limit and one Region. */
    uint32_t gc_trigger_policy;
    size_t gc_trigger_soft_limit;
    size_t gc_trigger_quantum;
    /* Nonzero latency feedback requires the existing monotonic clock hook. */
    uint64_t gc_trigger_latency_target_ns;
} nggc_config;

typedef struct nggc_type_desc {
    uint32_t size;
    uint16_t type_id;
    uint16_t flags;

    size_t object_size;
    size_t alignment;
    nggc_scan_fn scan;
    const char *name;
    /* Optional ABI-88 scanner with explicit failure/cancellation status. */
    nggc_scan_fn_ex scan_ex;
} nggc_type_desc;

typedef struct nggc_stats {
    uint32_t size;
    uint16_t abi_major;
    uint16_t abi_minor;
    uint32_t flags;
    uint32_t soft_reference_policy;
    uint32_t soft_reference_retain_percent;
    uint32_t gc_trigger_policy;
    uint32_t gc_trigger_last_action;
    uint32_t gc_trigger_last_reason;
    uint32_t gc_trigger_pending;
    size_t gc_trigger_soft_limit;
    size_t gc_trigger_quantum;
    uint64_t gc_trigger_latency_target_ns;
    uint64_t gc_trigger_evaluation_count;
    uint64_t gc_trigger_request_count;
    uint64_t gc_trigger_collection_count;
    uint64_t gc_trigger_busy_count;
    uint64_t gc_trigger_noop_count;
    uint64_t gc_trigger_failure_count;
    size_t gc_trigger_last_used;
    size_t gc_trigger_last_request_bytes;
    size_t gc_trigger_last_projected_used;
    uint64_t gc_trigger_last_pause_ns;
    uint64_t gc_trigger_pause_ewma_ns;
    uint64_t gc_trigger_pause_sample_count;
    uint64_t gc_trigger_clock_failure_count;
    uint64_t allocated_bytes;

    size_t gc_arena_size;
    size_t gc_arena_used;
    size_t region_size;
    size_t tlab_size;
    uint32_t tlab_sizing_policy;
    size_t tlab_min_size;
    size_t tlab_max_size;
    size_t tlab_target_min_size;
    size_t tlab_target_max_size;
    size_t region_count;
    size_t active_region_count;
    size_t decommitted_region_count;
    size_t region_decommitted_bytes;
    size_t young_region_count;
    size_t old_region_count;
    size_t special_region_count;
    size_t young_object_count;
    size_t old_object_count;
    size_t young_survivor_object_count;
    size_t young_tenuring_candidate_count;
    size_t live_region_count;
    size_t free_region_count;
    size_t large_region_count;
    size_t pinned_region_count;
    size_t non_movable_region_count;
    size_t quarantined_region_count;
    size_t collection_set_region_count;
    size_t allocator_owned_region_count;
    size_t active_tlab_count;
    size_t tlab_reserved_block_count;
    size_t tlab_reserved_bytes;
    size_t region_padding_created_bytes;
    size_t live_object_count;
    size_t live_payload_bytes;
    size_t large_object_count;
    size_t large_free_object_count;
    size_t large_free_bytes;
    size_t humongous_run_count;
    size_t humongous_live_run_count;
    size_t humongous_free_run_count;
    size_t humongous_region_count;
    size_t large_free_bin_capacity;
    size_t large_free_active_bin_count;
    size_t largest_large_free_run_regions;
    size_t decommitted_large_free_active_bin_count;
    size_t interior_decommitted_run_count;
    size_t interior_decommitted_region_count;
    size_t interior_decommitted_bytes;
    size_t interior_decommit_min_regions;
    uint32_t interior_decommit_policy;
    uint32_t interior_decommit_smart_last_action;
    uint32_t interior_decommit_smart_cooldown_remaining;
    uint32_t interior_decommit_smart_committed_pressure_basis_points;
    uint32_t interior_decommit_smart_large_free_pressure_basis_points;
    uint32_t interior_decommit_smart_pressure_source;
    uint32_t interior_decommit_smart_external_pressure_valid;
    uint32_t interior_decommit_smart_external_pressure_basis_points;
    uint32_t interior_decommit_smart_slow_vm_observed;
    size_t interior_decommit_smart_resident_bytes;
    size_t interior_decommit_smart_memory_limit_bytes;
    uint64_t interior_decommit_slow_ns_per_region;
    uint64_t region_decommit_latency_ewma_ns_per_region;
    uint64_t region_recommit_latency_ewma_ns_per_region;
    size_t pinnable_object_count;
    size_t non_movable_object_count;
    size_t free_object_count;
    size_t free_bytes;
    size_t root_count;
    size_t card_size;
    size_t card_count;
    uint32_t remset_card_atomic_lock_free;
    size_t dirty_card_count;
    size_t remset_max_dirty_cards;
    uint32_t remset_mode;
    size_t remset_buffer_capacity;
    size_t remset_buffered_card_count;
    size_t remset_buffered_thread_count;
    size_t remset_buffer_inflight_card_count;
    size_t remset_buffer_inflight_thread_count;
    size_t remset_classification_pending_thread_count;
    size_t registered_thread_count;
    size_t registered_type_count;
    uint32_t type_table_frozen;
    uint64_t allocation_count;
    uint64_t large_allocation_count;
    uint64_t large_reuse_count;
    uint64_t large_free_search_count;
    uint64_t large_free_probe_count;
    uint64_t large_free_committed_probe_count;
    uint64_t large_free_decommitted_probe_count;
    uint64_t large_free_size_reject_count;
    uint64_t large_free_placement_reject_count;
    uint64_t interior_run_decommit_count;
    uint64_t interior_run_recommit_count;
    uint64_t interior_run_tail_collapse_count;
    uint64_t interior_run_tail_collapsed_region_count;
    uint64_t interior_run_tail_collapsed_bytes;
    uint64_t interior_decommit_smart_evaluation_count;
    uint64_t interior_decommit_smart_raise_count;
    uint64_t interior_decommit_smart_lower_count;
    uint64_t interior_decommit_smart_cooldown_hold_count;
    uint64_t interior_decommit_smart_last_recommit_delta;
    uint64_t interior_decommit_smart_last_large_allocation_delta;
    uint64_t interior_decommit_smart_last_large_reuse_delta;
    uint64_t interior_decommit_smart_last_decommit_latency_sample_delta;
    uint64_t interior_decommit_smart_last_recommit_latency_sample_delta;
    uint64_t interior_decommit_smart_external_sample_count;
    uint64_t interior_decommit_smart_external_failure_count;
    uint64_t region_decommit_latency_sample_count;
    uint64_t region_recommit_latency_sample_count;
    uint64_t region_vm_clock_failure_count;
    uint64_t tlab_refill_count;
    uint64_t tlab_retire_count;
    uint64_t tlab_allocation_count;
    uint64_t tlab_refill_bytes;
    uint64_t tlab_retired_unused_bytes;
    uint64_t tlab_fallback_allocation_count;
    uint64_t tlab_fast_allocation_count;
    uint64_t tlab_fast_slow_path_count;
    uint64_t tlab_fast_flush_count;
    uint64_t tlab_fast_flushed_object_count;
    uint64_t tlab_fast_gate_close_count;
    uint64_t tlab_fast_wrong_owner_count;
    uint64_t tlab_sizing_sample_count;
    uint64_t tlab_sizing_grow_count;
    uint64_t tlab_sizing_shrink_count;
    uint64_t tlab_sizing_hold_count;
    uint64_t region_padding_block_count;
    uint64_t allocator_lease_epoch;
    uint64_t allocator_lease_acquire_count;
    uint64_t allocator_lease_revoke_count;
    uint64_t region_decommit_count;
    uint64_t region_recommit_count;
    uint64_t region_decommit_failure_count;
    uint64_t region_recommit_failure_count;
    uint64_t mutator_assist_charged_bytes;
    uint64_t mutator_assist_repaid_bytes;
    size_t mutator_assist_debt_bytes;
    uint64_t mutator_assist_repay_count;
    uint64_t mutator_assist_packet_count;
    uint64_t mutator_assist_cycle_count;
    uint64_t mutator_assist_progress_count;
    uint64_t mutator_assist_no_progress_count;
    uint64_t mutator_assist_mark_items_processed;
    uint64_t mutator_assist_sweep_bytes_processed;
    size_t mutator_assist_mark_backlog_items;
    uint32_t mutator_assist_state;
    size_t mutator_assist_pending_thread_count;
    size_t mutator_assist_max_thread_debt_bytes;
    size_t mutator_assist_current_budget_items;
    size_t mutator_assist_current_budget_bytes;
    uint64_t mutator_assist_fairness_cap_count;
    uint64_t mutator_assist_backoff_count;
    uint64_t mutator_assist_dynamic_budget_count;
    uint64_t mutator_assist_thread_packet_count;
    uint64_t mutator_assist_thread_progress_count;
    uint64_t mutator_assist_thread_no_progress_count;
    uint64_t collection_count;
    uint64_t young_collection_count;
    uint64_t young_reclaimed_object_count;
    uint64_t young_mark_old_object_count;
    uint64_t young_mark_dirty_slot_count;
    uint32_t young_tenuring_threshold;
    uint64_t young_evacuation_preflight_count;
    uint64_t young_evacuation_capacity_fallback_count;
    size_t young_evacuation_survivor_bytes;
    size_t young_evacuation_promotion_bytes;
    uint64_t young_evacuated_object_count;
    uint64_t young_promoted_object_count;
    uint64_t young_in_place_promoted_object_count;
    uint64_t young_evacuation_emergency_completion_count;
    uint64_t young_evacuation_emergency_failure_count;
    uint32_t evacuation_epoch;
    uint64_t forwarding_cas_success_count;
    uint64_t forwarding_cas_failure_count;
    uint32_t evacuation_worker_epoch;
    uint32_t evacuation_workers_active;
    uint32_t evacuation_workers_peak_active;
    uint32_t evacuation_workers_quiescent;
    uint32_t from_space_reclaim_pending;
    uint32_t from_space_quarantine_epoch;
    uint32_t from_space_quarantine_active;
    uint64_t from_space_quarantine_release_epoch;
    size_t from_space_quarantine_object_count;
    size_t from_space_quarantine_bytes;
    uint64_t from_space_quarantine_cycle_count;
    uint64_t from_space_delayed_reclaim_count;
    uint64_t from_space_delayed_reclaimed_object_count;
    uint64_t from_space_delayed_reclaimed_bytes;
    uint64_t evacuation_parallel_cycle_count;
    uint32_t evacuation_worker_pool_size;
    uint64_t evacuation_worker_created_count;
    uint64_t evacuation_worker_create_failure_count;
    uint64_t evacuation_worker_dispatch_count;
    uint64_t evacuation_worker_fallback_count;
    uint64_t evacuation_worker_task_count;
    uint64_t evacuation_loser_reclaimed_count;
    uint32_t remap_worker_epoch;
    uint32_t remap_workers_active;
    uint32_t remap_workers_peak_active;
    uint32_t remap_workers_quiescent;
    uint64_t remap_parallel_cycle_count;
    uint64_t remap_worker_dispatch_count;
    uint64_t remap_worker_fallback_count;
    uint64_t remap_worker_block_count;
    uint64_t remap_scanned_slot_count;
    uint64_t remap_rewritten_slot_count;
    uint64_t reference_process_count;
    uint64_t reference_process_packet_count;
    uint64_t reference_process_items_processed;
    uint64_t reference_process_no_progress_count;
    size_t reference_process_pending_items;
    uint32_t reference_discovered_state;
    uint32_t reference_discovered_epoch;
    size_t reference_discovered_weak_count;
    size_t reference_discovered_soft_count;
    size_t reference_discovered_phantom_count;
    uint64_t soft_reference_discovered_count;
    uint64_t soft_reference_retained_count;
    uint64_t soft_reference_cleared_count;
    uint64_t weak_reference_discovered_count;
    uint64_t weak_reference_cleared_count;
    uint64_t phantom_reference_discovered_count;
    uint64_t phantom_reference_enqueued_count;
    uint64_t phantom_queue_promoted_count;
    size_t phantom_reaper_watched_queue_count;
    uint64_t phantom_reaper_dropped_queue_count;
    size_t pinned_object_count;
    uint64_t pin_lease_count;
    uint64_t pin_acquire_count;
    uint64_t pin_release_count;
    uint64_t pin_busy_count;
    uint64_t pin_unpin_error_count;
    size_t pin_lease_record_count;
    uint64_t pin_owner_conflict_count;
    uint64_t pin_thread_exit_busy_count;
    uint64_t reclaimed_object_count;
    uint64_t load_barrier_count;
    uint64_t store_barrier_count;
    uint64_t remset_record_count;
    uint64_t remset_cross_region_record_count;
    uint64_t remset_old_to_young_record_count;
    uint64_t remset_overflow_count;
    uint64_t remset_rebuild_count;
    size_t remset_rebuild_cross_region_edge_count;
    size_t remset_rebuild_old_to_young_edge_count;
    uint64_t remset_buffer_record_count;
    uint64_t remset_buffer_deduplicated_count;
    uint64_t remset_buffer_flush_count;
    uint64_t remset_buffer_flushed_card_count;
    uint64_t remset_buffer_overflow_flush_count;
    uint64_t remset_buffer_deferred_publish_count;
    uint64_t remset_buffer_deferred_deduplicated_count;
    uint64_t remset_buffer_deferred_covered_count;
    uint64_t remset_buffer_deferred_classification_count;
    uint64_t remset_direct_record_count;
    uint64_t remset_barrier_tss_hit_count;
    uint64_t remset_barrier_tss_fallback_scan_count;
    uint64_t remset_barrier_tss_ambiguous_count;
    uint64_t remset_barrier_tss_update_failure_count;
    uint64_t remset_barrier_no_owner_direct_count;
    uint64_t safepoint_epoch;
    uint64_t safepoint_request_count;
    uint64_t safepoint_poll_count;
    uint64_t safepoint_ack_count;
    uint64_t safepoint_wait_count;
    uint64_t safepoint_release_count;
    uint64_t safepoint_release_epoch;
    size_t safepoint_pending_threads;
    size_t safepoint_parked_threads;
    size_t safepoint_collector_owned_threads;
    uint32_t safepoint_active;
    uint32_t gc_phase;
    uint32_t read_barrier_active;
    uint64_t phase_transition_count;
    uint64_t read_barrier_active_load_count;
    uint32_t mutator_concurrent_active;
    uint32_t mutator_concurrent_epoch;
    uint64_t mutator_concurrent_window_count;
    uint64_t mutator_concurrent_load_count;
    uint64_t mutator_concurrent_store_count;
    uint64_t mutator_concurrent_rejected_count;
    size_t mutator_concurrent_running_threads;
    size_t mutator_concurrent_repark_pending_threads;
    uint32_t mutator_concurrent_repark_requested;
    uint64_t mutator_concurrent_resume_count;
    uint64_t mutator_concurrent_repark_count;

    size_t uhm_arena_size;
    size_t uhm_committed_bytes;
    size_t uhm_decommitted_bytes;
    size_t uhm_mmap_live_bytes;
    size_t uhm_page_size;
    size_t uhm_allocation_granularity;
} nggc_stats;

nggc_result nggc_open(const nggc_config *config, nggc_heap **out_heap);
nggc_result nggc_close(nggc_heap *heap);
/*
 * Returns the lifecycle state and the preserved cause of a fail-stop state.
 * The cause is NGGC_OK while the heap is running.
 */
nggc_result nggc_get_heap_state(nggc_heap *heap,
                                uint32_t *out_state,
                                nggc_result *out_fail_stop_reason);
/*
 * Releases a fail-stop heap after the host has stopped every mutator that can
 * access it. This bypasses the normal registered-thread/safepoint close gate.
 */
nggc_result nggc_force_destroy(nggc_heap *heap);

nggc_result nggc_register_type(nggc_heap *heap,
                               const nggc_type_desc *desc,
                               uint16_t *out_type_id);
nggc_result nggc_unregister_type(nggc_heap *heap, uint16_t type_id);
nggc_result nggc_type_table_freeze(nggc_heap *heap);

void *nggc_alloc(nggc_heap *heap, uint16_t type_id);
void *nggc_alloc_size(nggc_heap *heap, uint16_t type_id, size_t payload_size);
void *nggc_thread_alloc(nggc_heap *heap, nggc_thread *thread,
                        uint16_t type_id);
void *nggc_thread_alloc_size(nggc_heap *heap, nggc_thread *thread,
                             uint16_t type_id, size_t payload_size);

nggc_result nggc_weak_ref_create(nggc_heap *heap,
                                 void *referent,
                                 void **out_weak_ref);
nggc_result nggc_weak_ref_get(nggc_heap *heap,
                              void *weak_ref,
                              void **out_referent);
nggc_result nggc_soft_ref_create(nggc_heap *heap,
                                 void *referent,
                                 void **out_soft_ref);
nggc_result nggc_soft_ref_get(nggc_heap *heap,
                              void *soft_ref,
                              void **out_referent);
nggc_result nggc_phantom_queue_create(nggc_heap *heap,
                                      void **out_queue);
nggc_result nggc_phantom_ref_create(nggc_heap *heap,
                                    void *referent,
                                    void *queue,
                                    void **out_phantom_ref);
nggc_result nggc_phantom_queue_count(nggc_heap *heap,
                                     void *queue,
                                     size_t *out_count);
nggc_result nggc_phantom_queue_pop(nggc_heap *heap,
                                   void *queue,
                                   void **out_phantom_ref);
nggc_result nggc_phantom_reaper_watch(nggc_heap *heap, void *queue);
nggc_result nggc_phantom_reaper_unwatch(nggc_heap *heap, void *queue);
nggc_result nggc_phantom_reaper_poll(nggc_heap *heap,
                                     void **out_queue,
                                     size_t *out_pending_count);
nggc_result nggc_phantom_reaper_wait(nggc_heap *heap,
                                     size_t max_collect_attempts,
                                     void **out_queue,
                                     size_t *out_pending_count);
/*
 * Waits for a watched queue to become non-empty or for the watched set to
 * change. It never starts a collection and returns BUSY when no watched
 * queue remains. C11 builds use a condition variable; other builds retain
 * the portable atomic wait fallback used by safepoint_park.
 */
nggc_result nggc_phantom_reaper_wait_blocking(
    nggc_heap *heap, void **out_queue, size_t *out_pending_count);
nggc_result nggc_phantom_reaper_pop(nggc_heap *heap,
                                    void **out_queue,
                                    void **out_phantom_ref);

nggc_result nggc_root_push(nggc_heap *heap, void **slot);
nggc_result nggc_root_pop(nggc_heap *heap, void **slot);

nggc_result nggc_thread_register(nggc_heap *heap,
                                 nggc_thread **out_thread);
nggc_result nggc_thread_unregister(nggc_heap *heap, nggc_thread *thread);
nggc_result nggc_thread_root_push(nggc_heap *heap, nggc_thread *thread,
                                  void **slot);
nggc_result nggc_thread_root_pop(nggc_heap *heap, nggc_thread *thread,
                                 void **slot);
nggc_result nggc_request_safepoint(nggc_heap *heap);
nggc_result nggc_safepoint(nggc_heap *heap, nggc_thread *thread);
nggc_result nggc_safepoint_park(nggc_heap *heap, nggc_thread *thread);
nggc_result nggc_safepoint_wait(nggc_heap *heap,
                                size_t *out_pending_threads);
nggc_result nggc_safepoint_release(nggc_heap *heap);
nggc_result nggc_phase_begin(nggc_heap *heap, uint32_t phase);
nggc_result nggc_phase_end(nggc_heap *heap, uint32_t phase);

/*
 * Requires an aligned, accessible pointer slot. While the read barrier is
 * active, a managed payload slot follows owner forwarding before its field
 * is read; external root/local slots retain their caller-owned storage.
 */
void *nggc_load(nggc_heap *heap, void **slot);
nggc_result nggc_store(nggc_heap *heap, void *owner, void **slot,
                       void *value);

#define NGGC_LOAD(heap, slot) nggc_load((heap), (void **)(slot))
#define NGGC_STORE(heap, owner, slot, value) \
    nggc_store((heap), (owner), (void **)(slot), (void *)(value))

uint16_t nggc_object_type(const void *object);
size_t nggc_object_size(const void *object);
uint32_t nggc_object_flags(const void *object);
/* Returns the nested dynamic pin count, or zero for a non-live object. */
uint32_t nggc_object_pin_count(const void *object);

/*
 * A pin prevents evacuation but does not keep an unreachable object alive.
 * A registered current thread owns its lease; an unregistered caller uses an
 * anonymous lease. Cross-owner unpin and unregister with outstanding leases
 * are rejected.
 */
nggc_result nggc_pin(nggc_heap *heap, void *object);
nggc_result nggc_unpin(nggc_heap *heap, void *object);

nggc_result nggc_collect(nggc_heap *heap);
/* Evaluate the configured trigger policy and collect when it is due. */
nggc_result nggc_collect_if_needed(nggc_heap *heap, size_t requested_bytes);
nggc_result nggc_thread_collect(nggc_heap *heap, nggc_thread *thread);
nggc_result nggc_collect_young(nggc_heap *heap);
nggc_result nggc_thread_collect_young(nggc_heap *heap,
                                      nggc_thread *thread);
nggc_result nggc_check(nggc_heap *heap);
nggc_result nggc_get_stats(nggc_heap *heap, nggc_stats *out_stats);
/*
 * Heap-wide atomic diagnostic; concurrent operations may replace it and
 * successful APIs may retain an earlier value. Use an API's returned
 * nggc_result to identify that particular call's outcome.
 */
nggc_result nggc_get_last_error(nggc_heap *heap);

#ifdef __cplusplus
}
#endif

#endif
