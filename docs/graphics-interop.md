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
| `GS_HANDLE_WIN32_D3D11` | NT handle of a D3D11 texture (`MISC_SHARED_NTHANDLE`) | import | — | Windows |
| `GS_HANDLE_WIN32_D3D11_KMT` | legacy global share handle (`MISC_SHARED`, game capture) | import | — | Windows |
| `GS_HANDLE_WIN32_D3D12_FENCE` | NT handle of an `ID3D12Fence` / `ID3D11Fence` | — | import | Windows |
| `GS_HANDLE_FD` | opaque fd exported by Vulkan | export, import | export, import | Linux |
| `GS_HANDLE_DMABUF` | dmabuf, one fd per plane, with offsets, pitches, modifier | import (export: single plane) | — | Linux |
| `GS_HANDLE_SYNC_FD` | sync_file, binary and one-shot | — | `gs_sync_file_*` | Linux |

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
* **Keyed mutex timeouts in `vkQueueSubmit2` differ.** NVIDIA returns `VK_TIMEOUT` (a success code: the batch runs
  and signals). AMD returns `VK_SUCCESS` even though the acquire couldn't succeed. Hence the CPU-side timeout above.

## Tests

`test/cmocka/test_vulkan_interop.cpp` runs on Windows under the validation layer with synchronization validation and
expects no messages. It covers: D3D11 → Vulkan BGRA through KMT and NT (legacy exports and `gs_texture_import`),
Vulkan writes read back by D3D11, keyed-mutex hand-off both ways with a timeout, D3D11 NV12 (exact on AMD, refused on
NVIDIA), export → import on a second VkDevice ordered by a shared timeline (BGRA and NV12; NVIDIA asserts the driver
issue above), CPU and GPU timeline waits and signals, an `ID3D11Fence` as a timeline, the NV12/P010 frame buffer
layout, and a child process that receives the texture, NV12 texture and timeline handles by `DuplicateHandle` over a
pipe, checks them, writes and signals back. Pick the GPU with `VK_LOADER_DRIVERS_SELECT` (`*nv-vk64*`,
`*amd-vulkan64*`). `test_vulkan_tracker` covers the queue-family acquire and release, and `test_vulkan_exports`
checks that the null module exports every import.
