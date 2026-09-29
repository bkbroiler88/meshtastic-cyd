#include "graphics/draw/MapRenderer.h"

#if defined(BASEUI_HAS_MAP)

#include "GPSStatus.h"
#include "NodeDB.h"
#include "gps/GeoCoord.h"
#include "gps/RTC.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/BaseMap.h"
#include "graphics/draw/UIRenderer.h"
#include "input/InputBroker.h"
#include "main.h"
#include "mesh/Throttle.h"
#include <cmath>

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

// Centre offset in metres from the origin, east/north positive. Held at zero for now:
// panning needs a gesture, and the only one this panel reliably emits is TAP, which has
// to stay with the frame carousel. Kept because BaseMap::Transform takes it.
float panEastM = 0.0f;
float panNorthM = 0.0f;

constexpr float kMinRangeM = 100.0f;     // ~a city block across
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
constexpr uint32_t kHintVisibleMs = 6000;
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
    const int16_t by = v.bottom - 8;
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
    display->drawCircle(p.x, p.y, 6);
    display->fillCircle(p.x, p.y, 2);
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
    v.bottom = display->getHeight() - 16;
    v.cx = display->getWidth() / 2;
    v.cy = (v.top + v.bottom) / 2;

    // Load the basemap before deciding what to centre on: with no fix it supplies the
    // origin itself, so the frame is still useful.
    const bool haveMap = BaseMap::begin();

#if defined(SDCARD_EXCLUSIVE_WITH_BLUETOOTH)
    // The card is not mounted while Bluetooth is on, so there are no maps. Say why, and
    // say how to change it - a blank frame just looks broken.
    if (!haveMap && config.bluetooth.enabled) {
        graphics::drawCommonHeader(display, x, y, "MAP");
        display->setFont(FONT_SMALL);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        const int16_t line = FONT_HEIGHT_SMALL + 2;
        display->drawString(v.cx, v.cy - line - line / 2, "Maps unavailable");
        display->drawString(v.cx, v.cy - line / 2, "while Bluetooth is on");
        display->drawString(v.cx, v.cy + line / 2, "Menu > Bluetooth > Disabled");
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

    // Always centred on the origin. Free panning would need a gesture, and the only one
    // this panel reliably produces is TAP, which has to stay with the carousel.
    panEastM = panNorthM = 0.0f;

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

    // Roads and water underneath, if a basemap for this area is on the filesystem. Drawn
    // first so node markers and labels sit on top of it.
    BaseMap::Transform mapT;
    if (haveMap) {
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

    // Nodes. Filled = heard recently, hollow = stale. Short name only; detail is a tap away.
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const uint32_t nowSec = getTime();
    uint8_t plotted = 0;

    // Nearest node gets the detail line. Picked automatically rather than by tapping,
    // because there is no gesture free to select with.
    meshtastic_NodeInfoLite *nearest = nullptr;
    float nearestM = 0.0f;

    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
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
        if (stale)
            display->drawCircle(p.x, p.y, 3);
        else
            display->fillCircle(p.x, p.y, 3);

        if (n->short_name[0]) {
            const int16_t lx = (int16_t)(p.x + 6), ly = (int16_t)(p.y - FONT_HEIGHT_SMALL / 2);
            display->drawString(lx, ly, n->short_name);
            if (haveMap) {
                const int16_t lw = (int16_t)display->getStringWidth(n->short_name, strlen(n->short_name), false);
                BaseMap::reserve(lx, ly, (int16_t)(lw + 2), FONT_HEIGHT_SMALL);
                BaseMap::reserve((int16_t)(p.x - 4), (int16_t)(p.y - 4), 9, 9); // the marker
            }
        }
        plotted++;
    }

    if (ownPosKnown)
        drawOwnPosition(display, v);
    drawScaleBar(display, v);

    // Two stacked rows at the foot of the band: the nearest-node line, and above it the
    // swipe hint while it is still showing. They used to share one row, left- and
    // right-aligned, and overlapped as soon as either string got long.
    const int16_t yDetail = (int16_t)(v.bottom - FONT_HEIGHT_SMALL - 12);
    const int16_t yHint = (int16_t)(yDetail - FONT_HEIGHT_SMALL - 2);
    const bool showHint = Throttle::isWithinTimespanMs(frameEnteredMs, kHintVisibleMs);

    if (haveMap) {
        // Reserve everything already on screen before the street names go down. The
        // position ring especially: a knockout box over it erases the one thing the frame
        // exists to show.
        BaseMap::reserve((int16_t)(v.cx - 8), (int16_t)(v.cy - 8), 17, 17);
        BaseMap::reserve(0, (int16_t)(yDetail - 2), display->getWidth(),
                         (int16_t)(v.bottom - yDetail + 2)); // scale bar + detail line band
        if (showHint)
            BaseMap::reserve(0, yHint, display->getWidth(), FONT_HEIGHT_SMALL);
    }

    // Detail line for the nearest node, along the bottom of the band.
    if (nearest) {
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
        display->drawString(6, yDetail, line);
    } else if (plotted == 0) {
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(v.cx, v.cy + 14, "No nodes with position");
        display->setTextAlignment(TEXT_ALIGN_LEFT);
    }

    // Street names last: they need to know what every marker already claimed.
    if (haveMap)
        BaseMap::drawLabels(display, mapT);

    // The frame has no visible controls otherwise, and a screen that looks inert reads as
    // a hang - which is exactly how the tap-consuming version presented. Shown on entry
    // only; the scale bar is the permanent indication of zoom.
    if (showHint) {
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(display->getWidth() - 4, yHint,
                            rangePinned ? "swipe up/dn zoom" : "swipe up/dn  auto");
        display->setTextAlignment(TEXT_ALIGN_LEFT);
    }
}

bool MapRenderer::handleInput(const InputEvent *event)
{
    if (!event)
        return false;

    // Same vocabulary the on-screen keyboard uses (see
    // OnScreenKeyboardModule::processVirtualKeyboardInput): swipes step, a tap advances,
    // a long press commits. Deliberately no coordinate hit-testing - nothing else in
    // BaseUI does it, and touchX/touchY would have to track the runtime portrait/landscape
    // flip and the panel's calibration to be usable.
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
