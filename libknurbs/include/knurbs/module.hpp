#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "knurbs/c/module.h"
#include "knurbs/export.hpp"
#include "knurbs/log.hpp"
#include "knurbs/object.hpp"
#include "knurbs/ref.hpp"
#include "knurbs/version.hpp"

namespace knurbs {

class core;
class registry;

namespace detail {
class module_impl;
class module_manager_impl;
} // namespace detail

// A loaded (or failed) module: a shared library with the entry points below, found by module_manager::discover.
//
// A C++ module uses KNURBS_MODULE_CPP() once and defines its entry points as
//     extern "C" KNURBS_MODULE_EXPORT void knurbs_module_load(knurbs::module *m);       // required
//     extern "C" KNURBS_MODULE_EXPORT void knurbs_module_post_load(knurbs::module *m);  // optional
//     extern "C" KNURBS_MODULE_EXPORT void knurbs_module_unload(knurbs::module *m);     // optional
// load runs for every module in parallel; post_load runs, again in parallel, once every load has returned, so it
// can find the types of other modules; unload runs at the core's shutdown, one module at a time in reverse name
// order, and only for modules that did not fail. Each call runs on a thread started for that module. An exception
// escaping load or post_load fails the module: its types and aliases are removed and "module <name>: <what>" is
// logged at level::error. An exception from unload is logged the same way, and also fails the
// module. Entry points must not call
// module_manager::load, module_manager::unload_all or core::shutdown.
//
// While unload runs the core is already shutting down, so the registry refuses every call (error::core_gone);
// shutdown drops the registry's references to all types right after.
//
// A module's library is never unloaded, not even after the core is gone: references to its types can outlive the
// core, and its code may still be on some thread's stack. Thread-safe.
class KNURBS_API module : public object {
	// Only created by module_manager::load; not meant to be subclassed.
	module() noexcept = default;
	friend class detail::module_impl;

public:
	// The name of the module's directory, as found by discover.
	std::string_view name() const noexcept;
	// The module's library: <dir>/<name>/bin/<prefix><name><suffix>.
	const std::filesystem::path &binary_path() const noexcept;
	// The module's data directory, <dir>/<name>/data. It need not exist.
	const std::filesystem::path &data_path() const noexcept;

	// The core that loaded the module. The module does not keep it alive: only call these while the core is
	// held, as it is during every entry point.
	class core &core() const;
	// The core's registry. A type or alias that the module adds with an empty `owner_module` is recorded as this
	// module's (removed with registry::remove_module(name())) only while one of the module's entry points runs,
	// on the thread the loader started for it. Registrations made later, or from other threads, must pass
	// name() as `owner_module` explicitly.
	class registry &registry() const;
	// The core's logger.
	ref<log::logger> logger() const;

	// True if the module was rejected or one of its entry points failed.
	bool failed() const noexcept;
};

// Finds and loads a core's modules (core::modules()). It cannot be created or copied. Thread-safe: load and
// unload_all run one at a time.
class KNURBS_API module_manager {
	// Only created by core.
	module_manager() noexcept = default;
	friend class detail::module_manager_impl;

public:
	struct found {
		std::string name;
		std::filesystem::path binary;
		std::filesystem::path data;
	};

	module_manager(const module_manager &) = delete;
	module_manager &operator=(const module_manager &) = delete;

	// Lists every <dir>/<name>/bin/<prefix><name><suffix> (the platform's prefix and suffix for loadable
	// modules), with <dir>/<name>/data as its data directory, without loading anything. Paths are absolute, and
	// the result is sorted by name. A name found in several directories is taken from the first of them; the
	// others are logged at level::warning and skipped. Directories that do not exist are skipped.
	std::vector<found> discover(const std::vector<std::filesystem::path> &search_dirs) const;

	// Loads the libraries and calls knurbs_module_load of every module in parallel, waits for all of them, then
	// calls knurbs_module_post_load of every module that has not failed, in parallel. Returns every module, failed
	// ones included, in the order of `modules`. A module is failed when its library cannot be loaded, its header
	// is missing or invalid, it is a C++ module of another API major version, it has no knurbs_module_load, its
	// name is already loaded into this core, or load or post_load threw; see module. Throws error::core_gone once
	// shutdown has begun.
	std::vector<ref<module>> load(const std::vector<found> &modules);

	// Calls knurbs_module_unload of every loaded module that has not failed, in reverse name order. Each module
	// is unloaded at most once. Does not remove the modules' types. core::shutdown() calls this before it clears
	// the registry.
	void unload_all() noexcept;
};

} // namespace knurbs

// Defines the knurbs_module_header of a C++ module, for the version of the headers it is compiled against. Use it
// once, at namespace scope, in one source file of the module.
#define KNURBS_MODULE_CPP()                                                                               \
	extern "C" KNURBS_MODULE_EXPORT const struct knurbs_module_header *knurbs_module_header(void)     \
	{                                                                                                 \
		static constexpr struct knurbs_module_header knurbs_header = {                            \
			sizeof(struct knurbs_module_header),                                              \
			KNURBS_MODULE_API_CPP,                                                            \
			static_cast<std::uint32_t>(::knurbs::version::major),                            \
			static_cast<std::uint32_t>(::knurbs::version::minor),                             \
		};                                                                                        \
		return &knurbs_header;                                                                    \
	}
