#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "knurbs/core.hpp"
#include "knurbs/error.hpp"
#include "knurbs/log.hpp"
#include "knurbs/module.hpp"
#include "knurbs/platform.hpp"
#include "knurbs/registry.hpp"

#include "test.hpp"

namespace fs = std::filesystem;

using knurbs::core;
using knurbs::module;
using knurbs::module_manager;
using knurbs::ref;

namespace {

// The fake modules, built by tests/CMakeLists.txt; sorted by name.
const char *const fake_names[] = {"fake-ok", "fake-other-major", "fake-throws"};
const char *const entry_points[] = {"load", "post_load", "unload"};

fs::path modules_dir()
{
	return knurbs::platform::path_from_utf8(KNURBS_TEST_MODULES_DIR);
}

// The marker file a fake module writes into the working directory when `entry` ran.
bool marker_exists(std::string_view name, std::string_view entry)
{
	return fs::exists(std::string(name) + "." + std::string(entry));
}

// Collects everything the core logs.
struct log_capture {
	std::mutex mutex;
	std::vector<std::string> lines;

	bool contains(std::string_view text)
	{
		std::scoped_lock lock(mutex);
		return std::ranges::any_of(lines, [&](const std::string &l) { return l.find(text) != l.npos; });
	}

	// The first line that contains `text`, or "".
	std::string line_with(std::string_view text)
	{
		std::scoped_lock lock(mutex);
		for (const std::string &l : lines) {
			if (l.find(text) != l.npos) {
				return l;
			}
		}
		return {};
	}
};

ref<core> make_core(const std::shared_ptr<log_capture> &log = {})
{
	core::options opts{"config", "cache"};
	if (log) {
		opts.sink = [log](knurbs::log::level, std::string_view message) noexcept {
			try {
				std::scoped_lock lock(log->mutex);
				log->lines.emplace_back(message);
			} catch (...) {
			}
		};
	}
	return core::create(std::move(opts));
}

const ref<module> &by_name(const std::vector<ref<module>> &mods, std::string_view name)
{
	auto it = std::ranges::find_if(mods, [&](const ref<module> &m) { return m->name() == name; });
	if (it == mods.end()) {
		fail_msg("module %.*s was not loaded", static_cast<int>(name.size()), name.data());
	}
	return *it;
}

// Removes the marker files (and the copied module tree) that a test leaves behind.
int remove_markers(void **)
{
	std::error_code ec;
	for (const char *name : fake_names) {
		for (const char *entry : entry_points) {
			fs::remove(std::string(name) + "." + entry, ec);
		}
	}
	fs::remove_all("tmp", ec);
	return 0;
}

} // namespace

static void test_discover_without_loading(void **)
{
	auto c = make_core();
	const auto dir = modules_dir();
	auto found = c->modules().discover({dir, dir / "does-not-exist"});

	assert_int_equal(found.size(), 3);
	for (std::size_t i = 0; i < found.size(); ++i) {
		assert_string_equal(found[i].name.c_str(), fake_names[i]);
		assert_true(fs::is_regular_file(found[i].binary));
		assert_true(found[i].binary.parent_path() == fs::absolute(dir / fake_names[i] / "bin"));
		assert_true(found[i].data == fs::absolute(dir / fake_names[i] / "data"));
		assert_true(fs::is_directory(found[i].data));
		assert_false(marker_exists(fake_names[i], "load"));
	}
}

static void test_discover_non_ascii_dir(void **)
{
	// "tmp" is removed by remove_markers.
	const fs::path root = knurbs::platform::path_from_utf8("tmp/Jürgen/日本");
	fs::create_directories(root);
	fs::copy(modules_dir() / "fake-ok", root / "fake-ok", fs::copy_options::recursive);

	auto c = make_core();
	auto found = c->modules().discover({root});
	assert_int_equal(found.size(), 1);
	assert_string_equal(found[0].name.c_str(), "fake-ok");
	assert_true(found[0].binary.parent_path() == fs::absolute(root / "fake-ok" / "bin"));

	auto mods = c->modules().load(found);
	assert_int_equal(mods.size(), 1);
	assert_false(mods[0]->failed());
	assert_true(mods[0]->binary_path() == found[0].binary);
	assert_true(mods[0]->data_path() == found[0].data);
	assert_true(marker_exists("fake-ok", "load"));
	assert_true(static_cast<bool>(c->registry().find("com.example.fake.ok")));
}

static void test_discover_skips_non_modules(void **)
{
	auto log = std::make_shared<log_capture>();
	auto c = make_core(log);
	// "tmp" is removed by remove_markers.
	const fs::path root = knurbs::platform::path_from_utf8("tmp/search");
	fs::create_directories(root);
	fs::copy(modules_dir() / "fake-ok", root / "fake-ok", fs::copy_options::recursive);
	// Entries that are not modules, in numbers that make sure some are listed before fake-ok.
	for (int i = 0; i < 8; ++i) {
		fs::create_directories(root / std::format("no-bin-{}", i));
		fs::create_directories(root / std::format("empty-bin-{}", i) / "bin");
		std::ofstream(root / std::format("file-{}", i)) << "not a module\n";
	}

	// fake-ok is found in both directories; the first one wins.
	auto found = c->modules().discover({root, modules_dir()});
	assert_int_equal(found.size(), 3);
	assert_string_equal(found[0].name.c_str(), "fake-ok");
	assert_true(found[0].binary.parent_path() == fs::absolute(root / "fake-ok" / "bin"));
	assert_true(log->contains("module fake-ok: also found in "));
}

