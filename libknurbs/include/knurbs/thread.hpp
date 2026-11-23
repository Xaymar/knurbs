#pragma once

#include <string_view>

#include "knurbs/function.hpp"
#include "knurbs/export.hpp"
#include "knurbs/log.hpp"
#include "knurbs/object.hpp"
#include "knurbs/ref.hpp"

namespace knurbs::thread {

// Names the calling thread for debuggers and profilers. On Linux the name is cut to 15 bytes (at a UTF-8
// character boundary), on macOS to 63. Failures are ignored.
KNURBS_API void set_current_name(std::string_view name) noexcept;

namespace detail {
class task_queue_impl;
}

// Runs tasks one after another, in the order they were posted, on a thread of its own named after the queue.
// Thread-safe. Releasing the last reference stops the queue (see stop()), also from a task on the queue.
class KNURBS_API task_queue : public object {
	// Only created by create(); not meant to be subclassed.
	task_queue() noexcept = default;
	friend class detail::task_queue_impl;

public:
	// A task that throws is logged to `logger` at level::error as "<name>: task threw: <what>", and the queue
	// keeps running. `logger` may be empty.
	static ref<task_queue> create(std::string_view name, ref<log::logger> logger);

	// Queues `task` to run after everything posted before it. Throws error::core_gone once the queue is
	// stopping, and error::invalid_argument for an empty `task`.
	void post(knurbs::move_only_function<void()> task);

	// Blocks until everything posted before the call has run. On the queue's own thread it runs those tasks
	// inline instead.
	void flush();

	// Refuses further posts, runs what is already queued, then joins the thread. On the queue's own thread
	// it does not join; the thread exits once the queue is drained.
	void stop() noexcept;

	bool on_queue_thread() const noexcept;
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251) // ref<task_queue> member of an exported class
#endif

// Base for objects that must be destroyed on a particular task queue: releasing the last reference posts the
// delete to that queue. Once the queue is stopped, the delete runs inline, on the releasing thread.
class KNURBS_API deferred_object : public object {
	ref<task_queue> queue_;

protected:
	explicit deferred_object(ref<task_queue> queue) noexcept;
	~deferred_object() override;

	void destroy() noexcept override;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace knurbs::thread
