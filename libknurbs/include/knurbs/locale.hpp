#pragma once

#include <filesystem>
#include <string_view>

#include "knurbs/export.hpp"
#include "knurbs/object.hpp"
#include "knurbs/ref.hpp"

namespace knurbs {

	namespace detail {
		class text_table_impl;
	} // namespace detail

	namespace locale {

		// Translated texts of one locale, over a fallback locale. Immutable after load, so thread-safe.
		class KNURBS_API text_table : public object {
			// Only created by load(); not meant to be subclassed.
			text_table() noexcept = default;
			friend class detail::text_table_impl;

			public:
			// Reads <dir>/<locale>.ini over <dir>/<fallback>.ini. A file holds lines of the form key="value" (UTF-8, an
			// optional BOM is skipped); inside a value `\n`, `\"` and `\\` are escapes and other backslash sequences are
			// kept as written. Empty lines, lines starting with '#' or ';', lines without '=' and lines whose value isn't
			// quoted are ignored. `locale` and `fallback` must be valid keys (naming::is_valid_key, checked in every build,
			// so they cannot name a path outside `dir`), else error::invalid_argument. A missing file for `locale` is fine; a missing or unreadable fallback file throws
			// error::io.
			static ref<text_table> load(const std::filesystem::path& dir, std::string_view locale, std::string_view fallback = "en-us");

			// The text for `key`, or `key` itself if neither file has it. The view stays valid as long as the table (or,
			// for a missing key, the argument) does.
			std::string_view get(std::string_view key) const noexcept;
		};

	} // namespace locale
} // namespace knurbs
