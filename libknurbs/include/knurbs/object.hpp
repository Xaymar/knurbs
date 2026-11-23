#pragma once

#include <atomic>

#include "knurbs/export.hpp"

namespace knurbs {

	class object;

	namespace detail {

		// Weak reference control block, defined in source/object.cpp. Created the first time a weak reference is
		// requested and shared by every weak reference to the same object.
		struct weak_block;

		// Returns a new weak reference to `obj` (the caller owns it and frees it with weak_release), or nullptr for a
		// null `obj`. `obj` must be alive: the caller holds a strong reference, or runs inside its destruction before
		// the delete. Requested while `obj` is dying, the reference is already expired.
		KNURBS_API weak_block* weak_get(object* obj);
		// Returns the object with a strong reference the caller owns, or nullptr once the object is dying.
		KNURBS_API object* weak_lock(weak_block* w) noexcept;
		// True once the object's last strong reference is gone (or for a null `w`).
		KNURBS_API bool weak_expired(weak_block* w) noexcept;
		KNURBS_API void weak_acquire(weak_block* w) noexcept;
		KNURBS_API void weak_release(weak_block* w) noexcept;

	} // namespace detail

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251) // std::atomic members of an exported class
#endif

	// Intrusive reference-counting base of every libknurbs object. Hold objects through knurbs::ref<T> and
	// knurbs::weak<T> (knurbs/ref.hpp) rather than calling these directly.
	class KNURBS_API object {
		// Frozen at 1.0. The data members of the public API are these, deferred_object::queue_ (knurbs/thread.hpp)
		// and the exception classes' code_ with their std::runtime_error base (knurbs/error.hpp).
		std::atomic<long>                refs_;
		std::atomic<detail::weak_block*> weak_;

		friend detail::weak_block* detail::weak_get(object* obj);

		public:
		object(const object&)            = delete;
		object(object&&)                 = delete;
		object& operator=(const object&) = delete;
		object& operator=(object&&)      = delete;

		// Unconditional increment; the caller must already hold a reference, or know someone else does. Never on an
		// object whose last reference is gone (inside its destroy()): that revives it, and it is destroyed twice.
		// Asserted in builds without NDEBUG.
		void acquire() noexcept;
		// Increments only while the count is above 0. Returns false once the object is dying.
		bool try_acquire() noexcept;
		// Decrements; the release that reaches 0 calls destroy().
		void release() noexcept;
		long use_count() const noexcept;

		protected:
		// The count starts at 1, owned by the creator.
		object() noexcept;
		virtual ~object();

		// Called once, when the last reference is released. The default runs `delete this`; overrides may defer
		// it to another context, as long as the object is eventually deleted.
		virtual void destroy() noexcept;
	};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace knurbs
