/* DXGI desktop duplication on renderers without libobs's gs_duplicator (Vulkan): a self-contained implementation on a
 * private D3D11 device of the adapter that drives the monitor (docs/graphics-interop.md, "Windows capture").
 * Every call runs on the graphics thread with the graphics context entered. */
#pragma once

#include <windows.h>
#include <graphics/graphics.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dxgi_duplicator;

/* Whether the monitor is an output of the renderer's adapter, so that frames reach the renderer without a CPU copy. */
bool dxgi_duplicator_monitor_on_renderer(HMONITOR monitor);

/* Duplicates the monitor, shared by every capture of it. NULL if the monitor is on no adapter or cannot be duplicated
 * now (another desktop is shown, too many duplications): try again later. */
struct dxgi_duplicator *dxgi_duplicator_create(HMONITOR monitor);
void dxgi_duplicator_destroy(struct dxgi_duplicator *d);

/* Takes the newest frame, at most once per video frame. False: duplication lost (mode change, secure desktop,
 * fullscreen switch, adapter gone); destroy it and create it again later. */
bool dxgi_duplicator_update_frame(struct dxgi_duplicator *d);

/* The newest frame's size; false before the first frame. */
bool dxgi_duplicator_get_size(const struct dxgi_duplicator *d, uint32_t *width, uint32_t *height);

/* The texture to draw (render only); NULL before the first frame. */
gs_texture_t *dxgi_duplicator_get_texture(struct dxgi_duplicator *d);

enum gs_color_space dxgi_duplicator_get_color_space(const struct dxgi_duplicator *d);
float dxgi_duplicator_get_sdr_white_level(const struct dxgi_duplicator *d);

/* Position, size and rotation of the monitor on the desktop, as the duplication was created. */
void dxgi_duplicator_get_monitor_info(const struct dxgi_duplicator *d, struct gs_monitor_info *info);

#ifdef __cplusplus
}
#endif
