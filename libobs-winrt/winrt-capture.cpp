#include "winrt-capture.h"

#include <d3d11_1.h>
#include <dxgi1_4.h>

extern "C" EXPORT BOOL winrt_capture_supported()
try {
	/* no contract for IGraphicsCaptureItemInterop, verify 10.0.18362.0 */
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
		L"Windows.Foundation.UniversalApiContract", 8);
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

extern "C" EXPORT BOOL winrt_capture_cursor_toggle_supported()
try {
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
		L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled");
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_cursor_toggle_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_cursor_toggle_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

template<typename T>
static winrt::com_ptr<T> GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const &object)
{
	auto access = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
	winrt::com_ptr<T> result;
	winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
	return result;
}

static bool get_client_box(HWND window, uint32_t width, uint32_t height, D3D11_BOX *client_box)
{
	RECT client_rect{}, window_rect{};
	POINT upper_left{};

	/* check iconic (minimized) twice, ABA is very unlikely */
	bool client_box_available = !IsIconic(window) && GetClientRect(window, &client_rect) && !IsIconic(window) &&
				    (client_rect.right > 0) && (client_rect.bottom > 0) &&
				    (DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect,
							   sizeof(window_rect)) == S_OK) &&
				    ClientToScreen(window, &upper_left);
	if (client_box_available) {
		const uint32_t left = (upper_left.x > window_rect.left) ? (upper_left.x - window_rect.left) : 0;
		client_box->left = left;

		const uint32_t top = (upper_left.y > window_rect.top) ? (upper_left.y - window_rect.top) : 0;
		client_box->top = top;

		uint32_t texture_width = 1;
		if (width > left) {
			texture_width = std::min(width - left, (uint32_t)client_rect.right);
		}

		uint32_t texture_height = 1;
		if (height > top) {
			texture_height = std::min(height - top, (uint32_t)client_rect.bottom);
		}

		client_box->right = left + texture_width;
		client_box->bottom = top + texture_height;

		client_box->front = 0;
		client_box->back = 1;

		client_box_available = (client_box->right <= width) && (client_box->bottom <= height);
	}

	return client_box_available;
}

/* Test hook: OBS_WINRT_TEST_HDR=1 captures as if every monitor were HDR (RGBA16F frames, scRGB). */
static bool test_force_hdr()
{
	static const bool force = [] {
		const char *v = getenv("OBS_WINRT_TEST_HDR");
		return v && !strcmp(v, "1");
	}();
	return force;
}

static DXGI_FORMAT get_pixel_format(HWND window, HMONITOR monitor, BOOL force_sdr)
{
	static constexpr DXGI_FORMAT sdr_format = DXGI_FORMAT_B8G8R8A8_UNORM;

	if (force_sdr) {
		return sdr_format;
	}

	if (test_force_hdr()) {
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	}

	if (window) {
		monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	}

	return (monitor && gs_is_monitor_hdr(monitor)) ? DXGI_FORMAT_R16G16B16A16_FLOAT : sdr_format;
}

/* ---- Vulkan renderer --------------------------------------------------------------------------------------------
 * WGC delivers frames to a D3D11 device only. On Vulkan there is none in the process, so captures share a private
 * D3D11 device on the renderer's adapter (by LUID). Each capture copies a frame into one of three D3D11 textures
 * (NT handle, keyed mutex) imported once into Vulkan: one copy per frame, as on D3D11.
 *
 * Everything runs on the graphics thread (WGC frames arrive through its dispatcher queue, see winrt-dispatch.cpp),
 * so the slot state needs no lock. Key 0 is used both ways; ownership is decided by that state:
 * - Vulkan holds the slot it draws (`current`) from the first draw after it was written until a newer one is drawn,
 *   so a static window costs no keyed-mutex work per frame.
 * - A frame is written to a slot other than `current`, round robin. Vulkan submits the release of a slot as soon as
 *   it stops drawing it (gs_texture_release_sync), so D3D11 waits at most for the GPU to reach that release. */

static constexpr int vk_slot_count = 3;
static constexpr DWORD vk_d3d11_acquire_ms = 500;

