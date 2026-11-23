#include "data-impl.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <mutex>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "knurbs/error.hpp"
#include "knurbs/naming.hpp"

namespace knurbs {

namespace detail {

data_impl::~data_impl() = default;
data_array_impl::~data_array_impl() = default;

namespace {

// Levels of nesting, counting every object and array, the top-level object included.
constexpr std::size_t max_depth = 64;

// data and data_array are only ever constructed as their impl classes (their constructors are private), so these
// casts are always to the dynamic type.
const data_impl &impl(const data &d) noexcept
{
	return static_cast<const data_impl &>(d);
}

data_impl &impl(data &d) noexcept
{
	return static_cast<data_impl &>(d);
}

const data_array_impl &impl(const data_array &a) noexcept
{
	return static_cast<const data_array_impl &>(a);
}

data_array_impl &impl(data_array &a) noexcept
{
	return static_cast<data_array_impl &>(a);
}

void check_depth(std::size_t depth)
{
	if (depth > max_depth) {
		throw error::invalid_argument(std::format("data: nested deeper than {} levels", max_depth));
	}
}

// The user value, else the default, else nullptr. The caller holds d.mutex.
const data_value *find_value(const data_impl &d, std::string_view key) noexcept
{
	if (auto it = d.user.find(key); it != d.user.end()) {
		return &it->second;
	}
	if (auto it = d.defaults.find(key); it != d.defaults.end()) {
		return &it->second;
	}
	return nullptr;
}

template<class T> T get_value(const data &self, std::string_view key, std::string_view type_name)
{
	const data_impl &d = impl(self);
	std::shared_lock lock(d.mutex);
	const data_value *value = find_value(d, key);
	if (!value) {
		throw error::not_found(std::format("data: no value for key: {}", key));
	}
	if (const T *typed = std::get_if<T>(value)) {
		return *typed;
	}
	if constexpr (std::is_same_v<T, double>) {
		if (const std::int64_t *integer = std::get_if<std::int64_t>(value)) {
			return static_cast<double>(*integer);
		}
	}
	throw error::invalid_argument(std::format("data: value of key {} is not {}", key, type_name));
}

// A node of a tree walk: exactly one of the two refs is set.
struct node {
	ref<data> object;
	ref<data_array> array;

