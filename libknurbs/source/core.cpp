#include "core-impl.hpp"

#include <utility>

#include "knurbs/error.hpp"
#include "knurbs/version.hpp"

namespace knurbs {

namespace detail {

core_impl::core_impl(ref<log::logger> logger, ref<thread::task_queue> destruction_queue) noexcept
	: logger_(std::move(logger)),
	  destruction_queue_(std::move(destruction_queue))
{
}

core_impl::~core_impl() = default;

void core_impl::shutdown() noexcept
{
	std::unique_lock lock(mutex_);
	if (state_ == state::running) {
		state_ = state::stopping;
		shut_down_.store(true, std::memory_order_release);
		lock.unlock();
		stop_threads();
		lock.lock();
		state_ = state::stopped;
		stopped_cv_.notify_all();
		return;
	}
	if (destruction_queue_->on_queue_thread()) {
		// The shutdown in progress may be waiting for the task that called us; waiting here would deadlock.
		return;
	}
	stopped_cv_.wait(lock, [this] { return state_ == state::stopped; });
}

void core_impl::stop_threads() noexcept
{
	// Threads added later stop here, before the destruction queue, so their last deletes still reach it.

	// The destruction queue goes last. stop() refuses new posts; from then on deferred objects delete inline.
	destruction_queue_->stop();
	if (destruction_queue_->on_queue_thread()) {
		// stop() does not drain the queue when called on its own thread. Run what is still queued now, while the
		// core is still there, instead of after this task returns.
		destruction_queue_->flush();
	}
}

void core_impl::destroy() noexcept
{
	shutdown();
	delete this;
}

} // namespace detail

ref<core> core::create(options opts)
{
	if (opts.config_path.empty()) {
		throw error::invalid_argument("core: config_path is empty");
	}
	if (opts.cache_path.empty()) {
		throw error::invalid_argument("core: cache_path is empty");
	}
	ref<log::logger> logger = log::logger::create();
	if (opts.sink) {
		logger->add_sink(std::move(opts.sink));
	}
	ref<thread::task_queue> destruction_queue = thread::task_queue::create("knurbs-destroy", logger);
	ref<core> result = make<detail::core_impl>(logger, std::move(destruction_queue));
	logger->info("knurbs {}.{}.{} core started", version::major, version::minor, version::patch);
	return result;
}

// core is only ever constructed as a core_impl (its constructor is private), so these casts are always to the
// dynamic type.
ref<log::logger> core::logger() const
{
	return static_cast<const detail::core_impl *>(this)->logger();
}

ref<thread::task_queue> core::destruction_queue() const
{
	return static_cast<const detail::core_impl *>(this)->destruction_queue();
}

void core::shutdown() noexcept
{
	static_cast<detail::core_impl *>(this)->shutdown();
}

bool core::is_shut_down() const noexcept
{
	return static_cast<const detail::core_impl *>(this)->is_shut_down();
}

void core::check_alive() const
{
	if (is_shut_down()) {
		throw error::core_gone("core: the core is shut down");
	}
}

} // namespace knurbs
