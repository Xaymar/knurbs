# libknurbs foundation (phase 0, layers 1 to 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the existing tests in CI, then build the first three layers of
libknurbs: foundation (objects, errors, logging, task queues, clock, paths),
data (settings, properties, localization), and the registry and module
loader. Each layer gets its C++ API, its C API and its tests.

**Architecture:** A new top-level directory `libknurbs/` builds one shared
library (`knurbs::knurbs`). Public C++ headers live in
`include/knurbs/`, the C API in `include/knurbs/c/`, the implementation in
`source/`, and tests in `tests/`. Nothing in `libobs/` changes.

**Tech Stack:** C++23, CMake 3.28+, cmocka, nlohmann_json 3.11+ (private
dependency only), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-22-libknurbs-cpp-core-design.md`

## Global Constraints

- C++23 (`target_compile_features(... PUBLIC cxx_std_23)`). Only use features
  that MSVC, GCC 14 and AppleClang all have. Record each new library feature
  in `libknurbs/README.md`.
- Toolchains: MSVC (clang-cl for coverage) on Windows, GCC or Clang with
  libstdc++ on Linux, AppleClang with libc++ on macOS.
- Directory names are written out in full: `source/`, `include/`, `tests/`,
  never `src/`.
- Namespaces are used like directories: `knurbs::error`, `knurbs::log`,
  `knurbs::thread`, `knurbs::platform`, `knurbs::locale`, `knurbs::naming`.
  Internals go in `knurbs::detail`.
- Exceptions live in `knurbs::error` and derive from
  `knurbs::error::exception`. No exception ever leaves a C API function.
- Every public object derives from `knurbs::object` and is held through
  `knurbs::ref<T>`/`knurbs::weak<T>`. There is no `std::shared_ptr` in the
  public API.
- No third-party types (nlohmann, Vulkan, Qt) in installed headers.
- Keys and IDs: lowercase, separators `-` or `.` only. Type IDs are
  reverse-DNS (`com.xaymar.knurbs.capture.video-device`).
- No process-wide mutable globals in libknurbs. Two `knurbs::core` instances
  never interfere.
- The library version is `0.1.0` until 1.0. There's no ABI promise before 1.0.
- Commit author `Narta Xaymar Dirks <info@xaymar.com>`, with a Co-Authored-By
  trailer naming the Claude model that made the commit.
- Test builds configure with `-DOBS_VERSION_OVERRIDE=33.0.0` (the repository's
  newest tags are 27.x).

## Review Focus

1. **Releasing the last reference from inside a callback on the same object**
   (for example a log sink removing itself, or a task releasing its own
   queue) must not deadlock or touch freed memory. Pinned by
   `test_log_sink_removes_itself` (Task 5) and `test_queue_released_from_own_task`
   (Task 6).
2. **Non-ASCII paths on Windows** (user names such as `Jürgen` or `日本`): a
   module or locale file under such a path must load. Pinned by
   `test_path_roundtrip_non_ascii` (Task 7) and `test_discover_non_ascii_dir`
   (Task 13).
3. **Malformed or hostile JSON** (truncated text, wrong types, very deep
   nesting) must throw `error::invalid_argument` and never crash. Pinned by
   `test_from_json_rejects_garbage` and `test_from_json_depth_limit` (Task 9).
4. **A plugin that throws, or that is built against another major**, must not
   take the loader down, and must leave no half-registered types behind.
   Pinned by `test_load_throwing_module` and `test_reject_other_major`
   (Task 13).
5. **Using objects after `core::shutdown()`** must throw
   `error::core_gone`, not crash. Pinned by `test_registry_after_shutdown`
   (Task 12).

---

## File structure

```
.github/workflows/build-project.yaml     (modify: add unit-test jobs)
CMakeLists.txt                           (modify: add_subdirectory(libknurbs))
libknurbs/
  CMakeLists.txt
  README.md                              toolchain floors, allowed C++23 features
  include/knurbs/
    export.hpp  version.hpp  error.hpp  object.hpp  ref.hpp
    log.hpp  thread.hpp  platform.hpp  naming.hpp  core.hpp
    data.hpp  properties.hpp  locale.hpp  registry.hpp  module.hpp
    c/  export.h  result.h  object.h  core.h  log.h  data.h  properties.h
        registry.h  module.h
  source/
    object.cpp  error.cpp  log.cpp  thread.cpp  platform.cpp  naming.cpp
    core.cpp  core-impl.hpp  data.cpp  data-impl.hpp  properties.cpp
    locale.cpp  registry.cpp  module.cpp  module-loader.cpp  module-loader.hpp
    c/  c-guard.hpp  object.cpp  core.cpp  log.cpp  data.cpp  properties.cpp
        registry.cpp  module.cpp
  tests/
    CMakeLists.txt  test.hpp
    test_version.cpp  test_error.cpp  test_object.cpp  test_log.cpp
    test_thread.cpp  test_platform.cpp  test_naming.cpp  test_core.cpp
    test_data.cpp  test_properties.cpp  test_locale.cpp  test_registry.cpp
    test_module.cpp
    modules/  fake-ok.cpp  fake-throws.cpp  fake-other-major.cpp  fake-c.c
    c/  test_c_object.c  test_c_data.c  test_c_registry.c
    data/  locale/en-us.ini  locale/de-de.ini
