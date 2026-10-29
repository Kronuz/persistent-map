/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace persistent_map {
namespace detail {
struct InvariantAccess;
}

// Immutable AVL paths. Captures share a root; updates replace O(log N) nodes.
// Access and ownership changes use one serialized executor.
template <class Key, class Value, class Compare = std::less<>, class Alloc = std::allocator<std::byte>>
class PersistentMap {
	friend struct detail::InvariantAccess;
	struct Node;
	using Ptr = std::shared_ptr<const Node>;
	using NodeAllocator = typename std::allocator_traits<Alloc>::template rebind_alloc<Node>;
	using NodeTraits = std::allocator_traits<NodeAllocator>;
	struct Arena {
		[[no_unique_address]] Compare compare;
		[[no_unique_address]] Alloc allocator;
		Arena(Compare order, const Alloc &allocation) : compare(std::move(order)), allocator(allocation) {}
		Node *retired = nullptr;
		std::size_t live = 0, allocations = 0;
		void retire(Node *node) noexcept {
			node->retired_next = retired;
			retired = node;
		}
		std::size_t collect(std::size_t budget) noexcept {
			if (!budget || !retired)
				return 0;
			NodeAllocator allocation(allocator);
			std::size_t count = 0;
			while (retired && count < budget) {
				auto node = retired;
				retired = node->retired_next;
				--live;
				++count;
				NodeTraits::destroy(allocation, node);
				NodeTraits::deallocate(allocation, node, 1);
			}
			return count;
		}
		~Arena() {
			while (retired) {
				collect(1024);
			}
		}
	};
	struct Node {
		Key key;
		Value value;
		Ptr left, right;
		int height;
		std::size_t count;
		Node *retired_next = nullptr;
		Node(Key k, Value v, Ptr l, Ptr r)
		    : key(std::move(k)), value(std::move(v)), left(std::move(l)), right(std::move(r)),
		      height(1 + std::max(h(left), h(right))), count(1 + n(left) + n(right)) {}
	};
	using ArenaAllocator = typename std::allocator_traits<Alloc>::template rebind_alloc<Arena>;
	static_assert(std::is_nothrow_copy_constructible_v<Alloc> &&
	              std::is_nothrow_copy_constructible_v<NodeAllocator>);
	static_assert(std::is_nothrow_constructible_v<NodeAllocator, const Alloc &>);
	static_assert(std::is_same_v<typename NodeTraits::pointer, Node *> &&
	              std::is_same_v<typename std::allocator_traits<ArenaAllocator>::pointer, Arena *>);
	static_assert(std::is_copy_constructible_v<Key> && std::is_copy_constructible_v<Value>);
	static_assert(std::is_nothrow_destructible_v<Key> && std::is_nothrow_destructible_v<Value>);
	struct DeleteNode {
		std::weak_ptr<Arena> owner;
		[[no_unique_address]] NodeAllocator allocator;
		DeleteNode(std::weak_ptr<Arena> arena, const NodeAllocator &allocation) noexcept
		    : owner(std::move(arena)), allocator(allocation) {}
		DeleteNode(const DeleteNode &) noexcept = default;
		DeleteNode(DeleteNode &&other) noexcept : owner(std::move(other.owner)), allocator(other.allocator) {}
		void operator()(const Node *pointer) noexcept {
			if (auto arena = owner.lock()) {
				arena->retire(const_cast<Node *>(pointer));
			} else {
				auto node = const_cast<Node *>(pointer);
				NodeTraits::destroy(allocator, node);
				NodeTraits::deallocate(allocator, node, 1);
			}
		}
	};
	std::shared_ptr<Arena> arena_;
	Ptr root_;
	static int h(const Ptr &p) { return p ? p->height : 0; }
	static std::size_t n(const Ptr &p) { return p ? p->count : 0; }
	Ptr make(const Key &k, const Value &v, Ptr l, Ptr r) {
		NodeAllocator allocation(arena_->allocator);
		DeleteNode deleter(arena_, allocation);
		auto node = NodeTraits::allocate(allocation, 1);
		try {
			NodeTraits::construct(allocation, node, k, v, std::move(l), std::move(r));
		} catch (...) {
			NodeTraits::deallocate(allocation, node, 1);
			throw;
		}
		++arena_->live;
		++arena_->allocations;
		return Ptr(node, deleter, arena_->allocator);
	}

	Ptr balance(const Key &k, const Value &v, Ptr l, Ptr r) {
		if (h(l) > h(r) + 1) {
			if (h(l->left) >= h(l->right)) {
				return make(l->key, l->value, l->left, make(k, v, l->right, r));
			}
			auto pivot = l->right;
			return make(pivot->key, pivot->value, make(l->key, l->value, l->left, pivot->left),
			            make(k, v, pivot->right, r));
		}
		if (h(r) > h(l) + 1) {
			if (h(r->right) >= h(r->left)) {
				return make(r->key, r->value, make(k, v, l, r->left), r->right);
			}
			auto pivot = r->left;
			return make(pivot->key, pivot->value, make(k, v, l, pivot->left),
			            make(r->key, r->value, pivot->right, r->right));
		}
		return make(k, v, std::move(l), std::move(r));
	}
	Ptr put(const Ptr &p, const Key &k, const Value &v) {
		if (!p) {
			return make(k, v, {}, {});
		}
		if (std::as_const(arena_->compare)(k, p->key)) {
			return balance(p->key, p->value, put(p->left, k, v), p->right);
		}
		if (std::as_const(arena_->compare)(p->key, k)) {
			return balance(p->key, p->value, p->left, put(p->right, k, v));
		}
		return make(k, v, p->left, p->right);
	}
	Ptr remove(const Ptr &p, const Key &k) {
		if (!p) {
			return {};
		}
		if (std::as_const(arena_->compare)(k, p->key)) {
			return balance(p->key, p->value, remove(p->left, k), p->right);
		}
		if (std::as_const(arena_->compare)(p->key, k)) {
			return balance(p->key, p->value, p->left, remove(p->right, k));
		}
		if (!p->left) {
			return p->right;
		}
		if (!p->right) {
			return p->left;
		}
		auto next = p->right;
		while (next->left) {
			next = next->left;
		}
		return balance(next->key, next->value, p->left, remove(p->right, next->key));
	}
	template <class F> static void visit(const Ptr &p, F &f) {
		if (!p) {
			return;
		}
		visit(p->left, f);
		f(p->key, p->value);
		visit(p->right, f);
	}

  public:
	explicit PersistentMap(Compare compare = {}, Alloc allocator = {})
	    : arena_(std::allocate_shared<Arena>(ArenaAllocator(allocator), std::move(compare), allocator)) {}
	class Cursor {
		std::shared_ptr<Arena> arena_;
		Ptr root_;
		// A 64-bit-sized AVL tree has height below 128. Borrowed stack rows
		// remain valid through the retained immutable root, without allocations.
		std::array<const Node *, 128> stack_{};
		std::size_t depth_ = 0;
		void push(const Node *node) {
			if (depth_ == stack_.size())
				throw std::length_error("persistent map cursor depth");
			stack_[depth_++] = node;
		}
		void left(const Node *node) {
			while (node) {
				push(node);
				node = node->left.get();
			}
		}

	  public:
		Cursor(Ptr root, std::shared_ptr<Arena> arena) : arena_(std::move(arena)), root_(std::move(root)) {
			left(root_.get());
		}
		template <class K>
		Cursor(Ptr root, std::shared_ptr<Arena> arena, const K &lower)
		    : arena_(std::move(arena)), root_(std::move(root)) {
			auto node = root_.get();
			while (node) {
				if (std::as_const(arena_->compare)(node->key, lower))
					node = node->right.get();
				else {
					push(node);
					node = node->left.get();
				}
			}
		}
		std::optional<std::pair<const Key *, const Value *>> next() {
			if (!depth_)
				return std::nullopt;
			auto node = stack_[--depth_];
			left(node->right.get());
			return std::pair{&node->key, &node->value};
		}
	};
	Cursor cursor() const { return Cursor(root_, arena_); }
	template <class K> Cursor cursor_from(const K &lower) const { return Cursor(root_, arena_, lower); }
	void retire_root() noexcept { root_.reset(); }
	std::size_t collect(std::size_t budget) noexcept { return arena_->collect(budget); }
	bool reclamation_pending() const noexcept { return arena_->retired != nullptr; }
	std::size_t retained_nodes() const noexcept { return arena_->live; }
	std::size_t allocations() const noexcept { return arena_->allocations; }
	std::size_t size() const noexcept { return n(root_); }
	int height() const noexcept { return h(root_); }
	template <class K> const Value *find(const K &key) const {
		auto p = root_.get();
		while (p) {
			if (std::as_const(arena_->compare)(key, p->key)) {
				p = p->left.get();
			} else if (std::as_const(arena_->compare)(p->key, key)) {
				p = p->right.get();
			} else {
				return &p->value;
			}
		}
		return nullptr;
	}
	PersistentMap set(const Key &k, const Value &v) const {
		PersistentMap result = *this;
		result.root_ = result.put(root_, k, v);
		return result;
	}
	PersistentMap erase(const Key &k) const {
		PersistentMap result = *this;
		result.root_ = result.remove(root_, k);
		return result;
	}
	template <class F> void for_each(F &&f) const { visit(root_, f); }
};

} // namespace persistent_map
