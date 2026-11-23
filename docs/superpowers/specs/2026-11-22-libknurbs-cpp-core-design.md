# libknurbs: a C++ core with a C API on top

Status: design approved by Xaymar on 2026-11-23 (open to change before the
1.0 ABI); implementation not started. Requested by Xaymar on 2026-11-22.

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
- **The Vulkan backend inside libknurbs**, with no separate graphics module,
  but still behind an internal RHI abstraction, so that a future Vulkan 2 (or
  another API) can be added as a second backend without touching the rest of
  the core (Xaymar, 2026-11-23).
- **Every plugin running out of process** through xaymar/datapath, so that a
  crashing plugin can't take Knurbs down. Each plugin process has its own
  Vulkan instance and device (Xaymar, 2026-11-23). Anything that crosses the
  plugin boundary (frames, textures, settings) must be expressible as data
  plus exportable handles (NT handles, file descriptors), never as raw
  pointers into the core.

## Non-goals

- ABI or source compatibility with OBS plugins. Legacy OBS plugins will run
  through containerization (xaymar/datapath) instead. No `obs_*` shim is
  planned in libknurbs.
- Compatibility between compilers. Each platform has one toolchain family:
  MSVC ABI on Windows (MSVC, and clang-cl for coverage), GCC or Clang with
  libstdc++ on Linux, and AppleClang with libc++ on macOS.
- Changing libobs. Only libknurbs ships (Xaymar, 2026-11-23). Knurbs is a
  hard fork, and libobs stays in the tree only as a reference that upstream
  fixes get merged into, so they can be ported. It is dropped entirely once
  libknurbs reaches feature parity.
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
  SDK's), and AppleClang. AppleClang supports the features we want
  (`std::move_only_function`, `std::print`, `std::flat_map`,
  `std::jthread`/`std::stop_token`, deducing `this`), but several of them need
  a minimum macOS version higher than the default deployment target, because
  parts of libc++ live in the OS (Xaymar, 2026-11-23). The builder works out
  the lowest macOS version that covers the features actually used, and sets
  `CMAKE_OSX_DEPLOYMENT_TARGET` to it.
- **One standard library per platform**: MSVC STL on Windows, libstdc++ on
  Linux (Clang on Linux must also use libstdc++, never libc++), and libc++ on
  macOS. The C++ ABI promise only holds within that combination.
- **The C++ ABI follows Qt's model** (Xaymar, 2026-11-22). It stays binary
  compatible across every minor and patch release of a major version, the way
  Qt 6.0 plugins keep loading in Qt 6.x, and breaks only at a major version.
  Plugins are rebuilt only for a new major. The C API stays stable across
  majors too. See "Binary compatibility rules" below.
- **Upstream OBS fixes keep being merged into libobs** while it exists as the
  reference. libknurbs must not live inside `libobs/`, so those merges never
  collide. Fixes worth having in libknurbs are ported by hand.
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
  source/                   implementation; nothing here is installed
  source/c/                 the C API layer
  source/gpu/rhi/           the internal RHI interface
  source/gpu/vulkan/        the Vulkan backend of the RHI
  tests/                    libknurbs tests (one file per area)
