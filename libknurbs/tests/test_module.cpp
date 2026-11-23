#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "knurbs/c/data.h"
#include "knurbs/c/module.h"
#include "knurbs/c/object.h"
#include "knurbs/c/properties.h"
#include "knurbs/c/registry.h"
#include "knurbs/core.hpp"
#include "knurbs/data.hpp"
#include "knurbs/error.hpp"
#include "knurbs/log.hpp"
#include "knurbs/module.hpp"
#include "knurbs/platform.hpp"
#include "knurbs/properties.hpp"
#include "knurbs/registry.hpp"

#include "test.hpp"

namespace fs = std::filesystem;

using knurbs::core;
using knurbs::module;
using knurbs::module_manager;
using knurbs::ref;

namespace {

// The fake modules, built by tests/CMakeLists.txt; sorted by name.
const char *const fake_names[] = {"fake-c", "fake-ok", "fake-other-major", "fake-throws"};
// fake-c also leaves a marker when its type's param is freed.
const char *const entry_points[] = {"load", "post_load", "unload", "free_param"};

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

	assert_int_equal(found.size(), 4);
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
	assert_int_equal(found.size(), 4);
	assert_string_equal(found[1].name.c_str(), "fake-ok");
	assert_true(found[1].binary.parent_path() == fs::absolute(root / "fake-ok" / "bin"));
	assert_true(log->contains("module fake-ok: also found in "));
}

static void test_load_registers_types(void **)
{
	auto c = make_core();
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));
	assert_int_equal(mods.size(), 4);

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

static void test_reject_c_other_major(void **)
{
	// fake-c built for API major version 99, in a directory of its own so the other tests do not see it.
	auto log = std::make_shared<log_capture>();
	auto c = make_core(log);
	auto mods = c->modules().load(c->modules().discover({KNURBS_TEST_OTHER_MODULES_DIR}));

	assert_int_equal(mods.size(), 1);
	assert_true(mods[0]->failed());
	assert_false(marker_exists("fake-c", "load"));
	assert_false(static_cast<bool>(c->registry().find("com.example.fake.c")));
	std::string line = log->line_with("module fake-c-other-major: ");
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
	assert_true(by_name(mods, "fake-ok")->name() == "fake-ok");
	KNURBS_ASSERT_THROWS(c->modules().load({}), knurbs::error::core_gone);
}

// A module held past its core (a host that keeps its module list in a member declared before its ref<core>) is
// still valid memory, but the core it points to is gone.
static void test_module_outlives_core(void **)
{
	std::vector<ref<module>> mods;
	{
		auto c = make_core();
		mods = c->modules().load(c->modules().discover({modules_dir()}));
	}
	const ref<module> &ok = by_name(mods, "fake-ok");
	assert_true(ok->name() == "fake-ok");
	assert_false(ok->failed());
	assert_true(static_cast<bool>(ok->logger()));
	KNURBS_ASSERT_THROWS(ok->core(), knurbs::error::core_gone);
	KNURBS_ASSERT_THROWS(ok->registry(), knurbs::error::core_gone);
	KNURBS_ASSERT_THROWS(ok->registry().find("com.example.fake.ok"), knurbs::error::core_gone);
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
	assert_int_equal(second.size(), 4);
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

static void test_load_c_module(void **)
{
	auto c = make_core();
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));

	assert_false(by_name(mods, "fake-c")->failed());
	assert_true(marker_exists("fake-c", "load"));
	// fake-c's post_load found its type through knurbs_module_core and knurbs_registry_find.
	assert_true(marker_exists("fake-c", "post_load"));

	{
		auto t = c->registry().find("com.example.fake.c");
		assert_true(static_cast<bool>(t));
		assert_true(t->id() == "com.example.fake.c");
		assert_true(t->kind() == knurbs::type_kind::source);
		assert_int_equal(t->version(), 2);
		auto settings = knurbs::data::create();
		t->defaults(*settings);
		assert_int_equal(settings->get_int("gain"), 5);
		auto props = t->properties(*settings);
		assert_true(static_cast<bool>(props->find("gain")));

		// The alias fake-c added through the C API resolves through the C++ find.
		assert_ptr_equal(c->registry().find("fake_c_old").get(), t.get());
		assert_true(c->registry().list(knurbs::type_kind::source).size() >= 2);
	}

	// Both were recorded as fake-c's; dropping the last reference to the type frees its param.
	c->registry().remove_module("fake-c");
	assert_false(static_cast<bool>(c->registry().find("com.example.fake.c")));
	assert_false(static_cast<bool>(c->registry().find("fake_c_old")));
	assert_true(marker_exists("fake-c", "free_param"));

	c->shutdown();
	assert_true(marker_exists("fake-c", "unload"));
}

