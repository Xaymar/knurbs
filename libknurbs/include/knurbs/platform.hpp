#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "knurbs/export.hpp"

namespace knurbs::platform {
	// Monotonic clock in nanoseconds. This is the core clock that every canvas and the audio pipeline share.
	KNURBS_API std::uint64_t now_ns() noexcept;

	// Converts UTF-8 text to a filesystem path. Never goes through the ANSI code page.
	KNURBS_API std::filesystem::path path_from_utf8(std::string_view utf8);

	// Converts a filesystem path to UTF-8 text. Never goes through the ANSI code page.
	KNURBS_API std::string path_to_utf8(const std::filesystem::path& path);
} // namespace knurbs::platform
