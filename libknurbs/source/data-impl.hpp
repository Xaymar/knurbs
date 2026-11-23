#pragma once

#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <variant>
#include <vector>

#include "knurbs/data.hpp"

namespace knurbs::detail {

using data_value = std::variant<bool, std::int64_t, double, std::string, ref<data>, ref<data_array>>;
using data_map = std::map<std::string, data_value, std::less<>>;

// Locking: each operation holds at most one object's lock at a time. Operations that walk a tree (to_json, clone,
// the cycle check) copy the child refs out under the parent's shared lock, release it, and only then visit the
// children, so no two locks are ever nested and two threads cannot deadlock on each other's order.
class data_impl final : public data {
public:
	mutable std::shared_mutex mutex;
	data_map user;
	data_map defaults;

	data_impl() = default;
	~data_impl() override;
};

class data_array_impl final : public data_array {
public:
	mutable std::shared_mutex mutex;
	std::vector<ref<data>> items;

	data_array_impl() = default;
	~data_array_impl() override;
};

} // namespace knurbs::detail
