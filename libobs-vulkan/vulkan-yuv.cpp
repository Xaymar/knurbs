// RGB -> two-plane YUV (NV12, P010; 4:2:2 for tests) in one compute dispatch, instead of the two fragment passes of
// format_conversion.effect (Y, then UV) and, for NVENC, the copy of both planes into the pitch-linear frame buffer.
//
// Targets: the planes of an NV12/P010 image through per-plane storage views (r8/rg8 or r16/rg16 UNORM views of a
// MUTABLE_FORMAT | EXTENDED_USAGE image created with STORAGE usage; see vk_yuv_init for when that is allowed), or a
// pitch-linear storage buffer at the offsets and pitches the encoder expects (gs_sharedbuf_t).
//
// The math is the effect's, term for term: the same colour vectors, transfer functions and chroma filter
// (left-sited [1 2 1] / 4 across, the two rows averaged, or [1 2 1] / 4 around the upper row for P010 PQ/HLG), with
// texel fetches where the effect lets bilinear filtering at texel edges do the averaging. 10-bit values are
// quantised as the effect does (floor(x * 1023 + 0.5) in the high bits). Results match the fragment path within one
// LSB (filtering precision); test_vulkan_yuv_compute compares them.
//
// One invocation converts a block 2 (images) or 4 (buffers) luma texels wide and 2 (4:2:0) or 1 (4:2:2) rows high.
// Buffer writes are whole 32-bit words; rows are 256-byte aligned, so texels past a width that is not a multiple of 4
// land in the row padding.
//
// OBS_VULKAN_YUV_CONVERT=fragment turns all of it off (gs_yuv_compute_available says no, images get no STORAGE),
// "compute" or unset uses it where the device allows it.
#include "vulkan-subsystem.hpp"

#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <util/base.h>

namespace {

// Luma texels per invocation across: 4 for buffers (whole 32-bit words for 8 and 16 bits), 2 for images, whose
// stores are much faster when neighbouring invocations write neighbouring texels (AMD 780M P010: 412 instead of 712
// us at 1080p; RTX 4070 Laptop NV12: 16 instead of 46 us).
uint32_t block_width(bool buffer)
{
	return buffer ? 4 : 2;
}

constexpr uint32_t group = 8; // local size in blocks, both dimensions

struct push_constants {
	float cy[4], cu[4], cv[4];
	float sdr_scale, hdr_lw;
	uint32_t width, height;       // luma
	uint32_t y_offset, uv_offset; // buffer target: bytes
	uint32_t y_pitch, uv_pitch;   // buffer target: bytes
	uint32_t chroma_rows, unused; // rows of chroma (height / 2 for 4:2:0)
};
static_assert(sizeof(push_constants) <= 128, "push constants must fit the guaranteed minimum");

// Variant: target, bits, transfer, subsampling, chroma siting.
struct variant {
	bool buffer;
	uint32_t bits; // 8, 10 (P010: high bits of 16) or 16
	enum gs_yuv_transfer transfer;
	bool sub_y; // 4:2:0 (true) or 4:2:2
	bool top_left;

