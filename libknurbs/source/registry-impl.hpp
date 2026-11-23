#pragma once

#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "knurbs/core.hpp"
#include "knurbs/registry.hpp"

namespace knurbs::detail {

class registry_impl final : public registry {
	struct type_entry {
		ref<type> value;
		type_kind kind; // read once in add(): no plugin virtual runs under the lock
		std::string owner;
	};
	struct alias_entry {
		std::string target;
		std::string owner;
	};

	const core &core_;
	mutable std::shared_mutex mutex_;
	std::map<std::string, type_entry, std::less<>> types_;
	std::map<std::string, alias_entry, std::less<>> aliases_;

	// Follows aliases from `id`; the caller holds mutex_.
	const type_entry *resolve(std::string_view id) const;

public:
	explicit registry_impl(const core &c) noexcept : core_(c) {}

	void add(ref<type> t, std::string_view owner_module);
	void add_alias(std::string_view old_id, std::string_view new_id, std::string_view owner_module);
	ref<type> find(std::string_view id) const;
	std::vector<ref<type>> list(type_kind kind) const;
	void remove_module(std::string_view owner_module);
	// Drops everything. Used by core's shutdown, so it does not check that the core is alive.
	void clear() noexcept;
};

} // namespace knurbs::detail
