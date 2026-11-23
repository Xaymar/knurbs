#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "knurbs/core.hpp"
#include "knurbs/error.hpp"
#include "knurbs/log.hpp"
#include "knurbs/thread.hpp"

#include "test.hpp"

using knurbs::core;
using knurbs::ref;
using knurbs::log::level;
using knurbs::thread::deferred_object;
using knurbs::thread::task_queue;

namespace {

	core::options test_options()
	{
		return core::options{"config", "cache"};
	}

	// Deleted on the core's destruction queue. Its destructor reads the core through a raw pointer, which ASan reports
	// if the core is already gone, and counts itself.
	class probe final : public deferred_object {
		const core*       core_;
		std::atomic<int>& deleted_;
		std::atomic<int>& saw_shut_down_;
		bool              slow_;

		public:
		probe(const core* c, std::atomic<int>& deleted, std::atomic<int>& saw_shut_down, bool slow = false) : deferred_object(c->destruction_queue()), core_(c), deleted_(deleted), saw_shut_down_(saw_shut_down), slow_(slow) {}

		protected:
		~probe() override
		{
			if (slow_) {
				std::this_thread::sleep_for(std::chrono::microseconds(50));
			}
			if (core_->is_shut_down()) {
				saw_shut_down_.fetch_add(1);
			}
			deleted_.fetch_add(1);
		}
	};

} // namespace

static void test_create_and_shutdown(void**)
{
	ref<core> c = core::create(test_options());
	assert_non_null(c.get());
	assert_non_null(c->logger().get());
	assert_non_null(c->destruction_queue().get());
	assert_false(c->is_shut_down());
	c->check_alive();

	c->shutdown();
	assert_true(c->is_shut_down());
	KNURBS_ASSERT_THROWS(c->check_alive(), knurbs::error::core_gone);
	KNURBS_ASSERT_THROWS(c->destruction_queue()->post([] {}), knurbs::error::core_gone);
}

static void test_create_requires_paths(void**)
{
	KNURBS_ASSERT_THROWS(core::create(core::options{"", "cache"}), knurbs::error::invalid_argument);
	KNURBS_ASSERT_THROWS(core::create(core::options{"config", ""}), knurbs::error::invalid_argument);
	KNURBS_ASSERT_THROWS(core::create(core::options{}), knurbs::error::invalid_argument);
}

static void test_startup_message_logged(void**)
{
	std::vector<std::pair<level, std::string>> got; // written only during create(), on this thread
	core::options                              opts = test_options();
	opts.sink                                       = [&got](level l, std::string_view text) noexcept { got.emplace_back(l, std::string(text)); };
	ref<core> c                                     = core::create(std::move(opts));
	assert_int_equal(got.size(), 1);
	assert_true(got[0].first == level::info);
	assert_string_equal(got[0].second.c_str(), "knurbs 0.1.0 core started");
}

static void test_two_cores_independent(void**)
{
	ref<core> a = core::create(test_options());
	ref<core> b = core::create(test_options());
	assert_ptr_not_equal(a->logger().get(), b->logger().get());
	assert_ptr_not_equal(a->destruction_queue().get(), b->destruction_queue().get());

	a->shutdown();
	assert_true(a->is_shut_down());
	assert_false(b->is_shut_down());
	b->check_alive();

	std::atomic<bool> ran{false};
	b->destruction_queue()->post([&ran] { ran.store(true); });
	b->destruction_queue()->flush();
	assert_true(ran.load());
}

static void test_shutdown_idempotent(void**)
{
	ref<core> c = core::create(test_options());
	c->shutdown();
	c->shutdown();
	assert_true(c->is_shut_down());
	c.reset(); // the last release runs shutdown a third time
}

