#include "nggc/nggc.h"
#include "nggc_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    nggc_backend *backend = 0;
    nggc_backend_stats backend_stats;
    nggc_stats stats;
    nggc_heap *heap = 0;
    nggc_config config;
    nggc_type_desc desc;
    uint16_t type_id = NGGC_TYPE_NONE;
    void *addr = 0;

    nggc_backend_fault_reset();

    nggc_backend_fault_fail_next_create();
    backend = (nggc_backend *)1;
    expect_true(nggc_backend_create((size_t)2u * 1024u * 1024u,
                                    &backend) == NGGC_ERR_BACKEND,
                "injected backend create failure");
    expect_true(backend == 0, "failed backend create clears output");

    expect_ok(nggc_backend_create((size_t)2u * 1024u * 1024u, &backend),
              "backend create");

    nggc_backend_fault_fail_next_map();
    addr = (void *)1;
    expect_true(nggc_backend_map(backend, 4096u, &addr) == NGGC_ERR_BACKEND,
                "injected backend map failure");
    expect_true(addr == 0, "failed backend map clears output");

    expect_ok(nggc_backend_map(backend, 8192u, &addr), "backend map");
    expect_true(addr != 0, "backend map returned address");
    memset(addr, 0xa5, 32u);

    nggc_backend_fault_fail_next_stats();
    memset(&backend_stats, 0, sizeof(backend_stats));
    expect_true(nggc_backend_get_stats(backend, &backend_stats) ==
                    NGGC_ERR_BACKEND,
                "injected backend stats failure");
    expect_ok(nggc_backend_get_stats(backend, &backend_stats),
              "backend stats after injected failure");
    expect_true(backend_stats.arena_size != 0u,
                "backend stats recovered after injected failure");
    expect_true(backend_stats.page_size != 0u &&
                    backend_stats.allocation_granularity >=
                        backend_stats.page_size,
                "backend page geometry stats");

    nggc_backend_fault_fail_next_check();
    expect_true(nggc_backend_check(backend) == NGGC_ERR_BACKEND,
                "injected backend check failure");
    expect_ok(nggc_backend_check(backend),
              "backend check after injected failure");

    nggc_backend_fault_fail_next_decommit();
    expect_true(nggc_backend_decommit(backend, addr, 8192u) ==
                    NGGC_ERR_BACKEND,
                "injected backend decommit failure");
    expect_ok(nggc_backend_decommit(backend, addr, 8192u),
              "backend decommit after injected failure");
    expect_ok(nggc_backend_check(backend),
              "backend check after decommit");
    memset(&backend_stats, 0, sizeof(backend_stats));
    expect_ok(nggc_backend_get_stats(backend, &backend_stats),
              "backend stats after decommit");
    expect_true(backend_stats.decommitted_bytes >= 8192u,
                "backend decommit stats");

    nggc_backend_fault_fail_next_recommit();
    expect_true(nggc_backend_recommit(backend, addr, 8192u) ==
                    NGGC_ERR_BACKEND,
                "injected backend recommit failure");
    expect_ok(nggc_backend_recommit(backend, addr, 8192u),
              "backend recommit after injected failure");
    memset(addr, 0x5a, 32u);
    expect_ok(nggc_backend_check(backend),
              "backend check after recommit");
    memset(&backend_stats, 0, sizeof(backend_stats));
    expect_ok(nggc_backend_get_stats(backend, &backend_stats),
              "backend stats after recommit");
    expect_true(backend_stats.committed_bytes >= 8192u &&
                    backend_stats.mmap_live_bytes >= 8192u,
                "backend recommit stats");

    nggc_backend_fault_fail_next_unmap();
    expect_true(nggc_backend_unmap(backend, addr, 8192u) ==
                    NGGC_ERR_BACKEND,
                "injected backend unmap failure");
    expect_ok(nggc_backend_unmap(backend, addr, 8192u),
              "backend unmap after injected failure");
    addr = 0;

    expect_ok(nggc_backend_destroy(backend), "backend destroy");
    backend = 0;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.gc_arena_size = 4096u;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    nggc_backend_fault_fail_next_map();
    heap = (nggc_heap *)1;
    expect_true(nggc_open(&config, &heap) == NGGC_ERR_BACKEND,
                "public open reports injected map failure");
    expect_true(heap == 0, "public open clears heap on injected map failure");

    nggc_backend_fault_reset();
    heap = 0;
    expect_ok(nggc_open(&config, &heap), "public open for backend check");
    memset(&stats, 0, sizeof(stats));
    stats.size = sizeof(stats);
    expect_ok(nggc_get_stats(heap, &stats),
              "public stats include backend geometry");
    expect_true(stats.uhm_page_size != 0u &&
                    stats.uhm_allocation_granularity >=
                        stats.uhm_page_size &&
                    stats.uhm_committed_bytes >= config.gc_arena_size,
                "public backend geometry stats");
    nggc_backend_fault_fail_next_check();
    expect_true(nggc_check(heap) == NGGC_ERR_BACKEND,
                "public check reports injected backend check failure");
    expect_true(nggc_get_last_error(heap) == NGGC_ERR_BACKEND,
                "public check records backend failure");
    expect_ok(nggc_check(heap), "public check recovers after backend fault");
    expect_ok(nggc_close(heap), "public close after backend check fault");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.region_size = (size_t)8192u;
    config.gc_arena_size = (size_t)24u * config.region_size;
    config.uhm_arena_size = (size_t)2u * 1024u * 1024u;
    heap = 0;
    expect_ok(nggc_open(&config, &heap),
              "open interior decommit fault heap");
    memset(&desc, 0, sizeof(desc));
    desc.size = sizeof(desc);
    desc.name = "interior-decommit-fault-leaf";
    expect_ok(nggc_register_type(heap, &desc, &type_id),
              "register interior decommit fault type");
    {
        void *prefix = nggc_alloc_size(heap, type_id, 512u);
        void *large = nggc_alloc_size(
            heap, type_id, (size_t)9u * config.region_size);
        void *guard = nggc_alloc_size(
            heap, type_id, config.region_size);
        void *tail = nggc_alloc_size(
            heap, type_id, (size_t)10u * config.region_size);
        void *replacement;
        uint64_t recommit_failures;
        expect_true(prefix != 0 && large != 0 && guard != 0 && tail != 0,
                    "fill interior decommit fault arena");
        expect_ok(nggc_root_push(heap, &prefix),
                  "root decommit fault prefix");
        expect_ok(nggc_root_push(heap, &guard),
                  "root decommit fault guard");
        expect_ok(nggc_root_push(heap, &tail),
                  "root decommit fault tail");

        nggc_backend_fault_fail_next_decommit();
        expect_ok(nggc_collect(heap),
                  "collection survives interior decommit failure");
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(heap, &stats),
                  "stats after interior decommit failure");
        expect_true(stats.interior_decommitted_run_count == 0u &&
                        stats.large_free_object_count == 1u &&
                        stats.large_free_bytes ==
                            (size_t)10u * config.region_size &&
                        stats.region_decommit_failure_count != 0u,
                    "failed interior decommit preserves committed run");
        expect_ok(nggc_check(heap),
                  "verify failed interior decommit rollback");

        expect_ok(nggc_collect(heap),
                  "retry interior decommit after backend recovery");
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(heap, &stats),
                  "stats after interior decommit retry");
        expect_true(stats.interior_decommitted_run_count == 1u &&
                        stats.interior_decommitted_region_count == 10u &&
                        stats.large_free_object_count == 0u,
                    "interior decommit retry succeeds");
        recommit_failures = stats.region_recommit_failure_count;

        nggc_backend_fault_fail_next_recommit();
        replacement = nggc_alloc_size(
            heap, type_id, (size_t)3u * config.region_size);
        expect_true(replacement == 0 &&
                        nggc_get_last_error(heap) == NGGC_ERR_BACKEND,
                    "interior recommit failure reaches allocator");
        memset(&stats, 0, sizeof(stats));
        stats.size = sizeof(stats);
        expect_ok(nggc_get_stats(heap, &stats),
                  "stats after interior recommit failure");
        expect_true(stats.interior_decommitted_run_count == 1u &&
                        stats.interior_decommitted_region_count == 10u &&
                        stats.region_recommit_failure_count ==
                            recommit_failures + 1u,
                    "failed recommit preserves side run");
        expect_ok(nggc_check(heap),
                  "verify failed interior recommit rollback");

        replacement = nggc_alloc_size(
            heap, type_id, (size_t)3u * config.region_size);
        expect_true(replacement == large,
                    "interior recommit retry reuses original address");
        expect_ok(nggc_check(heap),
                  "verify interior recommit recovery");
        expect_ok(nggc_root_pop(heap, &tail),
                  "unroot decommit fault tail");
        expect_ok(nggc_root_pop(heap, &guard),
                  "unroot decommit fault guard");
        expect_ok(nggc_root_pop(heap, &prefix),
                  "unroot decommit fault prefix");
    }
    expect_ok(nggc_close(heap), "close interior decommit fault heap");

    nggc_backend_fault_reset();
    return 0;
}
