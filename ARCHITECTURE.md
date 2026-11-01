# Architecture

`persistent_map.h` is one immutable AVL tree. Every node is shared and never changes after
construction; an update builds new nodes along one root-to-leaf path and returns a map with a
new root. [AGENTS.md](AGENTS.md) holds the rules for changing it.

## Versions and borrowed pointers

Copies of a map share a root. `set` and `erase` return versions and copy the logarithmic path
they change; the old version keeps the old path. Every node stores its height and its subtree
count, so `size()` is constant time.

`find` returns a borrowed const value pointer. `cursor` and `cursor_from` retain their root and
return borrowed const key/value pointers. Keep the retaining map or cursor alive while using
those pointers. `for_each` visits in comparator order. Lookups and updates are logarithmic;
full traversal is linear. Cursors use a fixed, checked stack of 128 entries, large enough for
any representable AVL tree, and allocate no traversal storage.

## Reclamation

Discarded paths enter the arena's queue. `collect(n)` destroys at most n queued nodes while the
arena remains alive; it can enqueue their children. The budget counts nodes, with payload
destructor cost determined by the payload type. `retained_nodes()` includes live and queued
nodes across retained versions; `allocations()` counts all constructed nodes in their shared
arena.

Final arena release can reclaim all remaining nodes synchronously. To drain incrementally,
keep a collector version alive, release other retaining maps and cursors, retire the
collector's root and collect before dropping it:

```cpp
saved.retire_root();
values.retire_root();
while (values.reclamation_pending()) values.collect(128);
```

All access, collection and reference-release operations use the same executor. Shared pointer
reference counts do not make the arena a concurrent container. Moved-from maps support
destruction and assignment.

## Comparator and allocator

`PersistentMap<Key, Value, Compare, Alloc>` accepts a comparator and allocator in its
constructor. Defaults are `std::less<>` and `std::allocator<std::byte>`. Copies share the
originating arena, comparator and allocation policy. Assignment replaces that ownership pair;
mutable-container allocator propagation does not apply.

The comparator supplies a stable strict weak ordering across keys and heterogeneous lookup
arguments. Keys and values are copyable, with nonthrowing destructors. Failed comparison,
construction or allocation preserves prior logical versions; constructed garbage can remain
queued until collection.

Allocators use ordinary pointers and nonthrowing copies and deallocation. They are rebound for
arena storage, nodes and shared controls. Allocator resources remain valid through all
corresponding allocations and weak controls. An owning accounting allocator can retain those
resources; nested key and value allocations follow their own types. The map adds no worker,
runtime or byte ledger.

## Testing

`test/test.cc` covers historical roots, the AVL invariants, failure injection and reclamation.
The allocation-denial test replaces the global allocation functions, which conflict with the
ThreadSanitizer runtime's interceptors; CI runs no TSan job because the API confines every
operation to one executor.
