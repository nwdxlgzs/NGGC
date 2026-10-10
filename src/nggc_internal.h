#ifndef NGGC_INTERNAL_H
#define NGGC_INTERNAL_H

#include "nggc/nggc.h"

#include <stddef.h>

void *nggc_internal_alloc_none(nggc_heap *heap, size_t payload_size);
void *nggc_internal_alloc_weak(nggc_heap *heap, void *referent);
void *nggc_internal_weak_get(nggc_heap *heap, void *weak_reference);
void *nggc_internal_alloc_soft(nggc_heap *heap, void *referent);
void *nggc_internal_soft_get(nggc_heap *heap, void *soft_reference);
void *nggc_internal_alloc_phantom_queue(nggc_heap *heap);
void *nggc_internal_alloc_phantom(nggc_heap *heap, void *referent,
                                  void *queue);
size_t nggc_internal_phantom_queue_count(nggc_heap *heap, void *queue);
void *nggc_internal_phantom_queue_pop(nggc_heap *heap, void *queue);
nggc_result nggc_internal_tlab_reserve(nggc_heap *heap,
                                        nggc_thread *thread,
                                        size_t requested_bytes);
nggc_result nggc_internal_tlab_retire(nggc_heap *heap,
                                       nggc_thread *thread);
void *nggc_internal_tlab_alloc_size(nggc_heap *heap,
                                     nggc_thread *thread,
                                     uint16_t type_id,
                                     size_t payload_size);
void *nggc_internal_thread_alloc_size(nggc_heap *heap,
                                       nggc_thread *thread,
                                       uint16_t type_id,
                                       size_t payload_size);

#if NGGC_ENABLE_VERIFY && defined(NGGC_ENABLE_TEST_HOOKS) && \
    NGGC_ENABLE_TEST_HOOKS
#define NGGC_TEST_REGISTRY_FAULT_ROOT_COUNT       1u
#define NGGC_TEST_REGISTRY_FAULT_ROOT_DUPLICATE   2u
#define NGGC_TEST_REGISTRY_FAULT_ROOT_UNALIGNED   3u
#define NGGC_TEST_REGISTRY_FAULT_THREAD_COUNT     4u
#define NGGC_TEST_REGISTRY_FAULT_THREAD_CYCLE     5u
#define NGGC_TEST_REGISTRY_FAULT_THREAD_MAGIC     6u
#define NGGC_TEST_REGISTRY_FAULT_THREAD_OWNER     7u
#define NGGC_TEST_REGISTRY_FAULT_THREAD_STATE     8u
#define NGGC_TEST_REGISTRY_FAULT_PENDING_COUNT    9u
#define NGGC_TEST_REGISTRY_FAULT_OBSERVED_EPOCH  10u
#define NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_COUNT      11u
#define NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_DUPLICATE  12u
#define NGGC_TEST_REGISTRY_FAULT_THREAD_ROOT_UNALIGNED  13u

nggc_result nggc_internal_test_registry_fault_check(nggc_heap *heap,
                                                     uint32_t fault);

#define NGGC_TEST_REFERENCE_FAULT_SOFT_REFERENT_MARK       101u
#define NGGC_TEST_REFERENCE_FAULT_WEAK_REFERENT_MARK       102u
#define NGGC_TEST_REFERENCE_FAULT_PHANTOM_REFERENT_MARK    103u
#define NGGC_TEST_REFERENCE_FAULT_PHANTOM_QUEUE_MARK       104u
#define NGGC_TEST_REFERENCE_FAULT_ENQUEUED_REFERENT        105u
#define NGGC_TEST_REFERENCE_FAULT_ENQUEUED_MARK            106u
#define NGGC_TEST_REFERENCE_FAULT_ORPHAN_ENQUEUED          107u
#define NGGC_TEST_REFERENCE_FAULT_QUEUE_OWNER              108u
#define NGGC_TEST_REFERENCE_FAULT_QUEUE_COUNT              109u
#define NGGC_TEST_REFERENCE_FAULT_ENQUEUED_FLAG            110u

nggc_result nggc_internal_test_reference_fault_check(
    nggc_heap *heap, uint32_t fault, void *target, void *auxiliary);

