#include "knurbs/locale.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_map>

#include "knurbs/error.hpp"
#include "knurbs/naming.hpp"
#include "knurbs/platform.hpp"

namespace knurbs {

	namespace detail {

		struct string_hash {
			using is_transparent = void;
			std::size_t operator()(std::string_view s) const noexcept
			{
				return std::hash<std::string_view>{}(s);
			}
		};

		class text_table_impl final : public locale::text_table {
			public:
			std::unordered_map<std::string, std::string, string_hash, std::equal_to<>> texts;

			text_table_impl()           = default;
			~text_table_impl() override = default;
		};

	} // namespace detail

	namespace locale {

		namespace {

			std::string_view trim(std::string_view s) noexcept
			{
				constexpr std::string_view space = " \t\r\n";
				size_t                     first = s.find_first_not_of(space);
				if (first == std::string_view::npos) {
					return {};
				}
				return s.substr(first, s.find_last_not_of(space) - first + 1);
			}

			// `quoted` includes the surrounding quotes.
			std::string unescape(std::string_view quoted)
			{
				std::string_view in = quoted.substr(1, quoted.size() - 2);
				std::string      out;
				out.reserve(in.size());
				for (size_t i = 0; i < in.size(); ++i) {
					if (in[i] == '\\' && i + 1 < in.size()) {
						char next = in[i + 1];
						if (next == 'n') {
							out += '\n';
							++i;
							continue;
						}
						if (next == '"' || next == '\\') {
							out += next;
							++i;
							continue;
						}
					}
					out += in[i];
				}
				return out;
			}

			void parse(std::string_view content, detail::text_table_impl& table)
			{
				if (content.starts_with("\xEF\xBB\xBF")) {
					content.remove_prefix(3);
				}
				while (!content.empty()) {
					size_t           end  = content.find('\n');
					std::string_view line = trim(content.substr(0, end));
					content               = (end == std::string_view::npos) ? std::string_view{} : content.substr(end + 1);

					if (line.empty() || line.front() == '#' || line.front() == ';') {
						continue;
					}
					size_t eq = line.find('=');
					if (eq == std::string_view::npos) {
						continue;
					}
					std::string_view key   = trim(line.substr(0, eq));
					std::string_view value = trim(line.substr(eq + 1));
					if (key.empty() || value.size() < 2 || value.front() != '"' || value.back() != '"') {
						continue;
					}
					table.texts.insert_or_assign(std::string(key), unescape(value));
				}
			}

			// False if the file can't be opened.
			bool load_file(const std::filesystem::path& path, detail::text_table_impl& table)
			{
				std::error_code ec;
				if (!std::filesystem::is_regular_file(path, ec)) {
					return false;
				}
				std::ifstream file(path, std::ios::binary);
				if (!file) {
					return false;
				}
				std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				if (file.bad()) {
					return false;
				}
				parse(content, table);
				return true;
			}

			std::filesystem::path locale_file(const std::filesystem::path& dir, std::string_view name)
			{
				return dir / platform::path_from_utf8(std::string(name) + ".ini");
			}

		} // namespace

		ref<text_table> text_table::load(const std::filesystem::path& dir, std::string_view locale, std::string_view fallback)
		{
			// The names become part of a path, so they are checked in every build, not only with KNURBS_CHECK_KEYS.
			for (std::string_view name : {locale, fallback}) {
				if (!naming::is_valid_key(name)) {
					throw error::invalid_argument("invalid locale: " + std::string(name));
				}
			}
			auto table = make<detail::text_table_impl>();

			std::filesystem::path fallback_path = locale_file(dir, fallback);
			if (!load_file(fallback_path, *table)) {
				throw error::io("locale: cannot read " + platform::path_to_utf8(fallback_path));
			}
			if (locale != fallback) {
				// A missing file for the requested locale is fine: the fallback texts stay.
				load_file(locale_file(dir, locale), *table);
			}
			return table;
		}

		std::string_view text_table::get(std::string_view key) const noexcept
		{
			const auto& texts = static_cast<const detail::text_table_impl&>(*this).texts;
			if (auto it = texts.find(key); it != texts.end()) {
				return it->second;
			}
			return key;
		}

	} // namespace locale
} // namespace knurbs