struct vk_bridge {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device{nullptr};
	LUID luid{};
	long refs = 0;
};

static vk_bridge *bridge;

static bool renderer_is_vulkan()
{
	return gs_get_device_type() == GS_DEVICE_VULKAN;
}

static void bridge_reset()
{
	if (bridge) {
		bridge->winrt_device = nullptr;
		bridge->context = nullptr;
		bridge->device = nullptr;
	}
}

/* The private D3D11 device for the current Vulkan device's adapter; graphics context entered. */
static bool bridge_create()
{
	gs_adapter_identity id = {};
	if (!gs_get_adapter_identity(&id) || !id.luid_valid) {
		blog(LOG_ERROR, "[winrt-capture] Vulkan: the renderer's adapter has no LUID");
		return false;
	}
	LUID luid;
	memcpy(&luid, id.luid, sizeof(luid));
	if (!bridge) {
		bridge = new vk_bridge;
	}
	if (bridge->device && !memcmp(&bridge->luid, &luid, sizeof(luid)) &&
	    bridge->device->GetDeviceRemovedReason() == S_OK) {
		return true;
	}
	bridge_reset();

	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter> adapter;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if (SUCCEEDED(hr)) {
		hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
	}
	if (FAILED(hr)) {
		blog(LOG_ERROR, "[winrt-capture] Vulkan: no DXGI adapter with the renderer's LUID (0x%08lX)",
		     (unsigned long)hr);
		return false;
	}
	static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			       levels, _countof(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
	if (FAILED(hr)) {
		blog(LOG_ERROR, "[winrt-capture] Vulkan: D3D11CreateDevice failed (0x%08lX)", (unsigned long)hr);
		return false;
	}
	ComPtr<IDXGIDevice> dxgi_device;
	winrt::com_ptr<IInspectable> inspectable;
	if (FAILED(device->QueryInterface(&dxgi_device)) ||
	    FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put()))) {
		blog(LOG_ERROR, "[winrt-capture] Vulkan: failed to get WinRT device");
		return false;
	}
	bridge->device = device;
	bridge->context = context;
	bridge->winrt_device = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	bridge->luid = luid;
	DXGI_ADAPTER_DESC ad;
	if (SUCCEEDED(adapter->GetDesc(&ad))) {
		blog(LOG_INFO, "[winrt-capture] Vulkan: capturing through a D3D11 device on %ls", ad.Description);
	}
	return true;
}

/* Whether the private device is alive and on the current Vulkan device's adapter; graphics context entered. */
static bool bridge_current()
{
	gs_adapter_identity id = {};
	return bridge && bridge->device && bridge->device->GetDeviceRemovedReason() == S_OK &&
	       gs_get_adapter_identity(&id) && id.luid_valid && !memcmp(&bridge->luid, id.luid, sizeof(LUID));
}

static void bridge_release()
{
	if (bridge && --bridge->refs <= 0) {
		delete bridge;
		bridge = nullptr;
	}
}

struct vk_slot {
	ComPtr<ID3D11Texture2D> d3d;
	ComPtr<IDXGIKeyedMutex> km;
	HANDLE handle = nullptr;
	gs_texture_t *tex = nullptr;
};

struct vk_slots {
	vk_slot slot[vk_slot_count];
	uint32_t width = 0, height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	int current = -1; /* held by Vulkan, drawn */
	int ready = -1;   /* newest frame */
	int next = 0;
};

/* Graphics context entered. */
static void slots_destroy(vk_slots &s)
{
	for (int i = 0; i < vk_slot_count; ++i) {
		vk_slot &sl = s.slot[i];
		if (sl.tex) {
			if (i == s.current) {
				gs_texture_release_sync(sl.tex, 0);
			}
			gs_texture_destroy(sl.tex);
		}
		if (sl.handle) {
			CloseHandle(sl.handle);
		}
		sl = vk_slot{};
	}
	s.width = s.height = 0;
	s.format = DXGI_FORMAT_UNKNOWN;
	s.current = s.ready = -1;
	s.next = 0;
}

