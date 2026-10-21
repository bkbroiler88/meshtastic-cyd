#include "graphics/draw/MapRenderer.h"

#if defined(BASEUI_HAS_MAP)

#include "GPSStatus.h"
#include "NodeDB.h"
#include "gps/GeoCoord.h"
#include "gps/RTC.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/TFTDisplay.h"
#include "graphics/draw/BaseMap.h"
#include "graphics/draw/UIRenderer.h"
#include "input/InputBroker.h"
#include "main.h"
#include "mesh/Throttle.h"
#ifdef LCDWIKI_EXTRAS
#include "modules/TraceRouteModule.h"
#endif
#include <cmath>
#include <cstdint>

namespace graphics
{
namespace
{

// ---------------------------------------------------------------------------
// View state
// ---------------------------------------------------------------------------

// Metres from the centre of the map band to its nearest edge. Auto-fit picks this from
// the furthest node; zooming pins it until a long press clears the pin.
float viewRangeM = 1000.0f;
bool rangePinned = false;

// The map keeps the upper band; everything the user can touch lives in a panel below it.
// That split is the point: the band is drawn straight to the panel in colour and the panel
// is ordinary 1-bit BaseUI, and because they share no pixels neither can erase the other.
// They used to overlap, which is why labels and the scale key blinked in and out.
// 56 px leaves the map 240x228. Mocked at device scale against 86 and 40: 86 spent a third
// of the frame on chrome, and 40 bought only 16 px more map while making the scale key and
// the node toggle fight over one row.
constexpr int16_t kPanelH = 56;
constexpr int16_t kBtnW = 40;
constexpr int16_t kBtnH = 24;
constexpr int16_t kBtnGap = 5;
bool showNodes = true;
int16_t panelTop = 0; // set while drawing, read by the hit test

enum MapBtn { BtnLeft = 0, BtnUp, BtnCentre, BtnDown, BtnRight, BtnCount };

struct Rect {
    int16_t x, y, w, h;
    bool holds(int16_t px, int16_t py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// Two rows, not three: at a 19 px font three rows need 57 px and the panel is 56. Row one
// is the scale key with the node toggle beside it, row two is the D-pad.
Rect buttonRect(uint8_t i, int16_t screenW, int16_t pTop)
{
    const int16_t total = BtnCount * kBtnW + (BtnCount - 1) * kBtnGap;
    const int16_t x0 = (int16_t)((screenW - total) / 2);
    return {(int16_t)(x0 + i * (kBtnW + kBtnGap)), (int16_t)(pTop + 26), kBtnW, kBtnH};
}

Rect nodesToggleRect(int16_t screenW, int16_t pTop)
{
    return {(int16_t)(screenW - 98), (int16_t)(pTop + 1), 94, (int16_t)(FONT_HEIGHT_SMALL + 2)};
}

// Centre offset in metres from the origin, east/north positive. Held at zero for now:
// panning needs a gesture, and the only one this panel reliably emits is TAP, which has
// to stay with the frame carousel. Kept because BaseMap::Transform takes it.
float panEastM = 0.0f;
float panNorthM = 0.0f;

constexpr float kMinRangeM = 25.0f;      // ~a house and its neighbours across
constexpr float kMaxRangeM = 200000.0f;  // 200 km - beyond any plausible LoRa hop
constexpr float kZoomStep = 2.0f;        // one press halves or doubles the view
constexpr uint32_t kStaleAfterSec = 900; // 15 min without being heard = hollow marker

// Screen width as of the last draw, for the on-screen test in project(). This board
// switches between 240x320 portrait and 320x240 landscape at runtime, so it cannot be a
// constant.
int16_t scrW = 320;

// The swipe hint is an affordance, not a readout: it shows for a few seconds when the
// frame is opened and then gets out of the way, because it shares the bottom of the band
// with the nearest-node line and the two were drawing straight through each other.
uint32_t lastDrawMs = 0;
uint32_t frameEnteredMs = 0;

// ---------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------

// Equirectangular around our own position. Good to a fraction of a pixel at LoRa ranges,
// and far cheaper than anything conformal - this runs per node, per redraw.
struct ScreenPt {
    int16_t x, y;
    bool onScreen;
};

struct MapView {
    double originLat, originLon; // our position, or last known
    int16_t cx, cy;              // band centre in pixels
    float mPerPx;
    int16_t top, bottom;
};

/// Metres east/north of the view origin, for a lat/lon.
void offsetMetres(const MapView &v, double lat, double lon, float &east, float &north)
{
    north = (float)(GeoCoord::latLongToMeter(v.originLat, v.originLon, lat, v.originLon) * (lat < v.originLat ? -1 : 1));
    east = (float)(GeoCoord::latLongToMeter(v.originLat, v.originLon, v.originLat, lon) * (lon < v.originLon ? -1 : 1));
}

ScreenPt project(const MapView &v, double lat, double lon)
{
    float east, north;
    offsetMetres(v, lat, lon, east, north);
    const int16_t x = (int16_t)lroundf(v.cx + (east - panEastM) / v.mPerPx);
    const int16_t y = (int16_t)lroundf(v.cy - (north - panNorthM) / v.mPerPx);
    const bool on = (x >= 0 && x < scrW && y >= v.top && y < v.bottom);
    return {x, y, on};
}

/// True when this node can be plotted at all.
bool plottable(const meshtastic_NodeInfoLite *n)
{
    return n && n->num != nodeDB->getNodeNum() && nodeDB->hasValidPosition(n);
}

#ifdef LCDWIKI_EXTRAS
// ---------------------------------------------------------------------------
// Last traceroute, drawn as a path: us -> each hop -> target, each leg coloured by the SNR
// measured where it arrived. A hop with no known position is bridged with a dashed grey leg.
// ---------------------------------------------------------------------------

constexpr uint32_t kRouteShowMs = 15UL * 60UL * 1000UL;

const TraceRouteModule::RouteResult *visibleRoute()
{
    if (!traceRouteModule)
        return nullptr;
    const TraceRouteModule::RouteResult &r = traceRouteModule->lastRoute();
    if (!r.atMs || millis() - r.atMs > kRouteShowMs)
        return nullptr;
    return &r;
}

uint16_t snrColour(int8_t q4)
{
    if (q4 == INT8_MIN)
        return 0x8410; // grey: not measured
    const float db = q4 / 4.0f;
    if (db >= 0.0f)
        return 0x3E8F; // green
    if (db >= -10.0f)
        return 0xFD20; // amber
    return 0xF800;     // red
}

/// Screen position of a route node; false if it has none. Off-screen points are fine (clipped).
bool routePoint(const MapView &v, NodeNum num, int16_t &x, int16_t &y)
{
    double lat, lon;
    if (num == nodeDB->getNodeNum()) {
        lat = v.originLat;
        lon = v.originLon;
    } else {
        const meshtastic_NodeInfoLite *n = (num == NODENUM_BROADCAST) ? nullptr : nodeDB->getMeshNode(num);
        meshtastic_PositionLite np;
        if (!n || !nodeDB->hasValidPosition(n) || !nodeDB->copyNodePosition(num, np))
            return false;
        lat = DegD(np.latitude_i);
        lon = DegD(np.longitude_i);
    }
    const ScreenPt p = project(v, lat, lon);
    x = p.x;
    y = p.y;
    return true;
}

/// Liang-Barsky clip to the map band, so a leg never draws outside the rows it owns.
bool clipToBand(const MapView &v, float &x0, float &y0, float &x1, float &y1)
{
    const float xmin = 0, xmax = scrW - 1, ymin = v.top, ymax = v.bottom - 1;
    float t0 = 0, t1 = 1;
    const float dx = x1 - x0, dy = y1 - y0;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0)
                return false;
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0) {
            if (t > t1)
                return false;
            if (t > t0)
                t0 = t;
        } else {
            if (t < t0)
                return false;
            if (t < t1)
                t1 = t;
        }
    }
    const float nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy;
    x1 = x0 + t1 * dx;
    y1 = y0 + t1 * dy;
    x0 = nx0;
    y0 = ny0;
    return true;
}

void routeLeg(OLEDDisplay *display, const MapView &v, bool bandDirect, int16_t ax, int16_t ay, int16_t bx, int16_t by,
              uint16_t colour, bool dashed)
{
    float x0 = ax, y0 = ay, x1 = bx, y1 = by;
    if (!clipToBand(v, x0, y0, x1, y1))
        return;
    const float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const int pieces = dashed ? (int)(len / 10.0f) + 1 : 1;
    for (int k = 0; k < pieces; k++) {
        // Dashed: 6 px on, 4 px off
        const float s0 = dashed ? (k * 10.0f) / len : 0.0f;
        float s1 = dashed ? (k * 10.0f + 6.0f) / len : 1.0f;
        if (s0 >= 1.0f)
            break;
        if (s1 > 1.0f)
            s1 = 1.0f;
        const int16_t sx = (int16_t)lroundf(x0 + (x1 - x0) * s0), sy = (int16_t)lroundf(y0 + (y1 - y0) * s0);
        const int16_t ex = (int16_t)lroundf(x0 + (x1 - x0) * s1), ey = (int16_t)lroundf(y0 + (y1 - y0) * s1);
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
        if (bandDirect) {
            graphics::tftDirectLine(sx, sy, ex, ey, colour, 2);
            continue;
        }
#endif
        display->drawLine(sx, sy, ex, ey);
    }
}

void drawRoute(OLEDDisplay *display, const MapView &v, bool bandDirect)
{
    const TraceRouteModule::RouteResult *r = visibleRoute();
    if (!r)
        return;
    // Path: us, the hops in order, the target
    NodeNum path[10];
    uint8_t n = 0;
    path[n++] = nodeDB->getNodeNum();
    for (uint8_t i = 0; i < r->hopCount; i++)
        path[n++] = r->via[i];
    path[n++] = r->target;

    int16_t px = 0, py = 0;
    bool havePrev = routePoint(v, path[0], px, py);
    bool gap = false;
    for (uint8_t k = 1; k < n; k++) {
        int16_t x, y;
        if (!routePoint(v, path[k], x, y)) {
            gap = true; // no position: bridge to the next node that has one
            continue;
        }
        if (havePrev) {
            const int8_t q4 = (k - 1 < r->snrCount) ? r->snrQ4[k - 1] : INT8_MIN;
            routeLeg(display, v, bandDirect, px, py, x, y, gap ? 0x8410 : snrColour(q4), gap);
        }
        px = x;
        py = y;
        havePrev = true;
        gap = false;
    }
}
#endif

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void drawScaleBar(OLEDDisplay *display, const MapView &v)
{
    // Pick a round distance that lands between 40 and 90 px.
    static const float steps[] = {100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};
    float chosen = steps[0];
    for (float s : steps) {
        const float px = s / v.mPerPx;
        if (px >= 40 && px <= 90) {
            chosen = s;
            break;
        }
        if (px < 40)
            chosen = s;
    }
    const int16_t px = (int16_t)lroundf(chosen / v.mPerPx);
    const int16_t bx = 6;
    // In the panel now, clear of the map band. drawString puts the label a font-height
    // above the bar, so the bar sits low enough for the text to land on row one.
    const int16_t by = (int16_t)(panelTop + FONT_HEIGHT_SMALL);
    display->drawLine(bx, by, bx + px, by);
    display->drawLine(bx, by - 3, bx, by + 1);
    display->drawLine(bx + px, by - 3, bx + px, by + 1);

    char label[16];
    if (chosen >= 1000)
        snprintf(label, sizeof(label), "%dkm", (int)(chosen / 1000));
    else
        snprintf(label, sizeof(label), "%dm", (int)chosen);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->drawString(bx + px + 4, by - FONT_HEIGHT_SMALL + 2, label);
}

void drawOwnPosition(OLEDDisplay *display, const MapView &v)
{
    const ScreenPt p = project(v, v.originLat, v.originLon);
    if (!p.onScreen)
        return;
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
    if (BaseMap::bandIsDirect()) {
        graphics::tftDirectCircle(p.x, p.y, 6, 0xFFFF, false);
        graphics::tftDirectCircle(p.x, p.y, 2, 0xFFFF, true);
        return;
    }
#endif
    display->drawCircle(p.x, p.y, 6);
    display->fillCircle(p.x, p.y, 2);
}

/// The touch panel below the map: scale key, a D-pad, and the node layer toggle. Ordinary
/// 1-bit BaseUI drawing - it sits outside the reserved band on purpose.
void drawControlPanel(OLEDDisplay *display, const MapView &v, bool haveMap)
{
    const int16_t w = display->getWidth();
    display->setFont(FONT_SMALL);

    display->drawLine(0, panelTop, w, panelTop);

    // Scale key, with the current range spelled out beside it
    drawScaleBar(display, v);

    static const char *glyph[BtnCount] = {"<", "^", "o", "v", ">"};
    for (uint8_t i = 0; i < BtnCount; i++) {
        const Rect r = buttonRect(i, w, panelTop);
        display->drawRect(r.x, r.y, r.w, r.h);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString((int16_t)(r.x + r.w / 2), (int16_t)(r.y + (r.h - FONT_HEIGHT_SMALL) / 2), glyph[i]);
    }

    (void)haveMap;
    const Rect t = nodesToggleRect(w, panelTop);
    char line[32];
    snprintf(line, sizeof(line), "%s Nodes", showNodes ? "[x]" : "[ ]");
    display->setTextAlignment(TEXT_ALIGN_RIGHT);
    display->drawString((int16_t)(w - 4), (int16_t)(t.y + 1), line);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
}

} // namespace

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void MapRenderer::resetView()
{
    rangePinned = false;
    panEastM = panNorthM = 0.0f;
}