static void test_last_release_shuts_down(void**)
{
	ref<core>        c = core::create(test_options());
	ref<task_queue>  q = c->destruction_queue();
	std::atomic<int> deleted{0};
	std::atomic<int> saw_shut_down{0};
	for (int i = 0; i < 10; ++i) {
		knurbs::make<probe>(c.get(), deleted, saw_shut_down, true).reset();
	}
	c.reset();
	// Every delete queued before the core went away ran while the core was still there.
	assert_int_equal(deleted.load(), 10);
	KNURBS_ASSERT_THROWS(q->post([] {}), knurbs::error::core_gone);
}

// The last reference to the core goes away inside a task on the core's own destruction queue. The deletes queued
// behind that task must still run before the core is deleted.
static void test_last_release_on_destruction_queue(void**)
{
	ref<core>       c   = core::create(test_options());
	const core*     raw = c.get();
	ref<task_queue> q   = c->destruction_queue();

	std::promise<void>       queued;
	std::shared_future<void> queued_future = queued.get_future().share();
	std::atomic<int>         deleted{0};
	std::atomic<int>         saw_shut_down{0};
	std::atomic<int>         deleted_when_released{-1};

	q->post([c = std::move(c), queued_future, &deleted, &deleted_when_released]() mutable {
		queued_future.wait();
		c.reset(); // the last reference: the core shuts down on this thread
		deleted_when_released.store(deleted.load());
	});
	for (int i = 0; i < 10; ++i) {
		knurbs::make<probe>(raw, deleted, saw_shut_down).reset();
	}
	queued.set_value();
	q->flush();

	assert_int_equal(deleted_when_released.load(), 10);
	assert_int_equal(deleted.load(), 10);
	assert_int_equal(saw_shut_down.load(), 10);
	KNURBS_ASSERT_THROWS(q->post([] {}), knurbs::error::core_gone);
}

// While shutdown runs, other threads keep releasing deferred objects and call shutdown() themselves. Every shutdown()
// returns only after the deletes queued before it began have finished, and nothing is lost.
static void test_shutdown_waits_for_deferred_deletes(void**)
{
	constexpr int queued_count   = 200;
	constexpr int releaser_count = 4;
	constexpr int per_releaser   = 50;
	constexpr int shutter_count  = 3;

	ref<core>        c = core::create(test_options());
	std::atomic<int> deleted{0};
	std::atomic<int> saw_shut_down{0};

	for (int i = 0; i < queued_count; ++i) {
		knurbs::make<probe>(c.get(), deleted, saw_shut_down, true).reset();
	}

	std::vector<std::vector<ref<probe>>> held(releaser_count);
	for (auto& list : held) {
		for (int i = 0; i < per_releaser; ++i) {
			list.push_back(knurbs::make<probe>(c.get(), deleted, saw_shut_down, true));
		}
	}

	std::barrier     start(releaser_count + shutter_count);
	std::atomic<int> early_returns{0}; // shutdown() returned before the queued deletes were done
	{
		std::vector<std::jthread> threads;
		for (auto& list : held) {
			threads.emplace_back([&start, &list] {
				start.arrive_and_wait();
				for (auto& p : list) {
					p.reset();
				}
			});
		}
		for (int i = 0; i < shutter_count; ++i) {
			threads.emplace_back([&start, &c, &deleted, &early_returns] {
				start.arrive_and_wait();
				c->shutdown();
				if (deleted.load() < queued_count) {
					early_returns.fetch_add(1);
				}
			});
		}
	}

	assert_int_equal(early_returns.load(), 0);
	assert_int_equal(deleted.load(), queued_count + releaser_count * per_releaser);
	assert_true(c->is_shut_down());
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_create_and_shutdown), cmocka_unit_test(test_create_requires_paths), cmocka_unit_test(test_startup_message_logged), cmocka_unit_test(test_two_cores_independent), cmocka_unit_test(test_shutdown_idempotent), cmocka_unit_test(test_last_release_shuts_down), cmocka_unit_test(test_last_release_on_destruction_queue), cmocka_unit_test(test_shutdown_waits_for_deferred_deletes),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