```

---

### Task 1: Run the existing unit tests in CI (phase 0)

**Files:**
- Modify: `.github/workflows/build-project.yaml`

**Interfaces:**
- Produces: two jobs, `ubuntu-tests` and `windows-tests`. Later tasks rely on
  them running `ctest` for every test registered under `ENABLE_UNIT_TESTS`.

- [ ] **Step 1: Add `ubuntu-tests`.** `runs-on: ubuntu-26.04`,
  `needs: check-event`. Install dependencies with the same
  `.github/scripts/utils.zsh/setup_ubuntu` the build uses (it already
  installs `libcmocka-dev` and `nlohmann-json3-dev`), plus `xvfb`. Configure
  with `cmake --preset ubuntu-ci -DENABLE_UNIT_TESTS=ON -DENABLE_FRONTEND=OFF
  -DENABLE_BROWSER=OFF -DOBS_VERSION_OVERRIDE=33.0.0`, build the target
  `obs-unit-tests`, then run
  `xvfb-run -a ctest --test-dir build_ubuntu --output-on-failure`.
- [ ] **Step 2: Add `windows-tests`.** `runs-on: windows-2025-vs2026`, x64
  only. Use the deps setup from `.github/scripts/Build-Windows.ps1`, then
  configure with `cmake --preset windows-ci-x64 -DENABLE_UNIT_TESTS=ON
  -DENABLE_FRONTEND=OFF -DOBS_VERSION_OVERRIDE=33.0.0`, build
  `obs-unit-tests`, then run
  `ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure`.
- [ ] **Step 3: Verify.** Push the branch and open the workflow run. Expected:
  both jobs green, and the ctest summary lists the cmocka tests (about 84
  executables). If a test is red on `root` as well, it's a real existing bug:
  fix it in its own commit with a regression test (project rule), and never
  disable it.
- [ ] **Step 4: Commit** `ci: Run the unit tests on Ubuntu and Windows`.

---

### Task 2: libknurbs skeleton, version and test harness

**Files:**
- Create: `libknurbs/CMakeLists.txt`, `libknurbs/README.md`,
  `libknurbs/include/knurbs/export.hpp`, `libknurbs/include/knurbs/version.hpp`,
  `libknurbs/include/knurbs/c/export.h`, `libknurbs/tests/CMakeLists.txt`,
  `libknurbs/tests/test.hpp`, `libknurbs/tests/test_version.cpp`
- Modify: `CMakeLists.txt` (add `add_subdirectory(libknurbs)` right after
  `add_subdirectory(libobs)`; `option(ENABLE_LIBKNURBS ... ON)`)
- Modify: `.github/workflows/build-project.yaml` (both test jobs also build
  `knurbs-tests`)

**Interfaces:**
- Produces:
  - CMake target `libknurbs` with alias `knurbs::knurbs`: a shared library,
    `OUTPUT_NAME knurbs`, `VERSION 0.1.0`, `SOVERSION 0`,
    `cxx_std_23` PUBLIC, `CXX_VISIBILITY_PRESET hidden`. On MSVC: `/EHsc
    /permissive- /Zc:__cplusplus /utf-8`. Headers install to
    `<prefix>/include/knurbs/`.
  - `KNURBS_API` (export.hpp) and `KNURBS_C_API` (c/export.h): dllexport
    when `KNURBS_BUILDING` is defined, dllimport otherwise on Windows,
    `__attribute__((visibility("default")))` elsewhere.
  - `namespace knurbs::version { inline constexpr int major = 0, minor = 1,
    patch = 0; KNURBS_API int runtime_major() noexcept; }`. The `runtime_*`
    functions return the values the DLL was built with.
  - CMake function `knurbs_add_test(name [C])`: builds `tests/<name>.cpp`
    (or `.c` with `C`), links `knurbs::knurbs` and `cmocka::cmocka`, and
    registers it with `add_test`, with the DLL directory prepended to `PATH`
    on Windows (copy the `_obs_runtime_env` pattern from
    `test/cmocka/CMakeLists.txt`). It adds the test to the custom target
    `knurbs-tests`.
  - `tests/test.hpp`: includes `<cmocka.h>` correctly from C++, plus a macro
    `KNURBS_ASSERT_THROWS(expr, Type)`, which fails unless `expr` throws
    `Type`.

- [ ] **Step 1: Write the failing test** `test_version.cpp`:
  `test_header_matches_runtime` asserts
  `knurbs::version::runtime_major() == knurbs::version::major` and the same
  for minor and patch.
- [ ] **Step 2: Run** `cmake --build build --target knurbs-tests`. Expected:
  fails, because `version.hpp` doesn't exist.
- [ ] **Step 3: Implement** the CMake files, the headers and
  `source/version.cpp`. `README.md` lists the toolchain floors (MSVC 17.14,
  GCC 14, AppleClang still to be confirmed, with the macOS deployment target to
  be decided when the first macOS build runs), plus an empty "C++23 library
  features in use" table.
- [ ] **Step 4: Run** `ctest --test-dir build -R test_version`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add the library skeleton and test harness`.

---

### Task 3: Error hierarchy

**Files:**
- Create: `include/knurbs/error.hpp`, `source/error.cpp`, `tests/test_error.cpp`

**Interfaces:**
- Produces, in `namespace knurbs::error`:
  - `enum class code : std::int32_t { ok = 0, unknown = 1, invalid_argument = 2,
    not_found = 3, already_exists = 4, out_of_range = 5, unsupported = 6,
    io = 7, core_gone = 8, gpu_lost = 9, plugin_failed = 10, version_mismatch = 11 };`
    Values are only ever appended.
  - `KNURBS_API std::string_view to_string(code) noexcept`. It returns the
    lowercase-hyphenated name (`"invalid-argument"`), or `"unknown"` for
    values it doesn't know.
  - `class KNURBS_API exception : public std::runtime_error {
    public: exception(code, std::string message); code value() const noexcept; };`
  - One subclass per code from 2 to 11, named exactly like the enum value
    (`invalid_argument`, `not_found`, ...), each constructible from
    `std::string message`.

- [ ] **Step 1: Write the failing tests.**
  - `test_subclass_codes`: each subclass's `value()` equals its enum value,
    and `what()` returns the message.
  - `test_catch_as_base`: throwing `not_found` is caught as `exception&` and
    as `std::runtime_error&`.
  - `test_to_string`: `to_string(code::core_gone) == "core-gone"`, and
    `to_string(static_cast<code>(999)) == "unknown"`.
- [ ] **Step 2: Run** `ctest -R test_error`. Expected: build failure.
- [ ] **Step 3: Implement.** Out-of-line key functions in `error.cpp` keep the
  vtables and typeinfo in the DLL, so `catch` works across modules.
