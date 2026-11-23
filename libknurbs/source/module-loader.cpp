#include "module-loader.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "c/c-guard.hpp"
#include "core-impl.hpp"
#include "knurbs/error.hpp"
#include "knurbs/platform.hpp"
#include "knurbs/thread.hpp"
#include "knurbs/version.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// CMake's prefix and suffix for loadable modules on the target platform, from libknurbs/CMakeLists.txt.
#ifndef KNURBS_MODULE_PREFIX
#error "KNURBS_MODULE_PREFIX must be defined by the build"
#endif
#ifndef KNURBS_MODULE_SUFFIX
#error "KNURBS_MODULE_SUFFIX must be defined by the build"
#endif

namespace knurbs::detail {

	namespace {

		using header_entry = const struct knurbs_module_header* (*)();

		// The module whose entry point runs on this thread. Per thread, not process-wide: set only by
		// current_module_scope, around one entry-point call.
		thread_local const module_impl* current_module = nullptr;

		// Loads a library. The handle is never closed (see module_impl::library_).
		void* open_library(const std::filesystem::path& path)
		{
#if defined(_WIN32)
			// The path's native form is UTF-16, so non-ASCII paths never go through the ANSI code page.
			// The module's own directory is searched for its dependencies, per call and without touching the process-wide
			// DLL directory, so parallel loads cannot race. LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs an absolute path with
			// backslashes (CMake and callers may hand us forward slashes).
			const std::filesystem::path full   = std::filesystem::absolute(path).make_preferred();
			HMODULE                     handle = LoadLibraryExW(full.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
			if (!handle) {
				throw error::plugin_failed(std::format("cannot load the library (error {})", GetLastError()));
			}
			return reinterpret_cast<void*>(handle);
#else
			void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
			if (!handle) {
				const char* message = dlerror();
				throw error::plugin_failed(std::format("cannot load the library: {}", message ? message : "unknown error"));
			}
			return handle;
#endif
		}

		// The address of the exported symbol `name`, or nullptr.
		template<class F>
		F find_symbol(void* library, const char* name) noexcept
		{
#if defined(_WIN32)
			// Through void (*)(), the generic function pointer type, so the cast is not reported as a type mismatch.
			return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(static_cast<HMODULE>(library), name)));
#else
			return reinterpret_cast<F>(dlsym(library, name));
#endif
		}

