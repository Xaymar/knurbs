/* DXGI desktop duplication on the Vulkan renderer (docs/graphics-interop.md, "Windows capture").
 *
 * libobs's gs_duplicator lives in libobs-d3d11 and duplicates with the renderer's own D3D11 device. This does the same
 * on a private D3D11 device of the adapter that drives the monitor, and hands frames to libobs as OS handles only:
 *
 * - Monitor on the renderer's adapter: each new desktop image is copied into a keyed-mutex NT-handle texture set
 *   imported once into Vulkan (shared/d3d11-vulkan-bridge). One GPU copy per frame, as gs_duplicator does.
 * - Monitor on another adapter (a laptop whose panel is driven by the other GPU): a D3D11 texture cannot be imported by
 *   another adapter's Vulkan device, so the image is copied into a staging texture, read back and uploaded (one GPU
 *   copy, one CPU readback, one upload). It is mapped one tick later, so the graphics thread never waits for the other
 *   GPU. Windows Graphics Capture does this copy inside the OS and is what "Automatic" picks there.
 *
 * As gs_duplicator: the desktop format is B8G8R8A8, or R16G16B16A16_FLOAT through IDXGIOutput5::DuplicateOutput1 (HDR
 * monitors, scRGB); every capture of a monitor shares one duplication, which takes at most one frame per video frame;
 * DXGI_ERROR_ACCESS_LOST (mode or resolution change, rotation, secure desktop, fullscreen switch) ends the duplication
 * and the source creates it again. Unlike gs_duplicator, a frame that only moved the pointer (LastPresentTime 0) is not
 * copied: the desktop image did not change. The cursor is drawn by the source (cursor-capture.c), as on D3D11.
 *
 * Device loss: a Vulkan-only loss leaves the D3D11 device and the duplication running and only replaces the imports
 * (the same NT handles imported again; the D3D11 textures keep the newest frame, so a static desktop, which sends no
 * new frames, is drawn again). If the lost device still holds a slot's keyed mutex, which it never gives back, the
 * slots are created again and the duplication is restarted on the same device: its first frame is the whole desktop.
 * Everything runs on the graphics thread. */

#include "dxgi-duplicator.h"

#include <d3d11-vulkan-bridge/d3d11-vulkan-bridge.hpp>

#include <dxgi1_6.h>
#include <obs.h>
#include <util/base.h>

#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

namespace bridge = d3d11_vulkan_bridge;

static const char *const log_prefix = "[dxgi-duplicator]";

/* Test hook: OBS_DXGI_DUPLICATOR_TEST_HDR=1 asks DXGI for R16G16B16A16_FLOAT desktop images only (DXGI converts the
 * SDR desktop) and treats the monitor as HDR, as on an HDR monitor. */
static bool test_force_hdr()
{
	static const bool force = [] {
		const char *v = getenv("OBS_DXGI_DUPLICATOR_TEST_HDR");
		return v && !strcmp(v, "1");
	}();
	return force;
}

/* The adapter (by LUID) and output showing `monitor`, from a new factory. */
static bool find_output(HMONITOR monitor, LUID &luid)
{
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return false;
	}
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
		ComPtr<IDXGIOutput> output;
		for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
			DXGI_OUTPUT_DESC desc;
			if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
				DXGI_ADAPTER_DESC1 ad;
				if (FAILED(adapter->GetDesc1(&ad))) {
					return false;
				}
				luid = ad.AdapterLuid;
				return true;
			}
		}
	}
	return false;
}

/* As libobs-d3d11's GetSdrMaxNits: the SDR white level Windows composes SDR content at on an HDR monitor. */
static float sdr_white_nits(HMONITOR monitor)
{
	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(monitor, &mi)) {
		return 80.f;
	}
	UINT32 n_paths = 0, n_modes = 0;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths;
	std::vector<DISPLAYCONFIG_MODE_INFO> modes;
	LONG r;
	do {
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &n_paths, &n_modes) != ERROR_SUCCESS) {
			return 80.f;
		}
		paths.resize(n_paths);
		modes.resize(n_modes);
		r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &n_paths, paths.data(), &n_modes, modes.data(), nullptr);
	} while (r == ERROR_INSUFFICIENT_BUFFER);
	if (r != ERROR_SUCCESS) {
		return 80.f;
	}
	for (UINT32 i = 0; i < n_paths; ++i) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof(source);
		source.header.adapterId = paths[i].sourceInfo.adapterId;
		source.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
		    wcscmp(mi.szDevice, source.viewGdiDeviceName) != 0) {
			continue;
		}
		DISPLAYCONFIG_SDR_WHITE_LEVEL level = {};
		level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
		level.header.size = sizeof(level);
		level.header.adapterId = paths[i].targetInfo.adapterId;
		level.header.id = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&level.header) == ERROR_SUCCESS) {
			return (float)((level.SDRWhiteLevel * 80) / 1000);
		}
		break;
	}
	return 80.f;
}