	uint32_t key() const
	{
		const uint32_t b = bits == 8 ? 0 : bits == 10 ? 1 : 2;
		return (buffer ? 1u : 0u) | (b << 1) | ((uint32_t)transfer << 3) | ((sub_y ? 1u : 0u) << 5) |
		       ((top_left ? 1u : 0u) << 6);
	}
};

const char *shader_body = R"GLSL(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
#if BUFFER
layout(set = 0, binding = 1, std430) writeonly buffer Frame { uint words[]; } frame;
#elif BITS == 8
layout(set = 0, binding = 1, r8) writeonly uniform image2D dst_y;
layout(set = 0, binding = 2, rg8) writeonly uniform image2D dst_uv;
#else
layout(set = 0, binding = 1, r16) writeonly uniform image2D dst_y;
layout(set = 0, binding = 2, rg16) writeonly uniform image2D dst_uv;
#endif
layout(push_constant) uniform PC {
	vec4 cy, cu, cv;
	float sdr_scale, hdr_lw;
	uint width, height;
	uint y_offset, uv_offset;
	uint y_pitch, uv_pitch;
	uint chroma_rows, unused;
} pc;

// color.effect
float srgb_linear_to_nonlinear_channel(float u)
{
	return (u <= 0.0031308) ? (12.92 * u) : ((1.055 * pow(u, 1. / 2.4)) - 0.055);
}
vec3 srgb_linear_to_nonlinear(vec3 v)
{
	return vec3(srgb_linear_to_nonlinear_channel(v.r), srgb_linear_to_nonlinear_channel(v.g),
		    srgb_linear_to_nonlinear_channel(v.b));
}
vec3 rec709_to_rec2020(vec3 v)
{
	float r = dot(v, vec3(0.62740389593469903, 0.32928303837788370, 0.043313065687417225));
	float g = dot(v, vec3(0.069097289358232075, 0.91954039507545871, 0.011362315566309178));
	float b = dot(v, vec3(0.016391438875150280, 0.088013307877225749, 0.89559525324762401));
	return vec3(r, g, b);
}
float linear_to_st2084_channel(float x)
{
	float c = pow(abs(x), 0.1593017578);
	return pow((0.8359375 + 18.8515625 * c) / (1. + 18.6875 * c), 78.84375);
}
vec3 linear_to_st2084(vec3 rgb)
{
	return vec3(linear_to_st2084_channel(rgb.r), linear_to_st2084_channel(rgb.g), linear_to_st2084_channel(rgb.b));
}
float st2084_to_linear_channel(float u)
{
	float c = pow(abs(u), 1. / 78.84375);
	return pow(abs(max(c - 0.8359375, 0.) / (18.8515625 - 18.6875 * c)), 1. / 0.1593017578);
}
float eetf_0_Lmax(float maxRGB1_pq, float Lw, float Lmax)
{
	float Lw_pq = linear_to_st2084_channel(Lw / 10000.);
	float E1 = clamp(maxRGB1_pq / Lw_pq, 0., 1.);
	float maxLum = linear_to_st2084_channel(Lmax / 10000.) / Lw_pq;
	float KS = (1.5 * maxLum) - 0.5;
	float E2 = E1;
	if (E1 > KS) {
		float T = (E1 - KS) / (1. - KS);
		float Tsquared = T * T;
		float Tcubed = Tsquared * T;
		float P = (2. * Tcubed - 3. * Tsquared + 1.) * KS + (Tcubed - 2. * Tsquared + T) * (1. - KS) +
			  (-2. * Tcubed + 3. * Tsquared) * maxLum;
		E2 = P;
	}
	return E2 * Lw_pq;
}
vec3 maxRGB_eetf_linear_to_linear(vec3 rgb_linear, float Lw, float Lmax)
{
	float maxRGB1_linear = max(max(rgb_linear.r, rgb_linear.g), rgb_linear.b);
	float maxRGB1_pq = linear_to_st2084_channel(maxRGB1_linear);
	float maxRGB2_pq = eetf_0_Lmax(maxRGB1_pq, Lw, Lmax);
	float maxRGB2_linear = st2084_to_linear_channel(maxRGB2_pq);
	maxRGB1_linear = max(6.10352e-5, maxRGB1_linear);
	return rgb_linear * (maxRGB2_linear / maxRGB1_linear);
}
float linear_to_hlg_channel(float u)
{
	float ln2_i = 1. / log(2.);
	float m = 0.17883277 / ln2_i;
	return (u <= (1. / 12.)) ? sqrt(3. * u) : ((log2((12. * u) - 0.28466892) * m) + 0.55991073);
}
vec3 linear_to_hlg(vec3 rgb, float Lw)
{
	rgb = clamp(rgb, 0., 1.);
	if (Lw > 1000.) {
		rgb = maxRGB_eetf_linear_to_linear(rgb, Lw, 1000.);
		rgb *= 10000. / Lw;
	} else {
		rgb *= 10.;
	}
	float Yd = dot(rgb, vec3(0.2627, 0.678, 0.0593));
	Yd = max(6.10352e-5, Yd);
	rgb *= pow(Yd, -1. / 6.);
	return vec3(linear_to_hlg_channel(rgb.r), linear_to_hlg_channel(rgb.g), linear_to_hlg_channel(rgb.b));
}

// format_conversion.effect: the transfer before the matrix (PQ/HLG scale first; chroma scales the average).
vec3 transfer(vec3 rgb)
{
#if TRANSFER == 1
	return srgb_linear_to_nonlinear(rgb);
#elif TRANSFER == 2
	return linear_to_st2084(rec709_to_rec2020(rgb * pc.sdr_scale));
#elif TRANSFER == 3
	return linear_to_hlg(rec709_to_rec2020(rgb * pc.sdr_scale), pc.hdr_lw);
#else
	return rgb;
#endif
}

ivec2 src_max;
vec3 px(int x, int y)
{
	return texelFetch(src, clamp(ivec2(x, y), ivec2(0), src_max), 0).rgb;
}

// Quantised value as the target stores it: 8 bits, 10 bits in the high bits of 16, or 16 bits.
uint quant(float v)
{
	v = clamp(v, 0., 1.);
#if BITS == 8
	return uint(v * 255. + 0.5);
#elif BITS == 10
	return uint(floor(v * 1023. + 0.5)) << 6;
#else
	return uint(v * 65535. + 0.5);
#endif
}
#if !BUFFER
// Image targets: the UNORM view converts; values are already quantised so the conversion is exact for 10 bits and
// rounds as the fragment path's render target does for 8 and 16.
float img(float v)
{
#if BITS == 10
	return float(quant(v)) / 65535.;
#else
	return v;
#endif
}
#endif

void main()
{
	uint bx = gl_GlobalInvocationID.x, by = gl_GlobalInvocationID.y;
	int x0 = int(bx * uint(BW));
#if SUB_Y
	int y0 = int(by * 2u);
	int rows = 2;
#else
	int y0 = int(by);
	int rows = 1;
#endif
	if (x0 >= int(pc.width) || by >= pc.chroma_rows)
		return;
	src_max = textureSize(src, 0) - ivec2(1);

	// Luma
	for (int j = 0; j < rows; ++j) {
		float y[BW];
		for (int i = 0; i < BW; ++i)
			y[i] = dot(pc.cy.xyz, transfer(px(x0 + i, y0 + j))) + pc.cy.w;
#if BUFFER
		uint row = (pc.y_offset + uint(y0 + j) * pc.y_pitch) / 4u + uint(x0) * uint(BYTES) / 4u;
#if BITS == 8
		frame.words[row] = packUnorm4x8(vec4(y[0], y[1], y[2], y[3]));
#else
		frame.words[row] = quant(y[0]) | (quant(y[1]) << 16);
		frame.words[row + 1u] = quant(y[2]) | (quant(y[3]) << 16);
#endif
#else
		for (int i = 0; i < BW; ++i)
			if (x0 + i < int(pc.width))
				imageStore(dst_y, ivec2(x0 + i, y0 + j), vec4(img(y[i]), 0., 0., 1.));
#endif
	}

	// Chroma: one sample per two luma columns (x0, x0 + 2; left-sited), each [1 2 1] / 4 across.
	float u[BW / 2], v[BW / 2];
	for (int c = 0; c < BW / 2; ++c) {
		int xc = x0 + 2 * c;
#if SUB_Y && TOP_LEFT
		// [1 2 1] / 4 around row y0 (the effect's top-left WideWide sampling)
		vec3 rgb = vec3(0.);
		for (int r = -1; r <= 1; ++r) {
			vec3 h = px(xc - 1, y0 + r) + 2. * px(xc, y0 + r) + px(xc + 1, y0 + r);
			rgb += (r == 0 ? 2. : 1.) * h;
		}
		rgb *= 1. / 16.;
#elif SUB_Y
		vec3 top = (px(xc - 1, y0) + px(xc, y0)) * 0.5 + (px(xc, y0) + px(xc + 1, y0)) * 0.5;
		vec3 bot = (px(xc - 1, y0 + 1) + px(xc, y0 + 1)) * 0.5 + (px(xc, y0 + 1) + px(xc + 1, y0 + 1)) * 0.5;
		vec3 rgb = (top + bot) * 0.25;
#else
		vec3 rgb = ((px(xc - 1, y0) + px(xc, y0)) * 0.5 + (px(xc, y0) + px(xc + 1, y0)) * 0.5) * 0.5;
#endif
		vec3 t = transfer(rgb);
		u[c] = dot(pc.cu.xyz, t) + pc.cu.w;
		v[c] = dot(pc.cv.xyz, t) + pc.cv.w;
	}
#if BUFFER
	uint crow = (pc.uv_offset + by * pc.uv_pitch) / 4u + uint(x0) * uint(BYTES) / 4u;
#if BITS == 8
	frame.words[crow] = packUnorm4x8(vec4(u[0], v[0], u[1], v[1]));
#else
	frame.words[crow] = quant(u[0]) | (quant(v[0]) << 16);
	frame.words[crow + 1u] = quant(u[1]) | (quant(v[1]) << 16);
#endif
#else
	for (int c = 0; c < BW / 2; ++c)
		if (x0 + 2 * c < int(pc.width))
			imageStore(dst_uv, ivec2(x0 / 2 + c, int(by)), vec4(img(u[c]), img(v[c]), 0., 1.));
#endif
}
)GLSL";

