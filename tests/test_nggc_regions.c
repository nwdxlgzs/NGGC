#include "nggc/nggc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void read_stats(nggc_heap *heap, nggc_stats *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->size = sizeof(*stats);
    expect_ok(nggc_get_stats(heap, stats), "read region stats");
}

typedef struct memory_pressure_test_ctx {
    uint32_t fail;
    uint32_t pressure_basis_points;
    size_t resident_bytes;
    size_t limit_bytes;
    uint64_t call_count;
} memory_pressure_test_ctx;

typedef struct clock_test_ctx {
    uint64_t now_ns;
    uint64_t step_ns;
    uint32_t fail;
} clock_test_ctx;

static nggc_result sample_memory_pressure(
    void *context, nggc_memory_pressure_sample *sample) {
    memory_pressure_test_ctx *ctx =
        (memory_pressure_test_ctx *)context;
    if (!ctx || !sample || sample->size < sizeof(*sample)) {
        return NGGC_ERR_INVALID_ARGUMENT;
    }
    ++ctx->call_count;
    if (ctx->fail == 1u) {
        return NGGC_ERR_BACKEND;
    }
    sample->flags = NGGC_MEMORY_PRESSURE_RESIDENT_VALID |
                    NGGC_MEMORY_PRESSURE_LIMIT_VALID |
                    NGGC_MEMORY_PRESSURE_DIRECT_VALID;
    sample->resident_bytes = ctx->resident_bytes;
    sample->limit_bytes = ctx->limit_bytes;
    sample->pressure_basis_points =
        ctx->fail == 2u ? 10001u : ctx->pressure_basis_points;
    return NGGC_OK;
}

static nggc_result sample_clock_ns(void *context, uint64_t *out_ns) {
    clock_test_ctx *ctx = (clock_test_ctx *)context;
    if (!ctx || !out_ns || ctx->fail) {
        return NGGC_ERR_BACKEND;
    }
    if (ctx->now_ns > UINT64_MAX - ctx->step_ns) {
        return NGGC_ERR_BACKEND;
    }
    ctx->now_ns += ctx->step_ns;
    *out_ns = ctx->now_ns;
    return NGGC_OK;
}

static void init_config(nggc_config *config, size_t gc_arena_size) {
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->abi_major = NGGC_ABI_MAJOR;
    config->abi_minor = NGGC_ABI_MINOR;
    config->gc_arena_size = gc_arena_size;
    config->uhm_arena_size = (size_t)2u * 1024u * 1024u;
}

static void expect_region_config_rejected(size_t region_size,
                                          size_t gc_arena_size,
                                          size_t tlab_size) {
    nggc_config config;
    nggc_heap *heap = 0;
    init_config(&config, gc_arena_size);
    config.region_size = region_size;
    config.tlab_size = tlab_size;
    if (nggc_open(&config, &heap) != NGGC_ERR_INVALID_ARGUMENT || heap) {
        if (heap) {
            (void)nggc_close(heap);
        }
        fail("reject invalid Region geometry");
    }
}

static void expect_tlab_sizing_config_rejected(
    uint32_t policy, size_t initial_size, size_t min_size,
    size_t max_size) {
    nggc_config config;
    nggc_heap *heap = 0;
    init_config(&config, (size_t)192u * 1024u);
    config.region_size = (size_t)24u * 1024u;
    config.tlab_sizing_policy = policy;
    config.tlab_size = initial_size;
    config.tlab_min_size = min_size;
    config.tlab_max_size = max_size;
    if (nggc_open(&config, &heap) != NGGC_ERR_INVALID_ARGUMENT || heap) {
        if (heap) {
            (void)nggc_close(heap);
        }
        fail("reject invalid adaptive TLAB geometry");
    }
}

static void expect_remset_buffer_config_rejected(size_t capacity) {
    nggc_config config;
    nggc_heap *heap = 0;
    init_config(&config, (size_t)8u * 1024u);
    config.region_size = (size_t)512u;
    config.remset_buffer_capacity = capacity;
    if (nggc_open(&config, &heap) != NGGC_ERR_INVALID_ARGUMENT || heap) {
        if (heap) {
            (void)nggc_close(heap);
        }
        fail("reject remembered-set buffer larger than card table");
    }
}

static void expect_interior_decommit_config_rejected(size_t threshold) {
    nggc_config config;
    nggc_heap *heap = 0;
    init_config(&config, (size_t)16u * 8192u);
    config.region_size = (size_t)8192u;
    config.interior_decommit_min_regions = threshold;
    if (nggc_open(&config, &heap) != NGGC_ERR_INVALID_ARGUMENT || heap) {
        if (heap) {
            (void)nggc_close(heap);
        }
        fail("reject invalid interior decommit threshold");
    }
}

static void verify_smart_callback_config_validation(void) {
    nggc_config config;
    nggc_heap *heap = 0;
    int context_token = 0;

    init_config(&config, (size_t)16u * 8192u);
    config.region_size = (size_t)8192u;
    config.memory_pressure_ctx = &context_token;
    if (nggc_open(&config, &heap) != NGGC_ERR_INVALID_ARGUMENT || heap) {
        fail("reject memory-pressure context without callback");
    }

    init_config(&config, (size_t)16u * 8192u);
    config.region_size = (size_t)8192u;
    config.clock_ns_ctx = &context_token;
    if (nggc_open(&config, &heap) != NGGC_ERR_INVALID_ARGUMENT || heap) {
        fail("reject clock context without callback");
    }

    init_config(&config, (size_t)16u * 8192u);
    config.region_size = (size_t)8192u;
    config.interior_decommit_slow_ns_per_region = 1u;
    if (nggc_open(&config, &heap) != NGGC_ERR_INVALID_ARGUMENT || heap) {
        fail("reject SMART latency threshold without monotonic clock");
    }
}

static void verify_custom_region_size(size_t region_size,
                                      size_t gc_arena_size,
                                      size_t regular_payload_size) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *regular;
    void *large;
    size_t expected_tlab_size = region_size < (size_t)16u * 1024u
                                    ? region_size
                                    : (size_t)16u * 1024u;
    size_t expected_tlab_min_size =
        expected_tlab_size < (size_t)4u * 1024u
            ? expected_tlab_size
            : (size_t)4u * 1024u;
    size_t expected_card_count = gc_arena_size / 512u;
    size_t expected_remset_buffer_capacity =
        expected_card_count < 64u ? expected_card_count : 64u;

    init_config(&config, gc_arena_size);
    if (gc_arena_size > (size_t)1024u * 1024u) {
        config.uhm_arena_size = (size_t)8u * 1024u * 1024u;
    }
    config.region_size = region_size;
    expect_ok(nggc_open(&config, &heap), "open configurable Region heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "configurable_region_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register configurable Region type");
    regular = nggc_alloc_size(heap, type_id, regular_payload_size);
    large = nggc_alloc_size(heap, type_id, region_size);
    if (!regular || !large ||
        (nggc_object_flags(regular) & NGGC_OBJECT_FLAG_LARGE) != 0u ||
        (nggc_object_flags(large) & NGGC_OBJECT_FLAG_LARGE) == 0u) {
        fail("Region-derived large object threshold");
    }
    read_stats(heap, &stats);
    if (stats.region_size != region_size ||
        stats.region_count !=
            (gc_arena_size + region_size - 1u) / region_size ||
        stats.gc_arena_used != (size_t)3u * region_size ||
        stats.tlab_size != expected_tlab_size ||
        stats.tlab_sizing_policy != NGGC_TLAB_SIZING_POLICY_SMART ||
        stats.tlab_min_size != expected_tlab_min_size ||
        stats.tlab_max_size != region_size ||
        stats.tlab_target_min_size != 0u ||
        stats.tlab_target_max_size != 0u ||
        stats.large_object_count != 1u ||
        stats.humongous_run_count != 1u ||
        stats.humongous_live_run_count != 1u ||
        stats.humongous_free_run_count != 0u ||
        stats.humongous_region_count != 2u ||
        stats.card_size != 512u ||
        stats.card_count != expected_card_count ||
        stats.remset_buffer_capacity !=
            expected_remset_buffer_capacity) {
        fail("configurable Region statistics");
    }
    expect_ok(nggc_check(heap), "verify configurable Region heap");
    expect_ok(nggc_close(heap), "close configurable Region heap");
}

