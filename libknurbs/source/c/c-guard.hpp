#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Argument checks and memory for the C API. Everything handed to the caller as memory comes from c_malloc and is
// freed with knurbs_free.

[[noreturn]] inline void c_null_argument(const char *what)
{
	throw error::invalid_argument(std::string(what) + " must not be NULL");
}

// The out parameter of a function: must not be NULL, and is cleared at once, so a caller never reads garbage after
// a failure. Call it before anything else that can throw.
template<class T> T &c_out(T *out, const char *what = "out")
{
	if (!out) {
		c_null_argument(what);
	}
	*out = T{};
	return *out;
}

template<class T, class H> T &c_self(H *handle, const char *what = "handle")
{
	if (!handle) {
		c_null_argument(what);
	}
	return *c_from_typed_handle<T>(handle);
}

inline std::string_view c_text(const char *text, const char *what)
{
	if (!text) {
		c_null_argument(what);
	}
	return text;
}

inline void *c_malloc(std::size_t size)
{
	void *memory = std::malloc(size ? size : 1);
	if (!memory) {
		throw std::bad_alloc();
	}
	return memory;
}

inline char *c_dup_string(std::string_view text)
{
	char *copy = static_cast<char *>(c_malloc(text.size() + 1));
	std::memcpy(copy, text.data(), text.size());
	copy[text.size()] = '\0';
	return copy;
}

// One block: `count` char pointers followed by the NUL-terminated strings they point to. NULL for no strings.
inline void c_dup_strings(const std::vector<std::string> &strings, char ***out, std::size_t *out_count)
{
	if (strings.empty()) {
		return;
	}
	// No overflow check: the sizes of keys of a settings tree are far from SIZE_MAX
	std::size_t size = strings.size() * sizeof(char *);
	for (const std::string &s : strings) {
		size += s.size() + 1;
	}
	char **array = static_cast<char **>(c_malloc(size));
	char *text = reinterpret_cast<char *>(array + strings.size());
	for (std::size_t i = 0; i < strings.size(); ++i) {
		array[i] = text;
		std::memcpy(text, strings[i].c_str(), strings[i].size() + 1);
		text += strings[i].size() + 1;
	}
	*out = array;
	*out_count = strings.size();
}

// Owns the `param` of a C callback: calls free_param (if any) when the C++ function object is destroyed.
class c_callback_param {
	void *param_;
	void (*free_)(void *);

public:
	c_callback_param(void *param, void (*free_param)(void *)) noexcept : param_(param), free_(free_param) {}
	c_callback_param(c_callback_param &&other) noexcept
		: param_(std::exchange(other.param_, nullptr)),
		  free_(std::exchange(other.free_, nullptr))
	{
	}
	c_callback_param(const c_callback_param &) = delete;
	c_callback_param &operator=(const c_callback_param &) = delete;
	c_callback_param &operator=(c_callback_param &&) = delete;
	~c_callback_param()
	{
		if (free_) {
			free_(param_);
		}
	}

	void *get() const noexcept { return param_; }
};

} // namespace knurbs::detail