void MapRenderer::drawMapFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();
    scrW = display->getWidth();

    // Other frames draw in between, so a gap means this frame was just opened.
    const uint32_t nowMs = millis();
    if (!Throttle::isWithinTimespanMs(lastDrawMs, 500))
        frameEnteredMs = nowMs;
    lastDrawMs = nowMs;

    MapView v;
    v.top = FONT_HEIGHT_SMALL + 1;
    panelTop = (int16_t)(display->getHeight() - 16 - kPanelH);
    v.bottom = panelTop;
    v.cx = display->getWidth() / 2;
    v.cy = (v.top + v.bottom) / 2;

    // Load the basemap before deciding what to centre on: with no fix it supplies the
    // origin itself, so the frame is still useful.
    const bool haveMap = BaseMap::begin();

#if defined(SDCARD_EXCLUSIVE_WITH_BLE_WIFI)
    // No map, no card, and a radio is up - so the radio is why. Name the one actually
    // responsible: "turn off Bluetooth" is useless advice when it is Wi-Fi holding the
    // card off. If the card did mount, this does not fire and any failure is about the
    // file, not the radios.
    if (!haveMap && !BaseMap::usingSdCard() && (config.bluetooth.enabled || config.network.wifi_enabled)) {
        const bool wifi = config.network.wifi_enabled;
        graphics::drawCommonHeader(display, x, y, "MAP");
        display->setFont(FONT_SMALL);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        const int16_t line = FONT_HEIGHT_SMALL + 2;
        display->drawString(v.cx, v.cy - line - line / 2, "Maps unavailable");
        display->drawString(v.cx, v.cy - line / 2, wifi ? "while Wi-Fi is on" : "while Bluetooth is on");
        display->drawString(v.cx, v.cy + line / 2, wifi ? "Turn Wi-Fi off to use maps" : "Menu > Bluetooth > Disabled");
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        return;
    }
