#include "graphics/draw/BaseMap.h"

#if defined(BASEUI_HAS_MAP)

#include "FSCommon.h"
#include "NodeDB.h" // for `config`, to check whether Bluetooth is holding the SD card off
#include "configuration.h"
#include "graphics/ScreenFonts.h" // FONT_SMALL / FONT_HEIGHT_SMALL for the street labels
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
#include "graphics/TFTDisplay.h"
#endif
#include <cmath>
#include <cstring>
#if defined(ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI)
#include "SPILock.h"
#include <SD.h>
#define BASEMAP_CAN_USE_SD 1
#endif

namespace graphics
{
namespace
{

constexpr const char *kPath = "/basemap.bin";
// v2: one global name blob, i16 points from the file origin. v3: each section carries its
// own name blob and points are i32, which lifts v2's 65 km box and 64 KiB name ceilings.
// Both are read, so a card written by an older packer keeps working.
constexpr uint8_t kFormatV2 = 2;
constexpr uint8_t kFormatV3 = 3;
uint8_t formatVer = kFormatV2;
constexpr uint8_t kMaxGrid = 16;
constexpr uint32_t kTableOff = 32; // header is 32 bytes in v2, offset table follows

// --- street labels ---------------------------------------------------------------------
// Horizontal only: OLEDDisplay has no rotated text, so a name sits near its road rather
// than along it. Every label knocks a hole in the geometry behind it, so they are rationed:
// gated by zoom, ranked by road class and how much of the road is actually on screen, and
// dropped outright when they would overlap anything already placed.
constexpr uint8_t kLabelsMax = 8;
// A name's zoom gate follows its road class: an interstate is worth naming from much
// further out than a cul-de-sac. One flat cutoff at 8 m/px meant a 1 km view - 8.8 m/px on
// a 228 px band - showed no names at all, which read as "the motorways have no names".
constexpr float kLabelZoomMPerPx = 30.0f; // no names of any class above this
constexpr float kMinorZoomMPerPx = 8.0f;  // secondary/tertiary names below this
constexpr float kLocalZoomMPerPx = 3.0f;  // residential names only below this
constexpr float kMinRunFactor = 0.8f;     // road must show this much of the label's width
constexpr int32_t kDedupePx = 110;        // do not repeat one name within this distance
constexpr int16_t kLabelPad = 2;
constexpr uint16_t kNameNone = 0xFFFF;
constexpr uint8_t kNameMax = 48; // matches NAME_MAX_BYTES in prepare_basemap.py, plus NUL

struct LabelCand {
    // v2 stores an offset into the one global blob; v3 an absolute file offset, because
    // each section has its own blob and the candidate outlives the section read. v4: an
    // arena offset when src is kSrcArena, else a file offset in pack `src`.
    uint32_t nameId;
    int16_t x, y;   // anchor: midpoint of the longest visible segment, so it is on the road
    uint16_t run;   // total visible length in px, which is what earns the label
    uint8_t layer;
    uint8_t src;
};
constexpr uint8_t kSrcArena = 0xFF;
// LabelCand::layer for a place or water name, rather than a road: kPlaceFlag | kind.
// Kinds: 0 city, 1 town, 2 village, 3 suburb/hamlet/neighbourhood, 4 water.
constexpr uint8_t kPlaceFlag = 0x10;
constexpr uint8_t kPlaceWater = 4;

/// Lower is more important. Places beat roads; a lake's name sits between major and minor roads.
inline uint8_t labelRank(uint8_t layer)
{
    if (layer & kPlaceFlag) {
        const uint8_t k = layer & 0x0F;
        return k == kPlaceWater ? 5 : k;
    }
    return layer == 1 ? 4 : layer == 2 ? 6 : 7;
}

/// Widest view, in m/px, at which each place kind is still named.
inline float placeZoomLimit(uint8_t kind)
{
    static const float lim[] = {2000.0f, 300.0f, 60.0f, 15.0f, 60.0f};
    return kind < 5 ? lim[kind] : 15.0f;
}
constexpr uint8_t kMaxCands = 32;
LabelCand cands[kMaxCands];
uint8_t nCands = 0;

/// Add a label candidate; when the list is full, a better one replaces the worst.
void addCand(const LabelCand &c)
{
    if (nCands < kMaxCands) {
        cands[nCands++] = c;
        return;
    }
    uint8_t worst = 0;
    for (uint8_t i = 1; i < nCands; i++) {
        const uint8_t ri = labelRank(cands[i].layer), rw = labelRank(cands[worst].layer);
        if (ri > rw || (ri == rw && cands[i].run < cands[worst].run))
            worst = i;
    }
    const uint8_t rc = labelRank(c.layer), rw = labelRank(cands[worst].layer);
    if (rc < rw || (rc == rw && c.run > cands[worst].run))
        cands[worst] = c;
}

struct Rect16 {
    int16_t x, y, w, h;
};
constexpr uint8_t kMaxReserved = 20;
Rect16 reserved[kMaxReserved];
uint8_t nReserved = 0;

// Geometry is streamed straight off the filesystem through this one window; nothing is
// cached and nothing is allocated.
//
// Two earlier shapes both failed on this board. A 12 KB .bss buffer dropped boot heap from
// 96 KB to 81 KB and BLE died allocating its characteristics. Moving it to a lazy malloc
// fixed boot but then the malloc itself failed - by the time the map frame is opened, BLE
// is up and there is no contiguous 8 KB block left, even though ~96 KB is free in total.
// A 1 KB static window has neither problem and costs about 3 ms of reads per redraw.
constexpr size_t kWindow = 1024;
constexpr uint16_t kPtsPerRead = kWindow / 4;
uint8_t win[kWindow];

// The most detail cells worth drawing at once. Beyond this the view is wide enough that
// the overview is the better picture anyway, and this bounds the per-frame read.
constexpr uint16_t kMaxDetailCells = 12;

bool inited = false;
bool ok = false;
bool useSd = false;
uint8_t grid = 0;
int32_t lat0_i = 0, lon0_i = 0;
uint32_t halfM = 0, cellM = 0;
uint32_t nameOff = 0, nameLen = 0;
double mPerDegLon = 0.0;
constexpr double kMPerDegLat = 111132.0;

float curOffsetE = 0.0f, curOffsetN = 0.0f;
bool curDetail = false;
int16_t cellX0 = 0, cellX1 = -1, cellY0 = 0, cellY1 = -1;

// Packed records are not aligned - a way header is 3 bytes - and Xtensa cannot do
// unaligned 16/32-bit loads. memcpy of a constant size compiles to safe byte loads.
inline uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
inline int16_t rdi16(const uint8_t *p)
{
    int16_t v;
    memcpy(&v, p, 2);
    return v;
}
inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// --- clipped line drawing -------------------------------------------------------------
// OLEDDisplay::drawLine bounds-checks against the whole framebuffer but knows nothing
// about the map band, so lines would run through the header and the navigation bar.

constexpr uint8_t kOutLeft = 1, kOutRight = 2, kOutTop = 4, kOutBottom = 8;

uint8_t outcode(int32_t x, int32_t y, const BaseMap::Transform &t)
{
    uint8_t c = 0;
    if (x < t.left)
        c |= kOutLeft;
    else if (x > t.right)
        c |= kOutRight;
    if (y < t.top)
        c |= kOutTop;
    else if (y > t.bottom)
        c |= kOutBottom;
    return c;
}

/// Cohen-Sutherland. Returns false when the segment is entirely outside the band.
bool clipSeg(int32_t &x0, int32_t &y0, int32_t &x1, int32_t &y1, const BaseMap::Transform &t)
{
    uint8_t c0 = outcode(x0, y0, t), c1 = outcode(x1, y1, t);
    for (int guard = 0; guard < 8; guard++) {
        if (!(c0 | c1))
            return true;
        if (c0 & c1)
            return false;
        const uint8_t c = c0 ? c0 : c1;
        int32_t x = 0, y = 0;
        const int32_t dx = x1 - x0, dy = y1 - y0;
        if (c & kOutTop) {
            x = x0 + (dx * (t.top - y0)) / (dy ? dy : 1);
            y = t.top;
        } else if (c & kOutBottom) {
            x = x0 + (dx * (t.bottom - y0)) / (dy ? dy : 1);
            y = t.bottom;
        } else if (c & kOutRight) {
            y = y0 + (dy * (t.right - x0)) / (dx ? dx : 1);
            x = t.right;
        } else {
            y = y0 + (dy * (t.left - x0)) / (dx ? dx : 1);
            x = t.left;
        }
        if (c == c0) {
            x0 = x;
            y0 = y;
            c0 = outcode(x0, y0, t);
        } else {
            x1 = x;
            y1 = y;
            c1 = outcode(x1, y1, t);
        }
    }
    return false;
}

/// Bresenham with an optional dash, so water reads differently from roads on a 1-bit panel.
/// Returns the drawn (clipped) length in pixels and, in midX/midY, that piece's midpoint -
/// the label engine needs both, and only the clipped part of a road counts toward a name.
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
// RGB565. The panel is 16-bit even though BaseUI composes in one bit, so the map band is
// drawn straight to it - see graphics::tftDirectAvailable() for why that is safe and what
// it costs (nothing: a per-pixel colour buffer for this area would be 15 KB at four
// colours, which this board does not have).
// Hierarchy comes from lightness and width; hue only hints at what a thing is. Saturated
// colour at full brightness on black reads as highlighter, not cartography, so everything
// here is deliberately dull - the brightest road is a warm off-white, not an amber.
constexpr uint16_t kColWater = 0x2A4B; // #27485C desaturated slate blue, sits back
constexpr uint16_t kColLocal = 0x4AAC; // #4E555F barely above the background
constexpr uint16_t kColMinor = 0x7C11; // #7A828E cool mid grey
constexpr uint16_t kColMajor = 0xC5F4; // #C9BFA8 warm pale sand, the brightest thing drawn
constexpr uint16_t kColLabel = 0xFFFF; // street names, knocked out of the geometry
constexpr uint16_t kColMapBg = 0x1082; // #101014 near-black, slightly cool
constexpr uint16_t kColPath = 0x6A67;  // #6B4F3A trail brown, below local roads
constexpr uint16_t kColRail = 0x8C53;  // #8A8A9A cool light grey
// Area fills: barely lifted off the background, so lines still carry the map
constexpr uint16_t kColFillWater = 0x1988; // #1B3344
constexpr uint16_t kColFillPark = 0x1984;  // #1E3324
constexpr uint16_t kColFillWood = 0x1943;  // #1A2B1E
constexpr uint16_t kColPlace = 0xE6F8;     // #E8DCC0 town names
constexpr uint16_t kColWaterName = 0x8E1C; // #8CC0E0 lake and river names

inline uint16_t layerColor(uint8_t layer)
{
    switch (layer) {
    case 0:
        return kColWater;
    case 1:
        return kColMajor;
    case 2:
        return kColMinor;
    case 4:
        return kColPath;
    case 5:
        return kColRail;
    default:
        return kColLocal;
    }
}

/// Width carries the hierarchy as much as lightness does. 3 px was too fat for a 240 px
/// panel - it blobbed at junctions and made the trunk roads look drawn on with a marker.
inline uint8_t layerWidth(uint8_t layer)
{
    return layer == 1 ? 2 : 1;
}

/// Set once per frame so the inner loop does not re-test the display type per segment.
bool directColor = false;
uint32_t statSegs = 0, statRows = 0; // per redraw, for the timing line in the log
#endif

int32_t drawSeg(OLEDDisplay *display, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t layer,
                const BaseMap::Transform &t, int32_t *midX = nullptr, int32_t *midY = nullptr)
{
    if (!clipSeg(x0, y0, x1, y1, t))
        return 0;

    if (midX && midY) {
        *midX = (x0 + x1) / 2;
        *midY = (y0 + y1) / 2;
    }
    const int32_t lenDx = x1 - x0, lenDy = y1 - y0;
    const int32_t drawn = (int32_t)lroundf(sqrtf((float)(lenDx * lenDx + lenDy * lenDy)));

#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
    if (directColor) {
        statSegs++;
        graphics::tftDirectLine((int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, layerColor(layer), layerWidth(layer));
        return drawn;
    }
#endif
    // Monochrome fallback: water is dashed, because at one bit it is the only way to tell
    // a creek from a lane.
    const bool dashed = (layer == 0);

    const int32_t adx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    const int32_t ady = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int32_t dx = adx, sx = x0 < x1 ? 1 : -1;
    int32_t dy = -ady, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    uint8_t phase = 0;
    for (;;) {
        if (!dashed || (phase & 3) < 2)
            display->setPixel((int16_t)x0, (int16_t)y0);
        phase++;
        if (x0 == x1 && y0 == y1)
            break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    return drawn;
}

struct Entry {
    uint32_t off, len;
};

// --- file access ----------------------------------------------------------------------
// On SD the card shares VSPI with the SX1262, so every transaction has to hold spiLock.
// The lock is taken per read rather than across a whole frame: a redraw moves up to ~20 KB
// at 4 MHz, and holding the bus for tens of milliseconds would stall the radio. Chunked
// this way the longest hold is one 1 KB window, and interleaving is safe because file
// position lives in the filesystem layer, not on the card.

File openPath(const char *path)
{
#if defined(BASEMAP_CAN_USE_SD)
    if (useSd) {
        concurrency::LockGuard g(spiLock);
        return SD.open(path, FILE_O_READ);
    }
#endif
    return FSCom.open(path, FILE_O_READ);
}

File openMap()
{
    return openPath(kPath);
}

size_t mapRead(File &f, void *dst, size_t n)
{
#if defined(BASEMAP_CAN_USE_SD)
    if (useSd) {
        concurrency::LockGuard g(spiLock);
        return f.readBytes(reinterpret_cast<char *>(dst), n);
    }
#endif
    return f.readBytes(reinterpret_cast<char *>(dst), n);
}

bool mapSeek(File &f, uint32_t pos)
{
#if defined(BASEMAP_CAN_USE_SD)
    if (useSd) {
        concurrency::LockGuard g(spiLock);
        return f.seek(pos);
    }
#endif
    return f.seek(pos);
}

void mapClose(File &f)
{
#if defined(BASEMAP_CAN_USE_SD)
    if (useSd) {
        concurrency::LockGuard g(spiLock);
        f.close();
        return;
    }
#endif
    f.close();
}

bool readEntry(File &f, uint16_t idx, Entry &e)
{
    if (!mapSeek(f, kTableOff + (uint32_t)idx * 8))
        return false;
    uint8_t b[8];
    if (mapRead(f, b, 8) != 8)
        return false;
    e.off = rd32(b);
    e.len = rd32(b + 4);
    return true;
}

/// Stream one section, drawing as it goes. Never holds more than kWindow bytes.
void drawSection(OLEDDisplay *display, File &f, const Entry &e, const BaseMap::Transform &t, bool drawLocal, float inv)
{
    if (e.len < 4 || !mapSeek(f, e.off))
        return;
    uint8_t hdr[4];
    if (mapRead(f, hdr, 4) != 4)
        return;

    // v3 puts this section's name blob first; skip past it to reach the way count, and
    // remember where it started so a label id can be turned into a file offset later.
    uint32_t sectionNameBase = 0;
    if (formatVer >= kFormatV3) {
        const uint32_t blobLen = rd32(hdr);
        sectionNameBase = e.off + 4;
        if (!mapSeek(f, sectionNameBase + blobLen) || mapRead(f, hdr, 4) != 4)
            return;
    }
    const uint32_t nWays = rd32(hdr);
    const uint8_t ptBytes = (formatVer >= kFormatV3) ? 8 : 4;

    for (uint32_t w = 0; w < nWays; w++) {
        uint8_t wh[5];
        if (mapRead(f, wh, 5) != 5)
            return;
        const uint8_t layer = wh[0];
        const uint16_t npts = rd16(wh + 1);
        const uint16_t nameId = rd16(wh + 3);
        if (npts == 0)
            continue;

        // Residential streets are noise once a pixel is more than a few metres across,
        // and they are the bulk of the detail cells. Skip the payload outright.
        if (layer == 3 && !drawLocal) {
            if (!mapSeek(f, (uint32_t)f.position() + (uint32_t)npts * 4))
                return;
            continue;
        }

        const bool isWater = (layer == 0);
        // A name is earned by the road's whole visible run, but the label is anchored to the
        // midpoint of its longest visible segment so it always sits on the road rather than
        // floating off a bend.
        const bool wantName = (nameId != kNameNone && !isWater && nCands < kMaxCands);
        int32_t totalRun = 0, bestSeg = 0, bestX = 0, bestY = 0;

        int32_t px = 0, py = 0;
        bool first = true;
        uint16_t remaining = npts;
        while (remaining) {
            const uint16_t perRead = (uint16_t)(kWindow / ptBytes);
            const uint16_t chunk = remaining < perRead ? remaining : perRead;
            const size_t want = (size_t)chunk * ptBytes;
            if (mapRead(f, win, want) != want)
                return;
            for (uint16_t i = 0; i < chunk; i++) {
                const int32_t rawE = (ptBytes == 8) ? (int32_t)rd32(win + i * 8) : (int32_t)rdi16(win + i * 4);
                const int32_t rawN = (ptBytes == 8) ? (int32_t)rd32(win + i * 8 + 4) : (int32_t)rdi16(win + i * 4 + 2);
                const float ee = (float)rawE + t.offsetE - t.panEastM;
                const float nn = (float)rawN + t.offsetN - t.panNorthM;
                const int32_t sx = t.cx + (int32_t)lroundf(ee * inv);
                const int32_t sy = t.cy - (int32_t)lroundf(nn * inv);
                if (!first) {
                    int32_t mx = 0, my = 0;
                    const int32_t drawn = drawSeg(display, px, py, sx, sy, layer, t, &mx, &my);
                    if (wantName && drawn > 0) {
                        totalRun += drawn;
                        if (drawn > bestSeg) {
                            bestSeg = drawn;
                            bestX = mx;
                            bestY = my;
                        }
                    }
                }
                px = sx;
                py = sy;
                first = false;
            }
            remaining -= chunk;
        }

        if (wantName && bestSeg > 0) {
            if (totalRun > 0xFFFF)
                totalRun = 0xFFFF;
            const uint32_t nameRef = (formatVer >= kFormatV3) ? (sectionNameBase + nameId) : (uint32_t)nameId;
            addCand({nameRef, (int16_t)bestX, (int16_t)bestY, (uint16_t)totalRun, layer, 0});
        }
    }
}


// =======================================================================================
// v4 map packs: /maps/*.map
//
// Each pack covers one area and is cut into tiles on a fixed global lat/lon grid at three
// detail levels, so packs for neighbouring areas meet seamlessly and adding an area is
// dropping another file in /maps. The level is picked by zoom, so a wide view draws a
// pre-simplified map instead of every street. Points are i16, tile-relative.
//
//   header (48 B): "MAP4", u8 version, u8 nLevels, u8 shift[3] (tile = 2^-shift degrees),
//                  pad, i32 minLat/minLon/maxLat/maxLon (1e-7 deg), u32 tileCount, u32 dirOff
//   directory:     tileCount x {u32 key, u32 off, u32 len}, sorted by key
//                  key = level << 28 | iy << 14 | ix,  ix = floor((lon+180)*2^shift)
//   tile:          u16 nameLen, names (u8 len + bytes)
//                  u16 areaCount, areas: u8 kind, u16 dataLen, data = u8 nRings,
//                        rings: u16 npts, npts x (i16 x, i16 y)   (even-odd fill)
//                  u16 wayCount,  ways: u8 layer, u16 npts, u16 nameId, npts x (i16 x, i16 y)
//                  u16 poiCount,  pois: u8 kind, u16 nameId, i16 x, i16 y
//                  u16 placeCount, places: u8 kind, u16 nameId, i16 x, i16 y
//                  x/y: 0..16384 across the tile, west->east and south->north
//   line layers 0 water, 1 major, 2 minor, 3 local, 4 path/trail, 5 rail
//   area kinds 0 water, 1 park, 2 wood; POI kinds 1..9 (see kPoi); place kinds as kPlaceFlag
//
// Tiles in view are kept in one RAM arena while Bluetooth and Wi-Fi are off (that is the
// only time the SD card is mounted anyway, and there is ~98 KB free then). It is allocated
// once, the first time the map needs it, and kept for the session: allocating and freeing
// a block this size on every visit would fragment the heap. It is released only if free
// heap runs low, and without it tiles stream off the card through the 1 KB window.
// =======================================================================================
namespace v4
{
constexpr uint8_t kVersion = 5;
constexpr uint8_t kLevels = 3;
constexpr int32_t kTileUnits = 16384;
constexpr uint8_t kMaxPacks = 6;
constexpr uint8_t kPathMax = 40;
constexpr uint8_t kMaxNeed = 24;   // tiles drawn per frame; a wider view keeps the first 24
constexpr uint8_t kMaxSlots = 24;  // tiles held in the arena
constexpr uint8_t kRowsMax = 12;   // tile rows searched per pack per view
constexpr uint32_t kReleaseBelow = 20 * 1024; // give the arena back below this much free heap
constexpr uint32_t kRetryMs = 60000;

struct Pack {
    char path[kPathMax];
    int32_t minLat, minLon, maxLat, maxLon;
    uint32_t count, dirOff;
    uint8_t shift[kLevels];
};
Pack packs[kMaxPacks];
uint8_t nPacks = 0;

struct Need {
    uint32_t key, off, len;
    uint8_t pack;
    int8_t slot; // arena slot, or -1 to stream from the card
};
Need need[kMaxNeed];
uint8_t nNeed = 0;
uint32_t needSig = 0;

struct Slot {
    uint32_t key;
    uint32_t at, len;
    uint32_t lastUse;
    uint8_t pack;
};
uint8_t *arena = nullptr;
uint32_t arenaSize = 0;
Slot slots[kMaxSlots];
uint8_t nSlots = 0;
uint32_t useTick = 0;
uint32_t lastAllocTryMs = 0;
bool allocTried = false;

uint8_t level = 0;
double viewLat = 0, viewLon = 0;
double mPerDegLonView = 0;

inline uint32_t makeKey(uint8_t lvl, uint32_t iy, uint32_t ix)
{
    return ((uint32_t)lvl << 28) | (iy << 14) | ix;
}

bool readHeader(const char *path, Pack &pk)
{
    File f = openPath(path);
    if (!f)
        return false;
    uint8_t h[48];
    const bool got = mapRead(f, h, sizeof(h)) == sizeof(h);
    mapClose(f);
    if (!got || memcmp(h, "MAP4", 4) != 0 || h[4] != kVersion || h[5] != kLevels)
        return false;
    for (uint8_t i = 0; i < kLevels; i++)
        pk.shift[i] = h[6 + i];
    pk.minLat = (int32_t)rd32(h + 12);
    pk.minLon = (int32_t)rd32(h + 16);
    pk.maxLat = (int32_t)rd32(h + 20);
    pk.maxLon = (int32_t)rd32(h + 24);
    pk.count = rd32(h + 28);
    pk.dirOff = rd32(h + 32);
    strncpy(pk.path, path, kPathMax - 1);
    pk.path[kPathMax - 1] = '\0';
    return pk.count > 0;
}

/// Find every *.map in /maps. Only the headers are read; directories stay on the card.
void scan()
{
    nPacks = 0;
    File dir = openPath("/maps");
    if (!dir)
        return;
    if (!dir.isDirectory()) {
        mapClose(dir);
        return;
    }
    while (nPacks < kMaxPacks) {
        File e;
        {
#if defined(BASEMAP_CAN_USE_SD)
            if (useSd) {
                concurrency::LockGuard g(spiLock);
                e = dir.openNextFile();
            } else
#endif
                e = dir.openNextFile();
        }
        if (!e)
            break;
        char path[kPathMax];
        const char *nm = e.name();
        const char *base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        const bool isDir = e.isDirectory();
        mapClose(e);
        const size_t n = strlen(base);
        if (isDir || n < 5 || strcasecmp(base + n - 4, ".map") != 0 || n + 7 >= kPathMax)
            continue;
        snprintf(path, sizeof(path), "/maps/%s", base);
        Pack &pk = packs[nPacks];
        if (readHeader(path, pk)) {
            LOG_INFO("BaseMap: pack %s, %u tiles, %.4f,%.4f .. %.4f,%.4f", path, pk.count, pk.minLat * 1e-7,
                     pk.minLon * 1e-7, pk.maxLat * 1e-7, pk.maxLon * 1e-7);
            nPacks++;
        } else {
            LOG_WARN("BaseMap: %s is not a v4 map pack, skipped", path);
        }
    }
    mapClose(dir);
}

/// First directory index whose key is >= want (binary search over the card).
uint32_t lowerBound(File &f, const Pack &pk, uint32_t want)
{
    uint32_t lo = 0, hi = pk.count;
    uint8_t b[4];
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (!mapSeek(f, pk.dirOff + mid * 12) || mapRead(f, b, 4) != 4)
            return pk.count;
        if (rd32(b) < want)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// --- arena -----------------------------------------------------------------------------

bool cacheAllowed()
{
    return !config.bluetooth.enabled && !config.network.wifi_enabled;
}

void releaseArena(const char *why)
{
    if (!arena)
        return;
    free(arena);
    arena = nullptr;
    arenaSize = 0;
    nSlots = 0;
    for (uint8_t i = 0; i < nNeed; i++)
        need[i].slot = -1;
    LOG_INFO("BaseMap: tile cache released (%s)", why);
}

void ensureArena()
{
#if defined(ARCH_ESP32)
    if (arena) {
        if (ESP.getFreeHeap() < kReleaseBelow)
            releaseArena("low heap");
        return;
    }
    if (!cacheAllowed())
        return;
    if (allocTried && millis() - lastAllocTryMs < kRetryMs)
        return;
    allocTried = true;
    lastAllocTryMs = millis();
    static const uint32_t sizes[] = {40 * 1024, 32 * 1024, 24 * 1024, 16 * 1024};
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint32_t freeNow = ESP.getFreeHeap();
    for (uint32_t sz : sizes) {
        // Leave a real margin: the block must not be the last big one, and the rest of
        // the firmware keeps at least 40 KB.
        if (largest < sz + 8 * 1024 || freeNow < sz + 40 * 1024)
            continue;
        arena = (uint8_t *)malloc(sz);
        if (arena) {
            arenaSize = sz;
            nSlots = 0;
            LOG_INFO("BaseMap: tile cache %u KB (free %u, largest %u)", sz / 1024, freeNow, largest);
            return;
        }
    }
    LOG_INFO("BaseMap: no room for a tile cache (free %u, largest %u), streaming from the card", freeNow, largest);
#endif
}

int8_t findSlot(uint8_t pack, uint32_t key)
{
    for (uint8_t i = 0; i < nSlots; i++)
        if (slots[i].key == key && slots[i].pack == pack)
            return (int8_t)i;
    return -1;
}

void removeSlot(uint8_t i)
{
    for (uint8_t j = i; j + 1 < nSlots; j++)
        slots[j] = slots[j + 1];
    nSlots--;
}

/// Close the gaps left by evicted tiles so the free space is one block at the end.
uint32_t compact()
{
    // Selection by address, then slide each tile down. Slots are few (<= 24).
    for (uint8_t i = 0; i < nSlots; i++)
        for (uint8_t j = i + 1; j < nSlots; j++)
            if (slots[j].at < slots[i].at) {
                const Slot t = slots[i];
                slots[i] = slots[j];
                slots[j] = t;
            }
    uint32_t end = 0;
    for (uint8_t i = 0; i < nSlots; i++) {
        if (slots[i].at != end)
            memmove(arena + end, arena + slots[i].at, slots[i].len);
        slots[i].at = end;
        end += slots[i].len;
    }
    return end;
}

/// Make the arena hold as many of this frame's tiles as fit, keeping anything already
/// there that is still in view and evicting the least recently used of the rest.
void fillArena()
{
    ensureArena();
    for (uint8_t i = 0; i < nNeed; i++)
        need[i].slot = -1;
    if (!arena)
        return;

    useTick++;
    uint32_t missing = 0;
    for (uint8_t i = 0; i < nNeed; i++) {
        const int8_t s = findSlot(need[i].pack, need[i].key);
        if (s >= 0)
            slots[s].lastUse = useTick;
        else if (need[i].len <= arenaSize)
            missing += need[i].len;
    }
    uint32_t used = 0;
    for (uint8_t i = 0; i < nSlots; i++)
        used += slots[i].len;
    // Evict tiles not in view, oldest first, until the missing ones fit.
    while (used + missing > arenaSize) {
        int8_t victim = -1;
        for (uint8_t i = 0; i < nSlots; i++)
            if (slots[i].lastUse != useTick && (victim < 0 || slots[i].lastUse < slots[victim].lastUse))
                victim = (int8_t)i;
        if (victim < 0)
            break;
        used -= slots[victim].len;
        removeSlot((uint8_t)victim);
    }
    uint32_t end = compact();

    File f;
    uint8_t openPack = 0xFF;
    for (uint8_t i = 0; i < nNeed; i++) {
        Need &n = need[i];
        int8_t s = findSlot(n.pack, n.key);
        if (s < 0 && nSlots < kMaxSlots && end + n.len <= arenaSize) {
            if (openPack != n.pack) {
                if (f)
                    mapClose(f);
                f = openPath(packs[n.pack].path);
                openPack = n.pack;
            }
            if (!f || !mapSeek(f, n.off))
                continue;
            // Read in modest chunks so the SPI lock (shared with the radio) is never held
            // for a whole tile.
            uint32_t got = 0;
            while (got < n.len) {
                const uint32_t chunk = (n.len - got) < 2048 ? (n.len - got) : 2048;
                if (mapRead(f, arena + end + got, chunk) != chunk)
                    break;
                got += chunk;
            }
            if (got != n.len)
                continue;
            slots[nSlots] = {n.key, end, n.len, useTick, n.pack};
            s = (int8_t)nSlots++;
            end += n.len;
        }
        n.slot = s;
    }
    if (f)
        mapClose(f);
}

// --- tile parsing ----------------------------------------------------------------------

/// A tile read straight out of the arena.
struct MemSrc {
    const uint8_t *p;
    uint32_t left;
    uint32_t pos; // arena offset of p, for name references
    bool read(void *dst, uint32_t n)
    {
        if (n > left)
            return false;
        memcpy(dst, p, n);
        p += n;
        pos += n;
        left -= n;
        return true;
    }
    bool skip(uint32_t n)
    {
        if (n > left)
            return false;
        p += n;
        pos += n;
        left -= n;
        return true;
    }
    /// Points: hand back a pointer into the arena, no copy.
    const uint8_t *points(uint32_t n)
    {
        if (n > left)
            return nullptr;
        const uint8_t *r = p;
        p += n;
        pos += n;
        left -= n;
        return r;
    }
};

/// A tile streamed off the card through the 1 KB window.
struct FileSrc {
    File &f;
    uint32_t pos; // file offset, for name references
    bool read(void *dst, uint32_t n)
    {
        if (mapRead(f, dst, n) != n)
            return false;
        pos += n;
        return true;
    }
    bool skip(uint32_t n)
    {
        if (!mapSeek(f, pos + n))
            return false;
        pos += n;
        return true;
    }
    const uint8_t *points(uint32_t n)
    {
        if (n > kWindow || mapRead(f, win, n) != n)
            return nullptr;
        pos += n;
        return win;
    }
};

#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
/// Even-odd scanline fill of one area record. Points are transformed on the fly per row:
/// there is no RAM for a converted copy, and an area is at most a tile.
void fillArea(const uint8_t *data, uint32_t len, uint8_t kind, float ax, float ay, float kx, float ky,
              const BaseMap::Transform &t)
{
    struct Ring {
        const uint8_t *p;
        uint16_t n;
    } rings[8];
    uint8_t nr = 0;
    const uint8_t *q = data, *end = data + len;
    if (q >= end)
        return;
    const uint8_t nRings = *q++;
    for (uint8_t r = 0; r < nRings && q + 2 <= end; r++) {
        const uint16_t n = rd16(q);
        q += 2;
        if (q + (uint32_t)n * 4 > end)
            break;
        if (nr < 8 && n >= 3)
            rings[nr++] = {q, n};
        q += (uint32_t)n * 4;
    }
    if (!nr)
        return;

    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (uint8_t r = 0; r < nr; r++)
        for (uint16_t i = 0; i < rings[r].n; i++) {
            const float x = ax + (float)rdi16(rings[r].p + i * 4) * kx;
            const float y = ay - (float)rdi16(rings[r].p + i * 4 + 2) * ky;
            minX = x < minX ? x : minX;
            maxX = x > maxX ? x : maxX;
            minY = y < minY ? y : minY;
            maxY = y > maxY ? y : maxY;
        }
    if (maxX < t.left || minX > t.right || maxY < t.top || minY > t.bottom)
        return;
    if (maxX - minX < 2.0f && maxY - minY < 2.0f)
        return; // smaller than a pixel or two: not worth a fill
    const int32_t y0 = (int32_t)fmaxf(ceilf(minY - 0.5f), (float)t.top);
    const int32_t y1 = (int32_t)fminf(floorf(maxY - 0.5f), (float)t.bottom);
    const uint16_t col = kind == 0 ? kColFillWater : kind == 1 ? kColFillPark : kColFillWood;

    // Rows are filled in bands. For each band, one pass over every edge picks out the few
    // that cross it; each row then tests only those. Testing every edge on every row made a
    // detailed wood cost rows x edges and was most of a redraw.
    constexpr uint8_t kMaxX = 48;
    constexpr int32_t kBand = 16;
    constexpr uint8_t kBandEdges = 64;
    int16_t xs[kMaxX];
    struct BandEdge {
        uint8_t ring;
        uint16_t idx; // edge from point idx-1 (wrapping) to idx
    } bandEdges[kBandEdges];
    auto ptX = [&](const Ring &rg, uint16_t i) { return ax + (float)rdi16(rg.p + i * 4) * kx; };
    auto ptY = [&](const Ring &rg, uint16_t i) { return ay - (float)rdi16(rg.p + i * 4 + 2) * ky; };

    for (int32_t b0 = y0; b0 <= y1; b0 += kBand) {
        const int32_t b1 = (b0 + kBand - 1 < y1) ? b0 + kBand - 1 : y1;
        const float bTop = (float)b0 + 0.5f, bBot = (float)b1 + 0.5f;
        uint8_t nb = 0;
        bool overflow = false;
        for (uint8_t r = 0; r < nr && !overflow; r++) {
            const Ring &rg = rings[r];
            float py = ptY(rg, rg.n - 1);
            for (uint16_t i = 0; i < rg.n; i++) {
                const float cy = ptY(rg, i);
                const float lo = py < cy ? py : cy, hi = py < cy ? cy : py;
                if (hi > bTop && lo <= bBot) {
                    if (nb == kBandEdges) {
                        overflow = true;
                        break;
                    }
                    bandEdges[nb++] = {r, i};
                }
                py = cy;
            }
        }

        for (int32_t y = b0; y <= b1; y++) {
            const float yc = (float)y + 0.5f;
            uint8_t n = 0;
            if (!overflow) {
                for (uint8_t e = 0; e < nb; e++) {
                    const Ring &rg = rings[bandEdges[e].ring];
                    const uint16_t i = bandEdges[e].idx, pi = i ? i - 1 : rg.n - 1;
                    const float px = ptX(rg, pi), py = ptY(rg, pi), cx = ptX(rg, i), cy = ptY(rg, i);
                    if (((py <= yc) && (cy > yc)) || ((cy <= yc) && (py > yc))) {
                        if (n < kMaxX)
                            xs[n++] = (int16_t)lroundf(px + (yc - py) * (cx - px) / (cy - py));
                    }
                }
            } else {
                // More edges in this band than the scratch list holds: test them all.
                for (uint8_t r = 0; r < nr; r++) {
                    const Ring &rg = rings[r];
                    float px = ptX(rg, rg.n - 1), py = ptY(rg, rg.n - 1);
                    for (uint16_t i = 0; i < rg.n; i++) {
                        const float cx = ptX(rg, i), cy = ptY(rg, i);
                        if (((py <= yc) && (cy > yc)) || ((cy <= yc) && (py > yc))) {
                            if (n < kMaxX)
                                xs[n++] = (int16_t)lroundf(px + (yc - py) * (cx - px) / (cy - py));
                        }
                        px = cx;
                        py = cy;
                    }
                }
            }
            for (uint8_t i = 1; i < n; i++) { // insertion sort, n is small
                const int16_t v = xs[i];
                int8_t j = (int8_t)i - 1;
                while (j >= 0 && xs[j] > v) {
                    xs[j + 1] = xs[j];
                    j--;
                }
                xs[j + 1] = v;
            }
            for (uint8_t i = 0; i + 1 < n; i += 2) {
                const int16_t xa = xs[i] < t.left ? t.left : xs[i];
                const int16_t xb = xs[i + 1] > t.right ? t.right : xs[i + 1];
                if (xb > xa) {
                    graphics::tftDirectFillRect(xa, (int16_t)y, (int16_t)(xb - xa), 1, col);
                    statRows++;
                }
            }
        }
    }
}

/// POI kinds 1..9: a coloured badge with a letter.
void drawPoi(uint8_t kind, int16_t x, int16_t y, float mPerPx, const BaseMap::Transform &t)
{
    static const char *const letters[] = {"?", "H", "F", "P", "W", "S", "^", "T", "C", "V"};
    static const uint16_t colours[] = {0x7BEF, 0xC987, 0xCC45, 0x3B36, 0x2CB6, 0x3C6A, 0x8B6A, 0x7BD1, 0x53C7, 0x8AD6};
    if (kind == 0 || kind > 9)
        return;
    if (kind == 3 && mPerPx > 1.5f)
        return; // parking is everywhere: only when right in close
    constexpr int16_t r = 6;
    if (x - r < t.left || x + r > t.right || y - r < t.top || y + r > t.bottom)
        return;
    graphics::tftDirectFillRoundRect((int16_t)(x - r), (int16_t)(y - r), 2 * r + 1, 2 * r + 1, 3, colours[kind]);
    graphics::tftDirectTextF(x, y, letters[kind], 0xFFFF, colours[kind], TftFont::Small, true);
    BaseMap::reserve((int16_t)(x - r), (int16_t)(y - r), 2 * r + 1, 2 * r + 1);
}
#endif

template <class Src>
void drawTile(OLEDDisplay *display, Src &src, uint8_t srcId, uint32_t key, uint8_t shift, const BaseMap::Transform &t, bool drawLocal,
              float inv)
{
    const uint32_t ix = key & 0x3FFF, iy = (key >> 14) & 0x3FFF;
    const double span = 1.0 / (double)(1u << shift);
    const double lon0 = (double)ix * span - 180.0, lat0 = (double)iy * span - 90.0;
    // Tile units -> screen pixels, as one multiply-add per axis.
    const float baseE = (float)((lon0 - viewLon) * mPerDegLonView) - t.panEastM;
    const float baseN = (float)((lat0 - viewLat) * kMPerDegLat) - t.panNorthM;
    const float ax = t.cx + baseE * inv, ay = t.cy - baseN * inv;
    const float kx = (float)(span * mPerDegLonView / kTileUnits) * inv;
    const float ky = (float)(span * kMPerDegLat / kTileUnits) * inv;

    uint8_t b2[2];
    if (!src.read(b2, 2))
        return;
    const uint32_t nameBase = src.pos;
    if (!src.skip(rd16(b2)) || !src.read(b2, 2))
        return;

    // Areas first, so every line lands on top of them. Fills are colour-panel only.
    const uint16_t nAreas = rd16(b2);
    for (uint16_t a = 0; a < nAreas; a++) {
        uint8_t ah[3];
        if (!src.read(ah, 3))
            return;
        const uint16_t dataLen = rd16(ah + 1);
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
        if (directColor && dataLen <= kWindow) {
            const uint8_t *data = src.points(dataLen);
            if (!data)
                return;
            fillArea(data, dataLen, ah[0], ax, ay, kx, ky, t);
            continue;
        }
#endif
        if (!src.skip(dataLen))
            return;
    }

    if (!src.read(b2, 2))
        return;
    const uint16_t nWays = rd16(b2);

    for (uint16_t w = 0; w < nWays; w++) {
        uint8_t wh[5];
        if (!src.read(wh, 5))
            return;
        const uint8_t layer = wh[0];
        const uint16_t npts = rd16(wh + 1);
        const uint16_t nameId = rd16(wh + 3);
        if (npts == 0)
            continue;
        // Residential streets once a pixel is a few metres, trails sooner, rail only out to
        // a regional view: skip the payload outright.
        if ((layer == 3 && !drawLocal) || (layer == 4 && t.mPerPx > 3.0f) || (layer == 5 && t.mPerPx > 30.0f)) {
            if (!src.skip((uint32_t)npts * 4))
                return;
            continue;
        }
        const bool wantName = (nameId != kNameNone && layer >= 1 && layer <= 3);
        int32_t totalRun = 0, bestSeg = 0, bestX = 0, bestY = 0;
        int32_t px = 0, py = 0;
        bool first = true;
        uint16_t remaining = npts;
        while (remaining) {
            const uint16_t perRead = (uint16_t)(kWindow / 4);
            const uint16_t chunk = remaining < perRead ? remaining : perRead;
            const uint8_t *pts = src.points((uint32_t)chunk * 4);
            if (!pts)
                return;
            for (uint16_t i = 0; i < chunk; i++) {
                const int32_t sx = (int32_t)lroundf(ax + (float)rdi16(pts + i * 4) * kx);
                const int32_t sy = (int32_t)lroundf(ay - (float)rdi16(pts + i * 4 + 2) * ky);
                if (!first) {
                    int32_t mx = 0, my = 0;
                    const int32_t drawn = drawSeg(display, px, py, sx, sy, layer, t, &mx, &my);
                    if (wantName && drawn > bestSeg) {
                        bestSeg = drawn;
                        bestX = mx;
                        bestY = my;
                    }
                    if (wantName && drawn > 0)
                        totalRun += drawn;
                }
                px = sx;
                py = sy;
                first = false;
            }
            remaining -= chunk;
        }
        if (wantName && bestSeg > 0) {
            if (totalRun > 0xFFFF)
                totalRun = 0xFFFF;
            addCand({nameBase + nameId, (int16_t)bestX, (int16_t)bestY, (uint16_t)totalRun, layer, srcId});
        }
    }

    // POIs: drawn now, so names placed later avoid them. Close zoom only.
    if (!src.read(b2, 2))
        return;
    const uint16_t nPois = rd16(b2);
    for (uint16_t i = 0; i < nPois; i++) {
        uint8_t ph[7];
        if (!src.read(ph, 7))
            return;
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
        if (directColor && t.mPerPx <= 4.0f)
            drawPoi(ph[0], (int16_t)lroundf(ax + (float)rdi16(ph + 3) * kx), (int16_t)lroundf(ay - (float)rdi16(ph + 5) * ky),
                    t.mPerPx, t);
#endif
    }

    // Place and water names become label candidates, ranked above street names.
    if (!src.read(b2, 2))
        return;
    const uint16_t nPlaces = rd16(b2);
    for (uint16_t i = 0; i < nPlaces; i++) {
        uint8_t ph[7];
        if (!src.read(ph, 7))
            return;
        const uint8_t kind = ph[0];
        const uint16_t nameId = rd16(ph + 1);
        if (nameId == kNameNone || t.mPerPx > placeZoomLimit(kind))
            continue;
        const int32_t x = lroundf(ax + (float)rdi16(ph + 3) * kx), y = lroundf(ay - (float)rdi16(ph + 5) * ky);
        if (x < t.left || x > t.right || y < t.top || y > t.bottom)
            continue;
        addCand({nameBase + nameId, (int16_t)x, (int16_t)y, 0xFFFF, (uint8_t)(kPlaceFlag | (kind & 0x0F)), srcId});
    }
}

} // namespace v4

bool usePacks = false;

} // namespace

bool BaseMap::begin()
{
    if (inited)
        return ok;
    inited = true;

    // No radio policy here any more. main.cpp decides whether the card gets mounted; this
    // just uses whatever is actually present - SD first, then internal flash. Duplicating
    // the policy meant a second gate could refuse maps even after the card had mounted,
    // which is exactly what happened when the two conditions drifted apart.

    // Prefer the SD card: it holds gigabytes, so the mapped area is not capped by the
    // 1 MB filesystem, and swapping regions is a file copy on a PC. Fall back to internal
    // flash when there is no card or no map on it.
#if defined(BASEMAP_CAN_USE_SD)
    {
        bool present = false;
        {
            concurrency::LockGuard g(spiLock);
            present = (SD.cardType() != CARD_NONE);
        }
        if (present) {
            useSd = true;
            auto probe = openMap();
            if (probe) {
                mapClose(probe);
                LOG_INFO("BaseMap: using %s from SD card", kPath);
            } else {
                useSd = false;
            }
        }
    }
#endif

    // v4 packs in /maps take precedence over the single legacy file.
    {
#if defined(BASEMAP_CAN_USE_SD)
        const bool sdWas = useSd;
        bool sdPresent = false;
        {
            concurrency::LockGuard g(spiLock);
            sdPresent = (SD.cardType() != CARD_NONE);
        }
        useSd = sdPresent;
#endif
        v4::scan();
        if (v4::nPacks > 0) {
            usePacks = true;
            ok = true;
            LOG_INFO("BaseMap: %u map pack(s) in /maps", v4::nPacks);
            return true;
        }
#if defined(BASEMAP_CAN_USE_SD)
        useSd = sdWas;
#endif
    }

    auto f = openMap();
    if (!f) {
        LOG_INFO("BaseMap: no /maps packs or %s on SD or internal flash, map frame will show nodes only", kPath);
        return false;
    }

    uint8_t hdr[32];
    if (mapRead(f, hdr, sizeof(hdr)) != sizeof(hdr)) {
        LOG_WARN("BaseMap: %s too short", kPath);
        mapClose(f);
        return false;
    }
    if (memcmp(hdr, "MBM1", 4) != 0 || (hdr[4] != kFormatV2 && hdr[4] != kFormatV3)) {
        LOG_WARN("BaseMap: bad magic/version in %s", kPath);
        mapClose(f);
        return false;
    }
    formatVer = hdr[4];
    grid = hdr[5];
    if (grid == 0 || grid > kMaxGrid) {
        LOG_WARN("BaseMap: grid %u unsupported (max %u)", grid, kMaxGrid);
        mapClose(f);
        return false;
    }
    lat0_i = (int32_t)rd32(hdr + 8);
    lon0_i = (int32_t)rd32(hdr + 12);
    halfM = rd32(hdr + 16);
    cellM = rd32(hdr + 20);
    nameOff = rd32(hdr + 24);
    nameLen = rd32(hdr + 28);
    mapClose(f);

    if (cellM == 0 || halfM == 0) {
        LOG_WARN("BaseMap: degenerate header (half=%u cell=%u)", halfM, cellM);
        return false;
    }

    mPerDegLon = 111320.0 * cos(lat0_i * 1e-7 * M_PI / 180.0);
    ok = true;
    LOG_INFO("BaseMap: %s loaded, %ux%u grid, %u m box, origin %.5f,%.5f", kPath, grid, grid, halfM * 2,
             lat0_i * 1e-7, lon0_i * 1e-7);
    return true;
}

bool BaseMap::available()
{
    return ok;
}

bool BaseMap::usingSdCard()
{
#if defined(BASEMAP_CAN_USE_SD)
    return useSd;
#else
    return false;
#endif
}

void BaseMap::beginFrame()
{
    nCands = 0;
    nReserved = 0;
}

void BaseMap::reserve(int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (nReserved < kMaxReserved)
        reserved[nReserved++] = {x, y, w, h};
}

void BaseMap::drawLabels(OLEDDisplay *display, const Transform &t)
{
    // nameLen is only meaningful for v2; v3 keeps names per section and leaves it zero.
    if (!ok || nCands == 0 || (!usePacks && formatVer < kFormatV3 && nameLen == 0))
        return;

    // Rank: places, then major roads, then by how much of the road is on screen. Selection
    // sort - a few dozen entries, and it avoids pulling in <algorithm>.
    for (uint8_t i = 0; i < nCands; i++) {
        uint8_t best = i;
        for (uint8_t j = i + 1; j < nCands; j++) {
            const uint8_t rj = labelRank(cands[j].layer), rb = labelRank(cands[best].layer);
            const bool better = (rj < rb) || (rj == rb && cands[j].run > cands[best].run);
            if (better)
                best = j;
        }
        if (best != i) {
            const LabelCand tmp = cands[i];
            cands[i] = cands[best];
            cands[best] = tmp;
        }
    }

    auto overlaps = [](int16_t x, int16_t y, int16_t w, int16_t h) {
        for (uint8_t i = 0; i < nReserved; i++) {
            const Rect16 &r = reserved[i];
            if (x < r.x + r.w && r.x < x + w && y < r.y + r.h && r.y < y + h)
                return true;
        }
        return false;
    };

    File f;
    uint8_t openPack = 0xFE;
    if (!usePacks) {
        f = openMap();
        if (!f)
            return;
    }

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    uint32_t placedIds[kLabelsMax]; // hash of the name text: one street spans many tiles
    int16_t placedX[kLabelsMax], placedY[kLabelsMax];
    uint8_t placed = 0;
    char name[kNameMax];

    for (uint8_t i = 0; i < nCands && placed < kLabelsMax; i++) {
        const LabelCand &c = cands[i];
        const bool isPlace = (c.layer & kPlaceFlag) != 0;
        if (!isPlace) {
            if (t.mPerPx > kLabelZoomMPerPx)
                continue;
            if (c.layer >= 3 && t.mPerPx > kLocalZoomMPerPx)
                continue;
            if (c.layer == 2 && t.mPerPx > kMinorZoomMPerPx)
                continue;
        }

        uint8_t len = 0;
        if (usePacks && c.src == kSrcArena) {
            if (!v4::arena || c.nameId >= v4::arenaSize)
                continue;
            len = v4::arena[c.nameId];
            if (len == 0 || len >= kNameMax || c.nameId + 1 + len > v4::arenaSize)
                continue;
            memcpy(name, v4::arena + c.nameId + 1, len);
        } else {
            if (usePacks && c.src != openPack) {
                if (f)
                    mapClose(f);
                f = openPath(v4::packs[c.src].path);
                openPack = c.src;
            }
            if (!f)
                continue;
            // v2: offset into the one global blob. v3/v4: already an absolute file offset,
            // since the name lives in its own section's blob.
            const uint32_t at = (usePacks || formatVer >= kFormatV3) ? c.nameId : (nameOff + c.nameId);
            if (!mapSeek(f, at))
                break;
            if (mapRead(f, &len, 1) != 1 || len == 0 || len >= kNameMax)
                continue;
            if (mapRead(f, name, len) != len)
                continue;
        }
        name[len] = '\0';

        // Same name already on screen nearby - one "Edgewood Road" is enough.
        uint32_t h = 2166136261u;
        for (uint8_t k = 0; k < len; k++)
            h = (h ^ (uint8_t)name[k]) * 16777619u;
        bool dupe = false;
        for (uint8_t p = 0; p < placed && !dupe; p++) {
            const int32_t dx = placedX[p] - c.x, dy = placedY[p] - c.y;
            dupe = (placedIds[p] == h) && (dx * dx + dy * dy < kDedupePx * kDedupePx);
        }
        if (dupe)
            continue;

        // Cities and towns get the large font on the colour panel.
        const uint8_t placeKind = c.layer & 0x0F;
        bool large = false;
        int16_t w = 0, th = FONT_HEIGHT_SMALL;
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
        if (directColor) {
            large = isPlace && placeKind <= 1;
            const TftFont font = large ? TftFont::Large : TftFont::Small;
            w = graphics::tftDirectTextWidthF(name, font);
            th = graphics::tftDirectTextHeightF(font);
        } else
#endif
            w = (int16_t)display->getStringWidth(name, len, false);
        if (w <= 0 || (!isPlace && (int32_t)c.run < (int32_t)(w * kMinRunFactor)))
            continue; // not enough road on screen to own this name

        int16_t x = (int16_t)(c.x - w / 2);
        int16_t y = (int16_t)(c.y - th / 2);
        if (x < t.left)
            x = t.left;
        if (x + w > t.right)
            x = (int16_t)(t.right - w);
        if (y < t.top)
            y = t.top;
        if (y + th > t.bottom)
            y = (int16_t)(t.bottom - th);

        const int16_t bx = (int16_t)(x - kLabelPad), bw = (int16_t)(w + 2 * kLabelPad);
        if (overlaps(bx, y, bw, th))
            continue; // never shuffle a label away from its road - drop it instead

        // Knock the geometry out from behind the text: a name laid straight over road lines
        // is unreadable at any depth.
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
        if (directColor) {
            // The band has one owner, so a label cannot be left in the 1-bit buffer: the
            // blit never reaches these rows. Opaque text gives the knock-out for free.
            const uint16_t fg = !isPlace ? kColLabel : placeKind == kPlaceWater ? kColWaterName : kColPlace;
            graphics::tftDirectTextF(x, y, name, fg, kColMapBg, large ? TftFont::Large : TftFont::Small);
        } else
#endif
        {
            display->setColor(BLACK);
            display->fillRect(bx, y, bw, FONT_HEIGHT_SMALL);
            display->setColor(WHITE);
            display->drawString(x, y, name);
        }

        reserve(bx, y, bw, th);
        placedIds[placed] = h;
        placedX[placed] = c.x;
        placedY[placed] = c.y;
        placed++;
    }

    if (f)
        mapClose(f);
}

bool BaseMap::usingDetail()
{
    return usePacks ? (v4::level == v4::kLevels - 1) : curDetail;
}

bool BaseMap::bandIsDirect()
{
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
    return directColor;
#else
    return false;
#endif
}

void BaseMap::getOriginOffset(float &offsetE, float &offsetN)
{
    offsetE = curOffsetE;
    offsetN = curOffsetN;
}

bool BaseMap::getOrigin(double &lat, double &lon)
{
    if (!ok)
        return false;
    if (usePacks) {
        const v4::Pack &pk = v4::packs[0];
        lat = ((double)pk.minLat + pk.maxLat) * 0.5e-7;
        lon = ((double)pk.minLon + pk.maxLon) * 0.5e-7;
        return true;
    }
    lat = lat0_i * 1e-7;
    lon = lon0_i * 1e-7;
    return true;
}

void BaseMap::prepare(double viewOriginLat, double viewOriginLon, float mPerPx, int16_t halfSpanPx)
{
    if (!ok)
        return;

    if (usePacks) {
        using namespace v4;
        curOffsetE = curOffsetN = 0.0f; // tiles are placed from the view origin directly
        viewLat = viewOriginLat;
        viewLon = viewOriginLon;
        mPerDegLonView = 111320.0 * cos(viewOriginLat * M_PI / 180.0);
        // Detail level by zoom: every street up close, minor roads in the middle, water and
        // major roads only when a pixel is tens of metres across.
        level = (mPerPx <= 8.0f) ? 2 : (mPerPx <= 25.0f) ? 1 : 0;

        // The widest panel is 480 px; err wide rather than leave an edge undrawn.
        const double halfWideM = 240.0 * mPerPx, halfSpanM = halfSpanPx * (double)mPerPx;
        const double dLat = halfSpanM / kMPerDegLat, dLon = halfWideM / (mPerDegLonView > 1 ? mPerDegLonView : 1);
        const double s0 = viewOriginLat - dLat, n0 = viewOriginLat + dLat;
        const double w0 = viewOriginLon - dLon, e0 = viewOriginLon + dLon;

        uint32_t sig = 2166136261u ^ level;
        struct Range {
            int32_t ix0, ix1, iy0, iy1;
        } ranges[kMaxPacks];
        for (uint8_t p = 0; p < nPacks; p++) {
            const Pack &pk = packs[p];
            Range &r = ranges[p];
            r.ix0 = 1;
            r.ix1 = 0; // empty
            const double ps = pk.minLat * 1e-7, pn = pk.maxLat * 1e-7, pw = pk.minLon * 1e-7, pe = pk.maxLon * 1e-7;
            const double s = s0 > ps ? s0 : ps, n = n0 < pn ? n0 : pn, w = w0 > pw ? w0 : pw, e = e0 < pe ? e0 : pe;
            if (s > n || w > e)
                continue;
            const double scale = (double)(1u << pk.shift[level]);
            r.ix0 = (int32_t)floor((w + 180.0) * scale);
            r.ix1 = (int32_t)floor((e + 180.0) * scale);
            r.iy0 = (int32_t)floor((s + 90.0) * scale);
            r.iy1 = (int32_t)floor((n + 90.0) * scale);
            if (r.iy1 - r.iy0 >= kRowsMax)
                r.iy1 = r.iy0 + kRowsMax - 1;
            sig = (sig ^ (uint32_t)(p + 1)) * 16777619u;
            sig = (sig ^ (uint32_t)r.ix0) * 16777619u;
            sig = (sig ^ (uint32_t)r.ix1) * 16777619u;
            sig = (sig ^ (uint32_t)r.iy0) * 16777619u;
            sig = (sig ^ (uint32_t)r.iy1) * 16777619u;
        }

        // Same tiles as last time: nothing to look up. Panning within a tile, or zooming
        // inside one detail level, never touches the card.
        if (sig != needSig || nNeed == 0) {
            needSig = sig;
            nNeed = 0;
            for (uint8_t p = 0; p < nPacks && nNeed < kMaxNeed; p++) {
                const Range &r = ranges[p];
                if (r.ix1 < r.ix0)
                    continue;
                File f = openPath(packs[p].path);
                if (!f)
                    continue;
                for (int32_t iy = r.iy0; iy <= r.iy1 && nNeed < kMaxNeed; iy++) {
                    const uint32_t lo = makeKey(level, (uint32_t)iy, (uint32_t)r.ix0);
                    const uint32_t hi = makeKey(level, (uint32_t)iy, (uint32_t)r.ix1);
                    uint32_t idx = lowerBound(f, packs[p], lo);
                    if (idx >= packs[p].count || !mapSeek(f, packs[p].dirOff + idx * 12))
                        continue;
                    // Entries for one row are contiguous, so read forward until past ix1.
                    uint8_t e[12];
                    while (idx < packs[p].count && nNeed < kMaxNeed && mapRead(f, e, 12) == 12) {
                        const uint32_t key = rd32(e);
                        if (key > hi)
                            break;
                        need[nNeed++] = {key, rd32(e + 4), rd32(e + 8), p, -1};
                        idx++;
                    }
                }
                mapClose(f);
            }
        }
        fillArena();
        curDetail = (level == kLevels - 1);
        return;
    }

    // Where the file's origin sits relative to where we are now. Same equirectangular
    // form the packer used, so points land exactly where it intended.
    curOffsetE = (float)((lon0_i * 1e-7 - viewOriginLon) * mPerDegLon);
    curOffsetN = (float)((lat0_i * 1e-7 - viewOriginLat) * kMPerDegLat);

    // Visible rectangle, converted into basemap metres.
    const float halfSpanM = halfSpanPx * mPerPx;
    const float halfWideM = 160.0f * mPerPx;
    auto cellOf = [&](float m) {
        int32_t c = (int32_t)floorf((m + (float)halfM) / (float)cellM);
        if (c < 0)
            c = 0;
        if (c > grid - 1)
            c = grid - 1;
        return (int16_t)c;
    };
    cellX0 = cellOf(-halfWideM - curOffsetE);
    cellX1 = cellOf(halfWideM - curOffsetE);
    cellY0 = cellOf(-halfSpanM - curOffsetN);
    cellY1 = cellOf(halfSpanM - curOffsetN);

    // Pure geometry, no I/O: a wide view means many cells, and at that scale the overview
    // is the better picture anyway.
    const uint16_t cells = (uint16_t)(cellX1 - cellX0 + 1) * (uint16_t)(cellY1 - cellY0 + 1);
    curDetail = (cells <= kMaxDetailCells);
}

void BaseMap::draw(OLEDDisplay *display, const Transform &t)
{
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
    // Decided before the early return, because the node overlay asks bandIsDirect() after
    // this call and a stale answer would send markers to the wrong layer.
    directColor = ok && graphics::tftDirectAvailable();
#endif
    if (!ok)
        return;

#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
    if (directColor) {
        // Own the band: clear it ourselves, since the monochrome buffer is left blank here
        // and so the blit will never clear it for us.
        //
        // Whether to redraw at all is decided by the caller, not here. This used to gate
        // itself on the view, which covered the roads and nothing else - labels, markers and
        // the detail line are drawn by MapRenderer after this returns, so they went onto the
        // SPI bus every tick regardless and the band visibly churned. The gate has to cover
        // everything in the band or it covers nothing.
        graphics::tftDirectFillRect(t.left, t.top, (int16_t)(t.right - t.left + 1), (int16_t)(t.bottom - t.top + 1), kColMapBg);
    }
#endif

    const bool drawLocal = t.mPerPx <= 8.0f;
    const float inv = 1.0f / t.mPerPx;

    if (usePacks) {
        const uint32_t t0 = millis();
        uint8_t fromArena = 0;
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
        statSegs = statRows = 0;
        if (directColor)
            graphics::tftDirectBatch(true); // one panel transaction for the whole band
#endif
        File f;
        uint8_t openPack = 0xFF;
        for (uint8_t i = 0; i < v4::nNeed; i++) {
            const v4::Need &n = v4::need[i];
            const uint8_t shift = v4::packs[n.pack].shift[v4::level];
            if (n.slot >= 0 && v4::arena) {
                const v4::Slot &sl = v4::slots[n.slot];
                v4::MemSrc src{v4::arena + sl.at, sl.len, sl.at};
                v4::drawTile(display, src, kSrcArena, n.key, shift, t, drawLocal, inv);
                fromArena++;
                continue;
            }
            if (openPack != n.pack) {
                if (f)
                    mapClose(f);
                f = openPath(v4::packs[n.pack].path);
                openPack = n.pack;
            }
            if (!f || !mapSeek(f, n.off))
                continue;
            v4::FileSrc src{f, n.off};
            v4::drawTile(display, src, n.pack, n.key, shift, t, drawLocal, inv);
        }
        if (f)
            mapClose(f);
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
        if (directColor)
            graphics::tftDirectBatch(false);
        LOG_DEBUG("BaseMap: L%u %u tiles (%u from RAM), %u segs, %u fill rows, %u ms", v4::level, v4::nNeed, fromArena,
                  statSegs, statRows, (unsigned)(millis() - t0));
#endif
        return;
    }

    auto f = openMap();
    if (!f)
        return;

    Entry e;

    if (curDetail) {
        for (int16_t cy = cellY0; cy <= cellY1; cy++)
            for (int16_t cx = cellX0; cx <= cellX1; cx++)
                if (readEntry(f, (uint16_t)(1 + cy * grid + cx), e))
                    drawSection(display, f, e, t, drawLocal, inv);
    } else if (readEntry(f, 0, e)) {
        drawSection(display, f, e, t, drawLocal, inv);
    }

    mapClose(f);
}

} // namespace graphics

#endif
