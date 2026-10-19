#include "SpectrumModule.h"

#if HAS_SCREEN && defined(SPECTRUM_ANALYZER)

#include "PowerFSM.h"
#include "graphics/Screen.h"
#include "main.h"
#include "mesh/RadioLibInterface.h"
#if SPECTRUM_DIRECT
#include "graphics/TFTDisplay.h"
using graphics::TftFont;
#else
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#endif

SpectrumModule *spectrumModule;

namespace
{
#if SPECTRUM_DIRECT
constexpr uint16_t C_BLACK = 0x0000;
constexpr uint16_t C_WHITE = 0xFFFF;
constexpr uint16_t HEADER_GREY = 0x4208; // BaseUI title bar
constexpr uint16_t GRID = 0x2104;
constexpr uint16_t LABEL = 0xAD55;
constexpr uint16_t BAR = 0x2C6A;     // dim green body
constexpr uint16_t BAR_TOP = 0x3E8F; // bright green crest
constexpr uint16_t PEAK = 0xE504;    // amber peak hold


uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Waterfall palette: black -> navy -> cyan -> green -> yellow -> red over -125..-75 dBm. The caller
// shifts readings so the sweep's mean sits at -120: background noise reads as dark blue speckle.
uint16_t heat(int8_t dbm)
{
    static const uint8_t stops[][3] = {{0, 0, 0}, {0, 0, 140}, {0, 160, 200}, {0, 200, 60}, {240, 220, 0}, {230, 30, 20}};
    constexpr int n = sizeof(stops) / sizeof(stops[0]);
    int t = (dbm + 125) * 256 / 50; // 0..256
    if (t < 0)
        t = 0;
    if (t > 256)
        t = 256;
    const int pos = t * (n - 1); // 0..256*(n-1)
    const int k = pos >> 8;
    const int u = pos & 0xFF;
    const uint8_t *a = stops[k];
    const uint8_t *b = stops[k + 1 < n ? k + 1 : k];
    return rgb565(a[0] + ((b[0] - a[0]) * u >> 8), a[1] + ((b[1] - a[1]) * u >> 8), a[2] + ((b[2] - a[2]) * u >> 8));
}
#endif

RadioLibInterface *radio()
{
    return RadioLibInterface::instance;
}
} // namespace

SpectrumModule::SpectrumModule() : MeshModule("spectrum"), concurrency::OSThread("Spectrum")
{
    memset(cur, DBM_BOTTOM, sizeof(cur));
    memset(peak, DBM_BOTTOM, sizeof(peak));
    memset(disp, DBM_BOTTOM, sizeof(disp));
    inputObserver.observe(inputBroker);
    disable();
}

// ---------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------

void SpectrumModule::start()
{
    if (isActive() || !screen)
        return;
    if (!radio() || !radio()->spectrumBegin()) {
        screen->showSimpleBanner("Radio busy, try again", 2000);
        return;
    }
    LOG_INFO("Spectrum: radio taken from the mesh");
    memset(cur, DBM_BOTTOM, sizeof(cur));
    memset(peak, DBM_BOTTOM, sizeof(peak));
    memset(disp, DBM_BOTTOM, sizeof(disp));
    sweepBin = 0;
    calibrationLeft = CALIBRATION_SWEEPS;
    calibrationSum = 0;
    mode = Mode::Sweep;
    recentPeakDbm = DBM_BOTTOM;
#if SPECTRUM_DIRECT
    layout();
#endif
    screen->setModalModule(this);
    framesChanged();
    // Called from the confirm banner's callback: wait for that banner to close before drawing
    bannerOpen = true;
    needChrome = true;
    lastHeader[0] = '\0';
    lastAwakeKickMs = 0;
    enabled = true;
    setIntervalFromNow(20);
}

void SpectrumModule::stop()
{
    if (!isActive())
        return;
    mode = Mode::Off;
    testInFlight = false;
    if (radio())
        radio()->spectrumEnd();
    LOG_INFO("Spectrum: radio returned to the mesh");
#if SPECTRUM_DIRECT
    graphics::tftReleaseDirectRows(); // the release itself queues a full repaint of the 1-bit UI
#endif
    framesChanged();
    if (screen) {
        screen->clearModalModule(this);
        screen->forceDisplay(true);
    }
}

