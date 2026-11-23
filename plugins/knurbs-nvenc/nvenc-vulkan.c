#include "nvenc-internal.h"
#include "nvenc-helpers.h"

/*
 * NVENC on the Vulkan renderer (docs/graphics-interop.md, "Texture encoders").
 *
 * libobs writes every frame into a pitch-linear frame buffer, exported as an NT handle: one compute dispatch from the
 * RGB output (gs_sharedbuf_convert), or a copy of the NV12/P010 image where compute is unavailable. It is
 * imported into CUDA as external memory and registered with NVENC as CUDADEVICEPTR: on Windows an NV12 image
 * imported as a CUDA array reads back garbage (spike 3), a buffer does not. libobs signals its ready timeline once a
 * frame is written; CUDA waits for it. Once NVENC has finished with a frame (its bitstream is locked, in submission
 * order), CUDA signals this encoder's release timeline and libobs may write the frame again.
 *
 * Nothing here touches a Vulkan object: only the handles in encoder_texture_shared cross over.
 */

#define CUDA_EXTERNAL_MEMORY_DEDICATED 0x1 /* not in the FFmpeg headers; libobs exports dedicated allocations */

/* ------------------------------------------------------------------------- */
/* Device                                                                    */

bool vulkan_cuda_ctx_init(struct nvenc_data *enc)
{
	struct gs_adapter_identity id;
	obs_enter_graphics();
	const bool have_id = gs_get_adapter_identity(&id);
	obs_leave_graphics();
	if (!have_id) {
		error("Vulkan: no adapter identity");
		return false;
	}
	if (!cu->cuDeviceGetUuid || !cu->cuImportExternalMemory || !cu->cuImportExternalSemaphore) {
		error("Vulkan: the CUDA driver lacks external memory or semaphores");
		return false;
	}

	int count = 0;
	CU_FAILED(cu->cuInit(0))
	CU_FAILED(cu->cuDeviceGetCount(&count))
	for (int i = 0; i < count; i++) {
		CUdevice device;
		CUuuid uuid;
		if (cu->cuDeviceGet(&device, i) != CUDA_SUCCESS || cu->cuDeviceGetUuid(&uuid, device) != CUDA_SUCCESS)
			continue;
		if (memcmp(uuid.bytes, id.device_uuid, sizeof(id.device_uuid)) != 0)
			continue;

		CU_FAILED(cu->cuCtxCreate(&enc->cu_ctx, 0, device))
		CU_FAILED(cu->cuStreamCreate(&enc->vk.stream, CU_STREAM_NON_BLOCKING))
		CU_FAILED(cu->cuCtxPopCurrent(NULL))
		info("Vulkan: CUDA device %d is the renderer's adapter", i);
		return true;
	}

	/* The renderer runs on another GPU (or not NVIDIA): the non-texture encoder takes over. */
	info("Vulkan: the renderer's adapter is not a CUDA device");
	return false;
}

bool vulkan_init_inputs(struct nvenc_data *enc)
{
	da_resize(enc->vk.inputs, enc->buf_count);
	memset(enc->vk.inputs.array, 0, enc->vk.inputs.num * sizeof(*enc->vk.inputs.array));
	return true;
}

static void destroy_semaphore(struct nvenc_data *enc, struct nv_vk_semaphore *sem)
{
	UNUSED_PARAMETER(enc);
	if (sem->sem)
		cu->cuDestroyExternalSemaphore(sem->sem);
	sem->sem = NULL;
	sem->handle = NULL;
}