static void verify_special_region_placement_isolation(void) {
    nggc_config config;
    nggc_type_desc movable_desc;
    nggc_type_desc pinned_desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    uint16_t movable_type = NGGC_TYPE_NONE;
    uint16_t pinned_type = NGGC_TYPE_NONE;
    void *movable_special;
    void *movable_special_again;
    void *pinned;
    void *pinned_addr;
    void *anchor_a;
    void *anchor_b;

    init_config(&config, (size_t)8u * 8192u);
    config.region_size = (size_t)8192u;
    expect_ok(nggc_open(&config, &heap),
              "open SPECIAL placement isolation heap");

    memset(&movable_desc, 0, sizeof(movable_desc));
    movable_desc.size = sizeof(movable_desc);
    movable_desc.name = "movable_special_leaf";
    expect_ok(nggc_register_type(heap, &movable_desc, &movable_type),
              "register movable SPECIAL type");

    memset(&pinned_desc, 0, sizeof(pinned_desc));
    pinned_desc.size = sizeof(pinned_desc);
    pinned_desc.flags = NGGC_TYPE_FLAG_PINNABLE |
                        NGGC_TYPE_FLAG_NON_MOVABLE;
    pinned_desc.object_size = 1024u;
    pinned_desc.name = "isolated_pinned_leaf";
    expect_ok(nggc_register_type(heap, &pinned_desc, &pinned_type),
              "register isolated pinned type");

    movable_special = nggc_alloc_size(heap, movable_type,
                                      config.region_size);
    anchor_a = nggc_alloc_size(heap, movable_type, 512u);
    if (!movable_special || !anchor_a) {
        fail("allocate movable SPECIAL and first anchor");
    }
    expect_ok(nggc_root_push(heap, &anchor_a), "root first anchor");
    expect_ok(nggc_collect(heap), "reclaim movable SPECIAL block");

    pinned = nggc_alloc(heap, pinned_type);
    if (!pinned || pinned == movable_special) {
        fail("pinned allocation reused movable SPECIAL Region");
    }
    pinned_addr = pinned;
    expect_ok(nggc_root_push(heap, &pinned), "root isolated pinned object");
    anchor_b = nggc_alloc_size(heap, movable_type, 512u);
    if (!anchor_b) {
        fail("allocate second anchor after pinned Region");
    }
    expect_ok(nggc_root_push(heap, &anchor_b), "root second anchor");
    read_stats(heap, &stats);
    if (stats.pinned_region_count == 0u ||
        stats.non_movable_region_count == 0u ||
        stats.special_region_count < 2u) {
        fail("pinned SPECIAL placement accounting");
    }
    expect_ok(nggc_check(heap), "verify pinned placement isolation");

    expect_ok(nggc_root_pop(heap, &anchor_b),
              "temporarily unroot second anchor");
    expect_ok(nggc_root_pop(heap, &pinned), "unroot isolated pinned object");
    pinned = 0;
    expect_ok(nggc_root_push(heap, &anchor_b),
              "reroot second anchor after pinned");
    expect_ok(nggc_collect(heap), "reclaim pinned SPECIAL block");

    movable_special_again = nggc_alloc_size(heap, movable_type,
                                            config.region_size);
    if (!movable_special_again ||
        movable_special_again != movable_special) {
        fail("movable SPECIAL did not prefer movable sticky Region");
    }
    if (movable_special_again == pinned_addr) {
        fail("movable SPECIAL reused pinned sticky Region");
    }
    expect_ok(nggc_check(heap), "verify movable placement isolation");

    expect_ok(nggc_root_pop(heap, &anchor_b), "unroot second anchor");
    expect_ok(nggc_root_pop(heap, &anchor_a), "unroot first anchor");
    expect_ok(nggc_close(heap), "close SPECIAL placement isolation heap");
}

