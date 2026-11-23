# AMF on a Vulkan context encodes the picture of an earlier input

A standalone reproducer with no OBS code. It needs only Vulkan, the AMF SDK headers (the runtime comes from the
driver) and libavcodec to decode and check the stream.

## Summary

When an AMF context is initialised **only** with `AMFContext1::InitVulkan` and input images are wrapped with
`CreateSurfaceFromVulkanNative`, the encoder sometimes encodes an image it was given earlier instead of the current
one, typically 2 to 30 frames back and up to 100+.

The reason is in AMF's zero-copy interop. For each new `AMFSurface`, AMF exports the image's `VkDeviceMemory`
(`vkGetMemoryWin32HandleKHR`), opens it in PAL (`CreateNativeFromWinResource`), closes the handle, and caches the
PAL image. Sometimes it **skips that import and reuses the PAL image of an older surface**: AMF's trace shows the
encode job patched with the GPU address of an earlier frame's image. In the run below, AMF exported memory for
102 of the 120 frames, and the other 18 are exactly the 18 stale frames. The cache key collides even when every
`VkImage`, `VkDeviceMemory` and `AMFVulkanSurface` is unique and alive (`--ring 0 --keep`). That points to a key
derived from an AMF-internal object that gets freed and reallocated. It is not the exported handle value and not
consistently the `AMFSurface` address.