int32_t SpectrumModule::runOnce()
{
    if (!isActive() || !screen)
        return disable();

    const uint32_t now = millis();
    if (now - lastAwakeKickMs > 2000) { // keep the panel lit; there is no inactivity exit by design
        powerFSM.trigger(EVENT_PRESS);
        lastAwakeKickMs = now;
    }

    if (bannerOpen) {
        if (screen->isOverlayBannerShowing())
            return 50;
        bannerOpen = false;
        if (!isActive())
            return disable(); // "Exit to Mesh" was picked
        if (pendingTestMenu) {
            pendingTestMenu = false;
            openTestTxMenu();
            return 50;
        }
        needChrome = true;
    }

    // End row is exclusive. While the whole panel is reserved, the 1-bit UI's full repaints skip it
    // (TFTDisplay leaves reserved rows alone), so they are not a reason to redraw: doing so blanked
    // the screen on every UI repaint. Chrome is redrawn only on our own state changes.
#if SPECTRUM_DIRECT
    graphics::tftReserveDirectRows(0, H, 3000);
    if (needChrome) {
        drawChrome();
        needChrome = false;
        needLabels = false;
    } else if (needLabels) {
        drawDbmLabels();
        needLabels = false;
    }
#else
    needChrome = needLabels = false; // the frame is drawn whole every time
#endif

    if (mode == Mode::TestTx) {
        runTestTx();
        return 5;
    }
    runSweepChunk();
    return 2; // let the rest of the main loop (BLE, input, screen) breathe between chunks
}

// ---------------------------------------------------------------------------------------
// Sweep and test transmit
// ---------------------------------------------------------------------------------------

void SpectrumModule::runSweepChunk()
{
    RadioLibInterface *r = radio();
    if (!r)
        return;
    if (sweepBin == 0)
        sweepStartMs = millis();

    const uint32_t chunkStart = micros();
    while (sweepBin < nBins && micros() - chunkStart < 20000) {
        const int16_t v = r->spectrumSample(binHz(sweepBin));
        int8_t d = DBM_BOTTOM;
        if (v != INT16_MIN)
            d = (int8_t)(v < -128 ? -128 : (v > 0 ? 0 : v));
        else
            sweepFails++;
        cur[sweepBin] = d;
        // Two-sweep running average for the drawn level; the first reading seeds it
        disp[sweepBin] = (disp[sweepBin] == DBM_BOTTOM) ? d : (int8_t)((disp[sweepBin] + d) / 2);
        if (d > peak[sweepBin])
            peak[sweepBin] = d;
        // Paint this bin now rather than the whole graph at the end of the sweep: the display
        // refreshes continuously left to right instead of jumping a few times a second
#if SPECTRUM_DIRECT
        if (!calibrationLeft && !needChrome)
            drawColumn(sweepBin);
#endif
        sweepBin++;
    }
    if (sweepBin < nBins)
        return;

    sweepBin = 0;
    const uint32_t took = millis() - sweepStartMs;
    sweepMsTenths = took ? (uint16_t)(10000UL / took) : 0;

    int8_t lo = 0, hi = -128;
    uint16_t hiBin = 0;
    for (uint16_t i = 0; i < nBins; i++) {
        if (cur[i] < lo)
            lo = cur[i];
        if (cur[i] > hi) {
            hi = cur[i];
            hiBin = i;
        }
    }
    // Floor = the 25th-percentile level. A strong nearby transmitter can fill much of a zoomed
    // span (and lift neighbouring bins), which dragged a mean-based floor up and rescaled the
    // graph mid-test; the quietest quarter of the bins is not moved by it.
    {
        uint16_t hist[129] = {0}; // dBm -128..0
        for (uint16_t i = 0; i < nBins; i++)
            hist[cur[i] + 128]++;
        const uint16_t want = nBins / 4 + 1;
        uint16_t seen = 0;
        floorDbm = -128;
        for (int16_t b = 0; b <= 128; b++) {
            seen += hist[b];
            if (seen >= want) {
                floorDbm = b - 128;
                break;
            }
        }
    }
    notePeak(hi, binHz(hiBin));
    if (millis() - lastStatsMs > 1000) {
        lastStatsMs = millis();
        const uint32_t hiHz = binHz(hiBin);
        LOG_INFO("Spectrum: sweep %lums fails %u dBm min %d avg %d max %d @ %lu.%03lu MHz", (unsigned long)took, sweepFails, lo,
                 floorDbm, hi, (unsigned long)(hiHz / 1000000UL), (unsigned long)((hiHz / 1000UL) % 1000UL));
        if (lo == hi) // every bin identical: the readings are not real, log why
            r->spectrumProbe(binHz(cursorBin));
    }
    sweepFails = 0;

    if (calibrationLeft) {
        // Splash phase: average a few sweeps, then fix the scale and draw the real screen
        calibrationSum += floorDbm;
        if (--calibrationLeft == 0) {
            floorDbm = (int16_t)(calibrationSum / CALIBRATION_SWEEPS);
            memset(peak, DBM_BOTTOM, sizeof(peak));
            LOG_INFO("Spectrum: noise floor %d dBm", floorDbm);
            rescaleToFloor(true);
        }
#if !SPECTRUM_DIRECT
        requestRedraw();
#endif
        return;
    }
    if (lo != hi) // a flat sweep is a dead reading, not a floor: do not rescale (and redraw) on it
        rescaleToFloor(false);

#if SPECTRUM_DIRECT
    if (view == View::Waterfall && !needChrome)
        finishWaterfallRow();

    char left[32], right[32];
    snprintf(left, sizeof(left), W >= 400 ? "Spectrum  %u.%u/s" : "%u.%u/s", sweepMsTenths / 10, sweepMsTenths % 10);
    const uint32_t hz = binHz(cursorBin);
    snprintf(right, sizeof(right), "%lu.%03lu MHz  %d dBm", (unsigned long)(hz / 1000000UL),
             (unsigned long)((hz / 1000UL) % 1000UL), smooth ? disp[cursorBin] : cur[cursorBin]);
    drawHeader(left, right);
#else
    requestRedraw();
#endif
}

