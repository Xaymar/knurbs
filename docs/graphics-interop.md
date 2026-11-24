# Graphics interop: external memory and synchronisation

libobs shares GPU memory and GPU sync with other graphics APIs (D3D11 producers, CUDA/NVENC, AMF), other devices
and other processes through the API in `libobs/graphics/graphics.h` ("External memory and synchronisation"). Its
backend side is a set of optional imports (`graphics-imports.c`, declared in `device-exports.h`). libobs-vulkan
implements them on Windows and Linux. libobs-d3d11, libobs-opengl and libobs-metal do not export them, so every call fails
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
| `GS_HANDLE_FD` | opaque fd exported by Vulkan | export (NV12/P010 render targets), import | export, import | Linux |
| `GS_HANDLE_DMABUF` | dmabuf, one fd per plane, with offsets, pitches, modifier | export (single plane; NV12/P010 copy targets via GBM), import | â€” | Linux |
| `GS_HANDLE_SYNC_FD` | sync_file, binary and one-shot | â€” | `gs_sync_file_*` | Linux |

`gs_shared_texture_supported(type, layout, format, import)` says whether a texture of that layout and format can be
imported from, or exported as, a type on the current device. The Linux rows needed no header change:
`gs_shared_texture_desc` carries per-plane fds, `offsets`, `pitches` and `modifier` (see the Linux section).

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
one GPU copy of both planes of an NV12/P010 texture; `gs_sharedbuf_convert(buf, src, conv)` instead writes it straight
from the RGB output in one compute dispatch (see "Compute YUV conversion" below). `gs_sharedbuf_export` returns its
handle and layout.

**Compute YUV conversion** (Vulkan, `libobs-vulkan/vulkan-yuv.cpp`). `gs_texture_convert_yuv(tex_y, src, conv)` and
`gs_sharedbuf_convert` convert RGB into NV12/P010 in one dispatch with the math of `format_conversion.effect` (same
colour vectors, sRGB/PQ/HLG transfers, left-sited chroma, P010 quantisation), replacing the two fragment passes and,
for the frame buffer, the copy. Images get per-plane storage views (r8/rg8, r16/rg16) of an image created with
`STORAGE` usage; the backend adds that usage, and `GS_SHADER_STORAGE` to the exported flags, only where the plane
formats support storage images (`test_vulkan_yuv_storage` probes the rule on every device). Importers create the
image with the exported flags, so AMF adds `STORAGE` too. `gs_yuv_compute_available(layout, buffer)` says which
targets work; `OBS_VULKAN_YUV_CONVERT=fragment` turns all of it off for comparison (`compute` or unset: on).
Output matches the fragment passes within one LSB (`test_vulkan_yuv_compute`, which also times both).

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
* **Imports borrow the handle.** `gs_texture_import`, `gs_timeline_import` and `gs_sync_file_wait` leave the handle
  with the caller, who may close it right after the import (the object keeps its own reference to the memory or
  semaphore). This is how Vulkan treats NT handles. Vulkan takes over imported fds, so the Linux backend imports a
  `dup()` of each, keeping the rule the same on Linux.
* **KMT handles are names, not references.** There is nothing to close, and `gs_shared_handle_close` only resets
  them. The texture stays valid only while its D3D11 creator keeps it alive.
* **Destruction is deferred**, as for every libobs-vulkan object: memory is freed once the GPU has finished the
  submissions that use it. The other side's references (its own imports) keep the memory alive independently.
* **Moving a handle to another process** is the transport's job. On Windows, `DuplicateHandle` it into the target
  process (the test does this, then sends the value over a pipe) and close the local copy. On Linux, send the fd with
  `SCM_RIGHTS`.

## Ordering and ownership of contents

Between this device's submissions, every external resource (exportable or imported) belongs to the external owner:
`VK_QUEUE_FAMILY_EXTERNAL`, or `VK_QUEUE_FAMILY_FOREIGN_EXT` for dmabufs (any driver or API may use them;
`vk_tracker::set_external` takes the owner), in `VK_IMAGE_LAYOUT_GENERAL`. The backend acquires it on its
first use in a submission, keeping the contents, and releases it at the end of every submission. That makes every
submission boundary a valid hand-off point. Whoever uses the resource next still has to be ordered after the work
that wrote it, through one of:

