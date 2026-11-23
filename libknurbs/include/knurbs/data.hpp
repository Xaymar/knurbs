#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "knurbs/export.hpp"
#include "knurbs/object.hpp"
#include "knurbs/ref.hpp"

namespace knurbs {

namespace detail {
class data_impl;
class data_array_impl;
} // namespace detail

class KNURBS_API data_array;

// A settings tree: a map from keys to values of type bool, integer, double, string, object (data) or array
// (data_array, a list of objects). Every key holds a user value, a default, or both; getters return the user value
// if there is one, else the default. Thread-safe.
//
// A tree never contains itself: setters that would make it do so throw error::invalid_argument. Two threads that
// concurrently link two trees into each other can still form a cycle; to_json and clone then stop at 64 levels.
// Subtrees may be shared between several parents (a get_object result is the stored object, not a copy).
//
// Trees deeper than 64 levels (counting every object and array, the top level included) cannot be written by
// to_json, copied by clone or read by from_json.
class KNURBS_API data : public object {
	// Only created by create(), from_json() and clone(); not meant to be subclassed.
	data() noexcept = default;
	friend class detail::data_impl;

public:
	static ref<data> create();

	// Parses a JSON object into user values. Throws error::invalid_argument on a parse error, on a top level that
	// isn't an object, on nesting deeper than 64 levels, and on values the tree cannot hold: null, arrays whose
	// elements are not objects, integers outside std::int64_t, and keys that naming::check_key rejects.
	static ref<data> from_json(std::string_view json);

	// Writes the user values as JSON, keys sorted; defaults are never written. Throws error::invalid_argument for
	// a tree deeper than 64 levels or a string that is not valid UTF-8.
	std::string to_json(bool pretty = false) const;

	// True for a user value or a default.
	bool has(std::string_view key) const;
	bool has_user_value(std::string_view key) const;
	// Removes the user value (the default, if any, stays). Unknown keys are ignored.
	void erase(std::string_view key);
	// The keys of user values and defaults, sorted.
	std::vector<std::string> keys() const;

	// Each returns the user value, else the default. Throws error::not_found if there is neither, and
	// error::invalid_argument if the value has another type; get_double also accepts an integer.
	bool get_bool(std::string_view key) const;
	std::int64_t get_int(std::string_view key) const;
	double get_double(std::string_view key) const;
	std::string get_string(std::string_view key) const;
	ref<data> get_object(std::string_view key) const;
	ref<data_array> get_array(std::string_view key) const;

	// Every key goes through naming::check_key. set_double throws error::invalid_argument for NaN and infinity,
	// set_object and set_array for an empty ref or a value that would make the tree contain itself.
	void set_bool(std::string_view key, bool value);
	void set_int(std::string_view key, std::int64_t value);
	void set_double(std::string_view key, double value);
	void set_string(std::string_view key, std::string_view value);
	void set_object(std::string_view key, ref<data> value);
	void set_array(std::string_view key, ref<data_array> value);

	void set_default_bool(std::string_view key, bool value);
	void set_default_int(std::string_view key, std::int64_t value);
	void set_default_double(std::string_view key, double value);
	void set_default_string(std::string_view key, std::string_view value);
	void set_default_object(std::string_view key, ref<data> value);
	void set_default_array(std::string_view key, ref<data_array> value);

	// A deep copy of user values and defaults. Throws error::invalid_argument for a tree deeper than 64 levels.
	ref<data> clone() const;
};

// A list of objects. Thread-safe.
class KNURBS_API data_array : public object {
	// Only created by create(); not meant to be subclassed.
	data_array() noexcept = default;
	friend class detail::data_array_impl;

public:
	static ref<data_array> create();

	std::size_t size() const;
	// Throws error::out_of_range for index >= size().
	ref<data> at(std::size_t index) const;
	// Throws error::invalid_argument for an empty ref or a value that would make the tree contain itself.
	void push_back(ref<data> value);
	// Throws error::out_of_range for index >= size().
	void erase(std::size_t index);
};

} // namespace knurbs