void SpectrumModule::beginTestTx(int8_t dbm)
{
    testDbm = dbm;
    testSent = 0;
    testInFlight = false;
    testNextMs = millis();
    mode = Mode::TestTx;
    needChrome = true;
    LOG_INFO("Spectrum: test transmit %u packets at %d dBm", TEST_PACKETS, dbm);
}

void SpectrumModule::runTestTx()
{
    RadioLibInterface *r = radio();
    if (!r)
        return;
    const uint32_t now = millis();

    if (testInFlight) {
        if (r->spectrumTestTxDone()) {
            testInFlight = false;
            testSent++;
            testNextMs = now + TEST_SPACING_MS;
#if SPECTRUM_DIRECT
            drawTestTx();
#else
            requestRedraw();
#endif
        } else if (now - testStartedMs > 3000) {
            LOG_WARN("Spectrum: test packet %u never reported TX done", testSent + 1);
            testInFlight = false;
            testSent++;
            testNextMs = now + TEST_SPACING_MS;
        }
        return;
    }

    if (testSent >= TEST_PACKETS) {
        LOG_INFO("Spectrum: test transmit done");
        mode = Mode::Sweep;
        sweepBin = 0;
        needChrome = true;
        return;
    }

    if ((int32_t)(now - testNextMs) >= 0) {
        if (r->spectrumTestTxStart(testDbm)) {
            testInFlight = true;
            testStartedMs = now;
        } else {
            LOG_WARN("Spectrum: test packet %u failed to start", testSent + 1);
            testSent++;
            testNextMs = now + TEST_SPACING_MS;
        }
    }
}

// ---------------------------------------------------------------------------------------
// Input and banners
// ---------------------------------------------------------------------------------------

int SpectrumModule::handleInputEvent(const InputEvent *event)
{
    if (!isActive())
        return 0;
    if (bannerOpen || (screen && screen->isOverlayBannerShowing()))
        return 0; // the banner owns input while it is up

    const input_broker_event ev = event->inputEvent;
    if (ev == INPUT_BROKER_SELECT || ev == INPUT_BROKER_SELECT_LONG) {
        openOptions(); // touch long-press arrives as SELECT
        return 1;
    }
    if (ev == INPUT_BROKER_USER_PRESS && mode == Mode::Sweep && !event->touchX && !event->touchY) {
        nextPeak(); // a physical button (the Heltec's): step the cursor through the strong peaks
        return 1;
    }
    if (ev == INPUT_BROKER_USER_PRESS && mode == Mode::Sweep && (event->touchX || event->touchY)) {
        const int16_t x = event->touchX;
        if (x >= plotX0 && x < plotX1) {
            cursorBin = (uint16_t)((int32_t)(x - plotX0) * nBins / (plotX1 - plotX0));
            if (cursorBin >= nBins)
                cursorBin = nBins - 1;
        }
    }
    return 1; // every other gesture is swallowed so nothing changes under the analyzer
}

