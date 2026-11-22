#pragma once

#include "cuda-helpers.h"
#include "nvenc-helpers.h"

#include <util/deque.h>
#include <opts-parser.h>

#ifdef _WIN32
#define INITGUID
#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#else
#include <glad/glad.h>
#endif

#define do_log(level, format, ...) \
	blog(level, "[knurbs-nvenc: '%s'] " format, obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

#define error_hr(msg) error("%s: %s: 0x%08lX", __FUNCTION__, msg, (uint32_t)hr);

#define NV_FAIL(format, ...) nv_fail(enc->encoder, format, ##__VA_ARGS__)
#define NV_FAILED(x) nv_failed(enc->encoder, x, __FUNCTION__, #x)

/* ------------------------------------------------------------------------- */
/* Main Implementation Structure                                             */

struct nvenc_properties {
	int64_t bitrate;
	int64_t max_bitrate;
	int64_t keyint_sec;
	int64_t cqp;
	int64_t device;
	int64_t bf;
	int64_t bframe_ref_mode;
	int64_t split_encode;
	int64_t target_quality;

	const char *rate_control;
	const char *preset;
	const char *profile;
	const char *tune;
	const char *multipass;
	const char *opts_str;

	bool adaptive_quantization;
	bool lookahead;
	bool disable_scenecut;
	bool repeat_headers;
	bool force_cuda_tex;

	struct obs_options opts;
	obs_data_t *data;
};

struct nvenc_data {
	obs_encoder_t *encoder;
	enum codec_type codec;
	GUID codec_guid;

	void *session;
	NV_ENC_INITIALIZE_PARAMS params;
	NV_ENC_CONFIG config;
	uint32_t buf_count;
	int output_delay;
	int buffers_queued;
	size_t next_bitstream;
	size_t cur_bitstream;
	bool encode_started;
	bool first_packet;
	bool can_change_bitrate;
	bool non_texture;

	DARRAY(struct handle_tex) input_textures;
	DARRAY(struct nv_bitstream) bitstreams;
	DARRAY(struct nv_cuda_surface) surfaces;
	NV_ENC_BUFFER_FORMAT surface_format;
	struct deque dts_list;

	DARRAY(uint8_t) packet_data;
	int64_t packet_pts;
	bool packet_keyframe;
	int packet_priority;

#ifdef _WIN32
	DARRAY(struct nv_texture) textures;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
#endif

	uint32_t cx;
	uint32_t cy;
	enum video_format in_format;

	uint8_t *header;
	size_t header_size;

	uint8_t *sei;
	size_t sei_size;

	int8_t *roi_map;
	size_t roi_map_size;
	uint32_t roi_increment;

#ifdef NVENC_13_0_OR_LATER
	CONTENT_LIGHT_LEVEL *cll;
	MASTERING_DISPLAY_INFO *mdi;
#endif

	struct nvenc_properties props;

	CUcontext cu_ctx;

	/* Vulkan renderer: frames arrive as handles (nvenc-vulkan.c) */
	bool vulkan;
	uint32_t input_pitch; /* of the next picture; 0: cx */
	struct {
		CUstream stream;
		struct nv_vk_semaphore {
			CUexternalSemaphore sem;
			void *handle; /* the libobs handle it was imported from */
		} ready, release;
		DARRAY(struct nv_vk_buffer) buffers;
		DARRAY(struct nv_vk_input) inputs; /* per bitstream slot */
		uint64_t queued;                   /* highest release value handed to NVENC */
		uint64_t signalled;                /* highest release value signalled */
	} vk;
};

/* ------------------------------------------------------------------------- */
/* Resource data structures                                                  */

/* Input texture handles */
struct handle_tex {
#ifdef _WIN32
	uint32_t handle;
	ID3D11Texture2D *tex;
	IDXGIKeyedMutex *km;
#else
	GLuint tex_id;
	/* CUDA mappings */
	CUgraphicsResource res_y;
	CUgraphicsResource res_uv;
#endif
};

/* Bitstream buffer */
struct nv_bitstream {
	void *ptr;
};

/** Mapped resources **/
/* CUDA Arrays */
struct nv_cuda_surface {
	CUarray tex;
	NV_ENC_REGISTERED_PTR res;
	NV_ENC_INPUT_PTR *mapped_res;
};

/* Vulkan: an imported frame buffer, registered with NVENC */
struct nv_vk_buffer {
	uint64_t id;
	CUexternalMemory mem;
	CUdeviceptr ptr;
	uint32_t pitch;
	NV_ENC_REGISTERED_PTR reg;
};

/* Vulkan: the input of a bitstream slot */
struct nv_vk_input {
	NV_ENC_INPUT_PTR mapped;
	uint64_t release_value;
};

#ifdef _WIN32
/* DX11 textures */
struct nv_texture {
	void *res;
	ID3D11Texture2D *tex;
	void *mapped_res;
};
#endif

/* ------------------------------------------------------------------------- */
/* Shared functions                                                          */

bool nvenc_encode_base(struct nvenc_data *enc, struct nv_bitstream *bs, void *pic, int64_t pts,
		       struct encoder_packet *packet, bool *received_packet);

/* ------------------------------------------------------------------------- */
/* Backend-specific functions                                                */

#ifdef _WIN32
/** D3D11 **/
bool d3d11_init(struct nvenc_data *enc, obs_data_t *settings);
void d3d11_free(struct nvenc_data *enc);

bool d3d11_init_textures(struct nvenc_data *enc);
void d3d11_free_textures(struct nvenc_data *enc);

bool d3d11_encode(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key, uint64_t *next_key,
		  struct encoder_packet *packet, bool *received_packet);

bool nvenc_encode_texture(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key,
			  uint64_t *next_key, struct encoder_packet *packet, bool *received_packet);

/** Vulkan (frames as handles) **/
bool vulkan_cuda_ctx_init(struct nvenc_data *enc);
bool vulkan_init_inputs(struct nvenc_data *enc);
void vulkan_free(struct nvenc_data *enc);
bool vulkan_release_input(struct nvenc_data *enc, size_t slot);
bool vulkan_encode(void *data, struct encoder_texture *texture, int64_t pts, struct encoder_packet *packet,
		   bool *received_packet);
#endif

/** CUDA **/
bool cuda_ctx_init(struct nvenc_data *enc, obs_data_t *settings, bool texture);
void cuda_ctx_free(struct nvenc_data *enc);

bool cuda_init_surfaces(struct nvenc_data *enc);
void cuda_free_surfaces(struct nvenc_data *enc);

bool cuda_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received_packet);

#ifndef _WIN32
/** CUDA OpenGL **/
void cuda_opengl_free(struct nvenc_data *enc);
bool cuda_opengl_encode(void *data, struct encoder_texture *tex, int64_t pts, uint64_t lock_key, uint64_t *next_key,
			struct encoder_packet *packet, bool *received_packet);
#endif

/* ------------------------------------------------------------------------- */
/* Properties crap                                                           */

void nvenc_properties_read(struct nvenc_properties *enc, obs_data_t *settings);

void h264_nvenc_defaults(obs_data_t *settings);
void hevc_nvenc_defaults(obs_data_t *settings);
void av1_nvenc_defaults(obs_data_t *settings);

obs_properties_t *h264_nvenc_properties(void *);
obs_properties_t *hevc_nvenc_properties(void *);
obs_properties_t *av1_nvenc_properties(void *);

/* Custom argument parsing */
bool apply_user_args(struct nvenc_data *enc);
bool get_user_arg_int(struct nvenc_data *enc, const char *name, int *val);