std::vector<uint32_t> compile(const variant &v, std::string &error)
{
	std::string src = "#version 450\n";
	src += "#define BUFFER " + std::to_string(v.buffer ? 1 : 0) + "\n";
	src += "#define BITS " + std::to_string(v.bits) + "\n";
	src += "#define BW " + std::to_string(block_width(v.buffer)) + "\n";
	src += "#define BYTES " + std::to_string(v.bits == 8 ? 1 : 2) + "\n";
	src += "#define TRANSFER " + std::to_string((int)v.transfer) + "\n";
	src += "#define SUB_Y " + std::to_string(v.sub_y ? 1 : 0) + "\n";
	src += "#define TOP_LEFT " + std::to_string(v.top_left ? 1 : 0) + "\n";
	src += shader_body;

	vk_glslang_ref();
	std::vector<uint32_t> spirv;
	{
		glslang::TShader sh(EShLangCompute);
		const char *s = src.c_str();
		sh.setStrings(&s, 1);
		sh.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
		sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
		sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
		const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
		glslang::TProgram program;
		if (!sh.parse(GetDefaultResources(), 450, false, msgs)) {
			error = sh.getInfoLog();
		} else {
			program.addShader(&sh);
			if (!program.link(msgs)) {
				error = program.getInfoLog();
			} else {
				glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv);
			}
		}
	}
	vk_glslang_unref();
	return spirv;
}

