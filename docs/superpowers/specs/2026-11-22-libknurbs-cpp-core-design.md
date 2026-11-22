# libknurbs: a C++ core with a C API on top

Status: design, not started. Requested by Xaymar on 2026-11-22.

## Intent and why

libobs does by hand much of what C++ handles automatically: lifetimes
(roughly 450 `bfree` calls and about 300 cleanup `goto`s in the core),
locking (about 300 manual `pthread_mutex_lock` calls, with unlocks paired by
hand), containers (`DARRAY`, `dstr`, uthash), callbacks (`calldata_t` signal
bags) and a process-wide global (`struct obs_core *obs`). Each of these is a
source of the leak, use-after-free and missed-unlock bugs Knurbs exists to get
rid of.

libknurbs is a new core library written in modern C++ that does the same job.
It exposes two public APIs:

- **A C++ API.** Exported classes, with the internals kept out of the public
  headers. The new frontend (knurbs-frontend) and plugins written in C++ use it
  directly.
- **A C API.** A thin `knurbs_*` layer over the C++ API, for plugins written in
  C or other languages, and for anything that needs an ABI that stays stable
  across major versions.

This is also where the planned API work lands: the `knurbs_` prefix,
reference counting on every object, the rename from libobs to libknurbs and,
later, the Vulkan backend built directly into the library.

## Goals

- A library `libknurbs` (CMake target `knurbs::knurbs`) in a new top-level
  directory `libknurbs/`, written in C++23.
- Every public object is reference counted with one mechanism, through both
  APIs. There are no create/destroy pairs left.
- Ownership, locking and cleanup are expressed through types (RAII), not by
  convention.
- No process-wide singleton. A `knurbs::core` instance owns everything, so
  tests and tools can create more than one, and shutdown order is explicit.
- Identical behaviour on every platform. Windows is the first target, then
  Linux (Wayland and Flatpak), then macOS (MoltenVK).
- The design leaves room for the planned features below, even though it
  doesn't build them.

### Room left for planned features

These are not built by this project, but the core must not make them hard:

- **Multiple canvases built in**, each with its own frame rate, format and
  resolution. A canvas is a first-class core object with its own clock and
  render schedule, not something layered on top of one global video mix.
- **A shared-texture render pipeline.** Sources, filters and transitions
  receive their input as reference-counted GPU frames and produce output
  frames, instead of each one grabbing or re-rendering its input tree.
- **GPU-only paths for 4:4:4(:4) at 8-bit and 16-bit (HDR)**, for YUVA, RGB and
  YUV capture, without going through system memory. Frame formats are
  described by a format descriptor (planes, bit depth, chroma siting, alpha,
  colour space, range, transfer), not a fixed enum of known layouts.
- **A central account system that also registers outputs and services**
  (Twitch, YouTube and so on). The core keeps a service registry that an
  account provider plugin can add to and remove from at runtime.
- **The Vulkan backend inside libknurbs**, with no separate graphics module.
- **Plugins running out of process** through xaymar/datapath. Anything that
  crosses the plugin boundary (frames, textures, settings) must be expressible
  as data plus exportable handles (NT handles, file descriptors), never as raw
  pointers into the core.

## Non-goals

- ABI or source compatibility with OBS plugins. Legacy OBS plugins will run
  through containerization (xaymar/datapath) instead. No `obs_*` shim is
  planned in libknurbs.
- Compatibility between compilers. Each platform has one toolchain family:
  MSVC ABI on Windows (MSVC, and clang-cl for coverage), GCC or Clang with
  libstdc++ on Linux, and AppleClang with libc++ on macOS.
- Changing libobs. It stays as it is until it is retired, apart from the
  upstream fixes that keep being merged into it.
- Building the planned features listed above.
- Scripting. The first release has none; it returns later as plugins.

## Anti-goals (a failure even if it works)

- A C++ API that leaks internals: private members, internal headers or
  third-party types (Vulkan, Qt, FFmpeg) in installed headers.
