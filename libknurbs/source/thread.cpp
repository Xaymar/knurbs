#include "knurbs/thread.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "knurbs/error.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace knurbs::thread {

	namespace {

#if !defined(_WIN32)
		// Longest prefix of `name` that fits in `max` bytes without splitting a UTF-8 sequence.
		std::string_view utf8_prefix(std::string_view name, std::size_t max) noexcept
		{
			if (name.size() <= max) {
				return name;
			}
			std::size_t n = max;
			// Back off over continuation bytes (10xxxxxx) to the start of the cut character.
			while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) {
				--n;
			}
			return name.substr(0, n);
		}
#endif

	} // namespace

	void set_current_name(std::string_view name) noexcept
	{
#if defined(_WIN32)
		// SetThreadDescription needs Windows 10 1607; look it up so older kernels and SDKs still work.
		using set_description_fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
		HMODULE kernel32         = GetModuleHandleW(L"kernel32.dll");
		if (!kernel32) {
			return;
		}
		auto set_description = reinterpret_cast<set_description_fn>(reinterpret_cast<void*>(GetProcAddress(kernel32, "SetThreadDescription")));
		if (!set_description) {
			return;
		}
		if (name.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
			return;
		}
		int bytes    = static_cast<int>(name.size());
		int wide_len = bytes ? MultiByteToWideChar(CP_UTF8, 0, name.data(), bytes, nullptr, 0) : 0;
		if (bytes && wide_len <= 0) {
			return; // not valid UTF-8
		}
		try {
			std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
			if (wide_len) {
				MultiByteToWideChar(CP_UTF8, 0, name.data(), bytes, wide.data(), wide_len);
			}
			set_description(GetCurrentThread(), wide.c_str());
		} catch (...) {
			// out of memory: leave the thread unnamed
		}
#elif defined(__APPLE__)
		// macOS names only the calling thread, up to MAXTHREADNAMESIZE - 1 (63) bytes.
		char             buffer[64] = {};
		std::string_view cut        = utf8_prefix(name, sizeof(buffer) - 1);
		cut.copy(buffer, cut.size());
		pthread_setname_np(buffer);
#else
		// Linux allows 16 bytes including the terminator.
		char             buffer[16] = {};
		std::string_view cut        = utf8_prefix(name, sizeof(buffer) - 1);
		cut.copy(buffer, cut.size());
		pthread_setname_np(pthread_self(), buffer);
#endif
	}

	namespace detail {

		class task_queue_impl final : public task_queue {
			struct entry {
				std::uint64_t                      seq;
				knurbs::move_only_function<void()> fn;
			};

			static constexpr std::uint64_t none = std::numeric_limits<std::uint64_t>::max();

			const std::string      name_;
			const ref<log::logger> logger_;

			mutable std::mutex      mutex_;
			std::condition_variable work_cv_; // tasks queued, or stopping
			std::condition_variable done_cv_; // a task finished
			std::deque<entry>       tasks_;
			std::uint64_t           next_seq_ = 0;
			// Sequence number of the task the loop itself is running (not one run inline by flush()), or `none`.
			// Tasks run inline by flush() come later in the queue, so they never finish before this one has started.
			std::uint64_t loop_seq_       = none;
			bool          stopping_       = false;
			bool          delete_on_exit_ = false; // the last reference went away on the queue thread

			std::mutex      join_mutex_; // serialises stop() calls that join
			std::jthread    thread_;
			std::thread::id thread_id_; // written once, in the constructor

			public:
			task_queue_impl(std::string_view name, ref<log::logger> logger) : name_(name), logger_(std::move(logger))
			{
				// The worker takes mutex_ before it touches thread_ or thread_id_.
				std::lock_guard lock(mutex_);
				thread_    = std::jthread([this] { run(); });
				thread_id_ = thread_.get_id();
			}

			~task_queue_impl() override = default;

			bool on_thread() const noexcept
			{
				return std::this_thread::get_id() == thread_id_;
			}

			void post(knurbs::move_only_function<void()> fn)
			{
				if (!fn) {
					throw error::invalid_argument(std::format("{}: posted an empty task", name_));
				}
				{
					std::lock_guard lock(mutex_);
					if (!stopping_) {
						tasks_.push_back(entry{next_seq_++, std::move(fn)});
						work_cv_.notify_one();
						return;
					}
				}
				throw error::core_gone(std::format("{}: the task queue is stopped", name_));
			}

			void flush()
			{
				std::unique_lock    lock(mutex_);
				const std::uint64_t target = next_seq_;
				if (on_thread()) {
					while (!tasks_.empty() && tasks_.front().seq < target) {
						run_front(lock);
					}
					return;
				}
				done_cv_.wait(lock, [&] { return (tasks_.empty() || tasks_.front().seq >= target) && (loop_seq_ == none || loop_seq_ >= target); });
			}

			void stop() noexcept
			{
				{
					std::lock_guard lock(mutex_);
					stopping_ = true;
					work_cv_.notify_one();
				}
				if (on_thread()) {
					return;
				}
				std::lock_guard join_lock(join_mutex_);
				if (thread_.joinable()) {
					thread_.join();
				}
			}

			protected:
			void destroy() noexcept override
			{
				if (on_thread()) {
					// Cannot join from here: the loop drains the queue and deletes the queue when it exits.
					std::lock_guard lock(mutex_);
					stopping_       = true;
					delete_on_exit_ = true;
					return;
				}
				stop();
				delete this;
			}

			private:
			// Pops and runs the front task with `lock` released. `lock` is held again on return.
			void run_front(std::unique_lock<std::mutex>& lock)
			{
				knurbs::move_only_function<void()> fn = std::move(tasks_.front().fn);
				tasks_.pop_front();
				lock.unlock();
				try {
					fn();
				} catch (const std::exception& e) {
					log_failure(e.what());
				} catch (...) {
					log_failure("unknown exception");
				}
				// Destroyed unlocked: the task may hold the last reference to this queue, or to anything else.
				fn = nullptr;
				lock.lock();
				done_cv_.notify_all();
			}

			void log_failure(const char* what) noexcept
			{
				if (!logger_) {
					return;
				}
				try {
					logger_->error("{}: task threw: {}", name_, what);
				} catch (...) {
					// out of memory while formatting: drop the message
				}
			}

			void run()
			{
				set_current_name(name_);
				std::unique_lock lock(mutex_);
				for (;;) {
					work_cv_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
					if (tasks_.empty()) {
						break; // stopping, and drained
					}
					loop_seq_ = tasks_.front().seq;
					run_front(lock);
					loop_seq_ = none;
					done_cv_.notify_all();
				}
				const bool self_delete = delete_on_exit_;
				lock.unlock();
				if (self_delete) {
					// Nobody else holds a reference, so nobody else touches thread_ any more.
					thread_.detach();
					delete this;
				}
			}
		};

	} // namespace detail

	ref<task_queue> task_queue::create(std::string_view name, ref<log::logger> logger)
	{
		return make<detail::task_queue_impl>(name, std::move(logger));
	}

	// task_queue is only ever constructed as a task_queue_impl (its constructor is private), so these casts are
	// always to the dynamic type.
	void task_queue::post(knurbs::move_only_function<void()> task)
	{
		static_cast<detail::task_queue_impl*>(this)->post(std::move(task));
	}

	void task_queue::flush()
	{
		static_cast<detail::task_queue_impl*>(this)->flush();
	}

	void task_queue::stop() noexcept
	{
		static_cast<detail::task_queue_impl*>(this)->stop();
	}

	bool task_queue::on_queue_thread() const noexcept
	{
		return static_cast<const detail::task_queue_impl*>(this)->on_thread();
	}

	deferred_object::deferred_object(ref<task_queue> queue) noexcept : queue_(std::move(queue)) {}

	deferred_object::~deferred_object() = default;

	void deferred_object::destroy() noexcept
	{
		if (queue_) {
			try {
				queue_->post([this] { delete this; });
				return;
			} catch (...) {
				// stopped (or out of memory): delete here instead
			}
		}
		delete this;
	}

} // namespace knurbs::thread
