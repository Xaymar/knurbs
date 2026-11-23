# A dmabuf imported with VkExportMemoryAllocateInfo is not the imported buffer (NVIDIA)

A standalone reproducer with no OBS code: Vulkan, GBM and libdrm only.

## Summary

`VkExternalImageFormatProperties` reports `exportFromImportedHandleTypes = DMA_BUF` for dmabuf images, so memory
imported from a dmabuf may be exported again if the import chains `VkExportMemoryAllocateInfo` (which
`vkGetMemoryFdKHR` requires, VUID-VkMemoryGetFdInfoKHR-handleType-00671). On NVIDIA, chaining it makes
`vkAllocateMemory` succeed **without importing**: the driver allocates new memory, what Vulkan writes never reaches
the dmabuf, and `vkGetMemoryFdKHR` returns a dmabuf of that new memory (another inode). No error, and the
validation layer reports nothing.

Without `VkExportMemoryAllocateInfo` the same import works. libobs-vulkan therefore never re-exports imported
memory: its GBM-allocated NV12/P010 textures keep GBM's dmabuf fd and export copies of it.

## Environment

| | |
|---|---|
| GPU | Tesla P100-PCIE-16GB (Pascal), headless |
| Driver | NVIDIA 580.196.07, Vulkan 1.4.312 |
| GBM | Mesa 26.0.8 libgbm, backend nvidia-drm_gbm.so |
| Kernel | 6.12.107 |

## Run

```sh
cmake -S . -B build -G Ninja && ninja -C build && build/repro [/dev/dri/renderD128]
```

A 256x128 GBM `NV12` buffer (`GBM_BO_USE_LINEAR`) is imported as `G8_B8R8_2PLANE_420_UNORM` with
`DRM_FORMAT_MOD_LINEAR` and GBM's plane layout, written with `vkCmdCopyImage` from R8/R8G8 images, released to
`VK_QUEUE_FAMILY_FOREIGN_EXT`, and read back through `mmap` of a dmabuf fd:

```
reexport 0 (len 131072), mutable+extended+list 1, write via vkCmdCopyImage, release to FOREIGN: 0/192 bad rows
reexport 1 (len 131072), mutable+extended+list 1, write via vkCmdCopyImage, release to FOREIGN: 192/192 bad rows
reexport 2 (len 131072), mutable+extended+list 1, write via vkCmdCopyImage, release to FOREIGN: 0/192 bad rows
   ... and the GBM fd itself: 128/128 bad Y rows
   GBM fd inode 7701, re-exported fd inode 7702
```

- `reexport 0`: plain import, read through GBM's fd: correct.
- `reexport 1`: import with `VkExportMemoryAllocateInfo`, read through GBM's fd: nothing written arrived.
- `reexport 2`: the same import, read through the fd `vkGetMemoryFdKHR` returned: correct, but that is a
  different dmabuf (inode), and GBM's buffer is still untouched.

The same happens without `MUTABLE_FORMAT`/`EXTENDED_USAGE` and with `vkCmdCopyBufferToImage` (edit the loop
bounds in `main.cpp`). Run under `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` it reports nothing.