static void device_loss_release(void *data);
static void device_loss_rebuild(void *device, void *data);

struct dxgi_duplicator {
	HMONITOR monitor = nullptr;
	long refs = 1;

	LUID luid{};                /* the output's adapter */
	bool cross_adapter = false; /* not the renderer's adapter: frames go through the CPU */
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIOutput1> output;
	ComPtr<IDXGIOutputDuplication> duplication;

	bool hdr = false;
	float sdr_white = 80.f;
	gs_color_space color_space = GS_CS_SRGB;
	gs_monitor_info info{};

	uint64_t updated_at = 0; /* video frame time of the last update */
	bool restart = false;    /* restart the duplication for a whole image (after a Vulkan device loss) */
	bool lost = false;
	bool have_frame = false;
	uint32_t width = 0, height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

	/* renderer's adapter */
	bridge::slots slots{log_prefix};

	/* another adapter */
	ComPtr<ID3D11Texture2D> staging[2];
	int staging_next = 0;
	int staging_pending = -1; /* copied, not uploaded yet */
	int staging_last = -1;    /* uploaded last; kept until the copy after next */
	gs_texture_t *upload = nullptr;
	bool uploaded = false;

	bool start();
	bool copy_frame(ID3D11Texture2D *frame);
	bool upload_pending();
	void destroy_frames();
	~dxgi_duplicator();
};

static std::vector<dxgi_duplicator *> instances;

bool dxgi_duplicator::start()
{
	duplication.Clear();
	ComPtr<IDXGIOutput5> output5;
	HRESULT hr;
	if (SUCCEEDED(output->QueryInterface(&output5))) {
		static const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
		const UINT n = test_force_hdr() ? 1 : _countof(formats);
		hr = output5->DuplicateOutput1(device.Get(), 0, n, formats, &duplication);
		/* DuplicateOutput1 needs a per-monitor DPI aware (v2) process; DuplicateOutput gives SDR frames
		 * without. libobs-d3d11 fails there. */
		if (hr == DXGI_ERROR_UNSUPPORTED && !test_force_hdr()) {
			blog(LOG_WARNING,
			     "%s DuplicateOutput1 unsupported (the process is not per-monitor DPI aware?), "
			     "duplicating in SDR",
			     log_prefix);
			hdr = false;
			hr = output->DuplicateOutput(device.Get(), &duplication);
		}
	} else {
		hr = output->DuplicateOutput(device.Get(), &duplication);
	}
	if (FAILED(hr)) {
		blog(LOG_DEBUG, "%s DuplicateOutput failed (0x%08lX)", log_prefix, (unsigned long)hr);
		return false;
	}
	return true;
}

void dxgi_duplicator::destroy_frames()
{
	slots.destroy();
	for (ComPtr<ID3D11Texture2D> &s : staging) {
		s.Clear();
	}
	staging_pending = -1;
	staging_last = -1;
	if (upload) {
		gs_texture_destroy(upload);
		upload = nullptr;
	}
	uploaded = false;
	have_frame = false;
}

dxgi_duplicator::~dxgi_duplicator()
{
	gs_unregister_loss_callbacks(this);
	destroy_frames();
	duplication.Clear();
}

static gs_color_format gs_format(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? GS_RGBA16F : GS_BGRA;
}

bool dxgi_duplicator::upload_pending()
{
	if (staging_pending < 0) {
		return true;
	}
	ID3D11Texture2D *const s = staging[staging_pending].Get();
	staging_last = staging_pending;
	staging_pending = -1;
	D3D11_MAPPED_SUBRESOURCE map;
	const HRESULT hr = context->Map(s, 0, D3D11_MAP_READ, 0, &map);
	if (FAILED(hr)) {
		blog(LOG_ERROR, "%s Map failed (0x%08lX)", log_prefix, (unsigned long)hr);
		return false;
	}
	gs_texture_set_image(upload, (const uint8_t *)map.pData, map.RowPitch, false);
	context->Unmap(s, 0);
	uploaded = true;
	return true;
}

