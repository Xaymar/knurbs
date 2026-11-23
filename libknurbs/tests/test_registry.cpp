#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "knurbs/core.hpp"
#include "knurbs/error.hpp"
#include "knurbs/registry.hpp"

#include "test.hpp"

using knurbs::core;
using knurbs::ref;
using knurbs::type;
using knurbs::type_kind;

namespace {

core::options test_options()
{
	return core::options{"config", "cache"};
}

std::atomic<int> reentrant_destroyed{0};

class fake_type final : public type {
	std::string id_;
	type_kind kind_;
	std::atomic<int> *destroyed_;
	const core *core_;
	std::atomic<int> *destroyed_after_shutdown_;

public:
	explicit fake_type(std::string id, type_kind kind = type_kind::source, std::atomic<int> *destroyed = nullptr,
			   const core *c = nullptr, std::atomic<int> *after_shutdown = nullptr)
		: id_(std::move(id)),
		  kind_(kind),
		  destroyed_(destroyed),
		  core_(c),
		  destroyed_after_shutdown_(after_shutdown)
	{
	}

	std::string_view id() const noexcept override { return id_; }
	type_kind kind() const noexcept override { return kind_; }

protected:
	~fake_type() override
	{
		if (destroyed_) {
			destroyed_->fetch_add(1);
		}
		if (core_ && core_->is_shut_down() && destroyed_after_shutdown_) {
			destroyed_after_shutdown_->fetch_add(1);
		}
	}
};

ref<type> make_type(std::string id, type_kind kind = type_kind::source)
{
	return knurbs::make<fake_type>(std::move(id), kind);
}

void test_add_find_list(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	r.add(make_type("com.example.b"));
	r.add(make_type("com.example.a"));
	r.add(make_type("com.example.f", type_kind::filter));

	auto sources = r.list(type_kind::source);
	assert_int_equal(sources.size(), 2);
	assert_true(sources[0]->id() == "com.example.a");
	assert_true(sources[1]->id() == "com.example.b");
	assert_int_equal(r.list(type_kind::filter).size(), 1);
	assert_int_equal(r.list(type_kind::output).size(), 0);

	auto found = r.find("com.example.a");
	assert_true(static_cast<bool>(found));
	assert_true(found->id() == "com.example.a");
	assert_false(static_cast<bool>(r.find("com.example.missing")));
}

void test_defaults_of_type(void **)
{
	ref<type> t = make_type("com.example.a");
	assert_int_equal(t->version(), 1);
	auto props = t->properties(*knurbs::data::create());
	assert_true(static_cast<bool>(props));
	assert_int_equal(props->list().size(), 0);
}

void test_invalid_id_throws(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	KNURBS_ASSERT_THROWS(r.add(make_type("Bad")), knurbs::error::invalid_argument);
	KNURBS_ASSERT_THROWS(r.add(make_type("a.b")), knurbs::error::invalid_argument);
	KNURBS_ASSERT_THROWS(r.add(ref<type>()), knurbs::error::invalid_argument);
}

void test_duplicate_throws(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	r.add(make_type("com.example.a"));
	KNURBS_ASSERT_THROWS(r.add(make_type("com.example.a", type_kind::filter)), knurbs::error::already_exists);
	assert_int_equal(r.list(type_kind::source).size(), 1);
}

void test_alias_resolves(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	r.add(make_type("com.xaymar.knurbs.capture.video-device"));
	r.add_alias("dshow_input", "com.xaymar.knurbs.capture.video-device");
	auto found = r.find("dshow_input");
	assert_true(static_cast<bool>(found));
	assert_true(found->id() == "com.xaymar.knurbs.capture.video-device");
}

void test_alias_registered_later(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	r.add_alias("old_a", "com.example.a");
	assert_false(static_cast<bool>(r.find("old_a")));
	r.add(make_type("com.example.a"));
	assert_true(static_cast<bool>(r.find("old_a")));
}

void test_alias_chain(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	r.add(make_type("com.example.c"));
	r.add_alias("old_x", "com.example.b");
	r.add_alias("com.example.b", "com.example.c");
	auto found = r.find("old_x");
	assert_true(static_cast<bool>(found));
	assert_true(found->id() == "com.example.c");
	// com.example.b is an alias now, so it cannot be registered
	KNURBS_ASSERT_THROWS(r.add(make_type("com.example.b")), knurbs::error::already_exists);
}

void test_alias_cycle_throws(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	r.add_alias("com.example.a", "com.example.b");
	KNURBS_ASSERT_THROWS(r.add_alias("com.example.b", "com.example.a"), knurbs::error::invalid_argument);
	// longer chain
	r.add_alias("com.example.b", "com.example.c");
	KNURBS_ASSERT_THROWS(r.add_alias("com.example.c", "com.example.a"), knurbs::error::invalid_argument);
	// self
	KNURBS_ASSERT_THROWS(r.add_alias("com.example.d", "com.example.d"), knurbs::error::invalid_argument);
	assert_false(static_cast<bool>(r.find("com.example.a")));
}

void test_alias_argument_checks(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	KNURBS_ASSERT_THROWS(r.add_alias("", "com.example.a"), knurbs::error::invalid_argument);
	KNURBS_ASSERT_THROWS(r.add_alias("old", "Bad"), knurbs::error::invalid_argument);
	r.add(make_type("com.example.a"));
	KNURBS_ASSERT_THROWS(r.add_alias("com.example.a", "com.example.b"), knurbs::error::already_exists);
	r.add_alias("old", "com.example.a");
	KNURBS_ASSERT_THROWS(r.add_alias("old", "com.example.a"), knurbs::error::already_exists);
}

void test_remove_module(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	r.add(make_type("com.example.a"), "m");
	r.add(make_type("com.example.b"), "m");
	r.add(make_type("com.example.c"), "other");
	r.add(make_type("com.example.d"));
	r.add_alias("old_a", "com.example.a", "m");
	r.add_alias("old_c", "com.example.c", "other");

	r.remove_module("m");
	assert_false(static_cast<bool>(r.find("com.example.a")));
	assert_false(static_cast<bool>(r.find("com.example.b")));
	assert_false(static_cast<bool>(r.find("old_a")));
	assert_true(static_cast<bool>(r.find("com.example.c")));
	assert_true(static_cast<bool>(r.find("old_c")));
	assert_true(static_cast<bool>(r.find("com.example.d")));
	// the IDs are free again
	r.add(make_type("com.example.a"));
}

void test_remove_module_releases_outside_lock(void **)
{
	// A type whose destructor calls back into the registry must not deadlock remove_module.
	struct reentrant final : type {
		const core *c;
		explicit reentrant(const core *core_ptr) : c(core_ptr) {}
		std::string_view id() const noexcept override { return "com.example.re"; }
		type_kind kind() const noexcept override { return type_kind::source; }
		~reentrant() override
		{
			reentrant_destroyed.fetch_add(1);
			try {
				(void)c->registry().find("com.example.re");
			} catch (const knurbs::error::core_gone &) {
				// Released by shutdown: the registry already refuses calls.
			}
		}
	};
	auto c = core::create(test_options());
	c->registry().add(knurbs::make<reentrant>(c.get()), "m");
	c->registry().remove_module("m");
	assert_int_equal(reentrant_destroyed.load(), 1);
	assert_false(static_cast<bool>(c->registry().find("com.example.re")));
	c->registry().add(knurbs::make<reentrant>(c.get()), "m");
	c->shutdown(); // clear() has the same rule
}

void test_virtuals_not_called_under_lock(void **)
{
	// id() and kind() call back into the registry. If the registry called them under its lock, find() would take
	// the shared lock again (or the writer lock would be held), which deadlocks or throws.
	struct calling_back final : type {
		const core *c;
		explicit calling_back(const core *core_ptr) : c(core_ptr) {}
		std::string_view id() const noexcept override
		{
			(void)c->registry().find("com.example.x");
			return "com.example.cb";
		}
		type_kind kind() const noexcept override
		{
			(void)c->registry().find("com.example.x");
			(void)c->registry().list(type_kind::filter);
			return type_kind::filter;
		}
	};
	auto c = core::create(test_options());
	auto &r = c->registry();
	r.add(knurbs::make<calling_back>(c.get()), "m");
	assert_int_equal(r.list(type_kind::filter).size(), 1);
	assert_int_equal(r.list(type_kind::source).size(), 0);
	assert_true(static_cast<bool>(r.find("com.example.cb")));
	r.remove_module("m");
}

void test_registry_after_shutdown(void **)
{
	auto c = core::create(test_options());
	c->shutdown();
	KNURBS_ASSERT_THROWS(c->registry().find("x"), knurbs::error::core_gone);
	KNURBS_ASSERT_THROWS(c->registry().list(type_kind::source), knurbs::error::core_gone);
	KNURBS_ASSERT_THROWS(c->registry().add(make_type("com.example.a")), knurbs::error::core_gone);
	KNURBS_ASSERT_THROWS(c->registry().add_alias("old", "com.example.a"), knurbs::error::core_gone);
	KNURBS_ASSERT_THROWS(c->registry().remove_module("m"), knurbs::error::core_gone);
}

void test_shutdown_releases_types(void **)
{
	std::atomic<int> destroyed{0};
	std::atomic<int> after_shutdown{0};
	auto c = core::create(test_options());
	c->registry().add(
		knurbs::make<fake_type>("com.example.a", type_kind::source, &destroyed, c.get(), &after_shutdown));
	assert_int_equal(destroyed.load(), 0);
	c->shutdown();
	// The registry held the only reference, so shutdown released the type; core is still alive, shut down.
	assert_int_equal(destroyed.load(), 1);
	assert_int_equal(after_shutdown.load(), 1);
}

void test_last_release_releases_types(void **)
{
	std::atomic<int> destroyed{0};
	{
		auto c = core::create(test_options());
		c->registry().add(knurbs::make<fake_type>("com.example.a", type_kind::source, &destroyed));
	}
	assert_int_equal(destroyed.load(), 1);
}

void test_concurrent_readers_and_writers(void **)
{
	auto c = core::create(test_options());
	auto &r = c->registry();
	constexpr int writers = 4;
	constexpr int readers = 4;
	constexpr int rounds = 300;
	std::atomic<bool> done{false};
	std::atomic<int> bad{0};
	r.add(make_type("com.example.stable"));
	r.add_alias("old_stable", "com.example.stable");

	std::vector<std::thread> reader_threads;
	for (int i = 0; i < readers; ++i) {
		reader_threads.emplace_back([&] {
			while (!done.load()) {
				auto list = r.list(type_kind::source);
				for (std::size_t k = 1; k < list.size(); ++k) {
					if (!(list[k - 1]->id() < list[k]->id())) {
						bad.fetch_add(1);
					}
				}
				if (!r.find("com.example.stable") || !r.find("old_stable")) {
					bad.fetch_add(1);
				}
				(void)r.find("com.example.w0.t3");
			}
		});
	}

	{
		std::vector<std::thread> writer_threads;
		for (int w = 0; w < writers; ++w) {
			writer_threads.emplace_back([&, w] {
				const std::string module = "m" + std::to_string(w);
				for (int n = 0; n < rounds; ++n) {
					for (int t = 0; t < 4; ++t) {
						r.add(make_type("com.example.w" + std::to_string(w) + ".t" +
								std::to_string(t)),
						      module);
					}
					r.add_alias("alias_" + std::to_string(w),
						    "com.example.w" + std::to_string(w) + ".t0", module);
					r.remove_module(module);
				}
			});
		}
		for (auto &t : writer_threads) {
			t.join();
		}
	}
	done = true;
	for (auto &t : reader_threads) {
		t.join();
	}
	assert_int_equal(bad.load(), 0);
	assert_int_equal(r.list(type_kind::source).size(), 1);
}

} // namespace

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_add_find_list),
		cmocka_unit_test(test_defaults_of_type),
		cmocka_unit_test(test_invalid_id_throws),
		cmocka_unit_test(test_duplicate_throws),
		cmocka_unit_test(test_alias_resolves),
		cmocka_unit_test(test_alias_registered_later),
		cmocka_unit_test(test_alias_chain),
		cmocka_unit_test(test_alias_cycle_throws),
		cmocka_unit_test(test_alias_argument_checks),
		cmocka_unit_test(test_remove_module),
		cmocka_unit_test(test_remove_module_releases_outside_lock),
		cmocka_unit_test(test_virtuals_not_called_under_lock),
		cmocka_unit_test(test_registry_after_shutdown),
		cmocka_unit_test(test_shutdown_releases_types),
		cmocka_unit_test(test_last_release_releases_types),
		cmocka_unit_test(test_concurrent_readers_and_writers),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