#define NGGC_TEST_PHASE_CHECK                       201u
#define NGGC_TEST_PHASE_FAULT_INVALID_PHASE         202u
#define NGGC_TEST_PHASE_FAULT_READ_BARRIER          203u
#define NGGC_TEST_PHASE_FAULT_TRANSITION_PARITY     204u
#define NGGC_TEST_PHASE_FAULT_CYCLE_FLAG            205u
#define NGGC_TEST_PHASE_FAULT_INACTIVE_EXPECTED     206u
#define NGGC_TEST_PHASE_FAULT_CYCLE_MISMATCH        207u
#define NGGC_TEST_PHASE_FAULT_ACTIVE_PENDING        208u
#define NGGC_TEST_PHASE_FAULT_ACTIVE_BARRIER        209u
#define NGGC_TEST_PHASE_FAULT_CYCLE_EPOCH           210u
#define NGGC_TEST_PHASE_FAULT_COLLECT_EPOCH         211u
#define NGGC_TEST_PHASE_ACTION_OUT_OF_ORDER         212u
#define NGGC_TEST_PHASE_ACTION_ABORT_CYCLE          213u
#define NGGC_TEST_PHASE_FAULT_SAFEPOINT_ACTIVE       214u
#define NGGC_TEST_PHASE_FAULT_PARKED_COUNT           215u
#define NGGC_TEST_PHASE_FAULT_COLLECT_OWNER          216u
#define NGGC_TEST_PHASE_ACTION_MOVING_CYCLE          217u
#define NGGC_TEST_PHASE_FAULT_EVACUATION_EPOCH       218u
#define NGGC_TEST_PHASE_FAULT_EVACUATION_WORKER_ACTIVE 219u
#define NGGC_TEST_PHASE_FAULT_FROM_SPACE_RECLAIM_GATE 220u
#define NGGC_TEST_PHASE_FAULT_REMAP_WORKER_ACTIVE     221u
#define NGGC_TEST_PHASE_FAULT_REMAP_RECLAIM_GATE      222u
#define NGGC_TEST_PHASE_FAULT_QUARANTINE_BARRIER       223u
#define NGGC_TEST_PHASE_FAULT_QUARANTINE_EPOCH         224u
#define NGGC_TEST_PHASE_FAULT_QUARANTINE_RECLAIM_PROOF 225u
#define NGGC_TEST_PHASE_FAULT_MUTATOR_CONCURRENT       226u

#define NGGC_TEST_COLLECT_FAULT_AFTER_CYCLE_BEGIN    701u
#define NGGC_TEST_COLLECT_FAULT_ROOT_OOM             702u
#define NGGC_TEST_COLLECT_FAULT_REFERENCE_PROCESS    703u
#define NGGC_TEST_COLLECT_FAULT_SWEEP                704u
#define NGGC_TEST_COLLECT_FAULT_BEFORE_FORWARDING    705u
#define NGGC_TEST_COLLECT_FAULT_AFTER_FORWARDING     706u
#define NGGC_TEST_COLLECT_FAULT_AFTER_EVACUATE       707u
#define NGGC_TEST_COLLECT_FAULT_DURING_REMAP         708u
#define NGGC_TEST_COLLECT_FAULT_DURING_SOURCE_RELEASE 709u
#define NGGC_TEST_COLLECT_ACTION_DUPLICATE_EVACUATION_TASK 710u
#define NGGC_TEST_COLLECT_ACTION_WORKER_TASK_RETRY   711u
#define NGGC_TEST_COLLECT_ACTION_WORKER_CREATE_FALLBACK 712u
#define NGGC_TEST_COLLECT_ACTION_CONCURRENT_WINDOW   713u
#define NGGC_TEST_COLLECT_ACTION_FORCE_EMERGENCY_FAILURE 714u
#define NGGC_TEST_COLLECT_ACTION_CONCURRENT_COPY     715u
#define NGGC_TEST_GC_POOL_FAULT_WAIT                801u
#define NGGC_TEST_GC_POOL_FAULT_BROADCAST          802u
#define NGGC_TEST_GC_POOL_FAULT_JOIN               803u
#define NGGC_TEST_GC_POOL_FAULT_WORKER_WAIT        804u
#define NGGC_TEST_GC_POOL_FAULT_WORKER_BEFORE_JOB  805u
#define NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_JOB   806u
#define NGGC_TEST_GC_POOL_FAULT_WORKER_AFTER_COMPLETE 807u
#define NGGC_TEST_GC_POOL_FAULT_REMAP_BEFORE_JOB   808u
#define NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_JOB    809u
#define NGGC_TEST_GC_POOL_FAULT_REMAP_AFTER_COMPLETE 810u

