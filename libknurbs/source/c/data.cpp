#include "knurbs/c/data.h"

#include <string>

#include "c-guard.hpp"
#include "knurbs/data.hpp"
#include "knurbs/error.hpp"
#include "knurbs/ref.hpp"

using namespace knurbs;

namespace {
	data& self(knurbs_data_t* handle)
	{
		return detail::c_self<data>(handle, "data");
	}

	data_array& self(knurbs_data_array_t* handle)
	{
		return detail::c_self<data_array>(handle, "array");
	}

	const data& self(const knurbs_data_t* handle)
	{
		return detail::c_self<data>(handle, "data");
	}

	const data_array& self(const knurbs_data_array_t* handle)
	{
		return detail::c_self<data_array>(handle, "array");
	}

	ref<data> retain(knurbs_data_t* handle)
	{
		return ref<data>::retain(handle ? detail::c_from_typed_handle<data>(handle) : nullptr);
	}

	ref<data_array> retain(knurbs_data_array_t* handle)
	{
		return ref<data_array>::retain(handle ? detail::c_from_typed_handle<data_array>(handle) : nullptr);
	}

	knurbs_data_t* give(ref<data> value) noexcept
	{
		return detail::c_to_typed_handle<knurbs_data_t>(value.detach());
	}

	knurbs_data_array_t* give(ref<data_array> value) noexcept
	{
		return detail::c_to_typed_handle<knurbs_data_array_t>(value.detach());
	}
} // namespace

extern "C" {

knurbs_result knurbs_data_create(knurbs_data_t** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(data::create());
	});
}

knurbs_result knurbs_data_from_json(const char* json, knurbs_data_t** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(data::from_json(detail::c_text(json, "json")));
	});
}

knurbs_result knurbs_data_to_json(const knurbs_data_t* handle, bool pretty, char** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = detail::c_dup_string(self(handle).to_json(pretty));
	});
}

knurbs_result knurbs_data_clone(const knurbs_data_t* handle, knurbs_data_t** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).clone());
	});
}

knurbs_result knurbs_data_has(const knurbs_data_t* handle, const char* key, bool* out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).has(detail::c_text(key, "key"));
	});
}

knurbs_result knurbs_data_has_user_value(const knurbs_data_t* handle, const char* key, bool* out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).has_user_value(detail::c_text(key, "key"));
	});
}

knurbs_result knurbs_data_erase(knurbs_data_t* handle, const char* key)
{
	return detail::c_guard([&] { self(handle).erase(detail::c_text(key, "key")); });
}

knurbs_result knurbs_data_keys(const knurbs_data_t* handle, char*** out_keys, size_t* out_count)
{
	return detail::c_guard([&] {
		detail::c_out(out_keys, "out_keys");
		detail::c_out(out_count, "out_count");
		detail::c_dup_strings(self(handle).keys(), out_keys, out_count);
	});
}

knurbs_result knurbs_data_get_bool(const knurbs_data_t* handle, const char* key, bool* out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).get_bool(detail::c_text(key, "key"));
	});
}

knurbs_result knurbs_data_get_int(const knurbs_data_t* handle, const char* key, int64_t* out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).get_int(detail::c_text(key, "key"));
	});
}

knurbs_result knurbs_data_get_double(const knurbs_data_t* handle, const char* key, double* out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).get_double(detail::c_text(key, "key"));
	});
}

knurbs_result knurbs_data_get_string(const knurbs_data_t* handle, const char* key, char** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = detail::c_dup_string(self(handle).get_string(detail::c_text(key, "key")));
	});
}

knurbs_result knurbs_data_get_object(const knurbs_data_t* handle, const char* key, knurbs_data_t** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).get_object(detail::c_text(key, "key")));
	});
}

knurbs_result knurbs_data_get_array(const knurbs_data_t* handle, const char* key, knurbs_data_array_t** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).get_array(detail::c_text(key, "key")));
	});
}

knurbs_result knurbs_data_set_bool(knurbs_data_t* handle, const char* key, bool value)
{
	return detail::c_guard([&] { self(handle).set_bool(detail::c_text(key, "key"), value); });
}

knurbs_result knurbs_data_set_int(knurbs_data_t* handle, const char* key, int64_t value)
{
	return detail::c_guard([&] { self(handle).set_int(detail::c_text(key, "key"), value); });
}

knurbs_result knurbs_data_set_double(knurbs_data_t* handle, const char* key, double value)
{
	return detail::c_guard([&] { self(handle).set_double(detail::c_text(key, "key"), value); });
}

knurbs_result knurbs_data_set_string(knurbs_data_t* handle, const char* key, const char* value)
{
	return detail::c_guard([&] { self(handle).set_string(detail::c_text(key, "key"), detail::c_text(value, "value")); });
}

knurbs_result knurbs_data_set_object(knurbs_data_t* handle, const char* key, knurbs_data_t* value)
{
	return detail::c_guard([&] { self(handle).set_object(detail::c_text(key, "key"), retain(value)); });
}

knurbs_result knurbs_data_set_array(knurbs_data_t* handle, const char* key, knurbs_data_array_t* value)
{
	return detail::c_guard([&] { self(handle).set_array(detail::c_text(key, "key"), retain(value)); });
}

knurbs_result knurbs_data_set_default_bool(knurbs_data_t* handle, const char* key, bool value)
{
	return detail::c_guard([&] { self(handle).set_default_bool(detail::c_text(key, "key"), value); });
}

knurbs_result knurbs_data_set_default_int(knurbs_data_t* handle, const char* key, int64_t value)
{
	return detail::c_guard([&] { self(handle).set_default_int(detail::c_text(key, "key"), value); });
}

knurbs_result knurbs_data_set_default_double(knurbs_data_t* handle, const char* key, double value)
{
	return detail::c_guard([&] { self(handle).set_default_double(detail::c_text(key, "key"), value); });
}

knurbs_result knurbs_data_set_default_string(knurbs_data_t* handle, const char* key, const char* value)
{
	return detail::c_guard([&] { self(handle).set_default_string(detail::c_text(key, "key"), detail::c_text(value, "value")); });
}

knurbs_result knurbs_data_set_default_object(knurbs_data_t* handle, const char* key, knurbs_data_t* value)
{
	return detail::c_guard([&] { self(handle).set_default_object(detail::c_text(key, "key"), retain(value)); });
}

knurbs_result knurbs_data_set_default_array(knurbs_data_t* handle, const char* key, knurbs_data_array_t* value)
{
	return detail::c_guard([&] { self(handle).set_default_array(detail::c_text(key, "key"), retain(value)); });
}

knurbs_result knurbs_data_array_create(knurbs_data_array_t** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(data_array::create());
	});
}

knurbs_result knurbs_data_array_size(const knurbs_data_array_t* handle, size_t* out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = self(handle).size();
	});
}

knurbs_result knurbs_data_array_at(const knurbs_data_array_t* handle, size_t index, knurbs_data_t** out)
{
	return detail::c_guard([&] {
		detail::c_out(out);
		*out = give(self(handle).at(index));
	});
}

knurbs_result knurbs_data_array_push_back(knurbs_data_array_t* handle, knurbs_data_t* value)
{
	return detail::c_guard([&] { self(handle).push_back(retain(value)); });
}

knurbs_result knurbs_data_array_erase(knurbs_data_array_t* handle, size_t index)
{
	return detail::c_guard([&] { self(handle).erase(index); });
}

} // extern "C"