static bool slots_create(vk_slots &s, uint32_t width, uint32_t height, DXGI_FORMAT format)
{
	slots_destroy(s);
	blog(LOG_DEBUG, "[winrt-capture] Vulkan: shared textures %ux%u, format %d", width, height, (int)format);
	for (vk_slot &sl : s.slot) {
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = width;
		td.Height = height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		ComPtr<IDXGIResource1> res;
		HRESULT hr = bridge->device->CreateTexture2D(&td, nullptr, &sl.d3d);
		if (SUCCEEDED(hr)) {
			hr = sl.d3d->QueryInterface(&sl.km);
		}
		if (SUCCEEDED(hr)) {
			hr = sl.d3d->QueryInterface(&res);
		}
		if (SUCCEEDED(hr)) {
			hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
						     nullptr, &sl.handle);
		}
		if (FAILED(hr)) {
			blog(LOG_ERROR, "[winrt-capture] Vulkan: failed to create a shared %ux%u texture (0x%08lX)",
			     width, height, (unsigned long)hr);
			slots_destroy(s);
			return false;
		}
		sl.tex = gs_texture_open_nt_shared((uint32_t)(uintptr_t)sl.handle);
		if (!sl.tex) {
			blog(LOG_ERROR, "[winrt-capture] Vulkan: failed to import the shared %ux%u texture", width,
			     height);
			slots_destroy(s);
			return false;
		}
	}
	s.width = width;
	s.height = height;
	s.format = format;
	return true;
}

/* Copies a frame into a slot Vulkan does not hold. Graphics context entered. */
static bool slots_write(vk_slots &s, ID3D11Texture2D *frame, const D3D11_BOX *box)
{
	int w = s.next % vk_slot_count;
	if (w == s.current) {
		w = (w + 1) % vk_slot_count;
	}
	vk_slot &sl = s.slot[w];
	const HRESULT hr = sl.km->AcquireSync(0, vk_d3d11_acquire_ms);
	if (hr != S_OK) {
		blog(LOG_WARNING, "[winrt-capture] Vulkan: shared texture busy (0x%08lX), frame dropped",
		     (unsigned long)hr);
		return false;
	}
	if (box) {
		bridge->context->CopySubresourceRegion(sl.d3d.Get(), 0, 0, 0, 0, frame, 0, box);
	} else {
		bridge->context->CopyResource(sl.d3d.Get(), frame);
	}
	sl.km->ReleaseSync(0);
	bridge->context->Flush();
	s.ready = w;
	s.next = w + 1;
	return true;
}

/* The texture to draw: makes Vulkan hold the newest frame's slot. Graphics context entered. */
static gs_texture_t *slots_draw_texture(vk_slots &s)
{
	if (s.ready < 0) {
		return nullptr;
	}
	if (s.ready != s.current) {
		if (s.current >= 0) {
			gs_texture_release_sync(s.slot[s.current].tex, 0);
			s.current = -1;
		}
		if (gs_texture_acquire_sync(s.slot[s.ready].tex, 0, INFINITE) != 0) {
			blog(LOG_ERROR, "[winrt-capture] Vulkan: keyed mutex acquire failed");
			return nullptr;
		}
		s.current = s.ready;
	}
	return s.slot[s.current].tex;
}

struct winrt_capture {
	HWND window;
	BOOL client_area;
	BOOL force_sdr;
	HMONITOR monitor;
	DXGI_FORMAT format;

	bool capture_cursor;
	BOOL cursor_visible;

	gs_texture_t *texture;
	bool texture_written;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{nullptr};
	ComPtr<ID3D11DeviceContext> context;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool{nullptr};
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
	winrt::Windows::Graphics::SizeInt32 last_size;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker closed;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived;

	uint32_t texture_width;
	uint32_t texture_height;
	D3D11_BOX client_box;

	BOOL active;
	struct winrt_capture *next;

	bool vulkan; /* renderer is Vulkan: frames go through `bridge` and `slots` */
	vk_slots slots;

	void on_closed(winrt::Windows::Graphics::Capture::GraphicsCaptureItem const &,
		       winrt::Windows::Foundation::IInspectable const &)
	{
		active = FALSE;
	}

