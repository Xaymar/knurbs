#include "knurbs/c/module.h"

#include <filesystem>
#include <vector>

#include "c-guard.hpp"
#include "knurbs/core.hpp"
#include "knurbs/module.hpp"
#include "knurbs/platform.hpp"
#include "knurbs/ref.hpp"
#include "knurbs/version.hpp"

using namespace knurbs;

static_assert(KNURBS_API_VERSION_MAJOR == version::major);
static_assert(KNURBS_API_VERSION_MINOR == version::minor);

extern "C" {

knurbs_core_t *knurbs_module_core(knurbs_module_t *handle)
{
	knurbs_core_t *result = nullptr;
	if (handle) {
		// The module does not keep its core alive, but the core outlives every module handle.
		core &c = detail::c_from_typed_handle<module>(handle)->core();
		result = detail::c_to_typed_handle<knurbs_core_t>(ref<core>::retain(&c).detach());
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
