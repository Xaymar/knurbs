#include "knurbs/c/module.h"

#include "c-guard.hpp"
#include "knurbs/core.hpp"
#include "knurbs/module.hpp"
#include "knurbs/version.hpp"

using namespace knurbs;

static_assert(KNURBS_API_VERSION_MAJOR == version::major);
static_assert(KNURBS_API_VERSION_MINOR == version::minor);

extern "C" {

knurbs_core_t *knurbs_module_core(knurbs_module_t *handle)
{
	// Borrowed, like module::core(): the core outlives every module handle, and taking a reference here could
	// revive a core whose last release is shutting it down (unload runs then).
	return handle ? detail::c_to_typed_handle<knurbs_core_t>(&detail::c_from_typed_handle<module>(handle)->core())
		      : nullptr;
}

} // extern "C"