	void copy_frame_d3d11(ID3D11Texture2D *frame_surface, DXGI_FORMAT frame_format)
	{
		if (texture) {
			if (texture_width != gs_texture_get_width(texture) ||
			    texture_height != gs_texture_get_height(texture)) {
				gs_texture_destroy(texture);
				texture = nullptr;
			}
		}

		if (!texture) {
			const gs_color_format color_format = frame_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? GS_RGBA16F
													    : GS_BGRA;
			texture = gs_texture_create(texture_width, texture_height, color_format, 1, NULL, 0);
		}

		if (client_area) {
			context->CopySubresourceRegion((ID3D11Texture2D *)gs_texture_get_obj(texture), 0, 0, 0, 0,
						       frame_surface, 0, &client_box);
		} else {
			/* if they gave an SRV, we could avoid this copy */
			context->CopyResource((ID3D11Texture2D *)gs_texture_get_obj(texture), frame_surface);
		}

		texture_written = true;
	}

	void copy_frame_vulkan(ID3D11Texture2D *frame_surface, DXGI_FORMAT frame_format)
	{
		const bool same = slots.width == texture_width && slots.height == texture_height &&
				  slots.format == frame_format;
		if (!same && !slots_create(slots, texture_width, texture_height, frame_format)) {
			return;
		}
		if (slots_write(slots, frame_surface, client_area ? &client_box : nullptr)) {
			texture_written = true;
		}
	}

	void on_frame_arrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender,
			      winrt::Windows::Foundation::IInspectable const &)
	{
		const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
		const winrt::Windows::Graphics::SizeInt32 frame_content_size = frame.ContentSize();

		winrt::com_ptr<ID3D11Texture2D> frame_surface =
			GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

		/* need GetDesc because ContentSize is not reliable */
		D3D11_TEXTURE2D_DESC desc;
		frame_surface->GetDesc(&desc);

		obs_enter_graphics();

		if (desc.Format == get_pixel_format(window, monitor, force_sdr)) {
			if (!client_area || get_client_box(window, desc.Width, desc.Height, &client_box)) {
				if (client_area) {
					texture_width = client_box.right - client_box.left;
					texture_height = client_box.bottom - client_box.top;
				} else {
					texture_width = desc.Width;
					texture_height = desc.Height;
				}

				if (vulkan) {
					copy_frame_vulkan(frame_surface.get(), desc.Format);
				} else {
					copy_frame_d3d11(frame_surface.get(), desc.Format);
				}
			}

			if (frame_content_size.Width != last_size.Width ||
			    frame_content_size.Height != last_size.Height) {
				format = desc.Format;
				frame_pool.Recreate(
					device,
					static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(format), 2,
					frame_content_size);

				last_size = frame_content_size;
			}
		} else {
			active = FALSE;
		}

		obs_leave_graphics();
	}
};

static struct winrt_capture *capture_list;

static void capture_teardown(winrt_capture *capture)
{
	capture->active = FALSE;

	capture->frame_arrived.revoke();

	try {
		capture->frame_pool.Close();
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X)", winrt::to_hresult().value);
	}

	try {
		capture->session.Close();
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X)", winrt::to_hresult().value);
	}

	capture->session = nullptr;
	capture->frame_pool = nullptr;
	capture->context = nullptr;
	capture->device = nullptr;
	capture->item = nullptr;
}

/* On Vulkan a lost renderer device leaves the private D3D11 device, and so the capture session, alive: only the
 * imported textures need replacing (winrt_capture_device_loss_rebuild). */
static void winrt_capture_device_loss_release(void *data)
{
	winrt_capture *capture = static_cast<winrt_capture *>(data);
	if (capture->vulkan && bridge && bridge->device && bridge->device->GetDeviceRemovedReason() == S_OK) {
		return;
	}
	capture_teardown(capture);
}