- [ ] **Step 4: Run** `ctest -R test_error`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add the knurbs::error exception hierarchy`.

---

### Task 4: object, ref and weak

**Files:**
- Create: `include/knurbs/object.hpp`, `include/knurbs/ref.hpp`,
  `source/object.cpp`, `tests/test_object.cpp`

**Interfaces:**
- Produces:
  - `class KNURBS_API object` with public `void acquire() noexcept` (an
    unconditional increment, for callers that already hold a ref),
    `bool try_acquire() noexcept` (increments only while the count is above
    0), `void release() noexcept`, and `long use_count() const noexcept`.
    Protected: `object() noexcept` (count starts at 1), `virtual ~object()`,
    and `virtual void destroy() noexcept`. The default runs `delete this`;
    overrides may defer it (the destruction context). Copying and moving are
    deleted. Its data members are exactly `std::atomic<long> refs_` and
    `std::atomic<detail::weak_block*> weak_`. These are frozen at 1.0 and are
    the only public data members in the API.
  - `detail::weak_block` (defined in `source/object.cpp` only), with
    `weak_refs`, `pins`, `dead` and `object*`. It is created lazily, and uses
    the pin protocol and the dying sentinel exactly as specified in
    `docs/superpowers/specs/2026-11-18-libobs-unified-refcount-design.md`,
    "Weak block" and "Weak → strong upgrade".
  - Exported helpers that `weak<T>` uses:
    `KNURBS_API detail::weak_block* detail::weak_get(object*)`,
    `KNURBS_API object* detail::weak_lock(detail::weak_block*) noexcept`
    (returns an acquired object or nullptr),
    `KNURBS_API bool detail::weak_expired(detail::weak_block*) noexcept`,
    `KNURBS_API void detail::weak_acquire(detail::weak_block*) noexcept`, and
    `KNURBS_API void detail::weak_release(detail::weak_block*) noexcept`.
  - `template<class T> class ref` (inline, in ref.hpp): `ref() noexcept`;
    `static ref adopt(T*) noexcept` (takes over an existing count);
    `static ref retain(T*) noexcept` (calls `acquire`); copy (acquire), move,
    destructor (release); `T* get() const noexcept`; `operator->`, `operator*`
    and `explicit operator bool`; `void reset() noexcept`;
    `T* detach() noexcept` (gives the count to the caller, for the C API);
    and converting construction from `ref<U>` when `U*` converts to `T*`.
  - `template<class T> class weak`: constructed from a `ref<T>`; `ref<T> lock()
    const noexcept`; `bool expired() const noexcept`; copy and move.
  - `template<class T, class... A> ref<T> make(A&&... args)` constructs with
    `new` and adopts the count. Plugins use it for their instance objects.

- [ ] **Step 1: Write the failing tests.**
  - `test_count_starts_at_one`: `auto r = make<probe>(); use_count() == 1`.
  - `test_last_release_destroys`: `probe` sets a flag in its destructor; the
    flag is set after `r.reset()`.
  - `test_try_acquire_fails_at_zero`: inside `probe::destroy()`, before it
    deletes, `try_acquire()` returns false.
  - `test_weak_lock_alive_and_expired`: `lock()` returns the object while a
    `ref` exists, and returns empty with `expired() == true` after the last
    ref is gone.
  - `test_weak_requested_during_destroy`: from inside the destructor, a
    `weak<probe>` built from `this` (through `ref::retain` failing, so use
    `detail::weak_get(this)`) is already expired and frees itself on release.
  - `test_detach_adopt_roundtrip`: `T* p = r.detach(); auto r2 = ref<T>::adopt(p);
    use_count() == 1`.
  - `test_upgrade_vs_release_stress`: 8 threads call `lock()` in a loop on a
    shared `weak` while the main thread drops the last `ref`. 10,000
    iterations. No thread ever sees a non-null result after the destructor
    flag is set, and ASan/TSan builds stay clean.
- [ ] **Step 2: Run** `ctest -R test_object`. Expected: build failure.
- [ ] **Step 3: Implement** `source/object.cpp` per the refcount spec.
  Spinning on `pins` uses `std::this_thread::yield()`.
- [ ] **Step 4: Run** `ctest -R test_object`. Expected: PASS. Run the stress
  test 20 times (`ctest -R test_object --repeat until-fail:20`). Expected:
  all pass.
- [ ] **Step 5: Commit** `libknurbs: Add object, ref and weak`.

---

### Task 5: Logging

**Files:**
- Create: `include/knurbs/log.hpp`, `source/log.cpp`, `tests/test_log.cpp`

**Interfaces:**
- Consumes: `object`, `ref`, `make` (Task 4).
- Produces, in `namespace knurbs::log`:
  - `enum class level : std::uint8_t { debug, info, warning, error };`
  - `using sink = std::move_only_function<void(level, std::string_view) noexcept>;`
  - `class KNURBS_API logger : public object`, with:
    - `static ref<logger> create()`;
    - `void write(level, std::string_view message) noexcept`;
    - `std::uint64_t add_sink(sink)`, which returns a non-zero id;
    - `void remove_sink(std::uint64_t id) noexcept`;
    - inline templates `debug/info/warning/error(std::format_string<A...>, A&&...)`,
      which format and then call `write`.
  - `write` calls every sink in the order they were added. A sink may call
    `remove_sink` for itself while it runs.

- [ ] **Step 1: Write the failing tests.**
  - `test_sinks_receive_in_order`: two sinks record `(level, text)`.
    `info("a {}", 1)` reaches both, as `"a 1"`, with `level::info`, sink 1
    first.
  - `test_remove_sink`: after `remove_sink`, the sink gets nothing more.
  - `test_log_sink_removes_itself`: a sink that removes itself on its first
    call doesn't deadlock, and the next `write` skips it.
  - `test_two_loggers_independent`: a sink on logger A never sees writes to
    logger B.
- [ ] **Step 2: Run** `ctest -R test_log`. Expected: build failure.
- [ ] **Step 3: Implement.** Copy the sink list under a lock, then call the
  sinks outside it (removal marks the entry, so a removed sink is skipped).
  This keeps self-removal deadlock-free.
- [ ] **Step 4: Run** `ctest -R test_log`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add the logger`.

---

### Task 6: Threads, task queues and deferred destruction

