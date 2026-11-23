#include <filesystem>
#include <fstream>

#include "test.hpp"

#include "knurbs/error.hpp"
#include "knurbs/locale.hpp"

using knurbs::ref;
using knurbs::locale::text_table;
namespace error = knurbs::error;
namespace fs = std::filesystem;

static void write_file(const fs::path &path, const std::string &content)
{
	std::ofstream out(path, std::ios::binary);
	out << content;
}

// A fresh directory under the test's working directory.
static fs::path fresh_dir(const char *name)
{
	fs::path dir = fs::current_path() / name;
	fs::remove_all(dir);
	fs::create_directories(dir);
	return dir;
}

static void test_locale_fallback(void **)
{
	ref<text_table> table = text_table::load(KNURBS_TEST_DATA_DIR "/locale", "de-de");
	assert_string_equal(std::string(table->get("a")).c_str(), "\xC3\x84");
	assert_string_equal(std::string(table->get("b")).c_str(), "B");
	assert_string_equal(std::string(table->get("c")).c_str(), "c");
}

static void test_locale_missing_fallback_throws(void **)
{
	fs::path dir = fresh_dir("empty");
	KNURBS_ASSERT_THROWS(text_table::load(dir, "de-de"), error::io);
}

static void test_locale_missing_requested_is_fine(void **)
{
	ref<text_table> table = text_table::load(KNURBS_TEST_DATA_DIR "/locale", "fr-fr");
	assert_string_equal(std::string(table->get("a")).c_str(), "A");
}

static void test_locale_syntax(void **)
{
	fs::path dir = fresh_dir("syntax");
	// BOM, CRLF, comments, escapes, and lines that must be ignored.
	write_file(dir / "en-us.ini", "\xEF\xBB\xBF# comment\r\n; another\r\n"
				      "esc = \"line1\\nline2 \\\"q\\\" back\\\\slash \\t\"\r\n"
				      "noquote=abc\r\n"
				      "no equals sign\r\n"
				      "empty=\"\"\r\n"
				      "  spaced  =  \"x\"  \r\n"
				      "last=\"end\"");
	ref<text_table> table = text_table::load(dir, "en-us");
	assert_string_equal(std::string(table->get("esc")).c_str(), "line1\nline2 \"q\" back\\slash \\t");
	assert_string_equal(std::string(table->get("noquote")).c_str(), "noquote");
	assert_string_equal(std::string(table->get("no equals sign")).c_str(), "no equals sign");
	assert_true(table->get("empty").empty());
	assert_string_equal(std::string(table->get("spaced")).c_str(), "x");
	assert_string_equal(std::string(table->get("last")).c_str(), "end");
}

static void test_locale_rejects_bad_names(void **)
{
	const char *dir = KNURBS_TEST_DATA_DIR "/locale";
	for (const char *bad : {"../x", "/etc/x", "de_DE", ""}) {
		KNURBS_ASSERT_THROWS(text_table::load(dir, bad), error::invalid_argument);
		KNURBS_ASSERT_THROWS(text_table::load(dir, "de-de", bad), error::invalid_argument);
	}
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_locale_fallback),
		cmocka_unit_test(test_locale_missing_fallback_throws),
		cmocka_unit_test(test_locale_missing_requested_is_fine),
		cmocka_unit_test(test_locale_syntax),
		cmocka_unit_test(test_locale_rejects_bad_names),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
