# Working in persistent-map

Follow the [C++ library rules](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md). Keep one tree engine. `persistent_map.h` contains the implementation; `test/test.cc` covers historical roots, invariants, failures and reclamation; `examples/demo.cc` shows retained versions.

Preserve immutable AVL paths, heterogeneous ordering and owner-executor confinement. Nodes, cursors and versions retain their originating allocator/comparator arena. Rebind standard allocators for arena, nodes and shared controls. Guard raw storage until construction succeeds, then transfer it once into shared ownership. Deleters retain the originating allocator through expired-arena teardown. Preserve the default allocation footprint.

Collection budgets count destroyed nodes, not payload bytes or time. Last-handle teardown can be synchronous; do not replace the internal queue with a worker or claim bounded final release. Do not introduce another reclamation runtime or byte ledger. Canonical accounting composes through standard allocator adapters.

Run the native test and example, relevant sanitizer/platform CI and matched production consumer costs before adoption. Keep runtime checks enabled in optimized tests. Changes to ordering, allocator identity, exception guarantees, final release or borrowed-row lifetime require explicit compatibility review. Document unsupported concurrency and allocator contracts in the README.