	const knurbs::object *identity() const noexcept
	{
		if (object) {
			return object.get();
		}
		return array.get();
	}
};

// Appends the objects and arrays held by `d` (user values and defaults) to `out`, under d's shared lock only.
void collect_children(const data_impl &d, std::vector<node> &out)
{
	std::shared_lock lock(d.mutex);
	for (const data_map *map : {&d.user, &d.defaults}) {
		for (const auto &[key, value] : *map) {
			if (const ref<data> *child = std::get_if<ref<data>>(&value)) {
				out.push_back({*child, {}});
			} else if (const ref<data_array> *list = std::get_if<ref<data_array>>(&value)) {
				out.push_back({{}, *list});
			}
		}
	}
}

// True if `target` is `start` or is reachable from it. Holds one shared lock at a time.
bool reaches(node start, const knurbs::object *target)
{
	std::vector<node> pending;
	pending.push_back(std::move(start));
	std::unordered_set<const knurbs::object *> visited;
	while (!pending.empty()) {
		node current = std::move(pending.back());
		pending.pop_back();
		const knurbs::object *self = current.identity();
		if (self == target) {
			return true;
		}
		if (!visited.insert(self).second) {
			continue;
		}
		if (current.object) {
			collect_children(impl(*current.object), pending);
		} else {
			const data_array_impl &list = impl(*current.array);
			std::shared_lock lock(list.mutex);
			for (const ref<data> &item : list.items) {
				pending.push_back({item, {}});
			}
		}
	}
	return false;
}

void store(data_impl &d, data_map data_impl::*which, std::string_view key, data_value value)
{
	data_value previous; // released after the lock, in case it held the last reference to a subtree
	std::unique_lock lock(d.mutex);
	data_map &map = d.*which;
	if (auto it = map.find(key); it != map.end()) {
		previous = std::exchange(it->second, std::move(value));
	} else {
		map.emplace(std::string(key), std::move(value));
	}
}

void check_finite(double value)
{
	if (!std::isfinite(value)) {
		throw error::invalid_argument("data: a double must be finite");
	}
}

void check_object(const data &self, const ref<data> &value)
{
	if (!value) {
		throw error::invalid_argument("data: object value is empty");
	}
	if (reaches({value, {}}, &self)) {
		throw error::invalid_argument("data: the value contains the object it is stored in");
	}
}

void check_array(const data &self, const ref<data_array> &value)
{
	if (!value) {
		throw error::invalid_argument("data: array value is empty");
	}
	if (reaches({{}, value}, &self)) {
		throw error::invalid_argument("data: the value contains the object it is stored in");
	}
}

nlohmann::json array_to_json(const data_array_impl &list, std::size_t depth);

nlohmann::json object_to_json(const data_impl &d, std::size_t depth)
{
	check_depth(depth);
	nlohmann::json out = nlohmann::json::object();
	std::vector<std::pair<std::string, node>> children;
	{
		std::shared_lock lock(d.mutex);
		for (const auto &[key, value] : d.user) {
			if (const bool *b = std::get_if<bool>(&value)) {
				out[key] = *b;
			} else if (const std::int64_t *i = std::get_if<std::int64_t>(&value)) {
				out[key] = *i;
			} else if (const double *f = std::get_if<double>(&value)) {
				out[key] = *f;
			} else if (const std::string *s = std::get_if<std::string>(&value)) {
				out[key] = *s;
			} else if (const ref<data> *child = std::get_if<ref<data>>(&value)) {
				children.emplace_back(key, node{*child, {}});
			} else {
				children.emplace_back(key, node{{}, std::get<ref<data_array>>(value)});
			}
		}
	}
	for (const auto &[key, child] : children) {
		if (child.object) {
			out[key] = object_to_json(impl(*child.object), depth + 1);
		} else {
			out[key] = array_to_json(impl(*child.array), depth + 1);
		}
	}
	return out;
}

nlohmann::json array_to_json(const data_array_impl &list, std::size_t depth)
{
	check_depth(depth);
	std::vector<ref<data>> items;
	{
		std::shared_lock lock(list.mutex);
		items = list.items;
	}
	nlohmann::json out = nlohmann::json::array();
	for (const ref<data> &item : items) {
		out.push_back(object_to_json(impl(*item), depth + 1));
	}
	return out;
}

ref<data_array_impl> clone_array(const data_array_impl &list, std::size_t depth);

ref<data_impl> clone_object(const data_impl &d, std::size_t depth)
{
	check_depth(depth);
	ref<data_impl> copy = make<data_impl>();
	{
		std::shared_lock lock(d.mutex);
		copy->user = d.user;
		copy->defaults = d.defaults;
	}
	// The copy is not shared yet, so its maps are changed without its lock.
	for (data_map *map : {&copy->user, &copy->defaults}) {
		for (auto &[key, value] : *map) {
			if (ref<data> *child = std::get_if<ref<data>>(&value)) {
				*child = clone_object(impl(**child), depth + 1);
			} else if (ref<data_array> *list = std::get_if<ref<data_array>>(&value)) {
				*list = clone_array(impl(**list), depth + 1);
			}
		}
	}
	return copy;
}

ref<data_array_impl> clone_array(const data_array_impl &list, std::size_t depth)
{
	check_depth(depth);
	ref<data_array_impl> copy = make<data_array_impl>();
	{
		std::shared_lock lock(list.mutex);
		copy->items = list.items;
	}
	for (ref<data> &item : copy->items) {
		item = clone_object(impl(*item), depth + 1);
	}
	return copy;
}

// nlohmann SAX handler that builds the tree directly, so the depth limit applies before anything deeper is built.
// The tree is not shared while it is built, so it is changed without locks.
class json_reader {
	struct frame {
		data_impl *object = nullptr; // exactly one of object and array is set
		data_array_impl *array = nullptr;
		std::string key;
	};

	std::vector<frame> stack_;
	ref<data_impl> root_;
	std::string error_;

	bool fail(std::string message)
	{
		if (error_.empty()) {
			error_ = std::move(message);
		}
		return false;
	}

	bool put(data_value value)
	{
		if (stack_.empty()) {
			return fail("the top level is not an object");
		}
		frame &top = stack_.back();
		if (!top.object) {
			return fail("array elements must be objects");
		}
		top.object->user.insert_or_assign(top.key, std::move(value));
		return true;
	}

	bool check_open()
	{
		if (stack_.size() >= max_depth) {
			return fail(std::format("nested deeper than {} levels", max_depth));
		}
		return true;
	}

public:
	using json = nlohmann::json;

	ref<data_impl> take_root() noexcept { return std::move(root_); }
	const std::string &error() const noexcept { return error_; }

