#include "registry-impl.hpp"

#include <mutex>
#include <string>
#include <utility>

#include "knurbs/error.hpp"
#include "knurbs/naming.hpp"
#include "module-loader.hpp"

namespace knurbs {

ref<properties> type::properties(const data &) const
{
	return knurbs::properties::create();
}

namespace detail {

const registry_impl::type_entry *registry_impl::resolve(std::string_view id) const
{
	// Alias chains are acyclic (add_alias refuses cycles), so this ends; the bound only guards against a bug.
	for (std::size_t steps = 0; steps <= aliases_.size(); ++steps) {
		if (auto t = types_.find(id); t != types_.end()) {
			return &t->second;
		}
		auto a = aliases_.find(id);
		if (a == aliases_.end()) {
			return nullptr;
		}
		id = a->second.target;
	}
	return nullptr;
}

std::string_view registry_impl::owner_or_current(std::string_view owner_module) const noexcept
{
	return owner_module.empty() ? current_module_name(core_) : owner_module;
}

void registry_impl::add(ref<type> t, std::string_view owner_module)
{
	core_.check_alive();
	if (!t) {
		throw error::invalid_argument("registry: type is empty");
	}
	// Plugin virtuals run here, before the lock is taken, and are never called again.
	const std::string id(t->id());
	const type_kind kind = t->kind();
	if (!naming::is_valid_type_id(id)) {
		throw error::invalid_argument("registry: invalid type ID: " + id);
	}
	std::unique_lock lock(mutex_);
	// Checked again under the lock: shutdown clears the registry under it, so nothing is added after the clear.
	core_.check_alive();
	if (types_.contains(id) || aliases_.contains(id)) {
		throw error::already_exists("registry: ID already registered: " + id);
	}
	types_.emplace(id, type_entry{std::move(t), kind, std::string(owner_or_current(owner_module))});
}

void registry_impl::add_alias(std::string_view old_id, std::string_view new_id, std::string_view owner_module)
{
	core_.check_alive();
	if (old_id.empty()) {
		throw error::invalid_argument("registry: alias old ID is empty");
	}
	if (!naming::is_valid_type_id(new_id)) {
		throw error::invalid_argument("registry: invalid type ID: " + std::string(new_id));
	}
	std::unique_lock lock(mutex_);
	core_.check_alive();
	if (types_.contains(old_id) || aliases_.contains(old_id)) {
		throw error::already_exists("registry: ID already registered: " + std::string(old_id));
	}
	// old_id is neither a type nor an alias yet, so existing chains are finite; the new alias closes a cycle only
	// if a chain from new_id reaches old_id.
	std::string_view cur = new_id;
	for (;;) {
		if (cur == old_id) {
			throw error::invalid_argument("registry: alias would form a cycle: " + std::string(old_id));
		}
		auto a = aliases_.find(cur);
		if (a == aliases_.end()) {
			break;
		}
		cur = a->second.target;
	}
	aliases_.emplace(std::string(old_id),
			 alias_entry{std::string(new_id), std::string(owner_or_current(owner_module))});
}

ref<type> registry_impl::find(std::string_view id) const
{
	core_.check_alive();
	std::shared_lock lock(mutex_);
	const type_entry *e = resolve(id);
	return e ? e->value : ref<type>();
}

std::vector<ref<type>> registry_impl::list(type_kind kind) const
{
	core_.check_alive();
	std::vector<ref<type>> result;
	std::shared_lock lock(mutex_);
	for (const auto &[id, entry] : types_) { // std::map: sorted by ID
		if (entry.kind == kind) {
			result.push_back(entry.value);
		}
	}
	return result;
}

void registry_impl::remove_module(std::string_view owner_module)
{
	core_.check_alive();
	// Dropped after unlocking: a type's destructor is plugin code and may call back into the registry.
	std::vector<ref<type>> dropped;
	{
		std::unique_lock lock(mutex_);
		core_.check_alive();
		dropped.reserve(types_.size()); // nothing below throws, so the removal is all or nothing
		for (auto it = types_.begin(); it != types_.end();) {
			if (it->second.owner == owner_module) {
				dropped.push_back(std::move(it->second.value));
				it = types_.erase(it);
			} else {
				++it;
			}
		}
		std::erase_if(aliases_, [&](const auto &a) { return a.second.owner == owner_module; });
	}
}

void registry_impl::clear() noexcept
{
	std::map<std::string, type_entry, std::less<>> types;
	{
		std::unique_lock lock(mutex_);
		types.swap(types_);
		aliases_.clear();
	}
	// `types` is destroyed here, without the lock held.
}

} // namespace detail

// registry is only ever constructed as a registry_impl (its constructor is private).
void registry::add(ref<type> t, std::string_view owner_module)
{
	static_cast<detail::registry_impl *>(this)->add(std::move(t), owner_module);
}

void registry::add_alias(std::string_view old_id, std::string_view new_id, std::string_view owner_module)
{
	static_cast<detail::registry_impl *>(this)->add_alias(old_id, new_id, owner_module);
}

ref<type> registry::find(std::string_view id) const
{
	return static_cast<const detail::registry_impl *>(this)->find(id);
}

std::vector<ref<type>> registry::list(type_kind kind) const
{
	return static_cast<const detail::registry_impl *>(this)->list(kind);
}

void registry::remove_module(std::string_view owner_module)
{
	static_cast<detail::registry_impl *>(this)->remove_module(owner_module);
}

} // namespace knurbs
