#include "d3d11-vulkan-bridge.hpp"

#include <dxgi1_4.h>
#include <util/base.h>

#include <string.h>

namespace d3d11_vulkan_bridge {

bool renderer_luid(LUID &luid)
{
	gs_adapter_identity id = {};
	if (!gs_get_adapter_identity(&id) || !id.luid_valid) {
		return false;
	}
	memcpy(&luid, id.luid, sizeof(luid));
	return true;
}

bool same_luid(const LUID &a, const LUID &b)
{
	return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

bool create_device(const LUID &luid, ComPtr<ID3D11Device> &device, ComPtr<ID3D11DeviceContext> &context,
		   std::wstring *name, const char *log_prefix)
{
	ComPtr<IDXGIFactory4> factory;
	ComPtr<IDXGIAdapter> adapter;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	if (SUCCEEDED(hr)) {
		hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
	}
	if (FAILED(hr)) {
		blog(LOG_ERROR, "%s Vulkan: no DXGI adapter with LUID %08lX:%08lX (0x%08lX)", log_prefix,
		     (unsigned long)luid.HighPart, (unsigned long)luid.LowPart, (unsigned long)hr);
		return false;
	}
	static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	ComPtr<ID3D11Device> dev;
	ComPtr<ID3D11DeviceContext> ctx;
	hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			       levels, _countof(levels), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	if (FAILED(hr)) {
		blog(LOG_ERROR, "%s Vulkan: D3D11CreateDevice failed (0x%08lX)", log_prefix, (unsigned long)hr);
		return false;
	}
	if (name) {
		DXGI_ADAPTER_DESC ad;
		*name = SUCCEEDED(adapter->GetDesc(&ad)) ? ad.Description : L"?";
	}
	device = dev;
	context = ctx;
	return true;
}

void slots::destroy()
{
	for (int i = 0; i < count; ++i) {
		slot_t &sl = slot[i];
		if (sl.tex) {
			if (i == current) {
				gs_texture_release_sync(sl.tex, 0);
			}
			gs_texture_destroy(sl.tex);
		}
		if (sl.handle) {
			CloseHandle(sl.handle);
		}
		sl = slot_t{};
	}
	width = height = 0;
	format = DXGI_FORMAT_UNKNOWN;
	current = ready = -1;
	next = 0;
}

bool slots::reimport()
{
	if (!slot[0].tex) {
		return true;
	}
	for (int i = 0; i < count; ++i) {
		slot_t &sl = slot[i];
		gs_texture_destroy(sl.tex); /* a placeholder: no keyed mutex to release */
		sl.tex = nullptr;
	}
	/* A mutex the lost device held and did not release is never free again (tested for seconds), so there is no
	 * point waiting. Every slot is checked, not only `current`: draw_texture forgets `current` once it has submitted
	 * its release, so a loss between that release and the acquire of the newer slot leaves the old slot held with
	 * `current` -1. D3D11 holds none here (write releases before it returns). */
	current = -1;
	for (int i = 0; i < count; ++i) {
		const HRESULT hr = slot[i].km->AcquireSync(0, 0);
		if (hr != S_OK) {
			blog(LOG_WARNING, "%s Vulkan: keyed mutex of slot %d still held by the lost device (0x%08lX)",
			     log_prefix, i, (unsigned long)hr);
			return false;
		}
		slot[i].km->ReleaseSync(0);
	}
	for (int i = 0; i < count; ++i) {
		slot_t &sl = slot[i];
		sl.tex = gs_texture_open_nt_shared((uint32_t)(uintptr_t)sl.handle);
		if (!sl.tex) {
			blog(LOG_ERROR, "%s Vulkan: failed to import the shared texture again", log_prefix);
			return false;
		}
	}
	blog(LOG_DEBUG, "%s Vulkan: shared textures imported again", log_prefix);
	return true;
}

bool slots::create(ID3D11Device *device, uint32_t w, uint32_t h, DXGI_FORMAT f)
{
	destroy();
	blog(LOG_DEBUG, "%s Vulkan: shared textures %ux%u, format %d", log_prefix, w, h, (int)f);
	for (slot_t &sl : slot) {
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = f;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		ComPtr<IDXGIResource1> res;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &sl.d3d);
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
			blog(LOG_ERROR, "%s Vulkan: failed to create a shared %ux%u texture (0x%08lX)", log_prefix, w,
			     h, (unsigned long)hr);
			destroy();
			return false;
		}
		sl.tex = gs_texture_open_nt_shared((uint32_t)(uintptr_t)sl.handle);
		if (!sl.tex) {
			blog(LOG_ERROR, "%s Vulkan: failed to import the shared %ux%u texture", log_prefix, w, h);
			destroy();
			return false;
		}
	}
	width = w;
	height = h;
	format = f;
	return true;
}

bool slots::write(ID3D11DeviceContext *context, ID3D11Resource *frame, const D3D11_BOX *box)
{
	int w = next % count;
	if (w == current) {
		w = (w + 1) % count;
	}
	slot_t &sl = slot[w];
	const HRESULT hr = sl.km->AcquireSync(0, d3d11_acquire_ms);
	if (hr != S_OK) {
		blog(LOG_WARNING, "%s Vulkan: shared texture busy (0x%08lX), frame dropped", log_prefix,
		     (unsigned long)hr);
		return false;
	}
	if (box) {
		context->CopySubresourceRegion(sl.d3d.Get(), 0, 0, 0, 0, frame, 0, box);
	} else {
		context->CopyResource(sl.d3d.Get(), frame);
	}
	sl.km->ReleaseSync(0);
	context->Flush();
	ready = w;
	next = w + 1;
	return true;
}

gs_texture_t *slots::draw_texture()
{
	if (ready < 0) {
		return nullptr;
	}
	if (ready != current) {
		if (current >= 0) {
			gs_texture_release_sync(slot[current].tex, 0);
			current = -1;
		}
		if (gs_texture_acquire_sync(slot[ready].tex, 0, INFINITE) != 0) {
			blog(LOG_ERROR, "%s Vulkan: keyed mutex acquire failed", log_prefix);
			return nullptr;
		}
		current = ready;
	}
	return slot[current].tex;
}

} // namespace d3d11_vulkan_bridge
