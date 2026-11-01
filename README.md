# persistent-map

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/persistent-map/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/persistent-map/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Header-only persistent ordered map for C++20: immutable AVL versions share structure and are reclaimed on a node budget.
An update copies the logarithmic path it changes and returns a new map, so every
saved copy stays a readable snapshot. Discarded nodes wait in a queue that
`collect(n)` drains at most n nodes at a time. Reach for it to keep cheap
historical versions of an ordered map on one executor.

## ✨ Highlights

- **Versions share structure.** `set` and `erase` return a new map and copy
  only the O(log n) path they change; copies share the root.
- **Borrowed reads.** `find` returns a const pointer into the tree, and
  `cursor` and `cursor_from` walk in order on a fixed 128-entry stack with no
  allocation.
- **Budgeted reclamation.** `collect(n)` destroys at most n queued nodes, so a
  retired version is drained in steps.
- **Comparator and allocator policies.** `PersistentMap<Key, Value, Compare,
  Alloc>` stores both; the default `std::less<>` allows heterogeneous lookup.
- **Prior versions survive failures.** A throwing comparison, construction or
  allocation leaves every earlier version intact.
- **One header, no dependencies.**

## 🚀 Quick start

```cpp
#include "persistent_map.h"

#include <iostream>
#include <string>

int main() {
	using Map = persistent_map::PersistentMap<std::string, int>;
	Map values;
	values = values.set("rollout", 10);
	auto saved = values;                 // a snapshot: shares the root
	values = values.set("rollout", 25);  // copies one path
	std::cout << *saved.find("rollout") << ' ' << *values.find("rollout") << '\n';  // 10 25

	saved.retire_root();                 // drain the old versions in steps
	values.retire_root();
	while (values.reclamation_pending())
		values.collect(128);
	std::cout << values.retained_nodes() << '\n';  // 0
}
```

[`examples/demo.cc`](examples/demo.cc) is the same program as a runnable demo.

## 📦 Installation

Requires a C++20 compiler and CMake 3.16 or newer; no dependencies. CI builds it
on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan and UBSan
builds; TSan is off because the API confines every operation to one executor.

```cmake
include(FetchContent)
FetchContent_Declare(persistent_map
  GIT_REPOSITORY https://github.com/Kronuz/persistent-map.git
  GIT_TAG        938d65ebd5d61128ea17d18d5a3c97f80cb0c298)  # or main for the tip
FetchContent_MakeAvailable(persistent_map)

target_link_libraries(my_app PRIVATE persistent_map::persistent_map)
```

```cpp
#include "persistent_map.h"
```

## 🎯 When to use it

Use it to keep cheap snapshots of an ordered map, such as successive
configurations, and to read old versions while new ones are built, all on one
executor.

Not for:

- **Concurrent access.** All access, collection and reference release use one
  serialized executor; shared-pointer reference counts do not make the arena a
  concurrent container.
- **Bounded release.** Dropping the last handle to the arena can reclaim every
  remaining node at once; retire and `collect` first to spread the cost.
- **Byte or time budgets.** `collect(n)` counts nodes; what a payload's
  destructor costs is up to the payload type.
- **Long-lived borrowed pointers.** A pointer from `find` or a cursor stays
  valid only while a map or cursor retains its root.
- **Mutable-container allocator semantics.** Assignment replaces the arena,
  comparator and allocator together; allocator propagation traits do not apply.

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): versions and borrowed pointers,
  reclamation, and the comparator and allocator contracts.
- [AGENTS.md](AGENTS.md): the rules for changing the tree.
- [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries): the catalog.

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

`./build/persistent_map_demo` runs the demo. The tests keep their checks in
optimized builds.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE). The tree
derives from the [immutable AVL map](https://github.com/Kronuz/Detent/blob/f1487c2/src/detent_authority/persistent_map.hh)
in Detent.
