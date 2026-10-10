#include "nggc/nggc.h"

#include "nggc_backend.h"
#include "nggc_internal.h"

#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef NGGC_HAVE_C11_THREADS
#define NGGC_HAVE_C11_THREADS 0
#endif

#if NGGC_HAVE_C11_THREADS
#include <threads.h>
#endif

#define NGGC_HEAP_MAGIC 0x4e474743u
#define NGGC_CLOSED_MAGIC 0x4e474758u
#define NGGC_THREAD_MAGIC 0x4e475443u
#define NGGC_THREAD_CLOSED_MAGIC 0x4e475458u
#define NGGC_BARRIER_TSS_ENTRY_MAGIC 0x4e475453u
#define NGGC_OBJECT_MAGIC 0x4e474f42u
#define NGGC_TYPE_LIMIT 65536u
#define NGGC_OBJECT_HEADER_BYTES 64u
#define NGGC_OBJECT_ALIGNMENT 64u
#define NGGC_DEFAULT_PAYLOAD_ALIGNMENT 16u
#define NGGC_MIN_FREE_BLOCK_SIZE \
    (NGGC_OBJECT_HEADER_BYTES + NGGC_OBJECT_ALIGNMENT)
#define NGGC_DEFAULT_UHM_ARENA_SIZE ((size_t)64u * 1024u * 1024u)
#define NGGC_DEFAULT_GC_ARENA_SIZE  ((size_t)16u * 1024u * 1024u)
#define NGGC_DEFAULT_CARD_SIZE 512u
#define NGGC_DEFAULT_REGION_SIZE ((size_t)64u * 1024u)
#define NGGC_MIN_REGION_SIZE ((size_t)NGGC_DEFAULT_CARD_SIZE)
#define NGGC_DEFAULT_REMSET_MAX_DIRTY_CARDS ((size_t)4096u)
#define NGGC_DEFAULT_REMSET_BUFFER_CAPACITY ((size_t)64u)
#define NGGC_ASSIST_MARK_BUDGET_ITEMS 8u
#define NGGC_ASSIST_MARK_BUDGET_MAX_ITEMS 32u
#define NGGC_ASSIST_SWEEP_BUDGET_BYTES ((size_t)1024u)
#define NGGC_ASSIST_SWEEP_BUDGET_MAX_BYTES ((size_t)8192u)
#define NGGC_ASSIST_NO_PROGRESS_BACKOFF_THRESHOLD 2u
#define NGGC_ASSIST_BACKOFF_PACKETS 2u
#define NGGC_ASSIST_MAX_CONSECUTIVE_PACKETS 4u
#define NGGC_DEFAULT_TLAB_BYTES ((size_t)16u * 1024u)
#define NGGC_DEFAULT_TLAB_MIN_BYTES ((size_t)4u * 1024u)
#define NGGC_TLAB_HIGH_UTILIZATION_STREAK 2u
#define NGGC_TLAB_LOW_UTILIZATION_STREAK 3u
#define NGGC_EVACUATION_WORKER_LIMIT 4u
#define NGGC_EVACUATION_BACKGROUND_WORKER_LIMIT \
    (NGGC_EVACUATION_WORKER_LIMIT - 1u)
#define NGGC_LARGE_FREE_BIN_COUNT 32u
#define NGGC_INTERIOR_DECOMMIT_SMART_MAX_REGIONS ((size_t)64u)
#define NGGC_INTERIOR_DECOMMIT_SMART_COOLDOWN_CYCLES 2u
#define NGGC_INTERIOR_DECOMMIT_SMART_HIGH_COMMITTED_BPS 7500u
#define NGGC_INTERIOR_DECOMMIT_SMART_HIGH_LARGE_FREE_BPS 2500u
#define NGGC_INTERIOR_DECOMMIT_SMART_HOT_LARGE_ALLOCATIONS 4u
#define NGGC_INTERIOR_DECOMMIT_SMART_HOT_LARGE_REUSES 2u

#define NGGC_CONFIG_KNOWN_FLAGS \
    (NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT | \
     NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM | \
     NGGC_CONFIG_COLLECT_AUTO_TRIGGER | \
     NGGC_CONFIG_YOUNG_IN_PLACE_PROMOTION)
#define NGGC_TYPE_KNOWN_FLAGS \
    (NGGC_TYPE_FLAG_INTERNAL | NGGC_TYPE_FLAG_PINNABLE | \
     NGGC_TYPE_FLAG_NON_MOVABLE)

enum {
    NGGC_OBJECT_ALLOCATING = 1u,
    NGGC_OBJECT_LIVE = 2u,
    NGGC_OBJECT_FREE = 3u,
    NGGC_OBJECT_TLAB_RESERVED = 4u,
    NGGC_OBJECT_FORWARDED = 5u,
    NGGC_OBJECT_DECOMMITTED_HUMONGOUS = 6u
};

enum {
    NGGC_EVACUATION_TASK_PENDING = 0u,
    NGGC_EVACUATION_TASK_RUNNING = 1u,
    NGGC_EVACUATION_TASK_COMPLETE = 2u,
    NGGC_EVACUATION_TASK_FINALIZED = 3u,
    NGGC_EVACUATION_TASK_RETRY = 4u,
    NGGC_EVACUATION_TASK_ERROR = 5u
};

enum {
    NGGC_INTERNAL_KIND_NONE = 0u,
    NGGC_INTERNAL_KIND_WEAK_REFERENCE = 1u,
    NGGC_INTERNAL_KIND_SOFT_REFERENCE = 2u,
    NGGC_INTERNAL_KIND_PHANTOM_QUEUE = 3u,
    NGGC_INTERNAL_KIND_PHANTOM_REFERENCE = 4u
};

enum {
    NGGC_DISCOVERED_LIST_SOFT = 1u,
    NGGC_DISCOVERED_LIST_WEAK = 2u,
    NGGC_DISCOVERED_LIST_PHANTOM = 3u,
    NGGC_DISCOVERED_LIST_PHANTOM_QUEUE = 4u
};

enum {
    NGGC_ASSIST_REFERENCE_SOFT = NGGC_DISCOVERED_LIST_SOFT,
    NGGC_ASSIST_REFERENCE_WEAK = NGGC_DISCOVERED_LIST_WEAK,
    NGGC_ASSIST_REFERENCE_PHANTOM = NGGC_DISCOVERED_LIST_PHANTOM,
    NGGC_ASSIST_REFERENCE_QUEUES = NGGC_DISCOVERED_LIST_PHANTOM_QUEUE
};

enum {
    NGGC_COLLECTOR_CYCLE_NONE = 0u,
    NGGC_COLLECTOR_CYCLE_NON_MOVING = 1u,
    NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION = 2u
};

typedef struct nggc_object_header {
    uint32_t magic;
    atomic_uint state;
    uint16_t type_id;
    uint16_t size_class;
    uint32_t flags;
    _Atomic(void *) forwarding;
    atomic_uint forwarding_epoch;
    atomic_uint pin_count;
    size_t payload_size;
    size_t alloc_size;
    uint64_t object_id;
    uint32_t mark_epoch;
    uint8_t generation;
    uint8_t young_age;
    atomic_flag payload_lock;
} nggc_object_header;

/* One record represents all nested leases held by one owner for one object. */
typedef struct nggc_pin_lease_record {
    nggc_object_header *header;
    nggc_thread *owner;
    uint64_t object_id;
    uint32_t count;
    struct nggc_pin_lease_record *next;
} nggc_pin_lease_record;

typedef struct nggc_block_view {
    nggc_object_header *header;
    uintptr_t start;
    uintptr_t end;
    uint32_t state;
} nggc_block_view;

typedef struct nggc_type_slot {
    int registered;
    nggc_type_desc desc;
} nggc_type_slot;

typedef struct nggc_phantom_reaper_slot {
    void *queue;
    uint64_t object_id;
} nggc_phantom_reaper_slot;

typedef struct nggc_reference_discovered_list {
    void *head;
    void *tail;
    size_t count;
} nggc_reference_discovered_list;

typedef struct nggc_mark_stack {
    nggc_object_header **items;
    size_t count;
    size_t capacity;
} nggc_mark_stack;

static nggc_result nggc_reference_discovered_clear_locked(
    nggc_heap *heap);
static nggc_result nggc_pin_lease_owner_count_locked(
    nggc_heap *heap, nggc_thread *owner, uint64_t *out_count);
static void nggc_pin_lease_dispose_all(nggc_heap *heap);

typedef void (*nggc_gc_worker_run_fn)(void *context);

typedef struct nggc_gc_worker_job {
    nggc_gc_worker_run_fn run;
    void *context;
} nggc_gc_worker_job;

typedef struct nggc_evacuation_worker_ctx nggc_evacuation_worker_ctx;
typedef struct nggc_remap_worker_ctx nggc_remap_worker_ctx;

#if NGGC_HAVE_C11_THREADS
typedef struct nggc_barrier_tss_entry {
    uint32_t magic;
    uint64_t heap_identity;
    nggc_thread *unique_thread;
    size_t handle_count;
    struct nggc_barrier_tss_entry *next;
} nggc_barrier_tss_entry;

enum {
    NGGC_BARRIER_TSS_UNINITIALIZED = 0u,
    NGGC_BARRIER_TSS_READY = 1u,
    NGGC_BARRIER_TSS_UNAVAILABLE = 2u
};

static once_flag nggc_barrier_tss_once = ONCE_FLAG_INIT;
static tss_t nggc_barrier_tss_key;
static atomic_uint nggc_barrier_tss_status =
    NGGC_BARRIER_TSS_UNINITIALIZED;
#endif

static atomic_uint_fast64_t nggc_heap_identity_next;

#if NGGC_HAVE_C11_THREADS
typedef struct nggc_gc_pool_worker {
    nggc_heap *heap;
    size_t index;
    uint64_t observed_dispatch_epoch;
    uint64_t completed_dispatch_epoch;
    thrd_t thread;
    /* Terminal failure publication is independent of gc_pool_lock. */
    atomic_uint failed;
    uint32_t running;
    uint32_t created;
} nggc_gc_pool_worker;
#endif

typedef struct nggc_region_meta {
    uintptr_t start;
    uintptr_t end;
    uintptr_t allocation_cursor;
    uintptr_t allocation_owner;
    size_t committed_bytes;
    size_t live_bytes;
    size_t free_bytes;
    size_t reserved_bytes;
    size_t quarantined_bytes;
    size_t live_object_count;
    size_t quarantined_object_count;
    size_t large_object_count;
    size_t pinnable_object_count;
    size_t pinned_object_count;
    size_t non_movable_object_count;
    uint32_t state;
    uint32_t generation;
    uint32_t flags;
    uint32_t in_collection_set;
    uint32_t decommitted;
    uint32_t placement;
    uint32_t humongous_role;
    uint32_t humongous_run_offset;
    uint32_t humongous_run_length;
    size_t decommitted_free_next;
    nggc_thread *tlab_owner;
    uint64_t tlab_lease_epoch;
    uintptr_t tlab_reserved_start;
    uintptr_t tlab_published_top;
    uintptr_t tlab_reserved_end;
    uint32_t tlab_state;
} nggc_region_meta;

enum {
    NGGC_THREAD_RUNNING = 1u,
    NGGC_THREAD_AT_SAFEPOINT = 2u,
    NGGC_THREAD_UNREGISTERED = 3u,
    NGGC_THREAD_COLLECTOR_OWNED = 4u,
    NGGC_THREAD_CONCURRENT_RUNNING = 5u,
    NGGC_THREAD_CONCURRENT_REPARKING = 6u
};

enum {
    NGGC_TLAB_INACTIVE = 0u,
    NGGC_TLAB_ACTIVE = 1u,
    NGGC_TLAB_RETIRING = 2u
};

enum {
    NGGC_TLAB_FAST_GATE_CLOSED = 0u,
    NGGC_TLAB_FAST_GATE_OPEN = 1u
};

enum {
    NGGC_REGION_PLACEMENT_NONE = 0u,
    NGGC_REGION_PLACEMENT_MOVABLE_SPECIAL = 1u,
    NGGC_REGION_PLACEMENT_PINNED_SPECIAL = 2u
};

enum {
    NGGC_REGION_HUMONGOUS_NONE = 0u,
    NGGC_REGION_HUMONGOUS_START = 1u,
    NGGC_REGION_HUMONGOUS_CONTINUATION = 2u
};

enum {
    NGGC_REMSET_DEFERRED_OUTCOME_NONE = 0u,
    NGGC_REMSET_DEFERRED_OUTCOME_APPENDED = 1u,
    NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED = 2u,
    NGGC_REMSET_DEFERRED_OUTCOME_COVERED = 3u,
    NGGC_REMSET_DEFERRED_OUTCOME_CORRUPT = 4u
};

enum {
    NGGC_REMSET_CLASSIFICATION_NONE = 0u,
    NGGC_REMSET_CLASSIFICATION_RECORD = (uint32_t)1u << 0,
    NGGC_REMSET_CLASSIFICATION_CROSS_REGION = (uint32_t)1u << 1,
    NGGC_REMSET_CLASSIFICATION_OLD_TO_YOUNG = (uint32_t)1u << 2,
    NGGC_REMSET_CLASSIFICATION_CORRUPT = (uint32_t)1u << 3
};

struct nggc_thread {
    uint32_t magic;
    nggc_heap *heap;
    struct nggc_thread *next;
    uint64_t observed_safepoint_epoch;
    uint64_t safepoint_poll_count;
    uint32_t state;
    void ***roots;
    size_t root_count;
    size_t root_capacity;
    size_t tlab_region_index;
    uint64_t tlab_lease_epoch;
    uintptr_t tlab_reserved_start;
    uintptr_t tlab_published_top;
    uintptr_t tlab_reserved_end;
    uint32_t tlab_state;
    size_t tlab_target_size;
    uint32_t tlab_high_utilization_streak;
    uint32_t tlab_low_utilization_streak;
    size_t *remset_card_buffer;
    atomic_uint remset_card_buffer_count;
    size_t remset_card_buffer_capacity;
    atomic_uint remset_card_publication_inflight;
    size_t remset_card_publication_index;
    size_t remset_card_publication_card;
    atomic_uint remset_card_deferred_outcome;
    atomic_uint remset_classification_publication_inflight;
    size_t remset_classification_owner_region;
    size_t remset_classification_value_region;
    uint32_t remset_classification_owner_generation;
    uint32_t remset_classification_value_generation;
    atomic_uint remset_classification_deferred_outcome;
    _Atomic(uintptr_t) tlab_fast_top;
    atomic_uint tlab_fast_inflight;
    uint64_t tlab_object_id_next;
    size_t tlab_object_id_remaining;
    size_t mutator_assist_debt_bytes;
    uint32_t mutator_assist_no_progress_streak;
    uint32_t mutator_assist_backoff_remaining;
    uint64_t mutator_assist_packet_count;
    uint64_t mutator_assist_progress_count;
    uint64_t mutator_assist_no_progress_count;
#if NGGC_HAVE_C11_THREADS
    thrd_t owner_thread;
    uint32_t owner_thread_bound;
#endif
};

typedef struct nggc_remset_publication {
    nggc_thread *thread;
    size_t index;
    size_t card;
    uint32_t pending;
    uint32_t classification_flags;
    uint32_t classification_pending;
} nggc_remset_publication;

struct nggc_heap {
    uint32_t magic;
    uint64_t heap_identity;
    uint32_t heap_state;
    nggc_result fail_stop_reason;
    atomic_flag lock;
    atomic_int last_error;
    uint32_t flags;
    uint32_t soft_reference_policy;
    uint32_t soft_reference_retain_percent;

    nggc_backend *backend;

    void *gc_arena_base;
    size_t gc_arena_size;
    size_t tlab_size;
    uint32_t tlab_sizing_policy;
    size_t tlab_min_size;
    size_t tlab_max_size;
    uint32_t young_tenuring_threshold;
    uintptr_t arena_cursor;
    uintptr_t arena_end;
    nggc_region_meta *regions;
    size_t region_size;
    size_t region_count;
    size_t active_region_count;
    size_t decommitted_region_count;
    size_t region_decommitted_bytes;
    size_t interior_decommit_min_regions;
    uint32_t interior_decommit_policy;
    uint32_t interior_decommit_smart_initialized;
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
    nggc_memory_pressure_fn memory_pressure;
    void *memory_pressure_ctx;
    nggc_clock_ns_fn clock_ns;
    void *clock_ns_ctx;
    uint64_t interior_decommit_slow_ns_per_region;
    uint64_t region_decommit_latency_ewma_ns_per_region;
    uint64_t region_recommit_latency_ewma_ns_per_region;
    uint64_t region_decommit_latency_sample_count;
    uint64_t region_recommit_latency_sample_count;
    uint64_t region_vm_clock_failure_count;
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
    uint64_t interior_decommit_smart_sample_recommit_count;
    uint64_t interior_decommit_smart_sample_large_allocation_count;
    uint64_t interior_decommit_smart_sample_large_reuse_count;
    uint64_t interior_decommit_smart_sample_decommit_latency_count;
    uint64_t interior_decommit_smart_sample_recommit_latency_count;
    size_t allocator_owned_region_index;
    uint32_t allocator_lease_active;
    uint64_t allocator_lease_epoch;
    size_t region_padding_created_bytes;
    nggc_object_header *free_list;
    nggc_object_header *large_free_bins[NGGC_LARGE_FREE_BIN_COUNT];
    uint32_t large_free_bin_mask;
    size_t decommitted_large_free_bins[NGGC_LARGE_FREE_BIN_COUNT];
    uint32_t decommitted_large_free_bin_mask;
    atomic_uchar *card_table;
    size_t card_size;
    size_t card_count;
    uint32_t remset_card_atomic_lock_free;
    size_t dirty_card_count;
    size_t remset_max_dirty_cards;
    uint32_t remset_mode;
    size_t remset_buffer_capacity;

    nggc_type_slot *types;
    uint16_t next_type_id;
    size_t registered_type_count;
    uint32_t type_table_frozen;
    atomic_uint type_table_published;
    atomic_uint tlab_fast_gate;

    void ***roots;
    size_t root_count;
    size_t root_capacity;
    nggc_phantom_reaper_slot *phantom_reaper_slots;
    size_t phantom_reaper_count;
    size_t phantom_reaper_capacity;
    atomic_uint_fast64_t phantom_reaper_event_epoch;
    nggc_thread *threads;

    size_t live_object_count;
    size_t live_payload_bytes;
    size_t large_object_count;
    size_t pinnable_object_count;
    size_t pinned_object_count;
    size_t non_movable_object_count;
    uint64_t pin_lease_count;
    uint64_t pin_acquire_count;
    uint64_t pin_release_count;
    uint64_t pin_busy_count;
    uint64_t pin_unpin_error_count;
    nggc_pin_lease_record *pin_leases;
    size_t pin_lease_record_count;
    uint64_t pin_owner_conflict_count;
    uint64_t pin_thread_exit_busy_count;
    size_t free_object_count;
    size_t free_bytes;
    size_t large_free_object_count;
    size_t large_free_bytes;
    size_t interior_decommitted_run_count;
    size_t interior_decommitted_region_count;
    size_t interior_decommitted_bytes;
    uint64_t allocation_count;
    uint64_t next_object_id;
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
    nggc_mark_stack mutator_assist_mark_stack;
    uint32_t mutator_assist_state;
    uintptr_t mutator_assist_sweep_cursor;
    size_t mutator_assist_target_reclaim_bytes;
    size_t mutator_assist_reclaimed_bytes;
    uint64_t mutator_assist_packet_count;
    uint64_t mutator_assist_cycle_count;
    uint64_t mutator_assist_progress_count;
    uint64_t mutator_assist_no_progress_count;
    uint64_t mutator_assist_mark_items_processed;
    uint64_t mutator_assist_sweep_bytes_processed;
    nggc_thread *mutator_assist_last_thread;
    size_t mutator_assist_current_budget_items;
    size_t mutator_assist_current_budget_bytes;
    uint32_t mutator_assist_consecutive_packets;
    uint64_t mutator_assist_fairness_cap_count;
    uint64_t mutator_assist_backoff_count;
    uint64_t mutator_assist_dynamic_budget_count;
    uint64_t mutator_assist_thread_packet_count;
    uint64_t mutator_assist_thread_progress_count;
    uint64_t mutator_assist_thread_no_progress_count;
    uint32_t mutator_assist_reference_kind;
    uintptr_t mutator_assist_reference_queue_scan_cursor;
    uintptr_t mutator_assist_reference_queue_scan_end;
    void *mutator_assist_reference_queue;
    uint64_t mutator_assist_reference_queue_object_id;
    void *mutator_assist_reference_queue_member;
    size_t mutator_assist_reference_queue_remaining;
    uint64_t reference_process_packet_count;
    uint64_t reference_process_items_processed;
    uint64_t reference_process_no_progress_count;
    uint64_t allocated_bytes;
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
    uint64_t gc_trigger_last_collection_allocated_bytes;
    uint64_t collection_count;
    uint64_t young_collection_count;
    uint64_t young_reclaimed_object_count;
    uint64_t young_mark_old_object_count;
    uint64_t young_mark_dirty_slot_count;
    uint64_t young_evacuation_preflight_count;
    uint64_t young_evacuation_capacity_fallback_count;
    size_t young_evacuation_survivor_bytes;
    size_t young_evacuation_promotion_bytes;
    uint64_t young_evacuated_object_count;
    uint64_t young_promoted_object_count;
    uint64_t young_in_place_promoted_object_count;
    uint64_t young_evacuation_emergency_completion_count;
    uint64_t young_evacuation_emergency_failure_count;
    uint64_t fail_stop_count;
    uint32_t evacuation_epoch;
    uint64_t forwarding_cas_success_count;
    uint64_t forwarding_cas_failure_count;
    uint32_t evacuation_worker_epoch;
    atomic_uint evacuation_workers_active;
    atomic_uint evacuation_workers_peak_active;
    uint32_t evacuation_workers_quiescent;
    uint32_t evacuation_copy_started;
    uint32_t from_space_reclaim_pending;
    uint32_t from_space_quarantine_epoch;
    uint32_t from_space_quarantine_active;
    uint64_t from_space_quarantine_release_epoch;
    uintptr_t from_space_quarantine_source_end;
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
    atomic_uint remap_workers_active;
    atomic_uint remap_workers_peak_active;
    uint32_t remap_workers_quiescent;
    uint64_t remap_parallel_cycle_count;
    uint64_t remap_worker_dispatch_count;
    uint64_t remap_worker_fallback_count;
    uint64_t remap_worker_block_count;
    uint64_t remap_scanned_slot_count;
    uint64_t remap_rewritten_slot_count;
#if NGGC_HAVE_C11_THREADS
    mtx_t gc_pool_lock;
    cnd_t gc_pool_work_condition;
    cnd_t gc_pool_done_condition;
    nggc_gc_pool_worker
        gc_pool_workers[NGGC_EVACUATION_BACKGROUND_WORKER_LIMIT];
    nggc_gc_worker_job *gc_pool_job;
    size_t gc_pool_requested_workers;
    size_t gc_pool_completed_workers;
    size_t gc_pool_running_workers;
    uint64_t gc_pool_dispatch_epoch;
    uint32_t gc_pool_initialized;
    uint32_t gc_pool_shutdown;
    uint32_t gc_pool_broken;
#endif
    uint64_t reference_process_count;
    uint32_t reference_discovered_state;
    uint32_t reference_discovered_epoch;
    nggc_reference_discovered_list reference_discovered_soft;
    nggc_reference_discovered_list reference_discovered_weak;
    nggc_reference_discovered_list reference_discovered_phantom;
    uint64_t soft_reference_discovered_count;
    uint64_t soft_reference_retained_count;
    uint64_t soft_reference_cleared_count;
    uint64_t weak_reference_discovered_count;
    uint64_t weak_reference_cleared_count;
    uint64_t phantom_reference_discovered_count;
    uint64_t phantom_reference_enqueued_count;
    uint64_t phantom_queue_promoted_count;
    uint64_t phantom_reaper_dropped_queue_count;
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
    size_t registered_thread_count;
    uint64_t safepoint_epoch;
    uint64_t safepoint_request_count;
    uint64_t safepoint_poll_count;
    uint64_t safepoint_ack_count;
    uint64_t safepoint_wait_count;
    uint64_t safepoint_release_count;
    atomic_uint_fast64_t safepoint_resume_epoch;
    atomic_uint_fast64_t safepoint_release_epoch;
    size_t safepoint_pending_threads;
    size_t safepoint_parked_threads;
    uint32_t safepoint_active;
    uint64_t collect_safepoint_epoch;
    nggc_thread *collect_safepoint_owner;
#if NGGC_HAVE_C11_THREADS
    mtx_t safepoint_wait_lock;
    cnd_t safepoint_wait_condition;
    uint32_t safepoint_wait_initialized;
#endif
    uint32_t gc_phase;
    uint32_t read_barrier_active;
    uint64_t phase_transition_count;
    uint64_t read_barrier_active_load_count;
    uint32_t collector_cycle_active;
    uint32_t collector_expected_phase;
    uint32_t collector_cycle_kind;
    uint64_t collector_cycle_epoch;
    uint32_t mark_epoch;
    uint32_t mutator_concurrent_active;
    uint32_t mutator_concurrent_epoch;
    uint64_t mutator_concurrent_window_count;
    uint64_t mutator_concurrent_load_count;
    uint64_t mutator_concurrent_store_count;
    uint64_t mutator_concurrent_rejected_count;
    size_t mutator_concurrent_running_threads;
    size_t mutator_concurrent_repark_pending_threads;
    size_t mutator_concurrent_repark_target_threads;
    uint32_t mutator_concurrent_repark_requested;
    uint64_t mutator_concurrent_resume_count;
    uint64_t mutator_concurrent_repark_count;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    uint32_t test_collect_fault;
    uint32_t test_force_emergency_failure;
    atomic_uint test_concurrent_window_entered;
    atomic_uint test_concurrent_window_release;
    atomic_uint test_concurrent_copy_entered;
    atomic_uint test_concurrent_copy_release;
    atomic_uint test_concurrent_copy_pause;
    _Atomic(void *) test_concurrent_copy_source;
    atomic_uint test_concurrent_copy_store_entered;
    atomic_uint test_concurrent_copy_load_entered;
    atomic_uint test_reference_create_pause_armed;
    atomic_uint test_reference_create_pause_entered;
    atomic_uint test_reference_create_pause_release;
    atomic_uint test_safepoint_blocked_threads;
    atomic_uint test_tlab_fast_pause_armed;
    atomic_uint test_tlab_fast_pause_entered;
    atomic_uint test_tlab_fast_pause_release;
    atomic_uint test_tlab_fast_enrollment_pause_armed;
    atomic_uint test_tlab_fast_enrollment_pause_stage;
    atomic_uint test_tlab_fast_enrollment_pause_release_stage;
    atomic_uint test_tlab_fast_flush_pause_armed;
    atomic_uint test_tlab_fast_flush_pause_entered;
    atomic_uint test_tlab_fast_flush_pause_release;
    atomic_uint test_tlab_fast_flush_busy_observed;
    atomic_uint test_remset_publish_pause_armed;
    atomic_uint test_remset_publish_pause_entered;
    atomic_uint test_remset_publish_pause_release;
    atomic_uint test_remset_classification_pause_armed;
    atomic_uint test_remset_classification_pause_entered;
    atomic_uint test_remset_classification_pause_release;
    atomic_uint test_gc_pool_fail_next_wait;
    atomic_uint test_gc_pool_fail_next_broadcast;
    atomic_uint test_gc_pool_fail_next_join;
    atomic_uint test_gc_pool_worker_fault;
    uint32_t test_barrier_tss_fail_next_update;
#endif
};

typedef struct nggc_check_ctx {
    nggc_heap *heap;
    nggc_object_header *owner;
    nggc_result result;
} nggc_check_ctx;

#if NGGC_ENABLE_VERIFY
typedef struct nggc_scan_replay_item {
    void **slot;
    void *value;
    unsigned char seen;
} nggc_scan_replay_item;

typedef struct nggc_scan_replay_ctx {
    nggc_check_ctx check;
    nggc_scan_replay_item *items;
    size_t count;
    size_t capacity;
    uint32_t replay_pass;
} nggc_scan_replay_ctx;
#endif

typedef struct nggc_remap_ctx {
    nggc_heap *heap;
    nggc_object_header *owner;
    int record_remset;
    nggc_result result;
    atomic_size_t *scanned_slot_count;
    atomic_size_t *rewritten_slot_count;
} nggc_remap_ctx;

typedef struct nggc_forwarded_slot_check_ctx {
    nggc_heap *heap;
    nggc_object_header *owner;
    int record_remset;
    nggc_result result;
} nggc_forwarded_slot_check_ctx;

typedef struct nggc_mark_ctx {
    nggc_heap *heap;
    nggc_mark_stack *stack;
    nggc_result result;
} nggc_mark_ctx;

typedef struct nggc_young_remset_mark_ctx {
    nggc_mark_ctx *mark;
    nggc_object_header *owner;
} nggc_young_remset_mark_ctx;

typedef struct nggc_young_evacuation_task {
    nggc_object_header *source;
    nggc_object_header *destination;
    uint32_t target_generation;
    atomic_uint state;
    nggc_result result;
    void *published_forwarding;
    uint32_t cas_won;
    uint32_t cas_attempted;
    uint32_t stats_recorded;
} nggc_young_evacuation_task;

typedef struct nggc_young_evacuation_plan {
    uintptr_t source_end;
    uintptr_t destination_end;
    size_t survivor_bytes;
    size_t promotion_bytes;
    size_t survivor_objects;
    size_t promotion_objects;
    nggc_young_evacuation_task *tasks;
    size_t task_count;
    size_t task_capacity;
    nggc_object_header **reclaim_queue;
    size_t reclaim_count;
    size_t reclaim_capacity;
    size_t reserved_task_count;
    uint32_t reservations_active;
    uint8_t *in_place_region_flags;
    size_t in_place_region_count;
} nggc_young_evacuation_plan;

struct nggc_evacuation_worker_ctx {
    nggc_heap *heap;
    nggc_young_evacuation_plan *plan;
    atomic_size_t next_task;
    atomic_uint entered_workers;
    atomic_uint completed_workers;
    size_t retry_task_index;
    atomic_uint retry_injected;
};

struct nggc_remap_worker_ctx {
    nggc_heap *heap;
    _Atomic(uintptr_t) next_cursor;
    uintptr_t end_cursor;
    atomic_uint entered_workers;
    atomic_uint completed_workers;
    atomic_int result;
    atomic_size_t block_count;
    atomic_size_t scanned_slot_count;
    atomic_size_t rewritten_slot_count;
};

#if NGGC_ENABLE_VERIFY
typedef struct nggc_remset_verify_ctx {
    nggc_heap *heap;
    nggc_object_header *owner;
    nggc_result result;
    size_t cross_region_edge_count;
    size_t old_to_young_edge_count;
} nggc_remset_verify_ctx;
#endif

typedef struct nggc_internal_weak_reference {
    void *referent;
    void *discovered_next;
    uint32_t discovered_epoch;
} nggc_internal_weak_reference;

typedef struct nggc_internal_soft_reference {
    void *referent;
    void *discovered_next;
    uint32_t discovered_epoch;
} nggc_internal_soft_reference;

typedef struct nggc_internal_phantom_queue {
    void *head;
    void *tail;
    size_t pending_count;
} nggc_internal_phantom_queue;

typedef struct nggc_internal_phantom_reference {
    void *referent;
    void *queue;
    void *next;
    uint32_t enqueued;
    uint32_t discovered_epoch;
    void *discovered_next;
} nggc_internal_phantom_reference;

_Static_assert(sizeof(nggc_object_header) <= NGGC_OBJECT_HEADER_BYTES,
               "nggc object header must fit the fixed header reservation");

static void nggc_forwarding_init(nggc_object_header *header) {
    atomic_init(&header->forwarding, 0);
    atomic_init(&header->forwarding_epoch, 0u);
    atomic_init(&header->pin_count, 0u);
}

static uint32_t nggc_pin_count_load(const nggc_object_header *header) {
    return header ? atomic_load_explicit(&header->pin_count,
                                         memory_order_acquire)
                  : 0u;
}

static void nggc_payload_lock_init(nggc_object_header *header) {
    atomic_flag_clear_explicit(&header->payload_lock, memory_order_release);
}

static void nggc_payload_lock_acquire(nggc_object_header *header) {
    if (!header) {
        return;
    }
    while (atomic_flag_test_and_set_explicit(&header->payload_lock,
                                             memory_order_acquire)) {
        atomic_signal_fence(memory_order_seq_cst);
    }
}

static void nggc_payload_lock_release(nggc_object_header *header) {
    if (header) {
        atomic_flag_clear_explicit(&header->payload_lock,
                                   memory_order_release);
    }
}

static void *nggc_forwarding_load_relaxed(
    const nggc_object_header *header) {
    return atomic_load_explicit(&header->forwarding,
                                memory_order_relaxed);
}

static void *nggc_forwarding_load_acquire(
    const nggc_object_header *header) {
    return atomic_load_explicit(&header->forwarding,
                                memory_order_acquire);
}

static uint32_t nggc_forwarding_epoch_load_acquire(
    const nggc_object_header *header) {
    return atomic_load_explicit(&header->forwarding_epoch,
                                memory_order_acquire);
}

static void nggc_forwarding_store_free_next(
    nggc_object_header *header, nggc_object_header *next) {
    atomic_store_explicit(&header->forwarding_epoch, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&header->forwarding, next,
                          memory_order_relaxed);
}

static int nggc_region_humongous_run_matches_locked(
    const nggc_heap *heap, uintptr_t start, size_t bytes);
static int nggc_interior_decommitted_run_locked(
    const nggc_heap *heap, size_t start_index,
    uint32_t *out_run_length, uint32_t *out_placement);

static int nggc_large_free_bin_index_locked(
    const nggc_heap *heap, size_t bytes, size_t *out_index) {
    size_t run_regions;
    size_t value;
    size_t index = 0u;
    if (!heap || !out_index || heap->region_size == 0u ||
        bytes <= heap->region_size ||
        (bytes % heap->region_size) != 0u) {
        return 0;
    }
    run_regions = bytes / heap->region_size;
    if (run_regions < 2u || run_regions > UINT32_MAX) {
        return 0;
    }
    value = run_regions - 1u;
    while (value > 1u) {
        value >>= 1u;
        ++index;
    }
    if (index >= NGGC_LARGE_FREE_BIN_COUNT) {
        return 0;
    }
    *out_index = index;
    return 1;
}

static nggc_result nggc_large_free_bin_link_existing_locked(
    nggc_heap *heap, nggc_object_header *header) {
    size_t index;
    nggc_object_header *current;
    nggc_object_header *previous = 0;
    nggc_object_header *insert_after = 0;
    nggc_object_header *insert_before = 0;
    size_t previous_bytes = 0u;
    uintptr_t previous_address = 0u;
    size_t visited = 0u;
    int insertion_found = 0;
    if (!heap || !header ||
        header->magic != NGGC_OBJECT_MAGIC ||
        header->type_id != NGGC_TYPE_NONE ||
        header->flags != NGGC_OBJECT_FLAG_LARGE ||
        !nggc_large_free_bin_index_locked(
            heap, header->alloc_size, &index) ||
        !nggc_region_humongous_run_matches_locked(
            heap, (uintptr_t)header, header->alloc_size)) {
        return NGGC_ERR_CORRUPTION;
    }
    current = heap->large_free_bins[index];
    if ((current != 0) !=
        ((heap->large_free_bin_mask &
          ((uint32_t)1u << index)) != 0u)) {
        return NGGC_ERR_CORRUPTION;
    }
    while (current) {
        uintptr_t current_address = (uintptr_t)current;
        uintptr_t header_address = (uintptr_t)header;
        size_t region_index;
        size_t current_bin_index;
        nggc_object_header *next;
        if (visited >= heap->region_count || current == header ||
            current_address < (uintptr_t)heap->gc_arena_base ||
            current_address >= heap->arena_cursor ||
            ((current_address -
              (uintptr_t)heap->gc_arena_base) %
             NGGC_OBJECT_ALIGNMENT) != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index =
            (size_t)((current_address -
                      (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
        if (region_index >= heap->region_count ||
            heap->regions[region_index].decommitted ||
            current->magic != NGGC_OBJECT_MAGIC ||
            atomic_load_explicit(&current->state,
                                 memory_order_relaxed) !=
                NGGC_OBJECT_FREE ||
            current->type_id != NGGC_TYPE_NONE ||
            current->flags != NGGC_OBJECT_FLAG_LARGE ||
            nggc_forwarding_epoch_load_acquire(current) != 0u ||
            !nggc_large_free_bin_index_locked(
                heap, current->alloc_size, &current_bin_index) ||
            current_bin_index != index ||
            !nggc_region_humongous_run_matches_locked(
                heap, current_address, current->alloc_size) ||
            (visited != 0u &&
             (current->alloc_size < previous_bytes ||
              (current->alloc_size == previous_bytes &&
               current_address <= previous_address)))) {
            return NGGC_ERR_CORRUPTION;
        }
        if (!insertion_found &&
            (current->alloc_size > header->alloc_size ||
             (current->alloc_size == header->alloc_size &&
              current_address > header_address))) {
            insert_after = previous;
            insert_before = current;
            insertion_found = 1;
        }
        next = (nggc_object_header *)
            nggc_forwarding_load_relaxed(current);
        previous = current;
        previous_bytes = current->alloc_size;
        previous_address = current_address;
        current = next;
        ++visited;
    }
    if (!insertion_found) {
        insert_after = previous;
        insert_before = 0;
    }
    nggc_forwarding_store_free_next(header, insert_before);
    if (insert_after) {
        nggc_forwarding_store_free_next(insert_after, header);
    } else {
        heap->large_free_bins[index] = header;
    }
    heap->large_free_bin_mask |= (uint32_t)1u << index;
    return NGGC_OK;
}

static int nggc_decommitted_run_decode_index(
    const nggc_heap *heap, size_t encoded, size_t *out_index) {
    size_t index;
    if (!heap || !out_index || encoded == 0u) {
        return 0;
    }
    index = encoded - 1u;
    if (index >= heap->region_count) {
        return 0;
    }
    *out_index = index;
    return 1;
}

static nggc_result nggc_decommitted_large_bin_link_locked(
    nggc_heap *heap, size_t start_index) {
    nggc_region_meta *region;
    uint32_t run_length;
    size_t bytes;
    size_t bin_index;
    size_t encoded;
    size_t previous_index = SIZE_MAX;
    size_t insert_after_index = SIZE_MAX;
    size_t insert_before_encoded = 0u;
    size_t previous_bytes = 0u;
    size_t previous_start_index = 0u;
    size_t visited = 0u;
    int insertion_found = 0;
    if (!heap || start_index >= heap->region_count ||
        !nggc_interior_decommitted_run_locked(
            heap, start_index, &run_length, 0) ||
        run_length > SIZE_MAX / heap->region_size) {
        return NGGC_ERR_CORRUPTION;
    }
    bytes = (size_t)run_length * heap->region_size;
    if (!nggc_large_free_bin_index_locked(
            heap, bytes, &bin_index) || start_index == SIZE_MAX) {
        return NGGC_ERR_CORRUPTION;
    }
    region = &heap->regions[start_index];
    encoded = heap->decommitted_large_free_bins[bin_index];
    if ((encoded != 0u) !=
        ((heap->decommitted_large_free_bin_mask &
          ((uint32_t)1u << bin_index)) != 0u)) {
        return NGGC_ERR_CORRUPTION;
    }
    while (encoded != 0u) {
        size_t current_index;
        uint32_t current_run_length;
        size_t current_bytes;
        size_t current_bin_index;
        size_t next_encoded;
        if (visited >= heap->interior_decommitted_run_count ||
            !nggc_decommitted_run_decode_index(
                heap, encoded, &current_index) ||
            current_index == start_index ||
            !nggc_interior_decommitted_run_locked(
                heap, current_index, &current_run_length, 0) ||
            current_run_length > SIZE_MAX / heap->region_size) {
            return NGGC_ERR_CORRUPTION;
        }
        current_bytes =
            (size_t)current_run_length * heap->region_size;
        if (!nggc_large_free_bin_index_locked(
                heap, current_bytes, &current_bin_index) ||
            current_bin_index != bin_index ||
            (visited != 0u &&
             (current_bytes < previous_bytes ||
              (current_bytes == previous_bytes &&
               current_index <= previous_start_index)))) {
            return NGGC_ERR_CORRUPTION;
        }
        if (!insertion_found &&
            (current_bytes > bytes ||
             (current_bytes == bytes && current_index > start_index))) {
            insert_after_index = previous_index;
            insert_before_encoded = encoded;
            insertion_found = 1;
        }
        next_encoded =
            heap->regions[current_index].decommitted_free_next;
        previous_index = current_index;
        previous_bytes = current_bytes;
        previous_start_index = current_index;
        encoded = next_encoded;
        ++visited;
    }
    if (!insertion_found) {
        insert_after_index = previous_index;
        insert_before_encoded = 0u;
    }
    region->decommitted_free_next = insert_before_encoded;
    if (insert_after_index != SIZE_MAX) {
        heap->regions[insert_after_index].decommitted_free_next =
            start_index + 1u;
    } else {
        heap->decommitted_large_free_bins[bin_index] = start_index + 1u;
    }
    heap->decommitted_large_free_bin_mask |=
        (uint32_t)1u << bin_index;
    return NGGC_OK;
}

static nggc_result nggc_decommitted_large_bin_unlink_locked(
    nggc_heap *heap, size_t start_index) {
    uint32_t run_length;
    size_t run_bytes;
    size_t bin_index;
    size_t encoded;
    size_t previous_index = SIZE_MAX;
    size_t visited = 0u;
    if (!heap || start_index >= heap->region_count ||
        !nggc_interior_decommitted_run_locked(
            heap, start_index, &run_length, 0) ||
        run_length > SIZE_MAX / heap->region_size) {
        return NGGC_ERR_CORRUPTION;
    }
    run_bytes = (size_t)run_length * heap->region_size;
    if (!nggc_large_free_bin_index_locked(
            heap, run_bytes, &bin_index)) {
        return NGGC_ERR_CORRUPTION;
    }
    encoded = heap->decommitted_large_free_bins[bin_index];
    if ((encoded != 0u) !=
        ((heap->decommitted_large_free_bin_mask &
          ((uint32_t)1u << bin_index)) != 0u)) {
        return NGGC_ERR_CORRUPTION;
    }
    while (encoded != 0u) {
        size_t current_index;
        size_t next_encoded;
        uint32_t current_run_length;
        size_t current_run_bytes;
        size_t current_bin_index;
        if (visited >= heap->interior_decommitted_run_count ||
            !nggc_decommitted_run_decode_index(
                heap, encoded, &current_index) ||
            !nggc_interior_decommitted_run_locked(
                heap, current_index, &current_run_length, 0) ||
            current_run_length > SIZE_MAX / heap->region_size) {
            return NGGC_ERR_CORRUPTION;
        }
        current_run_bytes =
            (size_t)current_run_length * heap->region_size;
        if (!nggc_large_free_bin_index_locked(
                heap, current_run_bytes, &current_bin_index) ||
            current_bin_index != bin_index) {
            return NGGC_ERR_CORRUPTION;
        }
        ++visited;
        next_encoded =
            heap->regions[current_index].decommitted_free_next;
        if (current_index == start_index) {
            if (previous_index != SIZE_MAX) {
                heap->regions[previous_index].decommitted_free_next =
                    next_encoded;
            } else {
                heap->decommitted_large_free_bins[bin_index] =
                    next_encoded;
            }
            heap->regions[start_index].decommitted_free_next = 0u;
            if (heap->decommitted_large_free_bins[bin_index] == 0u) {
                heap->decommitted_large_free_bin_mask &=
                    ~((uint32_t)1u << bin_index);
            }
            return NGGC_OK;
        }
        previous_index = current_index;
        encoded = next_encoded;
    }
    return NGGC_ERR_CORRUPTION;
}

static int nggc_free_block_is_large(const nggc_object_header *header) {
    return header &&
           (header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u;
}

static nggc_result nggc_free_block_link_locked(
    nggc_heap *heap, nggc_object_header *header) {
    int is_large;
    if (!heap || !header || header->alloc_size < NGGC_OBJECT_HEADER_BYTES ||
        (header->flags & ~NGGC_OBJECT_FLAG_LARGE) != 0u ||
        heap->free_object_count == SIZE_MAX ||
        heap->free_bytes > SIZE_MAX - header->alloc_size) {
        return NGGC_ERR_CORRUPTION;
    }
    is_large = nggc_free_block_is_large(header);
    if (is_large &&
        !nggc_region_humongous_run_matches_locked(
            heap, (uintptr_t)header, header->alloc_size)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (is_large &&
        (heap->large_free_object_count == SIZE_MAX ||
         heap->large_free_bytes > SIZE_MAX - header->alloc_size)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (is_large) {
        if (nggc_large_free_bin_link_existing_locked(
                heap, header) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
    } else {
        nggc_forwarding_store_free_next(header, heap->free_list);
        heap->free_list = header;
    }
    ++heap->free_object_count;
    heap->free_bytes += header->alloc_size;
    if (is_large) {
        ++heap->large_free_object_count;
        heap->large_free_bytes += header->alloc_size;
    }
    return NGGC_OK;
}

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
static void nggc_forwarding_clear_live(nggc_object_header *header) {
    atomic_store_explicit(&header->forwarding, 0,
                          memory_order_release);
    atomic_store_explicit(&header->forwarding_epoch, 0u,
                          memory_order_relaxed);
}
#endif

static nggc_result nggc_forwarding_publish_atomic(
    nggc_object_header *source, void *target,
    uint32_t epoch, int *out_won, void **out_forwarding) {
    void *observed;
    void *expected = 0;
    uint32_t observed_epoch;
    if (!source || !target || epoch == 0u || !out_won ||
        !out_forwarding) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_won = 0;
    *out_forwarding = 0;
    observed = nggc_forwarding_load_acquire(source);
    if (!observed) {
        atomic_store_explicit(&source->forwarding_epoch, epoch,
                              memory_order_relaxed);
    }
    if (atomic_compare_exchange_strong_explicit(
            &source->forwarding, &expected, target,
            memory_order_release, memory_order_acquire)) {
        *out_won = 1;
        *out_forwarding = target;
        return NGGC_OK;
    }
    *out_forwarding = expected;
    observed_epoch = nggc_forwarding_epoch_load_acquire(source);
    if (!expected || observed_epoch != epoch) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
static nggc_result nggc_forwarding_publish_locked(
    nggc_heap *heap, nggc_object_header *source, void *target,
    uint32_t epoch, int *out_won, void **out_forwarding) {
    nggc_result result;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_forwarding_publish_atomic(
        source, target, epoch, out_won, out_forwarding);
    if (result == NGGC_ERR_INVALID_ARGUMENT) {
        return result;
    }
    if (*out_won) {
        if (heap->forwarding_cas_success_count != UINT64_MAX) {
            ++heap->forwarding_cas_success_count;
        }
    } else if (heap->forwarding_cas_failure_count != UINT64_MAX) {
        ++heap->forwarding_cas_failure_count;
    }
    return result;
}
#endif

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
static nggc_result nggc_test_collect_fault_take_locked(
    nggc_heap *heap, uint32_t fault, nggc_result injected_result) {
    if (heap && heap->test_collect_fault == fault) {
        heap->test_collect_fault = 0u;
        return injected_result;
    }
    return NGGC_OK;
}
#endif

static void nggc_lock(nggc_heap *heap) {
    while (atomic_flag_test_and_set_explicit(&heap->lock,
                                             memory_order_acquire)) {
    }
}

static void nggc_unlock(nggc_heap *heap) {
    atomic_flag_clear_explicit(&heap->lock, memory_order_release);
}

#if NGGC_ENABLE_VERIFY
typedef struct nggc_scan_guard {
    nggc_heap *heap;
    nggc_result result;
} nggc_scan_guard;

/* A scanner may run on a pool worker while the coordinator holds heap->lock.
   Detect API reentry on that native thread before touching any heap or lock.
   Do not write heap->last_error from a rejected worker-side API call. */
static _Thread_local nggc_scan_guard *nggc_scan_guard_active;

static int nggc_scan_api_reentry(void) {
    if (!nggc_scan_guard_active) {
        return 0;
    }
    nggc_scan_guard_active->result = NGGC_ERR_CORRUPTION;
    return 1;
}

#define NGGC_SCAN_API_GUARD(value) \
    do { \
        if (nggc_scan_api_reentry()) { \
            return (value); \
        } \
    } while (0)
#else
#define NGGC_SCAN_API_GUARD(value) ((void)0)
#endif

/* Zero is reserved for an empty/uninitialized TSS identity. */
static uint64_t nggc_next_heap_identity(void) {
    for (;;) {
        uint64_t identity = atomic_load_explicit(&nggc_heap_identity_next,
                                                 memory_order_relaxed);
        uint64_t expected = identity;
        uint64_t next = identity == UINT64_MAX ? UINT64_C(1)
                                                : identity + UINT64_C(1);
        if (identity == 0u) {
            identity = UINT64_C(1);
            next = UINT64_C(2);
        }
        if (atomic_compare_exchange_weak_explicit(
                &nggc_heap_identity_next, &expected, next,
                memory_order_relaxed, memory_order_relaxed)) {
            return identity;
        }
    }
}

static nggc_result nggc_safepoint_broadcast_locked(nggc_heap *heap) {
#if NGGC_HAVE_C11_THREADS
    nggc_result result = NGGC_OK;
    if (!heap || !heap->safepoint_wait_initialized) {
        return NGGC_ERR_BACKEND;
    }
    if (mtx_lock(&heap->safepoint_wait_lock) != thrd_success) {
        return NGGC_ERR_BACKEND;
    }
    if (cnd_broadcast(&heap->safepoint_wait_condition) != thrd_success) {
        result = NGGC_ERR_BACKEND;
    }
    if (mtx_unlock(&heap->safepoint_wait_lock) != thrd_success &&
        result == NGGC_OK) {
        result = NGGC_ERR_BACKEND;
    }
    return result;
#else
    (void)heap;
    return NGGC_OK;
#endif
}

/*
 * The queue list and queue payload remain protected by heap->lock. The
 * separate epoch closes the check-then-sleep window without taking the
 * condition-variable mutex while a waiter is inspecting the heap.
 */
static void nggc_phantom_reaper_notify_locked(nggc_heap *heap) {
    uint_fast64_t epoch;
    uint_fast64_t next_epoch;

    if (!heap) {
        return;
    }
    epoch = atomic_load_explicit(&heap->phantom_reaper_event_epoch,
                                memory_order_relaxed);
    next_epoch = epoch == UINT_FAST64_MAX ? (uint_fast64_t)1 : epoch + 1u;
    atomic_store_explicit(&heap->phantom_reaper_event_epoch, next_epoch,
                          memory_order_release);
#if NGGC_HAVE_C11_THREADS
    if (heap->safepoint_wait_initialized &&
        mtx_lock(&heap->safepoint_wait_lock) == thrd_success) {
        (void)cnd_broadcast(&heap->safepoint_wait_condition);
        (void)mtx_unlock(&heap->safepoint_wait_lock);
    }
#endif
}

static int nggc_heap_handle_live(const nggc_heap *heap) {
    return heap && heap->magic == NGGC_HEAP_MAGIC;
}

static int nggc_heap_live(const nggc_heap *heap) {
    return nggc_heap_handle_live(heap) &&
           heap->heap_state == NGGC_HEAP_STATE_RUNNING;
}

static void nggc_enter_fail_stop_locked(nggc_heap *heap,
                                        nggc_result reason) {
    if (!heap || !nggc_heap_handle_live(heap)) {
        return;
    }
    if (heap->heap_state == NGGC_HEAP_STATE_FAIL_STOP) {
        return;
    }
    heap->heap_state = NGGC_HEAP_STATE_FAIL_STOP;
    heap->fail_stop_reason = reason == NGGC_OK
                                 ? NGGC_ERR_CORRUPTION
                                 : reason;
    if (heap->fail_stop_count != UINT64_MAX) {
        ++heap->fail_stop_count;
    }
}

static int nggc_thread_live(const nggc_thread *thread, const nggc_heap *heap) {
    return thread && thread->magic == NGGC_THREAD_MAGIC &&
           thread->heap == heap;
}

static int nggc_thread_owned_by_current(const nggc_thread *thread) {
#if NGGC_HAVE_C11_THREADS
    return thread && thread->owner_thread_bound != 0u &&
           thrd_equal(thread->owner_thread, thrd_current());
#else
    (void)thread;
    return 1;
#endif
}

#if NGGC_HAVE_C11_THREADS
static void nggc_barrier_tss_destructor(void *value) {
    nggc_barrier_tss_entry *entry = (nggc_barrier_tss_entry *)value;
    while (entry) {
        nggc_barrier_tss_entry *next = entry->next;
        entry->magic = 0u;
        entry->heap_identity = 0u;
        entry->unique_thread = 0;
        entry->handle_count = 0u;
        entry->next = 0;
        free(entry);
        entry = next;
    }
}

static void nggc_barrier_tss_initialize_once(void) {
    uint32_t status = NGGC_BARRIER_TSS_UNAVAILABLE;
    if (tss_create(&nggc_barrier_tss_key,
                   nggc_barrier_tss_destructor) == thrd_success) {
        status = NGGC_BARRIER_TSS_READY;
    }
    atomic_store_explicit(&nggc_barrier_tss_status, status,
                          memory_order_release);
}

static int nggc_barrier_tss_available(void) {
    uint32_t status = atomic_load_explicit(
        &nggc_barrier_tss_status, memory_order_acquire);
    if (status == NGGC_BARRIER_TSS_UNINITIALIZED) {
        call_once(&nggc_barrier_tss_once,
                  nggc_barrier_tss_initialize_once);
        status = atomic_load_explicit(&nggc_barrier_tss_status,
                                      memory_order_acquire);
    }
    return status == NGGC_BARRIER_TSS_READY;
}

static nggc_barrier_tss_entry *nggc_barrier_tss_find_entry(
    nggc_barrier_tss_entry *entries, const nggc_heap *heap) {
    nggc_barrier_tss_entry *entry;
    for (entry = entries; entry; entry = entry->next) {
        if (entry->magic != NGGC_BARRIER_TSS_ENTRY_MAGIC) {
            return 0;
        }
        if (heap && entry->heap_identity == heap->heap_identity) {
            return entry;
        }
    }
    return 0;
}

static void nggc_barrier_tss_remove_current(const nggc_heap *heap) {
    nggc_barrier_tss_entry *entries;
    nggc_barrier_tss_entry **cursor;
    nggc_barrier_tss_entry *entry;

    if (!heap || !nggc_barrier_tss_available()) {
        return;
    }
    entries = (nggc_barrier_tss_entry *)tss_get(nggc_barrier_tss_key);
    cursor = &entries;
    while (*cursor) {
        entry = *cursor;
        if (entry->magic != NGGC_BARRIER_TSS_ENTRY_MAGIC) {
            return;
        }
        if (entry->heap_identity == heap->heap_identity) {
            if (tss_set(nggc_barrier_tss_key, entry->next) !=
                thrd_success) {
                return;
            }
            *cursor = entry->next;
            entry->magic = 0u;
            entry->heap_identity = 0u;
            entry->unique_thread = 0;
            entry->handle_count = 0u;
            entry->next = 0;
            free(entry);
            return;
        }
        cursor = &entry->next;
    }
}

static void nggc_barrier_registry_current_summary_locked(
    nggc_heap *heap, size_t *out_count, nggc_thread **out_unique) {
    nggc_thread *thread;
    size_t count = 0u;
    nggc_thread *unique = 0;
    for (thread = heap ? heap->threads : 0; thread; thread = thread->next) {
        if (thread->magic != NGGC_THREAD_MAGIC || thread->heap != heap ||
            thread->owner_thread_bound == 0u ||
            !thrd_equal(thread->owner_thread, thrd_current())) {
            continue;
        }
        if (count != SIZE_MAX) {
            ++count;
        }
        unique = count == 1u ? thread : 0;
    }
    if (out_count) {
        *out_count = count;
    }
    if (out_unique) {
        *out_unique = count == 1u ? unique : 0;
    }
}

static void nggc_barrier_tss_update_failed_locked(nggc_heap *heap) {
    if (heap && heap->remset_barrier_tss_update_failure_count !=
                    UINT64_MAX) {
        ++heap->remset_barrier_tss_update_failure_count;
    }
}

/*
 * Refreshes only the calling native thread's non-authoritative cache.  A
 * failure deliberately leaves no usable cache entry; the barrier then scans
 * the authoritative heap registry under the existing heap lock.
 */
static void nggc_barrier_tss_refresh_current_locked(nggc_heap *heap) {
    nggc_barrier_tss_entry *entries;
    nggc_barrier_tss_entry *entry;
    nggc_thread *unique = 0;
    size_t count = 0u;
    if (!heap || !nggc_barrier_tss_available()) {
        nggc_barrier_tss_update_failed_locked(heap);
        return;
    }
    entries = (nggc_barrier_tss_entry *)tss_get(nggc_barrier_tss_key);
    entry = nggc_barrier_tss_find_entry(entries, heap);
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (heap->test_barrier_tss_fail_next_update != 0u) {
        heap->test_barrier_tss_fail_next_update = 0u;
        if (entry) {
            entry->unique_thread = 0;
            entry->handle_count = 0u;
        }
        nggc_barrier_tss_update_failed_locked(heap);
        return;
    }
#endif
    nggc_barrier_registry_current_summary_locked(heap, &count, &unique);
    if (entry) {
        entry->unique_thread = unique;
        entry->handle_count = count;
        return;
    }
    if (count == 0u) {
        return;
    }
    entry = (nggc_barrier_tss_entry *)calloc(1u, sizeof(*entry));
    if (!entry) {
        nggc_barrier_tss_update_failed_locked(heap);
        return;
    }
    entry->magic = NGGC_BARRIER_TSS_ENTRY_MAGIC;
    entry->heap_identity = heap->heap_identity;
    entry->unique_thread = unique;
    entry->handle_count = count;
    entry->next = entries;
    if (tss_set(nggc_barrier_tss_key, entry) != thrd_success) {
        entry->magic = 0u;
        entry->heap_identity = 0u;
        entry->unique_thread = 0;
        entry->handle_count = 0u;
        entry->next = 0;
        free(entry);
        nggc_barrier_tss_update_failed_locked(heap);
    }
}

#if NGGC_ENABLE_VERIFY
static nggc_result nggc_verify_barrier_tss_current_locked(
    nggc_heap *heap) {
    nggc_barrier_tss_entry *entries;
    nggc_barrier_tss_entry *entry;
    nggc_barrier_tss_entry *match = 0;
    nggc_barrier_tss_entry *slow;
    nggc_barrier_tss_entry *fast;
    nggc_thread *expected_unique = 0;
    size_t expected_count = 0u;
    if (!heap || atomic_load_explicit(&nggc_barrier_tss_status,
                                      memory_order_acquire) !=
                     NGGC_BARRIER_TSS_READY) {
        return NGGC_OK;
    }
    entries = (nggc_barrier_tss_entry *)tss_get(nggc_barrier_tss_key);
    slow = entries;
    fast = entries;
    while (fast) {
        if (fast->magic != NGGC_BARRIER_TSS_ENTRY_MAGIC ||
            (slow && slow->magic != NGGC_BARRIER_TSS_ENTRY_MAGIC)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (!fast->next) {
            break;
        }
        if (fast->next->magic != NGGC_BARRIER_TSS_ENTRY_MAGIC) {
            return NGGC_ERR_CORRUPTION;
        }
        slow = slow ? slow->next : 0;
        fast = fast->next->next;
        if (slow && slow == fast) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    for (entry = entries; entry; entry = entry->next) {
        if (entry->magic != NGGC_BARRIER_TSS_ENTRY_MAGIC ||
            (entry->handle_count == 0u && entry->unique_thread) ||
            (entry->handle_count == 1u && !entry->unique_thread) ||
            (entry->handle_count > 1u && entry->unique_thread)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (entry->heap_identity == heap->heap_identity) {
            if (match) {
                return NGGC_ERR_CORRUPTION;
            }
            match = entry;
        }
    }
    nggc_barrier_registry_current_summary_locked(
        heap, &expected_count, &expected_unique);
    /* A missing/tombstoned entry is the supported allocation/TSS fallback. */
    if (!match || match->handle_count == 0u) {
        return NGGC_OK;
    }
    if (match->handle_count != expected_count ||
        match->unique_thread != expected_unique) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}
#endif
#else
static void nggc_barrier_tss_remove_current(const nggc_heap *heap) {
    (void)heap;
}

static void nggc_barrier_tss_refresh_current_locked(nggc_heap *heap) {
    (void)heap;
}

#if NGGC_ENABLE_VERIFY
static nggc_result nggc_verify_barrier_tss_current_locked(
    nggc_heap *heap) {
    (void)heap;
    return NGGC_OK;
}
#endif
#endif

static nggc_thread *nggc_barrier_registry_scan_current_locked(
    nggc_heap *heap, int *out_ambiguous) {
    nggc_thread *match = 0;
    nggc_thread *thread;
    int ambiguous = 0;
    if (!heap) {
        return 0;
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        if (thread->magic != NGGC_THREAD_MAGIC ||
            thread->heap != heap ||
            (thread->state != NGGC_THREAD_RUNNING &&
             thread->state != NGGC_THREAD_CONCURRENT_RUNNING)) {
            continue;
        }
#if NGGC_HAVE_C11_THREADS
        if (thread->owner_thread_bound == 0u ||
            !thrd_equal(thread->owner_thread, thrd_current())) {
            continue;
        }
#endif
        if (match) {
            ambiguous = 1;
            match = 0;
            break;
        }
        match = thread;
    }
    if (out_ambiguous) {
        *out_ambiguous = ambiguous;
    }
    return match;
}

static nggc_thread *nggc_current_registered_thread_locked(
    nggc_heap *heap) {
    nggc_thread *match;
    int ambiguous = 0;
    if (!heap) {
        return 0;
    }
#if NGGC_HAVE_C11_THREADS
    if (nggc_barrier_tss_available()) {
        nggc_barrier_tss_entry *entries =
            (nggc_barrier_tss_entry *)tss_get(nggc_barrier_tss_key);
        nggc_barrier_tss_entry *entry =
            nggc_barrier_tss_find_entry(entries, heap);
        if (entry && entry->handle_count > 1u) {
            if (heap->remset_barrier_tss_ambiguous_count != UINT64_MAX) {
                ++heap->remset_barrier_tss_ambiguous_count;
            }
            return 0;
        }
        if (entry && entry->handle_count == 1u &&
            entry->unique_thread &&
            entry->unique_thread->magic == NGGC_THREAD_MAGIC &&
            entry->unique_thread->heap == heap &&
            (entry->unique_thread->state == NGGC_THREAD_RUNNING ||
             entry->unique_thread->state ==
                 NGGC_THREAD_CONCURRENT_RUNNING) &&
            nggc_thread_owned_by_current(entry->unique_thread)) {
            if (heap->remset_barrier_tss_hit_count != UINT64_MAX) {
                ++heap->remset_barrier_tss_hit_count;
            }
            return entry->unique_thread;
        }
    }
#endif
    if (heap->remset_barrier_tss_fallback_scan_count != UINT64_MAX) {
        ++heap->remset_barrier_tss_fallback_scan_count;
    }
    match = nggc_barrier_registry_scan_current_locked(heap, &ambiguous);
    if (ambiguous) {
        if (heap->remset_barrier_tss_ambiguous_count != UINT64_MAX) {
            ++heap->remset_barrier_tss_ambiguous_count;
        }
    } else if (!match &&
               heap->remset_barrier_no_owner_direct_count != UINT64_MAX) {
        ++heap->remset_barrier_no_owner_direct_count;
    }
    return match;
}

static void nggc_set_error(nggc_heap *heap, nggc_result result) {
    if (heap) {
        /* Diagnostic only: publishing an error does not publish heap state. */
        atomic_store_explicit(&heap->last_error, (int)result,
                              memory_order_relaxed);
    }
}

static int nggc_soft_reference_policy_valid(uint32_t policy) {
    return policy == NGGC_SOFT_REFERENCE_POLICY_HEAP_PRESSURE ||
           policy == NGGC_SOFT_REFERENCE_POLICY_ALWAYS_RETAIN ||
           policy == NGGC_SOFT_REFERENCE_POLICY_ALWAYS_CLEAR;
}

static int nggc_interior_decommit_policy_valid(uint32_t policy) {
    return policy == NGGC_INTERIOR_DECOMMIT_POLICY_SMART ||
           policy == NGGC_INTERIOR_DECOMMIT_POLICY_MANUAL ||
           policy == NGGC_INTERIOR_DECOMMIT_POLICY_DISABLED;
}

static int nggc_interior_decommit_smart_action_valid(uint32_t action) {
    return action == NGGC_INTERIOR_DECOMMIT_SMART_ACTION_HOLD ||
           action == NGGC_INTERIOR_DECOMMIT_SMART_ACTION_LOWER ||
           action == NGGC_INTERIOR_DECOMMIT_SMART_ACTION_RAISE ||
           action == NGGC_INTERIOR_DECOMMIT_SMART_ACTION_COOLDOWN;
}

static int nggc_tlab_sizing_policy_config_valid(uint32_t policy) {
    return policy == NGGC_TLAB_SIZING_POLICY_AUTO ||
           policy == NGGC_TLAB_SIZING_POLICY_SMART ||
           policy == NGGC_TLAB_SIZING_POLICY_FIXED;
}

static int nggc_tlab_sizing_policy_resolved_valid(uint32_t policy) {
    return policy == NGGC_TLAB_SIZING_POLICY_SMART ||
           policy == NGGC_TLAB_SIZING_POLICY_FIXED;
}

static int nggc_trigger_policy_valid(uint32_t policy) {
    return policy == NGGC_TRIGGER_SMART ||
           policy == NGGC_TRIGGER_HEAP_LIMIT ||
           policy == NGGC_TRIGGER_ALLOCATION_RATE ||
           policy == NGGC_TRIGGER_LATENCY_TARGET ||
           policy == NGGC_TRIGGER_THROUGHPUT ||
           policy == NGGC_TRIGGER_MEMORY_SAVING ||
           policy == NGGC_TRIGGER_MANUAL;
}

static int nggc_trigger_action_valid(uint32_t action) {
    return action == NGGC_TRIGGER_ACTION_NONE ||
           action == NGGC_TRIGGER_ACTION_YOUNG ||
           action == NGGC_TRIGGER_ACTION_FULL;
}

static int nggc_trigger_reason_valid(uint32_t reason) {
    return reason == NGGC_TRIGGER_REASON_NONE ||
           reason == NGGC_TRIGGER_REASON_HEAP_LIMIT ||
           reason == NGGC_TRIGGER_REASON_ALLOCATION_RATE ||
           reason == NGGC_TRIGGER_REASON_ASSIST_DEBT ||
           reason == NGGC_TRIGGER_REASON_LATENCY ||
           reason == NGGC_TRIGGER_REASON_MEMORY_PRESSURE;
}

static int nggc_thread_is_registered(const nggc_heap *heap,
                                     const nggc_thread *thread) {
    const nggc_thread *current;
    if (!heap || !thread) {
        return 0;
    }
    current = heap->threads;
    while (current) {
        if (current == thread) {
            return 1;
        }
        current = current->next;
    }
    return 0;
}

static int nggc_mutator_concurrent_moving_locked(const nggc_heap *heap);
static nggc_result nggc_mutator_concurrent_wait_repark(nggc_heap *heap);
static void nggc_auto_trigger_before_allocation(nggc_heap *heap,
                                                 size_t requested_bytes);

static nggc_result nggc_mutator_concurrent_resume_locked(
    nggc_heap *heap) {
    nggc_thread *thread;
    size_t running = 0u;
    nggc_result wake_result;

    if (!heap || !heap->collector_cycle_active ||
        heap->collector_cycle_kind !=
            NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION ||
        heap->gc_phase != NGGC_PHASE_EVACUATE ||
        heap->evacuation_epoch == 0u ||
        (!heap->safepoint_active && heap->registered_thread_count != 0u) ||
        (heap->safepoint_active &&
         heap->safepoint_pending_threads != 0u) ||
        heap->mutator_concurrent_active != 0u ||
        heap->mutator_concurrent_epoch != 0u ||
        heap->mutator_concurrent_running_threads != 0u ||
        heap->mutator_concurrent_repark_pending_threads != 0u ||
        heap->mutator_concurrent_repark_target_threads != 0u ||
        heap->mutator_concurrent_repark_requested != 0u) {
        return NGGC_ERR_CORRUPTION;
    }

    for (thread = heap->threads; thread; thread = thread->next) {
        if (thread->state == NGGC_THREAD_AT_SAFEPOINT) {
            if (running == SIZE_MAX) {
                return NGGC_ERR_CORRUPTION;
            }
            ++running;
        } else if (thread->state != NGGC_THREAD_COLLECTOR_OWNED) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (running > heap->safepoint_parked_threads) {
        return NGGC_ERR_CORRUPTION;
    }

    for (thread = heap->threads; thread; thread = thread->next) {
        if (thread->state == NGGC_THREAD_AT_SAFEPOINT) {
            thread->state = NGGC_THREAD_CONCURRENT_RUNNING;
        }
    }
    heap->safepoint_parked_threads -= running;
    heap->mutator_concurrent_running_threads = running;
    heap->mutator_concurrent_repark_target_threads = 0u;
    heap->mutator_concurrent_active = 1u;
    heap->mutator_concurrent_epoch = heap->evacuation_epoch;
    atomic_store_explicit(&heap->safepoint_resume_epoch,
                          heap->safepoint_epoch,
                          memory_order_release);
    wake_result = nggc_safepoint_broadcast_locked(heap);
    if (wake_result != NGGC_OK) {
        for (thread = heap->threads; thread; thread = thread->next) {
            if (thread->state == NGGC_THREAD_CONCURRENT_RUNNING) {
                thread->state = NGGC_THREAD_AT_SAFEPOINT;
            }
        }
        heap->safepoint_parked_threads += running;
        heap->mutator_concurrent_running_threads = 0u;
        heap->mutator_concurrent_active = 0u;
        heap->mutator_concurrent_epoch = 0u;
        atomic_store_explicit(&heap->safepoint_resume_epoch, 0u,
                              memory_order_release);
        return wake_result;
    }
    if (heap->mutator_concurrent_window_count != UINT64_MAX) {
        ++heap->mutator_concurrent_window_count;
    }
    if (heap->mutator_concurrent_resume_count >
        UINT64_MAX - (uint64_t)running) {
        heap->mutator_concurrent_resume_count = UINT64_MAX;
    } else {
        heap->mutator_concurrent_resume_count += (uint64_t)running;
    }
    return NGGC_OK;
}

static nggc_result nggc_mutator_concurrent_request_repark_locked(
    nggc_heap *heap) {
    nggc_thread *thread;
    size_t running = 0u;

    if (!heap || heap->mutator_concurrent_active == 0u ||
        heap->mutator_concurrent_epoch != heap->evacuation_epoch ||
        heap->gc_phase != NGGC_PHASE_EVACUATE ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u ||
        heap->mutator_concurrent_repark_pending_threads != 0u ||
        heap->mutator_concurrent_repark_requested != 0u) {
        return NGGC_ERR_CORRUPTION;
    }

    for (thread = heap->threads; thread; thread = thread->next) {
        if (thread->state == NGGC_THREAD_CONCURRENT_RUNNING) {
            if (running == SIZE_MAX) {
                return NGGC_ERR_CORRUPTION;
            }
            ++running;
        } else if (thread->state != NGGC_THREAD_COLLECTOR_OWNED &&
                   thread->state != NGGC_THREAD_AT_SAFEPOINT) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (running != heap->mutator_concurrent_running_threads ||
        running > heap->registered_thread_count ||
        heap->safepoint_parked_threads >
            heap->registered_thread_count - running) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->mutator_concurrent_repark_requested = 1u;
    heap->mutator_concurrent_repark_target_threads = running;
    return NGGC_OK;
}

static nggc_result nggc_mutator_concurrent_finish_repark_locked(
    nggc_heap *heap) {
    nggc_thread *thread;
    size_t reparks;
    if (!heap || heap->mutator_concurrent_active == 0u ||
        heap->mutator_concurrent_epoch != heap->evacuation_epoch ||
        heap->gc_phase != NGGC_PHASE_EVACUATE ||
        heap->mutator_concurrent_repark_requested == 0u ||
        heap->mutator_concurrent_repark_pending_threads != 0u ||
        heap->mutator_concurrent_running_threads != 0u ||
        heap->safepoint_parked_threads != heap->registered_thread_count) {
        return NGGC_ERR_BUSY;
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        if (thread->state != NGGC_THREAD_AT_SAFEPOINT &&
            thread->state != NGGC_THREAD_COLLECTOR_OWNED) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    reparks = heap->mutator_concurrent_repark_target_threads;
    heap->mutator_concurrent_repark_requested = 0u;
    heap->mutator_concurrent_running_threads = 0u;
    heap->mutator_concurrent_repark_pending_threads = 0u;
    heap->mutator_concurrent_repark_target_threads = 0u;
    heap->mutator_concurrent_active = 0u;
    heap->mutator_concurrent_epoch = 0u;
    if (heap->mutator_concurrent_repark_count >
        UINT64_MAX - (uint64_t)reparks) {
        heap->mutator_concurrent_repark_count = UINT64_MAX;
    } else {
        heap->mutator_concurrent_repark_count += (uint64_t)reparks;
    }
    return NGGC_OK;
}

static int nggc_tlab_spans_overlap(uintptr_t a_start, uintptr_t a_end,
                                   uintptr_t b_start, uintptr_t b_end) {
    return a_start < b_end && b_start < a_end;
}

static nggc_result nggc_verify_tlabs_locked(const nggc_heap *heap) {
    const nggc_thread *thread;
    if (!heap || !heap->regions) {
        return NGGC_ERR_CORRUPTION;
    }
    if (atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
        NGGC_TLAB_FAST_GATE_CLOSED) {
        return NGGC_ERR_CORRUPTION;
    }
    if (!nggc_tlab_sizing_policy_resolved_valid(
            heap->tlab_sizing_policy) ||
        heap->tlab_min_size < NGGC_OBJECT_HEADER_BYTES ||
        heap->tlab_min_size > heap->tlab_size ||
        heap->tlab_size > heap->tlab_max_size ||
        heap->tlab_max_size > heap->region_size ||
        (heap->tlab_min_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        (heap->tlab_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        (heap->tlab_max_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        (heap->tlab_sizing_policy == NGGC_TLAB_SIZING_POLICY_FIXED &&
         (heap->tlab_min_size != heap->tlab_size ||
          heap->tlab_max_size != heap->tlab_size))) {
        return NGGC_ERR_CORRUPTION;
    }
    for (size_t i = 0; i < heap->region_count; ++i) {
        const nggc_region_meta *region = &heap->regions[i];
        if (region->tlab_state == NGGC_TLAB_INACTIVE) {
            if (region->tlab_owner || region->tlab_lease_epoch != 0u ||
                region->tlab_reserved_start != 0u ||
                region->tlab_published_top != 0u ||
                region->tlab_reserved_end != 0u) {
                return NGGC_ERR_CORRUPTION;
            }
            continue;
        }
        if ((region->tlab_state != NGGC_TLAB_ACTIVE &&
             region->tlab_state != NGGC_TLAB_RETIRING) ||
            !region->tlab_owner || region->tlab_lease_epoch == 0u ||
            region->tlab_reserved_start < region->start ||
            region->tlab_reserved_start >= region->tlab_reserved_end ||
            region->tlab_reserved_start > region->tlab_published_top ||
            region->tlab_published_top > region->tlab_reserved_end ||
            region->tlab_reserved_end > region->end ||
            region->tlab_owner->tlab_region_index != i ||
            region->tlab_owner->tlab_lease_epoch !=
                region->tlab_lease_epoch ||
            region->tlab_owner->tlab_reserved_start !=
                region->tlab_reserved_start ||
            region->tlab_owner->tlab_published_top !=
                region->tlab_published_top ||
            region->tlab_owner->tlab_reserved_end !=
                region->tlab_reserved_end ||
            region->tlab_owner->tlab_state != region->tlab_state) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    thread = heap->threads;
    while (thread) {
        if (thread->tlab_target_size < heap->tlab_min_size ||
            thread->tlab_target_size > heap->tlab_max_size ||
            (thread->tlab_target_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
            thread->tlab_high_utilization_streak >=
                NGGC_TLAB_HIGH_UTILIZATION_STREAK ||
            thread->tlab_low_utilization_streak >=
                NGGC_TLAB_LOW_UTILIZATION_STREAK ||
            (thread->tlab_high_utilization_streak != 0u &&
             thread->tlab_low_utilization_streak != 0u) ||
            (heap->tlab_sizing_policy == NGGC_TLAB_SIZING_POLICY_FIXED &&
             (thread->tlab_target_size != heap->tlab_size ||
              thread->tlab_high_utilization_streak != 0u ||
              thread->tlab_low_utilization_streak != 0u))) {
            return NGGC_ERR_CORRUPTION;
        }
        if (thread->tlab_state == NGGC_TLAB_INACTIVE) {
            if (thread->tlab_region_index != SIZE_MAX ||
                thread->tlab_lease_epoch != 0u ||
                thread->tlab_reserved_start != 0u ||
                thread->tlab_published_top != 0u ||
                thread->tlab_reserved_end != 0u ||
                atomic_load_explicit(&thread->tlab_fast_top,
                                     memory_order_acquire) != 0u ||
                atomic_load_explicit(&thread->tlab_fast_inflight,
                                     memory_order_acquire) != 0u ||
                thread->tlab_object_id_next != 0u ||
                thread->tlab_object_id_remaining != 0u) {
                return NGGC_ERR_CORRUPTION;
            }
        } else {
            const nggc_thread *other;
            uint64_t object_id_lease_end = 0u;
            uintptr_t fast_top = atomic_load_explicit(
                &thread->tlab_fast_top, memory_order_acquire);
            if ((thread->tlab_state != NGGC_TLAB_ACTIVE &&
                 thread->tlab_state != NGGC_TLAB_RETIRING) ||
                thread->tlab_reserved_start >= thread->tlab_reserved_end ||
                thread->tlab_reserved_start > thread->tlab_published_top ||
                thread->tlab_published_top > thread->tlab_reserved_end ||
                fast_top != thread->tlab_published_top ||
                atomic_load_explicit(&thread->tlab_fast_inflight,
                                     memory_order_acquire) != 0u ||
                (thread->tlab_object_id_remaining != 0u &&
                 thread->tlab_object_id_next == 0u) ||
                (heap->safepoint_active &&
                 thread->observed_safepoint_epoch ==
                     heap->safepoint_epoch) ||
                thread->tlab_region_index >= heap->region_count ||
                heap->regions[thread->tlab_region_index].tlab_owner !=
                    thread) {
                return NGGC_ERR_CORRUPTION;
            }
            if (thread->tlab_object_id_remaining != 0u) {
                uint64_t lease_delta =
                    (uint64_t)(thread->tlab_object_id_remaining - 1u);
                if (lease_delta >
                        UINT64_MAX - thread->tlab_object_id_next ||
                    (object_id_lease_end =
                         thread->tlab_object_id_next + lease_delta) >
                        heap->next_object_id) {
                    return NGGC_ERR_CORRUPTION;
                }
            }
            for (other = heap->threads; other; other = other->next) {
                if (other == thread ||
                    other->tlab_state == NGGC_TLAB_INACTIVE) {
                    continue;
                }
                if ((other->tlab_state != NGGC_TLAB_ACTIVE &&
                     other->tlab_state != NGGC_TLAB_RETIRING) ||
                    other->tlab_reserved_start >=
                        other->tlab_reserved_end ||
                    nggc_tlab_spans_overlap(
                        thread->tlab_reserved_start,
                        thread->tlab_reserved_end,
                        other->tlab_reserved_start,
                        other->tlab_reserved_end)) {
                    return NGGC_ERR_CORRUPTION;
                }
                if (thread->tlab_object_id_remaining != 0u &&
                    other->tlab_object_id_remaining != 0u) {
                    uint64_t other_delta =
                        (uint64_t)(other->tlab_object_id_remaining - 1u);
                    uint64_t other_end;
                    if (other->tlab_object_id_next == 0u ||
                        other_delta >
                            UINT64_MAX - other->tlab_object_id_next) {
                        return NGGC_ERR_CORRUPTION;
                    }
                    other_end = other->tlab_object_id_next + other_delta;
                    if (thread->tlab_object_id_next <= other_end &&
                        other->tlab_object_id_next <=
                            object_id_lease_end) {
                        return NGGC_ERR_CORRUPTION;
                    }
                }
            }
        }
        thread = thread->next;
    }
    return NGGC_OK;
}

static int nggc_phase_valid(uint32_t phase) {
    return phase == NGGC_PHASE_ROOT_SNAPSHOT ||
           phase == NGGC_PHASE_MARK ||
           phase == NGGC_PHASE_SWEEP ||
           phase == NGGC_PHASE_EVACUATE ||
           phase == NGGC_PHASE_REMAP ||
           phase == NGGC_PHASE_REFERENCE_PROCESS;
}

static int nggc_phase_uses_read_barrier(uint32_t phase) {
    return phase == NGGC_PHASE_EVACUATE || phase == NGGC_PHASE_REMAP;
}

static uint32_t nggc_expected_read_barrier_locked(const nggc_heap *heap) {
    return heap &&
                   (nggc_phase_uses_read_barrier(heap->gc_phase) ||
                    heap->from_space_quarantine_active)
               ? 1u
               : 0u;
}

static int nggc_collector_phase_valid(uint32_t phase) {
    return phase == NGGC_PHASE_ROOT_SNAPSHOT ||
           phase == NGGC_PHASE_MARK ||
           phase == NGGC_PHASE_REFERENCE_PROCESS ||
           phase == NGGC_PHASE_EVACUATE ||
           phase == NGGC_PHASE_REMAP ||
           phase == NGGC_PHASE_SWEEP;
}

static nggc_result nggc_verify_gc_pool_locked(
    const nggc_heap *heap) {
    if (!heap ||
        heap->evacuation_worker_pool_size >
            NGGC_EVACUATION_BACKGROUND_WORKER_LIMIT ||
        heap->evacuation_worker_created_count <
            heap->evacuation_worker_pool_size) {
        return NGGC_ERR_CORRUPTION;
    }
#if NGGC_HAVE_C11_THREADS
    {
        nggc_heap *mutable_heap = (nggc_heap *)heap;
        nggc_result result = NGGC_OK;
        size_t running = 0u;
        size_t completed = 0u;
        if (!heap->gc_pool_initialized ||
            mtx_lock(&mutable_heap->gc_pool_lock) !=
                thrd_success) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->gc_pool_shutdown > 1u || heap->gc_pool_broken > 1u ||
            (heap->gc_pool_shutdown == 0u &&
             heap->evacuation_worker_created_count !=
                 heap->evacuation_worker_pool_size)) {
            result = NGGC_ERR_CORRUPTION;
        }
        if (result == NGGC_OK && heap->gc_pool_job) {
            if (heap->gc_pool_shutdown != 0u ||
                heap->gc_pool_dispatch_epoch == 0u ||
                heap->gc_pool_requested_workers == 0u ||
                heap->gc_pool_requested_workers >
                    heap->evacuation_worker_pool_size ||
                heap->gc_pool_completed_workers >
                    heap->gc_pool_requested_workers ||
                heap->gc_pool_running_workers >
                    heap->gc_pool_requested_workers -
                        heap->gc_pool_completed_workers ||
                !heap->gc_pool_job->run ||
                !heap->gc_pool_job->context ||
                !heap->collector_cycle_active ||
                heap->collector_cycle_kind !=
                    NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION ||
                (heap->gc_phase != NGGC_PHASE_EVACUATE &&
                 heap->gc_phase != NGGC_PHASE_REMAP)) {
                result = NGGC_ERR_CORRUPTION;
            }
        } else if (result == NGGC_OK &&
                   (heap->gc_pool_requested_workers != 0u ||
                    heap->gc_pool_completed_workers != 0u ||
                    heap->gc_pool_running_workers != 0u)) {
            result = NGGC_ERR_CORRUPTION;
        }
        for (size_t i = 0u;
             result == NGGC_OK &&
             i < NGGC_EVACUATION_BACKGROUND_WORKER_LIMIT;
             ++i) {
            const nggc_gc_pool_worker *slot =
                &heap->gc_pool_workers[i];
            if (i < heap->evacuation_worker_pool_size) {
                if (slot->heap != heap || slot->index != i ||
                    slot->running > 1u || slot->created > 1u ||
                    atomic_load_explicit(&slot->failed,
                                         memory_order_acquire) > 1u ||
                    (heap->gc_pool_shutdown == 0u && !slot->created)) {
                    result = NGGC_ERR_CORRUPTION;
                }
                if (slot->running) {
                    if (!heap->gc_pool_job ||
                        i >= heap->gc_pool_requested_workers ||
                        slot->completed_dispatch_epoch ==
                            heap->gc_pool_dispatch_epoch) {
                        result = NGGC_ERR_CORRUPTION;
                    }
                    ++running;
                }
                if (heap->gc_pool_job &&
                    i < heap->gc_pool_requested_workers &&
                    slot->completed_dispatch_epoch ==
                        heap->gc_pool_dispatch_epoch) {
                    ++completed;
                }
            } else if (slot->created) {
                result = NGGC_ERR_CORRUPTION;
            }
        }
        if (running != heap->gc_pool_running_workers ||
            completed != heap->gc_pool_completed_workers) {
            result = NGGC_ERR_CORRUPTION;
        }
        if (mtx_unlock(&mutable_heap->gc_pool_lock) !=
                thrd_success &&
            result == NGGC_OK) {
            result = NGGC_ERR_CORRUPTION;
        }
        return result;
    }
#else
    if (heap->evacuation_worker_pool_size != 0u ||
        heap->evacuation_worker_created_count != 0u ||
        heap->evacuation_worker_dispatch_count != 0u ||
        heap->evacuation_parallel_cycle_count != 0u ||
        heap->remap_worker_dispatch_count != 0u ||
        heap->remap_parallel_cycle_count != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
#endif
}

static nggc_result nggc_verify_phase_state_locked(const nggc_heap *heap) {
    uint32_t evacuation_workers_active;
    uint32_t remap_workers_active;
    uint32_t type_table_published;
    uint32_t tlab_fast_gate;
    size_t safepoint_accounted;
    if (!heap) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_verify_gc_pool_locked(heap) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    evacuation_workers_active = atomic_load_explicit(
        &heap->evacuation_workers_active, memory_order_acquire);
    remap_workers_active = atomic_load_explicit(
        &heap->remap_workers_active, memory_order_acquire);
    type_table_published = atomic_load_explicit(
        &heap->type_table_published, memory_order_acquire);
    tlab_fast_gate = atomic_load_explicit(
        &heap->tlab_fast_gate, memory_order_acquire);
    safepoint_accounted = heap->safepoint_pending_threads;
    if (safepoint_accounted > SIZE_MAX - heap->safepoint_parked_threads) {
        return NGGC_ERR_CORRUPTION;
    }
    safepoint_accounted += heap->safepoint_parked_threads;
    if (safepoint_accounted >
        SIZE_MAX - heap->mutator_concurrent_running_threads) {
        return NGGC_ERR_CORRUPTION;
    }
    safepoint_accounted += heap->mutator_concurrent_running_threads;
    if (safepoint_accounted >
        SIZE_MAX - heap->mutator_concurrent_repark_pending_threads) {
        return NGGC_ERR_CORRUPTION;
    }
    safepoint_accounted += heap->mutator_concurrent_repark_pending_threads;
    if (type_table_published > 1u || tlab_fast_gate > 1u ||
        type_table_published != (heap->type_table_frozen ? 1u : 0u) ||
        (tlab_fast_gate == NGGC_TLAB_FAST_GATE_OPEN &&
         (type_table_published == 0u || heap->safepoint_active ||
          heap->gc_phase != NGGC_PHASE_IDLE ||
          heap->collector_cycle_active ||
          heap->mutator_concurrent_active ||
          heap->from_space_quarantine_active ||
          heap->mutator_assist_state != NGGC_MUTATOR_ASSIST_STATE_IDLE)) ||
        heap->read_barrier_active > 1u ||
        heap->collector_cycle_active > 1u ||
        heap->safepoint_active > 1u ||
        heap->evacuation_workers_quiescent > 1u ||
        heap->remap_workers_quiescent > 1u ||
        heap->evacuation_copy_started > 1u ||
        heap->from_space_reclaim_pending > 1u ||
        heap->from_space_quarantine_active > 1u ||
        heap->mutator_concurrent_active > 1u ||
        heap->mutator_concurrent_running_threads >
            heap->registered_thread_count ||
        heap->mutator_concurrent_repark_pending_threads >
            heap->registered_thread_count ||
        heap->mutator_concurrent_repark_target_threads >
            heap->registered_thread_count ||
        evacuation_workers_active > NGGC_EVACUATION_WORKER_LIMIT ||
        remap_workers_active > NGGC_EVACUATION_WORKER_LIMIT ||
        heap->safepoint_pending_threads > heap->registered_thread_count ||
        heap->safepoint_parked_threads > heap->registered_thread_count ||
        safepoint_accounted !=
            (heap->safepoint_active ? heap->registered_thread_count : 0u)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->mutator_concurrent_active) {
        if (!heap->collector_cycle_active ||
            heap->collector_cycle_kind !=
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION ||
            heap->gc_phase != NGGC_PHASE_EVACUATE ||
            heap->mutator_concurrent_epoch == 0u ||
            heap->mutator_concurrent_epoch != heap->evacuation_epoch ||
            heap->evacuation_copy_started == 0u ||
            heap->mutator_concurrent_repark_pending_threads != 0u ||
            (heap->mutator_concurrent_repark_requested != 0u &&
             heap->mutator_concurrent_repark_target_threads <
                 heap->mutator_concurrent_running_threads) ||
            (heap->mutator_concurrent_repark_requested == 0u &&
             heap->mutator_concurrent_repark_target_threads != 0u) ||
            (heap->safepoint_active &&
             (heap->safepoint_pending_threads != 0u ||
              heap->safepoint_parked_threads +
                      heap->mutator_concurrent_running_threads !=
                  heap->registered_thread_count)) ||
            (!heap->safepoint_active &&
             heap->registered_thread_count != 0u)) {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (heap->mutator_concurrent_epoch != 0u ||
               heap->mutator_concurrent_running_threads != 0u ||
               heap->mutator_concurrent_repark_pending_threads != 0u ||
               heap->mutator_concurrent_repark_target_threads != 0u ||
               heap->mutator_concurrent_repark_requested != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->from_space_quarantine_active) {
        if (heap->from_space_quarantine_epoch == 0u ||
            heap->from_space_quarantine_epoch != heap->evacuation_epoch ||
            heap->from_space_quarantine_source_end <=
                (uintptr_t)heap->gc_arena_base ||
            heap->from_space_quarantine_source_end > heap->arena_cursor ||
            heap->from_space_quarantine_object_count == 0u ||
            heap->from_space_quarantine_bytes == 0u ||
            heap->from_space_quarantine_release_epoch >
                heap->safepoint_epoch) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->collector_cycle_active &&
            (heap->collector_cycle_kind !=
                 NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION ||
             heap->collector_expected_phase != NGGC_PHASE_SWEEP ||
             heap->gc_phase != NGGC_PHASE_SWEEP)) {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (heap->from_space_quarantine_epoch != 0u ||
               heap->from_space_quarantine_release_epoch != 0u ||
               heap->from_space_quarantine_source_end != 0u ||
               heap->from_space_quarantine_object_count != 0u ||
               heap->from_space_quarantine_bytes != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->gc_phase == NGGC_PHASE_IDLE) {
        if (heap->read_barrier_active !=
                nggc_expected_read_barrier_locked(heap) ||
            (heap->phase_transition_count & UINT64_C(1)) != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
    } else {
        if (!nggc_phase_valid(heap->gc_phase) ||
            heap->read_barrier_active !=
                nggc_expected_read_barrier_locked(heap) ||
            (heap->phase_transition_count & UINT64_C(1)) == 0u ||
            heap->safepoint_pending_threads != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (heap->collector_cycle_active) {
        if (!nggc_collector_phase_valid(heap->collector_expected_phase) ||
             (heap->collector_cycle_kind !=
                  NGGC_COLLECTOR_CYCLE_NON_MOVING &&
              heap->collector_cycle_kind !=
                  NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) ||
            (heap->collector_cycle_kind ==
                 NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION &&
             heap->evacuation_epoch == 0u) ||
            (heap->collector_cycle_kind ==
                 NGGC_COLLECTOR_CYCLE_NON_MOVING &&
             (heap->collector_expected_phase == NGGC_PHASE_EVACUATE ||
              heap->collector_expected_phase == NGGC_PHASE_REMAP)) ||
            heap->collector_cycle_epoch == 0u ||
            heap->safepoint_pending_threads != 0u ||
            (heap->collect_safepoint_epoch != 0u &&
             (!heap->safepoint_active ||
              heap->safepoint_parked_threads !=
                  heap->registered_thread_count)) ||
            (heap->gc_phase != NGGC_PHASE_IDLE &&
             heap->gc_phase != heap->collector_expected_phase)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->collector_cycle_kind ==
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) {
            if (heap->evacuation_worker_epoch !=
                    heap->evacuation_epoch ||
                (heap->remap_worker_epoch != 0u &&
                 heap->remap_worker_epoch != heap->evacuation_epoch) ||
                (evacuation_workers_active != 0u &&
                 heap->gc_phase != NGGC_PHASE_EVACUATE) ||
                (remap_workers_active != 0u &&
                 (heap->gc_phase != NGGC_PHASE_REMAP ||
                  heap->remap_worker_epoch != heap->evacuation_epoch)) ||
                (heap->evacuation_workers_quiescent != 0u &&
                 evacuation_workers_active != 0u) ||
                (heap->remap_workers_quiescent != 0u &&
                 (remap_workers_active != 0u ||
                  heap->remap_worker_epoch != heap->evacuation_epoch)) ||
                (heap->from_space_reclaim_pending != 0u &&
                 heap->evacuation_copy_started == 0u) ||
                ((heap->gc_phase == NGGC_PHASE_REMAP ||
                  heap->gc_phase == NGGC_PHASE_SWEEP) &&
                 (heap->evacuation_copy_started == 0u ||
                  heap->evacuation_workers_quiescent == 0u ||
                  evacuation_workers_active != 0u)) ||
                (heap->gc_phase == NGGC_PHASE_REMAP &&
                 heap->from_space_reclaim_pending == 0u)) {
                return NGGC_ERR_CORRUPTION;
            }
            if (heap->collector_expected_phase == NGGC_PHASE_REMAP) {
                if (heap->gc_phase == NGGC_PHASE_IDLE &&
                    (heap->remap_worker_epoch != 0u ||
                     remap_workers_active != 0u ||
                     heap->remap_workers_quiescent != 0u)) {
                    return NGGC_ERR_CORRUPTION;
                }
            } else if (heap->collector_expected_phase ==
                       NGGC_PHASE_SWEEP) {
                if (heap->remap_worker_epoch != heap->evacuation_epoch ||
                    remap_workers_active != 0u ||
                    heap->remap_workers_quiescent == 0u) {
                    return NGGC_ERR_CORRUPTION;
                }
            } else if (heap->remap_worker_epoch != 0u ||
                       remap_workers_active != 0u ||
                       heap->remap_workers_quiescent != 0u) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (heap->evacuation_worker_epoch != 0u ||
                   evacuation_workers_active != 0u ||
                   heap->evacuation_workers_quiescent != 0u ||
                   heap->remap_worker_epoch != 0u ||
                   remap_workers_active != 0u ||
                   heap->remap_workers_quiescent != 0u ||
                   heap->evacuation_copy_started != 0u ||
                   heap->from_space_reclaim_pending != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (heap->collector_expected_phase != NGGC_PHASE_IDLE ||
               heap->collector_cycle_kind != NGGC_COLLECTOR_CYCLE_NONE ||
               heap->mutator_concurrent_active != 0u ||
               heap->mutator_concurrent_epoch != 0u ||
               heap->evacuation_worker_epoch != 0u ||
               evacuation_workers_active != 0u ||
               heap->evacuation_workers_quiescent != 0u ||
               heap->remap_worker_epoch != 0u ||
               remap_workers_active != 0u ||
               heap->remap_workers_quiescent != 0u ||
               heap->evacuation_copy_started != 0u ||
               heap->from_space_reclaim_pending != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->collect_safepoint_epoch != 0u &&
        (heap->collect_safepoint_epoch != heap->safepoint_epoch ||
         !heap->safepoint_active)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->collect_safepoint_owner &&
        (heap->collect_safepoint_epoch == 0u ||
         !heap->safepoint_active)) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_evacuation_epoch_begin_locked(nggc_heap *heap);

static nggc_result nggc_collector_cycle_begin_locked(
    nggc_heap *heap, uint32_t cycle_kind) {
    nggc_result result = nggc_verify_phase_state_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    if ((cycle_kind != NGGC_COLLECTOR_CYCLE_NON_MOVING &&
         cycle_kind != NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) ||
        heap->collector_cycle_active ||
        heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->from_space_quarantine_active ||
        heap->safepoint_pending_threads != 0u) {
        return NGGC_ERR_BUSY;
    }
    if (cycle_kind == NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) {
        result = nggc_evacuation_epoch_begin_locked(heap);
        if (result != NGGC_OK) {
            return result;
        }
    }
    ++heap->collector_cycle_epoch;
    if (heap->collector_cycle_epoch == 0u) {
        heap->collector_cycle_epoch = 1u;
    }
    heap->collector_cycle_active = 1u;
    heap->collector_expected_phase = NGGC_PHASE_ROOT_SNAPSHOT;
    heap->collector_cycle_kind = cycle_kind;
    return nggc_verify_phase_state_locked(heap);
}

static nggc_result nggc_collector_cycle_abort_locked(nggc_heap *heap) {
    nggc_result discovered_result;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (!heap->collector_cycle_active ||
        heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->from_space_quarantine_active ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u ||
        atomic_load_explicit(&heap->remap_workers_active,
                             memory_order_acquire) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->evacuation_worker_epoch = 0u;
    heap->evacuation_workers_quiescent = 0u;
    heap->remap_worker_epoch = 0u;
    heap->remap_workers_quiescent = 0u;
    heap->evacuation_copy_started = 0u;
    heap->from_space_reclaim_pending = 0u;
    heap->mutator_concurrent_active = 0u;
    heap->mutator_concurrent_epoch = 0u;
    heap->mutator_concurrent_running_threads = 0u;
    heap->mutator_concurrent_repark_pending_threads = 0u;
    heap->mutator_concurrent_repark_target_threads = 0u;
    heap->mutator_concurrent_repark_requested = 0u;
    discovered_result = nggc_reference_discovered_clear_locked(heap);
    heap->collector_cycle_active = 0u;
    heap->collector_expected_phase = NGGC_PHASE_IDLE;
    heap->collector_cycle_kind = NGGC_COLLECTOR_CYCLE_NONE;
    if (discovered_result != NGGC_OK) {
        return discovered_result;
    }
    return nggc_verify_phase_state_locked(heap);
}

static nggc_result nggc_collector_cycle_advance_locked(
    nggc_heap *heap, uint32_t phase) {
    if (!heap || !heap->collector_cycle_active ||
        phase != heap->collector_expected_phase) {
        return NGGC_ERR_CORRUPTION;
    }
    switch (phase) {
        case NGGC_PHASE_ROOT_SNAPSHOT:
            heap->collector_expected_phase = NGGC_PHASE_MARK;
            break;
        case NGGC_PHASE_MARK:
            heap->collector_expected_phase =
                NGGC_PHASE_REFERENCE_PROCESS;
            break;
        case NGGC_PHASE_REFERENCE_PROCESS:
            heap->collector_expected_phase =
                heap->collector_cycle_kind ==
                        NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION
                    ? NGGC_PHASE_EVACUATE
                    : NGGC_PHASE_SWEEP;
            break;
        case NGGC_PHASE_EVACUATE:
            if (heap->collector_cycle_kind !=
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) {
                return NGGC_ERR_CORRUPTION;
            }
            heap->collector_expected_phase = NGGC_PHASE_REMAP;
            break;
        case NGGC_PHASE_REMAP:
            if (heap->collector_cycle_kind !=
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) {
                return NGGC_ERR_CORRUPTION;
            }
            heap->collector_expected_phase = NGGC_PHASE_SWEEP;
            break;
        case NGGC_PHASE_SWEEP:
            if (heap->collector_cycle_kind ==
                    NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) {
                if (atomic_load_explicit(
                        &heap->evacuation_workers_active,
                        memory_order_acquire) != 0u ||
                    atomic_load_explicit(&heap->remap_workers_active,
                                         memory_order_acquire) != 0u ||
                    heap->remap_worker_epoch != heap->evacuation_epoch ||
                    heap->remap_workers_quiescent == 0u ||
                    heap->from_space_reclaim_pending != 0u) {
                    return NGGC_ERR_CORRUPTION;
                }
                heap->evacuation_worker_epoch = 0u;
                heap->evacuation_workers_quiescent = 0u;
                heap->remap_worker_epoch = 0u;
                heap->remap_workers_quiescent = 0u;
                heap->evacuation_copy_started = 0u;
                heap->mutator_concurrent_active = 0u;
                heap->mutator_concurrent_epoch = 0u;
                heap->mutator_concurrent_running_threads = 0u;
                heap->mutator_concurrent_repark_pending_threads = 0u;
                heap->mutator_concurrent_repark_target_threads = 0u;
                heap->mutator_concurrent_repark_requested = 0u;
            }
            heap->collector_cycle_active = 0u;
            heap->collector_expected_phase = NGGC_PHASE_IDLE;
            heap->collector_cycle_kind = NGGC_COLLECTOR_CYCLE_NONE;
            break;
        default:
            return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_remap_forwarded_slots_locked(nggc_heap *heap);
static void nggc_allocator_lease_revoke_locked(nggc_heap *heap);
#if NGGC_HAVE_C11_THREADS
static size_t nggc_gc_pool_ensure_locked(nggc_heap *heap,
                                         size_t desired_workers,
                                         int fail_next_create);
static nggc_result nggc_gc_pool_dispatch_locked(
    nggc_heap *heap, nggc_gc_worker_job *job,
    size_t requested_workers);
static nggc_result nggc_gc_pool_wait_locked(
    nggc_heap *heap, nggc_gc_worker_job *job,
    size_t requested_workers, int *out_drained);
#endif

static nggc_result nggc_phase_begin_locked(nggc_heap *heap, uint32_t phase) {
    nggc_result result;
    if (!nggc_phase_valid(phase)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_verify_phase_state_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    if (heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->safepoint_pending_threads != 0) {
        return NGGC_ERR_BUSY;
    }
    if (heap->from_space_quarantine_active) {
        return NGGC_ERR_BUSY;
    }
    if (heap->collector_cycle_active) {
        if (phase != heap->collector_expected_phase) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->collector_cycle_kind ==
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION &&
            (phase == NGGC_PHASE_REMAP || phase == NGGC_PHASE_SWEEP) &&
            (heap->evacuation_copy_started == 0u ||
             heap->evacuation_workers_quiescent == 0u ||
             heap->from_space_reclaim_pending == 0u ||
             atomic_load_explicit(&heap->evacuation_workers_active,
                                  memory_order_acquire) != 0u)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->collector_cycle_kind ==
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION &&
            phase == NGGC_PHASE_SWEEP &&
            (heap->remap_worker_epoch != heap->evacuation_epoch ||
             heap->remap_workers_quiescent == 0u ||
             atomic_load_explicit(&heap->remap_workers_active,
                                  memory_order_acquire) != 0u)) {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (heap->collect_safepoint_epoch != 0u) {
        return NGGC_ERR_BUSY;
    }
    if (heap->phase_transition_count == UINT64_MAX) {
        return NGGC_ERR_CORRUPTION;
    }
    nggc_allocator_lease_revoke_locked(heap);
    heap->gc_phase = phase;
    heap->read_barrier_active = nggc_expected_read_barrier_locked(heap);
    ++heap->phase_transition_count;
    return nggc_verify_phase_state_locked(heap);
}

static nggc_result nggc_phase_end_locked(nggc_heap *heap, uint32_t phase) {
    nggc_result result;
    if (!nggc_phase_valid(phase) || heap->gc_phase != phase) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_verify_phase_state_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    if (phase == NGGC_PHASE_REMAP) {
        result = nggc_remap_forwarded_slots_locked(heap);
        if (result != NGGC_OK) {
            return result;
        }
    }
    if (heap->phase_transition_count == UINT64_MAX) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->gc_phase = NGGC_PHASE_IDLE;
    heap->read_barrier_active = nggc_expected_read_barrier_locked(heap);
    ++heap->phase_transition_count;
    if (heap->collector_cycle_active) {
        result = nggc_collector_cycle_advance_locked(heap, phase);
        if (result != NGGC_OK) {
            return result;
        }
    }
    return nggc_verify_phase_state_locked(heap);
}

static void nggc_threads_destroy_all(nggc_heap *heap) {
    nggc_thread *thread;
    if (!heap) {
        return;
    }
    nggc_pin_lease_dispose_all(heap);
    thread = heap->threads;
    while (thread) {
        nggc_thread *next = thread->next;
        thread->magic = NGGC_THREAD_CLOSED_MAGIC;
        thread->heap = 0;
        thread->next = 0;
        free(thread->roots);
        free(thread->remset_card_buffer);
        free(thread);
        thread = next;
    }
    heap->threads = 0;
    heap->registered_thread_count = 0;
}

static int nggc_is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static int nggc_align_up_size(size_t value, size_t alignment, size_t *out) {
    size_t mask;
    if (!out || !nggc_is_power_of_two(alignment)) {
        return 0;
    }
    mask = alignment - 1u;
    if (value > SIZE_MAX - mask) {
        return 0;
    }
    *out = (value + mask) & ~mask;
    return 1;
}

static int nggc_range_end(uintptr_t addr, size_t size, uintptr_t *out) {
    if (!out || size > (size_t)(UINTPTR_MAX - addr)) {
        return 0;
    }
    *out = addr + size;
    return 1;
}

static int nggc_round_up_multiple_size(size_t value, size_t multiple,
                                       size_t *out) {
    size_t remainder;
    size_t add;
    if (!out || multiple == 0u) {
        return 0;
    }
    remainder = value % multiple;
    if (remainder == 0u) {
        *out = value;
        return 1;
    }
    add = multiple - remainder;
    if (value > SIZE_MAX - add) {
        return 0;
    }
    *out = value + add;
    return 1;
}

static nggc_result nggc_validate_heap_geometry(
    size_t gc_arena_size, size_t region_size, size_t tlab_size,
    uint32_t tlab_sizing_policy, size_t tlab_min_size,
    size_t tlab_max_size,
    size_t *out_region_count, size_t *out_card_count) {
    size_t region_count;
    size_t card_count;
    if (!out_region_count || !out_card_count ||
        gc_arena_size < NGGC_OBJECT_HEADER_BYTES ||
        (gc_arena_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        region_size < NGGC_MIN_REGION_SIZE ||
        region_size > UINT32_MAX ||
        (region_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        (region_size % NGGC_DEFAULT_CARD_SIZE) != 0u ||
        !nggc_tlab_sizing_policy_resolved_valid(tlab_sizing_policy) ||
        tlab_size < NGGC_OBJECT_HEADER_BYTES ||
        tlab_size > region_size ||
        (tlab_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        tlab_min_size < NGGC_OBJECT_HEADER_BYTES ||
        tlab_min_size > tlab_size ||
        (tlab_min_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        tlab_max_size < tlab_size ||
        tlab_max_size > region_size ||
        (tlab_max_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        (tlab_sizing_policy == NGGC_TLAB_SIZING_POLICY_FIXED &&
         (tlab_min_size != tlab_size || tlab_max_size != tlab_size)) ||
        gc_arena_size > SIZE_MAX - (region_size - 1u) ||
        gc_arena_size > SIZE_MAX - (NGGC_DEFAULT_CARD_SIZE - 1u)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    region_count =
        (gc_arena_size + region_size - 1u) / region_size;
    card_count =
        (gc_arena_size + NGGC_DEFAULT_CARD_SIZE - 1u) /
        NGGC_DEFAULT_CARD_SIZE;
    if (region_count == 0u || card_count == 0u ||
        region_count > UINT32_MAX || card_count > UINT32_MAX ||
        region_count > SIZE_MAX / sizeof(nggc_region_meta) ||
        card_count > SIZE_MAX / sizeof(atomic_uchar)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_region_count = region_count;
    *out_card_count = card_count;
    return NGGC_OK;
}

static int nggc_tlab_reserved_block_matches_locked(
    nggc_heap *heap, uintptr_t start, uintptr_t end) {
    size_t index;
    nggc_region_meta *region;
    nggc_thread *thread;
    if (!heap || !heap->regions || start < (uintptr_t)heap->gc_arena_base ||
        start >= end || end > heap->arena_end) {
        return 0;
    }
    index = (size_t)((start - (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
    if (index >= heap->region_count) {
        return 0;
    }
    region = &heap->regions[index];
    thread = region->tlab_owner;
    return (region->tlab_state == NGGC_TLAB_ACTIVE ||
            region->tlab_state == NGGC_TLAB_RETIRING) &&
           nggc_thread_live(thread, heap) &&
           region->tlab_lease_epoch != 0u &&
           region->tlab_reserved_start <= start &&
           region->tlab_published_top == start &&
           region->tlab_reserved_end == end && end <= region->end &&
           thread->tlab_region_index == index &&
           thread->tlab_lease_epoch == region->tlab_lease_epoch &&
           thread->tlab_reserved_start == region->tlab_reserved_start &&
           thread->tlab_published_top == start &&
           thread->tlab_reserved_end == end &&
           thread->tlab_state == region->tlab_state;
}

static int nggc_interior_decommitted_run_locked(
    const nggc_heap *heap, size_t start_index,
    uint32_t *out_run_length, uint32_t *out_placement) {
    const nggc_region_meta *start;
    uint32_t run_length;
    uint32_t placement;
    if (!heap || !heap->regions || start_index >= heap->region_count) {
        return 0;
    }
    start = &heap->regions[start_index];
    run_length = start->humongous_run_length;
    placement = start->placement;
    if (!start->decommitted ||
        start->humongous_role != NGGC_REGION_HUMONGOUS_START ||
        start->humongous_run_offset != 0u || run_length < 2u ||
        run_length > heap->region_count - start_index ||
        (placement != NGGC_REGION_PLACEMENT_MOVABLE_SPECIAL &&
         placement != NGGC_REGION_PLACEMENT_PINNED_SPECIAL) ||
        start->start >= heap->arena_cursor ||
        run_length >
            (size_t)(heap->arena_cursor - start->start) /
                heap->region_size) {
        return 0;
    }
    for (uint32_t offset = 0u; offset < run_length; ++offset) {
        const nggc_region_meta *region =
            &heap->regions[start_index + (size_t)offset];
        uint32_t expected_role =
            offset == 0u ? NGGC_REGION_HUMONGOUS_START
                         : NGGC_REGION_HUMONGOUS_CONTINUATION;
        if (!region->decommitted ||
            region->humongous_role != expected_role ||
            region->humongous_run_offset != offset ||
            region->humongous_run_length != run_length ||
            region->placement != placement ||
            region->allocation_cursor != region->start ||
            region->allocation_owner != 0u ||
            region->committed_bytes != 0u ||
            region->live_bytes != 0u || region->free_bytes != 0u ||
            region->reserved_bytes != 0u ||
            region->quarantined_bytes != 0u ||
            region->live_object_count != 0u ||
            region->quarantined_object_count != 0u ||
            region->large_object_count != 0u ||
            region->pinnable_object_count != 0u ||
            region->pinned_object_count != 0u ||
            region->non_movable_object_count != 0u ||
            region->state != NGGC_REGION_STATE_INACTIVE ||
            region->generation != NGGC_REGION_GENERATION_NONE ||
            region->flags != 0u || region->in_collection_set != 0u ||
            region->tlab_owner != 0 ||
            region->tlab_lease_epoch != 0u ||
            region->tlab_reserved_start != 0u ||
            region->tlab_published_top != 0u ||
            region->tlab_reserved_end != 0u ||
            region->tlab_state != NGGC_TLAB_INACTIVE ||
            (offset != 0u && region->decommitted_free_next != 0u) ||
            (size_t)(region->end - region->start) !=
                heap->region_size) {
            return 0;
        }
    }
    if (out_run_length) {
        *out_run_length = run_length;
    }
    if (out_placement) {
        *out_placement = placement;
    }
    return 1;
}

static nggc_result nggc_block_view_locked(nggc_heap *heap,
                                           uintptr_t cursor,
                                           nggc_block_view *out) {
    nggc_object_header *header;
    uintptr_t end;
    uint32_t state;
    if (!heap || !out || cursor < (uintptr_t)heap->gc_arena_base ||
        cursor >= heap->arena_cursor) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    {
        size_t region_index =
            (size_t)((cursor - (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->regions[region_index].decommitted) {
            uint32_t run_length;
            if (cursor != heap->regions[region_index].start ||
                !nggc_interior_decommitted_run_locked(
                    heap, region_index, &run_length, 0) ||
                run_length > SIZE_MAX / heap->region_size ||
                !nggc_range_end(cursor,
                                (size_t)run_length * heap->region_size,
                                &end) ||
                end > heap->arena_cursor) {
                return NGGC_ERR_CORRUPTION;
            }
            out->header = 0;
            out->start = cursor;
            out->end = end;
            out->state = NGGC_OBJECT_DECOMMITTED_HUMONGOUS;
            return NGGC_OK;
        }
    }
    header = (nggc_object_header *)cursor;
    if (header->magic != NGGC_OBJECT_MAGIC ||
        header->alloc_size < NGGC_OBJECT_HEADER_BYTES ||
        (header->alloc_size % NGGC_OBJECT_ALIGNMENT) != 0u ||
        !nggc_range_end(cursor, header->alloc_size, &end) ||
        end > heap->arena_cursor) {
        return NGGC_ERR_CORRUPTION;
    }
    state = atomic_load_explicit(&header->state, memory_order_acquire);
    if (state != NGGC_OBJECT_LIVE && state != NGGC_OBJECT_FREE &&
        state != NGGC_OBJECT_TLAB_RESERVED &&
        state != NGGC_OBJECT_FORWARDED &&
        !(state == NGGC_OBJECT_ALLOCATING &&
          heap->mutator_concurrent_active)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (state == NGGC_OBJECT_TLAB_RESERVED &&
        (header->type_id != NGGC_TYPE_NONE || header->size_class != 0u ||
         header->flags != 0u ||
         nggc_forwarding_load_relaxed(header) != 0 ||
         nggc_forwarding_epoch_load_acquire(header) != 0u ||
         nggc_pin_count_load(header) != 0u ||
         header->payload_size != 0u || header->object_id != 0u ||
         header->mark_epoch != 0u ||
         header->generation != NGGC_REGION_GENERATION_NONE ||
         header->young_age != 0u ||
         !nggc_tlab_reserved_block_matches_locked(heap, cursor, end))) {
        return NGGC_ERR_CORRUPTION;
    }
    if (state == NGGC_OBJECT_FORWARDED &&
        (((header->type_id == NGGC_TYPE_NONE &&
           (header->flags & NGGC_OBJECT_FLAG_INTERNAL) == 0u) ||
          (header->type_id != NGGC_TYPE_NONE &&
           !heap->types[header->type_id].registered)) ||
         nggc_forwarding_load_acquire(header) == 0 ||
         nggc_forwarding_epoch_load_acquire(header) == 0u ||
         header->payload_size > header->alloc_size -
                                    NGGC_OBJECT_HEADER_BYTES ||
         header->object_id == 0u ||
         nggc_pin_count_load(header) != 0u ||
         header->generation != NGGC_REGION_GENERATION_YOUNG ||
         header->young_age > heap->young_tenuring_threshold)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (state == NGGC_OBJECT_FREE && nggc_pin_count_load(header) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    out->header = header;
    out->start = cursor;
    out->end = end;
    out->state = state;
    return NGGC_OK;
}

static uint64_t nggc_saturating_add_u64_size(uint64_t value, size_t add) {
    uint64_t wide_add = (uint64_t)add;
    if (UINT64_MAX - value < wide_add) {
        return UINT64_MAX;
    }
    return value + wide_add;
}

static size_t nggc_saturating_add_size(size_t value, size_t add) {
    if (SIZE_MAX - value < add) {
        return SIZE_MAX;
    }
    return value + add;
}

static uint64_t nggc_monotonic_delta_u64(uint64_t current,
                                         uint64_t previous) {
    return current >= previous ? current - previous : 0u;
}

static uint32_t nggc_size_ratio_basis_points(size_t numerator,
                                             size_t denominator) {
    uint32_t low = 0u;
    uint32_t high = 10000u;
    size_t denominator_base;
    size_t denominator_remainder;
    if (denominator == 0u || numerator == 0u) {
        return 0u;
    }
    if (numerator >= denominator) {
        return 10000u;
    }
    denominator_base = denominator / (size_t)10000u;
    denominator_remainder = denominator % (size_t)10000u;
    while (low < high) {
        uint32_t middle = low + (high - low + 1u) / 2u;
        size_t remainder_product =
            denominator_remainder * (size_t)middle;
        size_t required = denominator_base * (size_t)middle;
        required += (remainder_product + (size_t)9999u) /
                    (size_t)10000u;
        if (numerator >= required) {
            low = middle;
        } else {
            high = middle - 1u;
        }
    }
    return low;
}

static nggc_result nggc_clock_now_ns_locked(nggc_heap *heap,
                                             uint64_t *out_ns) {
    if (!heap || !out_ns) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_ns = 0u;
    return heap->clock_ns
               ? heap->clock_ns(heap->clock_ns_ctx, out_ns)
               : NGGC_ERR_UNSUPPORTED;
}

static uint64_t nggc_latency_ewma_update(uint64_t current,
                                         uint64_t sample) {
    uint64_t distance;
    uint64_t adjustment;
    if (current == 0u) {
        return sample;
    }
    if (sample >= current) {
        distance = sample - current;
        adjustment = distance / 8u + (distance % 8u != 0u);
        return current > UINT64_MAX - adjustment
                   ? UINT64_MAX
                   : current + adjustment;
    }
    distance = current - sample;
    adjustment = distance / 8u + (distance % 8u != 0u);
    return current - adjustment;
}

static void nggc_trigger_clock_failure_locked(nggc_heap *heap) {
    if (heap && heap->gc_trigger_clock_failure_count != UINT64_MAX) {
        ++heap->gc_trigger_clock_failure_count;
    }
}

static int nggc_trigger_latency_start_locked(nggc_heap *heap,
                                             uint64_t *out_begin_ns) {
    if (!heap || !out_begin_ns || heap->gc_trigger_latency_target_ns == 0u ||
        !heap->clock_ns) {
        return 0;
    }
    *out_begin_ns = 0u;
    if (nggc_clock_now_ns_locked(heap, out_begin_ns) != NGGC_OK) {
        nggc_trigger_clock_failure_locked(heap);
        return 0;
    }
    return 1;
}

static void nggc_trigger_latency_finish_locked(nggc_heap *heap,
                                               int started,
                                               uint64_t begin_ns) {
    uint64_t end_ns = 0u;
    uint64_t elapsed_ns;
    if (!heap || !started) {
        return;
    }
    if (nggc_clock_now_ns_locked(heap, &end_ns) != NGGC_OK ||
        end_ns < begin_ns) {
        nggc_trigger_clock_failure_locked(heap);
        return;
    }
    elapsed_ns = end_ns - begin_ns;
    heap->gc_trigger_last_pause_ns = elapsed_ns;
    heap->gc_trigger_pause_ewma_ns = nggc_latency_ewma_update(
        heap->gc_trigger_pause_ewma_ns, elapsed_ns);
    if (heap->gc_trigger_pause_sample_count != UINT64_MAX) {
        ++heap->gc_trigger_pause_sample_count;
    }
}

static void nggc_region_vm_latency_record_locked(
    nggc_heap *heap, size_t bytes, uint64_t begin_ns, uint64_t end_ns,
    int decommit) {
    size_t region_count;
    uint64_t elapsed_ns;
    uint64_t sample_ns_per_region;
    uint64_t *ewma;
    uint64_t *sample_count;
    if (!heap || heap->region_size == 0u || bytes == 0u ||
        end_ns < begin_ns) {
        return;
    }
    region_count = bytes / heap->region_size;
    if ((bytes % heap->region_size) != 0u) {
        if (region_count == SIZE_MAX) {
            return;
        }
        ++region_count;
    }
    if (region_count == 0u) {
        return;
    }
    elapsed_ns = end_ns - begin_ns;
    sample_ns_per_region = elapsed_ns / (uint64_t)region_count;
    if (sample_ns_per_region == 0u && elapsed_ns != 0u) {
        sample_ns_per_region = 1u;
    }
    ewma = decommit
               ? &heap->region_decommit_latency_ewma_ns_per_region
               : &heap->region_recommit_latency_ewma_ns_per_region;
    sample_count = decommit
                       ? &heap->region_decommit_latency_sample_count
                       : &heap->region_recommit_latency_sample_count;
    *ewma = nggc_latency_ewma_update(*ewma, sample_ns_per_region);
    if (*sample_count != UINT64_MAX) {
        ++*sample_count;
    }
}

static nggc_result nggc_region_backend_vm_locked(
    nggc_heap *heap, void *addr, size_t bytes, int decommit) {
    uint64_t begin_ns = 0u;
    uint64_t end_ns = 0u;
    int clock_started;
    nggc_result result;
    if (!heap || !addr || bytes == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (!heap->clock_ns) {
        return decommit
                   ? nggc_backend_decommit(heap->backend, addr, bytes)
                   : nggc_backend_recommit(heap->backend, addr, bytes);
    }
    clock_started =
        nggc_clock_now_ns_locked(heap, &begin_ns) == NGGC_OK;
    result = decommit
                 ? nggc_backend_decommit(heap->backend, addr, bytes)
                 : nggc_backend_recommit(heap->backend, addr, bytes);
    if (!clock_started ||
        nggc_clock_now_ns_locked(heap, &end_ns) != NGGC_OK ||
        end_ns < begin_ns) {
        if (heap->region_vm_clock_failure_count != UINT64_MAX) {
            ++heap->region_vm_clock_failure_count;
        }
    } else if (result == NGGC_OK) {
        nggc_region_vm_latency_record_locked(
            heap, bytes, begin_ns, end_ns, decommit);
    }
    return result;
}

static int nggc_memory_pressure_sample_locked(
    nggc_heap *heap, uint32_t *out_pressure_basis_points) {
    nggc_memory_pressure_sample sample;
    nggc_result result;
    uint32_t pressure;
    if (!heap || !out_pressure_basis_points || !heap->memory_pressure) {
        return 0;
    }
    memset(&sample, 0, sizeof(sample));
    sample.size = sizeof(sample);
    result = heap->memory_pressure(heap->memory_pressure_ctx, &sample);
    if (result != NGGC_OK || sample.size < sizeof(sample) ||
        (sample.flags & ~NGGC_MEMORY_PRESSURE_KNOWN_FLAGS) != 0u ||
        ((sample.flags & NGGC_MEMORY_PRESSURE_DIRECT_VALID) != 0u &&
         sample.pressure_basis_points > 10000u)) {
        if (heap->interior_decommit_smart_external_failure_count !=
            UINT64_MAX) {
            ++heap->interior_decommit_smart_external_failure_count;
        }
        return 0;
    }
    if ((sample.flags & NGGC_MEMORY_PRESSURE_DIRECT_VALID) != 0u) {
        pressure = sample.pressure_basis_points;
    } else if ((sample.flags &
                (NGGC_MEMORY_PRESSURE_RESIDENT_VALID |
                 NGGC_MEMORY_PRESSURE_LIMIT_VALID)) ==
                   (NGGC_MEMORY_PRESSURE_RESIDENT_VALID |
                    NGGC_MEMORY_PRESSURE_LIMIT_VALID) &&
               sample.limit_bytes != 0u) {
        pressure = nggc_size_ratio_basis_points(
            sample.resident_bytes, sample.limit_bytes);
    } else {
        if (heap->interior_decommit_smart_external_failure_count !=
            UINT64_MAX) {
            ++heap->interior_decommit_smart_external_failure_count;
        }
        return 0;
    }
    heap->interior_decommit_smart_resident_bytes =
        (sample.flags & NGGC_MEMORY_PRESSURE_RESIDENT_VALID) != 0u
            ? sample.resident_bytes
            : 0u;
    heap->interior_decommit_smart_memory_limit_bytes =
        (sample.flags & NGGC_MEMORY_PRESSURE_LIMIT_VALID) != 0u
            ? sample.limit_bytes
            : 0u;
    *out_pressure_basis_points = pressure;
    if (heap->interior_decommit_smart_external_sample_count != UINT64_MAX) {
        ++heap->interior_decommit_smart_external_sample_count;
    }
    return 1;
}

static void nggc_assist_charge_allocation_locked(nggc_heap *heap,
                                                 nggc_thread *thread,
                                                 size_t bytes) {
    if (!heap || bytes == 0) {
        return;
    }
    heap->mutator_assist_charged_bytes =
        nggc_saturating_add_u64_size(heap->mutator_assist_charged_bytes,
                                     bytes);
    heap->mutator_assist_debt_bytes =
        nggc_saturating_add_size(heap->mutator_assist_debt_bytes, bytes);
    if (thread && nggc_thread_is_registered(heap, thread)) {
        thread->mutator_assist_debt_bytes = nggc_saturating_add_size(
            thread->mutator_assist_debt_bytes, bytes);
    }
}

static size_t nggc_assist_repay_thread_locked(nggc_thread *thread,
                                               size_t bytes) {
    size_t repaid;
    if (!thread || bytes == 0u) {
        return 0u;
    }
    repaid = bytes < thread->mutator_assist_debt_bytes
                 ? bytes
                 : thread->mutator_assist_debt_bytes;
    thread->mutator_assist_debt_bytes -= repaid;
    return repaid;
}

static void nggc_assist_repay_work_locked(nggc_heap *heap,
                                          nggc_thread *thread,
                                          size_t bytes) {
    size_t requested;
    size_t repaid = 0u;
    size_t remaining;
    nggc_thread *candidate;
    if (!heap || bytes == 0 || heap->mutator_assist_debt_bytes == 0) {
        return;
    }
    requested = bytes < heap->mutator_assist_debt_bytes
                    ? bytes
                    : heap->mutator_assist_debt_bytes;
    if (thread && nggc_thread_is_registered(heap, thread)) {
        repaid = nggc_assist_repay_thread_locked(thread, requested);
    }
    remaining = requested - repaid;
    for (candidate = heap->threads;
         candidate && remaining != 0u;
         candidate = candidate->next) {
        if (candidate != thread) {
            size_t candidate_repaid =
                nggc_assist_repay_thread_locked(candidate, remaining);
            repaid = nggc_saturating_add_size(repaid, candidate_repaid);
            remaining -= candidate_repaid;
        }
    }
    /* Any remaining debt belongs to allocations without a thread owner. */
    repaid = nggc_saturating_add_size(repaid, remaining);
    heap->mutator_assist_debt_bytes -= repaid;
    heap->mutator_assist_repaid_bytes =
        nggc_saturating_add_u64_size(heap->mutator_assist_repaid_bytes,
                                     repaid);
    if (heap->mutator_assist_repay_count != UINT64_MAX) {
        ++heap->mutator_assist_repay_count;
    }
}

static int nggc_assist_other_thread_has_debt_locked(
    const nggc_heap *heap, const nggc_thread *thread) {
    const nggc_thread *candidate;
    if (!heap) {
        return 0;
    }
    for (candidate = heap->threads; candidate; candidate = candidate->next) {
        if (candidate != thread && candidate->mutator_assist_debt_bytes != 0u) {
            return 1;
        }
    }
    return 0;
}

static int nggc_assist_prepare_budget_locked(
    nggc_heap *heap, nggc_thread *thread, size_t alloc_size,
    size_t *out_mark_items, size_t *out_sweep_bytes) {
    size_t debt;
    size_t mark_items = NGGC_ASSIST_MARK_BUDGET_ITEMS;
    size_t sweep_bytes = NGGC_ASSIST_SWEEP_BUDGET_BYTES;

    if (!heap || !out_mark_items || !out_sweep_bytes) {
        return 0;
    }
    if (thread && thread->mutator_assist_backoff_remaining != 0u) {
        --thread->mutator_assist_backoff_remaining;
        if (heap->mutator_assist_backoff_count != UINT64_MAX) {
            ++heap->mutator_assist_backoff_count;
        }
        *out_mark_items = 0u;
        *out_sweep_bytes = 0u;
        return 0;
    }
    debt = heap->mutator_assist_debt_bytes;
    if (thread && thread->mutator_assist_debt_bytes > debt) {
        debt = thread->mutator_assist_debt_bytes;
    }
    if (alloc_size > debt) {
        debt = alloc_size;
    }
    if (debt >= (size_t)65536u) {
        mark_items = NGGC_ASSIST_MARK_BUDGET_MAX_ITEMS;
        sweep_bytes = NGGC_ASSIST_SWEEP_BUDGET_MAX_BYTES;
    } else if (debt >= (size_t)16384u) {
        mark_items = (size_t)24u;
        sweep_bytes = (size_t)4096u;
    } else if (debt >= (size_t)4096u) {
        mark_items = (size_t)16u;
        sweep_bytes = (size_t)2048u;
    }
    if (thread && heap->mutator_assist_last_thread == thread &&
        heap->mutator_assist_consecutive_packets >=
            NGGC_ASSIST_MAX_CONSECUTIVE_PACKETS &&
        nggc_assist_other_thread_has_debt_locked(heap, thread)) {
        mark_items = NGGC_ASSIST_MARK_BUDGET_ITEMS;
        sweep_bytes = NGGC_ASSIST_SWEEP_BUDGET_BYTES;
        heap->mutator_assist_consecutive_packets = 0u;
        if (heap->mutator_assist_fairness_cap_count != UINT64_MAX) {
            ++heap->mutator_assist_fairness_cap_count;
        }
    }
    if (mark_items != NGGC_ASSIST_MARK_BUDGET_ITEMS ||
        sweep_bytes != NGGC_ASSIST_SWEEP_BUDGET_BYTES) {
        if (heap->mutator_assist_dynamic_budget_count != UINT64_MAX) {
            ++heap->mutator_assist_dynamic_budget_count;
        }
    }
    heap->mutator_assist_current_budget_items = mark_items;
    heap->mutator_assist_current_budget_bytes = sweep_bytes;
    *out_mark_items = mark_items;
    *out_sweep_bytes = sweep_bytes;
    return 1;
}

static nggc_object_header *nggc_header_from_object(const void *object) {
    return object ? (nggc_object_header *)((uintptr_t)object -
                                           NGGC_OBJECT_HEADER_BYTES)
                  : 0;
}

static void *nggc_object_from_header(nggc_object_header *header) {
    return header ? (void *)((uintptr_t)header + NGGC_OBJECT_HEADER_BYTES) : 0;
}

static int nggc_type_id_valid(uint16_t type_id) {
    return type_id != NGGC_TYPE_NONE;
}

static int nggc_internal_none_object(const nggc_object_header *header) {
    return header && header->type_id == NGGC_TYPE_NONE &&
           (header->flags & NGGC_OBJECT_FLAG_INTERNAL) != 0u;
}

static int nggc_internal_weak_reference_object(
    const nggc_object_header *header) {
    return nggc_internal_none_object(header) &&
           header->size_class == NGGC_INTERNAL_KIND_WEAK_REFERENCE;
}

static int nggc_internal_soft_reference_object(
    const nggc_object_header *header) {
    return nggc_internal_none_object(header) &&
           header->size_class == NGGC_INTERNAL_KIND_SOFT_REFERENCE;
}

static int nggc_internal_phantom_queue_object(
    const nggc_object_header *header) {
    return nggc_internal_none_object(header) &&
           header->size_class == NGGC_INTERNAL_KIND_PHANTOM_QUEUE;
}

static int nggc_internal_phantom_reference_object(
    const nggc_object_header *header) {
    return nggc_internal_none_object(header) &&
           header->size_class == NGGC_INTERNAL_KIND_PHANTOM_REFERENCE;
}

static int nggc_type_desc_has_scan_ex(const nggc_type_desc *desc) {
    return desc && desc->size >=
                       offsetof(nggc_type_desc, scan_ex) +
                           sizeof(desc->scan_ex);
}

static nggc_result nggc_scan_poll(void *ctx) {
#if NGGC_ENABLE_VERIFY
    nggc_scan_guard *guard = (nggc_scan_guard *)ctx;
    /* Compare before dereferencing: the control is only valid in its own
       callback on the current thread. Poll remains the permitted callback. */
    if (!guard || guard != nggc_scan_guard_active) {
        if (nggc_scan_guard_active) {
            nggc_scan_guard_active->result = NGGC_ERR_CORRUPTION;
        }
        return NGGC_ERR_CORRUPTION;
    }
    if (guard->result != NGGC_OK) {
        return guard->result;
    }
    const nggc_heap *heap = guard->heap;
#else
    const nggc_heap *heap = (const nggc_heap *)ctx;
#endif
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return nggc_heap_live(heap) ? NGGC_OK : NGGC_ERR_BUSY;
}

static nggc_result nggc_invoke_scan(
    nggc_heap *heap, nggc_object_header *owner,
    const nggc_type_desc *desc, nggc_visit_fn visit, void *ctx) {
    nggc_result result = NGGC_OK;
#if NGGC_ENABLE_VERIFY
    nggc_scan_guard guard;
#endif
    if (!heap || !owner || !desc || !visit) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if NGGC_ENABLE_VERIFY
    if (nggc_scan_api_reentry()) {
        return NGGC_ERR_CORRUPTION;
    }
    guard.heap = heap;
    guard.result = NGGC_OK;
    nggc_scan_guard_active = &guard;
#endif
    if (desc->scan_ex) {
        nggc_scan_control control;
        control.size = sizeof(control);
        control.poll = nggc_scan_poll;
#if NGGC_ENABLE_VERIFY
        control.ctx = &guard;
#else
        control.ctx = heap;
#endif
        result = desc->scan_ex(nggc_object_from_header(owner), visit, ctx,
                               &control);
    } else if (desc->scan) {
        desc->scan(nggc_object_from_header(owner), visit, ctx);
    }
#if NGGC_ENABLE_VERIFY
    nggc_scan_guard_active = 0;
    if (guard.result != NGGC_OK) {
        result = guard.result;
    }
#endif
    return result;
}

static nggc_result nggc_validate_type_desc(const nggc_type_desc *desc) {
    if (!desc || desc->size < offsetof(nggc_type_desc, scan_ex)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if ((desc->flags & ~NGGC_TYPE_KNOWN_FLAGS) != 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (nggc_type_desc_has_scan_ex(desc) && desc->scan && desc->scan_ex) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (desc->alignment != 0 &&
        (!nggc_is_power_of_two(desc->alignment) ||
         desc->alignment > NGGC_OBJECT_ALIGNMENT)) {
        return NGGC_ERR_UNSUPPORTED;
    }
    return NGGC_OK;
}

static uint32_t nggc_object_flags_from_type(const nggc_heap *heap,
                                            const nggc_type_desc *desc,
                                            size_t payload_size) {
    uint32_t flags = 0u;
    if (!heap || !desc) {
        return 0u;
    }
    if ((desc->flags & NGGC_TYPE_FLAG_INTERNAL) != 0u) {
        flags |= NGGC_OBJECT_FLAG_INTERNAL;
    }
    if ((desc->flags & NGGC_TYPE_FLAG_PINNABLE) != 0u) {
        flags |= NGGC_OBJECT_FLAG_PINNABLE;
    }
    if ((desc->flags & NGGC_TYPE_FLAG_NON_MOVABLE) != 0u) {
        flags |= NGGC_OBJECT_FLAG_NON_MOVABLE;
    }
    if (payload_size >= heap->region_size) {
        flags |= NGGC_OBJECT_FLAG_LARGE;
    }
    return flags;
}

static int nggc_region_generation_valid(uint32_t generation) {
    return generation == NGGC_REGION_GENERATION_NONE ||
           generation == NGGC_REGION_GENERATION_YOUNG ||
           generation == NGGC_REGION_GENERATION_OLD ||
           generation == NGGC_REGION_GENERATION_SPECIAL;
}

static int nggc_live_object_generation_valid(uint32_t generation) {
    return generation == NGGC_REGION_GENERATION_YOUNG ||
           generation == NGGC_REGION_GENERATION_OLD;
}

static uint32_t nggc_allocation_region_generation(uint32_t object_flags) {
    return (object_flags & (NGGC_OBJECT_FLAG_LARGE |
                            NGGC_OBJECT_FLAG_NON_MOVABLE)) != 0u
               ? NGGC_REGION_GENERATION_SPECIAL
               : NGGC_REGION_GENERATION_YOUNG;
}

static int nggc_object_requires_pinned_region(uint32_t object_flags) {
    return (object_flags & NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u;
}

static uint32_t nggc_region_target_placement(uint32_t object_flags) {
    if (nggc_object_requires_pinned_region(object_flags)) {
        return NGGC_REGION_PLACEMENT_PINNED_SPECIAL;
    }
    return nggc_allocation_region_generation(object_flags) ==
                   NGGC_REGION_GENERATION_SPECIAL
               ? NGGC_REGION_PLACEMENT_MOVABLE_SPECIAL
               : NGGC_REGION_PLACEMENT_NONE;
}

static int nggc_region_placement_valid(uint32_t placement) {
    return placement == NGGC_REGION_PLACEMENT_NONE ||
           placement == NGGC_REGION_PLACEMENT_MOVABLE_SPECIAL ||
           placement == NGGC_REGION_PLACEMENT_PINNED_SPECIAL;
}

static int nggc_region_humongous_role_valid(uint32_t role) {
    return role == NGGC_REGION_HUMONGOUS_NONE ||
           role == NGGC_REGION_HUMONGOUS_START ||
           role == NGGC_REGION_HUMONGOUS_CONTINUATION;
}

static int nggc_region_humongous_geometry(
    const nggc_heap *heap, uintptr_t start, size_t bytes,
    size_t *out_start_index, uint32_t *out_run_length) {
    uintptr_t end;
    size_t offset;
    size_t start_index;
    size_t run_length;
    if (!heap || !heap->regions || heap->region_size == 0u ||
        bytes <= heap->region_size ||
        (bytes % heap->region_size) != 0u ||
        start < (uintptr_t)heap->gc_arena_base ||
        !nggc_range_end(start, bytes, &end) || end > heap->arena_end) {
        return 0;
    }
    offset = (size_t)(start - (uintptr_t)heap->gc_arena_base);
    if ((offset % heap->region_size) != 0u) {
        return 0;
    }
    start_index = offset / heap->region_size;
    run_length = bytes / heap->region_size;
    if (run_length < 2u || run_length > UINT32_MAX ||
        start_index >= heap->region_count ||
        run_length > heap->region_count - start_index) {
        return 0;
    }
    for (size_t i = 0u; i < run_length; ++i) {
        const nggc_region_meta *region =
            &heap->regions[start_index + i];
        if (region->end < region->start ||
            (size_t)(region->end - region->start) != heap->region_size) {
            return 0;
        }
    }
    if (out_start_index) {
        *out_start_index = start_index;
    }
    if (out_run_length) {
        *out_run_length = (uint32_t)run_length;
    }
    return 1;
}

static int nggc_region_humongous_run_matches_locked(
    const nggc_heap *heap, uintptr_t start, size_t bytes) {
    size_t start_index;
    uint32_t run_length;
    if (!nggc_region_humongous_geometry(
            heap, start, bytes, &start_index, &run_length)) {
        return 0;
    }
    for (uint32_t i = 0u; i < run_length; ++i) {
        const nggc_region_meta *region =
            &heap->regions[start_index + (size_t)i];
        uint32_t expected_role =
            i == 0u ? NGGC_REGION_HUMONGOUS_START
                    : NGGC_REGION_HUMONGOUS_CONTINUATION;
        if (region->humongous_role != expected_role ||
            region->humongous_run_offset != i ||
            region->humongous_run_length != run_length) {
            return 0;
        }
    }
    return 1;
}

static nggc_result nggc_region_assign_humongous_run_locked(
    nggc_heap *heap, uintptr_t start, size_t bytes) {
    size_t start_index;
    uint32_t run_length;
    if (!nggc_region_humongous_geometry(
            heap, start, bytes, &start_index, &run_length)) {
        return NGGC_ERR_CORRUPTION;
    }
    for (uint32_t i = 0u; i < run_length; ++i) {
        nggc_region_meta *region =
            &heap->regions[start_index + (size_t)i];
        region->humongous_role =
            i == 0u ? NGGC_REGION_HUMONGOUS_START
                    : NGGC_REGION_HUMONGOUS_CONTINUATION;
        region->humongous_run_offset = i;
        region->humongous_run_length = run_length;
    }
    return NGGC_OK;
}

static int nggc_region_placement_compatible_locked(
    const nggc_region_meta *region, uint32_t generation,
    uint32_t object_flags) {
    uint32_t target_placement;
    if (!region || !nggc_region_placement_valid(region->placement)) {
        return 0;
    }
    target_placement = nggc_region_target_placement(object_flags);
    if (generation != NGGC_REGION_GENERATION_SPECIAL) {
        return target_placement == NGGC_REGION_PLACEMENT_NONE &&
               region->placement == NGGC_REGION_PLACEMENT_NONE;
    }
    if (target_placement == NGGC_REGION_PLACEMENT_NONE) {
        return 0;
    }
    return region->placement == NGGC_REGION_PLACEMENT_NONE ||
           region->placement == target_placement;
}

static void nggc_region_refresh_flags(nggc_region_meta *region);

static nggc_result nggc_region_assign_generation_range_locked(
    nggc_heap *heap, uintptr_t start, size_t bytes, uint32_t generation) {
    uintptr_t end;
    uintptr_t cursor;
    if (!heap || bytes == 0u ||
        generation == NGGC_REGION_GENERATION_NONE ||
        !nggc_region_generation_valid(generation) ||
        !nggc_range_end(start, bytes, &end) ||
        start < (uintptr_t)heap->gc_arena_base || end > heap->arena_end) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = start;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region;
        if (index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[index];
        if (region->decommitted) {
            return NGGC_ERR_BUSY;
        }
        if (region->generation != NGGC_REGION_GENERATION_NONE &&
            region->generation != generation) {
            return NGGC_ERR_BUSY;
        }
        cursor = region->end < end ? region->end : end;
    }
    cursor = start;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region = &heap->regions[index];
        region->generation = generation;
        cursor = region->end < end ? region->end : end;
    }
    return NGGC_OK;
}

static int nggc_region_range_generation_compatible_locked(
    const nggc_heap *heap, uintptr_t start, size_t bytes,
    uint32_t generation) {
    uintptr_t end;
    uintptr_t cursor;
    if (!heap || bytes == 0u || !nggc_range_end(start, bytes, &end) ||
        start < (uintptr_t)heap->gc_arena_base || end > heap->arena_end) {
        return 0;
    }
    cursor = start;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        const nggc_region_meta *region;
        if (index >= heap->region_count) {
            return 0;
        }
        region = &heap->regions[index];
        if (region->decommitted) {
            return 0;
        }
        if (region->generation != NGGC_REGION_GENERATION_NONE &&
            region->generation != generation) {
            return 0;
        }
        cursor = region->end < end ? region->end : end;
    }
    return 1;
}

static int nggc_region_range_allocation_compatible_locked(
    const nggc_heap *heap, uintptr_t start, size_t bytes,
    uint32_t generation, uint32_t object_flags) {
    uintptr_t end;
    uintptr_t cursor;
    if (!heap || bytes == 0u || !nggc_range_end(start, bytes, &end) ||
        start < (uintptr_t)heap->gc_arena_base || end > heap->arena_end ||
        !nggc_region_generation_valid(generation) ||
        generation == NGGC_REGION_GENERATION_NONE) {
        return 0;
    }
    cursor = start;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        const nggc_region_meta *region;
        if (index >= heap->region_count) {
            return 0;
        }
        region = &heap->regions[index];
        if (region->decommitted) {
            return 0;
        }
        if (region->generation != NGGC_REGION_GENERATION_NONE &&
            region->generation != generation) {
            return 0;
        }
        if (!nggc_region_placement_compatible_locked(
                region, generation, object_flags)) {
            return 0;
        }
        cursor = region->end < end ? region->end : end;
    }
    return 1;
}

static nggc_result nggc_region_assign_allocation_range_locked(
    nggc_heap *heap, uintptr_t start, size_t bytes, uint32_t generation,
    uint32_t object_flags) {
    uintptr_t end;
    uintptr_t cursor;
    uint32_t target_placement;
    if (!heap || bytes == 0u ||
        generation == NGGC_REGION_GENERATION_NONE ||
        !nggc_region_generation_valid(generation) ||
        !nggc_range_end(start, bytes, &end) ||
        start < (uintptr_t)heap->gc_arena_base || end > heap->arena_end) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    target_placement = nggc_region_target_placement(object_flags);
    cursor = start;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region;
        if (index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[index];
        if (region->decommitted) {
            return NGGC_ERR_BUSY;
        }
        if (region->generation != NGGC_REGION_GENERATION_NONE &&
            region->generation != generation) {
            return NGGC_ERR_BUSY;
        }
        if (!nggc_region_placement_compatible_locked(
                region, generation, object_flags)) {
            return NGGC_ERR_BUSY;
        }
        cursor = region->end < end ? region->end : end;
    }
    cursor = start;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region = &heap->regions[index];
        region->generation = generation;
        if (generation == NGGC_REGION_GENERATION_SPECIAL &&
            region->placement == NGGC_REGION_PLACEMENT_NONE) {
            region->placement = target_placement;
        }
        cursor = region->end < end ? region->end : end;
    }
    return NGGC_OK;
}

static nggc_result nggc_region_range_adjust_locked(
    nggc_heap *heap, uintptr_t start, size_t bytes, int committed_delta,
    int live_delta, int free_delta, int quarantined_delta) {
    uintptr_t end;
    uintptr_t cursor;
    if (!heap || bytes == 0u || !nggc_range_end(start, bytes, &end) ||
        start < (uintptr_t)heap->gc_arena_base || end > heap->arena_end) {
        return NGGC_ERR_CORRUPTION;
    }
    cursor = start;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region;
        uintptr_t part_end;
        size_t part_bytes;
        if (index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[index];
        if (region->decommitted) {
            return NGGC_ERR_CORRUPTION;
        }
        part_end = end < region->end ? end : region->end;
        part_bytes = (size_t)(part_end - cursor);
        if (committed_delta > 0) {
            if (region->committed_bytes == 0u) {
                ++heap->active_region_count;
            }
            region->committed_bytes += part_bytes;
        }
        if (live_delta > 0) {
            region->live_bytes += part_bytes;
        } else if (live_delta < 0) {
            if (region->live_bytes < part_bytes) {
                return NGGC_ERR_CORRUPTION;
            }
            region->live_bytes -= part_bytes;
        }
        if (free_delta > 0) {
            region->free_bytes += part_bytes;
        } else if (free_delta < 0) {
            if (region->free_bytes < part_bytes) {
                return NGGC_ERR_CORRUPTION;
            }
            region->free_bytes -= part_bytes;
        }
        if (quarantined_delta > 0) {
            region->quarantined_bytes += part_bytes;
        } else if (quarantined_delta < 0) {
            if (region->quarantined_bytes < part_bytes) {
                return NGGC_ERR_CORRUPTION;
            }
            region->quarantined_bytes -= part_bytes;
        }
        nggc_region_refresh_flags(region);
        cursor = part_end;
    }
    return NGGC_OK;
}

static void nggc_region_refresh_flags(nggc_region_meta *region) {
    uint32_t flags = 0u;
    if (!region) {
        return;
    }
    region->state = region->committed_bytes != 0u
                        ? NGGC_REGION_STATE_ACTIVE
                        : NGGC_REGION_STATE_INACTIVE;
    if (region->live_bytes != 0u) {
        flags |= NGGC_REGION_FLAG_HAS_LIVE;
    }
    if (region->free_bytes != 0u) {
        flags |= NGGC_REGION_FLAG_HAS_FREE;
    }
    if (region->large_object_count != 0u) {
        flags |= NGGC_REGION_FLAG_HAS_LARGE;
    }
    if (region->pinned_object_count != 0u ||
        region->non_movable_object_count != 0u) {
        flags |= NGGC_REGION_FLAG_HAS_PINNED;
    }
    if (region->non_movable_object_count != 0u) {
        flags |= NGGC_REGION_FLAG_HAS_NON_MOVABLE;
    }
    if (region->quarantined_bytes != 0u) {
        flags |= NGGC_REGION_FLAG_HAS_QUARANTINE;
    }
    region->flags = flags;
}

static nggc_result nggc_region_object_attributes_adjust_locked(
    nggc_heap *heap, nggc_object_header *header, int delta) {
    uintptr_t cursor;
    uintptr_t end;
    if (!heap || !header || (delta != 1 && delta != -1) ||
        !nggc_range_end((uintptr_t)header, header->alloc_size, &end)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)header;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region;
        if (index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[index];
        if ((header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u) {
            if (delta > 0) {
                ++region->large_object_count;
            } else if (region->large_object_count != 0u) {
                --region->large_object_count;
            } else {
                return NGGC_ERR_CORRUPTION;
            }
        }
        if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) != 0u) {
            if (delta > 0) {
                ++region->pinnable_object_count;
            } else if (region->pinnable_object_count != 0u) {
                --region->pinnable_object_count;
            } else {
                return NGGC_ERR_CORRUPTION;
            }
        }
        if ((header->flags & NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u) {
            if (delta > 0) {
                ++region->non_movable_object_count;
            } else if (region->non_movable_object_count != 0u) {
                --region->non_movable_object_count;
            } else {
                return NGGC_ERR_CORRUPTION;
            }
        }
        nggc_region_refresh_flags(region);
        cursor = region->end < end ? region->end : end;
    }
    return NGGC_OK;
}

static nggc_result nggc_region_pinned_object_adjust_locked(
    nggc_heap *heap, nggc_object_header *header, int delta) {
    uintptr_t cursor;
    uintptr_t end;
    if (!heap || !header || (delta != 1 && delta != -1) ||
        nggc_pin_count_load(header) == 0u ||
        !nggc_range_end((uintptr_t)header, header->alloc_size, &end)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)header;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region;
        if (index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[index];
        if (region->end <= cursor) {
            return NGGC_ERR_CORRUPTION;
        }
        if (delta > 0) {
            if (region->pinned_object_count == SIZE_MAX) {
                return NGGC_ERR_OUT_OF_MEMORY;
            }
        } else if (region->pinned_object_count == 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        cursor = region->end < end ? region->end : end;
    }

    cursor = (uintptr_t)header;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region = &heap->regions[index];
        if (delta > 0) {
            ++region->pinned_object_count;
        } else {
            --region->pinned_object_count;
        }
        nggc_region_refresh_flags(region);
        cursor = region->end < end ? region->end : end;
    }
    return NGGC_OK;
}

static nggc_result nggc_region_publish_object_locked(
    nggc_heap *heap, nggc_object_header *header, int reused_free_block) {
    size_t index;
    nggc_result result;
    if (!heap || !header) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_region_range_adjust_locked(
        heap, (uintptr_t)header, header->alloc_size,
        reused_free_block ? 0 : 1, 1, reused_free_block ? -1 : 0, 0);
    if (result != NGGC_OK) {
        return result;
    }
    index = (size_t)(((uintptr_t)header -
                      (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
    if (index >= heap->region_count) {
        return NGGC_ERR_CORRUPTION;
    }
    ++heap->regions[index].live_object_count;
    if (nggc_region_object_attributes_adjust_locked(heap, header, 1) !=
        NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_region_reclaim_object_locked(
    nggc_heap *heap, nggc_object_header *header) {
    size_t index;
    nggc_result result;
    if (!heap || !header) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_region_object_attributes_adjust_locked(heap, header, -1);
    if (result != NGGC_OK) {
        return result;
    }
    if (nggc_pin_count_load(header) != 0u) {
        result = nggc_region_pinned_object_adjust_locked(heap, header, -1);
        if (result != NGGC_OK) {
            return result;
        }
    }
    result = nggc_region_range_adjust_locked(
        heap, (uintptr_t)header, header->alloc_size, 0, -1, 1, 0);
    if (result != NGGC_OK) {
        return result;
    }
    index = (size_t)(((uintptr_t)header -
                      (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
    if (index >= heap->region_count ||
        heap->regions[index].live_object_count == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    --heap->regions[index].live_object_count;
    return NGGC_OK;
}

static nggc_result nggc_region_quarantine_object_locked(
    nggc_heap *heap, nggc_object_header *header) {
    size_t index;
    nggc_result result;
    if (!heap || !header) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (nggc_pin_count_load(header) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    index = (size_t)(((uintptr_t)header -
                      (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
    if (index >= heap->region_count ||
        heap->regions[index].live_object_count == 0u ||
        heap->regions[index].quarantined_object_count == SIZE_MAX) {
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_region_object_attributes_adjust_locked(heap, header, -1);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_region_range_adjust_locked(
        heap, (uintptr_t)header, header->alloc_size, 0, -1, 0, 1);
    if (result != NGGC_OK) {
        return result;
    }
    --heap->regions[index].live_object_count;
    ++heap->regions[index].quarantined_object_count;
    return NGGC_OK;
}

static nggc_result nggc_region_reclaim_quarantined_object_locked(
    nggc_heap *heap, nggc_object_header *header) {
    size_t index;
    nggc_result result;
    if (!heap || !header) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    index = (size_t)(((uintptr_t)header -
                      (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
    if (index >= heap->region_count ||
        heap->regions[index].quarantined_object_count == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_region_range_adjust_locked(
        heap, (uintptr_t)header, header->alloc_size, 0, 0, 1, -1);
    if (result != NGGC_OK) {
        return result;
    }
    --heap->regions[index].quarantined_object_count;
    return NGGC_OK;
}

static nggc_result nggc_region_refresh_allocation_owner_locked(
    nggc_heap *heap) {
    size_t new_index = SIZE_MAX;
    if (!heap || !heap->regions) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->allocator_owned_region_index != SIZE_MAX) {
        if (heap->allocator_owned_region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        heap->regions[heap->allocator_owned_region_index].allocation_owner =
            0u;
    }
    if (heap->arena_cursor < heap->arena_end) {
        new_index = (size_t)((heap->arena_cursor -
                              (uintptr_t)heap->gc_arena_base) /
                             heap->region_size);
        if (new_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->allocator_lease_active &&
            !heap->regions[new_index].decommitted) {
            heap->regions[new_index].allocation_owner = (uintptr_t)heap;
        }
    }
    heap->allocator_owned_region_index = new_index;
    return NGGC_OK;
}

static void nggc_allocator_lease_revoke_locked(nggc_heap *heap) {
    if (!heap || !heap->allocator_lease_active) {
        return;
    }
    heap->allocator_lease_active = 0u;
    if (heap->allocator_owned_region_index != SIZE_MAX &&
        heap->allocator_owned_region_index < heap->region_count) {
        heap->regions[heap->allocator_owned_region_index].allocation_owner =
            0u;
    }
    if (heap->allocator_lease_revoke_count != UINT64_MAX) {
        ++heap->allocator_lease_revoke_count;
    }
}

static nggc_result nggc_allocator_lease_acquire_locked(nggc_heap *heap) {
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->allocator_lease_active) {
        return NGGC_OK;
    }
    if (heap->safepoint_pending_threads != 0u ||
        heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active) {
        return NGGC_ERR_BUSY;
    }
    ++heap->allocator_lease_epoch;
    if (heap->allocator_lease_epoch == 0u) {
        heap->allocator_lease_epoch = 1u;
    }
    heap->allocator_lease_active = 1u;
    if (heap->allocator_lease_acquire_count != UINT64_MAX) {
        ++heap->allocator_lease_acquire_count;
    }
    return nggc_region_refresh_allocation_owner_locked(heap);
}

static nggc_result nggc_region_publish_cursor_locked(nggc_heap *heap,
                                                      uintptr_t cursor) {
    size_t old_index;
    size_t new_index;
    if (!heap || cursor < (uintptr_t)heap->gc_arena_base ||
        cursor > heap->arena_end) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    old_index = heap->allocator_owned_region_index == SIZE_MAX
                    ? 0u
                    : heap->allocator_owned_region_index;
    new_index = cursor == heap->arena_end
                    ? heap->region_count
                    : (size_t)((cursor -
                                (uintptr_t)heap->gc_arena_base) /
                               heap->region_size);
    if (old_index > new_index || new_index > heap->region_count) {
        return NGGC_ERR_CORRUPTION;
    }
    for (size_t i = old_index; i < new_index; ++i) {
        heap->regions[i].allocation_cursor = heap->regions[i].end;
    }
    if (new_index < heap->region_count) {
        if (cursor < heap->regions[new_index].allocation_cursor ||
            cursor > heap->regions[new_index].end) {
            return NGGC_ERR_CORRUPTION;
        }
        heap->regions[new_index].allocation_cursor = cursor;
    }
    return nggc_region_refresh_allocation_owner_locked(heap);
}

static size_t nggc_region_span_size(const nggc_region_meta *region) {
    return region && region->end >= region->start
               ? (size_t)(region->end - region->start)
               : 0u;
}

static int nggc_region_inactive_empty_locked(
    const nggc_region_meta *region) {
    return region && region->committed_bytes == 0u &&
           region->live_bytes == 0u && region->free_bytes == 0u &&
           region->reserved_bytes == 0u &&
           region->quarantined_bytes == 0u &&
           region->live_object_count == 0u &&
           region->quarantined_object_count == 0u &&
           region->large_object_count == 0u &&
           region->pinnable_object_count == 0u &&
           region->pinned_object_count == 0u &&
           region->non_movable_object_count == 0u &&
           region->state == NGGC_REGION_STATE_INACTIVE &&
           region->generation == NGGC_REGION_GENERATION_NONE &&
           region->flags == 0u && region->in_collection_set == 0u &&
           region->humongous_role == NGGC_REGION_HUMONGOUS_NONE &&
           region->humongous_run_offset == 0u &&
           region->humongous_run_length == 0u &&
           region->decommitted_free_next == 0u &&
           region->allocation_owner == 0u &&
           region->tlab_owner == 0 &&
           region->tlab_lease_epoch == 0u &&
           region->tlab_reserved_start == 0u &&
           region->tlab_published_top == 0u &&
           region->tlab_reserved_end == 0u &&
           region->tlab_state == NGGC_TLAB_INACTIVE;
}

static nggc_result nggc_region_ensure_committed_range_locked(
    nggc_heap *heap, uintptr_t start, size_t bytes) {
    uintptr_t end;
    uintptr_t cursor;
    if (!heap || bytes == 0u || !nggc_range_end(start, bytes, &end) ||
        start < (uintptr_t)heap->gc_arena_base || end > heap->arena_end) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = start;
    while (cursor < end) {
        size_t index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region;
        uintptr_t part_end;
        size_t span;
        nggc_result result;
        if (index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[index];
        part_end = end < region->end ? end : region->end;
        if (region->decommitted) {
            span = nggc_region_span_size(region);
            if (span == 0u ||
                !nggc_region_inactive_empty_locked(region) ||
                heap->decommitted_region_count == 0u ||
                heap->region_decommitted_bytes < span) {
                return NGGC_ERR_CORRUPTION;
            }
            result = nggc_region_backend_vm_locked(
                heap, (void *)region->start, span, 0);
            if (result != NGGC_OK) {
                if (heap->region_recommit_failure_count != UINT64_MAX) {
                    ++heap->region_recommit_failure_count;
                }
                return result;
            }
            region->decommitted = 0u;
            --heap->decommitted_region_count;
            heap->region_decommitted_bytes -= span;
            if (heap->region_recommit_count != UINT64_MAX) {
                ++heap->region_recommit_count;
            }
        }
        cursor = part_end;
    }
    return NGGC_OK;
}

static nggc_result nggc_region_publish_padding_locked(nggc_heap *heap,
                                                       size_t bytes) {
    nggc_object_header *header;
    nggc_result result;
    if (!heap || bytes < NGGC_OBJECT_HEADER_BYTES ||
        (bytes % NGGC_OBJECT_ALIGNMENT) != 0u ||
        heap->arena_cursor >= heap->arena_end ||
        bytes > (size_t)(heap->arena_end - heap->arena_cursor)) {
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_region_ensure_committed_range_locked(
        heap, heap->arena_cursor, bytes);
    if (result != NGGC_OK) {
        return result;
    }
    header = (nggc_object_header *)heap->arena_cursor;
    memset(header, 0, sizeof(*header));
    header->magic = NGGC_OBJECT_MAGIC;
    atomic_init(&header->state, NGGC_OBJECT_FREE);
    nggc_forwarding_init(header);
    nggc_payload_lock_init(header);
    header->type_id = NGGC_TYPE_NONE;
    header->alloc_size = bytes;
    if (nggc_free_block_link_locked(heap, header) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->region_padding_created_bytes += bytes;
    if (heap->region_padding_block_count != UINT64_MAX) {
        ++heap->region_padding_block_count;
    }
    if (nggc_region_range_adjust_locked(
            heap, heap->arena_cursor, bytes, 1, 0, 1, 0) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->arena_cursor += bytes;
    return nggc_region_publish_cursor_locked(heap, heap->arena_cursor);
}

static nggc_result nggc_next_object_id_locked(nggc_heap *heap,
                                               uint64_t *out_object_id) {
    if (!heap || !out_object_id) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->next_object_id == UINT64_MAX) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    ++heap->next_object_id;
    *out_object_id = heap->next_object_id;
    return NGGC_OK;
}

static nggc_result nggc_tlab_object_id_lease_locked(nggc_heap *heap,
                                                     nggc_thread *thread,
                                                     size_t span_size) {
    size_t lease_count;
    uint64_t wide_count;
    if (!heap || !thread || span_size < NGGC_OBJECT_HEADER_BYTES) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    lease_count = span_size / NGGC_OBJECT_HEADER_BYTES;
    wide_count = (uint64_t)lease_count;
    if (lease_count == 0u || wide_count > UINT64_MAX - heap->next_object_id) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    thread->tlab_object_id_next = heap->next_object_id + 1u;
    thread->tlab_object_id_remaining = lease_count;
    heap->next_object_id += wide_count;
    return NGGC_OK;
}

static nggc_result nggc_tlab_take_object_id(nggc_thread *thread,
                                             uint64_t *out_object_id) {
    if (!thread || !out_object_id ||
        thread->tlab_object_id_remaining == 0u ||
        thread->tlab_object_id_next == 0u) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    *out_object_id = thread->tlab_object_id_next;
    --thread->tlab_object_id_remaining;
    if (thread->tlab_object_id_remaining == 0u) {
        thread->tlab_object_id_next = 0u;
    } else {
        ++thread->tlab_object_id_next;
    }
    return NGGC_OK;
}

static nggc_result nggc_tlab_flush_thread_locked(nggc_heap *heap,
                                                  nggc_thread *thread) {
    nggc_region_meta *region;
    uintptr_t cursor;
    uintptr_t fast_top;
    size_t object_count = 0u;
    size_t payload_bytes = 0u;
    size_t alloc_bytes = 0u;
    if (!heap || !thread || !nggc_thread_live(thread, heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(&thread->tlab_fast_inflight,
                             memory_order_acquire) != 0u) {
        return NGGC_ERR_BUSY;
    }
    fast_top = atomic_load_explicit(&thread->tlab_fast_top,
                                    memory_order_acquire);
    if (thread->tlab_state == NGGC_TLAB_INACTIVE) {
        return fast_top == 0u ? NGGC_OK : NGGC_ERR_CORRUPTION;
    }
    if (thread->tlab_region_index >= heap->region_count ||
        thread->tlab_reserved_start > thread->tlab_published_top ||
        thread->tlab_published_top > fast_top ||
        fast_top > thread->tlab_reserved_end) {
        return NGGC_ERR_CORRUPTION;
    }
    region = &heap->regions[thread->tlab_region_index];
    if (region->tlab_owner != thread ||
        region->tlab_state != thread->tlab_state ||
        region->tlab_lease_epoch != thread->tlab_lease_epoch ||
        region->tlab_reserved_start != thread->tlab_reserved_start ||
        region->tlab_published_top != thread->tlab_published_top ||
        region->tlab_reserved_end != thread->tlab_reserved_end) {
        return NGGC_ERR_CORRUPTION;
    }
    cursor = thread->tlab_published_top;
    while (cursor < fast_top) {
        nggc_block_view block;
        nggc_object_header *header;
        nggc_type_desc *desc;
        uint32_t expected_flags;
        size_t block_bytes;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK ||
            block.state != NGGC_OBJECT_LIVE || block.end > fast_top) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        if (!nggc_type_id_valid(header->type_id) ||
            !heap->types[header->type_id].registered ||
            header->size_class != 0u || header->object_id == 0u ||
            header->object_id > heap->next_object_id ||
            header->payload_size >
                header->alloc_size - NGGC_OBJECT_HEADER_BYTES ||
            header->generation != NGGC_REGION_GENERATION_YOUNG ||
            header->young_age != 0u ||
            nggc_forwarding_load_relaxed(header) != 0 ||
            nggc_forwarding_epoch_load_acquire(header) != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        desc = &heap->types[header->type_id].desc;
        expected_flags = nggc_object_flags_from_type(
            heap, desc, header->payload_size);
        if (header->flags != expected_flags ||
            (expected_flags & (NGGC_OBJECT_FLAG_LARGE |
                               NGGC_OBJECT_FLAG_PINNABLE |
                               NGGC_OBJECT_FLAG_NON_MOVABLE)) != 0u ||
            (desc->object_size != 0u &&
             header->payload_size < desc->object_size)) {
            return NGGC_ERR_CORRUPTION;
        }
        block_bytes = (size_t)(block.end - block.start);
        if (object_count == SIZE_MAX ||
            payload_bytes > SIZE_MAX - header->payload_size ||
            alloc_bytes > SIZE_MAX - block_bytes) {
            return NGGC_ERR_CORRUPTION;
        }
        ++object_count;
        payload_bytes += header->payload_size;
        alloc_bytes += block_bytes;
        cursor = block.end;
    }
    if (cursor != fast_top || region->reserved_bytes < alloc_bytes ||
        region->live_bytes > SIZE_MAX - alloc_bytes ||
        region->live_object_count > SIZE_MAX - object_count ||
        heap->live_object_count > SIZE_MAX - object_count ||
        heap->live_payload_bytes > SIZE_MAX - payload_bytes ||
        heap->allocation_count > UINT64_MAX - (uint64_t)object_count) {
        return NGGC_ERR_CORRUPTION;
    }
    if (object_count == 0u) {
        return NGGC_OK;
    }
    region->reserved_bytes -= alloc_bytes;
    region->live_bytes += alloc_bytes;
    region->live_object_count += object_count;
    region->tlab_published_top = fast_top;
    thread->tlab_published_top = fast_top;
    nggc_region_refresh_flags(region);
    heap->live_object_count += object_count;
    heap->live_payload_bytes += payload_bytes;
    heap->allocation_count += (uint64_t)object_count;
    heap->allocated_bytes = nggc_saturating_add_u64_size(
        heap->allocated_bytes, alloc_bytes);
    nggc_assist_charge_allocation_locked(heap, thread, alloc_bytes);
    heap->tlab_allocation_count = nggc_saturating_add_u64_size(
        heap->tlab_allocation_count, object_count);
    heap->tlab_fast_allocation_count = nggc_saturating_add_u64_size(
        heap->tlab_fast_allocation_count, object_count);
    heap->tlab_fast_flushed_object_count = nggc_saturating_add_u64_size(
        heap->tlab_fast_flushed_object_count, object_count);
    if (heap->tlab_fast_flush_count != UINT64_MAX) {
        ++heap->tlab_fast_flush_count;
    }
    return NGGC_OK;
}

static nggc_result nggc_tlab_fast_close_and_flush_locked(nggc_heap *heap) {
    nggc_thread *thread;
    uint32_t previous;
    int pending;
    size_t visited;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    previous = atomic_exchange_explicit(
        &heap->tlab_fast_gate, NGGC_TLAB_FAST_GATE_CLOSED,
        memory_order_acq_rel);
    if (previous > NGGC_TLAB_FAST_GATE_OPEN) {
        return NGGC_ERR_CORRUPTION;
    }
    if (previous == NGGC_TLAB_FAST_GATE_OPEN &&
        heap->tlab_fast_gate_close_count != UINT64_MAX) {
        ++heap->tlab_fast_gate_close_count;
    }
    for (;;) {
        pending = 0;
        visited = 0u;
        for (thread = heap->threads; thread; thread = thread->next) {
            if (visited >= heap->registered_thread_count) {
                return NGGC_ERR_CORRUPTION;
            }
            ++visited;
            if (atomic_load_explicit(&thread->tlab_fast_inflight,
                                     memory_order_acquire) != 0u) {
                pending = 1;
                break;
            }
        }
        if (!pending && visited != heap->registered_thread_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (pending) {
#if NGGC_HAVE_C11_THREADS
            thrd_yield();
#else
            atomic_signal_fence(memory_order_seq_cst);
#endif
            continue;
        }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        if (atomic_exchange_explicit(
                &heap->test_tlab_fast_flush_pause_armed, 0u,
                memory_order_acq_rel) != 0u) {
            atomic_store_explicit(
                &heap->test_tlab_fast_flush_pause_entered, 1u,
                memory_order_release);
            while (atomic_load_explicit(
                       &heap->test_tlab_fast_flush_pause_release,
                       memory_order_acquire) == 0u) {
#if NGGC_HAVE_C11_THREADS
                thrd_yield();
#else
                atomic_signal_fence(memory_order_seq_cst);
#endif
            }
            atomic_store_explicit(
                &heap->test_tlab_fast_flush_pause_release, 0u,
                memory_order_release);
        }
#endif
        visited = 0u;
        for (thread = heap->threads; thread; thread = thread->next) {
            nggc_result result;
            if (visited >= heap->registered_thread_count) {
                return NGGC_ERR_CORRUPTION;
            }
            ++visited;
            result = nggc_tlab_flush_thread_locked(heap, thread);
            if (result == NGGC_ERR_BUSY) {
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
                atomic_store_explicit(
                    &heap->test_tlab_fast_flush_busy_observed, 1u,
                    memory_order_release);
#endif
                pending = 1;
                break;
            }
            if (result != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
        }
        if (!pending && visited != heap->registered_thread_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (!pending) {
            return NGGC_OK;
        }
#if NGGC_HAVE_C11_THREADS
        thrd_yield();
#else
        atomic_signal_fence(memory_order_seq_cst);
#endif
    }
}

static void nggc_tlab_fast_maybe_open_locked(nggc_heap *heap) {
#if NGGC_HAVE_C11_THREADS
    if (heap && heap->magic == NGGC_HEAP_MAGIC &&
        heap->type_table_frozen != 0u && !heap->safepoint_active &&
        heap->gc_phase == NGGC_PHASE_IDLE &&
        !heap->collector_cycle_active &&
        !heap->mutator_concurrent_active &&
        !heap->from_space_quarantine_active &&
        heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_IDLE) {
        atomic_store_explicit(&heap->tlab_fast_gate,
                              NGGC_TLAB_FAST_GATE_OPEN,
                              memory_order_release);
    }
#else
    (void)heap;
#endif
}

static void nggc_tlab_sizing_observe_retire_locked(
    nggc_heap *heap, nggc_thread *thread, size_t span_size,
    size_t used_bytes, size_t unused_bytes) {
    int changed = 0;
    if (!heap || !thread ||
        heap->tlab_sizing_policy != NGGC_TLAB_SIZING_POLICY_SMART ||
        span_size != thread->tlab_target_size ||
        used_bytes > span_size || unused_bytes > span_size ||
        used_bytes != span_size - unused_bytes) {
        return;
    }
    if (heap->tlab_sizing_sample_count != UINT64_MAX) {
        ++heap->tlab_sizing_sample_count;
    }
    if (unused_bytes <= span_size / 8u) {
        thread->tlab_low_utilization_streak = 0u;
        if (thread->tlab_target_size < heap->tlab_max_size) {
            if (thread->tlab_high_utilization_streak <
                NGGC_TLAB_HIGH_UTILIZATION_STREAK) {
                ++thread->tlab_high_utilization_streak;
            }
            if (thread->tlab_high_utilization_streak >=
                NGGC_TLAB_HIGH_UTILIZATION_STREAK) {
                size_t grown =
                    thread->tlab_target_size > heap->tlab_max_size / 2u
                        ? heap->tlab_max_size
                        : thread->tlab_target_size * 2u;
                if (grown > heap->tlab_max_size) {
                    grown = heap->tlab_max_size;
                }
                if (grown != thread->tlab_target_size) {
                    thread->tlab_target_size = grown;
                    changed = 1;
                    if (heap->tlab_sizing_grow_count != UINT64_MAX) {
                        ++heap->tlab_sizing_grow_count;
                    }
                }
                thread->tlab_high_utilization_streak = 0u;
            }
        } else {
            thread->tlab_high_utilization_streak = 0u;
        }
    } else if (used_bytes <= span_size / 2u) {
        thread->tlab_high_utilization_streak = 0u;
        if (thread->tlab_target_size > heap->tlab_min_size) {
            if (thread->tlab_low_utilization_streak <
                NGGC_TLAB_LOW_UTILIZATION_STREAK) {
                ++thread->tlab_low_utilization_streak;
            }
            if (thread->tlab_low_utilization_streak >=
                NGGC_TLAB_LOW_UTILIZATION_STREAK) {
                size_t shrunk = thread->tlab_target_size / 2u;
                shrunk -= shrunk % NGGC_OBJECT_ALIGNMENT;
                if (shrunk < heap->tlab_min_size) {
                    shrunk = heap->tlab_min_size;
                }
                if (shrunk != thread->tlab_target_size) {
                    thread->tlab_target_size = shrunk;
                    changed = 1;
                    if (heap->tlab_sizing_shrink_count != UINT64_MAX) {
                        ++heap->tlab_sizing_shrink_count;
                    }
                }
                thread->tlab_low_utilization_streak = 0u;
            }
        } else {
            thread->tlab_low_utilization_streak = 0u;
        }
    } else {
        thread->tlab_high_utilization_streak = 0u;
        thread->tlab_low_utilization_streak = 0u;
    }
    if (!changed && heap->tlab_sizing_hold_count != UINT64_MAX) {
        ++heap->tlab_sizing_hold_count;
    }
}

static nggc_result nggc_tlab_retire_locked(nggc_heap *heap,
                                            nggc_thread *thread) {
    nggc_region_meta *region;
    nggc_object_header *header;
    size_t span_size;
    size_t used;
    size_t remaining;
    if (!heap || !thread || !nggc_thread_live(thread, heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
            NGGC_TLAB_FAST_GATE_CLOSED ||
        atomic_load_explicit(&thread->tlab_fast_inflight,
                             memory_order_acquire) != 0u) {
        return NGGC_ERR_BUSY;
    }
    {
        nggc_result flush_result =
            nggc_tlab_flush_thread_locked(heap, thread);
        if (flush_result != NGGC_OK) {
            return flush_result;
        }
    }
    if (thread->tlab_state == NGGC_TLAB_INACTIVE) {
        return NGGC_OK;
    }
    if (thread->tlab_region_index >= heap->region_count) {
        return NGGC_ERR_CORRUPTION;
    }
    region = &heap->regions[thread->tlab_region_index];
    if (region->tlab_owner != thread ||
        region->tlab_lease_epoch != thread->tlab_lease_epoch ||
        region->tlab_reserved_start != thread->tlab_reserved_start ||
        region->tlab_published_top != thread->tlab_published_top ||
        region->tlab_reserved_end != thread->tlab_reserved_end ||
        region->tlab_state != thread->tlab_state ||
        thread->tlab_reserved_start > thread->tlab_published_top ||
        thread->tlab_published_top > thread->tlab_reserved_end) {
        return NGGC_ERR_CORRUPTION;
    }
    region->tlab_state = NGGC_TLAB_RETIRING;
    thread->tlab_state = NGGC_TLAB_RETIRING;
    span_size = (size_t)(thread->tlab_reserved_end -
                         thread->tlab_reserved_start);
    remaining = (size_t)(thread->tlab_reserved_end -
                         thread->tlab_published_top);
    used = span_size - remaining;
    heap->tlab_retired_unused_bytes = nggc_saturating_add_u64_size(
        heap->tlab_retired_unused_bytes, remaining);
    if (remaining != 0u) {
        header = (nggc_object_header *)thread->tlab_published_top;
        if (remaining < NGGC_OBJECT_HEADER_BYTES ||
            atomic_load_explicit(&header->state, memory_order_acquire) !=
                NGGC_OBJECT_TLAB_RESERVED ||
            header->alloc_size != remaining ||
            region->reserved_bytes < remaining) {
            return NGGC_ERR_CORRUPTION;
        }
        atomic_store_explicit(&header->state, NGGC_OBJECT_FREE,
                              memory_order_release);
        header->type_id = NGGC_TYPE_NONE;
        header->size_class = 0u;
        header->flags = 0u;
        header->payload_size = 0u;
        header->object_id = 0u;
        atomic_store_explicit(&header->pin_count, 0u, memory_order_release);
        header->mark_epoch = 0u;
        header->generation = NGGC_REGION_GENERATION_NONE;
        header->young_age = 0u;
        nggc_payload_lock_init(header);
        if (nggc_free_block_link_locked(heap, header) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region->reserved_bytes -= remaining;
        region->free_bytes += remaining;
        nggc_region_refresh_flags(region);
    }
    nggc_tlab_sizing_observe_retire_locked(
        heap, thread, span_size, used, remaining);
    region->tlab_owner = 0;
    region->tlab_lease_epoch = 0u;
    region->tlab_reserved_start = 0u;
    region->tlab_published_top = 0u;
    region->tlab_reserved_end = 0u;
    region->tlab_state = NGGC_TLAB_INACTIVE;
    thread->tlab_region_index = SIZE_MAX;
    thread->tlab_lease_epoch = 0u;
    thread->tlab_reserved_start = 0u;
    thread->tlab_published_top = 0u;
    thread->tlab_reserved_end = 0u;
    thread->tlab_state = NGGC_TLAB_INACTIVE;
    atomic_store_explicit(&thread->tlab_fast_top, 0u,
                          memory_order_release);
    thread->tlab_object_id_next = 0u;
    thread->tlab_object_id_remaining = 0u;
    if (heap->tlab_retire_count != UINT64_MAX) {
        ++heap->tlab_retire_count;
    }
    return NGGC_OK;
}

static nggc_result nggc_tlab_reserve_locked(nggc_heap *heap,
                                             nggc_thread *thread,
                                             size_t requested_bytes) {
    nggc_region_meta *region;
    nggc_object_header *header;
    uintptr_t end;
    size_t span_size;
    size_t region_index;
    size_t region_remaining;
    nggc_result result;
    if (!heap || !thread || !nggc_thread_live(thread, heap) ||
        requested_bytes < NGGC_OBJECT_HEADER_BYTES ||
        !nggc_align_up_size(requested_bytes, NGGC_OBJECT_ALIGNMENT,
                            &span_size) ||
        span_size > heap->region_size ||
        thread->tlab_state != NGGC_TLAB_INACTIVE ||
        !nggc_thread_is_registered(heap, thread)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
            NGGC_TLAB_FAST_GATE_CLOSED ||
        atomic_load_explicit(&thread->tlab_fast_inflight,
                             memory_order_acquire) != 0u ||
        atomic_load_explicit(&thread->tlab_fast_top,
                             memory_order_acquire) != 0u ||
        thread->tlab_object_id_next != 0u ||
        thread->tlab_object_id_remaining != 0u) {
        return NGGC_ERR_BUSY;
    }
    if (thread->state != NGGC_THREAD_RUNNING) {
        return NGGC_ERR_BUSY;
    }
    result = nggc_allocator_lease_acquire_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    for (;;) {
        region_index = (size_t)((heap->arena_cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        region = &heap->regions[region_index];
        region_remaining = (size_t)(region->end - heap->arena_cursor);
        if ((region->generation == NGGC_REGION_GENERATION_NONE ||
             region->generation == NGGC_REGION_GENERATION_YOUNG) &&
            region->tlab_state == NGGC_TLAB_INACTIVE &&
            span_size <= region_remaining) {
            break;
        }
        if (region->generation == NGGC_REGION_GENERATION_NONE &&
            nggc_region_assign_generation_range_locked(
                heap, heap->arena_cursor, region_remaining,
                NGGC_REGION_GENERATION_YOUNG) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        if (region_remaining >= NGGC_OBJECT_HEADER_BYTES) {
            result = nggc_region_publish_padding_locked(heap,
                                                        region_remaining);
            if (result != NGGC_OK) {
                return result;
            }
        } else if (region_remaining != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (region->tlab_state != NGGC_TLAB_INACTIVE ||
        !nggc_range_end(heap->arena_cursor, span_size, &end) ||
        end > region->end || end > heap->arena_end) {
        return NGGC_ERR_BUSY;
    }
    result = nggc_region_ensure_committed_range_locked(
        heap, heap->arena_cursor, span_size);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_region_assign_generation_range_locked(
        heap, heap->arena_cursor, span_size,
        NGGC_REGION_GENERATION_YOUNG);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_tlab_object_id_lease_locked(heap, thread, span_size);
    if (result != NGGC_OK) {
        return result;
    }
    header = (nggc_object_header *)heap->arena_cursor;
    memset(header, 0, sizeof(*header));
    header->magic = NGGC_OBJECT_MAGIC;
    atomic_init(&header->state, NGGC_OBJECT_TLAB_RESERVED);
    nggc_forwarding_init(header);
    header->type_id = NGGC_TYPE_NONE;
    header->alloc_size = span_size;
    if (region->committed_bytes == 0u) {
        ++heap->active_region_count;
    }
    region->committed_bytes += span_size;
    region->reserved_bytes += span_size;
    nggc_region_refresh_flags(region);
    region->tlab_owner = thread;
    region->tlab_lease_epoch = heap->allocator_lease_epoch;
    region->tlab_reserved_start = heap->arena_cursor;
    region->tlab_published_top = heap->arena_cursor;
    region->tlab_reserved_end = end;
    region->tlab_state = NGGC_TLAB_ACTIVE;
    thread->tlab_region_index = region_index;
    thread->tlab_lease_epoch = region->tlab_lease_epoch;
    thread->tlab_reserved_start = heap->arena_cursor;
    thread->tlab_published_top = heap->arena_cursor;
    thread->tlab_reserved_end = end;
    thread->tlab_state = NGGC_TLAB_ACTIVE;
    atomic_store_explicit(&thread->tlab_fast_top, heap->arena_cursor,
                          memory_order_release);
    heap->arena_cursor = end;
    result = nggc_region_publish_cursor_locked(heap, end);
    if (result == NGGC_OK && heap->tlab_refill_count != UINT64_MAX) {
        ++heap->tlab_refill_count;
    }
    if (result == NGGC_OK) {
        heap->tlab_refill_bytes = nggc_saturating_add_u64_size(
            heap->tlab_refill_bytes, span_size);
    }
    return result;
}

static nggc_result nggc_tlab_alloc_locked(nggc_heap *heap,
                                           nggc_thread *thread,
                                           uint16_t type_id,
                                           size_t payload_size,
                                           void **out_object) {
    nggc_region_meta *region;
    nggc_type_desc *desc;
    nggc_object_header *header;
    nggc_object_header *tail_header = 0;
    uintptr_t object_end;
    uint64_t object_id;
    size_t total_size;
    size_t alloc_size;
    size_t available;
    size_t tail_size;
    uint32_t object_flags;
    if (!heap || !thread || !out_object ||
        !nggc_type_id_valid(type_id) ||
        !nggc_thread_live(thread, heap) ||
        !nggc_thread_is_registered(heap, thread)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
            NGGC_TLAB_FAST_GATE_CLOSED ||
        atomic_load_explicit(&thread->tlab_fast_inflight,
                             memory_order_acquire) != 0u) {
        return NGGC_ERR_BUSY;
    }
    if (thread->state != NGGC_THREAD_RUNNING) {
        return NGGC_ERR_BUSY;
    }
    *out_object = 0;
    if (!heap->types[type_id].registered ||
        thread->tlab_state != NGGC_TLAB_ACTIVE ||
        thread->tlab_region_index >= heap->region_count ||
        payload_size > SIZE_MAX - NGGC_OBJECT_HEADER_BYTES) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    desc = &heap->types[type_id].desc;
    if (desc->object_size != 0u && payload_size < desc->object_size) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    total_size = NGGC_OBJECT_HEADER_BYTES + payload_size;
    if (!nggc_align_up_size(total_size, NGGC_OBJECT_ALIGNMENT,
                            &alloc_size)) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    object_flags = nggc_object_flags_from_type(heap, desc, payload_size);
    if ((object_flags & (NGGC_OBJECT_FLAG_LARGE |
                         NGGC_OBJECT_FLAG_PINNABLE |
                         NGGC_OBJECT_FLAG_NON_MOVABLE)) != 0u) {
        return NGGC_ERR_UNSUPPORTED;
    }
    region = &heap->regions[thread->tlab_region_index];
    if (region->tlab_owner != thread ||
        region->tlab_state != NGGC_TLAB_ACTIVE ||
        region->tlab_lease_epoch != thread->tlab_lease_epoch ||
        region->tlab_published_top != thread->tlab_published_top ||
        region->tlab_reserved_end != thread->tlab_reserved_end ||
        thread->tlab_published_top > thread->tlab_reserved_end ||
        atomic_load_explicit(&thread->tlab_fast_top,
                             memory_order_acquire) !=
            thread->tlab_published_top) {
        return NGGC_ERR_CORRUPTION;
    }
    available = (size_t)(thread->tlab_reserved_end -
                         thread->tlab_published_top);
    if (alloc_size > available) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    tail_size = available - alloc_size;
    if (tail_size != 0u && tail_size < NGGC_OBJECT_HEADER_BYTES) {
        alloc_size += tail_size;
        tail_size = 0u;
    }
    header = (nggc_object_header *)thread->tlab_published_top;
    if (atomic_load_explicit(&header->state, memory_order_acquire) !=
            NGGC_OBJECT_TLAB_RESERVED ||
        header->alloc_size != available ||
        region->reserved_bytes < alloc_size ||
        !nggc_range_end((uintptr_t)header, alloc_size, &object_end)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->allocation_count == UINT64_MAX ||
        nggc_tlab_take_object_id(thread, &object_id) != NGGC_OK) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    if (tail_size != 0u) {
        tail_header = (nggc_object_header *)object_end;
        memset(tail_header, 0, sizeof(*tail_header));
        tail_header->magic = NGGC_OBJECT_MAGIC;
        atomic_init(&tail_header->state, NGGC_OBJECT_TLAB_RESERVED);
        nggc_forwarding_init(tail_header);
        nggc_payload_lock_init(tail_header);
        tail_header->type_id = NGGC_TYPE_NONE;
        tail_header->alloc_size = tail_size;
    }
    memset(header, 0, alloc_size);
    header->magic = NGGC_OBJECT_MAGIC;
    atomic_init(&header->state, NGGC_OBJECT_ALLOCATING);
    nggc_forwarding_init(header);
    nggc_payload_lock_init(header);
    header->type_id = type_id;
    header->flags = object_flags;
    header->payload_size = payload_size;
    header->alloc_size = alloc_size;
    header->object_id = object_id;
    header->generation = NGGC_REGION_GENERATION_YOUNG;
    header->young_age = 0u;
    if (heap->mutator_assist_state != NGGC_MUTATOR_ASSIST_STATE_IDLE) {
        header->mark_epoch = heap->mark_epoch;
    }
    atomic_store_explicit(&header->state, NGGC_OBJECT_LIVE,
                          memory_order_release);
    region->reserved_bytes -= alloc_size;
    region->live_bytes += alloc_size;
    ++region->live_object_count;
    if (nggc_region_object_attributes_adjust_locked(heap, header, 1) !=
        NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    region->tlab_published_top = object_end;
    thread->tlab_published_top = object_end;
    atomic_store_explicit(&thread->tlab_fast_top, object_end,
                          memory_order_release);
    nggc_region_refresh_flags(region);
    nggc_assist_charge_allocation_locked(heap, thread, alloc_size);
    ++heap->live_object_count;
    heap->live_payload_bytes += payload_size;
    ++heap->allocation_count;
    heap->allocated_bytes = nggc_saturating_add_u64_size(
        heap->allocated_bytes, alloc_size);
    if ((header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u) {
        ++heap->large_object_count;
    }
    if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) != 0u) {
        ++heap->pinnable_object_count;
    }
    if ((header->flags & NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u) {
        ++heap->non_movable_object_count;
    }
    if (heap->tlab_allocation_count != UINT64_MAX) {
        ++heap->tlab_allocation_count;
    }
    *out_object = nggc_object_from_header(header);
    return NGGC_OK;
}

static nggc_result nggc_tlab_alloc_fast(nggc_heap *heap,
                                         nggc_thread *thread,
                                         uint16_t type_id,
                                         size_t payload_size,
                                         void **out_object) {
#if NGGC_HAVE_C11_THREADS
    const nggc_type_desc *desc;
    nggc_object_header *header;
    nggc_object_header *tail_header = 0;
    uintptr_t top;
    uintptr_t object_end;
    size_t total_size;
    size_t alloc_size;
    size_t available;
    size_t tail_size;
    uint32_t object_flags;
    uint64_t object_id;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    int test_enrollment_pause = 0;
#endif
    if (!heap || !thread || !out_object ||
        !nggc_type_id_valid(type_id) ||
        payload_size > SIZE_MAX - NGGC_OBJECT_HEADER_BYTES) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_object = 0;
    if (!nggc_thread_owned_by_current(thread)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(&heap->type_table_published,
                             memory_order_acquire) == 0u ||
        atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
            NGGC_TLAB_FAST_GATE_OPEN) {
        return NGGC_ERR_BUSY;
    }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (atomic_exchange_explicit(
            &heap->test_tlab_fast_enrollment_pause_armed, 0u,
            memory_order_acq_rel) != 0u) {
        test_enrollment_pause = 1;
        atomic_store_explicit(
            &heap->test_tlab_fast_enrollment_pause_stage, 1u,
            memory_order_release);
        while (atomic_load_explicit(
                   &heap->test_tlab_fast_enrollment_pause_release_stage,
                   memory_order_acquire) < 1u) {
            thrd_yield();
        }
    }
#endif
    atomic_fetch_add_explicit(&thread->tlab_fast_inflight, 1u,
                              memory_order_acq_rel);
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (test_enrollment_pause) {
        atomic_store_explicit(
            &heap->test_tlab_fast_enrollment_pause_stage, 2u,
            memory_order_release);
        while (atomic_load_explicit(
                   &heap->test_tlab_fast_enrollment_pause_release_stage,
                   memory_order_acquire) < 2u) {
            thrd_yield();
        }
    }
#endif
    if (atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
            NGGC_TLAB_FAST_GATE_OPEN ||
        atomic_load_explicit(&heap->type_table_published,
                             memory_order_acquire) == 0u) {
        atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
        return NGGC_ERR_BUSY;
    }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (atomic_exchange_explicit(&heap->test_tlab_fast_pause_armed, 0u,
                                 memory_order_acq_rel) != 0u) {
        atomic_store_explicit(&heap->test_tlab_fast_pause_entered, 1u,
                              memory_order_release);
        while (atomic_load_explicit(&heap->test_tlab_fast_pause_release,
                                    memory_order_acquire) == 0u) {
            thrd_yield();
        }
        atomic_store_explicit(&heap->test_tlab_fast_pause_release, 0u,
                              memory_order_release);
    }
#endif
    if (thread->magic != NGGC_THREAD_MAGIC || thread->heap != heap ||
        thread->state != NGGC_THREAD_RUNNING ||
        thread->tlab_state != NGGC_TLAB_ACTIVE ||
        thread->tlab_region_index >= heap->region_count ||
        !heap->types[type_id].registered) {
        atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
        return NGGC_ERR_BUSY;
    }
    desc = &heap->types[type_id].desc;
    if (desc->object_size != 0u && payload_size < desc->object_size) {
        atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    total_size = NGGC_OBJECT_HEADER_BYTES + payload_size;
    if (!nggc_align_up_size(total_size, NGGC_OBJECT_ALIGNMENT,
                            &alloc_size)) {
        atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    object_flags = nggc_object_flags_from_type(heap, desc, payload_size);
    if (alloc_size > heap->region_size ||
        (object_flags & (NGGC_OBJECT_FLAG_LARGE |
                         NGGC_OBJECT_FLAG_PINNABLE |
                         NGGC_OBJECT_FLAG_NON_MOVABLE)) != 0u) {
        atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
        return NGGC_ERR_UNSUPPORTED;
    }
    top = atomic_load_explicit(&thread->tlab_fast_top,
                               memory_order_relaxed);
    if (top < thread->tlab_reserved_start ||
        top > thread->tlab_reserved_end) {
        atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
        return NGGC_ERR_CORRUPTION;
    }
    available = (size_t)(thread->tlab_reserved_end - top);
    if (alloc_size > available) {
        atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    tail_size = available - alloc_size;
    if (tail_size != 0u && tail_size < NGGC_OBJECT_HEADER_BYTES) {
        alloc_size += tail_size;
        tail_size = 0u;
    }
    header = (nggc_object_header *)top;
    if (atomic_load_explicit(&header->state, memory_order_acquire) !=
            NGGC_OBJECT_TLAB_RESERVED ||
        header->magic != NGGC_OBJECT_MAGIC ||
        header->type_id != NGGC_TYPE_NONE ||
        header->alloc_size != available ||
        !nggc_range_end(top, alloc_size, &object_end) ||
        object_end > thread->tlab_reserved_end ||
        nggc_tlab_take_object_id(thread, &object_id) != NGGC_OK) {
        atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
        return NGGC_ERR_CORRUPTION;
    }
    if (tail_size != 0u) {
        tail_header = (nggc_object_header *)object_end;
        memset(tail_header, 0, sizeof(*tail_header));
        tail_header->magic = NGGC_OBJECT_MAGIC;
        atomic_init(&tail_header->state, NGGC_OBJECT_TLAB_RESERVED);
        nggc_forwarding_init(tail_header);
        nggc_payload_lock_init(tail_header);
        tail_header->type_id = NGGC_TYPE_NONE;
        tail_header->alloc_size = tail_size;
    }
    memset(header, 0, alloc_size);
    header->magic = NGGC_OBJECT_MAGIC;
    atomic_init(&header->state, NGGC_OBJECT_ALLOCATING);
    nggc_forwarding_init(header);
    nggc_payload_lock_init(header);
    header->type_id = type_id;
    header->flags = object_flags;
    header->payload_size = payload_size;
    header->alloc_size = alloc_size;
    header->object_id = object_id;
    header->generation = NGGC_REGION_GENERATION_YOUNG;
    header->young_age = 0u;
    atomic_store_explicit(&header->state, NGGC_OBJECT_LIVE,
                          memory_order_release);
    atomic_store_explicit(&thread->tlab_fast_top, object_end,
                          memory_order_release);
    *out_object = nggc_object_from_header(header);
    atomic_fetch_sub_explicit(&thread->tlab_fast_inflight, 1u,
                              memory_order_release);
    return NGGC_OK;
#else
    (void)heap;
    (void)thread;
    (void)type_id;
    (void)payload_size;
    (void)out_object;
    return NGGC_ERR_UNSUPPORTED;
#endif
}

static nggc_result nggc_assist_reclaim_for_allocation_locked(
    nggc_heap *heap, nggc_thread *assist_thread, size_t alloc_size);
static nggc_result nggc_collect_prepare_safepoint_locked(nggc_heap *heap);
static nggc_result nggc_rebuild_free_list_locked(nggc_heap *heap);
static nggc_result nggc_verify_regions_locked(nggc_heap *heap);
static uintptr_t nggc_region_boundary_ceil_locked(
    const nggc_heap *heap, uintptr_t addr);
static nggc_result nggc_alloc_from_decommitted_large_bins_locked(
    nggc_heap *heap, size_t requested_bin, size_t requested_size,
    uint32_t target_generation, uint32_t object_flags,
    nggc_object_header **out_header, size_t *out_alloc_size,
    int *out_found);

static nggc_result nggc_alloc_size_with_desc_locked(nggc_heap *heap,
                                                    uint16_t type_id,
                                                    const nggc_type_desc *desc,
                                                    uint16_t size_class,
                                                    size_t payload_size,
                                                    void **out_object,
                                                    nggc_thread *assist_thread) {
    nggc_object_header *header;
    nggc_object_header *free_header;
    nggc_object_header *prev_free;
    nggc_object_header **selected_free_list;
    uintptr_t header_addr;
    uintptr_t next_cursor;
    uintptr_t proposed_start;
    size_t total_size;
    size_t object_size;
    size_t alloc_size;
    size_t requested_large_bin = 0u;
    size_t current_large_bin = 0u;
    uint64_t object_id;
    uint32_t object_flags;
    uint32_t target_generation;
    int is_large_allocation;
    int assisted = 0;
    int reused_free_block = 0;

    if (!heap || !desc || !out_object) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_object = 0;
    {
        nggc_result flush_result =
            nggc_tlab_fast_close_and_flush_locked(heap);
        if (flush_result != NGGC_OK) {
            return flush_result;
        }
    }
    if (heap->safepoint_active) {
        return NGGC_ERR_BUSY;
    }
    if (payload_size > SIZE_MAX - NGGC_OBJECT_HEADER_BYTES) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    total_size = NGGC_OBJECT_HEADER_BYTES + payload_size;
    if (!nggc_align_up_size(total_size,
                            NGGC_OBJECT_ALIGNMENT, &object_size)) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    alloc_size = object_size;
    object_flags = nggc_object_flags_from_type(heap, desc, payload_size);
    if (alloc_size > heap->region_size) {
        object_flags |= NGGC_OBJECT_FLAG_LARGE;
    }
    is_large_allocation =
        (object_flags & NGGC_OBJECT_FLAG_LARGE) != 0u;
    if (is_large_allocation &&
        (!nggc_round_up_multiple_size(
             object_size, heap->region_size, &alloc_size) ||
         alloc_size / heap->region_size < 2u ||
         !nggc_large_free_bin_index_locked(
             heap, alloc_size, &requested_large_bin))) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    target_generation = nggc_allocation_region_generation(object_flags);

try_free_list:
    if (is_large_allocation) {
        if (heap->large_free_search_count != UINT64_MAX) {
            ++heap->large_free_search_count;
        }
        current_large_bin = requested_large_bin;
        while (current_large_bin < NGGC_LARGE_FREE_BIN_COUNT &&
               (heap->large_free_bin_mask &
                ((uint32_t)1u << current_large_bin)) == 0u) {
            ++current_large_bin;
        }
        if (current_large_bin == NGGC_LARGE_FREE_BIN_COUNT) {
            goto free_list_search_complete;
        }
        selected_free_list =
            &heap->large_free_bins[current_large_bin];
        if (!*selected_free_list) {
            return NGGC_ERR_CORRUPTION;
        }
    } else {
        selected_free_list = &heap->free_list;
    }

scan_selected_free_list:
    prev_free = 0;
    free_header = *selected_free_list;
    while (free_header) {
        nggc_object_header *next_free =
            (nggc_object_header *)nggc_forwarding_load_relaxed(
                free_header);
        size_t actual_large_bin = 0u;
        int size_fits = free_header->alloc_size >= alloc_size;
        int placement_compatible = 0;
        if (is_large_allocation &&
            heap->large_free_probe_count != UINT64_MAX) {
            ++heap->large_free_probe_count;
        }
        if (is_large_allocation &&
            heap->large_free_committed_probe_count != UINT64_MAX) {
            ++heap->large_free_committed_probe_count;
        }
        if ((free_header->flags & ~NGGC_OBJECT_FLAG_LARGE) != 0u ||
            nggc_free_block_is_large(free_header) !=
            is_large_allocation) {
            return NGGC_ERR_CORRUPTION;
        }
        if (is_large_allocation &&
            (!nggc_region_humongous_run_matches_locked(
                 heap, (uintptr_t)free_header,
                 free_header->alloc_size) ||
             !nggc_large_free_bin_index_locked(
                 heap, free_header->alloc_size,
                 &actual_large_bin) ||
             actual_large_bin != current_large_bin)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (size_fits) {
            placement_compatible =
                nggc_region_range_allocation_compatible_locked(
                heap, (uintptr_t)free_header, alloc_size,
                target_generation, object_flags);
        }
        if (is_large_allocation && !size_fits &&
            heap->large_free_size_reject_count != UINT64_MAX) {
            ++heap->large_free_size_reject_count;
        } else if (is_large_allocation && size_fits &&
                   !placement_compatible &&
                   heap->large_free_placement_reject_count !=
                       UINT64_MAX) {
            ++heap->large_free_placement_reject_count;
        }
        if (size_fits && placement_compatible) {
            size_t free_alloc_size = free_header->alloc_size;
            size_t remaining_size = free_alloc_size - alloc_size;
            int split_block =
                is_large_allocation
                    ? (heap->region_size <= SIZE_MAX / 2u &&
                       remaining_size >= heap->region_size * 2u)
                    : remaining_size >= NGGC_MIN_FREE_BLOCK_SIZE;
            size_t reused_size = split_block ? alloc_size
                                             : free_alloc_size;
            nggc_result generation_result =
                nggc_region_assign_allocation_range_locked(
                    heap, (uintptr_t)free_header, reused_size,
                    target_generation, object_flags);
            if (generation_result != NGGC_OK) {
                prev_free = free_header;
                free_header = next_free;
                continue;
            }
            if (split_block) {
                nggc_object_header *split_header =
                    (nggc_object_header *)((uintptr_t)free_header +
                                           alloc_size);
                if (heap->free_bytes < alloc_size ||
                    (is_large_allocation &&
                     heap->large_free_bytes < alloc_size)) {
                    return NGGC_ERR_CORRUPTION;
                }
                if (is_large_allocation &&
                    (nggc_region_assign_humongous_run_locked(
                         heap, (uintptr_t)free_header,
                         alloc_size) != NGGC_OK ||
                     nggc_region_assign_humongous_run_locked(
                         heap, (uintptr_t)split_header,
                         remaining_size) != NGGC_OK)) {
                    return NGGC_ERR_CORRUPTION;
                }
                memset(split_header, 0, sizeof(*split_header));
                split_header->magic = NGGC_OBJECT_MAGIC;
                atomic_init(&split_header->state, NGGC_OBJECT_FREE);
                nggc_forwarding_init(split_header);
                nggc_payload_lock_init(split_header);
                split_header->type_id = NGGC_TYPE_NONE;
                split_header->flags = is_large_allocation
                                          ? NGGC_OBJECT_FLAG_LARGE
                                          : 0u;
                split_header->alloc_size = remaining_size;
                if (is_large_allocation) {
                    if (prev_free) {
                        nggc_forwarding_store_free_next(prev_free,
                                                        next_free);
                    } else {
                        *selected_free_list = next_free;
                    }
                    if (!*selected_free_list) {
                        heap->large_free_bin_mask &=
                            ~((uint32_t)1u << current_large_bin);
                    }
                    if (nggc_large_free_bin_link_existing_locked(
                            heap, split_header) != NGGC_OK) {
                        return NGGC_ERR_CORRUPTION;
                    }
                } else {
                    nggc_forwarding_store_free_next(split_header,
                                                    next_free);
                    if (prev_free) {
                        nggc_forwarding_store_free_next(prev_free,
                                                        split_header);
                    } else {
                        *selected_free_list = split_header;
                    }
                }
                heap->free_bytes -= alloc_size;
                if (is_large_allocation) {
                    heap->large_free_bytes -= alloc_size;
                }
            } else {
                if (heap->free_object_count == 0u ||
                    heap->free_bytes < free_alloc_size ||
                    (is_large_allocation &&
                     (heap->large_free_object_count == 0u ||
                      heap->large_free_bytes < free_alloc_size))) {
                    return NGGC_ERR_CORRUPTION;
                }
                if (prev_free) {
                    nggc_forwarding_store_free_next(prev_free, next_free);
                } else {
                    *selected_free_list = next_free;
                }
                if (is_large_allocation && !*selected_free_list) {
                    heap->large_free_bin_mask &=
                        ~((uint32_t)1u << current_large_bin);
                }
                heap->free_bytes -= free_alloc_size;
                --heap->free_object_count;
                if (is_large_allocation) {
                    --heap->large_free_object_count;
                    heap->large_free_bytes -= free_alloc_size;
                }
                alloc_size = free_alloc_size;
            }
            header = free_header;
            reused_free_block = 1;
            if (is_large_allocation &&
                heap->large_reuse_count != UINT64_MAX) {
                ++heap->large_reuse_count;
            }
            goto initialize_object;
        }
        prev_free = free_header;
        free_header = next_free;
    }

    if (is_large_allocation) {
        ++current_large_bin;
        while (current_large_bin < NGGC_LARGE_FREE_BIN_COUNT &&
               (heap->large_free_bin_mask &
                ((uint32_t)1u << current_large_bin)) == 0u) {
            ++current_large_bin;
        }
        if (current_large_bin < NGGC_LARGE_FREE_BIN_COUNT) {
            selected_free_list =
                &heap->large_free_bins[current_large_bin];
            if (!*selected_free_list) {
                return NGGC_ERR_CORRUPTION;
            }
            goto scan_selected_free_list;
        }
    }

free_list_search_complete:
    if (is_large_allocation) {
        int found_decommitted = 0;
        nggc_result decommitted_result =
            nggc_alloc_from_decommitted_large_bins_locked(
                heap, requested_large_bin, alloc_size,
                target_generation, object_flags, &header,
                &alloc_size, &found_decommitted);
        if (decommitted_result != NGGC_OK) {
            return decommitted_result;
        }
        if (found_decommitted) {
            reused_free_block = 0;
            if (heap->large_reuse_count != UINT64_MAX) {
                ++heap->large_reuse_count;
            }
            goto initialize_object;
        }
    }
    proposed_start = is_large_allocation
                         ? nggc_region_boundary_ceil_locked(
                               heap, heap->arena_cursor)
                         : heap->arena_cursor;
    if (!nggc_range_end(proposed_start, alloc_size, &next_cursor) ||
        next_cursor > heap->arena_end) {
        if (!assisted &&
            (heap->flags & NGGC_CONFIG_MUTATOR_ASSIST_RECLAIM) != 0u &&
            nggc_assist_reclaim_for_allocation_locked(
                heap, assist_thread, alloc_size) ==
                NGGC_OK) {
            assisted = 1;
            goto try_free_list;
        }
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    {
        nggc_result lease_result =
            nggc_allocator_lease_acquire_locked(heap);
        if (lease_result != NGGC_OK &&
            !(lease_result == NGGC_ERR_BUSY &&
              heap->safepoint_pending_threads != 0u &&
              heap->gc_phase == NGGC_PHASE_IDLE &&
              !heap->collector_cycle_active)) {
            return lease_result;
        }
    }
    while (heap->arena_cursor < heap->arena_end) {
        size_t region_index =
            (size_t)((heap->arena_cursor -
                      (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
        nggc_region_meta *region;
        size_t region_remaining;
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[region_index];
        region_remaining = (size_t)(region->end - heap->arena_cursor);
        if ((region->generation == NGGC_REGION_GENERATION_NONE ||
              region->generation == target_generation) &&
            nggc_region_placement_compatible_locked(
                region, target_generation, object_flags) &&
            region->tlab_state == NGGC_TLAB_INACTIVE &&
            ((is_large_allocation &&
              heap->arena_cursor == region->start &&
              nggc_region_humongous_geometry(
                  heap, heap->arena_cursor, alloc_size, 0, 0)) ||
             (!is_large_allocation && alloc_size <= region_remaining))) {
            break;
        }
        if (region->generation == NGGC_REGION_GENERATION_NONE &&
            nggc_region_assign_allocation_range_locked(
                heap, heap->arena_cursor, region_remaining,
                target_generation, object_flags) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        if (region_remaining >= NGGC_OBJECT_HEADER_BYTES) {
            if (nggc_region_publish_padding_locked(heap,
                                                   region_remaining) !=
                NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (region_remaining != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (!nggc_range_end(heap->arena_cursor, alloc_size, &next_cursor) ||
        next_cursor > heap->arena_end) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    {
        nggc_result commit_result =
            nggc_region_ensure_committed_range_locked(
                heap, heap->arena_cursor, alloc_size);
        if (commit_result != NGGC_OK) {
            return commit_result;
        }
    }
    {
        nggc_result generation_result =
            nggc_region_assign_allocation_range_locked(
                heap, heap->arena_cursor, alloc_size,
                target_generation, object_flags);
        if (generation_result != NGGC_OK) {
            return generation_result;
        }
    }
    if (is_large_allocation &&
        nggc_region_assign_humongous_run_locked(
            heap, heap->arena_cursor, alloc_size) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    header_addr = heap->arena_cursor;
    heap->arena_cursor = next_cursor;
    if (nggc_region_publish_cursor_locked(heap, heap->arena_cursor) !=
        NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    header = (nggc_object_header *)header_addr;

initialize_object:
    if (is_large_allocation &&
        !nggc_region_humongous_run_matches_locked(
            heap, (uintptr_t)header, alloc_size)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->allocation_count == UINT64_MAX ||
        nggc_next_object_id_locked(heap, &object_id) != NGGC_OK) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    memset(header, 0, alloc_size);
    header->magic = NGGC_OBJECT_MAGIC;
    atomic_init(&header->state, NGGC_OBJECT_ALLOCATING);
    nggc_forwarding_init(header);
    nggc_payload_lock_init(header);
    header->type_id = type_id;
    header->size_class = size_class;
    header->flags = object_flags;
    header->payload_size = payload_size;
    header->alloc_size = alloc_size;
    header->object_id = object_id;
    header->generation = NGGC_REGION_GENERATION_YOUNG;
    header->young_age = 0u;
    if (heap->mutator_assist_state != NGGC_MUTATOR_ASSIST_STATE_IDLE) {
        header->mark_epoch = heap->mark_epoch;
    }
    atomic_store_explicit(&header->state, NGGC_OBJECT_LIVE,
                          memory_order_release);
    if (nggc_region_publish_object_locked(heap, header,
                                          reused_free_block) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    nggc_assist_charge_allocation_locked(heap, assist_thread, alloc_size);
    ++heap->allocation_count;
    ++heap->live_object_count;
    heap->live_payload_bytes += payload_size;
    heap->allocated_bytes = nggc_saturating_add_u64_size(
        heap->allocated_bytes, alloc_size);
    if ((header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u) {
        ++heap->large_object_count;
        if (heap->large_allocation_count != UINT64_MAX) {
            ++heap->large_allocation_count;
        }
    }
    if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) != 0u) {
        ++heap->pinnable_object_count;
    }
    if ((header->flags & NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u) {
        ++heap->non_movable_object_count;
    }
    *out_object = nggc_object_from_header(header);
    return NGGC_OK;
}

static nggc_object_header *nggc_live_header_from_object(nggc_heap *heap,
                                                        const void *object) {
    nggc_object_header *header;
    uintptr_t object_addr = (uintptr_t)object;
    uintptr_t header_addr;
    uintptr_t header_struct_end;

    if (!heap || !object ||
        object_addr < (uintptr_t)heap->gc_arena_base +
                          NGGC_OBJECT_HEADER_BYTES ||
        object_addr >= heap->arena_cursor) {
        return 0;
    }
    header_addr = object_addr - NGGC_OBJECT_HEADER_BYTES;
    if (header_addr < (uintptr_t)heap->gc_arena_base ||
        !nggc_range_end(header_addr, sizeof(*header), &header_struct_end) ||
        header_struct_end > heap->arena_cursor) {
        return 0;
    }
    {
        size_t region_index =
            (size_t)((header_addr - (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
        if (region_index >= heap->region_count ||
            heap->regions[region_index].decommitted ||
            header_struct_end > heap->regions[region_index].end) {
            return 0;
        }
    }
    if (((header_addr - (uintptr_t)heap->gc_arena_base) %
         NGGC_OBJECT_ALIGNMENT) != 0u) {
        return 0;
    }
    header = (nggc_object_header *)header_addr;
    if (atomic_load_explicit(&header->state, memory_order_acquire) !=
            NGGC_OBJECT_LIVE ||
        header->magic != NGGC_OBJECT_MAGIC ||
        nggc_object_from_header(header) != object) {
        return 0;
    }
    return header;
}

static int nggc_pin_owner_is_registered_locked(
    const nggc_heap *heap, const nggc_thread *owner) {
    const nggc_thread *thread;
    size_t seen = 0u;

    if (!heap || !owner) {
        return 0;
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        if (seen == heap->registered_thread_count) {
            return 0;
        }
        ++seen;
        if (thread == owner && thread->magic == NGGC_THREAD_MAGIC &&
            thread->heap == heap) {
            return 1;
        }
    }
    return 0;
}

static nggc_result nggc_pin_lease_verify_locked(
    const nggc_heap *heap, size_t *out_record_count,
    uint64_t *out_lease_count) {
    const nggc_pin_lease_record *record;
    const nggc_pin_lease_record *other;
    size_t record_count = 0u;
    uint64_t lease_count = 0u;

    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    for (record = heap->pin_leases; record; record = record->next) {
        uint32_t pin_count;
        size_t other_count = 0u;
        uint64_t header_lease_count = 0u;
        if (record_count == heap->pin_lease_record_count ||
            !record->header || record->count == 0u ||
            record->header->magic != NGGC_OBJECT_MAGIC ||
            record->object_id == 0u ||
            record->object_id != record->header->object_id ||
            atomic_load_explicit(&record->header->state,
                                 memory_order_acquire) != NGGC_OBJECT_LIVE) {
            return NGGC_ERR_CORRUPTION;
        }
        if (record->owner &&
            !nggc_pin_owner_is_registered_locked(heap, record->owner)) {
            return NGGC_ERR_CORRUPTION;
        }
        pin_count = nggc_pin_count_load(record->header);
        if ((record->header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u ||
            pin_count == 0u || record->count > pin_count ||
            lease_count > UINT64_MAX - (uint64_t)record->count) {
            return NGGC_ERR_CORRUPTION;
        }
        ++record_count;
        lease_count += (uint64_t)record->count;
        for (other = heap->pin_leases; other; other = other->next) {
            if (other_count == heap->pin_lease_record_count) {
                return NGGC_ERR_CORRUPTION;
            }
            ++other_count;
            if (other->header == record->header) {
                if (header_lease_count >
                    UINT64_MAX - (uint64_t)other->count) {
                    return NGGC_ERR_CORRUPTION;
                }
                header_lease_count += (uint64_t)other->count;
            }
            if (other != record && other->header == record->header &&
                other->owner == record->owner) {
                return NGGC_ERR_CORRUPTION;
            }
        }
        if (header_lease_count != (uint64_t)pin_count) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (record_count != heap->pin_lease_record_count ||
        lease_count != heap->pin_lease_count) {
        return NGGC_ERR_CORRUPTION;
    }
    if (out_record_count) {
        *out_record_count = record_count;
    }
    if (out_lease_count) {
        *out_lease_count = lease_count;
    }
    return NGGC_OK;
}

static nggc_result nggc_pin_lease_find_locked(
    nggc_heap *heap, nggc_object_header *header, nggc_thread *owner,
    nggc_pin_lease_record **out_record) {
    nggc_pin_lease_record *record;
    size_t record_count = 0u;

    if (!heap || !header || !out_record) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_record = 0;
    for (record = heap->pin_leases; record; record = record->next) {
        if (record_count == heap->pin_lease_record_count ||
            !record->header || record->count == 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        if (record->header->object_id != record->object_id) {
            return NGGC_ERR_CORRUPTION;
        }
        ++record_count;
        if (record->header == header && record->owner == owner) {
            *out_record = record;
        }
    }
    if (record_count != heap->pin_lease_record_count) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_pin_current_owner_locked(
    nggc_heap *heap, nggc_thread **out_owner) {
    int ambiguous = 0;

    if (!heap || !out_owner) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_owner = 0;
    *out_owner = nggc_barrier_registry_scan_current_locked(heap, &ambiguous);
    if (ambiguous) {
        return NGGC_ERR_BUSY;
    }
    return NGGC_OK;
}

static nggc_result nggc_pin_lease_add_locked(
    nggc_heap *heap, nggc_object_header *header, nggc_thread *owner) {
    nggc_pin_lease_record *record = 0;
    nggc_result result;

    result = nggc_pin_lease_find_locked(heap, header, owner, &record);
    if (result != NGGC_OK) {
        return result;
    }
    if (record) {
        if (record->count == UINT32_MAX) {
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        ++record->count;
        return NGGC_OK;
    }
    if (heap->pin_lease_record_count == SIZE_MAX) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    record = (nggc_pin_lease_record *)calloc(1u, sizeof(*record));
    if (!record) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    record->header = header;
    record->owner = owner;
    record->object_id = header->object_id;
    record->count = 1u;
    record->next = heap->pin_leases;
    heap->pin_leases = record;
    ++heap->pin_lease_record_count;
    return NGGC_OK;
}

static nggc_result nggc_pin_lease_remove_locked(
    nggc_heap *heap, nggc_object_header *header, nggc_thread *owner) {
    nggc_pin_lease_record **cursor;
    nggc_pin_lease_record *record = 0;
    nggc_result result;

    if (!heap || !header) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_pin_lease_find_locked(heap, header, owner, &record);
    if (result != NGGC_OK) {
        return result;
    }
    if (!record || record->count == 0u || heap->pin_lease_count == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (record->count > 1u) {
        --record->count;
        return NGGC_OK;
    }
    cursor = &heap->pin_leases;
    while (*cursor && *cursor != record) {
        cursor = &(*cursor)->next;
    }
    if (*cursor != record || heap->pin_lease_record_count == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    *cursor = record->next;
    --heap->pin_lease_record_count;
    free(record);
    return NGGC_OK;
}

static nggc_result nggc_pin_lease_remove_object_locked(
    nggc_heap *heap, nggc_object_header *header) {
    nggc_pin_lease_record **cursor;
    nggc_pin_lease_record *record;
    uint64_t removed_leases = 0u;
    size_t removed_records = 0u;
    size_t record_count = 0u;
    uint32_t pin_count;
    nggc_result result;

    if (!heap || !header) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_pin_lease_verify_locked(heap, &record_count, 0);
    if (result != NGGC_OK) {
        return result;
    }
    pin_count = nggc_pin_count_load(header);
    for (record = heap->pin_leases; record; record = record->next) {
        if (record->header == header &&
            record->object_id == header->object_id) {
            if (removed_leases > UINT64_MAX - (uint64_t)record->count) {
                return NGGC_ERR_CORRUPTION;
            }
            removed_leases += (uint64_t)record->count;
            if (removed_records == SIZE_MAX) {
                return NGGC_ERR_CORRUPTION;
            }
            ++removed_records;
        }
    }
    if (removed_leases != (uint64_t)pin_count ||
        removed_records > record_count ||
        removed_records > heap->pin_lease_record_count) {
        return NGGC_ERR_CORRUPTION;
    }
    cursor = &heap->pin_leases;
    while (*cursor) {
        record = *cursor;
        if (record->header == header &&
            record->object_id == header->object_id) {
            *cursor = record->next;
            free(record);
            --heap->pin_lease_record_count;
        } else {
            cursor = &record->next;
        }
    }
    return NGGC_OK;
}

static int nggc_pin_lease_has_other_owner_locked(
    const nggc_heap *heap, const nggc_object_header *header,
    const nggc_thread *owner) {
    const nggc_pin_lease_record *record;
    if (!heap || !header) {
        return 0;
    }
    for (record = heap->pin_leases; record; record = record->next) {
        if (record->header == header && record->owner != owner &&
            record->object_id == header->object_id) {
            return 1;
        }
    }
    return 0;
}

static nggc_result nggc_pin_lease_owner_count_locked(
    nggc_heap *heap, nggc_thread *owner, uint64_t *out_count) {
    nggc_pin_lease_record *record;
    uint64_t count = 0u;
    nggc_result result;

    if (!heap || !owner || !out_count) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_pin_lease_verify_locked(heap, 0, 0);
    if (result != NGGC_OK) {
        return result;
    }
    for (record = heap->pin_leases; record; record = record->next) {
        if (record->owner == owner) {
            if (count > UINT64_MAX - (uint64_t)record->count) {
                return NGGC_ERR_CORRUPTION;
            }
            count += (uint64_t)record->count;
        }
    }
    *out_count = count;
    return NGGC_OK;
}

static void nggc_pin_lease_dispose_all(nggc_heap *heap) {
    nggc_pin_lease_record *record;
    if (!heap) {
        return;
    }
    record = heap->pin_leases;
    while (record) {
        nggc_pin_lease_record *next = record->next;
        free(record);
        record = next;
    }
    heap->pin_leases = 0;
    heap->pin_lease_record_count = 0u;
}

static int nggc_pin_operation_busy_locked(const nggc_heap *heap) {
    if (!heap) {
        return 1;
    }
    return heap->safepoint_active != 0u ||
           heap->safepoint_pending_threads != 0u ||
           heap->gc_phase != NGGC_PHASE_IDLE ||
           heap->collector_cycle_active != 0u ||
           heap->mutator_concurrent_active != 0u ||
           heap->mutator_assist_state != NGGC_MUTATOR_ASSIST_STATE_IDLE ||
           heap->from_space_quarantine_active != 0u ||
           heap->from_space_reclaim_pending != 0u ||
           heap->evacuation_copy_started != 0u ||
           heap->read_barrier_active != 0u;
}

static void nggc_pin_busy_count_locked(nggc_heap *heap) {
    if (heap && heap->pin_busy_count != UINT64_MAX) {
        ++heap->pin_busy_count;
    }
}

static void nggc_pin_unpin_error_count_locked(nggc_heap *heap) {
    if (heap && heap->pin_unpin_error_count != UINT64_MAX) {
        ++heap->pin_unpin_error_count;
    }
}

static int nggc_pointer_is_object_start(nggc_heap *heap, const void *object) {
    return nggc_live_header_from_object(heap, object) != 0;
}

static nggc_result nggc_forwarding_target_locked(
    nggc_heap *heap, nggc_object_header *source,
    nggc_object_header **out_target) {
    void *forwarding;
    uint32_t epoch;
    uint32_t source_state;
    nggc_object_header *target;
    int publish_in_progress;
    if (!heap || !source || !out_target) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_target = 0;
    source_state = atomic_load_explicit(&source->state,
                                        memory_order_acquire);
    if (source_state != NGGC_OBJECT_LIVE &&
        source_state != NGGC_OBJECT_ALLOCATING &&
        source_state != NGGC_OBJECT_FORWARDED) {
        return NGGC_ERR_CORRUPTION;
    }
    forwarding = nggc_forwarding_load_acquire(source);
    epoch = nggc_forwarding_epoch_load_acquire(source);
    publish_in_progress =
        heap->collector_cycle_active &&
        heap->collector_cycle_kind ==
            NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION &&
        heap->gc_phase == NGGC_PHASE_EVACUATE &&
        epoch == heap->evacuation_epoch;
    if (source_state == NGGC_OBJECT_FORWARDED &&
        (!heap->from_space_quarantine_active ||
         epoch != heap->from_space_quarantine_epoch || !forwarding)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (!forwarding) {
        return epoch == 0u || publish_in_progress
                   ? NGGC_OK
                   : NGGC_ERR_CORRUPTION;
    }
    if (nggc_pin_count_load(source) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (epoch == 0u ||
        (heap->collector_cycle_active &&
         (heap->collector_cycle_kind !=
              NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION ||
          epoch != heap->evacuation_epoch))) {
        return NGGC_ERR_CORRUPTION;
    }
    target = nggc_live_header_from_object(heap, forwarding);
    if (!target || target == source || nggc_pin_count_load(target) != 0u ||
        nggc_forwarding_load_acquire(target) != 0 ||
        nggc_forwarding_epoch_load_acquire(target) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_target = target;
    return NGGC_OK;
}

static nggc_object_header *nggc_barrier_header_from_object_locked(
    nggc_heap *heap, const void *object) {
    nggc_object_header *header;
    uintptr_t object_addr = (uintptr_t)object;
    uintptr_t header_addr;
    uintptr_t header_struct_end;
    uint32_t state;
    if (!heap || !object ||
        object_addr < (uintptr_t)heap->gc_arena_base +
                          NGGC_OBJECT_HEADER_BYTES ||
        object_addr >= heap->arena_cursor) {
        return 0;
    }
    header_addr = object_addr - NGGC_OBJECT_HEADER_BYTES;
    if (header_addr < (uintptr_t)heap->gc_arena_base ||
        !nggc_range_end(header_addr, sizeof(*header),
                        &header_struct_end) ||
        header_struct_end > heap->arena_cursor) {
        return 0;
    }
    {
        size_t region_index =
            (size_t)((header_addr - (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
        if (region_index >= heap->region_count ||
            heap->regions[region_index].decommitted ||
            header_struct_end > heap->regions[region_index].end) {
            return 0;
        }
    }
    if (((header_addr - (uintptr_t)heap->gc_arena_base) %
         NGGC_OBJECT_ALIGNMENT) != 0u) {
        return 0;
    }
    header = (nggc_object_header *)header_addr;
    state = atomic_load_explicit(&header->state, memory_order_acquire);
    if (header->magic != NGGC_OBJECT_MAGIC ||
        (state != NGGC_OBJECT_LIVE && state != NGGC_OBJECT_FORWARDED) ||
        nggc_object_from_header(header) != object) {
        return 0;
    }
    return header;
}

static nggc_result nggc_barrier_resolve_object_locked(
    nggc_heap *heap, const void *object, nggc_object_header **out_source,
    nggc_object_header **out_canonical) {
    nggc_object_header *source;
    nggc_object_header *target = 0;
    uint32_t state;
    if (!heap || !object || !out_canonical) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (out_source) {
        *out_source = 0;
    }
    *out_canonical = 0;
    source = nggc_barrier_header_from_object_locked(heap, object);
    if (!source) {
        return NGGC_ERR_CORRUPTION;
    }
    state = atomic_load_explicit(&source->state, memory_order_acquire);
    if (state == NGGC_OBJECT_FORWARDED &&
        !heap->read_barrier_active) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_forwarding_target_locked(heap, source, &target) != NGGC_OK ||
        (state == NGGC_OBJECT_FORWARDED && !target)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (target && !heap->read_barrier_active) {
        return NGGC_ERR_CORRUPTION;
    }
    if (out_source) {
        *out_source = source;
    }
    *out_canonical = target ? target : source;
    return NGGC_OK;
}

static nggc_result nggc_verify_forwarding_quiescent_locked(
    nggc_heap *heap) {
    uintptr_t cursor;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_LIVE &&
            (nggc_forwarding_load_acquire(block.header) != 0 ||
             nggc_forwarding_epoch_load_acquire(block.header) != 0u)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_FORWARDED) {
            nggc_object_header *target;
            if (!heap->from_space_quarantine_active ||
                nggc_forwarding_target_locked(
                    heap, block.header, &target) != NGGC_OK ||
                !target) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (block.state != NGGC_OBJECT_LIVE &&
            block.state != NGGC_OBJECT_DECOMMITTED_HUMONGOUS &&
            nggc_forwarding_epoch_load_acquire(block.header) != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static nggc_result nggc_evacuation_epoch_begin_locked(nggc_heap *heap) {
    if (!heap || heap->from_space_quarantine_active) {
        return heap ? NGGC_ERR_BUSY : NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_result result = nggc_verify_forwarding_quiescent_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    ++heap->evacuation_epoch;
    if (heap->evacuation_epoch == 0u) {
        heap->evacuation_epoch = 1u;
    }
    if (atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u ||
        atomic_load_explicit(&heap->remap_workers_active,
                             memory_order_acquire) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->evacuation_worker_epoch = heap->evacuation_epoch;
    heap->evacuation_workers_quiescent = 0u;
    heap->remap_worker_epoch = 0u;
    heap->remap_workers_quiescent = 0u;
    heap->evacuation_copy_started = 0u;
    heap->from_space_reclaim_pending = 0u;
    return NGGC_OK;
}

static nggc_result nggc_from_space_quarantine_reclaim_locked(
    nggc_heap *heap) {
    uintptr_t cursor;
    size_t seen_objects = 0u;
    size_t seen_bytes = 0u;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (!heap->from_space_quarantine_active) {
        return NGGC_OK;
    }
    if (heap->collector_cycle_active || heap->gc_phase != NGGC_PHASE_IDLE) {
        return NGGC_ERR_BUSY;
    }
    if (heap->registered_thread_count != 0u &&
        (!heap->safepoint_active ||
         heap->safepoint_pending_threads != 0u ||
         heap->safepoint_parked_threads !=
             heap->registered_thread_count ||
         heap->safepoint_epoch ==
             heap->from_space_quarantine_release_epoch)) {
        return NGGC_ERR_BUSY;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_FORWARDED) {
            nggc_object_header *target;
            if (block.start >= heap->from_space_quarantine_source_end ||
                nggc_forwarding_target_locked(
                    heap, block.header, &target) != NGGC_OK ||
                !target || target->type_id != block.header->type_id ||
                target->size_class != block.header->size_class ||
                target->flags != block.header->flags ||
                target->payload_size != block.header->payload_size ||
                target->alloc_size != block.header->alloc_size ||
                target->object_id != block.header->object_id) {
                return NGGC_ERR_CORRUPTION;
            }
            if (seen_objects == SIZE_MAX ||
                seen_bytes > SIZE_MAX - block.header->alloc_size) {
                return NGGC_ERR_CORRUPTION;
            }
            ++seen_objects;
            seen_bytes += block.header->alloc_size;
        }
        cursor = block.end;
    }
    if (seen_objects != heap->from_space_quarantine_object_count ||
        seen_bytes != heap->from_space_quarantine_bytes ||
        heap->free_object_count > SIZE_MAX - seen_objects ||
        heap->free_bytes > SIZE_MAX - seen_bytes) {
        return NGGC_ERR_CORRUPTION;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_FORWARDED) {
            nggc_object_header *header = block.header;
            if (nggc_region_reclaim_quarantined_object_locked(
                    heap, header) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            header->type_id = NGGC_TYPE_NONE;
            header->size_class = 0u;
            header->flags = 0u;
            header->payload_size = 0u;
            header->object_id = 0u;
            atomic_store_explicit(&header->pin_count, 0u,
                                  memory_order_release);
            header->mark_epoch = 0u;
            header->generation = NGGC_REGION_GENERATION_NONE;
            header->young_age = 0u;
            if (nggc_free_block_link_locked(heap, header) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            atomic_store_explicit(&header->state, NGGC_OBJECT_FREE,
                                  memory_order_release);
        }
        cursor = block.end;
    }
    heap->from_space_quarantine_epoch = 0u;
    heap->from_space_quarantine_active = 0u;
    heap->from_space_quarantine_release_epoch = 0u;
    heap->from_space_quarantine_source_end = 0u;
    heap->from_space_quarantine_object_count = 0u;
    heap->from_space_quarantine_bytes = 0u;
    heap->read_barrier_active = nggc_expected_read_barrier_locked(heap);
    if (heap->from_space_delayed_reclaim_count != UINT64_MAX) {
        ++heap->from_space_delayed_reclaim_count;
    }
    heap->from_space_delayed_reclaimed_object_count =
        nggc_saturating_add_u64_size(
            heap->from_space_delayed_reclaimed_object_count,
            seen_objects);
    heap->from_space_delayed_reclaimed_bytes =
        nggc_saturating_add_u64_size(
            heap->from_space_delayed_reclaimed_bytes, seen_bytes);
    return nggc_rebuild_free_list_locked(heap);
}

static int nggc_slot_in_object(nggc_object_header *header, void **slot) {
    uintptr_t object_addr;
    uintptr_t payload_end;
    uintptr_t slot_end;
    uintptr_t slot_addr = (uintptr_t)slot;
    if (!header || !slot) {
        return 0;
    }
    object_addr = (uintptr_t)nggc_object_from_header(header);
    if (!nggc_range_end(object_addr, header->payload_size, &payload_end) ||
        !nggc_range_end(slot_addr, sizeof(void *), &slot_end)) {
        return 0;
    }
    return slot_addr >= object_addr && slot_end <= payload_end;
}

static nggc_result nggc_payload_owner_from_slot_locked(
    nggc_heap *heap, void **slot, nggc_object_header **out_owner) {
    uintptr_t cursor;
    uintptr_t slot_end;
    uintptr_t slot_addr;
    if (!heap || !slot || !out_owner) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_owner = 0;
    slot_addr = (uintptr_t)slot;
    if (!nggc_range_end(slot_addr, sizeof(void *), &slot_end)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (slot_end <= (uintptr_t)heap->gc_arena_base ||
        slot_addr >= heap->arena_end) {
        return NGGC_OK;
    }
    /* An external root/local has no payload owner. An address overlapping
       this arena must instead name a complete published payload slot. */
    if (slot_addr < (uintptr_t)heap->gc_arena_base ||
        slot_end > heap->arena_cursor) {
        return NGGC_ERR_CORRUPTION;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_result result = nggc_block_view_locked(heap, cursor, &block);
        if (result != NGGC_OK) {
            return result;
        }
        if (slot_addr < block.end && slot_end > block.start) {
            if ((block.state == NGGC_OBJECT_LIVE ||
                 block.state == NGGC_OBJECT_FORWARDED) &&
                nggc_slot_in_object(block.header, slot)) {
                *out_owner = block.header;
                return NGGC_OK;
            }
            return NGGC_ERR_CORRUPTION;
        }
        cursor = block.end;
    }
    return NGGC_ERR_CORRUPTION;
}

static int nggc_remset_classification_valid(uint32_t flags) {
    return flags == NGGC_REMSET_CLASSIFICATION_RECORD ||
           flags == (NGGC_REMSET_CLASSIFICATION_RECORD |
                     NGGC_REMSET_CLASSIFICATION_CROSS_REGION) ||
           flags == (NGGC_REMSET_CLASSIFICATION_RECORD |
                     NGGC_REMSET_CLASSIFICATION_CROSS_REGION |
                     NGGC_REMSET_CLASSIFICATION_OLD_TO_YOUNG);
}

static nggc_result nggc_remset_classification_expected(
    const nggc_heap *heap, size_t owner_region, size_t value_region,
    uint32_t owner_generation, uint32_t value_generation,
    uint32_t *out_flags) {
    uint32_t flags = NGGC_REMSET_CLASSIFICATION_RECORD;
    if (!heap || !out_flags || owner_region >= heap->region_count ||
        !nggc_live_object_generation_valid(owner_generation)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (value_region == SIZE_MAX) {
        if (value_generation != NGGC_REGION_GENERATION_NONE) {
            return NGGC_ERR_CORRUPTION;
        }
    } else {
        if (value_region >= heap->region_count ||
            !nggc_live_object_generation_valid(value_generation)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (owner_region != value_region) {
            flags |= NGGC_REMSET_CLASSIFICATION_CROSS_REGION;
            if (owner_generation == NGGC_REGION_GENERATION_OLD &&
                value_generation == NGGC_REGION_GENERATION_YOUNG) {
                flags |= NGGC_REMSET_CLASSIFICATION_OLD_TO_YOUNG;
            }
        }
    }
    if (!nggc_remset_classification_valid(flags)) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_flags = flags;
    return NGGC_OK;
}

#if NGGC_HAVE_C11_THREADS
static int nggc_remset_classification_descriptor_valid(
    const nggc_heap *heap, size_t owner_region, size_t value_region,
    uint32_t owner_generation, uint32_t value_generation) {
    if (!heap || owner_region >= heap->region_count ||
        !nggc_live_object_generation_valid(owner_generation)) {
        return 0;
    }
    if (value_region == SIZE_MAX) {
        return value_generation == NGGC_REGION_GENERATION_NONE;
    }
    return value_region < heap->region_count &&
           nggc_live_object_generation_valid(value_generation);
}
#endif

static nggc_result nggc_remset_count_classification_locked(
    nggc_heap *heap, uint32_t flags, int buffered, int deferred) {
    if (!heap || !nggc_remset_classification_valid(flags) ||
        (deferred && !buffered)) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->remset_record_count = nggc_saturating_add_u64_size(
        heap->remset_record_count, 1u);
    if ((flags & NGGC_REMSET_CLASSIFICATION_CROSS_REGION) != 0u) {
        heap->remset_cross_region_record_count =
            nggc_saturating_add_u64_size(
                heap->remset_cross_region_record_count, 1u);
    }
    if ((flags & NGGC_REMSET_CLASSIFICATION_OLD_TO_YOUNG) != 0u) {
        heap->remset_old_to_young_record_count =
            nggc_saturating_add_u64_size(
                heap->remset_old_to_young_record_count, 1u);
    }
    if (buffered) {
        heap->remset_buffer_record_count =
            nggc_saturating_add_u64_size(
                heap->remset_buffer_record_count, 1u);
    }
    if (deferred) {
        heap->remset_buffer_deferred_classification_count =
            nggc_saturating_add_u64_size(
                heap->remset_buffer_deferred_classification_count, 1u);
    }
    return NGGC_OK;
}

/*
 * The descriptor is written under the heap lock and published by the
 * owning mutator with a 32-bit release store.  A zero inflight value with
 * a non-zero outcome denotes a completed event waiting to be folded into
 * the heap's shared 64-bit telemetry counters.
 */
static nggc_result nggc_remset_classification_snapshot_locked(
    const nggc_heap *heap, const nggc_thread *thread,
    uint32_t *out_inflight, uint32_t *out_outcome,
    uint32_t *out_expected_flags) {
    uint32_t inflight;
    uint32_t outcome;
    uint32_t expected_flags = NGGC_REMSET_CLASSIFICATION_NONE;
    if (!heap || !thread) {
        return NGGC_ERR_CORRUPTION;
    }
    inflight = atomic_load_explicit(
        &thread->remset_classification_publication_inflight,
        memory_order_acquire);
    outcome = atomic_load_explicit(
        &thread->remset_classification_deferred_outcome,
        memory_order_acquire);
    if (inflight > 1u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (inflight == 0u &&
        outcome == NGGC_REMSET_CLASSIFICATION_NONE) {
        if (thread->remset_classification_owner_region != SIZE_MAX ||
            thread->remset_classification_value_region != SIZE_MAX ||
            thread->remset_classification_owner_generation !=
                NGGC_REGION_GENERATION_NONE ||
            thread->remset_classification_value_generation !=
                NGGC_REGION_GENERATION_NONE) {
            return NGGC_ERR_CORRUPTION;
        }
    } else {
        if ((thread->state != NGGC_THREAD_RUNNING &&
             thread->state != NGGC_THREAD_CONCURRENT_RUNNING) ||
            nggc_remset_classification_expected(
                heap,
                thread->remset_classification_owner_region,
                thread->remset_classification_value_region,
                thread->remset_classification_owner_generation,
                thread->remset_classification_value_generation,
                &expected_flags) != NGGC_OK ||
            (outcome != NGGC_REMSET_CLASSIFICATION_NONE &&
             outcome != expected_flags)) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (out_inflight) {
        *out_inflight = inflight;
    }
    if (out_outcome) {
        *out_outcome = outcome;
    }
    if (out_expected_flags) {
        *out_expected_flags = expected_flags;
    }
    return NGGC_OK;
}

/*
 * The heap lock stabilizes registry membership and the plain publication
 * descriptor.  The owning mutator may still observe global coverage,
 * deduplicate, or append one pre-authorized card without the heap lock, so
 * readers acquire inflight first and the published prefix second.  If
 * inflight is already zero, its
 * release store orders the preceding append/dedup/global-covered result
 * before the remaining acquire loads below.
 */
static nggc_result nggc_remset_buffer_snapshot_locked(
    const nggc_heap *heap, const nggc_thread *thread,
    size_t *out_published_count, size_t *out_effective_count,
    uint32_t *out_inflight, uint32_t *out_deferred_outcome) {
    size_t published_count;
    size_t effective_count;
    uint32_t inflight;
    uint32_t deferred_outcome;
    if (!heap || !thread || !thread->remset_card_buffer ||
        thread->remset_card_buffer_capacity !=
            heap->remset_buffer_capacity) {
        return NGGC_ERR_CORRUPTION;
    }
    inflight = atomic_load_explicit(
        &thread->remset_card_publication_inflight,
        memory_order_acquire);
    published_count = (size_t)atomic_load_explicit(
        &thread->remset_card_buffer_count, memory_order_acquire);
    deferred_outcome = atomic_load_explicit(
        &thread->remset_card_deferred_outcome,
        memory_order_acquire);
    if (inflight > 1u ||
        deferred_outcome > NGGC_REMSET_DEFERRED_OUTCOME_COVERED ||
        published_count > thread->remset_card_buffer_capacity) {
        return NGGC_ERR_CORRUPTION;
    }
    effective_count = published_count;
    if (inflight != 0u) {
        size_t index = thread->remset_card_publication_index;
        size_t card = thread->remset_card_publication_card;
        if ((thread->state != NGGC_THREAD_RUNNING &&
             thread->state != NGGC_THREAD_CONCURRENT_RUNNING) ||
            index >= thread->remset_card_buffer_capacity ||
            card >= heap->card_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (published_count == index) {
            int descriptor_duplicate = 0;
            unsigned char global_card = atomic_load_explicit(
                &heap->card_table[card], memory_order_acquire);
            if (global_card > 1u) {
                return NGGC_ERR_CORRUPTION;
            }
            for (size_t i = 0u; i < published_count; ++i) {
                if (thread->remset_card_buffer[i] == card) {
                    descriptor_duplicate = 1;
                    break;
                }
            }
            effective_count = descriptor_duplicate || global_card != 0u
                                  ? published_count
                                  : published_count + 1u;
            if ((deferred_outcome ==
                     NGGC_REMSET_DEFERRED_OUTCOME_COVERED &&
                 global_card == 0u) ||
                (deferred_outcome ==
                     NGGC_REMSET_DEFERRED_OUTCOME_APPENDED &&
                 descriptor_duplicate) ||
                (deferred_outcome ==
                     NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED &&
                 !descriptor_duplicate)) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (index < UINT_MAX && published_count == index + 1u &&
                   thread->remset_card_buffer[index] == card) {
            if (deferred_outcome ==
                    NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED ||
                deferred_outcome ==
                    NGGC_REMSET_DEFERRED_OUTCOME_COVERED) {
                return NGGC_ERR_CORRUPTION;
            }
            effective_count = published_count;
        } else {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (deferred_outcome != NGGC_REMSET_DEFERRED_OUTCOME_NONE) {
        size_t index = thread->remset_card_publication_index;
        size_t card = thread->remset_card_publication_card;
        if ((thread->state != NGGC_THREAD_RUNNING &&
             thread->state != NGGC_THREAD_CONCURRENT_RUNNING) ||
            index >= thread->remset_card_buffer_capacity ||
            card >= heap->card_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (deferred_outcome ==
            NGGC_REMSET_DEFERRED_OUTCOME_APPENDED) {
            if (index >= UINT_MAX || published_count != index + 1u ||
                thread->remset_card_buffer[index] != card) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (deferred_outcome ==
                   NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED) {
            int descriptor_duplicate = 0;
            if (published_count != index) {
                return NGGC_ERR_CORRUPTION;
            }
            for (size_t i = 0u; i < published_count; ++i) {
                if (thread->remset_card_buffer[i] == card) {
                    descriptor_duplicate = 1;
                    break;
                }
            }
            if (!descriptor_duplicate) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (deferred_outcome ==
                   NGGC_REMSET_DEFERRED_OUTCOME_COVERED) {
            unsigned char global_card = atomic_load_explicit(
                &heap->card_table[card], memory_order_acquire);
            if (published_count != index || global_card != 1u) {
                return NGGC_ERR_CORRUPTION;
            }
        } else {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (out_published_count) {
        *out_published_count = published_count;
    }
    if (out_effective_count) {
        *out_effective_count = effective_count;
    }
    if (out_inflight) {
        *out_inflight = inflight;
    }
    if (out_deferred_outcome) {
        *out_deferred_outcome = deferred_outcome;
    }
    return NGGC_OK;
}

static nggc_result nggc_verify_remset_buffers_locked(
    nggc_heap *heap, size_t *out_buffered_cards,
    size_t *out_buffered_threads,
    size_t *out_inflight_cards,
    size_t *out_inflight_threads,
    size_t *out_deferred_outcome_pending,
    size_t *out_classification_pending_threads,
    size_t *out_classification_pending_records,
    size_t *out_classification_pending_cross_region,
    size_t *out_classification_pending_old_to_young) {
    nggc_thread *thread;
    size_t seen_threads = 0u;
    size_t buffered_cards = 0u;
    size_t buffered_threads = 0u;
    size_t inflight_cards = 0u;
    size_t inflight_threads = 0u;
    size_t deferred_outcome_pending = 0u;
    size_t classification_pending_threads = 0u;
    size_t classification_pending_records = 0u;
    size_t classification_pending_cross_region = 0u;
    size_t classification_pending_old_to_young = 0u;
    if (!heap || !heap->card_table || heap->card_count == 0u ||
        heap->remset_buffer_capacity == 0u ||
        heap->remset_buffer_capacity > heap->card_count ||
        heap->remset_buffer_capacity > UINT_MAX ||
        heap->remset_buffer_capacity > SIZE_MAX / sizeof(size_t)) {
        return NGGC_ERR_CORRUPTION;
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        size_t published_count;
        size_t effective_count;
        uint32_t inflight;
        uint32_t deferred_outcome;
        uint32_t classification_inflight;
        uint32_t classification_outcome;
        uint32_t classification_flags;
        if (seen_threads >= heap->registered_thread_count ||
            thread->magic != NGGC_THREAD_MAGIC ||
            thread->heap != heap || !thread->remset_card_buffer ||
            thread->remset_card_buffer_capacity !=
                heap->remset_buffer_capacity ||
            nggc_remset_buffer_snapshot_locked(
                heap, thread, &published_count, &effective_count,
                &inflight, &deferred_outcome) != NGGC_OK ||
            nggc_remset_classification_snapshot_locked(
                heap, thread, &classification_inflight,
                &classification_outcome,
                &classification_flags) != NGGC_OK ||
            ((thread->state == NGGC_THREAD_AT_SAFEPOINT ||
              thread->state == NGGC_THREAD_COLLECTOR_OWNED ||
              thread->state == NGGC_THREAD_CONCURRENT_REPARKING) &&
             (effective_count != 0u || inflight != 0u ||
              deferred_outcome != 0u ||
              classification_inflight != 0u ||
              classification_outcome != 0u))) {
            return NGGC_ERR_CORRUPTION;
        }
        ++seen_threads;
        if (effective_count != 0u) {
            if (buffered_threads == SIZE_MAX ||
                effective_count >
                    SIZE_MAX - buffered_cards) {
                return NGGC_ERR_CORRUPTION;
            }
            ++buffered_threads;
            buffered_cards += effective_count;
        }
        if (inflight != 0u) {
            if (inflight_cards == SIZE_MAX ||
                inflight_threads == SIZE_MAX) {
                return NGGC_ERR_CORRUPTION;
            }
            ++inflight_cards;
            ++inflight_threads;
        }
        if (deferred_outcome != 0u) {
            if (deferred_outcome_pending == SIZE_MAX) {
                return NGGC_ERR_CORRUPTION;
            }
            ++deferred_outcome_pending;
        }
        if (classification_inflight != 0u ||
            classification_outcome != 0u) {
            if (classification_pending_threads == SIZE_MAX ||
                classification_pending_records == SIZE_MAX) {
                return NGGC_ERR_CORRUPTION;
            }
            ++classification_pending_threads;
            ++classification_pending_records;
            if ((classification_flags &
                 NGGC_REMSET_CLASSIFICATION_CROSS_REGION) != 0u) {
                if (classification_pending_cross_region == SIZE_MAX) {
                    return NGGC_ERR_CORRUPTION;
                }
                ++classification_pending_cross_region;
            }
            if ((classification_flags &
                 NGGC_REMSET_CLASSIFICATION_OLD_TO_YOUNG) != 0u) {
                if (classification_pending_old_to_young == SIZE_MAX) {
                    return NGGC_ERR_CORRUPTION;
                }
                ++classification_pending_old_to_young;
            }
        }
        for (size_t i = 0u;
             i < published_count; ++i) {
            if (thread->remset_card_buffer[i] >= heap->card_count) {
                return NGGC_ERR_CORRUPTION;
            }
            for (size_t j = 0u; j < i; ++j) {
                if (thread->remset_card_buffer[j] ==
                    thread->remset_card_buffer[i]) {
                    return NGGC_ERR_CORRUPTION;
                }
            }
        }
    }
    if (seen_threads != heap->registered_thread_count) {
        return NGGC_ERR_CORRUPTION;
    }
    if (out_buffered_cards) {
        *out_buffered_cards = buffered_cards;
    }
    if (out_buffered_threads) {
        *out_buffered_threads = buffered_threads;
    }
    if (out_inflight_cards) {
        *out_inflight_cards = inflight_cards;
    }
    if (out_inflight_threads) {
        *out_inflight_threads = inflight_threads;
    }
    if (out_deferred_outcome_pending) {
        *out_deferred_outcome_pending = deferred_outcome_pending;
    }
    if (out_classification_pending_threads) {
        *out_classification_pending_threads =
            classification_pending_threads;
    }
    if (out_classification_pending_records) {
        *out_classification_pending_records =
            classification_pending_records;
    }
    if (out_classification_pending_cross_region) {
        *out_classification_pending_cross_region =
            classification_pending_cross_region;
    }
    if (out_classification_pending_old_to_young) {
        *out_classification_pending_old_to_young =
            classification_pending_old_to_young;
    }
    return NGGC_OK;
}

#if NGGC_ENABLE_VERIFY
static int nggc_remset_card_covered_locked(const nggc_heap *heap,
                                           size_t card_index) {
    const nggc_thread *thread;
    size_t seen_threads = 0u;
    if (!heap || !heap->card_table || card_index >= heap->card_count) {
        return 0;
    }
    if (atomic_load_explicit(&heap->card_table[card_index],
                             memory_order_acquire) == 1u) {
        return 1;
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        size_t published_count;
        size_t effective_count;
        if (seen_threads >= heap->registered_thread_count) {
            return 0;
        }
        ++seen_threads;
        if (nggc_remset_buffer_snapshot_locked(
                heap, thread, &published_count, &effective_count,
                0, 0) != NGGC_OK) {
            return 0;
        }
        for (size_t i = 0u;
             i < published_count; ++i) {
            if (thread->remset_card_buffer[i] == card_index) {
                return 1;
            }
        }
        if (effective_count > published_count &&
            thread->remset_card_publication_card == card_index) {
            return 1;
        }
    }
    return 0;
}
#endif

static nggc_result nggc_remset_prepare_write_locked(
    nggc_heap *heap, nggc_object_header *owner_header,
    void **slot, void *value, size_t *out_card_index,
    size_t *out_owner_region, size_t *out_value_region,
    uint32_t *out_owner_generation,
    uint32_t *out_value_generation,
    uint32_t *out_classification_flags) {
    uintptr_t slot_addr;
    size_t card_index;
    size_t owner_region;
    size_t value_region = SIZE_MAX;
    uint32_t owner_generation;
    uint32_t value_generation = NGGC_REGION_GENERATION_NONE;
    if (!heap || !owner_header || !slot || !out_card_index ||
        !out_owner_region || !out_value_region ||
        !out_owner_generation || !out_value_generation ||
        !out_classification_flags ||
        !heap->card_table || heap->card_size == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    slot_addr = (uintptr_t)slot;
    if (slot_addr < (uintptr_t)heap->gc_arena_base ||
        slot_addr >= heap->arena_end) {
        return NGGC_ERR_CORRUPTION;
    }
    card_index = (size_t)((slot_addr - (uintptr_t)heap->gc_arena_base) /
                          heap->card_size);
    if (card_index >= heap->card_count) {
        return NGGC_ERR_CORRUPTION;
    }
    owner_region = (size_t)(((uintptr_t)owner_header -
                             (uintptr_t)heap->gc_arena_base) /
                            heap->region_size);
    owner_generation = owner_header->generation;
    if (value) {
        nggc_object_header *value_header =
            nggc_live_header_from_object(heap, value);
        if (!value_header) {
            return NGGC_ERR_CORRUPTION;
        }
        value_region = (size_t)(((uintptr_t)value_header -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        value_generation = value_header->generation;
    }
    *out_card_index = card_index;
    *out_owner_region = owner_region;
    *out_value_region = value_region;
    *out_owner_generation = owner_generation;
    *out_value_generation = value_generation;
    *out_classification_flags = NGGC_REMSET_CLASSIFICATION_NONE;
    return NGGC_OK;
}

static nggc_result nggc_remset_commit_card_locked(
    nggc_heap *heap, size_t card_index) {
    unsigned char card_value;
    if (!heap || !heap->card_table || card_index >= heap->card_count ||
        heap->remset_max_dirty_cards == 0u ||
        heap->remset_max_dirty_cards > heap->card_count ||
        heap->remset_mode > NGGC_REMSET_MODE_COARSE_HEAP) {
        return NGGC_ERR_CORRUPTION;
    }
    card_value = atomic_load_explicit(&heap->card_table[card_index],
                                      memory_order_acquire);
    if (card_value > 1u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->remset_mode == NGGC_REMSET_MODE_COARSE_HEAP) {
        if (card_value == 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        return NGGC_OK;
    }
    if (card_value == 0u &&
        heap->dirty_card_count >= heap->remset_max_dirty_cards) {
        for (size_t i = 0u; i < heap->card_count; ++i) {
            atomic_store_explicit(&heap->card_table[i], 1u,
                                  memory_order_release);
        }
        heap->dirty_card_count = heap->card_count;
        heap->remset_mode = NGGC_REMSET_MODE_COARSE_HEAP;
        if (heap->remset_overflow_count != UINT64_MAX) {
            ++heap->remset_overflow_count;
        }
    } else if (card_value == 0u) {
        atomic_store_explicit(&heap->card_table[card_index], 1u,
                              memory_order_release);
        ++heap->dirty_card_count;
    }
    return NGGC_OK;
}

static nggc_result nggc_remset_fold_deferred_outcome_locked(
    nggc_heap *heap, nggc_thread *thread) {
    unsigned outcome;
    uint32_t snapshot_outcome;
    if (!heap || !thread) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_remset_buffer_snapshot_locked(
            heap, thread, 0, 0, 0, &snapshot_outcome) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    outcome = snapshot_outcome;
    if (outcome == NGGC_REMSET_DEFERRED_OUTCOME_NONE) {
        return NGGC_OK;
    }
    outcome = atomic_exchange_explicit(
        &thread->remset_card_deferred_outcome,
        NGGC_REMSET_DEFERRED_OUTCOME_NONE,
        memory_order_acq_rel);
    if (outcome == NGGC_REMSET_DEFERRED_OUTCOME_APPENDED) {
        heap->remset_buffer_deferred_publish_count =
            nggc_saturating_add_u64_size(
                heap->remset_buffer_deferred_publish_count, 1u);
        return NGGC_OK;
    }
    if (outcome == NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED) {
        heap->remset_buffer_deduplicated_count =
            nggc_saturating_add_u64_size(
                heap->remset_buffer_deduplicated_count, 1u);
        heap->remset_buffer_deferred_deduplicated_count =
            nggc_saturating_add_u64_size(
                heap->remset_buffer_deferred_deduplicated_count, 1u);
        return NGGC_OK;
    }
    if (outcome != NGGC_REMSET_DEFERRED_OUTCOME_COVERED) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->remset_buffer_deferred_covered_count =
        nggc_saturating_add_u64_size(
            heap->remset_buffer_deferred_covered_count, 1u);
    return NGGC_OK;
}

static nggc_result nggc_remset_fold_classification_outcome_locked(
    nggc_heap *heap, nggc_thread *thread) {
    uint32_t inflight;
    uint32_t snapshot_outcome;
    uint32_t expected_flags;
    unsigned outcome;
    if (!heap || !thread ||
        nggc_remset_classification_snapshot_locked(
            heap, thread, &inflight, &snapshot_outcome,
            &expected_flags) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    if (inflight != 0u ||
        snapshot_outcome == NGGC_REMSET_CLASSIFICATION_NONE) {
        return NGGC_OK;
    }
    outcome = atomic_exchange_explicit(
        &thread->remset_classification_deferred_outcome,
        NGGC_REMSET_CLASSIFICATION_NONE, memory_order_acq_rel);
    if (outcome != snapshot_outcome || outcome != expected_flags ||
        nggc_remset_count_classification_locked(
            heap, outcome, 1, 1) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    thread->remset_classification_owner_region = SIZE_MAX;
    thread->remset_classification_value_region = SIZE_MAX;
    thread->remset_classification_owner_generation =
        NGGC_REGION_GENERATION_NONE;
    thread->remset_classification_value_generation =
        NGGC_REGION_GENERATION_NONE;
    return NGGC_OK;
}

static nggc_result nggc_remset_fold_all_deferred_outcomes_locked(
    nggc_heap *heap) {
    nggc_thread *thread;
    size_t seen = 0u;
    if (!heap) {
        return NGGC_ERR_CORRUPTION;
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        if (seen >= heap->registered_thread_count ||
            nggc_remset_fold_deferred_outcome_locked(
                heap, thread) != NGGC_OK ||
            nggc_remset_fold_classification_outcome_locked(
                heap, thread) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        ++seen;
    }
    return seen == heap->registered_thread_count
               ? NGGC_OK
               : NGGC_ERR_CORRUPTION;
}

static nggc_result nggc_remset_flush_thread_locked(
    nggc_heap *heap, nggc_thread *thread) {
    size_t count;
    size_t effective_count;
    uint32_t inflight;
    uint32_t deferred_outcome;
    nggc_result result;
    if (!heap || !thread || thread->magic != NGGC_THREAD_MAGIC ||
        thread->heap != heap || !thread->remset_card_buffer ||
        thread->remset_card_buffer_capacity !=
            heap->remset_buffer_capacity ||
        !heap->card_table || heap->card_count == 0u ||
        heap->remset_max_dirty_cards == 0u ||
        heap->remset_max_dirty_cards > heap->card_count ||
        heap->remset_mode > NGGC_REMSET_MODE_COARSE_HEAP ||
        nggc_remset_buffer_snapshot_locked(
            heap, thread, &count, &effective_count,
            &inflight, &deferred_outcome) != NGGC_OK ||
        inflight != 0u || effective_count != count) {
        return NGGC_ERR_CORRUPTION;
    }
    if (deferred_outcome != 0u &&
        nggc_remset_fold_deferred_outcome_locked(heap, thread) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_remset_fold_classification_outcome_locked(
            heap, thread) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    if (count == 0u) {
        return NGGC_OK;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (thread->remset_card_buffer[i] >= heap->card_count) {
            return NGGC_ERR_CORRUPTION;
        }
        for (size_t j = 0u; j < i; ++j) {
            if (thread->remset_card_buffer[j] ==
                thread->remset_card_buffer[i]) {
                return NGGC_ERR_CORRUPTION;
            }
        }
    }
    for (size_t i = 0u; i < count; ++i) {
        result = nggc_remset_commit_card_locked(
            heap, thread->remset_card_buffer[i]);
        if (result != NGGC_OK) {
            return result;
        }
    }
    atomic_store_explicit(&thread->remset_card_buffer_count, 0u,
                          memory_order_release);
    thread->remset_card_publication_index = SIZE_MAX;
    thread->remset_card_publication_card = SIZE_MAX;
    heap->remset_buffer_flush_count = nggc_saturating_add_u64_size(
        heap->remset_buffer_flush_count, 1u);
    heap->remset_buffer_flushed_card_count =
        nggc_saturating_add_u64_size(
            heap->remset_buffer_flushed_card_count, count);
    return NGGC_OK;
}

#if NGGC_HAVE_C11_THREADS
static nggc_result nggc_remset_reserve_classification_locked(
    const nggc_heap *heap, nggc_thread *thread,
    nggc_remset_publication *publication,
    size_t owner_region, size_t value_region,
    uint32_t owner_generation, uint32_t value_generation,
    uint32_t classification_flags) {
    if (!thread || !publication ||
        !heap ||
        (classification_flags != NGGC_REMSET_CLASSIFICATION_NONE &&
         !nggc_remset_classification_valid(classification_flags)) ||
        !nggc_remset_classification_descriptor_valid(
            heap, owner_region, value_region, owner_generation,
            value_generation) ||
        atomic_load_explicit(
            &thread->remset_classification_publication_inflight,
            memory_order_acquire) != 0u ||
        atomic_load_explicit(
            &thread->remset_classification_deferred_outcome,
            memory_order_acquire) != NGGC_REMSET_CLASSIFICATION_NONE ||
        thread->remset_classification_owner_region != SIZE_MAX ||
        thread->remset_classification_value_region != SIZE_MAX ||
        thread->remset_classification_owner_generation !=
            NGGC_REGION_GENERATION_NONE ||
        thread->remset_classification_value_generation !=
            NGGC_REGION_GENERATION_NONE ||
        (publication->thread && publication->thread != thread)) {
        return NGGC_ERR_CORRUPTION;
    }
    thread->remset_classification_owner_region = owner_region;
    thread->remset_classification_value_region = value_region;
    thread->remset_classification_owner_generation = owner_generation;
    thread->remset_classification_value_generation = value_generation;
    publication->thread = thread;
    publication->classification_flags = classification_flags;
    publication->classification_pending = 1u;
    atomic_store_explicit(
        &thread->remset_classification_publication_inflight, 1u,
        memory_order_release);
    return NGGC_OK;
}
#endif

static nggc_result nggc_remset_record_write_impl(
    nggc_heap *heap, nggc_thread *thread,
    nggc_object_header *owner_header, void **slot, void *value,
    nggc_remset_publication *out_publication) {
    size_t card_index;
    size_t owner_region;
    size_t value_region;
    size_t count = 0u;
    size_t effective_count = 0u;
    uint32_t owner_generation;
    uint32_t value_generation;
    uint32_t classification_flags;
    uint32_t inflight = 0u;
    uint32_t deferred_outcome = 0u;
#if NGGC_HAVE_C11_THREADS
    uint32_t classification_inflight = 0u;
    uint32_t classification_outcome = 0u;
    uint32_t defer_card_publication = 0u;
#endif
    nggc_result result;
    if (out_publication) {
        memset(out_publication, 0, sizeof(*out_publication));
    }
    if (!heap || !heap->card_table || heap->card_count == 0u ||
        heap->remset_max_dirty_cards == 0u ||
        heap->remset_max_dirty_cards > heap->card_count ||
        heap->remset_mode > NGGC_REMSET_MODE_COARSE_HEAP ||
        heap->remset_buffer_capacity == 0u ||
        heap->remset_buffer_capacity > heap->card_count ||
        heap->remset_buffer_capacity > UINT_MAX) {
        return NGGC_ERR_CORRUPTION;
    }
    if (thread && (!out_publication ||
        (thread->magic != NGGC_THREAD_MAGIC || thread->heap != heap ||
         !nggc_thread_is_registered(heap, thread) ||
         !thread->remset_card_buffer ||
         thread->remset_card_buffer_capacity !=
             heap->remset_buffer_capacity ||
         nggc_remset_buffer_snapshot_locked(
             heap, thread, &count, &effective_count,
             &inflight, &deferred_outcome) != NGGC_OK ||
#if NGGC_HAVE_C11_THREADS
         nggc_remset_classification_snapshot_locked(
             heap, thread, &classification_inflight,
             &classification_outcome, 0) != NGGC_OK ||
         classification_inflight != 0u ||
#endif
         inflight != 0u || effective_count != count))) {
        return NGGC_ERR_CORRUPTION;
    }
    if (thread && deferred_outcome != 0u &&
        nggc_remset_fold_deferred_outcome_locked(heap, thread) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    if (thread &&
        nggc_remset_fold_classification_outcome_locked(
            heap, thread) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_remset_prepare_write_locked(
        heap, owner_header, slot, value, &card_index,
        &owner_region, &value_region, &owner_generation,
        &value_generation, &classification_flags);
    if (result != NGGC_OK) {
        return result;
    }
#if NGGC_HAVE_C11_THREADS
    if (!thread &&
        nggc_remset_classification_expected(
            heap, owner_region, value_region, owner_generation,
            value_generation, &classification_flags) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
#else
    if (nggc_remset_classification_expected(
            heap, owner_region, value_region, owner_generation,
            value_generation, &classification_flags) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
#endif
    if (!thread) {
        result = nggc_remset_commit_card_locked(heap, card_index);
        if (result != NGGC_OK ||
            nggc_remset_count_classification_locked(
                heap, classification_flags, 0, 0) != NGGC_OK) {
            return result != NGGC_OK ? result : NGGC_ERR_CORRUPTION;
        }
        heap->remset_direct_record_count =
            nggc_saturating_add_u64_size(
                heap->remset_direct_record_count, 1u);
        return NGGC_OK;
    }
    if (heap->remset_mode == NGGC_REMSET_MODE_COARSE_HEAP) {
        goto record_success;
    }
#if NGGC_HAVE_C11_THREADS
    /* A non-full C11-owner buffer defers global coverage and prefix scans
     * until after unlock.  A full buffer has no descriptor slot, so its
     * global-card check and duplicate scan remain in this slow boundary. */
    if (count == thread->remset_card_buffer_capacity) {
        unsigned char global_card = atomic_load_explicit(
            &heap->card_table[card_index], memory_order_acquire);
        if (global_card > 1u) {
            return NGGC_ERR_CORRUPTION;
        }
        if (global_card != 0u) {
            goto record_success;
        }
#endif
#if !NGGC_HAVE_C11_THREADS
    {
        unsigned char global_card = atomic_load_explicit(
            &heap->card_table[card_index], memory_order_acquire);
        if (global_card > 1u) {
            return NGGC_ERR_CORRUPTION;
        }
        if (global_card != 0u) {
            goto record_success;
        }
    }
#endif
    for (size_t i = 0u; i < count; ++i) {
        if (thread->remset_card_buffer[i] == card_index) {
            heap->remset_buffer_deduplicated_count =
                nggc_saturating_add_u64_size(
                    heap->remset_buffer_deduplicated_count, 1u);
            goto record_success;
        }
    }
#if NGGC_HAVE_C11_THREADS
    }
#endif
    if (count == thread->remset_card_buffer_capacity) {
        heap->remset_buffer_overflow_flush_count =
            nggc_saturating_add_u64_size(
                heap->remset_buffer_overflow_flush_count, 1u);
        result = nggc_remset_flush_thread_locked(heap, thread);
        if (result != NGGC_OK) {
            return result;
        }
        if (heap->remset_mode == NGGC_REMSET_MODE_COARSE_HEAP) {
            goto record_success;
        }
        {
            unsigned char global_card = atomic_load_explicit(
                &heap->card_table[card_index], memory_order_acquire);
            if (global_card > 1u) {
                return NGGC_ERR_CORRUPTION;
            }
            if (global_card != 0u) {
                goto record_success;
            }
        }
        count = 0u;
    }
#if NGGC_HAVE_C11_THREADS
    defer_card_publication = 1u;
#else
    /* Without native-thread identity, another host thread could unregister
     * the sole eligible handle after unlock.  Keep this fallback publication
     * inside the heap lock rather than dereferencing a potentially freed
     * nggc_thread from an unlocked path. */
    thread->remset_card_buffer[count] = card_index;
    atomic_store_explicit(&thread->remset_card_buffer_count,
                          (unsigned)(count + 1u),
                          memory_order_release);
#endif

record_success:
#if NGGC_HAVE_C11_THREADS
    result = nggc_remset_reserve_classification_locked(
        heap, thread, out_publication, owner_region, value_region,
        owner_generation, value_generation, classification_flags);
    if (result != NGGC_OK) {
        return result;
    }
    if (defer_card_publication != 0u) {
        thread->remset_card_publication_index = count;
        thread->remset_card_publication_card = card_index;
        out_publication->index = count;
        out_publication->card = card_index;
        out_publication->pending = 1u;
        atomic_store_explicit(
            &thread->remset_card_publication_inflight, 1u,
            memory_order_release);
    }
#else
    if (nggc_remset_count_classification_locked(
            heap, classification_flags, 1, 0) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
#endif
    return NGGC_OK;
}

static nggc_result nggc_remset_finish_deferred_outcome(
    nggc_thread *thread, unsigned outcome) {
    unsigned previous;
    if (!thread ||
        (outcome != NGGC_REMSET_DEFERRED_OUTCOME_APPENDED &&
         outcome != NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED &&
         outcome != NGGC_REMSET_DEFERRED_OUTCOME_COVERED)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (atomic_load_explicit(
            &thread->remset_card_publication_inflight,
            memory_order_acquire) != 1u) {
        atomic_store_explicit(
            &thread->remset_card_deferred_outcome,
            NGGC_REMSET_DEFERRED_OUTCOME_CORRUPT,
            memory_order_release);
        return NGGC_ERR_CORRUPTION;
    }
    previous = atomic_exchange_explicit(
        &thread->remset_card_deferred_outcome, outcome,
        memory_order_acq_rel);
    if (previous != NGGC_REMSET_DEFERRED_OUTCOME_NONE) {
        atomic_store_explicit(
            &thread->remset_card_deferred_outcome,
            NGGC_REMSET_DEFERRED_OUTCOME_CORRUPT,
            memory_order_release);
    }
    atomic_store_explicit(&thread->remset_card_publication_inflight, 0u,
                          memory_order_release);
    return previous == NGGC_REMSET_DEFERRED_OUTCOME_NONE
               ? NGGC_OK
               : NGGC_ERR_CORRUPTION;
}

static nggc_result nggc_remset_publish_card_deferred(
    nggc_remset_publication *publication) {
    nggc_heap *heap;
    nggc_thread *thread;
    if (!publication || publication->pending == 0u ||
        !publication->thread) {
        return NGGC_OK;
    }
    thread = publication->thread;
    heap = thread->heap;
    if (!heap || !heap->card_table || !thread->remset_card_buffer ||
        publication->index >= thread->remset_card_buffer_capacity ||
        publication->card >= heap->card_count) {
        atomic_store_explicit(
            &thread->remset_card_deferred_outcome,
            NGGC_REMSET_DEFERRED_OUTCOME_CORRUPT,
            memory_order_release);
        atomic_store_explicit(
            &thread->remset_card_publication_inflight, 0u,
            memory_order_release);
        publication->pending = 0u;
        return NGGC_ERR_CORRUPTION;
    }
#if NGGC_HAVE_C11_THREADS && NGGC_ENABLE_VERIFY && \
    defined(NGGC_ENABLE_TEST_HOOKS) && NGGC_ENABLE_TEST_HOOKS
    if (atomic_exchange_explicit(
            &heap->test_remset_publish_pause_armed, 0u,
            memory_order_acq_rel) != 0u) {
        atomic_store_explicit(
            &heap->test_remset_publish_pause_entered, 1u,
            memory_order_release);
        while (atomic_load_explicit(
                   &heap->test_remset_publish_pause_release,
                   memory_order_acquire) == 0u) {
            thrd_yield();
        }
        atomic_store_explicit(
            &heap->test_remset_publish_pause_release, 0u,
            memory_order_release);
    }
#endif
    {
        unsigned char global_card = atomic_load_explicit(
            &heap->card_table[publication->card], memory_order_acquire);
        if (global_card > 1u) {
            atomic_store_explicit(
                &thread->remset_card_deferred_outcome,
                NGGC_REMSET_DEFERRED_OUTCOME_CORRUPT,
                memory_order_release);
            atomic_store_explicit(
                &thread->remset_card_publication_inflight, 0u,
                memory_order_release);
            publication->pending = 0u;
            return NGGC_ERR_CORRUPTION;
        }
        if (global_card != 0u) {
            nggc_result covered_result =
                nggc_remset_finish_deferred_outcome(
                    thread, NGGC_REMSET_DEFERRED_OUTCOME_COVERED);
            publication->pending = 0u;
            return covered_result;
        }
    }
    for (size_t i = 0u; i < publication->index; ++i) {
        if (thread->remset_card_buffer[i] == publication->card) {
            nggc_result dedup_result =
                nggc_remset_finish_deferred_outcome(
                    thread, NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED);
            publication->pending = 0u;
            return dedup_result;
        }
    }
    thread->remset_card_buffer[publication->index] = publication->card;
    atomic_store_explicit(&thread->remset_card_buffer_count,
                          (unsigned)(publication->index + 1u),
                          memory_order_release);
    {
        nggc_result append_result = nggc_remset_finish_deferred_outcome(
            thread, NGGC_REMSET_DEFERRED_OUTCOME_APPENDED);
        publication->pending = 0u;
        return append_result;
    }
}

static nggc_result nggc_remset_publish_classification_deferred(
    nggc_remset_publication *publication) {
    nggc_heap *heap;
    nggc_thread *thread;
    unsigned previous;
    if (!publication || publication->classification_pending == 0u) {
        return NGGC_OK;
    }
    thread = publication->thread;
    heap = thread ? thread->heap : 0;
    if (!thread || !heap ||
        atomic_load_explicit(
            &thread->remset_classification_publication_inflight,
            memory_order_acquire) != 1u) {
        if (thread) {
            atomic_store_explicit(
                &thread->remset_classification_deferred_outcome,
                NGGC_REMSET_CLASSIFICATION_CORRUPT,
                memory_order_release);
            atomic_store_explicit(
                &thread->remset_classification_publication_inflight, 0u,
                memory_order_release);
        }
        publication->classification_pending = 0u;
        return NGGC_ERR_CORRUPTION;
    }
#if NGGC_HAVE_C11_THREADS && NGGC_ENABLE_VERIFY && \
    defined(NGGC_ENABLE_TEST_HOOKS) && NGGC_ENABLE_TEST_HOOKS
    if (atomic_exchange_explicit(
            &thread->heap->test_remset_classification_pause_armed, 0u,
            memory_order_acq_rel) != 0u) {
        atomic_store_explicit(
            &thread->heap->test_remset_classification_pause_entered, 1u,
            memory_order_release);
        while (atomic_load_explicit(
                   &thread->heap
                        ->test_remset_classification_pause_release,
                   memory_order_acquire) == 0u) {
            thrd_yield();
        }
        atomic_store_explicit(
            &thread->heap->test_remset_classification_pause_release, 0u,
            memory_order_release);
    }
#endif
    if (publication->classification_flags ==
            NGGC_REMSET_CLASSIFICATION_NONE &&
        nggc_remset_classification_expected(
            heap,
            thread->remset_classification_owner_region,
            thread->remset_classification_value_region,
            thread->remset_classification_owner_generation,
            thread->remset_classification_value_generation,
            &publication->classification_flags) != NGGC_OK) {
        atomic_store_explicit(
            &thread->remset_classification_deferred_outcome,
            NGGC_REMSET_CLASSIFICATION_CORRUPT,
            memory_order_release);
        atomic_store_explicit(
            &thread->remset_classification_publication_inflight, 0u,
            memory_order_release);
        publication->classification_pending = 0u;
        return NGGC_ERR_CORRUPTION;
    }
    if (!nggc_remset_classification_valid(
            publication->classification_flags)) {
        atomic_store_explicit(
            &thread->remset_classification_deferred_outcome,
            NGGC_REMSET_CLASSIFICATION_CORRUPT,
            memory_order_release);
        atomic_store_explicit(
            &thread->remset_classification_publication_inflight, 0u,
            memory_order_release);
        publication->classification_pending = 0u;
        return NGGC_ERR_CORRUPTION;
    }
    previous = atomic_exchange_explicit(
        &thread->remset_classification_deferred_outcome,
        publication->classification_flags, memory_order_acq_rel);
    if (previous != NGGC_REMSET_CLASSIFICATION_NONE) {
        atomic_store_explicit(
            &thread->remset_classification_deferred_outcome,
            NGGC_REMSET_CLASSIFICATION_CORRUPT,
            memory_order_release);
    }
    atomic_store_explicit(
        &thread->remset_classification_publication_inflight, 0u,
        memory_order_release);
    publication->classification_pending = 0u;
    return previous == NGGC_REMSET_CLASSIFICATION_NONE
               ? NGGC_OK
               : NGGC_ERR_CORRUPTION;
}

static nggc_result nggc_remset_publish_deferred(
    nggc_remset_publication *publication) {
    nggc_result card_result;
    nggc_result classification_result;
    if (!publication) {
        return NGGC_OK;
    }
    card_result = nggc_remset_publish_card_deferred(publication);
    classification_result =
        nggc_remset_publish_classification_deferred(publication);
    return card_result != NGGC_OK ? card_result : classification_result;
}

static nggc_result nggc_remset_record_write(
    nggc_heap *heap, nggc_object_header *owner_header,
    void **slot, void *value) {
    return nggc_remset_record_write_impl(
        heap, 0, owner_header, slot, value, 0);
}

static void nggc_check_visit(void *ctx, void **slot) {
    nggc_check_ctx *check = (nggc_check_ctx *)ctx;
    nggc_object_header *header;
    nggc_object_header *target = 0;
    void *value;
    if (!check || check->result != NGGC_OK) {
        return;
    }
    if (!slot || !check->owner ||
        !nggc_slot_in_object(check->owner, slot) ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        check->result = NGGC_ERR_CORRUPTION;
        return;
    }
    value = *slot;
    header = value ? nggc_live_header_from_object(check->heap, value) : 0;
    if (value &&
        (!header ||
         nggc_forwarding_target_locked(check->heap, header, &target) !=
             NGGC_OK ||
         target)) {
        check->result = NGGC_ERR_CORRUPTION;
    }
}

#if NGGC_ENABLE_VERIFY
static void nggc_remset_verify_visit(void *ctx, void **slot) {
    nggc_remset_verify_ctx *verify = (nggc_remset_verify_ctx *)ctx;
    nggc_object_header *value_header;
    uintptr_t slot_addr;
    size_t card_index;
    size_t owner_region;
    size_t value_region;
    if (!verify || verify->result != NGGC_OK || !slot || !*slot) {
        return;
    }
    if (!verify->owner) {
        verify->result = NGGC_ERR_CORRUPTION;
        return;
    }
    value_header = nggc_live_header_from_object(verify->heap, *slot);
    if (!value_header) {
        verify->result = NGGC_ERR_CORRUPTION;
        return;
    }
    owner_region = (size_t)(((uintptr_t)verify->owner -
                             (uintptr_t)verify->heap->gc_arena_base) /
                            verify->heap->region_size);
    value_region = (size_t)(((uintptr_t)value_header -
                             (uintptr_t)verify->heap->gc_arena_base) /
                            verify->heap->region_size);
    if (owner_region >= verify->heap->region_count ||
        value_region >= verify->heap->region_count) {
        verify->result = NGGC_ERR_CORRUPTION;
        return;
    }
    if (owner_region == value_region) {
        return;
    }
    if (verify->cross_region_edge_count == SIZE_MAX) {
        verify->result = NGGC_ERR_CORRUPTION;
        return;
    }
    ++verify->cross_region_edge_count;
    if (verify->owner->generation == NGGC_REGION_GENERATION_OLD &&
        value_header->generation == NGGC_REGION_GENERATION_YOUNG) {
        if (verify->old_to_young_edge_count == SIZE_MAX) {
            verify->result = NGGC_ERR_CORRUPTION;
            return;
        }
        ++verify->old_to_young_edge_count;
    }
    slot_addr = (uintptr_t)slot;
    if (slot_addr < (uintptr_t)verify->heap->gc_arena_base ||
        slot_addr >= verify->heap->arena_end) {
        verify->result = NGGC_ERR_CORRUPTION;
        return;
    }
    card_index = (size_t)((slot_addr -
                           (uintptr_t)verify->heap->gc_arena_base) /
                          verify->heap->card_size);
    if (card_index >= verify->heap->card_count ||
        !nggc_remset_card_covered_locked(verify->heap, card_index)) {
        verify->result = NGGC_ERR_CORRUPTION;
    }
}

static nggc_result nggc_verify_remset_rebuild_locked(nggc_heap *heap) {
    uintptr_t cursor;
    nggc_remset_verify_ctx verify;
    nggc_result scan_result;
    if (!heap || !heap->card_table) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (nggc_verify_remset_buffers_locked(
            heap, 0, 0, 0, 0, 0, 0, 0, 0, 0) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    verify.heap = heap;
    verify.owner = 0;
    verify.result = NGGC_OK;
    verify.cross_region_edge_count = 0u;
    verify.old_to_young_edge_count = 0u;
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor && verify.result == NGGC_OK) {
        nggc_block_view block;
        nggc_object_header *header;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        if (block.state == NGGC_OBJECT_LIVE &&
            !nggc_internal_none_object(header) &&
            heap->types[header->type_id].registered &&
            (heap->types[header->type_id].desc.scan ||
             heap->types[header->type_id].desc.scan_ex)) {
            verify.owner = header;
            scan_result = nggc_invoke_scan(
                heap, header, &heap->types[header->type_id].desc,
                nggc_remset_verify_visit, &verify);
            if (verify.result == NGGC_OK && scan_result != NGGC_OK) {
                verify.result = scan_result;
            }
        }
        cursor = block.end;
    }
    heap->remset_rebuild_cross_region_edge_count =
        verify.cross_region_edge_count;
    heap->remset_rebuild_old_to_young_edge_count =
        verify.old_to_young_edge_count;
    if (heap->remset_rebuild_count != UINT64_MAX) {
        ++heap->remset_rebuild_count;
    }
    return verify.result;
}
#endif

#if NGGC_ENABLE_VERIFY
static int nggc_scan_replay_append(nggc_scan_replay_ctx *replay,
                                   void **slot, void *value) {
    nggc_scan_replay_item *items;
    size_t new_capacity;
    if (!replay || !slot) {
        return 0;
    }
    if (replay->count == replay->capacity) {
        new_capacity = replay->capacity != 0 ? replay->capacity * 2u : 8u;
        if (new_capacity < replay->capacity ||
            new_capacity > SIZE_MAX / sizeof(*items)) {
            return 0;
        }
        items = (nggc_scan_replay_item *)realloc(
            replay->items, new_capacity * sizeof(*items));
        if (!items) {
            return 0;
        }
        replay->items = items;
        replay->capacity = new_capacity;
    }
    replay->items[replay->count].slot = slot;
    replay->items[replay->count].value = value;
    replay->items[replay->count].seen = 0u;
    ++replay->count;
    return 1;
}

static void nggc_scan_replay_visit(void *ctx, void **slot) {
    nggc_scan_replay_ctx *replay = (nggc_scan_replay_ctx *)ctx;
    size_t i;
    if (!replay || replay->check.result != NGGC_OK) {
        return;
    }
    nggc_check_visit(&replay->check, slot);
    if (replay->check.result != NGGC_OK) {
        return;
    }
    if (replay->replay_pass == 0u) {
        for (i = 0; i < replay->count; ++i) {
            if (replay->items[i].slot == slot) {
                replay->check.result = NGGC_ERR_CORRUPTION;
                return;
            }
        }
        if (!nggc_scan_replay_append(replay, slot, *slot)) {
            replay->check.result = NGGC_ERR_OUT_OF_MEMORY;
        }
        return;
    }
    for (i = 0; i < replay->count; ++i) {
        if (replay->items[i].slot == slot) {
            if (replay->items[i].seen || replay->items[i].value != *slot) {
                replay->check.result = NGGC_ERR_CORRUPTION;
                return;
            }
            replay->items[i].seen = 1u;
            return;
        }
    }
    replay->check.result = NGGC_ERR_CORRUPTION;
}

static nggc_result nggc_verify_scan_replay_locked(
    nggc_heap *heap, nggc_object_header *owner,
    const nggc_type_desc *desc) {
    nggc_scan_replay_ctx replay;
    unsigned char *payload_snapshot = 0;
    void *object;
    nggc_result result;
    size_t i;
    if (!heap || !owner || !desc || (!desc->scan && !desc->scan_ex)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    memset(&replay, 0, sizeof(replay));
    replay.check.heap = heap;
    replay.check.owner = owner;
    replay.check.result = NGGC_OK;
    object = nggc_object_from_header(owner);
    if (owner->payload_size != 0u) {
        payload_snapshot = (unsigned char *)malloc(owner->payload_size);
        if (!payload_snapshot) {
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        memcpy(payload_snapshot, object, owner->payload_size);
    }

    result = nggc_invoke_scan(heap, owner, desc,
                              nggc_scan_replay_visit, &replay);
    if (result != NGGC_OK) {
        replay.check.result = result;
    }
    if (replay.check.result == NGGC_OK &&
        owner->payload_size != 0u &&
        memcmp(payload_snapshot, object, owner->payload_size) != 0) {
        replay.check.result = NGGC_ERR_CORRUPTION;
    }
    if (replay.check.result == NGGC_OK) {
        replay.replay_pass = 1u;
        result = nggc_invoke_scan(heap, owner, desc,
                                  nggc_scan_replay_visit, &replay);
        if (result != NGGC_OK) {
            replay.check.result = result;
        }
    }
    if (replay.check.result == NGGC_OK &&
        owner->payload_size != 0u &&
        memcmp(payload_snapshot, object, owner->payload_size) != 0) {
        replay.check.result = NGGC_ERR_CORRUPTION;
    }
    if (replay.check.result == NGGC_OK) {
        for (i = 0; i < replay.count; ++i) {
            if (!replay.items[i].seen ||
                replay.items[i].value != *replay.items[i].slot) {
                replay.check.result = NGGC_ERR_CORRUPTION;
                break;
            }
        }
    }
    result = replay.check.result;
    free(payload_snapshot);
    free(replay.items);
    return result;
}
#endif

static nggc_result nggc_remap_slot_locked(nggc_heap *heap, void **slot,
                                          int *out_rewritten) {
    void *value;
    nggc_object_header *header;
    nggc_object_header *target;
    if (out_rewritten) {
        *out_rewritten = 0;
    }
    if (!heap || !slot ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    value = *slot;
    if (!value) {
        return NGGC_OK;
    }
    header = nggc_live_header_from_object(heap, value);
    if (!header) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_forwarding_target_locked(heap, header, &target) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    if (target) {
        *slot = nggc_object_from_header(target);
        if (out_rewritten) {
            *out_rewritten = 1;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_remap_phantom_reaper_slots_locked(
    nggc_heap *heap);

static nggc_result nggc_remap_internal_slot_locked(
    nggc_heap *heap, nggc_object_header *owner, void **slot,
    atomic_size_t *scanned_slot_count,
    atomic_size_t *rewritten_slot_count) {
    int rewritten = 0;
    nggc_result result;

    if (!heap || !owner || !slot || !nggc_slot_in_object(owner, slot) ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (scanned_slot_count) {
        atomic_fetch_add_explicit(scanned_slot_count, 1u,
                                  memory_order_relaxed);
    }
    result = nggc_remap_slot_locked(heap, slot, &rewritten);
    if (result == NGGC_OK && rewritten && rewritten_slot_count) {
        atomic_fetch_add_explicit(rewritten_slot_count, 1u,
                                  memory_order_relaxed);
    }
    return result;
}

static nggc_result nggc_remap_internal_object_locked(
    nggc_heap *heap, nggc_object_header *header,
    atomic_size_t *scanned_slot_count,
    atomic_size_t *rewritten_slot_count) {
    void *object;

    if (!heap || !header || !nggc_internal_none_object(header)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    object = nggc_object_from_header(header);
    if (nggc_internal_weak_reference_object(header)) {
        nggc_internal_weak_reference *weak =
            (nggc_internal_weak_reference *)object;
        if (header->payload_size < sizeof(*weak)) {
            return NGGC_ERR_CORRUPTION;
        }
        return nggc_remap_internal_slot_locked(
            heap, header, &weak->referent, scanned_slot_count,
            rewritten_slot_count);
    }
    if (nggc_internal_soft_reference_object(header)) {
        nggc_internal_soft_reference *soft =
            (nggc_internal_soft_reference *)object;
        if (header->payload_size < sizeof(*soft)) {
            return NGGC_ERR_CORRUPTION;
        }
        return nggc_remap_internal_slot_locked(
            heap, header, &soft->referent, scanned_slot_count,
            rewritten_slot_count);
    }
    if (nggc_internal_phantom_queue_object(header)) {
        nggc_internal_phantom_queue *queue =
            (nggc_internal_phantom_queue *)object;
        nggc_result result;
        if (header->payload_size < sizeof(*queue)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (queue->pending_count == 0u) {
            return (queue->head == 0 && queue->tail == 0)
                       ? NGGC_OK
                       : NGGC_ERR_CORRUPTION;
        }
        if (!queue->head || !queue->tail) {
            return NGGC_ERR_CORRUPTION;
        }
        result = nggc_remap_internal_slot_locked(
            heap, header, &queue->head, scanned_slot_count,
            rewritten_slot_count);
        if (result != NGGC_OK) {
            return result;
        }
        result = nggc_remap_internal_slot_locked(
            heap, header, &queue->tail, scanned_slot_count,
            rewritten_slot_count);
        if (result != NGGC_OK) {
            return result;
        }
        if (!nggc_internal_phantom_reference_object(
                nggc_live_header_from_object(heap, queue->head)) ||
            !nggc_internal_phantom_reference_object(
                nggc_live_header_from_object(heap, queue->tail))) {
            return NGGC_ERR_CORRUPTION;
        }
        return NGGC_OK;
    }
    if (nggc_internal_phantom_reference_object(header)) {
        nggc_internal_phantom_reference *phantom =
            (nggc_internal_phantom_reference *)object;
        nggc_object_header *queue_header;
        nggc_result result;
        if (header->payload_size < sizeof(*phantom) ||
            phantom->enqueued > 1u || !phantom->queue ||
            (!phantom->enqueued && phantom->next) ||
            (phantom->enqueued && phantom->referent)) {
            return NGGC_ERR_CORRUPTION;
        }
        result = nggc_remap_internal_slot_locked(
            heap, header, &phantom->queue, scanned_slot_count,
            rewritten_slot_count);
        if (result != NGGC_OK) {
            return result;
        }
        queue_header = nggc_live_header_from_object(heap, phantom->queue);
        if (!nggc_internal_phantom_queue_object(queue_header) ||
            queue_header->payload_size < sizeof(nggc_internal_phantom_queue)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (!phantom->enqueued) {
            return nggc_remap_internal_slot_locked(
                heap, header, &phantom->referent, scanned_slot_count,
                rewritten_slot_count);
        }
        return nggc_remap_internal_slot_locked(
            heap, header, &phantom->next, scanned_slot_count,
            rewritten_slot_count);
    }
    if (header->size_class != NGGC_INTERNAL_KIND_NONE) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static void nggc_remap_visit(void *ctx, void **slot) {
    nggc_remap_ctx *remap = (nggc_remap_ctx *)ctx;
    int rewritten = 0;
    if (!remap || remap->result != NGGC_OK) {
        return;
    }
    if (!remap->owner || !nggc_slot_in_object(remap->owner, slot) ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        remap->result = NGGC_ERR_CORRUPTION;
        return;
    }
    if (remap->scanned_slot_count) {
        atomic_fetch_add_explicit(remap->scanned_slot_count, 1u,
                                  memory_order_relaxed);
    }
    remap->result = nggc_remap_slot_locked(remap->heap, slot, &rewritten);
    if (remap->result == NGGC_OK && rewritten &&
        remap->rewritten_slot_count) {
        atomic_fetch_add_explicit(remap->rewritten_slot_count, 1u,
                                  memory_order_relaxed);
    }
    if (remap->result == NGGC_OK && remap->record_remset &&
        remap->owner && *slot &&
        !nggc_forwarding_load_acquire(remap->owner)) {
        remap->result = nggc_remset_record_write(
            remap->heap, remap->owner, slot, *slot);
    }
}

static nggc_result nggc_verify_no_forwarded_slot_locked(nggc_heap *heap,
                                                        void **slot) {
    void *value;
    nggc_object_header *header;
    nggc_object_header *target;
    if (!heap || !slot) {
        return NGGC_ERR_CORRUPTION;
    }
    value = *slot;
    if (!value) {
        return NGGC_OK;
    }
    header = nggc_live_header_from_object(heap, value);
    if (!header ||
        nggc_forwarding_target_locked(heap, header, &target) != NGGC_OK ||
        target) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_verify_root_registry_locked(nggc_heap *heap) {
    size_t i;
    if (!heap || heap->root_count > heap->root_capacity ||
        heap->root_capacity > SIZE_MAX / sizeof(*heap->roots) ||
        ((heap->root_capacity == 0u) != (heap->roots == 0))) {
        return NGGC_ERR_CORRUPTION;
    }
    for (i = 0; i < heap->root_count; ++i) {
        void **slot = heap->roots[i];
        nggc_result result;
        if (!slot || ((uintptr_t)slot % _Alignof(void *)) != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
#if NGGC_ENABLE_VERIFY
        for (size_t j = 0; j < i; ++j) {
            if (heap->roots[j] == slot) {
                return NGGC_ERR_CORRUPTION;
            }
        }
#endif
        result = nggc_verify_no_forwarded_slot_locked(heap, slot);
        if (result != NGGC_OK) {
            return result;
        }
    }
    return NGGC_OK;
}

#if NGGC_ENABLE_VERIFY
static int nggc_registered_thread_node_valid_locked(
    const nggc_heap *heap, const nggc_thread *thread) {
    if (!heap || !thread || thread->magic != NGGC_THREAD_MAGIC ||
        thread->heap != heap ||
        (thread->state != NGGC_THREAD_RUNNING &&
         thread->state != NGGC_THREAD_AT_SAFEPOINT &&
         thread->state != NGGC_THREAD_COLLECTOR_OWNED &&
         thread->state != NGGC_THREAD_CONCURRENT_RUNNING &&
         thread->state != NGGC_THREAD_CONCURRENT_REPARKING) ||
        thread->root_count > thread->root_capacity ||
        thread->root_capacity > SIZE_MAX / sizeof(*thread->roots) ||
        ((thread->root_capacity == 0u) != (thread->roots == 0)) ||
        thread->safepoint_poll_count > heap->safepoint_poll_count) {
        return 0;
    }
    if ((thread->state == NGGC_THREAD_AT_SAFEPOINT ||
         thread->state == NGGC_THREAD_COLLECTOR_OWNED) &&
        thread->observed_safepoint_epoch != heap->safepoint_epoch) {
        return 0;
    }
    for (size_t i = 0; i < thread->root_count; ++i) {
        void **slot = thread->roots[i];
        if (!slot || ((uintptr_t)slot % _Alignof(void *)) != 0u ||
            nggc_verify_no_forwarded_slot_locked((nggc_heap *)heap, slot) !=
                NGGC_OK) {
            return 0;
        }
        for (size_t j = 0; j < i; ++j) {
            if (thread->roots[j] == slot) {
                return 0;
            }
        }
        for (size_t j = 0; j < heap->root_count; ++j) {
            if (heap->roots[j] == slot) {
                return 0;
            }
        }
        for (const nggc_thread *other = heap->threads;
             other && other != thread; other = other->next) {
            if (other->root_count > other->root_capacity ||
                other->root_capacity > SIZE_MAX / sizeof(*other->roots) ||
                ((other->root_capacity == 0u) != (other->roots == 0))) {
                return 0;
            }
            for (size_t j = 0; j < other->root_count; ++j) {
                if (other->roots[j] == slot) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static nggc_result nggc_verify_thread_registry_locked(nggc_heap *heap) {
    nggc_thread *slow;
    nggc_thread *fast;
    nggc_thread *thread;
    size_t seen = 0;
    size_t pending = 0;
    size_t parked = 0;
    size_t concurrent_running = 0;
    size_t repark_pending = 0;
    size_t collector_owned = 0;
    if (!heap || heap->safepoint_pending_threads >
                     heap->registered_thread_count ||
        (heap->safepoint_epoch == 0u &&
         heap->safepoint_pending_threads != 0u)) {
        return NGGC_ERR_CORRUPTION;
    }

    slow = heap->threads;
    fast = heap->threads;
    while (fast) {
        if (!nggc_registered_thread_node_valid_locked(heap, slow) ||
            !nggc_registered_thread_node_valid_locked(heap, fast)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (!fast->next) {
            break;
        }
        if (!nggc_registered_thread_node_valid_locked(heap, fast->next)) {
            return NGGC_ERR_CORRUPTION;
        }
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            return NGGC_ERR_CORRUPTION;
        }
    }

    thread = heap->threads;
    while (thread) {
        if (!nggc_registered_thread_node_valid_locked(heap, thread) ||
            seen == SIZE_MAX) {
            return NGGC_ERR_CORRUPTION;
        }
        ++seen;
        if (thread->observed_safepoint_epoch != heap->safepoint_epoch) {
            if (pending == SIZE_MAX) {
                return NGGC_ERR_CORRUPTION;
            }
            ++pending;
            if (thread->state != NGGC_THREAD_RUNNING) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (heap->safepoint_active) {
            if (thread->state == NGGC_THREAD_AT_SAFEPOINT ||
                thread->state == NGGC_THREAD_COLLECTOR_OWNED) {
                if (parked == SIZE_MAX) {
                    return NGGC_ERR_CORRUPTION;
                }
                ++parked;
            } else if (thread->state == NGGC_THREAD_CONCURRENT_RUNNING) {
                if (concurrent_running == SIZE_MAX) {
                    return NGGC_ERR_CORRUPTION;
                }
                ++concurrent_running;
            } else if (thread->state == NGGC_THREAD_CONCURRENT_REPARKING) {
                if (repark_pending == SIZE_MAX) {
                    return NGGC_ERR_CORRUPTION;
                }
                ++repark_pending;
            } else {
                return NGGC_ERR_CORRUPTION;
            }
            if (thread->state == NGGC_THREAD_COLLECTOR_OWNED) {
                if (collector_owned != 0u ||
                    heap->collect_safepoint_owner != thread) {
                    return NGGC_ERR_CORRUPTION;
                }
                ++collector_owned;
            }
        } else if (thread->state != NGGC_THREAD_RUNNING) {
            return NGGC_ERR_CORRUPTION;
        }
        thread = thread->next;
    }
    if (seen != heap->registered_thread_count ||
        pending != heap->safepoint_pending_threads ||
        parked != heap->safepoint_parked_threads ||
        concurrent_running != heap->mutator_concurrent_running_threads ||
        repark_pending !=
            heap->mutator_concurrent_repark_pending_threads ||
        collector_owned !=
            (heap->collect_safepoint_owner ? 1u : 0u)) {
        return NGGC_ERR_CORRUPTION;
    }
    return nggc_verify_barrier_tss_current_locked(heap);
}
#endif

static nggc_result nggc_verify_registries_locked(nggc_heap *heap) {
    nggc_result result = nggc_verify_root_registry_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
#if NGGC_ENABLE_VERIFY
    result = nggc_verify_thread_registry_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
#else
    result = NGGC_OK;
#endif
    return nggc_verify_tlabs_locked(heap);
}

static void nggc_forwarded_slot_check_visit(void *ctx, void **slot) {
    nggc_forwarded_slot_check_ctx *check =
        (nggc_forwarded_slot_check_ctx *)ctx;
    if (!check || check->result != NGGC_OK) {
        return;
    }
    if (!check->owner || !nggc_slot_in_object(check->owner, slot) ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        check->result = NGGC_ERR_CORRUPTION;
        return;
    }
    check->result = nggc_verify_no_forwarded_slot_locked(check->heap, slot);
    if (check->result == NGGC_OK && check->record_remset && *slot &&
        !nggc_forwarding_load_acquire(check->owner)) {
        check->result = nggc_remset_record_write(
            check->heap, check->owner, slot, *slot);
    }
}

static nggc_result nggc_finalize_remapped_strong_slots_locked(
    nggc_heap *heap, int record_remset) {
    nggc_forwarded_slot_check_ctx check;
    uintptr_t cursor;
    nggc_result result;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    check.heap = heap;
    check.owner = 0;
    check.record_remset = record_remset;
    check.result = NGGC_OK;
    for (size_t i = 0; i < heap->root_count; ++i) {
        check.result =
            nggc_verify_no_forwarded_slot_locked(heap, heap->roots[i]);
        if (check.result != NGGC_OK) {
            return check.result;
        }
    }
    for (nggc_thread *thread = heap->threads; thread;
         thread = thread->next) {
        if (thread->root_count > thread->root_capacity ||
            thread->root_capacity > SIZE_MAX / sizeof(*thread->roots) ||
            ((thread->root_capacity == 0u) != (thread->roots == 0))) {
            return NGGC_ERR_CORRUPTION;
        }
        for (size_t i = 0; i < thread->root_count; ++i) {
            check.result = nggc_verify_no_forwarded_slot_locked(
                heap, thread->roots[i]);
            if (check.result != NGGC_OK) {
                return check.result;
            }
        }
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        uint32_t state;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        state = block.state;
        if (state == NGGC_OBJECT_LIVE) {
            nggc_type_slot *slot;
            if (!nggc_internal_none_object(header)) {
                if (!nggc_type_id_valid(header->type_id) ||
                    !heap->types[header->type_id].registered) {
                    return NGGC_ERR_CORRUPTION;
                }
                slot = &heap->types[header->type_id];
                if (slot->desc.scan || slot->desc.scan_ex) {
                    check.owner = header;
                    result = nggc_invoke_scan(
                        heap, header, &slot->desc,
                        nggc_forwarded_slot_check_visit, &check);
                    if (check.result == NGGC_OK && result != NGGC_OK) {
                        check.result = result;
                    }
                    check.owner = 0;
                    if (check.result != NGGC_OK) {
                        return check.result;
                    }
                }
            }
        } else if (state != NGGC_OBJECT_FREE &&
                   state != NGGC_OBJECT_DECOMMITTED_HUMONGOUS) {
            return NGGC_ERR_CORRUPTION;
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static nggc_result nggc_remap_fault_after_rewrite_locked(
    nggc_heap *heap, int rewritten) {
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (rewritten && heap && heap->collector_cycle_active &&
        heap->collector_cycle_kind ==
            NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) {
        return nggc_test_collect_fault_take_locked(
            heap, NGGC_TEST_COLLECT_FAULT_DURING_REMAP,
            NGGC_ERR_CORRUPTION);
    }
#else
    (void)heap;
    (void)rewritten;
#endif
    return NGGC_OK;
}

static nggc_result nggc_remap_registered_roots_locked(
    nggc_heap *heap, atomic_size_t *scanned_slot_count,
    atomic_size_t *rewritten_slot_count, int inject_fault) {
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0u; i < heap->root_count; ++i) {
        int rewritten = 0;
        nggc_result result;
        if (scanned_slot_count) {
            atomic_fetch_add_explicit(scanned_slot_count, 1u,
                                      memory_order_relaxed);
        }
        result = nggc_remap_slot_locked(heap, heap->roots[i], &rewritten);
        if (result != NGGC_OK) {
            return result;
        }
        if (rewritten && rewritten_slot_count) {
            atomic_fetch_add_explicit(rewritten_slot_count, 1u,
                                      memory_order_relaxed);
        }
        if (inject_fault) {
            result = nggc_remap_fault_after_rewrite_locked(heap, rewritten);
            if (result != NGGC_OK) {
                return result;
            }
        }
    }
    for (nggc_thread *thread = heap->threads; thread;
         thread = thread->next) {
        if (thread->root_count > thread->root_capacity ||
            thread->root_capacity > SIZE_MAX / sizeof(*thread->roots) ||
            ((thread->root_capacity == 0u) != (thread->roots == 0))) {
            return NGGC_ERR_CORRUPTION;
        }
        for (size_t i = 0u; i < thread->root_count; ++i) {
            int rewritten = 0;
            nggc_result result;
            if (scanned_slot_count) {
                atomic_fetch_add_explicit(scanned_slot_count, 1u,
                                          memory_order_relaxed);
            }
            result = nggc_remap_slot_locked(
                heap, thread->roots[i], &rewritten);
            if (result != NGGC_OK) {
                return result;
            }
            if (rewritten && rewritten_slot_count) {
                atomic_fetch_add_explicit(rewritten_slot_count, 1u,
                                          memory_order_relaxed);
            }
            if (inject_fault) {
                result = nggc_remap_fault_after_rewrite_locked(
                    heap, rewritten);
                if (result != NGGC_OK) {
                    return result;
                }
            }
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_remap_forwarded_slots_serial_locked(
    nggc_heap *heap) {
    nggc_remap_ctx remap;
    uintptr_t cursor;
    nggc_result scan_result;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    remap.heap = heap;
    remap.owner = 0;
    remap.record_remset = 0;
    remap.result = NGGC_OK;
    remap.scanned_slot_count = 0;
    remap.rewritten_slot_count = 0;
    remap.result = nggc_remap_registered_roots_locked(
        heap, 0, 0, 0);
    if (remap.result != NGGC_OK) {
        return remap.result;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        if (block.state == NGGC_OBJECT_LIVE) {
            nggc_object_header *forwarding_target;
            nggc_type_slot *slot;
            if (nggc_forwarding_target_locked(
                    heap, header, &forwarding_target) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            if (!nggc_internal_none_object(header)) {
                if (!nggc_type_id_valid(header->type_id) ||
                    !heap->types[header->type_id].registered) {
                    return NGGC_ERR_CORRUPTION;
                }
                slot = &heap->types[header->type_id];
                if (slot->desc.scan || slot->desc.scan_ex) {
                    remap.owner = header;
                    scan_result = nggc_invoke_scan(
                        heap, header, &slot->desc, nggc_remap_visit, &remap);
                    if (remap.result == NGGC_OK && scan_result != NGGC_OK) {
                        remap.result = scan_result;
                    }
                    remap.owner = 0;
                    if (remap.result != NGGC_OK) {
                        return remap.result;
                    }
                }
            } else {
                remap.result = nggc_remap_internal_object_locked(
                    heap, header, 0, 0);
                if (remap.result != NGGC_OK) {
                    return remap.result;
                }
            }
        }
        cursor = block.end;
    }
    remap.result = nggc_remap_phantom_reaper_slots_locked(heap);
    if (remap.result != NGGC_OK) {
        return remap.result;
    }
    return nggc_finalize_remapped_strong_slots_locked(heap, 0);
}

static void nggc_remap_worker_record_error(
    nggc_remap_worker_ctx *worker, nggc_result result) {
    int expected = NGGC_OK;
    if (!worker || result == NGGC_OK) {
        return;
    }
    (void)atomic_compare_exchange_strong_explicit(
        &worker->result, &expected, (int)result,
        memory_order_release, memory_order_relaxed);
}

static void nggc_remap_worker_peak_update(nggc_heap *heap,
                                           uint32_t active) {
    uint32_t peak = atomic_load_explicit(
        &heap->remap_workers_peak_active, memory_order_relaxed);
    while (peak < active &&
           !atomic_compare_exchange_weak_explicit(
               &heap->remap_workers_peak_active, &peak, active,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void nggc_remap_worker_run(nggc_remap_worker_ctx *worker) {
    uint32_t active;
    if (!worker || !worker->heap) {
        return;
    }
    active = atomic_fetch_add_explicit(
                 &worker->heap->remap_workers_active, 1u,
                 memory_order_acq_rel) +
             1u;
    nggc_remap_worker_peak_update(worker->heap, active);
    atomic_fetch_add_explicit(&worker->entered_workers, 1u,
                              memory_order_relaxed);
    for (;;) {
        uintptr_t cursor;
        nggc_block_view block;
        nggc_result result;
        if ((nggc_result)atomic_load_explicit(
                &worker->result, memory_order_acquire) != NGGC_OK) {
            break;
        }
        cursor = atomic_load_explicit(&worker->next_cursor,
                                      memory_order_acquire);
        if (cursor >= worker->end_cursor) {
            break;
        }
        result = nggc_block_view_locked(worker->heap, cursor, &block);
        if (result != NGGC_OK) {
            nggc_remap_worker_record_error(worker, result);
            break;
        }
        if (!atomic_compare_exchange_weak_explicit(
                &worker->next_cursor, &cursor, block.end,
                memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }
        atomic_fetch_add_explicit(&worker->block_count, 1u,
                                  memory_order_relaxed);
        if (block.state == NGGC_OBJECT_LIVE) {
            nggc_object_header *forwarding_target;
            nggc_object_header *header = block.header;
            nggc_type_slot *type_slot;
            result = nggc_forwarding_target_locked(
                worker->heap, header, &forwarding_target);
            if (result != NGGC_OK) {
                nggc_remap_worker_record_error(worker, result);
                break;
            }
            if (nggc_internal_none_object(header)) {
                result = nggc_remap_internal_object_locked(
                    worker->heap, header, &worker->scanned_slot_count,
                    &worker->rewritten_slot_count);
                if (result != NGGC_OK) {
                    nggc_remap_worker_record_error(worker, result);
                    break;
                }
                continue;
            }
            if (!nggc_type_id_valid(header->type_id) ||
                !worker->heap->types[header->type_id].registered) {
                nggc_remap_worker_record_error(
                    worker, NGGC_ERR_CORRUPTION);
                break;
            }
            type_slot = &worker->heap->types[header->type_id];
            if (type_slot->desc.scan || type_slot->desc.scan_ex) {
                nggc_remap_ctx remap;
                memset(&remap, 0, sizeof(remap));
                remap.heap = worker->heap;
                remap.owner = header;
                remap.result = NGGC_OK;
                remap.scanned_slot_count = &worker->scanned_slot_count;
                remap.rewritten_slot_count =
                    &worker->rewritten_slot_count;
                result = nggc_invoke_scan(
                    worker->heap, header, &type_slot->desc,
                    nggc_remap_visit, &remap);
                if (remap.result == NGGC_OK && result != NGGC_OK) {
                    remap.result = result;
                }
                if (remap.result != NGGC_OK) {
                    nggc_remap_worker_record_error(
                        worker, remap.result);
                    break;
                }
            }
        } else if (block.state != NGGC_OBJECT_FREE &&
                   block.state != NGGC_OBJECT_DECOMMITTED_HUMONGOUS) {
            nggc_remap_worker_record_error(worker, NGGC_ERR_CORRUPTION);
            break;
        }
    }
    atomic_fetch_sub_explicit(&worker->heap->remap_workers_active, 1u,
                              memory_order_acq_rel);
    atomic_fetch_add_explicit(&worker->completed_workers, 1u,
                              memory_order_release);
}

static void nggc_remap_worker_job_run(void *context) {
    nggc_remap_worker_run((nggc_remap_worker_ctx *)context);
}

static nggc_result nggc_remap_forwarded_slots_parallel_locked(
    nggc_heap *heap) {
    nggc_remap_worker_ctx worker;
    nggc_gc_worker_job job;
    uintptr_t begin;
    size_t span;
    size_t estimated_blocks;
    size_t desired_workers;
    size_t background_workers;
    size_t dispatched_workers = 0u;
    int background_drained = 1;
    nggc_result result;
    if (!heap || heap->evacuation_epoch == 0u ||
        atomic_load_explicit(&heap->remap_workers_active,
                             memory_order_acquire) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    begin = (uintptr_t)heap->gc_arena_base;
    span = (size_t)(heap->arena_cursor - begin);
    memset(&worker, 0, sizeof(worker));
    worker.heap = heap;
    worker.end_cursor = heap->arena_cursor;
    atomic_init(&worker.next_cursor, begin);
    atomic_init(&worker.entered_workers, 0u);
    atomic_init(&worker.completed_workers, 0u);
    atomic_init(&worker.result, NGGC_OK);
    atomic_init(&worker.block_count, 0u);
    atomic_init(&worker.scanned_slot_count, 0u);
    atomic_init(&worker.rewritten_slot_count, 0u);
    job.run = nggc_remap_worker_job_run;
    job.context = &worker;
    heap->remap_worker_epoch = heap->evacuation_epoch;
    heap->remap_workers_quiescent = 0u;

    result = nggc_remap_registered_roots_locked(
        heap, &worker.scanned_slot_count,
        &worker.rewritten_slot_count, 1);
    if (result != NGGC_OK) {
        heap->remap_workers_quiescent = 1u;
        heap->remap_scanned_slot_count = nggc_saturating_add_u64_size(
            heap->remap_scanned_slot_count,
            atomic_load_explicit(&worker.scanned_slot_count,
                                 memory_order_acquire));
        heap->remap_rewritten_slot_count = nggc_saturating_add_u64_size(
            heap->remap_rewritten_slot_count,
            atomic_load_explicit(&worker.rewritten_slot_count,
                                 memory_order_acquire));
        return result;
    }

    estimated_blocks = span / NGGC_MIN_FREE_BLOCK_SIZE;
    if (span != 0u && estimated_blocks == 0u) {
        estimated_blocks = 1u;
    }
    desired_workers = estimated_blocks < NGGC_EVACUATION_WORKER_LIMIT
                          ? estimated_blocks
                          : NGGC_EVACUATION_WORKER_LIMIT;
    if (desired_workers == 0u) {
        desired_workers = 1u;
    }
    background_workers = desired_workers > 1u
                             ? desired_workers - 1u
                             : 0u;
#if NGGC_HAVE_C11_THREADS
    if (background_workers != 0u) {
        size_t available_workers = nggc_gc_pool_ensure_locked(
            heap, background_workers, 0);
        dispatched_workers = available_workers < background_workers
                                 ? available_workers
                                 : background_workers;
        if (dispatched_workers != 0u) {
            result = nggc_gc_pool_dispatch_locked(
                heap, &job, dispatched_workers);
            if (result != NGGC_OK) {
                dispatched_workers = 0u;
                result = NGGC_OK;
            } else {
                heap->remap_worker_dispatch_count =
                    nggc_saturating_add_u64_size(
                        heap->remap_worker_dispatch_count,
                        dispatched_workers);
            }
        }
        if (dispatched_workers < background_workers &&
            heap->remap_worker_fallback_count != UINT64_MAX) {
            ++heap->remap_worker_fallback_count;
        }
    }
#else
    (void)job;
    if (background_workers != 0u &&
        heap->remap_worker_fallback_count != UINT64_MAX) {
        ++heap->remap_worker_fallback_count;
    }
#endif
    nggc_remap_worker_run(&worker);
#if NGGC_HAVE_C11_THREADS
    if (dispatched_workers != 0u) {
        nggc_result wait_result = nggc_gc_pool_wait_locked(
            heap, &job, dispatched_workers, &background_drained);
        if (wait_result != NGGC_OK) {
            result = wait_result;
        }
    }
#endif
    if (dispatched_workers != 0u &&
        heap->remap_parallel_cycle_count != UINT64_MAX) {
        ++heap->remap_parallel_cycle_count;
    }
    if (!background_drained ||
        atomic_load_explicit(&heap->remap_workers_active,
                             memory_order_acquire) != 0u ||
        atomic_load_explicit(&worker.entered_workers,
                             memory_order_acquire) !=
            atomic_load_explicit(&worker.completed_workers,
                                 memory_order_acquire) ||
        atomic_load_explicit(&worker.entered_workers,
                             memory_order_acquire) >
            dispatched_workers + 1u ||
        (result == NGGC_OK &&
         atomic_load_explicit(&worker.entered_workers,
                              memory_order_acquire) !=
             dispatched_workers + 1u)) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->remap_workers_quiescent = 1u;
    heap->remap_worker_block_count = nggc_saturating_add_u64_size(
        heap->remap_worker_block_count,
        atomic_load_explicit(&worker.block_count, memory_order_acquire));
    heap->remap_scanned_slot_count = nggc_saturating_add_u64_size(
        heap->remap_scanned_slot_count,
        atomic_load_explicit(&worker.scanned_slot_count,
                             memory_order_acquire));
    heap->remap_rewritten_slot_count = nggc_saturating_add_u64_size(
        heap->remap_rewritten_slot_count,
        atomic_load_explicit(&worker.rewritten_slot_count,
                             memory_order_acquire));
    if (result == NGGC_OK) {
        result = (nggc_result)atomic_load_explicit(
            &worker.result, memory_order_acquire);
    }
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_remap_phantom_reaper_slots_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_remap_fault_after_rewrite_locked(
        heap, atomic_load_explicit(&worker.rewritten_slot_count,
                                   memory_order_acquire) != 0u);
    if (result != NGGC_OK) {
        return result;
    }
    return nggc_finalize_remapped_strong_slots_locked(heap, 1);
}

static nggc_result nggc_remap_forwarded_slots_locked(nggc_heap *heap) {
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->collector_cycle_active &&
        heap->collector_cycle_kind ==
            NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) {
        return nggc_remap_forwarded_slots_parallel_locked(heap);
    }
    return nggc_remap_forwarded_slots_serial_locked(heap);
}

static int nggc_mark_stack_push(nggc_mark_stack *stack,
                                nggc_object_header *header) {
    nggc_object_header **items;
    size_t new_capacity;
    if (!stack || !header) {
        return 0;
    }
    if (stack->count == stack->capacity) {
        new_capacity = stack->capacity != 0 ? stack->capacity * 2u : 64u;
        if (new_capacity < stack->capacity ||
            new_capacity > SIZE_MAX / sizeof(*items)) {
            return 0;
        }
        items = (nggc_object_header **)realloc(
            stack->items, new_capacity * sizeof(*items));
        if (!items) {
            return 0;
        }
        stack->items = items;
        stack->capacity = new_capacity;
    }
    stack->items[stack->count++] = header;
    return 1;
}

static uint32_t nggc_reference_discovered_kind(
    const nggc_object_header *header) {
    if (nggc_internal_soft_reference_object(header)) {
        return NGGC_DISCOVERED_LIST_SOFT;
    }
    if (nggc_internal_weak_reference_object(header)) {
        return NGGC_DISCOVERED_LIST_WEAK;
    }
    if (nggc_internal_phantom_reference_object(header)) {
        return NGGC_DISCOVERED_LIST_PHANTOM;
    }
    return 0u;
}

static nggc_reference_discovered_list *
nggc_reference_discovered_list_for_kind_locked(
    nggc_heap *heap, uint32_t kind) {
    if (!heap) {
        return 0;
    }
    switch (kind) {
        case NGGC_DISCOVERED_LIST_SOFT:
            return &heap->reference_discovered_soft;
        case NGGC_DISCOVERED_LIST_WEAK:
            return &heap->reference_discovered_weak;
        case NGGC_DISCOVERED_LIST_PHANTOM:
            return &heap->reference_discovered_phantom;
        default:
            return 0;
    }
}

static nggc_result nggc_reference_discovered_fields_locked(
    nggc_object_header *header, uint32_t kind, void ***out_next,
    uint32_t **out_epoch) {
    if (!header || !out_next || !out_epoch) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_next = 0;
    *out_epoch = 0;
    if (kind == NGGC_DISCOVERED_LIST_SOFT) {
        nggc_internal_soft_reference *soft =
            (nggc_internal_soft_reference *)nggc_object_from_header(header);
        if (!nggc_internal_soft_reference_object(header) ||
            header->payload_size < sizeof(*soft)) {
            return NGGC_ERR_CORRUPTION;
        }
        *out_next = &soft->discovered_next;
        *out_epoch = &soft->discovered_epoch;
        return NGGC_OK;
    }
    if (kind == NGGC_DISCOVERED_LIST_WEAK) {
        nggc_internal_weak_reference *weak =
            (nggc_internal_weak_reference *)nggc_object_from_header(header);
        if (!nggc_internal_weak_reference_object(header) ||
            header->payload_size < sizeof(*weak)) {
            return NGGC_ERR_CORRUPTION;
        }
        *out_next = &weak->discovered_next;
        *out_epoch = &weak->discovered_epoch;
        return NGGC_OK;
    }
    if (kind == NGGC_DISCOVERED_LIST_PHANTOM) {
        nggc_internal_phantom_reference *phantom =
            (nggc_internal_phantom_reference *)nggc_object_from_header(header);
        if (!nggc_internal_phantom_reference_object(header) ||
            header->payload_size < sizeof(*phantom)) {
            return NGGC_ERR_CORRUPTION;
        }
        *out_next = &phantom->discovered_next;
        *out_epoch = &phantom->discovered_epoch;
        return NGGC_OK;
    }
    return NGGC_ERR_INVALID_ARGUMENT;
}

static nggc_result nggc_reference_discovered_begin_locked(
    nggc_heap *heap) {
    if (!heap || heap->mark_epoch == 0u ||
        heap->reference_discovered_state != NGGC_REFERENCE_DISCOVERY_IDLE ||
        heap->reference_discovered_epoch != 0u ||
        heap->reference_discovered_soft.head ||
        heap->reference_discovered_soft.tail ||
        heap->reference_discovered_soft.count != 0u ||
        heap->reference_discovered_weak.head ||
        heap->reference_discovered_weak.tail ||
        heap->reference_discovered_weak.count != 0u ||
        heap->reference_discovered_phantom.head ||
        heap->reference_discovered_phantom.tail ||
        heap->reference_discovered_phantom.count != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->reference_discovered_epoch = heap->mark_epoch;
    heap->reference_discovered_state = NGGC_REFERENCE_DISCOVERY_ACTIVE;
    return NGGC_OK;
}

static nggc_result nggc_reference_discover_marked_object_locked(
    nggc_heap *heap, nggc_object_header *header) {
    nggc_reference_discovered_list *list;
    void **next;
    uint32_t *discovered_epoch;
    uint32_t kind;
    nggc_result result;

    if (!heap || !header) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    kind = nggc_reference_discovered_kind(header);
    if (kind == 0u) {
        return NGGC_OK;
    }
    if (kind == NGGC_DISCOVERED_LIST_PHANTOM) {
        nggc_internal_phantom_reference *phantom =
            (nggc_internal_phantom_reference *)nggc_object_from_header(header);
        if (header->payload_size < sizeof(*phantom) ||
            phantom->enqueued > 1u ||
            (phantom->enqueued && phantom->referent)) {
            return NGGC_ERR_CORRUPTION;
        }
        if (phantom->enqueued == 1u) {
            /* Enqueued queue members are already retained and need no drain. */
            return NGGC_OK;
        }
    }
    if (heap->reference_discovered_state !=
            NGGC_REFERENCE_DISCOVERY_ACTIVE &&
        heap->reference_discovered_state !=
            NGGC_REFERENCE_DISCOVERY_DRAINING) {
        return NGGC_ERR_CORRUPTION;
    }
    if (header->mark_epoch != heap->mark_epoch ||
        heap->reference_discovered_epoch != heap->mark_epoch) {
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_reference_discovered_fields_locked(
        header, kind, &next, &discovered_epoch);
    if (result != NGGC_OK) {
        return result;
    }
    if (*discovered_epoch == heap->reference_discovered_epoch) {
        return NGGC_OK;
    }
    if (*discovered_epoch != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    list = nggc_reference_discovered_list_for_kind_locked(heap, kind);
    if (!list || list->count == SIZE_MAX) {
        return NGGC_ERR_CORRUPTION;
    }
    if (list->tail) {
        nggc_object_header *tail =
            nggc_live_header_from_object(heap, list->tail);
        void **tail_next;
        uint32_t *tail_epoch;
        if (!tail || nggc_reference_discovered_kind(tail) != kind ||
            nggc_reference_discovered_fields_locked(
                tail, kind, &tail_next, &tail_epoch) != NGGC_OK ||
            *tail_epoch != heap->reference_discovered_epoch ||
            *tail_next != 0) {
            return NGGC_ERR_CORRUPTION;
        }
        *tail_next = nggc_object_from_header(header);
    } else if (list->head) {
        return NGGC_ERR_CORRUPTION;
    } else {
        list->head = nggc_object_from_header(header);
    }
    *next = 0;
    *discovered_epoch = heap->reference_discovered_epoch;
    list->tail = nggc_object_from_header(header);
    ++list->count;
    return NGGC_OK;
}

static nggc_result nggc_reference_discovered_pop_locked(
    nggc_heap *heap, uint32_t kind, nggc_object_header **out_header) {
    nggc_reference_discovered_list *list;
    nggc_object_header *header;
    void **next;
    uint32_t *discovered_epoch;
    nggc_result result;

    if (!heap || !out_header ||
        heap->reference_discovered_state !=
            NGGC_REFERENCE_DISCOVERY_DRAINING ||
        heap->reference_discovered_epoch == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_header = 0;
    list = nggc_reference_discovered_list_for_kind_locked(heap, kind);
    if (!list || list->count == 0u) {
        return list && !list->head && !list->tail ? NGGC_OK
                                                   : NGGC_ERR_CORRUPTION;
    }
    if (list->count > heap->live_object_count || !list->head ||
        !list->tail) {
        return NGGC_ERR_CORRUPTION;
    }
    header = nggc_live_header_from_object(heap, list->head);
    if (!header || nggc_reference_discovered_kind(header) != kind ||
        header->mark_epoch != heap->mark_epoch) {
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_reference_discovered_fields_locked(
        header, kind, &next, &discovered_epoch);
    if (result != NGGC_OK ||
        *discovered_epoch != heap->reference_discovered_epoch) {
        return NGGC_ERR_CORRUPTION;
    }
    if (*next) {
        nggc_object_header *next_header =
            nggc_live_header_from_object(heap, *next);
        if (!next_header ||
            nggc_reference_discovered_kind(next_header) != kind) {
            return NGGC_ERR_CORRUPTION;
        }
        list->head = *next;
    } else {
        if (list->tail != nggc_object_from_header(header)) {
            return NGGC_ERR_CORRUPTION;
        }
        list->head = 0;
        list->tail = 0;
    }
    *next = 0;
    *discovered_epoch = 0u;
    --list->count;
    if (list->count == 0u) {
        if (list->head || list->tail) {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (!list->head || !list->tail) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_header = header;
    return NGGC_OK;
}

static nggc_result nggc_reference_discovered_finish_locked(
    nggc_heap *heap) {
    if (!heap || heap->reference_discovered_state !=
                         NGGC_REFERENCE_DISCOVERY_DRAINING ||
        heap->reference_discovered_soft.head ||
        heap->reference_discovered_soft.tail ||
        heap->reference_discovered_soft.count != 0u ||
        heap->reference_discovered_weak.head ||
        heap->reference_discovered_weak.tail ||
        heap->reference_discovered_weak.count != 0u ||
        heap->reference_discovered_phantom.head ||
        heap->reference_discovered_phantom.tail ||
        heap->reference_discovered_phantom.count != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->reference_discovered_epoch = 0u;
    heap->reference_discovered_state = NGGC_REFERENCE_DISCOVERY_IDLE;
    return NGGC_OK;
}

static nggc_result nggc_reference_discovered_clear_locked(
    nggc_heap *heap) {
    uintptr_t cursor;
    nggc_result result = NGGC_OK;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            result = NGGC_ERR_CORRUPTION;
            break;
        }
        if (block.state == NGGC_OBJECT_LIVE) {
            uint32_t kind = nggc_reference_discovered_kind(block.header);
            if (kind != 0u) {
                void **next;
                uint32_t *discovered_epoch;
                if (nggc_reference_discovered_fields_locked(
                        block.header, kind, &next,
                        &discovered_epoch) != NGGC_OK) {
                    result = NGGC_ERR_CORRUPTION;
                    break;
                }
                *next = 0;
                *discovered_epoch = 0u;
            }
        }
        cursor = block.end;
    }
    memset(&heap->reference_discovered_soft, 0,
           sizeof(heap->reference_discovered_soft));
    memset(&heap->reference_discovered_weak, 0,
           sizeof(heap->reference_discovered_weak));
    memset(&heap->reference_discovered_phantom, 0,
           sizeof(heap->reference_discovered_phantom));
    heap->reference_discovered_epoch = 0u;
    heap->reference_discovered_state = NGGC_REFERENCE_DISCOVERY_IDLE;
    return result;
}

static nggc_result nggc_verify_reference_discovered_list_locked(
    nggc_heap *heap, const nggc_reference_discovered_list *list,
    uint32_t kind) {
    void *current;
    if (!heap || !list || !nggc_reference_discovered_list_for_kind_locked(
                         heap, kind)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (list->count == 0u) {
        return (!list->head && !list->tail) ? NGGC_OK
                                             : NGGC_ERR_CORRUPTION;
    }
    if (heap->reference_discovered_state == NGGC_REFERENCE_DISCOVERY_IDLE ||
        heap->reference_discovered_epoch == 0u || !list->head ||
        !list->tail || list->count > heap->live_object_count) {
        return NGGC_ERR_CORRUPTION;
    }
    current = list->head;
    for (size_t i = 0u; i < list->count; ++i) {
        nggc_object_header *header =
            nggc_live_header_from_object(heap, current);
        void **next;
        uint32_t *discovered_epoch;
        if (!header || nggc_reference_discovered_kind(header) != kind ||
            header->mark_epoch != heap->mark_epoch ||
            nggc_reference_discovered_fields_locked(
                header, kind, &next, &discovered_epoch) != NGGC_OK ||
            *discovered_epoch != heap->reference_discovered_epoch) {
            return NGGC_ERR_CORRUPTION;
        }
        if (i + 1u == list->count) {
            if (current != list->tail || *next != 0) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (!*next) {
            return NGGC_ERR_CORRUPTION;
        }
        current = *next;
    }
    return NGGC_OK;
}

static nggc_result nggc_verify_reference_discovered_locked(
    nggc_heap *heap) {
    uintptr_t cursor;
    if (!heap ||
        (heap->reference_discovered_state != NGGC_REFERENCE_DISCOVERY_IDLE &&
         heap->reference_discovered_state != NGGC_REFERENCE_DISCOVERY_ACTIVE &&
         heap->reference_discovered_state !=
             NGGC_REFERENCE_DISCOVERY_DRAINING)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->reference_discovered_state == NGGC_REFERENCE_DISCOVERY_IDLE &&
        heap->reference_discovered_epoch != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->reference_discovered_state != NGGC_REFERENCE_DISCOVERY_IDLE &&
        heap->reference_discovered_epoch != heap->mark_epoch) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_verify_reference_discovered_list_locked(
            heap, &heap->reference_discovered_soft,
            NGGC_DISCOVERED_LIST_SOFT) != NGGC_OK ||
        nggc_verify_reference_discovered_list_locked(
            heap, &heap->reference_discovered_weak,
            NGGC_DISCOVERED_LIST_WEAK) != NGGC_OK ||
        nggc_verify_reference_discovered_list_locked(
            heap, &heap->reference_discovered_phantom,
            NGGC_DISCOVERED_LIST_PHANTOM) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->reference_discovered_state == NGGC_REFERENCE_DISCOVERY_IDLE) {
        cursor = (uintptr_t)heap->gc_arena_base;
        while (cursor < heap->arena_cursor) {
            nggc_block_view block;
            uint32_t kind;
            void **next;
            uint32_t *discovered_epoch;
            if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            kind = block.state == NGGC_OBJECT_LIVE
                       ? nggc_reference_discovered_kind(block.header)
                       : 0u;
            if (kind != 0u &&
                (nggc_reference_discovered_fields_locked(
                     block.header, kind, &next, &discovered_epoch) != NGGC_OK ||
                 *next != 0 || *discovered_epoch != 0u)) {
                return NGGC_ERR_CORRUPTION;
            }
            cursor = block.end;
        }
    }
    return NGGC_OK;
}

static void nggc_mark_object(nggc_mark_ctx *ctx, void *object) {
    nggc_object_header *header;
    if (!ctx || ctx->result != NGGC_OK || !object) {
        return;
    }
    header = nggc_live_header_from_object(ctx->heap, object);
    if (!header) {
        ctx->result = NGGC_ERR_CORRUPTION;
        return;
    }
    if (header->mark_epoch == ctx->heap->mark_epoch) {
        return;
    }
    header->mark_epoch = ctx->heap->mark_epoch;
    if (!nggc_mark_stack_push(ctx->stack, header)) {
        ctx->result = NGGC_ERR_OUT_OF_MEMORY;
        return;
    }
    if (nggc_reference_discover_marked_object_locked(ctx->heap, header) !=
        NGGC_OK) {
        ctx->result = NGGC_ERR_CORRUPTION;
    }
}

static void nggc_mark_visit(void *ctx, void **slot) {
    nggc_mark_ctx *mark = (nggc_mark_ctx *)ctx;
    if (!slot) {
        if (mark) {
            mark->result = NGGC_ERR_CORRUPTION;
        }
        return;
    }
    nggc_mark_object(mark, *slot);
}

static nggc_result nggc_young_mark_non_collection_implicit_live_locked(
    nggc_heap *heap) {
    uintptr_t cursor;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_LIVE &&
            heap->regions[region_index].in_collection_set == 0u) {
            if (!nggc_live_object_generation_valid(
                    block.header->generation)) {
                return NGGC_ERR_CORRUPTION;
            }
            block.header->mark_epoch = heap->mark_epoch;
            if (block.header->generation == NGGC_REGION_GENERATION_OLD &&
                heap->young_mark_old_object_count != UINT64_MAX) {
                ++heap->young_mark_old_object_count;
            }
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static void nggc_young_remset_mark_visit(void *ctx, void **slot) {
    nggc_young_remset_mark_ctx *young =
        (nggc_young_remset_mark_ctx *)ctx;
    uintptr_t slot_addr;
    size_t card_index;
    if (!young || !young->mark ||
        young->mark->result != NGGC_OK || !young->owner || !slot ||
        !nggc_slot_in_object(young->owner, slot)) {
        if (young && young->mark) {
            young->mark->result = NGGC_ERR_CORRUPTION;
        }
        return;
    }
    slot_addr = (uintptr_t)slot;
    if (slot_addr < (uintptr_t)young->mark->heap->gc_arena_base ||
        slot_addr >= young->mark->heap->arena_end) {
        young->mark->result = NGGC_ERR_CORRUPTION;
        return;
    }
    card_index = (size_t)((slot_addr -
                           (uintptr_t)young->mark->heap->gc_arena_base) /
                          young->mark->heap->card_size);
    if (card_index >= young->mark->heap->card_count) {
        young->mark->result = NGGC_ERR_CORRUPTION;
        return;
    }
    if (atomic_load_explicit(
            &young->mark->heap->card_table[card_index],
            memory_order_acquire) == 0u) {
        return;
    }
    if (young->mark->heap->young_mark_dirty_slot_count != UINT64_MAX) {
        ++young->mark->heap->young_mark_dirty_slot_count;
    }
    nggc_mark_object(young->mark, *slot);
}

static nggc_result nggc_young_mark_remembered_edges_locked(
    nggc_heap *heap, nggc_mark_ctx *mark_ctx) {
    uintptr_t cursor;
    nggc_young_remset_mark_ctx young;
    nggc_result scan_result;
    if (!heap || !mark_ctx) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    young.mark = mark_ctx;
    young.owner = 0;
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_LIVE &&
            heap->regions[region_index].in_collection_set == 0u &&
            !nggc_internal_none_object(header)) {
            nggc_type_slot *type;
            if (!nggc_type_id_valid(header->type_id) ||
                !heap->types[header->type_id].registered) {
                return NGGC_ERR_CORRUPTION;
            }
            type = &heap->types[header->type_id];
            if (type->desc.scan || type->desc.scan_ex) {
                young.owner = header;
                scan_result = nggc_invoke_scan(
                    heap, header, &type->desc,
                    nggc_young_remset_mark_visit, &young);
                if (mark_ctx->result == NGGC_OK && scan_result != NGGC_OK) {
                    mark_ctx->result = scan_result;
                }
                if (mark_ctx->result != NGGC_OK) {
                    return mark_ctx->result;
                }
            }
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static nggc_result nggc_advance_mark_epoch_locked(nggc_heap *heap) {
    uintptr_t cursor;
    nggc_result result;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_reference_discovered_clear_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    ++heap->mark_epoch;
    if (heap->mark_epoch == 0u) {
        heap->mark_epoch = 1u;
        cursor = (uintptr_t)heap->gc_arena_base;
        while (cursor < heap->arena_cursor) {
            nggc_block_view block;
            if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            if (block.header) {
                block.header->mark_epoch = 0u;
            }
            cursor = block.end;
        }
    }
    result = nggc_reference_discovered_begin_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    return NGGC_OK;
}

static nggc_result nggc_mark_roots_locked(nggc_heap *heap,
                                          nggc_mark_ctx *mark_ctx) {
    if (!heap || !mark_ctx) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < heap->root_count; ++i) {
        void **slot = heap->roots[i];
        if (!slot) {
            mark_ctx->result = NGGC_ERR_CORRUPTION;
            break;
        }
        nggc_mark_object(mark_ctx, *slot);
        if (mark_ctx->result != NGGC_OK) {
            break;
        }
    }
    for (nggc_thread *thread = heap->threads;
         thread && mark_ctx->result == NGGC_OK; thread = thread->next) {
        for (size_t i = 0; i < thread->root_count; ++i) {
            void **slot = thread->roots[i];
            if (!slot) {
                mark_ctx->result = NGGC_ERR_CORRUPTION;
                break;
            }
            nggc_mark_object(mark_ctx, *slot);
            if (mark_ctx->result != NGGC_OK) {
                break;
            }
        }
    }
    return mark_ctx->result;
}

static nggc_result nggc_drain_mark_stack_locked(nggc_mark_ctx *mark_ctx) {
    nggc_heap *heap;
    if (!mark_ctx || !mark_ctx->heap || !mark_ctx->stack) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    heap = mark_ctx->heap;
    while (mark_ctx->result == NGGC_OK && mark_ctx->stack->count != 0) {
        nggc_object_header *header =
            mark_ctx->stack->items[--mark_ctx->stack->count];
        nggc_type_slot *slot;
        if (nggc_internal_none_object(header)) {
            continue;
        }
        if (!nggc_type_id_valid(header->type_id) ||
            !heap->types[header->type_id].registered) {
            mark_ctx->result = NGGC_ERR_CORRUPTION;
            break;
        }
        slot = &heap->types[header->type_id];
        if (slot->desc.scan || slot->desc.scan_ex) {
            nggc_result scan_result = nggc_invoke_scan(
                heap, header, &slot->desc, nggc_mark_visit, mark_ctx);
            if (mark_ctx->result == NGGC_OK && scan_result != NGGC_OK) {
                mark_ctx->result = scan_result;
            }
        }
    }
    return mark_ctx->result;
}

static nggc_result nggc_drain_mark_stack_bounded_locked(
    nggc_mark_ctx *mark_ctx, size_t budget_items, size_t *out_processed) {
    nggc_heap *heap;
    size_t processed = 0;
    if (!mark_ctx || !mark_ctx->heap || !mark_ctx->stack ||
        budget_items == 0) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    heap = mark_ctx->heap;
    while (mark_ctx->result == NGGC_OK && mark_ctx->stack->count != 0 &&
           processed < budget_items) {
        nggc_object_header *header =
            mark_ctx->stack->items[--mark_ctx->stack->count];
        nggc_type_slot *slot;
        ++processed;
        if (nggc_internal_none_object(header)) {
            continue;
        }
        if (!nggc_type_id_valid(header->type_id) ||
            !heap->types[header->type_id].registered) {
            mark_ctx->result = NGGC_ERR_CORRUPTION;
            break;
        }
        slot = &heap->types[header->type_id];
        if (slot->desc.scan || slot->desc.scan_ex) {
            nggc_result scan_result = nggc_invoke_scan(
                heap, header, &slot->desc, nggc_mark_visit, mark_ctx);
            if (mark_ctx->result == NGGC_OK && scan_result != NGGC_OK) {
                mark_ctx->result = scan_result;
            }
        }
    }
    if (out_processed) {
        *out_processed = processed;
    }
    return mark_ctx->result;
}

static int nggc_soft_reference_should_retain_locked(const nggc_heap *heap) {
    size_t used;
    size_t retain_limit;
    if (!heap || heap->gc_arena_size == 0) {
        return 0;
    }
    if (heap->soft_reference_policy ==
        NGGC_SOFT_REFERENCE_POLICY_ALWAYS_RETAIN) {
        return 1;
    }
    if (heap->soft_reference_policy ==
        NGGC_SOFT_REFERENCE_POLICY_ALWAYS_CLEAR) {
        return 0;
    }
    used = (size_t)(heap->arena_cursor - (uintptr_t)heap->gc_arena_base);
    retain_limit =
        (heap->gc_arena_size / 100u) *
            heap->soft_reference_retain_percent +
        ((heap->gc_arena_size % 100u) *
         heap->soft_reference_retain_percent) /
            100u;
    return used <= retain_limit;
}

static nggc_result nggc_reference_process_soft_locked(
    nggc_heap *heap, nggc_mark_ctx *mark_ctx) {
    if (!heap || !mark_ctx) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    heap->reference_discovered_state = NGGC_REFERENCE_DISCOVERY_DRAINING;
    do {
        while (heap->reference_discovered_soft.count != 0u) {
            nggc_object_header *header;
            nggc_internal_soft_reference *soft;
            nggc_object_header *referent_header;
            nggc_result result = nggc_reference_discovered_pop_locked(
                heap, NGGC_DISCOVERED_LIST_SOFT, &header);
            if (result != NGGC_OK) {
                return result;
            }
            soft = (nggc_internal_soft_reference *)
                nggc_object_from_header(header);
            ++heap->soft_reference_discovered_count;
            if (!soft->referent) {
                continue;
            }
            referent_header = nggc_live_header_from_object(
                heap, soft->referent);
            if (!referent_header) {
                return NGGC_ERR_CORRUPTION;
            }
            if (referent_header->mark_epoch != heap->mark_epoch) {
                if (nggc_soft_reference_should_retain_locked(heap)) {
                    nggc_mark_object(mark_ctx, soft->referent);
                    if (mark_ctx->result != NGGC_OK) {
                        return mark_ctx->result;
                    }
                    ++heap->soft_reference_retained_count;
                } else {
                    soft->referent = 0;
                    ++heap->soft_reference_cleared_count;
                }
            }
        }
        if (nggc_drain_mark_stack_locked(mark_ctx) != NGGC_OK) {
            return mark_ctx->result;
        }
    } while (heap->reference_discovered_soft.count != 0u ||
             mark_ctx->stack->count != 0u);
    return NGGC_OK;
}

static nggc_result nggc_reference_process_weak_locked(nggc_heap *heap) {
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    while (heap->reference_discovered_weak.count != 0u) {
        nggc_object_header *header;
        nggc_internal_weak_reference *weak;
        nggc_object_header *referent_header;
        nggc_result result = nggc_reference_discovered_pop_locked(
            heap, NGGC_DISCOVERED_LIST_WEAK, &header);
        if (result != NGGC_OK) {
            return result;
        }
        weak = (nggc_internal_weak_reference *)nggc_object_from_header(header);
        ++heap->weak_reference_discovered_count;
        if (!weak->referent) {
            continue;
        }
        referent_header = nggc_live_header_from_object(heap, weak->referent);
        if (!referent_header) {
            return NGGC_ERR_CORRUPTION;
        }
        if (referent_header->mark_epoch != heap->mark_epoch) {
            weak->referent = 0;
            ++heap->weak_reference_cleared_count;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_mark_phantom_queue_locked(
    nggc_heap *heap,
    nggc_mark_ctx *mark_ctx,
    nggc_internal_phantom_queue *queue) {
    void *current;
    if (!heap || !mark_ctx || !queue) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    current = queue->head;
    for (size_t i = 0; i < queue->pending_count; ++i) {
        nggc_object_header *ref_header;
        nggc_internal_phantom_reference *phantom;
        if (!current || i >= heap->live_object_count) {
            return NGGC_ERR_CORRUPTION;
        }
        ref_header = nggc_live_header_from_object(heap, current);
        if (!nggc_internal_phantom_reference_object(ref_header) ||
            ref_header->payload_size < sizeof(*phantom)) {
            return NGGC_ERR_CORRUPTION;
        }
        phantom =
            (nggc_internal_phantom_reference *)nggc_object_from_header(
                ref_header);
        if (phantom->enqueued != 1u || phantom->referent ||
            phantom->queue != (const void *)queue) {
            return NGGC_ERR_CORRUPTION;
        }
        nggc_mark_object(mark_ctx, current);
        if (mark_ctx->result != NGGC_OK) {
            return mark_ctx->result;
        }
        if (i + 1u == queue->pending_count) {
            if (current != queue->tail || phantom->next != 0) {
                return NGGC_ERR_CORRUPTION;
            }
        }
        current = phantom->next;
    }
    if (queue->pending_count == 0u) {
        if (queue->head || queue->tail) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_validate_phantom_queue_locked(
    nggc_heap *heap,
    const nggc_internal_phantom_queue *queue) {
    void *current;
    if (!heap || !queue) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    current = queue->head;
    for (size_t i = 0; i < queue->pending_count; ++i) {
        nggc_object_header *ref_header;
        nggc_internal_phantom_reference *phantom;
        if (!current || i >= heap->live_object_count) {
            return NGGC_ERR_CORRUPTION;
        }
        ref_header = nggc_live_header_from_object(heap, current);
        if (!nggc_internal_phantom_reference_object(ref_header) ||
            ref_header->payload_size < sizeof(*phantom)) {
            return NGGC_ERR_CORRUPTION;
        }
        phantom =
            (nggc_internal_phantom_reference *)nggc_object_from_header(
                ref_header);
        if (phantom->enqueued != 1u || phantom->referent ||
            phantom->queue != (const void *)queue) {
            return NGGC_ERR_CORRUPTION;
        }
        if (i + 1u == queue->pending_count) {
            if (current != queue->tail || phantom->next != 0) {
                return NGGC_ERR_CORRUPTION;
            }
        }
        current = phantom->next;
    }
    if (queue->pending_count == 0u && (queue->head || queue->tail)) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_phantom_queue_from_object_locked(
    nggc_heap *heap, void *queue, nggc_object_header **out_header,
    nggc_internal_phantom_queue **out_queue) {
    nggc_object_header *header;
    nggc_internal_phantom_queue *phantom_queue;

    if (!heap || !queue || !out_header || !out_queue) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    header = nggc_live_header_from_object(heap, queue);
    if (!nggc_internal_phantom_queue_object(header) ||
        header->payload_size < sizeof(*phantom_queue)) {
        return NGGC_ERR_CORRUPTION;
    }
    phantom_queue =
        (nggc_internal_phantom_queue *)nggc_object_from_header(header);
    if (nggc_validate_phantom_queue_locked(heap, phantom_queue) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_header = header;
    *out_queue = phantom_queue;
    return NGGC_OK;
}

static void nggc_phantom_reaper_remove_slot_locked(nggc_heap *heap,
                                                   size_t index) {
    if (!heap || index >= heap->phantom_reaper_count) {
        return;
    }
    if (index + 1u < heap->phantom_reaper_count) {
        memmove(&heap->phantom_reaper_slots[index],
                &heap->phantom_reaper_slots[index + 1u],
                (heap->phantom_reaper_count - index - 1u) *
                    sizeof(*heap->phantom_reaper_slots));
    }
    --heap->phantom_reaper_count;
    nggc_phantom_reaper_notify_locked(heap);
}

static int nggc_phantom_reaper_slot_live_locked(
    nggc_heap *heap, size_t index,
    nggc_internal_phantom_queue **out_queue) {
    nggc_object_header *header;
    nggc_internal_phantom_queue *queue;
    nggc_result result;

    if (!heap || index >= heap->phantom_reaper_count) {
        return -1;
    }
    result = nggc_phantom_queue_from_object_locked(
        heap, heap->phantom_reaper_slots[index].queue, &header, &queue);
    if (result != NGGC_OK ||
        header->object_id != heap->phantom_reaper_slots[index].object_id) {
        nggc_phantom_reaper_remove_slot_locked(heap, index);
        if (heap->phantom_reaper_dropped_queue_count != UINT64_MAX) {
            ++heap->phantom_reaper_dropped_queue_count;
        }
        return 0;
    }
    if (out_queue) {
        *out_queue = queue;
    }
    return 1;
}

static nggc_result nggc_remap_phantom_reaper_slots_locked(
    nggc_heap *heap) {
    size_t i = 0u;

    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    while (i < heap->phantom_reaper_count) {
        nggc_phantom_reaper_slot *slot = &heap->phantom_reaper_slots[i];
        nggc_object_header *header =
            nggc_live_header_from_object(heap, slot->queue);
        nggc_object_header *target = 0;
        nggc_result result;

        /* Reaper entries are weak observations, never roots. */
        if (!header || header->object_id != slot->object_id ||
            header->mark_epoch != heap->mark_epoch ||
            !nggc_internal_phantom_queue_object(header) ||
            header->payload_size < sizeof(nggc_internal_phantom_queue)) {
            nggc_phantom_reaper_remove_slot_locked(heap, i);
            if (heap->phantom_reaper_dropped_queue_count != UINT64_MAX) {
                ++heap->phantom_reaper_dropped_queue_count;
            }
            continue;
        }
        result = nggc_forwarding_target_locked(heap, header, &target);
        if (result != NGGC_OK) {
            return result;
        }
        if (target) {
            if (target->object_id != slot->object_id ||
                !nggc_internal_phantom_queue_object(target) ||
                target->payload_size < sizeof(nggc_internal_phantom_queue)) {
                return NGGC_ERR_CORRUPTION;
            }
            slot->queue = nggc_object_from_header(target);
        }
        ++i;
    }
    return NGGC_OK;
}

static nggc_result nggc_reference_process_phantom_refs_locked(
    nggc_heap *heap, nggc_mark_ctx *mark_ctx) {
    if (!heap || !mark_ctx) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    while (heap->reference_discovered_phantom.count != 0u) {
        nggc_object_header *header;
        nggc_internal_phantom_reference *phantom;
        nggc_object_header *referent_header;
        nggc_object_header *queue_header;
        nggc_internal_phantom_queue *queue;
        nggc_result result = nggc_reference_discovered_pop_locked(
            heap, NGGC_DISCOVERED_LIST_PHANTOM, &header);
        if (result != NGGC_OK) {
            return result;
        }
        phantom = (nggc_internal_phantom_reference *)
            nggc_object_from_header(header);
        if (phantom->enqueued > 1u ||
            (!phantom->enqueued && phantom->next) ||
            (phantom->enqueued && phantom->referent)) {
            return NGGC_ERR_CORRUPTION;
        }
        queue_header = nggc_live_header_from_object(heap, phantom->queue);
        if (!nggc_internal_phantom_queue_object(queue_header) ||
            queue_header->payload_size < sizeof(*queue)) {
            return NGGC_ERR_CORRUPTION;
        }
        queue = (nggc_internal_phantom_queue *)
            nggc_object_from_header(queue_header);
        if (nggc_validate_phantom_queue_locked(heap, queue) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        nggc_mark_object(mark_ctx, phantom->queue);
        if (mark_ctx->result != NGGC_OK) {
            return mark_ctx->result;
        }
        ++heap->phantom_reference_discovered_count;
        if (!phantom->referent) {
            continue;
        }
        referent_header = nggc_live_header_from_object(heap, phantom->referent);
        if (!referent_header) {
            return NGGC_ERR_CORRUPTION;
        }
        if (referent_header->mark_epoch != heap->mark_epoch) {
            phantom->referent = 0;
            if (!phantom->enqueued) {
                if (queue->pending_count == SIZE_MAX) {
                    return NGGC_ERR_OUT_OF_MEMORY;
                }
                phantom->next = 0;
                if (queue->pending_count == 0u) {
                    if (queue->head || queue->tail) {
                        return NGGC_ERR_CORRUPTION;
                    }
                    queue->head = nggc_object_from_header(header);
                    queue->tail = nggc_object_from_header(header);
                } else {
                    nggc_object_header *tail_header =
                        nggc_live_header_from_object(heap, queue->tail);
                    nggc_internal_phantom_reference *tail;
                    if (!nggc_internal_phantom_reference_object(tail_header) ||
                        tail_header->payload_size < sizeof(*tail)) {
                        return NGGC_ERR_CORRUPTION;
                    }
                    tail = (nggc_internal_phantom_reference *)
                        nggc_object_from_header(tail_header);
                    if (!tail->enqueued || tail->next) {
                        return NGGC_ERR_CORRUPTION;
                    }
                    tail->next = nggc_object_from_header(header);
                    queue->tail = nggc_object_from_header(header);
                }
                phantom->enqueued = 1u;
                ++queue->pending_count;
                ++heap->phantom_reference_enqueued_count;
                nggc_phantom_reaper_notify_locked(heap);
            }
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_reference_process_phantom_queues_locked(
    nggc_heap *heap, nggc_mark_ctx *mark_ctx) {
    uintptr_t cursor;
    if (!heap || !mark_ctx) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        if (block.state == NGGC_OBJECT_LIVE &&
            nggc_internal_phantom_queue_object(header)) {
            nggc_internal_phantom_queue *queue =
                (nggc_internal_phantom_queue *)nggc_object_from_header(
                    header);
            if (header->payload_size < sizeof(*queue)) {
                return NGGC_ERR_CORRUPTION;
            }
            if (queue->pending_count != 0u) {
                nggc_result mark_result;
                nggc_mark_object(mark_ctx, nggc_object_from_header(header));
                if (mark_ctx->result != NGGC_OK) {
                    return mark_ctx->result;
                }
                if (heap->phantom_queue_promoted_count != UINT64_MAX) {
                    ++heap->phantom_queue_promoted_count;
                }
                mark_result =
                    nggc_mark_phantom_queue_locked(heap, mark_ctx, queue);
                if (mark_result != NGGC_OK) {
                    return mark_result;
                }
            } else if (queue->head || queue->tail) {
                return NGGC_ERR_CORRUPTION;
            }
        }
        cursor = block.end;
    }
    return nggc_drain_mark_stack_locked(mark_ctx);
}

static nggc_result nggc_reference_process_soft_one_locked(
    nggc_heap *heap, nggc_mark_ctx *mark_ctx) {
    nggc_object_header *header;
    nggc_internal_soft_reference *soft;
    nggc_object_header *referent_header;
    nggc_result result;

    if (!heap || !mark_ctx) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_reference_discovered_pop_locked(
        heap, NGGC_DISCOVERED_LIST_SOFT, &header);
    if (result != NGGC_OK) {
        return result;
    }
    soft = (nggc_internal_soft_reference *)nggc_object_from_header(header);
    if (heap->soft_reference_discovered_count != UINT64_MAX) {
        ++heap->soft_reference_discovered_count;
    }
    if (!soft->referent) {
        return NGGC_OK;
    }
    referent_header = nggc_live_header_from_object(heap, soft->referent);
    if (!referent_header) {
        return NGGC_ERR_CORRUPTION;
    }
    if (referent_header->mark_epoch != heap->mark_epoch) {
        if (nggc_soft_reference_should_retain_locked(heap)) {
            nggc_mark_object(mark_ctx, soft->referent);
            if (mark_ctx->result != NGGC_OK) {
                return mark_ctx->result;
            }
            if (heap->soft_reference_retained_count != UINT64_MAX) {
                ++heap->soft_reference_retained_count;
            }
        } else {
            soft->referent = 0;
            if (heap->soft_reference_cleared_count != UINT64_MAX) {
                ++heap->soft_reference_cleared_count;
            }
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_validate_phantom_queue_shallow_locked(
    nggc_heap *heap, const nggc_internal_phantom_queue *queue) {
    nggc_object_header *head_header;
    nggc_object_header *tail_header;
    nggc_internal_phantom_reference *head;
    nggc_internal_phantom_reference *tail;

    if (!heap || !queue) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (queue->pending_count == 0u) {
        return (!queue->head && !queue->tail) ? NGGC_OK
                                               : NGGC_ERR_CORRUPTION;
    }
    if (!queue->head || !queue->tail) {
        return NGGC_ERR_CORRUPTION;
    }
    head_header = nggc_live_header_from_object(heap, queue->head);
    tail_header = nggc_live_header_from_object(heap, queue->tail);
    if (!nggc_internal_phantom_reference_object(head_header) ||
        !nggc_internal_phantom_reference_object(tail_header) ||
        head_header->payload_size <
            sizeof(nggc_internal_phantom_reference) ||
        tail_header->payload_size <
            sizeof(nggc_internal_phantom_reference)) {
        return NGGC_ERR_CORRUPTION;
    }
    head = (nggc_internal_phantom_reference *)
        nggc_object_from_header(head_header);
    tail = (nggc_internal_phantom_reference *)
        nggc_object_from_header(tail_header);
    if (!head->enqueued || head->referent || tail->enqueued != 1u ||
        tail->referent || tail->next ||
        head->queue != (const void *)queue ||
        tail->queue != (const void *)queue) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_reference_process_weak_one_locked(
    nggc_heap *heap) {
    nggc_object_header *header;
    nggc_internal_weak_reference *weak;
    nggc_object_header *referent_header;
    nggc_result result;

    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_reference_discovered_pop_locked(
        heap, NGGC_DISCOVERED_LIST_WEAK, &header);
    if (result != NGGC_OK) {
        return result;
    }
    weak = (nggc_internal_weak_reference *)nggc_object_from_header(header);
    if (heap->weak_reference_discovered_count != UINT64_MAX) {
        ++heap->weak_reference_discovered_count;
    }
    if (!weak->referent) {
        return NGGC_OK;
    }
    referent_header = nggc_live_header_from_object(heap, weak->referent);
    if (!referent_header) {
        return NGGC_ERR_CORRUPTION;
    }
    if (referent_header->mark_epoch != heap->mark_epoch) {
        weak->referent = 0;
        if (heap->weak_reference_cleared_count != UINT64_MAX) {
            ++heap->weak_reference_cleared_count;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_reference_process_phantom_one_locked(
    nggc_heap *heap, nggc_mark_ctx *mark_ctx) {
    nggc_object_header *header;
    nggc_internal_phantom_reference *phantom;
    nggc_object_header *referent_header;
    nggc_object_header *queue_header;
    nggc_internal_phantom_queue *queue;
    nggc_result result;

    if (!heap || !mark_ctx) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_reference_discovered_pop_locked(
        heap, NGGC_DISCOVERED_LIST_PHANTOM, &header);
    if (result != NGGC_OK) {
        return result;
    }
    phantom = (nggc_internal_phantom_reference *)
        nggc_object_from_header(header);
    if (phantom->enqueued > 1u ||
        (!phantom->enqueued && phantom->next) ||
        (phantom->enqueued && phantom->referent)) {
        return NGGC_ERR_CORRUPTION;
    }
    queue_header = nggc_live_header_from_object(heap, phantom->queue);
    if (!nggc_internal_phantom_queue_object(queue_header) ||
        queue_header->payload_size < sizeof(*queue)) {
        return NGGC_ERR_CORRUPTION;
    }
    queue = (nggc_internal_phantom_queue *)
        nggc_object_from_header(queue_header);
    if (nggc_validate_phantom_queue_shallow_locked(heap, queue) !=
        NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    nggc_mark_object(mark_ctx, phantom->queue);
    if (mark_ctx->result != NGGC_OK) {
        return mark_ctx->result;
    }
    if (heap->phantom_reference_discovered_count != UINT64_MAX) {
        ++heap->phantom_reference_discovered_count;
    }
    if (!phantom->referent) {
        return NGGC_OK;
    }
    referent_header = nggc_live_header_from_object(heap, phantom->referent);
    if (!referent_header) {
        return NGGC_ERR_CORRUPTION;
    }
    if (referent_header->mark_epoch != heap->mark_epoch) {
        phantom->referent = 0;
        if (!phantom->enqueued) {
            if (queue->pending_count == SIZE_MAX) {
                return NGGC_ERR_OUT_OF_MEMORY;
            }
            phantom->next = 0;
            if (queue->pending_count == 0u) {
                if (queue->head || queue->tail) {
                    return NGGC_ERR_CORRUPTION;
                }
                queue->head = nggc_object_from_header(header);
                queue->tail = nggc_object_from_header(header);
            } else {
                nggc_object_header *tail_header =
                    nggc_live_header_from_object(heap, queue->tail);
                nggc_internal_phantom_reference *tail;
                if (!nggc_internal_phantom_reference_object(tail_header) ||
                    tail_header->payload_size < sizeof(*tail)) {
                    return NGGC_ERR_CORRUPTION;
                }
                tail = (nggc_internal_phantom_reference *)
                    nggc_object_from_header(tail_header);
                if (!tail->enqueued || tail->next) {
                    return NGGC_ERR_CORRUPTION;
                }
                tail->next = nggc_object_from_header(header);
                queue->tail = nggc_object_from_header(header);
            }
            phantom->enqueued = 1u;
            ++queue->pending_count;
            if (heap->phantom_reference_enqueued_count != UINT64_MAX) {
                ++heap->phantom_reference_enqueued_count;
            }
            nggc_phantom_reaper_notify_locked(heap);
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_assist_reference_queue_item_locked(
    nggc_heap *heap, nggc_mark_ctx *mark_ctx) {
    nggc_object_header *queue_header;
    nggc_internal_phantom_queue *queue;

    if (!heap || !mark_ctx) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->mutator_assist_reference_queue) {
        nggc_object_header *member_header;
        nggc_internal_phantom_reference *phantom;
        if (heap->mutator_assist_reference_queue_object_id == 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        queue_header = nggc_live_header_from_object(
            heap, heap->mutator_assist_reference_queue);
        if (!nggc_internal_phantom_queue_object(queue_header) ||
            queue_header->object_id !=
                heap->mutator_assist_reference_queue_object_id ||
            queue_header->payload_size < sizeof(*queue)) {
            return NGGC_ERR_CORRUPTION;
        }
        queue = (nggc_internal_phantom_queue *)
            nggc_object_from_header(queue_header);
        if (heap->mutator_assist_reference_queue_remaining == 0u ||
            heap->mutator_assist_reference_queue_remaining >
                queue->pending_count ||
            !heap->mutator_assist_reference_queue_member) {
            return NGGC_ERR_CORRUPTION;
        }
        member_header = nggc_live_header_from_object(
            heap, heap->mutator_assist_reference_queue_member);
        if (!nggc_internal_phantom_reference_object(member_header) ||
            member_header->payload_size < sizeof(*phantom)) {
            return NGGC_ERR_CORRUPTION;
        }
        phantom = (nggc_internal_phantom_reference *)
            nggc_object_from_header(member_header);
        if (!phantom->enqueued || phantom->referent ||
            phantom->queue != heap->mutator_assist_reference_queue) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->mutator_assist_reference_queue_remaining == 1u) {
            if (heap->mutator_assist_reference_queue_member != queue->tail ||
                phantom->next) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (!phantom->next) {
            return NGGC_ERR_CORRUPTION;
        }
        nggc_mark_object(mark_ctx,
                         heap->mutator_assist_reference_queue_member);
        if (mark_ctx->result != NGGC_OK) {
            return mark_ctx->result;
        }
        --heap->mutator_assist_reference_queue_remaining;
        heap->mutator_assist_reference_queue_member = phantom->next;
        if (heap->mutator_assist_reference_queue_remaining == 0u) {
            heap->mutator_assist_reference_queue = 0;
            heap->mutator_assist_reference_queue_object_id = 0u;
            heap->mutator_assist_reference_queue_member = 0;
        }
        return NGGC_OK;
    }

    if (heap->mutator_assist_reference_queue_scan_cursor >=
        heap->mutator_assist_reference_queue_scan_end) {
        return NGGC_OK;
    }
    {
        nggc_block_view block;
        uintptr_t cursor = heap->mutator_assist_reference_queue_scan_cursor;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK ||
            block.end <= cursor) {
            return NGGC_ERR_CORRUPTION;
        }
        heap->mutator_assist_reference_queue_scan_cursor = block.end;
        if (block.state != NGGC_OBJECT_LIVE ||
            !nggc_internal_phantom_queue_object(block.header)) {
            return NGGC_OK;
        }
        if (block.header->payload_size < sizeof(*queue)) {
            return NGGC_ERR_CORRUPTION;
        }
        queue = (nggc_internal_phantom_queue *)
            nggc_object_from_header(block.header);
        if (queue->pending_count == 0u) {
            if (queue->head || queue->tail) {
                return NGGC_ERR_CORRUPTION;
            }
            return NGGC_OK;
        }
        if (!queue->head || !queue->tail) {
            return NGGC_ERR_CORRUPTION;
        }
        heap->mutator_assist_reference_queue =
            nggc_object_from_header(block.header);
        heap->mutator_assist_reference_queue_object_id =
            block.header->object_id;
        heap->mutator_assist_reference_queue_member = queue->head;
        heap->mutator_assist_reference_queue_remaining =
            queue->pending_count;
        nggc_mark_object(mark_ctx,
                         nggc_object_from_header(block.header));
        if (mark_ctx->result != NGGC_OK) {
            return mark_ctx->result;
        }
    }
    return NGGC_OK;
}

static size_t nggc_reference_process_pending_items_locked(
    const nggc_heap *heap) {
    size_t pending = 0u;
    if (!heap || heap->mutator_assist_state !=
                     NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) {
        return 0u;
    }
    pending = nggc_saturating_add_size(
        pending, heap->mutator_assist_mark_stack.count);
    pending = nggc_saturating_add_size(
        pending, heap->reference_discovered_soft.count);
    pending = nggc_saturating_add_size(
        pending, heap->reference_discovered_weak.count);
    pending = nggc_saturating_add_size(
        pending, heap->reference_discovered_phantom.count);
    pending = nggc_saturating_add_size(
        pending, heap->mutator_assist_reference_queue_remaining);
    if (heap->mutator_assist_reference_queue_scan_cursor <
        heap->mutator_assist_reference_queue_scan_end) {
        pending = nggc_saturating_add_size(pending, 1u);
    }
    return pending;
}

static nggc_result nggc_reference_process_bounded_locked(
    nggc_heap *heap, nggc_mark_ctx *mark_ctx, size_t budget_items,
    size_t *out_processed, int *out_complete) {
    size_t processed = 0u;
    nggc_result result;

    if (!heap || !mark_ctx || budget_items == 0u || !out_complete ||
        (heap->reference_discovered_state !=
             NGGC_REFERENCE_DISCOVERY_ACTIVE &&
         heap->reference_discovered_state !=
             NGGC_REFERENCE_DISCOVERY_DRAINING)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->reference_discovered_state ==
        NGGC_REFERENCE_DISCOVERY_ACTIVE) {
        result = nggc_verify_reference_discovered_locked(heap);
        if (result != NGGC_OK) {
            return result;
        }
        heap->reference_discovered_state =
            NGGC_REFERENCE_DISCOVERY_DRAINING;
    }
    *out_complete = 0;
    while (processed < budget_items) {
        /* A mutator can publish a newly marked reference between packets. */
        if (heap->mutator_assist_reference_kind !=
                NGGC_ASSIST_REFERENCE_SOFT &&
            heap->reference_discovered_soft.count != 0u) {
            heap->mutator_assist_reference_kind =
                NGGC_ASSIST_REFERENCE_SOFT;
        } else if (heap->mutator_assist_reference_kind !=
                       NGGC_ASSIST_REFERENCE_SOFT &&
                   heap->mutator_assist_reference_kind !=
                       NGGC_ASSIST_REFERENCE_WEAK &&
                   heap->reference_discovered_weak.count != 0u) {
            heap->mutator_assist_reference_kind =
                NGGC_ASSIST_REFERENCE_WEAK;
        } else if (heap->mutator_assist_reference_kind ==
                       NGGC_ASSIST_REFERENCE_QUEUES &&
                   heap->reference_discovered_phantom.count != 0u) {
            heap->mutator_assist_reference_kind =
                NGGC_ASSIST_REFERENCE_PHANTOM;
        }
        if (heap->mutator_assist_reference_kind ==
                NGGC_ASSIST_REFERENCE_SOFT ||
            heap->mutator_assist_reference_kind ==
                NGGC_ASSIST_REFERENCE_PHANTOM ||
            heap->mutator_assist_reference_kind ==
                NGGC_ASSIST_REFERENCE_QUEUES) {
            if (heap->mutator_assist_mark_stack.count != 0u) {
                size_t drained = 0u;
                result = nggc_drain_mark_stack_bounded_locked(
                    mark_ctx, 1u, &drained);
                if (result != NGGC_OK) {
                    return result;
                }
                if (drained == 0u) {
                    return NGGC_ERR_CORRUPTION;
                }
                ++processed;
                continue;
            }
        } else if (heap->mutator_assist_mark_stack.count != 0u) {
            return NGGC_ERR_CORRUPTION;
        }

        if (heap->mutator_assist_reference_kind ==
            NGGC_ASSIST_REFERENCE_SOFT) {
            if (heap->reference_discovered_soft.count != 0u) {
                result = nggc_reference_process_soft_one_locked(
                    heap, mark_ctx);
                if (result != NGGC_OK) {
                    return result;
                }
                ++processed;
                continue;
            }
            heap->mutator_assist_reference_kind =
                NGGC_ASSIST_REFERENCE_WEAK;
            continue;
        }
        if (heap->mutator_assist_reference_kind ==
            NGGC_ASSIST_REFERENCE_WEAK) {
            if (heap->reference_discovered_weak.count != 0u) {
                result = nggc_reference_process_weak_one_locked(heap);
                if (result != NGGC_OK) {
                    return result;
                }
                ++processed;
                continue;
            }
            heap->mutator_assist_reference_kind =
                NGGC_ASSIST_REFERENCE_PHANTOM;
            continue;
        }
        if (heap->mutator_assist_reference_kind ==
            NGGC_ASSIST_REFERENCE_PHANTOM) {
            if (heap->reference_discovered_phantom.count != 0u) {
                result = nggc_reference_process_phantom_one_locked(
                    heap, mark_ctx);
                if (result != NGGC_OK) {
                    return result;
                }
                ++processed;
                continue;
            }
            heap->mutator_assist_reference_kind =
                NGGC_ASSIST_REFERENCE_QUEUES;
            heap->mutator_assist_reference_queue_scan_cursor =
                (uintptr_t)heap->gc_arena_base;
            heap->mutator_assist_reference_queue_scan_end =
                heap->arena_cursor;
            continue;
        }
        if (heap->mutator_assist_reference_kind ==
            NGGC_ASSIST_REFERENCE_QUEUES) {
            if (heap->arena_cursor >
                heap->mutator_assist_reference_queue_scan_end) {
                heap->mutator_assist_reference_queue_scan_end =
                    heap->arena_cursor;
            }
            if (heap->mutator_assist_reference_queue ||
                heap->mutator_assist_reference_queue_scan_cursor <
                    heap->mutator_assist_reference_queue_scan_end) {
                result = nggc_assist_reference_queue_item_locked(
                    heap, mark_ctx);
                if (result != NGGC_OK) {
                    return result;
                }
                ++processed;
                continue;
            }
            if (heap->mutator_assist_mark_stack.count != 0u) {
                continue;
            }
            result = nggc_reference_discovered_finish_locked(heap);
            if (result != NGGC_OK) {
                return result;
            }
            heap->mutator_assist_reference_kind = 0u;
            heap->mutator_assist_reference_queue_scan_cursor = 0u;
            heap->mutator_assist_reference_queue_scan_end = 0u;
            *out_complete = 1;
            break;
        }
        return NGGC_ERR_CORRUPTION;
    }
    if (out_processed) {
        *out_processed = processed;
    }
    return NGGC_OK;
}

#if NGGC_ENABLE_VERIFY
static nggc_result nggc_phantom_queue_membership_count_locked(
    nggc_heap *heap, nggc_internal_phantom_queue *queue, void *target,
    size_t *out_count) {
    void *current;
    size_t count = 0;
    if (!heap || !queue || !target || !out_count ||
        nggc_validate_phantom_queue_locked(heap, queue) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    current = queue->head;
    for (size_t i = 0; i < queue->pending_count; ++i) {
        nggc_object_header *header =
            nggc_live_header_from_object(heap, current);
        nggc_internal_phantom_reference *phantom;
        if (!nggc_internal_phantom_reference_object(header) ||
            header->payload_size < sizeof(*phantom)) {
            return NGGC_ERR_CORRUPTION;
        }
        phantom = (nggc_internal_phantom_reference *)
            nggc_object_from_header(header);
        if (current == target) {
            if (count == SIZE_MAX) {
                return NGGC_ERR_CORRUPTION;
            }
            ++count;
        }
        current = phantom->next;
    }
    *out_count = count;
    return NGGC_OK;
}

static nggc_result nggc_verify_reference_processing_locked(nggc_heap *heap) {
    uintptr_t cursor;
    int partial;
    if (!heap || heap->mark_epoch == 0u) {
        return heap ? NGGC_OK : NGGC_ERR_INVALID_ARGUMENT;
    }
    partial = heap->reference_discovered_state !=
              NGGC_REFERENCE_DISCOVERY_IDLE;
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        if (block.state == NGGC_OBJECT_LIVE &&
            nggc_internal_weak_reference_object(header)) {
            nggc_internal_weak_reference *weak =
                (nggc_internal_weak_reference *)
                    nggc_object_from_header(header);
            nggc_object_header *referent_header;
            if (header->payload_size < sizeof(*weak)) {
                return NGGC_ERR_CORRUPTION;
            }
            referent_header = weak->referent
                                  ? nggc_live_header_from_object(
                                        heap, weak->referent)
                                  : 0;
            if ((weak->referent && !referent_header) ||
                (!partial &&
                 (header->mark_epoch == heap->mark_epoch &&
                  referent_header &&
                  referent_header->mark_epoch != heap->mark_epoch))) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (block.state == NGGC_OBJECT_LIVE &&
                   nggc_internal_soft_reference_object(header)) {
            nggc_internal_soft_reference *soft =
                (nggc_internal_soft_reference *)
                    nggc_object_from_header(header);
            nggc_object_header *referent_header;
            if (header->payload_size < sizeof(*soft)) {
                return NGGC_ERR_CORRUPTION;
            }
            referent_header = soft->referent
                                  ? nggc_live_header_from_object(
                                        heap, soft->referent)
                                  : 0;
            if ((soft->referent && !referent_header) ||
                (!partial &&
                 (header->mark_epoch == heap->mark_epoch &&
                  referent_header &&
                  referent_header->mark_epoch != heap->mark_epoch))) {
                return NGGC_ERR_CORRUPTION;
            }
        } else if (block.state == NGGC_OBJECT_LIVE &&
                   nggc_internal_phantom_queue_object(header)) {
            nggc_internal_phantom_queue *queue =
                (nggc_internal_phantom_queue *)
                    nggc_object_from_header(header);
            void *queue_object = nggc_object_from_header(header);
            void *current;
            if (header->payload_size < sizeof(*queue) ||
                nggc_validate_phantom_queue_locked(heap, queue) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            if (!partial && queue->pending_count != 0u &&
                header->mark_epoch != heap->mark_epoch) {
                return NGGC_ERR_CORRUPTION;
            }
            current = queue->head;
            for (size_t i = 0; i < queue->pending_count; ++i) {
                nggc_object_header *ref_header =
                    nggc_live_header_from_object(heap, current);
                nggc_internal_phantom_reference *phantom;
                if (!nggc_internal_phantom_reference_object(ref_header) ||
                    ref_header->payload_size < sizeof(*phantom)) {
                    return NGGC_ERR_CORRUPTION;
                }
                phantom = (nggc_internal_phantom_reference *)
                    nggc_object_from_header(ref_header);
                if (!phantom->enqueued || phantom->referent ||
                    phantom->queue != queue_object ||
                    (!partial &&
                     ref_header->mark_epoch != heap->mark_epoch)) {
                    return NGGC_ERR_CORRUPTION;
                }
                current = phantom->next;
            }
        } else if (block.state == NGGC_OBJECT_LIVE &&
                   nggc_internal_phantom_reference_object(header)) {
            nggc_internal_phantom_reference *phantom =
                (nggc_internal_phantom_reference *)
                    nggc_object_from_header(header);
            nggc_object_header *queue_header;
            nggc_internal_phantom_queue *queue;
            nggc_object_header *referent_header;
            size_t membership_count = 0;
            if (header->payload_size < sizeof(*phantom) ||
                phantom->enqueued > 1u ||
                (!phantom->enqueued && phantom->next) ||
                (phantom->enqueued && phantom->referent)) {
                return NGGC_ERR_CORRUPTION;
            }
            queue_header =
                nggc_live_header_from_object(heap, phantom->queue);
            if (!nggc_internal_phantom_queue_object(queue_header) ||
                queue_header->payload_size < sizeof(*queue)) {
                return NGGC_ERR_CORRUPTION;
            }
            queue = (nggc_internal_phantom_queue *)
                nggc_object_from_header(queue_header);
            if (!partial && header->mark_epoch == heap->mark_epoch &&
                queue_header->mark_epoch != heap->mark_epoch) {
                return NGGC_ERR_CORRUPTION;
            }
            referent_header = phantom->referent
                                  ? nggc_live_header_from_object(
                                        heap, phantom->referent)
                                  : 0;
            if ((phantom->referent && !referent_header) ||
                (!partial &&
                 (header->mark_epoch == heap->mark_epoch &&
                  referent_header &&
                  referent_header->mark_epoch != heap->mark_epoch))) {
                return NGGC_ERR_CORRUPTION;
            }
            if (phantom->enqueued) {
                if (header->mark_epoch != heap->mark_epoch ||
                    queue_header->mark_epoch != heap->mark_epoch ||
                    nggc_phantom_queue_membership_count_locked(
                        heap, queue, nggc_object_from_header(header),
                        &membership_count) != NGGC_OK ||
                    membership_count != 1u) {
                    return NGGC_ERR_CORRUPTION;
                }
            }
        }
        cursor = block.end;
    }
    return NGGC_OK;
}
#endif

static nggc_result nggc_reference_process_locked(nggc_heap *heap,
                                                 nggc_mark_ctx *mark_ctx) {
    nggc_result result;
    if (!heap || !mark_ctx ||
        heap->reference_discovered_state !=
            NGGC_REFERENCE_DISCOVERY_ACTIVE) {
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_verify_reference_discovered_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_reference_process_soft_locked(heap, mark_ctx);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_reference_process_weak_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_reference_process_phantom_refs_locked(heap, mark_ctx);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_reference_process_phantom_queues_locked(heap, mark_ctx);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_reference_discovered_finish_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
#if NGGC_ENABLE_VERIFY
    if (mark_ctx->result != NGGC_OK || mark_ctx->stack->count != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    return nggc_verify_reference_processing_locked(heap);
#else
    return NGGC_OK;
#endif
}

static nggc_result nggc_reclaim_live_object_locked(
    nggc_heap *heap, nggc_object_header *header) {
    uint32_t object_flags;
    uint32_t pin_count;
    if (!heap || !header) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    pin_count = nggc_pin_count_load(header);
    if (pin_count != 0u &&
        (heap->pinned_object_count == 0u ||
         heap->pin_lease_count < (uint64_t)pin_count)) {
        return NGGC_ERR_CORRUPTION;
    }
    if (pin_count != 0u &&
        nggc_pin_lease_remove_object_locked(heap, header) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_region_reclaim_object_locked(heap, header) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    object_flags = header->flags;
    if (pin_count != 0u) {
        --heap->pinned_object_count;
        heap->pin_lease_count -= (uint64_t)pin_count;
    }
    if (heap->live_payload_bytes >= header->payload_size) {
        heap->live_payload_bytes -= header->payload_size;
    } else {
        heap->live_payload_bytes = 0;
    }
    if (heap->live_object_count != 0) {
        --heap->live_object_count;
    }
    if ((object_flags & NGGC_OBJECT_FLAG_LARGE) != 0u &&
        heap->large_object_count != 0u) {
        --heap->large_object_count;
    }
    if ((object_flags & NGGC_OBJECT_FLAG_PINNABLE) != 0u &&
        heap->pinnable_object_count != 0u) {
        --heap->pinnable_object_count;
    }
    if ((object_flags & NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u &&
        heap->non_movable_object_count != 0u) {
        --heap->non_movable_object_count;
    }
    header->type_id = NGGC_TYPE_NONE;
    header->size_class = 0u;
    header->flags = object_flags & NGGC_OBJECT_FLAG_LARGE;
    header->payload_size = 0;
    header->object_id = 0u;
    atomic_store_explicit(&header->pin_count, 0u, memory_order_release);
    header->mark_epoch = 0u;
    header->generation = NGGC_REGION_GENERATION_NONE;
    header->young_age = 0u;
    nggc_payload_lock_init(header);
    if (nggc_free_block_link_locked(heap, header) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    ++heap->reclaimed_object_count;
    atomic_store_explicit(&header->state, NGGC_OBJECT_FREE,
                          memory_order_release);
    return NGGC_OK;
}

static nggc_result nggc_collection_set_select_young_locked(
    nggc_heap *heap) {
    if (!heap || !heap->collector_cycle_active) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < heap->region_count; ++i) {
        nggc_region_meta *region = &heap->regions[i];
        if (region->in_collection_set != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        if (region->generation == NGGC_REGION_GENERATION_YOUNG &&
            region->tlab_state != NGGC_TLAB_INACTIVE) {
            return NGGC_ERR_BUSY;
        }
        if (region->generation == NGGC_REGION_GENERATION_YOUNG &&
            region->committed_bytes != 0u) {
            region->in_collection_set = 1u;
        }
    }
    return NGGC_OK;
}

static void nggc_collection_set_clear_locked(nggc_heap *heap) {
    if (!heap) {
        return;
    }
    for (size_t i = 0; i < heap->region_count; ++i) {
        heap->regions[i].in_collection_set = 0u;
    }
}

static nggc_result nggc_collector_cycle_enable_young_evacuation_locked(
    nggc_heap *heap) {
    nggc_result result;
    if (!heap || !heap->collector_cycle_active ||
        heap->collector_cycle_kind != NGGC_COLLECTOR_CYCLE_NON_MOVING ||
        heap->collector_expected_phase != NGGC_PHASE_SWEEP ||
        heap->gc_phase != NGGC_PHASE_IDLE) {
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_evacuation_epoch_begin_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    heap->collector_cycle_kind = NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION;
    heap->collector_expected_phase = NGGC_PHASE_EVACUATE;
    return nggc_verify_phase_state_locked(heap);
}

static nggc_result nggc_young_evacuation_simulate_alloc_locked(
    nggc_heap *heap, uintptr_t *cursor, size_t alloc_size,
    uint32_t generation) {
    if (!heap || !cursor || alloc_size < NGGC_OBJECT_HEADER_BYTES ||
        alloc_size > heap->region_size ||
        (alloc_size % NGGC_OBJECT_ALIGNMENT) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    while (*cursor < heap->arena_end) {
        size_t index = (size_t)((*cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region;
        size_t remaining;
        if (index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[index];
        remaining = (size_t)(region->end - *cursor);
        if (region->in_collection_set != 0u ||
            (region->generation != NGGC_REGION_GENERATION_NONE &&
             region->generation != generation) ||
            alloc_size > remaining) {
            *cursor = region->end;
            continue;
        }
        if (!nggc_region_range_generation_compatible_locked(
                heap, *cursor, alloc_size, generation) ||
            alloc_size > (size_t)(heap->arena_end - *cursor)) {
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        *cursor += alloc_size;
        return NGGC_OK;
    }
    return NGGC_ERR_OUT_OF_MEMORY;
}

static void nggc_young_evacuation_plan_dispose(
    nggc_young_evacuation_plan *plan) {
    if (!plan) {
        return;
    }
    free(plan->reclaim_queue);
    free(plan->tasks);
    free(plan->in_place_region_flags);
    memset(plan, 0, sizeof(*plan));
}

static nggc_result nggc_young_evacuation_plan_append_task(
    nggc_young_evacuation_plan *plan, nggc_object_header *source,
    uintptr_t destination_addr, uint32_t target_generation) {
    nggc_young_evacuation_task *task;
    if (!plan || !source || destination_addr == 0u ||
        (target_generation != NGGC_REGION_GENERATION_YOUNG &&
         target_generation != NGGC_REGION_GENERATION_OLD)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (plan->task_count == plan->task_capacity) {
        size_t new_capacity = plan->task_capacity == 0u
                                  ? 16u
                                  : plan->task_capacity * 2u;
        nggc_young_evacuation_task *new_tasks;
        if (new_capacity < plan->task_capacity ||
            new_capacity > SIZE_MAX / sizeof(*new_tasks)) {
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        new_tasks = (nggc_young_evacuation_task *)realloc(
            plan->tasks, new_capacity * sizeof(*new_tasks));
        if (!new_tasks) {
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        plan->tasks = new_tasks;
        plan->task_capacity = new_capacity;
    }
    task = &plan->tasks[plan->task_count++];
    memset(task, 0, sizeof(*task));
    task->source = source;
    task->destination = (nggc_object_header *)destination_addr;
    task->target_generation = target_generation;
    atomic_init(&task->state, NGGC_EVACUATION_TASK_PENDING);
    task->result = NGGC_OK;
    return NGGC_OK;
}

static int nggc_young_evacuation_region_in_place(
    const nggc_young_evacuation_plan *plan, size_t region_index) {
    return plan && plan->in_place_region_flags &&
           region_index < plan->in_place_region_count &&
           plan->in_place_region_flags[region_index] != 0u;
}

static nggc_result nggc_young_evacuation_find_in_place_regions_locked(
    nggc_heap *heap, nggc_young_evacuation_plan *plan) {
    uintptr_t cursor;
    if (!heap || !plan || heap->region_count == 0u ||
        heap->region_count > SIZE_MAX / sizeof(*plan->in_place_region_flags)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if ((heap->flags & NGGC_CONFIG_YOUNG_IN_PLACE_PROMOTION) == 0u) {
        return NGGC_OK;
    }
    plan->in_place_region_flags = (uint8_t *)calloc(
        heap->region_count, sizeof(*plan->in_place_region_flags));
    if (!plan->in_place_region_flags) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    plan->in_place_region_count = heap->region_count;
    for (size_t i = 0u; i < heap->region_count; ++i) {
        const nggc_region_meta *region = &heap->regions[i];
        if (region->in_collection_set != 0u &&
            region->generation == NGGC_REGION_GENERATION_YOUNG &&
            region->committed_bytes != 0u) {
            plan->in_place_region_flags[i] = 1u;
        }
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < plan->source_end) {
        nggc_block_view block;
        size_t region_index;
        uint8_t next_age;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_LIVE &&
            heap->regions[region_index].in_collection_set != 0u &&
            block.header->mark_epoch == heap->mark_epoch &&
            plan->in_place_region_flags[region_index] != 0u) {
            if (block.header->generation !=
                    NGGC_REGION_GENERATION_YOUNG ||
                block.header->young_age > heap->young_tenuring_threshold) {
                return NGGC_ERR_CORRUPTION;
            }
            if (nggc_pin_count_load(block.header) != 0u) {
                if ((block.header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u ||
                    nggc_forwarding_load_acquire(block.header) != 0 ||
                    nggc_forwarding_epoch_load_acquire(block.header) != 0u) {
                    return NGGC_ERR_CORRUPTION;
                }
                plan->in_place_region_flags[region_index] = 0u;
            } else if ((block.header->flags &
                        (NGGC_OBJECT_FLAG_LARGE |
                         NGGC_OBJECT_FLAG_NON_MOVABLE)) != 0u) {
                plan->in_place_region_flags[region_index] = 0u;
            } else {
                next_age = block.header->young_age <
                                    heap->young_tenuring_threshold
                                ? (uint8_t)(block.header->young_age + 1u)
                                : block.header->young_age;
                if (next_age < heap->young_tenuring_threshold) {
                    plan->in_place_region_flags[region_index] = 0u;
                } else if (plan->in_place_region_flags[region_index] == 1u) {
                    /* 2 means this region has at least one eligible live object. */
                    plan->in_place_region_flags[region_index] = 2u;
                }
            }
        }
        cursor = block.end;
    }
    for (size_t i = 0u; i < heap->region_count; ++i) {
        if (plan->in_place_region_flags[i] == 1u) {
            plan->in_place_region_flags[i] = 0u;
        } else if (plan->in_place_region_flags[i] == 2u) {
            plan->in_place_region_flags[i] = 1u;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_plan_group_locked(
    nggc_heap *heap, uintptr_t source_end, uintptr_t *destination_cursor,
    uint32_t target_generation, nggc_young_evacuation_plan *plan) {
    uintptr_t cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < source_end) {
        nggc_block_view block;
        size_t region_index;
        uint32_t object_generation;
        uint8_t next_age;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state != NGGC_OBJECT_LIVE ||
            heap->regions[region_index].in_collection_set == 0u ||
            block.header->mark_epoch != heap->mark_epoch) {
            cursor = block.end;
            continue;
        }
        if (nggc_young_evacuation_region_in_place(plan, region_index)) {
            cursor = block.end;
            continue;
        }
        if (nggc_pin_count_load(block.header) != 0u) {
            if ((block.header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u ||
                nggc_forwarding_load_acquire(block.header) != 0 ||
                nggc_forwarding_epoch_load_acquire(block.header) != 0u) {
                return NGGC_ERR_CORRUPTION;
            }
            cursor = block.end;
            continue;
        }
        if ((block.header->flags &
             (NGGC_OBJECT_FLAG_LARGE |
              NGGC_OBJECT_FLAG_NON_MOVABLE)) != 0u) {
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        if (block.header->generation != NGGC_REGION_GENERATION_YOUNG) {
            return NGGC_ERR_CORRUPTION;
        }
        next_age = block.header->young_age <
                           heap->young_tenuring_threshold
                       ? (uint8_t)(block.header->young_age + 1u)
                       : block.header->young_age;
        object_generation =
            next_age >= heap->young_tenuring_threshold
                ? NGGC_REGION_GENERATION_OLD
                : NGGC_REGION_GENERATION_YOUNG;
        if (object_generation == target_generation) {
            nggc_result result =
                nggc_young_evacuation_simulate_alloc_locked(
                    heap, destination_cursor, block.header->alloc_size,
                    target_generation);
            if (result != NGGC_OK) {
                return result;
            }
            result = nggc_young_evacuation_plan_append_task(
                plan, block.header,
                *destination_cursor - block.header->alloc_size,
                target_generation);
            if (result != NGGC_OK) {
                return result;
            }
            if (target_generation == NGGC_REGION_GENERATION_YOUNG) {
                plan->survivor_bytes = nggc_saturating_add_size(
                    plan->survivor_bytes, block.header->alloc_size);
                if (plan->survivor_objects != SIZE_MAX) {
                    ++plan->survivor_objects;
                }
            } else {
                plan->promotion_bytes = nggc_saturating_add_size(
                    plan->promotion_bytes, block.header->alloc_size);
                if (plan->promotion_objects != SIZE_MAX) {
                    ++plan->promotion_objects;
                }
            }
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_has_target_locked(
    nggc_heap *heap, const nggc_young_evacuation_plan *plan,
    uintptr_t source_end, uint32_t target_generation,
    int *out_has_target) {
    uintptr_t cursor = (uintptr_t)heap->gc_arena_base;
    if (!heap || !out_has_target) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_has_target = 0;
    while (cursor < source_end) {
        nggc_block_view block;
        size_t region_index;
        uint8_t next_age;
        uint32_t object_generation;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (block.state == NGGC_OBJECT_LIVE &&
            region_index < heap->region_count &&
            heap->regions[region_index].in_collection_set != 0u &&
            block.header->mark_epoch == heap->mark_epoch) {
            if (nggc_young_evacuation_region_in_place(plan, region_index)) {
                cursor = block.end;
                continue;
            }
            if (nggc_pin_count_load(block.header) != 0u) {
                if ((block.header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u ||
                    nggc_forwarding_load_acquire(block.header) != 0 ||
                    nggc_forwarding_epoch_load_acquire(block.header) != 0u) {
                    return NGGC_ERR_CORRUPTION;
                }
                cursor = block.end;
                continue;
            }
            next_age = block.header->young_age <
                               heap->young_tenuring_threshold
                           ? (uint8_t)(block.header->young_age + 1u)
                           : block.header->young_age;
            object_generation =
                next_age >= heap->young_tenuring_threshold
                    ? NGGC_REGION_GENERATION_OLD
                    : NGGC_REGION_GENERATION_YOUNG;
            if (object_generation == target_generation) {
                *out_has_target = 1;
                return NGGC_OK;
            }
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_plan_locked(
    nggc_heap *heap, nggc_young_evacuation_plan *plan) {
    uintptr_t destination_cursor;
    nggc_result result;
    if (!heap || !plan || heap->young_tenuring_threshold == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    memset(plan, 0, sizeof(*plan));
    plan->source_end = heap->arena_cursor;
    destination_cursor = heap->arena_cursor;
    result = nggc_young_evacuation_find_in_place_regions_locked(
        heap, plan);
    if (result != NGGC_OK) {
        nggc_young_evacuation_plan_dispose(plan);
        return result;
    }
    result = nggc_young_evacuation_plan_group_locked(
        heap, plan->source_end, &destination_cursor,
        NGGC_REGION_GENERATION_YOUNG, plan);
    if (result == NGGC_OK) {
        int has_promotions = 0;
        result = nggc_young_evacuation_has_target_locked(
            heap, plan, plan->source_end, NGGC_REGION_GENERATION_OLD,
            &has_promotions);
        if (result == NGGC_OK && has_promotions &&
            plan->survivor_objects != 0u &&
            destination_cursor < heap->arena_end &&
            (size_t)((destination_cursor -
                      (uintptr_t)heap->gc_arena_base) %
                     heap->region_size) != 0u) {
            size_t index = (size_t)((destination_cursor -
                                     (uintptr_t)heap->gc_arena_base) /
                                    heap->region_size);
            if (index >= heap->region_count) {
                return NGGC_ERR_CORRUPTION;
            }
            destination_cursor = heap->regions[index].end;
        }
    }
    if (result == NGGC_OK) {
        result = nggc_young_evacuation_plan_group_locked(
            heap, plan->source_end, &destination_cursor,
            NGGC_REGION_GENERATION_OLD, plan);
    }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (result == NGGC_OK && plan->task_count != 0u &&
        heap->test_collect_fault ==
            NGGC_TEST_COLLECT_ACTION_DUPLICATE_EVACUATION_TASK) {
        nggc_young_evacuation_task *canonical =
            &plan->tasks[plan->task_count - 1u];
        result = nggc_young_evacuation_simulate_alloc_locked(
            heap, &destination_cursor, canonical->source->alloc_size,
            canonical->target_generation);
        if (result == NGGC_OK) {
            result = nggc_young_evacuation_plan_append_task(
                plan, canonical->source,
                destination_cursor - canonical->source->alloc_size,
                canonical->target_generation);
        }
        if (result == NGGC_OK) {
            heap->test_collect_fault = 0u;
        }
    }
#endif
    plan->destination_end = destination_cursor;
    if (result == NGGC_OK && plan->task_count != 0u) {
        if (plan->task_count > SIZE_MAX / sizeof(*plan->reclaim_queue)) {
            result = NGGC_ERR_OUT_OF_MEMORY;
        } else {
            plan->reclaim_queue = (nggc_object_header **)calloc(
                plan->task_count, sizeof(*plan->reclaim_queue));
            if (!plan->reclaim_queue) {
                result = NGGC_ERR_OUT_OF_MEMORY;
            } else {
                plan->reclaim_capacity = plan->task_count;
            }
        }
    }
    if (result != NGGC_OK) {
        nggc_young_evacuation_plan_dispose(plan);
    }
    return result;
}

static nggc_result nggc_young_evacuation_publish_padding_locked(
    nggc_heap *heap, uintptr_t target_end, uint32_t target_generation) {
    while (heap->arena_cursor < target_end) {
        size_t index = (size_t)((heap->arena_cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        nggc_region_meta *region;
        size_t bytes;
        uint32_t generation;
        if (index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        region = &heap->regions[index];
        bytes = (size_t)(region->end - heap->arena_cursor);
        if (bytes == 0u || heap->arena_cursor + bytes > target_end) {
            return NGGC_ERR_CORRUPTION;
        }
        generation = region->generation != NGGC_REGION_GENERATION_NONE
                         ? region->generation
                         : target_generation;
        if (nggc_region_assign_generation_range_locked(
                heap, heap->arena_cursor, bytes, generation) != NGGC_OK ||
            nggc_region_publish_padding_locked(heap, bytes) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_discard_destination_locked(
    nggc_heap *heap, nggc_object_header *destination) {
    nggc_object_header *target;
    uint32_t state;
    if (!heap || !destination) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    state = atomic_load_explicit(&destination->state,
                                 memory_order_acquire);
    if ((state != NGGC_OBJECT_LIVE &&
         state != NGGC_OBJECT_ALLOCATING) ||
        nggc_forwarding_target_locked(heap, destination, &target) !=
            NGGC_OK ||
        target ||
        nggc_region_reclaim_object_locked(heap, destination) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    destination->type_id = NGGC_TYPE_NONE;
    destination->size_class = 0u;
    destination->flags = 0u;
    destination->payload_size = 0u;
    destination->object_id = 0u;
    atomic_store_explicit(&destination->pin_count, 0u,
                          memory_order_release);
    destination->mark_epoch = 0u;
    destination->generation = NGGC_REGION_GENERATION_NONE;
    destination->young_age = 0u;
    if (nggc_free_block_link_locked(heap, destination) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    nggc_payload_lock_init(destination);
    atomic_store_explicit(&destination->state, NGGC_OBJECT_FREE,
                          memory_order_release);
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_validate_forwarding_locked(
    nggc_heap *heap, nggc_object_header *source,
    uint32_t target_generation) {
    nggc_object_header *target;
    size_t target_index;
    uint8_t next_age;
    uint8_t target_age;
    if (!heap || !source ||
        source->mark_epoch != heap->mark_epoch ||
        source->generation != NGGC_REGION_GENERATION_YOUNG ||
        nggc_pin_count_load(source) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_forwarding_target_locked(heap, source, &target) != NGGC_OK ||
        !target) {
        return NGGC_ERR_CORRUPTION;
    }
    target_index = (size_t)(((uintptr_t)target -
                             (uintptr_t)heap->gc_arena_base) /
                            heap->region_size);
    next_age = source->young_age < heap->young_tenuring_threshold
                   ? (uint8_t)(source->young_age + 1u)
                   : source->young_age;
    target_age = target_generation == NGGC_REGION_GENERATION_OLD
                     ? 0u
                     : next_age;
    if (target_index >= heap->region_count ||
        heap->regions[target_index].in_collection_set != 0u ||
        heap->regions[target_index].generation != target_generation ||
        target->type_id != source->type_id ||
        target->size_class != source->size_class ||
        target->flags != source->flags ||
        target->payload_size != source->payload_size ||
        target->alloc_size != source->alloc_size ||
        target->object_id != source->object_id ||
        nggc_pin_count_load(target) != 0u ||
        target->mark_epoch != source->mark_epoch ||
        target->generation != target_generation ||
        target->young_age != target_age) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_reserve_tasks_locked(
    nggc_heap *heap, nggc_young_evacuation_plan *plan) {
    if (!heap || !plan || heap->evacuation_epoch == 0u ||
        heap->evacuation_worker_epoch != heap->evacuation_epoch) {
        return NGGC_ERR_CORRUPTION;
    }
    if (plan->reservations_active) {
        return heap->arena_cursor == plan->destination_end
                   ? NGGC_OK
                   : NGGC_ERR_CORRUPTION;
    }
    plan->reservations_active = 1u;
    heap->evacuation_copy_started = 1u;
    for (size_t i = 0u; i < plan->task_count; ++i) {
        nggc_young_evacuation_task *task = &plan->tasks[i];
        nggc_object_header *source = task->source;
        nggc_object_header *destination = task->destination;
        uintptr_t destination_addr = (uintptr_t)destination;
        uintptr_t destination_end;
        uint8_t next_age;
        nggc_result result;
        if (!source || !destination || destination_addr < heap->arena_cursor ||
            source->mark_epoch != heap->mark_epoch ||
            source->generation != NGGC_REGION_GENERATION_YOUNG ||
            nggc_pin_count_load(source) != 0u ||
            !nggc_range_end(destination_addr, source->alloc_size,
                            &destination_end) ||
            destination_end > plan->destination_end) {
            return NGGC_ERR_CORRUPTION;
        }
        result = nggc_young_evacuation_publish_padding_locked(
            heap, destination_addr, task->target_generation);
        if (result != NGGC_OK || heap->arena_cursor != destination_addr) {
            return result != NGGC_OK ? result : NGGC_ERR_CORRUPTION;
        }
        result = nggc_region_ensure_committed_range_locked(
            heap, destination_addr, source->alloc_size);
        if (result != NGGC_OK) {
            return result;
        }
        result = nggc_region_assign_generation_range_locked(
            heap, destination_addr, source->alloc_size,
            task->target_generation);
        if (result != NGGC_OK) {
            return result;
        }
        heap->arena_cursor = destination_end;
        if (nggc_region_publish_cursor_locked(heap, destination_end) !=
            NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        memset(destination, 0, NGGC_OBJECT_HEADER_BYTES);
        destination->magic = NGGC_OBJECT_MAGIC;
        atomic_init(&destination->state, NGGC_OBJECT_ALLOCATING);
        nggc_forwarding_init(destination);
        nggc_payload_lock_init(destination);
        destination->type_id = source->type_id;
        destination->size_class = source->size_class;
        destination->flags = source->flags;
        destination->payload_size = source->payload_size;
        destination->alloc_size = source->alloc_size;
        destination->object_id = source->object_id;
        destination->mark_epoch = source->mark_epoch;
        destination->generation = (uint8_t)task->target_generation;
        next_age = source->young_age < heap->young_tenuring_threshold
                       ? (uint8_t)(source->young_age + 1u)
                       : source->young_age;
        destination->young_age =
            task->target_generation == NGGC_REGION_GENERATION_OLD
                ? 0u
                : next_age;
        if (nggc_region_publish_object_locked(heap, destination, 0) !=
            NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        ++plan->reserved_task_count;
    }
    if (heap->arena_cursor != plan->destination_end) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static void nggc_evacuation_worker_peak_update(nggc_heap *heap,
                                                uint32_t active) {
    uint32_t peak = atomic_load_explicit(
        &heap->evacuation_workers_peak_active, memory_order_relaxed);
    while (peak < active &&
           !atomic_compare_exchange_weak_explicit(
               &heap->evacuation_workers_peak_active, &peak, active,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void nggc_young_evacuation_process_task(
    nggc_evacuation_worker_ctx *worker,
    nggc_young_evacuation_task *task, size_t task_index,
    int allow_retry_injection) {
    unsigned expected_state = NGGC_EVACUATION_TASK_PENDING;
    int cas_won = 0;
    void *published = 0;
    nggc_result result;
    if (!worker || !task) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &task->state, &expected_state,
            NGGC_EVACUATION_TASK_RUNNING,
            memory_order_acq_rel, memory_order_acquire)) {
        if (expected_state != NGGC_EVACUATION_TASK_RETRY ||
            !atomic_compare_exchange_strong_explicit(
                &task->state, &expected_state,
                NGGC_EVACUATION_TASK_RUNNING,
                memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
    if (allow_retry_injection &&
        task_index == worker->retry_task_index &&
        atomic_exchange_explicit(&worker->retry_injected, 1u,
                                 memory_order_acq_rel) == 0u) {
        task->result = NGGC_ERR_BACKEND;
        atomic_store_explicit(&task->state,
                              NGGC_EVACUATION_TASK_RETRY,
                              memory_order_release);
        return;
    }
    nggc_payload_lock_acquire(task->source);
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    int copy_pause = atomic_exchange_explicit(
        &worker->heap->test_concurrent_copy_pause, 0u,
        memory_order_acq_rel) != 0u;
    if (copy_pause) {
        atomic_store_explicit(&worker->heap->test_concurrent_copy_source,
                              nggc_object_from_header(task->source),
                              memory_order_release);
        atomic_store_explicit(&worker->heap->test_concurrent_copy_entered,
                              1u, memory_order_release);
        while (atomic_load_explicit(
                   &worker->heap->test_concurrent_copy_release,
                   memory_order_acquire) < 1u) {
#if NGGC_HAVE_C11_THREADS
            thrd_yield();
#else
            atomic_signal_fence(memory_order_seq_cst);
#endif
        }
    }
#endif
    memcpy(nggc_object_from_header(task->destination),
           nggc_object_from_header(task->source),
           task->source->payload_size);
    atomic_store_explicit(&task->destination->state, NGGC_OBJECT_LIVE,
                          memory_order_release);
    result = nggc_forwarding_publish_atomic(
        task->source, nggc_object_from_header(task->destination),
        worker->heap->evacuation_epoch, &cas_won, &published);
    task->result = result;
    task->published_forwarding = published;
    task->cas_won = cas_won ? 1u : 0u;
    task->cas_attempted = 1u;
    nggc_payload_lock_release(task->source);
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (copy_pause) {
        atomic_store_explicit(&worker->heap->test_concurrent_copy_entered,
                              2u, memory_order_release);
        while (atomic_load_explicit(
                   &worker->heap->test_concurrent_copy_release,
                   memory_order_acquire) < 2u) {
#if NGGC_HAVE_C11_THREADS
            thrd_yield();
#else
            atomic_signal_fence(memory_order_seq_cst);
#endif
        }
    }
#endif
    atomic_store_explicit(&task->state,
                          NGGC_EVACUATION_TASK_COMPLETE,
                          memory_order_release);
}

static void nggc_young_evacuation_worker_run(
    nggc_evacuation_worker_ctx *worker) {
    uint32_t active;
    if (!worker || !worker->heap || !worker->plan) {
        return;
    }
    active = atomic_fetch_add_explicit(
                 &worker->heap->evacuation_workers_active, 1u,
                 memory_order_acq_rel) +
             1u;
    nggc_evacuation_worker_peak_update(worker->heap, active);
    atomic_fetch_add_explicit(&worker->entered_workers, 1u,
                              memory_order_relaxed);
    for (;;) {
        size_t index = atomic_fetch_add_explicit(
            &worker->next_task, 1u, memory_order_relaxed);
        if (index >= worker->plan->task_count) {
            break;
        }
        nggc_young_evacuation_process_task(
            worker, &worker->plan->tasks[index], index, 1);
    }
    atomic_fetch_sub_explicit(&worker->heap->evacuation_workers_active, 1u,
                              memory_order_acq_rel);
    atomic_fetch_add_explicit(&worker->completed_workers, 1u,
                              memory_order_release);
}

static void nggc_young_evacuation_worker_job_run(void *context) {
    nggc_young_evacuation_worker_run(
        (nggc_evacuation_worker_ctx *)context);
}

#if NGGC_HAVE_C11_THREADS
static void nggc_gc_pool_worker_mark_failed(
    nggc_gc_pool_worker *slot) {
    if (slot) {
        atomic_store_explicit(&slot->failed, 1u,
                              memory_order_release);
    }
}

static void nggc_gc_pool_refresh_failures_locked(nggc_heap *heap) {
    if (!heap) {
        return;
    }
    for (size_t i = 0u; i < heap->evacuation_worker_pool_size; ++i) {
        if (atomic_load_explicit(&heap->gc_pool_workers[i].failed,
                                 memory_order_acquire) != 0u) {
            heap->gc_pool_broken = 1u;
            break;
        }
    }
}

static void nggc_gc_pool_reconcile_failures_locked(nggc_heap *heap) {
    if (!heap) {
        return;
    }
    nggc_gc_pool_refresh_failures_locked(heap);
    if (!heap->gc_pool_job) {
        return;
    }
    for (size_t i = 0u; i < heap->gc_pool_requested_workers; ++i) {
        nggc_gc_pool_worker *slot = &heap->gc_pool_workers[i];
        if (atomic_load_explicit(&slot->failed, memory_order_acquire) == 0u ||
            slot->completed_dispatch_epoch == heap->gc_pool_dispatch_epoch) {
            continue;
        }
        /* Retire each selected slot once, even if it failed after completion. */
        slot->completed_dispatch_epoch = heap->gc_pool_dispatch_epoch;
        if (slot->running != 0u) {
            slot->running = 0u;
            if (heap->gc_pool_running_workers != 0u) {
                --heap->gc_pool_running_workers;
            } else {
                heap->gc_pool_broken = 1u;
            }
        }
        if (heap->gc_pool_completed_workers <
            heap->gc_pool_requested_workers) {
            ++heap->gc_pool_completed_workers;
        } else {
            heap->gc_pool_broken = 1u;
        }
    }
}

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
static int nggc_gc_pool_worker_test_fault(
    nggc_gc_pool_worker *slot, unsigned int fault) {
    unsigned int expected = fault;
    if (slot->index != 0u) {
        return 0;
    }
    if (atomic_compare_exchange_strong_explicit(
            &slot->heap->test_gc_pool_worker_fault, &expected, 0u,
            memory_order_acq_rel, memory_order_acquire)) {
        return 1;
    }
    if (slot->heap->gc_phase != NGGC_PHASE_REMAP) {
        return 0;
    }
    switch (fault) {
        case NGGC_TEST_GC_POOL_FAULT_WORKER_BEFORE_JOB:
            expected = NGGC_TEST_GC_POOL_FAULT_REMAP_BEFORE_JOB;
            break;
        case NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_JOB:
            expected = NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_JOB;
            break;
        case NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_COMPLETE:
            expected = NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_COMPLETE;
            break;
        default:
            return 0;
    }
    return atomic_compare_exchange_strong_explicit(
        &slot->heap->test_gc_pool_worker_fault, &expected, 0u,
        memory_order_acq_rel, memory_order_acquire);
}
#endif

static int nggc_gc_pool_wait_condition(
    nggc_heap *heap, cnd_t *condition, mtx_t *lock, int timed,
    int coordinator) {
    (void)heap;
    (void)coordinator;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (coordinator && heap && atomic_exchange_explicit(
                    &heap->test_gc_pool_fail_next_wait, 0u,
                    memory_order_acq_rel) != 0u) {
        return thrd_error;
    }
#endif
    if (!timed) {
        return cnd_wait(condition, lock);
    }
    {
        struct timespec deadline;
        if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
            /* Keep polling terminal failures if a completion wakeup is lost. */
            return thrd_error;
        }
        deadline.tv_nsec += 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            ++deadline.tv_sec;
        }
        return cnd_timedwait(condition, lock, &deadline);
    }
}

static int nggc_gc_pool_broadcast(
    nggc_heap *heap, cnd_t *condition) {
    (void)heap;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (heap && atomic_exchange_explicit(
                    &heap->test_gc_pool_fail_next_broadcast, 0u,
                    memory_order_acq_rel) != 0u) {
        return thrd_error;
    }
#endif
    return cnd_broadcast(condition);
}

static int nggc_gc_pool_join(
    nggc_heap *heap, thrd_t thread, int *thread_result) {
    (void)heap;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (heap && atomic_exchange_explicit(
                    &heap->test_gc_pool_fail_next_join, 0u,
                    memory_order_acq_rel) != 0u) {
        return thrd_error;
    }
#endif
    return thrd_join(thread, thread_result);
}

static int nggc_gc_pool_worker_main(void *arg) {
    nggc_gc_pool_worker *slot = (nggc_gc_pool_worker *)arg;
    nggc_heap *heap;
    if (!slot || !slot->heap) {
        return -1;
    }
    heap = slot->heap;
    if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
        nggc_gc_pool_worker_mark_failed(slot);
        return -1;
    }
    for (;;) {
        nggc_gc_worker_job *job;
        uint64_t dispatch_epoch;
        while (!heap->gc_pool_shutdown &&
               heap->gc_pool_dispatch_epoch ==
                   slot->observed_dispatch_epoch) {
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
            if (nggc_gc_pool_worker_test_fault(
                    slot, NGGC_TEST_GC_POOL_FAULT_WORKER_WAIT)) {
                goto failed_locked;
            }
#endif
            if (nggc_gc_pool_wait_condition(
                    heap, &heap->gc_pool_work_condition,
                    &heap->gc_pool_lock, 0, 0) != thrd_success) {
                goto failed_locked;
            }
        }
        if (heap->gc_pool_shutdown) {
            break;
        }
        dispatch_epoch = heap->gc_pool_dispatch_epoch;
        slot->observed_dispatch_epoch = dispatch_epoch;
        if (slot->index >= heap->gc_pool_requested_workers) {
            continue;
        }
        job = heap->gc_pool_job;
        if (!job || !job->run || !job->context) {
            goto failed_locked;
        }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        if (nggc_gc_pool_worker_test_fault(
                slot, NGGC_TEST_GC_POOL_FAULT_WORKER_BEFORE_JOB)) {
            goto failed_locked;
        }
#endif
        if (slot->running ||
            heap->gc_pool_running_workers >=
                heap->gc_pool_requested_workers) {
            goto failed_locked;
        }
        ++heap->gc_pool_running_workers;
        slot->running = 1u;
        if (mtx_unlock(&heap->gc_pool_lock) != thrd_success) {
            nggc_gc_pool_worker_mark_failed(slot);
            return -1;
        }

        job->run(job->context);

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        if (nggc_gc_pool_worker_test_fault(
                slot, NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_JOB)) {
            nggc_gc_pool_worker_mark_failed(slot);
            return -1;
        }
        /* Ensure the injected coordinator wait is actually reached. */
        while (atomic_load_explicit(&heap->test_gc_pool_fail_next_wait,
                                    memory_order_acquire) != 0u) {
            thrd_yield();
        }
#endif
        if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
            nggc_gc_pool_worker_mark_failed(slot);
            return -1;
        }
        if (heap->gc_pool_job != job ||
            heap->gc_pool_dispatch_epoch != dispatch_epoch ||
            heap->gc_pool_completed_workers >=
                heap->gc_pool_requested_workers ||
            heap->gc_pool_running_workers == 0u || slot->running == 0u ||
            slot->completed_dispatch_epoch == dispatch_epoch) {
            goto failed_locked;
        }
        --heap->gc_pool_running_workers;
        slot->running = 0u;
        slot->completed_dispatch_epoch = dispatch_epoch;
        ++heap->gc_pool_completed_workers;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        if (nggc_gc_pool_worker_test_fault(
                slot, NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_COMPLETE)) {
            goto failed_locked;
        }
#endif
        if (nggc_gc_pool_broadcast(
                heap, &heap->gc_pool_done_condition) != thrd_success) {
            heap->gc_pool_broken = 1u;
        }
    }
    if (mtx_unlock(&heap->gc_pool_lock) != thrd_success) {
        nggc_gc_pool_worker_mark_failed(slot);
        return -1;
    }
    return 0;

failed_locked:
    heap->gc_pool_broken = 1u;
    nggc_gc_pool_worker_mark_failed(slot);
    (void)nggc_gc_pool_broadcast(heap, &heap->gc_pool_done_condition);
    (void)mtx_unlock(&heap->gc_pool_lock);
    return -1;
}

static nggc_result nggc_gc_pool_init(nggc_heap *heap) {
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (mtx_init(&heap->gc_pool_lock, mtx_plain) !=
        thrd_success) {
        return NGGC_ERR_BACKEND;
    }
    if (cnd_init(&heap->gc_pool_work_condition) !=
        thrd_success) {
        mtx_destroy(&heap->gc_pool_lock);
        return NGGC_ERR_BACKEND;
    }
    if (cnd_init(&heap->gc_pool_done_condition) !=
        thrd_success) {
        cnd_destroy(&heap->gc_pool_work_condition);
        mtx_destroy(&heap->gc_pool_lock);
        return NGGC_ERR_BACKEND;
    }
    heap->gc_pool_initialized = 1u;
    return NGGC_OK;
}

static nggc_result nggc_gc_pool_destroy(nggc_heap *heap) {
    nggc_result result = NGGC_OK;
    size_t worker_count;
    if (!heap || !heap->gc_pool_initialized) {
        return heap ? NGGC_OK : NGGC_ERR_INVALID_ARGUMENT;
    }
    if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
        return NGGC_ERR_BACKEND;
    }
    worker_count = heap->evacuation_worker_pool_size;
    if (heap->gc_pool_job ||
        heap->gc_pool_requested_workers != 0u ||
        heap->gc_pool_completed_workers != 0u ||
        heap->gc_pool_running_workers != 0u) {
        (void)mtx_unlock(&heap->gc_pool_lock);
        return NGGC_ERR_BUSY;
    }
    heap->gc_pool_shutdown = 1u;
    if (nggc_gc_pool_broadcast(
            heap, &heap->gc_pool_work_condition) != thrd_success) {
        (void)mtx_unlock(&heap->gc_pool_lock);
        return NGGC_ERR_BACKEND;
    }
    if (mtx_unlock(&heap->gc_pool_lock) != thrd_success) {
        return NGGC_ERR_BACKEND;
    }
    for (size_t i = 0u; i < worker_count; ++i) {
        int thread_result = 0;
        if (!heap->gc_pool_workers[i].created) {
            continue;
        }
        if (nggc_gc_pool_join(heap, heap->gc_pool_workers[i].thread,
                              &thread_result) != thrd_success) {
            return NGGC_ERR_BACKEND;
        }
        heap->gc_pool_workers[i].created = 0u;
        if (thread_result != 0) {
            return NGGC_ERR_BACKEND;
        }
    }
    cnd_destroy(&heap->gc_pool_done_condition);
    cnd_destroy(&heap->gc_pool_work_condition);
    mtx_destroy(&heap->gc_pool_lock);
    heap->evacuation_worker_pool_size = 0u;
    heap->gc_pool_initialized = 0u;
    heap->gc_pool_shutdown = 0u;
    heap->gc_pool_broken = 0u;
    return result;
}

static size_t nggc_gc_pool_ensure_locked(
    nggc_heap *heap, size_t desired_workers,
    int fail_next_create) {
    size_t available_workers;
    if (!heap || !heap->gc_pool_initialized ||
        desired_workers == 0u) {
        return 0u;
    }
    if (desired_workers > NGGC_EVACUATION_BACKGROUND_WORKER_LIMIT) {
        desired_workers = NGGC_EVACUATION_BACKGROUND_WORKER_LIMIT;
    }
    if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
        return 0u;
    }
    nggc_gc_pool_refresh_failures_locked(heap);
    if (heap->gc_pool_shutdown || heap->gc_pool_broken ||
        heap->gc_pool_job || heap->gc_pool_requested_workers != 0u ||
        heap->gc_pool_completed_workers != 0u) {
        available_workers = 0u;
    } else {
        while (heap->evacuation_worker_pool_size < desired_workers) {
            size_t index = heap->evacuation_worker_pool_size;
            nggc_gc_pool_worker *slot = &heap->gc_pool_workers[index];
            memset(slot, 0, sizeof(*slot));
            slot->heap = heap;
            slot->index = index;
            slot->observed_dispatch_epoch =
                heap->gc_pool_dispatch_epoch;
            atomic_init(&slot->failed, 0u);
            if (fail_next_create ||
                thrd_create(&slot->thread,
                            nggc_gc_pool_worker_main,
                            slot) != thrd_success) {
                fail_next_create = 0;
                memset(slot, 0, sizeof(*slot));
                if (heap->evacuation_worker_create_failure_count !=
                    UINT64_MAX) {
                    ++heap->evacuation_worker_create_failure_count;
                }
                break;
            }
            slot->created = 1u;
            ++heap->evacuation_worker_pool_size;
            if (heap->evacuation_worker_created_count != UINT64_MAX) {
                ++heap->evacuation_worker_created_count;
            }
        }
        available_workers = heap->evacuation_worker_pool_size;
    }
    if (mtx_unlock(&heap->gc_pool_lock) != thrd_success) {
        return 0u;
    }
    return available_workers;
}

static nggc_result nggc_gc_pool_dispatch_locked(
    nggc_heap *heap, nggc_gc_worker_job *job,
    size_t requested_workers) {
    uint64_t next_epoch;
    if (!heap || !job || !job->run || !job->context ||
        requested_workers == 0u ||
        requested_workers > heap->evacuation_worker_pool_size) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
        return NGGC_ERR_BACKEND;
    }
    nggc_gc_pool_refresh_failures_locked(heap);
    if (heap->gc_pool_shutdown || heap->gc_pool_broken ||
        heap->gc_pool_job || heap->gc_pool_requested_workers != 0u ||
        heap->gc_pool_completed_workers != 0u) {
        (void)mtx_unlock(&heap->gc_pool_lock);
        return NGGC_ERR_CORRUPTION;
    }
    next_epoch = heap->gc_pool_dispatch_epoch + UINT64_C(1);
    if (next_epoch == 0u) {
        next_epoch = UINT64_C(1);
    }
    heap->gc_pool_job = job;
    heap->gc_pool_requested_workers = requested_workers;
    heap->gc_pool_dispatch_epoch = next_epoch;
    for (size_t i = 0u; i < requested_workers; ++i) {
        heap->gc_pool_workers[i].completed_dispatch_epoch = 0u;
    }
    if (nggc_gc_pool_broadcast(
            heap, &heap->gc_pool_work_condition) !=
        thrd_success) {
        heap->gc_pool_broken = 1u;
        heap->gc_pool_job = 0;
        heap->gc_pool_requested_workers = 0u;
        (void)mtx_unlock(&heap->gc_pool_lock);
        return NGGC_ERR_BACKEND;
    }
    return mtx_unlock(&heap->gc_pool_lock) == thrd_success
               ? NGGC_OK
               : NGGC_ERR_BACKEND;
}

static nggc_result nggc_gc_pool_wait_locked(
    nggc_heap *heap, nggc_gc_worker_job *job,
    size_t requested_workers, int *out_drained) {
    nggc_result result = NGGC_OK;
    int condition_wait_failed = 0;
    if (!out_drained) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_drained = 0;
    if (!heap || !job || requested_workers == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
        return NGGC_ERR_BACKEND;
    }
    for (;;) {
        int wait_result;
        nggc_gc_pool_reconcile_failures_locked(heap);
        if (heap->gc_pool_completed_workers >= requested_workers) {
            break;
        }
        wait_result = nggc_gc_pool_wait_condition(
            heap, &heap->gc_pool_done_condition, &heap->gc_pool_lock, 1, 1);
        if (wait_result == thrd_success || wait_result == thrd_timedout) {
            continue;
        }
        if (!condition_wait_failed) {
            heap->gc_pool_broken = 1u;
            result = NGGC_ERR_BACKEND;
            condition_wait_failed = 1;
        }
        if (mtx_unlock(&heap->gc_pool_lock) != thrd_success) {
            return NGGC_ERR_BACKEND;
        }
        thrd_yield();
        if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
            return NGGC_ERR_BACKEND;
        }
    }
    if (heap->gc_pool_job != job ||
        heap->gc_pool_requested_workers != requested_workers ||
        heap->gc_pool_completed_workers != requested_workers ||
        heap->gc_pool_running_workers != 0u) {
        result = NGGC_ERR_CORRUPTION;
    } else {
        /* Only this boundary proves that no selected slot can use context. */
        *out_drained = 1;
        if (heap->gc_pool_broken && result == NGGC_OK) {
            result = NGGC_ERR_BACKEND;
        }
    }
    heap->gc_pool_job = 0;
    heap->gc_pool_requested_workers = 0u;
    heap->gc_pool_completed_workers = 0u;
    heap->gc_pool_running_workers = 0u;
    if (mtx_unlock(&heap->gc_pool_lock) != thrd_success &&
        result == NGGC_OK) {
        result = NGGC_ERR_BACKEND;
    }
    return result;
}
#else
static nggc_result nggc_gc_pool_init(nggc_heap *heap) {
    return heap ? NGGC_OK : NGGC_ERR_INVALID_ARGUMENT;
}

static nggc_result nggc_gc_pool_destroy(nggc_heap *heap) {
    return heap ? NGGC_OK : NGGC_ERR_INVALID_ARGUMENT;
}
#endif

static size_t nggc_young_evacuation_pending_task_count(
    const nggc_young_evacuation_plan *plan) {
    size_t count = 0u;
    if (!plan) {
        return 0u;
    }
    for (size_t i = 0u; i < plan->task_count; ++i) {
        uint32_t state = atomic_load_explicit(
            &plan->tasks[i].state, memory_order_acquire);
        if (state == NGGC_EVACUATION_TASK_PENDING ||
            state == NGGC_EVACUATION_TASK_RETRY) {
            ++count;
        }
    }
    return count;
}

static nggc_result nggc_young_evacuation_run_workers_locked(
    nggc_heap *heap, nggc_young_evacuation_plan *plan,
    size_t retry_task_index, int fail_worker_create) {
    nggc_evacuation_worker_ctx worker;
    nggc_gc_worker_job job;
    size_t pending;
    size_t desired_workers;
    size_t background_workers;
    size_t dispatched_workers = 0u;
    int background_drained = 1;
    nggc_result result = NGGC_OK;
    if (!heap || !plan ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    pending = nggc_young_evacuation_pending_task_count(plan);
    heap->evacuation_workers_quiescent = 0u;
    if (pending == 0u) {
        heap->evacuation_workers_quiescent = 1u;
        return NGGC_OK;
    }
    memset(&worker, 0, sizeof(worker));
    worker.heap = heap;
    worker.plan = plan;
    worker.retry_task_index = retry_task_index;
    atomic_init(&worker.next_task, 0u);
    atomic_init(&worker.entered_workers, 0u);
    atomic_init(&worker.completed_workers, 0u);
    atomic_init(&worker.retry_injected, 0u);
    job.run = nggc_young_evacuation_worker_job_run;
    job.context = &worker;
    desired_workers = pending < NGGC_EVACUATION_WORKER_LIMIT
                          ? pending
                          : NGGC_EVACUATION_WORKER_LIMIT;
    background_workers = desired_workers > 1u
                             ? desired_workers - 1u
                             : 0u;
#if NGGC_HAVE_C11_THREADS
    if (background_workers != 0u) {
        size_t available_workers = nggc_gc_pool_ensure_locked(
            heap, background_workers, fail_worker_create);
        dispatched_workers = available_workers < background_workers
                                 ? available_workers
                                 : background_workers;
        if (dispatched_workers != 0u) {
            result = nggc_gc_pool_dispatch_locked(
                heap, &job, dispatched_workers);
            if (result != NGGC_OK) {
                dispatched_workers = 0u;
                result = NGGC_OK;
            } else {
                heap->evacuation_worker_dispatch_count =
                    nggc_saturating_add_u64_size(
                        heap->evacuation_worker_dispatch_count,
                        dispatched_workers);
            }
        }
        if (dispatched_workers < background_workers &&
            heap->evacuation_worker_fallback_count != UINT64_MAX) {
            ++heap->evacuation_worker_fallback_count;
        }
    }
#else
    (void)job;
    (void)fail_worker_create;
    if (background_workers != 0u &&
        heap->evacuation_worker_fallback_count != UINT64_MAX) {
        ++heap->evacuation_worker_fallback_count;
    }
#endif
    nggc_young_evacuation_worker_run(&worker);
#if NGGC_HAVE_C11_THREADS
    if (dispatched_workers != 0u) {
        nggc_result wait_result = nggc_gc_pool_wait_locked(
            heap, &job, dispatched_workers, &background_drained);
        if (wait_result != NGGC_OK) {
            result = wait_result;
        }
    }
#endif
    if (dispatched_workers != 0u) {
        if (heap->evacuation_parallel_cycle_count != UINT64_MAX) {
            ++heap->evacuation_parallel_cycle_count;
        }
    }
    if (!background_drained ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u ||
        atomic_load_explicit(&worker.entered_workers,
                             memory_order_acquire) !=
            atomic_load_explicit(&worker.completed_workers,
                                 memory_order_acquire) ||
        atomic_load_explicit(&worker.entered_workers,
                             memory_order_acquire) >
            dispatched_workers + 1u ||
        (result == NGGC_OK &&
         atomic_load_explicit(&worker.entered_workers,
                              memory_order_acquire) !=
             dispatched_workers + 1u)) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->evacuation_workers_quiescent = 1u;
    for (size_t i = 0u; i < plan->task_count; ++i) {
        if (atomic_load_explicit(&plan->tasks[i].state,
                                 memory_order_acquire) ==
            NGGC_EVACUATION_TASK_RETRY) {
            if (heap->evacuation_worker_fallback_count != UINT64_MAX) {
                ++heap->evacuation_worker_fallback_count;
            }
            nggc_young_evacuation_process_task(
                &worker, &plan->tasks[i], i, 0);
        }
    }
    return result;
}

static nggc_result nggc_young_evacuation_enqueue_reclaim_locked(
    nggc_young_evacuation_plan *plan,
    nggc_object_header *destination) {
    if (!plan || !destination ||
        plan->reclaim_count >= plan->reclaim_capacity) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    plan->reclaim_queue[plan->reclaim_count++] = destination;
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_drain_reclaim_locked(
    nggc_heap *heap, nggc_young_evacuation_plan *plan) {
    if (!heap || !plan ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u ||
        heap->evacuation_workers_quiescent == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    for (size_t i = 0u; i < plan->reclaim_count; ++i) {
        if (nggc_young_evacuation_discard_destination_locked(
                heap, plan->reclaim_queue[i]) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->evacuation_loser_reclaimed_count != UINT64_MAX) {
            ++heap->evacuation_loser_reclaimed_count;
        }
        plan->reclaim_queue[i] = 0;
    }
    plan->reclaim_count = 0u;
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_finalize_tasks_locked(
    nggc_heap *heap, nggc_young_evacuation_plan *plan,
    int *evacuation_committed) {
    nggc_result first_error = NGGC_OK;
    if (!heap || !plan || !evacuation_committed ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u ||
        heap->evacuation_workers_quiescent == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    for (size_t i = 0u; i < plan->task_count; ++i) {
        nggc_young_evacuation_task *task = &plan->tasks[i];
        uint32_t state = atomic_load_explicit(&task->state,
                                              memory_order_acquire);
        if (state == NGGC_EVACUATION_TASK_ERROR &&
            first_error == NGGC_OK) {
            first_error = task->result;
            continue;
        }
        if (state != NGGC_EVACUATION_TASK_COMPLETE) {
            continue;
        }
        if (task->cas_attempted && !task->stats_recorded) {
            task->stats_recorded = 1u;
            if (task->cas_won) {
                if (heap->forwarding_cas_success_count != UINT64_MAX) {
                    ++heap->forwarding_cas_success_count;
                }
            } else if (heap->forwarding_cas_failure_count != UINT64_MAX) {
                ++heap->forwarding_cas_failure_count;
            }
            if (heap->evacuation_worker_task_count != UINT64_MAX) {
                ++heap->evacuation_worker_task_count;
            }
        }
        if (task->published_forwarding) {
            *evacuation_committed = 1;
        }
        if (task->published_forwarding !=
            nggc_object_from_header(task->destination)) {
            nggc_result enqueue_result =
                nggc_young_evacuation_enqueue_reclaim_locked(
                    plan, task->destination);
            if (enqueue_result != NGGC_OK && first_error == NGGC_OK) {
                first_error = enqueue_result;
            }
        }
        if (task->result != NGGC_OK ||
            (!task->cas_won && !task->published_forwarding) ||
            (task->cas_won &&
             task->published_forwarding !=
                 nggc_object_from_header(task->destination)) ||
            nggc_young_evacuation_validate_forwarding_locked(
                heap, task->source, task->target_generation) != NGGC_OK) {
            task->result = task->result != NGGC_OK
                               ? task->result
                               : NGGC_ERR_CORRUPTION;
            atomic_store_explicit(&task->state,
                                  NGGC_EVACUATION_TASK_ERROR,
                                  memory_order_release);
            if (first_error == NGGC_OK) {
                first_error = task->result;
            }
        } else {
            atomic_store_explicit(&task->state,
                                  NGGC_EVACUATION_TASK_FINALIZED,
                                  memory_order_release);
        }
    }
    if (plan->reclaim_count != 0u) {
        nggc_result reclaim_result =
            nggc_young_evacuation_drain_reclaim_locked(heap, plan);
        if (reclaim_result != NGGC_OK && first_error == NGGC_OK) {
            first_error = reclaim_result;
        }
    }
    return first_error;
}

static int nggc_young_evacuation_all_tasks_finalized(
    const nggc_young_evacuation_plan *plan) {
    if (!plan) {
        return 0;
    }
    for (size_t i = 0u; i < plan->task_count; ++i) {
        if (atomic_load_explicit(&plan->tasks[i].state,
                                 memory_order_acquire) !=
            NGGC_EVACUATION_TASK_FINALIZED) {
            return 0;
        }
    }
    return 1;
}

static nggc_result nggc_young_evacuation_abort_reservations_locked(
    nggc_heap *heap, nggc_young_evacuation_plan *plan) {
    if (!heap || !plan ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    for (size_t i = 0u; i < plan->reserved_task_count; ++i) {
        nggc_young_evacuation_task *task = &plan->tasks[i];
        uint32_t destination_state = atomic_load_explicit(
            &task->destination->state, memory_order_acquire);
        if (nggc_forwarding_load_acquire(task->source) != 0) {
            return NGGC_ERR_CORRUPTION;
        }
        if (destination_state == NGGC_OBJECT_FREE) {
            continue;
        }
        if (nggc_young_evacuation_discard_destination_locked(
                heap, task->destination) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    plan->reservations_active = 0u;
    plan->reserved_task_count = 0u;
    heap->evacuation_workers_quiescent = 0u;
    heap->evacuation_copy_started = 0u;
    heap->from_space_reclaim_pending = 0u;
    if (nggc_rebuild_free_list_locked(heap) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    return nggc_verify_regions_locked(heap);
}

static nggc_result nggc_young_evacuation_copy_locked(
    nggc_heap *heap, nggc_young_evacuation_plan *plan,
    int *evacuation_committed) {
    nggc_result run_result;
    nggc_result finalize_result;
    nggc_result result;
    size_t retry_task_index = SIZE_MAX;
    int fail_worker_create = 0;
    int concurrent_window = 0;
    nggc_result repark_result;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    int concurrent_window_wait = 0;
    int concurrent_copy_wait = 0;
#endif
    if (!heap || !plan || !evacuation_committed) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_young_evacuation_reserve_tasks_locked(heap, plan);
    if (result != NGGC_OK) {
        if (nggc_young_evacuation_abort_reservations_locked(
                heap, plan) != NGGC_OK) {
            *evacuation_committed = 1;
        }
        return result;
    }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (heap->test_collect_fault ==
            NGGC_TEST_COLLECT_FAULT_AFTER_FORWARDING) {
        nggc_evacuation_worker_ctx worker;
        memset(&worker, 0, sizeof(worker));
        worker.heap = heap;
        worker.plan = plan;
        worker.retry_task_index = SIZE_MAX;
        atomic_init(&worker.next_task, 0u);
        atomic_init(&worker.entered_workers, 0u);
        atomic_init(&worker.completed_workers, 0u);
        atomic_init(&worker.retry_injected, 0u);
        for (size_t i = 0u; i < plan->task_count; ++i) {
            if (atomic_load_explicit(&plan->tasks[i].state,
                                     memory_order_acquire) ==
                NGGC_EVACUATION_TASK_PENDING) {
                nggc_young_evacuation_process_task(
                    &worker, &plan->tasks[i], i, 0);
                break;
            }
        }
        heap->evacuation_workers_quiescent = 1u;
        finalize_result = nggc_young_evacuation_finalize_tasks_locked(
            heap, plan, evacuation_committed);
        if (*evacuation_committed) {
            heap->from_space_reclaim_pending = 1u;
        } else {
            nggc_result abort_result =
                nggc_young_evacuation_abort_reservations_locked(
                    heap, plan);
            if (abort_result != NGGC_OK) {
                *evacuation_committed = 1;
                return abort_result;
            }
        }
        if (finalize_result != NGGC_OK) {
            return finalize_result;
        }
        return nggc_test_collect_fault_take_locked(
            heap, NGGC_TEST_COLLECT_FAULT_AFTER_FORWARDING,
            NGGC_ERR_CORRUPTION);
    }
    if (heap->test_collect_fault ==
            NGGC_TEST_COLLECT_ACTION_WORKER_TASK_RETRY) {
        heap->test_collect_fault = 0u;
        retry_task_index = 0u;
    } else if (heap->test_collect_fault ==
               NGGC_TEST_COLLECT_ACTION_WORKER_CREATE_FALLBACK) {
        heap->test_collect_fault = 0u;
        fail_worker_create = 1;
    } else if (heap->test_collect_fault ==
               NGGC_TEST_COLLECT_ACTION_CONCURRENT_WINDOW) {
        heap->test_collect_fault = 0u;
        concurrent_window_wait = 1;
    } else if (heap->test_collect_fault ==
               NGGC_TEST_COLLECT_ACTION_CONCURRENT_COPY) {
        heap->test_collect_fault = 0u;
        concurrent_window_wait = 1;
        concurrent_copy_wait = 1;
    }
#endif
    if (plan->task_count != 0u) {
        result = nggc_mutator_concurrent_resume_locked(heap);
        if (result != NGGC_OK) {
            if (nggc_young_evacuation_abort_reservations_locked(
                    heap, plan) != NGGC_OK) {
                *evacuation_committed = 1;
            }
            return result;
        }
        concurrent_window = 1;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        if (concurrent_copy_wait) {
            atomic_store_explicit(&heap->test_concurrent_copy_pause, 1u,
                                  memory_order_release);
        }
#endif
        nggc_unlock(heap);
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        if (concurrent_window_wait) {
            atomic_store_explicit(&heap->test_concurrent_window_entered,
                                  1u, memory_order_release);
            while (atomic_load_explicit(
                       &heap->test_concurrent_window_release,
                       memory_order_acquire) == 0u) {
#if NGGC_HAVE_C11_THREADS
                thrd_yield();
#else
                atomic_signal_fence(memory_order_seq_cst);
#endif
            }
        }
#endif
    }
    run_result = nggc_young_evacuation_run_workers_locked(
        heap, plan, retry_task_index, fail_worker_create);
    if (concurrent_window) {
        nggc_lock(heap);
        repark_result = nggc_mutator_concurrent_request_repark_locked(heap);
        nggc_unlock(heap);
        if (repark_result == NGGC_OK) {
            repark_result = nggc_mutator_concurrent_wait_repark(heap);
        }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        if (concurrent_copy_wait && repark_result == NGGC_OK) {
            atomic_store_explicit(&heap->test_concurrent_copy_entered, 3u,
                                  memory_order_release);
            while (atomic_load_explicit(&heap->test_concurrent_copy_release,
                                        memory_order_acquire) < 3u) {
#if NGGC_HAVE_C11_THREADS
                thrd_yield();
#else
                atomic_signal_fence(memory_order_seq_cst);
#endif
            }
        }
#endif
        nggc_lock(heap);
        if (repark_result != NGGC_OK) {
            *evacuation_committed = 1;
            return repark_result;
        }
    }
    if (atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u) {
        *evacuation_committed = 1;
        return NGGC_ERR_CORRUPTION;
    }
    finalize_result = nggc_young_evacuation_finalize_tasks_locked(
        heap, plan, evacuation_committed);
    if (*evacuation_committed ||
        (run_result == NGGC_OK && finalize_result == NGGC_OK &&
         nggc_young_evacuation_all_tasks_finalized(plan))) {
        heap->from_space_reclaim_pending = 1u;
    }
    if (finalize_result != NGGC_OK) {
        result = finalize_result;
    } else if (run_result != NGGC_OK) {
        result = run_result;
    } else if (!nggc_young_evacuation_all_tasks_finalized(plan) ||
               heap->arena_cursor != plan->destination_end ||
               heap->evacuation_workers_quiescent == 0u ||
               heap->from_space_reclaim_pending == 0u) {
        result = NGGC_ERR_CORRUPTION;
    } else {
        result = NGGC_OK;
    }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (result == NGGC_OK && *evacuation_committed &&
        heap->test_force_emergency_failure != 0u) {
        result = NGGC_ERR_CORRUPTION;
    }
#endif
    if (result != NGGC_OK && !*evacuation_committed) {
        nggc_result abort_result =
            nggc_young_evacuation_abort_reservations_locked(heap, plan);
        if (abort_result != NGGC_OK) {
            *evacuation_committed = 1;
            return abort_result;
        }
    }
    return result;
}

static nggc_result nggc_age_young_survivors_locked(nggc_heap *heap) {
    uintptr_t cursor;
    if (!heap || heap->young_tenuring_threshold == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_LIVE &&
            heap->regions[region_index].in_collection_set != 0u) {
            if (block.header->mark_epoch != heap->mark_epoch) {
                return NGGC_ERR_CORRUPTION;
            }
            if (block.header->generation !=
                NGGC_REGION_GENERATION_YOUNG) {
                return NGGC_ERR_CORRUPTION;
            }
            if (block.header->young_age > heap->young_tenuring_threshold) {
                return NGGC_ERR_CORRUPTION;
            }
        }
        cursor = block.end;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (block.state == NGGC_OBJECT_LIVE &&
            heap->regions[region_index].in_collection_set != 0u &&
            block.header->young_age < heap->young_tenuring_threshold) {
            ++block.header->young_age;
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static nggc_result nggc_age_special_objects_locked(nggc_heap *heap) {
    uintptr_t cursor;
    if (!heap || heap->young_tenuring_threshold == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_LIVE &&
            heap->regions[region_index].generation ==
                NGGC_REGION_GENERATION_SPECIAL) {
            uint8_t next_age;
            if (block.header->mark_epoch != heap->mark_epoch ||
                !nggc_live_object_generation_valid(
                    block.header->generation)) {
                return NGGC_ERR_CORRUPTION;
            }
            if (block.header->generation == NGGC_REGION_GENERATION_OLD) {
                if (block.header->young_age != 0u) {
                    return NGGC_ERR_CORRUPTION;
                }
                cursor = block.end;
                continue;
            }
            next_age = block.header->young_age <
                               heap->young_tenuring_threshold
                           ? (uint8_t)(block.header->young_age + 1u)
                           : block.header->young_age;
            if (next_age >= heap->young_tenuring_threshold) {
                block.header->generation = NGGC_REGION_GENERATION_OLD;
                block.header->young_age = 0u;
                if (heap->young_promoted_object_count != UINT64_MAX) {
                    ++heap->young_promoted_object_count;
                }
                if (heap->young_in_place_promoted_object_count !=
                    UINT64_MAX) {
                    ++heap->young_in_place_promoted_object_count;
                }
            } else {
                block.header->young_age = next_age;
            }
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static nggc_result nggc_sweep_unmarked_collection_set_locked(
    nggc_heap *heap, size_t *out_work_bytes,
    size_t *out_reclaimed_objects) {
    uintptr_t cursor;
    size_t work_bytes = 0u;
    size_t reclaimed_objects = 0u;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        work_bytes = nggc_saturating_add_size(
            work_bytes, (size_t)(block.end - block.start));
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (block.state == NGGC_OBJECT_LIVE &&
            heap->regions[region_index].in_collection_set != 0u &&
            header->mark_epoch != heap->mark_epoch) {
            if (nggc_reclaim_live_object_locked(heap, header) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            if (reclaimed_objects != SIZE_MAX) {
                ++reclaimed_objects;
            }
        }
        cursor = block.end;
    }
    if (out_work_bytes) {
        *out_work_bytes = work_bytes;
    }
    if (out_reclaimed_objects) {
        *out_reclaimed_objects = reclaimed_objects;
    }
    return NGGC_OK;
}

static nggc_result nggc_young_promote_in_place_regions_locked(
    nggc_heap *heap, const nggc_young_evacuation_plan *plan) {
    uintptr_t cursor;
    if (!heap || !plan || !plan->in_place_region_flags ||
        plan->in_place_region_count != heap->region_count) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < plan->source_end) {
        nggc_block_view block;
        size_t region_index;
        uint8_t next_age;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (plan->in_place_region_flags[region_index] != 0u) {
            if (block.state == NGGC_OBJECT_FORWARDED ||
                (block.state == NGGC_OBJECT_LIVE &&
                 (block.header->mark_epoch != heap->mark_epoch ||
                  block.header->generation !=
                      NGGC_REGION_GENERATION_YOUNG ||
                  nggc_pin_count_load(block.header) != 0u ||
                  (block.header->flags &
                   (NGGC_OBJECT_FLAG_LARGE |
                    NGGC_OBJECT_FLAG_NON_MOVABLE)) != 0u ||
                  nggc_forwarding_load_acquire(block.header) != 0 ||
                  nggc_forwarding_epoch_load_acquire(block.header) != 0u ||
                  block.header->young_age >
                      heap->young_tenuring_threshold))) {
                return NGGC_ERR_CORRUPTION;
            }
            if (block.state == NGGC_OBJECT_LIVE) {
                next_age = block.header->young_age <
                                    heap->young_tenuring_threshold
                                ? (uint8_t)(block.header->young_age + 1u)
                                : block.header->young_age;
                if (next_age < heap->young_tenuring_threshold) {
                    return NGGC_ERR_CORRUPTION;
                }
            }
        }
        cursor = block.end;
    }
    for (size_t i = 0u; i < heap->region_count; ++i) {
        nggc_region_meta *region = &heap->regions[i];
        if (plan->in_place_region_flags[i] == 0u) {
            continue;
        }
        if (region->generation != NGGC_REGION_GENERATION_YOUNG ||
            region->in_collection_set == 0u ||
            region->live_object_count == 0u ||
            region->tlab_state != NGGC_TLAB_INACTIVE) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < plan->source_end) {
        nggc_block_view block;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        if (plan->in_place_region_flags[region_index] != 0u &&
            block.state == NGGC_OBJECT_LIVE) {
            block.header->generation = NGGC_REGION_GENERATION_OLD;
            block.header->young_age = 0u;
            if (heap->young_promoted_object_count != UINT64_MAX) {
                ++heap->young_promoted_object_count;
            }
            if (heap->young_in_place_promoted_object_count != UINT64_MAX) {
                ++heap->young_in_place_promoted_object_count;
            }
        }
        cursor = block.end;
    }
    for (size_t i = 0u; i < heap->region_count; ++i) {
        if (plan->in_place_region_flags[i] != 0u) {
            heap->regions[i].generation = NGGC_REGION_GENERATION_OLD;
            heap->regions[i].in_collection_set = 0u;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_from_space_quarantine_begin_locked(
    nggc_heap *heap, uintptr_t source_end) {
    if (!heap || source_end < (uintptr_t)heap->gc_arena_base ||
        source_end > heap->arena_cursor || heap->evacuation_epoch == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->from_space_quarantine_active) {
        return heap->from_space_quarantine_epoch ==
                       heap->evacuation_epoch &&
                   heap->from_space_quarantine_source_end == source_end
               ? NGGC_OK
               : NGGC_ERR_CORRUPTION;
    }
    if (heap->from_space_quarantine_epoch != 0u ||
        heap->from_space_quarantine_release_epoch != 0u ||
        heap->from_space_quarantine_source_end != 0u ||
        heap->from_space_quarantine_object_count != 0u ||
        heap->from_space_quarantine_bytes != 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->from_space_quarantine_epoch = heap->evacuation_epoch;
    heap->from_space_quarantine_active = 1u;
    heap->from_space_quarantine_release_epoch = heap->safepoint_epoch;
    heap->from_space_quarantine_source_end = source_end;
    heap->read_barrier_active = nggc_expected_read_barrier_locked(heap);
    return NGGC_OK;
}

static nggc_result nggc_quarantine_forwarded_source_locked(
    nggc_heap *heap, nggc_object_header *header) {
    nggc_object_header *target;
    if (!heap || !header ||
        !heap->from_space_quarantine_active ||
        heap->from_space_quarantine_epoch != heap->evacuation_epoch ||
        atomic_load_explicit(&header->state, memory_order_acquire) !=
            NGGC_OBJECT_LIVE ||
        nggc_pin_count_load(header) != 0u ||
        nggc_forwarding_target_locked(heap, header, &target) != NGGC_OK ||
        !target ||
        heap->from_space_quarantine_object_count == SIZE_MAX ||
        heap->from_space_quarantine_bytes >
            SIZE_MAX - header->alloc_size ||
        nggc_region_quarantine_object_locked(heap, header) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    ++heap->from_space_quarantine_object_count;
    heap->from_space_quarantine_bytes += header->alloc_size;
    atomic_store_explicit(&header->state, NGGC_OBJECT_FORWARDED,
                          memory_order_release);
    return NGGC_OK;
}

static nggc_result nggc_from_space_quarantine_finalize_locked(
    nggc_heap *heap) {
    if (!heap || !heap->from_space_quarantine_active ||
        heap->from_space_quarantine_epoch != heap->evacuation_epoch) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->from_space_quarantine_object_count == 0u) {
        if (heap->from_space_quarantine_bytes != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
        heap->from_space_quarantine_epoch = 0u;
        heap->from_space_quarantine_active = 0u;
        heap->from_space_quarantine_release_epoch = 0u;
        heap->from_space_quarantine_source_end = 0u;
        heap->read_barrier_active = nggc_expected_read_barrier_locked(heap);
        return NGGC_OK;
    }
    if (heap->from_space_quarantine_bytes == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->from_space_quarantine_cycle_count != UINT64_MAX) {
        ++heap->from_space_quarantine_cycle_count;
    }
    heap->read_barrier_active = nggc_expected_read_barrier_locked(heap);
    return NGGC_OK;
}

static nggc_result nggc_sweep_evacuated_collection_set_locked(
    nggc_heap *heap, const nggc_young_evacuation_plan *plan,
    uintptr_t source_end, size_t *out_work_bytes,
    size_t *out_reclaimed_objects) {
    uintptr_t cursor = (uintptr_t)heap->gc_arena_base;
    size_t work_bytes = 0u;
    size_t reclaimed_objects = out_reclaimed_objects
                                   ? *out_reclaimed_objects
                                   : 0u;
    if (!heap || !plan || plan->source_end != source_end ||
        heap->evacuation_worker_epoch != heap->evacuation_epoch ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u ||
        heap->evacuation_workers_quiescent == 0u ||
        heap->remap_worker_epoch != heap->evacuation_epoch ||
        atomic_load_explicit(&heap->remap_workers_active,
                             memory_order_acquire) != 0u ||
        heap->remap_workers_quiescent == 0u ||
        heap->from_space_reclaim_pending == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_from_space_quarantine_begin_locked(heap, source_end) !=
        NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    while (cursor < source_end) {
        nggc_block_view block;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        work_bytes = nggc_saturating_add_size(
            work_bytes, (size_t)(block.end - block.start));
        if (out_work_bytes) {
            *out_work_bytes = work_bytes;
        }
        if (block.state == NGGC_OBJECT_LIVE &&
            region_index < heap->region_count &&
            heap->regions[region_index].in_collection_set != 0u) {
            if (block.header->mark_epoch == heap->mark_epoch) {
                if (nggc_young_evacuation_region_in_place(
                        plan, region_index)) {
                    cursor = block.end;
                    continue;
                }
                nggc_object_header *target;
                size_t target_index;
                if (nggc_pin_count_load(block.header) != 0u) {
                    if ((block.header->flags & NGGC_OBJECT_FLAG_PINNABLE) ==
                            0u ||
                        nggc_forwarding_load_acquire(block.header) != 0 ||
                        nggc_forwarding_epoch_load_acquire(block.header) !=
                            0u ||
                        block.header->generation !=
                            NGGC_REGION_GENERATION_YOUNG) {
                        return NGGC_ERR_CORRUPTION;
                    }
                    if (block.header->young_age <
                        heap->young_tenuring_threshold) {
                        ++block.header->young_age;
                    }
                    cursor = block.end;
                    continue;
                }
                if (nggc_forwarding_target_locked(
                        heap, block.header, &target) != NGGC_OK ||
                    !target) {
                    return NGGC_ERR_CORRUPTION;
                }
                target_index = (size_t)(((uintptr_t)target -
                    (uintptr_t)heap->gc_arena_base) / heap->region_size);
                if (target_index >= heap->region_count) {
                    return NGGC_ERR_CORRUPTION;
                }
                if (target->type_id != block.header->type_id ||
                    target->size_class != block.header->size_class ||
                    target->flags != block.header->flags ||
                    target->payload_size != block.header->payload_size ||
                    target->alloc_size != block.header->alloc_size ||
                    target->object_id != block.header->object_id ||
                    target->mark_epoch != heap->mark_epoch ||
                    target->generation !=
                        heap->regions[target_index].generation) {
                    return NGGC_ERR_CORRUPTION;
                }
                if (nggc_quarantine_forwarded_source_locked(
                        heap, block.header) != NGGC_OK) {
                    return NGGC_ERR_CORRUPTION;
                }
                if (heap->young_evacuated_object_count != UINT64_MAX) {
                    ++heap->young_evacuated_object_count;
                }
                if (heap->regions[target_index].generation ==
                        NGGC_REGION_GENERATION_OLD &&
                    heap->young_promoted_object_count != UINT64_MAX) {
                    ++heap->young_promoted_object_count;
                }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
                {
                    nggc_result fault_result =
                        nggc_test_collect_fault_take_locked(
                            heap,
                            NGGC_TEST_COLLECT_FAULT_DURING_SOURCE_RELEASE,
                            NGGC_ERR_CORRUPTION);
                    if (fault_result != NGGC_OK) {
                        return fault_result;
                    }
                }
#endif
            } else {
                if (nggc_reclaim_live_object_locked(
                        heap, block.header) != NGGC_OK) {
                    return NGGC_ERR_CORRUPTION;
                }
                if (reclaimed_objects != SIZE_MAX) {
                    ++reclaimed_objects;
                }
                if (out_reclaimed_objects) {
                    *out_reclaimed_objects = reclaimed_objects;
                }
            }
        }
        cursor = block.end;
    }
    if (out_work_bytes) {
        *out_work_bytes = work_bytes;
    }
    if (out_reclaimed_objects) {
        *out_reclaimed_objects = reclaimed_objects;
    }
    return NGGC_OK;
}

static nggc_result nggc_evacuation_reclaim_complete_locked(
    nggc_heap *heap) {
    if (!heap || heap->evacuation_worker_epoch != heap->evacuation_epoch ||
        atomic_load_explicit(&heap->evacuation_workers_active,
                             memory_order_acquire) != 0u ||
        heap->evacuation_workers_quiescent == 0u ||
        heap->remap_worker_epoch != heap->evacuation_epoch ||
        atomic_load_explicit(&heap->remap_workers_active,
                             memory_order_acquire) != 0u ||
        heap->remap_workers_quiescent == 0u ||
        heap->from_space_reclaim_pending == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (nggc_from_space_quarantine_finalize_locked(heap) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->from_space_reclaim_pending = 0u;
    return NGGC_OK;
}

static nggc_result nggc_verify_evacuated_collection_set_locked(
    nggc_heap *heap, uintptr_t source_end) {
    uintptr_t cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < source_end) {
        nggc_block_view block;
        size_t region_index;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        region_index = (size_t)((cursor -
                                 (uintptr_t)heap->gc_arena_base) /
                                heap->region_size);
        if (region_index >= heap->region_count ||
            (heap->regions[region_index].in_collection_set != 0u &&
             block.state == NGGC_OBJECT_LIVE &&
             (nggc_pin_count_load(block.header) == 0u ||
              (block.header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u ||
              nggc_forwarding_load_acquire(block.header) != 0 ||
              nggc_forwarding_epoch_load_acquire(block.header) != 0u ||
              block.header->generation != NGGC_REGION_GENERATION_YOUNG))) {
            return NGGC_ERR_CORRUPTION;
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

static nggc_result nggc_young_evacuation_emergency_complete_locked(
            nggc_heap *heap, nggc_young_evacuation_plan *plan,
            int *evacuation_committed, size_t *sweep_work_bytes,
            size_t *young_reclaimed_objects) {
    nggc_result result;
    if (!heap || !plan || !evacuation_committed ||
        !*evacuation_committed || !heap->collector_cycle_active ||
        heap->collector_cycle_kind !=
            NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION) {
        return NGGC_ERR_CORRUPTION;
    }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    if (heap->test_force_emergency_failure != 0u) {
        heap->test_force_emergency_failure = 0u;
        return NGGC_ERR_CORRUPTION;
    }
#endif
    while (heap->collector_cycle_active) {
        if (heap->gc_phase == NGGC_PHASE_IDLE) {
            if (heap->collector_expected_phase != NGGC_PHASE_EVACUATE &&
                heap->collector_expected_phase != NGGC_PHASE_REMAP &&
                heap->collector_expected_phase != NGGC_PHASE_SWEEP) {
                return NGGC_ERR_CORRUPTION;
            }
            result = nggc_phase_begin_locked(
                heap, heap->collector_expected_phase);
            if (result != NGGC_OK) {
                return result;
            }
        }
        switch (heap->gc_phase) {
            case NGGC_PHASE_EVACUATE:
                result = nggc_young_evacuation_copy_locked(
                    heap, plan, evacuation_committed);
                if (result == NGGC_OK) {
                    result = nggc_phase_end_locked(
                        heap, NGGC_PHASE_EVACUATE);
                }
                break;
            case NGGC_PHASE_REMAP:
                result = nggc_phase_end_locked(heap, NGGC_PHASE_REMAP);
                break;
            case NGGC_PHASE_SWEEP:
                result = nggc_sweep_evacuated_collection_set_locked(
                    heap, plan, plan->source_end, sweep_work_bytes,
                    young_reclaimed_objects);
                if (result == NGGC_OK && plan->in_place_region_flags) {
                    result = nggc_young_promote_in_place_regions_locked(
                        heap, plan);
                }
                if (result == NGGC_OK) {
                    result = nggc_verify_evacuated_collection_set_locked(
                        heap, plan->source_end);
                }
                if (result == NGGC_OK) {
                    result = nggc_rebuild_free_list_locked(heap);
                }
                if (result == NGGC_OK) {
                    result = nggc_evacuation_reclaim_complete_locked(heap);
                }
                if (result == NGGC_OK) {
                    result = nggc_phase_end_locked(heap, NGGC_PHASE_SWEEP);
                }
                break;
            default:
                return NGGC_ERR_CORRUPTION;
        }
        if (result != NGGC_OK) {
            return result;
        }
    }
    if (heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_expected_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_kind != NGGC_COLLECTOR_CYCLE_NONE) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static nggc_result nggc_sweep_unmarked_locked(nggc_heap *heap,
                                              size_t sweep_budget_bytes,
                                              size_t target_reclaim_bytes,
                                              int full_sweep,
                                              size_t *out_work_bytes,
                                              size_t *out_reclaimed_bytes) {
    uintptr_t cursor;
    size_t work_bytes = 0;
    size_t reclaimed_bytes = 0;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        work_bytes = nggc_saturating_add_size(
            work_bytes, (size_t)(block.end - block.start));
        if (block.state == NGGC_OBJECT_LIVE &&
            header->mark_epoch != heap->mark_epoch) {
            size_t alloc_size = header->alloc_size;
            if (nggc_reclaim_live_object_locked(heap, header) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            reclaimed_bytes = nggc_saturating_add_size(reclaimed_bytes,
                                                       alloc_size);
        }
        cursor = block.end;
        if (!full_sweep) {
            if (target_reclaim_bytes != 0 &&
                reclaimed_bytes >= target_reclaim_bytes) {
                break;
            }
            if (sweep_budget_bytes != 0 && work_bytes >= sweep_budget_bytes) {
                break;
            }
        }
    }
    if (out_work_bytes) {
        *out_work_bytes = work_bytes;
    }
    if (out_reclaimed_bytes) {
        *out_reclaimed_bytes = reclaimed_bytes;
    }
    return NGGC_OK;
}

static nggc_result nggc_assist_state_verify_locked(const nggc_heap *heap) {
    uintptr_t arena_base;
    size_t thread_debt = 0u;
    const nggc_thread *thread;
    if (!heap || heap->mutator_assist_state >
                     NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->mutator_assist_current_budget_items >
            NGGC_ASSIST_MARK_BUDGET_MAX_ITEMS ||
        heap->mutator_assist_current_budget_bytes >
            NGGC_ASSIST_SWEEP_BUDGET_MAX_BYTES ||
        heap->mutator_assist_consecutive_packets >
            NGGC_ASSIST_MAX_CONSECUTIVE_PACKETS ||
        (heap->mutator_assist_last_thread &&
         !nggc_thread_is_registered(heap, heap->mutator_assist_last_thread)) ||
        (heap->mutator_assist_last_thread == 0 &&
         heap->mutator_assist_consecutive_packets != 0u)) {
        return NGGC_ERR_CORRUPTION;
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        if (thread->mutator_assist_debt_bytes > SIZE_MAX - thread_debt ||
            thread->mutator_assist_no_progress_streak >
                NGGC_ASSIST_NO_PROGRESS_BACKOFF_THRESHOLD ||
            thread->mutator_assist_backoff_remaining >
                NGGC_ASSIST_BACKOFF_PACKETS) {
            return NGGC_ERR_CORRUPTION;
        }
        thread_debt += thread->mutator_assist_debt_bytes;
    }
    if (thread_debt > heap->mutator_assist_debt_bytes) {
        return NGGC_ERR_CORRUPTION;
    }
    arena_base = (uintptr_t)heap->gc_arena_base;
    if (heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_IDLE) {
        if (heap->mutator_assist_mark_stack.count != 0u ||
            heap->mutator_assist_sweep_cursor != 0u ||
            heap->mutator_assist_target_reclaim_bytes != 0u ||
            heap->mutator_assist_reclaimed_bytes != 0u ||
            heap->mutator_assist_current_budget_items != 0u ||
            heap->mutator_assist_current_budget_bytes != 0u ||
            heap->mutator_assist_last_thread != 0 ||
            heap->mutator_assist_reference_kind != 0u ||
            heap->mutator_assist_reference_queue_scan_cursor != 0u ||
            heap->mutator_assist_reference_queue_scan_end != 0u ||
            heap->mutator_assist_reference_queue != 0 ||
            heap->mutator_assist_reference_queue_object_id != 0u ||
            heap->mutator_assist_reference_queue_member != 0 ||
            heap->mutator_assist_reference_queue_remaining != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (heap->mutator_assist_state ==
               NGGC_MUTATOR_ASSIST_STATE_MARK) {
        if (heap->mutator_assist_sweep_cursor != 0u ||
            heap->mutator_assist_current_budget_items == 0u ||
            heap->mutator_assist_current_budget_bytes == 0u ||
            heap->mutator_assist_reference_kind != 0u ||
            heap->mutator_assist_reference_queue_scan_cursor != 0u ||
            heap->mutator_assist_reference_queue_scan_end != 0u ||
            heap->mutator_assist_reference_queue != 0 ||
            heap->mutator_assist_reference_queue_object_id != 0u ||
            heap->mutator_assist_reference_queue_member != 0 ||
            heap->mutator_assist_reference_queue_remaining != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (heap->mutator_assist_state ==
               NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) {
        if (heap->mutator_assist_current_budget_items == 0u ||
            heap->mutator_assist_current_budget_bytes == 0u ||
            heap->mutator_assist_reference_kind <
                NGGC_ASSIST_REFERENCE_SOFT ||
            heap->mutator_assist_reference_kind >
                NGGC_ASSIST_REFERENCE_QUEUES ||
            heap->reference_discovered_state !=
                NGGC_REFERENCE_DISCOVERY_DRAINING ||
            heap->mutator_assist_reference_queue_scan_cursor < arena_base ||
            heap->mutator_assist_reference_queue_scan_cursor >
                heap->mutator_assist_reference_queue_scan_end ||
            heap->mutator_assist_reference_queue_scan_end >
                heap->arena_cursor ||
            ((heap->mutator_assist_reference_queue != 0) !=
             (heap->mutator_assist_reference_queue_remaining != 0u)) ||
            (heap->mutator_assist_reference_queue != 0 &&
             (heap->mutator_assist_reference_queue_object_id == 0u ||
              heap->mutator_assist_reference_queue_member == 0))) {
            return NGGC_ERR_CORRUPTION;
        }
    } else if (heap->mutator_assist_sweep_cursor < arena_base ||
               heap->mutator_assist_sweep_cursor > heap->arena_cursor ||
               heap->mutator_assist_current_budget_items == 0u ||
               heap->mutator_assist_current_budget_bytes == 0u ||
               heap->mutator_assist_reference_kind != 0u ||
               heap->mutator_assist_reference_queue_scan_cursor != 0u ||
               heap->mutator_assist_reference_queue_scan_end != 0u ||
               heap->mutator_assist_reference_queue != 0 ||
               heap->mutator_assist_reference_queue_object_id != 0u ||
               heap->mutator_assist_reference_queue_member != 0 ||
               heap->mutator_assist_reference_queue_remaining != 0u ||
               heap->reference_discovered_state !=
                   NGGC_REFERENCE_DISCOVERY_IDLE) {
        return NGGC_ERR_CORRUPTION;
    }
    return NGGC_OK;
}

static void nggc_assist_reset_locked(nggc_heap *heap) {
    if (!heap) {
        return;
    }
    (void)nggc_reference_discovered_clear_locked(heap);
    heap->mutator_assist_mark_stack.count = 0u;
    heap->mutator_assist_state = NGGC_MUTATOR_ASSIST_STATE_IDLE;
    heap->mutator_assist_sweep_cursor = 0u;
    heap->mutator_assist_target_reclaim_bytes = 0u;
    heap->mutator_assist_reclaimed_bytes = 0u;
    heap->mutator_assist_last_thread = 0;
    heap->mutator_assist_current_budget_items = 0u;
    heap->mutator_assist_current_budget_bytes = 0u;
    heap->mutator_assist_consecutive_packets = 0u;
    heap->mutator_assist_reference_kind = 0u;
    heap->mutator_assist_reference_queue_scan_cursor = 0u;
    heap->mutator_assist_reference_queue_scan_end = 0u;
    heap->mutator_assist_reference_queue = 0;
    heap->mutator_assist_reference_queue_object_id = 0u;
    heap->mutator_assist_reference_queue_member = 0;
    heap->mutator_assist_reference_queue_remaining = 0u;
}

static nggc_result nggc_assist_sweep_packet_locked(
    nggc_heap *heap, size_t sweep_budget_bytes, size_t *out_work_bytes,
    size_t *out_reclaimed_bytes, int *out_complete) {
    uintptr_t cursor;
    size_t work_bytes = 0u;
    size_t reclaimed_bytes = 0u;
    if (!heap || !out_complete) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    cursor = heap->mutator_assist_sweep_cursor;
    while (cursor < heap->arena_cursor &&
           work_bytes < sweep_budget_bytes) {
        nggc_block_view block;
        nggc_object_header *header;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        work_bytes = nggc_saturating_add_size(
            work_bytes, (size_t)(block.end - block.start));
        if (block.state == NGGC_OBJECT_LIVE &&
            header->mark_epoch != heap->mark_epoch) {
            size_t alloc_size = header->alloc_size;
            if (nggc_reclaim_live_object_locked(heap, header) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            reclaimed_bytes = nggc_saturating_add_size(reclaimed_bytes,
                                                       alloc_size);
        }
        cursor = block.end;
    }
    heap->mutator_assist_sweep_cursor = cursor;
    heap->mutator_assist_reclaimed_bytes = nggc_saturating_add_size(
        heap->mutator_assist_reclaimed_bytes, reclaimed_bytes);
    *out_complete = cursor >= heap->arena_cursor;
    if (out_work_bytes) {
        *out_work_bytes = work_bytes;
    }
    if (out_reclaimed_bytes) {
        *out_reclaimed_bytes = reclaimed_bytes;
    }
    return NGGC_OK;
}

static nggc_result nggc_assist_reclaim_for_allocation_locked(
    nggc_heap *heap, nggc_thread *assist_thread, size_t alloc_size) {
    nggc_mark_ctx mark_ctx;
    uint32_t active_phase = NGGC_PHASE_IDLE;
    nggc_result result;
    size_t processed = 0u;
    size_t work_bytes = 0u;
    size_t reclaimed_bytes = 0u;
    size_t mark_budget_items = 0u;
    size_t sweep_budget_bytes = 0u;
    size_t reference_processed = 0u;
    int complete = 0;
    int reference_complete = 0;
    int progress = 0;

    if (!heap || alloc_size == 0) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_assist_state_verify_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    if (!nggc_assist_prepare_budget_locked(
            heap, assist_thread, alloc_size, &mark_budget_items,
            &sweep_budget_bytes)) {
        return NGGC_OK;
    }
    result = nggc_collect_prepare_safepoint_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    heap->collect_safepoint_epoch = 0u;
    if (heap->mutator_assist_packet_count != UINT64_MAX) {
        ++heap->mutator_assist_packet_count;
    }
    if (assist_thread && nggc_thread_is_registered(heap, assist_thread)) {
        if (assist_thread->mutator_assist_packet_count != UINT64_MAX) {
            ++assist_thread->mutator_assist_packet_count;
        }
        if (heap->mutator_assist_last_thread != assist_thread) {
            heap->mutator_assist_consecutive_packets = 0u;
        }
        heap->mutator_assist_last_thread = assist_thread;
        if (heap->mutator_assist_consecutive_packets < UINT32_MAX) {
            ++heap->mutator_assist_consecutive_packets;
        }
        if (heap->mutator_assist_thread_packet_count != UINT64_MAX) {
            ++heap->mutator_assist_thread_packet_count;
        }
    }
    if (heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_IDLE) {
        result = nggc_phase_begin_locked(heap, NGGC_PHASE_ROOT_SNAPSHOT);
        if (result != NGGC_OK) {
            goto done;
        }
        active_phase = NGGC_PHASE_ROOT_SNAPSHOT;
        result = nggc_advance_mark_epoch_locked(heap);
        if (result != NGGC_OK) {
            goto done;
        }
        heap->mutator_assist_target_reclaim_bytes = alloc_size;
        mark_ctx.heap = heap;
        mark_ctx.stack = &heap->mutator_assist_mark_stack;
        mark_ctx.result = NGGC_OK;
        result = nggc_mark_roots_locked(heap, &mark_ctx);
        if (result != NGGC_OK) {
            goto done;
        }
        result = nggc_phase_end_locked(heap, NGGC_PHASE_ROOT_SNAPSHOT);
        if (result != NGGC_OK) {
            goto done;
        }
        active_phase = NGGC_PHASE_IDLE;
        heap->mutator_assist_state = NGGC_MUTATOR_ASSIST_STATE_MARK;
        if (heap->mutator_assist_cycle_count != UINT64_MAX) {
            ++heap->mutator_assist_cycle_count;
        }
        progress = 1;
    }

    if (heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_MARK) {
        result = nggc_phase_begin_locked(heap, NGGC_PHASE_MARK);
        if (result != NGGC_OK) {
            goto done;
        }
        active_phase = NGGC_PHASE_MARK;
        mark_ctx.heap = heap;
        mark_ctx.stack = &heap->mutator_assist_mark_stack;
        mark_ctx.result = NGGC_OK;
        result = nggc_drain_mark_stack_bounded_locked(
            &mark_ctx, mark_budget_items, &processed);
        if (result != NGGC_OK) {
            goto done;
        }
        heap->mutator_assist_mark_items_processed =
            nggc_saturating_add_u64_size(
                heap->mutator_assist_mark_items_processed, processed);
        progress = progress || processed != 0u;
        result = nggc_phase_end_locked(heap, NGGC_PHASE_MARK);
        if (result != NGGC_OK) {
            goto done;
        }
        active_phase = NGGC_PHASE_IDLE;
        if (heap->mutator_assist_mark_stack.count == 0u) {
            heap->mutator_assist_state =
                NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS;
            heap->mutator_assist_reference_kind =
                NGGC_ASSIST_REFERENCE_SOFT;
            heap->mutator_assist_reference_queue_scan_cursor =
                (uintptr_t)heap->gc_arena_base;
            heap->mutator_assist_reference_queue_scan_end =
                heap->arena_cursor;
            ++heap->reference_process_count;
            progress = 1;
        }
    }

    if (heap->mutator_assist_state ==
        NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) {
        result = nggc_phase_begin_locked(
            heap, NGGC_PHASE_REFERENCE_PROCESS);
        if (result != NGGC_OK) {
            goto done;
        }
        active_phase = NGGC_PHASE_REFERENCE_PROCESS;
        if (heap->reference_process_packet_count != UINT64_MAX) {
            ++heap->reference_process_packet_count;
        }
        mark_ctx.heap = heap;
        mark_ctx.stack = &heap->mutator_assist_mark_stack;
        mark_ctx.result = NGGC_OK;
        result = nggc_reference_process_bounded_locked(
            heap, &mark_ctx, mark_budget_items, &reference_processed,
            &reference_complete);
        if (result != NGGC_OK) {
            goto done;
        }
        heap->reference_process_items_processed =
            nggc_saturating_add_u64_size(
                heap->reference_process_items_processed,
                reference_processed);
        if (reference_processed == 0u && !reference_complete &&
            heap->reference_process_no_progress_count != UINT64_MAX) {
            ++heap->reference_process_no_progress_count;
        }
        progress = progress || reference_processed != 0u || reference_complete;
        result = nggc_phase_end_locked(heap, NGGC_PHASE_REFERENCE_PROCESS);
        if (result != NGGC_OK) {
            goto done;
        }
        active_phase = NGGC_PHASE_IDLE;
        if (reference_complete) {
            heap->mutator_assist_state =
                NGGC_MUTATOR_ASSIST_STATE_SWEEP;
            heap->mutator_assist_sweep_cursor =
                (uintptr_t)heap->gc_arena_base;
            heap->mutator_assist_reference_kind = 0u;
            heap->mutator_assist_reference_queue_scan_cursor = 0u;
            heap->mutator_assist_reference_queue_scan_end = 0u;
            heap->mutator_assist_reference_queue = 0;
            heap->mutator_assist_reference_queue_object_id = 0u;
            heap->mutator_assist_reference_queue_member = 0;
            heap->mutator_assist_reference_queue_remaining = 0u;
        }
    }

    if (heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_SWEEP) {
        result = nggc_phase_begin_locked(heap, NGGC_PHASE_SWEEP);
        if (result != NGGC_OK) {
            goto done;
        }
        active_phase = NGGC_PHASE_SWEEP;
        result = nggc_assist_sweep_packet_locked(
            heap, sweep_budget_bytes, &work_bytes, &reclaimed_bytes,
            &complete);
        if (result != NGGC_OK) {
            goto done;
        }
        heap->mutator_assist_sweep_bytes_processed =
            nggc_saturating_add_u64_size(
                heap->mutator_assist_sweep_bytes_processed, work_bytes);
        nggc_assist_repay_work_locked(heap, assist_thread, work_bytes);
        progress = work_bytes != 0u || reclaimed_bytes != 0u;
        result = nggc_phase_end_locked(heap, NGGC_PHASE_SWEEP);
        if (result != NGGC_OK) {
            goto done;
        }
        active_phase = NGGC_PHASE_IDLE;
        if (complete) {
            result = nggc_rebuild_free_list_locked(heap);
            if (result != NGGC_OK) {
                goto done;
            }
            if (heap->collection_count != UINT64_MAX) {
                ++heap->collection_count;
            }
            nggc_assist_reset_locked(heap);
            progress = 1;
        }
    }
    result = NGGC_OK;

done:
    if (active_phase != NGGC_PHASE_IDLE) {
        (void)nggc_phase_end_locked(heap, active_phase);
    }
    heap->collect_safepoint_epoch = 0u;
    if (result != NGGC_OK) {
        nggc_assist_reset_locked(heap);
    }
    if (progress) {
        if (heap->mutator_assist_progress_count != UINT64_MAX) {
            ++heap->mutator_assist_progress_count;
        }
        if (assist_thread && nggc_thread_is_registered(heap, assist_thread)) {
            assist_thread->mutator_assist_no_progress_streak = 0u;
            assist_thread->mutator_assist_backoff_remaining = 0u;
            if (assist_thread->mutator_assist_progress_count != UINT64_MAX) {
                ++assist_thread->mutator_assist_progress_count;
            }
            if (heap->mutator_assist_thread_progress_count != UINT64_MAX) {
                ++heap->mutator_assist_thread_progress_count;
            }
        }
    } else if (heap->mutator_assist_no_progress_count != UINT64_MAX) {
        ++heap->mutator_assist_no_progress_count;
        if (assist_thread && nggc_thread_is_registered(heap, assist_thread)) {
            if (assist_thread->mutator_assist_no_progress_streak <
                NGGC_ASSIST_NO_PROGRESS_BACKOFF_THRESHOLD) {
                ++assist_thread->mutator_assist_no_progress_streak;
            }
            if (assist_thread->mutator_assist_no_progress_streak >=
                    NGGC_ASSIST_NO_PROGRESS_BACKOFF_THRESHOLD &&
                assist_thread->mutator_assist_backoff_remaining == 0u) {
                assist_thread->mutator_assist_backoff_remaining =
                    NGGC_ASSIST_BACKOFF_PACKETS;
            }
            if (assist_thread->mutator_assist_no_progress_count !=
                UINT64_MAX) {
                ++assist_thread->mutator_assist_no_progress_count;
            }
            if (heap->mutator_assist_thread_no_progress_count !=
                UINT64_MAX) {
                ++heap->mutator_assist_thread_no_progress_count;
            }
        }
    }
    return result;
}

static nggc_result nggc_rebuild_free_list_locked(nggc_heap *heap) {
    nggc_object_header *free_list = 0;
    nggc_object_header *large_free_bins[NGGC_LARGE_FREE_BIN_COUNT] = {0};
    uint32_t large_free_bin_mask = 0u;
    uintptr_t cursor = (uintptr_t)heap->gc_arena_base;
    size_t free_object_count = 0;
    size_t free_bytes = 0;
    size_t large_free_object_count = 0;
    size_t large_free_bytes = 0;

    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        uintptr_t object_end;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        object_end = block.end;
        if (block.state == NGGC_OBJECT_FREE) {
            uint32_t free_flags;
            int is_large;
            if ((header->flags & ~NGGC_OBJECT_FLAG_LARGE) != 0u) {
                return NGGC_ERR_CORRUPTION;
            }
            free_flags = header->flags & NGGC_OBJECT_FLAG_LARGE;
            is_large = free_flags != 0u;
            if (is_large &&
                !nggc_region_humongous_run_matches_locked(
                    heap, (uintptr_t)header,
                    header->alloc_size)) {
                return NGGC_ERR_CORRUPTION;
            }
            while (!is_large && object_end < heap->arena_cursor) {
                nggc_block_view next_block;
                nggc_object_header *next;
                if (nggc_block_view_locked(heap, object_end,
                                           &next_block) != NGGC_OK) {
                    return NGGC_ERR_CORRUPTION;
                }
                if (next_block.state != NGGC_OBJECT_FREE ||
                    (next_block.header->flags &
                     ~NGGC_OBJECT_FLAG_LARGE) != 0u ||
                    nggc_free_block_is_large(next_block.header) !=
                        is_large) {
                    break;
                }
                {
                    size_t left_index =
                        (size_t)((object_end - 1u -
                                  (uintptr_t)heap->gc_arena_base) /
                                 heap->region_size);
                    size_t right_index =
                        (size_t)((object_end -
                                  (uintptr_t)heap->gc_arena_base) /
                                 heap->region_size);
                    if (left_index != right_index &&
                        (left_index >= heap->region_count ||
                         right_index >= heap->region_count ||
                         heap->regions[left_index].generation !=
                             heap->regions[right_index].generation ||
                         heap->regions[left_index].placement !=
                             heap->regions[right_index].placement)) {
                        break;
                    }
                }
                next = next_block.header;
                if (header->alloc_size > SIZE_MAX - next->alloc_size) {
                    return NGGC_ERR_CORRUPTION;
                }
                header->alloc_size += next->alloc_size;
                object_end = next_block.end;
            }

            header->type_id = NGGC_TYPE_NONE;
            header->size_class = 0u;
            header->flags = free_flags;
            header->payload_size = 0;
            header->object_id = 0u;
            atomic_store_explicit(&header->pin_count, 0u,
                                  memory_order_release);
            header->mark_epoch = 0u;
            header->generation = NGGC_REGION_GENERATION_NONE;
            header->young_age = 0u;
            nggc_payload_lock_init(header);
            if (is_large) {
                size_t bin_index;
                nggc_object_header *previous = 0;
                nggc_object_header *current;
                if (!nggc_large_free_bin_index_locked(
                        heap, header->alloc_size, &bin_index)) {
                    return NGGC_ERR_CORRUPTION;
                }
                current = large_free_bins[bin_index];
                while (current &&
                       (current->alloc_size < header->alloc_size ||
                        (current->alloc_size == header->alloc_size &&
                         (uintptr_t)current < (uintptr_t)header))) {
                    previous = current;
                    current = (nggc_object_header *)
                        nggc_forwarding_load_relaxed(current);
                }
                nggc_forwarding_store_free_next(header, current);
                if (previous) {
                    nggc_forwarding_store_free_next(previous, header);
                } else {
                    large_free_bins[bin_index] = header;
                }
                large_free_bin_mask |=
                    (uint32_t)1u << bin_index;
            } else {
                nggc_forwarding_store_free_next(header, free_list);
                free_list = header;
            }
            ++free_object_count;
            if (free_bytes > SIZE_MAX - header->alloc_size) {
                return NGGC_ERR_CORRUPTION;
            }
            free_bytes += header->alloc_size;
            if (is_large) {
                if (large_free_object_count == SIZE_MAX ||
                    large_free_bytes > SIZE_MAX - header->alloc_size) {
                    return NGGC_ERR_CORRUPTION;
                }
                ++large_free_object_count;
                large_free_bytes += header->alloc_size;
            }
        }

        cursor = object_end;
    }

    heap->free_list = free_list;
    memcpy(heap->large_free_bins, large_free_bins,
           sizeof(large_free_bins));
    heap->large_free_bin_mask = large_free_bin_mask;
    heap->free_object_count = free_object_count;
    heap->free_bytes = free_bytes;
    heap->large_free_object_count = large_free_object_count;
    heap->large_free_bytes = large_free_bytes;
    return NGGC_OK;
}

static uintptr_t nggc_region_boundary_ceil_locked(const nggc_heap *heap,
                                                   uintptr_t addr) {
    uintptr_t base;
    size_t offset;
    size_t index;
    if (!heap || addr <= (uintptr_t)heap->gc_arena_base) {
        return heap ? (uintptr_t)heap->gc_arena_base : 0u;
    }
    if (addr >= heap->arena_end) {
        return heap->arena_end;
    }
    base = (uintptr_t)heap->gc_arena_base;
    offset = (size_t)(addr - base);
    index = offset / heap->region_size;
    if ((offset % heap->region_size) != 0u) {
        ++index;
    }
    return index >= heap->region_count ? heap->arena_end
                                        : heap->regions[index].start;
}

static int nggc_region_tail_decommit_candidate_locked(
    const nggc_region_meta *region) {
    if (!region || region->decommitted) {
        return 0;
    }
    if (region->live_bytes != 0u || region->reserved_bytes != 0u ||
        region->quarantined_bytes != 0u ||
        region->live_object_count != 0u ||
        region->quarantined_object_count != 0u ||
        region->large_object_count != 0u ||
        region->pinnable_object_count != 0u ||
        region->pinned_object_count != 0u ||
        region->non_movable_object_count != 0u ||
        region->in_collection_set != 0u ||
        region->allocation_owner != 0u ||
        region->tlab_owner != 0 ||
        region->tlab_lease_epoch != 0u ||
        region->tlab_reserved_start != 0u ||
        region->tlab_published_top != 0u ||
        region->tlab_reserved_end != 0u ||
        region->tlab_state != NGGC_TLAB_INACTIVE ||
        !nggc_region_generation_valid(region->generation)) {
        return 0;
    }
    if (region->committed_bytes == 0u) {
        return region->free_bytes == 0u &&
               region->state == NGGC_REGION_STATE_INACTIVE &&
               region->generation == NGGC_REGION_GENERATION_NONE &&
               region->placement == NGGC_REGION_PLACEMENT_NONE &&
               region->humongous_role == NGGC_REGION_HUMONGOUS_NONE &&
               region->humongous_run_offset == 0u &&
               region->humongous_run_length == 0u &&
               region->flags == 0u;
    }
    return region->free_bytes == region->committed_bytes &&
           region->state == NGGC_REGION_STATE_ACTIVE &&
           (region->flags & ~NGGC_REGION_FLAG_HAS_FREE) == 0u;
}

static int nggc_region_humongous_decommit_span_valid_locked(
    const nggc_heap *heap, size_t start_index, size_t end_index) {
    size_t i = start_index;
    if (!heap || !heap->regions || start_index > end_index ||
        end_index > heap->region_count) {
        return 0;
    }
    while (i < end_index) {
        const nggc_region_meta *region = &heap->regions[i];
        if (region->humongous_role == NGGC_REGION_HUMONGOUS_NONE) {
            if (region->humongous_run_offset != 0u ||
                region->humongous_run_length != 0u) {
                return 0;
            }
            ++i;
            continue;
        }
        if (region->humongous_role != NGGC_REGION_HUMONGOUS_START ||
            region->humongous_run_offset != 0u ||
            region->humongous_run_length < 2u ||
            region->humongous_run_length > end_index - i) {
            return 0;
        }
        for (uint32_t offset = 0u;
             offset < region->humongous_run_length; ++offset) {
            const nggc_region_meta *part =
                &heap->regions[i + (size_t)offset];
            uint32_t expected_role =
                offset == 0u ? NGGC_REGION_HUMONGOUS_START
                             : NGGC_REGION_HUMONGOUS_CONTINUATION;
            if (part->humongous_role != expected_role ||
                part->humongous_run_offset != offset ||
                part->humongous_run_length !=
                    region->humongous_run_length) {
                return 0;
            }
        }
        i += region->humongous_run_length;
    }
    return 1;
}

static void nggc_region_mark_decommitted_locked(nggc_heap *heap,
                                                size_t index) {
    nggc_region_meta *region = &heap->regions[index];
    size_t span = nggc_region_span_size(region);
    if (region->committed_bytes != 0u && heap->active_region_count != 0u) {
        --heap->active_region_count;
    }
    region->allocation_cursor = region->start;
    region->allocation_owner = 0u;
    region->committed_bytes = 0u;
    region->live_bytes = 0u;
    region->free_bytes = 0u;
    region->reserved_bytes = 0u;
    region->quarantined_bytes = 0u;
    region->live_object_count = 0u;
    region->quarantined_object_count = 0u;
    region->large_object_count = 0u;
    region->pinnable_object_count = 0u;
    region->pinned_object_count = 0u;
    region->non_movable_object_count = 0u;
    region->state = NGGC_REGION_STATE_INACTIVE;
    region->generation = NGGC_REGION_GENERATION_NONE;
    region->flags = 0u;
    region->in_collection_set = 0u;
    region->placement = NGGC_REGION_PLACEMENT_NONE;
    region->humongous_role = NGGC_REGION_HUMONGOUS_NONE;
    region->humongous_run_offset = 0u;
    region->humongous_run_length = 0u;
    region->decommitted_free_next = 0u;
    region->tlab_owner = 0;
    region->tlab_lease_epoch = 0u;
    region->tlab_reserved_start = 0u;
    region->tlab_published_top = 0u;
    region->tlab_reserved_end = 0u;
    region->tlab_state = NGGC_TLAB_INACTIVE;
    if (!region->decommitted) {
        region->decommitted = 1u;
        ++heap->decommitted_region_count;
        heap->region_decommitted_bytes =
            nggc_saturating_add_size(heap->region_decommitted_bytes, span);
        if (heap->region_decommit_count != UINT64_MAX) {
            ++heap->region_decommit_count;
        }
    }
}

static nggc_result nggc_region_restore_committed_empty_locked(
    nggc_heap *heap, size_t index) {
    nggc_region_meta *region = &heap->regions[index];
    size_t span = nggc_region_span_size(region);
    if (!region->decommitted || span == 0u ||
        heap->decommitted_region_count == 0u ||
        heap->region_decommitted_bytes < span) {
        return NGGC_ERR_CORRUPTION;
    }
    region->allocation_cursor = region->start;
    region->allocation_owner = 0u;
    region->committed_bytes = 0u;
    region->live_bytes = 0u;
    region->free_bytes = 0u;
    region->reserved_bytes = 0u;
    region->quarantined_bytes = 0u;
    region->live_object_count = 0u;
    region->quarantined_object_count = 0u;
    region->large_object_count = 0u;
    region->pinnable_object_count = 0u;
    region->pinned_object_count = 0u;
    region->non_movable_object_count = 0u;
    region->state = NGGC_REGION_STATE_INACTIVE;
    region->generation = NGGC_REGION_GENERATION_NONE;
    region->flags = 0u;
    region->in_collection_set = 0u;
    region->decommitted = 0u;
    region->placement = NGGC_REGION_PLACEMENT_NONE;
    region->humongous_role = NGGC_REGION_HUMONGOUS_NONE;
    region->humongous_run_offset = 0u;
    region->humongous_run_length = 0u;
    region->decommitted_free_next = 0u;
    region->tlab_owner = 0;
    region->tlab_lease_epoch = 0u;
    region->tlab_reserved_start = 0u;
    region->tlab_published_top = 0u;
    region->tlab_reserved_end = 0u;
    region->tlab_state = NGGC_TLAB_INACTIVE;
    --heap->decommitted_region_count;
    heap->region_decommitted_bytes -= span;
    if (heap->region_recommit_count != UINT64_MAX) {
        ++heap->region_recommit_count;
    }
    return NGGC_OK;
}

static nggc_result nggc_alloc_from_decommitted_large_bins_locked(
    nggc_heap *heap, size_t requested_bin, size_t requested_size,
    uint32_t target_generation, uint32_t object_flags,
    nggc_object_header **out_header, size_t *out_alloc_size,
    int *out_found) {
    uint32_t target_placement;
    size_t visited_runs = 0u;
    if (!heap || !out_header || !out_alloc_size || !out_found ||
        requested_bin >= NGGC_LARGE_FREE_BIN_COUNT ||
        target_generation != NGGC_REGION_GENERATION_SPECIAL ||
        requested_size <= heap->region_size ||
        (requested_size % heap->region_size) != 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_header = 0;
    *out_alloc_size = requested_size;
    *out_found = 0;
    target_placement = nggc_region_target_placement(object_flags);
    if (target_placement != NGGC_REGION_PLACEMENT_MOVABLE_SPECIAL &&
        target_placement != NGGC_REGION_PLACEMENT_PINNED_SPECIAL) {
        return NGGC_ERR_CORRUPTION;
    }
    for (size_t bin_index = requested_bin;
         bin_index < NGGC_LARGE_FREE_BIN_COUNT; ++bin_index) {
        size_t encoded = heap->decommitted_large_free_bins[bin_index];
        size_t previous_index = SIZE_MAX;
        int has_mask =
            (heap->decommitted_large_free_bin_mask &
             ((uint32_t)1u << bin_index)) != 0u;
        if ((encoded != 0u) != has_mask) {
            return NGGC_ERR_CORRUPTION;
        }
        while (encoded != 0u) {
            size_t start_index;
            nggc_region_meta *start_region;
            size_t next_encoded;
            uint32_t run_length;
            uint32_t placement;
            size_t run_bytes;
            size_t actual_bin;
            size_t remaining_bytes;
            size_t reused_bytes;
            size_t recommit_regions;
            size_t remaining_regions;
            if (visited_runs >= heap->interior_decommitted_run_count) {
                return NGGC_ERR_CORRUPTION;
            }
            ++visited_runs;
            if (!nggc_decommitted_run_decode_index(
                    heap, encoded, &start_index)) {
                return NGGC_ERR_CORRUPTION;
            }
            start_region = &heap->regions[start_index];
            next_encoded = start_region->decommitted_free_next;
            if (heap->large_free_probe_count != UINT64_MAX) {
                ++heap->large_free_probe_count;
            }
            if (heap->large_free_decommitted_probe_count != UINT64_MAX) {
                ++heap->large_free_decommitted_probe_count;
            }
            if (!nggc_interior_decommitted_run_locked(
                    heap, start_index, &run_length, &placement) ||
                run_length > SIZE_MAX / heap->region_size) {
                return NGGC_ERR_CORRUPTION;
            }
            run_bytes = (size_t)run_length * heap->region_size;
            if (!nggc_large_free_bin_index_locked(
                    heap, run_bytes, &actual_bin) ||
                actual_bin != bin_index) {
                return NGGC_ERR_CORRUPTION;
            }
            if (run_bytes < requested_size) {
                if (heap->large_free_size_reject_count != UINT64_MAX) {
                    ++heap->large_free_size_reject_count;
                }
                previous_index = start_index;
                encoded = next_encoded;
                continue;
            }
            if (placement != target_placement) {
                if (heap->large_free_placement_reject_count !=
                    UINT64_MAX) {
                    ++heap->large_free_placement_reject_count;
                }
                previous_index = start_index;
                encoded = next_encoded;
                continue;
            }
            remaining_bytes = run_bytes - requested_size;
            reused_bytes =
                heap->region_size <= SIZE_MAX / 2u &&
                        remaining_bytes >= heap->region_size * 2u
                    ? requested_size
                    : run_bytes;
            recommit_regions = reused_bytes / heap->region_size;
            remaining_regions =
                (size_t)run_length - recommit_regions;
            if (recommit_regions == 0u ||
                recommit_regions > run_length ||
                (remaining_regions != 0u && remaining_regions < 2u) ||
                heap->decommitted_region_count < recommit_regions ||
                heap->region_decommitted_bytes < reused_bytes ||
                heap->interior_decommitted_run_count == 0u ||
                heap->interior_decommitted_region_count < run_length ||
                heap->interior_decommitted_bytes < run_bytes) {
                return NGGC_ERR_CORRUPTION;
            }
            if (nggc_region_backend_vm_locked(
                    heap, (void *)start_region->start,
                    reused_bytes, 0) != NGGC_OK) {
                if (heap->region_recommit_failure_count != UINT64_MAX) {
                    ++heap->region_recommit_failure_count;
                }
                return NGGC_ERR_BACKEND;
            }
            if (previous_index != SIZE_MAX) {
                heap->regions[previous_index].decommitted_free_next =
                    next_encoded;
            } else {
                heap->decommitted_large_free_bins[bin_index] =
                    next_encoded;
            }
            if (heap->decommitted_large_free_bins[bin_index] == 0u) {
                heap->decommitted_large_free_bin_mask &=
                    ~((uint32_t)1u << bin_index);
            }
            --heap->interior_decommitted_run_count;
            heap->interior_decommitted_region_count -= run_length;
            heap->interior_decommitted_bytes -= run_bytes;
            for (size_t i = 0u; i < recommit_regions; ++i) {
                if (nggc_region_restore_committed_empty_locked(
                        heap, start_index + i) != NGGC_OK) {
                    return NGGC_ERR_CORRUPTION;
                }
                heap->regions[start_index + i].allocation_cursor =
                    heap->regions[start_index + i].end;
            }
            if (remaining_regions != 0u) {
                size_t remainder_start =
                    start_index + recommit_regions;
                if (remaining_regions > UINT32_MAX) {
                    return NGGC_ERR_CORRUPTION;
                }
                for (size_t i = 0u; i < remaining_regions; ++i) {
                    nggc_region_meta *region =
                        &heap->regions[remainder_start + i];
                    region->humongous_role =
                        i == 0u ? NGGC_REGION_HUMONGOUS_START
                                : NGGC_REGION_HUMONGOUS_CONTINUATION;
                    region->humongous_run_offset = (uint32_t)i;
                    region->humongous_run_length =
                        (uint32_t)remaining_regions;
                    region->decommitted_free_next = 0u;
                }
                ++heap->interior_decommitted_run_count;
                heap->interior_decommitted_region_count +=
                    remaining_regions;
                heap->interior_decommitted_bytes +=
                    remaining_regions * heap->region_size;
                if (nggc_decommitted_large_bin_link_locked(
                        heap, remainder_start) != NGGC_OK) {
                    return NGGC_ERR_CORRUPTION;
                }
            }
            if (heap->interior_run_recommit_count != UINT64_MAX) {
                ++heap->interior_run_recommit_count;
            }
            if (nggc_region_assign_allocation_range_locked(
                    heap, start_region->start, reused_bytes,
                    target_generation, object_flags) != NGGC_OK ||
                nggc_region_assign_humongous_run_locked(
                    heap, start_region->start, reused_bytes) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            *out_header =
                (nggc_object_header *)start_region->start;
            *out_alloc_size = reused_bytes;
            *out_found = 1;
            return NGGC_OK;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_region_validate_interior_decommit_run_locked(
    nggc_heap *heap, size_t start_index, uint32_t run_length,
    uint32_t placement) {
    size_t bytes;
    if (!heap || !heap->regions || heap->region_size == 0u ||
        start_index >= heap->region_count || run_length < 2u ||
        run_length > heap->region_count - start_index ||
        run_length > SIZE_MAX / heap->region_size ||
        (placement != NGGC_REGION_PLACEMENT_MOVABLE_SPECIAL &&
         placement != NGGC_REGION_PLACEMENT_PINNED_SPECIAL) ||
        heap->active_region_count < run_length) {
        return NGGC_ERR_CORRUPTION;
    }
    bytes = (size_t)run_length * heap->region_size;
    if (heap->interior_decommitted_run_count == SIZE_MAX ||
        heap->interior_decommitted_region_count > SIZE_MAX - run_length ||
        heap->interior_decommitted_bytes > SIZE_MAX - bytes ||
        heap->decommitted_region_count > SIZE_MAX - run_length ||
        heap->region_decommitted_bytes > SIZE_MAX - bytes) {
        return NGGC_ERR_CORRUPTION;
    }
    for (uint32_t offset = 0u; offset < run_length; ++offset) {
        const nggc_region_meta *region =
            &heap->regions[start_index + (size_t)offset];
        size_t span = nggc_region_span_size(region);
        uint32_t expected_role =
            offset == 0u ? NGGC_REGION_HUMONGOUS_START
                         : NGGC_REGION_HUMONGOUS_CONTINUATION;
        if (span != heap->region_size || region->decommitted ||
            region->committed_bytes != span ||
            region->free_bytes != span || region->live_bytes != 0u ||
            region->reserved_bytes != 0u ||
            region->quarantined_bytes != 0u ||
            region->live_object_count != 0u ||
            region->quarantined_object_count != 0u ||
            region->large_object_count != 0u ||
            region->pinnable_object_count != 0u ||
            region->pinned_object_count != 0u ||
            region->non_movable_object_count != 0u ||
            region->state != NGGC_REGION_STATE_ACTIVE ||
            region->generation != NGGC_REGION_GENERATION_SPECIAL ||
            region->flags != NGGC_REGION_FLAG_HAS_FREE ||
            region->in_collection_set != 0u ||
            region->placement != placement ||
            region->humongous_role != expected_role ||
            region->humongous_run_offset != offset ||
            region->humongous_run_length != run_length ||
            region->decommitted_free_next != 0u ||
            region->allocation_cursor != region->end ||
            region->allocation_owner != 0u ||
            region->tlab_owner != 0 ||
            region->tlab_lease_epoch != 0u ||
            region->tlab_reserved_start != 0u ||
            region->tlab_published_top != 0u ||
            region->tlab_reserved_end != 0u ||
            region->tlab_state != NGGC_TLAB_INACTIVE) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_region_mark_interior_decommitted_run_locked(
    nggc_heap *heap, size_t start_index, uint32_t run_length,
    uint32_t placement) {
    size_t bytes;
    if (nggc_region_validate_interior_decommit_run_locked(
            heap, start_index, run_length, placement) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    bytes = (size_t)run_length * heap->region_size;
    for (uint32_t offset = 0u; offset < run_length; ++offset) {
        nggc_region_meta *region =
            &heap->regions[start_index + (size_t)offset];
        region->allocation_cursor = region->start;
        region->allocation_owner = 0u;
        region->committed_bytes = 0u;
        region->live_bytes = 0u;
        region->free_bytes = 0u;
        region->reserved_bytes = 0u;
        region->quarantined_bytes = 0u;
        region->live_object_count = 0u;
        region->quarantined_object_count = 0u;
        region->large_object_count = 0u;
        region->pinnable_object_count = 0u;
        region->pinned_object_count = 0u;
        region->non_movable_object_count = 0u;
        region->state = NGGC_REGION_STATE_INACTIVE;
        region->generation = NGGC_REGION_GENERATION_NONE;
        region->flags = 0u;
        region->in_collection_set = 0u;
        region->decommitted = 1u;
        region->placement = placement;
        region->humongous_role =
            offset == 0u ? NGGC_REGION_HUMONGOUS_START
                         : NGGC_REGION_HUMONGOUS_CONTINUATION;
        region->humongous_run_offset = offset;
        region->humongous_run_length = run_length;
        region->decommitted_free_next = 0u;
        region->tlab_owner = 0;
        region->tlab_lease_epoch = 0u;
        region->tlab_reserved_start = 0u;
        region->tlab_published_top = 0u;
        region->tlab_reserved_end = 0u;
        region->tlab_state = NGGC_TLAB_INACTIVE;
        --heap->active_region_count;
        ++heap->decommitted_region_count;
        heap->region_decommitted_bytes = nggc_saturating_add_size(
            heap->region_decommitted_bytes, heap->region_size);
        if (heap->region_decommit_count != UINT64_MAX) {
            ++heap->region_decommit_count;
        }
    }
    ++heap->interior_decommitted_run_count;
    heap->interior_decommitted_region_count += run_length;
    heap->interior_decommitted_bytes += bytes;
    if (heap->interior_run_decommit_count != UINT64_MAX) {
        ++heap->interior_run_decommit_count;
    }
    return nggc_decommitted_large_bin_link_locked(heap, start_index);
}

static nggc_result
nggc_region_collapse_tail_decommitted_humongous_locked(
    nggc_heap *heap, size_t *out_collapsed_runs) {
    size_t collapsed_runs = 0u;
    if (!heap || !heap->regions || heap->region_size == 0u ||
        !out_collapsed_runs) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_collapsed_runs = 0u;
    while (heap->arena_cursor > (uintptr_t)heap->gc_arena_base) {
        size_t end_index;
        size_t last_index;
        size_t start_index;
        nggc_region_meta *last;
        uint32_t run_length;
        size_t run_bytes;
        uintptr_t run_end;
        if (((heap->arena_cursor -
              (uintptr_t)heap->gc_arena_base) %
             heap->region_size) != 0u) {
            break;
        }
        end_index =
            (size_t)((heap->arena_cursor -
                      (uintptr_t)heap->gc_arena_base) /
                     heap->region_size);
        if (end_index == 0u || end_index > heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        last_index = end_index - 1u;
        last = &heap->regions[last_index];
        if (!last->decommitted ||
            last->humongous_role == NGGC_REGION_HUMONGOUS_NONE) {
            break;
        }
        if (last->humongous_role !=
                NGGC_REGION_HUMONGOUS_CONTINUATION ||
            last->humongous_run_offset == 0u ||
            last->humongous_run_offset >=
                last->humongous_run_length ||
            last->humongous_run_offset + 1u !=
                last->humongous_run_length ||
            (size_t)last->humongous_run_offset > last_index) {
            return NGGC_ERR_CORRUPTION;
        }
        start_index =
            last_index - (size_t)last->humongous_run_offset;
        if (!nggc_interior_decommitted_run_locked(
                heap, start_index, &run_length, 0) ||
            run_length != last->humongous_run_length ||
            run_length > SIZE_MAX / heap->region_size) {
            return NGGC_ERR_CORRUPTION;
        }
        run_bytes = (size_t)run_length * heap->region_size;
        if (!nggc_range_end(heap->regions[start_index].start,
                            run_bytes, &run_end) ||
            run_end != heap->arena_cursor ||
            heap->interior_decommitted_run_count == 0u ||
            heap->interior_decommitted_region_count < run_length ||
            heap->interior_decommitted_bytes < run_bytes) {
            return NGGC_ERR_CORRUPTION;
        }
        if (nggc_decommitted_large_bin_unlink_locked(
                heap, start_index) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        for (uint32_t offset = 0u; offset < run_length; ++offset) {
            nggc_region_meta *region =
                &heap->regions[start_index + (size_t)offset];
            region->placement = NGGC_REGION_PLACEMENT_NONE;
            region->humongous_role = NGGC_REGION_HUMONGOUS_NONE;
            region->humongous_run_offset = 0u;
            region->humongous_run_length = 0u;
            region->decommitted_free_next = 0u;
        }
        --heap->interior_decommitted_run_count;
        heap->interior_decommitted_region_count -= run_length;
        heap->interior_decommitted_bytes -= run_bytes;
        heap->arena_cursor = heap->regions[start_index].start;
        if (heap->interior_run_tail_collapse_count != UINT64_MAX) {
            ++heap->interior_run_tail_collapse_count;
        }
        heap->interior_run_tail_collapsed_region_count =
            nggc_saturating_add_u64_size(
                heap->interior_run_tail_collapsed_region_count,
                run_length);
        heap->interior_run_tail_collapsed_bytes =
            nggc_saturating_add_u64_size(
                heap->interior_run_tail_collapsed_bytes, run_bytes);
        ++collapsed_runs;
    }
    if (collapsed_runs != 0u &&
        nggc_region_refresh_allocation_owner_locked(heap) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_collapsed_runs = collapsed_runs;
    return NGGC_OK;
}

static nggc_result nggc_region_find_tail_free_start_locked(
    nggc_heap *heap, uintptr_t *out_start, int *out_found) {
    uintptr_t cursor;
    uintptr_t free_run_start;
    int in_free_run = 0;
    if (!heap || !out_start || !out_found) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_start = heap->arena_cursor;
    *out_found = 0;
    if (heap->arena_cursor == (uintptr_t)heap->gc_arena_base) {
        *out_start = (uintptr_t)heap->gc_arena_base;
        *out_found = 1;
        return NGGC_OK;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    free_run_start = heap->arena_cursor;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_result result = nggc_block_view_locked(heap, cursor, &block);
        if (result != NGGC_OK) {
            return result;
        }
        if (block.state == NGGC_OBJECT_FREE) {
            if (!in_free_run) {
                free_run_start = block.start;
                in_free_run = 1;
            }
        } else {
            free_run_start = heap->arena_cursor;
            in_free_run = 0;
        }
        cursor = block.end;
    }
    if (in_free_run) {
        *out_start = free_run_start;
        *out_found = 1;
    }
    return NGGC_OK;
}

static nggc_result nggc_region_decommit_empty_tail_once_locked(
    nggc_heap *heap) {
    nggc_backend_stats backend_stats;
    uintptr_t old_cursor;
    uintptr_t tail_free_start;
    uintptr_t reclaim_start;
    uintptr_t new_cursor;
    size_t start_index;
    size_t run_end_index;
    size_t active_to_remove = 0u;
    size_t run_bytes;
    int has_tail_free;
    nggc_result result;

    if (!heap || !heap->regions || heap->region_count == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    old_cursor = heap->arena_cursor;
    result = nggc_region_find_tail_free_start_locked(
        heap, &tail_free_start, &has_tail_free);
    if (result != NGGC_OK) {
        return result;
    }
    reclaim_start = nggc_region_boundary_ceil_locked(heap, old_cursor);
    new_cursor = old_cursor;
    if (has_tail_free) {
        uintptr_t free_boundary =
            nggc_region_boundary_ceil_locked(heap, tail_free_start);
        if (free_boundary <= old_cursor && free_boundary < reclaim_start) {
            reclaim_start = free_boundary;
            new_cursor = free_boundary;
        }
    }
    if (reclaim_start >= heap->arena_end) {
        return NGGC_OK;
    }
    if (((reclaim_start - (uintptr_t)heap->gc_arena_base) %
         heap->region_size) != 0u) {
        return NGGC_OK;
    }
    if (new_cursor < old_cursor && tail_free_start < new_cursor) {
        uintptr_t trim_cursor = tail_free_start;
        while (trim_cursor < new_cursor) {
            nggc_block_view block;
            size_t prefix;
            if (nggc_block_view_locked(heap, trim_cursor, &block) !=
                    NGGC_OK ||
                block.state != NGGC_OBJECT_FREE) {
                return NGGC_ERR_CORRUPTION;
            }
            if (block.end <= new_cursor) {
                trim_cursor = block.end;
                continue;
            }
            prefix = (size_t)(new_cursor - block.start);
            if (prefix < NGGC_OBJECT_HEADER_BYTES ||
                (prefix % NGGC_OBJECT_ALIGNMENT) != 0u) {
                return NGGC_OK;
            }
            break;
        }
    }

    start_index = (size_t)((reclaim_start -
                            (uintptr_t)heap->gc_arena_base) /
                           heap->region_size);
    if (start_index >= heap->region_count ||
        heap->regions[start_index].decommitted) {
        return NGGC_OK;
    }
    run_end_index = start_index;
    while (run_end_index < heap->region_count &&
           !heap->regions[run_end_index].decommitted) {
        const nggc_region_meta *region = &heap->regions[run_end_index];
        if (!nggc_region_tail_decommit_candidate_locked(region)) {
            return NGGC_OK;
        }
        if (region->committed_bytes != 0u) {
            ++active_to_remove;
        }
        ++run_end_index;
    }
    if (active_to_remove > heap->active_region_count) {
        return NGGC_ERR_CORRUPTION;
    }
    if (!nggc_region_humongous_decommit_span_valid_locked(
            heap, start_index, run_end_index)) {
        return NGGC_ERR_CORRUPTION;
    }
    run_bytes =
        (size_t)(heap->regions[run_end_index - 1u].end - reclaim_start);
    memset(&backend_stats, 0, sizeof(backend_stats));
    if (nggc_backend_get_stats(heap->backend, &backend_stats) != NGGC_OK ||
        backend_stats.page_size == 0u ||
        (reclaim_start % backend_stats.page_size) != 0u ||
        (run_bytes % backend_stats.page_size) != 0u) {
        return NGGC_OK;
    }
    for (size_t i = start_index; i < run_end_index; ++i) {
        size_t span = nggc_region_span_size(&heap->regions[i]);
        if ((heap->regions[i].start % backend_stats.page_size) != 0u ||
            span == 0u || (span % backend_stats.page_size) != 0u) {
            return NGGC_OK;
        }
    }
    result = nggc_region_backend_vm_locked(
        heap, (void *)reclaim_start, run_bytes, 1);
    if (result != NGGC_OK) {
        if (heap->region_decommit_failure_count != UINT64_MAX) {
            ++heap->region_decommit_failure_count;
        }
        return NGGC_OK;
    }

    if (new_cursor < old_cursor && tail_free_start < new_cursor) {
        uintptr_t trim_cursor = tail_free_start;
        while (trim_cursor < new_cursor) {
            nggc_block_view block;
            if (nggc_block_view_locked(heap, trim_cursor, &block) !=
                NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            if (block.end > new_cursor) {
                block.header->alloc_size =
                    (size_t)(new_cursor - block.start);
                break;
            }
            trim_cursor = block.end;
        }
    }
    for (size_t i = start_index; i < run_end_index; ++i) {
        nggc_region_mark_decommitted_locked(heap, i);
    }
    heap->arena_cursor = new_cursor;
    result = nggc_region_refresh_allocation_owner_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    return nggc_rebuild_free_list_locked(heap);
}

static nggc_result nggc_region_decommit_empty_tail_locked(nggc_heap *heap) {
    size_t pass_count = 0u;
    if (!heap || !heap->regions) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    for (;;) {
        uintptr_t previous_cursor = heap->arena_cursor;
        size_t collapsed_runs = 0u;
        nggc_result result =
            nggc_region_decommit_empty_tail_once_locked(heap);
        if (result != NGGC_OK) {
            return result;
        }
        result =
            nggc_region_collapse_tail_decommitted_humongous_locked(
                heap, &collapsed_runs);
        if (result != NGGC_OK) {
            return result;
        }
        if (heap->arena_cursor == previous_cursor) {
            return NGGC_OK;
        }
        if (heap->arena_cursor > previous_cursor) {
            return NGGC_ERR_CORRUPTION;
        }
        if (collapsed_runs == 0u) {
            return NGGC_OK;
        }
        if (pass_count >= heap->region_count) {
            return NGGC_ERR_CORRUPTION;
        }
        ++pass_count;
    }
}

static size_t nggc_interior_decommit_smart_threshold_cap_locked(
    const nggc_heap *heap) {
    size_t cap;
    if (!heap) {
        return NGGC_INTERIOR_DECOMMIT_MIN_REGIONS_DEFAULT;
    }
    cap = heap->region_count < NGGC_INTERIOR_DECOMMIT_SMART_MAX_REGIONS
              ? heap->region_count
              : NGGC_INTERIOR_DECOMMIT_SMART_MAX_REGIONS;
    if (cap < NGGC_INTERIOR_DECOMMIT_MIN_REGIONS_DEFAULT) {
        cap = NGGC_INTERIOR_DECOMMIT_MIN_REGIONS_DEFAULT;
    }
    return cap;
}

static void nggc_interior_decommit_smart_snapshot_locked(
    nggc_heap *heap) {
    heap->interior_decommit_smart_sample_recommit_count =
        heap->interior_run_recommit_count;
    heap->interior_decommit_smart_sample_large_allocation_count =
        heap->large_allocation_count;
    heap->interior_decommit_smart_sample_large_reuse_count =
        heap->large_reuse_count;
    heap->interior_decommit_smart_sample_decommit_latency_count =
        heap->region_decommit_latency_sample_count;
    heap->interior_decommit_smart_sample_recommit_latency_count =
        heap->region_recommit_latency_sample_count;
}

static nggc_result nggc_interior_decommit_smart_evaluate_locked(
    nggc_heap *heap) {
    size_t committed_region_count;
    size_t committed_region_bytes;
    size_t threshold_cap;
    size_t next_threshold;
    uint64_t recommit_delta = 0u;
    uint64_t large_allocation_delta = 0u;
    uint64_t large_reuse_delta = 0u;
    uint64_t decommit_latency_sample_delta = 0u;
    uint64_t recommit_latency_sample_delta = 0u;
    uint32_t external_pressure_basis_points = 0u;
    int hot_large_path;
    int high_reclaim_pressure;
    int slow_vm_observed;
    if (!heap || !heap->regions || heap->region_count == 0u ||
        heap->region_size == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->interior_decommit_policy !=
        NGGC_INTERIOR_DECOMMIT_POLICY_SMART) {
        return NGGC_OK;
    }
    threshold_cap =
        nggc_interior_decommit_smart_threshold_cap_locked(heap);
    if (heap->interior_decommit_min_regions < 2u ||
        heap->interior_decommit_min_regions > threshold_cap ||
        heap->decommitted_region_count > heap->region_count ||
        heap->region_decommitted_bytes > heap->gc_arena_size) {
        return NGGC_ERR_CORRUPTION;
    }
    committed_region_count =
        heap->region_count - heap->decommitted_region_count;
    committed_region_bytes =
        heap->gc_arena_size - heap->region_decommitted_bytes;
    if (heap->large_free_bytes > committed_region_bytes) {
        return NGGC_ERR_CORRUPTION;
    }
    heap->interior_decommit_smart_committed_pressure_basis_points =
        nggc_size_ratio_basis_points(committed_region_count,
                                     heap->region_count);
    heap->interior_decommit_smart_large_free_pressure_basis_points =
        nggc_size_ratio_basis_points(heap->large_free_bytes,
                                     committed_region_bytes);
    heap->interior_decommit_smart_pressure_source =
        NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_PORTABLE;
    heap->interior_decommit_smart_external_pressure_valid = 0u;
    heap->interior_decommit_smart_external_pressure_basis_points = 0u;
    heap->interior_decommit_smart_resident_bytes = 0u;
    heap->interior_decommit_smart_memory_limit_bytes = 0u;
    if (nggc_memory_pressure_sample_locked(
            heap, &external_pressure_basis_points)) {
        heap->interior_decommit_smart_pressure_source =
            NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_CALLBACK;
        heap->interior_decommit_smart_external_pressure_valid = 1u;
        heap->interior_decommit_smart_external_pressure_basis_points =
            external_pressure_basis_points;
    }
    heap->interior_decommit_smart_slow_vm_observed = 0u;
    heap->interior_decommit_smart_last_action =
        NGGC_INTERIOR_DECOMMIT_SMART_ACTION_HOLD;
    if (heap->interior_decommit_smart_evaluation_count != UINT64_MAX) {
        ++heap->interior_decommit_smart_evaluation_count;
    }
    if (!heap->interior_decommit_smart_initialized) {
        heap->interior_decommit_smart_initialized = 1u;
        heap->interior_decommit_smart_last_recommit_delta = 0u;
        heap->interior_decommit_smart_last_large_allocation_delta = 0u;
        heap->interior_decommit_smart_last_large_reuse_delta = 0u;
        heap->interior_decommit_smart_last_decommit_latency_sample_delta =
            0u;
        heap->interior_decommit_smart_last_recommit_latency_sample_delta =
            0u;
        nggc_interior_decommit_smart_snapshot_locked(heap);
        return NGGC_OK;
    }
    recommit_delta = nggc_monotonic_delta_u64(
        heap->interior_run_recommit_count,
        heap->interior_decommit_smart_sample_recommit_count);
    large_allocation_delta = nggc_monotonic_delta_u64(
        heap->large_allocation_count,
        heap->interior_decommit_smart_sample_large_allocation_count);
    large_reuse_delta = nggc_monotonic_delta_u64(
        heap->large_reuse_count,
        heap->interior_decommit_smart_sample_large_reuse_count);
    decommit_latency_sample_delta = nggc_monotonic_delta_u64(
        heap->region_decommit_latency_sample_count,
        heap->interior_decommit_smart_sample_decommit_latency_count);
    recommit_latency_sample_delta = nggc_monotonic_delta_u64(
        heap->region_recommit_latency_sample_count,
        heap->interior_decommit_smart_sample_recommit_latency_count);
    heap->interior_decommit_smart_last_recommit_delta =
        recommit_delta;
    heap->interior_decommit_smart_last_large_allocation_delta =
        large_allocation_delta;
    heap->interior_decommit_smart_last_large_reuse_delta =
        large_reuse_delta;
    heap->interior_decommit_smart_last_decommit_latency_sample_delta =
        decommit_latency_sample_delta;
    heap->interior_decommit_smart_last_recommit_latency_sample_delta =
        recommit_latency_sample_delta;
    hot_large_path =
        large_allocation_delta >=
            NGGC_INTERIOR_DECOMMIT_SMART_HOT_LARGE_ALLOCATIONS &&
        large_reuse_delta >=
            NGGC_INTERIOR_DECOMMIT_SMART_HOT_LARGE_REUSES;
    slow_vm_observed =
        heap->interior_decommit_slow_ns_per_region != 0u &&
        ((decommit_latency_sample_delta != 0u &&
          heap->region_decommit_latency_ewma_ns_per_region >=
              heap->interior_decommit_slow_ns_per_region) ||
         (recommit_latency_sample_delta != 0u &&
          heap->region_recommit_latency_ewma_ns_per_region >=
              heap->interior_decommit_slow_ns_per_region));
    heap->interior_decommit_smart_slow_vm_observed =
        slow_vm_observed ? 1u : 0u;
    high_reclaim_pressure =
        (heap->interior_decommit_smart_external_pressure_valid
             ? heap->interior_decommit_smart_external_pressure_basis_points
             : heap->interior_decommit_smart_committed_pressure_basis_points) >=
            NGGC_INTERIOR_DECOMMIT_SMART_HIGH_COMMITTED_BPS ||
        heap->interior_decommit_smart_large_free_pressure_basis_points >=
            NGGC_INTERIOR_DECOMMIT_SMART_HIGH_LARGE_FREE_BPS;

    if (recommit_delta != 0u || hot_large_path || slow_vm_observed) {
        next_threshold =
            heap->interior_decommit_min_regions > threshold_cap / 2u
                ? threshold_cap
                : heap->interior_decommit_min_regions * 2u;
        if (next_threshold > heap->interior_decommit_min_regions) {
            heap->interior_decommit_min_regions = next_threshold;
            heap->interior_decommit_smart_last_action =
                NGGC_INTERIOR_DECOMMIT_SMART_ACTION_RAISE;
            heap->interior_decommit_smart_cooldown_remaining =
                NGGC_INTERIOR_DECOMMIT_SMART_COOLDOWN_CYCLES;
            if (heap->interior_decommit_smart_raise_count != UINT64_MAX) {
                ++heap->interior_decommit_smart_raise_count;
            }
        } else {
            heap->interior_decommit_smart_last_action =
                NGGC_INTERIOR_DECOMMIT_SMART_ACTION_COOLDOWN;
            heap->interior_decommit_smart_cooldown_remaining =
                NGGC_INTERIOR_DECOMMIT_SMART_COOLDOWN_CYCLES;
            if (heap->interior_decommit_smart_cooldown_hold_count !=
                UINT64_MAX) {
                ++heap->interior_decommit_smart_cooldown_hold_count;
            }
        }
    } else if (heap->interior_decommit_smart_cooldown_remaining != 0u) {
        --heap->interior_decommit_smart_cooldown_remaining;
        heap->interior_decommit_smart_last_action =
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_COOLDOWN;
        if (heap->interior_decommit_smart_cooldown_hold_count !=
            UINT64_MAX) {
            ++heap->interior_decommit_smart_cooldown_hold_count;
        }
    } else if (high_reclaim_pressure &&
               heap->interior_decommit_min_regions > 2u) {
        next_threshold = heap->interior_decommit_min_regions / 2u;
        if (next_threshold < 2u) {
            next_threshold = 2u;
        }
        heap->interior_decommit_min_regions = next_threshold;
        heap->interior_decommit_smart_last_action =
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_LOWER;
        heap->interior_decommit_smart_cooldown_remaining =
            NGGC_INTERIOR_DECOMMIT_SMART_COOLDOWN_CYCLES;
        if (heap->interior_decommit_smart_lower_count != UINT64_MAX) {
            ++heap->interior_decommit_smart_lower_count;
        }
    }
    nggc_interior_decommit_smart_snapshot_locked(heap);
    return NGGC_OK;
}

static nggc_result nggc_region_decommit_interior_humongous_locked(
    nggc_heap *heap) {
    nggc_backend_stats backend_stats;
    size_t minimum_bin;
    if (!heap || !heap->regions || heap->region_size == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->interior_decommit_min_regions == 0u ||
        heap->interior_decommit_min_regions > heap->region_count) {
        return NGGC_OK;
    }
    if (heap->interior_decommit_min_regions < 2u ||
        heap->interior_decommit_min_regions > UINT32_MAX ||
        heap->interior_decommit_min_regions >
            SIZE_MAX / heap->region_size ||
        !nggc_large_free_bin_index_locked(
            heap,
            heap->interior_decommit_min_regions *
                heap->region_size,
            &minimum_bin)) {
        return NGGC_ERR_CORRUPTION;
    }
    memset(&backend_stats, 0, sizeof(backend_stats));
    if (nggc_backend_get_stats(heap->backend, &backend_stats) != NGGC_OK ||
        backend_stats.page_size == 0u) {
        return NGGC_OK;
    }
    for (size_t bin_index = minimum_bin;
         bin_index < NGGC_LARGE_FREE_BIN_COUNT; ++bin_index) {
        nggc_object_header *previous = 0;
        nggc_object_header *header = heap->large_free_bins[bin_index];
        while (header) {
            nggc_object_header *next =
                (nggc_object_header *)nggc_forwarding_load_relaxed(
                    header);
            uintptr_t start = (uintptr_t)header;
            size_t start_index;
            uint32_t run_length;
            uint32_t placement;
            size_t actual_bin;
            size_t bytes = header->alloc_size;
            int eligible = 1;
            if ((header->flags & ~NGGC_OBJECT_FLAG_LARGE) != 0u ||
                !nggc_free_block_is_large(header) ||
                !nggc_large_free_bin_index_locked(
                    heap, bytes, &actual_bin) ||
                actual_bin != bin_index ||
                !nggc_region_humongous_geometry(
                    heap, start, bytes, &start_index, &run_length) ||
                !nggc_region_humongous_run_matches_locked(
                    heap, start, bytes) ||
                start_index >= heap->region_count ||
                start >= heap->arena_cursor ||
                bytes > (size_t)(heap->arena_cursor - start)) {
                return NGGC_ERR_CORRUPTION;
            }
            if ((size_t)run_length <
                heap->interior_decommit_min_regions) {
                previous = header;
                header = next;
                continue;
            }
            placement = heap->regions[start_index].placement;
            if (placement != NGGC_REGION_PLACEMENT_MOVABLE_SPECIAL &&
                placement != NGGC_REGION_PLACEMENT_PINNED_SPECIAL) {
                return NGGC_ERR_CORRUPTION;
            }
            for (uint32_t offset = 0u;
                 offset < run_length; ++offset) {
                const nggc_region_meta *region =
                    &heap->regions[start_index + (size_t)offset];
                size_t span = nggc_region_span_size(region);
                if (region->decommitted || span != heap->region_size ||
                    region->committed_bytes != span ||
                    region->free_bytes != span ||
                    region->live_bytes != 0u ||
                    region->reserved_bytes != 0u ||
                    region->quarantined_bytes != 0u ||
                    region->live_object_count != 0u ||
                    region->quarantined_object_count != 0u ||
                    region->large_object_count != 0u ||
                    region->pinnable_object_count != 0u ||
                    region->pinned_object_count != 0u ||
                    region->non_movable_object_count != 0u ||
                    region->in_collection_set != 0u ||
                    region->allocation_owner != 0u ||
                    region->tlab_state != NGGC_TLAB_INACTIVE ||
                    region->placement != placement) {
                    return NGGC_ERR_CORRUPTION;
                }
                if ((region->start % backend_stats.page_size) != 0u ||
                    (span % backend_stats.page_size) != 0u) {
                    eligible = 0;
                }
            }
            if ((start % backend_stats.page_size) != 0u ||
                (bytes % backend_stats.page_size) != 0u) {
                eligible = 0;
            }
            if (!eligible) {
                previous = header;
                header = next;
                continue;
            }
            if (heap->free_object_count == 0u ||
                heap->large_free_object_count == 0u ||
                heap->free_bytes < bytes ||
                heap->large_free_bytes < bytes ||
                nggc_region_validate_interior_decommit_run_locked(
                    heap, start_index, run_length, placement) !=
                    NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            if (nggc_region_backend_vm_locked(
                    heap, (void *)start, bytes, 1) != NGGC_OK) {
                if (heap->region_decommit_failure_count != UINT64_MAX) {
                    ++heap->region_decommit_failure_count;
                }
                previous = header;
                header = next;
                continue;
            }
            if (previous) {
                nggc_forwarding_store_free_next(previous, next);
            } else {
                heap->large_free_bins[bin_index] = next;
            }
            if (!heap->large_free_bins[bin_index]) {
                heap->large_free_bin_mask &=
                    ~((uint32_t)1u << bin_index);
            }
            --heap->free_object_count;
            heap->free_bytes -= bytes;
            --heap->large_free_object_count;
            heap->large_free_bytes -= bytes;
            if (nggc_region_mark_interior_decommitted_run_locked(
                    heap, start_index, run_length, placement) != NGGC_OK) {
                return NGGC_ERR_CORRUPTION;
            }
            header = next;
        }
    }
    return NGGC_OK;
}

static nggc_result nggc_backend_unmap_gc_arena_for_close(nggc_heap *heap) {
    nggc_result result = NGGC_OK;
    if (!heap || !heap->gc_arena_base || heap->gc_arena_size == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->decommitted_region_count == 0u &&
        heap->region_recommit_count == 0u) {
        return nggc_backend_unmap(heap->backend, heap->gc_arena_base,
                                  heap->gc_arena_size);
    }
    for (size_t i = 0u; i < heap->region_count; ++i) {
        nggc_region_meta *region = &heap->regions[i];
        if (region->decommitted) {
            continue;
        }
        if (nggc_backend_unmap(heap->backend, (void *)region->start,
                               nggc_region_span_size(region)) != NGGC_OK) {
            result = NGGC_ERR_BACKEND;
        }
    }
    return result;
}

static nggc_result nggc_type_has_live_objects_locked(
    nggc_heap *heap, uint16_t type_id, int *out_in_use) {
    uintptr_t cursor;
    if (!heap || !out_in_use || !nggc_type_id_valid(type_id)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_in_use = 0;
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        if (block.state == NGGC_OBJECT_LIVE) {
            if (header->type_id == type_id) {
                *out_in_use = 1;
                return NGGC_OK;
            }
        }
        cursor = block.end;
    }
    return NGGC_OK;
}

nggc_result nggc_open(const nggc_config *config, nggc_heap **out_heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_heap *heap;
    nggc_result result;
    void *arena = 0;
    size_t gc_arena_size;
    size_t uhm_arena_size;
    size_t region_size;
    size_t tlab_size;
    size_t tlab_min_size;
    size_t tlab_max_size;
    size_t region_count;
    size_t card_count;
    size_t remset_buffer_capacity;
    size_t interior_decommit_min_regions =
        NGGC_INTERIOR_DECOMMIT_MIN_REGIONS_DEFAULT;
    uint32_t interior_decommit_policy =
        NGGC_INTERIOR_DECOMMIT_POLICY_SMART;
    uint32_t soft_reference_policy =
        NGGC_SOFT_REFERENCE_POLICY_HEAP_PRESSURE;
    uint32_t soft_reference_retain_percent =
        NGGC_SOFT_REFERENCE_RETAIN_PERCENT_DEFAULT;
    uint32_t young_tenuring_threshold =
        NGGC_YOUNG_TENURING_THRESHOLD_DEFAULT;
    uint32_t tlab_sizing_policy = NGGC_TLAB_SIZING_POLICY_AUTO;
    uint32_t gc_trigger_policy = NGGC_TRIGGER_SMART;
    size_t gc_trigger_soft_limit = 0u;
    size_t gc_trigger_quantum = 0u;
    uint64_t gc_trigger_latency_target_ns = 0u;

    if (!out_heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_heap = 0;
    if (config && (config->size < sizeof(*config) ||
                   config->abi_major != NGGC_ABI_MAJOR ||
                   (config->flags & ~NGGC_CONFIG_KNOWN_FLAGS) != 0u ||
                   (config->memory_pressure_ctx != 0 &&
                    !config->memory_pressure) ||
                   (config->clock_ns_ctx != 0 && !config->clock_ns) ||
                   (config->interior_decommit_slow_ns_per_region != 0u &&
                    !config->clock_ns) ||
                   (config->gc_trigger_latency_target_ns != 0u &&
                    !config->clock_ns))) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (config) {
        if (!nggc_soft_reference_policy_valid(
                config->soft_reference_policy) ||
            config->soft_reference_retain_percent > 100u ||
            !nggc_tlab_sizing_policy_config_valid(
                config->tlab_sizing_policy) ||
            !nggc_trigger_policy_valid(config->gc_trigger_policy)) {
            return NGGC_ERR_INVALID_ARGUMENT;
        }
        tlab_sizing_policy = config->tlab_sizing_policy;
        gc_trigger_policy = config->gc_trigger_policy;
        gc_trigger_soft_limit = config->gc_trigger_soft_limit;
        gc_trigger_quantum = config->gc_trigger_quantum;
        gc_trigger_latency_target_ns =
            config->gc_trigger_latency_target_ns;
        soft_reference_policy = config->soft_reference_policy;
        if (config->soft_reference_retain_percent != 0u) {
            soft_reference_retain_percent =
                config->soft_reference_retain_percent;
        }
        if (config->young_tenuring_threshold > UINT8_MAX) {
            return NGGC_ERR_INVALID_ARGUMENT;
        }
        if (config->young_tenuring_threshold != 0u) {
            young_tenuring_threshold = config->young_tenuring_threshold;
        }
        if (config->interior_decommit_min_regions ==
            NGGC_INTERIOR_DECOMMIT_DISABLED) {
            interior_decommit_min_regions = 0u;
            interior_decommit_policy =
                NGGC_INTERIOR_DECOMMIT_POLICY_DISABLED;
        } else if (config->interior_decommit_min_regions != 0u) {
            if (config->interior_decommit_min_regions < 2u ||
                config->interior_decommit_min_regions > UINT32_MAX) {
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            interior_decommit_min_regions =
                config->interior_decommit_min_regions;
            interior_decommit_policy =
                NGGC_INTERIOR_DECOMMIT_POLICY_MANUAL;
        }
    }

    gc_arena_size = config && config->gc_arena_size != 0
                        ? config->gc_arena_size
                        : NGGC_DEFAULT_GC_ARENA_SIZE;
    uhm_arena_size = config && config->uhm_arena_size != 0
                         ? config->uhm_arena_size
                         : NGGC_DEFAULT_UHM_ARENA_SIZE;
    region_size = config && config->region_size != 0u
                      ? config->region_size
                      : NGGC_DEFAULT_REGION_SIZE;
    tlab_size = config && config->tlab_size != 0u
                    ? config->tlab_size
                    : (region_size < NGGC_DEFAULT_TLAB_BYTES
                           ? region_size
                           : NGGC_DEFAULT_TLAB_BYTES);
    if (tlab_sizing_policy == NGGC_TLAB_SIZING_POLICY_AUTO) {
        tlab_sizing_policy = config && config->tlab_size != 0u
                                 ? NGGC_TLAB_SIZING_POLICY_FIXED
                                 : NGGC_TLAB_SIZING_POLICY_SMART;
    }
    if (tlab_sizing_policy == NGGC_TLAB_SIZING_POLICY_FIXED) {
        if (config &&
            (config->tlab_min_size != 0u ||
             config->tlab_max_size != 0u)) {
            return NGGC_ERR_INVALID_ARGUMENT;
        }
        tlab_min_size = tlab_size;
        tlab_max_size = tlab_size;
    } else {
        tlab_min_size = config && config->tlab_min_size != 0u
                            ? config->tlab_min_size
                            : (tlab_size < NGGC_DEFAULT_TLAB_MIN_BYTES
                                   ? tlab_size
                                   : NGGC_DEFAULT_TLAB_MIN_BYTES);
        tlab_max_size = config && config->tlab_max_size != 0u
                            ? config->tlab_max_size
                            : region_size;
    }
    result = nggc_validate_heap_geometry(
        gc_arena_size, region_size, tlab_size, tlab_sizing_policy,
        tlab_min_size, tlab_max_size,
        &region_count, &card_count);
    if (result != NGGC_OK) {
        return result;
    }
    if (gc_trigger_soft_limit == 0u) {
        gc_trigger_soft_limit = gc_arena_size - gc_arena_size / 4u;
        if (gc_trigger_soft_limit == 0u) {
            gc_trigger_soft_limit = 1u;
        }
    } else if (gc_trigger_soft_limit > gc_arena_size) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (gc_trigger_quantum == 0u) {
        gc_trigger_quantum = region_size;
    }
    if (gc_trigger_quantum == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    remset_buffer_capacity =
        config && config->remset_buffer_capacity != 0u
            ? config->remset_buffer_capacity
            : (card_count < NGGC_DEFAULT_REMSET_BUFFER_CAPACITY
                   ? card_count
                   : NGGC_DEFAULT_REMSET_BUFFER_CAPACITY);
    if (remset_buffer_capacity == 0u ||
        remset_buffer_capacity > card_count ||
        remset_buffer_capacity > UINT_MAX ||
        remset_buffer_capacity > SIZE_MAX / sizeof(size_t)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }

    heap = (nggc_heap *)calloc(1u, sizeof(*heap));
    if (!heap) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    heap->heap_identity = nggc_next_heap_identity();
    heap->types = (nggc_type_slot *)calloc(NGGC_TYPE_LIMIT,
                                           sizeof(*heap->types));
    if (!heap->types) {
        free(heap);
        return NGGC_ERR_OUT_OF_MEMORY;
    }

    result = nggc_backend_create(uhm_arena_size, &heap->backend);
    if (result != NGGC_OK) {
        free(heap->types);
        free(heap);
        return result;
    }

    result = nggc_backend_map(heap->backend, gc_arena_size, &arena);
    if (result != NGGC_OK) {
        (void)nggc_backend_destroy(heap->backend);
        free(heap->types);
        free(heap);
        return result;
    }

    heap->magic = NGGC_HEAP_MAGIC;
    atomic_init(&heap->last_error, (int)NGGC_OK);
    heap->heap_state = NGGC_HEAP_STATE_RUNNING;
    heap->fail_stop_reason = NGGC_OK;
    atomic_flag_clear(&heap->lock);
    atomic_init(&heap->phantom_reaper_event_epoch, (uint_fast64_t)0);
    atomic_init(&heap->safepoint_release_epoch, UINT64_C(0));
    atomic_init(&heap->safepoint_resume_epoch, UINT64_C(0));
    atomic_init(&heap->type_table_published, 0u);
    atomic_init(&heap->tlab_fast_gate, NGGC_TLAB_FAST_GATE_CLOSED);
    atomic_init(&heap->evacuation_workers_active, 0u);
    atomic_init(&heap->evacuation_workers_peak_active, 0u);
    atomic_init(&heap->remap_workers_active, 0u);
    atomic_init(&heap->remap_workers_peak_active, 0u);
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    atomic_init(&heap->test_concurrent_window_entered, 0u);
    atomic_init(&heap->test_concurrent_window_release, 0u);
    atomic_init(&heap->test_concurrent_copy_entered, 0u);
    atomic_init(&heap->test_concurrent_copy_release, 0u);
    atomic_init(&heap->test_concurrent_copy_pause, 0u);
    atomic_init(&heap->test_concurrent_copy_source, 0);
    atomic_init(&heap->test_concurrent_copy_store_entered, 0u);
    atomic_init(&heap->test_concurrent_copy_load_entered, 0u);
    atomic_init(&heap->test_reference_create_pause_armed, 0u);
    atomic_init(&heap->test_reference_create_pause_entered, 0u);
    atomic_init(&heap->test_reference_create_pause_release, 0u);
    atomic_init(&heap->test_safepoint_blocked_threads, 0u);
    atomic_init(&heap->test_tlab_fast_pause_armed, 0u);
    atomic_init(&heap->test_tlab_fast_pause_entered, 0u);
    atomic_init(&heap->test_tlab_fast_pause_release, 0u);
    atomic_init(&heap->test_tlab_fast_enrollment_pause_armed, 0u);
    atomic_init(&heap->test_tlab_fast_enrollment_pause_stage, 0u);
    atomic_init(&heap->test_tlab_fast_enrollment_pause_release_stage, 0u);
    atomic_init(&heap->test_tlab_fast_flush_pause_armed, 0u);
    atomic_init(&heap->test_tlab_fast_flush_pause_entered, 0u);
    atomic_init(&heap->test_tlab_fast_flush_pause_release, 0u);
    atomic_init(&heap->test_tlab_fast_flush_busy_observed, 0u);
    atomic_init(&heap->test_remset_publish_pause_armed, 0u);
    atomic_init(&heap->test_remset_publish_pause_entered, 0u);
    atomic_init(&heap->test_remset_publish_pause_release, 0u);
    atomic_init(&heap->test_remset_classification_pause_armed, 0u);
    atomic_init(&heap->test_remset_classification_pause_entered, 0u);
    atomic_init(&heap->test_remset_classification_pause_release, 0u);
    atomic_init(&heap->test_gc_pool_fail_next_wait, 0u);
    atomic_init(&heap->test_gc_pool_fail_next_broadcast, 0u);
    atomic_init(&heap->test_gc_pool_fail_next_join, 0u);
    atomic_init(&heap->test_gc_pool_worker_fault, 0u);
#endif
    heap->flags = config ? config->flags : 0u;
    heap->soft_reference_policy = soft_reference_policy;
    heap->soft_reference_retain_percent = soft_reference_retain_percent;
    heap->gc_trigger_policy = gc_trigger_policy;
    heap->gc_trigger_soft_limit = gc_trigger_soft_limit;
    heap->gc_trigger_quantum = gc_trigger_quantum;
    heap->gc_trigger_latency_target_ns = gc_trigger_latency_target_ns;
    heap->gc_trigger_last_action = NGGC_TRIGGER_ACTION_NONE;
    heap->gc_trigger_last_reason = NGGC_TRIGGER_REASON_NONE;
    heap->gc_arena_base = arena;
    heap->gc_arena_size = gc_arena_size;
    heap->tlab_size = tlab_size;
    heap->tlab_sizing_policy = tlab_sizing_policy;
    heap->tlab_min_size = tlab_min_size;
    heap->tlab_max_size = tlab_max_size;
    heap->young_tenuring_threshold = young_tenuring_threshold;
    heap->arena_cursor = (uintptr_t)arena;
    if (!nggc_range_end((uintptr_t)arena, gc_arena_size, &heap->arena_end)) {
        (void)nggc_backend_unmap(heap->backend, arena, gc_arena_size);
        (void)nggc_backend_destroy(heap->backend);
        free(heap->types);
        free(heap);
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    heap->region_size = region_size;
    heap->region_count = region_count;
    heap->interior_decommit_min_regions =
        interior_decommit_min_regions;
    heap->interior_decommit_policy = interior_decommit_policy;
    heap->memory_pressure = config ? config->memory_pressure : 0;
    heap->memory_pressure_ctx =
        config ? config->memory_pressure_ctx : 0;
    heap->clock_ns = config ? config->clock_ns : 0;
    heap->clock_ns_ctx = config ? config->clock_ns_ctx : 0;
    heap->interior_decommit_slow_ns_per_region =
        config ? config->interior_decommit_slow_ns_per_region : 0u;
    heap->allocator_owned_region_index = SIZE_MAX;
    heap->allocator_lease_active = 1u;
    heap->allocator_lease_epoch = 1u;
    heap->allocator_lease_acquire_count = 1u;
    heap->regions = (nggc_region_meta *)calloc(heap->region_count,
                                               sizeof(*heap->regions));
    if (!heap->regions) {
        (void)nggc_backend_unmap(heap->backend, arena, gc_arena_size);
        (void)nggc_backend_destroy(heap->backend);
        free(heap->types);
        free(heap);
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < heap->region_count; ++i) {
        uintptr_t start = (uintptr_t)arena + i * heap->region_size;
        uintptr_t end = heap->arena_end - start < heap->region_size
                            ? heap->arena_end
                            : start + heap->region_size;
        heap->regions[i].start = start;
        heap->regions[i].end = end;
        heap->regions[i].allocation_cursor = start;
    }
    if (nggc_region_refresh_allocation_owner_locked(heap) != NGGC_OK) {
        (void)nggc_backend_unmap(heap->backend, arena, gc_arena_size);
        (void)nggc_backend_destroy(heap->backend);
        free(heap->regions);
        free(heap->types);
        free(heap);
        return NGGC_ERR_CORRUPTION;
    }
    heap->card_size = NGGC_DEFAULT_CARD_SIZE;
    heap->card_count = card_count;
    heap->remset_buffer_capacity = remset_buffer_capacity;
    heap->remset_max_dirty_cards =
        heap->card_count < NGGC_DEFAULT_REMSET_MAX_DIRTY_CARDS
            ? heap->card_count
            : NGGC_DEFAULT_REMSET_MAX_DIRTY_CARDS;
    heap->remset_mode = NGGC_REMSET_MODE_PRECISE_CARD;
    heap->card_table = (atomic_uchar *)calloc(
        heap->card_count, sizeof(*heap->card_table));
    if (!heap->card_table) {
        (void)nggc_backend_unmap(heap->backend, arena, gc_arena_size);
        (void)nggc_backend_destroy(heap->backend);
        free(heap->regions);
        free(heap->types);
        free(heap);
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0u; i < heap->card_count; ++i) {
        atomic_init(&heap->card_table[i], 0u);
    }
    heap->remset_card_atomic_lock_free =
        atomic_is_lock_free(&heap->card_table[0]) ? 1u : 0u;
#if NGGC_HAVE_C11_THREADS
    if (mtx_init(&heap->safepoint_wait_lock, mtx_plain) != thrd_success) {
        (void)nggc_backend_unmap(heap->backend, arena, gc_arena_size);
        (void)nggc_backend_destroy(heap->backend);
        free((void *)heap->card_table);
        free(heap->regions);
        free(heap->types);
        free(heap);
        return NGGC_ERR_BACKEND;
    }
    if (cnd_init(&heap->safepoint_wait_condition) != thrd_success) {
        mtx_destroy(&heap->safepoint_wait_lock);
        (void)nggc_backend_unmap(heap->backend, arena, gc_arena_size);
        (void)nggc_backend_destroy(heap->backend);
        free((void *)heap->card_table);
        free(heap->regions);
        free(heap->types);
        free(heap);
        return NGGC_ERR_BACKEND;
    }
    heap->safepoint_wait_initialized = 1u;
#endif
    result = nggc_gc_pool_init(heap);
    if (result != NGGC_OK) {
#if NGGC_HAVE_C11_THREADS
        if (heap->safepoint_wait_initialized) {
            cnd_destroy(&heap->safepoint_wait_condition);
            mtx_destroy(&heap->safepoint_wait_lock);
        }
#endif
        (void)nggc_backend_unmap(heap->backend, arena, gc_arena_size);
        (void)nggc_backend_destroy(heap->backend);
        free((void *)heap->card_table);
        free(heap->regions);
        free(heap->types);
        free(heap);
        return result;
    }
    heap->next_type_id = 1u;
    result = NGGC_OK;
    *out_heap = heap;
    return result;
}

static nggc_result nggc_destroy_heap_resources(nggc_heap *heap) {
    nggc_result result = NGGC_OK;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_gc_pool_destroy(heap);
    if (result != NGGC_OK) {
        return result;
    }
    nggc_barrier_tss_remove_current(heap);
    if (heap->gc_arena_base &&
        nggc_backend_unmap_gc_arena_for_close(heap) != NGGC_OK) {
        result = NGGC_ERR_BACKEND;
    }
    if (nggc_backend_destroy(heap->backend) != NGGC_OK &&
        result == NGGC_OK) {
        result = NGGC_ERR_BACKEND;
    }
    nggc_threads_destroy_all(heap);
#if NGGC_HAVE_C11_THREADS
    if (heap->safepoint_wait_initialized) {
        cnd_destroy(&heap->safepoint_wait_condition);
        mtx_destroy(&heap->safepoint_wait_lock);
    }
#endif
    free(heap->mutator_assist_mark_stack.items);
    free(heap->regions);
    free(heap->phantom_reaper_slots);
    free((void *)heap->card_table);
    free(heap->roots);
    free(heap->types);
    free(heap);
    return result;
}

nggc_result nggc_close(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    if (!nggc_heap_handle_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (heap->heap_state == NGGC_HEAP_STATE_FAIL_STOP) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (heap->registered_thread_count != 0u || heap->safepoint_active ||
        heap->collector_cycle_active || heap->gc_phase != NGGC_PHASE_IDLE) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    nggc_unlock(heap);
    result = nggc_destroy_heap_resources(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

nggc_result nggc_get_heap_state(nggc_heap *heap,
                                uint32_t *out_state,
                                nggc_result *out_fail_stop_reason) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    if (!nggc_heap_handle_live(heap) || !out_state ||
        !out_fail_stop_reason) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    *out_state = heap->heap_state;
    *out_fail_stop_reason = heap->heap_state == NGGC_HEAP_STATE_FAIL_STOP
                                ? heap->fail_stop_reason
                                : NGGC_OK;
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_force_destroy(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    if (!nggc_heap_handle_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (heap->heap_state != NGGC_HEAP_STATE_FAIL_STOP) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    nggc_unlock(heap);
    result = nggc_destroy_heap_resources(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

nggc_result nggc_register_type(nggc_heap *heap,
                               const nggc_type_desc *desc,
                               uint16_t *out_type_id) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_type_desc stored;
    uint16_t type_id;
    nggc_result valid;

    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    valid = nggc_validate_type_desc(desc);
    if (valid != NGGC_OK) {
        nggc_set_error(heap, valid);
        return valid;
    }

    nggc_lock(heap);
    if (heap->type_table_frozen) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_FROZEN);
        return NGGC_ERR_FROZEN;
    }
    if (desc->type_id != NGGC_TYPE_NONE) {
        type_id = desc->type_id;
        if (heap->types[type_id].registered) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_TYPE_EXISTS);
            return NGGC_ERR_TYPE_EXISTS;
        }
    } else {
        type_id = heap->next_type_id;
        while (type_id != NGGC_TYPE_NONE && heap->types[type_id].registered) {
            ++type_id;
        }
        if (type_id == NGGC_TYPE_NONE) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_TYPE_FULL);
            return NGGC_ERR_TYPE_FULL;
        }
        heap->next_type_id = (uint16_t)(type_id + 1u);
    }

    memset(&stored, 0, sizeof(stored));
    memcpy(&stored, desc,
           desc->size < sizeof(stored) ? desc->size : sizeof(stored));
    stored.type_id = type_id;
    if (stored.alignment == 0) {
        stored.alignment = NGGC_DEFAULT_PAYLOAD_ALIGNMENT;
    }
    heap->types[type_id].desc = stored;
    heap->types[type_id].registered = 1;
    ++heap->registered_type_count;
    nggc_unlock(heap);

    if (out_type_id) {
        *out_type_id = type_id;
    }
    return NGGC_OK;
}

nggc_result nggc_unregister_type(nggc_heap *heap, uint16_t type_id) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    int in_use = 0;

    if (!nggc_heap_live(heap) || !nggc_type_id_valid(type_id)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (heap->type_table_frozen) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_FROZEN);
        return NGGC_ERR_FROZEN;
    }
    if (!heap->types[type_id].registered) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_TYPE_NOT_FOUND);
        return NGGC_ERR_TYPE_NOT_FOUND;
    }
    result = nggc_type_has_live_objects_locked(heap, type_id, &in_use);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    if (in_use) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    memset(&heap->types[type_id], 0, sizeof(heap->types[type_id]));
    if (heap->registered_type_count != 0u) {
        --heap->registered_type_count;
    }
    if (heap->next_type_id == NGGC_TYPE_NONE ||
        type_id < heap->next_type_id) {
        heap->next_type_id = type_id;
    }
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_type_table_freeze(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    heap->type_table_frozen = 1u;
    atomic_store_explicit(&heap->type_table_published, 1u,
                          memory_order_release);
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    return NGGC_OK;
}

void *nggc_alloc(nggc_heap *heap, uint16_t type_id) {
    NGGC_SCAN_API_GUARD(0);
    nggc_type_desc desc;
    if (!nggc_heap_live(heap) || !nggc_type_id_valid(type_id)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_lock(heap);
    if (!heap->types[type_id].registered) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_TYPE_NOT_FOUND);
        return 0;
    }
    desc = heap->types[type_id].desc;
    nggc_unlock(heap);
    if (desc.object_size == 0) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    return nggc_alloc_size(heap, type_id, desc.object_size);
}

void *nggc_alloc_size(nggc_heap *heap, uint16_t type_id, size_t payload_size) {
    NGGC_SCAN_API_GUARD(0);
    nggc_type_desc desc;
    nggc_result result;
    void *object = 0;

    if (!nggc_heap_live(heap) || !nggc_type_id_valid(type_id)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_auto_trigger_before_allocation(heap, payload_size);

    nggc_lock(heap);
    if (!heap->types[type_id].registered) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_TYPE_NOT_FOUND);
        return 0;
    }
    desc = heap->types[type_id].desc;
    if (desc.object_size != 0 && payload_size < desc.object_size) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    result = nggc_alloc_size_with_desc_locked(heap, type_id, &desc, 0u,
                                              payload_size, &object, 0);
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }

    return object;
}

void *nggc_thread_alloc(nggc_heap *heap, nggc_thread *thread,
                        uint16_t type_id) {
    NGGC_SCAN_API_GUARD(0);
    nggc_type_desc desc;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        !nggc_type_id_valid(type_id)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    if (atomic_load_explicit(&heap->type_table_published,
                             memory_order_acquire) != 0u) {
        if (!heap->types[type_id].registered) {
            nggc_set_error(heap, NGGC_ERR_TYPE_NOT_FOUND);
            return 0;
        }
        if (heap->types[type_id].desc.object_size == 0u) {
            nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
            return 0;
        }
        return nggc_internal_thread_alloc_size(
            heap, thread, type_id,
            heap->types[type_id].desc.object_size);
    }
    nggc_lock(heap);
    if (!heap->types[type_id].registered) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_TYPE_NOT_FOUND);
        return 0;
    }
    desc = heap->types[type_id].desc;
    nggc_unlock(heap);
    if (desc.object_size == 0u) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    return nggc_internal_thread_alloc_size(heap, thread, type_id,
                                           desc.object_size);
}

void *nggc_thread_alloc_size(nggc_heap *heap, nggc_thread *thread,
                             uint16_t type_id, size_t payload_size) {
    NGGC_SCAN_API_GUARD(0);
    return nggc_internal_thread_alloc_size(heap, thread, type_id,
                                           payload_size);
}

void *nggc_internal_alloc_none(nggc_heap *heap, size_t payload_size) {
    nggc_type_desc desc;
    nggc_result result;
    void *object = 0;

    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_auto_trigger_before_allocation(heap, payload_size);
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.type_id = NGGC_TYPE_NONE;
    desc.flags = NGGC_TYPE_FLAG_INTERNAL;
    desc.object_size = 0;
    desc.alignment = 0;
    desc.scan = 0;
    desc.name = "nggc_internal_none";

    nggc_lock(heap);
    result = nggc_alloc_size_with_desc_locked(heap, NGGC_TYPE_NONE, &desc,
                                              NGGC_INTERNAL_KIND_NONE,
                                              payload_size, &object, 0);
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }
    return object;
}

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
static void nggc_test_reference_create_error_pause(nggc_heap *heap) {
    unsigned expected = 1u;
    if (!atomic_compare_exchange_strong_explicit(
            &heap->test_reference_create_pause_armed, &expected, 0u,
            memory_order_acq_rel, memory_order_acquire)) {
        return;
    }
    atomic_store_explicit(&heap->test_reference_create_pause_entered, 1u,
                          memory_order_release);
    while (atomic_load_explicit(&heap->test_reference_create_pause_release,
                                memory_order_acquire) == 0u) {
#if NGGC_HAVE_C11_THREADS
        thrd_yield();
#else
        atomic_signal_fence(memory_order_seq_cst);
#endif
    }
}
#else
#define nggc_test_reference_create_error_pause(heap) ((void)0)
#endif

static nggc_result nggc_alloc_weak_impl(nggc_heap *heap, void *referent,
                                       void **out_object) {
    nggc_type_desc desc;
    nggc_result result;
    void *object = 0;

    *out_object = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_auto_trigger_before_allocation(heap, sizeof(nggc_internal_weak_reference));
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.type_id = NGGC_TYPE_NONE;
    desc.flags = NGGC_TYPE_FLAG_INTERNAL;
    desc.object_size = sizeof(nggc_internal_weak_reference);
    desc.alignment = 0;
    desc.scan = 0;
    desc.name = "nggc_internal_weak";

    nggc_lock(heap);
    if (referent && !nggc_pointer_is_object_start(heap, referent)) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_alloc_size_with_desc_locked(
        heap, NGGC_TYPE_NONE, &desc, NGGC_INTERNAL_KIND_WEAK_REFERENCE,
        sizeof(nggc_internal_weak_reference), &object, 0);
    if (result == NGGC_OK) {
        ((nggc_internal_weak_reference *)object)->referent = referent;
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    *out_object = object;
    return NGGC_OK;
}

void *nggc_internal_alloc_weak(nggc_heap *heap, void *referent) {
    void *object = 0;
    (void)nggc_alloc_weak_impl(heap, referent, &object);
    return object;
}

static nggc_result nggc_weak_ref_get_locked(nggc_heap *heap,
                                            void *weak_reference,
                                            void **out_referent) {
    nggc_object_header *header;
    void *referent;

    header = nggc_live_header_from_object(heap, weak_reference);
    if (!nggc_internal_weak_reference_object(header) ||
        header->payload_size < sizeof(nggc_internal_weak_reference)) {
        return NGGC_ERR_CORRUPTION;
    }
    referent = ((nggc_internal_weak_reference *)weak_reference)->referent;
    if (referent && !nggc_pointer_is_object_start(heap, referent)) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_referent = referent;
    return NGGC_OK;
}

void *nggc_internal_weak_get(nggc_heap *heap, void *weak_reference) {
    nggc_result result;
    void *referent = 0;

    if (!nggc_heap_live(heap) || !weak_reference) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_lock(heap);
    result = nggc_weak_ref_get_locked(heap, weak_reference, &referent);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }
    return referent;
}

nggc_result nggc_weak_ref_create(nggc_heap *heap,
                                 void *referent,
                                 void **out_weak_ref) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    void *weak_ref;
    nggc_result result;

    if (!out_weak_ref) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_weak_ref = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_alloc_weak_impl(heap, referent, &weak_ref);
    if (result != NGGC_OK) {
        nggc_test_reference_create_error_pause(heap);
        return result;
    }
    *out_weak_ref = weak_ref;
    return NGGC_OK;
}

nggc_result nggc_weak_ref_get(nggc_heap *heap,
                              void *weak_ref,
                              void **out_referent) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    void *referent = 0;

    if (!out_referent) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_referent = 0;
    if (!nggc_heap_live(heap) || !weak_ref) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_weak_ref_get_locked(heap, weak_ref, &referent);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    *out_referent = referent;
    return NGGC_OK;
}

static nggc_result nggc_alloc_soft_impl(nggc_heap *heap, void *referent,
                                       void **out_object) {
    nggc_type_desc desc;
    nggc_result result;
    void *object = 0;

    *out_object = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_auto_trigger_before_allocation(heap, sizeof(nggc_internal_soft_reference));
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.type_id = NGGC_TYPE_NONE;
    desc.flags = NGGC_TYPE_FLAG_INTERNAL;
    desc.object_size = sizeof(nggc_internal_soft_reference);
    desc.alignment = 0;
    desc.scan = 0;
    desc.name = "nggc_internal_soft";

    nggc_lock(heap);
    if (referent && !nggc_pointer_is_object_start(heap, referent)) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_alloc_size_with_desc_locked(
        heap, NGGC_TYPE_NONE, &desc, NGGC_INTERNAL_KIND_SOFT_REFERENCE,
        sizeof(nggc_internal_soft_reference), &object, 0);
    if (result == NGGC_OK) {
        ((nggc_internal_soft_reference *)object)->referent = referent;
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    *out_object = object;
    return NGGC_OK;
}

void *nggc_internal_alloc_soft(nggc_heap *heap, void *referent) {
    void *object = 0;
    (void)nggc_alloc_soft_impl(heap, referent, &object);
    return object;
}

static nggc_result nggc_soft_ref_get_locked(nggc_heap *heap,
                                            void *soft_reference,
                                            void **out_referent) {
    nggc_object_header *header;
    void *referent;

    header = nggc_live_header_from_object(heap, soft_reference);
    if (!nggc_internal_soft_reference_object(header) ||
        header->payload_size < sizeof(nggc_internal_soft_reference)) {
        return NGGC_ERR_CORRUPTION;
    }
    referent = ((nggc_internal_soft_reference *)soft_reference)->referent;
    if (referent && !nggc_pointer_is_object_start(heap, referent)) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_referent = referent;
    return NGGC_OK;
}

void *nggc_internal_soft_get(nggc_heap *heap, void *soft_reference) {
    nggc_result result;
    void *referent = 0;

    if (!nggc_heap_live(heap) || !soft_reference) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_lock(heap);
    result = nggc_soft_ref_get_locked(heap, soft_reference, &referent);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }
    return referent;
}

nggc_result nggc_soft_ref_create(nggc_heap *heap,
                                 void *referent,
                                 void **out_soft_ref) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    void *soft_ref;
    nggc_result result;

    if (!out_soft_ref) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_soft_ref = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_alloc_soft_impl(heap, referent, &soft_ref);
    if (result != NGGC_OK) {
        nggc_test_reference_create_error_pause(heap);
        return result;
    }
    *out_soft_ref = soft_ref;
    return NGGC_OK;
}

nggc_result nggc_soft_ref_get(nggc_heap *heap,
                              void *soft_ref,
                              void **out_referent) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    void *referent = 0;

    if (!out_referent) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_referent = 0;
    if (!nggc_heap_live(heap) || !soft_ref) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_soft_ref_get_locked(heap, soft_ref, &referent);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    *out_referent = referent;
    return NGGC_OK;
}

static nggc_result nggc_alloc_phantom_queue_impl(nggc_heap *heap,
                                                void **out_object) {
    nggc_type_desc desc;
    nggc_result result;
    void *object = 0;

    *out_object = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_auto_trigger_before_allocation(heap, sizeof(nggc_internal_phantom_queue));
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.type_id = NGGC_TYPE_NONE;
    desc.flags = NGGC_TYPE_FLAG_INTERNAL;
    desc.object_size = sizeof(nggc_internal_phantom_queue);
    desc.alignment = 0;
    desc.scan = 0;
    desc.name = "nggc_internal_phantom_queue";

    nggc_lock(heap);
    result = nggc_alloc_size_with_desc_locked(
        heap, NGGC_TYPE_NONE, &desc, NGGC_INTERNAL_KIND_PHANTOM_QUEUE,
        sizeof(nggc_internal_phantom_queue), &object, 0);
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    *out_object = object;
    return NGGC_OK;
}

void *nggc_internal_alloc_phantom_queue(nggc_heap *heap) {
    void *object = 0;
    (void)nggc_alloc_phantom_queue_impl(heap, &object);
    return object;
}

nggc_result nggc_phantom_queue_create(nggc_heap *heap,
                                      void **out_queue) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    void *queue;
    nggc_result result;

    if (!out_queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_queue = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_alloc_phantom_queue_impl(heap, &queue);
    if (result != NGGC_OK) {
        nggc_test_reference_create_error_pause(heap);
        return result;
    }
    *out_queue = queue;
    return NGGC_OK;
}

static nggc_result nggc_alloc_phantom_impl(nggc_heap *heap, void *referent,
                                          void *queue, void **out_object) {
    nggc_type_desc desc;
    nggc_object_header *queue_header;
    nggc_result result;
    void *object = 0;

    *out_object = 0;
    if (!nggc_heap_live(heap) || !queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_auto_trigger_before_allocation(
        heap, sizeof(nggc_internal_phantom_reference));
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.type_id = NGGC_TYPE_NONE;
    desc.flags = NGGC_TYPE_FLAG_INTERNAL;
    desc.object_size = sizeof(nggc_internal_phantom_reference);
    desc.alignment = 0;
    desc.scan = 0;
    desc.name = "nggc_internal_phantom";

    nggc_lock(heap);
    if (referent && !nggc_pointer_is_object_start(heap, referent)) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    queue_header = nggc_live_header_from_object(heap, queue);
    if (!nggc_internal_phantom_queue_object(queue_header) ||
        queue_header->payload_size < sizeof(nggc_internal_phantom_queue)) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_alloc_size_with_desc_locked(
        heap, NGGC_TYPE_NONE, &desc, NGGC_INTERNAL_KIND_PHANTOM_REFERENCE,
        sizeof(nggc_internal_phantom_reference), &object, 0);
    if (result == NGGC_OK) {
        nggc_internal_phantom_reference *phantom =
            (nggc_internal_phantom_reference *)object;
        phantom->referent = referent;
        phantom->queue = queue;
        phantom->next = 0;
        phantom->enqueued = 0u;
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    *out_object = object;
    return NGGC_OK;
}

void *nggc_internal_alloc_phantom(nggc_heap *heap, void *referent,
                                  void *queue) {
    void *object = 0;
    (void)nggc_alloc_phantom_impl(heap, referent, queue, &object);
    return object;
}

nggc_result nggc_phantom_ref_create(nggc_heap *heap,
                                    void *referent,
                                    void *queue,
                                    void **out_phantom_ref) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    void *phantom_ref;
    nggc_result result;

    if (!out_phantom_ref) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_phantom_ref = 0;
    if (!nggc_heap_live(heap) || !queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_alloc_phantom_impl(heap, referent, queue, &phantom_ref);
    if (result != NGGC_OK) {
        nggc_test_reference_create_error_pause(heap);
        return result;
    }
    *out_phantom_ref = phantom_ref;
    return NGGC_OK;
}

static nggc_result nggc_phantom_queue_count_locked(
    nggc_heap *heap, void *queue, size_t *out_count) {
    nggc_object_header *header;
    nggc_internal_phantom_queue *phantom_queue;

    header = nggc_live_header_from_object(heap, queue);
    if (!nggc_internal_phantom_queue_object(header) ||
        header->payload_size < sizeof(*phantom_queue)) {
        return NGGC_ERR_CORRUPTION;
    }
    phantom_queue =
        (nggc_internal_phantom_queue *)nggc_object_from_header(header);
    if (nggc_validate_phantom_queue_locked(heap, phantom_queue) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_count = phantom_queue->pending_count;
    return NGGC_OK;
}

size_t nggc_internal_phantom_queue_count(nggc_heap *heap, void *queue) {
    nggc_result result;
    size_t count = 0;

    if (!nggc_heap_live(heap) || !queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_lock(heap);
    result = nggc_phantom_queue_count_locked(heap, queue, &count);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }
    return count;
}

nggc_result nggc_phantom_queue_count(nggc_heap *heap,
                                     void *queue,
                                     size_t *out_count) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    size_t count = 0;

    if (!out_count) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;
    if (!nggc_heap_live(heap) || !queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_phantom_queue_count_locked(heap, queue, &count);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    *out_count = count;
    return NGGC_OK;
}

static nggc_result nggc_phantom_queue_pop_locked(
    nggc_heap *heap, void *queue, void **out_phantom_ref) {
    nggc_object_header *header;
    nggc_internal_phantom_queue *phantom_queue;
    nggc_internal_phantom_reference *phantom;
    void *item;

    header = nggc_live_header_from_object(heap, queue);
    if (!nggc_internal_phantom_queue_object(header) ||
        header->payload_size < sizeof(*phantom_queue)) {
        return NGGC_ERR_CORRUPTION;
    }
    phantom_queue =
        (nggc_internal_phantom_queue *)nggc_object_from_header(header);
    if (nggc_validate_phantom_queue_locked(heap, phantom_queue) != NGGC_OK) {
        return NGGC_ERR_CORRUPTION;
    }
    if (phantom_queue->pending_count == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    item = phantom_queue->head;
    phantom = (nggc_internal_phantom_reference *)item;
    phantom_queue->head = phantom->next;
    --phantom_queue->pending_count;
    if (phantom_queue->pending_count == 0u) {
        phantom_queue->tail = 0;
    }
    phantom->next = 0;
    phantom->enqueued = 0u;
    nggc_phantom_reaper_notify_locked(heap);
    *out_phantom_ref = item;
    return NGGC_OK;
}

void *nggc_internal_phantom_queue_pop(nggc_heap *heap, void *queue) {
    nggc_result result;
    void *item = 0;

    if (!nggc_heap_live(heap) || !queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_lock(heap);
    result = nggc_phantom_queue_pop_locked(heap, queue, &item);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }
    return item;
}

nggc_result nggc_phantom_queue_pop(nggc_heap *heap,
                                   void *queue,
                                   void **out_phantom_ref) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    void *item = 0;

    if (!out_phantom_ref) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_phantom_ref = 0;
    if (!nggc_heap_live(heap) || !queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_phantom_queue_pop_locked(heap, queue, &item);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    *out_phantom_ref = item;
    return NGGC_OK;
}

nggc_result nggc_phantom_reaper_watch(nggc_heap *heap, void *queue) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_object_header *header;
    nggc_internal_phantom_queue *phantom_queue;
    nggc_phantom_reaper_slot *slots;
    nggc_result result;
    size_t new_capacity;
    size_t i = 0;

    if (!nggc_heap_live(heap) || !queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_phantom_queue_from_object_locked(heap, queue, &header,
                                                   &phantom_queue);
    (void)phantom_queue;
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    while (i < heap->phantom_reaper_count) {
        int live = nggc_phantom_reaper_slot_live_locked(heap, i, 0);
        if (live < 0) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        if (live == 0) {
            continue;
        }
        if (heap->phantom_reaper_slots[i].queue == queue &&
            heap->phantom_reaper_slots[i].object_id == header->object_id) {
            nggc_unlock(heap);
            return NGGC_OK;
        }
        ++i;
    }
    if (heap->phantom_reaper_count == heap->phantom_reaper_capacity) {
        new_capacity = heap->phantom_reaper_capacity != 0u
                           ? heap->phantom_reaper_capacity * 2u
                           : (size_t)16u;
        if (new_capacity < heap->phantom_reaper_capacity ||
            new_capacity > SIZE_MAX / sizeof(*slots)) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        slots = (nggc_phantom_reaper_slot *)realloc(
            heap->phantom_reaper_slots, new_capacity * sizeof(*slots));
        if (!slots) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        heap->phantom_reaper_slots = slots;
        heap->phantom_reaper_capacity = new_capacity;
    }
    heap->phantom_reaper_slots[heap->phantom_reaper_count].queue = queue;
    heap->phantom_reaper_slots[heap->phantom_reaper_count].object_id =
        header->object_id;
    ++heap->phantom_reaper_count;
    nggc_phantom_reaper_notify_locked(heap);
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_phantom_reaper_unwatch(nggc_heap *heap, void *queue) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_object_header *header;
    nggc_internal_phantom_queue *phantom_queue;
    nggc_result result;
    uint64_t object_id;
    size_t i = 0;

    if (!nggc_heap_live(heap) || !queue) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_phantom_queue_from_object_locked(heap, queue, &header,
                                                   &phantom_queue);
    (void)phantom_queue;
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    object_id = header->object_id;
    while (i < heap->phantom_reaper_count) {
        int live = nggc_phantom_reaper_slot_live_locked(heap, i, 0);
        if (live < 0) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        if (live == 0) {
            continue;
        }
        if (heap->phantom_reaper_slots[i].queue == queue &&
            heap->phantom_reaper_slots[i].object_id == object_id) {
            nggc_phantom_reaper_remove_slot_locked(heap, i);
            nggc_unlock(heap);
            return NGGC_OK;
        }
        ++i;
    }
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_phantom_reaper_poll(nggc_heap *heap,
                                     void **out_queue,
                                     size_t *out_pending_count) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    size_t i = 0;

    if (!out_queue || !out_pending_count) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_queue = 0;
    *out_pending_count = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    while (i < heap->phantom_reaper_count) {
        nggc_internal_phantom_queue *queue = 0;
        int live = nggc_phantom_reaper_slot_live_locked(heap, i, &queue);
        if (live < 0) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        if (live == 0) {
            continue;
        }
        if (queue->pending_count != 0u) {
            *out_queue = heap->phantom_reaper_slots[i].queue;
            *out_pending_count = queue->pending_count;
            nggc_unlock(heap);
            return NGGC_OK;
        }
        ++i;
    }
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_phantom_reaper_wait(nggc_heap *heap,
                                     size_t max_collect_attempts,
                                     void **out_queue,
                                     size_t *out_pending_count) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    size_t remaining_collects = max_collect_attempts;

    if (!out_queue || !out_pending_count) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_queue = 0;
    *out_pending_count = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }

    for (;;) {
        nggc_result result =
            nggc_phantom_reaper_poll(heap, out_queue, out_pending_count);
        if (result != NGGC_OK) {
            return result;
        }
        if (*out_queue && *out_pending_count != 0u) {
            return NGGC_OK;
        }
        if (remaining_collects == 0u) {
            nggc_set_error(heap, NGGC_ERR_BUSY);
            return NGGC_ERR_BUSY;
        }
        result = nggc_collect(heap);
        if (result != NGGC_OK) {
            return result;
        }
        --remaining_collects;
    }
}

nggc_result nggc_phantom_reaper_wait_blocking(
    nggc_heap *heap, void **out_queue, size_t *out_pending_count) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;

    if (!out_queue || !out_pending_count) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_queue = 0;
    *out_pending_count = 0u;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }

    for (;;) {
        uint_fast64_t observed_epoch;
        size_t watched_count;

        result = nggc_phantom_reaper_poll(
            heap, out_queue, out_pending_count);
        if (result != NGGC_OK) {
            return result;
        }
        if (*out_queue && *out_pending_count != 0u) {
            return NGGC_OK;
        }

        nggc_lock(heap);
        watched_count = heap->phantom_reaper_count;
        observed_epoch = atomic_load_explicit(
            &heap->phantom_reaper_event_epoch, memory_order_acquire);
        nggc_unlock(heap);
        if (watched_count == 0u) {
            nggc_set_error(heap, NGGC_ERR_BUSY);
            return NGGC_ERR_BUSY;
        }

#if NGGC_HAVE_C11_THREADS
        if (!heap->safepoint_wait_initialized ||
            mtx_lock(&heap->safepoint_wait_lock) != thrd_success) {
            nggc_set_error(heap, NGGC_ERR_BACKEND);
            return NGGC_ERR_BACKEND;
        }
        while (atomic_load_explicit(&heap->phantom_reaper_event_epoch,
                                    memory_order_acquire) ==
               observed_epoch) {
            if (cnd_wait(&heap->safepoint_wait_condition,
                         &heap->safepoint_wait_lock) != thrd_success) {
                (void)mtx_unlock(&heap->safepoint_wait_lock);
                nggc_set_error(heap, NGGC_ERR_BACKEND);
                return NGGC_ERR_BACKEND;
            }
        }
        if (mtx_unlock(&heap->safepoint_wait_lock) != thrd_success) {
            nggc_set_error(heap, NGGC_ERR_BACKEND);
            return NGGC_ERR_BACKEND;
        }
#else
        while (atomic_load_explicit(&heap->phantom_reaper_event_epoch,
                                    memory_order_acquire) ==
               observed_epoch) {
            atomic_signal_fence(memory_order_seq_cst);
        }
#endif
    }
}

nggc_result nggc_phantom_reaper_pop(nggc_heap *heap,
                                    void **out_queue,
                                    void **out_phantom_ref) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    size_t i = 0;

    if (!out_queue || !out_phantom_ref) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_queue = 0;
    *out_phantom_ref = 0;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    while (i < heap->phantom_reaper_count) {
        nggc_internal_phantom_queue *queue = 0;
        int live = nggc_phantom_reaper_slot_live_locked(heap, i, &queue);
        if (live < 0) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        if (live == 0) {
            continue;
        }
        if (queue->pending_count != 0u) {
            void *item = 0;
            nggc_result result = nggc_phantom_queue_pop_locked(
                heap, heap->phantom_reaper_slots[i].queue, &item);
            if (result != NGGC_OK) {
                nggc_unlock(heap);
                nggc_set_error(heap, result);
                return result;
            }
            *out_queue = heap->phantom_reaper_slots[i].queue;
            *out_phantom_ref = item;
            nggc_unlock(heap);
            return NGGC_OK;
        }
        ++i;
    }
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_root_push(nggc_heap *heap, void **slot) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    void ***roots;
    size_t new_capacity;

    if (!nggc_heap_live(heap) || !slot ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (heap->safepoint_active) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (heap->root_count > heap->root_capacity ||
        heap->root_capacity > SIZE_MAX / sizeof(*heap->roots) ||
        ((heap->root_capacity == 0u) != (heap->roots == 0))) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    for (size_t i = 0; i < heap->root_count; ++i) {
        if (heap->roots[i] == slot) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
            return NGGC_ERR_INVALID_ARGUMENT;
        }
    }
    for (nggc_thread *thread = heap->threads; thread;
         thread = thread->next) {
        for (size_t i = 0; i < thread->root_count; ++i) {
            if (thread->roots[i] == slot) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
        }
    }
    if (heap->root_count == heap->root_capacity) {
        new_capacity = heap->root_capacity != 0
                           ? heap->root_capacity * 2u
                           : (size_t)64u;
        if (new_capacity < heap->root_capacity ||
            new_capacity > SIZE_MAX / sizeof(*roots)) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        roots = (void ***)realloc(heap->roots,
                                  new_capacity * sizeof(*roots));
        if (!roots) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        heap->roots = roots;
        heap->root_capacity = new_capacity;
    }
    heap->roots[heap->root_count++] = slot;
    if ((heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_MARK ||
         heap->mutator_assist_state ==
             NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) &&
        *slot) {
        nggc_mark_ctx mark_ctx;
        mark_ctx.heap = heap;
        mark_ctx.stack = &heap->mutator_assist_mark_stack;
        mark_ctx.result = NGGC_OK;
        nggc_mark_object(&mark_ctx, *slot);
        if (mark_ctx.result != NGGC_OK) {
            --heap->root_count;
            heap->roots[heap->root_count] = 0;
            nggc_unlock(heap);
            nggc_set_error(heap, mark_ctx.result);
            return mark_ctx.result;
        }
    }
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_root_pop(nggc_heap *heap, void **slot) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    if (!nggc_heap_live(heap) || !slot ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (heap->safepoint_active) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (heap->root_count > heap->root_capacity ||
        heap->root_capacity > SIZE_MAX / sizeof(*heap->roots) ||
        ((heap->root_capacity == 0u) != (heap->roots == 0))) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->root_count == 0u ||
        heap->roots[heap->root_count - 1u] != slot) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    --heap->root_count;
    heap->roots[heap->root_count] = 0;
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_thread_root_push(nggc_heap *heap, nggc_thread *thread,
                                  void **slot) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    void ***roots;
    size_t new_capacity;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) || !slot ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (!nggc_thread_is_registered(heap, thread) ||
        thread->root_count > thread->root_capacity ||
        thread->root_capacity > SIZE_MAX / sizeof(*thread->roots) ||
        ((thread->root_capacity == 0u) != (thread->roots == 0))) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    if (thread->state != NGGC_THREAD_RUNNING) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    for (size_t i = 0; i < heap->root_count; ++i) {
        if (heap->roots[i] == slot) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
            return NGGC_ERR_INVALID_ARGUMENT;
        }
    }
    for (nggc_thread *owner = heap->threads; owner; owner = owner->next) {
        if (owner->root_count > owner->root_capacity ||
            owner->root_capacity > SIZE_MAX / sizeof(*owner->roots) ||
            ((owner->root_capacity == 0u) != (owner->roots == 0))) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        for (size_t i = 0; i < owner->root_count; ++i) {
            if (owner->roots[i] == slot) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
        }
    }
    if (thread->root_count == thread->root_capacity) {
        new_capacity = thread->root_capacity != 0u
                           ? thread->root_capacity * 2u
                           : (size_t)16u;
        if (new_capacity < thread->root_capacity ||
            new_capacity > SIZE_MAX / sizeof(*roots)) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        roots = (void ***)realloc(thread->roots,
                                  new_capacity * sizeof(*roots));
        if (!roots) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
            return NGGC_ERR_OUT_OF_MEMORY;
        }
        thread->roots = roots;
        thread->root_capacity = new_capacity;
    }
    thread->roots[thread->root_count++] = slot;
    if ((heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_MARK ||
         heap->mutator_assist_state ==
             NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) &&
        *slot) {
        nggc_mark_ctx mark_ctx;
        mark_ctx.heap = heap;
        mark_ctx.stack = &heap->mutator_assist_mark_stack;
        mark_ctx.result = NGGC_OK;
        nggc_mark_object(&mark_ctx, *slot);
        if (mark_ctx.result != NGGC_OK) {
            --thread->root_count;
            thread->roots[thread->root_count] = 0;
            nggc_unlock(heap);
            nggc_set_error(heap, mark_ctx.result);
            return mark_ctx.result;
        }
    }
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_thread_root_pop(nggc_heap *heap, nggc_thread *thread,
                                 void **slot) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) || !slot ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (!nggc_thread_is_registered(heap, thread) ||
        thread->root_count > thread->root_capacity ||
        thread->root_capacity > SIZE_MAX / sizeof(*thread->roots) ||
        ((thread->root_capacity == 0u) != (thread->roots == 0))) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    if (thread->state != NGGC_THREAD_RUNNING) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (thread->root_count == 0u ||
        thread->roots[thread->root_count - 1u] != slot) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    --thread->root_count;
    thread->roots[thread->root_count] = 0;
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_thread_register(nggc_heap *heap,
                                 nggc_thread **out_thread) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_thread *thread;
    if (!nggc_heap_live(heap) || !out_thread) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_thread = 0;
    thread = (nggc_thread *)calloc(1u, sizeof(*thread));
    if (!thread) {
        nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    thread->remset_card_buffer = (size_t *)calloc(
        heap->remset_buffer_capacity, sizeof(*thread->remset_card_buffer));
    if (!thread->remset_card_buffer) {
        free(thread);
        nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    thread->remset_card_buffer_capacity = heap->remset_buffer_capacity;
    atomic_init(&thread->remset_card_buffer_count, 0u);
    atomic_init(&thread->remset_card_publication_inflight, 0u);
    thread->remset_card_publication_index = SIZE_MAX;
    thread->remset_card_publication_card = SIZE_MAX;
    atomic_init(&thread->remset_card_deferred_outcome,
                NGGC_REMSET_DEFERRED_OUTCOME_NONE);
    atomic_init(&thread->remset_classification_publication_inflight, 0u);
    thread->remset_classification_owner_region = SIZE_MAX;
    thread->remset_classification_value_region = SIZE_MAX;
    thread->remset_classification_owner_generation =
        NGGC_REGION_GENERATION_NONE;
    thread->remset_classification_value_generation =
        NGGC_REGION_GENERATION_NONE;
    atomic_init(&thread->remset_classification_deferred_outcome,
                NGGC_REMSET_CLASSIFICATION_NONE);
    atomic_init(&thread->tlab_fast_top, 0u);
    atomic_init(&thread->tlab_fast_inflight, 0u);
#if NGGC_HAVE_C11_THREADS
    thread->owner_thread = thrd_current();
    thread->owner_thread_bound = 1u;
#endif
    nggc_lock(heap);
    if (heap->safepoint_active) {
        nggc_unlock(heap);
        free(thread->remset_card_buffer);
        free(thread);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    thread->magic = NGGC_THREAD_MAGIC;
    thread->heap = heap;
    thread->observed_safepoint_epoch = heap->safepoint_epoch;
    thread->state = NGGC_THREAD_RUNNING;
    thread->tlab_region_index = SIZE_MAX;
    thread->tlab_target_size = heap->tlab_size;
    thread->next = heap->threads;
    if (heap->registered_thread_count == SIZE_MAX) {
        nggc_unlock(heap);
        free(thread->remset_card_buffer);
        free(thread);
        nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    heap->threads = thread;
    ++heap->registered_thread_count;
    nggc_barrier_tss_refresh_current_locked(heap);
    nggc_unlock(heap);
    *out_thread = thread;
    return NGGC_OK;
}

nggc_result nggc_thread_unregister(nggc_heap *heap, nggc_thread *thread) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_thread **cursor;
    uint64_t pin_lease_count;
    nggc_result pin_result;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        !nggc_thread_owned_by_current(thread)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    cursor = &heap->threads;
    while (*cursor && *cursor != thread) {
        cursor = &(*cursor)->next;
    }
    if (*cursor != thread) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->registered_thread_count == 0u) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    if (thread->root_count > thread->root_capacity ||
        thread->root_capacity > SIZE_MAX / sizeof(*thread->roots) ||
        ((thread->root_capacity == 0u) != (thread->roots == 0))) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    if (thread->root_count != 0u) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (heap->safepoint_active ||
        thread->state == NGGC_THREAD_AT_SAFEPOINT) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    pin_result = nggc_pin_lease_owner_count_locked(
        heap, thread, &pin_lease_count);
    if (pin_result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, pin_result);
        return pin_result;
    }
    if (pin_lease_count != 0u) {
        if (heap->pin_thread_exit_busy_count != UINT64_MAX) {
            ++heap->pin_thread_exit_busy_count;
        }
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (nggc_remset_flush_thread_locked(heap, thread) != NGGC_OK ||
        nggc_tlab_fast_close_and_flush_locked(heap) != NGGC_OK ||
        nggc_tlab_retire_locked(heap, thread) != NGGC_OK) {
        nggc_tlab_fast_maybe_open_locked(heap);
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    *cursor = thread->next;
    if (thread->observed_safepoint_epoch != heap->safepoint_epoch &&
        heap->safepoint_pending_threads != 0) {
        --heap->safepoint_pending_threads;
        ++heap->safepoint_ack_count;
    }
    --heap->registered_thread_count;
    thread->magic = NGGC_THREAD_CLOSED_MAGIC;
    thread->state = NGGC_THREAD_UNREGISTERED;
    thread->heap = 0;
    thread->next = 0;
    nggc_barrier_tss_refresh_current_locked(heap);
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    free(thread->roots);
    free(thread->remset_card_buffer);
    free(thread);
    return NGGC_OK;
}

nggc_result nggc_request_safepoint(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_verify_phase_state_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_assist_state_verify_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    if (heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active || heap->safepoint_active) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    heap->collect_safepoint_epoch = 0u;
    nggc_allocator_lease_revoke_locked(heap);
    ++heap->safepoint_epoch;
    if (heap->safepoint_epoch == 0u) {
        heap->safepoint_epoch = 1u;
    }
    heap->safepoint_pending_threads = heap->registered_thread_count;
    heap->safepoint_parked_threads = 0u;
    heap->safepoint_active = 1u;
    ++heap->safepoint_request_count;
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_safepoint(nggc_heap *heap, nggc_thread *thread) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (!nggc_thread_is_registered(heap, thread)) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (thread->state == NGGC_THREAD_COLLECTOR_OWNED) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (thread->state == NGGC_THREAD_CONCURRENT_RUNNING) {
        if (!nggc_mutator_concurrent_moving_locked(heap) ||
            thread->observed_safepoint_epoch != heap->safepoint_epoch) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->mutator_concurrent_repark_requested != 0u) {
            if (nggc_remset_flush_thread_locked(heap, thread) != NGGC_OK) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            if (heap->mutator_concurrent_running_threads == 0u ||
                heap->safepoint_parked_threads == SIZE_MAX) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            thread->state = NGGC_THREAD_CONCURRENT_REPARKING;
            --heap->mutator_concurrent_running_threads;
            ++heap->safepoint_parked_threads;
            thread->state = NGGC_THREAD_AT_SAFEPOINT;
        }
        ++thread->safepoint_poll_count;
        ++heap->safepoint_poll_count;
        nggc_unlock(heap);
        return NGGC_OK;
    }
    if (thread->state == NGGC_THREAD_CONCURRENT_REPARKING) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (!heap->safepoint_active) {
        ++thread->safepoint_poll_count;
        ++heap->safepoint_poll_count;
        nggc_unlock(heap);
        return NGGC_OK;
    }
    if (thread->observed_safepoint_epoch != heap->safepoint_epoch) {
        if (nggc_remset_flush_thread_locked(heap, thread) != NGGC_OK ||
            nggc_tlab_retire_locked(heap, thread) != NGGC_OK) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        if (heap->safepoint_pending_threads != 0) {
            --heap->safepoint_pending_threads;
            ++heap->safepoint_ack_count;
        }
        if (heap->safepoint_parked_threads == SIZE_MAX) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        ++heap->safepoint_parked_threads;
    }
    thread->observed_safepoint_epoch = heap->safepoint_epoch;
    thread->state = NGGC_THREAD_AT_SAFEPOINT;
    ++thread->safepoint_poll_count;
    ++heap->safepoint_poll_count;
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_safepoint_wait(nggc_heap *heap,
                                size_t *out_pending_threads) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    size_t pending;
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    pending = heap->safepoint_pending_threads;
    ++heap->safepoint_wait_count;
    if (out_pending_threads) {
        *out_pending_threads = pending;
    }
    nggc_unlock(heap);
    return pending == 0 ? NGGC_OK : NGGC_ERR_BUSY;
}

static nggc_result nggc_safepoint_release_locked(nggc_heap *heap) {
    nggc_thread *thread;
    nggc_result wake_result = NGGC_OK;
    nggc_result reclaim_result;
    if (!heap || !heap->safepoint_active) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->safepoint_pending_threads != 0u ||
        heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active) {
        return NGGC_ERR_BUSY;
    }
    if (heap->safepoint_parked_threads !=
        heap->registered_thread_count) {
        return NGGC_ERR_CORRUPTION;
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        size_t published_count;
        size_t effective_count;
        uint32_t inflight;
        uint32_t deferred_outcome;
        uint32_t expected_state =
            thread == heap->collect_safepoint_owner
                ? NGGC_THREAD_COLLECTOR_OWNED
                : NGGC_THREAD_AT_SAFEPOINT;
        if (thread->state != expected_state ||
            thread->observed_safepoint_epoch != heap->safepoint_epoch ||
            nggc_remset_buffer_snapshot_locked(
                heap, thread, &published_count, &effective_count,
                &inflight, &deferred_outcome) != NGGC_OK ||
            published_count != 0u || effective_count != 0u ||
            inflight != 0u || deferred_outcome != 0u) {
            return NGGC_ERR_CORRUPTION;
        }
    }
    if (heap->from_space_quarantine_active &&
        heap->safepoint_epoch !=
            heap->from_space_quarantine_release_epoch) {
        reclaim_result = nggc_from_space_quarantine_reclaim_locked(heap);
        if (reclaim_result != NGGC_OK) {
            return reclaim_result;
        }
    }
    for (thread = heap->threads; thread; thread = thread->next) {
        thread->state = NGGC_THREAD_RUNNING;
    }
    heap->safepoint_active = 0u;
    heap->safepoint_parked_threads = 0u;
    heap->collect_safepoint_epoch = 0u;
    heap->collect_safepoint_owner = 0;
    if (heap->safepoint_release_count != UINT64_MAX) {
        ++heap->safepoint_release_count;
    }
#if NGGC_HAVE_C11_THREADS
    if (!heap->safepoint_wait_initialized ||
        mtx_lock(&heap->safepoint_wait_lock) != thrd_success) {
        atomic_store_explicit(&heap->safepoint_release_epoch,
                              heap->safepoint_epoch,
                              memory_order_release);
        return NGGC_ERR_BACKEND;
    }
#endif
    atomic_store_explicit(&heap->safepoint_release_epoch,
                          heap->safepoint_epoch,
                          memory_order_release);
#if NGGC_HAVE_C11_THREADS
    if (cnd_broadcast(&heap->safepoint_wait_condition) != thrd_success) {
        wake_result = NGGC_ERR_BACKEND;
    }
    if (mtx_unlock(&heap->safepoint_wait_lock) != thrd_success) {
        wake_result = NGGC_ERR_BACKEND;
    }
#endif
    if (wake_result == NGGC_OK) {
        nggc_tlab_fast_maybe_open_locked(heap);
    }
    return wake_result;
}

nggc_result nggc_safepoint_release(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_safepoint_release_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

nggc_result nggc_safepoint_park(nggc_heap *heap, nggc_thread *thread) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    uint64_t wait_epoch;
    int allow_resume;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_safepoint(heap, thread);
    if (result != NGGC_OK) {
        return result;
    }

    for (;;) {
        nggc_lock(heap);
        wait_epoch = thread->observed_safepoint_epoch;
        if (thread->state == NGGC_THREAD_RUNNING ||
            thread->state == NGGC_THREAD_CONCURRENT_RUNNING) {
            nggc_unlock(heap);
            return NGGC_OK;
        }
        if (thread->state != NGGC_THREAD_AT_SAFEPOINT ||
            !heap->safepoint_active) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        /* A parked thread in an epoch that already resumed has reparked.
           It must wait for final release, not consume that resume again.
           Keep the published resume epoch for initial waiters that have
           not yet woken; clearing it could lose their notification. */
        allow_resume = atomic_load_explicit(&heap->safepoint_resume_epoch,
                                            memory_order_acquire) != wait_epoch;
        nggc_unlock(heap);

#if NGGC_HAVE_C11_THREADS
        if (mtx_lock(&heap->safepoint_wait_lock) != thrd_success) {
            nggc_set_error(heap, NGGC_ERR_BACKEND);
            return NGGC_ERR_BACKEND;
        }
        while (atomic_load_explicit(&heap->safepoint_release_epoch,
                                    memory_order_acquire) != wait_epoch &&
               (!allow_resume ||
                atomic_load_explicit(&heap->safepoint_resume_epoch,
                                     memory_order_acquire) != wait_epoch)) {
            int wait_result;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
            atomic_fetch_add_explicit(&heap->test_safepoint_blocked_threads,
                                      1u, memory_order_release);
#endif
            wait_result = cnd_wait(&heap->safepoint_wait_condition,
                                   &heap->safepoint_wait_lock);
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
            atomic_fetch_sub_explicit(&heap->test_safepoint_blocked_threads,
                                      1u, memory_order_release);
#endif
            if (wait_result != thrd_success) {
                (void)mtx_unlock(&heap->safepoint_wait_lock);
                nggc_set_error(heap, NGGC_ERR_BACKEND);
                return NGGC_ERR_BACKEND;
            }
        }
        if (mtx_unlock(&heap->safepoint_wait_lock) != thrd_success) {
            nggc_set_error(heap, NGGC_ERR_BACKEND);
            return NGGC_ERR_BACKEND;
        }
#else
        while (atomic_load_explicit(&heap->safepoint_release_epoch,
                                    memory_order_acquire) != wait_epoch &&
               (!allow_resume ||
                atomic_load_explicit(&heap->safepoint_resume_epoch,
                                     memory_order_acquire) != wait_epoch)) {
            atomic_signal_fence(memory_order_seq_cst);
        }
#endif
    }
}

static nggc_result nggc_collect_prepare_safepoint_locked(nggc_heap *heap) {
    nggc_result result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    result = nggc_verify_phase_state_locked(heap);
    if (result != NGGC_OK) {
        return result;
    }
    if (heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active) {
        return NGGC_ERR_BUSY;
    }
    if (heap->from_space_quarantine_active) {
        result = nggc_from_space_quarantine_reclaim_locked(heap);
        if (result == NGGC_OK) {
            return NGGC_OK;
        }
        if (result != NGGC_ERR_BUSY) {
            return result;
        }
        if (heap->safepoint_active &&
            heap->safepoint_epoch ==
                heap->from_space_quarantine_release_epoch) {
            return NGGC_ERR_BUSY;
        }
        if ((heap->flags & NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT) == 0u) {
            return NGGC_ERR_BUSY;
        }
    }
    if ((heap->flags & NGGC_CONFIG_COLLECT_AUTO_SAFEPOINT) == 0u ||
        heap->registered_thread_count == 0u) {
        return NGGC_OK;
    }

    if (heap->collect_safepoint_epoch != 0u &&
        heap->collect_safepoint_epoch == heap->safepoint_epoch &&
        heap->safepoint_pending_threads == 0u &&
        heap->safepoint_active &&
        heap->safepoint_parked_threads == heap->registered_thread_count) {
        return NGGC_OK;
    }

    if (heap->safepoint_active &&
        heap->safepoint_pending_threads == 0u &&
        heap->safepoint_parked_threads == heap->registered_thread_count) {
        return NGGC_OK;
    }

    if (heap->safepoint_active) {
        ++heap->safepoint_wait_count;
        return NGGC_ERR_BUSY;
    }

    ++heap->safepoint_epoch;
    if (heap->safepoint_epoch == 0u) {
        heap->safepoint_epoch = 1u;
    }
    heap->collect_safepoint_epoch = heap->safepoint_epoch;
    nggc_allocator_lease_revoke_locked(heap);
    heap->safepoint_pending_threads = heap->registered_thread_count;
    heap->safepoint_parked_threads = 0u;
    heap->safepoint_active = 1u;
    ++heap->safepoint_request_count;
    ++heap->safepoint_wait_count;
    return heap->safepoint_pending_threads == 0u ? NGGC_OK : NGGC_ERR_BUSY;
}

nggc_result nggc_phase_begin(nggc_heap *heap, uint32_t phase) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    if (!nggc_heap_live(heap) || !nggc_phase_valid(phase)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result == NGGC_OK) {
        result = nggc_phase_begin_locked(heap, phase);
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

nggc_result nggc_phase_end(nggc_heap *heap, uint32_t phase) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_result result;
    if (!nggc_heap_live(heap) || !nggc_phase_valid(phase)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_phase_end_locked(heap, phase);
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

static int nggc_mutator_concurrent_moving_locked(const nggc_heap *heap) {
    return heap && heap->mutator_concurrent_active != 0u &&
           heap->collector_cycle_active &&
           heap->collector_cycle_kind ==
               NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION &&
           heap->gc_phase == NGGC_PHASE_EVACUATE &&
           heap->read_barrier_active != 0u;
}

static nggc_result nggc_mutator_concurrent_wait_repark(nggc_heap *heap) {
    nggc_result result;
    if (!heap) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    for (;;) {
        nggc_lock(heap);
        if (!nggc_mutator_concurrent_moving_locked(heap) ||
            heap->mutator_concurrent_repark_requested == 0u) {
            result = NGGC_ERR_CORRUPTION;
        } else if (heap->mutator_concurrent_running_threads != 0u) {
            result = NGGC_ERR_BUSY;
        } else {
            result = nggc_mutator_concurrent_finish_repark_locked(heap);
        }
        nggc_unlock(heap);
        if (result != NGGC_ERR_BUSY) {
            return result;
        }
#if NGGC_HAVE_C11_THREADS
        thrd_yield();
#else
        atomic_signal_fence(memory_order_seq_cst);
#endif
    }
}

void *nggc_load(nggc_heap *heap, void **slot) {
    NGGC_SCAN_API_GUARD(0);
    void *value = 0;
    nggc_object_header *source;
    nggc_object_header *canonical;
    nggc_object_header *payload_owner = 0;
    nggc_object_header *canonical_owner = 0;
    void **canonical_slot = slot;
    nggc_result result = NGGC_OK;
    int payload_owner_locked = 0;
    int canonical_owner_locked = 0;
    int concurrent_moving;
    if (!nggc_heap_live(heap) || !slot ||
        ((uintptr_t)slot % _Alignof(void *)) != 0u) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_lock(heap);
    concurrent_moving = nggc_mutator_concurrent_moving_locked(heap);
    if (heap->safepoint_active && !concurrent_moving) {
        if (heap->mutator_concurrent_rejected_count != UINT64_MAX) {
            ++heap->mutator_concurrent_rejected_count;
        }
        result = NGGC_ERR_BUSY;
        goto load_done;
    }
    if (heap->read_barrier_active) {
        result = nggc_payload_owner_from_slot_locked(
            heap, slot, &payload_owner);
        if (result != NGGC_OK) {
            goto load_done;
        }
        if (payload_owner) {
            if (concurrent_moving) {
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
                if (atomic_load_explicit(&heap->test_concurrent_copy_entered,
                                         memory_order_acquire) == 1u &&
                    atomic_load_explicit(&heap->test_concurrent_copy_source,
                                         memory_order_acquire) ==
                        nggc_object_from_header(payload_owner)) {
                    atomic_store_explicit(
                        &heap->test_concurrent_copy_load_entered, 1u,
                        memory_order_release);
                }
#endif
                nggc_payload_lock_acquire(payload_owner);
                payload_owner_locked = 1;
            }
            /* Copy can publish forwarding while this load waits for the
               source lock. Resolve the owner after acquiring that lock. */
            result = nggc_barrier_resolve_object_locked(
                heap, nggc_object_from_header(payload_owner), 0,
                &canonical_owner);
            if (result != NGGC_OK) {
                goto load_done;
            }
            if (canonical_owner != payload_owner) {
                uintptr_t offset = (uintptr_t)slot -
                    (uintptr_t)nggc_object_from_header(payload_owner);
                canonical_slot = (void **)(
                    (uintptr_t)nggc_object_from_header(canonical_owner) +
                    offset);
                if (!nggc_slot_in_object(canonical_owner, canonical_slot)) {
                    result = NGGC_ERR_CORRUPTION;
                    goto load_done;
                }
                if (concurrent_moving) {
                    nggc_payload_lock_acquire(canonical_owner);
                    canonical_owner_locked = 1;
                }
            }
        }
    }
    if (concurrent_moving) {
        if (heap->mutator_concurrent_load_count != UINT64_MAX) {
            ++heap->mutator_concurrent_load_count;
        }
    }
    value = *canonical_slot;
    ++heap->load_barrier_count;
    if (heap->read_barrier_active) {
        ++heap->read_barrier_active_load_count;
        if (value) {
            result = nggc_barrier_resolve_object_locked(
                heap, value, &source, &canonical);
            if (result != NGGC_OK) {
                goto load_done;
            }
            if (canonical != source) {
                value = nggc_object_from_header(canonical);
                if (atomic_load_explicit(&source->state,
                                         memory_order_acquire) ==
                    NGGC_OBJECT_FORWARDED) {
                    *canonical_slot = value;
                }
            }
        }
    } else if (value && !nggc_live_header_from_object(heap, value)) {
        result = NGGC_ERR_CORRUPTION;
    }

load_done:
    if (canonical_owner_locked) {
        nggc_payload_lock_release(canonical_owner);
    }
    if (payload_owner_locked) {
        nggc_payload_lock_release(payload_owner);
    }
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }
    return value;
}

nggc_result nggc_store(nggc_heap *heap, void *owner, void **slot,
                       void *value) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_object_header *owner_source = 0;
    nggc_object_header *owner_header = 0;
    nggc_object_header *value_source = 0;
    nggc_object_header *value_header = 0;
    void **canonical_slot = slot;
    void *canonical_value = value;
    nggc_remset_publication remset_publication = {0};
    nggc_result publication_result;
    nggc_result result = NGGC_OK;
    int concurrent_moving;
    int owner_source_locked = 0;
    int owner_header_locked = 0;
    int value_source_locked = 0;
    if (!nggc_heap_live(heap) || !slot) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    concurrent_moving = nggc_mutator_concurrent_moving_locked(heap);
    if (heap->safepoint_active && !concurrent_moving) {
        if (heap->mutator_concurrent_rejected_count != UINT64_MAX) {
            ++heap->mutator_concurrent_rejected_count;
        }
        result = NGGC_ERR_BUSY;
        goto store_done;
    }
    ++heap->store_barrier_count;
    if (concurrent_moving && !owner) {
        if (heap->mutator_concurrent_rejected_count != UINT64_MAX) {
            ++heap->mutator_concurrent_rejected_count;
        }
        result = NGGC_ERR_BUSY;
        goto store_done;
    }
    if (owner) {
        if (heap->read_barrier_active) {
            result = nggc_barrier_resolve_object_locked(
                heap, owner, &owner_source, &owner_header);
        } else {
            owner_source = nggc_live_header_from_object(heap, owner);
            owner_header = owner_source;
            result = owner_header ? NGGC_OK : NGGC_ERR_CORRUPTION;
        }
        if (result != NGGC_OK || !nggc_slot_in_object(owner_source, slot)) {
            result = NGGC_ERR_CORRUPTION;
            goto store_done;
        }
        if (concurrent_moving) {
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
            if (atomic_load_explicit(&heap->test_concurrent_copy_entered,
                                     memory_order_acquire) == 1u &&
                atomic_load_explicit(&heap->test_concurrent_copy_source,
                                     memory_order_acquire) == owner) {
                atomic_store_explicit(&heap->test_concurrent_copy_store_entered,
                                      1u, memory_order_release);
            }
#endif
            nggc_payload_lock_acquire(owner_source);
            owner_source_locked = 1;
            if (heap->read_barrier_active) {
                result = nggc_barrier_resolve_object_locked(
                    heap, owner, &owner_source, &owner_header);
            } else {
                owner_header = owner_source;
                result = NGGC_OK;
            }
            if (result != NGGC_OK ||
                !nggc_slot_in_object(owner_source, slot)) {
                result = NGGC_ERR_CORRUPTION;
                goto store_done;
            }
        }
        if (owner_header != owner_source) {
            uintptr_t offset = (uintptr_t)slot -
                               (uintptr_t)nggc_object_from_header(
                                   owner_source);
            canonical_slot = (void **)((uintptr_t)nggc_object_from_header(
                                           owner_header) +
                                       offset);
            if (!nggc_slot_in_object(owner_header, canonical_slot)) {
                result = NGGC_ERR_CORRUPTION;
                goto store_done;
            }
            if (concurrent_moving) {
                nggc_payload_lock_acquire(owner_header);
                owner_header_locked = 1;
            }
        }
    }
    if (value) {
        if (heap->read_barrier_active) {
            result = nggc_barrier_resolve_object_locked(
                heap, value, &value_source, &value_header);
        } else {
            value_header = nggc_live_header_from_object(heap, value);
            value_source = value_header;
            result = value_header ? NGGC_OK : NGGC_ERR_CORRUPTION;
        }
        if (result != NGGC_OK) {
            result = NGGC_ERR_CORRUPTION;
            goto store_done;
        }
        if (concurrent_moving && value_source &&
            value_source != owner_source && value_source != owner_header) {
            nggc_payload_lock_acquire(value_source);
            value_source_locked = 1;
            result = nggc_barrier_resolve_object_locked(
                heap, value, &value_source, &value_header);
            if (result != NGGC_OK) {
                result = NGGC_ERR_CORRUPTION;
                goto store_done;
            }
        }
        canonical_value = nggc_object_from_header(value_header);
    }
    if (heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_SWEEP &&
        value_header && value_header->mark_epoch != heap->mark_epoch) {
        result = NGGC_ERR_BUSY;
        goto store_done;
    }
    if ((heap->mutator_assist_state == NGGC_MUTATOR_ASSIST_STATE_MARK ||
         heap->mutator_assist_state ==
             NGGC_MUTATOR_ASSIST_STATE_REFERENCE_PROCESS) &&
        value_header && (!owner_header ||
                         owner_header->mark_epoch == heap->mark_epoch) &&
        value_header->mark_epoch != heap->mark_epoch) {
        nggc_mark_ctx mark_ctx;
        mark_ctx.heap = heap;
        mark_ctx.stack = &heap->mutator_assist_mark_stack;
        mark_ctx.result = NGGC_OK;
        nggc_mark_object(&mark_ctx, canonical_value);
        if (mark_ctx.result != NGGC_OK) {
            result = mark_ctx.result;
            goto store_done;
        }
    }
    if (owner_header) {
        result = nggc_remset_record_write_impl(
            heap, nggc_current_registered_thread_locked(heap),
            owner_header, canonical_slot, canonical_value,
            &remset_publication);
        if (result != NGGC_OK) {
            goto store_done;
        }
    }
    *canonical_slot = canonical_value;
    if (canonical_slot != slot) {
        *slot = canonical_value;
    }
    if (concurrent_moving && heap->mutator_concurrent_store_count !=
                                  UINT64_MAX) {
        ++heap->mutator_concurrent_store_count;
    }

store_done:
    if (value_source_locked) {
        nggc_payload_lock_release(value_source);
    }
    if (owner_header_locked) {
        nggc_payload_lock_release(owner_header);
    }
    if (owner_source_locked) {
        nggc_payload_lock_release(owner_source);
    }
    nggc_unlock(heap);
    publication_result = nggc_remset_publish_deferred(
        &remset_publication);
    if (result == NGGC_OK && publication_result != NGGC_OK) {
        result = publication_result;
    }
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

uint16_t nggc_object_type(const void *object) {
    NGGC_SCAN_API_GUARD(NGGC_TYPE_NONE);
    nggc_object_header *header = nggc_header_from_object(object);
    if (!header || header->magic != NGGC_OBJECT_MAGIC ||
        atomic_load_explicit(&header->state, memory_order_acquire) !=
            NGGC_OBJECT_LIVE) {
        return NGGC_TYPE_NONE;
    }
    return header->type_id;
}

size_t nggc_object_size(const void *object) {
    NGGC_SCAN_API_GUARD(0);
    nggc_object_header *header = nggc_header_from_object(object);
    if (!header || header->magic != NGGC_OBJECT_MAGIC ||
        atomic_load_explicit(&header->state, memory_order_acquire) !=
            NGGC_OBJECT_LIVE) {
        return 0;
    }
    return header->payload_size;
}

uint32_t nggc_object_flags(const void *object) {
    NGGC_SCAN_API_GUARD(0);
    nggc_object_header *header = nggc_header_from_object(object);
    if (!header || header->magic != NGGC_OBJECT_MAGIC ||
        atomic_load_explicit(&header->state, memory_order_acquire) !=
            NGGC_OBJECT_LIVE) {
        return 0u;
    }
    return header->flags;
}

uint32_t nggc_object_pin_count(const void *object) {
    NGGC_SCAN_API_GUARD(0);
    nggc_object_header *header = nggc_header_from_object(object);
    if (!header || header->magic != NGGC_OBJECT_MAGIC ||
        atomic_load_explicit(&header->state, memory_order_acquire) !=
            NGGC_OBJECT_LIVE) {
        return 0u;
    }
    return nggc_pin_count_load(header);
}

nggc_result nggc_pin(nggc_heap *heap, void *object) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_object_header *header;
    nggc_thread *owner;
    nggc_result result;
    uint32_t pin_count;

    if (!nggc_heap_live(heap) || !object) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }

    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        goto done;
    }
    header = nggc_live_header_from_object(heap, object);
    if (!header) {
        result = NGGC_ERR_INVALID_ARGUMENT;
        goto done;
    }
    result = nggc_pin_lease_verify_locked(heap, 0, 0);
    if (result != NGGC_OK) {
        goto done;
    }
    if (nggc_pin_operation_busy_locked(heap)) {
        nggc_pin_busy_count_locked(heap);
        result = NGGC_ERR_BUSY;
        goto done;
    }
    if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u) {
        result = NGGC_ERR_UNSUPPORTED;
        goto done;
    }
    result = nggc_pin_current_owner_locked(heap, &owner);
    if (result != NGGC_OK) {
        nggc_pin_busy_count_locked(heap);
        goto done;
    }
    pin_count = nggc_pin_count_load(header);
    if (pin_count == UINT32_MAX || heap->pin_lease_count == UINT64_MAX ||
        (pin_count == 0u && heap->pinned_object_count == SIZE_MAX)) {
        result = NGGC_ERR_OUT_OF_MEMORY;
        goto done;
    }
    result = nggc_pin_lease_add_locked(heap, header, owner);
    if (result != NGGC_OK) {
        goto done;
    }
    atomic_store_explicit(&header->pin_count, pin_count + 1u,
                          memory_order_release);
    if (pin_count == 0u) {
        result = nggc_region_pinned_object_adjust_locked(heap, header, 1);
        if (result != NGGC_OK) {
            atomic_store_explicit(&header->pin_count, 0u,
                                  memory_order_release);
            if (nggc_pin_lease_remove_locked(heap, header, owner) !=
                NGGC_OK) {
                result = NGGC_ERR_CORRUPTION;
            }
            goto done;
        }
        ++heap->pinned_object_count;
    }
    ++heap->pin_lease_count;
    if (heap->pin_acquire_count != UINT64_MAX) {
        ++heap->pin_acquire_count;
    }
    result = NGGC_OK;

done:
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    nggc_set_error(heap, result);
    return result;
}

nggc_result nggc_unpin(nggc_heap *heap, void *object) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_object_header *header;
    nggc_thread *owner;
    nggc_pin_lease_record *lease;
    nggc_result result;
    uint32_t pin_count;

    if (!nggc_heap_live(heap) || !object) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }

    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        goto done;
    }
    header = nggc_live_header_from_object(heap, object);
    if (!header) {
        nggc_pin_unpin_error_count_locked(heap);
        result = NGGC_ERR_INVALID_ARGUMENT;
        goto done;
    }
    result = nggc_pin_lease_verify_locked(heap, 0, 0);
    if (result != NGGC_OK) {
        nggc_pin_unpin_error_count_locked(heap);
        goto done;
    }
    if (nggc_pin_operation_busy_locked(heap)) {
        nggc_pin_busy_count_locked(heap);
        result = NGGC_ERR_BUSY;
        goto done;
    }
    if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u) {
        nggc_pin_unpin_error_count_locked(heap);
        result = NGGC_ERR_UNSUPPORTED;
        goto done;
    }
    result = nggc_pin_current_owner_locked(heap, &owner);
    if (result != NGGC_OK) {
        nggc_pin_busy_count_locked(heap);
        goto done;
    }
    pin_count = nggc_pin_count_load(header);
    if (pin_count == 0u || heap->pin_lease_count == 0u) {
        nggc_pin_unpin_error_count_locked(heap);
        result = NGGC_ERR_INVALID_ARGUMENT;
        goto done;
    }
    if (pin_count == 1u && heap->pinned_object_count == 0u) {
        nggc_pin_unpin_error_count_locked(heap);
        result = NGGC_ERR_CORRUPTION;
        goto done;
    }
    result = nggc_pin_lease_find_locked(heap, header, owner, &lease);
    if (result != NGGC_OK) {
        nggc_pin_unpin_error_count_locked(heap);
        goto done;
    }
    if (!lease) {
        if (nggc_pin_lease_has_other_owner_locked(heap, header, owner)) {
            if (heap->pin_owner_conflict_count != UINT64_MAX) {
                ++heap->pin_owner_conflict_count;
            }
        }
        nggc_pin_unpin_error_count_locked(heap);
        result = NGGC_ERR_INVALID_ARGUMENT;
        goto done;
    }
    if (pin_count == 1u) {
        result = nggc_region_pinned_object_adjust_locked(heap, header, -1);
        if (result != NGGC_OK) {
            nggc_pin_unpin_error_count_locked(heap);
            goto done;
        }
    }
    result = nggc_pin_lease_remove_locked(heap, header, owner);
    if (result != NGGC_OK) {
        if (pin_count == 1u &&
            nggc_region_pinned_object_adjust_locked(heap, header, 1) !=
                NGGC_OK) {
            result = NGGC_ERR_CORRUPTION;
        }
        nggc_pin_unpin_error_count_locked(heap);
        goto done;
    }
    atomic_store_explicit(&header->pin_count, pin_count - 1u,
                          memory_order_release);
    if (pin_count == 1u) {
        --heap->pinned_object_count;
    }
    --heap->pin_lease_count;
    if (heap->pin_release_count != UINT64_MAX) {
        ++heap->pin_release_count;
    }
    result = NGGC_OK;

done:
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    nggc_set_error(heap, result);
    return result;
}

static nggc_result nggc_collect_impl(nggc_heap *heap,
                                     nggc_thread *collect_owner,
                                     int young_only) {
    nggc_mark_stack stack;
    nggc_mark_ctx mark_ctx;
    uint32_t active_phase = NGGC_PHASE_IDLE;
    nggc_result result;
    nggc_result return_result = NGGC_OK;
    int release_auto_safepoint = 0;
    int evacuating_young = 0;
    int evacuation_committed = 0;
    int emergency_completed = 0;
    size_t sweep_work_bytes = 0;
    size_t young_reclaimed_objects = 0u;
    uint64_t trigger_pause_begin_ns = 0u;
    int trigger_pause_started = 0;
    nggc_young_evacuation_plan evacuation_plan;
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }

    memset(&stack, 0, sizeof(stack));
    memset(&evacuation_plan, 0, sizeof(evacuation_plan));
    nggc_lock(heap);
    if (heap->collect_safepoint_owner != collect_owner) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (collect_owner &&
        (!nggc_thread_is_registered(heap, collect_owner) ||
         collect_owner->state != NGGC_THREAD_COLLECTOR_OWNED ||
         collect_owner->observed_safepoint_epoch !=
             heap->safepoint_epoch)) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    trigger_pause_started = nggc_trigger_latency_start_locked(
        heap, &trigger_pause_begin_ns);
    nggc_assist_reset_locked(heap);
    result = nggc_collect_prepare_safepoint_locked(heap);
    if (result != NGGC_OK) {
        nggc_trigger_latency_finish_locked(
            heap, trigger_pause_started, trigger_pause_begin_ns);
        nggc_tlab_fast_maybe_open_locked(heap);
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    release_auto_safepoint = heap->collect_safepoint_epoch != 0u;
    result = nggc_collector_cycle_begin_locked(
        heap, NGGC_COLLECTOR_CYCLE_NON_MOVING);
    if (result != NGGC_OK) {
        nggc_trigger_latency_finish_locked(
            heap, trigger_pause_started, trigger_pause_begin_ns);
        if (release_auto_safepoint) {
            (void)nggc_safepoint_release_locked(heap);
        }
        nggc_tlab_fast_maybe_open_locked(heap);
        nggc_unlock(heap);
        nggc_young_evacuation_plan_dispose(&evacuation_plan);
        free(stack.items);
        nggc_set_error(heap, result);
        return result;
    }
    if (young_only) {
        size_t buffered_cards = 0u;
        if (heap->registered_thread_count != 0u &&
            (!heap->safepoint_active ||
             heap->safepoint_pending_threads != 0u ||
             heap->safepoint_parked_threads !=
                 heap->registered_thread_count)) {
            result = NGGC_ERR_BUSY;
            goto fail_locked;
        }
        result = nggc_verify_remset_buffers_locked(
            heap, &buffered_cards, 0, 0, 0, 0, 0, 0, 0, 0);
        if (result != NGGC_OK || buffered_cards != 0u) {
            result = NGGC_ERR_CORRUPTION;
            goto fail_locked;
        }
        result = nggc_collection_set_select_young_locked(heap);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
#if NGGC_ENABLE_VERIFY
        result = nggc_verify_remset_rebuild_locked(heap);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
#endif
    }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    result = nggc_test_collect_fault_take_locked(
        heap, NGGC_TEST_COLLECT_FAULT_AFTER_CYCLE_BEGIN,
        NGGC_ERR_CORRUPTION);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
#endif

    result = nggc_phase_begin_locked(heap, NGGC_PHASE_ROOT_SNAPSHOT);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    active_phase = NGGC_PHASE_ROOT_SNAPSHOT;

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    result = nggc_test_collect_fault_take_locked(
        heap, NGGC_TEST_COLLECT_FAULT_ROOT_OOM,
        NGGC_ERR_OUT_OF_MEMORY);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
#endif

    result = nggc_advance_mark_epoch_locked(heap);
    if (result != NGGC_OK) {
        goto fail_locked;
    }

    mark_ctx.heap = heap;
    mark_ctx.stack = &stack;
    mark_ctx.result = NGGC_OK;
    if (young_only) {
        result =
            nggc_young_mark_non_collection_implicit_live_locked(heap);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
    }
    result = nggc_mark_roots_locked(heap, &mark_ctx);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    if (young_only) {
        result = nggc_young_mark_remembered_edges_locked(heap, &mark_ctx);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
    }

    result = nggc_phase_end_locked(heap, NGGC_PHASE_ROOT_SNAPSHOT);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    active_phase = NGGC_PHASE_IDLE;

    result = nggc_phase_begin_locked(heap, NGGC_PHASE_MARK);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    active_phase = NGGC_PHASE_MARK;

    result = nggc_drain_mark_stack_locked(&mark_ctx);
    if (result != NGGC_OK) {
        goto fail_locked;
    }

    result = nggc_phase_end_locked(heap, NGGC_PHASE_MARK);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    active_phase = NGGC_PHASE_IDLE;

    result = nggc_phase_begin_locked(heap, NGGC_PHASE_REFERENCE_PROCESS);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    active_phase = NGGC_PHASE_REFERENCE_PROCESS;

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    result = nggc_test_collect_fault_take_locked(
        heap, NGGC_TEST_COLLECT_FAULT_REFERENCE_PROCESS,
        NGGC_ERR_CORRUPTION);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
#endif

    ++heap->reference_process_count;
    result = nggc_reference_process_locked(heap, &mark_ctx);
    if (result != NGGC_OK) {
        goto fail_locked;
    }

    result = nggc_phase_end_locked(heap, NGGC_PHASE_REFERENCE_PROCESS);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    active_phase = NGGC_PHASE_IDLE;

    if (young_only) {
        result = nggc_young_evacuation_plan_locked(
            heap, &evacuation_plan);
        if (heap->young_evacuation_preflight_count != UINT64_MAX) {
            ++heap->young_evacuation_preflight_count;
        }
        if (result == NGGC_ERR_OUT_OF_MEMORY) {
            if (heap->young_evacuation_capacity_fallback_count !=
                UINT64_MAX) {
                ++heap->young_evacuation_capacity_fallback_count;
            }
            heap->young_evacuation_survivor_bytes = 0u;
            heap->young_evacuation_promotion_bytes = 0u;
            result = NGGC_OK;
        } else if (result != NGGC_OK) {
            goto fail_locked;
        } else {
            heap->young_evacuation_survivor_bytes =
                evacuation_plan.survivor_bytes;
            heap->young_evacuation_promotion_bytes =
                evacuation_plan.promotion_bytes;
            result = nggc_collector_cycle_enable_young_evacuation_locked(
                heap);
            if (result != NGGC_OK) {
                goto fail_locked;
            }
            evacuating_young = 1;
        }
    }

    if (evacuating_young) {
        result = nggc_phase_begin_locked(heap, NGGC_PHASE_EVACUATE);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
        active_phase = NGGC_PHASE_EVACUATE;
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        result = nggc_test_collect_fault_take_locked(
            heap, NGGC_TEST_COLLECT_FAULT_BEFORE_FORWARDING,
            NGGC_ERR_CORRUPTION);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
#endif
        result = nggc_young_evacuation_copy_locked(
            heap, &evacuation_plan, &evacuation_committed);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
        result = nggc_test_collect_fault_take_locked(
            heap, NGGC_TEST_COLLECT_FAULT_AFTER_EVACUATE,
            NGGC_ERR_CORRUPTION);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
#endif
        result = nggc_phase_end_locked(heap, NGGC_PHASE_EVACUATE);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
        active_phase = NGGC_PHASE_IDLE;

        result = nggc_phase_begin_locked(heap, NGGC_PHASE_REMAP);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
        active_phase = NGGC_PHASE_REMAP;
        result = nggc_phase_end_locked(heap, NGGC_PHASE_REMAP);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
        active_phase = NGGC_PHASE_IDLE;
    }

    result = nggc_phase_begin_locked(heap, NGGC_PHASE_SWEEP);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    active_phase = NGGC_PHASE_SWEEP;

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
    result = nggc_test_collect_fault_take_locked(
        heap, NGGC_TEST_COLLECT_FAULT_SWEEP,
        NGGC_ERR_CORRUPTION);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
#endif

    if (evacuating_young) {
        result = nggc_sweep_evacuated_collection_set_locked(
            heap, &evacuation_plan, evacuation_plan.source_end,
            &sweep_work_bytes,
            &young_reclaimed_objects);
    } else if (young_only) {
        result = nggc_sweep_unmarked_collection_set_locked(
            heap, &sweep_work_bytes, &young_reclaimed_objects);
    } else {
        result = nggc_sweep_unmarked_locked(heap, 0, 0, 1,
                                            &sweep_work_bytes, 0);
    }
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    if (young_only && evacuation_plan.in_place_region_flags) {
        result = nggc_young_promote_in_place_regions_locked(
            heap, &evacuation_plan);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
    }
    if (evacuating_young) {
        result = nggc_verify_evacuated_collection_set_locked(
            heap, evacuation_plan.source_end);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
    }
    result = nggc_rebuild_free_list_locked(heap);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    if (!young_only) {
        result = nggc_region_decommit_empty_tail_locked(heap);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
        result = nggc_interior_decommit_smart_evaluate_locked(heap);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
        result = nggc_region_decommit_interior_humongous_locked(heap);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
    }
    if (evacuating_young) {
        result = nggc_evacuation_reclaim_complete_locked(heap);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
    }
    result = nggc_phase_end_locked(heap, NGGC_PHASE_SWEEP);
    if (result != NGGC_OK) {
        goto fail_locked;
    }
    active_phase = NGGC_PHASE_IDLE;

finish_collection_locked:
    if (young_only) {
        if (!evacuating_young) {
            result = nggc_age_young_survivors_locked(heap);
            if (result != NGGC_OK) {
                goto fail_locked;
            }
        }
        result = nggc_age_special_objects_locked(heap);
        if (result != NGGC_OK) {
            goto fail_locked;
        }
    }
    nggc_assist_repay_work_locked(heap, 0, sweep_work_bytes);
    if (emergency_completed &&
        heap->young_evacuation_emergency_completion_count != UINT64_MAX) {
        ++heap->young_evacuation_emergency_completion_count;
    }
    nggc_trigger_latency_finish_locked(
        heap, trigger_pause_started, trigger_pause_begin_ns);
    ++heap->collection_count;
    heap->gc_trigger_pending = 0u;
    heap->gc_trigger_last_collection_allocated_bytes =
        heap->allocated_bytes;
    heap->gc_trigger_last_used =
        (size_t)(heap->arena_cursor - (uintptr_t)heap->gc_arena_base);
    if (young_only) {
        if (heap->young_collection_count != UINT64_MAX) {
            ++heap->young_collection_count;
        }
        heap->young_reclaimed_object_count =
            nggc_saturating_add_u64_size(
                heap->young_reclaimed_object_count,
                young_reclaimed_objects);
        nggc_collection_set_clear_locked(heap);
    }
    if (release_auto_safepoint) {
        result = nggc_safepoint_release_locked(heap);
        if (result != NGGC_OK) {
            return_result = result;
        }
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_phantom_reaper_notify_locked(heap);
    nggc_unlock(heap);
    nggc_young_evacuation_plan_dispose(&evacuation_plan);
    free(stack.items);
    if (return_result != NGGC_OK) {
        nggc_set_error(heap, return_result);
    }
    return return_result;

fail_locked:
    if (evacuating_young && evacuation_committed &&
        heap->collector_cycle_active) {
        nggc_result reported_result = result;
        nggc_result recovery_result =
            nggc_young_evacuation_emergency_complete_locked(
                heap, &evacuation_plan, &evacuation_committed,
                &sweep_work_bytes, &young_reclaimed_objects);
        if (recovery_result == NGGC_OK) {
            active_phase = NGGC_PHASE_IDLE;
            emergency_completed = 1;
            return_result = reported_result;
            goto finish_collection_locked;
        }
        if (heap->young_evacuation_emergency_failure_count != UINT64_MAX) {
            ++heap->young_evacuation_emergency_failure_count;
        }
        result = recovery_result;
        nggc_enter_fail_stop_locked(heap, result);
        nggc_trigger_latency_finish_locked(
            heap, trigger_pause_started, trigger_pause_begin_ns);
        /* Forwarding is committed. Never expose an incomplete moving heap. */
        nggc_phantom_reaper_notify_locked(heap);
        nggc_unlock(heap);
        nggc_young_evacuation_plan_dispose(&evacuation_plan);
        free(stack.items);
        nggc_set_error(heap, result);
        return result;
    }
    if (young_only) {
        nggc_collection_set_clear_locked(heap);
    }
    if (active_phase != NGGC_PHASE_IDLE) {
        (void)nggc_phase_end_locked(heap, active_phase);
    }
    if (heap->collector_cycle_active) {
        (void)nggc_collector_cycle_abort_locked(heap);
    }
    nggc_trigger_latency_finish_locked(
        heap, trigger_pause_started, trigger_pause_begin_ns);
    if (release_auto_safepoint && heap->safepoint_active) {
        nggc_result release_result = nggc_safepoint_release_locked(heap);
        if (release_result != NGGC_OK) {
            result = release_result;
        }
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_phantom_reaper_notify_locked(heap);
    nggc_unlock(heap);
    nggc_young_evacuation_plan_dispose(&evacuation_plan);
    free(stack.items);
    nggc_set_error(heap, result);
    return result;
}

nggc_result nggc_collect(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    return nggc_collect_impl(heap, 0, 0);
}

nggc_result nggc_collect_young(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    return nggc_collect_impl(heap, 0, 1);
}

static size_t nggc_trigger_scaled_limit(size_t limit, uint32_t basis_points) {
    size_t base;
    size_t remainder;
    size_t scaled;
    size_t extra;
    if (basis_points >= 10000u) {
        return limit;
    }
    base = limit / (size_t)10000u;
    remainder = limit % (size_t)10000u;
    scaled = base * (size_t)basis_points;
    extra = (remainder * (size_t)basis_points + (size_t)9999u) /
            (size_t)10000u;
    return scaled > SIZE_MAX - extra ? SIZE_MAX : scaled + extra;
}

static size_t nggc_heap_used_locked(const nggc_heap *heap) {
    uintptr_t base;
    if (!heap || !heap->gc_arena_base) {
        return 0u;
    }
    base = (uintptr_t)heap->gc_arena_base;
    if (heap->arena_cursor < base ||
        heap->arena_cursor - base > heap->gc_arena_size) {
        return SIZE_MAX;
    }
    return (size_t)(heap->arena_cursor - base);
}

static int nggc_trigger_young_available_locked(const nggc_heap *heap) {
    size_t i;
    if (!heap || !heap->regions) {
        return 0;
    }
    for (i = 0u; i < heap->region_count; ++i) {
        if (heap->regions[i].generation == NGGC_REGION_GENERATION_YOUNG &&
            heap->regions[i].live_object_count != 0u) {
            return 1;
        }
    }
    return 0;
}

static nggc_result nggc_trigger_decide_locked(
    nggc_heap *heap, size_t requested_bytes, uint32_t *out_action,
    uint32_t *out_reason, size_t *out_used, size_t *out_projected) {
    size_t used;
    size_t projected;
    size_t threshold;
    size_t next_quantum;
    uint32_t threshold_basis_points = 10000u;
    uint64_t allocated_delta;
    size_t assist_limit;
    int pressure_due;
    int rate_due;
    int assist_due;
    int critical;
    int due = 0;
    uint32_t action = NGGC_TRIGGER_ACTION_NONE;
    uint32_t reason = NGGC_TRIGGER_REASON_NONE;
    if (!heap || !out_action || !out_reason || !out_used ||
        !out_projected || !nggc_trigger_policy_valid(heap->gc_trigger_policy) ||
        heap->gc_trigger_soft_limit == 0u ||
        heap->gc_trigger_quantum == 0u) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->gc_trigger_policy == NGGC_TRIGGER_MANUAL) {
        *out_action = NGGC_TRIGGER_ACTION_NONE;
        *out_reason = NGGC_TRIGGER_REASON_NONE;
        *out_used = nggc_heap_used_locked(heap);
        *out_projected = nggc_saturating_add_size(*out_used,
                                                  requested_bytes);
        return NGGC_ERR_BUSY;
    }
    used = nggc_heap_used_locked(heap);
    projected = nggc_saturating_add_size(used, requested_bytes);
    if (heap->gc_trigger_policy == NGGC_TRIGGER_LATENCY_TARGET) {
        threshold_basis_points = 6000u;
        if (heap->gc_trigger_latency_target_ns != 0u &&
            heap->gc_trigger_pause_ewma_ns >
                heap->gc_trigger_latency_target_ns) {
            threshold_basis_points = 5000u;
        }
    } else if (heap->gc_trigger_policy == NGGC_TRIGGER_THROUGHPUT) {
        threshold_basis_points = 9000u;
    } else if (heap->gc_trigger_policy == NGGC_TRIGGER_MEMORY_SAVING) {
        threshold_basis_points = 5000u;
    }
    threshold = nggc_trigger_scaled_limit(
        heap->gc_trigger_soft_limit, threshold_basis_points);
    if (threshold == 0u) {
        threshold = 1u;
    }
    allocated_delta = heap->allocated_bytes >=
                              heap->gc_trigger_last_collection_allocated_bytes
                          ? heap->allocated_bytes -
                                heap->gc_trigger_last_collection_allocated_bytes
                          : 0u;
    rate_due = allocated_delta >= (uint64_t)heap->gc_trigger_quantum ||
               requested_bytes >= heap->gc_trigger_quantum;
    assist_limit = heap->gc_trigger_quantum > SIZE_MAX / 4u
                       ? SIZE_MAX
                       : heap->gc_trigger_quantum * 4u;
    assist_due = heap->mutator_assist_debt_bytes >= assist_limit;
    critical = projected >= heap->gc_arena_size;
    pressure_due = projected >= threshold;
    if (heap->gc_trigger_pending != 0u || critical) {
        due = 1;
    } else if (heap->gc_trigger_policy == NGGC_TRIGGER_ALLOCATION_RATE) {
        due = pressure_due || rate_due;
    } else if (heap->gc_trigger_policy == NGGC_TRIGGER_SMART) {
        due = pressure_due || assist_due;
    } else {
        due = pressure_due;
    }
    if (due && !critical && heap->gc_trigger_pending == 0u &&
        heap->gc_trigger_last_used != 0u) {
        next_quantum = nggc_saturating_add_size(
            heap->gc_trigger_last_used, heap->gc_trigger_quantum);
        if (projected < next_quantum &&
            !(heap->gc_trigger_policy == NGGC_TRIGGER_ALLOCATION_RATE &&
              rate_due) &&
            !(heap->gc_trigger_policy == NGGC_TRIGGER_SMART &&
              assist_due)) {
            due = 0;
        }
    }
    if (due) {
        switch (heap->gc_trigger_policy) {
            case NGGC_TRIGGER_ALLOCATION_RATE:
                reason = rate_due
                             ? NGGC_TRIGGER_REASON_ALLOCATION_RATE
                             : NGGC_TRIGGER_REASON_HEAP_LIMIT;
                action = nggc_trigger_young_available_locked(heap)
                             ? NGGC_TRIGGER_ACTION_YOUNG
                             : NGGC_TRIGGER_ACTION_FULL;
                break;
            case NGGC_TRIGGER_LATENCY_TARGET:
                reason = NGGC_TRIGGER_REASON_LATENCY;
                action = nggc_trigger_young_available_locked(heap)
                             ? NGGC_TRIGGER_ACTION_YOUNG
                             : NGGC_TRIGGER_ACTION_FULL;
                break;
            case NGGC_TRIGGER_MEMORY_SAVING:
                reason = NGGC_TRIGGER_REASON_MEMORY_PRESSURE;
                action = NGGC_TRIGGER_ACTION_FULL;
                break;
            case NGGC_TRIGGER_SMART:
                if (critical ||
                    (pressure_due &&
                     (nggc_size_ratio_basis_points(
                          projected, heap->gc_trigger_soft_limit) >= 9000u ||
                      assist_due))) {
                    reason = assist_due ? NGGC_TRIGGER_REASON_ASSIST_DEBT
                                         : NGGC_TRIGGER_REASON_MEMORY_PRESSURE;
                    action = NGGC_TRIGGER_ACTION_FULL;
                } else if (nggc_trigger_young_available_locked(heap)) {
                    reason = assist_due ? NGGC_TRIGGER_REASON_ASSIST_DEBT
                                         : NGGC_TRIGGER_REASON_HEAP_LIMIT;
                    action = NGGC_TRIGGER_ACTION_YOUNG;
                } else {
                    reason = NGGC_TRIGGER_REASON_HEAP_LIMIT;
                    action = NGGC_TRIGGER_ACTION_FULL;
                }
                break;
            case NGGC_TRIGGER_THROUGHPUT:
            case NGGC_TRIGGER_HEAP_LIMIT:
                reason = NGGC_TRIGGER_REASON_HEAP_LIMIT;
                action = NGGC_TRIGGER_ACTION_FULL;
                break;
            case NGGC_TRIGGER_MANUAL:
            default:
                due = 0;
                break;
        }
    }
    *out_action = action;
    *out_reason = reason;
    *out_used = used;
    *out_projected = projected;
    return due ? NGGC_OK : NGGC_ERR_BUSY;
}

static nggc_result nggc_trigger_collect_if_needed_internal(
    nggc_heap *heap, size_t requested_bytes, int automatic) {
    nggc_result result;
    uint32_t action = NGGC_TRIGGER_ACTION_NONE;
    uint32_t reason = NGGC_TRIGGER_REASON_NONE;
    size_t used = 0u;
    size_t projected = 0u;
    if (!nggc_heap_live(heap)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    if (heap->gc_trigger_evaluation_count != UINT64_MAX) {
        ++heap->gc_trigger_evaluation_count;
    }
    if (automatic &&
        ((heap->flags & NGGC_CONFIG_COLLECT_AUTO_TRIGGER) == 0u ||
         heap->registered_thread_count != 0u)) {
        if (heap->gc_trigger_noop_count != UINT64_MAX) {
            ++heap->gc_trigger_noop_count;
        }
        nggc_tlab_fast_maybe_open_locked(heap);
        nggc_unlock(heap);
        return NGGC_OK;
    }
    result = nggc_trigger_decide_locked(
        heap, requested_bytes, &action, &reason, &used, &projected);
    if (result == NGGC_ERR_BUSY) {
        if (heap->gc_trigger_noop_count != UINT64_MAX) {
            ++heap->gc_trigger_noop_count;
        }
        nggc_tlab_fast_maybe_open_locked(heap);
        nggc_unlock(heap);
        return NGGC_OK;
    }
    if (result != NGGC_OK) {
        if (heap->gc_trigger_failure_count != UINT64_MAX) {
            ++heap->gc_trigger_failure_count;
        }
        nggc_tlab_fast_maybe_open_locked(heap);
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    heap->gc_trigger_last_action = action;
    heap->gc_trigger_last_reason = reason;
    heap->gc_trigger_last_request_bytes = requested_bytes;
    heap->gc_trigger_last_projected_used = projected;
    heap->gc_trigger_last_used = used;
    heap->gc_trigger_pending = 1u;
    if (heap->gc_trigger_request_count != UINT64_MAX) {
        ++heap->gc_trigger_request_count;
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);

    result = action == NGGC_TRIGGER_ACTION_YOUNG
                 ? nggc_collect_young(heap)
                 : nggc_collect(heap);

    nggc_lock(heap);
    if (result == NGGC_OK) {
        heap->gc_trigger_pending = 0u;
        heap->gc_trigger_last_used = nggc_heap_used_locked(heap);
        heap->gc_trigger_last_collection_allocated_bytes =
            heap->allocated_bytes;
        if (heap->gc_trigger_collection_count != UINT64_MAX) {
            ++heap->gc_trigger_collection_count;
        }
    } else if (result == NGGC_ERR_BUSY) {
        heap->gc_trigger_pending = 1u;
        if (heap->gc_trigger_busy_count != UINT64_MAX) {
            ++heap->gc_trigger_busy_count;
        }
    } else {
        heap->gc_trigger_pending = 0u;
        if (heap->gc_trigger_failure_count != UINT64_MAX) {
            ++heap->gc_trigger_failure_count;
        }
    }
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

nggc_result nggc_collect_if_needed(nggc_heap *heap, size_t requested_bytes) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    return nggc_trigger_collect_if_needed_internal(heap, requested_bytes, 0);
}

static void nggc_auto_trigger_before_allocation(nggc_heap *heap,
                                                 size_t requested_bytes) {
    if (!heap || (heap->flags & NGGC_CONFIG_COLLECT_AUTO_TRIGGER) == 0u) {
        return;
    }
    (void)nggc_trigger_collect_if_needed_internal(heap, requested_bytes, 1);
}

static nggc_result nggc_thread_collect_impl(nggc_heap *heap,
                                             nggc_thread *thread,
                                             int young_only) {
    nggc_result result;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        !nggc_thread_owned_by_current(thread)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    if (!nggc_thread_is_registered(heap, thread)) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (heap->collect_safepoint_owner &&
        heap->collect_safepoint_owner != thread) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_BUSY);
        return NGGC_ERR_BUSY;
    }
    if (!heap->collect_safepoint_owner) {
        result = nggc_verify_phase_state_locked(heap);
        if (result != NGGC_OK) {
            nggc_unlock(heap);
            nggc_set_error(heap, result);
            return result;
        }
        result = nggc_assist_state_verify_locked(heap);
        if (result != NGGC_OK) {
            nggc_unlock(heap);
            nggc_set_error(heap, result);
            return result;
        }
        if (thread->state != NGGC_THREAD_RUNNING ||
            heap->safepoint_active ||
            heap->gc_phase != NGGC_PHASE_IDLE ||
            heap->collector_cycle_active ||
            heap->registered_thread_count == 0u) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_BUSY);
            return NGGC_ERR_BUSY;
        }
        result = nggc_remset_flush_thread_locked(heap, thread);
        if (result == NGGC_OK) {
            result = nggc_tlab_retire_locked(heap, thread);
        }
        if (result != NGGC_OK) {
            nggc_unlock(heap);
            nggc_set_error(heap, result);
            return result;
        }
        nggc_allocator_lease_revoke_locked(heap);
        ++heap->safepoint_epoch;
        if (heap->safepoint_epoch == 0u) {
            heap->safepoint_epoch = 1u;
        }
        heap->safepoint_active = 1u;
        heap->safepoint_pending_threads =
            heap->registered_thread_count - 1u;
        heap->safepoint_parked_threads = 1u;
        heap->collect_safepoint_epoch = heap->safepoint_epoch;
        heap->collect_safepoint_owner = thread;
        thread->observed_safepoint_epoch = heap->safepoint_epoch;
        thread->state = NGGC_THREAD_COLLECTOR_OWNED;
        ++thread->safepoint_poll_count;
        ++heap->safepoint_poll_count;
        ++heap->safepoint_ack_count;
        ++heap->safepoint_request_count;
    } else if (thread->state != NGGC_THREAD_COLLECTOR_OWNED ||
               thread->observed_safepoint_epoch !=
                   heap->safepoint_epoch) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    nggc_unlock(heap);
    return nggc_collect_impl(heap, thread, young_only);
}

nggc_result nggc_thread_collect(nggc_heap *heap, nggc_thread *thread) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    return nggc_thread_collect_impl(heap, thread, 0);
}

nggc_result nggc_thread_collect_young(nggc_heap *heap,
                                      nggc_thread *thread) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    return nggc_thread_collect_impl(heap, thread, 1);
}

static nggc_result nggc_verify_regions_locked(nggc_heap *heap) {
    nggc_region_meta *expected;
    uintptr_t cursor;
    size_t active = 0u;
    size_t decommitted = 0u;
    size_t decommitted_bytes = 0u;
    size_t expected_region_count;
    size_t expected_card_count;
    if (!heap || !heap->regions || heap->region_size == 0u ||
        heap->region_count == 0u ||
        heap->card_size != NGGC_DEFAULT_CARD_SIZE ||
        nggc_validate_heap_geometry(
            heap->gc_arena_size, heap->region_size, heap->tlab_size,
            heap->tlab_sizing_policy, heap->tlab_min_size,
            heap->tlab_max_size,
            &expected_region_count, &expected_card_count) != NGGC_OK ||
        heap->region_count != expected_region_count ||
        heap->card_count != expected_card_count ||
        heap->young_tenuring_threshold == 0u ||
        heap->young_tenuring_threshold > UINT8_MAX ||
        !nggc_interior_decommit_policy_valid(
            heap->interior_decommit_policy) ||
        !nggc_interior_decommit_smart_action_valid(
            heap->interior_decommit_smart_last_action) ||
        heap->interior_decommit_smart_initialized > 1u ||
        heap->interior_decommit_smart_cooldown_remaining >
            NGGC_INTERIOR_DECOMMIT_SMART_COOLDOWN_CYCLES ||
        heap->interior_decommit_smart_committed_pressure_basis_points >
            10000u ||
        heap->interior_decommit_smart_large_free_pressure_basis_points >
            10000u ||
        (heap->interior_decommit_smart_pressure_source !=
             NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_PORTABLE &&
         heap->interior_decommit_smart_pressure_source !=
             NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_CALLBACK) ||
        heap->interior_decommit_smart_external_pressure_valid > 1u ||
        heap->interior_decommit_smart_external_pressure_basis_points >
            10000u ||
        heap->interior_decommit_smart_slow_vm_observed > 1u ||
        (heap->interior_decommit_smart_external_pressure_valid != 0u &&
         (!heap->interior_decommit_smart_initialized ||
          heap->interior_decommit_smart_pressure_source !=
              NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_CALLBACK ||
          !heap->memory_pressure)) ||
        (heap->interior_decommit_smart_external_pressure_valid == 0u &&
         (heap->interior_decommit_smart_pressure_source !=
              NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_PORTABLE ||
          heap->interior_decommit_smart_external_pressure_basis_points !=
              0u ||
          heap->interior_decommit_smart_resident_bytes != 0u ||
          heap->interior_decommit_smart_memory_limit_bytes != 0u)) ||
        (heap->interior_decommit_smart_slow_vm_observed != 0u &&
         (heap->interior_decommit_slow_ns_per_region == 0u ||
          !heap->interior_decommit_smart_initialized ||
          (heap->interior_decommit_smart_last_decommit_latency_sample_delta ==
               0u &&
           heap->interior_decommit_smart_last_recommit_latency_sample_delta ==
               0u))) ||
        (heap->memory_pressure_ctx != 0 && !heap->memory_pressure) ||
        (heap->clock_ns_ctx != 0 && !heap->clock_ns) ||
        (heap->interior_decommit_slow_ns_per_region != 0u &&
         !heap->clock_ns) ||
        (heap->interior_decommit_policy ==
             NGGC_INTERIOR_DECOMMIT_POLICY_DISABLED &&
         heap->interior_decommit_min_regions != 0u) ||
        (heap->interior_decommit_policy ==
             NGGC_INTERIOR_DECOMMIT_POLICY_MANUAL &&
         (heap->interior_decommit_min_regions < 2u ||
          heap->interior_decommit_min_regions > UINT32_MAX)) ||
        (heap->interior_decommit_policy ==
             NGGC_INTERIOR_DECOMMIT_POLICY_SMART &&
         (heap->interior_decommit_min_regions < 2u ||
          heap->interior_decommit_min_regions >
              nggc_interior_decommit_smart_threshold_cap_locked(heap) ||
          (heap->interior_decommit_smart_initialized == 0u) !=
              (heap->interior_decommit_smart_evaluation_count == 0u))) ||
        (heap->interior_decommit_policy !=
             NGGC_INTERIOR_DECOMMIT_POLICY_SMART &&
         (heap->interior_decommit_smart_initialized != 0u ||
          heap->interior_decommit_smart_evaluation_count != 0u ||
          heap->interior_decommit_smart_cooldown_remaining != 0u))) {
        return NGGC_ERR_CORRUPTION;
    }
    expected = (nggc_region_meta *)calloc(heap->region_count,
                                          sizeof(*expected));
    if (!expected) {
        return NGGC_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < heap->region_count; ++i) {
        expected[i].start = heap->regions[i].start;
        expected[i].end = heap->regions[i].end;
        expected[i].allocation_cursor =
            heap->regions[i].decommitted
                ? expected[i].start
                : heap->arena_cursor <= expected[i].start
                ? expected[i].start
                : (heap->arena_cursor < expected[i].end
                       ? heap->arena_cursor
                       : expected[i].end);
        expected[i].decommitted = heap->regions[i].decommitted;
        expected[i].allocation_owner =
            heap->allocator_lease_active &&
                    heap->arena_cursor < heap->arena_end &&
                    heap->arena_cursor >= expected[i].start &&
                    heap->arena_cursor < expected[i].end &&
                    !heap->regions[i].decommitted
                ? (uintptr_t)heap
                : 0u;
        if (expected[i].start !=
                (uintptr_t)heap->gc_arena_base + i * heap->region_size ||
            expected[i].end !=
                (i + 1u == heap->region_count
                     ? heap->arena_end
                     : expected[i].start + heap->region_size) ||
            expected[i].end <= expected[i].start ||
            expected[i].end > heap->arena_end) {
            free(expected);
            return NGGC_ERR_CORRUPTION;
        }
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        uintptr_t object_end;
        uintptr_t part = cursor;
        uint32_t state;
        uint32_t live_region_generation = NGGC_REGION_GENERATION_NONE;
        uint32_t live_region_placement = NGGC_REGION_PLACEMENT_NONE;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            free(expected);
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        object_end = block.end;
        state = block.state;
        if (state == NGGC_OBJECT_DECOMMITTED_HUMONGOUS) {
            size_t start_index =
                (size_t)((cursor -
                          (uintptr_t)heap->gc_arena_base) /
                         heap->region_size);
            uint32_t run_length;
            uint32_t placement;
            if (header ||
                !nggc_interior_decommitted_run_locked(
                    heap, start_index, &run_length, &placement)) {
                free(expected);
                return NGGC_ERR_CORRUPTION;
            }
            for (uint32_t i = 0u; i < run_length; ++i) {
                nggc_region_meta *run_region =
                    &expected[start_index + (size_t)i];
                run_region->decommitted = 1u;
                run_region->placement = placement;
                run_region->humongous_role =
                    i == 0u ? NGGC_REGION_HUMONGOUS_START
                            : NGGC_REGION_HUMONGOUS_CONTINUATION;
                run_region->humongous_run_offset = i;
                run_region->humongous_run_length = run_length;
            }
        } else if (state == NGGC_OBJECT_LIVE) {
            if (!nggc_live_object_generation_valid(header->generation) ||
                header->young_age > heap->young_tenuring_threshold ||
                (header->generation == NGGC_REGION_GENERATION_OLD &&
                 header->young_age != 0u)) {
                free(expected);
                return NGGC_ERR_CORRUPTION;
            }
            {
                uint32_t allocation_generation =
                    nggc_allocation_region_generation(header->flags);
                live_region_generation =
                    allocation_generation == NGGC_REGION_GENERATION_SPECIAL
                        ? NGGC_REGION_GENERATION_SPECIAL
                        : header->generation;
            }
            if (live_region_generation ==
                NGGC_REGION_GENERATION_SPECIAL) {
                live_region_placement =
                    nggc_region_target_placement(header->flags);
                if (live_region_placement ==
                    NGGC_REGION_PLACEMENT_NONE) {
                    free(expected);
                    return NGGC_ERR_CORRUPTION;
                }
            }
        } else if (state == NGGC_OBJECT_FORWARDED) {
            nggc_object_header *target;
            if (!heap->from_space_quarantine_active ||
                cursor >= heap->from_space_quarantine_source_end ||
                header->generation != NGGC_REGION_GENERATION_YOUNG ||
                nggc_forwarding_target_locked(
                    heap, header, &target) != NGGC_OK ||
                !target) {
                free(expected);
                return NGGC_ERR_CORRUPTION;
            }
        } else if (!header ||
                   header->generation != NGGC_REGION_GENERATION_NONE) {
            free(expected);
            return NGGC_ERR_CORRUPTION;
        }
        if (state == NGGC_OBJECT_FREE &&
            (header->flags & ~NGGC_OBJECT_FLAG_LARGE) != 0u) {
            free(expected);
            return NGGC_ERR_CORRUPTION;
        }
        if ((state == NGGC_OBJECT_LIVE ||
             state == NGGC_OBJECT_FREE) &&
            (header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u) {
            size_t start_index;
            uint32_t run_length;
            if (!nggc_region_humongous_geometry(
                    heap, cursor, header->alloc_size,
                    &start_index, &run_length) ||
                run_length < 2u) {
                free(expected);
                return NGGC_ERR_CORRUPTION;
            }
            for (uint32_t i = 0u; i < run_length; ++i) {
                nggc_region_meta *run_region =
                    &expected[start_index + (size_t)i];
                if (run_region->humongous_role !=
                    NGGC_REGION_HUMONGOUS_NONE) {
                    free(expected);
                    return NGGC_ERR_CORRUPTION;
                }
                run_region->humongous_role =
                    i == 0u ? NGGC_REGION_HUMONGOUS_START
                            : NGGC_REGION_HUMONGOUS_CONTINUATION;
                run_region->humongous_run_offset = i;
                run_region->humongous_run_length = run_length;
            }
        }
        if (state == NGGC_OBJECT_LIVE &&
            (header->flags & NGGC_OBJECT_FLAG_LARGE) == 0u &&
            (size_t)((cursor -
                      (uintptr_t)heap->gc_arena_base) %
                     heap->region_size) + header->alloc_size >
                heap->region_size) {
            free(expected);
            return NGGC_ERR_CORRUPTION;
        }
        while (part < object_end) {
            size_t index = (size_t)((part -
                                     (uintptr_t)heap->gc_arena_base) /
                                    heap->region_size);
            uintptr_t part_end;
            size_t part_bytes;
            if (index >= heap->region_count) {
                free(expected);
                return NGGC_ERR_CORRUPTION;
            }
            part_end = object_end < expected[index].end
                           ? object_end
                           : expected[index].end;
            part_bytes = (size_t)(part_end - part);
            if (state != NGGC_OBJECT_DECOMMITTED_HUMONGOUS) {
                expected[index].committed_bytes += part_bytes;
            }
            if (state == NGGC_OBJECT_LIVE) {
                if (heap->regions[index].generation !=
                    live_region_generation) {
                    free(expected);
                    return NGGC_ERR_CORRUPTION;
                }
                if (live_region_generation ==
                    NGGC_REGION_GENERATION_SPECIAL) {
                    if (expected[index].placement ==
                        NGGC_REGION_PLACEMENT_NONE) {
                        expected[index].placement =
                            live_region_placement;
                    } else if (expected[index].placement !=
                               live_region_placement) {
                        free(expected);
                        return NGGC_ERR_CORRUPTION;
                    }
                }
                expected[index].live_bytes += part_bytes;
                if ((header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u) {
                    ++expected[index].large_object_count;
                }
                if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) != 0u) {
                    ++expected[index].pinnable_object_count;
                }
                if (nggc_pin_count_load(header) != 0u) {
                    if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u ||
                        expected[index].pinned_object_count == SIZE_MAX) {
                        free(expected);
                        return NGGC_ERR_CORRUPTION;
                    }
                    ++expected[index].pinned_object_count;
                }
                if ((header->flags & NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u) {
                    ++expected[index].non_movable_object_count;
                }
            } else if (state == NGGC_OBJECT_FREE) {
                expected[index].free_bytes += part_bytes;
            } else if (state == NGGC_OBJECT_TLAB_RESERVED) {
                expected[index].reserved_bytes += part_bytes;
            } else if (state == NGGC_OBJECT_FORWARDED) {
                expected[index].quarantined_bytes += part_bytes;
            } else if (state ==
                       NGGC_OBJECT_DECOMMITTED_HUMONGOUS) {
                if (!expected[index].decommitted) {
                    free(expected);
                    return NGGC_ERR_CORRUPTION;
                }
            } else {
                free(expected);
                return NGGC_ERR_CORRUPTION;
            }
            part = part_end;
        }
        if (state == NGGC_OBJECT_LIVE) {
            size_t index = (size_t)((cursor -
                                     (uintptr_t)heap->gc_arena_base) /
                                    heap->region_size);
            ++expected[index].live_object_count;
        } else if (state == NGGC_OBJECT_FORWARDED) {
            size_t index = (size_t)((cursor -
                                     (uintptr_t)heap->gc_arena_base) /
                                    heap->region_size);
            ++expected[index].quarantined_object_count;
        }
        cursor = object_end;
    }
    for (size_t i = 0; i < heap->region_count; ++i) {
        const nggc_region_meta *actual = &heap->regions[i];
        nggc_region_refresh_flags(&expected[i]);
        if (!nggc_region_generation_valid(actual->generation) ||
            !nggc_region_placement_valid(actual->placement) ||
            !nggc_region_humongous_role_valid(
                actual->humongous_role) ||
            ((actual->humongous_role ==
                  NGGC_REGION_HUMONGOUS_NONE) !=
             (actual->humongous_run_length == 0u)) ||
            (actual->humongous_role ==
                 NGGC_REGION_HUMONGOUS_NONE &&
             actual->humongous_run_offset != 0u) ||
            (actual->humongous_role ==
                 NGGC_REGION_HUMONGOUS_START &&
             (actual->humongous_run_offset != 0u ||
              actual->humongous_run_length < 2u)) ||
            (actual->humongous_role ==
                 NGGC_REGION_HUMONGOUS_CONTINUATION &&
             (actual->humongous_run_offset == 0u ||
              actual->humongous_run_offset >=
                  actual->humongous_run_length)) ||
            (actual->humongous_role !=
                 NGGC_REGION_HUMONGOUS_NONE &&
             ((!actual->decommitted &&
               actual->generation !=
                   NGGC_REGION_GENERATION_SPECIAL) ||
              (actual->decommitted &&
               actual->generation !=
                   NGGC_REGION_GENERATION_NONE))) ||
            (actual->placement != NGGC_REGION_PLACEMENT_NONE &&
             actual->generation != NGGC_REGION_GENERATION_SPECIAL &&
             !actual->decommitted) ||
            (!actual->decommitted &&
             actual->decommitted_free_next != 0u) ||
            (actual->decommitted &&
             actual->humongous_role != NGGC_REGION_HUMONGOUS_START &&
             actual->decommitted_free_next != 0u) ||
            (actual->decommitted &&
             actual->humongous_role == NGGC_REGION_HUMONGOUS_NONE &&
             actual->placement != NGGC_REGION_PLACEMENT_NONE) ||
            ((actual->committed_bytes == 0u) !=
             (actual->generation == NGGC_REGION_GENERATION_NONE)) ||
            (actual->generation == NGGC_REGION_GENERATION_YOUNG &&
             (actual->large_object_count != 0u ||
              actual->non_movable_object_count != 0u)) ||
            (actual->tlab_state != NGGC_TLAB_INACTIVE &&
             actual->generation != NGGC_REGION_GENERATION_YOUNG) ||
            actual->in_collection_set > 1u ||
            (actual->in_collection_set != 0u &&
             (actual->generation != NGGC_REGION_GENERATION_YOUNG ||
              !heap->collector_cycle_active))) {
            free(expected);
            return NGGC_ERR_CORRUPTION;
        }
        if (expected[i].committed_bytes != heap->regions[i].committed_bytes ||
            expected[i].live_bytes != heap->regions[i].live_bytes ||
            expected[i].free_bytes != heap->regions[i].free_bytes ||
            expected[i].reserved_bytes != heap->regions[i].reserved_bytes ||
            expected[i].quarantined_bytes !=
                heap->regions[i].quarantined_bytes ||
            expected[i].live_object_count !=
                heap->regions[i].live_object_count ||
            expected[i].quarantined_object_count !=
                heap->regions[i].quarantined_object_count ||
            expected[i].large_object_count !=
                heap->regions[i].large_object_count ||
            expected[i].pinnable_object_count !=
                heap->regions[i].pinnable_object_count ||
            expected[i].pinned_object_count !=
                heap->regions[i].pinned_object_count ||
            expected[i].non_movable_object_count !=
                heap->regions[i].non_movable_object_count ||
            expected[i].humongous_role !=
                heap->regions[i].humongous_role ||
            expected[i].humongous_run_offset !=
                heap->regions[i].humongous_run_offset ||
            expected[i].humongous_run_length !=
                heap->regions[i].humongous_run_length ||
            expected[i].decommitted !=
                heap->regions[i].decommitted ||
            expected[i].state != heap->regions[i].state ||
            expected[i].flags != heap->regions[i].flags ||
            expected[i].allocation_cursor !=
                heap->regions[i].allocation_cursor ||
            expected[i].allocation_owner !=
                heap->regions[i].allocation_owner ||
            expected[i].live_bytes > expected[i].committed_bytes ||
            expected[i].free_bytes >
                expected[i].committed_bytes - expected[i].live_bytes ||
            expected[i].quarantined_bytes >
                expected[i].committed_bytes - expected[i].live_bytes -
                    expected[i].free_bytes ||
            expected[i].reserved_bytes !=
                expected[i].committed_bytes - expected[i].live_bytes -
                    expected[i].free_bytes -
                    expected[i].quarantined_bytes) {
            free(expected);
            return NGGC_ERR_CORRUPTION;
        }
        if ((expected[i].placement != NGGC_REGION_PLACEMENT_NONE &&
             actual->placement != expected[i].placement) ||
            (expected[i].placement == NGGC_REGION_PLACEMENT_NONE &&
             actual->placement != NGGC_REGION_PLACEMENT_NONE &&
             (actual->generation != NGGC_REGION_GENERATION_SPECIAL ||
              actual->live_bytes != 0u ||
              actual->free_bytes != actual->committed_bytes ||
              actual->committed_bytes == 0u))) {
            free(expected);
            return NGGC_ERR_CORRUPTION;
        }
        if (expected[i].committed_bytes != 0u) {
            ++active;
        }
        if (actual->decommitted) {
            size_t span = nggc_region_span_size(actual);
            if (span == 0u || decommitted == SIZE_MAX ||
                decommitted_bytes > SIZE_MAX - span) {
                free(expected);
                return NGGC_ERR_CORRUPTION;
            }
            ++decommitted;
            decommitted_bytes += span;
        }
    }
    free(expected);
    if (active != heap->active_region_count ||
        decommitted != heap->decommitted_region_count ||
        decommitted_bytes != heap->region_decommitted_bytes) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->allocator_lease_active > 1u ||
        heap->allocator_lease_epoch == 0u ||
        (heap->allocator_lease_active &&
         (heap->safepoint_pending_threads != 0u ||
          heap->gc_phase != NGGC_PHASE_IDLE ||
          heap->collector_cycle_active))) {
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->arena_cursor == heap->arena_end) {
        return heap->allocator_owned_region_index == SIZE_MAX
                   ? NGGC_OK
                   : NGGC_ERR_CORRUPTION;
    }
    return heap->allocator_owned_region_index ==
                   (size_t)((heap->arena_cursor -
                             (uintptr_t)heap->gc_arena_base) /
                            heap->region_size)
               ? NGGC_OK
               : NGGC_ERR_CORRUPTION;
}

static nggc_result nggc_verify_free_list_locked(
    nggc_heap *heap, nggc_object_header *head, uint32_t expected_flags,
    size_t expected_large_bin, size_t max_nodes,
    size_t *out_count, size_t *out_bytes) {
    nggc_object_header *header = head;
    size_t count = 0u;
    size_t bytes = 0u;
    size_t previous_alloc_size = 0u;
    uintptr_t previous_address = 0u;
    if (!heap || !out_count || !out_bytes ||
        (expected_flags & ~NGGC_OBJECT_FLAG_LARGE) != 0u ||
        ((expected_flags == NGGC_OBJECT_FLAG_LARGE) !=
         (expected_large_bin < NGGC_LARGE_FREE_BIN_COUNT))) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    while (header) {
        nggc_block_view block;
        uintptr_t address = (uintptr_t)header;
        size_t actual_large_bin = SIZE_MAX;
        if (count >= max_nodes ||
            address < (uintptr_t)heap->gc_arena_base ||
            address >= heap->arena_cursor ||
            nggc_block_view_locked(heap, address, &block) != NGGC_OK ||
            block.header != header || block.state != NGGC_OBJECT_FREE ||
            header->flags != expected_flags ||
            (expected_flags == NGGC_OBJECT_FLAG_LARGE &&
             (!nggc_region_humongous_run_matches_locked(
                  heap, address, header->alloc_size) ||
              !nggc_large_free_bin_index_locked(
                  heap, header->alloc_size,
                  &actual_large_bin) ||
              actual_large_bin != expected_large_bin ||
              (count != 0u &&
               (header->alloc_size < previous_alloc_size ||
                (header->alloc_size == previous_alloc_size &&
                 address <= previous_address))))) ||
            bytes > SIZE_MAX - header->alloc_size) {
            return NGGC_ERR_CORRUPTION;
        }
        ++count;
        bytes += header->alloc_size;
        previous_alloc_size = header->alloc_size;
        previous_address = address;
        header = (nggc_object_header *)
            nggc_forwarding_load_relaxed(header);
    }
    *out_count = count;
    *out_bytes = bytes;
    return NGGC_OK;
}

static nggc_result nggc_verify_decommitted_large_bins_locked(
    nggc_heap *heap, size_t max_runs, size_t *out_runs,
    size_t *out_regions, size_t *out_bytes, uint32_t *out_mask) {
    size_t runs = 0u;
    size_t regions = 0u;
    size_t bytes = 0u;
    uint32_t mask = 0u;
    if (!heap || !out_runs || !out_regions || !out_bytes || !out_mask) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    for (size_t bin_index = 0u;
         bin_index < NGGC_LARGE_FREE_BIN_COUNT; ++bin_index) {
        size_t encoded = heap->decommitted_large_free_bins[bin_index];
        size_t previous_run_bytes = 0u;
        size_t previous_start_index = 0u;
        size_t bin_run_count = 0u;
        if (encoded != 0u) {
            mask |= (uint32_t)1u << bin_index;
        }
        while (encoded != 0u) {
            size_t start_index;
            uint32_t run_length;
            size_t run_bytes;
            size_t actual_bin;
            if (runs >= max_runs ||
                !nggc_decommitted_run_decode_index(
                    heap, encoded, &start_index) ||
                !nggc_interior_decommitted_run_locked(
                    heap, start_index, &run_length, 0) ||
                run_length > SIZE_MAX / heap->region_size) {
                return NGGC_ERR_CORRUPTION;
            }
            run_bytes = (size_t)run_length * heap->region_size;
            if (!nggc_large_free_bin_index_locked(
                    heap, run_bytes, &actual_bin) ||
                actual_bin != bin_index ||
                (bin_run_count != 0u &&
                 (run_bytes < previous_run_bytes ||
                  (run_bytes == previous_run_bytes &&
                   start_index <= previous_start_index))) ||
                regions > SIZE_MAX - run_length ||
                bytes > SIZE_MAX - run_bytes) {
                return NGGC_ERR_CORRUPTION;
            }
            ++runs;
            ++bin_run_count;
            regions += run_length;
            bytes += run_bytes;
            previous_run_bytes = run_bytes;
            previous_start_index = start_index;
            encoded =
                heap->regions[start_index].decommitted_free_next;
        }
    }
    if (mask != heap->decommitted_large_free_bin_mask) {
        return NGGC_ERR_CORRUPTION;
    }
    *out_runs = runs;
    *out_regions = regions;
    *out_bytes = bytes;
    *out_mask = mask;
    return NGGC_OK;
}

nggc_result nggc_check(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    uintptr_t cursor;
    size_t seen_objects = 0;
    size_t seen_payload = 0;
    size_t seen_large_objects = 0;
    size_t seen_pinnable_objects = 0;
    size_t seen_non_movable_objects = 0;
    size_t seen_pinned_objects = 0;
    uint64_t seen_pin_leases = 0u;
    size_t seen_free_objects = 0;
    size_t seen_free_bytes = 0;
    size_t seen_large_free_objects = 0;
    size_t seen_large_free_bytes = 0;
    size_t seen_reserved_blocks = 0;
    size_t seen_reserved_bytes = 0;
    size_t seen_quarantined_objects = 0;
    size_t seen_quarantined_bytes = 0;
    size_t seen_interior_decommitted_runs = 0u;
    size_t seen_interior_decommitted_regions = 0u;
    size_t seen_interior_decommitted_bytes = 0u;
    size_t list_small_free_objects = 0;
    size_t list_small_free_bytes = 0;
    size_t list_large_free_objects = 0;
    size_t list_large_free_bytes = 0;
    size_t list_decommitted_runs = 0u;
    size_t list_decommitted_regions = 0u;
    size_t list_decommitted_bytes = 0u;
    uint32_t list_large_free_bin_mask = 0u;
    uint32_t list_decommitted_bin_mask = 0u;
    size_t seen_dirty_cards = 0;
    nggc_result result;

    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }

    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_verify_phase_state_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_assist_state_verify_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_verify_reference_discovered_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_pin_lease_verify_locked(heap, 0, 0);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    if (!nggc_trigger_policy_valid(heap->gc_trigger_policy) ||
        !nggc_trigger_action_valid(heap->gc_trigger_last_action) ||
        !nggc_trigger_reason_valid(heap->gc_trigger_last_reason) ||
        heap->gc_trigger_pending > 1u ||
        heap->gc_trigger_soft_limit == 0u ||
        heap->gc_trigger_soft_limit > heap->gc_arena_size ||
        heap->gc_trigger_quantum == 0u ||
        (heap->gc_trigger_latency_target_ns != 0u &&
         !heap->clock_ns)) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_verify_registries_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_verify_regions_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    cursor = (uintptr_t)heap->gc_arena_base;
    while (cursor < heap->arena_cursor) {
        nggc_block_view block;
        nggc_object_header *header;
        nggc_type_slot *slot;
        uintptr_t object_end;
        uint32_t state;
        if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        header = block.header;
        object_end = block.end;
        state = block.state;
        if (state == NGGC_OBJECT_LIVE) {
            nggc_object_header *forwarding_target;
            uint32_t pin_count = nggc_pin_count_load(header);
            if (pin_count != 0u) {
                if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) == 0u ||
                    seen_pinned_objects == SIZE_MAX ||
                    seen_pin_leases > UINT64_MAX - (uint64_t)pin_count) {
                    nggc_unlock(heap);
                    nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                    return NGGC_ERR_CORRUPTION;
                }
                ++seen_pinned_objects;
                seen_pin_leases += (uint64_t)pin_count;
            }
            if (nggc_forwarding_target_locked(
                    heap, header, &forwarding_target) != NGGC_OK ||
                header->object_id == 0u ||
                header->object_id > heap->next_object_id ||
                (forwarding_target && !heap->collector_cycle_active &&
                 heap->gc_phase == NGGC_PHASE_IDLE)) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            if (nggc_internal_none_object(header)) {
                if (nggc_internal_weak_reference_object(header)) {
                    nggc_internal_weak_reference *weak =
                        (nggc_internal_weak_reference *)
                            nggc_object_from_header(header);
                    if (header->payload_size < sizeof(*weak) ||
                        (weak->referent &&
                         !nggc_pointer_is_object_start(heap,
                                                       weak->referent))) {
                        nggc_unlock(heap);
                        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                        return NGGC_ERR_CORRUPTION;
                    }
                } else if (nggc_internal_soft_reference_object(header)) {
                    nggc_internal_soft_reference *soft =
                        (nggc_internal_soft_reference *)
                            nggc_object_from_header(header);
                    if (header->payload_size < sizeof(*soft) ||
                        (soft->referent &&
                         !nggc_pointer_is_object_start(heap,
                                                       soft->referent))) {
                        nggc_unlock(heap);
                        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                        return NGGC_ERR_CORRUPTION;
                    }
                } else if (nggc_internal_phantom_queue_object(header)) {
                    nggc_internal_phantom_queue *queue =
                        (nggc_internal_phantom_queue *)
                            nggc_object_from_header(header);
                    if (header->payload_size < sizeof(*queue) ||
                        nggc_validate_phantom_queue_locked(heap, queue) !=
                            NGGC_OK) {
                        nggc_unlock(heap);
                        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                        return NGGC_ERR_CORRUPTION;
                    }
                } else if (nggc_internal_phantom_reference_object(header)) {
                    nggc_internal_phantom_reference *phantom =
                        (nggc_internal_phantom_reference *)
                            nggc_object_from_header(header);
                    nggc_object_header *queue_header;
                    if (header->payload_size < sizeof(*phantom) ||
                        (phantom->referent &&
                         !nggc_pointer_is_object_start(heap,
                                                       phantom->referent))) {
                        nggc_unlock(heap);
                        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                        return NGGC_ERR_CORRUPTION;
                    }
                    queue_header =
                        nggc_live_header_from_object(heap, phantom->queue);
                    if (!nggc_internal_phantom_queue_object(queue_header)) {
                        nggc_unlock(heap);
                        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                        return NGGC_ERR_CORRUPTION;
                    }
                } else if (header->size_class != NGGC_INTERNAL_KIND_NONE) {
                    nggc_unlock(heap);
                    nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                    return NGGC_ERR_CORRUPTION;
                }
                ++seen_objects;
                if ((header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u) {
                    ++seen_large_objects;
                }
                if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) != 0u) {
                    ++seen_pinnable_objects;
                }
                if ((header->flags & NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u) {
                    ++seen_non_movable_objects;
                }
                if (seen_payload > SIZE_MAX - header->payload_size) {
                    nggc_unlock(heap);
                    nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                    return NGGC_ERR_CORRUPTION;
                }
                seen_payload += header->payload_size;
                cursor = object_end;
                continue;
            }
            if (!nggc_type_id_valid(header->type_id) ||
                !heap->types[header->type_id].registered) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            slot = &heap->types[header->type_id];
            if (slot->desc.scan || slot->desc.scan_ex) {
#if NGGC_ENABLE_VERIFY
                result = nggc_verify_scan_replay_locked(
                    heap, header, &slot->desc);
                if (result != NGGC_OK) {
                    nggc_unlock(heap);
                    nggc_set_error(heap, result);
                    return result;
                }
#else
                nggc_check_ctx check_ctx;
                check_ctx.heap = heap;
                check_ctx.owner = header;
                check_ctx.result = NGGC_OK;
                result = nggc_invoke_scan(
                    heap, header, &slot->desc,
                    nggc_check_visit, &check_ctx);
                if (check_ctx.result == NGGC_OK && result != NGGC_OK) {
                    check_ctx.result = result;
                }
                if (check_ctx.result != NGGC_OK) {
                    nggc_unlock(heap);
                    nggc_set_error(heap, check_ctx.result);
                    return check_ctx.result;
                }
#endif
            }

            ++seen_objects;
            if ((header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u) {
                ++seen_large_objects;
            }
            if ((header->flags & NGGC_OBJECT_FLAG_PINNABLE) != 0u) {
                ++seen_pinnable_objects;
            }
            if ((header->flags & NGGC_OBJECT_FLAG_NON_MOVABLE) != 0u) {
                ++seen_non_movable_objects;
            }
            if (seen_payload > SIZE_MAX - header->payload_size) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            seen_payload += header->payload_size;
        } else if (state == NGGC_OBJECT_FREE) {
            if (header->type_id != NGGC_TYPE_NONE ||
                header->size_class != 0u ||
                (header->flags & ~NGGC_OBJECT_FLAG_LARGE) != 0u ||
                header->payload_size != 0 ||
                header->object_id != 0u ||
                nggc_pin_count_load(header) != 0u ||
                nggc_forwarding_epoch_load_acquire(header) != 0u) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            if (seen_free_objects == SIZE_MAX ||
                seen_free_bytes > SIZE_MAX - header->alloc_size) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            ++seen_free_objects;
            seen_free_bytes += header->alloc_size;
            if ((header->flags & NGGC_OBJECT_FLAG_LARGE) != 0u) {
                if (seen_large_free_objects == SIZE_MAX ||
                    seen_large_free_bytes >
                        SIZE_MAX - header->alloc_size) {
                    nggc_unlock(heap);
                    nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                    return NGGC_ERR_CORRUPTION;
                }
                ++seen_large_free_objects;
                seen_large_free_bytes += header->alloc_size;
            }
        } else if (state == NGGC_OBJECT_TLAB_RESERVED) {
            ++seen_reserved_blocks;
            if (seen_reserved_bytes > SIZE_MAX - header->alloc_size) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            seen_reserved_bytes += header->alloc_size;
        } else if (state == NGGC_OBJECT_FORWARDED) {
            nggc_object_header *target;
            if (!heap->from_space_quarantine_active ||
                cursor >= heap->from_space_quarantine_source_end ||
                nggc_pin_count_load(header) != 0u ||
                nggc_forwarding_target_locked(
                    heap, header, &target) != NGGC_OK ||
                !target || target->type_id != header->type_id ||
                target->size_class != header->size_class ||
                target->flags != header->flags ||
                target->payload_size != header->payload_size ||
                target->alloc_size != header->alloc_size ||
                target->object_id != header->object_id ||
                seen_quarantined_objects == SIZE_MAX ||
                seen_quarantined_bytes >
                    SIZE_MAX - header->alloc_size) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            ++seen_quarantined_objects;
            seen_quarantined_bytes += header->alloc_size;
        } else if (state == NGGC_OBJECT_DECOMMITTED_HUMONGOUS) {
            size_t start_index =
                (size_t)((cursor -
                          (uintptr_t)heap->gc_arena_base) /
                         heap->region_size);
            uint32_t run_length;
            size_t bytes = (size_t)(object_end - cursor);
            if (header ||
                !nggc_interior_decommitted_run_locked(
                    heap, start_index, &run_length, 0) ||
                seen_interior_decommitted_runs == SIZE_MAX ||
                seen_interior_decommitted_regions >
                    SIZE_MAX - run_length ||
                seen_interior_decommitted_bytes > SIZE_MAX - bytes) {
                nggc_unlock(heap);
                nggc_set_error(heap, NGGC_ERR_CORRUPTION);
                return NGGC_ERR_CORRUPTION;
            }
            ++seen_interior_decommitted_runs;
            seen_interior_decommitted_regions += run_length;
            seen_interior_decommitted_bytes += bytes;
        } else {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        cursor = object_end;
    }
    result = nggc_verify_free_list_locked(
        heap, heap->free_list, 0u, SIZE_MAX, seen_free_objects,
        &list_small_free_objects, &list_small_free_bytes);
    for (size_t i = 0u;
         result == NGGC_OK && i < NGGC_LARGE_FREE_BIN_COUNT; ++i) {
        size_t bin_objects = 0u;
        size_t bin_bytes = 0u;
        if (heap->large_free_bins[i]) {
            list_large_free_bin_mask |= (uint32_t)1u << i;
        }
        result = nggc_verify_free_list_locked(
            heap, heap->large_free_bins[i],
            NGGC_OBJECT_FLAG_LARGE, i, seen_free_objects,
            &bin_objects, &bin_bytes);
        if (result == NGGC_OK &&
            (list_large_free_objects > SIZE_MAX - bin_objects ||
             list_large_free_bytes > SIZE_MAX - bin_bytes)) {
            result = NGGC_ERR_CORRUPTION;
        }
        if (result == NGGC_OK) {
            list_large_free_objects += bin_objects;
            list_large_free_bytes += bin_bytes;
        }
    }
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_verify_decommitted_large_bins_locked(
        heap, seen_interior_decommitted_runs,
        &list_decommitted_runs, &list_decommitted_regions,
        &list_decommitted_bytes, &list_decommitted_bin_mask);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    if (heap->allocation_count > heap->next_object_id ||
        seen_objects != heap->live_object_count ||
        seen_payload != heap->live_payload_bytes ||
        seen_large_objects != heap->large_object_count ||
        seen_pinnable_objects != heap->pinnable_object_count ||
        seen_non_movable_objects != heap->non_movable_object_count ||
        seen_pinned_objects != heap->pinned_object_count ||
        seen_pin_leases != heap->pin_lease_count ||
        seen_free_objects != heap->free_object_count ||
        seen_free_bytes != heap->free_bytes ||
        seen_large_free_objects != heap->large_free_object_count ||
        seen_large_free_bytes != heap->large_free_bytes ||
        heap->large_free_object_count > heap->free_object_count ||
        heap->large_free_bytes > heap->free_bytes ||
        list_small_free_objects !=
            heap->free_object_count - heap->large_free_object_count ||
        list_small_free_bytes !=
            heap->free_bytes - heap->large_free_bytes ||
        list_large_free_objects != heap->large_free_object_count ||
        list_large_free_bytes != heap->large_free_bytes ||
        list_large_free_bin_mask != heap->large_free_bin_mask ||
        list_decommitted_bin_mask !=
            heap->decommitted_large_free_bin_mask ||
        seen_interior_decommitted_runs !=
            heap->interior_decommitted_run_count ||
        seen_interior_decommitted_regions !=
            heap->interior_decommitted_region_count ||
        seen_interior_decommitted_bytes !=
            heap->interior_decommitted_bytes ||
        list_decommitted_runs !=
            heap->interior_decommitted_run_count ||
        list_decommitted_regions !=
            heap->interior_decommitted_region_count ||
        list_decommitted_bytes !=
            heap->interior_decommitted_bytes ||
        seen_reserved_blocks > heap->region_count ||
        seen_quarantined_objects !=
            heap->from_space_quarantine_object_count ||
        seen_quarantined_bytes != heap->from_space_quarantine_bytes) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    if (!heap->card_table || heap->card_size != NGGC_DEFAULT_CARD_SIZE ||
        heap->card_count == 0 || heap->remset_max_dirty_cards == 0u ||
        heap->remset_max_dirty_cards > heap->card_count ||
        heap->remset_card_atomic_lock_free > 1u ||
        heap->remset_mode > NGGC_REMSET_MODE_COARSE_HEAP) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    for (size_t i = 0; i < heap->card_count; ++i) {
        unsigned char card_value = atomic_load_explicit(
            &heap->card_table[i], memory_order_acquire);
        if (card_value > 1u) {
            nggc_unlock(heap);
            nggc_set_error(heap, NGGC_ERR_CORRUPTION);
            return NGGC_ERR_CORRUPTION;
        }
        if (card_value != 0u) {
            ++seen_dirty_cards;
        }
    }
    if (seen_dirty_cards != heap->dirty_card_count) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->remset_mode == NGGC_REMSET_MODE_COARSE_HEAP &&
        heap->dirty_card_count != heap->card_count) {
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_CORRUPTION);
        return NGGC_ERR_CORRUPTION;
    }
    result = nggc_verify_remset_buffers_locked(
        heap, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
#if NGGC_ENABLE_VERIFY
    result = nggc_verify_reference_processing_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
#endif
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    result = nggc_backend_check(heap->backend);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return result;
    }
    return NGGC_OK;
}

nggc_result nggc_internal_tlab_reserve(nggc_heap *heap,
                                        nggc_thread *thread,
                                        size_t requested_bytes) {
    nggc_result result;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        !nggc_thread_owned_by_current(thread)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result == NGGC_OK) {
        result = nggc_tlab_reserve_locked(heap, thread, requested_bytes);
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

nggc_result nggc_internal_tlab_retire(nggc_heap *heap,
                                       nggc_thread *thread) {
    nggc_result result;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        !nggc_thread_owned_by_current(thread)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result == NGGC_OK) {
        result = nggc_tlab_retire_locked(heap, thread);
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
    }
    return result;
}

void *nggc_internal_tlab_alloc_size(nggc_heap *heap,
                                     nggc_thread *thread,
                                     uint16_t type_id,
                                     size_t payload_size) {
    nggc_result result;
    void *object = 0;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        !nggc_thread_owned_by_current(thread)) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result == NGGC_OK) {
        result = nggc_tlab_alloc_locked(heap, thread, type_id,
                                        payload_size, &object);
    }
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }
    return object;
}

void *nggc_internal_thread_alloc_size(nggc_heap *heap,
                                       nggc_thread *thread,
                                       uint16_t type_id,
                                       size_t payload_size) {
    nggc_result result;
    void *object = 0;
    size_t total_size;
    size_t alloc_size;
    size_t reserve_size;
    nggc_type_desc desc;
    uint32_t object_flags;
    int fast_attempted = 0;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        !nggc_type_id_valid(type_id) ||
        payload_size > SIZE_MAX - NGGC_OBJECT_HEADER_BYTES) {
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    if (!nggc_thread_owned_by_current(thread)) {
        nggc_lock(heap);
        if (heap->tlab_fast_wrong_owner_count != UINT64_MAX) {
            ++heap->tlab_fast_wrong_owner_count;
        }
        nggc_unlock(heap);
        nggc_set_error(heap, NGGC_ERR_INVALID_ARGUMENT);
        return 0;
    }
    total_size = NGGC_OBJECT_HEADER_BYTES + payload_size;
    if (!nggc_align_up_size(total_size, NGGC_OBJECT_ALIGNMENT,
                            &alloc_size)) {
        nggc_set_error(heap, NGGC_ERR_OUT_OF_MEMORY);
        return 0;
    }
    if (atomic_load_explicit(&heap->type_table_published,
                             memory_order_acquire) != 0u) {
        fast_attempted = 1;
        result = nggc_tlab_alloc_fast(heap, thread, type_id,
                                      payload_size, &object);
        if (result == NGGC_OK) {
            return object;
        }
        if (result == NGGC_ERR_INVALID_ARGUMENT ||
            result == NGGC_ERR_CORRUPTION) {
            nggc_set_error(heap, result);
            return 0;
        }
    }
    nggc_auto_trigger_before_allocation(heap, payload_size);
    nggc_lock(heap);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        goto done;
    }
    if (fast_attempted && heap->tlab_fast_slow_path_count != UINT64_MAX) {
        ++heap->tlab_fast_slow_path_count;
    }
    if (!nggc_thread_is_registered(heap, thread)) {
        result = NGGC_ERR_INVALID_ARGUMENT;
        goto done;
    }
    if (thread->state != NGGC_THREAD_RUNNING) {
        result = NGGC_ERR_BUSY;
        goto done;
    }
    if (heap->safepoint_active || heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active || heap->mutator_concurrent_active) {
        result = NGGC_ERR_BUSY;
        goto done;
    }
    if (!heap->types[type_id].registered) {
        result = NGGC_ERR_TYPE_NOT_FOUND;
        goto done;
    }
    desc = heap->types[type_id].desc;
    if (desc.object_size != 0u && payload_size < desc.object_size) {
        result = NGGC_ERR_INVALID_ARGUMENT;
        goto done;
    }
    object_flags = nggc_object_flags_from_type(heap, &desc, payload_size);
    if (alloc_size > heap->region_size ||
        (object_flags & (NGGC_OBJECT_FLAG_LARGE |
                         NGGC_OBJECT_FLAG_PINNABLE |
                         NGGC_OBJECT_FLAG_NON_MOVABLE)) != 0u) {
        result = nggc_alloc_size_with_desc_locked(
            heap, type_id, &desc, 0u, payload_size, &object, thread);
        if (result == NGGC_OK &&
            heap->tlab_fallback_allocation_count != UINT64_MAX) {
            ++heap->tlab_fallback_allocation_count;
        }
        goto done;
    }
    if (thread->tlab_state == NGGC_TLAB_ACTIVE) {
        result = nggc_tlab_alloc_locked(heap, thread, type_id,
                                        payload_size, &object);
        if (result == NGGC_OK || result != NGGC_ERR_OUT_OF_MEMORY) {
            goto done;
        }
        result = nggc_tlab_retire_locked(heap, thread);
        if (result != NGGC_OK) {
            goto done;
        }
    } else if (thread->tlab_state != NGGC_TLAB_INACTIVE) {
        result = NGGC_ERR_BUSY;
        goto done;
    }
    reserve_size = alloc_size > thread->tlab_target_size
                       ? alloc_size
                       : thread->tlab_target_size;
    result = nggc_tlab_reserve_locked(heap, thread, reserve_size);
    if (result == NGGC_OK) {
        result = nggc_tlab_alloc_locked(heap, thread, type_id,
                                        payload_size, &object);
    } else if ((result == NGGC_ERR_BUSY ||
                result == NGGC_ERR_OUT_OF_MEMORY) &&
               heap->gc_phase == NGGC_PHASE_IDLE &&
               !heap->collector_cycle_active &&
               heap->types[type_id].registered) {
        result = nggc_alloc_size_with_desc_locked(
            heap, type_id, &desc, 0u, payload_size, &object, thread);
        if (result == NGGC_OK &&
            heap->tlab_fallback_allocation_count != UINT64_MAX) {
            ++heap->tlab_fallback_allocation_count;
        }
    }
done:
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);
    if (result != NGGC_OK) {
        nggc_set_error(heap, result);
        return 0;
    }
    return object;
}

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
nggc_result nggc_internal_test_scanner_phase(uint32_t *out_phase) {
    if (!nggc_scan_guard_active || !out_phase) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_phase = nggc_scan_guard_active->heap->gc_phase;
    return NGGC_OK;
}

nggc_result nggc_internal_test_registry_fault_check(nggc_heap *heap,
                                                     uint32_t fault) {
    nggc_thread *thread = 0;
    nggc_thread *saved_next = 0;
    nggc_heap *saved_heap = 0;
    void **saved_root_slot = 0;
    size_t saved_size = 0;
    uint64_t saved_u64 = 0;
    uint32_t saved_u32 = 0;
    nggc_result result;

    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    switch (fault) {
        case NGGC_TEST_REGISTRY_FAULT_ROOT_COUNT:
            if (heap->root_capacity == SIZE_MAX) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_size = heap->root_count;
            heap->root_count = heap->root_capacity + 1u;
            break;
        case NGGC_TEST_REGISTRY_FAULT_ROOT_DUPLICATE:
            if (heap->root_count < 2u) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_root_slot = heap->roots[1];
            heap->roots[1] = heap->roots[0];
            break;
        case NGGC_TEST_REGISTRY_FAULT_ROOT_UNALIGNED:
            if (heap->root_count == 0u) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_root_slot = heap->roots[0];
            heap->roots[0] =
                (void **)((unsigned char *)saved_root_slot + 1u);
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_COUNT:
            if (heap->registered_thread_count == SIZE_MAX) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_size = heap->registered_thread_count;
            ++heap->registered_thread_count;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_CYCLE:
            thread = heap->threads;
            if (!thread) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_next = thread->next;
            thread->next = thread;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_MAGIC:
            thread = heap->threads;
            if (!thread) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_u32 = thread->magic;
            thread->magic = NGGC_THREAD_CLOSED_MAGIC;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_OWNER:
            thread = heap->threads;
            if (!thread) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_heap = thread->heap;
            thread->heap = 0;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_STATE:
            thread = heap->threads;
            if (!thread) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_u32 = thread->state;
            thread->state = UINT32_MAX;
            break;
        case NGGC_TEST_REGISTRY_FAULT_PENDING_COUNT:
            if (heap->registered_thread_count == 0u) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_size = heap->safepoint_pending_threads;
            heap->safepoint_pending_threads =
                saved_size == 0u ? 1u : 0u;
            break;
        case NGGC_TEST_REGISTRY_FAULT_OBSERVED_EPOCH:
            thread = heap->threads;
            if (!thread) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_u64 = thread->observed_safepoint_epoch;
            thread->observed_safepoint_epoch =
                heap->safepoint_epoch ^ UINT64_C(1);
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_COUNT:
            thread = heap->threads;
            if (!thread || thread->root_capacity == SIZE_MAX) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_size = thread->root_count;
            thread->root_count = thread->root_capacity + 1u;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_DUPLICATE:
            thread = heap->threads;
            if (!thread || thread->root_count < 2u) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_root_slot = thread->roots[1];
            thread->roots[1] = thread->roots[0];
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_UNALIGNED:
            thread = heap->threads;
            if (!thread || thread->root_count == 0u) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_root_slot = thread->roots[0];
            thread->roots[0] =
                (void **)((unsigned char *)saved_root_slot + 1u);
            break;
        default:
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_unlock(heap);

    result = nggc_check(heap);

    nggc_lock(heap);
    switch (fault) {
        case NGGC_TEST_REGISTRY_FAULT_ROOT_COUNT:
            heap->root_count = saved_size;
            break;
        case NGGC_TEST_REGISTRY_FAULT_ROOT_DUPLICATE:
            heap->roots[1] = saved_root_slot;
            break;
        case NGGC_TEST_REGISTRY_FAULT_ROOT_UNALIGNED:
            heap->roots[0] = saved_root_slot;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_COUNT:
            heap->registered_thread_count = saved_size;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_CYCLE:
            thread->next = saved_next;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_MAGIC:
            thread->magic = saved_u32;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_OWNER:
            thread->heap = saved_heap;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_STATE:
            thread->state = saved_u32;
            break;
        case NGGC_TEST_REGISTRY_FAULT_PENDING_COUNT:
            heap->safepoint_pending_threads = saved_size;
            break;
        case NGGC_TEST_REGISTRY_FAULT_OBSERVED_EPOCH:
            thread->observed_safepoint_epoch = saved_u64;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_COUNT:
            thread->root_count = saved_size;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_DUPLICATE:
            thread->roots[1] = saved_root_slot;
            break;
        case NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_UNALIGNED:
            thread->roots[0] = saved_root_slot;
            break;
        default:
            break;
    }
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_reference_fault_check(
    nggc_heap *heap, uint32_t fault, void *target, void *auxiliary) {
    nggc_object_header *header;
    nggc_object_header *mutated_header = 0;
    nggc_internal_soft_reference *soft = 0;
    nggc_internal_weak_reference *weak = 0;
    nggc_internal_phantom_reference *phantom = 0;
    nggc_internal_phantom_queue *queue = 0;
    void *saved_pointer = 0;
    void *saved_pointer2 = 0;
    size_t saved_size = 0;
    uint32_t saved_u32 = 0;
    nggc_result result;

    if (!nggc_heap_live(heap) || !target) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    header = nggc_live_header_from_object(heap, target);
    switch (fault) {
        case NGGC_TEST_REFERENCE_FAULT_SOFT_REFERENT_MARK:
            if (!nggc_internal_soft_reference_object(header) ||
                header->payload_size < sizeof(*soft)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            soft = (nggc_internal_soft_reference *)target;
            mutated_header =
                nggc_live_header_from_object(heap, soft->referent);
            if (!mutated_header) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_u32 = mutated_header->mark_epoch;
            mutated_header->mark_epoch = heap->mark_epoch ^ 1u;
            break;
        case NGGC_TEST_REFERENCE_FAULT_WEAK_REFERENT_MARK:
            if (!nggc_internal_weak_reference_object(header) ||
                header->payload_size < sizeof(*weak)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            weak = (nggc_internal_weak_reference *)target;
            mutated_header =
                nggc_live_header_from_object(heap, weak->referent);
            if (!mutated_header) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_u32 = mutated_header->mark_epoch;
            mutated_header->mark_epoch = heap->mark_epoch ^ 1u;
            break;
        case NGGC_TEST_REFERENCE_FAULT_PHANTOM_REFERENT_MARK:
            if (!nggc_internal_phantom_reference_object(header) ||
                header->payload_size < sizeof(*phantom)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            phantom = (nggc_internal_phantom_reference *)target;
            mutated_header =
                nggc_live_header_from_object(heap, phantom->referent);
            if (!mutated_header || phantom->enqueued) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_u32 = mutated_header->mark_epoch;
            mutated_header->mark_epoch = heap->mark_epoch ^ 1u;
            break;
        case NGGC_TEST_REFERENCE_FAULT_PHANTOM_QUEUE_MARK:
            if (!nggc_internal_phantom_reference_object(header) ||
                header->payload_size < sizeof(*phantom)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            phantom = (nggc_internal_phantom_reference *)target;
            mutated_header =
                nggc_live_header_from_object(heap, phantom->queue);
            if (!nggc_internal_phantom_queue_object(mutated_header)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_u32 = mutated_header->mark_epoch;
            mutated_header->mark_epoch = heap->mark_epoch ^ 1u;
            break;
        case NGGC_TEST_REFERENCE_FAULT_ENQUEUED_REFERENT:
            if (!nggc_internal_phantom_reference_object(header) ||
                header->payload_size < sizeof(*phantom) ||
                !auxiliary || !nggc_pointer_is_object_start(heap, auxiliary)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            phantom = (nggc_internal_phantom_reference *)target;
            if (!phantom->enqueued) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_pointer = phantom->referent;
            phantom->referent = auxiliary;
            break;
        case NGGC_TEST_REFERENCE_FAULT_ENQUEUED_MARK:
            if (!nggc_internal_phantom_reference_object(header)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            mutated_header = header;
            saved_u32 = mutated_header->mark_epoch;
            mutated_header->mark_epoch = heap->mark_epoch ^ 1u;
            break;
        case NGGC_TEST_REFERENCE_FAULT_ORPHAN_ENQUEUED:
            if (!nggc_internal_phantom_reference_object(header) ||
                header->payload_size < sizeof(*phantom)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            phantom = (nggc_internal_phantom_reference *)target;
            if (phantom->enqueued) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_pointer = phantom->referent;
            saved_pointer2 = phantom->next;
            saved_u32 = phantom->enqueued;
            phantom->referent = 0;
            phantom->next = 0;
            phantom->enqueued = 1u;
            break;
        case NGGC_TEST_REFERENCE_FAULT_QUEUE_OWNER:
            if (!nggc_internal_phantom_reference_object(header) ||
                header->payload_size < sizeof(*phantom) || !auxiliary ||
                !nggc_internal_phantom_queue_object(
                    nggc_live_header_from_object(heap, auxiliary))) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            phantom = (nggc_internal_phantom_reference *)target;
            if (!phantom->enqueued) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_pointer = phantom->queue;
            phantom->queue = auxiliary;
            break;
        case NGGC_TEST_REFERENCE_FAULT_QUEUE_COUNT:
            if (!nggc_internal_phantom_queue_object(header) ||
                header->payload_size < sizeof(*queue)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            queue = (nggc_internal_phantom_queue *)target;
            if (queue->pending_count == 0u ||
                queue->pending_count == SIZE_MAX) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            saved_size = queue->pending_count;
            ++queue->pending_count;
            break;
        case NGGC_TEST_REFERENCE_FAULT_ENQUEUED_FLAG:
            if (!nggc_internal_phantom_reference_object(header) ||
                header->payload_size < sizeof(*phantom)) {
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            phantom = (nggc_internal_phantom_reference *)target;
            saved_u32 = phantom->enqueued;
            phantom->enqueued = 2u;
            break;
        default:
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
    }

    result = nggc_verify_reference_processing_locked(heap);

    switch (fault) {
        case NGGC_TEST_REFERENCE_FAULT_SOFT_REFERENT_MARK:
        case NGGC_TEST_REFERENCE_FAULT_WEAK_REFERENT_MARK:
        case NGGC_TEST_REFERENCE_FAULT_PHANTOM_REFERENT_MARK:
        case NGGC_TEST_REFERENCE_FAULT_PHANTOM_QUEUE_MARK:
        case NGGC_TEST_REFERENCE_FAULT_ENQUEUED_MARK:
            mutated_header->mark_epoch = saved_u32;
            break;
        case NGGC_TEST_REFERENCE_FAULT_ENQUEUED_REFERENT:
            phantom->referent = saved_pointer;
            break;
        case NGGC_TEST_REFERENCE_FAULT_ORPHAN_ENQUEUED:
            phantom->referent = saved_pointer;
            phantom->next = saved_pointer2;
            phantom->enqueued = saved_u32;
            break;
        case NGGC_TEST_REFERENCE_FAULT_QUEUE_OWNER:
            phantom->queue = saved_pointer;
            break;
        case NGGC_TEST_REFERENCE_FAULT_QUEUE_COUNT:
            queue->pending_count = saved_size;
            break;
        case NGGC_TEST_REFERENCE_FAULT_ENQUEUED_FLAG:
            phantom->enqueued = saved_u32;
            break;
        default:
            break;
    }
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_collect_fault_set(nggc_heap *heap,
                                                  uint32_t fault) {
    if (!nggc_heap_live(heap) ||
        (fault != NGGC_TEST_COLLECT_FAULT_AFTER_CYCLE_BEGIN &&
         fault != NGGC_TEST_COLLECT_FAULT_ROOT_OOM &&
         fault != NGGC_TEST_COLLECT_FAULT_REFERENCE_PROCESS &&
         fault != NGGC_TEST_COLLECT_FAULT_SWEEP &&
         fault != NGGC_TEST_COLLECT_FAULT_BEFORE_FORWARDING &&
         fault != NGGC_TEST_COLLECT_FAULT_AFTER_FORWARDING &&
         fault != NGGC_TEST_COLLECT_FAULT_AFTER_EVACUATE &&
         fault != NGGC_TEST_COLLECT_FAULT_DURING_REMAP &&
         fault != NGGC_TEST_COLLECT_FAULT_DURING_SOURCE_RELEASE &&
         fault !=
             NGGC_TEST_COLLECT_ACTION_DUPLICATE_EVACUATION_TASK &&
         fault != NGGC_TEST_COLLECT_ACTION_WORKER_TASK_RETRY &&
         fault !=
              NGGC_TEST_COLLECT_ACTION_WORKER_CREATE_FALLBACK &&
         fault != NGGC_TEST_COLLECT_ACTION_CONCURRENT_WINDOW &&
         fault != NGGC_TEST_COLLECT_ACTION_CONCURRENT_COPY &&
         fault != NGGC_TEST_COLLECT_ACTION_FORCE_EMERGENCY_FAILURE)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if !NGGC_HAVE_C11_THREADS
    if (fault == NGGC_TEST_COLLECT_ACTION_CONCURRENT_WINDOW ||
        fault == NGGC_TEST_COLLECT_ACTION_CONCURRENT_COPY) {
        return NGGC_ERR_UNSUPPORTED;
    }
#endif
    nggc_lock(heap);
    if (heap->test_collect_fault != 0u ||
        heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active) {
        nggc_unlock(heap);
        return NGGC_ERR_BUSY;
    }
    heap->test_collect_fault = fault;
#if NGGC_HAVE_C11_THREADS
    if (fault == NGGC_TEST_COLLECT_ACTION_CONCURRENT_WINDOW ||
        fault == NGGC_TEST_COLLECT_ACTION_CONCURRENT_COPY) {
        atomic_store_explicit(&heap->test_concurrent_window_entered, 0u,
                              memory_order_release);
        atomic_store_explicit(&heap->test_concurrent_window_release, 0u,
                              memory_order_release);
        atomic_store_explicit(&heap->test_concurrent_copy_entered, 0u,
                              memory_order_release);
        atomic_store_explicit(&heap->test_concurrent_copy_release, 0u,
                              memory_order_release);
        atomic_store_explicit(&heap->test_concurrent_copy_pause, 0u,
                              memory_order_release);
        atomic_store_explicit(&heap->test_concurrent_copy_source, 0,
                              memory_order_release);
        atomic_store_explicit(&heap->test_concurrent_copy_store_entered, 0u,
                              memory_order_release);
        atomic_store_explicit(&heap->test_concurrent_copy_load_entered, 0u,
                              memory_order_release);
    }
#endif
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_internal_test_gc_pool_fault_set(nggc_heap *heap,
                                                  uint32_t fault) {
    if (!nggc_heap_live(heap) ||
        (fault != NGGC_TEST_GC_POOL_FAULT_WAIT &&
         fault != NGGC_TEST_GC_POOL_FAULT_BROADCAST &&
         fault != NGGC_TEST_GC_POOL_FAULT_JOIN &&
         fault != NGGC_TEST_GC_POOL_FAULT_WORKER_WAIT &&
         fault != NGGC_TEST_GC_POOL_FAULT_WORKER_BEFORE_JOB &&
         fault != NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_JOB &&
         fault != NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_COMPLETE &&
         fault != NGGC_TEST_GC_POOL_FAULT_REMAP_BEFORE_JOB &&
         fault != NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_JOB &&
         fault != NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_COMPLETE)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if !NGGC_HAVE_C11_THREADS
    (void)fault;
    return NGGC_ERR_UNSUPPORTED;
#else
    nggc_lock(heap);
    if (heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active || !heap->gc_pool_initialized) {
        nggc_unlock(heap);
        return NGGC_ERR_BUSY;
    }
    if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
        nggc_unlock(heap);
        return NGGC_ERR_BACKEND;
    }
    nggc_gc_pool_refresh_failures_locked(heap);
    if (heap->gc_pool_shutdown || heap->gc_pool_broken ||
        atomic_load_explicit(&heap->test_gc_pool_fail_next_wait,
                             memory_order_acquire) != 0u ||
        atomic_load_explicit(&heap->test_gc_pool_fail_next_broadcast,
                             memory_order_acquire) != 0u ||
        atomic_load_explicit(&heap->test_gc_pool_fail_next_join,
                             memory_order_acquire) != 0u ||
        atomic_load_explicit(&heap->test_gc_pool_worker_fault,
                             memory_order_acquire) != 0u) {
        (void)mtx_unlock(&heap->gc_pool_lock);
        nggc_unlock(heap);
        return NGGC_ERR_BUSY;
    }
    switch (fault) {
        case NGGC_TEST_GC_POOL_FAULT_WAIT:
            atomic_store_explicit(&heap->test_gc_pool_fail_next_wait, 1u,
                                  memory_order_release);
            break;
        case NGGC_TEST_GC_POOL_FAULT_BROADCAST:
            atomic_store_explicit(
                &heap->test_gc_pool_fail_next_broadcast, 1u,
                memory_order_release);
            break;
        case NGGC_TEST_GC_POOL_FAULT_JOIN:
            atomic_store_explicit(&heap->test_gc_pool_fail_next_join, 1u,
                                  memory_order_release);
            break;
        case NGGC_TEST_GC_POOL_FAULT_WORKER_WAIT:
        case NGGC_TEST_GC_POOL_FAULT_WORKER_BEFORE_JOB:
        case NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_JOB:
        case NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_COMPLETE:
        case NGGC_TEST_GC_POOL_FAULT_REMAP_BEFORE_JOB:
        case NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_JOB:
        case NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_COMPLETE:
            if (heap->evacuation_worker_pool_size == 0u) {
                (void)mtx_unlock(&heap->gc_pool_lock);
                nggc_unlock(heap);
                return NGGC_ERR_BUSY;
            }
            atomic_store_explicit(&heap->test_gc_pool_worker_fault, fault,
                                  memory_order_release);
            if (fault == NGGC_TEST_GC_POOL_FAULT_WORKER_WAIT &&
                cnd_broadcast(&heap->gc_pool_work_condition) != thrd_success) {
                atomic_store_explicit(&heap->test_gc_pool_worker_fault, 0u,
                                      memory_order_release);
                (void)mtx_unlock(&heap->gc_pool_lock);
                nggc_unlock(heap);
                return NGGC_ERR_BACKEND;
            }
            break;
        default:
            (void)mtx_unlock(&heap->gc_pool_lock);
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (mtx_unlock(&heap->gc_pool_lock) != thrd_success) {
        nggc_unlock(heap);
        return NGGC_ERR_BACKEND;
    }
    nggc_unlock(heap);
    return NGGC_OK;
#endif
}

nggc_result nggc_internal_test_gc_pool_worker_failure_wait(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if !NGGC_HAVE_C11_THREADS
    return NGGC_ERR_UNSUPPORTED;
#else
    nggc_result result = NGGC_ERR_BUSY;
    nggc_lock(heap);
    if (mtx_lock(&heap->gc_pool_lock) != thrd_success) {
        nggc_unlock(heap);
        return NGGC_ERR_BACKEND;
    }
    nggc_gc_pool_refresh_failures_locked(heap);
    for (size_t i = 0u; i < heap->evacuation_worker_pool_size; ++i) {
        if (atomic_load_explicit(&heap->gc_pool_workers[i].failed,
                                 memory_order_acquire) != 0u) {
            result = NGGC_OK;
            break;
        }
    }
    if (mtx_unlock(&heap->gc_pool_lock) != thrd_success) {
        result = NGGC_ERR_BACKEND;
    }
    nggc_unlock(heap);
    return result;
#endif
}

nggc_result nggc_internal_test_force_emergency_failure(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (heap->test_force_emergency_failure != 0u ||
        heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active) {
        nggc_unlock(heap);
        return NGGC_ERR_BUSY;
    }
    heap->test_force_emergency_failure = 1u;
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_internal_test_concurrent_window_wait(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(&heap->test_concurrent_window_entered,
                                memory_order_acquire) != 0u
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

nggc_result nggc_internal_test_concurrent_window_release(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&heap->test_concurrent_window_release, 1u,
                          memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_concurrent_copy_wait(
    nggc_heap *heap, uint32_t stage) {
    if (!nggc_heap_live(heap) || stage == 0u || stage > 3u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(&heap->test_concurrent_copy_entered,
                                memory_order_acquire) == stage
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

nggc_result nggc_internal_test_concurrent_copy_release(
    nggc_heap *heap, uint32_t stage) {
    if (!nggc_heap_live(heap) || stage == 0u || stage > 3u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&heap->test_concurrent_copy_release, stage,
                          memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_concurrent_copy_source(
    nggc_heap *heap, void **out_object) {
    void *source;
    if (!nggc_heap_live(heap) || !out_object) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    source = atomic_load_explicit(&heap->test_concurrent_copy_source,
                                  memory_order_acquire);
    if (!source ||
        atomic_load_explicit(&heap->test_concurrent_copy_entered,
                             memory_order_acquire) == 0u) {
        return NGGC_ERR_BUSY;
    }
    *out_object = source;
    return NGGC_OK;
}

nggc_result nggc_internal_test_concurrent_copy_store_wait(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(&heap->test_concurrent_copy_store_entered,
                                memory_order_acquire) != 0u
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

nggc_result nggc_internal_test_concurrent_copy_load_wait(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(&heap->test_concurrent_copy_load_entered,
                                memory_order_acquire) != 0u
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

size_t nggc_internal_test_safepoint_blocked_threads(nggc_heap *heap) {
    return nggc_heap_live(heap)
               ? atomic_load_explicit(&heap->test_safepoint_blocked_threads,
                                      memory_order_acquire)
               : 0u;
}

nggc_result nggc_internal_test_reference_create_pause_arm(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(&heap->test_reference_create_pause_armed,
                             memory_order_acquire) != 0u ||
        (atomic_load_explicit(&heap->test_reference_create_pause_entered,
                              memory_order_acquire) != 0u &&
         atomic_load_explicit(&heap->test_reference_create_pause_release,
                              memory_order_acquire) == 0u)) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(&heap->test_reference_create_pause_entered, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_reference_create_pause_release, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_reference_create_pause_armed, 1u,
                          memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_reference_create_pause_wait(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(&heap->test_reference_create_pause_entered,
                                memory_order_acquire) != 0u
               ? NGGC_OK : NGGC_ERR_BUSY;
}

nggc_result nggc_internal_test_reference_create_pause_release(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&heap->test_reference_create_pause_release, 1u,
                          memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_phase_fault_check(nggc_heap *heap,
                                                  uint32_t fault) {
    uint32_t saved_phase;
    uint32_t saved_read_barrier;
    uint32_t saved_cycle_active;
    uint32_t saved_expected_phase;
    uint32_t saved_cycle_kind;
    uint64_t saved_transition_count;
    uint64_t saved_cycle_epoch;
    uint32_t saved_evacuation_epoch;
    uint32_t saved_evacuation_worker_epoch;
    uint32_t saved_evacuation_workers_active;
    uint32_t saved_evacuation_workers_quiescent;
    uint32_t saved_remap_worker_epoch;
    uint32_t saved_remap_workers_active;
    uint32_t saved_remap_workers_quiescent;
    uint32_t saved_evacuation_copy_started;
    uint32_t saved_from_space_reclaim_pending;
    uint32_t saved_quarantine_epoch;
    uint32_t saved_quarantine_active;
    uint64_t saved_quarantine_release_epoch;
    uintptr_t saved_quarantine_source_end;
    size_t saved_quarantine_object_count;
    size_t saved_quarantine_bytes;
    uint64_t saved_collect_epoch;
    uint64_t saved_safepoint_epoch;
    size_t saved_pending_threads;
    size_t saved_parked_threads;
    uint32_t saved_safepoint_active;
    uint32_t saved_mutator_concurrent_active;
    uint32_t saved_mutator_concurrent_epoch;
    size_t saved_mutator_concurrent_running_threads;
    size_t saved_mutator_concurrent_repark_pending_threads;
    nggc_thread *saved_collect_owner;
    nggc_result result;

    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (fault == NGGC_TEST_PHASE_CHECK) {
        result = nggc_verify_phase_state_locked(heap);
        nggc_unlock(heap);
        return result;
    }

    saved_phase = heap->gc_phase;
    saved_read_barrier = heap->read_barrier_active;
    saved_cycle_active = heap->collector_cycle_active;
    saved_expected_phase = heap->collector_expected_phase;
    saved_cycle_kind = heap->collector_cycle_kind;
    saved_transition_count = heap->phase_transition_count;
    saved_cycle_epoch = heap->collector_cycle_epoch;
    saved_evacuation_epoch = heap->evacuation_epoch;
    saved_evacuation_worker_epoch = heap->evacuation_worker_epoch;
    saved_evacuation_workers_active = atomic_load_explicit(
        &heap->evacuation_workers_active, memory_order_acquire);
    saved_evacuation_workers_quiescent =
        heap->evacuation_workers_quiescent;
    saved_remap_worker_epoch = heap->remap_worker_epoch;
    saved_remap_workers_active = atomic_load_explicit(
        &heap->remap_workers_active, memory_order_acquire);
    saved_remap_workers_quiescent = heap->remap_workers_quiescent;
    saved_evacuation_copy_started = heap->evacuation_copy_started;
    saved_from_space_reclaim_pending =
        heap->from_space_reclaim_pending;
    saved_quarantine_epoch = heap->from_space_quarantine_epoch;
    saved_quarantine_active = heap->from_space_quarantine_active;
    saved_quarantine_release_epoch =
        heap->from_space_quarantine_release_epoch;
    saved_quarantine_source_end =
        heap->from_space_quarantine_source_end;
    saved_quarantine_object_count =
        heap->from_space_quarantine_object_count;
    saved_quarantine_bytes = heap->from_space_quarantine_bytes;
    saved_collect_epoch = heap->collect_safepoint_epoch;
    saved_safepoint_epoch = heap->safepoint_epoch;
    saved_pending_threads = heap->safepoint_pending_threads;
    saved_parked_threads = heap->safepoint_parked_threads;
    saved_safepoint_active = heap->safepoint_active;
    saved_mutator_concurrent_active = heap->mutator_concurrent_active;
    saved_mutator_concurrent_epoch = heap->mutator_concurrent_epoch;
    saved_mutator_concurrent_running_threads =
        heap->mutator_concurrent_running_threads;
    saved_mutator_concurrent_repark_pending_threads =
        heap->mutator_concurrent_repark_pending_threads;
    saved_collect_owner = heap->collect_safepoint_owner;

    switch (fault) {
        case NGGC_TEST_PHASE_FAULT_INVALID_PHASE:
            heap->gc_phase = UINT32_MAX;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_READ_BARRIER:
            heap->read_barrier_active = 1u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_TRANSITION_PARITY:
            heap->phase_transition_count ^= UINT64_C(1);
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_CYCLE_FLAG:
            heap->collector_cycle_active = 2u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_INACTIVE_EXPECTED:
            heap->collector_expected_phase = NGGC_PHASE_MARK;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_CYCLE_MISMATCH:
            heap->collector_cycle_active = 1u;
            heap->collector_expected_phase = NGGC_PHASE_MARK;
            heap->collector_cycle_epoch =
                saved_cycle_epoch != 0u ? saved_cycle_epoch : 1u;
            heap->gc_phase = NGGC_PHASE_ROOT_SNAPSHOT;
            heap->read_barrier_active = 0u;
            heap->phase_transition_count |= UINT64_C(1);
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_ACTIVE_PENDING:
            heap->gc_phase = NGGC_PHASE_MARK;
            heap->read_barrier_active = 0u;
            heap->phase_transition_count |= UINT64_C(1);
            heap->safepoint_pending_threads = 1u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_ACTIVE_BARRIER:
            heap->gc_phase = NGGC_PHASE_REMAP;
            heap->read_barrier_active = 0u;
            heap->phase_transition_count |= UINT64_C(1);
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_CYCLE_EPOCH:
            heap->collector_cycle_active = 1u;
            heap->collector_expected_phase = NGGC_PHASE_ROOT_SNAPSHOT;
            heap->collector_cycle_epoch = 0u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_COLLECT_EPOCH:
            heap->collect_safepoint_epoch =
                heap->safepoint_epoch == UINT64_MAX
                    ? UINT64_MAX - UINT64_C(1)
                    : heap->safepoint_epoch + UINT64_C(1);
            if (heap->collect_safepoint_epoch == 0u) {
                heap->collect_safepoint_epoch = 1u;
            }
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_SAFEPOINT_ACTIVE:
            heap->safepoint_active = 2u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_PARKED_COUNT:
            heap->safepoint_parked_threads =
                heap->registered_thread_count + 1u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_COLLECT_OWNER:
            heap->collect_safepoint_owner =
                (nggc_thread *)(uintptr_t)1u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_EVACUATION_EPOCH:
            heap->collector_cycle_active = 1u;
            heap->collector_expected_phase = NGGC_PHASE_ROOT_SNAPSHOT;
            heap->collector_cycle_kind =
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION;
            heap->collector_cycle_epoch =
                saved_cycle_epoch != 0u ? saved_cycle_epoch : 1u;
            heap->evacuation_epoch = 0u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_EVACUATION_WORKER_ACTIVE:
            heap->collector_cycle_active = 1u;
            heap->collector_expected_phase = NGGC_PHASE_EVACUATE;
            heap->collector_cycle_kind =
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION;
            heap->collector_cycle_epoch =
                saved_cycle_epoch != 0u ? saved_cycle_epoch : 1u;
            heap->evacuation_epoch =
                saved_evacuation_epoch != 0u
                    ? saved_evacuation_epoch
                    : 1u;
            heap->evacuation_worker_epoch = heap->evacuation_epoch;
            atomic_store_explicit(&heap->evacuation_workers_active, 1u,
                                  memory_order_release);
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_FROM_SPACE_RECLAIM_GATE:
            heap->collector_cycle_active = 1u;
            heap->collector_expected_phase = NGGC_PHASE_REMAP;
            heap->collector_cycle_kind =
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION;
            heap->collector_cycle_epoch =
                saved_cycle_epoch != 0u ? saved_cycle_epoch : 1u;
            heap->evacuation_epoch =
                saved_evacuation_epoch != 0u
                    ? saved_evacuation_epoch
                    : 1u;
            heap->evacuation_worker_epoch = heap->evacuation_epoch;
            heap->evacuation_copy_started = 1u;
            heap->evacuation_workers_quiescent = 1u;
            heap->from_space_reclaim_pending = 0u;
            heap->gc_phase = NGGC_PHASE_REMAP;
            heap->read_barrier_active = 1u;
            heap->phase_transition_count |= UINT64_C(1);
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_REMAP_WORKER_ACTIVE:
            heap->collector_cycle_active = 1u;
            heap->collector_expected_phase = NGGC_PHASE_REMAP;
            heap->collector_cycle_kind =
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION;
            heap->collector_cycle_epoch =
                saved_cycle_epoch != 0u ? saved_cycle_epoch : 1u;
            heap->evacuation_epoch =
                saved_evacuation_epoch != 0u
                    ? saved_evacuation_epoch
                    : 1u;
            heap->evacuation_worker_epoch = heap->evacuation_epoch;
            heap->evacuation_copy_started = 1u;
            heap->evacuation_workers_quiescent = 1u;
            heap->from_space_reclaim_pending = 1u;
            heap->remap_worker_epoch = heap->evacuation_epoch;
            atomic_store_explicit(&heap->remap_workers_active, 1u,
                                  memory_order_release);
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_REMAP_RECLAIM_GATE:
            heap->collector_cycle_active = 1u;
            heap->collector_expected_phase = NGGC_PHASE_SWEEP;
            heap->collector_cycle_kind =
                NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION;
            heap->collector_cycle_epoch =
                saved_cycle_epoch != 0u ? saved_cycle_epoch : 1u;
            heap->evacuation_epoch =
                saved_evacuation_epoch != 0u
                    ? saved_evacuation_epoch
                    : 1u;
            heap->evacuation_worker_epoch = heap->evacuation_epoch;
            heap->evacuation_copy_started = 1u;
            heap->evacuation_workers_quiescent = 1u;
            heap->from_space_reclaim_pending = 1u;
            heap->remap_worker_epoch = heap->evacuation_epoch;
            heap->remap_workers_quiescent = 0u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_QUARANTINE_BARRIER:
            heap->evacuation_epoch =
                saved_evacuation_epoch != 0u
                    ? saved_evacuation_epoch
                    : 1u;
            heap->from_space_quarantine_epoch = heap->evacuation_epoch;
            heap->from_space_quarantine_active = 1u;
            heap->from_space_quarantine_release_epoch =
                heap->safepoint_epoch;
            heap->from_space_quarantine_source_end = heap->arena_cursor;
            heap->from_space_quarantine_object_count = 1u;
            heap->from_space_quarantine_bytes = NGGC_OBJECT_HEADER_BYTES;
            heap->read_barrier_active = 0u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_QUARANTINE_EPOCH:
            heap->evacuation_epoch =
                saved_evacuation_epoch != 0u
                    ? saved_evacuation_epoch
                    : 1u;
            heap->from_space_quarantine_epoch =
                heap->evacuation_epoch == UINT32_MAX
                    ? UINT32_MAX - 1u
                    : heap->evacuation_epoch + 1u;
            heap->from_space_quarantine_active = 1u;
            heap->from_space_quarantine_release_epoch =
                heap->safepoint_epoch;
            heap->from_space_quarantine_source_end = heap->arena_cursor;
            heap->from_space_quarantine_object_count = 1u;
            heap->from_space_quarantine_bytes = NGGC_OBJECT_HEADER_BYTES;
            heap->read_barrier_active = 1u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_QUARANTINE_RECLAIM_PROOF:
            heap->evacuation_epoch =
                saved_evacuation_epoch != 0u
                    ? saved_evacuation_epoch
                    : 1u;
            heap->from_space_quarantine_epoch = heap->evacuation_epoch;
            heap->from_space_quarantine_active = 1u;
            if (heap->safepoint_epoch == UINT64_MAX) {
                heap->safepoint_epoch = UINT64_MAX - UINT64_C(1);
            }
            heap->from_space_quarantine_release_epoch =
                heap->safepoint_epoch + UINT64_C(1);
            heap->from_space_quarantine_source_end = heap->arena_cursor;
            heap->from_space_quarantine_object_count = 1u;
            heap->from_space_quarantine_bytes = NGGC_OBJECT_HEADER_BYTES;
            heap->read_barrier_active = 1u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_FAULT_MUTATOR_CONCURRENT:
            heap->mutator_concurrent_active = 1u;
            heap->mutator_concurrent_epoch = 1u;
            result = nggc_verify_phase_state_locked(heap);
            break;
        case NGGC_TEST_PHASE_ACTION_OUT_OF_ORDER:
            result = nggc_collector_cycle_begin_locked(
                heap, NGGC_COLLECTOR_CYCLE_NON_MOVING);
            if (result == NGGC_OK) {
                result = nggc_phase_begin_locked(heap, NGGC_PHASE_MARK);
            }
            break;
        case NGGC_TEST_PHASE_ACTION_ABORT_CYCLE:
            result = nggc_collector_cycle_begin_locked(
                heap, NGGC_COLLECTOR_CYCLE_NON_MOVING);
            if (result == NGGC_OK) {
                result = nggc_phase_begin_locked(
                    heap, NGGC_PHASE_ROOT_SNAPSHOT);
            }
            if (result == NGGC_OK) {
                result = nggc_phase_end_locked(
                    heap, NGGC_PHASE_ROOT_SNAPSHOT);
            }
            if (result == NGGC_OK) {
                result = nggc_collector_cycle_abort_locked(heap);
            }
            break;
        case NGGC_TEST_PHASE_ACTION_MOVING_CYCLE: {
            static const uint32_t moving_phases[] = {
                NGGC_PHASE_ROOT_SNAPSHOT,
                NGGC_PHASE_MARK,
                NGGC_PHASE_REFERENCE_PROCESS,
                NGGC_PHASE_EVACUATE,
                NGGC_PHASE_REMAP,
                NGGC_PHASE_SWEEP
            };
            result = nggc_collector_cycle_begin_locked(
                heap, NGGC_COLLECTOR_CYCLE_YOUNG_EVACUATION);
            for (size_t i = 0;
                 result == NGGC_OK &&
                 i < sizeof(moving_phases) / sizeof(moving_phases[0]);
                 ++i) {
                result = nggc_phase_begin_locked(heap, moving_phases[i]);
                if (result == NGGC_OK) {
                    if (moving_phases[i] == NGGC_PHASE_EVACUATE) {
                        heap->evacuation_copy_started = 1u;
                        heap->evacuation_workers_quiescent = 1u;
                        heap->from_space_reclaim_pending = 1u;
                    } else if (moving_phases[i] == NGGC_PHASE_SWEEP) {
                        heap->from_space_reclaim_pending = 0u;
                    }
                    result = nggc_phase_end_locked(heap,
                                                   moving_phases[i]);
                }
            }
            break;
        }
        default:
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
    }

    heap->gc_phase = saved_phase;
    heap->read_barrier_active = saved_read_barrier;
    heap->collector_cycle_active = saved_cycle_active;
    heap->collector_expected_phase = saved_expected_phase;
    heap->collector_cycle_kind = saved_cycle_kind;
    heap->phase_transition_count = saved_transition_count;
    heap->collector_cycle_epoch = saved_cycle_epoch;
    heap->evacuation_epoch = saved_evacuation_epoch;
    heap->evacuation_worker_epoch = saved_evacuation_worker_epoch;
    atomic_store_explicit(&heap->evacuation_workers_active,
                          saved_evacuation_workers_active,
                          memory_order_release);
    heap->evacuation_workers_quiescent =
        saved_evacuation_workers_quiescent;
    heap->remap_worker_epoch = saved_remap_worker_epoch;
    atomic_store_explicit(&heap->remap_workers_active,
                          saved_remap_workers_active,
                          memory_order_release);
    heap->remap_workers_quiescent = saved_remap_workers_quiescent;
    heap->evacuation_copy_started = saved_evacuation_copy_started;
    heap->from_space_reclaim_pending =
        saved_from_space_reclaim_pending;
    heap->from_space_quarantine_epoch = saved_quarantine_epoch;
    heap->from_space_quarantine_active = saved_quarantine_active;
    heap->from_space_quarantine_release_epoch =
        saved_quarantine_release_epoch;
    heap->from_space_quarantine_source_end =
        saved_quarantine_source_end;
    heap->from_space_quarantine_object_count =
        saved_quarantine_object_count;
    heap->from_space_quarantine_bytes = saved_quarantine_bytes;
    heap->collect_safepoint_epoch = saved_collect_epoch;
    heap->safepoint_epoch = saved_safepoint_epoch;
    heap->safepoint_pending_threads = saved_pending_threads;
    heap->safepoint_parked_threads = saved_parked_threads;
    heap->safepoint_active = saved_safepoint_active;
    heap->mutator_concurrent_active = saved_mutator_concurrent_active;
    heap->mutator_concurrent_epoch = saved_mutator_concurrent_epoch;
    heap->mutator_concurrent_running_threads =
        saved_mutator_concurrent_running_threads;
    heap->mutator_concurrent_repark_pending_threads =
        saved_mutator_concurrent_repark_pending_threads;
    heap->collect_safepoint_owner = saved_collect_owner;
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_barrier_tss_fail_next_update(
    nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
#if NGGC_HAVE_C11_THREADS
    nggc_lock(heap);
    if (heap->test_barrier_tss_fail_next_update != 0u) {
        nggc_unlock(heap);
        return NGGC_ERR_BUSY;
    }
    heap->test_barrier_tss_fail_next_update = 1u;
    nggc_unlock(heap);
    return NGGC_OK;
#else
    (void)heap;
    return NGGC_ERR_UNSUPPORTED;
#endif
}

nggc_result nggc_internal_test_remset_publish_pause_arm(nggc_heap *heap) {
#if NGGC_HAVE_C11_THREADS
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    atomic_store_explicit(&heap->test_remset_publish_pause_entered, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_remset_publish_pause_release, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_remset_publish_pause_armed, 1u,
                          memory_order_release);
    return NGGC_OK;
#else
    (void)heap;
    return NGGC_ERR_UNSUPPORTED;
#endif
}

nggc_result nggc_internal_test_remset_publish_pause_wait(nggc_heap *heap) {
#if NGGC_HAVE_C11_THREADS
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(
               &heap->test_remset_publish_pause_entered,
               memory_order_acquire) != 0u
               ? NGGC_OK
               : NGGC_ERR_BUSY;
#else
    (void)heap;
    return NGGC_ERR_UNSUPPORTED;
#endif
}

nggc_result nggc_internal_test_remset_publish_pause_release(
    nggc_heap *heap) {
#if NGGC_HAVE_C11_THREADS
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(
            &heap->test_remset_publish_pause_entered,
            memory_order_acquire) == 0u) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(&heap->test_remset_publish_pause_release, 1u,
                          memory_order_release);
    return NGGC_OK;
#else
    (void)heap;
    return NGGC_ERR_UNSUPPORTED;
#endif
}

nggc_result nggc_internal_test_remset_classification_pause_arm(
    nggc_heap *heap) {
#if NGGC_HAVE_C11_THREADS
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    atomic_store_explicit(
        &heap->test_remset_classification_pause_entered, 0u,
        memory_order_release);
    atomic_store_explicit(
        &heap->test_remset_classification_pause_release, 0u,
        memory_order_release);
    atomic_store_explicit(
        &heap->test_remset_classification_pause_armed, 1u,
        memory_order_release);
    return NGGC_OK;
#else
    (void)heap;
    return NGGC_ERR_UNSUPPORTED;
#endif
}

nggc_result nggc_internal_test_remset_classification_pause_wait(
    nggc_heap *heap) {
#if NGGC_HAVE_C11_THREADS
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(
               &heap->test_remset_classification_pause_entered,
               memory_order_acquire) != 0u
               ? NGGC_OK
               : NGGC_ERR_BUSY;
#else
    (void)heap;
    return NGGC_ERR_UNSUPPORTED;
#endif
}

nggc_result nggc_internal_test_remset_classification_pause_release(
    nggc_heap *heap) {
#if NGGC_HAVE_C11_THREADS
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load_explicit(
            &heap->test_remset_classification_pause_entered,
            memory_order_acquire) == 0u) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(
        &heap->test_remset_classification_pause_release, 1u,
        memory_order_release);
    return NGGC_OK;
#else
    (void)heap;
    return NGGC_ERR_UNSUPPORTED;
#endif
}

nggc_result nggc_internal_test_remset_set_limit(nggc_heap *heap,
                                                 size_t max_dirty_cards) {
    if (!nggc_heap_live(heap) || max_dirty_cards == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (max_dirty_cards < heap->dirty_card_count ||
        max_dirty_cards > heap->card_count) {
        nggc_unlock(heap);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    heap->remset_max_dirty_cards = max_dirty_cards;
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_internal_test_remset_rebuild_check(nggc_heap *heap) {
    nggc_result result;
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    result = nggc_verify_remset_rebuild_locked(heap);
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_remset_fault_check(nggc_heap *heap,
                                                   uint32_t fault) {
    size_t saved_dirty_count;
    size_t saved_card_index = 0u;
    size_t saved_buffer_card = 0u;
    unsigned saved_deferred_outcome = 0u;
    unsigned saved_classification_outcome = 0u;
    size_t saved_classification_owner_region = SIZE_MAX;
    size_t saved_classification_value_region = SIZE_MAX;
    uint32_t saved_classification_owner_generation =
        NGGC_REGION_GENERATION_NONE;
    uint32_t saved_classification_value_generation =
        NGGC_REGION_GENERATION_NONE;
    unsigned char saved_card;
    uint32_t saved_mode;
    nggc_thread *saved_buffer_thread = 0;
    nggc_result result;
    if (!nggc_heap_live(heap) || heap->card_count == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    saved_dirty_count = heap->dirty_card_count;
    saved_card = atomic_load_explicit(&heap->card_table[0],
                                      memory_order_acquire);
    saved_mode = heap->remset_mode;
    if (fault == NGGC_TEST_REMSET_FAULT_CARD_COUNT) {
        heap->dirty_card_count = heap->card_count + 1u;
    } else if (fault == NGGC_TEST_REMSET_FAULT_COARSE_CARD) {
        heap->remset_mode = NGGC_REMSET_MODE_COARSE_HEAP;
        heap->dirty_card_count = heap->card_count;
        for (size_t i = 0u; i < heap->card_count; ++i) {
            atomic_store_explicit(&heap->card_table[i], 1u,
                                  memory_order_release);
        }
        atomic_store_explicit(&heap->card_table[0], 0u,
                              memory_order_release);
    } else if (fault == NGGC_TEST_REMSET_FAULT_CROSS_REGION_CARD) {
        while (saved_card_index < heap->card_count &&
               atomic_load_explicit(
                   &heap->card_table[saved_card_index],
                   memory_order_acquire) == 0u) {
            ++saved_card_index;
        }
        if (saved_card_index == heap->card_count ||
            heap->dirty_card_count == 0u) {
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
        }
        saved_card = atomic_load_explicit(
            &heap->card_table[saved_card_index], memory_order_acquire);
        atomic_store_explicit(&heap->card_table[saved_card_index], 0u,
                              memory_order_release);
        --heap->dirty_card_count;
        result = nggc_verify_remset_rebuild_locked(heap);
        heap->dirty_card_count = saved_dirty_count;
        atomic_store_explicit(&heap->card_table[saved_card_index],
                              saved_card, memory_order_release);
        heap->remset_mode = saved_mode;
        nggc_unlock(heap);
        return result;
    } else if (fault == NGGC_TEST_REMSET_FAULT_BUFFER_CARD) {
        for (saved_buffer_thread = heap->threads;
             saved_buffer_thread;
             saved_buffer_thread = saved_buffer_thread->next) {
            if (atomic_load_explicit(
                    &saved_buffer_thread->remset_card_buffer_count,
                    memory_order_acquire) != 0u &&
                atomic_load_explicit(
                    &saved_buffer_thread
                         ->remset_card_publication_inflight,
                    memory_order_acquire) == 0u) {
                break;
            }
        }
        if (!saved_buffer_thread) {
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
        }
        saved_buffer_card =
            saved_buffer_thread->remset_card_buffer[0];
        saved_buffer_thread->remset_card_buffer[0] = heap->card_count;
        nggc_unlock(heap);
        result = nggc_check(heap);
        nggc_lock(heap);
        saved_buffer_thread->remset_card_buffer[0] = saved_buffer_card;
        nggc_unlock(heap);
        return result;
    } else if (fault == NGGC_TEST_REMSET_FAULT_INFLIGHT_CARD) {
        for (saved_buffer_thread = heap->threads;
             saved_buffer_thread;
             saved_buffer_thread = saved_buffer_thread->next) {
            if (atomic_load_explicit(
                    &saved_buffer_thread
                         ->remset_card_publication_inflight,
                    memory_order_acquire) != 0u) {
                break;
            }
        }
        if (!saved_buffer_thread) {
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
        }
        saved_buffer_card =
            saved_buffer_thread->remset_card_publication_card;
        saved_buffer_thread->remset_card_publication_card =
            heap->card_count;
        nggc_unlock(heap);
        result = nggc_check(heap);
        nggc_lock(heap);
        saved_buffer_thread->remset_card_publication_card =
            saved_buffer_card;
        nggc_unlock(heap);
        return result;
    } else if (fault == NGGC_TEST_REMSET_FAULT_DEFERRED_OUTCOME) {
        unsigned injected_deferred_outcome = 0u;
        nggc_stats fault_stats;
        for (saved_buffer_thread = heap->threads;
             saved_buffer_thread;
             saved_buffer_thread = saved_buffer_thread->next) {
            size_t count;
            size_t index;
            size_t card;
            size_t prefix_count;
            int descriptor_duplicate = 0;
            if (atomic_load_explicit(
                    &saved_buffer_thread
                         ->remset_card_publication_inflight,
                    memory_order_acquire) != 0u) {
                continue;
            }
            saved_deferred_outcome = atomic_load_explicit(
                &saved_buffer_thread->remset_card_deferred_outcome,
                memory_order_acquire);
            count = (size_t)atomic_load_explicit(
                &saved_buffer_thread->remset_card_buffer_count,
                memory_order_acquire);
            index = saved_buffer_thread->remset_card_publication_index;
            card = saved_buffer_thread->remset_card_publication_card;
            if (saved_deferred_outcome >
                    NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED ||
                count > saved_buffer_thread->remset_card_buffer_capacity ||
                index >= saved_buffer_thread->remset_card_buffer_capacity ||
                card >= heap->card_count) {
                continue;
            }
            prefix_count = count < index ? count : index;
            for (size_t i = 0u; i < prefix_count; ++i) {
                if (saved_buffer_thread->remset_card_buffer[i] == card) {
                    descriptor_duplicate = 1;
                    break;
                }
            }
            if (count == index + 1u && !descriptor_duplicate &&
                saved_buffer_thread->remset_card_buffer[index] == card &&
                (saved_deferred_outcome ==
                     NGGC_REMSET_DEFERRED_OUTCOME_NONE ||
                 saved_deferred_outcome ==
                     NGGC_REMSET_DEFERRED_OUTCOME_APPENDED)) {
                injected_deferred_outcome =
                    NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED;
                break;
            }
            if (count == index && descriptor_duplicate &&
                (saved_deferred_outcome ==
                     NGGC_REMSET_DEFERRED_OUTCOME_NONE ||
                 saved_deferred_outcome ==
                     NGGC_REMSET_DEFERRED_OUTCOME_DEDUPLICATED)) {
                injected_deferred_outcome =
                    NGGC_REMSET_DEFERRED_OUTCOME_APPENDED;
                break;
            }
        }
        if (!saved_buffer_thread || injected_deferred_outcome == 0u) {
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
        }
        atomic_store_explicit(
            &saved_buffer_thread->remset_card_deferred_outcome,
            injected_deferred_outcome,
            memory_order_release);
        nggc_unlock(heap);
        result = nggc_check(heap);
        if (result == NGGC_ERR_CORRUPTION) {
            memset(&fault_stats, 0, sizeof(fault_stats));
            fault_stats.size = sizeof(fault_stats);
            result = nggc_get_stats(heap, &fault_stats);
        }
        nggc_lock(heap);
        atomic_store_explicit(
            &saved_buffer_thread->remset_card_deferred_outcome,
            saved_deferred_outcome, memory_order_release);
        nggc_unlock(heap);
        return result;
    } else if (fault == NGGC_TEST_REMSET_FAULT_CARD_VALUE) {
        atomic_store_explicit(&heap->card_table[0], 2u,
                              memory_order_release);
    } else if (fault ==
               NGGC_TEST_REMSET_FAULT_CLASSIFICATION_OUTCOME) {
        nggc_stats fault_stats;
        for (saved_buffer_thread = heap->threads;
             saved_buffer_thread;
             saved_buffer_thread = saved_buffer_thread->next) {
            if ((saved_buffer_thread->state == NGGC_THREAD_RUNNING ||
                 saved_buffer_thread->state ==
                     NGGC_THREAD_CONCURRENT_RUNNING) &&
                atomic_load_explicit(
                    &saved_buffer_thread
                         ->remset_classification_publication_inflight,
                    memory_order_acquire) == 0u &&
                atomic_load_explicit(
                    &saved_buffer_thread
                         ->remset_classification_deferred_outcome,
                    memory_order_acquire) ==
                    NGGC_REMSET_CLASSIFICATION_NONE &&
                saved_buffer_thread
                        ->remset_classification_owner_region ==
                    SIZE_MAX &&
                saved_buffer_thread
                        ->remset_classification_value_region ==
                    SIZE_MAX) {
                break;
            }
        }
        if (!saved_buffer_thread || heap->region_count == 0u) {
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
        }
        saved_classification_owner_region =
            saved_buffer_thread->remset_classification_owner_region;
        saved_classification_value_region =
            saved_buffer_thread->remset_classification_value_region;
        saved_classification_owner_generation =
            saved_buffer_thread
                ->remset_classification_owner_generation;
        saved_classification_value_generation =
            saved_buffer_thread
                ->remset_classification_value_generation;
        saved_classification_outcome = atomic_load_explicit(
            &saved_buffer_thread
                 ->remset_classification_deferred_outcome,
            memory_order_acquire);
        saved_buffer_thread->remset_classification_owner_region = 0u;
        saved_buffer_thread->remset_classification_value_region =
            SIZE_MAX;
        saved_buffer_thread->remset_classification_owner_generation =
            NGGC_REGION_GENERATION_YOUNG;
        saved_buffer_thread->remset_classification_value_generation =
            NGGC_REGION_GENERATION_NONE;
        atomic_store_explicit(
            &saved_buffer_thread
                 ->remset_classification_deferred_outcome,
            NGGC_REMSET_CLASSIFICATION_RECORD |
                NGGC_REMSET_CLASSIFICATION_CROSS_REGION,
            memory_order_release);
        nggc_unlock(heap);
        result = nggc_check(heap);
        if (result == NGGC_ERR_CORRUPTION) {
            memset(&fault_stats, 0, sizeof(fault_stats));
            fault_stats.size = sizeof(fault_stats);
            result = nggc_get_stats(heap, &fault_stats);
        }
        nggc_lock(heap);
        atomic_store_explicit(
            &saved_buffer_thread
                 ->remset_classification_deferred_outcome,
            saved_classification_outcome, memory_order_release);
        saved_buffer_thread->remset_classification_owner_region =
            saved_classification_owner_region;
        saved_buffer_thread->remset_classification_value_region =
            saved_classification_value_region;
        saved_buffer_thread->remset_classification_owner_generation =
            saved_classification_owner_generation;
        saved_buffer_thread->remset_classification_value_generation =
            saved_classification_value_generation;
        nggc_unlock(heap);
        return result;
    } else {
        nggc_unlock(heap);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_unlock(heap);
    result = nggc_check(heap);
    nggc_lock(heap);
    heap->dirty_card_count = saved_dirty_count;
    atomic_store_explicit(&heap->card_table[0], saved_card,
                          memory_order_release);
    heap->remset_mode = saved_mode;
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_region_fault_check(nggc_heap *heap,
                                                   uint32_t fault) {
    uintptr_t saved_cursor;
    uintptr_t saved_owner0;
    uintptr_t saved_owner1 = 0u;
    uint32_t saved_lease_active;
    uint32_t saved_generation;
    uint32_t saved_placement;
    uint32_t saved_humongous_role = 0u;
    uint32_t saved_humongous_offset = 0u;
    uint32_t saved_humongous_length = 0u;
    uint32_t saved_smart_pressure_source;
    uint32_t saved_smart_slow_vm;
    uint64_t saved_smart_slow_threshold;
    size_t saved_region_index = 0u;
    size_t saved_pending;
    int restore_humongous = 0;
    nggc_result result;
    if (!nggc_heap_live(heap) || heap->region_count == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    saved_cursor = heap->regions[0].allocation_cursor;
    saved_owner0 = heap->regions[0].allocation_owner;
    if (heap->region_count > 1u) {
        saved_owner1 = heap->regions[1].allocation_owner;
    }
    saved_lease_active = heap->allocator_lease_active;
    saved_generation = heap->regions[0].generation;
    saved_placement = heap->regions[0].placement;
    saved_pending = heap->safepoint_pending_threads;
    saved_smart_pressure_source =
        heap->interior_decommit_smart_pressure_source;
    saved_smart_slow_vm =
        heap->interior_decommit_smart_slow_vm_observed;
    saved_smart_slow_threshold =
        heap->interior_decommit_slow_ns_per_region;
    switch (fault) {
        case NGGC_TEST_REGION_FAULT_CURSOR:
            heap->regions[0].allocation_cursor =
                heap->regions[0].start + NGGC_OBJECT_ALIGNMENT;
            break;
        case NGGC_TEST_REGION_FAULT_OWNER:
            heap->regions[0].allocation_owner = UINTPTR_MAX;
            break;
        case NGGC_TEST_REGION_FAULT_LEASE_PENDING:
            heap->allocator_lease_active = 1u;
            heap->safepoint_pending_threads = 1u;
            break;
        case NGGC_TEST_REGION_FAULT_OWNER_DUPLICATE:
            if (heap->region_count < 2u) {
                nggc_unlock(heap);
                return NGGC_ERR_UNSUPPORTED;
            }
            heap->regions[0].allocation_owner = (uintptr_t)heap;
            heap->regions[1].allocation_owner = (uintptr_t)heap;
            break;
        case NGGC_TEST_REGION_FAULT_GENERATION_INVALID:
            heap->regions[0].generation = UINT32_MAX;
            break;
        case NGGC_TEST_REGION_FAULT_PLACEMENT_INVALID:
            heap->regions[0].placement = UINT32_MAX;
            break;
        case NGGC_TEST_REGION_FAULT_PLACEMENT_NON_SPECIAL:
            heap->regions[0].generation = NGGC_REGION_GENERATION_NONE;
            heap->regions[0].placement =
                NGGC_REGION_PLACEMENT_PINNED_SPECIAL;
            break;
        case NGGC_TEST_REGION_FAULT_PLACEMENT_MISMATCH:
            saved_region_index = heap->region_count;
            for (size_t i = 0u; i < heap->region_count; ++i) {
                if (heap->regions[i].generation ==
                        NGGC_REGION_GENERATION_SPECIAL &&
                    heap->regions[i].placement !=
                        NGGC_REGION_PLACEMENT_NONE &&
                    heap->regions[i].live_bytes != 0u) {
                    saved_region_index = i;
                    break;
                }
            }
            if (saved_region_index >= heap->region_count) {
                nggc_unlock(heap);
                return NGGC_ERR_UNSUPPORTED;
            }
            saved_generation = heap->regions[saved_region_index].generation;
            saved_placement = heap->regions[saved_region_index].placement;
            heap->regions[saved_region_index].placement =
                heap->regions[saved_region_index].placement ==
                        NGGC_REGION_PLACEMENT_PINNED_SPECIAL
                    ? NGGC_REGION_PLACEMENT_MOVABLE_SPECIAL
                    : NGGC_REGION_PLACEMENT_PINNED_SPECIAL;
            break;
        case NGGC_TEST_REGION_FAULT_HUMONGOUS_ROLE_INVALID:
        case NGGC_TEST_REGION_FAULT_HUMONGOUS_OFFSET:
            saved_region_index = heap->region_count;
            for (size_t i = 0u; i < heap->region_count; ++i) {
                uint32_t role = heap->regions[i].humongous_role;
                if ((fault ==
                         NGGC_TEST_REGION_FAULT_HUMONGOUS_ROLE_INVALID &&
                     role != NGGC_REGION_HUMONGOUS_NONE) ||
                    (fault == NGGC_TEST_REGION_FAULT_HUMONGOUS_OFFSET &&
                     role == NGGC_REGION_HUMONGOUS_CONTINUATION)) {
                    saved_region_index = i;
                    break;
                }
            }
            if (saved_region_index >= heap->region_count) {
                nggc_unlock(heap);
                return NGGC_ERR_UNSUPPORTED;
            }
            saved_generation =
                heap->regions[saved_region_index].generation;
            saved_placement =
                heap->regions[saved_region_index].placement;
            saved_humongous_role =
                heap->regions[saved_region_index].humongous_role;
            saved_humongous_offset =
                heap->regions[saved_region_index].humongous_run_offset;
            saved_humongous_length =
                heap->regions[saved_region_index].humongous_run_length;
            restore_humongous = 1;
            if (fault ==
                NGGC_TEST_REGION_FAULT_HUMONGOUS_ROLE_INVALID) {
                heap->regions[saved_region_index].humongous_role =
                    UINT32_MAX;
            } else {
                heap->regions[saved_region_index].humongous_run_offset =
                    0u;
            }
            break;
        case NGGC_TEST_REGION_FAULT_SMART_PRESSURE_SOURCE:
            heap->interior_decommit_smart_pressure_source =
                NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_CALLBACK;
            break;
        case NGGC_TEST_REGION_FAULT_SMART_SLOW_VM:
            heap->interior_decommit_smart_slow_vm_observed = 1u;
            heap->interior_decommit_slow_ns_per_region = 0u;
            break;
        default:
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
    }
    result = nggc_verify_regions_locked(heap);
    heap->regions[0].allocation_cursor = saved_cursor;
    heap->regions[0].allocation_owner = saved_owner0;
    if (heap->region_count > 1u) {
        heap->regions[1].allocation_owner = saved_owner1;
    }
    heap->allocator_lease_active = saved_lease_active;
    heap->regions[saved_region_index].generation = saved_generation;
    heap->regions[saved_region_index].placement = saved_placement;
    if (restore_humongous) {
        heap->regions[saved_region_index].humongous_role =
            saved_humongous_role;
        heap->regions[saved_region_index].humongous_run_offset =
            saved_humongous_offset;
        heap->regions[saved_region_index].humongous_run_length =
            saved_humongous_length;
    }
    heap->safepoint_pending_threads = saved_pending;
    heap->interior_decommit_smart_pressure_source =
        saved_smart_pressure_source;
    heap->interior_decommit_smart_slow_vm_observed =
        saved_smart_slow_vm;
    heap->interior_decommit_slow_ns_per_region =
        saved_smart_slow_threshold;
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_large_free_bin_fault_check(
    nggc_heap *heap, uint32_t fault) {
    nggc_object_header *saved_target_head = 0;
    nggc_object_header *order_first = 0;
    nggc_object_header *order_second = 0;
    nggc_object_header *order_third = 0;
    size_t source_index = NGGC_LARGE_FREE_BIN_COUNT;
    size_t target_index = NGGC_LARGE_FREE_BIN_COUNT;
    uint32_t saved_mask;
    nggc_result result;
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    for (size_t i = 0u; i < NGGC_LARGE_FREE_BIN_COUNT; ++i) {
        if (heap->large_free_bins[i]) {
            source_index = i;
            break;
        }
    }
    if (source_index == NGGC_LARGE_FREE_BIN_COUNT) {
        nggc_unlock(heap);
        return NGGC_ERR_UNSUPPORTED;
    }
    if ((heap->large_free_bin_mask &
         ((uint32_t)1u << source_index)) == 0u) {
        nggc_unlock(heap);
        return NGGC_ERR_CORRUPTION;
    }
    saved_mask = heap->large_free_bin_mask;
    switch (fault) {
        case NGGC_TEST_LARGE_BIN_FAULT_MASK:
            heap->large_free_bin_mask &=
                ~((uint32_t)1u << source_index);
            break;
        case NGGC_TEST_LARGE_BIN_FAULT_WRONG_BIN:
            target_index = source_index + 1u;
            if (target_index == NGGC_LARGE_FREE_BIN_COUNT) {
                target_index = 0u;
            }
            saved_target_head = heap->large_free_bins[target_index];
            heap->large_free_bins[target_index] =
                heap->large_free_bins[source_index];
            heap->large_free_bin_mask |=
                (uint32_t)1u << target_index;
            break;
        case NGGC_TEST_LARGE_BIN_FAULT_ORDER:
            source_index = NGGC_LARGE_FREE_BIN_COUNT;
            for (size_t i = 0u;
                 i < NGGC_LARGE_FREE_BIN_COUNT; ++i) {
                nggc_object_header *first = heap->large_free_bins[i];
                nggc_object_header *second =
                    first ? (nggc_object_header *)
                                nggc_forwarding_load_relaxed(first)
                          : 0;
                if (second) {
                    source_index = i;
                    order_first = first;
                    order_second = second;
                    order_third = (nggc_object_header *)
                        nggc_forwarding_load_relaxed(second);
                    break;
                }
            }
            if (source_index == NGGC_LARGE_FREE_BIN_COUNT) {
                nggc_unlock(heap);
                return NGGC_ERR_UNSUPPORTED;
            }
            heap->large_free_bins[source_index] = order_second;
            nggc_forwarding_store_free_next(order_second, order_first);
            nggc_forwarding_store_free_next(order_first, order_third);
            break;
        default:
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_unlock(heap);
    result = nggc_check(heap);
    nggc_lock(heap);
    if (target_index < NGGC_LARGE_FREE_BIN_COUNT) {
        heap->large_free_bins[target_index] = saved_target_head;
    }
    if (order_first && order_second) {
        heap->large_free_bins[source_index] = order_first;
        nggc_forwarding_store_free_next(order_first, order_second);
        nggc_forwarding_store_free_next(order_second, order_third);
    }
    heap->large_free_bin_mask = saved_mask;
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_decommitted_run_fault_check(
    nggc_heap *heap, uint32_t fault) {
    size_t bin_index = NGGC_LARGE_FREE_BIN_COUNT;
    size_t start_index = SIZE_MAX;
    size_t changed_index = SIZE_MAX;
    size_t order_first_encoded = 0u;
    size_t order_second_encoded = 0u;
    size_t order_third_encoded = 0u;
    size_t order_first_index = SIZE_MAX;
    size_t order_second_index = SIZE_MAX;
    uint32_t saved_mask;
    uint32_t saved_offset = 0u;
    nggc_result result;
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    for (size_t i = 0u; i < NGGC_LARGE_FREE_BIN_COUNT; ++i) {
        if (heap->decommitted_large_free_bins[i] != 0u) {
            if (!nggc_decommitted_run_decode_index(
                    heap, heap->decommitted_large_free_bins[i],
                    &start_index)) {
                nggc_unlock(heap);
                return NGGC_ERR_CORRUPTION;
            }
            bin_index = i;
            break;
        }
    }
    if (bin_index == NGGC_LARGE_FREE_BIN_COUNT ||
        start_index >= heap->region_count ||
        !nggc_interior_decommitted_run_locked(
            heap, start_index, 0, 0)) {
        nggc_unlock(heap);
        return NGGC_ERR_UNSUPPORTED;
    }
    saved_mask = heap->decommitted_large_free_bin_mask;
    switch (fault) {
        case NGGC_TEST_DECOMMITTED_BIN_FAULT_MASK:
            heap->decommitted_large_free_bin_mask &=
                ~((uint32_t)1u << bin_index);
            break;
        case NGGC_TEST_DECOMMITTED_RUN_FAULT_OFFSET:
            if (heap->regions[start_index].humongous_run_length < 2u) {
                nggc_unlock(heap);
                return NGGC_ERR_UNSUPPORTED;
            }
            changed_index = start_index + 1u;
            saved_offset =
                heap->regions[changed_index].humongous_run_offset;
            heap->regions[changed_index].humongous_run_offset = 0u;
            break;
        case NGGC_TEST_DECOMMITTED_BIN_FAULT_ORDER:
            bin_index = NGGC_LARGE_FREE_BIN_COUNT;
            for (size_t i = 0u;
                 i < NGGC_LARGE_FREE_BIN_COUNT; ++i) {
                size_t first_encoded =
                    heap->decommitted_large_free_bins[i];
                size_t first_index;
                size_t second_encoded;
                size_t second_index;
                if (first_encoded == 0u ||
                    !nggc_decommitted_run_decode_index(
                        heap, first_encoded, &first_index)) {
                    continue;
                }
                second_encoded =
                    heap->regions[first_index].decommitted_free_next;
                if (second_encoded == 0u ||
                    !nggc_decommitted_run_decode_index(
                        heap, second_encoded, &second_index)) {
                    continue;
                }
                bin_index = i;
                order_first_encoded = first_encoded;
                order_second_encoded = second_encoded;
                order_first_index = first_index;
                order_second_index = second_index;
                order_third_encoded =
                    heap->regions[second_index].decommitted_free_next;
                break;
            }
            if (bin_index == NGGC_LARGE_FREE_BIN_COUNT) {
                nggc_unlock(heap);
                return NGGC_ERR_UNSUPPORTED;
            }
            heap->decommitted_large_free_bins[bin_index] =
                order_second_encoded;
            heap->regions[order_second_index].decommitted_free_next =
                order_first_encoded;
            heap->regions[order_first_index].decommitted_free_next =
                order_third_encoded;
            break;
        default:
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_unlock(heap);
    result = nggc_check(heap);
    nggc_lock(heap);
    heap->decommitted_large_free_bin_mask = saved_mask;
    if (changed_index < heap->region_count) {
        heap->regions[changed_index].humongous_run_offset = saved_offset;
    }
    if (order_first_index < heap->region_count &&
        order_second_index < heap->region_count) {
        heap->decommitted_large_free_bins[bin_index] =
            order_first_encoded;
        heap->regions[order_first_index].decommitted_free_next =
            order_second_encoded;
        heap->regions[order_second_index].decommitted_free_next =
            order_third_encoded;
    }
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_object_generation(
    nggc_heap *heap, void *object, uint32_t *out_generation) {
    nggc_object_header *header;
    if (!nggc_heap_live(heap) || !object || !out_generation) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    header = nggc_live_header_from_object(heap, object);
    if (!header) {
        nggc_unlock(heap);
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    *out_generation = header->generation;
    nggc_unlock(heap);
    return NGGC_OK;
}

nggc_result nggc_internal_test_forwarding_cas_check(
    nggc_heap *heap, void *source, void *winner, void *loser) {
    nggc_object_header *source_header;
    nggc_object_header *winner_header;
    nggc_object_header *loser_header;
    nggc_object_header *target;
    void *published;
    uint32_t epoch;
    uint32_t wrong_epoch;
    int won;
    nggc_result result = NGGC_OK;
    if (!nggc_heap_live(heap) || !source || !winner || !loser ||
        source == winner || source == loser || winner == loser) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    if (heap->gc_phase != NGGC_PHASE_IDLE ||
        heap->collector_cycle_active || heap->safepoint_active) {
        nggc_unlock(heap);
        return NGGC_ERR_BUSY;
    }
    source_header = nggc_live_header_from_object(heap, source);
    winner_header = nggc_live_header_from_object(heap, winner);
    loser_header = nggc_live_header_from_object(heap, loser);
    if (!source_header || !winner_header || !loser_header ||
        nggc_forwarding_target_locked(heap, source_header, &target) !=
            NGGC_OK ||
        target || nggc_forwarding_load_acquire(winner_header) != 0 ||
        nggc_forwarding_epoch_load_acquire(winner_header) != 0u ||
        nggc_forwarding_load_acquire(loser_header) != 0 ||
        nggc_forwarding_epoch_load_acquire(loser_header) != 0u) {
        nggc_unlock(heap);
        return NGGC_ERR_CORRUPTION;
    }
    epoch = heap->evacuation_epoch + 1u;
    if (epoch == 0u) {
        epoch = 1u;
    }
    wrong_epoch = epoch + 1u;
    if (wrong_epoch == 0u) {
        wrong_epoch = 1u;
    }
    if (nggc_forwarding_publish_locked(
            heap, source_header, winner, epoch, &won, &published) !=
            NGGC_OK ||
        !won || published != winner ||
        nggc_forwarding_publish_locked(
            heap, source_header, loser, epoch, &won, &published) !=
            NGGC_OK ||
        won || published != winner ||
        nggc_forwarding_publish_locked(
            heap, source_header, loser, wrong_epoch, &won, &published) !=
            NGGC_ERR_CORRUPTION ||
        won || published != winner) {
        result = NGGC_ERR_CORRUPTION;
    }
    if (result == NGGC_OK &&
        (nggc_forwarding_target_locked(heap, source_header, &target) !=
             NGGC_OK ||
         target != winner_header)) {
        result = NGGC_ERR_CORRUPTION;
    }
    nggc_forwarding_clear_live(source_header);
    if (result == NGGC_OK &&
        (nggc_forwarding_target_locked(heap, source_header, &target) !=
             NGGC_OK ||
         target)) {
        result = NGGC_ERR_CORRUPTION;
    }
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_tlab_fast_pause_arm(nggc_heap *heap) {
    if (!nggc_heap_live(heap) ||
        atomic_load_explicit(&heap->type_table_published,
                             memory_order_acquire) == 0u ||
        atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
            NGGC_TLAB_FAST_GATE_OPEN) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(&heap->test_tlab_fast_pause_entered, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_tlab_fast_pause_release, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_tlab_fast_pause_armed, 1u,
                          memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_tlab_fast_pause_wait(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(&heap->test_tlab_fast_pause_entered,
                                memory_order_acquire) != 0u
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

nggc_result nggc_internal_test_tlab_fast_gate_closed(nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(&heap->tlab_fast_gate,
                                memory_order_acquire) ==
                   NGGC_TLAB_FAST_GATE_CLOSED
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

nggc_result nggc_internal_test_tlab_fast_pause_release(nggc_heap *heap) {
    if (!nggc_heap_live(heap) ||
        atomic_load_explicit(&heap->test_tlab_fast_pause_entered,
                             memory_order_acquire) == 0u) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(&heap->test_tlab_fast_pause_release, 1u,
                          memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_tlab_fast_enrollment_pause_arm(
    nggc_heap *heap) {
    if (!nggc_heap_live(heap) ||
        atomic_load_explicit(&heap->type_table_published,
                             memory_order_acquire) == 0u ||
        atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
            NGGC_TLAB_FAST_GATE_OPEN) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(
        &heap->test_tlab_fast_enrollment_pause_stage, 0u,
        memory_order_release);
    atomic_store_explicit(
        &heap->test_tlab_fast_enrollment_pause_release_stage, 0u,
        memory_order_release);
    atomic_store_explicit(
        &heap->test_tlab_fast_enrollment_pause_armed, 1u,
        memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_tlab_fast_enrollment_pause_wait(
    nggc_heap *heap, uint32_t stage) {
    if (!nggc_heap_live(heap) || (stage != 1u && stage != 2u)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(
               &heap->test_tlab_fast_enrollment_pause_stage,
               memory_order_acquire) >= stage
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

nggc_result nggc_internal_test_tlab_fast_enrollment_pause_advance(
    nggc_heap *heap, uint32_t stage) {
    uint32_t entered;
    uint32_t released;
    if (!nggc_heap_live(heap) || (stage != 1u && stage != 2u)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    entered = atomic_load_explicit(
        &heap->test_tlab_fast_enrollment_pause_stage,
        memory_order_acquire);
    released = atomic_load_explicit(
        &heap->test_tlab_fast_enrollment_pause_release_stage,
        memory_order_acquire);
    if (entered != stage || released + 1u != stage) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(
        &heap->test_tlab_fast_enrollment_pause_release_stage, stage,
        memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_tlab_fast_flush_pause_arm(
    nggc_heap *heap) {
    if (!nggc_heap_live(heap) ||
        atomic_load_explicit(&heap->tlab_fast_gate,
                             memory_order_acquire) !=
            NGGC_TLAB_FAST_GATE_OPEN) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(&heap->test_tlab_fast_flush_pause_entered, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_tlab_fast_flush_pause_release, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_tlab_fast_flush_busy_observed, 0u,
                          memory_order_release);
    atomic_store_explicit(&heap->test_tlab_fast_flush_pause_armed, 1u,
                          memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_tlab_fast_flush_pause_wait(
    nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(
               &heap->test_tlab_fast_flush_pause_entered,
               memory_order_acquire) != 0u
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

nggc_result nggc_internal_test_tlab_fast_flush_pause_release(
    nggc_heap *heap) {
    if (!nggc_heap_live(heap) ||
        atomic_load_explicit(&heap->test_tlab_fast_flush_pause_entered,
                             memory_order_acquire) == 0u) {
        return NGGC_ERR_BUSY;
    }
    atomic_store_explicit(&heap->test_tlab_fast_flush_pause_release, 1u,
                          memory_order_release);
    return NGGC_OK;
}

nggc_result nggc_internal_test_tlab_fast_flush_busy_wait(
    nggc_heap *heap) {
    if (!nggc_heap_live(heap)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    return atomic_load_explicit(
               &heap->test_tlab_fast_flush_busy_observed,
               memory_order_acquire) != 0u
               ? NGGC_OK
               : NGGC_ERR_BUSY;
}

typedef struct nggc_test_tlab_thread_snapshot {
    nggc_thread *next;
    uint64_t observed_safepoint_epoch;
    size_t tlab_region_index;
    uint64_t tlab_lease_epoch;
    uintptr_t tlab_reserved_start;
    uintptr_t tlab_published_top;
    uintptr_t tlab_reserved_end;
    uint32_t tlab_state;
    size_t tlab_target_size;
    uint32_t tlab_high_utilization_streak;
    uint32_t tlab_low_utilization_streak;
    uint64_t tlab_object_id_next;
    size_t tlab_object_id_remaining;
    uintptr_t tlab_fast_top;
    uint32_t tlab_fast_inflight;
} nggc_test_tlab_thread_snapshot;

static void nggc_test_tlab_thread_snapshot_take(
    const nggc_thread *thread, nggc_test_tlab_thread_snapshot *snapshot) {
    snapshot->next = thread->next;
    snapshot->observed_safepoint_epoch =
        thread->observed_safepoint_epoch;
    snapshot->tlab_region_index = thread->tlab_region_index;
    snapshot->tlab_lease_epoch = thread->tlab_lease_epoch;
    snapshot->tlab_reserved_start = thread->tlab_reserved_start;
    snapshot->tlab_published_top = thread->tlab_published_top;
    snapshot->tlab_reserved_end = thread->tlab_reserved_end;
    snapshot->tlab_state = thread->tlab_state;
    snapshot->tlab_target_size = thread->tlab_target_size;
    snapshot->tlab_high_utilization_streak =
        thread->tlab_high_utilization_streak;
    snapshot->tlab_low_utilization_streak =
        thread->tlab_low_utilization_streak;
    snapshot->tlab_object_id_next = thread->tlab_object_id_next;
    snapshot->tlab_object_id_remaining =
        thread->tlab_object_id_remaining;
    snapshot->tlab_fast_top = atomic_load_explicit(
        &thread->tlab_fast_top, memory_order_acquire);
    snapshot->tlab_fast_inflight = atomic_load_explicit(
        &thread->tlab_fast_inflight, memory_order_acquire);
}

static void nggc_test_tlab_thread_snapshot_restore(
    nggc_thread *thread,
    const nggc_test_tlab_thread_snapshot *snapshot) {
    thread->next = snapshot->next;
    thread->observed_safepoint_epoch =
        snapshot->observed_safepoint_epoch;
    thread->tlab_region_index = snapshot->tlab_region_index;
    thread->tlab_lease_epoch = snapshot->tlab_lease_epoch;
    thread->tlab_reserved_start = snapshot->tlab_reserved_start;
    thread->tlab_published_top = snapshot->tlab_published_top;
    thread->tlab_reserved_end = snapshot->tlab_reserved_end;
    thread->tlab_state = snapshot->tlab_state;
    thread->tlab_target_size = snapshot->tlab_target_size;
    thread->tlab_high_utilization_streak =
        snapshot->tlab_high_utilization_streak;
    thread->tlab_low_utilization_streak =
        snapshot->tlab_low_utilization_streak;
    thread->tlab_object_id_next = snapshot->tlab_object_id_next;
    thread->tlab_object_id_remaining =
        snapshot->tlab_object_id_remaining;
    atomic_store_explicit(&thread->tlab_fast_top,
                          snapshot->tlab_fast_top,
                          memory_order_release);
    atomic_store_explicit(&thread->tlab_fast_inflight,
                          snapshot->tlab_fast_inflight,
                          memory_order_release);
}

nggc_result nggc_internal_test_tlab_fault_check(nggc_heap *heap,
                                                 nggc_thread *thread,
                                                 uint32_t fault) {
    nggc_region_meta saved_region;
    nggc_region_meta saved_second_region;
    nggc_test_tlab_thread_snapshot saved_thread;
    nggc_thread overlap_thread;
    nggc_thread *saved_threads;
    nggc_object_header *saved_free_list;
    size_t saved_pending;
    uint32_t saved_fast_gate;
    nggc_result result;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        heap->region_count == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    saved_fast_gate = atomic_load_explicit(
        &heap->tlab_fast_gate, memory_order_acquire);
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        return result;
    }
    saved_region = heap->regions[0];
    if (heap->region_count > 1u) {
        saved_second_region = heap->regions[1];
    } else {
        memset(&saved_second_region, 0, sizeof(saved_second_region));
    }
    nggc_test_tlab_thread_snapshot_take(thread, &saved_thread);
    saved_threads = heap->threads;
    saved_free_list = heap->free_list;
    saved_pending = heap->safepoint_pending_threads;
    switch (fault) {
        case NGGC_TEST_TLAB_FAULT_REGION_ORPHAN:
            heap->regions[0].tlab_owner = thread;
            heap->regions[0].tlab_lease_epoch = 1u;
            heap->regions[0].tlab_reserved_start =
                heap->regions[0].start;
            heap->regions[0].tlab_published_top =
                heap->regions[0].start;
            heap->regions[0].tlab_reserved_end = heap->regions[0].end;
            heap->regions[0].tlab_state = NGGC_TLAB_ACTIVE;
            break;
        case NGGC_TEST_TLAB_FAULT_THREAD_ORPHAN:
            thread->tlab_region_index = 0u;
            thread->tlab_lease_epoch = 1u;
            thread->tlab_reserved_start = heap->regions[0].start;
            thread->tlab_published_top = heap->regions[0].start;
            thread->tlab_reserved_end = heap->regions[0].end;
            thread->tlab_state = NGGC_TLAB_ACTIVE;
            break;
        case NGGC_TEST_TLAB_FAULT_TOP_BOUNDS:
            heap->regions[0].tlab_owner = thread;
            heap->regions[0].tlab_lease_epoch = 1u;
            heap->regions[0].tlab_reserved_start =
                heap->regions[0].start;
            heap->regions[0].tlab_published_top = heap->regions[0].end;
            heap->regions[0].tlab_reserved_end = heap->regions[0].start;
            heap->regions[0].tlab_state = NGGC_TLAB_ACTIVE;
            thread->tlab_region_index = 0u;
            thread->tlab_lease_epoch = 1u;
            thread->tlab_reserved_start = heap->regions[0].start;
            thread->tlab_published_top = heap->regions[0].end;
            thread->tlab_reserved_end = heap->regions[0].start;
            thread->tlab_state = NGGC_TLAB_ACTIVE;
            break;
        case NGGC_TEST_TLAB_FAULT_EPOCH_MISMATCH:
            heap->regions[0].tlab_owner = thread;
            heap->regions[0].tlab_lease_epoch = 2u;
            heap->regions[0].tlab_reserved_start =
                heap->regions[0].start;
            heap->regions[0].tlab_published_top =
                heap->regions[0].start;
            heap->regions[0].tlab_reserved_end = heap->regions[0].end;
            heap->regions[0].tlab_state = NGGC_TLAB_ACTIVE;
            thread->tlab_region_index = 0u;
            thread->tlab_lease_epoch = 1u;
            thread->tlab_reserved_start = heap->regions[0].start;
            thread->tlab_published_top = heap->regions[0].start;
            thread->tlab_reserved_end = heap->regions[0].end;
            thread->tlab_state = NGGC_TLAB_ACTIVE;
            break;
        case NGGC_TEST_TLAB_FAULT_ACK_NOT_RETIRED:
            if (thread->tlab_state != NGGC_TLAB_ACTIVE) {
                atomic_store_explicit(&heap->tlab_fast_gate,
                                      saved_fast_gate,
                                      memory_order_release);
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            heap->safepoint_pending_threads = 1u;
            thread->observed_safepoint_epoch = heap->safepoint_epoch;
            break;
        case NGGC_TEST_TLAB_FAULT_RESERVED_IN_FREE_LIST:
            if (thread->tlab_state != NGGC_TLAB_ACTIVE ||
                thread->tlab_published_top >= thread->tlab_reserved_end) {
                atomic_store_explicit(&heap->tlab_fast_gate,
                                      saved_fast_gate,
                                      memory_order_release);
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            heap->free_list =
                (nggc_object_header *)thread->tlab_published_top;
            break;
        case NGGC_TEST_TLAB_FAULT_INTERVAL_OVERLAP:
            if (heap->region_count < 2u || heap->region_size < 1024u) {
                atomic_store_explicit(&heap->tlab_fast_gate,
                                      saved_fast_gate,
                                      memory_order_release);
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            memset(&overlap_thread, 0, sizeof(overlap_thread));
            overlap_thread.magic = NGGC_THREAD_MAGIC;
            overlap_thread.heap = heap;
            overlap_thread.state = NGGC_THREAD_RUNNING;
            overlap_thread.tlab_region_index = 1u;
            overlap_thread.tlab_lease_epoch = 2u;
            overlap_thread.tlab_reserved_start =
                heap->regions[0].start + 256u;
            overlap_thread.tlab_published_top =
                overlap_thread.tlab_reserved_start;
            overlap_thread.tlab_reserved_end =
                heap->regions[0].start + 768u;
            overlap_thread.tlab_state = NGGC_TLAB_ACTIVE;
            overlap_thread.tlab_target_size = heap->tlab_size;
            atomic_init(&overlap_thread.tlab_fast_top,
                        overlap_thread.tlab_published_top);
            atomic_init(&overlap_thread.tlab_fast_inflight, 0u);
            overlap_thread.next = 0;

            heap->threads = thread;
            thread->next = &overlap_thread;
            heap->regions[0].tlab_owner = thread;
            heap->regions[0].tlab_lease_epoch = 1u;
            heap->regions[0].tlab_reserved_start =
                heap->regions[0].start;
            heap->regions[0].tlab_published_top =
                heap->regions[0].start;
            heap->regions[0].tlab_reserved_end =
                heap->regions[0].start + 512u;
            heap->regions[0].tlab_state = NGGC_TLAB_ACTIVE;
            thread->tlab_region_index = 0u;
            thread->tlab_lease_epoch = 1u;
            thread->tlab_reserved_start = heap->regions[0].start;
            thread->tlab_published_top = heap->regions[0].start;
            thread->tlab_reserved_end = heap->regions[0].start + 512u;
            thread->tlab_state = NGGC_TLAB_ACTIVE;

            heap->regions[1].start = overlap_thread.tlab_reserved_start;
            heap->regions[1].end = overlap_thread.tlab_reserved_end;
            heap->regions[1].tlab_owner = &overlap_thread;
            heap->regions[1].tlab_lease_epoch =
                overlap_thread.tlab_lease_epoch;
            heap->regions[1].tlab_reserved_start =
                overlap_thread.tlab_reserved_start;
            heap->regions[1].tlab_published_top =
                overlap_thread.tlab_published_top;
            heap->regions[1].tlab_reserved_end =
                overlap_thread.tlab_reserved_end;
            heap->regions[1].tlab_state = NGGC_TLAB_ACTIVE;

            result = nggc_verify_tlabs_locked(heap);
            heap->regions[1] = saved_second_region;
            heap->regions[0] = saved_region;
            nggc_test_tlab_thread_snapshot_restore(thread,
                                                   &saved_thread);
            heap->threads = saved_threads;
            heap->free_list = saved_free_list;
            heap->safepoint_pending_threads = saved_pending;
            atomic_store_explicit(&heap->tlab_fast_gate,
                                  saved_fast_gate,
                                  memory_order_release);
            nggc_unlock(heap);
            return result;
        case NGGC_TEST_TLAB_FAULT_FAST_TOP_BOUNDS:
            if (thread->tlab_state != NGGC_TLAB_ACTIVE ||
                thread->tlab_reserved_end >
                    UINTPTR_MAX - NGGC_OBJECT_ALIGNMENT) {
                atomic_store_explicit(&heap->tlab_fast_gate,
                                      saved_fast_gate,
                                      memory_order_release);
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            atomic_store_explicit(
                &thread->tlab_fast_top,
                thread->tlab_reserved_end + NGGC_OBJECT_ALIGNMENT,
                memory_order_release);
            break;
        case NGGC_TEST_TLAB_FAULT_FAST_INFLIGHT:
            atomic_store_explicit(&thread->tlab_fast_inflight, 1u,
                                  memory_order_release);
            result = nggc_verify_tlabs_locked(heap);
            atomic_store_explicit(&thread->tlab_fast_inflight,
                                  saved_thread.tlab_fast_inflight,
                                  memory_order_release);
            atomic_store_explicit(&heap->tlab_fast_gate,
                                  saved_fast_gate,
                                  memory_order_release);
            nggc_unlock(heap);
            return result;
        case NGGC_TEST_TLAB_FAULT_OBJECT_ID_LEASE:
            if (thread->tlab_state != NGGC_TLAB_ACTIVE) {
                atomic_store_explicit(&heap->tlab_fast_gate,
                                      saved_fast_gate,
                                      memory_order_release);
                nggc_unlock(heap);
                return NGGC_ERR_INVALID_ARGUMENT;
            }
            thread->tlab_object_id_next = 0u;
            thread->tlab_object_id_remaining = 1u;
            break;
        case NGGC_TEST_TLAB_FAULT_FAST_GATE:
            atomic_store_explicit(&heap->tlab_fast_gate, 2u,
                                  memory_order_release);
            break;
        case NGGC_TEST_TLAB_FAULT_ADAPTIVE_TARGET:
            thread->tlab_target_size = 0u;
            break;
        default:
            atomic_store_explicit(&heap->tlab_fast_gate,
                                  saved_fast_gate,
                                  memory_order_release);
            nggc_unlock(heap);
            return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_unlock(heap);
    result = nggc_check(heap);
    nggc_lock(heap);
    heap->regions[0] = saved_region;
    nggc_test_tlab_thread_snapshot_restore(thread, &saved_thread);
    heap->threads = saved_threads;
    heap->free_list = saved_free_list;
    heap->safepoint_pending_threads = saved_pending;
    atomic_store_explicit(&heap->tlab_fast_gate, saved_fast_gate,
                          memory_order_release);
    nggc_unlock(heap);
    return result;
}

nggc_result nggc_internal_test_tlab_reserved_span_check(
    nggc_heap *heap, nggc_thread *thread) {
    nggc_region_meta saved_region;
    nggc_test_tlab_thread_snapshot saved_thread;
    unsigned char saved_header[NGGC_OBJECT_HEADER_BYTES];
    uintptr_t saved_arena_cursor;
    size_t saved_active_regions;
    uint32_t saved_fast_gate;
    size_t span_size = (size_t)1024u;
    nggc_object_header *header;
    nggc_result result;
    nggc_stats stats;
    if (!nggc_heap_live(heap) || !nggc_thread_live(thread, heap) ||
        heap->region_count == 0u) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    nggc_lock(heap);
    saved_fast_gate = atomic_load_explicit(
        &heap->tlab_fast_gate, memory_order_acquire);
    if (nggc_tlab_fast_close_and_flush_locked(heap) != NGGC_OK) {
        nggc_unlock(heap);
        return NGGC_ERR_CORRUPTION;
    }
    if (heap->arena_cursor != (uintptr_t)heap->gc_arena_base ||
        span_size > heap->region_size ||
        span_size < NGGC_OBJECT_HEADER_BYTES) {
        atomic_store_explicit(&heap->tlab_fast_gate, saved_fast_gate,
                              memory_order_release);
        nggc_unlock(heap);
        return NGGC_ERR_BUSY;
    }
    saved_region = heap->regions[0];
    nggc_test_tlab_thread_snapshot_take(thread, &saved_thread);
    saved_arena_cursor = heap->arena_cursor;
    saved_active_regions = heap->active_region_count;
    header = (nggc_object_header *)heap->gc_arena_base;
    memcpy(saved_header, header, sizeof(saved_header));
    memset(header, 0, sizeof(*header));
    header->magic = NGGC_OBJECT_MAGIC;
    atomic_init(&header->state, NGGC_OBJECT_TLAB_RESERVED);
    nggc_forwarding_init(header);
    nggc_payload_lock_init(header);
    header->type_id = NGGC_TYPE_NONE;
    header->alloc_size = span_size;
    heap->arena_cursor = (uintptr_t)header + span_size;
    heap->regions[0].allocation_cursor = heap->arena_cursor;
    heap->regions[0].committed_bytes = span_size;
    heap->regions[0].reserved_bytes = span_size;
    heap->regions[0].state = NGGC_REGION_STATE_ACTIVE;
    heap->regions[0].generation = NGGC_REGION_GENERATION_YOUNG;
    heap->regions[0].tlab_owner = thread;
    heap->regions[0].tlab_lease_epoch = 1u;
    heap->regions[0].tlab_reserved_start = (uintptr_t)header;
    heap->regions[0].tlab_published_top = (uintptr_t)header;
    heap->regions[0].tlab_reserved_end = heap->arena_cursor;
    heap->regions[0].tlab_state = NGGC_TLAB_ACTIVE;
    thread->tlab_region_index = 0u;
    thread->tlab_lease_epoch = 1u;
    thread->tlab_reserved_start = (uintptr_t)header;
    thread->tlab_published_top = (uintptr_t)header;
    thread->tlab_reserved_end = heap->arena_cursor;
    thread->tlab_state = NGGC_TLAB_ACTIVE;
    atomic_store_explicit(&thread->tlab_fast_top, (uintptr_t)header,
                          memory_order_release);
    heap->active_region_count = 1u;
    nggc_unlock(heap);

    result = nggc_check(heap);
    if (result == NGGC_OK) {
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        result = nggc_get_stats(heap, &stats);
        if (result == NGGC_OK &&
            (stats.active_tlab_count != 1u ||
             stats.young_region_count != 1u ||
             stats.tlab_reserved_block_count != 1u ||
             stats.tlab_reserved_bytes != span_size)) {
            result = NGGC_ERR_CORRUPTION;
        }
    }

    nggc_lock(heap);
    {
        nggc_result close_result =
            nggc_tlab_fast_close_and_flush_locked(heap);
        if (result == NGGC_OK && close_result != NGGC_OK) {
            result = close_result;
        }
    }
    memcpy(header, saved_header, sizeof(saved_header));
    heap->regions[0] = saved_region;
    nggc_test_tlab_thread_snapshot_restore(thread, &saved_thread);
    heap->arena_cursor = saved_arena_cursor;
    heap->active_region_count = saved_active_regions;
    atomic_store_explicit(&heap->tlab_fast_gate, saved_fast_gate,
                          memory_order_release);
    nggc_unlock(heap);
    return result;
}
#endif

nggc_result nggc_get_stats(nggc_heap *heap, nggc_stats *out_stats) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    nggc_stats temp;
    nggc_backend_stats backend_stats;
    nggc_result result;
    size_t pending_classification_records = 0u;
    size_t pending_classification_cross_region = 0u;
    size_t pending_classification_old_to_young = 0u;

    if (!nggc_heap_live(heap) || !out_stats ||
        out_stats->size < sizeof(*out_stats)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }

    memset(&temp, 0, sizeof(temp));
    temp.size = sizeof(temp);
    temp.abi_major = NGGC_ABI_MAJOR;
    temp.abi_minor = NGGC_ABI_MINOR;

    nggc_lock(heap);
    result = nggc_remset_fold_all_deferred_outcomes_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_tlab_fast_close_and_flush_locked(heap);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    result = nggc_pin_lease_verify_locked(heap, 0, 0);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    temp.flags = heap->flags;
    temp.soft_reference_policy = heap->soft_reference_policy;
    temp.soft_reference_retain_percent =
        heap->soft_reference_retain_percent;
    temp.gc_trigger_policy = heap->gc_trigger_policy;
    temp.gc_trigger_last_action = heap->gc_trigger_last_action;
    temp.gc_trigger_last_reason = heap->gc_trigger_last_reason;
    temp.gc_trigger_pending = heap->gc_trigger_pending;
    temp.gc_trigger_soft_limit = heap->gc_trigger_soft_limit;
    temp.gc_trigger_quantum = heap->gc_trigger_quantum;
    temp.gc_trigger_latency_target_ns = heap->gc_trigger_latency_target_ns;
    temp.gc_trigger_evaluation_count = heap->gc_trigger_evaluation_count;
    temp.gc_trigger_request_count = heap->gc_trigger_request_count;
    temp.gc_trigger_collection_count = heap->gc_trigger_collection_count;
    temp.gc_trigger_busy_count = heap->gc_trigger_busy_count;
    temp.gc_trigger_noop_count = heap->gc_trigger_noop_count;
    temp.gc_trigger_failure_count = heap->gc_trigger_failure_count;
    temp.gc_trigger_last_used = heap->gc_trigger_last_used;
    temp.gc_trigger_last_request_bytes =
        heap->gc_trigger_last_request_bytes;
    temp.gc_trigger_last_projected_used =
        heap->gc_trigger_last_projected_used;
    temp.gc_trigger_last_pause_ns = heap->gc_trigger_last_pause_ns;
    temp.gc_trigger_pause_ewma_ns = heap->gc_trigger_pause_ewma_ns;
    temp.gc_trigger_pause_sample_count = heap->gc_trigger_pause_sample_count;
    temp.gc_trigger_clock_failure_count =
        heap->gc_trigger_clock_failure_count;
    temp.allocated_bytes = heap->allocated_bytes;
    temp.gc_arena_size = heap->gc_arena_size;
    temp.gc_arena_used = (size_t)(heap->arena_cursor -
                                  (uintptr_t)heap->gc_arena_base);
    temp.region_size = heap->region_size;
    temp.tlab_size = heap->tlab_size;
    temp.tlab_sizing_policy = heap->tlab_sizing_policy;
    temp.tlab_min_size = heap->tlab_min_size;
    temp.tlab_max_size = heap->tlab_max_size;
    temp.young_tenuring_threshold = heap->young_tenuring_threshold;
    temp.region_count = heap->region_count;
    temp.active_region_count = heap->active_region_count;
    temp.decommitted_region_count = heap->decommitted_region_count;
    temp.region_decommitted_bytes = heap->region_decommitted_bytes;
    temp.region_padding_created_bytes =
        heap->region_padding_created_bytes;
    temp.large_free_bin_capacity = NGGC_LARGE_FREE_BIN_COUNT;
    for (size_t i = 0u; i < NGGC_LARGE_FREE_BIN_COUNT; ++i) {
        int has_head = heap->large_free_bins[i] != 0;
        int has_mask =
            (heap->large_free_bin_mask &
             ((uint32_t)1u << i)) != 0u;
        if (has_head != has_mask) {
            nggc_unlock(heap);
            return NGGC_ERR_CORRUPTION;
        }
        if (has_head) {
            ++temp.large_free_active_bin_count;
        }
        has_head = heap->decommitted_large_free_bins[i] != 0u;
        has_mask =
            (heap->decommitted_large_free_bin_mask &
             ((uint32_t)1u << i)) != 0u;
        if (has_head != has_mask) {
            nggc_unlock(heap);
            return NGGC_ERR_CORRUPTION;
        }
        if (has_head) {
            ++temp.decommitted_large_free_active_bin_count;
        }
    }
    temp.interior_decommitted_run_count =
        heap->interior_decommitted_run_count;
    temp.interior_decommitted_region_count =
        heap->interior_decommitted_region_count;
    temp.interior_decommitted_bytes =
        heap->interior_decommitted_bytes;
    temp.interior_decommit_min_regions =
        heap->interior_decommit_min_regions;
    temp.interior_decommit_policy = heap->interior_decommit_policy;
    temp.interior_decommit_smart_last_action =
        heap->interior_decommit_smart_last_action;
    temp.interior_decommit_smart_cooldown_remaining =
        heap->interior_decommit_smart_cooldown_remaining;
    temp.interior_decommit_smart_committed_pressure_basis_points =
        heap->interior_decommit_smart_committed_pressure_basis_points;
    temp.interior_decommit_smart_large_free_pressure_basis_points =
        heap->interior_decommit_smart_large_free_pressure_basis_points;
    temp.interior_decommit_smart_pressure_source =
        heap->interior_decommit_smart_pressure_source;
    temp.interior_decommit_smart_external_pressure_valid =
        heap->interior_decommit_smart_external_pressure_valid;
    temp.interior_decommit_smart_external_pressure_basis_points =
        heap->interior_decommit_smart_external_pressure_basis_points;
    temp.interior_decommit_smart_slow_vm_observed =
        heap->interior_decommit_smart_slow_vm_observed;
    temp.interior_decommit_smart_resident_bytes =
        heap->interior_decommit_smart_resident_bytes;
    temp.interior_decommit_smart_memory_limit_bytes =
        heap->interior_decommit_smart_memory_limit_bytes;
    temp.interior_decommit_slow_ns_per_region =
        heap->interior_decommit_slow_ns_per_region;
    temp.region_decommit_latency_ewma_ns_per_region =
        heap->region_decommit_latency_ewma_ns_per_region;
    temp.region_recommit_latency_ewma_ns_per_region =
        heap->region_recommit_latency_ewma_ns_per_region;
    for (size_t i = 0; i < heap->region_count; ++i) {
        uint32_t region_flags = heap->regions[i].flags;
        uint32_t humongous_role =
            heap->regions[i].humongous_role;
        if (!nggc_region_humongous_role_valid(humongous_role)) {
            nggc_unlock(heap);
            return NGGC_ERR_CORRUPTION;
        }
        if (humongous_role != NGGC_REGION_HUMONGOUS_NONE) {
            ++temp.humongous_region_count;
        }
        if (humongous_role == NGGC_REGION_HUMONGOUS_START) {
            ++temp.humongous_run_count;
            if (heap->regions[i].decommitted) {
                uint32_t run_length;
                if (!nggc_interior_decommitted_run_locked(
                        heap, i, &run_length, 0)) {
                    nggc_unlock(heap);
                    return NGGC_ERR_CORRUPTION;
                }
                if (run_length >
                    temp.largest_large_free_run_regions) {
                    temp.largest_large_free_run_regions = run_length;
                }
            } else if (heap->regions[i].large_object_count != 0u &&
                heap->regions[i].live_bytes != 0u) {
                ++temp.humongous_live_run_count;
            } else if (heap->regions[i].large_object_count == 0u &&
                       heap->regions[i].live_bytes == 0u &&
                       heap->regions[i].free_bytes ==
                           heap->regions[i].committed_bytes &&
                       heap->regions[i].committed_bytes != 0u) {
                ++temp.humongous_free_run_count;
            } else {
                nggc_unlock(heap);
                return NGGC_ERR_CORRUPTION;
            }
        }
        if (heap->regions[i].generation ==
            NGGC_REGION_GENERATION_YOUNG) {
            ++temp.young_region_count;
        } else if (heap->regions[i].generation ==
                   NGGC_REGION_GENERATION_OLD) {
            ++temp.old_region_count;
        } else if (heap->regions[i].generation ==
                   NGGC_REGION_GENERATION_SPECIAL) {
            ++temp.special_region_count;
        }
        if ((region_flags & NGGC_REGION_FLAG_HAS_LIVE) != 0u) {
            ++temp.live_region_count;
        }
        if ((region_flags & NGGC_REGION_FLAG_HAS_FREE) != 0u) {
            ++temp.free_region_count;
        }
        if ((region_flags & NGGC_REGION_FLAG_HAS_LARGE) != 0u) {
            ++temp.large_region_count;
        }
        if ((region_flags & NGGC_REGION_FLAG_HAS_PINNED) != 0u) {
            ++temp.pinned_region_count;
        }
        if ((region_flags & NGGC_REGION_FLAG_HAS_NON_MOVABLE) != 0u) {
            ++temp.non_movable_region_count;
        }
        if ((region_flags & NGGC_REGION_FLAG_HAS_QUARANTINE) != 0u) {
            ++temp.quarantined_region_count;
        }
        if (heap->regions[i].in_collection_set != 0u) {
            ++temp.collection_set_region_count;
        }
        if (heap->regions[i].allocation_owner != 0u) {
            ++temp.allocator_owned_region_count;
        }
        if (heap->regions[i].tlab_state != NGGC_TLAB_INACTIVE) {
            ++temp.active_tlab_count;
        }
        temp.tlab_reserved_bytes += heap->regions[i].reserved_bytes;
    }
    if (temp.humongous_run_count !=
        temp.humongous_live_run_count +
            temp.humongous_free_run_count +
            temp.interior_decommitted_run_count) {
        nggc_unlock(heap);
        return NGGC_ERR_CORRUPTION;
    }
    {
        uintptr_t cursor = (uintptr_t)heap->gc_arena_base;
        while (cursor < heap->arena_cursor) {
            nggc_block_view block;
            size_t region_index;
            if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
                nggc_unlock(heap);
                return NGGC_ERR_CORRUPTION;
            }
            region_index = (size_t)((cursor -
                                     (uintptr_t)heap->gc_arena_base) /
                                    heap->region_size);
            if (block.state == NGGC_OBJECT_LIVE &&
                region_index < heap->region_count) {
                if (block.header->generation ==
                    NGGC_REGION_GENERATION_YOUNG) {
                    ++temp.young_object_count;
                    if (block.header->young_age != 0u) {
                        ++temp.young_survivor_object_count;
                        if (block.header->young_age >=
                            heap->young_tenuring_threshold) {
                            ++temp.young_tenuring_candidate_count;
                        }
                    }
                } else if (block.header->generation ==
                           NGGC_REGION_GENERATION_OLD) {
                    ++temp.old_object_count;
                } else {
                    nggc_unlock(heap);
                    return NGGC_ERR_CORRUPTION;
                }
            } else if (block.state == NGGC_OBJECT_FREE &&
                       (block.header->flags &
                        NGGC_OBJECT_FLAG_LARGE) != 0u) {
                size_t run_regions;
                if (heap->region_size == 0u ||
                    (block.header->alloc_size %
                     heap->region_size) != 0u) {
                    nggc_unlock(heap);
                    return NGGC_ERR_CORRUPTION;
                }
                run_regions =
                    block.header->alloc_size / heap->region_size;
                if (run_regions >
                    temp.largest_large_free_run_regions) {
                    temp.largest_large_free_run_regions = run_regions;
                }
            }
            cursor = block.end;
        }
    }
    if (temp.tlab_reserved_bytes != 0u) {
        uintptr_t cursor = (uintptr_t)heap->gc_arena_base;
        while (cursor < heap->arena_cursor) {
            nggc_block_view block;
            if (nggc_block_view_locked(heap, cursor, &block) != NGGC_OK) {
                nggc_unlock(heap);
                return NGGC_ERR_CORRUPTION;
            }
            if (block.state == NGGC_OBJECT_TLAB_RESERVED) {
                ++temp.tlab_reserved_block_count;
            }
            cursor = block.end;
        }
    }
    temp.live_object_count = heap->live_object_count;
    temp.live_payload_bytes = heap->live_payload_bytes;
    temp.large_object_count = heap->large_object_count;
    temp.large_free_object_count = heap->large_free_object_count;
    temp.large_free_bytes = heap->large_free_bytes;
    temp.pinnable_object_count = heap->pinnable_object_count;
    temp.non_movable_object_count = heap->non_movable_object_count;
    temp.pinned_object_count = heap->pinned_object_count;
    temp.pin_lease_count = heap->pin_lease_count;
    temp.pin_acquire_count = heap->pin_acquire_count;
    temp.pin_release_count = heap->pin_release_count;
    temp.pin_busy_count = heap->pin_busy_count;
    temp.pin_unpin_error_count = heap->pin_unpin_error_count;
    temp.pin_lease_record_count = heap->pin_lease_record_count;
    temp.pin_owner_conflict_count = heap->pin_owner_conflict_count;
    temp.pin_thread_exit_busy_count = heap->pin_thread_exit_busy_count;
    temp.free_object_count = heap->free_object_count;
    temp.free_bytes = heap->free_bytes;
    temp.root_count = heap->root_count;
    for (nggc_thread *thread = heap->threads; thread;
         thread = thread->next) {
        if (thread->root_count > SIZE_MAX - temp.root_count) {
            nggc_unlock(heap);
            return NGGC_ERR_CORRUPTION;
        }
        temp.root_count += thread->root_count;
        if (thread->mutator_assist_debt_bytes != 0u) {
            if (temp.mutator_assist_pending_thread_count == SIZE_MAX) {
                nggc_unlock(heap);
                return NGGC_ERR_CORRUPTION;
            }
            ++temp.mutator_assist_pending_thread_count;
        }
        if (thread->mutator_assist_debt_bytes >
            temp.mutator_assist_max_thread_debt_bytes) {
            temp.mutator_assist_max_thread_debt_bytes =
                thread->mutator_assist_debt_bytes;
        }
        if (temp.tlab_target_min_size == 0u ||
            thread->tlab_target_size < temp.tlab_target_min_size) {
            temp.tlab_target_min_size = thread->tlab_target_size;
        }
        if (thread->tlab_target_size > temp.tlab_target_max_size) {
            temp.tlab_target_max_size = thread->tlab_target_size;
        }
    }
    temp.card_size = heap->card_size;
    temp.card_count = heap->card_count;
    temp.remset_card_atomic_lock_free =
        heap->remset_card_atomic_lock_free;
    temp.dirty_card_count = heap->dirty_card_count;
    temp.remset_max_dirty_cards = heap->remset_max_dirty_cards;
    temp.remset_mode = heap->remset_mode;
    temp.remset_buffer_capacity = heap->remset_buffer_capacity;
    result = nggc_verify_remset_buffers_locked(
        heap, &temp.remset_buffered_card_count,
        &temp.remset_buffered_thread_count,
        &temp.remset_buffer_inflight_card_count,
        &temp.remset_buffer_inflight_thread_count, 0,
        &temp.remset_classification_pending_thread_count,
        &pending_classification_records,
        &pending_classification_cross_region,
        &pending_classification_old_to_young);
    if (result != NGGC_OK) {
        nggc_unlock(heap);
        nggc_set_error(heap, result);
        return result;
    }
    temp.registered_thread_count = heap->registered_thread_count;
    temp.registered_type_count = heap->registered_type_count;
    temp.type_table_frozen = heap->type_table_frozen;
    temp.allocation_count = heap->allocation_count;
    temp.large_allocation_count = heap->large_allocation_count;
    temp.large_reuse_count = heap->large_reuse_count;
    temp.large_free_search_count = heap->large_free_search_count;
    temp.large_free_probe_count = heap->large_free_probe_count;
    temp.large_free_committed_probe_count =
        heap->large_free_committed_probe_count;
    temp.large_free_decommitted_probe_count =
        heap->large_free_decommitted_probe_count;
    temp.large_free_size_reject_count =
        heap->large_free_size_reject_count;
    temp.large_free_placement_reject_count =
        heap->large_free_placement_reject_count;
    temp.interior_run_decommit_count =
        heap->interior_run_decommit_count;
    temp.interior_run_recommit_count =
        heap->interior_run_recommit_count;
    temp.interior_run_tail_collapse_count =
        heap->interior_run_tail_collapse_count;
    temp.interior_run_tail_collapsed_region_count =
        heap->interior_run_tail_collapsed_region_count;
    temp.interior_run_tail_collapsed_bytes =
        heap->interior_run_tail_collapsed_bytes;
    temp.interior_decommit_smart_evaluation_count =
        heap->interior_decommit_smart_evaluation_count;
    temp.interior_decommit_smart_raise_count =
        heap->interior_decommit_smart_raise_count;
    temp.interior_decommit_smart_lower_count =
        heap->interior_decommit_smart_lower_count;
    temp.interior_decommit_smart_cooldown_hold_count =
        heap->interior_decommit_smart_cooldown_hold_count;
    temp.interior_decommit_smart_last_recommit_delta =
        heap->interior_decommit_smart_last_recommit_delta;
    temp.interior_decommit_smart_last_large_allocation_delta =
        heap->interior_decommit_smart_last_large_allocation_delta;
    temp.interior_decommit_smart_last_large_reuse_delta =
        heap->interior_decommit_smart_last_large_reuse_delta;
    temp.interior_decommit_smart_last_decommit_latency_sample_delta =
        heap->interior_decommit_smart_last_decommit_latency_sample_delta;
    temp.interior_decommit_smart_last_recommit_latency_sample_delta =
        heap->interior_decommit_smart_last_recommit_latency_sample_delta;
    temp.interior_decommit_smart_external_sample_count =
        heap->interior_decommit_smart_external_sample_count;
    temp.interior_decommit_smart_external_failure_count =
        heap->interior_decommit_smart_external_failure_count;
    temp.region_decommit_latency_sample_count =
        heap->region_decommit_latency_sample_count;
    temp.region_recommit_latency_sample_count =
        heap->region_recommit_latency_sample_count;
    temp.region_vm_clock_failure_count =
        heap->region_vm_clock_failure_count;
    temp.tlab_refill_count = heap->tlab_refill_count;
    temp.tlab_retire_count = heap->tlab_retire_count;
    temp.tlab_allocation_count = heap->tlab_allocation_count;
    temp.tlab_refill_bytes = heap->tlab_refill_bytes;
    temp.tlab_retired_unused_bytes = heap->tlab_retired_unused_bytes;
    temp.tlab_fallback_allocation_count =
        heap->tlab_fallback_allocation_count;
    temp.tlab_fast_allocation_count = heap->tlab_fast_allocation_count;
    temp.tlab_fast_slow_path_count = heap->tlab_fast_slow_path_count;
    temp.tlab_fast_flush_count = heap->tlab_fast_flush_count;
    temp.tlab_fast_flushed_object_count =
        heap->tlab_fast_flushed_object_count;
    temp.tlab_fast_gate_close_count = heap->tlab_fast_gate_close_count;
    temp.tlab_fast_wrong_owner_count =
        heap->tlab_fast_wrong_owner_count;
    temp.tlab_sizing_sample_count = heap->tlab_sizing_sample_count;
    temp.tlab_sizing_grow_count = heap->tlab_sizing_grow_count;
    temp.tlab_sizing_shrink_count = heap->tlab_sizing_shrink_count;
    temp.tlab_sizing_hold_count = heap->tlab_sizing_hold_count;
    temp.region_padding_block_count = heap->region_padding_block_count;
    temp.allocator_lease_epoch = heap->allocator_lease_epoch;
    temp.allocator_lease_acquire_count =
        heap->allocator_lease_acquire_count;
    temp.allocator_lease_revoke_count =
        heap->allocator_lease_revoke_count;
    temp.region_decommit_count = heap->region_decommit_count;
    temp.region_recommit_count = heap->region_recommit_count;
    temp.region_decommit_failure_count =
        heap->region_decommit_failure_count;
    temp.region_recommit_failure_count =
        heap->region_recommit_failure_count;
    temp.mutator_assist_charged_bytes =
        heap->mutator_assist_charged_bytes;
    temp.mutator_assist_repaid_bytes = heap->mutator_assist_repaid_bytes;
    temp.mutator_assist_debt_bytes = heap->mutator_assist_debt_bytes;
    temp.mutator_assist_repay_count = heap->mutator_assist_repay_count;
    temp.mutator_assist_packet_count = heap->mutator_assist_packet_count;
    temp.mutator_assist_cycle_count = heap->mutator_assist_cycle_count;
    temp.mutator_assist_progress_count =
        heap->mutator_assist_progress_count;
    temp.mutator_assist_no_progress_count =
        heap->mutator_assist_no_progress_count;
    temp.mutator_assist_mark_items_processed =
        heap->mutator_assist_mark_items_processed;
    temp.mutator_assist_sweep_bytes_processed =
        heap->mutator_assist_sweep_bytes_processed;
    temp.mutator_assist_mark_backlog_items =
        heap->mutator_assist_mark_stack.count;
    temp.mutator_assist_state = heap->mutator_assist_state;
    temp.mutator_assist_current_budget_items =
        heap->mutator_assist_current_budget_items;
    temp.mutator_assist_current_budget_bytes =
        heap->mutator_assist_current_budget_bytes;
    temp.mutator_assist_fairness_cap_count =
        heap->mutator_assist_fairness_cap_count;
    temp.mutator_assist_backoff_count = heap->mutator_assist_backoff_count;
    temp.mutator_assist_dynamic_budget_count =
        heap->mutator_assist_dynamic_budget_count;
    temp.mutator_assist_thread_packet_count =
        heap->mutator_assist_thread_packet_count;
    temp.mutator_assist_thread_progress_count =
        heap->mutator_assist_thread_progress_count;
    temp.mutator_assist_thread_no_progress_count =
        heap->mutator_assist_thread_no_progress_count;
    temp.collection_count = heap->collection_count;
    temp.young_collection_count = heap->young_collection_count;
    temp.young_reclaimed_object_count =
        heap->young_reclaimed_object_count;
    temp.young_mark_old_object_count =
        heap->young_mark_old_object_count;
    temp.young_mark_dirty_slot_count =
        heap->young_mark_dirty_slot_count;
    temp.young_evacuation_preflight_count =
        heap->young_evacuation_preflight_count;
    temp.young_evacuation_capacity_fallback_count =
        heap->young_evacuation_capacity_fallback_count;
    temp.young_evacuation_survivor_bytes =
        heap->young_evacuation_survivor_bytes;
    temp.young_evacuation_promotion_bytes =
        heap->young_evacuation_promotion_bytes;
    temp.young_evacuated_object_count =
        heap->young_evacuated_object_count;
    temp.young_promoted_object_count =
        heap->young_promoted_object_count;
    temp.young_in_place_promoted_object_count =
        heap->young_in_place_promoted_object_count;
    temp.young_evacuation_emergency_completion_count =
        heap->young_evacuation_emergency_completion_count;
    temp.young_evacuation_emergency_failure_count =
        heap->young_evacuation_emergency_failure_count;
    temp.evacuation_epoch = heap->evacuation_epoch;
    temp.forwarding_cas_success_count =
        heap->forwarding_cas_success_count;
    temp.forwarding_cas_failure_count =
        heap->forwarding_cas_failure_count;
    temp.evacuation_worker_epoch = heap->evacuation_worker_epoch;
    temp.evacuation_workers_active = atomic_load_explicit(
        &heap->evacuation_workers_active, memory_order_acquire);
    temp.evacuation_workers_peak_active = atomic_load_explicit(
        &heap->evacuation_workers_peak_active, memory_order_acquire);
    temp.evacuation_workers_quiescent =
        heap->evacuation_workers_quiescent;
    temp.from_space_reclaim_pending =
        heap->from_space_reclaim_pending;
    temp.from_space_quarantine_epoch =
        heap->from_space_quarantine_epoch;
    temp.from_space_quarantine_active =
        heap->from_space_quarantine_active;
    temp.from_space_quarantine_release_epoch =
        heap->from_space_quarantine_release_epoch;
    temp.from_space_quarantine_object_count =
        heap->from_space_quarantine_object_count;
    temp.from_space_quarantine_bytes =
        heap->from_space_quarantine_bytes;
    temp.from_space_quarantine_cycle_count =
        heap->from_space_quarantine_cycle_count;
    temp.from_space_delayed_reclaim_count =
        heap->from_space_delayed_reclaim_count;
    temp.from_space_delayed_reclaimed_object_count =
        heap->from_space_delayed_reclaimed_object_count;
    temp.from_space_delayed_reclaimed_bytes =
        heap->from_space_delayed_reclaimed_bytes;
    temp.evacuation_parallel_cycle_count =
        heap->evacuation_parallel_cycle_count;
    temp.evacuation_worker_pool_size =
        heap->evacuation_worker_pool_size;
    temp.evacuation_worker_created_count =
        heap->evacuation_worker_created_count;
    temp.evacuation_worker_create_failure_count =
        heap->evacuation_worker_create_failure_count;
    temp.evacuation_worker_dispatch_count =
        heap->evacuation_worker_dispatch_count;
    temp.evacuation_worker_fallback_count =
        heap->evacuation_worker_fallback_count;
    temp.evacuation_worker_task_count =
        heap->evacuation_worker_task_count;
    temp.evacuation_loser_reclaimed_count =
        heap->evacuation_loser_reclaimed_count;
    temp.remap_worker_epoch = heap->remap_worker_epoch;
    temp.remap_workers_active = atomic_load_explicit(
        &heap->remap_workers_active, memory_order_acquire);
    temp.remap_workers_peak_active = atomic_load_explicit(
        &heap->remap_workers_peak_active, memory_order_acquire);
    temp.remap_workers_quiescent = heap->remap_workers_quiescent;
    temp.remap_parallel_cycle_count = heap->remap_parallel_cycle_count;
    temp.remap_worker_dispatch_count = heap->remap_worker_dispatch_count;
    temp.remap_worker_fallback_count = heap->remap_worker_fallback_count;
    temp.remap_worker_block_count = heap->remap_worker_block_count;
    temp.remap_scanned_slot_count = heap->remap_scanned_slot_count;
    temp.remap_rewritten_slot_count = heap->remap_rewritten_slot_count;
    temp.reference_process_count = heap->reference_process_count;
    temp.reference_process_packet_count =
        heap->reference_process_packet_count;
    temp.reference_process_items_processed =
        heap->reference_process_items_processed;
    temp.reference_process_no_progress_count =
        heap->reference_process_no_progress_count;
    temp.reference_process_pending_items =
        nggc_reference_process_pending_items_locked(heap);
    temp.reference_discovered_state = heap->reference_discovered_state;
    temp.reference_discovered_epoch = heap->reference_discovered_epoch;
    temp.reference_discovered_weak_count =
        heap->reference_discovered_weak.count;
    temp.reference_discovered_soft_count =
        heap->reference_discovered_soft.count;
    temp.reference_discovered_phantom_count =
        heap->reference_discovered_phantom.count;
    temp.soft_reference_discovered_count =
        heap->soft_reference_discovered_count;
    temp.soft_reference_retained_count =
        heap->soft_reference_retained_count;
    temp.soft_reference_cleared_count =
        heap->soft_reference_cleared_count;
    temp.weak_reference_discovered_count =
        heap->weak_reference_discovered_count;
    temp.weak_reference_cleared_count =
        heap->weak_reference_cleared_count;
    temp.phantom_reference_discovered_count =
        heap->phantom_reference_discovered_count;
    temp.phantom_reference_enqueued_count =
        heap->phantom_reference_enqueued_count;
    temp.phantom_queue_promoted_count =
        heap->phantom_queue_promoted_count;
    temp.phantom_reaper_watched_queue_count =
        heap->phantom_reaper_count;
    temp.phantom_reaper_dropped_queue_count =
        heap->phantom_reaper_dropped_queue_count;
    temp.reclaimed_object_count = heap->reclaimed_object_count;
    temp.load_barrier_count = heap->load_barrier_count;
    temp.store_barrier_count = heap->store_barrier_count;
    temp.remset_record_count = nggc_saturating_add_u64_size(
        heap->remset_record_count, pending_classification_records);
    temp.remset_cross_region_record_count =
        nggc_saturating_add_u64_size(
            heap->remset_cross_region_record_count,
            pending_classification_cross_region);
    temp.remset_old_to_young_record_count =
        nggc_saturating_add_u64_size(
            heap->remset_old_to_young_record_count,
            pending_classification_old_to_young);
    temp.remset_overflow_count = heap->remset_overflow_count;
    temp.remset_rebuild_count = heap->remset_rebuild_count;
    temp.remset_rebuild_cross_region_edge_count =
        heap->remset_rebuild_cross_region_edge_count;
    temp.remset_rebuild_old_to_young_edge_count =
        heap->remset_rebuild_old_to_young_edge_count;
    temp.remset_buffer_record_count = nggc_saturating_add_u64_size(
        heap->remset_buffer_record_count,
        pending_classification_records);
    temp.remset_buffer_deduplicated_count =
        heap->remset_buffer_deduplicated_count;
    temp.remset_buffer_flush_count =
        heap->remset_buffer_flush_count;
    temp.remset_buffer_flushed_card_count =
        heap->remset_buffer_flushed_card_count;
    temp.remset_buffer_overflow_flush_count =
        heap->remset_buffer_overflow_flush_count;
    temp.remset_buffer_deferred_publish_count =
        heap->remset_buffer_deferred_publish_count;
    temp.remset_buffer_deferred_deduplicated_count =
        heap->remset_buffer_deferred_deduplicated_count;
    temp.remset_buffer_deferred_covered_count =
        heap->remset_buffer_deferred_covered_count;
    temp.remset_buffer_deferred_classification_count =
        nggc_saturating_add_u64_size(
            heap->remset_buffer_deferred_classification_count,
            pending_classification_records);
    temp.remset_direct_record_count =
        heap->remset_direct_record_count;
    temp.remset_barrier_tss_hit_count =
        heap->remset_barrier_tss_hit_count;
    temp.remset_barrier_tss_fallback_scan_count =
        heap->remset_barrier_tss_fallback_scan_count;
    temp.remset_barrier_tss_ambiguous_count =
        heap->remset_barrier_tss_ambiguous_count;
    temp.remset_barrier_tss_update_failure_count =
        heap->remset_barrier_tss_update_failure_count;
    temp.remset_barrier_no_owner_direct_count =
        heap->remset_barrier_no_owner_direct_count;
    temp.safepoint_epoch = heap->safepoint_epoch;
    temp.safepoint_request_count = heap->safepoint_request_count;
    temp.safepoint_poll_count = heap->safepoint_poll_count;
    temp.safepoint_ack_count = heap->safepoint_ack_count;
    temp.safepoint_wait_count = heap->safepoint_wait_count;
    temp.safepoint_release_count = heap->safepoint_release_count;
    temp.safepoint_release_epoch =
        atomic_load_explicit(&heap->safepoint_release_epoch,
                             memory_order_acquire);
    temp.safepoint_pending_threads = heap->safepoint_pending_threads;
    temp.safepoint_parked_threads = heap->safepoint_parked_threads;
    temp.safepoint_collector_owned_threads =
        heap->collect_safepoint_owner ? 1u : 0u;
    temp.safepoint_active = heap->safepoint_active;
    temp.gc_phase = heap->gc_phase;
    temp.read_barrier_active = heap->read_barrier_active;
    temp.phase_transition_count = heap->phase_transition_count;
    temp.read_barrier_active_load_count =
        heap->read_barrier_active_load_count;
    temp.mutator_concurrent_active = heap->mutator_concurrent_active;
    temp.mutator_concurrent_epoch = heap->mutator_concurrent_epoch;
    temp.mutator_concurrent_window_count =
        heap->mutator_concurrent_window_count;
    temp.mutator_concurrent_load_count =
        heap->mutator_concurrent_load_count;
    temp.mutator_concurrent_store_count =
        heap->mutator_concurrent_store_count;
    temp.mutator_concurrent_rejected_count =
        heap->mutator_concurrent_rejected_count;
    temp.mutator_concurrent_running_threads =
        heap->mutator_concurrent_running_threads;
    temp.mutator_concurrent_repark_pending_threads =
        heap->mutator_concurrent_repark_pending_threads;
    temp.mutator_concurrent_repark_requested =
        heap->mutator_concurrent_repark_requested;
    temp.mutator_concurrent_resume_count =
        heap->mutator_concurrent_resume_count;
    temp.mutator_concurrent_repark_count =
        heap->mutator_concurrent_repark_count;
    nggc_tlab_fast_maybe_open_locked(heap);
    nggc_unlock(heap);

    if (nggc_backend_get_stats(heap->backend, &backend_stats) == NGGC_OK) {
        temp.uhm_arena_size = backend_stats.arena_size;
        temp.uhm_committed_bytes = backend_stats.committed_bytes;
        temp.uhm_decommitted_bytes = backend_stats.decommitted_bytes;
        temp.uhm_mmap_live_bytes = backend_stats.mmap_live_bytes;
        temp.uhm_page_size = backend_stats.page_size;
        temp.uhm_allocation_granularity =
            backend_stats.allocation_granularity;
    }

    memcpy(out_stats, &temp, sizeof(temp));
    return NGGC_OK;
}

nggc_result nggc_get_last_error(nggc_heap *heap) {
    NGGC_SCAN_API_GUARD(NGGC_ERR_BUSY);
    return nggc_heap_live(heap)
               ? (nggc_result)atomic_load_explicit(&heap->last_error,
                                                   memory_order_relaxed)
               : NGGC_ERR_INVALID_ARGUMENT;
}
