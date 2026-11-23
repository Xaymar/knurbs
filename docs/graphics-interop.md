# Graphics interop: external memory and synchronisation

libobs shares GPU memory and GPU sync with other graphics APIs (D3D11 producers, CUDA/NVENC, AMF), other devices
and other processes through the API in `libobs/graphics/graphics.h` ("External memory and synchronisation"). Its
backend side is a set of optional imports (`graphics-imports.c`, declared in `device-exports.h`). libobs-vulkan
implements them on Windows. libobs-d3d11, libobs-opengl and libobs-metal do not export them, so every call fails
there (false, NULL, 0 or -1), and so does the null test module (`test/null-graphics`).

## Design rules

* **Only OS handles cross the boundary**: NT handles on Windows, file descriptors on Linux. A `VkDevice`, `VkImage`
  or `ID3D11Texture2D` is never handed out. A plugin may later run in its own process and receive the handles over a
  pipe or socket (`DuplicateHandle` on Windows, `SCM_RIGHTS` on Linux). Nothing in the API assumes a shared address
  space.
* **One copy is acceptable where zero-copy isn't possible.** The pitch-linear frame buffer (below) is that one copy for
  NVENC.
* **Same behaviour on every platform.** Handle types differ, but ownership, lifetime and ordering rules are the same.
  For example, an import never takes over the handle on any platform, even though Vulkan's fd imports would.
* **Existing callers keep working.** `gs_texture_open_shared`, `gs_texture_open_nt_shared`,
  `gs_texture_get_shared_handle`, `gs_texture_acquire_sync` / `gs_texture_release_sync` and
  `gs_shared_texture_available` are implemented on top of the new paths (Windows section below).

## Handle types

| `enum gs_handle_type` | What it is | Textures | Timelines | Platform |
|---|---|---|---|---|
| `GS_HANDLE_WIN32` | NT handle exported by Vulkan (`OPAQUE_WIN32`) | export, import | export, import | Windows |
| `GS_HANDLE_WIN32_D3D11` | NT handle of a D3D11 texture (`MISC_SHARED_NTHANDLE`) | import | â€” | Windows |
| `GS_HANDLE_WIN32_D3D11_KMT` | legacy global share handle (`MISC_SHARED`, game capture) | import | â€” | Windows |
| `GS_HANDLE_WIN32_D3D12_FENCE` | NT handle of an `ID3D12Fence` / `ID3D11Fence` | â€” | import | Windows |
| `GS_HANDLE_FD` | opaque fd exported by Vulkan | export, import | export, import | Linux |
| `GS_HANDLE_DMABUF` | dmabuf, one fd per plane, with offsets, pitches, modifier | import (export: single plane) | â€” | Linux |
| `GS_HANDLE_SYNC_FD` | sync_file, binary and one-shot | â€” | `gs_sync_file_*` | Linux |

`gs_shared_texture_supported(type, layout, format, import)` says whether a texture of that layout and format can be
imported from, or exported as, a type on the current device. The Linux rows are declared but not implemented yet. The
Linux backend adds them without changing the header: `gs_shared_texture_desc` already carries per-plane fds,
`offsets`, `pitches` and `modifier`.

## Objects and calls

**Adapter identity.** `gs_get_adapter_identity` returns the device UUID, the driver UUID, the LUID (`luid_valid` on
Windows), and the vendor and device IDs. Another API or process uses these to open the same GPU: DXGI
`EnumAdapterByLuid`, CUDA `cuDeviceGetUuid`, or Vulkan `VkPhysicalDeviceIDProperties`.

**Textures.**
* `gs_texture_create_exportable(w, h, format, layout, flags, &tex, &tex_uv)` creates a texture in a dedicated,
  exportable allocation. `GS_SHARED_SINGLE` gives one texture of `format`. `GS_SHARED_NV12` / `GS_SHARED_P010` give
  the two plane views, as `gs_texture_create_nv12`/`_p010` do (Y: R8/R16, UV: R8G8/RG16), so the colour conversion
  can render into them. The only meaningful flag is `GS_RENDER_TARGET`.
* `gs_texture_export(tex, type, &desc)` fills a `gs_shared_texture_desc` with a **new** handle plus everything an
  importer needs: size, format, layout, flags, allocation size and the exporter's device UUID. Either plane view
  exports the whole image.
