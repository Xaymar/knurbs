#include "knurbs/platform.hpp"

#include <chrono>

namespace knurbs::platform {

std::uint64_t now_ns() noexcept
{
	auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

// std::filesystem::path accepts and produces std::u8string on every platform: on Windows this bypasses the ANSI
// code page, elsewhere the native encoding already is UTF-8.
std::filesystem::path path_from_utf8(std::string_view utf8)
{
	std::u8string text(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size());
	return std::filesystem::path(text);
}

std::string path_to_utf8(const std::filesystem::path &path)
{
	std::u8string text = path.u8string();
	return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

} // namespace knurbs::platform