void SpectrumModule::openOptions()
{
    static char viewLine[24];
    static char spanLine[24];
    static char smoothLine[24];
    static char peakLine[24];
    snprintf(viewLine, sizeof(viewLine), "View: %s", view == View::Spectrum ? "Spectrum" : "Waterfall");
    snprintf(spanLine, sizeof(spanLine), "Span: %s", nBins == BINS ? "Full Band" : "Zoom 4 MHz");
    snprintf(smoothLine, sizeof(smoothLine), "Smoothing: %s", smooth ? "On" : "Off");
    snprintf(peakLine, sizeof(peakLine), "Peak Hold: %s", peakHold ? "On" : "Off");
    enum { Back, ViewToggle, SpanToggle, SmoothToggle, PeakToggle, ClearPeaks, TestTx, Exit, OptionEnd };
    static const char *options[OptionEnd];
    static int optionEnums[OptionEnd];
    int n = 0;
    options[n] = "Back";
    optionEnums[n++] = Back;
#if SPECTRUM_DIRECT
    options[n] = viewLine; // a waterfall needs colour: the mono screen shows a peak readout instead
    optionEnums[n++] = ViewToggle;
#endif
    options[n] = spanLine;
    optionEnums[n++] = SpanToggle;
    options[n] = smoothLine;
    optionEnums[n++] = SmoothToggle;
    options[n] = peakLine;
    optionEnums[n++] = PeakToggle;
    options[n] = "Clear Peaks";
    optionEnums[n++] = ClearPeaks;
    options[n] = "Test Transmit";
    optionEnums[n++] = TestTx;
    options[n] = "Exit to Mesh";
    optionEnums[n++] = Exit;

    graphics::BannerOverlayOptions b;
    b.message = "Spectrum";
    b.optionsArrayPtr = options;
    b.optionsEnumPtr = optionEnums;
    b.optionsCount = n;
    b.bannerCallback = [this](int selected) {
        switch (selected) {
        case ViewToggle:
            view = (view == View::Spectrum) ? View::Waterfall : View::Spectrum;
            break;
        case SpanToggle:
            setSpan(nBins == BINS); // zoom in around the cursor, or back out to the full band
            break;
        case SmoothToggle:
            smooth = !smooth;
            break;
        case PeakToggle:
            peakHold = !peakHold;
            break;
        case ClearPeaks:
            memset(peak, DBM_BOTTOM, sizeof(peak));
            break;
        case TestTx:
            pendingTestMenu = true; // opened from runOnce once this banner has closed
            break;
        case Exit:
            stop();
            break;
        default:
            break;
        }
    };
    bannerOpen = true;
#if SPECTRUM_DIRECT
    graphics::tftReleaseDirectRows();
#endif
    screen->showOverlayBanner(b);
}

void SpectrumModule::openTestTxMenu()
{
    // Antenna power (through the board's PA, if any). The other board watches it in its Spectrum view.
    static char maxLine[20];
    snprintf(maxLine, sizeof(maxLine), "%d dBm (max)", SPECTRUM_TEST_MAX_DBM);
    static const char *options[] = {"Back", "10 dBm", "20 dBm", maxLine};
    static const int8_t levels[] = {0, 10, 20, SPECTRUM_TEST_MAX_DBM};
    graphics::BannerOverlayOptions b;
    b.message = "Test Transmit";
    b.optionsArrayPtr = options;
    b.optionsCount = 4;
    b.bannerCallback = [this](int selected) {
        if (selected >= 1 && selected <= 3)
            beginTestTx(levels[selected]);
    };
    bannerOpen = true;
#if SPECTRUM_DIRECT
    graphics::tftReleaseDirectRows();
#endif
    screen->showOverlayBanner(b);
}