- A C API that isn't thin, meaning C-layer logic that the C++ API doesn't have.
  Each C function should only convert arguments, call the C++ API and map
  errors.
- An exception or a C++ type crossing the C API boundary.
- Hidden global state that makes two `knurbs::core` instances interfere.
- Two threads submitting to the same `VkQueue`. All submissions to a queue go
  through one thread (project rule, 2026-11-21).
- A rewrite that ends up with OBS's design in C++ syntax. The point is to use
  what C++ gives us, not to translate files.

## Constraints

- **C++23** (Xaymar, 2026-11-22). A feature may be used only when the oldest
  supported compiler on all three platforms has it. The builder records the
  minimum versions in `libknurbs/README.md`: current MSVC, GCC 14 (the Flatpak
  SDK's), and the AppleClang release that first ships the features we need.
  Verify AppleClang's libc++ before relying on any of these:
  `std::expected`, `std::move_only_function`, `std::print`, `std::flat_map`,
  `std::jthread`/`std::stop_token`, and deducing `this`.
- **One standard library per platform**: MSVC STL on Windows, libstdc++ on
  Linux (Clang on Linux must also use libstdc++, never libc++), and libc++ on
  macOS. The C++ ABI promise only holds within that combination.
- **The C++ ABI follows Qt's model** (Xaymar, 2026-11-22). It stays binary
  compatible across every minor and patch release of a major version, the way
  Qt 6.0 plugins keep loading in Qt 6.x, and breaks only at a major version.
  Plugins are rebuilt only for a new major. The C API stays stable across
  majors too. See "Binary compatibility rules" below.
- **Upstream OBS fixes keep being merged into libobs.** libknurbs must not live
  inside `libobs/`, so those merges never collide. Fixes worth having in
  libknurbs are ported by hand.
- **The existing test suite** (84 cmocka files in `test/cmocka/`) tests libobs
  through its public API. It guards libobs, not libknurbs. Each libknurbs layer
  needs its own ported tests before anything moves onto it. CI doesn't run the
  tests today.
- **Naming**: keys and IDs are lowercase with only `-` or `.` as separators.
  Type IDs use reverse-DNS (`com.xaymar.knurbs.capture.video-device`). C
  identifiers are the exception. Formatting follows `.clang-format`.
- **Headers** install under `<prefix>/include/knurbs/` (already the rule for
  libobs).
- **Encoders** start their settings with the patent notice (libobs already does
  this through `OBS_ENCODER_PATENT_NOTICE`). libknurbs has to keep that
  guarantee at the encoder registry level, so encoder modules get it for free.

## Approaches considered

### Where the new core lives

1. **A new library beside libobs (chosen).** `libknurbs/` is built bottom-up,
   one layer at a time. Tests are ported per layer, and knurbs-frontend and the
   bundled plugins move over one by one. libobs is untouched until it's retired.
   Upstream merges never collide, at the cost of porting upstream fixes by hand
   and running two cores side by side for a while.
2. **Underneath libobs.** The `obs_*` functions would become shims over the C++
   core, one subsystem at a time, and the existing tests would run unchanged
   throughout. But it rewrites libobs files in place, which is the upstream
   merge collision the project avoids, and it ties the new design to the old
   API's semantics.
3. **Converting libobs to C++ in place.** This keeps the old structure and has
   the merge problems of option 2, so it was dropped.

Chosen by Xaymar: **1**, because it carries the least risk of merge conflicts
when pulling in upstream updates.

### How the C++ API is exposed

1. **Exported C++ classes with the internals hidden (chosen).** The ABI is
   stable across a major version (Qt-style), and the C API stays stable
   across majors too.
2. **Header-only C++ wrappers over the C ABI** (like `obs.hpp`, but complete).
   That would keep the ABI stable forever, but every call would pass through C,
   and the frontend would lose much of what it gains from C++.

Chosen: **1** (described by Xaymar; confirmed 2026-11-22).

## Design

### Layout

```
libknurbs/
  CMakeLists.txt
  README.md                 toolchain minimums, allowed C++23 features
  include/knurbs/           installed C++ API  (knurbs/core.hpp, knurbs/source.hpp, ...)
  include/knurbs/c/         installed C API    (knurbs/c/core.h, knurbs/c/source.h, ...)
  src/                      implementation; nothing here is installed
  src/c/                    the C API layer
  tests/                    libknurbs tests (one file per area)
```

Output names: `knurbs.dll`/`knurbs.lib` on Windows, `libknurbs.so.<major>` on
Linux and `libknurbs.<major>.dylib` on macOS. The soname carries the major
version because the C++ ABI only changes there.

### Hiding the internals

A public class declares only public, non-virtual member functions. Its state
lives in a private implementation class that derives from it:

```cpp
// include/knurbs/source.hpp (installed)
namespace knurbs {
class KNURBS_API source : public object {
public:
	std::string_view id() const noexcept;
	std::string name() const;
	expected<void> set_name(std::string_view name);
	ref<data> settings() const;
	// ...
protected:
	source() = default;           // only src/ can construct
};
}

// src/source-impl.hpp (not installed)
namespace knurbs::detail {
class source_impl final : public knurbs::source { /* members */ };
}
```

The exported member functions `static_cast` `this` to `source_impl`. This
avoids a pimpl's second allocation and pointer hop, and keeps the vtable out of
the ABI. Virtual functions appear only on interfaces that plugins implement
(see Plugins). My call; a pimpl would also meet the goal.

### Binary compatibility rules

These follow Qt's binary compatibility rules, applied to the two kinds of
class:

- **Core classes** (`source`, `canvas`, `data` and so on) have no data members
  in the public header and no virtual functions. They are created only by
  `src/`, never by plugins, so their size and layout aren't part of the ABI.
  Adding non-virtual member functions is always safe, and so is adding
  members to the `_impl` class.
- **Plugin interfaces** (`source_type`, `encoder_type` and so on) have
  vtables that plugins compile against. Within a major version:
  - Virtual functions are never reordered or removed, and existing ones keep
    their signatures.
  - New virtual functions may only be appended at the end of a class that is
    never further derived from inside the API, and they must have a default
    implementation, so older plugins (with shorter vtables) are never asked
    for them. The core checks the interface version that the module reports
    before calling anything newer than that version.
  - Bigger additions go into a new interface (for example
    `source_type_v2 : source_type`), and the core finds it with a
    `dynamic_cast` or a `query_interface` call.
- **Inline code** in public headers is kept to trivial accessors and
  templates over `ref<T>`, since inline code is compiled into plugins and
  can't be changed later.
- **Enums** only get new values appended. Plugins have to handle unknown
  values.
- **CI** runs an ABI check against the previous release of the same major
  (abidiff on Linux; the builder chooses a tool for Windows, where the
  equivalent check is weaker).

### Object model and reference counting

- `knurbs::object` is the base of every public object. It holds an atomic
  strong count, starting at 1, plus a lazily created weak block. It uses the
  semantics already proven in `libobs/obs-mem.c`: the count starts at 1, the
  object is destroyed at 0, and the pin protocol makes weak-to-strong upgrades
  safe.
- `knurbs::ref<T>` is an intrusive strong pointer and `knurbs::weak<T>` a weak
  one. They are not `std::shared_ptr`: a raw `T*` has to be able to become a
  `ref<T>` again when it comes back from the C API, and `shared_ptr` can't do
  that without a separate control block.
- **Destruction context.** An object can say where its final teardown must run
  (for example sources on the core's destruction queue, GPU objects on the GPU
  thread). The last `release` then queues the teardown there instead of running
  it inline. That keeps the rule libobs enforces by hand (sources are destroyed
  on the destroy thread) while every holder stays free to release from any
  thread.
- **Re-entrancy rule** (carried over from the refcount design): any core loop
  or callback dispatch that hands an object to user code holds a strong
  reference for the duration of the call.
- **The count lives inline.** `object` is the one public class with data in its
  header: the strong count and the weak-block pointer, frozen from the first
  release. This keeps `ref<T>` inline and refcounting free of exported calls on
  hot render paths. My call.
- **Core shutdown with live references.** `core::shutdown()` (also run by the
  last release of the core) stops the threads in a defined order, releases
  everything the core itself holds, and marks every object it created as
  detached. A reference that outlives the core stays valid memory until it is
  released, but every call on it returns `error::core_gone`, and weak refs to
  it expire. Leftover references are logged in debug builds. Objects don't hold
  a strong reference to their core, so a forgotten reference can't keep a
  whole core alive. My call.
- In the C API, a handle is the object's address (`knurbs_source_t *` is a
  `knurbs::source *`). `knurbs_ref`/`knurbs_release`, plus typed aliases, map
  straight onto the count, so handles go back and forth with no lookup table.

