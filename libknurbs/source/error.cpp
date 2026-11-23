#include "knurbs/error.hpp"

#include <utility>

namespace knurbs::error {

std::string_view to_string(code value) noexcept
{
	switch (value) {
	case code::ok:
		return "ok";
	case code::unknown:
		return "unknown";
	case code::invalid_argument:
		return "invalid-argument";
	case code::not_found:
		return "not-found";
	case code::already_exists:
		return "already-exists";
	case code::out_of_range:
		return "out-of-range";
	case code::unsupported:
		return "unsupported";
	case code::io:
		return "io";
	case code::core_gone:
		return "core-gone";
	case code::gpu_lost:
		return "gpu-lost";
	case code::plugin_failed:
		return "plugin-failed";
	case code::version_mismatch:
		return "version-mismatch";
	}
	return "unknown";
}

exception::exception(code value, std::string message) : std::runtime_error(std::move(message)), code_(value) {}

exception::~exception() = default;

invalid_argument::invalid_argument(std::string message) : exception(code::invalid_argument, std::move(message)) {}

invalid_argument::~invalid_argument() = default;

not_found::not_found(std::string message) : exception(code::not_found, std::move(message)) {}

not_found::~not_found() = default;

already_exists::already_exists(std::string message) : exception(code::already_exists, std::move(message)) {}

already_exists::~already_exists() = default;

out_of_range::out_of_range(std::string message) : exception(code::out_of_range, std::move(message)) {}

out_of_range::~out_of_range() = default;

unsupported::unsupported(std::string message) : exception(code::unsupported, std::move(message)) {}

unsupported::~unsupported() = default;

io::io(std::string message) : exception(code::io, std::move(message)) {}

io::~io() = default;

core_gone::core_gone(std::string message) : exception(code::core_gone, std::move(message)) {}

core_gone::~core_gone() = default;

gpu_lost::gpu_lost(std::string message) : exception(code::gpu_lost, std::move(message)) {}

gpu_lost::~gpu_lost() = default;

plugin_failed::plugin_failed(std::string message) : exception(code::plugin_failed, std::move(message)) {}

plugin_failed::~plugin_failed() = default;

version_mismatch::version_mismatch(std::string message) : exception(code::version_mismatch, std::move(message)) {}

version_mismatch::~version_mismatch() = default;

} // namespace knurbs::error