// ---------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------

#if SPECTRUM_DIRECT
void SpectrumModule::layout()
{
    W = screen->getWidth();
    H = screen->getHeight();
    const int16_t th = graphics::tftDirectTextHeightF(TftFont::Small);
    hdrH = th + 8;
    plotX0 = graphics::tftDirectTextWidthF("-100", TftFont::Small) + 6;
    plotX1 = W - 4;
    plotY0 = hdrH + 4;
    plotY1 = H - th - 6;
    stripY1 = plotY0 + (plotY1 - plotY0) / 5;
    wfY0 = stripY1 + 3;
    wfRow = 0;
}

int16_t SpectrumModule::binX(uint16_t bin) const
{
    return plotX0 + (int16_t)((int32_t)(plotX1 - plotX0) * bin / nBins);
}

#endif

int16_t SpectrumModule::dbmY(int16_t dbm, int16_t top, int16_t bottom) const
{
    if (dbm > viewTop)
        dbm = viewTop;
    if (dbm < viewBottom)
        dbm = viewBottom;
    return top + (int16_t)((int32_t)(viewTop - dbm) * (bottom - top) / (viewTop - viewBottom));
}

#if SPECTRUM_DIRECT
void SpectrumModule::drawHeader(const char *left, const char *right)
{
    char key[sizeof(lastHeader)];
    snprintf(key, sizeof(key), "%s|%s", left, right);
    if (strcmp(key, lastHeader) == 0)
        return; // unchanged: no repaint, no flicker
    const bool fresh = lastHeader[0] == '\0'; // after drawChrome: the bar itself is not there yet
    strncpy(lastHeader, key, sizeof(lastHeader) - 1);

    // Text paints its own background, so it overwrites the old text in place; only the gap
    // between the two labels is refilled. Clearing the whole bar first made the text blink.
    const int16_t th = graphics::tftDirectTextHeightF(TftFont::Small);
    const int16_t ty = (hdrH - th) / 2;
    if (fresh)
        graphics::tftDirectFillRect(0, 0, W, hdrH, HEADER_GREY);
    const int16_t lw = graphics::tftDirectTextWidthF(left, TftFont::Small);
    const int16_t rw = graphics::tftDirectTextWidthF(right, TftFont::Small);
    const int16_t rx = W - 6 - rw;
    graphics::tftDirectTextF(6, ty, left, C_WHITE, HEADER_GREY, TftFont::Small);
    if (rx > 6 + lw)
        graphics::tftDirectFillRect(6 + lw, ty, rx - (6 + lw), th, HEADER_GREY);
    graphics::tftDirectTextF(rx, ty, right, C_WHITE, HEADER_GREY, TftFont::Small);
}

void SpectrumModule::drawChrome()
{
    graphics::tftDirectFillRect(0, 0, W, H, C_BLACK);
    lastHeader[0] = '\0';
    const int16_t th = graphics::tftDirectTextHeightF(TftFont::Small);

    if (mode == Mode::TestTx) {
        char right[16];
        snprintf(right, sizeof(right), "%d dBm", testDbm);
        drawHeader("Test Transmit", right);
        drawTestTx();
        return;
    }
    if (calibrationLeft) {
        drawSplash();
        return;
    }
    drawHeader("Spectrum", "");

    // Five evenly spaced frequency labels across whatever span is showing
    char buf[12];
    const uint32_t spanKhz = (uint32_t)nBins * (STEP_HZ / 1000);
    for (uint8_t k = 0; k <= 4; k++) {
        const uint32_t khz = spanStartHz / 1000 + spanKhz * k / 4;
        const int16_t x = plotX0 + (int16_t)((int32_t)(plotX1 - plotX0) * k / 4);
        if (khz % 1000 == 0)
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)(khz / 1000));
        else
            snprintf(buf, sizeof(buf), "%lu.%lu", (unsigned long)(khz / 1000), (unsigned long)((khz % 1000) / 100));
        const int16_t w = graphics::tftDirectTextWidthF(buf, TftFont::Small);
        int16_t lx = x - w / 2;
        if (lx + w > W)
            lx = W - w;
        graphics::tftDirectTextF(lx, plotY1 + 4, buf, LABEL, C_BLACK, TftFont::Small);
    }
    if (view == View::Spectrum)
        drawDbmLabels();
    else
        wfRow = 0;
}

