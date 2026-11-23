#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "knurbs/log.hpp"
#include "knurbs/module.hpp"
#include "knurbs/ref.hpp"

namespace knurbs::detail {

class core_impl;

using module_entry = void (*)(module *);

class module_impl final : public module {
public:
	module_impl(core_impl &c, module_manager::found f);

	core_impl &core_;
	const ref<log::logger> logger_;
	const std::string name_;
	const std::filesystem::path binary_;
	const std::filesystem::path data_;
	std::atomic<bool> failed_{false};

	// Written by the module's load thread; read only after that thread was joined.
	// The library handle is never closed (see module): types handed out by the registry can outlive the core,
	// and the module's code may still be on a stack.
	void *library_ = nullptr;
	module_entry post_load_ = nullptr;
	module_entry unload_ = nullptr;

	// Guarded by module_manager_impl::mutex_.
	bool unloaded_ = false;
};

// Records which module's entry point runs on the calling thread, for as long as it exists, so the registry can
// attribute registrations made without an owner. Scopes nest.
class current_module_scope {
	const module_impl *previous_;

public:
	explicit current_module_scope(const module_impl &m) noexcept;
	~current_module_scope();
	current_module_scope(const current_module_scope &) = delete;
	current_module_scope &operator=(const current_module_scope &) = delete;
};

// The name of the module of core `c` whose entry point runs on this thread, or "" if there is none.
std::string_view current_module_name(const core &c) noexcept;

class module_manager_impl final : public module_manager {
	core_impl &core_;
	// Held for the whole of load and unload_all, so they run one at a time and shutdown waits for a load in
	// progress.
	std::mutex mutex_;
	std::vector<ref<module_impl>> modules_;

	// Marks `m` failed and logs "module <name>: <reason>". With `remove_types`, also drops what it registered.
	void fail(module_impl &m, std::string_view reason, bool remove_types) noexcept;
	// Runs `entry` on a thread of its own per module in `mods`, with that module as the current module, and
	// waits for all of them. An exception fails the module, with `remove_types` as for fail().
	template<class F> void run_each(const std::vector<module_impl *> &mods, F entry, bool remove_types);
	// Loads the library of `m`, checks its header and calls its knurbs_module_load.
	static void open_and_load(module_impl &m);

public:
	explicit module_manager_impl(core_impl &c) noexcept : core_(c) {}

	std::vector<found> discover(const std::vector<std::filesystem::path> &search_dirs) const;
	std::vector<ref<module>> load(const std::vector<found> &modules);
	void unload_all() noexcept;
};

} // namespace knurbs::detail
