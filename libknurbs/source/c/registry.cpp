#include "knurbs/c/registry.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "c-guard.hpp"
#include "knurbs/core.hpp"
#include "knurbs/data.hpp"
#include "knurbs/error.hpp"
#include "knurbs/module.hpp"
#include "knurbs/properties.hpp"
#include "knurbs/ref.hpp"
#include "knurbs/registry.hpp"

using namespace knurbs;

#define KNURBS_CHECK_KIND(c_name, cpp_name) \
	static_assert(static_cast<int>(c_name) == static_cast<int>(type_kind::cpp_name))

KNURBS_CHECK_KIND(KNURBS_TYPE_SOURCE, source);
KNURBS_CHECK_KIND(KNURBS_TYPE_FILTER, filter);
KNURBS_CHECK_KIND(KNURBS_TYPE_TRANSITION, transition);
KNURBS_CHECK_KIND(KNURBS_TYPE_ENCODER, encoder);
KNURBS_CHECK_KIND(KNURBS_TYPE_OUTPUT, output);
KNURBS_CHECK_KIND(KNURBS_TYPE_SERVICE, service);

#undef KNURBS_CHECK_KIND

namespace {

// The smallest knurbs_type_info this version accepts: every field up to and including free_param.
constexpr std::size_t min_info_size = offsetof(knurbs_type_info, free_param) + sizeof(knurbs_type_info::free_param);

constexpr std::uint32_t last_kind = KNURBS_TYPE_SERVICE;

// A type implemented in C: the core has one plugin model, knurbs::type, and this adapts a knurbs_type_info to it.
class c_type final : public type {
	// First, so it is destroyed last: free_param runs once the rest is gone.
	const detail::c_callback_param param_;
	const std::string id_;
	const type_kind kind_;
	const std::uint32_t version_;
	void (*const defaults_)(void *, knurbs_data_t *);
	knurbs_properties_t *(*const properties_)(void *, const knurbs_data_t *);

public:
	c_type(const knurbs_type_info &info, detail::c_callback_param param)
		: param_(std::move(param)),
		  id_(info.id),
		  kind_(static_cast<type_kind>(info.kind)),
		  version_(info.version),
		  defaults_(info.defaults),
		  properties_(info.properties)
	{
	}

	std::string_view id() const noexcept override { return id_; }
	type_kind kind() const noexcept override { return kind_; }
	std::uint32_t version() const noexcept override { return version_; }

	ref<knurbs::properties> properties(const data &settings) const override
	{
		if (!properties_) {
			return type::properties(settings);
		}
		auto *handle = detail::c_to_typed_handle<const knurbs_data_t>(const_cast<data *>(&settings));
		knurbs_properties_t *result = properties_(param_.get(), handle);
		if (!result) {
			return knurbs::properties::create();
		}
		return ref<knurbs::properties>::adopt(detail::c_from_typed_handle<knurbs::properties>(result));
	}

	void defaults(data &settings) const override
	{
		if (defaults_) {
			defaults_(param_.get(), detail::c_to_typed_handle<knurbs_data_t>(&settings));
		}
	}
};

module &self(knurbs_module_t *handle)
{
	return detail::c_self<module>(handle, "module");
}

} // namespace

extern "C" {

knurbs_result knurbs_registry_add(knurbs_module_t *handle, const struct knurbs_type_info *info)
{
	return detail::c_guard([&] {
		if (!info) {
			detail::c_null_argument("info");
		}
		if (info->size < min_info_size) {
			throw error::invalid_argument("knurbs_registry_add: info->size is too small");
		}
		// Only the first `size` bytes are read; the fields of later versions are ignored.
		knurbs_type_info known{};
		std::memcpy(&known, info, info->size < sizeof(known) ? info->size : sizeof(known));
		// From here on the library owns param: every failure below frees it at once.
		detail::c_callback_param param(known.param, known.free_param);

		module &m = self(handle);
		if (!known.id) {
			detail::c_null_argument("info->id");
		}
		if (known.kind > last_kind) {
			throw error::invalid_argument("knurbs_registry_add: unknown kind " +
						      std::to_string(known.kind));
		}
		// Ours is the last reference if add fails, so free_param runs here, outside the registry's lock.
		ref<type> t = make<c_type>(known, std::move(param));
		m.registry().add(t, m.name());
	});
}

knurbs_result knurbs_registry_add_alias(knurbs_module_t *handle, const char *old_id, const char *new_id)
{
	return detail::c_guard([&] {
		module &m = self(handle);
		m.registry().add_alias(detail::c_text(old_id, "old_id"), detail::c_text(new_id, "new_id"), m.name());
	});
}

knurbs_type_t *knurbs_registry_find(knurbs_core_t *handle, const char *id)
{
	knurbs_type_t *result = nullptr;
	detail::c_guard([&] {
		ref<type> t = detail::c_self<core>(handle, "core").registry().find(detail::c_text(id, "id"));
		result = detail::c_to_typed_handle<knurbs_type_t>(t.detach());
	});
	return result;
}

knurbs_result knurbs_type_defaults(knurbs_type_t *handle, knurbs_data_t *settings)
{
	return detail::c_guard([&] {
		type &t = detail::c_self<type>(handle, "type");
		t.defaults(detail::c_self<data>(settings, "settings"));
	});
}

} // extern "C"