**Files:**
- Create: `include/knurbs/thread.hpp`, `source/thread.cpp`, `tests/test_thread.cpp`

**Interfaces:**
- Consumes: `logger` (Task 5), `object`/`ref` (Task 4).
- Produces, in `namespace knurbs::thread`:
  - `KNURBS_API void set_current_name(std::string_view name) noexcept`.
    It uses `SetThreadDescription` on Windows, `pthread_setname_np` on Linux
    (truncated to 15 bytes) and on macOS.
  - `class KNURBS_API task_queue : public object`, with:
    - `static ref<task_queue> create(std::string_view name, ref<log::logger>)`;
    - `void post(std::move_only_function<void()>)`, which throws
      `error::core_gone` once the queue is stopped;
    - `void flush()`, which blocks until everything posted so far has run.
      Called from the queue's own thread, it runs the pending tasks inline;
    - `void stop() noexcept`, which runs what is queued, then joins;
    - `bool on_queue_thread() const noexcept`.
    It is a single `std::jthread` named after the queue, and runs tasks in
    FIFO order. An exception from a task is caught and logged at
    `level::error` as `"<queue name>: task threw: <what()>"`, and the queue
    keeps running.
  - `class KNURBS_API deferred_object : public object`: overrides `destroy()`
    to post `delete this` to the `task_queue` given to its constructor
    (`explicit deferred_object(ref<task_queue>)`). If the queue is stopped,
    it deletes inline.

- [ ] **Step 1: Write the failing tests.**
  - `test_fifo_order`: 1,000 posted tasks append their index; after `flush()`,
    the vector is 0 to 999 in order.
  - `test_task_exception_is_logged`: a task throws `std::runtime_error("boom")`,
    the next task still runs, and a sink got `"q: task threw: boom"`.
  - `test_post_after_stop_throws`: `KNURBS_ASSERT_THROWS(q->post(...),
    knurbs::error::core_gone)`.
  - `test_deferred_destroy_runs_on_queue`: the destructor of a
    `deferred_object` subclass records `on_queue_thread() == true`.
  - `test_queue_released_from_own_task`: a task drops the last external
    `ref<task_queue>`, and the queue shuts down without deadlocking (the
    join happens on another thread or is skipped when called from its own
    thread).
- [ ] **Step 2: Run** `ctest -R test_thread`. Expected: build failure.
- [ ] **Step 3: Implement.** `task_queue`'s own `destroy()` calls `stop()`.
  When it is called on the queue thread, it detaches the final delete to run
  after the loop exits, instead of joining itself.
- [ ] **Step 4: Run** `ctest -R test_thread --repeat until-fail:20`. Expected:
  PASS.
- [ ] **Step 5: Commit** `libknurbs: Add task queues and deferred destruction`.

---

### Task 7: Platform helpers (clock and UTF-8 paths)

**Files:**
- Create: `include/knurbs/platform.hpp`, `source/platform.cpp`,
  `tests/test_platform.cpp`

**Interfaces:**
- Produces, in `namespace knurbs::platform`:
  - `KNURBS_API std::uint64_t now_ns() noexcept`, a monotonic clock
    (`std::chrono::steady_clock`). It is the core clock that every canvas and
    the audio pipeline will share later.
  - `KNURBS_API std::filesystem::path path_from_utf8(std::string_view)`
  - `KNURBS_API std::string path_to_utf8(const std::filesystem::path&)`.
    On Windows, this goes through `std::u8string`, never the ANSI code page.
- CPU info, power state and the profiler are deferred until a consumer needs
  them (spec layer 1 lists them; there is no user before layer 6).

- [ ] **Step 1: Write the failing tests.**
  - `test_now_monotonic`: 10,000 calls never decrease.
  - `test_path_roundtrip_non_ascii`: for `"Jürgen/日本/ü.ini"`,
    `path_to_utf8(path_from_utf8(s)) == s`. Creating a file there under a
    temp directory, then reading it back through `std::ifstream`, works.
