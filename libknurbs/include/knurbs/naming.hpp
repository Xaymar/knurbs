#pragma once

#include <string_view>

#include "knurbs/export.hpp"

namespace knurbs::naming {

// A key is non-empty, made of [a-z0-9], '-' and '.', does not start or end with a separator and never has two
// separators in a row.
KNURBS_API bool is_valid_key(std::string_view key) noexcept;

// A type ID is a valid key with at least three '.'-separated segments (reverse-DNS), whose first segment is
// letters only.
KNURBS_API bool is_valid_type_id(std::string_view id) noexcept;

// Throws error::invalid_argument("invalid key: <key>") if the key is not valid. Does nothing unless the library
// was built with KNURBS_CHECK_KEYS (Debug builds and test builds).
KNURBS_API void check_key(std::string_view key);

} // namespace knurbs::naming
