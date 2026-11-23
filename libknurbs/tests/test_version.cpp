#include "test.hpp"

#include "knurbs/version.hpp"

static void test_header_matches_runtime(void **)
{
	assert_int_equal(knurbs::version::runtime_major(), knurbs::version::major);
	assert_int_equal(knurbs::version::runtime_minor(), knurbs::version::minor);
	assert_int_equal(knurbs::version::runtime_patch(), knurbs::version::patch);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_header_matches_runtime),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