```

Directory names are written out in full (`source`, not `src`), matching
`include` (Xaymar, 2026-11-23).

Namespaces are used like directories: related things get their own nested
namespace (`knurbs::error`, `knurbs::gpu`, `knurbs::audio`, `knurbs::format`)
rather than flat, C-style prefixed names in `knurbs::` (Xaymar, 2026-11-23).
Internals live in `knurbs::detail` (or a nested `detail` inside the
relevant namespace).

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
	void set_name(std::string_view name);   // throws knurbs::error::invalid_argument
	ref<data> settings() const;
	// ...
protected:
	source() = default;           // only source/ can construct
};
}

// source/source-impl.hpp (not installed)
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
  `source/`, never by plugins, so their size and layout aren't part of the ABI.
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
  one. They are not `std::shared_ptr`, because other languages bind to
  libknurbs through the C API, and they need a count that lives in the object
  and can be driven through plain functions (`knurbs_ref`/`knurbs_release`). A
  raw `T*` coming back from the C API also has to be able to become a `ref<T>`
  again, which `shared_ptr` can't do without its separate control block
  (confirmed by Xaymar, 2026-11-23).
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
  released, but every call on it throws `knurbs::error::core_gone`, and weak refs to
  it expire. Leftover references are logged in debug builds. Objects don't hold
  a strong reference to their core, so a forgotten reference can't keep a
  whole core alive. My call.
- In the C API, a handle is the object's address (`knurbs_source_t *` is a
  `knurbs::source *`). `knurbs_ref`/`knurbs_release`, plus typed aliases, map
  straight onto the count, so handles go back and forth with no lookup table.

### Errors

- **The C++ API reports errors with exceptions** (Xaymar, 2026-11-22). The
  reasons: the success path costs nothing (the zero-cost model that compilers
  and CPUs have been tuned for), and constructors can fail properly, so a
  constructed object is always valid. All exceptions thrown by libknurbs
  live in the `knurbs::error` namespace (Xaymar, 2026-11-23) and derive from
  `knurbs::error::exception`, which derives from `std::runtime_error` and
  carries a code from the public `knurbs::error::code` enum. Subclasses (for
  example `knurbs::error::not_found`, `knurbs::error::invalid_argument`,
  `knurbs::error::core_gone`, `knurbs::error::gpu_lost`) exist where callers
  need to catch them separately.
  Exceptions cross the C++ API boundary in both directions, which is safe
  because every platform has a single toolchain.
- Exceptions are for errors, not for control flow. Lookups that can
  legitimately find nothing (`registry.find(id)`, `scene.find_item(name)`)
  return an empty `ref<T>` or a `std::optional`. Each public function says in
  its header comment what it throws.
- Functions that must never throw are marked `noexcept`: destructors,
  `release`, getters of plain values, and everything on the render and audio
  threads' hot paths that can't fail.
- **The C API catches everything.** Every C function wraps its body. It
  returns a `knurbs_result` code (functions that return objects return NULL on
  failure), and `knurbs_last_error()` (thread-local) gives the message. No
  exception ever leaves a C function.
- **Calls into plugins are wrapped.** The dispatch helper catches whatever a
  plugin throws. The plugin's instance is then disabled and logged (a source
  shows an error state, an output stops with an error), and the core keeps
  running. Errors the core throws to a plugin that calls it are ordinary
  exceptions the plugin can catch.
- Threads owned by the core catch at their top level, so an exception there is
  logged and stops only the work it belongs to, never `std::terminate`s the
  process.

### Threads and locking

- Threads are `std::jthread` with stop tokens. Each core thread has a name and
  an owner, and is joined at `core` shutdown in a defined order.
- Locks use `std::mutex`, `std::shared_mutex` and `std::scoped_lock`, with no
  manual unlock paths. Recursive mutexes are allowed only where a documented
  re-entrancy need exists. Each one is listed in `source/README.md` with its
  reason.
- Each lock has a documented level. In debug builds a lock-order checker
  catches inversions.
- **GPU:** one submission thread per `VkQueue`. Other threads record command
  buffers, or post work to that thread.
- Work queues (a destruction queue, a UI task hook and per-canvas render
  schedules) are small types over `std::move_only_function`.

### Core services by layer, built bottom-up

Each layer has its own tests and is usable before the next one starts.

1. **Foundation:** `object`/`ref`/`weak`, the `error` exception hierarchy, logging (with a
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
4. **GPU:** a public, backend-neutral C++ API for reference-counted GPU
   objects (textures, buffers, shaders, frames), built on an internal RHI
   interface (`source/gpu/rhi/`). The only RHI backend is Vulkan
   (`source/gpu/vulkan/`), built from the libobs-vulkan code after the phase 2
   work on the `vulkan-backend-foundations` branch lands in root. Nothing
   outside `source/gpu/vulkan/` includes Vulkan headers, so a Vulkan 2 backend
   could be added beside it. The RHI is compiled into libknurbs, not loaded as
   a module. Exportable handles are used for every resource that can cross a
   process or module boundary. The `.effect` shader format and the existing
   shader processor are carried over first; offline SPIR-V compilation is a
   later step.
5. **Frames and formats:** the format descriptor described above; CPU and
   GPU frames as reference-counted objects; conversion (matrix, range,
   transfer) as GPU passes. This replaces the parts of `media-io/` that deal
   with video.
   - **Every frame carries all its metadata** (Xaymar, 2026-11-23): the pixel
     format descriptor plus colour primaries, transfer function, matrix,
     range, chroma siting, alpha mode (straight or premultiplied), HDR
     metadata (mastering display, content light levels) and timing. A frame
     without complete metadata can't be created.
   - **The core does the rendering of frames, the same way everywhere.** A
     source, filter or transition never picks its own conversion shader or
     works out how to draw a given format, which is the libobs mess where every
     plugin renders differently. It hands over or receives frames, and the
     core converts them to whatever the consumer asked for (canvas format,
     encoder format, preview) through one conversion path.
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
- **Plugins will all run in their own process** (containerized through
  datapath) so that a crashing plugin can't take Knurbs down, and each
  plugin process has its own Vulkan context (Xaymar, 2026-11-23). Until
  the out-of-process host is built, plugins load in-process, but they are already designed as
  if they were out of process: a plugin never gets the core's `VkDevice` or
  raw `VkImage`s. It gets GPU resources only as exported memory plus exported
  synchronization (timeline semaphores or sync files), and imports them into
  its own device. The in-process mode is a convenience, not a separate
  model.
- **datapath is ready** (https://github.com/Xaymar/datapath, Xaymar,
  2026-11-23). One message carries up to 64 KiB. A round trip averages about
  80 µs, and stays under 0.5 ms even in the worst case of 100% CPU load on all
  cores and threads. The shared-memory path (benchmarked 2026-11-23) is far
  faster. Idle, a message takes about 2 µs on Windows (about 10 µs for
  16-byte messages) and under 1 µs on Linux up to 256 bytes. At 1 KiB it
  takes about 2.5 µs on Windows and 1 µs on Linux, and at 4 KiB about 4.5 µs
  and 1.5 µs. Above that, latency doubles each time the size quadruples. Under
  stress the numbers varied widely, but usually stayed within double the idle
  figures. That shapes the interfaces:
  - **Coarse-grained calls.** Interfaces are built around a few calls per
    frame or per settings change (one `render` with all inputs, one `update`
    with the whole `data`), not chatty getters. Shared memory makes a
    handful of calls per frame cheap, but a getter per property per frame
    would still add up, and the pipe/socket path (about 80 µs) must keep
    working too.
  - **Bulk data never goes through messages.** Frames, textures and audio
    buffers cross as exported handles (GPU memory, shared memory) and only
    their descriptors travel in the message. Anything else that could exceed
    64 KiB (large `data` trees, property lists) is split by the proxy layer,
    never by the plugin.
  - The out-of-process plugin host can be built as soon as the plugin
    interfaces of layer 6 exist, instead of waiting for datapath.
- **Every interface must be proxyable** so that datapath can run the plugin in
  another process. In-process plugins get live objects. The datapath
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
7. Once libknurbs reaches feature parity, libobs, the old frontend and the
   D3D11/OpenGL/Metal backends are deleted from the tree. Legacy OBS plugins
   run through datapath.

libknurbs is the only core that ships. Until step 7, libobs is a reference
only: upstream fixes keep being merged into it, each one gets checked for
whether libknurbs needs the same fix, and it's ported by hand if it does. The
builder keeps a list of what was checked.

## Decided by Xaymar

- A C++ core with the C API as a thin layer on top (2026-11-22).
- The C++ API is exported, with internals hidden, and the frontend uses it
  directly (2026-11-22).
- C++23 (2026-11-22).
- Exceptions in the C++ API, not `std::expected` (2026-11-22). The C API
  catches them and returns result codes.
- Importing old OBS or libobs-era user data (scene collections, profiles,
  settings) is frontend work: a later one-time importer. The libknurbs data
  layer has no legacy key or ID migration path (2026-11-22).
- The C++ ABI stays compatible across a major version, the way Qt 6 does
  (2026-11-22).
- The C++ API is for third-party plugins too. Compatibility between compilers
  isn't required: Windows is MSVC only, Linux is GCC or Clang, macOS is
  AppleClang only (2026-11-22).
- A new library beside libobs, not underneath it (2026-11-22).
- No OBS plugin compatibility. Containerization handles legacy plugins
  (2026-11-19).
- Vulkan only. Metal is dropped and macOS runs on MoltenVK (2026-11-22).
  Vulkan sits behind an internal RHI abstraction, for a possible Vulkan 2
  (2026-11-23).
- Intrusive `ref<T>`/`weak<T>`, not `std::shared_ptr`, because other
  languages bind through the C API (2026-11-23).
- No `obs_*` compatibility shim in libknurbs; plugin containerization covers
  legacy plugins (2026-11-23).
- Every plugin eventually runs in its own process with its own Vulkan
  context (2026-11-23).
- Every frame carries its complete metadata, and the core does all frame
  rendering and conversion in one place (2026-11-23).
- Exceptions live in `knurbs::error`; namespaces are used like directories
  (2026-11-23).
- `source/`, not `src/` (2026-11-23).
- Only libknurbs ships. libobs is a reference for upstream fixes, deleted at
  feature parity (2026-11-23).
- The design is good for now; it can still change before the 1.0 ABI
  (2026-11-23).
- Windows first (project direction).

## My calls (open to correction)

- The derived-implementation pattern instead of pimpl for hiding internals.
- cmocka for the new tests as well.
- Inline refcount in `object` (the only public data member).
- Shutdown with live references: detached objects that throw `knurbs::error::core_gone`.
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

- **Missed upstream fixes.** libobs is only a reference and is never shipped,
  so there's no double maintenance, but a fix merged into libobs can be
  overlooked for libknurbs. The checked-fix list is the mitigation, and the
  risk ends when libobs is deleted at feature parity.
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