* a **timeline**: the writer signals `v` after its work, and the reader waits for `v` before its own (GPU or CPU wait);
* a **sync_file** (Linux): the writer exports one after its work, the reader waits on it (`gs_sync_file_*`, or
  `gs_sync_*` with DRM syncobj timeline points);
* a **keyed mutex** (D3D11 producers, Windows only): see below;
* nothing: game capture's KMT textures have no sync today, just as on D3D11.

A new exportable texture or buffer is transitioned and released once at creation (with a short GPU wait). Its handle
therefore never leaves the device before the resource is in the hand-off state.

## Linux

Extensions: `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf` with `VK_EXT_image_drm_format_modifier`,
`VK_KHR_external_semaphore_fd` and `VK_EXT_queue_family_foreign`, each used when the device has it. The render node
comes from `VK_EXT_physical_device_drm`; GBM (`libgbm.so.1`, loaded at run time) is opened on it on first use.

**One handle type per exportable texture.** A driver offers `OPAQUE_FD` and `DMA_BUF` only as separate allocations
(NVIDIA 580: each one's `compatibleHandleTypes` is itself alone), so `gs_texture_create_exportable` picks the type
from the layout and flags, and exporting as the other type fails:

| Texture | Allocation | Export as |
|---|---|---|
| single plane | Vulkan, `DRM_FORMAT_MODIFIER` tiling with every modifier that has the needed features; the driver picks (NVIDIA: a block-linear one, `LINEAR` cannot be rendered to) | `GS_HANDLE_DMABUF` |
| NV12/P010 with `GS_RENDER_TARGET` | Vulkan, optimal tiling | `GS_HANDLE_FD` (CUDA, other Vulkan devices and processes) |
| NV12/P010 without `GS_RENDER_TARGET` (a copy target) | a `LINEAR` dmabuf from GBM, imported | `GS_HANDLE_DMABUF` (VA-API, PipeWire, compositors) |

NVIDIA lays out an NV12 image that Vulkan itself exports as a "LINEAR" dmabuf differently from what it reports
(spike C), hence GBM for planar dmabufs. GBM's textures keep GBM's fd and export copies of it: importing with
`VkExportMemoryAllocateInfo` (to re-export through Vulkan) gives a separate allocation on NVIDIA (Driver issues).
`gs_shared_texture_supported(type, layout, format, false)` is true when either flag combination exports as `type`.
`gs_sharedbuf_t` is an opaque fd (CUDA). Timelines are opaque fds.

**Imports.** `GS_HANDLE_FD` needs the exporter's parameters and allocation size, as `GS_HANDLE_WIN32`.
`GS_HANDLE_DMABUF` needs an explicit modifier (`GS_SHARED_MODIFIER_INVALID`, an implicit modifier, is refused:
Vulkan has none) and one offset and pitch per memory plane of that modifier. All planes must be in one buffer (fds of
the same dmabuf); disjoint planes are refused. sRGB formats fall back to their UNORM format when the modifier refuses
`MUTABLE_FORMAT`. A render target import is refused when the modifier cannot be rendered to (NVIDIA: `LINEAR`).

**Sync files.** `gs_sync_file_export` signals a binary `SYNC_FD` semaphore from a submission after all work so far
and exports it; `gs_sync_file_wait` imports a copy temporarily and makes the next submission wait (fd -1: already
signalled). Vulkan requires the signals that exported work waits on to be submitted already
(VUID-VkSemaphoreGetFdInfoKHR-handleType-03254): do not export a sync_file while a `gs_timeline_wait` on a value
nobody has submitted a signal for is pending (a later CPU signal does not count).

**The existing Linux exports** (`linux-pipewire`) work on Vulkan on top of the same paths:

* `gs_texture_create_from_dmabuf` imports a single-plane dmabuf (`gs_texture_import` for NV12/P010, which needs two
  plane views). `gs_query_dmabuf_capabilities` lists the DRM formats that import with some modifier (never
  `GS_DMABUF_FLAG_IMPLICIT_MODIFIERS_SUPPORTED`), `gs_query_dmabuf_modifiers_for_format` their modifiers.
* `gs_sync_*` (PipeWire explicit sync) bridge DRM syncobjs through sync_files, because a Vulkan semaphore cannot
  import a syncobj (spike D). An acquire point is waited for on the CPU until it has a fence (`WAIT_AVAILABLE`),
  transferred to a binary syncobj and exported as a sync_file. A release point gets the sync_file of this device's
  work, imported into a binary syncobj and transferred to the point. The syncobjs are opened on the render node.
* `gs_texture_create_from_pixmap` returns NULL: X11 is not supported.

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

Game capture, window capture and display capture run on the Vulkan renderer as on D3D11. The hook side (graphics-hook, inject-helper,
`graphics-hook-info.h`, the named objects and the shmem/shtex layouts) is unchanged, and Knurbs ships OBS Studio's signed hook binaries rather than building its own
(see [game-capture-hook.md](game-capture-hook.md)); `test_vulkan_win_capture` runs against them (below).

| Method | Path on Vulkan | Copies per frame in OBS | Sync |
|---|---|---|---|
| Game capture, shared texture (BGRA, BGRX, RGBA, R10G10B10A2, RGBA16F, typeless or not) | the hook's legacy D3D11 handle opened with `gs_texture_open_shared` and sampled directly | 0 (R10G10B10A2: the conversion pass D3D11 has too) | none, as on D3D11 (the hook's texture has no keyed mutex) |
| Game capture, memory (compatibility mode, or no shared textures) | unchanged CPU upload | 1 CPU upload, as on D3D11 | the hook's mutexes, unchanged |
| Window capture, Windows Graphics Capture | WGC delivers frames to a private D3D11 device on the renderer's adapter (by LUID); each frame is copied into one of three NT-handle, keyed-mutex textures of the frame's format (BGRA, or RGBA16F for HDR), imported once with `gs_texture_open_nt_shared` | 1 GPU copy (D3D11), as on D3D11 | keyed mutex, key 0 both ways: Vulkan holds the slot it draws until a newer frame is drawn; frames go to the other slots |
| Window capture, BitBlt | the compatibility path (BitBlt into a DIB, then upload): Vulkan has no GDI-compatible textures | 1 CPU upload (D3D11 BitBlts into the texture's DC instead) | none |
| Display capture, DXGI duplication, monitor on the renderer's adapter | `dxgi-duplicator.cpp` (win-capture) duplicates the output with a private D3D11 device on that adapter and copies each new desktop image into one of three keyed-mutex NT-handle textures (BGRA, or RGBA16F through `DuplicateOutput1` for HDR), imported once | 1 GPU copy (D3D11), as on D3D11; none for a pointer-only update | keyed mutex, as for WGC |
| Display capture, DXGI duplication, monitor on another adapter | the private D3D11 device is on the monitor's adapter; each new image is copied into a staging texture, mapped one tick later and uploaded | 1 GPU copy + 1 readback + 1 CPU upload (D3D11: no duplication at all, "Automatic" picks WGC) | none (the map waits for the copy) |
| Display capture, Windows Graphics Capture | libobs-winrt's monitor capture, through the same private D3D11 device and slots as window capture | 1 GPU copy (D3D11), as on D3D11 | keyed mutex |

The private D3D11 device and the keyed-mutex texture set are `shared/d3d11-vulkan-bridge`, used by libobs-winrt and
by the duplicator. libobs-winrt (window capture and display capture's WGC mode) keeps its private D3D11 device
while any capture uses it. WGC frames arrive on the graphics thread (its dispatcher queue), so the slot state needs no
lock. On a Vulkan device loss the D3D11 device and the capture session keep running; only the imported textures are
replaced, with the next frame. If the D3D11 device is lost too, or the adapter changed, the session is recreated.
`OBS_WINRT_TEST_HDR=1` makes WGC capture RGBA16F frames as on an HDR monitor (test hook).

**Display capture (DXGI duplication).** libobs's `gs_duplicator` is part of libobs-d3d11 and duplicates with the
renderer's own D3D11 device, so on D3D11 display capture still uses it, unchanged. On Vulkan, win-capture's
`dxgi-duplicator.cpp` does the same with only OS handles crossing into libobs:

* The D3D11 device is created on the adapter that drives the monitor (found by enumerating every adapter's outputs),
  not on the renderer's. When that is the renderer's adapter (LUID match), frames go through the keyed-mutex slots;
  otherwise a D3D11 texture can't be imported into the renderer's Vulkan device, so they go through the CPU (table
  above). "Automatic" picks WGC when the monitor is not on the renderer's adapter or on a laptop on battery with
  several adapters, as on D3D11; WGC does the cross-adapter copy inside the OS.
* `IDXGIOutput5::DuplicateOutput1` with R16G16B16A16_FLOAT and B8G8R8A8 as libobs-d3d11 does (HDR monitors give
  scRGB, `GS_CS_709_SCRGB`, with the SDR white level from DisplayConfig). It needs a per-monitor-v2 DPI aware process
  (OBS's manifest); where it reports `DXGI_ERROR_UNSUPPORTED` the duplicator falls back to `DuplicateOutput` (SDR),
  where libobs-d3d11 fails.
* Every capture of a monitor shares one duplication, which takes at most one frame per video frame (D3D11: per
  `gs_begin_frame`). A frame whose `LastPresentTime` is 0 only moved the pointer and is not copied. The cursor is drawn
  by the source from GDI (`cursor-capture.c`) on both renderers.
* `DXGI_ERROR_ACCESS_LOST` (mode or resolution change, rotation, the secure desktop, a fullscreen switch) or any other
  failure ends the duplication; the source creates it again after 3 s, as on D3D11. Rotation is applied at draw time
  from the output's `DXGI_MODE_ROTATION`, as on D3D11.
* On a Vulkan device loss the D3D11 device and the duplication stay, and only the imports are replaced: the same NT
  handles are imported again (`slots::reimport`), and the D3D11 textures still hold the newest frame, so a static
  desktop (which sends no new frames) is drawn again at once. A slot Vulkan held when the device was lost can stay
  held: a keyed mutex acquired by a lost device is never released (still held after seconds, NVIDIA 621.24). Every
  slot is checked, not only the drawn one: a loss between the release of the drawn slot and the acquire of a newer
  one leaves the old slot held although it is no longer the drawn one (`test_vulkan_d3d11_bridge`). Then
  the slots are created again and the duplication is restarted on the same D3D11 device, whose first frame is the
  whole desktop. On the other-adapter path the last staged frame is uploaded again. If the renderer comes back on
  another adapter, the duplication is created again.
* `OBS_DXGI_DUPLICATOR_TEST_HDR=1` asks DXGI for RGBA16F images only (DXGI converts an SDR desktop) and treats the
  monitor as HDR (test hook).

On Vulkan the source registered as `monitor_capture` is now this display capture (DXGI/WGC), as on D3D11, instead of
the GDI monitor capture (`monitor-capture.c`), which Vulkan used before. Scenes made on D3D11 therefore keep working;
the settings of a `monitor_capture` created on Vulkan before (the GDI source's `monitor` index) are not read by it.

Hook files: on start-up win-capture copies its graphics-hook into `%ProgramData%\obs-studio-hook` only when it is signed
by OBS Project, LLC and newer than the one there, and uses the one there only when it is signed (see
[game-capture-hook.md](game-capture-hook.md)). Knurbs ships OBS Studio 33.0.3's signed hook (1.8.8), so it no longer
replaces OBS Studio's signed hook in that shared directory with an unsigned one.

## Texture encoders

On the Vulkan renderer, GPU encoding (`obs-video-gpu-encode.c`) hands frames to encoders as OS handles, never as
D3D11 share handles (Vulkan has none) or Vulkan objects. An encoder opts in with `OBS_ENCODER_CAP_SHARED_HANDLES`
and finds the frame in `encoder_texture::shared` (`struct encoder_texture_shared`, `obs-encoder.h`):

* `tex`: the NV12/P010 image (both planes, `GS_RENDER_TARGET`), exported once per image and identified by `tex_id`.
  The encode conversion targets and the encoder frames are exportable textures on Vulkan; they swap as before, so an
  image moves between frames and `tex_id` follows the image, not the frame.
* `buf`: with `OBS_ENCODER_CAP_SHARED_BUFFER` also, a pitch-linear frame (`gs_sharedbuf_t`) identified by `buf_id`,
  written only while such an encoder is active: by compute straight from the RGB output (`gs_sharedbuf_convert`), or
  where that is unavailable as a copy of the image right after the conversion. When only such encoders read the
  frame (no raw output, no image encoder), the image is not converted at all.
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
| knurbs-nvenc | `buf` imported into CUDA, registered as `CUDADEVICEPTR` | 0 (compute writes the buffer from the RGB output; 1, image to buffer, with `OBS_VULKAN_YUV_CONVERT=fragment`) | CUDA waits `ready` on its stream (and the encode thread waits for the stream); CUDA signals `release` once the frame's bitstream is locked |
| AMF (knurbs-amf; `OBS_AMF_VULKAN_TEXTURE=0` forces the non-texture encoder) | `tex` imported as a VkImage into the encoder's own VkDevice and wrapped **once per image** with `CreateSurfaceFromVulkanNative`; that `AMFSurface` is resubmitted for the image's lifetime (a repeated frame still held by AMF gets a second one) | 0 in OBS (1 with a raw output active, as on D3D11); the image is written by one compute dispatch where the device allows storage plane views, else by the fragment passes; AMF's own conversion as on D3D11 | host waits on `ready` (`vkWaitSemaphores`); host signals `release` once AMF holds no reference to the frame's surface any more (polled on each encode call: `OnSurfaceDataRelease` does not fire while the encoder keeps its own reference) |

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

## Driver issues found (RTX 4070 Laptop 621.24, Radeon 780M Adrenalin 26.11.2; Linux: Tesla P100, NVIDIA 580.196.07)

* **NVIDIA Linux: a dmabuf imported with `VkExportMemoryAllocateInfo` is not imported.** `vkAllocateMemory` succeeds
  but allocates new memory: Vulkan's writes never reach the dmabuf, and `vkGetMemoryFdKHR` returns a different
  dmabuf. Validation reports nothing, and `exportFromImportedHandleTypes` claims `DMA_BUF`
  (`repro/dmabuf-import-with-export-info`). libobs-vulkan never re-exports imported memory.
* **NVIDIA Linux: an NV12 image Vulkan exports as a `LINEAR` dmabuf is not laid out as reported** (spike C on
  `vulkan-interop-spikes-linux`): GBM allocates planar dmabufs.
* **NVIDIA Linux: no P010 in GBM** (`gbm_device_is_format_supported` is false): P010 copy targets are opaque fds.
* **NVIDIA Linux: in-process import works.** Unlike on Windows, opaque fds and dmabufs exported by this process
  import into a second VkDevice of the same process.

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
  resubmits that surface for as long as the image lives (`plugins/knurbs-amf/amf-encoder-vulkan.hpp`), so AMF imports each image once
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

`test/cmocka/test_vulkan_interop_linux.cpp` is its Linux counterpart, under the validation layer with synchronization
validation, expecting no messages: the support matrix; the dmabuf formats and modifiers linux-pipewire is offered;
export → import on a second VkDevice ordered by a shared opaque-fd timeline (BGRA as a dmabuf with the driver's
modifier, NV12 as an opaque fd); GBM NV12/P010 copy targets written by Vulkan and read through `mmap` at the
described offsets and pitches (P010 reports itself unchecked where GBM has no P010); LINEAR BGRA and NV12 GBM
buffers written on the CPU and imported (through `gs_texture_import` and `gs_texture_create_from_dmabuf`); CPU
timelines; sync_files both ways between two devices; DRM syncobj acquire, release and CPU-signalled points
(`gs_sync_*`); the frame buffer as an opaque fd; and a child process that gets the BGRA dmabuf, the NV12 opaque fd
and the timeline over `SCM_RIGHTS`, checks them, writes and signals back. `test_vulkan_loss` rebuilds an exportable
dmabuf texture, and `test_vulkan_tracker` covers the foreign owner.

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
loaded checked against the shipped one and for OBS Project's signature; WGC of a D3D11 and of a GDI window, with RGBA16F frames, while the window
keeps resizing; BitBlt of a GDI window; game capture and WGC across a simulated device loss; display capture of the primary monitor (DXGI and WGC; a topmost
borderless window covers it with the pattern and hides the pointer) in SDR and RGBA16F, across a device loss, and
across a change to a smaller resolution and back (`_mode`, which restores the original mode even when the test fails).
The display cases need an unlocked, awake desktop (on the lock screen DXGI sends nothing and WGC sends the lock screen)
and run per-monitor DPI aware, as OBS does. Each runs on the NVIDIA and the AMD adapter, and on the D3D11 renderer as a
reference. The game capture cases run against the hook Knurbs ships, OBS Studio's signed release binaries.