#endif

    // Origin: our own fix, else our stored node position, else the centre of the mapped
    // area. Only the first two mean we know where *we* are.
    bool haveOrigin = false;
    bool ownPosKnown = false;
    if (gpsStatus && gpsStatus->getHasLock()) {
        v.originLat = DegD(gpsStatus->getLatitude());
        v.originLon = DegD(gpsStatus->getLongitude());
        haveOrigin = ownPosKnown = true;
    } else if (localPosition.latitude_i != 0 || localPosition.longitude_i != 0) {
        // Our own fix is kept in localPosition, not in the node's own entry.
        v.originLat = DegD(localPosition.latitude_i);
        v.originLon = DegD(localPosition.longitude_i);
        haveOrigin = ownPosKnown = true;
    } else if (haveMap && BaseMap::getOrigin(v.originLat, v.originLon)) {
        // No fix - on this board the GPS shares UART0 with USB, so that is the normal
        // state on the bench. Show the mapped area rather than a blank screen.
        haveOrigin = true;
    }

    char title[24];
    if (haveOrigin && !ownPosKnown)
        snprintf(title, sizeof(title), "MAP  NO FIX");
    else
        snprintf(title, sizeof(title), "MAP");
    graphics::drawCommonHeader(display, x, y, title);

    if (!haveOrigin) {
        display->setFont(FONT_SMALL);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(v.cx, v.cy - FONT_HEIGHT_SMALL, "No position yet");
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        return;
    }

    // Panning is the D-pad's job now. It used to be pinned to the origin every frame
    // because the only gesture this panel reliably produces is TAP, and TAP has to stay
    // with the carousel - the buttons in the panel below give pan its own input without
    // taking that away.

    // Auto-fit: scale so the furthest node with a position sits inside the band, unless
    // the user has pinned the range by zooming.
    const int16_t halfSpanPx = (v.bottom - v.top) / 2;
    if (!rangePinned) {
        // With no fix and no nodes there is nothing to fit, and a 100 m view of a 24 km
        // map is useless - open at a legible scale instead.
        float furthest = ownPosKnown ? kMinRangeM : 4000.0f;
        for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
            meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
            if (!plottable(n))
                continue;
            meshtastic_PositionLite np;
            if (!nodeDB->copyNodePosition(n->num, np))
                continue;
            const float d = GeoCoord::latLongToMeter(DegD(np.latitude_i), DegD(np.longitude_i), v.originLat, v.originLon);
            if (d > furthest)
                furthest = d;
        }
        viewRangeM = furthest * 1.15f; // a margin so the furthest marker is not on the edge
        if (viewRangeM < kMinRangeM)
            viewRangeM = kMinRangeM;
        if (viewRangeM > kMaxRangeM)
            viewRangeM = kMaxRangeM;
    }
    v.mPerPx = viewRangeM / (float)halfSpanPx;

    // Does the band need redrawing at all?
    //
    // When the band is drawn direct, every line, label and marker goes onto the SPI bus, so
    // repeating an unchanged picture every tick is pure churn - it reads as a constant
    // redraw, worst where there are the most labels. The monochrome path has no such cost
    // (it writes a framebuffer the blit then diffs), so it always draws.
    //
    // Everything that can change what the band shows has to be in here, which is why it
    // covers the node set and not just the view: a marker appearing is as much a change as
    // a pan. Positions are folded in coarsely - whole metres - so that float noise in a
    // position report cannot by itself force a repaint.
    uint32_t bandSig = (uint32_t)(v.cx * 31 + v.cy * 131 + v.top * 13 + v.bottom * 17);
    bandSig = bandSig * 2654435761u + (uint32_t)lroundf(v.mPerPx * 64.0f);
    bandSig = bandSig * 2246822519u + (uint32_t)((int32_t)panEastM * 7 + (int32_t)panNorthM * 11);
    bandSig = bandSig * 3266489917u + (showNodes ? 1u : 0u) + (ownPosKnown ? 2u : 0u);
    bandSig = bandSig * 668265263u + (uint32_t)(v.originLat * 1e4) + (uint32_t)(v.originLon * 1e4);
    if (showNodes) {
        for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
            meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
            if (!plottable(n))
                continue;
            meshtastic_PositionLite np;
            if (!nodeDB->copyNodePosition(n->num, np))
                continue;
            bandSig = bandSig * 374761393u + n->num + (uint32_t)(np.latitude_i / 100) + (uint32_t)(np.longitude_i / 100) +
                      (uint32_t)(n->last_heard / 30); // 30 s buckets: "stale" flips slowly
        }
    }
