#pragma once

#include <exception>
#include <new>
#include <string>
#include <utility>

#include "knurbs/c/object.h"
#include "knurbs/c/result.h"
#include "knurbs/error.hpp"
#include "knurbs/object.hpp"

namespace knurbs::detail {

// The message behind knurbs_last_error, one per thread.
inline std::string &c_last_error() noexcept
{
	thread_local std::string message;
	return message;
}

inline void c_set_last_error(const char *message) noexcept
{
	try {
		c_last_error() = message;
	} catch (...) {
		c_last_error().clear();
	}
}

// Runs `f`, which returns void. No exception leaves: it becomes the result and the last-error message.
template<class F> knurbs_result c_guard(F &&f) noexcept
{
	try {
		std::forward<F>(f)();
		return KNURBS_OK;
	} catch (const error::exception &e) {
		c_set_last_error(e.what());
		return static_cast<knurbs_result>(static_cast<int>(e.value()));
	} catch (const std::bad_alloc &) {
		c_set_last_error("out of memory");
		return KNURBS_ERROR_UNKNOWN;
	} catch (const std::exception &e) {
		c_set_last_error(e.what());
		return KNURBS_ERROR_UNKNOWN;
	} catch (...) {
		c_set_last_error("unknown error");
		return KNURBS_ERROR_UNKNOWN;
	}
}

// Handles are the address of the knurbs::object subobject. The conversions go through knurbs::object, so they stay
// correct if a class ever gains another base. The void * hop is only because the handle structs are opaque.
inline knurbs_object_t *c_to_handle(object *obj) noexcept
{
	return static_cast<knurbs_object_t *>(static_cast<void *>(obj));
}

inline object *c_from_handle(knurbs_object_t *handle) noexcept
{
	return static_cast<object *>(static_cast<void *>(handle));
}

// For the typed handles (knurbs_core_t, knurbs_logger_t): H is the handle struct, T the C++ class.
template<class H> H *c_to_typed_handle(object *obj) noexcept
{
	return static_cast<H *>(static_cast<void *>(obj));
}

template<class T, class H> T *c_from_typed_handle(H *handle) noexcept
{
	return static_cast<T *>(static_cast<object *>(static_cast<void *>(handle)));
}

} // namespace knurbs::detail
