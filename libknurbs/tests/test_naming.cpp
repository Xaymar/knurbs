#include "test.hpp"

#include "knurbs/error.hpp"
#include "knurbs/naming.hpp"

using knurbs::naming::check_key;
using knurbs::naming::is_valid_key;
using knurbs::naming::is_valid_type_id;

static void test_valid_keys(void**)
{
	assert_true(is_valid_key("frame-rate"));
	assert_true(is_valid_key("dshow.control.gain"));
	assert_true(is_valid_key("a1"));

	assert_false(is_valid_key(""));
	assert_false(is_valid_key("Frame"));
	assert_false(is_valid_key("frame_rate"));
	assert_false(is_valid_key("-a"));
	assert_false(is_valid_key("a."));
	assert_false(is_valid_key("a..b"));
	assert_false(is_valid_key("a b"));
	assert_false(is_valid_key("\xC3\xBC"));
}

static void test_type_ids(void**)
{
	assert_true(is_valid_type_id("com.xaymar.knurbs.capture.video-device"));
	assert_true(is_valid_type_id("com.xaymar.knurbs"));

	assert_false(is_valid_type_id("video-device"));
	assert_false(is_valid_type_id("com.xaymar"));
	assert_false(is_valid_type_id("1com.x.y"));
}

static void test_check_key_throws(void**)
{
	KNURBS_ASSERT_THROWS(check_key("Bad_Key"), knurbs::error::invalid_argument);
	check_key("good-key");
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_valid_keys),
		cmocka_unit_test(test_type_ids),
		cmocka_unit_test(test_check_key_throws),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
