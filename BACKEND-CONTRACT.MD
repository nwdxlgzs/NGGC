# NGGC Backend Contract

This document describes the current private backend boundary between NGGC and the memory-operation substrate.

The first backend implementation is backed by `mock-uhmas/UHMAS-HL`. Later, when real UHMAS is ready enough, this contract is the intended replacement seam.

## Scope

The backend owns raw virtual-memory style operations only:

- create and destroy a backing heap;
- map a read/write arena;
- unmap a previously mapped arena;
- decommit and recommit an NGGC-owned mapped range;
- run the backing heap's own consistency check;
- report coarse backing statistics.

The backend does not know about GC object headers, types, roots, mark bits, weak references, barriers, regions, or generations.

## Current API

The private API lives in `src/nggc_backend.h`:

```text
nggc_backend_create
nggc_backend_destroy
nggc_backend_map
nggc_backend_unmap
nggc_backend_decommit
nggc_backend_recommit
nggc_backend_check
nggc_backend_get_stats
```

`src/nggc_backend.c` is currently the only file that includes UHMAS-HL platform headers directly.

## Rules

- `nggc_backend_map` returns memory usable for ordinary read/write GC object storage.
- NGGC is responsible for object alignment inside mapped memory.
- NGGC must unmap every still-committed mapped arena span before destroying the backend. If GC has decommitted tail Regions, close may unmap the remaining committed spans individually instead of assuming the original arena is still one live mmap.
- `nggc_backend_decommit` and `nggc_backend_recommit` are private substrate hooks for Region reclamation. The current UHMAS-HL backend emulates this with unmap plus fixed no-replace remap, so callers must treat the range as still owned by NGGC and must not ask the backend to reuse it for unrelated mappings before recommit.
- Backend failures are translated to `NGGC_ERR_BACKEND` unless the caller supplied invalid arguments.
- Backend stats expose arena size, committed/decommitted bytes, live mmap bytes, page size, and allocation granularity. They are advisory and must not be required for collector correctness, but page geometry is the required gate for GC-side Region decommit/recommit decisions.
- `nggc_check` treats a failed backend check as a real heap check failure and returns `NGGC_ERR_BACKEND`; backend stats remain advisory.

## Fault Injection

When `NGGC_ENABLE_FAULT_INJECTION=1`, the backend exposes private test hooks for one-shot create/map/unmap/decommit/recommit/stats/check failures. These hooks exist only for diagnostic and test builds and are compiled out of normal builds.

The hooks must not become part of the stable public API. They are allowed to live in `src/nggc_backend.h` because that header is private to NGGC and tests.

## Future UHMAS Migration

When moving from UHMAS-HL to real UHMAS, keep the NGGC-facing contract stable first. Add backend capabilities only when the collector actually needs them, for example:

- page protection for debug verification;
- map hints for region placement;
- native decommit/recommit for region reclamation;
- NUMA placement hints;
- failure injection hooks.

The collector should not reach around this layer to call UHMAS directly.
