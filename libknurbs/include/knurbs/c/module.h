#pragma once

#include <stdint.h>

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
 * or (for C++ modules) of another api_major than the core's.
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