bool layouts(gs_device *d)
{
	vk_yuv &y = d->yuv;
	if (y.layout[0]) {
		return true;
	}
	if (!y.sampler) {
		VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = si.minFilter = VK_FILTER_NEAREST;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		if (vkCreateSampler(d->dev, &si, nullptr, &y.sampler) != VK_SUCCESS) {
			return false;
		}
	}
	for (int buffer = 0; buffer < 2; ++buffer) {
		VkDescriptorSetLayoutBinding b[3] = {};
		b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &y.sampler};
		if (buffer) {
			b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
		} else {
			b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
			b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
		}
		VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		ci.bindingCount = buffer ? 2 : 3;
		ci.pBindings = b;
		if (vkCreateDescriptorSetLayout(d->dev, &ci, nullptr, &y.set_layout[buffer]) != VK_SUCCESS) {
			return false;
		}
		const VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants)};
		VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		li.setLayoutCount = 1;
		li.pSetLayouts = &y.set_layout[buffer];
		li.pushConstantRangeCount = 1;
		li.pPushConstantRanges = &pr;
		if (vkCreatePipelineLayout(d->dev, &li, nullptr, &y.layout[buffer]) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

VkPipeline pipeline(gs_device *d, const variant &v)
{
	auto it = d->yuv.pipelines.find(v.key());
	if (it != d->yuv.pipelines.end()) {
		return it->second;
	}
	VkPipeline p = VK_NULL_HANDLE;
	std::string error;
	std::vector<uint32_t> spirv;
	if (!layouts(d)) {
		blog(LOG_ERROR, "[libobs-vulkan] YUV conversion: layout creation failed");
	} else if ((spirv = compile(v, error)).empty()) {
		blog(LOG_ERROR, "[libobs-vulkan] YUV conversion: compute shader failed to compile:\n%s", error.c_str());
	} else {
		VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		mi.codeSize = spirv.size() * sizeof(uint32_t);
		mi.pCode = spirv.data();
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(d->dev, &mi, nullptr, &module) == VK_SUCCESS) {
			VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
			ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			ci.stage.module = module;
			ci.stage.pName = "main";
			ci.layout = d->yuv.layout[v.buffer ? 1 : 0];
			if (vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) {
				p = VK_NULL_HANDLE;
				blog(LOG_ERROR, "[libobs-vulkan] YUV conversion: vkCreateComputePipelines failed");
			}
			vkDestroyShaderModule(d->dev, module, nullptr);
		}
	}
	d->yuv.pipelines[v.key()] = p; // failures are not retried
	return p;
}

