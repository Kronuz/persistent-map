#include "persistent_map.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <new>
#include <random>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
thread_local bool deny_new = false;
}
void *operator new(std::size_t size) {
	if (deny_new)
		throw std::bad_alloc();
	if (auto pointer = std::malloc(size ? size : 1))
		return pointer;
	throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { std::free(pointer); }
void *operator new(std::size_t size, std::align_val_t alignment) {
	if (deny_new)
		throw std::bad_alloc();
	void *pointer = nullptr;
	if (::posix_memalign(&pointer, static_cast<std::size_t>(alignment), size ? size : 1) == 0)
		return pointer;
	throw std::bad_alloc();
}
void *operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void *pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void check(bool condition, std::source_location at = std::source_location::current()) {
	if (!condition)
		throw std::runtime_error("persistent map invariant at " + std::to_string(at.line()));
}
namespace persistent_map::detail {
struct InvariantAccess {
	template <class Map> static void verify(const Map &map) {
		const auto &compare = std::as_const(map.arena_->compare);
		auto walk = [&](auto &&self, const auto &node) -> std::pair<int, std::size_t> {
			if (!node)
				return {0, 0};
			const auto [lh, lc] = self(self, node->left);
			const auto [rh, rc] = self(self, node->right);
			check(std::abs(lh - rh) <= 1 && node->height == 1 + std::max(lh, rh));
			check(node->count == 1 + lc + rc);
			if (node->left)
				check(compare(node->left->key, node->key));
			if (node->right)
				check(compare(node->key, node->right->key));
			return {node->height, node->count};
		};
		const auto [height, size] = walk(walk, map.root_);
		check(height == map.height() && size == map.size());
	}
};
} // namespace persistent_map::detail
struct Counts {
	std::unordered_map<void *, std::size_t> live;
	std::size_t bytes = 0, attempts = 0, fail_at = std::numeric_limits<std::size_t>::max();
};
template <class T> struct Allocator {
	using value_type = T;
	std::shared_ptr<Counts> state;
	Allocator() : state(std::make_shared<Counts>()) {}
	explicit Allocator(std::shared_ptr<Counts> value) noexcept : state(std::move(value)) {}
	template <class U> Allocator(const Allocator<U> &other) noexcept : state(other.state) {}
	T *allocate(std::size_t count) {
		if (state->attempts++ == state->fail_at)
			throw std::bad_alloc();
		auto pointer = std::allocator<T>{}.allocate(count);
		try {
			check(state->live.emplace(pointer, sizeof(T) * count).second);
		} catch (...) {
			std::allocator<T>{}.deallocate(pointer, count);
			throw;
		}
		state->bytes += sizeof(T) * count;
		return pointer;
	}
	void deallocate(T *pointer, std::size_t count) noexcept {
		const auto found = state->live.find(pointer);
		if (found == state->live.end() || found->second != sizeof(T) * count)
			std::abort();
		state->bytes -= found->second;
		state->live.erase(found);
		std::allocator<T>{}.deallocate(pointer, count);
	}
	template <class U> bool operator==(const Allocator<U> &other) const noexcept {
		return state == other.state;
	}
};
template <class Map> void drain(Map &map) {
	while (map.reclamation_pending())
		check(map.collect(7) <= 7);
}
template <class Map, class Oracle> void agrees(const Map &map, const Oracle &oracle) {
	persistent_map::detail::InvariantAccess::verify(map);
	check(map.size() == oracle.size());
	auto cursor = map.cursor();
	for (const auto &[key, value] : oracle) {
		const auto row = cursor.next();
		check(row && *row->first == key && *row->second == value);
		check(map.find(key) && *map.find(key) == value);
	}
	check(!cursor.next());
}
void randomized() {
	using Map = persistent_map::PersistentMap<int, int>;
	Map map;
	std::map<int, int> oracle;
	std::vector<std::pair<Map, std::map<int, int>>> saved;
	std::mt19937 random(4217);
	for (int step = 0; step != 5000; ++step) {
		const auto key = static_cast<int>(random() % 257);
		if (random() % 3) {
			map = map.set(key, step);
			oracle[key] = step;
		} else {
			map = map.erase(key);
			oracle.erase(key);
		}
		agrees(map, oracle);
		if (step % 37 == 0) {
			saved.emplace_back(map, oracle);
			if (saved.size() > 16)
				saved.erase(saved.begin());
			for (const auto &[version, values] : saved)
				agrees(version, values);
		}
		check(map.collect(7) <= 7);
	}
	auto held = std::optional(map.cursor());
	saved.clear();
	map.retire_root();
	drain(map);
	check(map.retained_nodes() > 0);
	held.reset();
	drain(map);
	check(map.retained_nodes() == 0);
}
struct Reverse {
	bool descending = false;
	bool operator()(int left, int right) const { return descending ? left > right : left < right; }
};
void ordering() {
	persistent_map::PersistentMap<int, int, Reverse> map(Reverse{true});
	std::map<int, int, Reverse> oracle(Reverse{true});
	for (int key = 0; key != 100; ++key) {
		map = map.set(key, key * 2);
		oracle[key] = key * 2;
	}
	agrees(map, oracle);
	auto lower = map.cursor_from(50);
	check(*lower.next()->first == 50 && *lower.next()->first == 49);
	persistent_map::PersistentMap<std::string, int> strings;
	{
		const std::string key(100, 'x');
		strings = strings.set(key, 17);
	}
	check(*strings.find(std::string_view(std::string(100, 'x'))) == 17);
	check(strings.cursor_from(std::string_view("w")).next().has_value());
}
void allocation_failures() {
	auto state = std::make_shared<Counts>();
	using Map = persistent_map::PersistentMap<int, int, std::less<>, Allocator<std::byte>>;
	{
		Map map({}, Allocator<std::byte>(state));
		for (int key = 0; key != 31; ++key)
			map = map.set(key, key);
		drain(map);
		const auto baseline = state->bytes;
		bool complete = false, queued_failure = false;
		for (std::size_t failure = 0; failure != 64; ++failure) {
			state->attempts = 0;
			state->fail_at = failure;
			try {
				auto changed = map.set(99, 99);
				check(changed.find(99));
				complete = true;
			} catch (const std::bad_alloc &) {
				queued_failure |= map.reclamation_pending();
			}
			state->fail_at = std::numeric_limits<std::size_t>::max();
			check(!map.find(99) && map.size() == 31);
			persistent_map::detail::InvariantAccess::verify(map);
			drain(map);
			check(state->bytes == baseline && map.retained_nodes() == 31);
			if (complete)
				break;
		}
		check(complete && queued_failure);
		const auto attempts = state->attempts;
		struct Deny {
			Deny() { deny_new = true; }
			~Deny() { deny_new = false; }
		};
		{
			Deny denied;
			{
				auto cursor = map.cursor_from(5LL);
				while (cursor.next()) {
				}
			}
			map.for_each([](int, int) {});
			check(*map.find(5) == 5);
			map.retire_root();
			const auto retained = map.retained_nodes();
			check(map.collect(0) == 0 && map.retained_nodes() == retained);
			drain(map);
			check(map.retained_nodes() == 0);
		}
		check(state->attempts == attempts);
	}
	check(state->bytes == 0 && state->live.empty());
}
void allocator_lifetime() {
	using Map = persistent_map::PersistentMap<int, int, std::less<>, Allocator<std::byte>>;
	auto first = std::make_shared<Counts>(), second = std::make_shared<Counts>();
	{
		Map a({}, Allocator<std::byte>(first)), b({}, Allocator<std::byte>(second));
		for (int key = 0; key != 100; ++key)
			a = a.set(key, key);
		b = b.set(500, 500);
		a = b;
		check(first->bytes == 0 && *a.find(500) == 500);
		Map moved(std::move(a));
		b = std::move(moved);
		check(*b.find(500) == 500);
	}
	check(first->bytes == 0 && second->bytes == 0);
	std::weak_ptr<Counts> weak;
	std::optional<Map::Cursor> held;
	{
		auto state = std::make_shared<Counts>();
		weak = state;
		Map map({}, Allocator<std::byte>(state));
		for (int key = 0; key != 100; ++key)
			map = map.set(key, key);
		held.emplace(map.cursor());
	}
	check(!weak.expired());
	int count = 0;
	while (held->next())
		++count;
	check(count == 100);
	held.reset();
	check(weak.expired());
}
struct CopyValue {
	static inline int live = 0, remaining = -1;
	int value;
	explicit CopyValue(int v) : value(v) { ++live; }
	CopyValue(const CopyValue &other) : value(other.value) {
		if (remaining == 0)
			throw std::runtime_error("copy failure");
		if (remaining > 0)
			--remaining;
		++live;
	}
	CopyValue(CopyValue &&other) noexcept : value(other.value) { ++live; }
	~CopyValue() { --live; }
};
void throwing_values() {
	{
		persistent_map::PersistentMap<int, CopyValue> map;
		for (int key = 0; key != 31; ++key)
			map = map.set(key, CopyValue(key));
		drain(map);
		const auto baseline = CopyValue::live;
		bool complete = false;
		for (int failure = 0; failure != 64; ++failure) {
			CopyValue input(99);
			CopyValue::remaining = failure;
			try {
				auto next = map.set(99, input);
				check(next.find(99)->value == 99);
				complete = true;
			} catch (const std::runtime_error &) {
			}
			CopyValue::remaining = -1;
			drain(map);
			check(map.size() == 31 && !map.find(99) && CopyValue::live == baseline + 1);
			if (complete)
				break;
		}
		check(complete);
	}
	check(CopyValue::live == 0);
}
struct Injected : std::exception {};
struct CompareState {
	int remaining = -1;
};
struct ThrowCompare {
	std::shared_ptr<CompareState> state;
	bool operator()(int left, int right) const {
		if (state->remaining == 0)
			throw Injected();
		if (state->remaining > 0)
			--state->remaining;
		return left < right;
	}
};
void throwing_comparisons() {
	auto state = std::make_shared<CompareState>();
	persistent_map::PersistentMap<int, int, ThrowCompare> map(ThrowCompare{state});
	for (int key = 0; key != 31; ++key)
		map = map.set(key, key);
	for (bool remove : {false, true}) {
		bool complete = false;
		for (int failure = 0; failure != 64; ++failure) {
			state->remaining = failure;
			try {
				auto next = remove ? map.erase(15) : map.set(99, 99);
				complete = true;
			} catch (const Injected &) {
			}
			state->remaining = -1;
			check(map.size() == 31 && *map.find(15) == 15 && !map.find(99));
			persistent_map::detail::InvariantAccess::verify(map);
			drain(map);
			if (complete)
				break;
		}
		check(complete);
	}
}
struct CopyOrder {
	std::shared_ptr<CompareState> state;
	explicit CopyOrder(std::shared_ptr<CompareState> value) : state(std::move(value)) {}
	CopyOrder(const CopyOrder &other) : state(other.state) { copy(); }
	CopyOrder(CopyOrder &&other) : state(other.state) { copy(); }
	void copy() {
		if (state->remaining == 0)
			throw Injected();
		if (state->remaining > 0)
			--state->remaining;
	}
	bool operator()(int left, int right) const { return left < right; }
};
void comparator_construction() {
	auto allocator = std::make_shared<Counts>();
	auto state = std::make_shared<CompareState>();
	CopyOrder order(state);
	using Map = persistent_map::PersistentMap<int, int, CopyOrder, Allocator<std::byte>>;
	bool complete = false;
	for (int failure = 0; failure != 16; ++failure) {
		state->remaining = failure;
		try {
			Map map(order, Allocator<std::byte>(allocator));
			complete = true;
		} catch (const Injected &) {
		}
		state->remaining = -1;
		check(allocator->bytes == 0 && allocator->live.empty());
		if (complete)
			break;
	}
	check(complete);
	{
		Map map(order, Allocator<std::byte>(allocator));
		state->remaining = 0;
		auto next = map.set(1, 1);
		check(*next.find(1) == 1);
		state->remaining = -1;
	}
	check(allocator->bytes == 0);
}
struct CopyKey : CopyValue {
	using CopyValue::CopyValue;
	friend bool operator<(const CopyKey &left, const CopyKey &right) { return left.value < right.value; }
};
void throwing_keys() {
	{
		persistent_map::PersistentMap<CopyKey, int> map;
		for (int key = 0; key != 31; ++key)
			map = map.set(CopyKey(key), key);
		drain(map);
		CopyKey input(99);
		const auto baseline = CopyValue::live;
		bool complete = false;
		for (int failure = 0; failure != 64; ++failure) {
			CopyValue::remaining = failure;
			try {
				auto next = map.set(input, 99);
				check(*next.find(input) == 99);
				complete = true;
			} catch (const std::runtime_error &) {
			}
			CopyValue::remaining = -1;
			drain(map);
			check(map.size() == 31 && !map.find(input) && CopyValue::live == baseline);
			if (complete)
				break;
		}
		check(complete);
	}
	check(CopyValue::live == 0);
}
struct alignas(128) Aligned {
	int value = 0;
};
void alignment_and_initial_failure() {
	auto state = std::make_shared<Counts>();
	using Map = persistent_map::PersistentMap<int, Aligned, std::less<>, Allocator<std::byte>>;
	state->fail_at = 0;
	bool failed = false;
	try {
		Map map({}, Allocator<std::byte>(state));
	} catch (const std::bad_alloc &) {
		failed = true;
	}
	check(failed && state->bytes == 0 && state->live.empty());
	state->fail_at = std::numeric_limits<std::size_t>::max();
	{
		Map map({}, Allocator<std::byte>(state));
		for (int key = 0; key != 100; ++key)
			map = map.set(key, Aligned{key});
		check(reinterpret_cast<std::uintptr_t>(map.find(17)) % alignof(Aligned) == 0);
		check(map.find(17)->value == 17);
	}
	check(state->bytes == 0);
}
int main() {
	try {
		randomized();
		ordering();
		allocation_failures();
		allocator_lifetime();
		throwing_values();
		throwing_comparisons();
		comparator_construction();
		throwing_keys();
		alignment_and_initial_failure();
		std::puts("Persistent map oracle, allocator, exception and reclamation checks passed");
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
