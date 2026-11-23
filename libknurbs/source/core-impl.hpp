#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "knurbs/core.hpp"
#include "module-loader.hpp"
#include "registry-impl.hpp"

namespace knurbs::detail {

	class core_impl final : public core {
		enum class state { running, stopping, stopped };

		const ref<log::logger>        logger_;
		const ref<thread::task_queue> destruction_queue_;

		std::atomic<bool>           shut_down_{false}; // read lock-free by is_shut_down() and check_alive()
		std::mutex                  mutex_;
		std::condition_variable     stopped_cv_;
		state                       state_ = state::running;
		mutable module_manager_impl modules_{*this};
		mutable registry_impl       registry_{*this}; // last: destroyed first

		public:
		core_impl(ref<log::logger> logger, ref<thread::task_queue> destruction_queue) noexcept;
		~core_impl() override;

		const ref<log::logger>& logger() const noexcept
		{
			return logger_;
		}
		const ref<thread::task_queue>& destruction_queue() const noexcept
		{
			return destruction_queue_;
		}

		registry_impl& registry() const noexcept
		{
			return registry_;
		}
		module_manager_impl& modules() const noexcept
		{
			return modules_;
		}

		void shutdown() noexcept;
		bool is_shut_down() const noexcept
		{
			return shut_down_.load(std::memory_order_acquire);
		}

		protected:
		void destroy() noexcept override;

		private:
		void stop_threads() noexcept;
	};

} // namespace knurbs::detail
