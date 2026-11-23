// A dmabuf imported into Vulkan with VkExportMemoryAllocateInfo chained (to export it again later) is not the
// imported buffer on NVIDIA 580.196.07: what Vulkan writes does not reach the dmabuf, and vkGetMemoryFdKHR returns
// a different dmabuf (another inode). Without VkExportMemoryAllocateInfo the import works.
//
// A GBM LINEAR NV12 buffer is imported with its explicit layout, written by vkCmdCopyImage from R8/R8G8 images, and
// read back on the CPU through mmap. Case 0: plain import, read through the GBM fd. Case 1: import with
// VkExportMemoryAllocateInfo (DMA_BUF, which exportFromImportedHandleTypes allows), read through the GBM fd. Case 2:
// the same, read through the fd vkGetMemoryFdKHR returns, and the GBM fd checked again.
//
// Build: g++ main.cpp -lvulkan -lgbm -I/usr/include/libdrm -o repro   Run: ./repro [/dev/dri/renderD128]
#include <vulkan/vulkan.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define CK(x) do { VkResult r_ = (x); if (r_) { printf("%s = %d\n", #x, r_); exit(1);} } while (0)
VkInstance inst;
VkPhysicalDevice phys;
VkDevice dev;
VkQueue q;
uint32_t W = 256, H = 128;
uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
			return i;
		}
	}
	return 0;
}
VkCommandPool pool;
VkCommandBuffer begin()
{
	VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	ai.commandPool = pool;
	ai.commandBufferCount = 1;
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(dev, &ai, &cb);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	vkBeginCommandBuffer(cb, &bi);
	return cb;
}
void submit(VkCommandBuffer cb)
{
	vkEndCommandBuffer(cb);
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	vkQueueSubmit(q, 1, &si, 0);
	vkQueueWaitIdle(q);
}
void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout o, VkImageLayout n, uint32_t srcq = VK_QUEUE_FAMILY_IGNORED,
	     uint32_t dstq = VK_QUEUE_FAMILY_IGNORED)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = o;
	b.newLayout = n;
	b.srcQueueFamilyIndex = srcq;
	b.dstQueueFamilyIndex = dstq;
	b.image = img;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0,
			     1, &b);
}
int main(int argc, char **argv)
{
	VkApplicationInfo ap{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ap.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &ap;
	CK(vkCreateInstance(&ici, 0, &inst));
	uint32_t n = 1;
	vkEnumeratePhysicalDevices(inst, &n, &phys);
	const char *ext[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
			     "VK_EXT_image_drm_format_modifier", "VK_EXT_queue_family_foreign"};
	float pr = 1;
	VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qi.queueCount = 1;
	qi.pQueuePriorities = &pr;
	VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	f11.samplerYcbcrConversion = 1;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f11};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qi;
	dci.enabledExtensionCount = 4;
	dci.ppEnabledExtensionNames = ext;
	CK(vkCreateDevice(phys, &dci, 0, &dev));
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	vkCreateCommandPool(dev, &pci, 0, &pool);
	int dfd = open(argc > 1 ? argv[1] : "/dev/dri/renderD128", O_RDWR);
	gbm_device *g = gbm_create_device(dfd);
	// pattern
	std::vector<uint8_t> pat(W * H * 3 / 2);
	for (size_t i = 0; i < pat.size(); ++i) {
		pat[i] = (uint8_t)(i * 7 + i / 256);
	}
	// staging buffer
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = pat.size();
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VkBuffer sb;
	vkCreateBuffer(dev, &bci, 0, &sb);
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, sb, &mr);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex =
		memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory sm;
	vkAllocateMemory(dev, &mai, 0, &sm);
	vkBindBufferMemory(dev, sb, sm, 0);
	void *sp;
	vkMapMemory(dev, sm, 0, VK_WHOLE_SIZE, 0, &sp);
	memcpy(sp, pat.data(), pat.size());
	// optimal R8 / RG8 sources filled from the buffer
	auto mkimg = [&](VkFormat f, uint32_t w, uint32_t h) {
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = f;
		ii.extent = {w, h, 1};
		ii.mipLevels = ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VkImage im;
		vkCreateImage(dev, &ii, 0, &im);
		VkMemoryRequirements r;
		vkGetImageMemoryRequirements(dev, im, &r);
		VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		a.allocationSize = r.size;
		a.memoryTypeIndex = memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory m;
		vkAllocateMemory(dev, &a, 0, &m);
		vkBindImageMemory(dev, im, m, 0);
		return im;
	};
	VkImage sy = mkimg(VK_FORMAT_R8_UNORM, W, H), suv = mkimg(VK_FORMAT_R8G8_UNORM, W / 2, H / 2);
	{
		auto cb = begin();
		barrier(cb, sy, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		barrier(cb, suv, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy c{};
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		c.imageExtent = {W, H, 1};
		vkCmdCopyBufferToImage(cb, sb, sy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		c.bufferOffset = W * H;
		c.imageExtent = {W / 2, H / 2, 1};
		vkCmdCopyBufferToImage(cb, sb, suv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		barrier(cb, sy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		barrier(cb, suv, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		submit(cb);
	}
	auto PFN_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
	for (int mutable_ = 1; mutable_ < 2; ++mutable_) {
		for (int via_image = 1; via_image < 2; ++via_image) {
			for (int foreign = 1; foreign < 2; ++foreign) {
				for (int reexp = 0; reexp < 3; ++reexp) {
					gbm_bo *bo = gbm_bo_create(g, W, H, GBM_FORMAT_NV12, GBM_BO_USE_LINEAR);
					int fd = gbm_bo_get_fd(bo);
					VkSubresourceLayout pl[2] = {};
					for (int p = 0; p < 2; ++p) {
						pl[p].offset = gbm_bo_get_offset(bo, p);
						pl[p].rowPitch = gbm_bo_get_stride_for_plane(bo, p);
					}
					VkImageDrmFormatModifierExplicitCreateInfoEXT ex{
						VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
					ex.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
					ex.drmFormatModifierPlaneCount = 2;
					ex.pPlaneLayouts = pl;
					VkExternalMemoryImageCreateInfo emi{
						VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ex};
					emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
					VkFormat views[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
							     VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT};
					VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
								       &emi};
					fl.viewFormatCount = 4;
					fl.pViewFormats = views;
					VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
							     mutable_ ? (void *)&fl : (void *)&emi};
					ii.imageType = VK_IMAGE_TYPE_2D;
					ii.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
					ii.extent = {W, H, 1};
					ii.mipLevels = ii.arrayLayers = 1;
					ii.samples = VK_SAMPLE_COUNT_1_BIT;
					ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
					ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
						   VK_IMAGE_USAGE_SAMPLED_BIT;
					if (mutable_) {
						ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
							   VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
					}
					VkImage img;
					CK(vkCreateImage(dev, &ii, 0, &img));
					VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
					PFN_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
					VkMemoryRequirements ir;
					vkGetImageMemoryRequirements(dev, img, &ir);
					VkMemoryDedicatedAllocateInfo dai{
						VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
					dai.image = img;
					VkExportMemoryAllocateInfo ea{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
								      &dai};
					ea.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
					VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
								    reexp ? (void *)&ea : (void *)&dai};
					imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
					imp.fd = dup(fd);
					VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
					ai.allocationSize = ir.size;
					ai.memoryTypeIndex = memtype(ir.memoryTypeBits & fp.memoryTypeBits, 0);
					VkDeviceMemory m;
					CK(vkAllocateMemory(dev, &ai, 0, &m));
					CK(vkBindImageMemory(dev, img, m, 0));
					auto cb = begin();
					barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
					if (via_image) {
						VkImageCopy c{};
						c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
						c.dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
						c.extent = {W, H, 1};
						vkCmdCopyImage(cb, sy, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img,
							       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
						c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
						c.extent = {W / 2, H / 2, 1};
						vkCmdCopyImage(cb, suv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img,
							       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
					} else {
						VkBufferImageCopy c{};
						c.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
						c.imageExtent = {W, H, 1};
						vkCmdCopyBufferToImage(cb, sb, img,
								       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
						c.bufferOffset = W * H;
						c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
						c.imageExtent = {W / 2, H / 2, 1};
						vkCmdCopyBufferToImage(cb, sb, img,
								       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
					}
					barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
						foreign ? 0 : VK_QUEUE_FAMILY_IGNORED,
						foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED);
					submit(cb);
					int rfd = fd;
					if (reexp == 2) {
						auto gf = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev,
												    "vkGetMemoryFdKHR");
						VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
						gi.memory = m;
						gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
						CK(gf(dev, &gi, &rfd));
					}
					size_t len = lseek(rfd, 0, SEEK_END);
					uint8_t *map = (uint8_t *)mmap(0, len, PROT_READ, MAP_SHARED, rfd, 0);
					size_t bad = 0;
					for (uint32_t y = 0; y < H; ++y) {
						bad += memcmp(map + pl[0].offset + y * pl[0].rowPitch, &pat[y * W],
							      W) != 0;
					}
					for (uint32_t y = 0; y < H / 2; ++y) {
						bad += memcmp(map + pl[1].offset + y * pl[1].rowPitch,
							      &pat[W * H + y * W], W) != 0;
					}
					printf("reexport %d (len %zu), mutable+extended+list %d, write via %s, release to %s: %zu/%u bad rows\n",
					       reexp, len, mutable_,
					       via_image ? "vkCmdCopyImage" : "vkCmdCopyBufferToImage",
					       foreign ? "FOREIGN" : "(none)", bad, H * 3 / 2);
					munmap(map, len);
					if (reexp == 2) {
						uint8_t *g2 = (uint8_t *)mmap(0, len, PROT_READ, MAP_SHARED, fd, 0);
						size_t b2 = 0;
						for (uint32_t y = 0; y < H; ++y) {
							b2 += memcmp(g2 + pl[0].offset + y * pl[0].rowPitch,
								     &pat[y * W], W) != 0;
						}
						printf("   ... and the GBM fd itself: %zu/%u bad Y rows\n", b2, H);
						munmap(g2, len);
						struct stat s1, s2;
						fstat(fd, &s1);
						fstat(rfd, &s2);
						printf("   GBM fd inode %lu, re-exported fd inode %lu\n",
						       (unsigned long)s1.st_ino, (unsigned long)s2.st_ino);
					}
					vkDestroyImage(dev, img, 0);
					vkFreeMemory(dev, m, 0);
					close(fd);
					gbm_bo_destroy(bo);
				}
			}
		}
	}
}
