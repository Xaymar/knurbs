#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "knurbs/export.hpp"

namespace knurbs::error {

// Values are only ever appended.
enum class code : std::int32_t {
	ok = 0,
	unknown = 1,
	invalid_argument = 2,
	not_found = 3,
	already_exists = 4,
	out_of_range = 5,
	unsupported = 6,
	io = 7,
	core_gone = 8,
	gpu_lost = 9,
	plugin_failed = 10,
	version_mismatch = 11,
};

// Lowercase-hyphenated name of the code ("invalid-argument"), or "unknown" for values this library doesn't know.
KNURBS_API std::string_view to_string(code) noexcept;

#if defined(_MSC_VER)
#pragma warning(push)
// std::runtime_error is not exported. Deriving an exported exception from a standard exception is the sanctioned
// case of C4275: both sides use the same C++ runtime (one toolchain per platform), so the base's layout matches.
#pragma warning(disable : 4275)
#endif

class KNURBS_API exception : public std::runtime_error {
	code code_;

public:
	exception(code value, std::string message);
	~exception() override;

	code value() const noexcept { return code_; }
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

class KNURBS_API invalid_argument : public exception {
public:
	explicit invalid_argument(std::string message);
	~invalid_argument() override;
};

class KNURBS_API not_found : public exception {
public:
	explicit not_found(std::string message);
	~not_found() override;
};

class KNURBS_API already_exists : public exception {
public:
	explicit already_exists(std::string message);
	~already_exists() override;
};

class KNURBS_API out_of_range : public exception {
public:
	explicit out_of_range(std::string message);
	~out_of_range() override;
};

class KNURBS_API unsupported : public exception {
public:
	explicit unsupported(std::string message);
	~unsupported() override;
};

class KNURBS_API io : public exception {
public:
	explicit io(std::string message);
	~io() override;
};

class KNURBS_API core_gone : public exception {
public:
	explicit core_gone(std::string message);
	~core_gone() override;
};

class KNURBS_API gpu_lost : public exception {
public:
	explicit gpu_lost(std::string message);
	~gpu_lost() override;
};

class KNURBS_API plugin_failed : public exception {
public:
	explicit plugin_failed(std::string message);
	~plugin_failed() override;
};

class KNURBS_API version_mismatch : public exception {
public:
	explicit version_mismatch(std::string message);
	~version_mismatch() override;
};

} // namespace knurbs::error