void vulkan_free(struct nvenc_data *enc)
{
	if (!enc->cu_ctx)
		return;
	cu->cuCtxPushCurrent(enc->cu_ctx);
	for (size_t i = 0; i < enc->vk.buffers.num; i++) {
		struct nv_vk_buffer *b = &enc->vk.buffers.array[i];
		if (b->mapped)
			nv.nvEncUnmapInputResource(enc->session, b->mapped);
		if (b->reg)
			nv.nvEncUnregisterResource(enc->session, b->reg);
		if (b->mem)
			cu->cuDestroyExternalMemory(b->mem); /* also frees the mapped pointer */
	}
	/* Everything still held is released: libobs stopped counting this encoder's holds anyway. */
	if (enc->vk.release.sem && enc->vk.signalled < enc->vk.queued) {
		CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS sp = {0};
		sp.params.fence.value = enc->vk.queued;
		cu->cuSignalExternalSemaphoresAsync(&enc->vk.release.sem, &sp, 1, enc->vk.stream);
	}
	if (enc->vk.stream) {
		cu->cuStreamSynchronize(enc->vk.stream);
		cu->cuStreamDestroy(enc->vk.stream);
		enc->vk.stream = NULL;
	}
	destroy_semaphore(enc, &enc->vk.ready);
	destroy_semaphore(enc, &enc->vk.release);
	cu->cuCtxPopCurrent(NULL);
	da_free(enc->vk.buffers);
	da_free(enc->vk.inputs);
}

/* ------------------------------------------------------------------------- */
/* Imports (cached; a handle is only borrowed for the import)                */

static bool import_semaphore(struct nvenc_data *enc, struct nv_vk_semaphore *sem, const struct gs_shared_handle *h)
{
	if (sem->sem && sem->handle == h->win32)
		return true;
	if (h->type != GS_HANDLE_WIN32 || !h->win32) {
		error("Vulkan: expected an NT timeline handle, got type %d", (int)h->type);
		return false;
	}
	destroy_semaphore(enc, sem);
	CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC desc = {0};
	desc.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32;
	desc.handle.win32.handle = h->win32;
	CU_FAILED(cu->cuImportExternalSemaphore(&sem->sem, &desc))
	sem->handle = h->win32;
	return true;
}

static struct nv_vk_buffer *import_buffer(struct nvenc_data *enc, uint64_t id, const struct gs_shared_texture_desc *d)
{
	for (size_t i = 0; i < enc->vk.buffers.num; i++) {
		if (enc->vk.buffers.array[i].id == id)
			return &enc->vk.buffers.array[i];
	}

	const bool p010 = obs_encoder_video_tex_active(enc->encoder, VIDEO_FORMAT_P010);
	if (d->type != GS_HANDLE_WIN32 || !d->win32 || d->num_planes != 2 || d->width != enc->cx ||
	    d->height != enc->cy || d->layout != (p010 ? GS_SHARED_P010 : GS_SHARED_NV12) || d->offsets[0] != 0 ||
	    d->pitches[0] != d->pitches[1] || d->offsets[1] != d->pitches[0] * d->height) {
		/* NVENC's CUDADEVICEPTR input has the chroma rows right after the luma rows at the same pitch. */
		error("Vulkan: frame buffer %llu has an unexpected layout (%ux%u, layout %d, pitch %u/%u, UV at %u)",
		      (unsigned long long)id, d->width, d->height, (int)d->layout, d->pitches[0], d->pitches[1],
		      d->offsets[1]);
		return NULL;
	}

	struct nv_vk_buffer b = {.id = id};
	CUDA_EXTERNAL_MEMORY_HANDLE_DESC md = {0};
	md.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
	md.handle.win32.handle = d->win32;
	md.size = d->size;
	md.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
	if (!cuda_error_check(enc, cu->cuImportExternalMemory(&b.mem, &md), __FUNCTION__, "cuImportExternalMemory"))
		return NULL;

	CUDA_EXTERNAL_MEMORY_BUFFER_DESC bd = {0};
	bd.size = (unsigned long long)d->pitches[0] * d->height * 3 / 2;
	if (!cuda_error_check(enc, cu->cuExternalMemoryGetMappedBuffer(&b.ptr, b.mem, &bd), __FUNCTION__,
			      "cuExternalMemoryGetMappedBuffer")) {
		cu->cuDestroyExternalMemory(b.mem);
		return NULL;
	}

	NV_ENC_REGISTER_RESOURCE res = {NV_ENC_REGISTER_RESOURCE_VER};
	res.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
	res.resourceToRegister = (void *)b.ptr;
	res.width = enc->cx;
	res.height = enc->cy;
	res.pitch = d->pitches[0];
	res.bufferFormat = p010 ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
	if (NV_FAILED(nv.nvEncRegisterResource(enc->session, &res))) {
		cu->cuDestroyExternalMemory(b.mem);
		return NULL;
	}
	b.reg = res.registeredResource;
	b.pitch = d->pitches[0];