- [ ] **Step 2: Run** `ctest -R test_platform`. Expected: build failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R test_platform`. Expected: PASS on Windows and
  Linux CI.
- [ ] **Step 5: Commit** `libknurbs: Add the core clock and UTF-8 path helpers`.

---

### Task 8: Naming rules

**Files:**
- Create: `include/knurbs/naming.hpp`, `source/naming.cpp`, `tests/test_naming.cpp`

**Interfaces:**
- Produces, in `namespace knurbs::naming`:
  - `KNURBS_API bool is_valid_key(std::string_view) noexcept`: non-empty,
    only `[a-z0-9]`, `-` and `.`, doesn't start or end with a separator, and
    has no two separators in a row.
  - `KNURBS_API bool is_valid_type_id(std::string_view) noexcept`: a valid key
    with at least three `.`-separated segments (`com.xaymar.knurbs`), whose
    first segment is letters only.
  - `KNURBS_API void check_key(std::string_view)`: throws
    `error::invalid_argument("invalid key: <key>")` when the key isn't valid,
    if `KNURBS_CHECK_KEYS` is defined (CMake defines it for Debug and for
    every test target). Otherwise it does nothing.

- [ ] **Step 1: Write the failing tests.**
  - `test_valid_keys`: `"frame-rate"`, `"dshow.control.gain"` and `"a1"` are
    valid; `""`, `"Frame"`, `"frame_rate"`, `"-a"`, `"a."`, `"a..b"`,
    `"a b"` and `"ü"` are not.
  - `test_type_ids`: `"com.xaymar.knurbs.capture.video-device"` is valid;
    `"video-device"`, `"com.xaymar"` and `"1com.x.y"` are not.
  - `test_check_key_throws`: `KNURBS_ASSERT_THROWS(check_key("Bad_Key"),
    knurbs::error::invalid_argument)`.
- [ ] **Step 2: Run** `ctest -R test_naming`. Expected: build failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R test_naming`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add key and type ID naming rules`.

---

### Task 9: core

**Files:**
- Create: `include/knurbs/core.hpp`, `source/core.cpp`, `source/core-impl.hpp`,
  `tests/test_core.cpp`

**Interfaces:**
- Consumes: Tasks 4 to 6.
- Produces:
  - `class KNURBS_API core : public object`, with:
    - `struct options { std::filesystem::path config_path; std::filesystem::path cache_path; };`
    - `static ref<core> create(options)`;
    - `ref<log::logger> logger() const`;
    - `ref<thread::task_queue> destruction_queue() const`;
    - `class registry& registry()` (added in Task 12);
    - `void shutdown() noexcept`;
    - `bool is_shut_down() const noexcept`;
    - `void check_alive() const`, which throws `error::core_gone` after
      shutdown.
  - `detail::core_impl` (in core-impl.hpp, not installed) derives from
    `core`. The public members `static_cast` to it (the derived-implementation
    pattern from the spec).
  - `shutdown()` stops the destruction queue last, after every other core
    thread. It is idempotent, and the core's own last `release` calls it.
  - Logs `"knurbs <major>.<minor>.<patch> core started"` at info level on
    create.

- [ ] **Step 1: Write the failing tests.**
  - `test_create_and_shutdown`: `create`, then `shutdown()`, after which
    `is_shut_down()` is true and `check_alive()` throws `core_gone`.
  - `test_two_cores_independent`: shutting down core A leaves core B's
    `destruction_queue()->post` working.
  - `test_shutdown_idempotent`: calling `shutdown()` twice, then dropping the
    ref, is clean.
- [ ] **Step 2: Run** `ctest -R test_core`. Expected: build failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R test_core`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add knurbs::core`.

---

### Task 10: data (the settings tree)

**Files:**
- Create: `include/knurbs/data.hpp`, `source/data.cpp`, `source/data-impl.hpp`,
  `tests/test_data.cpp`
- Modify: `libknurbs/CMakeLists.txt` (`find_package(nlohmann_json 3.11 REQUIRED)`,
  linked PRIVATE)

**Interfaces:**
- Consumes: `object`/`ref` (Task 4), `naming::check_key` (Task 8), `error` (Task 3).
- Produces:
  - `class KNURBS_API data_array;` (forward)
  - `class KNURBS_API data : public object`, with:
    - `static ref<data> create()`;
    - `static ref<data> from_json(std::string_view)`, which throws
      `error::invalid_argument` on parse errors, on a top level that isn't an
      object, and on nesting deeper than 64;
    - `std::string to_json(bool pretty = false) const`, with keys in sorted
      order, so output is deterministic;
    - `bool has(std::string_view key) const`, true for user values and
      defaults;
    - `bool has_user_value(std::string_view key) const`;
    - `void erase(std::string_view key)`, which removes the user value;
    - `std::vector<std::string> keys() const`, sorted, the union of user
      values and defaults;
    - getters: `bool get_bool(key) const`, `std::int64_t get_int(key) const`,
      `double get_double(key) const`, `std::string get_string(key) const`,
      `ref<data> get_object(key) const` and `ref<data_array> get_array(key) const`.
      Each returns the user value, else the default, else throws
      `error::not_found`. A value of another type throws
      `error::invalid_argument`, except that `get_double` accepts an int.
    - setters `set_bool/int/double/string/object/array(key, value)` and
      `set_default_*` with the same signatures. Every key goes through
      `naming::check_key`;
    - `ref<data> clone() const`, a deep copy.
  - `class KNURBS_API data_array : public object`: `static ref<data_array>
    create()`, `std::size_t size() const`, `ref<data> at(std::size_t) const`
    (throws `error::out_of_range`), `void push_back(ref<data>)` and
    `void erase(std::size_t)`.
  - All members are thread-safe (a `std::shared_mutex` per object).
    `to_json` writes only user values; defaults are never serialized.
  - Legacy key migration: none (spec: importing old data is frontend work).

- [ ] **Step 1: Write the failing tests.**
  - `test_default_vs_user`: `set_default_int("a", 1)` then
    `get_int("a") == 1` and `has_user_value("a") == false`. After
    `set_int("a", 2)`, `get_int("a") == 2`. After `erase("a")`, it is 1 again.
  - `test_missing_key_throws`: `get_int("nope")` throws `not_found`.
  - `test_wrong_type_throws`: `set_string("a","x")`, then `get_int("a")` throws
    `invalid_argument`. `set_int("b",3)`, then `get_double("b") == 3.0`.
  - `test_json_roundtrip`: nested objects and arrays survive
    `from_json(d->to_json())` with equal `to_json()`, and the output's keys
    are sorted.
  - `test_from_json_rejects_garbage`: `"{"`, `"[]"`, `"{\"a\":}"` and `""`
    each throw `invalid_argument`.
  - `test_from_json_depth_limit`: 65 levels of nesting throw
    `invalid_argument`, and 64 parse.
  - `test_invalid_key_throws`: `set_int("Bad_Key", 1)` throws
    `invalid_argument`.
  - `test_clone_is_deep`: changing a nested object in the clone leaves the
    original unchanged.
  - `test_concurrent_set_get`: 4 writer threads and 4 reader threads, 10,000
    operations each, with no crash and a clean TSan run.
- [ ] **Step 2: Run** `ctest -R test_data`. Expected: build failure.
- [ ] **Step 3: Implement.** Store values as `std::variant<bool, std::int64_t,
  double, std::string, ref<data>, ref<data_array>>` in a `std::map`, and use
  nlohmann only inside `from_json`/`to_json`.
- [ ] **Step 4: Run** `ctest -R test_data`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add knurbs::data`.

---

### Task 11: properties and locale

**Files:**
- Create: `include/knurbs/properties.hpp`, `source/properties.cpp`,
  `include/knurbs/locale.hpp`, `source/locale.cpp`, `tests/test_properties.cpp`,
  `tests/test_locale.cpp`, `tests/data/locale/en-us.ini`,
  `tests/data/locale/de-de.ini`