nggc_result nggc_internal_test_phase_fault_check(nggc_heap *heap,
                                                  uint32_t fault);
nggc_result nggc_internal_test_collect_fault_set(nggc_heap *heap,
                                                  uint32_t fault);
nggc_result nggc_internal_test_force_emergency_failure(nggc_heap *heap);
nggc_result nggc_internal_test_concurrent_window_wait(nggc_heap *heap);
nggc_result nggc_internal_test_concurrent_window_release(nggc_heap *heap);
nggc_result nggc_internal_test_concurrent_copy_wait(
    nggc_heap *heap, uint32_t stage);
nggc_result nggc_internal_test_concurrent_copy_release(
    nggc_heap *heap, uint32_t stage);
nggc_result nggc_internal_test_concurrent_copy_source(
    nggc_heap *heap, void **out_object);
nggc_result nggc_internal_test_concurrent_copy_store_wait(nggc_heap *heap);
nggc_result nggc_internal_test_concurrent_copy_load_wait(nggc_heap *heap);
nggc_result nggc_internal_test_reference_create_pause_arm(nggc_heap *heap);
nggc_result nggc_internal_test_reference_create_pause_wait(nggc_heap *heap);
nggc_result nggc_internal_test_reference_create_pause_release(nggc_heap *heap);
size_t nggc_internal_test_safepoint_blocked_threads(nggc_heap *heap);
nggc_result nggc_internal_test_gc_pool_fault_set(nggc_heap *heap,
                                                  uint32_t fault);
nggc_result nggc_internal_test_gc_pool_worker_failure_wait(nggc_heap *heap);
/* Callback-only diagnostic: observes the scanner's stable collector phase. */
nggc_result nggc_internal_test_scanner_phase(uint32_t *out_phase);

#define NGGC_TEST_REMSET_FAULT_CARD_COUNT 301u
#define NGGC_TEST_REMSET_FAULT_COARSE_CARD 302u
#define NGGC_TEST_REMSET_FAULT_CROSS_REGION_CARD 303u
#define NGGC_TEST_REMSET_FAULT_BUFFER_CARD 304u
#define NGGC_TEST_REMSET_FAULT_INFLIGHT_CARD 305u
#define NGGC_TEST_REMSET_FAULT_DEFERRED_OUTCOME 306u
#define NGGC_TEST_REMSET_FAULT_CARD_VALUE 307u
#define NGGC_TEST_REMSET_FAULT_CLASSIFICATION_OUTCOME 308u

nggc_result nggc_internal_test_barrier_tss_fail_next_update(
    nggc_heap *heap);
nggc_result nggc_internal_test_remset_publish_pause_arm(nggc_heap *heap);
nggc_result nggc_internal_test_remset_publish_pause_wait(nggc_heap *heap);
nggc_result nggc_internal_test_remset_publish_pause_release(
    nggc_heap *heap);
nggc_result nggc_internal_test_remset_classification_pause_arm(
    nggc_heap *heap);
nggc_result nggc_internal_test_remset_classification_pause_wait(
    nggc_heap *heap);
nggc_result nggc_internal_test_remset_classification_pause_release(
    nggc_heap *heap);
nggc_result nggc_internal_test_remset_set_limit(nggc_heap *heap,
                                                 size_t max_dirty_cards);
nggc_result nggc_internal_test_remset_rebuild_check(nggc_heap *heap);
nggc_result nggc_internal_test_remset_fault_check(nggc_heap *heap,
                                                   uint32_t fault);

