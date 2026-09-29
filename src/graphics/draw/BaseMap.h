#pragma once

#include "configuration.h"

#if defined(BASEUI_HAS_MAP)

#include <OLEDDisplay.h>
#include <stdint.h>

namespace graphics
{

/// Vector basemap: roads and water read from /basemap.bin on LittleFS.
///
/// Built by tools/prepare_basemap.py. Polylines rather than raster tiles because this
/// variant has no SD card, LittleFS is 1.09 MB, and the map frame zooms continuously
/// instead of in tile steps - see that script's header for the full reasoning.
///
/// Memory discipline: nothing is allocated on the heap. One fixed working buffer in .bss
/// holds whatever the current view needs, and it is refilled only when the view actually
/// changes, so a redraw never touches flash.
namespace BaseMap
{

/// Metres east/north of the *view* origin, mapped to screen pixels by the caller.
struct Transform {
    float offsetE, offsetN; // basemap origin relative to the view origin, metres
    float mPerPx;
    float panEastM, panNorthM;
    int16_t cx, cy;                    // band centre, pixels
    int16_t left, right, top, bottom;  // clip rect, pixels
};

/// Open the file and read its header. Safe to call repeatedly; only the first call does
/// work. Returns false when there is no usable basemap, which is not an error - the map
/// frame just draws nodes on an empty field.
bool begin();

/// True once begin() has found a valid file.
bool available();

/// Our own position, so the file's origin can be turned into a metre offset. Also picks
/// overview vs. detail for this zoom and refills the working buffer if the visible set
/// changed. Cheap to call every frame; it returns immediately when nothing moved.
void prepare(double viewOriginLat, double viewOriginLon, float mPerPx, int16_t halfSpanPx);

/// Draw the loaded lines. Clipped to the transform's rect so nothing spills into the
/// header or the navigation bar. Also collects street-label candidates for drawLabels().
void draw(OLEDDisplay *display, const Transform &t);

/// Clear the per-frame label candidates and reserved boxes. Call before draw().
void beginFrame();

/// Mark a screen rectangle as taken, so no street label is drawn over it. The map frame
/// reserves its own position marker, every node name, the scale bar and the detail line;
/// a label that would land on any of them is dropped rather than moved, because a name
/// shifted away from its road is worse than no name.
void reserve(int16_t x, int16_t y, int16_t w, int16_t h);

/// Draw street names, after the markers have reserved their space. Horizontal only -
/// OLEDDisplay cannot rotate text - knocked out of the background, ranked by road class
/// and visible length, and switched off entirely above ~8 m/px.
void drawLabels(OLEDDisplay *display, const Transform &t);

/// Metre offset from the view origin to the basemap origin, filled by prepare().
void getOriginOffset(float &offsetE, float &offsetN);

/// The centre of the mapped area. Lets the frame show the map with no GPS fix at all -
/// on this board the GPS shares UART0 with USB, so a bench-tested node usually has no
/// position and would otherwise get a blank screen.
bool getOrigin(double &lat, double &lon);

/// True when the last prepare() selected the cell-indexed detail rather than the overview.
bool usingDetail();

} // namespace BaseMap

} // namespace graphics

#endif