* `gs_texture_import(&desc, &tex, &tex_uv)` imports from a description. The description comes from
  `gs_texture_export`, or the caller fills it for memory made by another API. For D3D11 types it needs `width`,
  `height`, `format` or `layout`, `flags` (`GS_RENDER_TARGET` if the D3D11 texture has `BIND_RENDER_TARGET`), and
  `keyed_mutex`. A non-zero `device_uuid` that isn't this adapter's is refused.

**Pitch-linear frame buffer** (`gs_sharedbuf_t`). This is GPU memory holding an NV12 or P010 frame: Y rows, then UV
rows. Each pitch is a multiple of 256 bytes, and `desc.offsets`/`desc.pitches` give the exact layout. On Windows NVENC
accepts zero-copy input only in this form (`CUDADEVICEPTR`, spike 3). `gs_sharedbuf_copy_texture(buf, tex_y)` records
one GPU copy of both planes of an NV12/P010 texture. `gs_sharedbuf_export` returns its handle and layout.

**Timelines** (`gs_timeline_t`, timeline semaphores).
* `gs_timeline_create(initial)` creates an exportable timeline. `gs_timeline_import(&handle)` imports one.
* `gs_timeline_export` returns a new handle.
* `gs_timeline_signal(t, v)` submits all work recorded so far and signals `v` once it has finished.
* `gs_timeline_wait(t, v)` makes work recorded *after* the call wait on the GPU. Earlier recorded work is submitted
  first and does not wait.
* `gs_timeline_get_value`, `gs_timeline_wait_cpu(t, v, timeout_ns)` (0, `ETIMEDOUT` or -1) and
  `gs_timeline_signal_cpu` (forward only) are the CPU side.

**Sync files** (Linux). `gs_sync_file_export` and `gs_sync_file_wait` exchange one-shot sync_files, for PipeWire
explicit sync, which the spikes showed must go through a sync_file in both directions.

## Ownership and lifetime of handles

* **Exported handles belong to the caller.** Every export call (`gs_texture_export`, `gs_sharedbuf_export`,
  `gs_timeline_export`, `gs_sync_file_export`) creates a new handle, and the caller closes it with
  `gs_shared_handle_close` (`CloseHandle` / `close`). Exporting twice gives two handles. Closing a handle never
  affects the object: the texture, buffer or timeline lives until it is destroyed.
* **Imports borrow the handle.** `gs_texture_import` and `gs_timeline_import` leave the handle with the caller, who
  may close it right after the import (the object keeps its own reference to the memory or semaphore). This is how
  Vulkan treats NT handles. Vulkan takes over imported fds, so the Linux backend must dup() them first, keeping the rule the same on Linux.
* **KMT handles are names, not references.** There is nothing to close, and `gs_shared_handle_close` only resets
  them. The texture stays valid only while its D3D11 creator keeps it alive.
* **Destruction is deferred**, as for every libobs-vulkan object: memory is freed once the GPU has finished the
  submissions that use it. The other side's references (its own imports) keep the memory alive independently.
* **Moving a handle to another process** is the transport's job. On Windows, `DuplicateHandle` it into the target
  process (the test does this, then sends the value over a pipe) and close the local copy. On Linux, send the fd with
  `SCM_RIGHTS`.

## Ordering and ownership of contents

Between this device's submissions, every external resource (exportable or imported) belongs to the external owner:
`VK_QUEUE_FAMILY_EXTERNAL` (`FOREIGN_EXT` for dmabufs) in `VK_IMAGE_LAYOUT_GENERAL`. The backend acquires it on its
first use in a submission, keeping the contents, and releases it at the end of every submission. That makes every
submission boundary a valid hand-off point. Whoever uses the resource next still has to be ordered after the work
that wrote it, through one of:

* a **timeline**: the writer signals `v` after its work, and the reader waits for `v` before its own (GPU or CPU wait);
* a **keyed mutex** (D3D11 producers, Windows only): see below;
* nothing: game capture's KMT textures have no sync today, just as on D3D11.

A new exportable texture or buffer is transitioned and released once at creation (with a short GPU wait). Its handle
therefore never leaves the device before the resource is in the hand-off state.

## Native objects (in-process only)