#ifdef LCDWIKI_EXTRAS
    // A traceroute arriving, or expiring off the map, changes the picture
    if (const TraceRouteModule::RouteResult *route = visibleRoute())
        bandSig = bandSig * 2654435761u + route->atMs;
#endif
    bandSig ^= graphics::tftFullRepaintCounter() * 2654435761u; // repainted over? draw again

    static uint32_t lastBandSig = 0;
    const bool bandDirty = (bandSig != lastBandSig);
    lastBandSig = bandSig;

    // Roads and water underneath, if a basemap for this area is on the filesystem. Drawn
    // first so node markers and labels sit on top of it.
    bool bandDirect = false;
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
    bandDirect = graphics::tftDirectAvailable() && haveMap;
    if (bandDirect) {
        // Claim the band. This is refreshed EVERY frame, never skipped by the redraw gate -
        // the lease is what keeps the blit off these rows, and letting it lapse while the
        // frame is still on screen would hand the band back mid-view and black it out.
        graphics::tftReserveDirectRows(v.top, v.bottom);
    }
#endif
    // The 1-bit path has to redraw every frame because the framebuffer was just cleared.
    const bool drawBand = !bandDirect || bandDirty;

    BaseMap::Transform mapT;
    if (haveMap && drawBand) {
        BaseMap::beginFrame();
        BaseMap::prepare(v.originLat, v.originLon, v.mPerPx, halfSpanPx);
        BaseMap::Transform t;
        BaseMap::getOriginOffset(t.offsetE, t.offsetN);
        t.mPerPx = v.mPerPx;
        t.panEastM = panEastM;
        t.panNorthM = panNorthM;
        t.cx = v.cx;
        t.cy = v.cy;
        t.left = 0;
        t.right = (int16_t)(display->getWidth() - 1);
        t.top = v.top;
        t.bottom = (int16_t)(v.bottom - 1);
        BaseMap::draw(display, t);
        mapT = t;
    }
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
    if (!haveMap)
        graphics::tftReleaseDirectRows(); // nothing to own; the messages below are 1-bit
