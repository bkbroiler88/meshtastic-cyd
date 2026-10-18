#pragma once

#include "configuration.h"

// Full-colour touch keyboard for the free-text compose screen. Drawn straight to the panel
// through the direct-draw API, so it needs a LovyanGFX TFT that opts into it.
#if defined(USE_VIRTUAL_KEYBOARD) && defined(MESHTASTIC_MAP_DIRECT_COLOR) && defined(USE_TFTDISPLAY)
#define HAS_COLOR_KEYBOARD 1

#include "concurrency/OSThread.h"
#include <stdint.h>

namespace graphics
{

/**
 * Layout, drawing and hit-testing for the colour keyboard. The text itself stays in the
 * caller (CannedMessageModule owns freetext/cursor and the send path); this class only
 * renders it and turns a touch into an action.
 *
 * Everything is drawn direct and redrawn by difference: a keypress repaints the lines of
 * text that changed, the suggestion cells that changed and nothing else, so the panel never
 * full-screen repaints while typing.
 */
class TouchKeyboard : private concurrency::OSThread
{
  public:
    enum class Action : uint8_t { None, Char, Backspace, Shift, Layer, Space, Send, Suggestion, Cursor, Cancel };

    struct Hit {
        Action action = Action::None;
        char ch = 0;          // Char: the character to insert (case already applied)
        uint16_t cursor = 0;  // Cursor: new cursor position
        const char *word = nullptr; // Suggestion: word to complete with
    };

    enum class DrawResult : uint8_t {
        Drawn,      // the keyboard owns the screen and is up to date
        Suppressed, // a banner or menu is up; draw nothing so it shows through
        Unavailable // no direct panel; caller should fall back to its own keyboard
    };

    TouchKeyboard();

    DrawResult draw(const char *text, uint16_t cursor, const char *header, uint16_t maxChars, int16_t width, int16_t height);
    Hit hitTest(int16_t x, int16_t y, const char *text, uint16_t cursor);

    /// Give the rows back to the 1-bit UI. Call when the compose screen goes away.
    void release();
    /// True while the keyboard owns the screen, so touches should come to hitTest().
    bool active() const { return holding; }

    bool shift = false;
    bool symbols = false;

  protected:
    int32_t runOnce() override;

  private:
    struct Layout {
        int16_t w = 0, h = 0;
        int16_t gap, hdrH, boxY, boxH, sugY, sugH, keysY, rowH, radius, closeW;
        float keyW;
        bool largeKeys;
    };
    struct KeyRect {
        int16_t x, y, w, h;
    };

    static constexpr uint8_t kMaxLines = 16;
    static constexpr uint8_t kSuggestions = 3;
    static constexpr uint8_t kWordMax = 24;

    Layout lay;
    bool laidOut = false;
    bool needFull = true;
    uint32_t lastDrawMs = 0;

    // What is on the panel now, so the next draw only touches what changed.
    uint32_t drawnLineHash[kMaxLines] = {};
    uint8_t drawnLineCount = 0;
    char drawnHeader[48] = {};
    uint16_t drawnLen = 0xFFFF;
    bool drawnShift = false, drawnSymbols = false;
    char drawnSug[kSuggestions][kWordMax] = {};

    // Text layout for the current frame: every wrapped line of the draft, of which
    // visibleLines starting at firstLine are shown (the box scrolls to keep the cursor in view).
    static constexpr uint8_t kMaxWrapped = 64;
    uint16_t lineStartAll[kMaxWrapped] = {};
    uint8_t lineLenAll[kMaxWrapped] = {};
    uint8_t totalLines = 0;
    uint8_t visibleLines = 0;
    uint8_t firstLine = 0;
    uint8_t cursorLine = 0;

    char sug[kSuggestions][kWordMax] = {};

    // Pop-up preview of the last key typed.
    int8_t previewRow = -1, previewCol = -1;
    char previewChar = 0;
    uint32_t previewUntil = 0;
    bool previewDrawn = false;
    bool previewNew = false; // set by hitTest(), cleared once draw() has shown it
    bool holding = false;    // we currently hold the direct-draw reservation
    KeyRect previewRect = {};

    uint8_t charW[96] = {}; // Small-font advance of each printable ASCII char
    bool charWReady = false;

    void computeLayout(int16_t width, int16_t height);
    const char *rowKeys(uint8_t row) const;
    uint8_t rowCount(uint8_t row) const;
    KeyRect keyRect(uint8_t row, uint8_t col) const;
    float keyWeight(uint8_t row, uint8_t col) const;
    float rowStart(uint8_t row) const;
    KeyRect closeRect() const;
    void wrapText(const char *text, uint16_t cursor);
    uint16_t textWidth(const char *s, uint16_t n);
    void updateSuggestions(const char *text, uint16_t cursor);

    void drawHeader(const char *header, uint16_t len, uint16_t maxChars);
    void drawTextBoxFrame();
    void drawLine(uint8_t slot, const char *text, uint16_t cursor);
    void drawSuggestions(bool force);
    void drawKeys();
    void drawKey(uint8_t row, uint8_t col);
    void drawPreview();
    void erasePreview();
};

} // namespace graphics

#endif