// One bin's column, painted top-down in a single pass: black above the level, green below.
// Nothing is cleared first, so a redraw never shows an empty column.
void SpectrumModule::drawColumn(uint16_t i)
{
    const int16_t x = binX(i);
    int16_t w = binX(i + 1) - x;
    if (w < 1)
        w = 1;
    const int8_t level = smooth ? disp[i] : cur[i];
    const bool grid = (view == View::Spectrum);
    const int16_t top = plotY0;
    const int16_t bottom = grid ? plotY1 : stripY1; // the waterfall keeps a small live strip on top
    const int16_t y = dbmY(level, top, bottom);
    if (y > top)
        graphics::tftDirectFillRect(x, top, w, y - top, C_BLACK);
    if (grid) {
        for (int16_t d = viewTop - 10; d > viewBottom; d -= 20) {
            const int16_t gy = dbmY(d, top, bottom);
            if (gy < y)
                graphics::tftDirectFillRect(x, gy, w, 1, GRID);
        }
    }
    const bool atCursor = (i == cursorBin);
    if (bottom > y)
        graphics::tftDirectFillRect(x, y, w, bottom - y, atCursor ? C_WHITE : BAR);
    if (!atCursor)
        graphics::tftDirectFillRect(x, y, w, 1, BAR_TOP);
    if (grid && peakHold && peak[i] > level)
        graphics::tftDirectFillRect(x, dbmY(peak[i], top, bottom), w, 1, PEAK);

    if (!grid) {
        // This sweep's waterfall row, coloured relative to the floor. Raw level: the waterfall
        // is the history, and smoothing it would blur short bursts.
        const int16_t rows = (plotY1 - wfY0) / 2;
        if (rows <= 0)
            return;
        if (wfRow >= rows)
            wfRow = 0;
        graphics::tftDirectFillRect(x, wfY0 + wfRow * 2, w, 2, heat((int8_t)max(-128, min(0, cur[i] - floorDbm - 120))));
    }
}

// The waterfall writes one 2 px row per sweep at a moving cursor and wraps, rather than
// scrolling: no frame buffer, and only one row of the panel changes per sweep.
void SpectrumModule::finishWaterfallRow()
{
    const int16_t rows = (plotY1 - wfY0) / 2;
    if (rows <= 0)
        return;
    wfRow = (wfRow + 1) % rows;
    graphics::tftDirectFillRect(plotX0, wfY0 + wfRow * 2, plotX1 - plotX0, 1, C_WHITE); // "now" marker
}

#endif

void SpectrumModule::setSpan(bool zoom)
{
    // Keep the cursor's frequency, and centre the zoom window on it where the band edge allows
    const uint16_t fullIdx = (uint16_t)((binHz(cursorBin) - START_HZ) / STEP_HZ);
    if (zoom) {
        int32_t first = (int32_t)fullIdx - ZOOM_BINS / 2;
        if (first < 0)
            first = 0;
        if (first > BINS - ZOOM_BINS)
            first = BINS - ZOOM_BINS;
        spanStartHz = START_HZ + (uint32_t)first * STEP_HZ;
        nBins = ZOOM_BINS;
        cursorBin = fullIdx - (uint16_t)first;
    } else {
        spanStartHz = START_HZ;
        nBins = BINS;
        cursorBin = fullIdx;
    }
    memset(cur, DBM_BOTTOM, sizeof(cur));
    memset(disp, DBM_BOTTOM, sizeof(disp));
    memset(peak, DBM_BOTTOM, sizeof(peak));
    sweepBin = 0;
    wfRow = 0;
    needChrome = true;
}

