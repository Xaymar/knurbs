#pragma once

#include "knurbs/c/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of a C API call. The values are numerically identical to knurbs::error::code and are only ever appended. */
typedef enum knurbs_result {
	KNURBS_OK = 0,
	KNURBS_ERROR_UNKNOWN = 1,
	KNURBS_ERROR_INVALID_ARGUMENT = 2,
	KNURBS_ERROR_NOT_FOUND = 3,
	KNURBS_ERROR_ALREADY_EXISTS = 4,
	KNURBS_ERROR_OUT_OF_RANGE = 5,
	KNURBS_ERROR_UNSUPPORTED = 6,
	KNURBS_ERROR_IO = 7,
	KNURBS_ERROR_CORE_GONE = 8,
	KNURBS_ERROR_GPU_LOST = 9,
	KNURBS_ERROR_PLUGIN_FAILED = 10,
	KNURBS_ERROR_VERSION_MISMATCH = 11
} knurbs_result;

/* The message of the last call on this thread that failed, or "" if none has. The pointer stays valid until the
 * next C API call on the same thread. A successful call does not clear the message: check the result of the call
 * first, and read this only after a failure. Never returns NULL. No C API function lets an exception escape. */
KNURBS_C_API const char *knurbs_last_error(void);

#ifdef __cplusplus
}
#endif