namespace {

// The C handle of a module: the address of its knurbs::object (see knurbs/c/object.h).
knurbs_module_t *c_handle(const ref<module> &m)
{
	return static_cast<knurbs_module_t *>(static_cast<void *>(static_cast<knurbs::object *>(m.get())));
}

// When set, count_free calls into the registry of this core, which would deadlock if free_param ran under the
// registry's lock.
knurbs_core_t *reentry_core = nullptr;

void count_free(void *param)
{
	++*static_cast<int *>(param);
	if (reentry_core) {
		knurbs_release(
			reinterpret_cast<knurbs_object_t *>(knurbs_registry_find(reentry_core, "com.example.fake.ok")));
	}
}

knurbs_core_t *c_handle(const ref<core> &c)
{
	return static_cast<knurbs_core_t *>(static_cast<void *>(static_cast<knurbs::object *>(c.get())));
}

int defaults_calls = 0;

void set_level(void *, knurbs_data_t *settings)
{
	++defaults_calls;
	knurbs_data_set_default_int(settings, "level", 3);
}

knurbs_properties_t *no_properties(void *, const knurbs_data_t *)
{
	return nullptr;
}

knurbs_properties_t *one_property(void *, const knurbs_data_t *)
{
	knurbs_properties_t *props = nullptr;
	knurbs_property_t *prop = nullptr;
	if (knurbs_properties_create(&props) == KNURBS_OK &&
	    knurbs_properties_add_bool(props, "on", "On", &prop) == KNURBS_OK) {
		knurbs_release(reinterpret_cast<knurbs_object_t *>(prop));
	}
	return props;
}

struct knurbs_type_info make_type_info(const char *id, int *freed)
{
	return {sizeof(struct knurbs_type_info), id, KNURBS_TYPE_FILTER, 1, nullptr, nullptr, freed, count_free};
}

// A later version of knurbs_type_info, with a field this version does not know.
struct type_info_v2 {
	struct knurbs_type_info info;
	std::uint64_t unknown;
};

} // namespace

