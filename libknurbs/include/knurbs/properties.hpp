#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "knurbs/function.hpp"
#include "knurbs/data.hpp"
#include "knurbs/export.hpp"
#include "knurbs/object.hpp"
#include "knurbs/ref.hpp"

namespace knurbs {

namespace detail {
class property_impl;
class properties_impl;
} // namespace detail

class KNURBS_API properties;

// Values are only ever appended.
enum class property_kind : std::uint8_t {
	boolean,
	integer,
	floating,
	text,
	path,
	list,
	color,
	button,
	group,
	notice,
};

// One entry of a properties list: a description of a setting (its id, kind, label and constraints), not its value;
// values live in a data tree. Created by the add_* functions of properties. Thread-safe.
//
// Calling an accessor that doesn't fit the property's kind (min() on a text property, add_item() on a property
// that isn't a list, group() on a property that isn't a group) throws error::invalid_argument.
class KNURBS_API property : public object {
	// Only created by properties::add_*; not meant to be subclassed.
	property() noexcept = default;
	friend class detail::property_impl;

public:
	std::string_view id() const noexcept;
	property_kind kind() const noexcept;
	// The label, or the text of a notice.
	std::string_view label() const noexcept;

	bool enabled() const;
	bool visible() const;
	void set_enabled(bool value);
	void set_visible(bool value);

	// Integer and floating properties.
	double min() const;
	double max() const;
	double step() const;

	// List properties.
	void add_item(std::string label, std::string value);
	std::size_t item_count() const;

	// Group properties.
	ref<properties> group() const;

	// Button properties: runs the button's callback and returns its result; false if the owning properties is
	// already gone. The callback runs without any internal lock held, while the owning properties is kept alive.
	bool click();
};

// An ordered description of the settings of an object, such as a source or a filter. Thread-safe, except that two
// threads concurrently adding each other as groups can form a cycle; callers must not do that.
//
// Callbacks run without any internal lock held and while a ref<properties> keeps this object alive, so a callback
// may call find, list and add_* on the same properties, and may drop the caller's last reference to it.
class KNURBS_API properties : public object {
	// Only created by create(); not meant to be subclassed.
	properties() noexcept = default;
	friend class detail::properties_impl;

public:
	static ref<properties> create();

	// Every id goes through naming::check_key; a second property with the same id throws error::already_exists.
	// add_int and add_float throw error::invalid_argument for min > max, step <= 0 and NaN. add_group throws
	// error::invalid_argument for an empty ref and for a group that would make the properties contain themselves.
	ref<property> add_bool(std::string_view id, std::string label);
	ref<property> add_int(std::string_view id, std::string label, double min, double max, double step);
	ref<property> add_float(std::string_view id, std::string label, double min, double max, double step);
	ref<property> add_text(std::string_view id, std::string label);
	ref<property> add_path(std::string_view id, std::string label);
	ref<property> add_list(std::string_view id, std::string label);
	ref<property> add_color(std::string_view id, std::string label);
	ref<property> add_button(std::string_view id, std::string label,
				 knurbs::move_only_function<bool(properties &, property &)> on_click);
	ref<property> add_group(std::string_view id, std::string label, ref<properties> group);
	ref<property> add_notice(std::string_view id, std::string text);

	// The property with this id, searching groups too (at most 64 levels deep); empty if there is none.
	ref<property> find(std::string_view id) const;
	// The properties of this level, in insertion order (groups are not expanded).
	std::vector<ref<property>> list() const;

	// Sets the callback that notify_modified runs for this id, replacing an earlier one. The id need not exist yet.
	void set_modified(std::string_view id,
			  knurbs::move_only_function<bool(properties &, property &, data &)> callback);
	// Runs the callback set for `id` and returns its result; false if there is no callback or no such property.
	bool notify_modified(std::string_view id, data &settings);
};

} // namespace knurbs