	bool null() { return fail("null values are not supported"); }
	bool boolean(bool value) { return put(value); }
	bool number_integer(json::number_integer_t value) { return put(std::int64_t{value}); }
	bool number_unsigned(json::number_unsigned_t value)
	{
		if (value > static_cast<json::number_unsigned_t>(std::numeric_limits<std::int64_t>::max())) {
			return fail(std::format("integer {} is out of range", value));
		}
		return put(static_cast<std::int64_t>(value));
	}
	bool number_float(json::number_float_t value, const json::string_t &text)
	{
		// The parser hands integers too large for its integer types over as floating point. A literal without a
		// fraction or exponent is such an integer: reject it rather than store it rounded.
		if (text.find_first_of(".eE") == json::string_t::npos) {
			return fail(std::format("integer {} is out of range", text));
		}
		return put(double{value});
	}
	bool string(json::string_t &value) { return put(std::move(value)); }
	bool binary(json::binary_t &) { return fail("binary values are not supported"); }

	bool start_object(std::size_t)
	{
		if (!check_open()) {
			return false;
		}
		ref<data_impl> child = make<data_impl>();
		data_impl *raw = child.get();
		if (stack_.empty()) {
			root_ = std::move(child);
		} else if (data_array_impl *parent = stack_.back().array) {
			parent->items.push_back(std::move(child));
		} else if (!put(ref<data>(std::move(child)))) {
			return false;
		}
		stack_.push_back({raw, nullptr, {}});
		return true;
	}

	bool key(json::string_t &value)
	{
		try {
			naming::check_key(value);
		} catch (const error::invalid_argument &e) {
			return fail(e.what());
		}
		stack_.back().key = std::move(value);
		return true;
	}

	bool end_object()
	{
		stack_.pop_back();
		return true;
	}

	bool start_array(std::size_t)
	{
		if (!check_open()) {
			return false;
		}
		ref<data_array_impl> child = make<data_array_impl>();
		data_array_impl *raw = child.get();
		if (!put(ref<data_array>(std::move(child)))) {
			return false;
		}
		stack_.push_back({nullptr, raw, {}});
		return true;
	}

	bool end_array()
	{
		stack_.pop_back();
		return true;
	}