	da_push_back(enc->vk.buffers, &b);
	debug("Vulkan: imported frame buffer %llu (pitch %u)", (unsigned long long)id, b.pitch);
	return &enc->vk.buffers.array[enc->vk.buffers.num - 1];
}

/* ------------------------------------------------------------------------- */
/* Encoding                                                                  */

/* Called once NVENC is done with the input of a bitstream slot (get_encoded_packet, in submission order). */
bool vulkan_release_input(struct nvenc_data *enc, size_t slot)
{
	struct nv_vk_input *in = &enc->vk.inputs.array[slot];
	if (in->buffer) {
		struct nv_vk_buffer *b = &enc->vk.buffers.array[in->buffer - 1];
		in->buffer = 0;
		if (--b->map_count == 0) {
			NVENCSTATUS err = nv.nvEncUnmapInputResource(enc->session, b->mapped);
			b->mapped = NULL;
			if (nv_failed(enc->encoder, err, __FUNCTION__, "unmap"))
				return false;
		}
	}
	if (in->release_value > enc->vk.signalled) {
		CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS sp = {0};
		sp.params.fence.value = in->release_value;
		CU_FAILED(cu->cuSignalExternalSemaphoresAsync(&enc->vk.release.sem, &sp, 1, enc->vk.stream))
		enc->vk.signalled = in->release_value;
	}
	in->release_value = 0;
	return true;
}

bool vulkan_encode(void *data, struct encoder_texture *texture, int64_t pts, struct encoder_packet *packet,
		   bool *received_packet)
{
	struct nvenc_data *enc = data;
	const struct encoder_texture_shared *shared = texture->shared;

	if (!shared || shared->buf.type == GS_HANDLE_NONE || !shared->buf_id) {
		error("Encode failed: libobs gave no shared frame buffer");
		return false;
	}

	bool ok = false;
	CUresult res = cu->cuCtxPushCurrent(enc->cu_ctx);
	if (!cuda_error_check(enc, res, __FUNCTION__, "cuCtxPushCurrent"))
		return false;

	if (!import_semaphore(enc, &enc->vk.ready, &shared->ready) ||
	    !import_semaphore(enc, &enc->vk.release, &shared->release))
		goto out;

	struct nv_vk_buffer *b = import_buffer(enc, shared->buf_id, &shared->buf);
	if (!b)
		goto out;

	/* NVENC is not ordered with the stream: wait here until libobs has written the frame. */
	CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wp = {0};
	wp.params.fence.value = shared->ready_value;
	res = cu->cuWaitExternalSemaphoresAsync(&enc->vk.ready.sem, &wp, 1, enc->vk.stream);
	if (!cuda_error_check(enc, res, __FUNCTION__, "cuWaitExternalSemaphoresAsync"))
		goto out;
	res = cu->cuStreamSynchronize(enc->vk.stream);
	if (!cuda_error_check(enc, res, __FUNCTION__, "cuStreamSynchronize"))
		goto out;

	struct nv_vk_input *in = &enc->vk.inputs.array[enc->next_bitstream];
	if (!b->map_count) {
		NV_ENC_MAP_INPUT_RESOURCE map = {NV_ENC_MAP_INPUT_RESOURCE_VER};
		map.registeredResource = b->reg;
		if (NV_FAILED(nv.nvEncMapInputResource(enc->session, &map)))
			goto out;
		b->mapped = map.mappedResource;
	}
	b->map_count++;
	in->buffer = (size_t)(b - enc->vk.buffers.array) + 1;
	in->release_value = shared->release_value;
	if (shared->release_value > enc->vk.queued)
		enc->vk.queued = shared->release_value;

	deque_push_back(&enc->dts_list, &pts, sizeof(pts));
	struct nv_bitstream *bs = &enc->bitstreams.array[enc->next_bitstream];
	enc->input_pitch = b->pitch;
	ok = nvenc_encode_base(enc, bs, b->mapped, pts, packet, received_packet);

out:
	cu->cuCtxPopCurrent(NULL);
	return ok;
}