It is **not** a synchronisation problem. Waiting for the producer with a fence, a timeline, `vkQueueWaitIdle`, or
even `vkDeviceWaitIdle` (with AMF's queue locked) before `SubmitInput` makes no difference. The bad pictures are
always complete, valid earlier frames, never torn or blank ones.

## Environment

| | |
|---|---|
| GPU | AMD Radeon 780M (RDNA 3, Phoenix), laptop with an RTX 4070 as second GPU |
| Driver | Adrenalin 26.11.2: Vulkan "AMD proprietary driver 26.11.2 (LLPC)", Vulkan 1.4.359; WDDM 32.0.32029.2010 |
| AMF runtime | 1.5.3.0 (`amfrt64.dll` → `amfrtdrv64.dll`, build tree `gfx\six\26.20`) |
| AMF SDK headers | 1.5.0 |
| OS | Windows 10 Pro 22H2 (10.0.19045) |

## Build

```
cmake -S . -B build -D AMF_INCLUDE_DIR=<AMF>/amf/public/include -D FFMPEG_DIR=<ffmpeg dev build> [-D VULKAN_HEADERS_DIR=<Vulkan-Headers>]
cmake --build build --config Release
```

* `AMF_INCLUDE_DIR`: the directory that contains `core/Factory.h` (GPUOpen's `amf/public/include`, or obs-deps'
  `include/AMF`). Without it, the AMF SDK is cloned.
* `FFMPEG_DIR`: any FFmpeg dev build with `include/` and `lib/avcodec.lib`, `lib/avutil.lib`. The DLLs in its
  `bin/` are copied next to the executable. With obs-deps, also copy `bin/zlib.dll` and the rest of `bin/*.dll`.
* Vulkan headers: `VULKAN_HEADERS_DIR`, or the Vulkan SDK's, or they are downloaded. `vulkan-1.dll` and
  `amfrt64.dll` are loaded at run time.

## Run

```
amf-vulkan-stale-frame --producer same                    # the bug, intermittently (~25% of runs at 120 frames)
amf-vulkan-stale-frame --producer same --ring 0 --keep    # amplified: every run, ~20-40 stale frames of 120
amf-vulkan-stale-frame --producer same --reuse-surface    # workaround: always clean
.\sweep.ps1 -Runs 10 -Configs "--producer same", "--producer same --reuse-surface"
```

Each frame is an NV12 `VkImage` (`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`, optimal tiling, dedicated, exportable
memory, as the OBS plugin creates it). Its frame number is written into it as 16 big luma blocks: 12 bits of
number and a 4-bit check. Below the blocks there is a moving gradient. The image is wrapped with
`CreateSurfaceFromVulkanNative` and submitted to an H.264 (or HEVC) encoder: transcoding usage, CBR 20 Mbit/s,
pre-encode on, no B-frames, as the OBS plugin sets it up. The stream is decoded with libavcodec and every picture
is checked against the number it should carry.

**Expected:** `PASS: 0 stale ... 120/120 decoded`

**Actual** (`--producer same --ring 0 --keep --spy`, 120 frames):

```
encoded 120 frames in 0.25 s, 120 packets, 120 decoded, AMF held up to 2 input surfaces, 0 images destroyed
  frame 34: shows frame 32 (-2)
  frame 36: shows frame 28 (-8)
  frame 42: shows frame 29 (-13)
  frame 48: shows frame 27 (-21)
  ...
  frame 114: shows frame 11 (-103)
  frame 116: shows frame 11 (-105)
spy: AMF exported 102 memory handles (0 with a value seen before), closed 102
FAIL: 18 stale (up to 105 frames back), 0 garbled, 0 blank, 120/120 decoded
```

The exit code is 0 for PASS, 1 for FAIL and 2 for a setup error. `--help` lists every switch.

## What flips it

10 runs of 120 frames each, unless noted. "Amplified" is `--producer same --ring 0 --keep`: a fresh image every
frame, never destroyed.

| Configuration | Failing runs | Notes |
|---|---|---|
| `--producer same` (ring of 8, timeline host wait) | 2/10 | 1–3 stale frames per failing run, 2–5 back |
| … `--sync fence` / `idle` / `devidle` / `gpu` (semaphore in `AMFVulkanSurface::Sync`) / `none` | all fail on the amplified base (6/6 each) | sync is irrelevant |
| … `--stall-at 30 --stall-ms 300 --stall-every 20` | 2/10 | |
| `--ring 1`, `--ring 2` | 0/6 each | a stale cache hit can only point at the same image or its twin |
| `--ring 4` / `--ring 16` | 1/6 / 5/6 | more distinct images, more collisions |
| `--ring 0` (fresh images, destroyed on release) | 6/6 | |
| amplified | 6/6 (~25 stale per run) | |
| amplified with `--format-list 0`, `--tiling linear`, `--pqueue compute1`, `--pqueue amf` (the producer on AMF's queue, under `LockVulkan`), `--amf-timeline`, `--preencode 0`, `--usage lowlatency`, `--codec hevc`, `--pace 16`, `--busy 20`, `--rt` | 6/6 each | none of these matter |
| amplified `--dedicated 0` | – | AMF rejects the surface (SubmitInput fails) |
| **amplified `--reuse-surface`** | **0/12** | one `AMFSurface` per image, made once and resubmitted |
| `--reuse-surface` (ring of 8), also with stalls, also 10 × 600 frames | **0/30** | |
| **amplified `--dx11`** (`InitDX11` on the same adapter, then `InitVulkan`) | **0/12** | AMF exports nothing: it copies |
| **amplified `--export 0`** (input memory not exportable) | **0/12** | the export fails and AMF copies |
| `--input alloc` (`AllocSurface(AMF_MEMORY_VULKAN)` + copy into it) | 0/8 | AMF's own pooled surfaces |

This is why VES never saw it. VES initialises its AMF context with **D3D11 as well as Vulkan** and copies into
surfaces from `AllocSurface(AMF_MEMORY_VULKAN)`. Both take AMF off the export/open/cache path.

1080p throughput (`--size 1920x1088 --frames 600 --producer same`, wall time of the whole encode, two runs each):

| | time | stale |
|---|---|---|
| baseline | 1.44 s | yes |
| `--reuse-surface` | 1.38 s | no |
| `--input alloc` | 1.40 s | no |
| `--dx11` | 1.52 s | no |
| `--export 0` | 2.30 s | no |

## Workaround

Create the `AMFSurface` for an image **once** (`CreateSurfaceFromVulkanNative`) and resubmit that same object every
time the image is encoded, for as long as the image lives. Do not wrap the image again per frame. AMF then imports
each image's memory once, and nothing in its cache gets recycled. This keeps zero-copy and is the fastest variant.

AMF holds a reference while it uses the surface, and the observer's `OnSurfaceDataRelease` does not fire while the
application keeps its own reference. So "AMF is done with it" is detected from the reference count: after
`Acquire()`, `Release()` returns more than 1 while AMF still holds the surface. Before reuse, wait on
`AMFVulkanSurface::Sync` of the surface's native struct (`GetPlaneAt(0)->GetNative()` → `AMFVulkanView::pSurface`)
if `bSubmitted` is set, and take the layout from its `eCurrentLayout`.

Alternatives that also work but cost a copy: `InitDX11` the context too (+6%), or non-exportable input memory (+60%).

## Limits of this reproducer

* `--producer separate` (the default) puts the images on a second `VkDevice`, export → import, as libobs does. AMF
  then fails `SubmitInput` in PAL (`Pal::IDevice::GetExternalSharedImageSizes() failed`,
  `CreateNativeFromWinResource() failed`). That happens with a minimal or a fully featured producer device and with
  or without `COLOR_ATTACHMENT` usage, so this mode cannot show the bug. The plugin's libobs-imported frames do
  get through, and the bug shows the same way there. `--producer same` takes the same AMF path:
  export → PAL open → cache.
* `--spy` IAT-patches `amfrtdrv64.dll`'s `GetProcAddress` and `CloseHandle` to log AMF's memory exports.
  `--no-close` keeps those handles open. It is diagnostic only.

## AMF trace excerpt

`--trace` writes AMF's debug trace to `amf-trace.log`. For each encode job, the fifth `BaseAddress` and the
`ChromaAddress` are the input picture. From a 40-frame amplified run in which frames 20, 32 and 33 came out as
frames 14, 24 and 31:

```
frame  14 luma 2:5080000 chroma 2:5170000
...
frame  19 luma 2:5f80000 chroma 2:6070000
frame  20 luma 2:5080000 chroma 2:5170000  <-- frame 14's image
frame  21 luma 2:6400000 chroma 2:64f0000
...
frame  31 luma 2:8200000 chroma 2:82f0000
frame  32 luma 2:6d00000 chroma 2:6df0000  <-- frame 24's image
frame  33 luma 2:8200000 chroma 2:82f0000  <-- frame 31's image
```

The raw lines of one job (frame 37 of that run, jobID = frame + 1):

```
[EncodeQueuePalImpl]   Debug: *SubmitInput():jobID=38
[EncodeQueuePalImpl]   Debug: PatchIbFromImageResource():BaseAddress=2:dc0000
[EncodeQueuePalImpl]   Debug: PatchIbFromImageResource():BaseAddress=2:3b80000
[EncodeQueuePalImpl]   Debug: PatchIbFromImageResource():BaseAddress=2:1900000
[EncodeQueuePalImpl]   Debug: PatchIbFromImageResource():BaseAddress=2:21eb000
[EncodeQueuePalImpl]   Debug: PatchIbFromImageResource():BaseAddress=2:9100000
[EncodeQueuePalImpl]   Debug: PatchIbFromImageResource():ChromaAddress=2:91f0000
```

AMF logs no error or warning for the bad frames. When it rejects an import (separate producer), it logs:

```
[AMFDevicePALImpl]   Error: ...\runtime\src\core\DevicePALImpl.cpp(3360):PAL failed with result = -0x00000020: Pal::IDevice::GetExternalSharedImageSizes() failed
[AMFDevicePALImpl]   Error: ...\runtime\src\core\DevicePALImpl.cpp(1578):AMF_ERROR 1 : AMF_FAIL: CreateNativeFromWinResource() failed
[AMFSurfaceImpl]   Error: ...\runtime\src\core\SurfaceImpl.cpp(390):Assertion failed:Convert() - InteropSurface failed
[AMFEncoderCoreH264]   Error: ...EncoderCoreH264Impl.cpp(711):AMF_ERROR 1 : AMF_FAIL: SubmitInput(): Failed to move surface to a different GPU
```
