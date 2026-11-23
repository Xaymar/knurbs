#pragma once

/* KNURBS_C_API marks C symbols that are part of the shared library's public interface.
 * KNURBS_BUILDING is defined while libknurbs itself is compiled. */
#if defined(_WIN32)
#if defined(KNURBS_BUILDING)
#define KNURBS_C_API __declspec(dllexport)
#else
#define KNURBS_C_API __declspec(dllimport)
#endif
#else
#define KNURBS_C_API __attribute__((visibility("default")))
#endif
