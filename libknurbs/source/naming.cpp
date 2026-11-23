#include "knurbs/naming.hpp"

#include <string>

#include "knurbs/error.hpp"

namespace knurbs::naming {

static bool is_separator(char c) noexcept
{
	return c == '-' || c == '.';
}

bool is_valid_key(std::string_view key) noexcept
{
	if (key.empty() || is_separator(key.front()) || is_separator(key.back())) {
		return false;
	}

	bool previous_separator = false;
	for (char c : key) {
		bool separator = is_separator(c);
		if (!separator && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9')) {
			return false;
		}
		if (separator && previous_separator) {
			return false;
		}
		previous_separator = separator;
	}
	return true;
}

bool is_valid_type_id(std::string_view id) noexcept
{
	if (!is_valid_key(id)) {
		return false;
	}

	std::size_t dots = 0;
	for (char c : id) {
		dots += (c == '.');
	}
	if (dots < 2) {
		return false;
	}

	for (char c : id.substr(0, id.find('.'))) {
		if (!(c >= 'a' && c <= 'z')) {
			return false;
		}
	}
	return true;
}

// The behaviour depends on KNURBS_CHECK_KEYS as defined when compiling the library itself (CMake defines it
// privately for Debug builds and test builds), not on the caller's definition.
void check_key([[maybe_unused]] std::string_view key)
{
#ifdef KNURBS_CHECK_KEYS
	if (!is_valid_key(key)) {
		throw error::invalid_argument("invalid key: " + std::string(key));
	}
#endif
}

} // namespace knurbs::naming
