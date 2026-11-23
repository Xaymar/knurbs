#include "knurbs/c/properties.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "c-guard.hpp"
#include "knurbs/data.hpp"
#include "knurbs/error.hpp"
#include "knurbs/properties.hpp"
#include "knurbs/ref.hpp"

using namespace knurbs;

#define KNURBS_CHECK_KIND(c_name, cpp_name) \
	static_assert(static_cast<int>(c_name) == static_cast<int>(property_kind::cpp_name))

KNURBS_CHECK_KIND(KNURBS_PROPERTY_BOOLEAN, boolean);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_INTEGER, integer);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_FLOATING, floating);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_TEXT, text);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_PATH, path);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_LIST, list);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_COLOR, color);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_BUTTON, button);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_GROUP, group);
KNURBS_CHECK_KIND(KNURBS_PROPERTY_NOTICE, notice);

#undef KNURBS_CHECK_KIND

namespace {
properties &self(knurbs_properties_t *handle)
{
	return detail::c_self<properties>(handle, "properties");
}

property &self(knurbs_property_t *handle)
{
	return detail::c_self<property>(handle, "property");
}

knurbs_property_t *give(ref<property> value) noexcept
{
	return detail::c_to_typed_handle<knurbs_property_t>(value.detach());
}

knurbs_properties_t *give(ref<properties> value) noexcept
{
	return detail::c_to_typed_handle<knurbs_properties_t>(value.detach());
}

// The handles a callback gets do not own a reference.
knurbs_properties_t *view(properties &value) noexcept
{
	return detail::c_to_typed_handle<knurbs_properties_t>(&value);
}

knurbs_property_t *view(property &value) noexcept
{
	return detail::c_to_typed_handle<knurbs_property_t>(&value);
}

knurbs_data_t *view(data &value) noexcept
{
	return detail::c_to_typed_handle<knurbs_data_t>(&value);
}
} // namespace

extern "C" {

knurbs_result knurbs_properties_create(knurbs_properties_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(properties::create());
	});
}

knurbs_result knurbs_properties_add_bool(knurbs_properties_t *handle, const char *id, const char *label,
					 knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(
			self(handle).add_bool(detail::c_text(id, "id"), std::string(detail::c_text(label, "label"))));
	});
}

knurbs_result knurbs_properties_add_text(knurbs_properties_t *handle, const char *id, const char *label,
					 knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(
			self(handle).add_text(detail::c_text(id, "id"), std::string(detail::c_text(label, "label"))));
	});
}

knurbs_result knurbs_properties_add_path(knurbs_properties_t *handle, const char *id, const char *label,
					 knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(
			self(handle).add_path(detail::c_text(id, "id"), std::string(detail::c_text(label, "label"))));
	});
}

knurbs_result knurbs_properties_add_list(knurbs_properties_t *handle, const char *id, const char *label,
					 knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(
			self(handle).add_list(detail::c_text(id, "id"), std::string(detail::c_text(label, "label"))));
	});
}

knurbs_result knurbs_properties_add_color(knurbs_properties_t *handle, const char *id, const char *label,
					  knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(
			self(handle).add_color(detail::c_text(id, "id"), std::string(detail::c_text(label, "label"))));
	});
}

knurbs_result knurbs_properties_add_int(knurbs_properties_t *handle, const char *id, const char *label, double min,
					double max, double step, knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).add_int(detail::c_text(id, "id"), std::string(detail::c_text(label, "label")),
						 min, max, step));
	});
}

knurbs_result knurbs_properties_add_float(knurbs_properties_t *handle, const char *id, const char *label, double min,
					  double max, double step, knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).add_float(detail::c_text(id, "id"),
						   std::string(detail::c_text(label, "label")), min, max, step));
	});
}

knurbs_result knurbs_properties_add_button(knurbs_properties_t *handle, const char *id, const char *label,
					   knurbs_property_click_fn on_click, void *param, void (*free_param)(void *),
					   knurbs_property_t **out)
{
	return detail::c_guard([&] {
		// First, so that free_param runs for every failure below
		detail::c_callback_param owner(param, free_param);
		detail::c_out(out);
		if (!on_click) {
			detail::c_null_argument("on_click");
		}
		*out = give(self(handle).add_button(
			detail::c_text(id, "id"), std::string(detail::c_text(label, "label")),
			[on_click, owner = std::move(owner)](properties &props, property &prop) {
				return on_click(view(props), view(prop), owner.get());
			}));
	});
}

