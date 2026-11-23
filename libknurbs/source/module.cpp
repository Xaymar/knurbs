#include "knurbs/module.hpp"

#include <utility>

#include "core-impl.hpp"
#include "knurbs/error.hpp"
#include "module-loader.hpp"

namespace knurbs {

	namespace detail {

		module_impl::module_impl(core_impl& c, module_manager::found f) : core_(&c), logger_(c.logger()), name_(std::move(f.name)), binary_(std::move(f.binary)), data_(std::move(f.data)) {}

	} // namespace detail

	// module and module_manager are only ever constructed as their impl classes (their constructors are private).
	std::string_view module::name() const noexcept
	{
		return static_cast<const detail::module_impl*>(this)->name_;
	}

	const std::filesystem::path& module::binary_path() const noexcept
	{
		return static_cast<const detail::module_impl*>(this)->binary_;
	}

	const std::filesystem::path& module::data_path() const noexcept
	{
		return static_cast<const detail::module_impl*>(this)->data_;
	}

	namespace {

		detail::core_impl& live_core(const detail::module_impl& m)
		{
			detail::core_impl* c = m.core_.load(std::memory_order_acquire);
			if (!c) {
				throw error::core_gone("module: the core that loaded it is gone");
			}
			return *c;
		}

	} // namespace

	core& module::core() const
	{
		return live_core(*static_cast<const detail::module_impl*>(this));
	}

	registry& module::registry() const
	{
		return live_core(*static_cast<const detail::module_impl*>(this)).registry();
	}

	ref<log::logger> module::logger() const
	{
		return static_cast<const detail::module_impl*>(this)->logger_;
	}

	bool module::failed() const noexcept
	{
		return static_cast<const detail::module_impl*>(this)->failed_.load(std::memory_order_acquire);
	}

	std::vector<module_manager::found> module_manager::discover(const std::vector<std::filesystem::path>& search_dirs) const
	{
		return static_cast<const detail::module_manager_impl*>(this)->discover(search_dirs);
	}

	std::vector<ref<module>> module_manager::load(const std::vector<found>& modules)
	{
		return static_cast<detail::module_manager_impl*>(this)->load(modules);
	}

	void module_manager::unload_all() noexcept
	{
		static_cast<detail::module_manager_impl*>(this)->unload_all();
	}

} // namespace knurbs