bool dxgi_duplicator::copy_frame(ID3D11Texture2D *frame)
{
	D3D11_TEXTURE2D_DESC desc;
	frame->GetDesc(&desc);
	const bool resized = desc.Width != width || desc.Height != height || desc.Format != format;
	if (resized) {
		destroy_frames();
		width = desc.Width;
		height = desc.Height;
		format = desc.Format;
		color_space = hdr ? GS_CS_709_SCRGB
				  : (format == DXGI_FORMAT_R16G16B16A16_FLOAT ? GS_CS_SRGB_16F : GS_CS_SRGB);
	}

	if (!cross_adapter) {
		if (!slots.matches(width, height, format) && !slots.create(device.Get(), width, height, format)) {
			return false;
		}
		if (slots.write(context.Get(), frame, nullptr)) {
			have_frame = true;
		}
		return true;
	}

	if (!staging[0]) {
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = width;
		td.Height = height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (ComPtr<ID3D11Texture2D> &s : staging) {
			const HRESULT hr = device->CreateTexture2D(&td, nullptr, &s);
			if (FAILED(hr)) {
				blog(LOG_ERROR, "%s failed to create a %ux%u staging texture (0x%08lX)", log_prefix,
				     width, height, (unsigned long)hr);
				destroy_frames();
				return false;
			}
		}
		upload = gs_texture_create(width, height, gs_format(format), 1, nullptr, GS_DYNAMIC);
		if (!upload) {
			destroy_frames();
			return false;
		}
		blog(LOG_DEBUG, "%s Vulkan: CPU copy textures %ux%u, format %d", log_prefix, width, height,
		     (int)format);
	}
	context->CopyResource(staging[staging_next].Get(), frame);
	context->Flush();
	staging_pending = staging_next;
	staging_next ^= 1;
	have_frame = true;
	return true;
}

/* ---- device loss ---------------------------------------------------------------------------------------------- */

/* The D3D11 device and the duplication do not depend on the Vulkan device: keep them. */
static void device_loss_release(void *data)
{
	UNUSED_PARAMETER(data);
}

static void device_loss_rebuild(void *device, void *data)
{
	UNUSED_PARAMETER(device);
	dxgi_duplicator *d = static_cast<dxgi_duplicator *>(data);
	LUID renderer;
	if (!bridge::renderer_luid(renderer) || bridge::same_luid(renderer, d->luid) == d->cross_adapter) {
		d->lost = true; /* the renderer moved to another adapter: the source creates the duplication again */
		return;
	}
	/* The imported textures are placeholders now. The D3D11 textures still hold the newest frame: import them
	 * again. The upload texture (other adapter) comes back without its contents: upload the last frame again,
	 * which its staging texture still holds. */
	if (d->cross_adapter) {
		if (d->staging_pending < 0) {
			d->staging_pending = d->staging_last;
		}
		return;
	}
	if (d->slots.reimport()) {
		return;
	}
	/* Last resort: new textures, and a new duplication for a whole desktop image. */
	d->destroy_frames();
	d->width = d->height = 0;
	d->format = DXGI_FORMAT_UNKNOWN;
	d->restart = true;
}

/* ---- API ------------------------------------------------------------------------------------------------------ */

extern "C" bool dxgi_duplicator_monitor_on_renderer(HMONITOR monitor)
{
	LUID luid, renderer;
	return monitor && find_output(monitor, luid) && bridge::renderer_luid(renderer) &&
	       bridge::same_luid(luid, renderer);
}

static dxgi_duplicator *create(HMONITOR monitor)
{
	LUID renderer;
	if (!bridge::renderer_luid(renderer)) {
		blog(LOG_ERROR, "%s Vulkan: the renderer's adapter has no LUID", log_prefix);
		return nullptr;
	}
	std::unique_ptr<dxgi_duplicator> d(new dxgi_duplicator);
	d->monitor = monitor;
	if (!find_output(monitor, d->luid)) {
		blog(LOG_DEBUG, "%s no adapter shows the monitor", log_prefix);
		return nullptr;
	}
	d->cross_adapter = !bridge::same_luid(d->luid, renderer);
	std::wstring adapter_name;
	if (!bridge::create_device(d->luid, d->device, d->context, &adapter_name, log_prefix)) {
		return nullptr;
	}

	/* The output from the device's own adapter, which DuplicateOutput needs. */
	ComPtr<IDXGIDevice> dxgi_device;
	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(d->device->QueryInterface(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter))) {
		return nullptr;
	}
	ComPtr<IDXGIOutput> output;
	DXGI_OUTPUT_DESC desc = {};
	for (UINT o = 0;; ++o) {
		if (adapter->EnumOutputs(o, &output) != S_OK) {
			blog(LOG_DEBUG, "%s the monitor is gone", log_prefix);
			return nullptr;
		}
		if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
			break;
		}
	}
	if (FAILED(output->QueryInterface(&d->output))) {
		blog(LOG_ERROR, "%s IDXGIOutput1 unavailable", log_prefix);
		return nullptr;
	}

	switch (desc.Rotation) {
	case DXGI_MODE_ROTATION_ROTATE90:
		d->info.rotation_degrees = 90;
		break;
	case DXGI_MODE_ROTATION_ROTATE180:
		d->info.rotation_degrees = 180;
		break;
	case DXGI_MODE_ROTATION_ROTATE270:
		d->info.rotation_degrees = 270;
		break;
	default:
		d->info.rotation_degrees = 0;
	}
	d->info.x = desc.DesktopCoordinates.left;
	d->info.y = desc.DesktopCoordinates.top;
	d->info.cx = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
	d->info.cy = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

	ComPtr<IDXGIOutput6> output6;
	DXGI_OUTPUT_DESC1 desc1;
	if (SUCCEEDED(output->QueryInterface(&output6)) && SUCCEEDED(output6->GetDesc1(&desc1))) {
		d->hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
	}
	d->hdr = d->hdr || test_force_hdr();
	d->sdr_white = d->hdr ? sdr_white_nits(monitor) : 80.f;

	if (!d->start()) {
		return nullptr;
	}

	blog(LOG_INFO, "%s Vulkan: duplicating %ls (%ldx%ld, rotation %ld, %s) on %ls, %s", log_prefix, desc.DeviceName,
	     d->info.cx, d->info.cy, d->info.rotation_degrees, d->hdr ? "HDR" : "SDR", adapter_name.c_str(),
	     d->cross_adapter ? "another adapter than the renderer's: frames are copied through the CPU"
			      : "the renderer's adapter: one GPU copy per frame into shared textures");

	const gs_device_loss callbacks = {device_loss_release, device_loss_rebuild, d.get()};
	gs_register_loss_callbacks(&callbacks);
	return d.release();
}

