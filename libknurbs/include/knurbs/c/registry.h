#pragma once

#include <stdint.h>

#include "knurbs/c/core.h"
#include "knurbs/c/data.h"
#include "knurbs/c/export.h"
#include "knurbs/c/module.h"
#include "knurbs/c/object.h"
#include "knurbs/c/properties.h"
#include "knurbs/c/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A registered type (knurbs::type). It is an object: cast the handle to knurbs_object_t * for knurbs_ref and
 * knurbs_release. The semantics are those of knurbs::registry and knurbs::type in knurbs/registry.hpp. The
 * conventions listed in knurbs/c/data.h apply, too. */
typedef struct knurbs_type knurbs_type_t;

/* Values of knurbs::type_kind, for knurbs_type_info.kind; only ever appended. */
typedef enum knurbs_type_kind {
	KNURBS_TYPE_SOURCE = 0,
	KNURBS_TYPE_FILTER = 1,
	KNURBS_TYPE_TRANSITION = 2,
	KNURBS_TYPE_ENCODER = 3,
	KNURBS_TYPE_OUTPUT = 4,
	KNURBS_TYPE_SERVICE = 5
} knurbs_type_kind;

/* A type implemented in C. Fields are only ever appended; the library reads only the first `size` bytes, and
 * needs at least every field up to and including `free_param`.
 *
 * The callbacks may run on any thread, also on several at once, with no lock of the library held, and must not let
 * an exception (C++) or longjmp escape. The handles they get do not own a reference and are only valid during the
 * call: take one with knurbs_ref to keep them. */
struct knurbs_type_info {
	/* sizeof(struct knurbs_type_info) as the caller was built. */
	uint32_t size;
	/* A valid type ID (reverse-DNS, see knurbs::naming::is_valid_type_id). Copied: it need not outlive the call. */
	const char *id;
	/* A knurbs_type_kind. */
	uint32_t kind;
	/* The version of the type, knurbs::type::version(). */
	uint32_t version;
	/* Fills in the default values of the settings. May be NULL: no defaults. */
	void (*defaults)(void *param, knurbs_data_t *settings);
	/* Describes the settings of an object of this type. Returns an owned reference, which the library takes over,
	 * or NULL for no properties (an empty list). The settings are only read: the data getters take a non-const
	 * handle, so cast the const away for them, but never change the settings. May be NULL: no properties. */
	knurbs_properties_t *(*properties)(void *param, const knurbs_data_t *settings);
	/* Passed to every callback. */
	void *param;
	/* Called once with `param` when the type is destroyed. May be NULL. */
	void (*free_param)(void *param);
};

/* Registers a type, owned by `module` (registry::add with module's name as owner_module: knurbs::registry::
 * remove_module drops it, also when it was added outside of the module's entry points).
 *
 * The library owns `param` from the call on, whether or not the call succeeds: free_param(param) runs at once if the
 * call fails, and otherwise when the type is destroyed (once the registry and everyone else released it), never under
 * an internal lock of the library. The exception is a call whose `info` is NULL or whose `size` is too small, as
 * free_param cannot be read then; that call is KNURBS_ERROR_INVALID_ARGUMENT and leaves `param` with the caller.
 *
 * KNURBS_ERROR_INVALID_ARGUMENT also for a NULL module or id, an unknown kind, or an id that is not a valid type ID;
 * KNURBS_ERROR_ALREADY_EXISTS when the id is already registered or is the old ID of an alias. */
KNURBS_C_API knurbs_result knurbs_registry_add(knurbs_module_t *module, const struct knurbs_type_info *info);

/* Makes `old_id` find the type `new_id` (knurbs::registry::add_alias), owned by `module` as for
 * knurbs_registry_add. */
KNURBS_C_API knurbs_result knurbs_registry_add_alias(knurbs_module_t *module, const char *old_id, const char *new_id);

/* The type with this ID, following aliases, with a new reference; NULL if there is none, and for a NULL core or id,
 * or on failure (such as a core that is shutting down; knurbs_last_error has the message then). */
KNURBS_C_API knurbs_type_t *knurbs_registry_find(knurbs_core_t *core, const char *id);

/* Fills in the type's default values of `settings` (knurbs::type::defaults). */
KNURBS_C_API knurbs_result knurbs_type_defaults(knurbs_type_t *type, knurbs_data_t *settings);

#ifdef __cplusplus
}
#endif
