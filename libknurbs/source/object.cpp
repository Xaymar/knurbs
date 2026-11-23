#include "knurbs/object.hpp"

#include <cassert>
#include <cstdint>
#include <thread>

// Ports the protocol of libobs/obs-mem.c (docs/superpowers/specs/2026-11-18-libobs-unified-refcount-design.md,
// "Weak block" and "Weak -> strong upgrade") to std::atomic.
//
// The pin protocol is a store-then-load handshake on two variables: an upgrade increments `pins` and then
// reads `dead`; the last release stores `dead` and then reads `pins`. Both sides use seq_cst so that at least
// one of them sees the other's store: either the upgrade sees `dead` and backs off, or the release sees the
// pin and waits for it to go away before the object is destroyed.

namespace knurbs::detail {

struct weak_block {
	std::atomic<long> weak_refs{1}; // starts at 1: the object's own reference (or the caller's, if unattached)
	std::atomic<long> pins{0};      // upgrades in progress
	std::atomic<bool> dead{false};  // set once the object's last strong reference is gone
	object *const target;           // never cleared, so identity stays available on expired blocks

	explicit weak_block(object *obj, bool is_dead = false) noexcept : dead(is_dead), target(obj) {}
};

namespace {

// Stored in object::weak_ once the object is dying and had no weak block, so later requests get a dead one.
weak_block *const dying = reinterpret_cast<weak_block *>(std::uintptr_t{1});

// Marks the weak block dead and waits for upgrades in flight. Returns the block it detached, if any.
// Unless forced, an existing block stays attached and the dying sentinel is only installed when none exists.
weak_block *mark_dead(std::atomic<weak_block *> &slot, bool force_dying) noexcept
{
	weak_block *w = nullptr;
	if (force_dying) {
		w = slot.exchange(dying, std::memory_order_acq_rel);
	} else if (slot.compare_exchange_strong(w, dying, std::memory_order_acq_rel, std::memory_order_acquire)) {
		return nullptr;
	}

	if (!w || w == dying) {
		return nullptr;
	}

	w->dead.store(true, std::memory_order_seq_cst);
	while (w->pins.load(std::memory_order_seq_cst) != 0) {
		std::this_thread::yield();
	}
	return w;
}

} // namespace

weak_block *weak_get(object *obj)
{
	if (!obj) {
		return nullptr;
	}

	weak_block *w = obj->weak_.load(std::memory_order_acquire);
	if (!w) {
		auto *created = new weak_block(obj);
		if (obj->weak_.compare_exchange_strong(w, created, std::memory_order_acq_rel,
						       std::memory_order_acquire)) {
			w = created;
		} else {
			delete created;
		}
	}

	if (w == dying) {
		// Already dying: hand out an unattached block that is expired from the start, owned by the caller.
		return new weak_block(obj, true);
	}

	w->weak_refs.fetch_add(1, std::memory_order_relaxed);
	return w;
}

object *weak_lock(weak_block *w) noexcept
{
	if (!w) {
		return nullptr;
	}

	object *result = nullptr;
	// The pin keeps the object's memory alive while its count is touched.
	w->pins.fetch_add(1, std::memory_order_seq_cst);
	if (!w->dead.load(std::memory_order_seq_cst) && w->target->try_acquire()) {
		result = w->target;
	}
	w->pins.fetch_sub(1, std::memory_order_seq_cst);
	return result;
}

bool weak_expired(weak_block *w) noexcept
{
	return !w || w->dead.load(std::memory_order_acquire);
}

void weak_acquire(weak_block *w) noexcept
{
	if (w) {
		w->weak_refs.fetch_add(1, std::memory_order_relaxed);
	}
}

void weak_release(weak_block *w) noexcept
{
	if (w && w->weak_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
		delete w;
	}
}

} // namespace knurbs::detail

namespace knurbs {

object::object() noexcept : refs_(1), weak_(nullptr) {}

object::~object()
{
	// Runs right before the memory is freed, however destroy() deferred it, and also covers a forced destroy
	// whose count never reached 0. Drops the object's own reference to its weak block.
	detail::weak_release(detail::mark_dead(weak_, true));
}

void object::destroy() noexcept
{
	delete this;
}

void object::acquire() noexcept
{
	[[maybe_unused]] const long previous = refs_.fetch_add(1, std::memory_order_relaxed);
	// 0 means the object is being destroyed: reviving it would destroy it twice.
	assert(previous > 0 && "object::acquire on an object without references");
}

bool object::try_acquire() noexcept
{
	long n = refs_.load(std::memory_order_relaxed);
	while (n > 0) {
		if (refs_.compare_exchange_weak(n, n + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
			return true;
		}
	}
	return false;
}

void object::release() noexcept
{
	if (refs_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
		return;
	}

	detail::mark_dead(weak_, false);
	destroy();
}

long object::use_count() const noexcept
{
	return refs_.load(std::memory_order_relaxed);
}

} // namespace knurbs
