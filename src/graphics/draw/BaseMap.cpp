#include "graphics/draw/BaseMap.h"

#if defined(BASEUI_HAS_MAP)

#include "FSCommon.h"
#include "NodeDB.h" // for `config`, to check whether Bluetooth is holding the SD card off
#include "configuration.h"
#include <cmath>
#include <cstring>

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
constexpr uint8_t kFormatVersion = 1;
constexpr uint8_t kMaxGrid = 16;
constexpr uint32_t kTableOff = 24; // header is 24 bytes, offset table follows

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
void drawSeg(OLEDDisplay *display, int32_t x0, int32_t y0, int32_t x1, int32_t y1, bool dashed,
             const BaseMap::Transform &t)
{
    if (!clipSeg(x0, y0, x1, y1, t))
        return;

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

File openMap()
{
#if defined(BASEMAP_CAN_USE_SD)
    if (useSd) {
        concurrency::LockGuard g(spiLock);
        return SD.open(kPath, FILE_O_READ);
    }
#endif
    return FSCom.open(kPath, FILE_O_READ);
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
    const uint32_t nWays = rd32(hdr);

    for (uint32_t w = 0; w < nWays; w++) {
        uint8_t wh[3];
        if (mapRead(f, wh, 3) != 3)
            return;
        const uint8_t layer = wh[0];
        const uint16_t npts = rd16(wh + 1);
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
        int32_t px = 0, py = 0;
        bool first = true;
        uint16_t remaining = npts;
        while (remaining) {
            const uint16_t chunk = remaining < kPtsPerRead ? remaining : kPtsPerRead;
            const size_t want = (size_t)chunk * 4;
            if (mapRead(f, win, want) != want)
                return;
            for (uint16_t i = 0; i < chunk; i++) {
                const float ee = (float)rdi16(win + i * 4) + t.offsetE - t.panEastM;
                const float nn = (float)rdi16(win + i * 4 + 2) + t.offsetN - t.panNorthM;
                const int32_t sx = t.cx + (int32_t)lroundf(ee * inv);
                const int32_t sy = t.cy - (int32_t)lroundf(nn * inv);
                if (!first)
                    drawSeg(display, px, py, sx, sy, isWater, t);
                px = sx;
                py = sy;
                first = false;
            }
            remaining -= chunk;
        }
    }
}

} // namespace

bool BaseMap::begin()
{
    if (inited)
        return ok;
    inited = true;

#if defined(SDCARD_EXCLUSIVE_WITH_BLUETOOTH)
    // Maps are an SD-card feature, and the card is not mounted while Bluetooth is on -
    // there is not enough 8-bit DRAM for both (see the gate in main.cpp). Report that
    // plainly rather than quietly serving a stale internal copy; the map frame turns this
    // into an on-screen explanation.
    if (config.bluetooth.enabled) {
        LOG_INFO("BaseMap: maps unavailable while Bluetooth is on (SD card not mounted)");
        return false;
    }
#endif

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

    auto f = openMap();
    if (!f) {
        LOG_INFO("BaseMap: no %s on SD or internal flash, map frame will show nodes only", kPath);
        return false;
    }

    uint8_t hdr[24];
    if (mapRead(f, hdr, sizeof(hdr)) != sizeof(hdr)) {
        LOG_WARN("BaseMap: %s too short", kPath);
        mapClose(f);
        return false;
    }
    if (memcmp(hdr, "MBM1", 4) != 0 || hdr[4] != kFormatVersion) {
        LOG_WARN("BaseMap: bad magic/version in %s", kPath);
        mapClose(f);
        return false;
    }
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

bool BaseMap::usingDetail()
{
    return curDetail;
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
    lat = lat0_i * 1e-7;
    lon = lon0_i * 1e-7;
    return true;
}

void BaseMap::prepare(double viewOriginLat, double viewOriginLon, float mPerPx, int16_t halfSpanPx)
{
    if (!ok)
        return;

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
    if (!ok)
        return;

    auto f = openMap();
    if (!f)
        return;

    const bool drawLocal = t.mPerPx <= 8.0f;
    const float inv = 1.0f / t.mPerPx;
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
