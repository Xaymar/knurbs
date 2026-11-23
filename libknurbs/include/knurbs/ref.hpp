#pragma once

#include <concepts>
#include <utility>

#include "knurbs/object.hpp"

namespace knurbs {

// Strong reference to a knurbs::object. Empty or holding exactly one count.
template<class T> class ref {
	T *ptr_ = nullptr;

	explicit ref(T *p) noexcept : ptr_(p) {}

	template<class U> friend class ref;

public:
	ref() noexcept = default;

	// Takes over a count the caller already owns.
	static ref adopt(T *p) noexcept { return ref(p); }
	// Takes a new count on `p`, which the caller must keep alive for the call.
	static ref retain(T *p) noexcept
	{
		if (p) {
			p->acquire();
		}
		return ref(p);
	}

	ref(const ref &other) noexcept : ref(retain(other.ptr_)) {}
	ref(ref &&other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

	template<class U> requires std::convertible_to<U *, T *> ref(const ref<U> &other) noexcept : ptr_(other.ptr_)
	{
		if (ptr_) {
			ptr_->acquire();
		}
	}

	template<class U>
	requires std::convertible_to<U *, T *> ref(ref<U> &&other) noexcept : ptr_(std::exchange(other.ptr_, nullptr))
	{
	}

	ref &operator=(const ref &other) noexcept
	{
		ref(other).swap(*this);
		return *this;
	}

	ref &operator=(ref &&other) noexcept
	{
		ref(std::move(other)).swap(*this);
		return *this;
	}

	~ref() { reset(); }

	T *get() const noexcept { return ptr_; }
	T *operator->() const noexcept { return ptr_; }
	T &operator*() const noexcept { return *ptr_; }
	explicit operator bool() const noexcept { return ptr_ != nullptr; }

	void reset() noexcept
	{
		if (T *p = std::exchange(ptr_, nullptr)) {
			p->release();
		}
	}

	// Gives the count to the caller (for the C API); the ref is empty afterwards.
	T *detach() noexcept { return std::exchange(ptr_, nullptr); }

	void swap(ref &other) noexcept { std::swap(ptr_, other.ptr_); }
};

// Weak reference to a knurbs::object: does not keep it alive, and lock() fails once it is dying.
template<class T> class weak {
	detail::weak_block *block_ = nullptr;

public:
	weak() noexcept = default;
	explicit weak(const ref<T> &r) : block_(detail::weak_get(r.get())) {}

	weak(const weak &other) noexcept : block_(other.block_) { detail::weak_acquire(block_); }
	weak(weak &&other) noexcept : block_(std::exchange(other.block_, nullptr)) {}

	weak &operator=(const weak &other) noexcept
	{
		weak(other).swap(*this);
		return *this;
	}

	weak &operator=(weak &&other) noexcept
	{
		weak(std::move(other)).swap(*this);
		return *this;
	}

	~weak() { detail::weak_release(block_); }

	ref<T> lock() const noexcept { return ref<T>::adopt(static_cast<T *>(detail::weak_lock(block_))); }
	bool expired() const noexcept { return detail::weak_expired(block_); }

	void swap(weak &other) noexcept { std::swap(block_, other.block_); }
};

// Constructs a T with `new` and adopts its initial count.
template<class T, class... A> ref<T> make(A &&...args)
{
	return ref<T>::adopt(new T(std::forward<A>(args)...));
}

} // namespace knurbs