static bool winrt_capture_border_toggle_supported()
try {
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
		L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_border_toggle_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_border_toggle_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

static winrt::Windows::Graphics::Capture::GraphicsCaptureItem
winrt_capture_create_item(IGraphicsCaptureItemInterop *const interop_factory, HWND window, HMONITOR monitor)
{
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item = {nullptr};
	if (window) {
		try {
			const HRESULT hr = interop_factory->CreateForWindow(
				window, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
				reinterpret_cast<void **>(winrt::put_abi(item)));
			if (FAILED(hr)) {
				blog(LOG_ERROR, "CreateForWindow (0x%08X)", hr);
			}
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "CreateForWindow (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "CreateForWindow (0x%08X)", winrt::to_hresult().value);
		}
	} else {
		assert(monitor);

		try {
			const HRESULT hr = interop_factory->CreateForMonitor(
				monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
				reinterpret_cast<void **>(winrt::put_abi(item)));
			if (FAILED(hr)) {
				blog(LOG_ERROR, "CreateForMonitor (0x%08X)", hr);
			}
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "CreateForMonitor (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "CreateForMonitor (0x%08X)", winrt::to_hresult().value);
		}
	}

	return item;
}

static void winrt_capture_device_loss_rebuild(void *device_void, void *data)
{
	winrt_capture *capture = static_cast<winrt_capture *>(data);

	if (capture->vulkan) {
		/* Imported textures are placeholders after a device loss; new ones are made with the next frame. */
		slots_destroy(capture->slots);
		capture->texture = nullptr;
		capture->texture_written = false;
		if (capture->session) {
			if (bridge_current()) {
				return; /* the session kept running (winrt_capture_device_loss_release) */
			}
			capture_teardown(capture);
		}
	}

	auto activation_factory =
		winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
	auto interop_factory = activation_factory.as<IGraphicsCaptureItemInterop>();
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item =
		winrt_capture_create_item(interop_factory.get(), capture->window, capture->monitor);
	if (!item) {
		return;
	}

	/* On Vulkan `device_void` is a gs_vulkan_device: capture through the private D3D11 device instead. */
	if (capture->vulkan && !bridge_create()) {
		return;
	}
	ID3D11Device *const d3d_device = capture->vulkan ? bridge->device.Get() : (ID3D11Device *)device_void;
	ComPtr<IDXGIDevice> dxgi_device;
	if (FAILED(d3d_device->QueryInterface(&dxgi_device))) {
		blog(LOG_ERROR, "Failed to get DXGI device");
	}

	winrt::com_ptr<IInspectable> inspectable;
	if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put()))) {
		blog(LOG_ERROR, "Failed to get WinRT device");
	}

	const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device =
		inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool =
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
			device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(capture->format), 2,
			capture->last_size);
	const winrt::Windows::Graphics::Capture::GraphicsCaptureSession session = frame_pool.CreateCaptureSession(item);

	if (winrt_capture_border_toggle_supported()) {
		winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
			winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
			.get();
		session.IsBorderRequired(false);
	}

	if (winrt_capture_cursor_toggle_supported()) {
		session.IsCursorCaptureEnabled(capture->capture_cursor && capture->cursor_visible);
	}

	capture->item = item;
	capture->device = device;
	d3d_device->GetImmediateContext(&capture->context);
	capture->frame_pool = frame_pool;
	capture->session = session;
	capture->frame_arrived =
		frame_pool.FrameArrived(winrt::auto_revoke, {capture, &winrt_capture::on_frame_arrived});

	try {
		session.StartCapture();
		capture->active = TRUE;
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "StartCapture (0x%08X): %s", err.code().value, winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "StartCapture (0x%08X)", winrt::to_hresult().value);
	}
}

static struct winrt_capture *winrt_capture_init_internal(BOOL cursor, HWND window, BOOL client_area, BOOL force_sdr,
							 HMONITOR monitor)
