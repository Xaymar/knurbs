#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

#include "knurbs/error.hpp"
#include "knurbs/log.hpp"
#include "knurbs/thread.hpp"

#include "test.hpp"

using knurbs::ref;
using knurbs::weak;
using knurbs::log::level;
using knurbs::log::logger;
using knurbs::thread::deferred_object;
using knurbs::thread::task_queue;

namespace {

	// Records whether its destructor ran on the queue thread.
	class probe final : public deferred_object {
		ref<task_queue>   queue_;
		std::atomic<int>& result_;

		public:
		probe(ref<task_queue> q, std::atomic<int>& result) : deferred_object(q), queue_(std::move(q)), result_(result) {}

		protected:
		~probe() override
		{
			result_.store(queue_->on_queue_thread() ? 1 : 2);
		}
	};

	// Waits up to five seconds for `done`.
	template<class F>
	bool wait_for(F done)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!done()) {
			if (std::chrono::steady_clock::now() > deadline) {
				return false;
			}
			std::this_thread::yield();
		}
		return true;
	}

} // namespace

static void test_fifo_order(void**)
{
	ref<task_queue>  q = task_queue::create("q", logger::create());
	std::vector<int> order;
	for (int i = 0; i < 1000; ++i) {
		q->post([&order, i] { order.push_back(i); });
	}
	q->flush();
	assert_int_equal(order.size(), 1000);
	for (int i = 0; i < 1000; ++i) {
		assert_int_equal(order[static_cast<std::size_t>(i)], i);
	}
}

static void test_task_exception_is_logged(void**)
{
	ref<logger>                                log = logger::create();
	std::mutex                                 mutex;
	std::vector<std::pair<level, std::string>> got;
	log->add_sink([&](level l, std::string_view text) noexcept {
		std::lock_guard lock(mutex);
		got.emplace_back(l, std::string(text));
	});

	ref<task_queue> q        = task_queue::create("q", log);
	bool            next_ran = false;
	q->post([] { throw std::runtime_error("boom"); });
	q->post([&] { next_ran = true; });
	q->flush();

	assert_true(next_ran);
	std::lock_guard lock(mutex);
	assert_int_equal(got.size(), 1);
	assert_true(got[0].first == level::error);
	assert_string_equal(got[0].second.c_str(), "q: task threw: boom");
}

static void test_post_after_stop_throws(void**)
{
	ref<task_queue> q   = task_queue::create("q", logger::create());
	bool            ran = false;
	q->post([&] { ran = true; });
	q->stop();
	assert_true(ran); // stop() runs what is queued
	KNURBS_ASSERT_THROWS(q->post([] {}), knurbs::error::core_gone);
	q->flush(); // nothing left: returns at once
	q->stop(); // a second stop is harmless
}

static void test_flush_on_queue_thread_runs_inline(void**)
{
	ref<task_queue>  q = task_queue::create("q", logger::create());
	std::vector<int> order;
	bool             inside = false;
	// Holds the queue until both tasks below are posted.
	std::atomic<bool> gate{false};
	q->post([&] {
		while (!gate.load()) {
			std::this_thread::yield();
		}
	});
	q->post([&] {
		inside = q->on_queue_thread();
		q->post([&] { order.push_back(2); });
		q->flush();
		order.push_back(3);
	});
	q->post([&] { order.push_back(1); });
	gate.store(true);
	q->flush();
	assert_true(inside);
	assert_false(q->on_queue_thread());
	assert_true(order == (std::vector<int>{1, 2, 3}));
}

static void test_flush_from_many_threads(void**)
{
	ref<task_queue>          q = task_queue::create("q", logger::create());
	std::atomic<int>         count{0};
	std::atomic<int>         missed{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&] {
			for (int i = 0; i < 100; ++i) {
				std::atomic<bool> ran{false};
				q->post([&] {
					ran.store(true);
					count.fetch_add(1);
				});
				q->flush();
				if (!ran.load()) {
					missed.fetch_add(1);
				}
			}
		});
	}
	for (auto& t : threads) {
		t.join();
	}
	assert_int_equal(missed.load(), 0);
	assert_int_equal(count.load(), 400);
}

static void test_deferred_destroy_runs_on_queue(void**)
{
	ref<task_queue>  q = task_queue::create("q", logger::create());
	std::atomic<int> result{0};
	{
		auto p = knurbs::make<probe>(q, result);
		p.reset(); // released here, destroyed on the queue
	}
	q->flush();
	assert_int_equal(result.load(), 1);

	// Once the queue is stopped, the delete runs inline.
	q->stop();
	auto p = knurbs::make<probe>(q, result);
	result.store(0);
	p.reset();
	assert_int_equal(result.load(), 2);
}

static void test_queue_released_from_own_task(void**)
{
	ref<logger>     log = logger::create();
	weak<logger>    log_alive(log);
	ref<task_queue> q = task_queue::create("q", log);
	log.reset();

	std::atomic<bool> ran{false};
	task_queue*       raw = q.get();
	raw->post([&ran, mine = std::move(q)]() mutable {
		mine.reset(); // the last reference to the queue, dropped on its own thread
		ran.store(true);
	});
	// The queue holds the logger until it is deleted.
	assert_true(wait_for([&] { return log_alive.expired(); }));
	assert_true(ran.load());
}

static void test_queue_released_by_deferred_object(void**)
{
	ref<logger>      log = logger::create();
	weak<logger>     log_alive(log);
	std::atomic<int> result{0};
	{
		ref<task_queue> q = task_queue::create("q", log);
		auto            p = knurbs::make<probe>(q, result);
		q.reset();
		p.reset(); // its destructor drops the last queue reference, on the queue thread
	}
	log.reset();
	assert_true(wait_for([&] { return log_alive.expired(); }));
	assert_int_equal(result.load(), 1);
}

static void test_set_current_name(void**)
{
	std::string long_name, short_name;
	std::thread t([&] {
		knurbs::thread::set_current_name("a-rather-long-thread-name");
		[[maybe_unused]] char buffer[64] = {};
#if defined(__linux__)
		pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
		long_name = buffer;
#endif
		knurbs::thread::set_current_name("");
		knurbs::thread::set_current_name("short");
#if defined(__linux__)
		pthread_getname_np(pthread_self(), buffer, sizeof(buffer));
		short_name = buffer;
#endif
	});
	t.join();
#if defined(__linux__)
	assert_string_equal(long_name.c_str(), "a-rather-long-t"); // 15 bytes
	assert_string_equal(short_name.c_str(), "short");
#endif
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_fifo_order), cmocka_unit_test(test_task_exception_is_logged), cmocka_unit_test(test_post_after_stop_throws), cmocka_unit_test(test_flush_on_queue_thread_runs_inline), cmocka_unit_test(test_flush_from_many_threads), cmocka_unit_test(test_deferred_destroy_runs_on_queue), cmocka_unit_test(test_queue_released_from_own_task), cmocka_unit_test(test_queue_released_by_deferred_object), cmocka_unit_test(test_set_current_name),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