**Interfaces:**
- Consumes: `data` (Task 10), `naming` (Task 8), `platform::path_from_utf8` (Task 7).
- Produces:
  - `enum class property_kind : std::uint8_t { boolean, integer, floating, text,
    path, list, color, button, group, notice };` (values only appended)
  - `class KNURBS_API property : public object`, with:
    - `std::string_view id() const noexcept`;
    - `property_kind kind() const noexcept`;
    - `std::string_view label() const noexcept`;
    - `bool enabled() const`, `bool visible() const`, `void set_enabled(bool)`
      and `void set_visible(bool)`;
    - for integer and floating properties, `double min() const`,
      `double max() const` and `double step() const`;
    - for list properties, `void add_item(std::string label, std::string value)`
      and `std::size_t item_count() const`;
    - for group properties, `ref<class properties> group() const`.
  - `class KNURBS_API properties : public object`, with:
    - `static ref<properties> create()`;
    - `ref<property> add_bool(id, label)`, `add_int(id, label, min, max, step)`,
      `add_float(id, label, min, max, step)`, `add_text(id, label)`,
      `add_path(id, label)`, `add_list(id, label)`, `add_color(id, label)`,
      `add_button(id, label, std::move_only_function<bool(properties&, property&)>)`,
      `add_group(id, label, ref<properties>)` and
      `add_notice(id, std::string text)`;
    - `ref<property> find(std::string_view id) const`, empty if none (searches
      groups too);
    - `std::vector<ref<property>> list() const`, in insertion order;
    - `void set_modified(std::string_view id,
      std::move_only_function<bool(properties&, property&, data&)>)`;
    - `bool notify_modified(std::string_view id, data& settings)`, which
      returns the callback's result, or false when there's none.
    A duplicate id throws `error::already_exists`, and ids go through
    `naming::check_key`. Callbacks run while holding a `ref<properties>`
    (the re-entrancy rule).
  - `class KNURBS_API locale::text_table : public object`, with:
    - `static ref<text_table> load(const std::filesystem::path& dir,
      std::string_view locale, std::string_view fallback = "en-us")`, which
      reads `<dir>/<locale>.ini` over `<dir>/<fallback>.ini` (lines of the
      form `key="value"`, `\n` and `\"` escapes, `#` and `;` comments);
    - `std::string_view get(std::string_view key) const noexcept`, which
      returns the key itself when the key is missing in both files.
    A missing file for the requested locale is fine. A missing fallback file
    throws `error::io`.

- [ ] **Step 1: Write the failing tests.**
  - `test_add_and_find`: add a group containing `"gain"`, then
    `find("gain")->kind() == property_kind::integer` with min, max and step
    preserved.
  - `test_duplicate_id_throws`: adding `"a"` twice throws `already_exists`.
  - `test_modified_callback`: the callback sets `"b"` in `settings`, and
    `notify_modified` returns true and `settings.get_bool("b")`.
  - `test_callback_releases_properties`: the callback drops the caller's last
    external ref to the `properties`, and nothing crashes.
  - `test_locale_fallback`: `de-de.ini` has `a="Ä"`, `en-us.ini` has `a="A"`
    and `b="B"`. Loading `de-de` gives `get("a") == "Ä"`, `get("b") == "B"`
    and `get("c") == "c"`.
  - `test_locale_missing_fallback_throws`: loading from an empty directory
    throws `error::io`.
- [ ] **Step 2: Run** `ctest -R "test_properties|test_locale"`. Expected:
  build failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R "test_properties|test_locale"`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add properties and locale text tables`.

---

### Task 12: Type registry

**Files:**
- Create: `include/knurbs/registry.hpp`, `source/registry.cpp`,
  `tests/test_registry.cpp`
- Modify: `include/knurbs/core.hpp`, `source/core.cpp` (`core::registry()`)

**Interfaces:**
- Consumes: `core::check_alive` (Task 9), `naming::is_valid_type_id` (Task 8).
- Produces:
  - `enum class type_kind : std::uint8_t { source, filter, transition, encoder,
    output, service };`
  - `class KNURBS_API type : public object`. This is a plugin interface, so
    virtuals are allowed and are only appended:
    - `virtual std::string_view id() const noexcept = 0`;
    - `virtual type_kind kind() const noexcept = 0`;
    - `virtual std::uint32_t version() const noexcept { return 1; }`;
    - `virtual ref<properties> properties(const data& settings) const`, which
      returns an empty `properties` by default;
    - `virtual void defaults(data& settings) const {}`.
  - `class KNURBS_API registry` (owned by `core`; not an `object`), with:
    - `void add(ref<type>, std::string_view owner_module = {})`, which throws
      `invalid_argument` when the ID isn't valid and `already_exists` on a
      duplicate;
    - `void add_alias(std::string_view old_id, std::string_view new_id)`. The
      old ID is exempt from the naming rules (it may be an OBS ID such as
      `dshow_input`). It throws `already_exists` when `old_id` is already
      registered, and `invalid_argument` when the alias would form a cycle;
    - `ref<type> find(std::string_view id) const`, which resolves aliases and
      returns empty when the ID isn't found;
    - `std::vector<ref<type>> list(type_kind) const`, sorted by ID, so the
      order is the same on every machine;
    - `void remove_module(std::string_view owner_module)`, which drops that
      module's types and aliases.
    There is one `std::shared_mutex` for all of it (the spec's single registry
    lock). Every member calls `core::check_alive()` first.

- [ ] **Step 1: Write the failing tests.**
  - `test_add_find_list`: add `com.example.b` and `com.example.a` (sources),
    then `list(source)` gives a then b, and `find("com.example.a")` returns it.
  - `test_invalid_id_throws`: `add` of a type with ID `"Bad"` throws
    `invalid_argument`.
  - `test_alias_resolves`: `add_alias("dshow_input",
    "com.xaymar.knurbs.capture.video-device")`, then `find("dshow_input")`
    returns the new type.
  - `test_alias_cycle_throws`: a to b, then b to a, throws `invalid_argument`.
  - `test_remove_module`: types added with `owner_module = "m"` are gone after
    `remove_module("m")`, and the others stay.
  - `test_registry_after_shutdown`: `core->shutdown()`, then
    `core->registry().find("x")` throws `error::core_gone`.
- [ ] **Step 2: Run** `ctest -R test_registry`. Expected: build failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R test_registry`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add the type registry`.

---

### Task 13: Module discovery and parallel loading

