// Cost of versions: lookup and update CPU, allocation requests, retained and peak heap and
// reclamation, over key counts, update batch sizes and retained-version counts, beside
// std::map, which keeps a version by copying it whole.
//   c++ -std=c++20 -O3 -DNDEBUG -I. bench/bench.cc -o bench && ./bench [MAX_KEYS]
#include "persistent_map.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

// Every allocation carries its size in a 16-byte header: live and peak count requested bytes.
namespace {
std::size_t calls = 0, live = 0, peak = 0;
}
void *operator new(std::size_t size) {
	auto block = static_cast<std::size_t *>(std::malloc(size + 16));
	if (!block)
		throw std::bad_alloc();
	*block = size;
	++calls;
	live += size;
	peak = std::max(peak, live);
	return reinterpret_cast<char *>(block) + 16;
}
void operator delete(void *pointer) noexcept {
	if (!pointer)
		return;
	auto block = reinterpret_cast<std::size_t *>(static_cast<char *>(pointer) - 16);
	live -= *block;
	std::free(block);
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void *pointer) noexcept { ::operator delete(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { ::operator delete(pointer); }

namespace {
using Map = persistent_map::PersistentMap<std::string, std::uint64_t>;
std::uint64_t cpu() {
	timespec time{};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time))
		throw std::runtime_error("clock");
	return std::uint64_t(time.tv_sec) * 1000000000 + std::uint64_t(time.tv_nsec);
}
template <class F> std::uint64_t measure(F &&action) {
	const auto start = cpu();
	action();
	return cpu() - start;
}
void check(bool condition, const char *what) {
	if (!condition)
		throw std::runtime_error(what);
}
// 34-byte keys, the authority's production shape, in a shuffled order.
std::vector<std::string> make_keys(std::size_t count) {
	std::vector<std::string> keys;
	keys.reserve(count);
	char buffer[64];
	for (std::size_t i = 0; i != count; ++i) {
		std::snprintf(buffer, sizeof buffer, "namespace0:service/feature%07zu", i);
		keys.emplace_back(buffer);
	}
	std::uint64_t state = 88172645463325252ull;
	for (std::size_t i = count; i > 1; --i) {
		state ^= state << 13, state ^= state >> 7, state ^= state << 17;
		std::swap(keys[i - 1], keys[state % i]);
	}
	return keys;
}
void drain(Map &map, std::size_t &turns, std::uint64_t &max_turn) {
	while (map.reclamation_pending()) {
		const auto start = cpu();
		check(map.collect(128) <= 128, "collection budget");
		max_turn = std::max(max_turn, cpu() - start);
		++turns;
	}
}
constexpr std::size_t updates = 16384, lookups = 1000000;
void run(std::size_t count) {
	const auto keys = make_keys(count);
	const auto heap_base = live;
	Map map;
	const auto load_ns = measure([&] {
		for (std::size_t i = 0; i != count; ++i)
			map = map.set(keys[i], i);
	});
	std::size_t turns = 0;
	std::uint64_t max_turn = 0;
	drain(map, turns, max_turn);
	check(map.size() == count && map.retained_nodes() == count, "load");
	const auto map_bytes = live - heap_base;
	std::uint64_t sum = 0;
	const auto lookup_ns = measure([&] {
		for (std::size_t i = 0; i != lookups; ++i)
			sum += *map.find(keys[(i * 7919) % count]);
	});
	std::map<std::string, std::uint64_t, std::less<>> oracle;
	const auto std_base = live;
	for (std::size_t i = 0; i != count; ++i)
		oracle.emplace(keys[i], i);
	const auto std_bytes = live - std_base;
	std::uint64_t std_sum = 0;
	const auto std_lookup_ns = measure([&] {
		for (std::size_t i = 0; i != lookups; ++i)
			std_sum += oracle.find(keys[(i * 7919) % count])->second;
	});
	check(sum == std_sum, "lookup identity");
	// std::map keeps a version by copying itself.
	std::uint64_t std_copy_ns = 0;
	for (int i = 0; i != 3; ++i) {
		std::uint64_t elapsed = 0;
		{
			std::map<std::string, std::uint64_t, std::less<>> copy;
			elapsed = measure([&] { copy = oracle; });
			check(copy.size() == count, "copy");
		}
		std_copy_ns = i ? std::min(std_copy_ns, elapsed) : elapsed;
	}
	std::printf("keys %zu: load %.0f ns/set, lookup %.0f ns (std::map %.0f), heap %zu B (std::map %zu), "
	            "std::map copy %.3f ms\n",
	            count, double(load_ns) / double(count), double(lookup_ns) / lookups,
	            double(std_lookup_ns) / lookups, map_bytes, std_bytes, double(std_copy_ns) / 1e6);
	for (std::size_t batch : {1, 16, 128}) {
		for (std::size_t retained : {1, 16, 64}) {
			auto current = map;
			std::deque<Map> versions;
			const auto heap_start = live;
			peak = live;
			const auto calls_start = calls;
			const auto nodes_start = current.allocations();
			std::size_t collected = 0;
			std::uint64_t collect_ns = 0;
			const auto update_ns = measure([&] {
				for (std::size_t step = 0; step * batch < updates; ++step) {
					versions.push_back(current);
					if (versions.size() > retained)
						versions.pop_front();
					for (std::size_t i = 0; i != batch; ++i) {
						const auto index = (step * batch + i) * 7919 % count;
						current = current.set(keys[index], count + step);
					}
					collect_ns -= cpu();
					while (current.reclamation_pending())
						collected += current.collect(128);
					collect_ns += cpu();
				}
			});
			const auto requests = calls - calls_start;
			const auto nodes = current.allocations() - nodes_start;
			const auto retained_bytes = live - heap_start;
			const auto peak_bytes = peak - heap_start;
			const auto retained_nodes = current.retained_nodes() - count;
			std::size_t final_turns = 0;
			std::uint64_t final_max = 0;
			versions.clear();
			const auto release_nodes = current.retained_nodes();
			current.retire_root();
			const auto release_ns = measure([&] { drain(current, final_turns, final_max); });
			check(current.retained_nodes() == count, "drained to the base version");
			// The same versions as whole copies: the base, the retained ones and the current one.
			const auto copies_bytes = double(std::min(retained, updates / batch) + 2) * double(std_bytes);
			std::printf("  batch %3zu retained %2zu: update %.0f ns/set (%.0f incl. collect), %.2f requests/set, "
			            "%.1f nodes/set, retained +%zu B (%zu nodes), peak +%zu B; versions as copies %.1f MB "
			            "(%.1fx); collect %.1f ns/node; release %zu nodes in %zu turns, max turn %.1f us, %.1f ns/node\n",
			            batch, retained, double(update_ns - collect_ns) / updates, double(update_ns) / updates,
			            double(requests) / updates, double(nodes) / updates, retained_bytes, retained_nodes,
			            peak_bytes, copies_bytes / 1e6, copies_bytes / double(std::max<std::size_t>(retained_bytes + map_bytes, 1)),
			            collected ? double(collect_ns) / double(collected) : 0.0, release_nodes - count, final_turns,
			            double(final_max) / 1e3,
			            release_nodes > count ? double(release_ns) / double(release_nodes - count) : 0.0);
		}
	}
	map.retire_root();
	turns = 0;
	max_turn = 0;
	const auto teardown_ns = measure([&] { drain(map, turns, max_turn); });
	check(map.retained_nodes() == 0, "teardown");
	std::printf("  teardown %zu nodes: %zu turns of 128, max turn %.1f us, %.1f ns/node\n", count, turns,
	            double(max_turn) / 1e3, double(teardown_ns) / double(count));
}
} // namespace
int main(int argc, char **argv) {
	try {
		const std::size_t max_keys = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
		for (std::size_t count = 1000; count <= max_keys; count *= 10)
			run(count);
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