### Errors

- Fallible public C++ calls return `knurbs::expected<T>`, which is
  `std::expected<T, knurbs::error>`. `knurbs::error` holds a code from a public
  enum plus a message.
- The C API returns a `knurbs_result` code. Functions that return objects
  return NULL on failure, and `knurbs_last_error()` (thread-local) gives the
  message.
- Exceptions are allowed inside `src/`, but are caught at every public
  boundary. Every call into a plugin is wrapped, so a plugin that throws
  disables itself (it is logged and its source turns into an error state)
  instead of taking the core down. My call: Xaymar may prefer exceptions
  through the C++ API instead.

### Threads and locking

- Threads are `std::jthread` with stop tokens. Each core thread has a name and
  an owner, and is joined at `core` shutdown in a defined order.
- Locks use `std::mutex`, `std::shared_mutex` and `std::scoped_lock`, with no
  manual unlock paths. Recursive mutexes are allowed only where a documented
  re-entrancy need exists. Each one is listed in `src/README.md` with its
  reason.
- Each lock has a documented level. In debug builds a lock-order checker
  catches inversions.
- **GPU:** one submission thread per `VkQueue`. Other threads record command
  buffers, or post work to that thread.
- Work queues (a destruction queue, a UI task hook and per-canvas render
  schedules) are small types over `std::move_only_function`.

