#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "knurbs/log.hpp"

#include "test.hpp"

using knurbs::ref;
using knurbs::log::level;
using knurbs::log::logger;

namespace {

	// Like every other core class, logger cannot be subclassed: its methods cast to the library's own implementation.
	struct plugin_logger : logger {};
	static_assert(!std::is_default_constructible_v<plugin_logger>);

	using record = std::pair<level, std::string>;

	knurbs::log::sink recorder(std::vector<record>& out)
	{
		return [&out](level l, std::string_view text) noexcept { out.emplace_back(l, std::string(text)); };
	}

} // namespace

static void test_sinks_receive_in_order(void**)
{
	ref<logger>              log = logger::create();
	std::vector<std::string> order;
	std::vector<record>      first, second;
	std::uint64_t            id1 = log->add_sink([&](level l, std::string_view t) noexcept {
		order.push_back("one");
		first.emplace_back(l, std::string(t));
	});
	std::uint64_t            id2 = log->add_sink([&](level l, std::string_view t) noexcept {
		order.push_back("two");
		second.emplace_back(l, std::string(t));
	});
	assert_int_equal(id1, 1);
	assert_true(id2 != 0 && id2 != id1);

	log->info("a {}", 1);

	assert_int_equal(first.size(), 1);
	assert_int_equal(second.size(), 1);
	assert_true(first[0] == record(level::info, "a 1"));
	assert_true(second[0] == record(level::info, "a 1"));
	assert_true(order == (std::vector<std::string>{"one", "two"}));

	log->debug("d");
	log->warning("w");
	log->error("e");
	assert_true(first[1].first == level::debug);
	assert_true(first[2].first == level::warning);
	assert_true(first[3].first == level::error);
}

static void test_remove_sink(void**)
{
	ref<logger>         log = logger::create();
	std::vector<record> got;
	std::uint64_t       id = log->add_sink(recorder(got));
	log->info("x");
	assert_int_equal(got.size(), 1);
	log->remove_sink(id);
	log->remove_sink(id); // unknown ids are ignored
	log->info("y");
	assert_int_equal(got.size(), 1);
}

static void test_log_sink_removes_itself(void**)
{
	ref<logger>         log   = logger::create();
	int                 calls = 0;
	std::uint64_t       id    = 0;
	std::vector<record> other;
	id = log->add_sink([&](level, std::string_view) noexcept {
		++calls;
		log->remove_sink(id);
	});
	log->add_sink(recorder(other));
	log->info("first");
	log->info("second");
	assert_int_equal(calls, 1);
	assert_int_equal(other.size(), 2);
}

static void test_removed_sink_skipped_in_running_write(void**)
{
	// The first sink removes the second while a write is in progress: the second must not be called.
	ref<logger>   log          = logger::create();
	std::uint64_t second       = 0;
	int           second_calls = 0;
	log->add_sink([&](level, std::string_view) noexcept { log->remove_sink(second); });
	second = log->add_sink([&](level, std::string_view) noexcept { ++second_calls; });
	log->info("x");
	assert_int_equal(second_calls, 0);
}

static void test_two_loggers_independent(void**)
{
	ref<logger>         a = logger::create();
	ref<logger>         b = logger::create();
	std::vector<record> got_a, got_b;
	a->add_sink(recorder(got_a));
	b->add_sink(recorder(got_b));
	b->info("only b");
	assert_int_equal(got_a.size(), 0);
	assert_int_equal(got_b.size(), 1);
	a->info("only a");
	assert_int_equal(got_a.size(), 1);
	assert_int_equal(got_b.size(), 1);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_sinks_receive_in_order), cmocka_unit_test(test_remove_sink), cmocka_unit_test(test_log_sink_removes_itself), cmocka_unit_test(test_removed_sink_skipped_in_running_write), cmocka_unit_test(test_two_loggers_independent),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