// Floor sits ~10 dB above the bottom edge, on a 10 dB line. Rescaling relabels the axis (a full
// chrome redraw), so it only happens when the floor drifts out of a band around that point.
void SpectrumModule::rescaleToFloor(bool force)
{
    if (!force) {
        // A board whose floor swings (the 2.8" does) would otherwise rescale every few sweeps.
        // Only act on a floor that has stayed out of band for a while, and not too often.
        if (floorDbm >= viewBottom + 4 && floorDbm <= viewBottom + 18) {
            outOfBandSweeps = 0;
            return;
        }
        if (++outOfBandSweeps < 5 || millis() - lastRescaleMs < 10000)
            return;
    }
    const int16_t below = -(floorDbm - 10);
    int16_t bottom = (int16_t)(-((below + 9) / 10) * 10);
    if (bottom < -140)
        bottom = -140;
    if (bottom > -60)
        bottom = -60;
    outOfBandSweeps = 0;
    if (!force && bottom == viewBottom)
        return;
    LOG_INFO("Spectrum: rescale, floor %d dBm -> axis %d..%d", floorDbm, bottom, bottom + VIEW_SPAN);
    viewBottom = (int8_t)bottom;
    viewTop = (int8_t)(bottom + VIEW_SPAN);
    lastRescaleMs = millis();
    if (force)
        needChrome = true; // end of the splash: draw the real screen
    else
        needLabels = true; // the bars repaint themselves on the next pass
}

#if SPECTRUM_DIRECT
// The dBm scale down the left edge; cleared first because a rescale changes every number
void SpectrumModule::drawDbmLabels()
{
    if (view != View::Spectrum)
        return;
    const int16_t th = graphics::tftDirectTextHeightF(TftFont::Small);
    graphics::tftDirectFillRect(0, plotY0 - th / 2, plotX0 - 1, plotY1 - plotY0 + th, C_BLACK);
    char buf[8];
    for (int16_t d = viewTop - 10; d > viewBottom; d -= 20) {
        snprintf(buf, sizeof(buf), "%d", d);
        const int16_t w = graphics::tftDirectTextWidthF(buf, TftFont::Small);
        graphics::tftDirectTextF(plotX0 - 4 - w, dbmY(d, plotY0, plotY1) - th / 2, buf, LABEL, C_BLACK, TftFont::Small);
    }
}

void SpectrumModule::drawSplash()
{
    drawHeader("Spectrum", "902-928 MHz");
    static const char *title = "Setting noise floor";
    const int16_t lh = graphics::tftDirectTextHeightF(TftFont::Large);
    const int16_t tw = graphics::tftDirectTextWidthF(title, TftFont::Large);
    const int16_t cy = (hdrH + H) / 2;
    graphics::tftDirectTextF((W - tw) / 2, cy - lh, title, C_WHITE, C_BLACK, TftFont::Large);
    static const char *hint = "Keep nearby transmitters quiet";
    const int16_t hw = graphics::tftDirectTextWidthF(hint, TftFont::Small);
    graphics::tftDirectTextF((W - hw) / 2, cy + 8, hint, LABEL, C_BLACK, TftFont::Small);
}

void SpectrumModule::drawTestTx()
{
    char line[24];
    snprintf(line, sizeof(line), "  Sent %u / %u  ", testSent, TEST_PACKETS);
    const int16_t lh = graphics::tftDirectTextHeightF(TftFont::Large);
    const int16_t lw = graphics::tftDirectTextWidthF(line, TftFont::Large);
    const int16_t cy = (hdrH + H) / 2;
    graphics::tftDirectTextF((W - lw) / 2, cy - lh, line, C_WHITE, C_BLACK, TftFont::Large);
    static const char *hint = "Watch the other board's Spectrum view";
    const int16_t hw = graphics::tftDirectTextWidthF(hint, TftFont::Small);
    graphics::tftDirectTextF((W - hw) / 2, cy + 8, hint, LABEL, C_BLACK, TftFont::Small);
}

#endif // SPECTRUM_DIRECT

void SpectrumModule::notePeak(int8_t dbm, uint32_t hz)
{
    const uint32_t now = millis();
    if (dbm > recentPeakDbm || now - recentPeakMs > 3000) { // hold a peak for 3 s, then let it decay
        recentPeakDbm = dbm;
        recentPeakHz = hz;
        recentPeakMs = now;
    }
}

void SpectrumModule::framesChanged()
{
#if !SPECTRUM_DIRECT
    // Add (or drop) our frame and jump to it
    if (isActive())
        requestFocus();
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REGENERATE_FRAMESET;
    notifyObservers(&e);
#endif
}

void SpectrumModule::requestRedraw()
{
#if !SPECTRUM_DIRECT
    const uint32_t now = millis();
    if (now - lastRedrawMs < 100) // the OLED cannot show more than ~10 fps of this usefully
        return;
    lastRedrawMs = now;
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REDRAW_ONLY;
    notifyObservers(&e);
#endif
}