static void verify_humongous_run_space(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats reclaimed_stats;
    nggc_stats small_stats;
    nggc_stats reused_stats;
    nggc_stats consumed_stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *large;
    void *guard;
    void *small;
    void *large_again;
    void *large_remainder;

    init_config(&config, (size_t)12u * region_size);
    config.region_size = region_size;
    expect_ok(nggc_open(&config, &heap), "open large object space heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "large_space_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register large object space type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    large = nggc_alloc_size(heap, type_id,
                            (size_t)3u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !large || !guard ||
        (nggc_object_flags(prefix) & NGGC_OBJECT_FLAG_LARGE) != 0u ||
        (nggc_object_flags(large) & NGGC_OBJECT_FLAG_LARGE) == 0u ||
        (nggc_object_flags(guard) & NGGC_OBJECT_FLAG_LARGE) == 0u) {
        fail("allocate isolated large object span");
    }
    expect_ok(nggc_root_push(heap, &prefix), "root large-space prefix");
    expect_ok(nggc_root_push(heap, &guard), "root large-space guard");
    expect_ok(nggc_collect(heap), "reclaim isolated large object");
    read_stats(heap, &reclaimed_stats);
    if (reclaimed_stats.large_object_count != 1u ||
        reclaimed_stats.large_free_object_count != 1u ||
        reclaimed_stats.large_free_bytes !=
            (size_t)4u * region_size ||
        reclaimed_stats.humongous_run_count != 2u ||
        reclaimed_stats.humongous_live_run_count != 1u ||
        reclaimed_stats.humongous_free_run_count != 1u ||
        reclaimed_stats.humongous_region_count != 6u ||
        reclaimed_stats.large_free_bin_capacity != 32u ||
        reclaimed_stats.large_free_active_bin_count != 1u ||
        reclaimed_stats.largest_large_free_run_regions != 4u ||
        reclaimed_stats.large_allocation_count != 2u ||
        reclaimed_stats.large_reuse_count != 0u) {
        fprintf(stderr,
                "large reclaim stats: live=%zu large_free_count=%zu "
                "large_free_bytes=%zu total_free_count=%zu "
                "total_free_bytes=%zu used=%zu active=%zu decommitted=%zu "
                "allocations=%llu reuse=%llu\n",
                reclaimed_stats.large_object_count,
                reclaimed_stats.large_free_object_count,
                reclaimed_stats.large_free_bytes,
                reclaimed_stats.free_object_count,
                reclaimed_stats.free_bytes,
                reclaimed_stats.gc_arena_used,
                reclaimed_stats.active_region_count,
                reclaimed_stats.decommitted_region_count,
                (unsigned long long)
                    reclaimed_stats.large_allocation_count,
                (unsigned long long)reclaimed_stats.large_reuse_count);
        fail("large free pool accounting after reclaim");
    }
    expect_ok(nggc_check(heap), "verify reclaimed large free pool");

    small = nggc_alloc_size(heap, type_id, 512u);
    if (!small || small == large ||
        (nggc_object_flags(small) & NGGC_OBJECT_FLAG_LARGE) != 0u) {
        fail("small allocation entered large free pool");
    }
    read_stats(heap, &small_stats);
    if (small_stats.large_free_object_count !=
            reclaimed_stats.large_free_object_count ||
        small_stats.large_free_bytes != reclaimed_stats.large_free_bytes ||
        small_stats.large_free_active_bin_count !=
            reclaimed_stats.large_free_active_bin_count ||
        small_stats.largest_large_free_run_regions !=
            reclaimed_stats.largest_large_free_run_regions ||
        small_stats.large_free_search_count !=
            reclaimed_stats.large_free_search_count ||
        small_stats.large_free_probe_count !=
            reclaimed_stats.large_free_probe_count ||
        small_stats.large_reuse_count != reclaimed_stats.large_reuse_count) {
        fail("small allocation changed large free pool");
    }
    expect_ok(nggc_check(heap), "verify small and large free isolation");

    large_again = nggc_alloc_size(heap, type_id, region_size);
    if (!large_again || large_again != large ||
        (nggc_object_flags(large_again) & NGGC_OBJECT_FLAG_LARGE) == 0u) {
        fail("large allocation did not reuse large free span");
    }
    read_stats(heap, &reused_stats);
    if (reused_stats.large_object_count != 2u ||
        reused_stats.large_free_object_count != 1u ||
        reused_stats.large_free_bytes !=
            (size_t)2u * region_size ||
        reused_stats.humongous_run_count != 3u ||
        reused_stats.humongous_live_run_count != 2u ||
        reused_stats.humongous_free_run_count != 1u ||
        reused_stats.humongous_region_count != 6u ||
        reused_stats.large_free_active_bin_count != 1u ||
        reused_stats.largest_large_free_run_regions != 2u ||
        reused_stats.large_allocation_count != 3u ||
        reused_stats.large_reuse_count != 1u ||
        reused_stats.large_free_search_count <=
            small_stats.large_free_search_count ||
        reused_stats.large_free_probe_count <=
            small_stats.large_free_probe_count) {
        fail("humongous free run split accounting");
    }
    expect_ok(nggc_check(heap), "verify split humongous free run");

    large_remainder = nggc_alloc_size(heap, type_id, region_size);
    if (!large_remainder ||
        large_remainder !=
            (void *)((unsigned char *)large_again +
                     (size_t)2u * region_size) ||
        (nggc_object_flags(large_remainder) &
         NGGC_OBJECT_FLAG_LARGE) == 0u) {
        fail("large allocation did not reuse split run remainder");
    }
    read_stats(heap, &consumed_stats);
    if (consumed_stats.large_object_count != 3u ||
        consumed_stats.large_free_object_count != 0u ||
        consumed_stats.large_free_bytes != 0u ||
        consumed_stats.humongous_run_count != 3u ||
        consumed_stats.humongous_live_run_count != 3u ||
        consumed_stats.humongous_free_run_count != 0u ||
        consumed_stats.humongous_region_count != 6u ||
        consumed_stats.large_free_active_bin_count != 0u ||
        consumed_stats.largest_large_free_run_regions != 0u ||
        consumed_stats.large_allocation_count != 4u ||
        consumed_stats.large_reuse_count != 2u ||
        consumed_stats.large_free_search_count <=
            reused_stats.large_free_search_count ||
        consumed_stats.large_free_probe_count <=
            reused_stats.large_free_probe_count) {
        fail("split humongous remainder reuse accounting");
    }
    expect_ok(nggc_check(heap),
              "verify consumed humongous free remainder");

    expect_ok(nggc_root_pop(heap, &guard), "unroot large-space guard");
    expect_ok(nggc_root_pop(heap, &prefix), "unroot large-space prefix");
    expect_ok(nggc_collect(heap), "reclaim large-space test objects");
    expect_ok(nggc_check(heap), "verify final large-space heap");
    expect_ok(nggc_close(heap), "close large object space heap");
}

static void verify_humongous_single_region_remainder_absorbed(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *three_region_run;
    void *guard;
    void *replacement;

    init_config(&config, (size_t)8u * region_size);
    config.region_size = region_size;
    expect_ok(nggc_open(&config, &heap),
              "open humongous remainder heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "humongous_remainder_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register humongous remainder type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    three_region_run = nggc_alloc_size(
        heap, type_id, (size_t)2u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !three_region_run || !guard) {
        fail("allocate three-Region humongous run and guard");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root humongous remainder prefix");
    expect_ok(nggc_root_push(heap, &guard),
              "root humongous remainder guard");
    expect_ok(nggc_collect(heap),
              "reclaim three-Region humongous run");
    read_stats(heap, &stats);
    if (stats.large_free_object_count != 1u ||
        stats.large_free_bytes != (size_t)3u * region_size ||
        stats.humongous_run_count != 2u ||
        stats.humongous_live_run_count != 1u ||
        stats.humongous_free_run_count != 1u ||
        stats.humongous_region_count != 5u ||
        stats.large_free_active_bin_count != 1u ||
        stats.largest_large_free_run_regions != 3u) {
        fail("three-Region humongous free accounting");
    }

    replacement = nggc_alloc_size(heap, type_id, region_size);
    if (!replacement || replacement != three_region_run) {
        fail("two-Region request did not absorb three-Region run");
    }
    read_stats(heap, &stats);
    if (stats.large_free_object_count != 0u ||
        stats.large_free_bytes != 0u ||
        stats.humongous_run_count != 2u ||
        stats.humongous_live_run_count != 2u ||
        stats.humongous_free_run_count != 0u ||
        stats.humongous_region_count != 5u ||
        stats.large_free_active_bin_count != 0u ||
        stats.largest_large_free_run_regions != 0u ||
        stats.large_reuse_count != 1u) {
        fail("single-Region remainder was not absorbed");
    }
    expect_ok(nggc_check(heap),
              "verify absorbed single-Region remainder");

    expect_ok(nggc_root_pop(heap, &guard),
              "unroot humongous remainder guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot humongous remainder prefix");
    expect_ok(nggc_close(heap), "close humongous remainder heap");
}

static void verify_large_free_bin_segregation(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats free_stats;
    nggc_stats large_reuse_stats;
    nggc_stats consumed_stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *two_region_run;
    void *middle_guard;
    void *five_region_run;
    void *tail_guard;
    void *four_region_replacement;
    void *two_region_replacement;

    init_config(&config, (size_t)16u * region_size);
    config.region_size = region_size;
    expect_ok(nggc_open(&config, &heap),
              "open size-segregated large free heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "large_free_bin_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register large free bin type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    two_region_run = nggc_alloc_size(heap, type_id, region_size);
    middle_guard = nggc_alloc_size(heap, type_id, region_size);
    five_region_run = nggc_alloc_size(
        heap, type_id, (size_t)4u * region_size);
    tail_guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !two_region_run || !middle_guard ||
        !five_region_run || !tail_guard) {
        fail("allocate size-segregated humongous runs");
    }
    expect_ok(nggc_root_push(heap, &prefix), "root bin prefix");
    expect_ok(nggc_root_push(heap, &middle_guard),
              "root middle bin guard");
    expect_ok(nggc_root_push(heap, &tail_guard),
              "root tail bin guard");
    expect_ok(nggc_collect(heap), "reclaim differently sized runs");
    read_stats(heap, &free_stats);
    if (free_stats.large_free_object_count != 2u ||
        free_stats.large_free_bytes != (size_t)7u * region_size ||
        free_stats.large_free_active_bin_count != 2u ||
        free_stats.largest_large_free_run_regions != 5u) {
        fail("multiple large free bins accounting");
    }
    expect_ok(nggc_check(heap), "verify multiple large free bins");

    four_region_replacement = nggc_alloc_size(
        heap, type_id, (size_t)3u * region_size);
    if (!four_region_replacement ||
        four_region_replacement != five_region_run) {
        fail("large bin upward search did not find five-Region run");
    }
    read_stats(heap, &large_reuse_stats);
    if (large_reuse_stats.large_free_object_count != 1u ||
        large_reuse_stats.large_free_bytes !=
            (size_t)2u * region_size ||
        large_reuse_stats.large_free_active_bin_count != 1u ||
        large_reuse_stats.largest_large_free_run_regions != 2u ||
        large_reuse_stats.large_free_search_count <=
            free_stats.large_free_search_count ||
        large_reuse_stats.large_free_probe_count <=
            free_stats.large_free_probe_count) {
        fail("large free bin upward-search accounting");
    }

    two_region_replacement = nggc_alloc_size(
        heap, type_id, region_size);
    if (!two_region_replacement ||
        two_region_replacement != two_region_run) {
        fail("two-Region free bin was not reused");
    }
    read_stats(heap, &consumed_stats);
    if (consumed_stats.large_free_object_count != 0u ||
        consumed_stats.large_free_bytes != 0u ||
        consumed_stats.large_free_active_bin_count != 0u ||
        consumed_stats.largest_large_free_run_regions != 0u ||
        consumed_stats.large_free_search_count <=
            large_reuse_stats.large_free_search_count ||
        consumed_stats.large_free_probe_count <=
            large_reuse_stats.large_free_probe_count) {
        fail("large free bins not empty after reuse");
    }
    expect_ok(nggc_check(heap), "verify consumed large free bins");

    expect_ok(nggc_root_pop(heap, &tail_guard),
              "unroot tail bin guard");
    expect_ok(nggc_root_pop(heap, &middle_guard),
              "unroot middle bin guard");
    expect_ok(nggc_root_pop(heap, &prefix), "unroot bin prefix");
    expect_ok(nggc_close(heap), "close size-segregated large free heap");
}

static void verify_ordered_large_best_fit_case(int decommitted) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats free_stats;
    nggc_stats reuse_stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *five_region_run;
    void *middle_guard;
    void *eight_region_run;
    void *tail_guard;
    void *replacement;

    init_config(&config, (size_t)24u * region_size);
    config.region_size = region_size;
    config.interior_decommit_min_regions =
        decommitted ? 5u : NGGC_INTERIOR_DECOMMIT_DISABLED;
    expect_ok(nggc_open(&config, &heap),
              decommitted
                  ? "open decommitted ordered best-fit heap"
                  : "open committed ordered best-fit heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = decommitted
                    ? "decommitted_ordered_best_fit_leaf"
                    : "committed_ordered_best_fit_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register ordered best-fit type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    five_region_run = nggc_alloc_size(
        heap, type_id, (size_t)4u * region_size);
    middle_guard = nggc_alloc_size(heap, type_id, region_size);
    eight_region_run = nggc_alloc_size(
        heap, type_id, (size_t)7u * region_size);
    tail_guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !five_region_run || !middle_guard ||
        !eight_region_run || !tail_guard) {
        fail("allocate ordered best-fit layout");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root ordered best-fit prefix");
    expect_ok(nggc_root_push(heap, &middle_guard),
              "root ordered best-fit middle guard");
    expect_ok(nggc_root_push(heap, &tail_guard),
              "root ordered best-fit tail guard");
    expect_ok(nggc_collect(heap),
              "reclaim same-bin differently-sized runs");
    read_stats(heap, &free_stats);
    if ((decommitted &&
         (free_stats.interior_decommitted_run_count != 2u ||
          free_stats.interior_decommitted_region_count != 13u ||
          free_stats.large_free_object_count != 0u)) ||
        (!decommitted &&
         (free_stats.interior_decommitted_run_count != 0u ||
          free_stats.large_free_object_count != 2u ||
          free_stats.large_free_bytes !=
              (size_t)13u * region_size))) {
        fail("ordered best-fit free-space accounting");
    }
    expect_ok(nggc_check(heap),
              "verify ordered same-bin free runs");

    replacement = nggc_alloc_size(
        heap, type_id, (size_t)4u * region_size);
    if (!replacement || replacement != five_region_run) {
        fail("ordered bin did not select smallest fitting run");
    }
    read_stats(heap, &reuse_stats);
    if (reuse_stats.large_free_probe_count !=
            free_stats.large_free_probe_count + 1u ||
        reuse_stats.large_free_size_reject_count !=
            free_stats.large_free_size_reject_count ||
        reuse_stats.large_free_placement_reject_count !=
            free_stats.large_free_placement_reject_count ||
        (decommitted &&
         (reuse_stats.large_free_decommitted_probe_count !=
              free_stats.large_free_decommitted_probe_count + 1u ||
          reuse_stats.large_free_committed_probe_count !=
              free_stats.large_free_committed_probe_count ||
          reuse_stats.interior_decommitted_run_count != 1u ||
          reuse_stats.interior_decommitted_region_count != 8u)) ||
        (!decommitted &&
         (reuse_stats.large_free_committed_probe_count !=
              free_stats.large_free_committed_probe_count + 1u ||
          reuse_stats.large_free_decommitted_probe_count !=
              free_stats.large_free_decommitted_probe_count ||
          reuse_stats.large_free_object_count != 1u ||
          reuse_stats.large_free_bytes !=
              (size_t)8u * region_size))) {
        fail("ordered best-fit source telemetry");
    }
    expect_ok(nggc_check(heap),
              "verify ordered best-fit reuse");

    expect_ok(nggc_root_pop(heap, &tail_guard),
              "unroot ordered best-fit tail guard");
    expect_ok(nggc_root_pop(heap, &middle_guard),
              "unroot ordered best-fit middle guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot ordered best-fit prefix");
    expect_ok(nggc_close(heap),
              "close ordered best-fit heap");
}

static void verify_large_bin_placement_filter_telemetry(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc movable_desc;
    nggc_type_desc pinned_desc;
    nggc_stats free_stats;
    nggc_stats reuse_stats;
    nggc_heap *heap = 0;
    uint16_t movable_type = NGGC_TYPE_NONE;
    uint16_t pinned_type = NGGC_TYPE_NONE;
    void *prefix;
    void *pinned_run;
    void *middle_guard;
    void *movable_run;
    void *tail_guard;
    void *replacement;

    init_config(&config, (size_t)24u * region_size);
    config.region_size = region_size;
    config.interior_decommit_min_regions =
        NGGC_INTERIOR_DECOMMIT_DISABLED;
    expect_ok(nggc_open(&config, &heap),
              "open large placement-filter telemetry heap");
    memset(&movable_desc, 0, sizeof(movable_desc));
    movable_desc.size = sizeof(movable_desc);
    movable_desc.name = "large_placement_filter_movable";
    expect_ok(nggc_register_type(heap, &movable_desc, &movable_type),
              "register placement-filter movable type");
    memset(&pinned_desc, 0, sizeof(pinned_desc));
    pinned_desc.size = sizeof(pinned_desc);
    pinned_desc.flags = NGGC_TYPE_FLAG_PINNABLE |
                        NGGC_TYPE_FLAG_NON_MOVABLE;
    pinned_desc.name = "large_placement_filter_pinned";
    expect_ok(nggc_register_type(heap, &pinned_desc, &pinned_type),
              "register placement-filter pinned type");

    prefix = nggc_alloc_size(heap, movable_type, 512u);
    pinned_run = nggc_alloc_size(
        heap, pinned_type, (size_t)4u * region_size);
    middle_guard = nggc_alloc_size(heap, movable_type, region_size);
    movable_run = nggc_alloc_size(
        heap, movable_type, (size_t)4u * region_size);
    tail_guard = nggc_alloc_size(heap, movable_type, region_size);
    if (!prefix || !pinned_run || !middle_guard ||
        !movable_run || !tail_guard) {
        fail("allocate placement-filter telemetry layout");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root placement-filter prefix");
    expect_ok(nggc_root_push(heap, &middle_guard),
              "root placement-filter middle guard");
    expect_ok(nggc_root_push(heap, &tail_guard),
              "root placement-filter tail guard");
    expect_ok(nggc_collect(heap),
              "reclaim mixed-placement same-size runs");
    read_stats(heap, &free_stats);
    if (free_stats.large_free_object_count != 2u ||
        free_stats.large_free_bytes != (size_t)10u * region_size) {
        fail("placement-filter free-space accounting");
    }

    replacement = nggc_alloc_size(
        heap, movable_type, (size_t)4u * region_size);
    if (!replacement || replacement != movable_run ||
        replacement == pinned_run) {
        fail("ordered bin ignored placement compatibility");
    }
    read_stats(heap, &reuse_stats);
    if (reuse_stats.large_free_probe_count !=
            free_stats.large_free_probe_count + 2u ||
        reuse_stats.large_free_committed_probe_count !=
            free_stats.large_free_committed_probe_count + 2u ||
        reuse_stats.large_free_decommitted_probe_count !=
            free_stats.large_free_decommitted_probe_count ||
        reuse_stats.large_free_size_reject_count !=
            free_stats.large_free_size_reject_count ||
        reuse_stats.large_free_placement_reject_count !=
            free_stats.large_free_placement_reject_count + 1u) {
        fail("placement-filter probe telemetry");
    }
    expect_ok(nggc_check(heap),
              "verify placement-filter telemetry heap");

    expect_ok(nggc_root_pop(heap, &tail_guard),
              "unroot placement-filter tail guard");
    expect_ok(nggc_root_pop(heap, &middle_guard),
              "unroot placement-filter middle guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot placement-filter prefix");
    expect_ok(nggc_close(heap),
              "close placement-filter telemetry heap");
}

static void verify_interior_humongous_decommit(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats reclaimed_stats;
    nggc_stats split_stats;
    nggc_stats consumed_stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *ten_region_run;
    void *guard;
    void *four_region_replacement;
    void *six_region_remainder;
    void *stale_large;

    init_config(&config, (size_t)24u * region_size);
    config.region_size = region_size;
    expect_ok(nggc_open(&config, &heap),
              "open interior humongous decommit heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "interior_decommitted_humongous_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register interior decommit type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    ten_region_run = nggc_alloc_size(
        heap, type_id, (size_t)9u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !ten_region_run || !guard) {
        fail("allocate isolated ten-Region humongous run");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root interior decommit prefix");
    expect_ok(nggc_root_push(heap, &guard),
              "root interior decommit guard");
    expect_ok(nggc_collect(heap),
              "decommit isolated interior humongous run");
    stale_large = ten_region_run;
    if (nggc_load(heap, &stale_large) != 0 ||
        nggc_get_last_error(heap) != NGGC_ERR_CORRUPTION) {
        fail("barrier touched or accepted an interior decommitted object");
    }
    read_stats(heap, &reclaimed_stats);
    if (reclaimed_stats.live_object_count != 2u ||
        reclaimed_stats.large_object_count != 1u ||
        reclaimed_stats.large_free_object_count != 0u ||
        reclaimed_stats.large_free_bytes != 0u ||
        reclaimed_stats.interior_decommitted_run_count != 1u ||
        reclaimed_stats.interior_decommitted_region_count != 10u ||
        reclaimed_stats.interior_decommitted_bytes !=
            (size_t)10u * region_size ||
        reclaimed_stats.interior_decommit_min_regions !=
            NGGC_INTERIOR_DECOMMIT_MIN_REGIONS_DEFAULT ||
        reclaimed_stats.interior_decommit_policy !=
            NGGC_INTERIOR_DECOMMIT_POLICY_SMART ||
        reclaimed_stats.interior_decommit_smart_evaluation_count != 1u ||
        reclaimed_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_HOLD ||
        reclaimed_stats.decommitted_large_free_active_bin_count != 1u ||
        reclaimed_stats.interior_run_decommit_count != 1u ||
        reclaimed_stats.interior_run_recommit_count != 0u ||
        reclaimed_stats.humongous_run_count != 2u ||
        reclaimed_stats.humongous_live_run_count != 1u ||
        reclaimed_stats.humongous_free_run_count != 0u ||
        reclaimed_stats.largest_large_free_run_regions != 10u ||
        reclaimed_stats.region_decommitted_bytes <
            reclaimed_stats.interior_decommitted_bytes ||
        reclaimed_stats.uhm_decommitted_bytes <
            reclaimed_stats.region_decommitted_bytes) {
        fail("interior humongous decommit accounting");
    }
    expect_ok(nggc_check(heap),
              "verify interior decommitted pseudo-block");
    expect_ok(nggc_collect(heap),
              "rescan persistent interior decommitted run");
    expect_ok(nggc_check(heap),
              "verify persistent interior decommitted run");

    four_region_replacement = nggc_alloc_size(
        heap, type_id, (size_t)3u * region_size);
    if (!four_region_replacement ||
        four_region_replacement != ten_region_run) {
        fail("partial recommit did not reuse interior run prefix");
    }
    read_stats(heap, &split_stats);
    if (split_stats.interior_decommitted_run_count != 1u ||
        split_stats.interior_decommitted_region_count != 6u ||
        split_stats.interior_decommitted_bytes !=
            (size_t)6u * region_size ||
        split_stats.decommitted_large_free_active_bin_count != 1u ||
        split_stats.interior_run_decommit_count != 1u ||
        split_stats.interior_run_recommit_count != 1u ||
        split_stats.humongous_run_count != 3u ||
        split_stats.humongous_live_run_count != 2u ||
        split_stats.humongous_free_run_count != 0u ||
        split_stats.largest_large_free_run_regions != 6u ||
        split_stats.large_reuse_count !=
            reclaimed_stats.large_reuse_count + 1u ||
        split_stats.region_recommit_count <
            reclaimed_stats.region_recommit_count + 4u) {
        fail("partial interior humongous recommit accounting");
    }
    expect_ok(nggc_check(heap),
              "verify split decommitted humongous run");

    six_region_remainder = nggc_alloc_size(
        heap, type_id, (size_t)5u * region_size);
    if (!six_region_remainder ||
        six_region_remainder !=
            (void *)((unsigned char *)ten_region_run +
                     (size_t)4u * region_size)) {
        fail("recommit did not consume decommitted run remainder");
    }
    read_stats(heap, &consumed_stats);
    if (consumed_stats.interior_decommitted_run_count != 0u ||
        consumed_stats.interior_decommitted_region_count != 0u ||
        consumed_stats.interior_decommitted_bytes != 0u ||
        consumed_stats.decommitted_large_free_active_bin_count != 0u ||
        consumed_stats.interior_run_decommit_count != 1u ||
        consumed_stats.interior_run_recommit_count != 2u ||
        consumed_stats.humongous_run_count != 3u ||
        consumed_stats.humongous_live_run_count != 3u ||
        consumed_stats.humongous_free_run_count != 0u ||
        consumed_stats.largest_large_free_run_regions != 0u ||
        consumed_stats.large_reuse_count !=
            split_stats.large_reuse_count + 1u ||
        consumed_stats.region_recommit_count <
            split_stats.region_recommit_count + 6u) {
        fail("complete interior humongous recommit accounting");
    }
    expect_ok(nggc_check(heap),
              "verify consumed decommitted humongous run");

    expect_ok(nggc_root_pop(heap, &guard),
              "unroot interior decommit guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot interior decommit prefix");
    expect_ok(nggc_close(heap),
              "close interior humongous decommit heap");
}

static void verify_interior_decommit_threshold_case(
    size_t configured_threshold, size_t expected_effective_threshold,
    int expect_decommitted) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats stats;
    uint32_t expected_policy;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *four_region_run;
    void *guard;

    init_config(&config, (size_t)12u * region_size);
    config.region_size = region_size;
    config.interior_decommit_min_regions = configured_threshold;
    expect_ok(nggc_open(&config, &heap),
              "open configurable interior decommit heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "configurable_interior_decommit_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register configurable interior decommit type");
    prefix = nggc_alloc_size(heap, type_id, 512u);
    four_region_run = nggc_alloc_size(
        heap, type_id, (size_t)3u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !four_region_run || !guard) {
        fail("allocate configurable interior decommit run");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root configurable interior prefix");
    expect_ok(nggc_root_push(heap, &guard),
              "root configurable interior guard");
    expect_ok(nggc_collect(heap),
              "collect configurable interior run");
    read_stats(heap, &stats);
    expected_policy =
        configured_threshold == NGGC_INTERIOR_DECOMMIT_DISABLED
            ? NGGC_INTERIOR_DECOMMIT_POLICY_DISABLED
            : NGGC_INTERIOR_DECOMMIT_POLICY_MANUAL;
    if (stats.interior_decommit_min_regions !=
            expected_effective_threshold ||
        stats.interior_decommit_policy != expected_policy ||
        stats.interior_decommit_smart_evaluation_count != 0u ||
        (expect_decommitted &&
         (stats.interior_decommitted_run_count != 1u ||
          stats.interior_decommitted_region_count != 4u ||
          stats.interior_decommitted_bytes !=
              (size_t)4u * region_size ||
          stats.large_free_object_count != 0u ||
          stats.interior_run_decommit_count != 1u)) ||
        (!expect_decommitted &&
         (stats.interior_decommitted_run_count != 0u ||
          stats.interior_decommitted_region_count != 0u ||
          stats.interior_decommitted_bytes != 0u ||
          stats.large_free_object_count != 1u ||
          stats.large_free_bytes != (size_t)4u * region_size ||
          stats.interior_run_decommit_count != 0u))) {
        fail("configurable interior decommit threshold accounting");
    }
    expect_ok(nggc_check(heap),
              "verify configurable interior decommit threshold");
    expect_ok(nggc_root_pop(heap, &guard),
              "unroot configurable interior guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot configurable interior prefix");
    expect_ok(nggc_close(heap),
              "close configurable interior decommit heap");
}

static void verify_smart_interior_decommit_hysteresis(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats warmup_stats;
    nggc_stats lowered_stats;
    nggc_stats raised_stats;
    nggc_stats cooldown_stats;
    nggc_stats redecommitted_stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *four_region_run;
    void *guard;
    void *replacement;

    init_config(&config, (size_t)12u * region_size);
    config.region_size = region_size;
    expect_ok(nggc_open(&config, &heap),
              "open SMART interior decommit heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "smart_interior_decommit_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register SMART interior decommit type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    four_region_run = nggc_alloc_size(
        heap, type_id, (size_t)3u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !four_region_run || !guard) {
        fail("allocate SMART interior decommit layout");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root SMART interior prefix");
    expect_ok(nggc_root_push(heap, &guard),
              "root SMART interior guard");

    expect_ok(nggc_collect(heap),
              "warm up SMART interior controller");
    read_stats(heap, &warmup_stats);
    if (warmup_stats.interior_decommit_policy !=
            NGGC_INTERIOR_DECOMMIT_POLICY_SMART ||
        warmup_stats.interior_decommit_min_regions !=
            NGGC_INTERIOR_DECOMMIT_MIN_REGIONS_DEFAULT ||
        warmup_stats.interior_decommit_smart_evaluation_count != 1u ||
        warmup_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_HOLD ||
        warmup_stats.interior_decommit_smart_lower_count != 0u ||
        warmup_stats.interior_decommit_smart_raise_count != 0u ||
        warmup_stats.interior_decommitted_run_count != 0u ||
        warmup_stats.large_free_object_count != 1u ||
        warmup_stats.interior_decommit_smart_large_free_pressure_basis_points <
            5000u) {
        fail("SMART interior warmup accounting");
    }

    expect_ok(nggc_collect(heap),
              "lower SMART interior threshold under pressure");
    read_stats(heap, &lowered_stats);
    if (lowered_stats.interior_decommit_min_regions != 4u ||
        lowered_stats.interior_decommit_smart_evaluation_count != 2u ||
        lowered_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_LOWER ||
        lowered_stats.interior_decommit_smart_lower_count != 1u ||
        lowered_stats.interior_decommit_smart_raise_count != 0u ||
        lowered_stats.interior_decommit_smart_cooldown_remaining != 2u ||
        lowered_stats.interior_decommitted_run_count != 1u ||
        lowered_stats.interior_decommitted_region_count != 4u ||
        lowered_stats.large_free_object_count != 0u) {
        fail("SMART pressure lowering and decommit accounting");
    }
    expect_ok(nggc_check(heap),
              "verify SMART pressure-decommitted run");

    replacement = nggc_alloc_size(
        heap, type_id, (size_t)3u * region_size);
    if (!replacement || replacement != four_region_run) {
        fail("SMART recommit did not reuse original run");
    }
    expect_ok(nggc_collect(heap),
              "raise SMART threshold after recommit churn");
    read_stats(heap, &raised_stats);
    if (raised_stats.interior_decommit_min_regions != 8u ||
        raised_stats.interior_decommit_smart_evaluation_count != 3u ||
        raised_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_RAISE ||
        raised_stats.interior_decommit_smart_raise_count != 1u ||
        raised_stats.interior_decommit_smart_lower_count != 1u ||
        raised_stats.interior_decommit_smart_last_recommit_delta != 1u ||
        raised_stats.interior_decommit_smart_last_large_allocation_delta !=
            1u ||
        raised_stats.interior_decommit_smart_last_large_reuse_delta != 1u ||
        raised_stats.interior_decommit_smart_cooldown_remaining != 2u ||
        raised_stats.interior_decommitted_run_count != 0u ||
        raised_stats.large_free_object_count != 1u ||
        raised_stats.interior_run_decommit_count != 1u ||
        raised_stats.interior_run_recommit_count != 1u) {
        fail("SMART recommit-churn raise accounting");
    }

    expect_ok(nggc_collect(heap),
              "hold SMART threshold for first cooldown cycle");
    expect_ok(nggc_collect(heap),
              "hold SMART threshold for second cooldown cycle");
    read_stats(heap, &cooldown_stats);
    if (cooldown_stats.interior_decommit_min_regions != 8u ||
        cooldown_stats.interior_decommit_smart_evaluation_count != 5u ||
        cooldown_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_COOLDOWN ||
        cooldown_stats.interior_decommit_smart_cooldown_remaining != 0u ||
        cooldown_stats.interior_decommit_smart_cooldown_hold_count != 2u ||
        cooldown_stats.interior_decommitted_run_count != 0u ||
        cooldown_stats.large_free_object_count != 1u) {
        fail("SMART cooldown hysteresis accounting");
    }

    expect_ok(nggc_collect(heap),
              "redecommit cold run after SMART cooldown");
    read_stats(heap, &redecommitted_stats);
    if (redecommitted_stats.interior_decommit_min_regions != 4u ||
        redecommitted_stats.interior_decommit_smart_evaluation_count != 6u ||
        redecommitted_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_LOWER ||
        redecommitted_stats.interior_decommit_smart_lower_count != 2u ||
        redecommitted_stats.interior_decommit_smart_raise_count != 1u ||
        redecommitted_stats.interior_decommitted_run_count != 1u ||
        redecommitted_stats.interior_decommitted_region_count != 4u ||
        redecommitted_stats.interior_run_decommit_count != 2u ||
        redecommitted_stats.interior_run_recommit_count != 1u) {
        fail("SMART post-cooldown redecommit accounting");
    }
    expect_ok(nggc_check(heap),
              "verify SMART hysteresis final state");

    expect_ok(nggc_root_pop(heap, &guard),
              "unroot SMART interior guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot SMART interior prefix");
    expect_ok(nggc_close(heap),
              "close SMART interior decommit heap");
}

static void verify_smart_external_pressure_fallback(void) {
    const size_t region_size = (size_t)8192u;
    memory_pressure_test_ctx pressure_ctx;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats callback_stats;
    nggc_stats fallback_stats;
    nggc_stats invalid_stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *four_region_run;
    void *middle_guard;
    void *live_tail;

    memset(&pressure_ctx, 0, sizeof(pressure_ctx));
    pressure_ctx.pressure_basis_points = 1000u;
    pressure_ctx.resident_bytes = (size_t)100u * 1024u;
    pressure_ctx.limit_bytes = (size_t)1024u * 1024u;
    init_config(&config, (size_t)32u * region_size);
    config.region_size = region_size;
    config.memory_pressure = sample_memory_pressure;
    config.memory_pressure_ctx = &pressure_ctx;
    expect_ok(nggc_open(&config, &heap),
              "open external-pressure SMART heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "smart_external_pressure_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register external-pressure SMART type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    four_region_run = nggc_alloc_size(
        heap, type_id, (size_t)3u * region_size);
    middle_guard = nggc_alloc_size(heap, type_id, region_size);
    live_tail = nggc_alloc_size(
        heap, type_id, (size_t)18u * region_size);
    if (!prefix || !four_region_run || !middle_guard || !live_tail) {
        fail("allocate external-pressure SMART layout");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root external-pressure prefix");
    expect_ok(nggc_root_push(heap, &middle_guard),
              "root external-pressure middle guard");
    expect_ok(nggc_root_push(heap, &live_tail),
              "root external-pressure live tail");

    expect_ok(nggc_collect(heap),
              "warm up external-pressure SMART controller");
    expect_ok(nggc_collect(heap),
              "hold threshold under low external pressure");
    read_stats(heap, &callback_stats);
    if (callback_stats.interior_decommit_min_regions != 8u ||
        callback_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_HOLD ||
        callback_stats.interior_decommit_smart_pressure_source !=
            NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_CALLBACK ||
        callback_stats.interior_decommit_smart_external_pressure_valid !=
            1u ||
        callback_stats.interior_decommit_smart_external_pressure_basis_points !=
            1000u ||
        callback_stats.interior_decommit_smart_resident_bytes !=
            pressure_ctx.resident_bytes ||
        callback_stats.interior_decommit_smart_memory_limit_bytes !=
            pressure_ctx.limit_bytes ||
        callback_stats.interior_decommit_smart_external_sample_count != 2u ||
        callback_stats.interior_decommit_smart_external_failure_count != 0u ||
        callback_stats.interior_decommitted_run_count != 0u ||
        callback_stats.interior_decommit_smart_committed_pressure_basis_points <
            7500u ||
        callback_stats.interior_decommit_smart_large_free_pressure_basis_points >=
            2500u) {
        fail("low external pressure did not override portable pressure");
    }

    pressure_ctx.fail = 1u;
    expect_ok(nggc_collect(heap),
              "fall back after external-pressure callback failure");
    read_stats(heap, &fallback_stats);
    if (fallback_stats.interior_decommit_min_regions != 4u ||
        fallback_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_LOWER ||
        fallback_stats.interior_decommit_smart_pressure_source !=
            NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_PORTABLE ||
        fallback_stats.interior_decommit_smart_external_pressure_valid !=
            0u ||
        fallback_stats.interior_decommit_smart_external_pressure_basis_points !=
            0u ||
        fallback_stats.interior_decommit_smart_resident_bytes != 0u ||
        fallback_stats.interior_decommit_smart_memory_limit_bytes != 0u ||
        fallback_stats.interior_decommit_smart_external_sample_count != 2u ||
        fallback_stats.interior_decommit_smart_external_failure_count != 1u ||
        fallback_stats.interior_decommitted_run_count != 1u ||
        fallback_stats.interior_decommitted_region_count != 4u ||
        pressure_ctx.call_count != 3u) {
        fail("external-pressure failure did not select portable fallback");
    }

    pressure_ctx.fail = 2u;
    expect_ok(nggc_collect(heap),
              "fall back after invalid external-pressure sample");
    read_stats(heap, &invalid_stats);
    if (invalid_stats.interior_decommit_smart_pressure_source !=
            NGGC_INTERIOR_DECOMMIT_PRESSURE_SOURCE_PORTABLE ||
        invalid_stats.interior_decommit_smart_external_pressure_valid !=
            0u ||
        invalid_stats.interior_decommit_smart_external_sample_count != 2u ||
        invalid_stats.interior_decommit_smart_external_failure_count != 2u ||
        invalid_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_COOLDOWN ||
        pressure_ctx.call_count != 4u) {
        fail("invalid external-pressure sample did not select fallback");
    }
    expect_ok(nggc_check(heap),
              "verify external-pressure fallback heap");

    expect_ok(nggc_root_pop(heap, &live_tail),
              "unroot external-pressure live tail");
    expect_ok(nggc_root_pop(heap, &middle_guard),
              "unroot external-pressure middle guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot external-pressure prefix");
    expect_ok(nggc_close(heap),
              "close external-pressure SMART heap");
}

static void verify_smart_vm_latency_feedback(void) {
    const size_t region_size = (size_t)8192u;
    clock_test_ctx clock_ctx;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats decommitted_stats;
    nggc_stats raised_stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *eight_region_run;
    void *guard;
    void *replacement;

    memset(&clock_ctx, 0, sizeof(clock_ctx));
    clock_ctx.step_ns = UINT64_C(8000000);
    init_config(&config, (size_t)24u * region_size);
    config.region_size = region_size;
    config.clock_ns = sample_clock_ns;
    config.clock_ns_ctx = &clock_ctx;
    config.interior_decommit_slow_ns_per_region = 1u;
    expect_ok(nggc_open(&config, &heap),
              "open VM-latency SMART heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "smart_vm_latency_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register VM-latency SMART type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    eight_region_run = nggc_alloc_size(
        heap, type_id, (size_t)7u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !eight_region_run || !guard) {
        fail("allocate VM-latency SMART layout");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root VM-latency prefix");
    expect_ok(nggc_root_push(heap, &guard),
              "root VM-latency guard");

    expect_ok(nggc_collect(heap),
              "decommit run with deterministic VM latency");
    read_stats(heap, &decommitted_stats);
    if (decommitted_stats.interior_decommitted_run_count != 1u ||
        decommitted_stats.interior_decommitted_region_count != 8u ||
        decommitted_stats.region_decommit_latency_sample_count == 0u ||
        decommitted_stats.region_decommit_latency_ewma_ns_per_region == 0u ||
        decommitted_stats.interior_decommit_slow_ns_per_region != 1u ||
        decommitted_stats.interior_decommit_smart_slow_vm_observed != 0u) {
        fail("deterministic decommit latency telemetry");
    }

    clock_ctx.fail = 1u;
    replacement = nggc_alloc_size(
        heap, type_id, (size_t)7u * region_size);
    if (!replacement || replacement != eight_region_run) {
        fail("clock failure prevented interior recommit");
    }
    expect_ok(nggc_collect(heap),
              "raise SMART threshold after slow VM sample");
    read_stats(heap, &raised_stats);
    if (raised_stats.interior_decommit_min_regions != 16u ||
        raised_stats.interior_decommit_smart_last_action !=
            NGGC_INTERIOR_DECOMMIT_SMART_ACTION_RAISE ||
        raised_stats.interior_decommit_smart_slow_vm_observed != 1u ||
        raised_stats.interior_decommit_smart_last_decommit_latency_sample_delta ==
            0u ||
        raised_stats.interior_decommit_smart_last_recommit_latency_sample_delta !=
            0u ||
        raised_stats.region_recommit_latency_sample_count != 0u ||
        raised_stats.region_vm_clock_failure_count == 0u ||
        raised_stats.interior_decommitted_run_count != 0u ||
        raised_stats.large_free_object_count != 1u) {
        fail("slow VM latency did not raise SMART threshold");
    }
    expect_ok(nggc_check(heap),
              "verify VM-latency SMART heap");

    expect_ok(nggc_root_pop(heap, &guard),
              "unroot VM-latency guard");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot VM-latency prefix");
    expect_ok(nggc_close(heap),
              "close VM-latency SMART heap");
}

static void verify_tail_adjacent_interior_collapse(void) {
    const size_t region_size = (size_t)8192u;
    nggc_config config;
    nggc_type_desc desc;
    nggc_stats interior_stats;
    nggc_stats collapsed_stats;
    nggc_stats reused_stats;
    nggc_heap *heap = 0;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *prefix;
    void *eight_region_run;
    void *nine_region_run;
    void *guard;
    void *replacement;

    init_config(&config, (size_t)32u * region_size);
    config.region_size = region_size;
    expect_ok(nggc_open(&config, &heap),
              "open tail-adjacent interior collapse heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "tail_adjacent_interior_collapse_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register tail-adjacent collapse type");

    prefix = nggc_alloc_size(heap, type_id, 512u);
    eight_region_run = nggc_alloc_size(
        heap, type_id, (size_t)7u * region_size);
    nine_region_run = nggc_alloc_size(
        heap, type_id, (size_t)8u * region_size);
    guard = nggc_alloc_size(heap, type_id, region_size);
    if (!prefix || !eight_region_run || !nine_region_run || !guard) {
        fail("allocate adjacent interior runs and guard");
    }
    expect_ok(nggc_root_push(heap, &prefix),
              "root tail-adjacent collapse prefix");
    expect_ok(nggc_root_push(heap, &guard),
              "root tail-adjacent collapse guard");

    expect_ok(nggc_collect(heap),
              "decommit adjacent interior humongous runs");
    read_stats(heap, &interior_stats);
    if (interior_stats.interior_decommitted_run_count != 2u ||
        interior_stats.interior_decommitted_region_count != 17u ||
        interior_stats.interior_decommitted_bytes !=
            (size_t)17u * region_size ||
        interior_stats.interior_run_decommit_count != 2u ||
        interior_stats.interior_run_tail_collapse_count != 0u ||
        interior_stats.humongous_free_run_count != 0u ||
        interior_stats.humongous_run_count != 3u) {
        fail("adjacent interior run decommit accounting");
    }
    expect_ok(nggc_check(heap),
              "verify adjacent interior side runs");

    expect_ok(nggc_root_pop(heap, &guard),
              "unroot tail-adjacent collapse guard");
    expect_ok(nggc_collect(heap),
              "collapse tail-adjacent interior side runs");
    read_stats(heap, &collapsed_stats);
    if (collapsed_stats.gc_arena_used != region_size ||
        collapsed_stats.interior_decommitted_run_count != 0u ||
        collapsed_stats.interior_decommitted_region_count != 0u ||
        collapsed_stats.interior_decommitted_bytes != 0u ||
        collapsed_stats.decommitted_large_free_active_bin_count != 0u ||
        collapsed_stats.interior_run_tail_collapse_count != 2u ||
        collapsed_stats.interior_run_tail_collapsed_region_count != 17u ||
        collapsed_stats.interior_run_tail_collapsed_bytes !=
            (uint64_t)((size_t)17u * region_size) ||
        collapsed_stats.humongous_run_count != 0u ||
        collapsed_stats.large_free_object_count != 0u ||
        collapsed_stats.region_decommitted_bytes !=
            (size_t)31u * region_size) {
        fail("tail-adjacent interior collapse accounting");
    }
    expect_ok(nggc_check(heap),
              "verify collapsed interior runs as tail state");

    replacement = nggc_alloc_size(
        heap, type_id, (size_t)16u * region_size);
    if (!replacement || replacement != eight_region_run) {
        fail("bump allocation did not reuse collapsed tail address");
    }
    read_stats(heap, &reused_stats);
    if (reused_stats.gc_arena_used !=
            (size_t)18u * region_size ||
        reused_stats.interior_decommitted_run_count != 0u ||
        reused_stats.interior_run_recommit_count != 0u ||
        reused_stats.interior_run_tail_collapse_count != 2u ||
        reused_stats.humongous_run_count != 1u ||
        reused_stats.humongous_live_run_count != 1u ||
        reused_stats.region_recommit_count <
            collapsed_stats.region_recommit_count + 17u) {
        fail("collapsed tail bump recommit accounting");
    }
    expect_ok(nggc_check(heap),
              "verify allocation across collapsed tail runs");
    expect_ok(nggc_root_pop(heap, &prefix),
              "unroot tail-adjacent collapse prefix");
    expect_ok(nggc_close(heap),
              "close tail-adjacent interior collapse heap");
}

int main(void) {
    nggc_config config;
    nggc_type_desc desc;
    nggc_type_desc pinned_desc;
    nggc_stats stats;
    nggc_heap *heap = 0;
    nggc_heap *padding_heap = 0;
    void *large;
    void *replacement;
    void *pinned;
    uint16_t type_id = NGGC_TYPE_NONE;
    uint16_t pinned_type_id = NGGC_TYPE_NONE;
    void *padding_a;
    void *padding_b;
    void *padding_reuse;

    verify_custom_region_size((size_t)512u,
                              (size_t)8u * 1024u,
                              (size_t)128u);
    verify_custom_region_size((size_t)8u * 1024u,
                              (size_t)128u * 1024u,
                              (size_t)4u * 1024u);
    verify_custom_region_size((size_t)24u * 1024u,
                              (size_t)192u * 1024u,
                              (size_t)12u * 1024u);
    verify_custom_region_size((size_t)128u * 1024u,
                              (size_t)512u * 1024u,
                              (size_t)96u * 1024u);
    verify_custom_region_size((size_t)1024u * 1024u,
                              (size_t)4u * 1024u * 1024u,
                              (size_t)768u * 1024u);
    verify_humongous_run_space();
    verify_humongous_single_region_remainder_absorbed();
    verify_large_free_bin_segregation();
    verify_ordered_large_best_fit_case(0);
    verify_ordered_large_best_fit_case(1);
    verify_large_bin_placement_filter_telemetry();
    verify_interior_humongous_decommit();
    verify_interior_decommit_threshold_case(4u, 4u, 1);
    verify_interior_decommit_threshold_case(5u, 5u, 0);
    verify_interior_decommit_threshold_case(
        NGGC_INTERIOR_DECOMMIT_DISABLED, 0u, 0);
    verify_smart_interior_decommit_hysteresis();
    verify_smart_external_pressure_fallback();
    verify_smart_vm_latency_feedback();
    verify_tail_adjacent_interior_collapse();
    verify_special_region_placement_isolation();
    expect_interior_decommit_config_rejected(1u);
    expect_remset_buffer_config_rejected(17u);
    verify_smart_callback_config_validation();
    expect_region_config_rejected(64u, (size_t)256u * 1024u, 0u);
    expect_region_config_rejected(513u, (size_t)256u * 1024u, 0u);
    expect_region_config_rejected(576u, (size_t)256u * 1024u, 0u);
    expect_region_config_rejected((size_t)16u * 1024u,
                                  (size_t)256u * 1024u + 1u, 0u);
    expect_region_config_rejected((size_t)16u * 1024u,
                                  (size_t)256u * 1024u,
                                  (size_t)32u * 1024u);
    expect_region_config_rejected((SIZE_MAX / 512u) * 512u,
                                  (size_t)256u * 1024u, 0u);
    expect_tlab_sizing_config_rejected(3u, 0u, 0u, 0u);
    expect_tlab_sizing_config_rejected(
        NGGC_TLAB_SIZING_POLICY_SMART,
        (size_t)4u * 1024u, 2000u, (size_t)16u * 1024u);
    expect_tlab_sizing_config_rejected(
        NGGC_TLAB_SIZING_POLICY_SMART,
        (size_t)4u * 1024u, (size_t)2u * 1024u,
        (size_t)32u * 1024u);
    expect_tlab_sizing_config_rejected(
        NGGC_TLAB_SIZING_POLICY_SMART,
        (size_t)4u * 1024u, (size_t)8u * 1024u,
        (size_t)16u * 1024u);
    expect_tlab_sizing_config_rejected(
        NGGC_TLAB_SIZING_POLICY_SMART,
        (size_t)16u * 1024u, (size_t)2u * 1024u,
        (size_t)8u * 1024u);
    expect_tlab_sizing_config_rejected(
        NGGC_TLAB_SIZING_POLICY_FIXED,
        (size_t)4u * 1024u, (size_t)2u * 1024u, 0u);

    init_config(&config, (size_t)256u * 1024u);
    expect_ok(nggc_open(&config, &heap), "open region heap");

    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.object_size = 0u;
    desc.name = "cross_region_leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register cross-region type");
    memset(&pinned_desc, 0, sizeof(pinned_desc));
    pinned_desc.size = sizeof(pinned_desc);
    pinned_desc.flags = NGGC_TYPE_FLAG_PINNABLE |
                        NGGC_TYPE_FLAG_NON_MOVABLE;
    pinned_desc.object_size = 1024u;
    pinned_desc.name = "pinned_region_leaf";
    expect_ok(nggc_register_type(heap, &pinned_desc, &pinned_type_id),
              "register pinned region type");

    large = nggc_alloc_size(heap, type_id, (size_t)96u * 1024u);
    if (!large) {
        fail("allocate cross-region object");
    }
    expect_ok(nggc_root_push(heap, &large), "root cross-region object");
    pinned = nggc_alloc(heap, pinned_type_id);
    if (!pinned) {
        fail("allocate pinned region object");
    }
    expect_ok(nggc_root_push(heap, &pinned), "root pinned region object");
    read_stats(heap, &stats);
    if (stats.region_size != (size_t)64u * 1024u ||
        stats.region_count != 4u || stats.active_region_count < 2u ||
        stats.live_object_count != 2u || stats.live_region_count < 2u ||
        stats.large_region_count < 2u || stats.pinned_region_count == 0u ||
        stats.non_movable_region_count == 0u ||
        stats.collection_set_region_count != 0u ||
        stats.young_region_count != 0u || stats.old_region_count != 0u ||
        stats.special_region_count < 2u ||
        stats.young_object_count != 2u || stats.old_object_count != 0u ||
        stats.allocator_owned_region_count != 1u) {
        fail("initial region accounting");
    }
    expect_ok(nggc_check(heap), "verify cross-region allocation");
    expect_ok(nggc_collect_young(heap),
              "age rooted special objects once");
    read_stats(heap, &stats);
    if (stats.young_object_count != 2u || stats.old_object_count != 0u ||
        stats.young_survivor_object_count != 2u ||
        stats.young_in_place_promoted_object_count != 0u ||
        stats.old_region_count != 0u || stats.special_region_count < 2u) {
        fail("special objects remain young before threshold");
    }
    expect_ok(nggc_collect_young(heap),
              "promote rooted special objects at threshold");
    read_stats(heap, &stats);
    if (stats.young_object_count != 0u || stats.old_object_count != 2u ||
        stats.young_in_place_promoted_object_count != 2u ||
        stats.young_promoted_object_count != 2u ||
        stats.old_region_count != 0u || stats.special_region_count < 2u) {
        fail("special objects promote logically without changing space");
    }
    expect_ok(nggc_check(heap), "verify special object promotion");
    expect_ok(nggc_collect(heap), "collect rooted cross-region object");
    expect_ok(nggc_check(heap), "verify rooted cross-region collection");

    expect_ok(nggc_root_pop(heap, &pinned), "unroot pinned region object");
    pinned = 0;
    expect_ok(nggc_root_pop(heap, &large), "unroot cross-region object");
    large = 0;
    expect_ok(nggc_collect(heap), "reclaim cross-region object");
    read_stats(heap, &stats);
    if (stats.live_object_count != 0u || stats.free_bytes != 0u ||
        stats.live_region_count != 0u || stats.free_region_count != 0u ||
        stats.large_region_count != 0u ||
        stats.pinned_region_count != 0u ||
        stats.non_movable_region_count != 0u ||
        stats.active_region_count != 0u ||
        stats.decommitted_region_count != stats.region_count ||
        stats.region_decommitted_bytes != config.gc_arena_size ||
        stats.region_decommit_count < stats.region_count ||
        stats.uhm_decommitted_bytes < stats.region_decommitted_bytes) {
        fail("tail Region decommit accounting");
    }
    expect_ok(nggc_check(heap), "verify reclaimed cross-region object");

    replacement = nggc_alloc_size(heap, type_id, (size_t)48u * 1024u);
    if (!replacement) {
        fail("recommit tail Region allocation");
    }
    read_stats(heap, &stats);
    if (stats.live_object_count != 1u ||
        stats.region_recommit_count == 0u ||
        stats.decommitted_region_count >= stats.region_count ||
        stats.region_decommitted_bytes >= config.gc_arena_size) {
        fail("tail Region recommit transfer");
    }
    expect_ok(nggc_check(heap), "verify tail Region recommit");
    expect_ok(nggc_close(heap), "close region heap");

    expect_ok(nggc_open(&config, &padding_heap), "open padding heap");
    expect_ok(nggc_register_type(padding_heap, &desc, &type_id),
              "register padding type");
    read_stats(padding_heap, &stats);
    if (stats.allocator_owned_region_count != 1u ||
        stats.allocator_lease_epoch != 1u ||
        stats.allocator_lease_acquire_count != 1u) {
        fail("initial allocator lease");
    }
    expect_ok(nggc_request_safepoint(padding_heap),
              "revoke allocator lease for safepoint");
    read_stats(padding_heap, &stats);
    if (stats.allocator_owned_region_count != 0u ||
        stats.allocator_lease_revoke_count != 1u) {
        fail("allocator lease revoked");
    }
    expect_ok(nggc_safepoint_release(padding_heap),
              "release allocator safepoint");
    padding_a = nggc_alloc_size(padding_heap, type_id,
                                (size_t)40u * 1024u);
    padding_b = nggc_alloc_size(padding_heap, type_id,
                                (size_t)40u * 1024u);
    if (!padding_a || !padding_b) {
        fail("allocate region-local objects");
    }
    read_stats(padding_heap, &stats);
    if (stats.region_padding_block_count != 1u ||
        stats.region_padding_created_bytes == 0u ||
        stats.free_object_count != 1u || stats.free_bytes == 0u ||
        stats.allocator_owned_region_count != 1u ||
        stats.allocator_lease_epoch != 2u ||
        stats.allocator_lease_acquire_count != 2u) {
        fail("region tail padding publication");
    }
    padding_reuse = nggc_alloc_size(padding_heap, type_id,
                                    (size_t)8u * 1024u);
    if (!padding_reuse) {
        fail("reuse region tail padding");
    }
    read_stats(padding_heap, &stats);
    if (stats.region_padding_block_count != 1u ||
        stats.free_bytes == 0u || stats.live_object_count != 3u) {
        fail("region tail padding split reuse");
    }
    expect_ok(nggc_check(padding_heap), "verify region-local padding");
    expect_ok(nggc_close(padding_heap), "close padding heap");
    return 0;
}
