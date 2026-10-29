# persistent-map

A C++20 header-only ordered map with immutable AVL nodes. Copies share a root; updates return a new version and copy a logarithmic path. Traversal and ownership changes belong to one serialized executor.

Part of the [Kronuz C++ libraries](https://github.com/Kronuz/cpp-libraries).

```cpp
#include <persistent_map.h>
#include <string>

using Map = persistent_map::PersistentMap<std::string, int>;
Map values;
values = values.set("rollout", 10);
auto saved = values;
values = values.set("rollout", 25);
// saved.find("rollout") points to 10; values.find("rollout") points to 25.
```

`set` and `erase` return versions. `find` returns a borrowed const value pointer; `cursor` and `cursor_from` retain their root and return borrowed const key/value pointers. Keep the retaining map or cursor alive while using those pointers. `for_each` visits in comparator order. Lookups and updates are logarithmic; full traversal is linear. Cursors use a fixed, checked stack large enough for a representable AVL tree and allocate no traversal storage.

## Reclamation

Discarded paths enter the arena's queue. `collect(n)` destroys at most n queued nodes while the arena remains alive; it can enqueue their children. The budget counts nodes, with payload destructor cost determined by the payload type. `retained_nodes()` includes live and queued nodes across retained versions; `allocations()` counts all constructed nodes in their shared arena.

Final arena release can reclaim all remaining nodes synchronously. To drain incrementally, keep a collector version alive, release other retaining maps and cursors, retire the collector's root and collect before dropping it:

```cpp
saved.retire_root();
values.retire_root();
while (values.reclamation_pending()) values.collect(128);
```

All access, collection and reference-release operations use the same executor. Shared pointer reference counts do not make the arena a concurrent container. Moved-from maps support destruction and assignment.

## Comparator and allocator

`PersistentMap<Key, Value, Compare, Alloc>` accepts a comparator and allocator in its constructor. Defaults are `std::less<>` and `std::allocator<std::byte>`. Copies share the originating arena, comparator and allocation policy. Assignment replaces that ownership pair; mutable-container allocator propagation does not apply.

The comparator supplies a stable strict weak ordering across keys and heterogeneous lookup arguments. Keys and values are copyable, with nonthrowing destructors. Failed comparison, construction or allocation preserves prior logical versions; constructed garbage can remain queued until collection.

Allocators use ordinary pointers and nonthrowing copies/deallocation. They are rebound for arena storage, nodes and shared controls. Allocator resources remain valid through all corresponding allocations and weak controls. An owning accounting allocator can retain those resources; nested key/value allocations follow their own types. The map adds no worker, runtime or byte ledger.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/persistent_map_demo
```

The CMake target is `persistent_map::persistent_map`. Add this repository with `FetchContent` or `add_subdirectory`; it has no third-party dependencies. Tests and the example build only as the top-level project.

CI covers Linux (GCC/Clang, x86_64/arm64), macOS (x86_64/arm64), FreeBSD and ASan/UBSan. TSan is excluded because the API confines all operations and releases to one executor. The allocation-denial test also replaces global allocation functions, which conflict with the TSan runtime interceptors.

## Provenance and license

The tree derives from the [immutable AVL implementation](https://github.com/Kronuz/Detent/blob/f1487c2/src/detent_authority/persistent_map.hh) by Germán Méndez Bravo. The extraction retains its balancing and collection algorithm, adding stored comparator and standard allocator support. [MIT license](LICENSE).
