#pragma once

#include "configuration.h"

#if HAS_SCREEN && defined(SPECTRUM_ANALYZER)

// Colour TFT boards (the LCDWIKI CYDs) draw straight to the panel through the direct-colour layer
// and own the whole screen; everything else (e.g. the Heltec V3's 128x64 OLED) gets an ordinary
// BaseUI frame drawn into the 1-bit framebuffer.
#if defined(MESHTASTIC_MAP_DIRECT_COLOR)
#define SPECTRUM_DIRECT 1
#else
#define SPECTRUM_DIRECT 0
#endif

// Highest Test Transmit level offered, in dBm at the antenna (an E22-900M30S reaches ~30)
#ifndef SPECTRUM_TEST_MAX_DBM
#define SPECTRUM_TEST_MAX_DBM 30
#endif

#include "Observer.h"
#include "concurrency/OSThread.h"
#include "input/InputBroker.h"
#include "mesh/MeshModule.h"

/**
 * LoRa Actions > Spectrum: a stepped-RSSI spectrum analyzer and test transmitter on the node's own
 * SX126x. While it is open the mesh is paused (RadioLibInterface::spectrumPaused) and the module
 * owns the whole panel through the TFT direct-draw layer, so it needs no frame of its own.
 *
 * Long-press opens the options banner; tap moves the cursor. The radio returns to the mesh only
 * through "Exit to Mesh" - by request there is no inactivity timeout.
 */
class SpectrumModule : public MeshModule, public Observable<const UIFrameEvent *>, private concurrency::OSThread
{
  public:
    SpectrumModule();

    /// Take the radio and the screen. Shows a banner and stays off if the radio is busy.
    void start();
    bool isActive() const { return mode != Mode::Off; }

    bool wantPacket(const meshtastic_MeshPacket *p) override { return false; }
    bool interceptingKeyboardInput() override { return isActive(); }

    // Mono builds show the analyzer as a frame of its own while it is open
    bool wantUIFrame() override { return !SPECTRUM_DIRECT && isActive(); }
    Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }
#if !SPECTRUM_DIRECT
    void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif

  protected:
    int32_t runOnce() override;

  private:
    enum class Mode : uint8_t { Off, Sweep, TestTx };
    enum class View : uint8_t { Spectrum, Waterfall };

    // 902-928 MHz in 125 kHz bins (bin centres 902.0625 ... 927.9375)
    static constexpr uint32_t START_HZ = 902000000UL;
    static constexpr uint32_t STEP_HZ = 125000UL;
    static constexpr uint16_t BINS = 208;
    static constexpr int8_t DBM_BOTTOM = -130;
    static constexpr uint8_t TEST_PACKETS = 20;
    static constexpr uint32_t TEST_SPACING_MS = 250;

    int handleInputEvent(const InputEvent *event);
    CallbackObserver<SpectrumModule, const InputEvent *> inputObserver =
        CallbackObserver<SpectrumModule, const InputEvent *>(this, &SpectrumModule::handleInputEvent);

    void stop();
    void openOptions();
    void openTestTxMenu();
    void beginTestTx(int8_t dbm);
    void runSweepChunk();
    void runTestTx();

    // Drawing (all through graphics::tftDirect*, inside rows this module reserves)
    void layout();
    void drawChrome();
    void drawHeader(const char *left, const char *right);
    void drawColumn(uint16_t bin);  // one bin, the moment it is measured
    void finishWaterfallRow(); // advance the waterfall row and its "now" marker
    void setSpan(bool zoom);
    uint32_t binHz(uint16_t bin) const { return spanStartHz + STEP_HZ / 2 + (uint32_t)bin * STEP_HZ; }
    void drawTestTx();
    int16_t binX(uint16_t bin) const;
    int16_t dbmY(int16_t dbm, int16_t top, int16_t bottom) const;

    Mode mode = Mode::Off;
    View view = View::Spectrum;
    bool peakHold = true;

    // Banner hand-off: the 1-bit UI draws banners, so the panel is released while one is up
    bool bannerOpen = false;
    bool pendingTestMenu = false;
    bool needChrome = true;
    char lastHeader[64] = "";

    int8_t cur[BINS];
    int8_t peak[BINS];
    int8_t disp[BINS]; // smoothed level actually drawn (running average of cur)
    bool smooth = true;

    // Span: the whole band, or a zoom window around the cursor (far fewer steps, so far faster)
    static constexpr uint16_t ZOOM_BINS = 32; // +/-2 MHz at 125 kHz
    uint32_t spanStartHz = START_HZ;
    uint16_t nBins = BINS;
    uint16_t sweepBin = 0;
    uint32_t sweepStartMs = 0;
    uint16_t sweepMsTenths = 0; // tenths of a sweep per second, for the header
    uint16_t cursorBin = 39;    // 906.875 MHz, the US LongFast default slot
    int16_t floorDbm = -120;    // mean of the last sweep: the waterfall colours relative to it
    // Bar-graph scale, rebased on the noise floor so a quiet band does not sit as a sliver at the bottom
    static constexpr int8_t VIEW_SPAN = 100; // room above the floor for a strong test transmitter close by
    int8_t viewBottom = -130;
    int8_t viewTop = -60;
    // Sweeps left in the "Setting noise floor" splash before the graph is drawn
    static constexpr uint8_t CALIBRATION_SWEEPS = 4;
    uint8_t calibrationLeft = 0;
    int32_t calibrationSum = 0;
    void rescaleToFloor(bool force);
    void drawDbmLabels();
    bool needLabels = false;       // relabel the dBm axis only (a rescale), no full redraw
    uint8_t outOfBandSweeps = 0;   // consecutive sweeps with the floor outside the comfortable band
    uint32_t lastRescaleMs = 0;
    void drawSplash();
    uint16_t sweepFails = 0;    // bins in the last sweep whose SPI read failed
    uint32_t lastStatsMs = 0;
    uint32_t lastAwakeKickMs = 0;

    // Strongest reading of the last few seconds, for the peak readout (and the log)
    int8_t recentPeakDbm = DBM_BOTTOM;
    uint32_t recentPeakHz = 0;
    uint32_t recentPeakMs = 0;
    void notePeak(int8_t dbm, uint32_t hz);

    void framesChanged(); // mono: add/remove the analyzer frame
    void requestRedraw(); // mono: repaint the frame (throttled)
    uint32_t lastRedrawMs = 0;
    void nextPeak(); // mono: button press moves the cursor to the next strong peak

    // Test transmit
    int8_t testDbm = 0;
    uint8_t testSent = 0;
    bool testInFlight = false;
    uint32_t testNextMs = 0;
    uint32_t testStartedMs = 0;

    // Geometry, from the panel size at start()
    int16_t W = 0, H = 0;
    int16_t hdrH = 0, plotX0 = 0, plotX1 = 0, plotY0 = 0, plotY1 = 0;
    int16_t stripY1 = 0, wfY0 = 0, wfRow = 0;
};

extern SpectrumModule *spectrumModule;

#endif
