#pragma once

#include <GpioLogic.h>
#include <OLEDDisplay.h>

/**
 * An adapter class that allows using the LovyanGFX library as if it was an OLEDDisplay implementation.
 *
 * Remaining TODO:
 * optimize display() to only draw changed pixels (see other OLED subclasses for examples)
 * Use the fast NRF52 SPI API rather than the slow standard arduino version
 *
 * turn radio back on - currently with both on spi bus is fucked? or are we leaving chip select asserted?
 */
class TFTDisplay : public OLEDDisplay
{
  public:
    /* constructor
    FIXME - the parameters are not used, just a temporary hack to keep working like the old displays
    */
    TFTDisplay(uint8_t, int, int, OLEDDISPLAY_GEOMETRY, HW_I2C);

    // Destructor to clean up allocated memory
    ~TFTDisplay();

    // Write the buffer to the display memory
    virtual void display() override { display(false); };
    virtual void display(bool fromBlank);
    void sdlLoop();

    // Turn the display upside down
    virtual void flipScreenVertically();

    // Touch screen (static handlers)
    static bool hasTouch(void);
    static bool getTouch(int16_t *x, int16_t *y);

    // Functions for changing display brightness
    void setDisplayBrightness(uint8_t);

#ifdef LCDWIKI_PANEL_INVERT_TOGGLE
    /// Panel-level colour inversion (the controller's INVON/INVOFF command), not a UI
    /// theme. CYD panels ship with either polarity under the same product name, so the
    /// correct setting is per-board and has to be a user control. Applies immediately
    /// and persists in NVS; re-applied from init().
    void setDisplayInverted(bool inverted);
    static bool getDisplayInverted();
#endif

    /**
     * shim to make the abstraction happy
     *
     */
    void setDetected(uint8_t detected);

    /**
     * This is normally managed entirely by TFTDisplay, but some rare applications (heltec tracker) might need to replace the
     * default GPIO behavior with something a bit more complex.
     *
     * We (cruftily) make it static so that variant.cpp can access it without needing a ptr to the TFTDisplay instance.
     */
    static GpioPin *backlightEnable;

  protected:
    // the header size of the buffer used, e.g. for the SPI command header
    virtual int getBufferOffset(void) override { return 0; }

    // Send a command to the display (low level function)
    virtual void sendCommand(uint8_t com) override;

    // Connect to the display
    virtual bool connect() override;

    uint16_t *linePixelBuffer = nullptr;
    uint16_t *repaintChunkBuffer = nullptr;
#ifndef OLEDDISPLAY_DOUBLE_BUFFER
    // Built with OLEDDISPLAY_REDUCE_MEMORY there is no buffer_back to diff against. One 32-bit
    // hash per 32-column tile of each 8-row page stands in for it - 2.4 KB at 320x480 instead
    // of a 19.2 KB second framebuffer. See TFTDisplay::display().
    uint32_t *tileHash = nullptr;
    uint16_t tileCols = 0;
    void rehashAllTiles();
#endif
};

namespace graphics
{
/**
 * Draw straight to the panel, past the 1-bit framebuffer.
 *
 * BaseUI composes into one bit per pixel and the blit colours it through rectangular
 * regions, which is fine for headers and bars but cannot colour a map: roads of different
 * classes cross each other, so no rectangle partition matches the data. A per-pixel colour
 * buffer is the obvious alternative and is unaffordable - 15 KB at four colours, 120 KB at
 * full depth, on a board where a 3.2 KB allocation already aborts the node.
 *
 * Drawing direct costs no RAM at all. It is safe only because the blit compares each byte
 * against buffer_back and skips what has not changed: leave the 1-bit buffer blank under
 * the area drawn this way and those pixels are never overwritten. Two rules follow, and
 * both matter:
 *
 *   - only draw where the monochrome buffer stays blank, or the next blit fights you;
 *   - redraw whenever fullRepaintCounter() changes, because a full repaint (a colour
 *     region change, a frame transition) repaints every row and wipes this.
 */
bool tftDirectAvailable();

/// Bumped every time the panel is fully repainted, so callers can tell their pixels are gone.
uint32_t tftFullRepaintCounter();
void tftNoteFullRepaint();

/**
 * Reserve a horizontal band for direct drawing: the blit leaves those rows alone entirely.
 *
 * Without this the two layers fight over the same pixels. The 1-bit buffer has no notion of
 * "nothing here", so a full repaint paints the band from it - black - over whatever was
 * drawn direct, and anything drawn into the buffer inside the band is erased by the direct
 * layer's own background fill and then never restored, because the blit only re-pushes bytes
 * that changed. The result is a map that flickers and labels that blink in and out.
 *
 * So the band gets exactly one owner. Whoever reserves it draws all of it - background,
 * content and text - and no 1-bit UI may be placed inside it, because it will not be shown.
 * Pass y1 <= y0 to release.
 *
 * The reservation is a lease that lapses `leaseMs` after the owner's last call. The default
 * suits an owner redrawn every frame; one that only redraws on input (the screen idles at
 * 1 fps) needs a lease longer than that, and must release explicitly when it goes away.
 */
void tftReserveDirectRows(int16_t y0, int16_t y1, uint32_t leaseMs = 500);
void tftReleaseDirectRows();

void tftDirectFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
/// Hold the panel's SPI transaction open across many direct draws (begin=true), then
/// release it (begin=false). Nests. Without it every line and fill row pays a full
/// transaction setup, which dominates a map redraw.
void tftDirectBatch(bool begin);
void tftDirectFillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
void tftDirectFillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);
void tftDirectLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color, uint8_t width = 1);
void tftDirectCircle(int16_t x, int16_t y, int16_t r, uint16_t color, bool filled);
/// Text with a solid background, which is what gives a label its halo over the map.
void tftDirectText(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg);
int16_t tftDirectTextWidth(const char *s);
int16_t tftDirectTextHeight();

/// Fonts for direct text beyond the panel's tiny default. Small is 16 px, Large 26 px.
enum class TftFont : uint8_t { Small, Large };
/// Text in `font`, anchored at its top-left or (centered) its middle. Restores the panel's
/// previous font afterwards, so callers of tftDirectText() are unaffected.
void tftDirectTextF(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, TftFont font, bool centered = false);
int16_t tftDirectTextWidthF(const char *s, TftFont font);
int16_t tftDirectTextHeightF(TftFont font);
} // namespace graphics