### Core services by layer, built bottom-up

Each layer has its own tests and is usable before the next one starts.

1. **Foundation:** `object`/`ref`/`weak`, `error`/`expected`, logging (with a
   sink that plugins and the frontend can hook), UTF-8 strings,
   `std::filesystem` paths (UTF-8 conversion on Windows), platform helpers
   (time, CPU, power, high-resolution clock), threads and queues, the profiler.
   These replace `util/` (bmem, dstr, darray, threading, platform).
2. **Data:** `knurbs::data` (the settings tree, JSON-backed, replacing
   `obs_data`) with defaults, auto-selection and migrations;
   `knurbs::properties` (UI descriptions with typed property objects); and
   localization lookup. Key naming rules are checked in debug builds.
3. **Registry and modules:** a type registry for sources, filters,
   transitions, encoders, outputs and services, keyed by reverse-DNS ID, with
   aliases from old IDs. There is one registry lock. Module discovery and
   loading follow the split already proven by the new frontend (branch
   `add-knurbs-frontend`, patches 0001 and 0002 in the project files):
   discover without loading, load all in parallel, a barrier, then post-load
   all.
4. **GPU:** the Vulkan backend, built from the libobs-vulkan code (after the
   phase 2 work on the `vulkan-backend-foundations` branch lands in root), moved in as `src/gpu/` with a
   C++ API for reference-counted GPU objects (textures, buffers, shaders,
   frames). Exportable handles are used for every resource that can cross a
   process or module boundary. The `.effect` shader format and the existing
   shader processor are carried over first; offline SPIR-V compilation is a
   later step.
5. **Frames and formats:** the format descriptor described above; CPU and
   GPU frames as reference-counted objects; conversion (matrix, range,
   transfer) as GPU passes. This replaces the parts of `media-io/` that deal
   with video.