		// The entry point `name` of a module of API kind `api_kind` (KNURBS_MODULE_API_C or KNURBS_MODULE_API_CPP).
		entry_point find_entry(void* library, std::uint32_t api_kind, const char* name) noexcept
		{
			if (api_kind == KNURBS_MODULE_API_C) {
				auto e = find_symbol<void (*)(knurbs_module_t*)>(library, name);
				return e ? entry_point(e) : entry_point();
			}
			auto e = find_symbol<void (*)(module*)>(library, name);
			return e ? entry_point(e) : entry_point();
		}

	} // namespace

	void entry_point::operator()(module_impl& m) const
	{
		if (c_) {
			c_(c_to_typed_handle<knurbs_module_t>(&m));
		} else {
			cpp_(&m);
		}
	}

	current_module_scope::current_module_scope(const module_impl& m) noexcept : previous_(current_module)
	{
		current_module = &m;
	}

	current_module_scope::~current_module_scope()
	{
		current_module = previous_;
	}

	std::string_view current_module_name(const core& c) noexcept
	{
		// Only a module of the same core counts: two cores never interfere.
		if (current_module && current_module->core_.load(std::memory_order_acquire) == &c) {
			return current_module->name_;
		}
		return {};
	}

	std::vector<module_manager::found> module_manager_impl::discover(const std::vector<std::filesystem::path>& search_dirs) const
	{
		const std::string                         file_prefix = KNURBS_MODULE_PREFIX;
		const std::string                         file_suffix = KNURBS_MODULE_SUFFIX;
		std::map<std::string, found, std::less<>> result; // sorted by name

		for (const std::filesystem::path& search_dir : search_dirs) {
			std::error_code             ec;
			const std::filesystem::path dir = std::filesystem::absolute(search_dir, ec);
			if (ec) {
				continue;
			}
			// A directory that is missing or cannot be read has nothing to find; a read error ends its listing.
			for (std::filesystem::directory_iterator it(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
				const std::filesystem::directory_entry& entry = *it;
				// Its own error code: a failed check skips this entry, not the rest of the directory.
				std::error_code entry_ec;
				if (!entry.is_directory(entry_ec)) {
					continue;
				}
				std::string           name   = platform::path_to_utf8(entry.path().filename());
				std::filesystem::path binary = entry.path() / "bin" / platform::path_from_utf8(file_prefix + name + file_suffix);
				if (!std::filesystem::is_regular_file(binary, entry_ec)) {
					continue;
				}
				if (result.contains(name)) {
					core_.logger()->warning("module {}: also found in {}, which is ignored", name, platform::path_to_utf8(entry.path()));
					continue;
				}
				std::filesystem::path data = entry.path() / "data";
				result.emplace(name, found{name, std::move(binary), std::move(data)});
			}
		}

		std::vector<found> sorted;
		sorted.reserve(result.size());
		for (auto& [name, f] : result) {
			sorted.push_back(std::move(f));
		}
		return sorted;
	}

	void module_manager_impl::fail(module_impl& m, std::string_view reason, bool remove_types) noexcept
	{
		m.failed_.store(true, std::memory_order_release);
		if (remove_types) {
			try {
				core_.registry().remove_module(m.name_);
			} catch (...) {
				// Only error::core_gone: shutdown has begun and clears the registry anyway.
			}
		}
		try {
			m.logger_->write(log::level::error, std::format("module {}: {}", m.name_, reason));
		} catch (...) {
			// Out of memory for the message; the module is failed all the same.
		}
	}

	template<class F>
	void module_manager_impl::run_each(const std::vector<module_impl*>& mods, F entry, bool remove_types)
	{
		std::vector<std::jthread> threads;
		threads.reserve(mods.size());
		for (module_impl* m : mods) {
			try {
				threads.emplace_back([this, m, &entry, remove_types] {
					thread::set_current_name(m->name_);
					current_module_scope scope(*m);
					// Nothing may escape a module's thread: that would terminate the process.
					try {
						entry(*m);
					} catch (const std::exception& e) {
						fail(*m, e.what(), remove_types);
					} catch (...) {
						fail(*m, "unknown exception", remove_types);
					}
				});
			} catch (const std::system_error& e) {
				fail(*m, std::format("cannot start a thread: {}", e.what()), remove_types);
			}
		}
		// The barrier: the jthreads join here.
	}

	void module_manager_impl::open_and_load(module_impl& m)
	{
		m.library_ = open_library(m.binary_);

		auto header_fn = find_symbol<header_entry>(m.library_, "knurbs_module_header");
		if (!header_fn) {
			throw error::plugin_failed("the library has no knurbs_module_header");
		}
		const struct knurbs_module_header* header = header_fn();
		if (!header || header->size < sizeof(struct knurbs_module_header)) {
			throw error::plugin_failed("invalid module header");
		}
		const std::uint32_t api_kind = header->api_kind;
		if (api_kind != KNURBS_MODULE_API_C && api_kind != KNURBS_MODULE_API_CPP) {
			throw error::plugin_failed(std::format("unknown API kind {}", api_kind));
		}
		// The same check for C and C++ modules.
		if (header->api_major != static_cast<std::uint32_t>(version::major)) {
			throw error::version_mismatch(std::format("built for API major version {}, but this is version {}", header->api_major, version::major));
		}

		const entry_point load = find_entry(m.library_, api_kind, "knurbs_module_load");
		if (!load) {
			throw error::plugin_failed("the library has no knurbs_module_load");
		}
		m.post_load_ = find_entry(m.library_, api_kind, "knurbs_module_post_load");
		m.unload_    = find_entry(m.library_, api_kind, "knurbs_module_unload");
		load(m);
	}

	std::vector<ref<module>> module_manager_impl::load(const std::vector<found>& modules)
	{
		std::scoped_lock lock(mutex_);
		core_.check_alive();

		std::set<std::string_view, std::less<>> names;
		for (const ref<module_impl>& m : modules_) {
			names.insert(m->name_);
		}

		std::vector<ref<module_impl>> batch;
		batch.reserve(modules.size());
		for (const found& f : modules) {
			batch.push_back(make<module_impl>(core_, f));
		}
		modules_.reserve(modules_.size() + batch.size());
		std::vector<module_impl*> to_load;
		to_load.reserve(batch.size());
		for (const ref<module_impl>& m : batch) {
			modules_.push_back(m); // no throw: reserved above
			if (!names.insert(m->name_).second) {
				// Its types would be indistinguishable from the other module's, so it never runs.
				fail(*m, "a module of this name is already loaded", false);
			} else {
				to_load.push_back(m.get());
			}
		}

		run_each(to_load, &module_manager_impl::open_and_load, true);

		std::vector<module_impl*> to_post_load;
		for (module_impl* m : to_load) {
			if (!m->failed_.load(std::memory_order_acquire) && m->post_load_) {
				to_post_load.push_back(m);
			}
		}
		run_each(to_post_load, [](module_impl& m) { m.post_load_(m); }, true);

		return {batch.begin(), batch.end()};
	}

	void module_manager_impl::detach_modules() noexcept
	{
		std::scoped_lock lock(mutex_);
		for (const ref<module_impl>& m : modules_) {
			m->core_.store(nullptr, std::memory_order_release);
		}
	}

	void module_manager_impl::unload_all() noexcept
	{
		std::scoped_lock          lock(mutex_);
		std::vector<module_impl*> order;
		try {
			order.reserve(modules_.size());
		} catch (...) {
			return; // out of memory: the modules are not told, but nothing else is lost
		}
		for (const ref<module_impl>& m : modules_) {
			if (!m->unloaded_ && !m->failed_.load(std::memory_order_acquire)) {
				order.push_back(m.get());
			}
		}
		std::ranges::sort(order, std::ranges::greater{}, [](const module_impl* m) -> const std::string& { return m->name_; });

		for (module_impl* m : order) {
			m->unloaded_ = true;
			if (!m->unload_) {
				continue;
			}
			try {
				// One at a time, each on its own thread like the other entry points.
				run_each({m}, [](module_impl& mod) { mod.unload_(mod); }, false);
			} catch (...) {
				// Out of memory starting it: the module is not told.
			}
		}
	}

} // namespace knurbs::detail