**Files:**
- Create: `include/knurbs/module.hpp`, `source/module.cpp`,
  `source/module-loader.cpp`, `source/module-loader.hpp`, `tests/test_module.cpp`,
  `tests/modules/fake-ok.cpp`, `tests/modules/fake-throws.cpp`,
  `tests/modules/fake-other-major.cpp`
- Modify: `include/knurbs/core.hpp`, `source/core.cpp` (`core::modules()`),
  `tests/CMakeLists.txt` (fake modules built as `MODULE` libraries into
  `${CMAKE_CURRENT_BINARY_DIR}/modules/<name>/`, where `<name>` uses the same
  prefix and suffix as real modules)

**Interfaces:**
- Consumes: `registry` (Task 12), `task_queue`/`logger` (Tasks 5 and 6),
  `platform` paths (Task 7).
- Produces:
  - `struct knurbs_module_header { std::uint32_t size; std::uint32_t api_kind;
    std::uint32_t api_major; std::uint32_t api_version; };` in
    `include/knurbs/c/module.h`. `api_kind` is `1` for C and `2` for C++.
    Every module exports
    `extern "C" const knurbs_module_header* knurbs_module_header(void)`.
  - C++ module entry points, all `extern "C"`:
    `void knurbs_module_load(knurbs::module*)` and
    `void knurbs_module_post_load(knurbs::module*)` (optional), plus
    `void knurbs_module_unload(knurbs::module*)` (optional). The macro
    `KNURBS_MODULE_CPP()` in `module.hpp` defines the header with
    `api_kind = 2, api_major = knurbs::version::major`.
  - `class KNURBS_API module : public object`, with:
    - `std::string_view name() const noexcept`;
    - `const std::filesystem::path& binary_path() const noexcept`;
    - `const std::filesystem::path& data_path() const noexcept`;
    - `class core& core() const`;
    - `class registry& registry() const`, which records the owner, so
      `add`/`add_alias` from here pass `name()` as `owner_module`;
    - `ref<log::logger> logger() const`;
    - `bool failed() const noexcept`.
  - `class KNURBS_API module_manager` (owned by `core`), with:
    - `struct found { std::string name; std::filesystem::path binary; std::filesystem::path data; };`
    - `std::vector<found> discover(const std::vector<std::filesystem::path>& search_dirs) const`.
      It lists `<dir>/<name>/bin/<prefix><name><suffix>` with
      `<dir>/<name>/data`, without loading anything, sorted by name;
    - `std::vector<ref<module>> load(const std::vector<found>&)`, which loads
      the libraries and calls `knurbs_module_load` on all of them in parallel,
      waits for all (the barrier), then calls `knurbs_module_post_load` on all
      in parallel. It returns every module, including failed ones;
    - `void unload_all() noexcept`, which calls `knurbs_module_unload` in
      reverse name order and is called by `core::shutdown()` before the
      registry is cleared.
  - Failure handling: a missing or invalid header, a C++ module whose
    `api_major` differs from `version::major`, or an exception or failure
    from `load`/`post_load` marks the module `failed()`. Its types are removed
    with `registry::remove_module`, and an error is logged as
    `"module <name>: <reason>"`. The library stays mapped until shutdown.
    Unmapping code that may still be on a stack isn't safe.

- [ ] **Step 1: Write the failing tests.**
  - `test_discover_without_loading`: `discover` on the fake modules directory
    returns 3 entries sorted by name, and none of them has run (each fake
    writes a marker file in `load`; none exists yet).
  - `test_discover_non_ascii_dir`: a copy of the `fake-ok` module tree under
    `"tmp/Jürgen/日本"` is discovered and loads.
  - `test_load_registers_types`: after `load`, `fake-ok`'s type
    `com.example.fake.ok` is found, and its `post_load` marker exists.
  - `test_load_throwing_module`: `fake-throws` registers
    `com.example.fake.throws`, then throws from `load`. After `load`, it is
    `failed()`, `find("com.example.fake.throws")` is empty, `fake-ok` still
    works, and the log has `"module fake-throws: "`.
  - `test_reject_other_major`: `fake-other-major` reports `api_major = 99`,
    so it is `failed()`, `knurbs_module_load` never ran (no marker), and the
    log contains `"version"`.
- [ ] **Step 2: Run** `ctest -R test_module`. Expected: build failure.
- [ ] **Step 3: Implement.** Use `LoadLibraryW`/`GetProcAddress` on Windows and
  `dlopen(RTLD_NOW | RTLD_LOCAL)`/`dlsym` elsewhere, in
  `module-loader.cpp`. Parallel calls use one `std::jthread` per module,
  joined at the barrier.
- [ ] **Step 4: Run** `ctest -R test_module --repeat until-fail:10`. Expected:
  PASS.
- [ ] **Step 5: Commit** `libknurbs: Add module discovery and parallel loading`.

---

### Task 14: C API for objects, errors, the logger and core

**Files:**
- Create: `include/knurbs/c/result.h`, `include/knurbs/c/object.h`,
  `include/knurbs/c/core.h`, `include/knurbs/c/log.h`, `source/c/c-guard.hpp`,
  `source/c/object.cpp`, `source/c/core.cpp`, `source/c/log.cpp`,
  `tests/c/test_c_object.c`