void SpectrumModule::nextPeak()
{
    // Next local maximum to the right of the cursor that stands well above the floor; wraps.
    // With nothing strong in the span, go to the strongest bin.
    auto level = [&](uint16_t b) { return (int)(smooth ? disp[b] : cur[b]); };
    for (uint16_t k = 1; k < nBins; k++) {
        const uint16_t b = (cursorBin + k) % nBins;
        const int l = level(b);
        if (l < floorDbm + 8)
            continue;
        if ((b == 0 || l >= level(b - 1)) && (b + 1 >= nBins || l >= level(b + 1))) {
            cursorBin = b;
            requestRedraw();
            return;
        }
    }
    uint16_t best = 0;
    for (uint16_t b = 1; b < nBins; b++)
        if (level(b) > level(best))
            best = b;
    cursorBin = best;
    requestRedraw();
}

#if !SPECTRUM_DIRECT
// 128x64 (or similar) mono layout: cursor readout on top, bars in the middle, peak readout below.
void SpectrumModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    const int16_t w = display->getWidth(), h = display->getHeight();
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    char buf[40];

    if (mode == Mode::TestTx) {
        snprintf(buf, sizeof(buf), "Test TX %d dBm", testDbm);
        display->drawString(x, y, buf);
        snprintf(buf, sizeof(buf), "Sent %u / %u", testSent, TEST_PACKETS);
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(x + w / 2, y + h / 2 - FONT_HEIGHT_SMALL / 2, buf);
        return;
    }
    if (calibrationLeft) {
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(x + w / 2, y + h / 2 - FONT_HEIGHT_SMALL, "Setting noise floor");
        display->drawString(x + w / 2, y + h / 2, "keep transmitters quiet");
        return;
    }

    // Cursor readout
    const uint32_t hz = binHz(cursorBin);
    snprintf(buf, sizeof(buf), "%lu.%03lu  %d dBm", (unsigned long)(hz / 1000000UL), (unsigned long)((hz / 1000UL) % 1000UL),
             smooth ? disp[cursorBin] : cur[cursorBin]);
    display->drawString(x, y, buf);

    // Bars: one pixel column per bin when zoomed, the strongest bin per column on the full band
    const int16_t top = y + FONT_HEIGHT_SMALL + 1;
    const int16_t bottom = y + h - FONT_HEIGHT_SMALL - 1;
    for (int16_t px = 0; px < w; px++) {
        uint16_t b0 = (uint16_t)((int32_t)px * nBins / w);
        uint16_t b1 = (uint16_t)((int32_t)(px + 1) * nBins / w);
        if (b1 <= b0)
            b1 = b0 + 1;
        int lvl = -128, pk = -128;
        bool cursorHere = false;
        for (uint16_t b = b0; b < b1 && b < nBins; b++) {
            const int l = smooth ? disp[b] : cur[b];
            if (l > lvl)
                lvl = l;
            if (peak[b] > pk)
                pk = peak[b];
            cursorHere |= (b == cursorBin);
        }
        const int16_t yy = dbmY(lvl, top, bottom);
        if (bottom > yy)
            display->drawVerticalLine(x + px, yy, bottom - yy);
        if (peakHold && pk > lvl)
            display->setPixel(x + px, dbmY(pk, top, bottom));
        if (cursorHere) // cursor: a dotted line from the top of the plot down to the bar
            for (int16_t cy = top; cy < yy; cy += 3)
                display->setPixel(x + px, cy);
    }
    for (int16_t px = 0; px < w; px += 4) // noise floor, dotted
        display->setPixel(x + px, dbmY(floorDbm, top, bottom));

    // Peak readout: the strongest thing seen in the last few seconds
    if (recentPeakHz)
        snprintf(buf, sizeof(buf), "Pk %d dBm %lu.%03lu", recentPeakDbm, (unsigned long)(recentPeakHz / 1000000UL),
                 (unsigned long)((recentPeakHz / 1000UL) % 1000UL));
    else
        snprintf(buf, sizeof(buf), "Pk --");
    display->drawString(x, y + h - FONT_HEIGHT_SMALL, buf);
}
#endif

#endif
