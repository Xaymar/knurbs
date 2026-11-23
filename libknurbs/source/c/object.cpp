#include "knurbs/c/object.h"

#include "c-guard.hpp"
#include "knurbs/c/result.h"
#include "knurbs/error.hpp"
#include "knurbs/object.hpp"

using namespace knurbs;

#define KNURBS_CHECK_CODE(c_name, cpp_name) \
	static_assert(static_cast<int>(c_name) == static_cast<int>(error::code::cpp_name))

KNURBS_CHECK_CODE(KNURBS_OK, ok);
KNURBS_CHECK_CODE(KNURBS_ERROR_UNKNOWN, unknown);
KNURBS_CHECK_CODE(KNURBS_ERROR_INVALID_ARGUMENT, invalid_argument);
KNURBS_CHECK_CODE(KNURBS_ERROR_NOT_FOUND, not_found);
KNURBS_CHECK_CODE(KNURBS_ERROR_ALREADY_EXISTS, already_exists);
KNURBS_CHECK_CODE(KNURBS_ERROR_OUT_OF_RANGE, out_of_range);
KNURBS_CHECK_CODE(KNURBS_ERROR_UNSUPPORTED, unsupported);
KNURBS_CHECK_CODE(KNURBS_ERROR_IO, io);
KNURBS_CHECK_CODE(KNURBS_ERROR_CORE_GONE, core_gone);
KNURBS_CHECK_CODE(KNURBS_ERROR_GPU_LOST, gpu_lost);
KNURBS_CHECK_CODE(KNURBS_ERROR_PLUGIN_FAILED, plugin_failed);
KNURBS_CHECK_CODE(KNURBS_ERROR_VERSION_MISMATCH, version_mismatch);

#undef KNURBS_CHECK_CODE

extern "C" {

const char *knurbs_last_error(void)
{
	return detail::c_last_error().c_str();
}

void knurbs_ref(knurbs_object_t *object)
{
	if (object) {
		detail::c_from_handle(object)->acquire();
	}
}

void knurbs_release(knurbs_object_t *object)
{
	if (object) {
		detail::c_from_handle(object)->release();
	}
}

knurbs_weak_t *knurbs_weak_get(knurbs_object_t *object)
{
	knurbs_weak_t *result = nullptr;
	detail::c_guard([&] {
		detail::weak_block *w = detail::weak_get(detail::c_from_handle(object));
		result = static_cast<knurbs_weak_t *>(static_cast<void *>(w));
	});
	return result;
}

knurbs_object_t *knurbs_weak_lock(knurbs_weak_t *weak)
{
	auto *w = static_cast<detail::weak_block *>(static_cast<void *>(weak));
	return detail::c_to_handle(detail::weak_lock(w));
}

void knurbs_weak_release(knurbs_weak_t *weak)
{
	detail::weak_release(static_cast<detail::weak_block *>(static_cast<void *>(weak)));
}

} // extern "C"
