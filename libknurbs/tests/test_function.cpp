#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "knurbs/function.hpp"

#include "test.hpp"

using knurbs::move_only_function;

namespace {

	// Counts how often a callable's copies are destroyed while they still own the count (moved-from ones don't).
	struct counted {
		int* destroyed;
		int  value;

		counted(int* d, int v) noexcept : destroyed(d), value(v) {}
		counted(counted&& other) noexcept : destroyed(std::exchange(other.destroyed, nullptr)), value(other.value) {}
		counted(const counted&)            = delete;
		counted& operator=(const counted&) = delete;
		counted& operator=(counted&&)      = delete;
		~counted()
		{
			if (destroyed) {
				++*destroyed;
			}
		}

		int operator()() const
		{
			return value;
		}
	};

	struct box {
		int v;
		int get() const
		{
			return v;
		}
	};

	int twice(int x)
	{
		return 2 * x;
	}

	void bump(int& x) noexcept
	{
		++x;
	}

	using sink = move_only_function<void(int, std::string_view) noexcept>;

	// Type properties: move-only, and a noexcept signature only takes callables that cannot throw.
	static_assert(!std::is_copy_constructible_v<move_only_function<void()>>);
	static_assert(!std::is_copy_assignable_v<move_only_function<void()>>);
	static_assert(std::is_nothrow_move_constructible_v<move_only_function<void()>>);
	static_assert(std::is_nothrow_move_assignable_v<move_only_function<void()>>);
	static_assert(std::is_nothrow_invocable_v<sink&, int, std::string_view>);
	static_assert(!std::is_nothrow_invocable_v<move_only_function<void()>&>);
	static_assert(std::is_constructible_v<sink, void (*)(int, std::string_view) noexcept>);
	static_assert(!std::is_constructible_v<sink, void (*)(int, std::string_view)>);
	static_assert(!std::is_constructible_v<sink, decltype([](int, std::string_view) {})>);
	static_assert(std::is_constructible_v<sink, decltype([](int, std::string_view) noexcept {})>);
	static_assert(!std::is_constructible_v<move_only_function<int()>, decltype([] { return std::string(); })>);
	static_assert(!std::is_constructible_v<move_only_function<void(int)>, decltype([](std::string) {})>);
	// Its layout is part of the public API.
	static_assert(sizeof(move_only_function<void()>) == 3 * sizeof(void*));

} // namespace

static void test_empty(void**)
{
	move_only_function<void()> a;
	assert_false(static_cast<bool>(a));
	assert_true(a == nullptr);
	move_only_function<void()> b = nullptr;
	assert_false(static_cast<bool>(b));
	void (*null_fp)()            = nullptr;
	move_only_function<void()> c = null_fp;
	assert_false(static_cast<bool>(c));
	int (box::*null_member)() const       = nullptr;
	move_only_function<int(const box&)> d = null_member;
	assert_false(static_cast<bool>(d));
}

static void test_move_only_callable(void**)
{
	move_only_function<int(int)> f = [p = std::make_unique<int>(40)](int x) { return *p + x; };
	assert_true(static_cast<bool>(f));
	assert_false(f == nullptr);
	assert_int_equal(f(2), 42);

	// The result may be move-only too, and converts to R.
	move_only_function<std::unique_ptr<int>()> make = [] { return std::make_unique<int>(7); };
	assert_int_equal(*make(), 7);
	move_only_function<long()> widened = [] { return 5; };
	assert_int_equal(widened(), 5);
}

static void test_function_pointers_and_members(void**)
{
	move_only_function<int(int)> f = twice;
	assert_int_equal(f(21), 42);
	move_only_function<int(const box&)> get = &box::get;
	assert_int_equal(get(box{3}), 3);
}

static void test_reference_arguments(void**)
{
	int                                     x = 1;
	move_only_function<void(int&) noexcept> f = bump;
	f(x);
	assert_int_equal(x, 2);

	// By-value arguments are moved into the callable.
	move_only_function<std::size_t(std::unique_ptr<std::string>)> g = [](std::unique_ptr<std::string> s) { return s->size(); };
	assert_int_equal(g(std::make_unique<std::string>("four")), 4);
}

static void test_noexcept_signature(void**)
{
	int         calls = 0;
	std::string last;
	sink        s = [&](int n, std::string_view text) noexcept {
		calls += n;
		last = text;
	};
	s(2, "hello");
	assert_int_equal(calls, 2);
	assert_string_equal(last.c_str(), "hello");
}

static void test_destroyed_exactly_once(void**)
{
	int destroyed = 0;
	{
		move_only_function<int()> f = counted(&destroyed, 1);
		assert_int_equal(destroyed, 0);
		move_only_function<int()> g = std::move(f);
		move_only_function<int()> h;
		h = std::move(g);
		assert_int_equal(h(), 1);
		assert_int_equal(destroyed, 0);
	}
	assert_int_equal(destroyed, 1);

	// Replacing the target destroys the old one at once, and each new one later exactly once.
	destroyed = 0;
	{
		move_only_function<int()> f = counted(&destroyed, 1);
		f                           = counted(&destroyed, 2);
		assert_int_equal(destroyed, 1);
		assert_int_equal(f(), 2);
		move_only_function<int()> other = counted(&destroyed, 3);
		f                               = std::move(other);
		assert_int_equal(destroyed, 2);
		assert_int_equal(f(), 3);
		f = nullptr;
		assert_int_equal(destroyed, 3);
		assert_false(static_cast<bool>(f));
	}
	assert_int_equal(destroyed, 3);
}

static void test_moved_from_is_empty(void**)
{
	move_only_function<int()> f = [] { return 1; };
	move_only_function<int()> g = std::move(f);
	assert_false(static_cast<bool>(f)); // NOLINT(bugprone-use-after-move): that is the point
	assert_true(static_cast<bool>(g));

	move_only_function<int()> h = [] { return 2; };
	h                           = std::move(g);
	assert_false(static_cast<bool>(g)); // NOLINT(bugprone-use-after-move)
	assert_int_equal(h(), 1);

	// A moved-from function can be given a new target.
	g = [] { return 3; };
	assert_int_equal(g(), 3);
}

static void test_swap_and_self_move(void**)
{
	int destroyed = 0;
	{
		move_only_function<int()> a = counted(&destroyed, 1);
		move_only_function<int()> b = counted(&destroyed, 2);
		swap(a, b);
		assert_int_equal(a(), 2);
		assert_int_equal(b(), 1);
		a.swap(b);
		assert_int_equal(a(), 1);

		move_only_function<int()>& alias = a;
		a                                = std::move(alias);
		assert_true(static_cast<bool>(a));
		assert_int_equal(a(), 1);
		assert_int_equal(destroyed, 0);
	}
	assert_int_equal(destroyed, 2);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_empty), cmocka_unit_test(test_move_only_callable), cmocka_unit_test(test_function_pointers_and_members), cmocka_unit_test(test_reference_arguments), cmocka_unit_test(test_noexcept_signature), cmocka_unit_test(test_destroyed_exactly_once), cmocka_unit_test(test_moved_from_is_empty), cmocka_unit_test(test_swap_and_self_move),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
