#pragma once

#include "knurbs/c/export.h"
#include "knurbs/c/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct knurbs_logger knurbs_logger_t;

/* Values of the `level` argument of knurbs_log_write; they match knurbs::log::level. */
#define KNURBS_LOG_DEBUG 0
#define KNURBS_LOG_INFO 1
#define KNURBS_LOG_WARNING 2
#define KNURBS_LOG_ERROR 3

/* Writes a UTF-8 message. KNURBS_ERROR_INVALID_ARGUMENT for a NULL logger or message, or an unknown level. */
KNURBS_C_API knurbs_result knurbs_log_write(knurbs_logger_t *logger, int level, const char *utf8);

#ifdef __cplusplus
}
#endif