6. **Sources, filters, transitions, scenes and canvases:** the source graph,
   with each canvas owning its own clock, frame rate, format, resolution and
   render schedule. There is one core clock (monotonic, in nanoseconds). Every
   canvas schedules its frames on it, and audio is mixed on the same
   timeline, so all canvases stay in A/V sync with each other. A canvas can
   choose which audio mixes it feeds. The render model is built so that the shared-texture
   pipeline can later replace "each node renders its own input" without an API
   break: a node's render call receives its input frames from the scheduler
   instead of pulling them.
7. **Audio:** the audio pipeline (mixing, monitoring, resampling), replacing
   `obs-audio.c` and the audio half of `media-io/`.
8. **Encoders, outputs and services:** the encoder registry (which adds the
   patent notice to every encoder's properties), outputs, service registry
   with runtime registration (for the account system later), and hotkeys.

### Plugins

- A plugin implements C++ interfaces such as `knurbs::source_type` and
  `knurbs::encoder_type`, which are abstract classes with virtual functions,
  and registers them with `core.registry()` under a reverse-DNS ID.
- **One object per instance.** A `*_type` is a factory: it describes the type
  (ID, name, capabilities, properties) and creates a per-instance object that
  the plugin implements (`knurbs::source_instance` and so on). That object is
  itself a `knurbs::object`, so it is reference counted like everything else
  and holds its own state, with no `void *data` passed around. My call.
- **Every interface must be proxyable** so that datapath can run the plugin in
  another process later. In-process plugins get live objects. The datapath
  host implements the same interfaces and forwards each call over IPC. For that
  to work, interface signatures may only use values that can be serialized
  (`data`, strings, numbers, format descriptors) and core objects (`ref<frame>`,
  `ref<texture>`) whose contents can be exported as handles. Raw pointers to
  memory, function pointers and plugin-owned buffers handed to the core are not
  allowed in interface signatures. A review checklist item for every new
  interface. My call.
- The C API mirrors each interface as a versioned struct of callbacks
  (`struct knurbs_source_type_info` with a leading `size` field). The core
  wraps such a struct in an adapter class that implements the same C++
  interface, so the core has exactly one plugin model internally.
- The module entry points are `knurbs_module_load`, `knurbs_module_post_load`
  and `knurbs_module_unload`. A module reports which API it was built against
  (`cpp` with the major version, or `c` with the C ABI version), so the loader
  refuses a C++ module from another major instead of crashing.
- Every call into a plugin goes through one dispatch helper, which catches
  exceptions, holds the re-entrancy reference and records profiler scopes.

### The C API layer

- `include/knurbs/c/*.h`, prefixed `knurbs_`, with opaque handle types and
  `extern "C"` only. No C++ types, no exceptions, no inline logic.
- The C layer is hand-written at first. If the mapping turns out to be
  mechanical enough, it can be generated later (builder's call).
- The C tests (`tests/c/`) check the whole C API, and are the regression
  guard for the "thin" anti-goal.

### Testing

- **Phase 0:** CI runs the existing libobs cmocka suite on Windows and Linux
  (GitHub Actions), so the old core stays covered while the new one grows.
- **ABI promise starts at libknurbs 1.0**, the first release that the frontend
  ships on (migration step 5). Before that, the version is 0.x and interfaces
  may change freely, so early layers can still be reshaped once later layers
  show problems. The abidiff job and interface version checks are switched on
  at 1.0. My call; the version numbering itself is Xaymar's call.
- libknurbs tests use cmocka for the C API (one framework across the repo).
  The C++ API tests use cmocka from C++ too, unless the builder finds that
  too limiting, in which case they can pick one C++ framework and record why.
- Each layer is ported from the matching libobs tests where they exist, then
  extended. Coverage is measured with clang-cl and llvm-cov on Windows and
  gcov or llvm-cov on Linux. The goal is the maximum practical coverage, with
  unreachable code on a reviewed list.
- A threaded stress test exists for every lifetime-sensitive type (refcount,
  weak upgrade against release, destruction queues).

### Migration and retirement of libobs

1. Phase 0: tests in CI.
2. Layers 1 to 3 (foundation, data, registry and modules), with tests.
3. Layer 4 (GPU), once vulkan-backend-foundations is merged. libknurbs is
   Vulkan-only from the start; D3D11, OpenGL and Metal never exist there.
4. Layers 5 to 8.
5. knurbs-frontend switches from libobs to the libknurbs C++ API.
6. Bundled plugins are ported one by one (capture, encoders such as
   knurbs-nvenc and knurbs-hw-encoders, outputs, text). Each one moves to new
   IDs with aliases from the old ones.
7. libobs, the old frontend and the D3D11/OpenGL/Metal backends are dropped
   from the default build. Legacy OBS plugins run through datapath.

Until step 7, upstream fixes keep landing in libobs. Each one gets checked for
whether libknurbs needs the same fix, and ported by hand if it does. The
builder keeps a list of what was checked.

## Decided by Xaymar

- A C++ core with the C API as a thin layer on top (2026-11-22).
- The C++ API is exported, with internals hidden, and the frontend uses it
  directly (2026-11-22).
- C++23 (2026-11-22).
- The C++ ABI stays compatible across a major version, the way Qt 6 does
  (2026-11-22).
- The C++ API is for third-party plugins too. Compatibility between compilers
  isn't required: Windows is MSVC only, Linux is GCC or Clang, macOS is
  AppleClang only (2026-11-22).
- A new library beside libobs, not underneath it (2026-11-22).
- No OBS plugin compatibility. Containerization handles legacy plugins
  (2026-11-19).
- Vulkan only. Metal is dropped and macOS runs on MoltenVK (2026-11-22).
- Windows first (project direction).

## My calls (open to correction)

- The derived-implementation pattern instead of pimpl for hiding internals.
- Intrusive `ref<T>`/`weak<T>` instead of `std::shared_ptr`.
- `std::expected` in the C++ API, exceptions caught at every boundary.
- No `obs_*` compatibility shim in libknurbs at all.
- cmocka for the new tests as well.
- Inline refcount in `object` (the only public data member).
- Shutdown with live references: detached objects, `error::core_gone`.
- Per-instance plugin objects that are themselves `knurbs::object`s.
- Every plugin interface proxyable for datapath; no raw pointers in signatures.
- One core clock shared by all canvases and audio.
- No ABI promise until libknurbs 1.0.
- The plan covers phase 0 and layers 1 to 3 in detail; later layers get their
  own plans once those land, because they depend on the Vulkan work.
- `include/knurbs/` for the C++ API and `include/knurbs/c/` for the C API.
- The soname carries the major version.
- How Qt-style binary compatibility applies to plugin interfaces: appending
  virtual functions with a version check, or a new interface for larger
  additions.

## Left to the builder

- The exact layer boundaries within each numbered layer, and the order of the
  types within them.
- Whether `knurbs::data` stays JSON-backed internally or only serializes to
  JSON.
- Debug-only checks (the lock-order checker, key naming checks): how they're
  built and switched on.
- Whether the C layer is hand-written throughout or generated.
- How the old `.effect` shader files map onto the new GPU API.

## Risks

- **Two cores side by side** for a long time doubles the maintenance work, and
  upstream fixes can be missed. The checked-fix list is the mitigation.
- **AppleClang lagging behind on C++23 library features.** This is mitigated by
  the feature floor in `libknurbs/README.md`. There is also no Mac among the
  test machines.
- **C++ ABI breaks within a major version** are easy to introduce by accident,
  for example a virtual function inserted in the middle of an interface or a
  changed inline function. The rules above and the ABI check in CI catch
  these. On Windows the check is weaker, so reviews there have to follow the
  rules closely.
- **Scope.** This is a rewrite of the whole core. Layers 1 to 3 are usable on
  their own, and nothing in libobs depends on libknurbs, so the project can stop
  after any layer without leaving the tree broken.
