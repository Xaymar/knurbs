#pragma once

#include <stddef.h>
#include <stdint.h>

#include "knurbs/c/core.h"
#include "knurbs/c/export.h"
#include "knurbs/c/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What kind of API a module is written against: knurbs_module_header.api_kind. */
#define KNURBS_MODULE_API_C 1
#define KNURBS_MODULE_API_CPP 2

/* Marks a module's entry points (knurbs_module_header, knurbs_module_load, ...) for export from its library. */
#if defined(_WIN32)
#define KNURBS_MODULE_EXPORT __declspec(dllexport)
#else
#define KNURBS_MODULE_EXPORT __attribute__((visibility("default")))
#endif

/* Every module exports
 *     const struct knurbs_module_header *knurbs_module_header(void);
 * (extern "C" in C++), returning a header that lives as long as the library. The loader calls it before any other
 * entry point, and rejects the module if the header is missing, smaller than this struct, of an unknown api_kind,
 * or of another api_major than the core's (for C and C++ modules alike).
 *
 * Fields are only ever appended; the loader reads only the first `size` bytes it knows. */
struct knurbs_module_header {
	/* sizeof(struct knurbs_module_header) as the module was built. */
	uint32_t size;
	/* KNURBS_MODULE_API_C or KNURBS_MODULE_API_CPP. */
	uint32_t api_kind;
	/* The major version of the API the module was built against. */
	uint32_t api_major;
	/* The minor version of the API the module was built against. Informational. */
	uint32_t api_version;
};

/* The version of the API that these headers describe: the C spelling of knurbs::version::major and minor. */
#define KNURBS_API_VERSION_MAJOR 0
#define KNURBS_API_VERSION_MINOR 1

/* A loaded module (knurbs::module), as its C entry points see it. It is an object (see knurbs/c/object.h), owned by
 * the core: an entry point gets it without a reference of its own, valid for as long as the core.
 *
 * A C module uses KNURBS_MODULE_C() once and defines its entry points as
 *     KNURBS_MODULE_EXPORT void knurbs_module_load(knurbs_module_t *m);       (required)
 *     KNURBS_MODULE_EXPORT void knurbs_module_post_load(knurbs_module_t *m);  (optional)
 *     KNURBS_MODULE_EXPORT void knurbs_module_unload(knurbs_module_t *m);     (optional)
 * They run when and on the threads that knurbs/module.hpp describes for C++ modules. A C entry point cannot fail the
 * module: what it does with the result of a failed call is up to it. It must not let a longjmp escape. */
typedef struct knurbs_module knurbs_module_t;

/* Returns the core that loaded the module, with a new reference, or NULL for NULL (or on failure). */
KNURBS_C_API knurbs_core_t *knurbs_module_core(knurbs_module_t *module);

/* Finds the modules in `search_dirs_utf8` (an array of `count` UTF-8 paths) and loads them, as
 * knurbs::module_manager::discover followed by load. A module that fails is logged and skipped; that is not an
 * error of this call. KNURBS_ERROR_INVALID_ARGUMENT for a NULL core, or a NULL array or path when count is not 0. */
KNURBS_C_API knurbs_result knurbs_core_load_modules(knurbs_core_t *core, const char *const *search_dirs_utf8,
						    size_t count);

#ifdef __cplusplus
}
#endif

/* Defines the knurbs_module_header of a C module, for the version of the headers it is compiled against. Use it
 * once, at file scope, in one source file of the module (C++ modules use KNURBS_MODULE_CPP from knurbs/module.hpp).
 */
#define KNURBS_MODULE_C()                                                                             \
	KNURBS_MODULE_EXPORT const struct knurbs_module_header *knurbs_module_header(void);           \
	KNURBS_MODULE_EXPORT const struct knurbs_module_header *knurbs_module_header(void)            \
	{                                                                                             \
		static const struct knurbs_module_header knurbs_header = {                            \
			sizeof(struct knurbs_module_header),                                          \
			KNURBS_MODULE_API_C,                                                          \
			KNURBS_API_VERSION_MAJOR,                                                     \
			KNURBS_API_VERSION_MINOR,                                                     \
		};                                                                                    \
		return &knurbs_header;                                                                \
	}