extern "C" struct dxgi_duplicator *dxgi_duplicator_create(HMONITOR monitor)
{
	for (dxgi_duplicator *d : instances) {
		if (d->monitor == monitor && !d->lost) {
			++d->refs;
			return d;
		}
	}
	dxgi_duplicator *d = create(monitor);
	if (d) {
		instances.push_back(d);
	}
	return d;
}

extern "C" void dxgi_duplicator_destroy(struct dxgi_duplicator *d)
{
	if (!d || --d->refs > 0) {
		return;
	}
	for (auto it = instances.begin(); it != instances.end(); ++it) {
		if (*it == d) {
			instances.erase(it);
			break;
		}
	}
	delete d;
}

extern "C" bool dxgi_duplicator_update_frame(struct dxgi_duplicator *d)
{
	if (d->lost) {
		return false;
	}
	const uint64_t now = obs_get_video_frame_time();
	if (now && now == d->updated_at) {
		return true;
	}
	d->updated_at = now;

	if (d->restart) {
		d->restart = false;
		if (!d->start()) {
			d->lost = true;
			return false;
		}
	}

	/* The previous tick's copy has had a frame's time to finish on the other adapter. */
	if (d->cross_adapter && !d->upload_pending()) {
		d->lost = true;
		return false;
	}

	DXGI_OUTDUPL_FRAME_INFO frame_info;
	ComPtr<IDXGIResource> resource;
	const HRESULT hr = d->duplication->AcquireNextFrame(0, &frame_info, &resource);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
		return true;
	}
	if (hr == DXGI_ERROR_ACCESS_LOST) {
		blog(LOG_DEBUG, "%s duplication lost (mode change, secure desktop or fullscreen switch)", log_prefix);
		d->lost = true;
		return false;
	}
	if (FAILED(hr)) {
		blog(LOG_ERROR, "%s AcquireNextFrame failed (0x%08lX)", log_prefix, (unsigned long)hr);
		d->lost = true;
		return false;
	}

	/* LastPresentTime 0: only the pointer changed, the desktop image is the one already copied. */
	if (frame_info.LastPresentTime.QuadPart != 0 || !d->have_frame) {
		ComPtr<ID3D11Texture2D> tex;
		if (SUCCEEDED(resource->QueryInterface(&tex))) {
			if (!d->copy_frame(tex.Get())) {
				d->duplication->ReleaseFrame();
				d->lost = true;
				return false;
			}
		} else {
			blog(LOG_ERROR, "%s frame is no ID3D11Texture2D", log_prefix);
		}
	}
	d->duplication->ReleaseFrame();
	return true;
}

extern "C" bool dxgi_duplicator_get_size(const struct dxgi_duplicator *d, uint32_t *width, uint32_t *height)
{
	if (!d->have_frame) {
		return false;
	}
	*width = d->width;
	*height = d->height;
	return true;
}

extern "C" gs_texture_t *dxgi_duplicator_get_texture(struct dxgi_duplicator *d)
{
	if (d->cross_adapter) {
		return d->uploaded ? d->upload : nullptr;
	}
	return d->slots.draw_texture();
}

extern "C" enum gs_color_space dxgi_duplicator_get_color_space(const struct dxgi_duplicator *d)
{
	return d->color_space;
}

extern "C" float dxgi_duplicator_get_sdr_white_level(const struct dxgi_duplicator *d)
{
	return d->sdr_white;
}

extern "C" void dxgi_duplicator_get_monitor_info(const struct dxgi_duplicator *d, struct gs_monitor_info *info)
{
	*info = d->info;
}