try {
	obs_enter_graphics();
	const bool vulkan = renderer_is_vulkan();
	const bool bridged = vulkan && bridge_create();
	if (bridged) {
		++bridge->refs;
	}
	obs_leave_graphics();
	if (vulkan && !bridged) {
		return nullptr;
	}
	/* Holds the bridge reference until the capture owns it (or the init failed). */
	struct bridge_ref {
		bool held;
		~bridge_ref()
		{
			if (held) {
				obs_enter_graphics();
				bridge_release();
				obs_leave_graphics();
			}
		}
	} ref{bridged};

	ID3D11Device *const d3d_device = vulkan ? bridge->device.Get() : (ID3D11Device *)gs_get_device_obj();
	ComPtr<IDXGIDevice> dxgi_device;

	HRESULT hr = d3d_device->QueryInterface(&dxgi_device);
	if (FAILED(hr)) {
		blog(LOG_ERROR, "Failed to get DXGI device");
		return nullptr;
	}

	winrt::com_ptr<IInspectable> inspectable;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put());
	if (FAILED(hr)) {
		blog(LOG_ERROR, "Failed to get WinRT device");
		return nullptr;
	}

	auto activation_factory =
		winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
	auto interop_factory = activation_factory.as<IGraphicsCaptureItemInterop>();
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item =
		winrt_capture_create_item(interop_factory.get(), window, monitor);
	if (!item) {
		return nullptr;
	}

	const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device =
		inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	const winrt::Windows::Graphics::SizeInt32 size = item.Size();
	const DXGI_FORMAT format = get_pixel_format(window, monitor, force_sdr);
	const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool =
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
			device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(format), 2, size);
	const winrt::Windows::Graphics::Capture::GraphicsCaptureSession session = frame_pool.CreateCaptureSession(item);

	if (winrt_capture_border_toggle_supported()) {
		winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
			winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
			.get();
		session.IsBorderRequired(false);
	}

	/* disable cursor capture if possible since ours performs better */
	const BOOL cursor_toggle_supported = winrt_capture_cursor_toggle_supported();
	if (cursor_toggle_supported) {
		session.IsCursorCaptureEnabled(cursor);
	}

	struct winrt_capture *capture = new winrt_capture{};
	capture->window = window;
	capture->client_area = client_area;
	capture->force_sdr = force_sdr;
	capture->monitor = monitor;
	capture->format = format;
	capture->capture_cursor = cursor && cursor_toggle_supported;
	capture->cursor_visible = cursor;
	capture->vulkan = vulkan;
	ref.held = false; /* the capture's now; winrt_capture_free releases it */
	capture->item = item;
	capture->device = device;
	d3d_device->GetImmediateContext(&capture->context);
	capture->frame_pool = frame_pool;
	capture->session = session;
	capture->last_size = size;
	capture->closed = item.Closed(winrt::auto_revoke, {capture, &winrt_capture::on_closed});
	capture->frame_arrived =
		frame_pool.FrameArrived(winrt::auto_revoke, {capture, &winrt_capture::on_frame_arrived});
	capture->next = capture_list;
	capture_list = capture;

	session.StartCapture();
	capture->active = TRUE;

	gs_device_loss callbacks;
	callbacks.device_loss_release = winrt_capture_device_loss_release;
	callbacks.device_loss_rebuild = winrt_capture_device_loss_rebuild;
	callbacks.data = capture;
	gs_register_loss_callbacks(&callbacks);

	return capture;

} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_init (0x%08X): %s", err.code().value, winrt::to_string(err.message()).c_str());
	return nullptr;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_init (0x%08X)", winrt::to_hresult().value);
	return nullptr;
}

extern "C" EXPORT struct winrt_capture *winrt_capture_init_window(BOOL cursor, HWND window, BOOL client_area,
								  BOOL force_sdr)
{
	return winrt_capture_init_internal(cursor, window, client_area, force_sdr, NULL);
}

extern "C" EXPORT struct winrt_capture *winrt_capture_init_monitor(BOOL cursor, HMONITOR monitor, BOOL force_sdr)
{
	return winrt_capture_init_internal(cursor, NULL, false, force_sdr, monitor);
}