VkImageView storage_view(gs_device *d, gs_texture *t)
{
	if (!t->storage_view) {
		const bool wide = t->image_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		const bool uv = t->aspect == VK_IMAGE_ASPECT_PLANE_1_BIT;
		VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
		vu.usage = VK_IMAGE_USAGE_STORAGE_BIT;
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu};
		vi.image = t->image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = wide ? (uv ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16_UNORM)
				 : (uv ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM);
		vi.subresourceRange = {t->aspect, 0, 1, 0, 1};
		vkCreateImageView(d->dev, &vi, nullptr, &t->storage_view);
	}
	return t->storage_view;
}

// The variant the effect's technique for this target and conversion corresponds to.
variant make_variant(bool buffer, uint32_t bits, bool sub_y, const gs_yuv_conversion &c)
{
	variant v;
	v.buffer = buffer;
	v.bits = bits;
	v.transfer = c.transfer <= GS_YUV_TRANSFER_HLG ? c.transfer : GS_YUV_TRANSFER_NONE;
	v.sub_y = sub_y;
	v.top_left = sub_y && c.chroma_top_left;
	return v;
}

struct target {
	gs_texture *y = nullptr;          // image target: the Y plane view (its peer is UV)
	VkBuffer buffer = VK_NULL_HANDLE; // buffer target
	VkDeviceSize buffer_size = 0;
	uint32_t width = 0, height = 0;
	uint32_t offsets[2] = {}, pitches[2] = {};
};

