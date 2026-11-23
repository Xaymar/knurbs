#include "test.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

#include "knurbs/error.hpp"

namespace e = knurbs::error;

template<typename T> static void check_subclass(e::code expected)
{
	T ex("message " + std::to_string(static_cast<int>(expected)));
	assert_int_equal(static_cast<int>(ex.value()), static_cast<int>(expected));
	assert_string_equal(ex.what(), ("message " + std::to_string(static_cast<int>(expected))).c_str());
}

static void test_subclass_codes(void **)
{
	check_subclass<e::invalid_argument>(e::code::invalid_argument);
	check_subclass<e::not_found>(e::code::not_found);
	check_subclass<e::already_exists>(e::code::already_exists);
	check_subclass<e::out_of_range>(e::code::out_of_range);
	check_subclass<e::unsupported>(e::code::unsupported);
	check_subclass<e::io>(e::code::io);
	check_subclass<e::core_gone>(e::code::core_gone);
	check_subclass<e::gpu_lost>(e::code::gpu_lost);
	check_subclass<e::plugin_failed>(e::code::plugin_failed);
	check_subclass<e::version_mismatch>(e::code::version_mismatch);
}

static void test_catch_as_base(void **)
{
	KNURBS_ASSERT_THROWS(throw e::not_found("x"), e::exception);
	KNURBS_ASSERT_THROWS(throw e::not_found("x"), std::runtime_error);
	KNURBS_ASSERT_THROWS(throw e::not_found("x"), e::not_found);
}

static void test_to_string(void **)
{
	assert_true(e::to_string(e::code::core_gone) == "core-gone");
	assert_true(e::to_string(e::code::invalid_argument) == "invalid-argument");
	assert_true(e::to_string(e::code::ok) == "ok");
	assert_true(e::to_string(static_cast<e::code>(999)) == "unknown");
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_subclass_codes),
		cmocka_unit_test(test_catch_as_base),
		cmocka_unit_test(test_to_string),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
