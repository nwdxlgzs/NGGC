#include "nggc/nggc.h"
#include "nggc_internal.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_OBJECT_HEADER_BYTES 64u

typedef struct test_header_prefix {
    uint32_t magic;
    atomic_uint state;
    uint16_t type_id;
    uint16_t size_class;
    uint32_t flags;
} test_header_prefix;

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

int main(void) {
    nggc_heap *heap = 0;
    nggc_heap *pressure_heap = 0;
    nggc_heap *phantom_heap = 0;
    nggc_config config;
    nggc_config pressure_config;
    nggc_config phantom_config;
    nggc_stats stats;
    void *internal_object = 0;
    void *rooted_internal = 0;
    void *weak_referent = 0;
    void *weak_reference = 0;
    void *rooted_referent = 0;
    void *rooted_weak = 0;
    void *soft_referent = 0;
    void *soft_reference = 0;
    void *pressure_soft_referent = 0;
    void *pressure_soft = 0;
    void *phantom_queue = 0;
    void *phantom_referent = 0;
    void *phantom_reference = 0;
    void *popped_phantom = 0;
    test_header_prefix *header;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = (size_t)512u * 1024u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&config, &heap), "open heap");

    expect_true(nggc_alloc(heap, NGGC_TYPE_NONE) == 0,
                "public none allocation rejected");
    expect_true(nggc_get_last_error(heap) == NGGC_ERR_INVALID_ARGUMENT,
                "public none allocation error");

    internal_object = nggc_internal_alloc_none(heap, 32u);
    expect_true(internal_object != 0, "internal none allocation");
    expect_true(nggc_object_type(internal_object) == NGGC_TYPE_NONE,
                "internal none type");
    expect_true((nggc_object_flags(internal_object) &
                 NGGC_OBJECT_FLAG_INTERNAL) != 0u,
                "internal none flag");
    expect_true(nggc_object_size(internal_object) == 32u,
                "internal none size");
    expect_ok(nggc_check(heap), "internal none check");

    expect_ok(nggc_collect(heap), "collect unrooted internal none");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "stats after internal none collect");
    expect_true(stats.live_object_count == 0u,
                "unrooted internal none reclaimed");
    expect_true(stats.reference_process_count == 1u,
                "internal none reference process count");
    expect_true(stats.reclaimed_object_count == 1u,
                "internal none reclaimed count");

    rooted_internal = nggc_internal_alloc_none(heap, 64u);
    expect_true(rooted_internal != 0, "rooted internal none allocation");
    expect_ok(nggc_root_push(heap, &rooted_internal),
              "push internal none root");
    expect_ok(nggc_collect(heap), "collect rooted internal none");
    expect_true(rooted_internal != 0, "rooted internal none pointer");
    expect_true(nggc_object_type(rooted_internal) == NGGC_TYPE_NONE,
                "rooted internal none type");
    expect_true((nggc_object_flags(rooted_internal) &
                 NGGC_OBJECT_FLAG_INTERNAL) != 0u,
                "rooted internal none flag");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "stats after rooted internal none collect");
    expect_true(stats.live_object_count == 1u,
                "rooted internal none remains live");

    header = (test_header_prefix *)((uintptr_t)rooted_internal -
                                    TEST_OBJECT_HEADER_BYTES);
    header->flags = 0u;
    expect_true(nggc_check(heap) == NGGC_ERR_CORRUPTION,
                "live none without internal flag rejected");
    header->flags = NGGC_OBJECT_FLAG_INTERNAL;
    expect_ok(nggc_check(heap), "internal none repaired");

    expect_ok(nggc_root_pop(heap, &rooted_internal),
              "pop internal none root");
    expect_ok(nggc_collect(heap), "collect unrooted internal none root");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "stats after final internal none collect");
    expect_true(stats.live_object_count == 0u,
                "final internal none reclaimed");

    expect_true(nggc_internal_alloc_weak(heap, (void *)0x1234) == 0,
                "invalid weak referent rejected");
    expect_true(nggc_get_last_error(heap) == NGGC_ERR_CORRUPTION,
                "invalid weak referent error");

    weak_referent = nggc_internal_alloc_none(heap, 16u);
    weak_reference = nggc_internal_alloc_weak(heap, weak_referent);
    expect_true(weak_referent != 0 && weak_reference != 0,
                "weak reference allocation");
    expect_true(nggc_internal_weak_get(heap, weak_reference) == weak_referent,
                "weak referent before collect");
    expect_ok(nggc_root_push(heap, &weak_reference),
              "push weak reference root");
    expect_ok(nggc_collect(heap), "collect weak referent");
    expect_true(nggc_internal_weak_get(heap, weak_reference) == 0,
                "weak referent cleared after collect");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "stats after weak referent collect");
    expect_true(stats.live_object_count == 1u,
                "weak reference object remains live");
    expect_true(stats.weak_reference_discovered_count == 1u,
                "weak reference discovered");
    expect_true(stats.weak_reference_cleared_count == 1u,
                "weak referent cleared count");
    expect_ok(nggc_root_pop(heap, &weak_reference),
              "pop weak reference root");
    expect_ok(nggc_collect(heap), "collect weak reference object");

    rooted_referent = nggc_internal_alloc_none(heap, 16u);
    rooted_weak = nggc_internal_alloc_weak(heap, rooted_referent);
    expect_true(rooted_referent != 0 && rooted_weak != 0,
                "rooted weak allocation");
    expect_ok(nggc_root_push(heap, &rooted_referent),
              "push rooted referent");
    expect_ok(nggc_root_push(heap, &rooted_weak), "push rooted weak");
    expect_ok(nggc_collect(heap), "collect retained weak referent");
    expect_true(nggc_internal_weak_get(heap, rooted_weak) == rooted_referent,
                "weak referent retained while strongly rooted");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "stats after retained weak referent collect");
    expect_true(stats.live_object_count == 2u,
                "rooted weak and referent remain live");
    expect_true(stats.weak_reference_discovered_count == 2u,
                "retained weak discovered");
    expect_true(stats.weak_reference_cleared_count == 1u,
                "retained weak not cleared");
    expect_ok(nggc_root_pop(heap, &rooted_weak), "pop rooted weak");
    expect_ok(nggc_root_pop(heap, &rooted_referent),
              "pop rooted referent");
    expect_ok(nggc_collect(heap), "collect retained weak objects");

    expect_true(nggc_internal_alloc_soft(heap, (void *)0x1234) == 0,
                "invalid soft referent rejected");
    expect_true(nggc_get_last_error(heap) == NGGC_ERR_CORRUPTION,
                "invalid soft referent error");

    soft_referent = nggc_internal_alloc_none(heap, 16u);
    soft_reference = nggc_internal_alloc_soft(heap, soft_referent);
    expect_true(soft_referent != 0 && soft_reference != 0,
                "soft reference allocation");
    expect_true(nggc_internal_soft_get(heap, soft_reference) == soft_referent,
                "soft referent before collect");
    expect_ok(nggc_root_push(heap, &soft_reference),
              "push soft reference root");
    expect_ok(nggc_collect(heap), "collect retained soft referent");
    expect_true(nggc_internal_soft_get(heap, soft_reference) == soft_referent,
                "soft referent retained under low pressure");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "stats after retained soft referent collect");
    expect_true(stats.live_object_count == 2u,
                "soft reference and referent remain live");
    expect_true(stats.soft_reference_discovered_count == 1u,
                "soft reference discovered");
    expect_true(stats.soft_reference_retained_count == 1u,
                "soft referent retained count");
    expect_true(stats.soft_reference_cleared_count == 0u,
                "soft referent not cleared count");
    expect_ok(nggc_check(heap), "soft retained heap check");
    expect_ok(nggc_root_pop(heap, &soft_reference),
              "pop soft reference root");
    expect_ok(nggc_collect(heap), "collect retained soft objects");

    memset(&pressure_config, 0, sizeof(pressure_config));
    pressure_config.size = sizeof(pressure_config);
    pressure_config.abi_major = NGGC_ABI_MAJOR;
    pressure_config.abi_minor = NGGC_ABI_MINOR;
    pressure_config.gc_arena_size = 4096u;
    pressure_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&pressure_config, &pressure_heap),
              "open pressure heap");
    pressure_soft_referent = nggc_internal_alloc_none(pressure_heap, 3072u);
    pressure_soft = nggc_internal_alloc_soft(pressure_heap,
                                             pressure_soft_referent);
    expect_true(pressure_soft_referent != 0 && pressure_soft != 0,
                "pressure soft allocation");
    expect_ok(nggc_root_push(pressure_heap, &pressure_soft),
              "push pressure soft root");
    expect_ok(nggc_collect(pressure_heap), "collect pressure soft referent");
    expect_true(nggc_internal_soft_get(pressure_heap, pressure_soft) == 0,
                "soft referent cleared under pressure");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(pressure_heap, &stats),
              "stats after pressure soft collect");
    expect_true(stats.live_object_count == 1u,
                "pressure soft reference remains live");
    expect_true(stats.soft_reference_discovered_count == 1u,
                "pressure soft discovered");
    expect_true(stats.soft_reference_retained_count == 0u,
                "pressure soft not retained");
    expect_true(stats.soft_reference_cleared_count == 1u,
                "pressure soft cleared count");
    expect_ok(nggc_close(pressure_heap), "close pressure heap");

    expect_true(nggc_internal_alloc_phantom(heap, 0, (void *)0x1234) == 0,
                "invalid phantom queue rejected");
    expect_true(nggc_get_last_error(heap) == NGGC_ERR_CORRUPTION,
                "invalid phantom queue error");

    memset(&phantom_config, 0, sizeof(phantom_config));
    phantom_config.size = sizeof(phantom_config);
    phantom_config.abi_major = NGGC_ABI_MAJOR;
    phantom_config.abi_minor = NGGC_ABI_MINOR;
    phantom_config.gc_arena_size = (size_t)512u * 1024u;
    phantom_config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    expect_ok(nggc_open(&phantom_config, &phantom_heap),
              "open phantom heap");
    phantom_queue = nggc_internal_alloc_phantom_queue(phantom_heap);
    phantom_referent = nggc_internal_alloc_none(phantom_heap, 16u);
    phantom_reference = nggc_internal_alloc_phantom(
        phantom_heap, phantom_referent, phantom_queue);
    expect_true(phantom_queue != 0 && phantom_referent != 0 &&
                    phantom_reference != 0,
                "phantom allocation");
    expect_true(nggc_internal_phantom_queue_count(phantom_heap,
                                                  phantom_queue) == 0u,
                "phantom queue initially empty");
    expect_ok(nggc_root_push(phantom_heap, &phantom_reference),
              "push phantom reference root");
    expect_ok(nggc_collect(phantom_heap), "collect phantom referent");
    expect_true(nggc_internal_phantom_queue_count(phantom_heap,
                                                  phantom_queue) == 1u,
                "phantom enqueued after referent death");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(phantom_heap, &stats),
              "stats after phantom enqueue");
    expect_true(stats.live_object_count == 2u,
                "phantom queue and reference remain live");
    expect_true(stats.phantom_reference_discovered_count == 1u,
                "phantom discovered");
    expect_true(stats.phantom_reference_enqueued_count == 1u,
                "phantom enqueued count");
    expect_true(stats.phantom_queue_promoted_count == 1u,
                "phantom queue promoted count");
    expect_ok(nggc_check(phantom_heap), "phantom heap check after enqueue");
    expect_ok(nggc_root_pop(phantom_heap, &phantom_reference),
              "pop phantom reference root");
    expect_ok(nggc_collect(phantom_heap),
              "collect non-empty phantom queue without external roots");
    expect_true(nggc_internal_phantom_queue_count(phantom_heap,
                                                  phantom_queue) == 1u,
                "non-empty phantom queue self-retained");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(phantom_heap, &stats),
              "stats after phantom queue promotion");
    expect_true(stats.live_object_count == 2u,
                "promoted phantom queue and reference remain live");
    expect_true(stats.phantom_queue_promoted_count == 2u,
                "phantom queue promoted again");
    popped_phantom = nggc_internal_phantom_queue_pop(phantom_heap,
                                                     phantom_queue);
    expect_true(popped_phantom == phantom_reference, "pop phantom reference");
    expect_true(nggc_internal_phantom_queue_count(phantom_heap,
                                                  phantom_queue) == 0u,
                "phantom queue drained");
    expect_ok(nggc_collect(phantom_heap), "collect drained phantom queue");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(phantom_heap, &stats),
              "stats after drained phantom queue collect");
    expect_true(stats.live_object_count == 0u,
                "drained phantom queue reclaimed");
    expect_ok(nggc_close(phantom_heap), "close phantom heap");

    expect_ok(nggc_close(heap), "close heap");
    return 0;
}
