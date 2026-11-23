#include "knurbs/c/log.h"

#include "c-guard.hpp"
#include "knurbs/error.hpp"
#include "knurbs/log.hpp"

using namespace knurbs;

static_assert(KNURBS_LOG_DEBUG == static_cast<int>(log::level::debug));
static_assert(KNURBS_LOG_INFO == static_cast<int>(log::level::info));
static_assert(KNURBS_LOG_WARNING == static_cast<int>(log::level::warning));
static_assert(KNURBS_LOG_ERROR == static_cast<int>(log::level::error));

extern "C" {

knurbs_result knurbs_log_write(knurbs_logger_t *handle, int level, const char *utf8)
{
	return detail::c_guard([&] {
		if (!handle || !utf8) {
			throw error::invalid_argument("knurbs_log_write: logger and message must not be NULL");
		}
		if (level < KNURBS_LOG_DEBUG || level > KNURBS_LOG_ERROR) {
			throw error::invalid_argument("knurbs_log_write: unknown level");
		}
		detail::c_from_typed_handle<log::logger>(handle)->write(static_cast<log::level>(level), utf8);
	});
}

} // extern "C"
