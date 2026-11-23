#include "knurbs/c/core.h"

#include <string_view>

#include "c-guard.hpp"
#include "knurbs/core.hpp"
#include "knurbs/error.hpp"
#include "knurbs/platform.hpp"
#include "knurbs/ref.hpp"

using namespace knurbs;

extern "C" {

knurbs_result knurbs_core_create(const char *config_path_utf8, const char *cache_path_utf8, knurbs_core_t **out)
{
	return detail::c_guard([&] {
		if (!out || !config_path_utf8 || !cache_path_utf8) {
			throw error::invalid_argument(
				"knurbs_core_create: config path, cache path and out must not be NULL");
		}
		core::options opts;
		opts.config_path = platform::path_from_utf8(config_path_utf8);
		opts.cache_path = platform::path_from_utf8(cache_path_utf8);
		ref<core> created = core::create(std::move(opts));
		*out = detail::c_to_typed_handle<knurbs_core_t>(created.detach());
	});
}

void knurbs_core_shutdown(knurbs_core_t *handle)
{
	if (handle) {
		detail::c_from_typed_handle<core>(handle)->shutdown();
	}
}

knurbs_logger_t *knurbs_core_logger(knurbs_core_t *handle)
{
	knurbs_logger_t *result = nullptr;
	if (handle) {
		detail::c_guard([&] {
			result = detail::c_to_typed_handle<knurbs_logger_t>(
				detail::c_from_typed_handle<core>(handle)->logger().detach());
		});
	}
	return result;
}

} // extern "C"
