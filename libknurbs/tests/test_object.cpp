#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

#include "knurbs/object.hpp"
#include "knurbs/ref.hpp"

#include "test.hpp"

using knurbs::make;
using knurbs::ref;
using knurbs::weak;

namespace {

struct probe : knurbs::object {
	enum class during_destroy { nothing, try_acquire, weak_get };

	std::atomic<bool> *destroyed;
	during_destroy action;
	bool *result;

	explicit probe(std::atomic<bool> *destroyed_flag = nullptr, during_destroy what = during_destroy::nothing,
		       bool *what_result = nullptr) noexcept
		: destroyed(destroyed_flag),
		  action(what),
		  result(what_result)
	{
	}

	~probe() override
	{
		if (destroyed) {
			destroyed->store(true);
		}
	}

protected:
	void destroy() noexcept override
	{
		// The count is already 0 here and the object is not yet deleted.
		if (action == during_destroy::try_acquire) {
			*result = try_acquire();
		} else if (action == during_destroy::weak_get) {
			knurbs::detail::weak_block *w = knurbs::detail::weak_get(this);
			*result = knurbs::detail::weak_expired(w) && knurbs::detail::weak_lock(w) == nullptr;
			knurbs::detail::weak_release(w);
		}
		object::destroy();
	}
};

} // namespace

static void test_count_starts_at_one(void **)
{
	auto r = make<probe>();
	assert_int_equal(r->use_count(), 1);
}

static void test_last_release_destroys(void **)
{
	std::atomic<bool> destroyed{false};
	auto r = make<probe>(&destroyed);
	auto copy = r;
	assert_int_equal(r->use_count(), 2);
	copy.reset();
	assert_false(destroyed.load());
	assert_int_equal(r->use_count(), 1);
	r.reset();
	assert_true(destroyed.load());
	assert_null(r.get());
}

static void test_try_acquire_fails_at_zero(void **)
{
	std::atomic<bool> destroyed{false};
	bool acquired = true;
	auto r = make<probe>(&destroyed, probe::during_destroy::try_acquire, &acquired);
	assert_true(r->try_acquire());
	assert_int_equal(r->use_count(), 2);
	r->release();
	r.reset();
	assert_true(destroyed.load());
	assert_false(acquired);
}

static void test_weak_lock_alive_and_expired(void **)
{
	std::atomic<bool> destroyed{false};
	auto r = make<probe>(&destroyed);
	weak<probe> w(r);
	assert_false(w.expired());
	{
		ref<probe> locked = w.lock();
		assert_ptr_equal(locked.get(), r.get());
		assert_int_equal(r->use_count(), 2);
	}
	assert_int_equal(r->use_count(), 1);

	weak<probe> copy = w;
	weak<probe> moved = std::move(copy);
	assert_true(copy.expired()); // moved-from weak is empty
	assert_false(moved.expired());

	r.reset();
	assert_true(destroyed.load());
	assert_true(w.expired());
	assert_true(moved.expired());
	assert_false(static_cast<bool>(w.lock()));
	assert_false(static_cast<bool>(moved.lock()));
}

static void test_weak_requested_during_destroy(void **)
{
	std::atomic<bool> destroyed{false};
	bool expired = false;
	auto r = make<probe>(&destroyed, probe::during_destroy::weak_get, &expired);
	r.reset();
	assert_true(destroyed.load());
	assert_true(expired);
}

static void test_weak_requested_during_destroy_existing_block(void **)
{
	// Same as above, but a weak block already exists before the last release.
	std::atomic<bool> destroyed{false};
	bool expired = false;
	auto r = make<probe>(&destroyed, probe::during_destroy::weak_get, &expired);
	weak<probe> w(r);
	r.reset();
	assert_true(destroyed.load());
	assert_true(expired);
	assert_true(w.expired());
}

static void test_detach_adopt_roundtrip(void **)
{
	std::atomic<bool> destroyed{false};
	auto r = make<probe>(&destroyed);
	probe *p = r.detach();
	assert_null(r.get());
	auto r2 = ref<probe>::adopt(p);
	assert_ptr_equal(r2.get(), p);
	assert_int_equal(r2->use_count(), 1);
	r2.reset();
	assert_true(destroyed.load());
}

static void test_retain_and_conversion(void **)
{
	std::atomic<bool> destroyed{false};
	auto r = make<probe>(&destroyed);
	auto retained = ref<probe>::retain(r.get());
	assert_int_equal(r->use_count(), 2);

	ref<knurbs::object> base = retained;
	assert_int_equal(r->use_count(), 3);
	ref<knurbs::object> moved_base = std::move(retained);
	assert_false(static_cast<bool>(retained));
	assert_int_equal(r->use_count(), 3);

	base.reset();
	moved_base.reset();
	assert_int_equal(r->use_count(), 1);
	assert_false(destroyed.load());
	r.reset();
	assert_true(destroyed.load());
}

static void test_upgrade_vs_release_stress(void **)
{
	constexpr int threads = 8;
	constexpr int iterations = 10000;
	constexpr int max_locks = 64;

	std::atomic<bool> destroyed{false};
	weak<probe> shared;
	std::atomic<long> violations{0};
	std::atomic<long> upgrades{0};
	std::atomic<bool> stop{false};
	std::barrier sync(threads + 1);

	std::vector<std::jthread> workers;
	for (int t = 0; t < threads; ++t) {
		workers.emplace_back([&] {
			for (;;) {
				sync.arrive_and_wait(); // iteration starts
				if (stop.load()) {
					return;
				}
				// Bounded, or overlapping locks from the workers could keep the count above 0 forever.
				for (int attempt = 0; attempt < max_locks; ++attempt) {
					bool dead_before = destroyed.load();
					ref<probe> s = shared.lock();
					if (!s) {
						break;
					}
					upgrades.fetch_add(1, std::memory_order_relaxed);
					if (dead_before || destroyed.load()) {
						violations.fetch_add(1);
					}
				}
				sync.arrive_and_wait(); // iteration ends
			}
		});
	}

	for (int i = 0; i < iterations; ++i) {
		destroyed.store(false);
		auto r = make<probe>(&destroyed);
		shared = weak<probe>(r);
		sync.arrive_and_wait();
		if (i % 2) {
			std::this_thread::yield();
		}
		r.reset(); // the last strong ref may now die here or in a worker
		sync.arrive_and_wait();
		if (!destroyed.load() || !shared.expired()) {
			violations.fetch_add(1);
		}
		shared = weak<probe>();
	}
	stop.store(true);
	sync.arrive_and_wait();
	workers.clear();

	assert_int_equal(violations.load(), 0);
	print_message("%ld successful upgrades during the stress run\n", upgrades.load());
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_count_starts_at_one),
		cmocka_unit_test(test_last_release_destroys),
		cmocka_unit_test(test_try_acquire_fails_at_zero),
		cmocka_unit_test(test_weak_lock_alive_and_expired),
		cmocka_unit_test(test_weak_requested_during_destroy),
		cmocka_unit_test(test_weak_requested_during_destroy_existing_block),
		cmocka_unit_test(test_detach_adopt_roundtrip),
		cmocka_unit_test(test_retain_and_conversion),
		cmocka_unit_test(test_upgrade_vs_release_stress),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
