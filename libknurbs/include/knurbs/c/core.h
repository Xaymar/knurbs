#pragma once

#include "knurbs/c/export.h"
#include "knurbs/c/log.h"
#include "knurbs/c/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct knurbs_core knurbs_core_t;

/* Starts a core. On success *out holds it with one reference, which the caller releases with knurbs_release
 * (after knurbs_core_shutdown, if shutting down explicitly). KNURBS_ERROR_INVALID_ARGUMENT for a NULL or empty
 * path, or a NULL `out`. */
KNURBS_C_API knurbs_result knurbs_core_create(const char *config_path_utf8, const char *cache_path_utf8,
					      knurbs_core_t **out);
/* Stops the core; idempotent. NULL is ignored. The handle stays valid until released. */
KNURBS_C_API void knurbs_core_shutdown(knurbs_core_t *core);
/* Returns the core's logger with a new reference, or NULL for NULL (or on failure). */
KNURBS_C_API knurbs_logger_t *knurbs_core_logger(knurbs_core_t *core);

#ifdef __cplusplus
}
#endif
