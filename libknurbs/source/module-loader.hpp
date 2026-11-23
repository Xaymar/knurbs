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

	class module_impl;

	// One of a module's entry points (load, post_load or unload), of the API kind the module is written against; empty
	// if the module does not export it.
	class entry_point {
		using cpp_entry = void (*)(module*);
		using c_entry   = void (*)(knurbs_module_t*);

		cpp_entry cpp_ = nullptr;
		c_entry   c_   = nullptr;

		public:
		entry_point() noexcept = default;
		explicit entry_point(cpp_entry e) noexcept : cpp_(e) {}
		explicit entry_point(c_entry e) noexcept : c_(e) {}

		explicit operator bool() const noexcept
		{
			return cpp_ || c_;
		}
		// Calls it with `m`, or its C handle for a C module.
		void operator()(module_impl& m) const;
	};

	class module_impl final : public module {
		public:
		module_impl(core_impl& c, module_manager::found f);

		// The core that loaded the module, until that core is destroyed (module_manager_impl::detach_modules), then
		// nullptr. The module does not keep the core alive.
		std::atomic<core_impl*>     core_;
		const ref<log::logger>      logger_;
		const std::string           name_;
		const std::filesystem::path binary_;
		const std::filesystem::path data_;
		std::atomic<bool>           failed_{false};

		// Written by the module's load thread; read only after that thread was joined.
		// The library handle is never closed (see module): types handed out by the registry can outlive the core,
		// and the module's code may still be on a stack.
		void*       library_ = nullptr;
		entry_point post_load_;
		entry_point unload_;

		// Guarded by module_manager_impl::mutex_.
		bool unloaded_ = false;
	};

	// Records which module's entry point runs on the calling thread, for as long as it exists, so the registry can
	// attribute registrations made without an owner. Scopes nest.
	class current_module_scope {
		const module_impl* previous_;

		public:
		explicit current_module_scope(const module_impl& m) noexcept;
		~current_module_scope();
		current_module_scope(const current_module_scope&)            = delete;
		current_module_scope& operator=(const current_module_scope&) = delete;
	};

	// The name of the module of core `c` whose entry point runs on this thread, or "" if there is none.
	std::string_view current_module_name(const core& c) noexcept;

	class module_manager_impl final : public module_manager {
		core_impl& core_;
		// Held for the whole of load and unload_all, so they run one at a time and shutdown waits for a load in
		// progress.
		std::mutex                    mutex_;
		std::vector<ref<module_impl>> modules_;

		// Marks `m` failed and logs "module <name>: <reason>". With `remove_types`, also drops what it registered.
		void fail(module_impl& m, std::string_view reason, bool remove_types) noexcept;
		// Runs `entry` on a thread of its own per module in `mods`, with that module as the current module, and
		// waits for all of them. An exception fails the module, with `remove_types` as for fail().
		template<class F>
		void run_each(const std::vector<module_impl*>& mods, F entry, bool remove_types);
		// Loads the library of `m`, checks its header and calls its knurbs_module_load.
		static void open_and_load(module_impl& m);

		public:
		explicit module_manager_impl(core_impl& c) noexcept : core_(c) {}

		// Tells every module the core is being destroyed: from then on module::core() and module::registry() throw
		// error::core_gone. Called from the core's destructor.
		void detach_modules() noexcept;

		std::vector<found>       discover(const std::vector<std::filesystem::path>& search_dirs) const;
		std::vector<ref<module>> load(const std::vector<found>& modules);
		void                     unload_all() noexcept;
	};

} // namespace knurbs::detail
