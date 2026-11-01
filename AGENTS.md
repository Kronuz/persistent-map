# Working in persistent-map

Follow the [C++ library rules](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md). Keep one tree engine. `persistent_map.h` contains the implementation; `test/test.cc` covers historical roots, invariants, failures and reclamation; `examples/demo.cc` shows retained versions.

Preserve immutable AVL paths, heterogeneous ordering and owner-executor confinement. Nodes, cursors and versions retain their originating allocator/comparator arena. Rebind standard allocators for arena, nodes and shared controls. Guard raw storage until construction succeeds, then transfer it once into shared ownership. Deleters retain the originating allocator through expired-arena teardown. Preserve the default allocation footprint.

Collection budgets count destroyed nodes, not payload bytes or time. Last-handle teardown can be synchronous; do not replace the internal queue with a worker or claim bounded final release. Do not introduce another reclamation runtime or byte ledger. Canonical accounting composes through standard allocator adapters.

Run the native test and example, relevant sanitizer/platform CI and matched production consumer costs before adoption. Keep runtime checks enabled in optimized tests. Changes to ordering, allocator identity, exception guarantees, final release or borrowed-row lifetime require explicit compatibility review. Document unsupported concurrency in the README and the allocator contracts in ARCHITECTURE.md.

## Verification and benchmarks

CI (`.github/workflows/ci.yml`) calls the catalog's shared
[`cmake.yml`](https://github.com/Kronuz/cpp-libraries/blob/main/.github/workflows/cmake.yml) on every push and nightly:
CTest on Linux (x86-64 GCC and Clang, arm64), macOS (arm64, x86-64) and FreeBSD, then again under
ASan+UBSan. The TSan job is off (`tsan: false`): the API confines every operation to one
executor, and the test replaces the global allocation functions, which TSan's runtime intercepts.
The shared method is the catalog's
[PERFORMANCE.md](https://github.com/Kronuz/cpp-libraries/blob/main/docs/PERFORMANCE.md).

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
./build/persistent_map_demo
# ASan+UBSan as CI's linux-asan-ubsan job runs it (Linux, Clang)
CC=clang CXX=clang++ cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS" \
  "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
cmake --build build-asan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
```

The oracle is `std::map`: `persistent_map_test` applies 5,000 seeded random `set`s and `erase`s
and compares the map, and up to 16 saved versions, with a `std::map` copied at the same step;
a reversed comparator checks ordering, and `InvariantAccess::verify` checks the AVL invariants.
Failure injection covers the rest: an allocator that fails the n-th allocation for each n in turn
(each prior version must read back intact and every byte must come back), a global `operator new`
that throws during reads, cursors and collection (none may allocate), and throwing values, keys,
comparisons and comparator copies. The checks stay on in optimized builds. There is no bench,
fuzz target or mutation script: the O(log n) path copy is a property of the algorithm, not a
measured claim.