**Interfaces:**
- Consumes: Tasks 3 to 9.
- Produces:
  - `typedef enum knurbs_result { KNURBS_OK = 0, KNURBS_ERROR_UNKNOWN = 1, ... }`,
    numerically identical to `knurbs::error::code`, with a `static_assert` per
    value in `source/c/object.cpp`.
  - `KNURBS_C_API const char* knurbs_last_error(void)`, a thread-local message
    from the last failed call on this thread, or `""`.
  - `source/c/c-guard.hpp`: `template<class F> knurbs_result knurbs::detail::c_guard(F&&) noexcept`
    catches `error::exception` (mapping `value()`), `std::bad_alloc` (to
    `UNKNOWN`) and `...`, and sets the last-error message. Every C function
    body goes through it.
  - Opaque handle types: `typedef struct knurbs_object knurbs_object_t;`,
    `knurbs_weak_t`, `knurbs_core_t` and `knurbs_logger_t` (in
    `c/object.h`, `c/core.h` and `c/log.h`; `knurbs_module_t` and
    `knurbs_type_t` follow the same pattern in Tasks 13 and 16). A handle is the C++ object's
    address.
  - `void knurbs_ref(knurbs_object_t*)`, `void knurbs_release(knurbs_object_t*)`
    (both accept NULL), and the weak functions `knurbs_weak_t*
    knurbs_weak_get(knurbs_object_t*)`, `knurbs_object_t*
    knurbs_weak_lock(knurbs_weak_t*)` and `void knurbs_weak_release(knurbs_weak_t*)`.
  - `knurbs_result knurbs_core_create(const char* config_path_utf8,
    const char* cache_path_utf8, knurbs_core_t** out)`,
    `void knurbs_core_shutdown(knurbs_core_t*)`, and
    `knurbs_logger_t* knurbs_core_logger(knurbs_core_t*)` (returns a new ref).
  - `knurbs_result knurbs_log_write(knurbs_logger_t*, int level, const char* utf8)`.

- [ ] **Step 1: Write the failing C tests** (compiled as C11).
  - `test_c_core_lifecycle`: `knurbs_core_create` gives `KNURBS_OK` and a
    non-NULL core; after `knurbs_core_shutdown` and `knurbs_release`,
    nothing leaks (ASan).
  - `test_c_error_mapping`: `knurbs_core_create(NULL, NULL, &out)` returns
    `KNURBS_ERROR_INVALID_ARGUMENT`, and `knurbs_last_error()` isn't empty.
  - `test_c_weak`: a weak lock works while the core is alive and returns NULL
    after the last release.
- [ ] **Step 2: Run** `ctest -R test_c_object`. Expected: build failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R test_c_object`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add the C API for objects and core`.

---

### Task 15: C API for data and properties

**Files:**
- Create: `include/knurbs/c/data.h`, `include/knurbs/c/properties.h`,
  `source/c/data.cpp`, `source/c/properties.cpp`, `tests/c/test_c_data.c`

**Interfaces:**
- Consumes: `c_guard` and `knurbs_result` (Task 14), `data`/`properties`
  (Tasks 10 and 11).
- Produces: `knurbs_data_t`, `knurbs_data_array_t`, `knurbs_properties_t` and
  `knurbs_property_t`. Every getter and setter of Tasks 10 and 11 gets a C
  version, named `knurbs_data_get_int(knurbs_data_t*, const char* key, int64_t* out)`
  and so on, returning `knurbs_result`. Strings come out as
  `knurbs_data_get_string(knurbs_data_t*, const char* key, char** out)`, freed
  with `knurbs_free(void*)`. Objects come out as new references. Callbacks
  are C function pointers plus a `void* param`, wrapped into the C++
  `std::move_only_function`.

- [ ] **Step 1: Write the failing C tests.**
  - `test_c_data_roundtrip`: set an int, string and nested object, then
    `knurbs_data_to_json`, then `knurbs_data_from_json` gives equal values.
  - `test_c_data_errors`: a missing key returns `KNURBS_ERROR_NOT_FOUND`, and
    a bad key returns `KNURBS_ERROR_INVALID_ARGUMENT`.
  - `test_c_properties_callback`: a C `modified` callback is called with its
    `param`, and its return value comes back through
    `knurbs_properties_notify_modified`.
- [ ] **Step 2: Run** `ctest -R test_c_data`. Expected: build failure.
- [ ] **Step 3: Implement.** Each function only converts, calls and maps,
  with no logic of its own (spec anti-goal).
- [ ] **Step 4: Run** `ctest -R test_c_data`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add the C API for data and properties`.

---

### Task 16: C API for the registry and C modules

**Files:**
- Create: `include/knurbs/c/registry.h`, `source/c/registry.cpp`,
  `source/c/module.cpp`, `tests/c/test_c_registry.c`, `tests/modules/fake-c.c`

**Interfaces:**
- Consumes: `registry`/`type` (Task 12), the module loader (Task 13),
  `c_guard` (Task 14).
- Produces:
  - `struct knurbs_type_info { uint32_t size; const char* id; uint32_t kind;
    uint32_t version; void (*defaults)(void* param, knurbs_data_t*);
    knurbs_properties_t* (*properties)(void* param, const knurbs_data_t*);
    void* param; void (*free_param)(void* param); };`
    New fields are only appended. The core reads only up to `size`.
  - `knurbs_result knurbs_registry_add(knurbs_module_t*, const struct knurbs_type_info*)`.
    The core wraps the struct in a C++ adapter class derived from
    `knurbs::type`, so the core has one plugin model.
  - `knurbs_result knurbs_registry_add_alias(knurbs_module_t*, const char* old_id, const char* new_id)`.
  - `knurbs_type_t* knurbs_registry_find(knurbs_core_t*, const char* id)`
    (a new ref, or NULL).
  - C module entry points `void knurbs_module_load(knurbs_module_t*)`,
    `knurbs_module_post_load` and `knurbs_module_unload`, with the header's
    `api_kind = 1`. The loader dispatches on `api_kind`.
  - `fake-c.c`: a module written in C that registers `com.example.fake.c`,
    with a `defaults` callback setting `"gain"` to 5.

- [ ] **Step 1: Write the failing tests.**
  - `test_c_module_loads`: loading the modules directory finds
    `com.example.fake.c`, and its defaults give `get_int("gain") == 5`.
  - `test_c_registry_find_missing`: `knurbs_registry_find(core, "com.example.none")`
    returns NULL.
  - `test_c_alias`: an alias added through C resolves through the C++ `find`.
- [ ] **Step 2: Run** `ctest -R "test_c_registry|test_module"`. Expected:
  build failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R "test_c_registry|test_module"`. Expected: PASS.
- [ ] **Step 5: Commit** `libknurbs: Add the C API for the registry and C modules`.

---

## After this plan

Layer 4 (GPU, the RHI and the Vulkan backend) gets its own plan once the
phase 2 work on `vulkan-backend-foundations` is in root. Layers 5 to 8 follow
after that.