#define NGGC_TEST_REGION_FAULT_CURSOR       401u
#define NGGC_TEST_REGION_FAULT_OWNER        402u
#define NGGC_TEST_REGION_FAULT_LEASE_PENDING 403u
#define NGGC_TEST_REGION_FAULT_OWNER_DUPLICATE 404u
#define NGGC_TEST_REGION_FAULT_GENERATION_INVALID 405u
#define NGGC_TEST_REGION_FAULT_PLACEMENT_INVALID 406u
#define NGGC_TEST_REGION_FAULT_PLACEMENT_NON_SPECIAL 407u
#define NGGC_TEST_REGION_FAULT_PLACEMENT_MISMATCH 408u
#define NGGC_TEST_REGION_FAULT_HUMONGOUS_ROLE_INVALID 409u
#define NGGC_TEST_REGION_FAULT_HUMONGOUS_OFFSET 410u
#define NGGC_TEST_LARGE_BIN_FAULT_MASK 411u
#define NGGC_TEST_LARGE_BIN_FAULT_WRONG_BIN 412u
#define NGGC_TEST_DECOMMITTED_BIN_FAULT_MASK 413u
#define NGGC_TEST_DECOMMITTED_RUN_FAULT_OFFSET 414u
#define NGGC_TEST_LARGE_BIN_FAULT_ORDER 415u
#define NGGC_TEST_DECOMMITTED_BIN_FAULT_ORDER 416u
#define NGGC_TEST_REGION_FAULT_SMART_PRESSURE_SOURCE 417u
#define NGGC_TEST_REGION_FAULT_SMART_SLOW_VM 418u

nggc_result nggc_internal_test_region_fault_check(nggc_heap *heap,
                                                   uint32_t fault);
nggc_result nggc_internal_test_large_free_bin_fault_check(
    nggc_heap *heap, uint32_t fault);
nggc_result nggc_internal_test_decommitted_run_fault_check(
    nggc_heap *heap, uint32_t fault);
nggc_result nggc_internal_test_object_generation(nggc_heap *heap,
                                                  void *object,
                                                  uint32_t *out_generation);
nggc_result nggc_internal_test_forwarding_cas_check(
    nggc_heap *heap, void *source, void *winner, void *loser);

#define NGGC_TEST_TLAB_FAULT_REGION_ORPHAN  501u
#define NGGC_TEST_TLAB_FAULT_THREAD_ORPHAN  502u
#define NGGC_TEST_TLAB_FAULT_TOP_BOUNDS     503u
#define NGGC_TEST_TLAB_FAULT_EPOCH_MISMATCH 504u
#define NGGC_TEST_TLAB_FAULT_ACK_NOT_RETIRED 505u
#define NGGC_TEST_TLAB_FAULT_RESERVED_IN_FREE_LIST 506u
#define NGGC_TEST_TLAB_FAULT_INTERVAL_OVERLAP 507u
#define NGGC_TEST_TLAB_FAULT_FAST_TOP_BOUNDS 508u
#define NGGC_TEST_TLAB_FAULT_FAST_INFLIGHT 509u
#define NGGC_TEST_TLAB_FAULT_OBJECT_ID_LEASE 510u
#define NGGC_TEST_TLAB_FAULT_FAST_GATE 511u
#define NGGC_TEST_TLAB_FAULT_ADAPTIVE_TARGET 512u

nggc_result nggc_internal_test_tlab_fault_check(nggc_heap *heap,
                                                 nggc_thread *thread,
                                                 uint32_t fault);
nggc_result nggc_internal_test_tlab_reserved_span_check(
    nggc_heap *heap, nggc_thread *thread);
nggc_result nggc_internal_test_tlab_fast_pause_arm(nggc_heap *heap);
nggc_result nggc_internal_test_tlab_fast_pause_wait(nggc_heap *heap);
nggc_result nggc_internal_test_tlab_fast_gate_closed(nggc_heap *heap);
nggc_result nggc_internal_test_tlab_fast_pause_release(nggc_heap *heap);
nggc_result nggc_internal_test_tlab_fast_enrollment_pause_arm(
    nggc_heap *heap);
nggc_result nggc_internal_test_tlab_fast_enrollment_pause_wait(
    nggc_heap *heap, uint32_t stage);
nggc_result nggc_internal_test_tlab_fast_enrollment_pause_advance(
    nggc_heap *heap, uint32_t stage);
nggc_result nggc_internal_test_tlab_fast_flush_pause_arm(
    nggc_heap *heap);
nggc_result nggc_internal_test_tlab_fast_flush_pause_wait(
    nggc_heap *heap);
nggc_result nggc_internal_test_tlab_fast_flush_pause_release(
    nggc_heap *heap);
nggc_result nggc_internal_test_tlab_fast_flush_busy_wait(
    nggc_heap *heap);
#endif

#endif
