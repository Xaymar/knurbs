// D3D11 producers on the Vulkan renderer (docs/graphics-interop.md, "Windows capture").
//
// Windows Graphics Capture and DXGI desktop duplication deliver frames to a D3D11 device only. On Vulkan there is none
// in the process, so a capture creates a private D3D11 device (create_device, by adapter LUID) and copies each frame
// into one of the textures of a `slots` set: three D3D11 textures with an NT handle and a keyed mutex, imported once
// into Vulkan with gs_texture_open_nt_shared. That is one GPU copy per frame, as on D3D11.
//
// Key 0 is used both ways; ownership is decided by the slot state, which is only touched from the graphics thread:
// - Vulkan holds the slot it draws (`current`) from its first draw after it was written until a newer one is drawn,
//   so a static picture costs no keyed-mutex work per frame.
// - A frame is written to a slot other than `current`, round robin. Vulkan submits the release of a slot as soon as it
//   stops drawing it (gs_texture_release_sync), so D3D11 waits at most for the GPU to reach that release.
//
// Only OS handles cross from D3D11 to libobs, so a producer using this can move to another process unchanged.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <graphics/graphics.h>
#include <util/windows/ComPtr.hpp>

#include <string>

namespace d3d11_vulkan_bridge {

/* The LUID of the renderer's adapter; graphics context entered. */
bool renderer_luid(LUID &luid);

bool same_luid(const LUID &a, const LUID &b);

/* A D3D11 device (BGRA support, feature level 11) on the adapter with `luid`. `name` gets the adapter's description.
 * Messages start with `log_prefix`. */
bool create_device(const LUID &luid, ComPtr<ID3D11Device> &device, ComPtr<ID3D11DeviceContext> &context,
		   std::wstring *name, const char *log_prefix);

class slots {
public:
	static constexpr int count = 3;
	static constexpr DWORD d3d11_acquire_ms = 500;

	explicit slots(const char *log_prefix_) : log_prefix(log_prefix_) {}
	slots(const slots &) = delete;
	slots &operator=(const slots &) = delete;

	/* Whether the set holds textures of this size and format. */
	bool matches(uint32_t w, uint32_t h, DXGI_FORMAT f) const
	{
		return width == w && height == h && format == f && slot[0].tex;
	}

	/* (Re)creates the textures on `device` and imports them; graphics context entered. */
	bool create(ID3D11Device *device, uint32_t w, uint32_t h, DXGI_FORMAT f);

	/* Graphics context entered. */
	void destroy();

	/* After a Vulkan device loss: imports the same D3D11 textures again (the old imports are placeholders), keeping
	 * their contents, so the newest frame is drawn again without a new one. False if a slot's keyed mutex is still
	 * held by the lost device; then destroy() and create() instead. Graphics context entered. */
	bool reimport();

	/* Copies `frame` (or `box` of it) into a slot Vulkan does not hold and flushes `context`. False: dropped (the slot
	 * stayed busy, see d3d11_acquire_ms). Graphics context entered. */
	bool write(ID3D11DeviceContext *context, ID3D11Resource *frame, const D3D11_BOX *box);

	/* The texture to draw: makes Vulkan hold the newest frame's slot. Null before the first frame. Graphics context
	 * entered. */
	gs_texture_t *draw_texture();

	bool has_frame() const { return ready >= 0; }
	uint32_t get_width() const { return width; }
	uint32_t get_height() const { return height; }
	DXGI_FORMAT get_format() const { return format; }

private:
	struct slot_t {
		ComPtr<ID3D11Texture2D> d3d;
		ComPtr<IDXGIKeyedMutex> km;
		HANDLE handle = nullptr;
		gs_texture_t *tex = nullptr;
	};

	const char *log_prefix;
	slot_t slot[count];
	uint32_t width = 0, height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	int current = -1; /* held by Vulkan, drawn */
	int ready = -1;   /* newest frame */
	int next = 0;
};

} // namespace d3d11_vulkan_bridge
