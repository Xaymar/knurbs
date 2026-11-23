#pragma once

#include "knurbs/export.hpp"

namespace knurbs::version {
	// The version of the headers this code was compiled against.
	inline constexpr int major = 0;
	inline constexpr int minor = 1;
	inline constexpr int patch = 0;

	// The version the shared library was built with.
	KNURBS_API int runtime_major() noexcept;
	KNURBS_API int runtime_minor() noexcept;
	KNURBS_API int runtime_patch() noexcept;
} // namespace knurbs::version
