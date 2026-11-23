#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "knurbs/platform.hpp"

#include "test.hpp"

namespace fs = std::filesystem;

namespace {

	// Creates a unique directory under the system temp directory and removes it on destruction.
	class temp_dir {
		fs::path path_;

		public:
		temp_dir()
		{
			std::error_code ec;
			auto            base  = fs::temp_directory_path();
			auto            stamp = knurbs::platform::now_ns();
			for (int i = 0; i < 100; ++i) {
				auto candidate = base / ("knurbs-test-platform-" + std::to_string(stamp) + "-" + std::to_string(i));
				if (fs::create_directory(candidate, ec)) {
					path_ = candidate;
					return;
				}
			}
			fail_msg("could not create a temporary directory");
		}
		~temp_dir()
		{
			std::error_code ec;
			if (!path_.empty()) {
				fs::remove_all(path_, ec);
			}
		}
		temp_dir(const temp_dir&)            = delete;
		temp_dir& operator=(const temp_dir&) = delete;

		const fs::path& path() const
		{
			return path_;
		}
	};

} // namespace

static void test_now_monotonic(void**)
{
	std::uint64_t prev = knurbs::platform::now_ns();
	for (int i = 0; i < 10000; ++i) {
		std::uint64_t cur = knurbs::platform::now_ns();
		assert_true(cur >= prev);
		prev = cur;
	}
}

static void test_path_roundtrip_non_ascii(void**)
{
	const std::string s = "Jürgen/日本/ü.ini";
	assert_true(knurbs::platform::path_to_utf8(knurbs::platform::path_from_utf8(s)) == s);

	temp_dir dir;
	fs::path file = dir.path() / knurbs::platform::path_from_utf8(s);
	fs::create_directories(file.parent_path());
	{
		std::ofstream out(file, std::ios::binary);
		assert_true(out.is_open());
		out << "hello";
	}
	std::ifstream in(file, std::ios::binary);
	assert_true(in.is_open());
	std::string content;
	in >> content;
	assert_string_equal(content.c_str(), "hello");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_now_monotonic),
		cmocka_unit_test(test_path_roundtrip_non_ascii),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