`gs_get_device_obj()` and `gs_texture_get_obj()` return `struct gs_vulkan_device` and `struct gs_vulkan_texture`
(`libobs/graphics/graphics-vulkan.h`) on the Vulkan renderer: instance, physical device, device, graphics queue and
its family/index; image, views, format, aspect, last recorded layout and size. They exist for code that runs **in
the OBS process** and wants to record Vulkan work against the renderer's own objects. They break the first design
rule on purpose and therefore stay out of this API: nothing out of process may use them (the handles mean nothing
there), and in-process code must not destroy them, may use the queue only with the graphics context entered, and
must order its work with the renderer itself (`gs_flush()` plus its own semaphores, or better a `gs_timeline_t`).
Code that may move to another process uses the exports above instead.

## Windows capture (win-capture)

Game capture and window capture run on the Vulkan renderer as on D3D11. The hook side (graphics-hook, inject-helper,
`graphics-hook-info.h`, the named objects and the shmem/shtex layouts) is unchanged, so OBS Studio's signed hook works
with Knurbs; `test_vulkan_win_capture` runs against both (below).

| Method | Path on Vulkan | Copies per frame in OBS | Sync |
|---|---|---|---|
| Game capture, shared texture (BGRA, BGRX, RGBA, R10G10B10A2, RGBA16F, typeless or not) | the hook's legacy D3D11 handle opened with `gs_texture_open_shared` and sampled directly | 0 (R10G10B10A2: the conversion pass D3D11 has too) | none, as on D3D11 (the hook's texture has no keyed mutex) |
| Game capture, memory (compatibility mode, or no shared textures) | unchanged CPU upload | 1 CPU upload, as on D3D11 | the hook's mutexes, unchanged |
| Window capture, Windows Graphics Capture | WGC delivers frames to a private D3D11 device on the renderer's adapter (by LUID); each frame is copied into one of three NT-handle, keyed-mutex textures of the frame's format (BGRA, or RGBA16F for HDR), imported once with `gs_texture_open_nt_shared` | 1 GPU copy (D3D11), as on D3D11 | keyed mutex, key 0 both ways: Vulkan holds the slot it draws until a newer frame is drawn; frames go to the other slots |
| Window capture, BitBlt | the compatibility path (BitBlt into a DIB, then upload): Vulkan has no GDI-compatible textures | 1 CPU upload (D3D11 BitBlts into the texture's DC instead) | none |

libobs-winrt (also used by display capture's WGC mode, which is still D3D11-only) keeps the private D3D11 device
while any capture uses it. WGC frames arrive on the graphics thread (its dispatcher queue), so the slot state needs no
lock. On a Vulkan device loss the D3D11 device and the capture session keep running; only the imported textures are
replaced, with the next frame. If the D3D11 device is lost too, or the adapter changed, the session is recreated.
`OBS_WINRT_TEST_HDR=1` makes WGC capture RGBA16F frames as on an HDR monitor (test hook).

Hook files: on start-up win-capture copies its own graphics-hook into `%ProgramData%\obs-studio-hook` when its version
is higher than the one there (`game-capture-file-init.c`, unchanged from OBS). Knurbs' hook is 1.8.8 and OBS Studio
31.0.2's is 1.8.6, so running Knurbs from its install directory replaces OBS Studio's signed hook in that shared
directory with Knurbs' unsigned one, for both programs.

## Texture encoders

On the Vulkan renderer, GPU encoding (`obs-video-gpu-encode.c`) hands frames to encoders as OS handles, never as
D3D11 share handles (Vulkan has none) or Vulkan objects. An encoder opts in with `OBS_ENCODER_CAP_SHARED_HANDLES`
and finds the frame in `encoder_texture::shared` (`struct encoder_texture_shared`, `obs-encoder.h`):

* `tex`: the NV12/P010 image (both planes, `GS_RENDER_TARGET`), exported once per image and identified by `tex_id`.
  The encode conversion targets and the encoder frames are exportable textures on Vulkan; they swap as before, so an
  image moves between frames and `tex_id` follows the image, not the frame.
* `buf`: with `OBS_ENCODER_CAP_SHARED_BUFFER` also, a pitch-linear copy (`gs_sharedbuf_t`) identified by `buf_id`.
  libobs records the copy right after the conversion, only while such an encoder is active.
* `ready` / `ready_value`: one timeline for the pool, signalled by the render thread once the frame (and its copy)
  is written. Read the frame only after it reached the value.
* `release` / `release_value`: a timeline per encoder (created in `start_gpu_encode`). The encoder signals it to at
  least `release_value` once it no longer reads the frame, in order (values only increase, so an encoder holding
  frames out of order signals up to just below the oldest one it still holds).

libobs keeps every handle open while the pool exists; encoders import them (imports borrow) and cache the imports by
id. A frame is reused only when every encoder it went to has released it (`gs_timeline_get_value` on the render
thread, no GPU or CPU wait). While encoders hold frames the pool grows instead, up to 80 frames, past which libobs
repeats the previous frame as it does when every frame is queued. A slow encoder therefore never stalls the render
queue, and one that stops or dies cannot hang it: `stop_gpu_encode` marks its holds as over. libobs hands a frame to
an encoder more than once when the renderer lags, so an encoder must expect the same `tex_id`/`buf_id` while an
earlier use of it is still in flight. Encoders without the cap get `shared == NULL` and must be done with the frame
when `encode_texture2` returns, as before. Exported memory is always a dedicated allocation (CUDA needs to know:
`CUDA_EXTERNAL_MEMORY_DEDICATED`).

| Encoder | Path | Copies per frame | Sync |
|---|---|---|---|
| knurbs-nvenc | `buf` imported into CUDA, registered as `CUDADEVICEPTR` | 1 (libobs: image to buffer) | CUDA waits `ready` on its stream (and the encode thread waits for the stream); CUDA signals `release` once the frame's bitstream is locked |
| AMF (obs-ffmpeg; `OBS_AMF_VULKAN_TEXTURE=0` forces the non-texture encoder) | `tex` imported as a VkImage into the encoder's own VkDevice and wrapped **once per image** with `CreateSurfaceFromVulkanNative`; that `AMFSurface` is resubmitted for the image's lifetime (a repeated frame still held by AMF gets a second one) | 0 in OBS (1 with a raw output active, as on D3D11); AMF's own conversion as on D3D11 | host waits on `ready` (`vkWaitSemaphores`); host signals `release` once AMF holds no reference to the frame's surface any more (polled on each encode call: `OnSurfaceDataRelease` does not fire while the encoder keeps its own reference) |

NVENC uses the buffer because on Windows an NV12 image imported as a CUDA array reads back garbage (spike 3). Its
CUDA device is the one whose UUID matches the renderer's adapter; on another GPU it reroutes to the non-texture
encoder, as before. A repeated frame is one buffer in several bitstream slots, so its NVENC mapping is counted.

AMF cannot create a Vulkan device itself (`InitVulkan(nullptr)` fails), so the encoder creates one on the
renderer's adapter (by device UUID) with every queue of every family, the device extensions AMF asks for
(`GetVulkanDeviceExtensions`), the external memory/semaphore extensions, every core feature, and the instance
extensions AMF needs: the surface extensions for `VK_KHR_swapchain`, and the KHR extensions whose function names AMF
looks up (`vkGetPhysicalDeviceProperties2KHR`, else `InitVulkan` fails). AMF submits to that device's queues from
its own threads, so the encoder itself never submits to a queue there: it only uses host waits and signals, which
keeps the rule that two threads never submit to the same `VkQueue`. With `OBS_VULKAN_VALIDATION=1` that device runs
under the validation layer too and logs its message count when destroyed. The capability probe's results are in
DXGI adapter order, so on Vulkan the adapter is looked up by LUID.

`test/cmocka/test_vulkan_gpu_encode.cpp` runs both end to end (see Tests).

## Device loss

`gs_register_loss_callbacks` works as on D3D11: when the device is lost (any `VK_ERROR_DEVICE_LOST`), the next safe
point (`gs_begin_frame`, `gs_is_present_ready`, the end of `gs_present`) calls every `device_loss_release`, recreates
the instance, the device and every object in place, then calls every `device_loss_rebuild` with the new
`struct gs_vulkan_device` (D3D11 passes its `ID3D11Device`). For this API that means:

* **Exportable textures and frame buffers get new memory.** Handles exported before the loss are dead; export again
  in `device_loss_rebuild` and hand the new handles over.
* **Imported textures and timelines can't come back by themselves**, because an import borrows its handle. They
  become placeholders (a texture of the same size and format, a local timeline at the last value seen); import
  again from the producer's new handles. Legacy KMT handles (`gs_texture_open_shared`) are opened again, as D3D11
  does, and stay placeholders if that fails.
* Everything else (static textures with their data, buffers, shaders, samplers, depth buffers, stage surfaces,
  swapchains, timers) keeps working without the caller doing anything. Render targets come back with undefined
  contents.

`OBS_VULKAN_TEST_DEVICE_LOST=N` makes the N-th submission report `VK_ERROR_DEVICE_LOST` (and every later one until
the rebuild); `test_vulkan_loss` uses it.

## Windows: the existing exports

* `gs_shared_texture_available()` is true when the last device created has `VK_KHR_external_memory_win32` and a
  valid LUID.
* `gs_texture_open_shared(kmt)` / `gs_texture_open_nt_shared(nt)` read the handle's D3D11 description (size,
  format, bind flags, keyed mutex). A legacy handle carries nothing else. They read it through a small D3D11 *probe*
  device, created on first use on the adapter with the Vulkan device's LUID, which holds no textures. Then they
  import with `D3D11_TEXTURE_KMT_BIT` / `D3D11_TEXTURE_BIT` into a dedicated allocation (every vendor reports
  `DEDICATED_ONLY`). BGRA, BGRX, RGBA, R10G10B10A2, RGBA16F, R8 and R8G8 are mapped. NV12/P010 handles are refused
  here because one `gs_texture_t` can't hold both planes; use `gs_texture_import`, which returns both plane views.
* `gs_texture_get_shared_handle(tex)` returns the handle the texture was opened from, as D3D11 does for opened
  textures. Textures created by Vulkan return `GS_INVALID_HANDLE`, because Vulkan can't create D3D11 share handles.
  So `GS_SHARED_TEX` / `GS_SHARED_KM_TEX` mean nothing on Vulkan, and libobs's encoder path (`obs-video.c`) gets -1
  from the keyed-mutex calls, as before.
* `gs_texture_acquire_sync(tex, key, ms)` flushes the recorded work, then acquires `key` with
  `VkWin32KeyedMutexAcquireReleaseInfoKHR` in a submission of its own, and returns once the texture is this
  device's (0, `ETIMEDOUT` or -1). `gs_texture_release_sync(tex, key)` releases it on the submission that holds the
  work recorded since. A **finite timeout** first waits on the CPU through the probe device's `IDXGIKeyedMutex` for the
  same resource and hands the key straight back. Vulkan then acquires with an infinite timeout, so a Vulkan acquire
  never times out (see driver issues). The only race is a third party that acquires the *same* key in between;
  two-party hand-offs use different keys in each direction.

## Driver issues found (RTX 4070 Laptop 621.24, Radeon 780M Adrenalin 26.11.2)

* **NVIDIA: D3D11 NV12 import is broken.** The chroma plane is assembled from the wrong place and the device is
  eventually lost (spike 2, `repro/d3d11-nv12-vulkan-import` on `vulkan-interop-spikes`). `gs_texture_import` refuses
  D3D11 NV12/P010 on NVIDIA and `gs_shared_texture_supported` reports false. D3D11 sources convert to RGB first (the
  decision of 2026-11-19). AMD imports them correctly.
* **NVIDIA: Vulkan-exported memory doesn't import in the process that exported it.** Importing into another VkDevice
  or the same one, or through a `DuplicateHandle` copy, returns `VK_ERROR_OUT_OF_DEVICE_MEMORY`. The parameters match
  (the validation layer agrees), and another process imports the same handle fine. Timelines import in-process
  without trouble. This doesn't affect the process-per-plugin model. In-process consumers on NVIDIA use CUDA or the
  device's own texture. The test asserts the failure, so a fixed driver shows up as a test failure.
* **AMD: AMF's Vulkan encoder encodes the wrong picture when each frame gets a new `AMFSurface`** (Adrenalin
  26.11.2, AMF runtime 1.5, Radeon 780M). For each new surface AMF exports the image's memory, opens it in PAL and
  caches that import, and now and then it reuses the cached import of an older surface instead: the encoder gets the
  picture of an earlier input (2 to 100+ frames back), complete and valid. Synchronisation, rate-control frame
  skipping (off in all of this), the plane view formats, AMF's semaphore mode and image reuse make no difference;
  `repro/amf-vulkan-stale-frame` shows it without OBS. The encoder therefore wraps each libobs image once and
  resubmits that surface for as long as the image lives (`texture-amf-vulkan.hpp`), so AMF imports each image once
  and nothing in its cache is recycled. The path is on by default; `OBS_AMF_VULKAN_TEXTURE=0` forces the non-texture
  encoder. AMF also creates `R8_UINT`/`R8G8_UINT` (`R16`/`R16G16` for P010) views of both planes, so those are in
  the format list of libobs's exportable NV12/P010 images and of the encoder's import. `AMFSurface::Duplicate` of
  such a surface crashes, and AMF's device then reports command buffers freed and fences reset while in use.