extern "C" EXPORT void winrt_capture_free(struct winrt_capture *capture)
{
	if (capture) {
		struct winrt_capture *current = capture_list;
		if (current == capture) {
			capture_list = capture->next;
		} else {
			struct winrt_capture *previous;
			do {
				previous = current;
				current = current->next;
			} while (current != capture);

			previous->next = current->next;
		}

		obs_enter_graphics();
		gs_unregister_loss_callbacks(capture);
		capture->frame_arrived.revoke();
		capture->closed.revoke();
		if (capture->vulkan) {
			slots_destroy(capture->slots); /* capture->texture is one of them */
		} else {
			gs_texture_destroy(capture->texture);
		}
		obs_leave_graphics();

		try {
			if (capture->frame_pool) {
				capture->frame_pool.Close();
			}
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X)", winrt::to_hresult().value);
		}

		try {
			if (capture->session) {
				capture->session.Close();
			}
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X)", winrt::to_hresult().value);
		}

		const bool vulkan = capture->vulkan;
		delete capture;

		if (vulkan) { /* after the frame pool and the session, which use its device */
			obs_enter_graphics();
			bridge_release();
			obs_leave_graphics();
		}
	}
}

extern "C" EXPORT BOOL winrt_capture_active(const struct winrt_capture *capture)
{
	return capture->active;
}

extern "C" EXPORT BOOL winrt_capture_show_cursor(struct winrt_capture *capture, BOOL visible)
{
	BOOL success = FALSE;

	try {
		if (capture->capture_cursor) {
			if (capture->cursor_visible != visible) {
				capture->session.IsCursorCaptureEnabled(visible);
				capture->cursor_visible = visible;
			}
		}

		success = TRUE;
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "GraphicsCaptureSession::IsCursorCaptureEnabled (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "GraphicsCaptureSession::IsCursorCaptureEnabled (0x%08X)", winrt::to_hresult().value);
	}

	return success;
}

extern "C" EXPORT enum gs_color_space winrt_capture_get_color_space(const struct winrt_capture *capture)
{
	return (capture->format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? GS_CS_709_EXTENDED : GS_CS_SRGB;
}

extern "C" EXPORT void winrt_capture_render(struct winrt_capture *capture)
{
	if (capture->vulkan) {
		capture->texture = capture->texture_written ? slots_draw_texture(capture->slots) : nullptr;
		if (!capture->texture) {
			return;
		}
	}
	if (capture->texture_written) {
		const char *tech_name = "Draw";
		float multiplier = 1.f;
		const gs_color_space current_space = gs_get_color_space();
		if (capture->format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
			switch (current_space) {
			case GS_CS_SRGB:
			case GS_CS_SRGB_16F:
				tech_name = "DrawMultiplyTonemap";
				multiplier = 80.f / obs_get_video_sdr_white_level();
				break;
			case GS_CS_709_EXTENDED:
				tech_name = "DrawMultiply";
				multiplier = 80.f / obs_get_video_sdr_white_level();
			}
		} else if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.f;
		}

		gs_effect_t *const effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_technique_t *tech = gs_effect_get_technique(effect, tech_name);

		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		gs_texture_t *const texture = capture->texture;
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);

		const size_t passes = gs_technique_begin(tech);
		for (size_t i = 0; i < passes; i++) {
			if (gs_technique_begin_pass(tech, i)) {
				gs_draw_sprite(texture, 0, 0, 0);

				gs_technique_end_pass(tech);
			}
		}
		gs_technique_end(tech);

		gs_blend_state_pop();

		gs_enable_framebuffer_srgb(previous);
	}
}

extern "C" EXPORT uint32_t winrt_capture_width(const struct winrt_capture *capture)
{
	return capture ? capture->texture_width : 0;
}

extern "C" EXPORT uint32_t winrt_capture_height(const struct winrt_capture *capture)
{
	return capture ? capture->texture_height : 0;
}

extern "C" EXPORT void winrt_capture_thread_start()
{
	struct winrt_capture *capture = capture_list;
	void *const device = gs_get_device_obj();
	while (capture) {
		winrt_capture_device_loss_rebuild(device, capture);
		capture = capture->next;
	}
}

extern "C" EXPORT void winrt_capture_thread_stop()
{
	struct winrt_capture *capture = capture_list;
	while (capture) {
		capture_teardown(capture);
		capture = capture->next;
	}
}
