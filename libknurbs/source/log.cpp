#include "knurbs/log.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace knurbs::log {

namespace detail {

struct sink_entry {
	std::uint64_t id;
	sink fn;
	std::atomic<bool> removed{false};

	sink_entry(std::uint64_t entry_id, sink s) : id(entry_id), fn(std::move(s)) {}
};

class logger_impl final : public logger {
	std::mutex mutex_;
	std::vector<std::shared_ptr<sink_entry>> sinks_;
	std::uint64_t next_id_ = 1;

public:
	std::uint64_t add(sink s)
	{
		std::lock_guard lock(mutex_);
		std::uint64_t id = next_id_++;
		sinks_.push_back(std::make_shared<sink_entry>(id, std::move(s)));
		return id;
	}

	void remove(std::uint64_t id) noexcept
	{
		std::shared_ptr<sink_entry> gone;
		{
			std::lock_guard lock(mutex_);
			auto it = std::ranges::find(sinks_, id, &sink_entry::id);
			if (it == sinks_.end()) {
				return;
			}
			gone = std::move(*it);
			sinks_.erase(it);
		}
		gone->removed.store(true, std::memory_order_release);
	}

	void write(level lvl, std::string_view message) noexcept
	{
		std::vector<std::shared_ptr<sink_entry>> snapshot;
		try {
			std::lock_guard lock(mutex_);
			snapshot = sinks_;
		} catch (...) {
			return; // out of memory: drop the message
		}
		// Outside the lock, so a sink can remove itself. The entries stay alive through the snapshot.
		for (auto &entry : snapshot) {
			if (!entry->removed.load(std::memory_order_acquire)) {
				entry->fn(lvl, message);
			}
		}
	}
};

} // namespace detail

ref<logger> logger::create()
{
	return make<detail::logger_impl>();
}

void logger::write(level lvl, std::string_view message) noexcept
{
	static_cast<detail::logger_impl *>(this)->write(lvl, message);
}

std::uint64_t logger::add_sink(sink s)
{
	return static_cast<detail::logger_impl *>(this)->add(std::move(s));
}

void logger::remove_sink(std::uint64_t id) noexcept
{
	static_cast<detail::logger_impl *>(this)->remove(id);
}

} // namespace knurbs::log
