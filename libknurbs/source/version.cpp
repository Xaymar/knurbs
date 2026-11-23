#include "knurbs/version.hpp"

namespace knurbs::version {
	int runtime_major() noexcept
	{
		return major;
	}

	int runtime_minor() noexcept
	{
		return minor;
	}

	int runtime_patch() noexcept
	{
		return patch;
	}
} // namespace knurbs::version