static void test_c_registry_add_checks_info(void **)
{
	auto c = make_core();
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));
	knurbs_module_t *m = c_handle(by_name(mods, "fake-ok"));
	int freed = 0;
	reentry_core = c_handle(c);

	// Without a readable struct the library cannot take ownership, so free_param does not run.
	assert_int_equal(knurbs_registry_add(m, nullptr), KNURBS_ERROR_INVALID_ARGUMENT);
	auto info = make_type_info("com.example.c.checked", &freed);
	info.size = offsetof(struct knurbs_type_info, free_param);
	assert_int_equal(knurbs_registry_add(m, &info), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(freed, 0);

	// Every other failure frees the param at once.
	info = make_type_info(nullptr, &freed);
	assert_int_equal(knurbs_registry_add(m, &info), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(freed, 1);
	info = make_type_info("Not A Type ID", &freed);
	assert_int_equal(knurbs_registry_add(m, &info), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(freed, 2);
	info = make_type_info("com.example.c.checked", &freed);
	info.kind = 99;
	assert_int_equal(knurbs_registry_add(m, &info), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(freed, 3);
	info = make_type_info("com.example.c.checked", &freed);
	assert_int_equal(knurbs_registry_add(nullptr, &info), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(freed, 4);
	info = make_type_info("com.example.fake.ok", &freed);
	assert_int_equal(knurbs_registry_add(m, &info), KNURBS_ERROR_ALREADY_EXISTS);
	assert_int_equal(freed, 5);
	assert_true(std::string_view(knurbs_last_error()).find("com.example.fake.ok") != std::string_view::npos);

	// A larger struct from a later version is fine; the id is copied.
	char id[] = "com.example.c.checked";
	type_info_v2 v2{make_type_info(id, &freed), 0};
	v2.info.size = sizeof(v2);
	v2.info.defaults = set_level;
	v2.info.properties = no_properties;
	assert_int_equal(knurbs_registry_add(m, &v2.info), KNURBS_OK);
	id[0] = 'x';
	assert_int_equal(freed, 5);

	{
		auto t = c->registry().find("com.example.c.checked");
		assert_true(static_cast<bool>(t));
		assert_true(t->id() == "com.example.c.checked");
		assert_true(t->kind() == knurbs::type_kind::filter);
		auto settings = knurbs::data::create();
		t->defaults(*settings);
		assert_int_equal(settings->get_int("level"), 3);
		// NULL from the properties callback means no properties.
		assert_true(t->properties(*settings)->list().empty());

		// Through the C handle as well.
		defaults_calls = 0;
		knurbs_type_t *ct = knurbs_registry_find(c_handle(c), "com.example.c.checked");
		assert_ptr_equal(static_cast<void *>(ct), static_cast<void *>(static_cast<knurbs::object *>(t.get())));
		knurbs_data_t *cs = nullptr;
		assert_int_equal(knurbs_data_create(&cs), KNURBS_OK);
		assert_int_equal(knurbs_type_defaults(ct, cs), KNURBS_OK);
		assert_int_equal(defaults_calls, 1);
		knurbs_release(reinterpret_cast<knurbs_object_t *>(cs));
		knurbs_release(reinterpret_cast<knurbs_object_t *>(ct));
	}

	// Recorded as fake-ok's although it was added outside fake-ok's entry points; free_param runs once the type
	// is gone.
	c->registry().remove_module("fake-ok");
	assert_false(static_cast<bool>(c->registry().find("com.example.c.checked")));
	assert_int_equal(freed, 6);
	reentry_core = nullptr;
}

static void test_c_registry_optional_callbacks(void **)
{
	auto c = make_core();
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));
	knurbs_module_t *m = c_handle(by_name(mods, "fake-ok"));

	// No callbacks and no free_param at all.
	struct knurbs_type_info bare = {sizeof(struct knurbs_type_info),
					"com.example.c.bare",
					KNURBS_TYPE_SERVICE,
					7,
					nullptr,
					nullptr,
					nullptr,
					nullptr};
	assert_int_equal(knurbs_registry_add(m, &bare), KNURBS_OK);
	auto t = c->registry().find("com.example.c.bare");
	assert_true(static_cast<bool>(t));
	assert_true(t->kind() == knurbs::type_kind::service);
	assert_int_equal(t->version(), 7);
	auto settings = knurbs::data::create();
	t->defaults(*settings);
	assert_true(settings->keys().empty());
	assert_true(t->properties(*settings)->list().empty());

	// The properties callback returns a reference that the library adopts (a leak or a double release shows up
	// under the sanitizers).
	int freed = 0;
	auto info = make_type_info("com.example.c.props", &freed);
	info.properties = one_property;
	assert_int_equal(knurbs_registry_add(m, &info), KNURBS_OK);
	auto p = c->registry().find("com.example.c.props")->properties(*settings);
	assert_int_equal(p->list().size(), 1);
	assert_true(static_cast<bool>(p->find("on")));
}

static void test_c_registry_add_alias(void **)
{
	auto c = make_core();
	auto mods = c->modules().load(c->modules().discover({modules_dir()}));
	knurbs_module_t *m = c_handle(by_name(mods, "fake-ok"));

	// The module's core is a borrowed handle: no reference is taken.
	const long refs = c->use_count();
	assert_ptr_equal(knurbs_module_core(m), c_handle(c));
	assert_int_equal(c->use_count(), refs);

	assert_int_equal(knurbs_registry_add_alias(m, "fake_ok_old", "com.example.fake.ok"), KNURBS_OK);
	assert_ptr_equal(c->registry().find("fake_ok_old").get(), c->registry().find("com.example.fake.ok").get());
	assert_int_equal(knurbs_registry_add_alias(m, "fake_ok_old", "com.example.fake.ok"),
			 KNURBS_ERROR_ALREADY_EXISTS);
	assert_int_equal(knurbs_registry_add_alias(m, "x", "Not A Type ID"), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_registry_add_alias(nullptr, "y", "com.example.fake.ok"), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_registry_add_alias(m, nullptr, "com.example.fake.ok"), KNURBS_ERROR_INVALID_ARGUMENT);
	assert_int_equal(knurbs_registry_add_alias(m, "y", nullptr), KNURBS_ERROR_INVALID_ARGUMENT);

	// The alias is recorded as the module's, so remove_module drops it.
	c->registry().remove_module("fake-ok");
	assert_false(static_cast<bool>(c->registry().find("fake_ok_old")));
}

// The same through the C API: a module handle that a C module (or host) kept with knurbs_ref.
static void test_c_module_outlives_core(void **)
{
	knurbs_module_t *m = nullptr;
	{
		auto c = make_core();
		auto mods = c->modules().load(c->modules().discover({modules_dir()}));
		m = c_handle(by_name(mods, "fake-c"));
		knurbs_ref(reinterpret_cast<knurbs_object_t *>(m));
	}

	assert_null(knurbs_module_core(m));
	assert_true(std::string_view(knurbs_last_error()).find("gone") != std::string_view::npos);

	// The library still owns param: it is freed at once.
	int freed = 0;
	auto info = make_type_info("com.example.c.late", &freed);
	assert_int_equal(knurbs_registry_add(m, &info), KNURBS_ERROR_CORE_GONE);
	assert_int_equal(freed, 1);
	assert_int_equal(knurbs_registry_add_alias(m, "c_late_old", "com.example.fake.c"), KNURBS_ERROR_CORE_GONE);

	knurbs_release(reinterpret_cast<knurbs_object_t *>(m));
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
		cmocka_unit_test_setup_teardown(test_reject_c_other_major, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_unload_on_shutdown, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_module_outlives_core, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_unload_all_runs_once, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_duplicate_name_fails, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_two_cores_do_not_interfere, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_load_c_module, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_c_registry_add_checks_info, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_c_registry_optional_callbacks, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_c_registry_add_alias, remove_markers, remove_markers),
		cmocka_unit_test_setup_teardown(test_c_module_outlives_core, remove_markers, remove_markers),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
