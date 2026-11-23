#pragma once

#include <filesystem>

#include "knurbs/export.hpp"
#include "knurbs/log.hpp"
#include "knurbs/object.hpp"
#include "knurbs/ref.hpp"
#include "knurbs/thread.hpp"

namespace knurbs {

class module_manager;
class registry;

namespace detail {
class core_impl;
}

// The root of a libknurbs instance: it owns the logger, the threads and everything created through it. There is
// no process-wide state, so several cores can exist side by side without interfering. Thread-safe.
//
// Shutdown, run by shutdown() or by the core's last release, does three things in this order:
// 1. It unloads the modules (module_manager::unload_all): knurbs_module_unload runs for every module that did not
//    fail, in reverse name order. The registry already refuses calls by then.
// 2. It clears the registry: the registry drops its own references to every type and alias. A type is destroyed
//    then only if nobody else holds it; references handed out by registry::find and list keep it alive until they
//    are released, as they do after registry::remove_module. Module libraries are never unloaded, so such a type's
//    code stays available.
// 3. It stops the destruction queue, last.
// After shutdown begins, check_alive() throws error::core_gone and the destruction queue refuses new tasks. What
// it guarantees about deferred deletes (thread::deferred_object on the destruction queue):
// - Every delete posted to the destruction queue before shutdown began has finished when shutdown returns. This
//   also holds when shutdown runs on the destruction queue's own thread (for example because a deferred delete
//   drops the last core reference): the queued deletes then run inline, before the core goes away.
// - An object released after shutdown began is deleted inline, on the releasing thread, inside that release. Such
//   deletes can overlap the queued deletes that shutdown is still draining; there is no ordering between the two.
// - Called while another thread's shutdown is in progress, shutdown() waits for it to finish. The exception is a
//   call from a task on the destruction queue itself, which returns at once, because the shutdown in progress is
//   waiting for that very task.
class KNURBS_API core : public object {
	// Only created by create(); not meant to be subclassed.
	core() noexcept = default;
	friend class detail::core_impl;

public:
	struct options {
		std::filesystem::path config_path;
		std::filesystem::path cache_path;
		// Attached to the core's logger before create() writes its first message. Empty means none.
		log::sink sink = {};
	};

	// Starts a core: adds opts.sink (if any) to the new logger, then logs
	// "knurbs <major>.<minor>.<patch> core started" at level::info. Throws error::invalid_argument when
	// config_path or cache_path is empty.
	static ref<core> create(options opts);

	// The core's logger. It stays usable after shutdown, for as long as a reference to it is held.
	ref<log::logger> logger() const;
	// The queue that deferred objects of this core are deleted on. Still returned after shutdown, but stopped:
	// post() then throws error::core_gone.
	ref<thread::task_queue> destruction_queue() const;

	// The type registry. Its members throw error::core_gone once shutdown has begun; the reference itself stays
	// valid for as long as the core is held.
	knurbs::registry &registry() const;

	// Finds and loads the core's modules. load() throws error::core_gone once shutdown has begun.
	knurbs::module_manager &modules() const;

	// Stops the core; see above. Idempotent. The core's last release calls it. The caller must hold a reference
	// to the core (or run inside its last release), as for every other member.
	void shutdown() noexcept;
	// True once shutdown has begun.
	bool is_shut_down() const noexcept;
	// Throws error::core_gone once shutdown has begun.
	void check_alive() const;
};

} // namespace knurbs