* **Keyed mutex timeouts in `vkQueueSubmit2` differ.** NVIDIA returns `VK_TIMEOUT` (a success code: the batch runs
  and signals). AMD returns `VK_SUCCESS` even though the acquire couldn't succeed. Hence the CPU-side timeout above.

## Tests

`test/cmocka/test_vulkan_interop.cpp` runs on Windows under the validation layer with synchronization validation and
expects no messages. It covers: D3D11 â†’ Vulkan BGRA through KMT and NT (legacy exports and `gs_texture_import`),
Vulkan writes read back by D3D11, keyed-mutex hand-off both ways with a timeout, D3D11 NV12 (exact on AMD, refused on
NVIDIA), export â†’ import on a second VkDevice ordered by a shared timeline (BGRA and NV12; NVIDIA asserts the driver
issue above), CPU and GPU timeline waits and signals, an `ID3D11Fence` as a timeline, the NV12/P010 frame buffer
layout, and a child process that receives the texture, NV12 texture and timeline handles by `DuplicateHandle` over a
pipe, checks them, writes and signals back. Pick the GPU with `VK_LOADER_DRIVERS_SELECT` (`*nv-vk64*`,
`*amd-vulkan64*`). `test_vulkan_tracker` covers the queue-family acquire and release, and `test_vulkan_exports`
checks that the null module exports every import.