	bool parse_error(std::size_t, const std::string &, const json::exception &e) { return fail(e.what()); }
};

} // namespace

} // namespace detail

using detail::impl;

ref<data> data::create()
{
	return make<detail::data_impl>();
}

ref<data> data::from_json(std::string_view json)
{
	detail::json_reader reader;
	bool ok = nlohmann::json::sax_parse(json.begin(), json.end(), &reader);
	ref<detail::data_impl> root = reader.take_root();
	if (!ok || !root) {
		throw error::invalid_argument(std::format("data: invalid JSON: {}", reader.error()));
	}
	return root;
}

std::string data::to_json(bool pretty) const
{
	nlohmann::json out = detail::object_to_json(impl(*this), 1);
	try {
		return out.dump(pretty ? 4 : -1, ' ', false, nlohmann::json::error_handler_t::strict);
	} catch (const nlohmann::json::type_error &e) {
		throw error::invalid_argument(std::format("data: cannot write JSON: {}", e.what()));
	}
}

bool data::has(std::string_view key) const
{
	const detail::data_impl &d = impl(*this);
	std::shared_lock lock(d.mutex);
	return detail::find_value(d, key) != nullptr;
}

bool data::has_user_value(std::string_view key) const
{
	const detail::data_impl &d = impl(*this);
	std::shared_lock lock(d.mutex);
	return d.user.find(key) != d.user.end();
}

void data::erase(std::string_view key)
{
	detail::data_impl &d = impl(*this);
	detail::data_value previous; // released after the lock
	std::unique_lock lock(d.mutex);
	if (auto it = d.user.find(key); it != d.user.end()) {
		previous = std::move(it->second);
		d.user.erase(it);
	}
}

std::vector<std::string> data::keys() const
{
	const detail::data_impl &d = impl(*this);
	std::shared_lock lock(d.mutex);
	std::vector<std::string> out;
	out.reserve(d.user.size() + d.defaults.size());
	auto user = d.user.begin();
	auto defaults = d.defaults.begin();
	while (user != d.user.end() || defaults != d.defaults.end()) {
		if (defaults == d.defaults.end() || (user != d.user.end() && user->first < defaults->first)) {
			out.push_back((user++)->first);
		} else if (user == d.user.end() || defaults->first < user->first) {
			out.push_back((defaults++)->first);
		} else {
			out.push_back(user->first);
			++user;
			++defaults;
		}
	}
	return out;
}

bool data::get_bool(std::string_view key) const
{
	return detail::get_value<bool>(*this, key, "a bool");
}

std::int64_t data::get_int(std::string_view key) const
{
	return detail::get_value<std::int64_t>(*this, key, "an integer");
}

double data::get_double(std::string_view key) const
{
	return detail::get_value<double>(*this, key, "a number");
}

std::string data::get_string(std::string_view key) const
{
	return detail::get_value<std::string>(*this, key, "a string");
}

ref<data> data::get_object(std::string_view key) const
{
	return detail::get_value<ref<data>>(*this, key, "an object");
}

ref<data_array> data::get_array(std::string_view key) const
{
	return detail::get_value<ref<data_array>>(*this, key, "an array");
}

void data::set_bool(std::string_view key, bool value)
{
	naming::check_key(key);
	detail::store(impl(*this), &detail::data_impl::user, key, value);
}

void data::set_int(std::string_view key, std::int64_t value)
{
	naming::check_key(key);
	detail::store(impl(*this), &detail::data_impl::user, key, value);
}

void data::set_double(std::string_view key, double value)
{
	naming::check_key(key);
	detail::check_finite(value);
	detail::store(impl(*this), &detail::data_impl::user, key, value);
}

void data::set_string(std::string_view key, std::string_view value)
{
	naming::check_key(key);
	detail::store(impl(*this), &detail::data_impl::user, key, std::string(value));
}

void data::set_object(std::string_view key, ref<data> value)
{
	naming::check_key(key);
	detail::check_object(*this, value);
	detail::store(impl(*this), &detail::data_impl::user, key, std::move(value));
}

void data::set_array(std::string_view key, ref<data_array> value)
{
	naming::check_key(key);
	detail::check_array(*this, value);
	detail::store(impl(*this), &detail::data_impl::user, key, std::move(value));
}

void data::set_default_bool(std::string_view key, bool value)
{
	naming::check_key(key);
	detail::store(impl(*this), &detail::data_impl::defaults, key, value);
}

void data::set_default_int(std::string_view key, std::int64_t value)
{
	naming::check_key(key);
	detail::store(impl(*this), &detail::data_impl::defaults, key, value);
}

void data::set_default_double(std::string_view key, double value)
{
	naming::check_key(key);
	detail::check_finite(value);
	detail::store(impl(*this), &detail::data_impl::defaults, key, value);
}

void data::set_default_string(std::string_view key, std::string_view value)
{
	naming::check_key(key);
	detail::store(impl(*this), &detail::data_impl::defaults, key, std::string(value));
}

void data::set_default_object(std::string_view key, ref<data> value)
{
	naming::check_key(key);
	detail::check_object(*this, value);
	detail::store(impl(*this), &detail::data_impl::defaults, key, std::move(value));
}

void data::set_default_array(std::string_view key, ref<data_array> value)
{
	naming::check_key(key);
	detail::check_array(*this, value);
	detail::store(impl(*this), &detail::data_impl::defaults, key, std::move(value));
}

ref<data> data::clone() const
{
	return detail::clone_object(impl(*this), 1);
}

ref<data_array> data_array::create()
{
	return make<detail::data_array_impl>();
}

std::size_t data_array::size() const
{
	const detail::data_array_impl &list = impl(*this);
	std::shared_lock lock(list.mutex);
	return list.items.size();
}

ref<data> data_array::at(std::size_t index) const
{
	const detail::data_array_impl &list = impl(*this);
	std::shared_lock lock(list.mutex);
	if (index >= list.items.size()) {
		throw error::out_of_range(
			std::format("data_array: index {} is out of range (size {})", index, list.items.size()));
	}
	return list.items[index];
}

void data_array::push_back(ref<data> value)
{
	if (!value) {
		throw error::invalid_argument("data_array: value is empty");
	}
	if (detail::reaches({value, {}}, this)) {
		throw error::invalid_argument("data_array: the value contains the array it is stored in");
	}
	detail::data_array_impl &list = impl(*this);
	std::unique_lock lock(list.mutex);
	list.items.push_back(std::move(value));
}

void data_array::erase(std::size_t index)
{
	detail::data_array_impl &list = impl(*this);
	ref<data> previous; // released after the lock
	std::unique_lock lock(list.mutex);
	if (index >= list.items.size()) {
		throw error::out_of_range(
			std::format("data_array: index {} is out of range (size {})", index, list.items.size()));
	}
	previous = std::move(list.items[index]);
	list.items.erase(list.items.begin() + static_cast<std::ptrdiff_t>(index));
}

} // namespace knurbs