bool dispatch(gs_device *d, gs_texture *src, const variant &v, const target &t, const gs_yuv_conversion &c)
{
	if (!src->image || !src->view || vk_is_plane(src)) {
		blog(LOG_ERROR, "[libobs-vulkan] YUV conversion: the source must be an RGB(A) texture");
		return false;
	}
	const VkPipeline p = pipeline(d, v);
	if (!p) {
		return false;
	}
	VkImageView vy = VK_NULL_HANDLE, vuv = VK_NULL_HANDLE;
	if (!v.buffer && (!(vy = storage_view(d, t.y)) || !(vuv = storage_view(d, t.y->plane_peer)))) {
		blog(LOG_ERROR, "[libobs-vulkan] YUV conversion: storage view creation failed");
		return false;
	}

	VkCommandBuffer cmd = vk_cmd(d);
	const VkDescriptorSet set = vk_alloc_set(d, d->yuv.set_layout[v.buffer ? 1 : 0]);
	if (!set) {
		blog(LOG_ERROR, "[libobs-vulkan] YUV conversion: descriptor set allocation failed");
		return false;
	}
	vk_barriers b;
	d->tracker.use_image(src->image, d->graphics.family,
			     {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			     b);
	if (v.buffer) {
		d->tracker.use_buffer(t.buffer, d->graphics.family,
				      {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				       VK_IMAGE_LAYOUT_UNDEFINED},
				      b);
	} else {
		// Plane views: the barrier covers the whole two-plane image (COLOR aspect), as for rendering.
		d->tracker.use_image(t.y->image, d->graphics.family,
				     {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				      VK_IMAGE_LAYOUT_GENERAL},
				     b);
	}
	vk_record_barriers(cmd, b);

	const VkDescriptorImageInfo si = {VK_NULL_HANDLE, src->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const VkDescriptorImageInfo yi = {VK_NULL_HANDLE, vy, VK_IMAGE_LAYOUT_GENERAL};
	const VkDescriptorImageInfo uvi = {VK_NULL_HANDLE, vuv, VK_IMAGE_LAYOUT_GENERAL};
	const VkDescriptorBufferInfo bi = {t.buffer, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet w[3] = {};
	for (VkWriteDescriptorSet &x : w) {
		x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		x.dstSet = set;
		x.descriptorCount = 1;
	}
	w[0].dstBinding = 0;
	w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w[0].pImageInfo = &si;
	w[1].dstBinding = 1;
	if (v.buffer) {
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		w[1].pBufferInfo = &bi;
	} else {
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w[1].pImageInfo = &yi;
		w[2].dstBinding = 2;
		w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w[2].pImageInfo = &uvi;
	}
	vkUpdateDescriptorSets(d->dev, v.buffer ? 2 : 3, w, 0, nullptr);

	push_constants pc = {};
	memcpy(pc.cy, c.color_vec[0], sizeof(pc.cy));
	memcpy(pc.cu, c.color_vec[1], sizeof(pc.cu));
	memcpy(pc.cv, c.color_vec[2], sizeof(pc.cv));
	pc.sdr_scale = c.sdr_white_nits_over_maximum;
	pc.hdr_lw = c.hdr_lw;
	pc.width = t.width;
	pc.height = t.height;
	pc.y_offset = t.offsets[0];
	pc.uv_offset = t.offsets[1];
	pc.y_pitch = t.pitches[0];
	pc.uv_pitch = t.pitches[1];
	pc.chroma_rows = v.sub_y ? t.height / 2 : t.height;

	const VkPipelineLayout layout = d->yuv.layout[v.buffer ? 1 : 0];
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	const uint32_t bw = block_width(v.buffer);
	const uint32_t blocks_x = (t.width + bw - 1) / bw;
	vkCmdDispatch(cmd, (blocks_x + group - 1) / group, (pc.chroma_rows + group - 1) / group, 1);
	return true;
}

// Buffer targets: every row must hold the block writes past the width (whole words of 4 texels).
bool buffer_fits(const target &t, uint32_t bytes, bool sub_y)
{
	const uint32_t row = (t.width + 3) / 4 * 4 * bytes; // a Y row; a UV row of width / 2 pairs is as long
	const uint32_t chroma_rows = sub_y ? t.height / 2 : t.height;
	return t.pitches[0] >= row && t.pitches[1] >= row && !(t.pitches[0] & 3) && !(t.pitches[1] & 3) &&
	       !(t.offsets[0] & 3) && !(t.offsets[1] & 3) &&
	       (VkDeviceSize)t.offsets[0] + (VkDeviceSize)t.pitches[0] * t.height <= t.buffer_size &&
	       (VkDeviceSize)t.offsets[1] + (VkDeviceSize)t.pitches[1] * chroma_rows <= t.buffer_size;
}

bool env_enabled()
{
	const char *e = getenv("OBS_VULKAN_YUV_CONVERT");
	return !e || strcmp(e, "fragment") != 0;
}

} // namespace

void vk_yuv_init(gs_device *d)
{
	vk_yuv &y = d->yuv;
	y = vk_yuv{};
	y.enabled = env_enabled();
	// Images: the plane formats (UNORM, which the conversion writes, and UINT, which AMF views imports as) support
	// STORAGE, the device has storage images of those formats (shaderStorageImageExtendedFormats), and the
	// multi-planar image is allowed with STORAGE usage next to the usual ones (test_vulkan_yuv_storage).
	struct cand {
		enum gs_shared_layout layout;
		VkFormat image, y, uv, y_uint, uv_uint;
	};
	const cand cands[] = {
		{GS_SHARED_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
		 VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT},
		{GS_SHARED_P010, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, VK_FORMAT_R16_UNORM,
		 VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT},
	};
	for (const cand &c : cands) {
		bool ok = d->storage_ext_formats;
		for (VkFormat f : {c.y, c.uv, c.y_uint, c.uv_uint}) {
			VkFormatProperties fp;
			vkGetPhysicalDeviceFormatProperties(d->phys, f, &fp);
			ok = ok && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
		}
		if (ok) {
			const VkFormat list[4] = {c.y, c.uv, c.y_uint, c.uv_uint};
			VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
			fl.viewFormatCount = 4;
			fl.pViewFormats = list;
			VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
								 &fl};
			info.format = c.image;
			info.type = VK_IMAGE_TYPE_2D;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
				     VK_IMAGE_USAGE_STORAGE_BIT;
			info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
			VkImageFormatProperties2 props = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
			ok = vkGetPhysicalDeviceImageFormatProperties2(d->phys, &info, &props) == VK_SUCCESS;
		}
		y.images[c.layout] = ok;
	}
	blog(LOG_INFO,
	     "[libobs-vulkan] YUV conversion: %s; compute into NV12 images %s, P010 images %s, frame buffers %s",
	     y.enabled ? "compute where possible" : "fragment (OBS_VULKAN_YUV_CONVERT=fragment)",
	     y.images[GS_SHARED_NV12] ? "yes" : "no", y.images[GS_SHARED_P010] ? "yes" : "no",
	     y.enabled ? "yes" : "no");
}

void vk_yuv_free(gs_device *d)
{
	vk_yuv &y = d->yuv;
	if (!d->dev) {
		y = vk_yuv{};
		return;
	}
	for (auto &p : y.pipelines) {
		vkDestroyPipeline(d->dev, p.second, nullptr);
	}
	for (int i = 0; i < 2; ++i) {
		vkDestroyPipelineLayout(d->dev, y.layout[i], nullptr);
		vkDestroyDescriptorSetLayout(d->dev, y.set_layout[i], nullptr);
	}
	vkDestroySampler(d->dev, y.sampler, nullptr);
	const bool enabled = y.enabled;
	y = vk_yuv{};
	y.enabled = enabled;
}

extern "C" {

EXPORT bool device_yuv_compute_available(gs_device_t *device, enum gs_shared_layout layout, bool buffer)
{
	if (layout != GS_SHARED_NV12 && layout != GS_SHARED_P010) {
		return false;
	}
	return buffer ? device->yuv.enabled : vk_yuv_storage(device, layout);
}

EXPORT bool device_texture_convert_yuv(gs_device_t *device, gs_texture_t *tex_y, gs_texture_t *src,
				       const struct gs_yuv_conversion *conv)
{
	const bool p010 = tex_y->image_format == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
	if (!device->yuv.enabled || !tex_y->storage || tex_y->aspect != VK_IMAGE_ASPECT_PLANE_0_BIT ||
	    !tex_y->plane_peer || !tex_y->image) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_convert_yuv: needs the Y plane of an NV12/P010 texture "
				"created for compute conversion");
		return false;
	}
	target t;
	t.y = tex_y;
	t.width = tex_y->width;
	t.height = tex_y->height;
	return dispatch(device, src, make_variant(false, p010 ? 10 : 8, true, *conv), t, *conv);
}

