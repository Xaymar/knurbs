#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "knurbs/c/data.h"
#include "knurbs/c/export.h"
#include "knurbs/c/object.h"
#include "knurbs/c/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A properties list (knurbs::properties) and one entry of it (knurbs::property). Both are objects: cast the handle
 * to knurbs_object_t * for knurbs_ref and knurbs_release. The semantics of every function are those of the C++
 * member of the same name in knurbs/properties.hpp. The conventions listed in knurbs/c/data.h apply, too. */
typedef struct knurbs_properties knurbs_properties_t;
typedef struct knurbs_property knurbs_property_t;

/* Values of knurbs::property_kind; only ever appended. */
typedef enum knurbs_property_kind {
	KNURBS_PROPERTY_BOOLEAN = 0,
	KNURBS_PROPERTY_INTEGER = 1,
	KNURBS_PROPERTY_FLOATING = 2,
	KNURBS_PROPERTY_TEXT = 3,
	KNURBS_PROPERTY_PATH = 4,
	KNURBS_PROPERTY_LIST = 5,
	KNURBS_PROPERTY_COLOR = 6,
	KNURBS_PROPERTY_BUTTON = 7,
	KNURBS_PROPERTY_GROUP = 8,
	KNURBS_PROPERTY_NOTICE = 9
} knurbs_property_kind;

/* Callbacks. Every callback is passed with a `param` and a `free_param` (which may be NULL). The library owns
 * `param` from the moment of the call, whether or not the call succeeds: it calls free_param(param) once, when the
 * callback is replaced or its properties are destroyed (at once, if the call fails). The handles passed to a
 * callback do not own a reference and are only valid during the call: take one with knurbs_ref to keep them. A
 * callback runs on whichever thread calls knurbs_property_click or knurbs_properties_notify_modified, with no lock
 * held, and must not let an exception (C++) or longjmp escape. */
typedef bool (*knurbs_property_click_fn)(knurbs_properties_t *properties, knurbs_property_t *property, void *param);
typedef bool (*knurbs_property_modified_fn)(knurbs_properties_t *properties, knurbs_property_t *property,
					    knurbs_data_t *settings, void *param);

/* Creates an empty list; *out has one reference. */
KNURBS_C_API knurbs_result knurbs_properties_create(knurbs_properties_t **out);

/* Each add function returns the new property in *out, with one reference. */
KNURBS_C_API knurbs_result knurbs_properties_add_bool(knurbs_properties_t *properties, const char *id,
						      const char *label, knurbs_property_t **out);
KNURBS_C_API knurbs_result knurbs_properties_add_int(knurbs_properties_t *properties, const char *id, const char *label,
						     double min, double max, double step, knurbs_property_t **out);
KNURBS_C_API knurbs_result knurbs_properties_add_float(knurbs_properties_t *properties, const char *id,
						       const char *label, double min, double max, double step,
						       knurbs_property_t **out);
KNURBS_C_API knurbs_result knurbs_properties_add_text(knurbs_properties_t *properties, const char *id,
						      const char *label, knurbs_property_t **out);
KNURBS_C_API knurbs_result knurbs_properties_add_path(knurbs_properties_t *properties, const char *id,
						      const char *label, knurbs_property_t **out);
KNURBS_C_API knurbs_result knurbs_properties_add_list(knurbs_properties_t *properties, const char *id,
						      const char *label, knurbs_property_t **out);
KNURBS_C_API knurbs_result knurbs_properties_add_color(knurbs_properties_t *properties, const char *id,
						       const char *label, knurbs_property_t **out);
/* `on_click` must not be NULL. */
KNURBS_C_API knurbs_result knurbs_properties_add_button(knurbs_properties_t *properties, const char *id,
							const char *label, knurbs_property_click_fn on_click,
							void *param, void (*free_param)(void *),
							knurbs_property_t **out);
KNURBS_C_API knurbs_result knurbs_properties_add_group(knurbs_properties_t *properties, const char *id,
						       const char *label, knurbs_properties_t *group,
						       knurbs_property_t **out);
KNURBS_C_API knurbs_result knurbs_properties_add_notice(knurbs_properties_t *properties, const char *id,
							const char *text, knurbs_property_t **out);

/* *out is the property with this id, or NULL if there is none (which is KNURBS_OK). */
KNURBS_C_API knurbs_result knurbs_properties_find(knurbs_properties_t *properties, const char *id,
						  knurbs_property_t **out);
/* Returns the properties of this level as an array of *out_count new references. Release each element with
 * knurbs_release, then free the array with knurbs_free. *out_list is NULL if *out_count is 0. */
KNURBS_C_API knurbs_result knurbs_properties_list(knurbs_properties_t *properties, knurbs_property_t ***out_list,
						  size_t *out_count);

/* Sets the callback that knurbs_properties_notify_modified runs for this id. `callback` must not be NULL. */
KNURBS_C_API knurbs_result knurbs_properties_set_modified(knurbs_properties_t *properties, const char *id,
							  knurbs_property_modified_fn callback, void *param,
							  void (*free_param)(void *));
/* *out is the callback's result; false if there is no callback or no such property. */
KNURBS_C_API knurbs_result knurbs_properties_notify_modified(knurbs_properties_t *properties, const char *id,
							     knurbs_data_t *settings, bool *out);

/* The id and the label (or text of a notice) are copies, freed with knurbs_free. */
KNURBS_C_API knurbs_result knurbs_property_get_id(knurbs_property_t *property, char **out);
KNURBS_C_API knurbs_result knurbs_property_get_kind(knurbs_property_t *property, knurbs_property_kind *out);
KNURBS_C_API knurbs_result knurbs_property_get_label(knurbs_property_t *property, char **out);

KNURBS_C_API knurbs_result knurbs_property_get_enabled(knurbs_property_t *property, bool *out);
KNURBS_C_API knurbs_result knurbs_property_get_visible(knurbs_property_t *property, bool *out);
KNURBS_C_API knurbs_result knurbs_property_set_enabled(knurbs_property_t *property, bool value);
KNURBS_C_API knurbs_result knurbs_property_set_visible(knurbs_property_t *property, bool value);

/* Integer and floating properties; KNURBS_ERROR_INVALID_ARGUMENT for other kinds. */
KNURBS_C_API knurbs_result knurbs_property_get_min(knurbs_property_t *property, double *out);
KNURBS_C_API knurbs_result knurbs_property_get_max(knurbs_property_t *property, double *out);
KNURBS_C_API knurbs_result knurbs_property_get_step(knurbs_property_t *property, double *out);

/* List properties; KNURBS_ERROR_INVALID_ARGUMENT for other kinds. */
KNURBS_C_API knurbs_result knurbs_property_add_item(knurbs_property_t *property, const char *label, const char *value);
KNURBS_C_API knurbs_result knurbs_property_item_count(knurbs_property_t *property, size_t *out);

/* Group properties; KNURBS_ERROR_INVALID_ARGUMENT for other kinds. */
KNURBS_C_API knurbs_result knurbs_property_get_group(knurbs_property_t *property, knurbs_properties_t **out);

/* Button properties: *out is the callback's result, false if the owning properties is gone. */
KNURBS_C_API knurbs_result knurbs_property_click(knurbs_property_t *property, bool *out);

#ifdef __cplusplus
}
#endif