static void test_load_registers_types(void **)
{
	auto c = make_core();
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));
	assert_int_equal(mods.size(), 3);

	const ref<module> &ok = by_name(mods, "fake-ok");
	assert_false(ok->failed());
	assert_true(ok->name() == "fake-ok");
	assert_ptr_equal(&ok->core(), c.get());
	assert_ptr_equal(&ok->registry(), &c->registry());
	assert_ptr_equal(ok->logger().get(), c->logger().get());

	auto t = c->registry().find("com.example.fake.ok");
	assert_true(static_cast<bool>(t));
	assert_true(t->id() == "com.example.fake.ok");
	assert_true(marker_exists("fake-ok", "post_load"));

	// The type was added from fake-ok's load without an owner, so it is recorded as fake-ok's.
	c->registry().remove_module("fake-ok");
	assert_false(static_cast<bool>(c->registry().find("com.example.fake.ok")));
}

static void test_load_throwing_module(void **)
{
	auto log = std::make_shared<log_capture>();
	auto c = make_core(log);
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));

	const ref<module> &throws = by_name(mods, "fake-throws");
	assert_true(throws->failed());
	assert_true(marker_exists("fake-throws", "load"));
	assert_false(marker_exists("fake-throws", "post_load"));
	// Its type and its alias were both removed.
	assert_false(static_cast<bool>(c->registry().find("com.example.fake.throws")));
	assert_false(static_cast<bool>(c->registry().find("fake_throws_old")));

	assert_false(by_name(mods, "fake-ok")->failed());
	assert_true(static_cast<bool>(c->registry().find("com.example.fake.ok")));
	assert_true(log->contains("module fake-throws: load failed on purpose"));
}

static void test_reject_other_major(void **)
{
	auto log = std::make_shared<log_capture>();
	auto c = make_core(log);
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));

	assert_true(by_name(mods, "fake-other-major")->failed());
	assert_false(marker_exists("fake-other-major", "load"));
	std::string line = log->line_with("module fake-other-major: ");
	assert_true(line.find("version") != line.npos);
}

static void test_unload_on_shutdown(void **)
{
	auto c = make_core();
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));
	assert_false(marker_exists("fake-ok", "unload"));

	c->shutdown();
	assert_true(marker_exists("fake-ok", "unload"));
	// Failed modules are not unloaded.
	assert_false(marker_exists("fake-throws", "unload"));
	// The modules outlive the core's shutdown.
	assert_true(mods[0]->name() == "fake-ok");
	KNURBS_ASSERT_THROWS(c->modules().load({}), knurbs::error::core_gone);
}

static void test_unload_all_runs_once(void **)
{
	auto c = make_core();
	c->modules().load(c->modules().discover({modules_dir()}));
	c->modules().unload_all();
	assert_true(marker_exists("fake-ok", "unload"));

	remove_markers(nullptr);
	c->shutdown();
	assert_false(marker_exists("fake-ok", "unload"));
}

static void test_duplicate_name_fails(void **)
{
	auto log = std::make_shared<log_capture>();
	auto c = make_core(log);
	auto found = c->modules().discover({modules_dir()});
	auto first = c->modules().load(found);
	remove_markers(nullptr);

	auto second = c->modules().load(found);
	assert_int_equal(second.size(), 3);
	for (const auto &m : second) {
		assert_true(m->failed());
	}
	assert_false(marker_exists("fake-ok", "load"));
	assert_true(log->contains("module fake-ok: "));
	// The module that was loaded first keeps its types.
	assert_false(by_name(first, "fake-ok")->failed());
	assert_true(static_cast<bool>(c->registry().find("com.example.fake.ok")));
}

static void test_two_cores_do_not_interfere(void **)
{
	auto a = make_core();
	auto b = make_core();
	auto found = a->modules().discover({modules_dir()});
	a->modules().load(found);
	b->modules().load(found);
	assert_true(static_cast<bool>(a->registry().find("com.example.fake.ok")));
	assert_true(static_cast<bool>(b->registry().find("com.example.fake.ok")));

	a->registry().remove_module("fake-ok");
	assert_false(static_cast<bool>(a->registry().find("com.example.fake.ok")));
	assert_true(static_cast<bool>(b->registry().find("com.example.fake.ok")));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(test_discover_without_loading, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_discover_non_ascii_dir, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_discover_skips_non_modules, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_load_registers_types, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_load_throwing_module, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_reject_other_major, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_unload_on_shutdown, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_unload_all_runs_once, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_duplicate_name_fails, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_two_cores_do_not_interfere, remove_markers, remove_markers),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