EXPORT bool device_sharedbuf_convert(gs_device_t *device, gs_sharedbuf_t *buf, gs_texture_t *src,
				     const struct gs_yuv_conversion *conv)
{
	if (!device->yuv.enabled || !buf->buffer) {
		return false;
	}
	target t;
	t.buffer = buf->buffer;
	t.buffer_size = buf->size;
	t.width = buf->width;
	t.height = buf->height;
	for (int i = 0; i < 2; ++i) {
		t.offsets[i] = buf->offsets[i];
		t.pitches[i] = buf->pitches[i];
	}
	const bool p010 = buf->layout == GS_SHARED_P010;
	if (!buffer_fits(t, p010 ? 2 : 1, true)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_sharedbuf_convert: frame buffer layout unsuitable");
		return false;
	}
	return dispatch(device, src, make_variant(true, p010 ? 10 : 8, true, *conv), t, *conv);
}

// Test hook: converts `src` into a new pitch-linear buffer of `bits` (8, 10 or 16) per sample, 4:2:0 (sub_y) or
// 4:2:2, at a 256-byte pitch with the chroma rows after the luma rows, and reads it back into `dst` (pitch * height
// bytes of luma, then the chroma rows; *pitch receives the pitch). Waits for the GPU.
EXPORT bool obs_vulkan_test_convert_yuv_buffer(gs_texture_t *src, const struct gs_yuv_conversion *conv, uint32_t bits,
					       bool sub_y, void *dst, uint64_t dst_size, uint32_t *pitch)
{
	gs_device *d = src->device;
	const uint32_t w = src->width, h = src->height, bytes = bits == 8 ? 1 : 2;
	const uint32_t p = (w * bytes + 255) & ~255u;
	const uint32_t chroma_rows = sub_y ? h / 2 : h;
	const VkDeviceSize size = (VkDeviceSize)p * (h + chroma_rows);
	*pitch = p;
	if (dst_size < size || (w & 1) || (sub_y && (h & 1))) {
		return false;
	}
	VkBuffer gpu = VK_NULL_HANDLE, host = VK_NULL_HANDLE;
	VmaAllocation gpu_alloc = VK_NULL_HANDLE, host_alloc = VK_NULL_HANDLE;
	void *mapped = nullptr;
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	if (vmaCreateBuffer(d->vma, &bi, &ai, &gpu, &gpu_alloc, nullptr) != VK_SUCCESS) {
		return false;
	}
	if (!vk_create_host_buffer(d, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, host, host_alloc, mapped)) {
		vmaDestroyBuffer(d->vma, gpu, gpu_alloc);
		return false;
	}
	target t;
	t.buffer = gpu;
	t.buffer_size = size;
	t.width = w;
	t.height = h;
	t.offsets[1] = p * h;
	t.pitches[0] = t.pitches[1] = p;
	bool ok = d->yuv.enabled && dispatch(d, src, make_variant(true, bits, sub_y, *conv), t, *conv);
	if (ok) {
		VkCommandBuffer cmd = vk_cmd(d);
		vk_barriers b;
		d->tracker.use_buffer(
			gpu, d->graphics.family,
			{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED}, b);
		vk_record_barriers(cmd, b);
		const VkBufferCopy r = {0, 0, size};
		vkCmdCopyBuffer(cmd, gpu, host, 1, &r);
		VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
		VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2(cmd, &di);
		ok = vk_flush(d);
		const uint64_t value = d->graphics.next_value - 1;
		VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
		wi.semaphoreCount = 1;
		wi.pSemaphores = &d->graphics.timeline;
		wi.pValues = &value;
		ok = ok && vk_check(d, vkWaitSemaphores(d->dev, &wi, UINT64_MAX)) == VK_SUCCESS;
	}
	if (ok) {
		vmaInvalidateAllocation(d->vma, host_alloc, 0, size);
		memcpy(dst, mapped, (size_t)size);
	}
	d->tracker.forget(gpu);
	d->frames.defer_destroy([vma = d->vma, gpu, gpu_alloc, host, host_alloc] {
		vmaDestroyBuffer(vma, gpu, gpu_alloc);
		vmaDestroyBuffer(vma, host, host_alloc);
	});
	return ok;
}
}