#endif

    // Nodes. Filled = heard recently, hollow = stale. Short name only; detail is a tap away.
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const uint32_t nowSec = getTime();
    uint8_t plotted = 0;

    // Nearest node gets the detail line. Picked automatically rather than by tapping,
    // because there is no gesture free to select with.
    meshtastic_NodeInfoLite *nearest = nullptr;
    float nearestM = 0.0f;

#ifdef LCDWIKI_EXTRAS
    // Under the markers, so node dots and names stay readable on top of the path
    if (drawBand)
        drawRoute(display, v, bandDirect);
#endif

    for (size_t i = 0; drawBand && showNodes && i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!plottable(n))
            continue;

        meshtastic_PositionLite np;
        if (!nodeDB->copyNodePosition(n->num, np))
            continue;

        const float d = GeoCoord::latLongToMeter(DegD(np.latitude_i), DegD(np.longitude_i), v.originLat, v.originLon);
        if (!nearest || d < nearestM) {
            nearest = n;
            nearestM = d;
        }

        const ScreenPt p = project(v, DegD(np.latitude_i), DegD(np.longitude_i));
        if (!p.onScreen)
            continue;

        const bool stale = (nowSec && n->last_heard && (nowSec - n->last_heard) > kStaleAfterSec);
        // Markers live inside the band, so they follow whoever owns it - put them in the
        // 1-bit buffer while the band is drawn direct and they simply never appear.
        if (bandDirect)
            graphics::tftDirectCircle(p.x, p.y, 3, 0xFFFF, !stale);
        else if (stale)
            display->drawCircle(p.x, p.y, 3);
        else
            display->fillCircle(p.x, p.y, 3);

        if (n->short_name[0]) {
            const int16_t lx = (int16_t)(p.x + 6), ly = (int16_t)(p.y - FONT_HEIGHT_SMALL / 2);
            if (bandDirect) {
                graphics::tftDirectText(lx, ly, n->short_name, 0xFFFF, 0x1082);
            } else {
                display->drawString(lx, ly, n->short_name);
            }
            if (haveMap) {
                const int16_t lw = (int16_t)display->getStringWidth(n->short_name, strlen(n->short_name), false);
                BaseMap::reserve(lx, ly, (int16_t)(lw + 2), FONT_HEIGHT_SMALL);
                BaseMap::reserve((int16_t)(p.x - 4), (int16_t)(p.y - 4), 9, 9); // the marker
            }
        }
        plotted++;
    }

    if (ownPosKnown && drawBand)
        drawOwnPosition(display, v);

    // The nearest-node line stays at the foot of the band - it is map content, not a
    // control. The swipe hint is gone: it existed because the frame had no visible controls
    // and an inert-looking screen reads as a hang, which the D-pad now answers directly.
    const int16_t yDetail = (int16_t)(v.bottom - FONT_HEIGHT_SMALL - 2);

    if (haveMap && drawBand) {
        // Reserve everything already on screen before the street names go down. The
        // position ring especially: a knockout box over it erases the one thing the frame
        // exists to show.
        BaseMap::reserve((int16_t)(v.cx - 8), (int16_t)(v.cy - 8), 17, 17);
        BaseMap::reserve(0, (int16_t)(yDetail - 2), display->getWidth(),
                         (int16_t)(v.bottom - yDetail + 2)); // the detail line
    }

    // Detail line for the nearest node, along the bottom of the band.
    if (nearest && drawBand) {
        char line[48];
        const char *nm = nearest->short_name[0] ? nearest->short_name : "?";
        // snr is the live in-memory reading and is zeroed on disk; snr_q4 is what
        // survives a reboot. Prefer the live value, fall back to the persisted one.
        float snrDb = nearest->snr;
        if (snrDb == 0.0f && nearest->snr_q4 != 0)
            snrDb = nearest->snr_q4 / 4.0f;
        if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL)
            snprintf(line, sizeof(line), "%s  %.1f mi  %.1fdB", nm, nearestM * METERS_TO_FEET / 5280.0f, snrDb);
        else if (nearestM >= 1000)
            snprintf(line, sizeof(line), "%s  %.1f km  %.1fdB", nm, nearestM / 1000.0f, snrDb);
        else
            snprintf(line, sizeof(line), "%s  %d m  %.1fdB", nm, (int)nearestM, snrDb);
        if (bandDirect)
            graphics::tftDirectText(6, yDetail, line, 0xFFFF, 0x1082);
        else
            display->drawString(6, yDetail, line);
    } else if (plotted == 0 && showNodes && drawBand) {
        // Only when nodes are meant to be shown. With the box unchecked the empty map is
        // the answer - the unticked box already says why there are no markers.
        const char *msg = "No nodes with position";
        if (bandDirect) {
            const int16_t tw = graphics::tftDirectTextWidth(msg);
            graphics::tftDirectText((int16_t)(v.cx - tw / 2), (int16_t)(v.cy + 14), msg, 0xFFFF, 0x1082);
        } else {
            display->setTextAlignment(TEXT_ALIGN_CENTER);
            display->drawString(v.cx, v.cy + 14, msg);
            display->setTextAlignment(TEXT_ALIGN_LEFT);
        }
    }

    // Street names last: they need to know what every marker already claimed.
    if (haveMap && drawBand)
        BaseMap::drawLabels(display, mapT);

