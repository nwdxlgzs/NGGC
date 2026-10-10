#include "nggc/nggc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_true(int value, const char *message) {
    if (!value) {
        fprintf(stderr, "%s\n", message);
        exit(1);
    }
}

int main(void) {
    nggc_heap *heap = 0;
    nggc_config config;

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config) - 1u;
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    heap = (nggc_heap *)1;
    expect_true(nggc_open(&config, &heap) == NGGC_ERR_INVALID_ARGUMENT,
                "short config rejected");
    expect_true(heap == 0, "short config leaves no heap");

    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.abi_major = NGGC_ABI_MAJOR;
    config.abi_minor = NGGC_ABI_MINOR;
    config.uhm_arena_size = 4096u;
    config.gc_arena_size = (size_t)2u * 1024u * 1024u;
    heap = (nggc_heap *)1;
    expect_true(nggc_open(&config, &heap) == NGGC_ERR_BACKEND,
                "undersized UHMAS backing rejects GC arena");
    expect_true(heap == 0, "backend failure leaves no heap");

    return 0;
}
