#include "knurbs/c/core.h"

#include <filesystem>
#include <string_view>
#include <vector>

#include "c-guard.hpp"
#include "knurbs/core.hpp"
#include "knurbs/error.hpp"
#include "knurbs/module.hpp"
#include "knurbs/platform.hpp"
#include "knurbs/ref.hpp"

using namespace knurbs;

extern "C" {

knurbs_result knurbs_core_create(const char *config_path_utf8, const char *cache_path_utf8, knurbs_core_t **out)
{
	if (out) {
		*out = nullptr;
	}
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

knurbs_result knurbs_core_load_modules(knurbs_core_t *handle, const char *const *search_dirs_utf8, size_t count)
{
	return detail::c_guard([&] {
		core &c = detail::c_self<core>(handle, "core");
		if (count && !search_dirs_utf8) {
			detail::c_null_argument("search_dirs_utf8");
		}
		std::vector<std::filesystem::path> dirs;
		dirs.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			dirs.push_back(
				platform::path_from_utf8(detail::c_text(search_dirs_utf8[i], "search directory")));
		}
		c.modules().load(c.modules().discover(dirs));
	});
}

} // extern "C"