`test/cmocka/test_vulkan_gpu_encode.cpp` encodes through a real encoder module on the real video pipeline: a
moving pattern carrying its frame number (top and bottom rows), a grey level and a bar derived from it, decoded back
with libavcodec. Every decoded frame must be whole (both numbers agree), current (grey and bar match the number)
and in order, the encoder must not have rerouted to its raw variant, and neither libobs's device nor the encoder's
own Vulkan device may report a validation message. `stall=N` makes every N-th render sleep for three frame times
(libobs then repeats frames), `overdraw=N` adds GPU load. NVENC runs H.264 NV12, HEVC P010, H.264 with lookahead
(the pool grows) and H.264 with stalls on the NVIDIA adapter. AMF runs H.264 NV12, HEVC P010, H.264 with stalls and
H.264 over many images (`OBS_AMF_VULKAN_TEST_HOLD=76` keeps frames held, so the pool grows to its limit and most
frames come in images AMF has not seen) on the AMD adapter; with `OBS_AMF_VULKAN_TEST_WRAP_EACH_FRAME=1` (a new
surface per frame, as before the fix) they show AMF's stale frames (see Driver issues). A missing ready wait could not be caught at a size the validation layer runs in reasonable
time: the GPU finishes a 640x352 frame before the encoder thread reads it.

`test/cmocka/test_vulkan_win_capture.cpp` starts a producer window (the test executable itself) showing four colour
quadrants that rotate every 200 ms, captures it through the real pipeline and reads the composited frames back as BGRA.
Every frame must show one of the two phases (8- and 10-bit: exact colours within 4 levels, so a missing or doubled
sRGB conversion fails; RGBA16F: the dominant channel), both must show up, the method must have taken the path above,
and the validation layer (with synchronization validation) must report nothing. Cases: game capture of a D3D11
swap chain in each format and in memory mode, with the producer on the renderer's adapter and the hook the producer
loaded checked against the data dir given; WGC of a D3D11 and of a GDI window, with RGBA16F frames, while the window
keeps resizing; BitBlt of a GDI window; game capture and WGC across a simulated device loss. Each runs on the NVIDIA
and the AMD adapter, and on the D3D11 renderer as a reference. `KNURBS_TEST_OBS_HOOK_DIR` (an OBS Studio
installation's `data/obs-plugins/win-capture`) adds the game capture cases against that hook.
