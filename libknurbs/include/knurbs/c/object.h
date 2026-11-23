#pragma once

#include "knurbs/c/export.h"
#include "knurbs/c/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles. A handle is the address of the C++ knurbs::object; every handle type of the C API (knurbs_core_t,
 * knurbs_logger_t, ...) can be converted to knurbs_object_t * with a C cast and passed to knurbs_ref and
 * knurbs_release. */
typedef struct knurbs_object knurbs_object_t;
typedef struct knurbs_weak knurbs_weak_t;

/* Takes a new reference. NULL is ignored. */
KNURBS_C_API void knurbs_ref(knurbs_object_t *object);
/* Drops a reference; the last one destroys the object. NULL is ignored. */
KNURBS_C_API void knurbs_release(knurbs_object_t *object);

/* Returns a new weak reference to `object`, or NULL on failure or for NULL. Free it with knurbs_weak_release. */
KNURBS_C_API knurbs_weak_t *knurbs_weak_get(knurbs_object_t *object);
/* Returns the object with a new strong reference, or NULL once its last strong reference is gone (or for NULL). */
KNURBS_C_API knurbs_object_t *knurbs_weak_lock(knurbs_weak_t *weak);
/* Frees a weak reference. NULL is ignored. */
KNURBS_C_API void knurbs_weak_release(knurbs_weak_t *weak);

/* Frees memory the C API allocated and handed to the caller: strings (knurbs_data_get_string, ...) and arrays
 * (knurbs_data_keys, ...). Every function that returns such memory says so. NULL is ignored. Never pass it memory
 * from anywhere else, and never release handles with it (use knurbs_release). */
KNURBS_C_API void knurbs_free(void *memory);

#ifdef __cplusplus
}
#endif
