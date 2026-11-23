#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace knurbs {

	// A move-only, type-erased callable: libknurbs' own std::move_only_function, for the signatures R(A...) and
	// R(A...) noexcept. libknurbs uses it on every platform instead of std::move_only_function, which Apple's libc++
	// does not have; it appears in public signatures, so it must be one type everywhere.
	//
	// The callable lives on the heap. An empty function (default-constructed, from nullptr, from a null function
	// pointer or a moved-from one) is false; calling it is a precondition violation (asserted in builds without
	// NDEBUG). Its layout is three pointers and is frozen at 1.0, like the rest of the public API. Not thread-safe:
	// like any other object, one function must not be called and changed at the same time.
	template<class Signature>
	class move_only_function;

	namespace detail {

		// The whole of move_only_function, for both kinds of signature.
		template<bool Noexcept, class R, class... A>
		class move_only_function_impl {
			// Not noexcept(Noexcept), even for a noexcept signature: clang 18 crashes taking the address of invoke<F> with a
			// dependent exception specification. A noexcept function only ever holds a callable that cannot throw, and its
			// operator() is noexcept itself.
			using invoke_fn  = R (*)(void*, A&&...);
			using destroy_fn = void (*)(void*) noexcept;

			void*      object_  = nullptr;
			invoke_fn  invoke_  = nullptr;
			destroy_fn destroy_ = nullptr;

			template<class F>
			static R invoke(void* object, A&&... args)
			{
				if constexpr (std::is_void_v<R>) {
					std::invoke(*static_cast<F*>(object), std::forward<A>(args)...);
				} else {
					return std::invoke(*static_cast<F*>(object), std::forward<A>(args)...);
				}
			}

			template<class F>
			static void destroy(void* object) noexcept
			{
				delete static_cast<F*>(object);
			}

			void reset() noexcept
			{
				destroy_fn destroy = std::exchange(destroy_, nullptr);
				void*      object  = std::exchange(object_, nullptr);
				invoke_            = nullptr;
				if (destroy) {
					destroy(object);
				}
			}

			// What a function can be made from: a callable of the signature, but not another function or nullptr. Two
			// separate constraints, so a function argument never reaches the second one (it would ask whether the
			// function is constructible from itself, which asks this again).
			template<class F>
			static constexpr bool other_than_function = !std::is_base_of_v<move_only_function_impl, std::remove_cvref_t<F>> && !std::is_same_v<std::remove_cvref_t<F>, std::nullptr_t>;
			template<class F>
			static constexpr bool callable = std::is_constructible_v<std::decay_t<F>, F> && (Noexcept ? std::is_nothrow_invocable_r_v<R, std::decay_t<F>&, A...> : std::is_invocable_r_v<R, std::decay_t<F>&, A...>);

			public:
			using result_type = R;

			move_only_function_impl() noexcept = default;
			move_only_function_impl(std::nullptr_t) noexcept {}

			template<class F>
				requires other_than_function<F> && callable<F>
			move_only_function_impl(F&& f)
			{
				using D = std::decay_t<F>;
				if constexpr (std::is_pointer_v<D> || std::is_member_pointer_v<D>) {
					if (f == nullptr) {
						return;
					}
				}
				object_  = new D(std::forward<F>(f));
				invoke_  = &invoke<D>;
				destroy_ = &destroy<D>;
			}

			move_only_function_impl(move_only_function_impl&& other) noexcept : object_(std::exchange(other.object_, nullptr)), invoke_(std::exchange(other.invoke_, nullptr)), destroy_(std::exchange(other.destroy_, nullptr)) {}

			move_only_function_impl(const move_only_function_impl&)            = delete;
			move_only_function_impl& operator=(const move_only_function_impl&) = delete;

			move_only_function_impl& operator=(move_only_function_impl&& other) noexcept
			{
				move_only_function_impl(std::move(other)).swap(*this);
				return *this;
			}

			move_only_function_impl& operator=(std::nullptr_t) noexcept
			{
				reset();
				return *this;
			}

			template<class F>
				requires other_than_function<F> && callable<F>
			move_only_function_impl& operator=(F&& f)
			{
				move_only_function_impl(std::forward<F>(f)).swap(*this);
				return *this;
			}

			~move_only_function_impl()
			{
				reset();
			}

			void swap(move_only_function_impl& other) noexcept
			{
				std::swap(object_, other.object_);
				std::swap(invoke_, other.invoke_);
				std::swap(destroy_, other.destroy_);
			}

			friend void swap(move_only_function_impl& a, move_only_function_impl& b) noexcept
			{
				a.swap(b);
			}

			explicit operator bool() const noexcept
			{
				return invoke_ != nullptr;
			}

			friend bool operator==(const move_only_function_impl& f, std::nullptr_t) noexcept
			{
				return !f;
			}

			R operator()(A... args) noexcept(Noexcept)
			{
				assert(invoke_ && "knurbs::move_only_function: called while empty");
				return invoke_(object_, std::forward<A>(args)...);
			}
		};

	} // namespace detail

	template<class R, class... A>
	class move_only_function<R(A...)> : public detail::move_only_function_impl<false, R, A...> {
		public:
		using detail::move_only_function_impl<false, R, A...>::move_only_function_impl;
		using detail::move_only_function_impl<false, R, A...>::operator=;
	};

	template<class R, class... A>
	class move_only_function<R(A...) noexcept> : public detail::move_only_function_impl<true, R, A...> {
		public:
		using detail::move_only_function_impl<true, R, A...>::move_only_function_impl;
		using detail::move_only_function_impl<true, R, A...>::operator=;
	};

} // namespace knurbs
