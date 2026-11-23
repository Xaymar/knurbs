#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "knurbs/c/export.h"
#include "knurbs/c/object.h"
#include "knurbs/c/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A settings tree (knurbs::data) and a list of such trees (knurbs::data_array). Both are objects: cast the handle
 * to knurbs_object_t * for knurbs_ref and knurbs_release. The semantics of every function are those of the C++
 * member of the same name in knurbs/data.hpp, which this header only repeats where the C API adds something.
 *
 * Conventions for all functions of the C API for data and properties:
 *  - Every function that has an out parameter sets *out to NULL, 0 or false before anything else, so it is never
 *    left unset after a failure. A NULL out parameter, handle, key or string is KNURBS_ERROR_INVALID_ARGUMENT.
 *  - Strings are UTF-8 and NUL-terminated. A string that comes out is a new copy, freed with knurbs_free.
 *  - An object that comes out is a new reference, released with knurbs_release.
 *  - An object that goes in (set_object, push_back, ...) gets a new reference from the library; the caller keeps
 *    and later releases its own.
 *  - Errors are those of the C++ member: a missing key is KNURBS_ERROR_NOT_FOUND, a bad key or a value of another
 *    type is KNURBS_ERROR_INVALID_ARGUMENT, and so on. knurbs_last_error has the message. */
typedef struct knurbs_data knurbs_data_t;
typedef struct knurbs_data_array knurbs_data_array_t;

/* Creates an empty tree; *out has one reference. */
KNURBS_C_API knurbs_result knurbs_data_create(knurbs_data_t **out);
/* Parses a JSON object (NUL-terminated UTF-8) into a new tree. */
KNURBS_C_API knurbs_result knurbs_data_from_json(const char *json, knurbs_data_t **out);
/* Writes the user values as JSON; *out is freed with knurbs_free. */
KNURBS_C_API knurbs_result knurbs_data_to_json(knurbs_data_t *data, bool pretty, char **out);
/* A deep copy; *out has one reference. */
KNURBS_C_API knurbs_result knurbs_data_clone(knurbs_data_t *data, knurbs_data_t **out);

KNURBS_C_API knurbs_result knurbs_data_has(knurbs_data_t *data, const char *key, bool *out);
KNURBS_C_API knurbs_result knurbs_data_has_user_value(knurbs_data_t *data, const char *key, bool *out);
KNURBS_C_API knurbs_result knurbs_data_erase(knurbs_data_t *data, const char *key);
/* Returns the sorted keys as an array of *out_count strings. The array and the strings are one block: free it with
 * a single knurbs_free(*out_keys), and do not free the strings separately. *out_keys is NULL if *out_count is 0. */
KNURBS_C_API knurbs_result knurbs_data_keys(knurbs_data_t *data, char ***out_keys, size_t *out_count);

KNURBS_C_API knurbs_result knurbs_data_get_bool(knurbs_data_t *data, const char *key, bool *out);
KNURBS_C_API knurbs_result knurbs_data_get_int(knurbs_data_t *data, const char *key, int64_t *out);
KNURBS_C_API knurbs_result knurbs_data_get_double(knurbs_data_t *data, const char *key, double *out);
/* *out is freed with knurbs_free. */
KNURBS_C_API knurbs_result knurbs_data_get_string(knurbs_data_t *data, const char *key, char **out);
KNURBS_C_API knurbs_result knurbs_data_get_object(knurbs_data_t *data, const char *key, knurbs_data_t **out);
KNURBS_C_API knurbs_result knurbs_data_get_array(knurbs_data_t *data, const char *key, knurbs_data_array_t **out);

KNURBS_C_API knurbs_result knurbs_data_set_bool(knurbs_data_t *data, const char *key, bool value);
KNURBS_C_API knurbs_result knurbs_data_set_int(knurbs_data_t *data, const char *key, int64_t value);
KNURBS_C_API knurbs_result knurbs_data_set_double(knurbs_data_t *data, const char *key, double value);
KNURBS_C_API knurbs_result knurbs_data_set_string(knurbs_data_t *data, const char *key, const char *value);
KNURBS_C_API knurbs_result knurbs_data_set_object(knurbs_data_t *data, const char *key, knurbs_data_t *value);
KNURBS_C_API knurbs_result knurbs_data_set_array(knurbs_data_t *data, const char *key, knurbs_data_array_t *value);

KNURBS_C_API knurbs_result knurbs_data_set_default_bool(knurbs_data_t *data, const char *key, bool value);
KNURBS_C_API knurbs_result knurbs_data_set_default_int(knurbs_data_t *data, const char *key, int64_t value);
KNURBS_C_API knurbs_result knurbs_data_set_default_double(knurbs_data_t *data, const char *key, double value);
KNURBS_C_API knurbs_result knurbs_data_set_default_string(knurbs_data_t *data, const char *key, const char *value);
KNURBS_C_API knurbs_result knurbs_data_set_default_object(knurbs_data_t *data, const char *key, knurbs_data_t *value);
KNURBS_C_API knurbs_result knurbs_data_set_default_array(knurbs_data_t *data, const char *key,
							 knurbs_data_array_t *value);

/* Creates an empty list; *out has one reference. */
KNURBS_C_API knurbs_result knurbs_data_array_create(knurbs_data_array_t **out);
KNURBS_C_API knurbs_result knurbs_data_array_size(knurbs_data_array_t *array, size_t *out);
/* KNURBS_ERROR_OUT_OF_RANGE for index >= size. */
KNURBS_C_API knurbs_result knurbs_data_array_at(knurbs_data_array_t *array, size_t index, knurbs_data_t **out);
KNURBS_C_API knurbs_result knurbs_data_array_push_back(knurbs_data_array_t *array, knurbs_data_t *value);
KNURBS_C_API knurbs_result knurbs_data_array_erase(knurbs_data_array_t *array, size_t index);

#ifdef __cplusplus
}
#endif