knurbs_result knurbs_properties_add_group(knurbs_properties_t *handle, const char *id, const char *label,
					  knurbs_properties_t *group, knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).add_group(detail::c_text(id, "id"),
						   std::string(detail::c_text(label, "label")),
						   ref<properties>::retain(group ? &self(group) : nullptr)));
	});
}

knurbs_result knurbs_properties_add_notice(knurbs_properties_t *handle, const char *id, const char *text,
					   knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(
			self(handle).add_notice(detail::c_text(id, "id"), std::string(detail::c_text(text, "text"))));
	});
}

knurbs_result knurbs_properties_find(knurbs_properties_t *handle, const char *id, knurbs_property_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).find(detail::c_text(id, "id")));
	});
}

knurbs_result knurbs_properties_list(knurbs_properties_t *handle, knurbs_property_t ***out_list, size_t *out_count)
{
	return detail::c_guard([&] {
		detail::c_out(out_list, "out_list");
		detail::c_out(out_count, "out_count");
		std::vector<ref<property>> items = self(handle).list();
		if (items.empty()) {
			return;
		}
		auto **array =
			static_cast<knurbs_property_t **>(detail::c_malloc(items.size() * sizeof(knurbs_property_t *)));
		for (std::size_t i = 0; i < items.size(); ++i) {
			array[i] = give(std::move(items[i]));
		}
		*out_list = array;
		*out_count = items.size();
	});
}

knurbs_result knurbs_properties_set_modified(knurbs_properties_t *handle, const char *id,
					     knurbs_property_modified_fn callback, void *param,
					     void (*free_param)(void *))
{
	return detail::c_guard([&] {
		// First, so that free_param runs for every failure below, and at once when the callback is cleared
		detail::c_callback_param owner(param, free_param);
		std::move_only_function<bool(properties &, property &, data &)> function;
		if (callback) {
			function = [callback, owner = std::move(owner)](properties &props, property &prop,
									data &settings) {
				return callback(view(props), view(prop), view(settings), owner.get());
			};
		}
		self(handle).set_modified(detail::c_text(id, "id"), std::move(function));
	});
}

knurbs_result knurbs_properties_notify_modified(knurbs_properties_t *handle, const char *id, knurbs_data_t *settings,
						bool *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).notify_modified(detail::c_text(id, "id"),
						    detail::c_self<data>(settings, "settings"));
	});
}

knurbs_result knurbs_property_get_id(knurbs_property_t *handle, char **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = detail::c_dup_string(self(handle).id());
	});
}

knurbs_result knurbs_property_get_kind(knurbs_property_t *handle, knurbs_property_kind *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = static_cast<knurbs_property_kind>(self(handle).kind());
	});
}

knurbs_result knurbs_property_get_label(knurbs_property_t *handle, char **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = detail::c_dup_string(self(handle).label());
	});
}

knurbs_result knurbs_property_get_enabled(knurbs_property_t *handle, bool *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).enabled();
	});
}

knurbs_result knurbs_property_get_visible(knurbs_property_t *handle, bool *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).visible();
	});
}

knurbs_result knurbs_property_set_enabled(knurbs_property_t *handle, bool value)
{
	return detail::c_guard([&] { self(handle).set_enabled(value); });
}

knurbs_result knurbs_property_set_visible(knurbs_property_t *handle, bool value)
{
	return detail::c_guard([&] { self(handle).set_visible(value); });
}

knurbs_result knurbs_property_get_min(knurbs_property_t *handle, double *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).min();
	});
}

knurbs_result knurbs_property_get_max(knurbs_property_t *handle, double *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).max();
	});
}

knurbs_result knurbs_property_get_step(knurbs_property_t *handle, double *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).step();
	});
}

knurbs_result knurbs_property_add_item(knurbs_property_t *handle, const char *label, const char *value)
{
	return detail::c_guard([&] {
		self(handle).add_item(std::string(detail::c_text(label, "label")),
				      std::string(detail::c_text(value, "value")));
	});
}

knurbs_result knurbs_property_item_count(knurbs_property_t *handle, size_t *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).item_count();
	});
}

knurbs_result knurbs_property_get_group(knurbs_property_t *handle, knurbs_properties_t **out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).group());
	});
}

knurbs_result knurbs_property_click(knurbs_property_t *handle, bool *out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).click();
	});
}

} // extern "C"
