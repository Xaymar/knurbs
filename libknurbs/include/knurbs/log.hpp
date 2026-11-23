#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "knurbs/export.hpp"
#include "knurbs/function.hpp"
#include "knurbs/object.hpp"
#include "knurbs/ref.hpp"

namespace knurbs::log {

	enum class level : std::uint8_t { debug, info, warning, error };

	// Receives every message written to the logger it was added to. Called without any logger lock held.
	using sink = knurbs::move_only_function<void(level, std::string_view) noexcept>;

	namespace detail {
		class logger_impl;
	} // namespace detail

	// Delivers messages to its sinks. Thread-safe. Two loggers never share sinks or state.
	class KNURBS_API logger : public object {
		// Only created by create(); not meant to be subclassed.
		logger() noexcept = default;
		friend class detail::logger_impl;

		public:
		static ref<logger> create();

		// Calls every sink, in the order they were added. A sink may call remove_sink for itself while it runs.
		void write(level lvl, std::string_view message) noexcept;

		// Returns a non-zero id for remove_sink.
		std::uint64_t add_sink(sink s);
		// Unknown ids are ignored. A removed sink is not called again, also not by a write already in progress that
		// has not reached it yet. It does not wait for a call that has: on another thread the sink may still be
		// running when this returns, so what it uses must stay valid until then.
		void remove_sink(std::uint64_t id) noexcept;

		template<class... A>
		void debug(std::format_string<A...> fmt, A&&... args)
		{
			write(level::debug, std::format(fmt, std::forward<A>(args)...));
		}
		template<class... A>
		void info(std::format_string<A...> fmt, A&&... args)
		{
			write(level::info, std::format(fmt, std::forward<A>(args)...));
		}
		template<class... A>
		void warning(std::format_string<A...> fmt, A&&... args)
		{
			write(level::warning, std::format(fmt, std::forward<A>(args)...));
		}
		template<class... A>
		void error(std::format_string<A...> fmt, A&&... args)
		{
			write(level::error, std::format(fmt, std::forward<A>(args)...));
		}
	};

} // namespace knurbs::log