#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
    // Re-take the lease now the drawing is done. Taking it only at the top was not enough:
    // zoomed out the band pushes thousands of segments straight at the panel and the draw
    // alone outruns the lease, so the blit that follows saw it expired, handed the rows
    // back, forced a full repaint, and blacked the band - which the next frame redrew, and
    // round again. Refreshing here means the lease only has to outlive the gap between
    // frames, not the work inside one.
    if (bandDirect)
        graphics::tftReserveDirectRows(v.top, v.bottom);
#endif

    // Controls below the band, in ordinary 1-bit BaseUI. Drawn after the map so a stale
    // reservation can never leave the panel unpainted.
    drawControlPanel(display, v, haveMap);
}

bool MapRenderer::handleInput(const InputEvent *event)
{
    if (!event)
        return false;

    // A tap is hit-tested against the control panel ONLY - never against the map band.
    // An earlier version hit-tested the whole frame and consumed every tap, which trapped
    // the user here: TAP is what Screen.cpp uses to advance the carousel, and on this panel
    // it is the gesture that demonstrably works. Taps on the map still fall through, so
    // there is always a way out, and a tap that hits no control is not consumed either.
    if (event->inputEvent == INPUT_BROKER_USER_PRESS && panelTop > 0 && (int16_t)event->touchY >= panelTop) {
        const int16_t tx = (int16_t)event->touchX, ty = (int16_t)event->touchY;
        const int16_t w = scrW ? scrW : 240;

        if (nodesToggleRect(w, panelTop).holds(tx, ty)) {
            showNodes = !showNodes;
            return true;
        }
        // D-pad row: anywhere in the row, the NEAREST button wins. Exact rectangles were too
        // strict for this panel - captured taps land ~10 px left of where the finger aims
        // (centre aimed 160, reported 150; right aimed 250, reported ~241). Mid-row that just
        // shifts a tap within a 40 px button, but on the leftmost button it put taps at x=48
        // against a rect starting at 50: the tap missed, fell through, and advanced the
        // carousel instead of panning. Nearest-wins absorbs the bias, covers the 5 px gaps,
        // and runs the end buttons out to the screen edges. Vertical slack is kept small so
        // a tap clearly above or below the row still falls through to the carousel.
        const Rect row = buttonRect(0, w, panelTop);
        if (ty >= row.y - 6 && ty < row.y + row.h + 6) {
            uint8_t i = 0;
            int32_t bestD = INT32_MAX;
            for (uint8_t b = 0; b < BtnCount; b++) {
                const Rect r = buttonRect(b, w, panelTop);
                const int32_t d = abs((int32_t)tx - (int32_t)(r.x + r.w / 2));
                if (d < bestD) {
                    bestD = d;
                    i = b;
                }
            }
            // Pan a third of the half-span per press: far enough to make progress, short
            // enough that you can still see where you came from.
            const float step = viewRangeM * 0.33f;
            switch (i) {
            case BtnLeft:
                panEastM -= step;
                break;
            case BtnRight:
                panEastM += step;
                break;
            case BtnUp:
                panNorthM += step;
                break;
            case BtnDown:
                panNorthM -= step;
                break;
            case BtnCentre:
                MapRenderer::resetView();
                break;
            }
            return true;
        }
        return false; // a tap in the panel that missed everything still advances the frame
    }

    // Otherwise the same vocabulary the on-screen keyboard uses (see
    // OnScreenKeyboardModule::processVirtualKeyboardInput): swipes step, a tap advances,
    // a long press commits.
    switch (event->inputEvent) {
    case INPUT_BROKER_UP:
        rangePinned = true;
        viewRangeM = fmaxf(viewRangeM / kZoomStep, kMinRangeM);
        return true;
    case INPUT_BROKER_DOWN:
        rangePinned = true;
        viewRangeM = fminf(viewRangeM * kZoomStep, kMaxRangeM);
        return true;
    case INPUT_BROKER_CANCEL:
        // Reset, but do not consume: cancel means the same thing everywhere else.
        MapRenderer::resetView();
        return false;
    default:
        // Everything else falls through, and TAP (INPUT_BROKER_USER_PRESS) especially.
        //
        // An earlier version consumed TAP to step between nodes, which trapped the user on
        // this frame: TAP is what Screen.cpp:2287 uses to advance the carousel, and on this
        // panel it is the gesture that demonstrably works - not one capture ever showed a
        // "action SWIPE" line. Taking it left no way off the map. Only UP/DOWN are consumed
        // now, and those are unused by the carousel (UP_LONG/DOWN_LONG do frame switching
        // and are separate events), so there is always a way out.
        return false;
    }
}

} // namespace graphics

#endif
