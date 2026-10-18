#include "TouchKeyboard.h"

#if HAS_COLOR_KEYBOARD

#include "KeyboardWords.h"
#include "graphics/Screen.h"
#include "graphics/TFTDisplay.h"
#include "graphics/draw/NotificationRenderer.h"
#include "main.h"
#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

namespace graphics
{
namespace
{
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t kBg = rgb(0x14, 0x16, 0x1a);
constexpr uint16_t kHeader = rgb(0x1f, 0x22, 0x27);
constexpr uint16_t kBox = rgb(0x1b, 0x1e, 0x23);
constexpr uint16_t kBoxEdge = rgb(0x3a, 0x3f, 0x48);
constexpr uint16_t kKey = rgb(0x2e, 0x32, 0x3a);
constexpr uint16_t kModKey = rgb(0x45, 0x4b, 0x56);
constexpr uint16_t kAccent = rgb(0x67, 0xea, 0x94); // Meshtastic green
constexpr uint16_t kOnAccent = rgb(0x0f, 0x2a, 0x1a);
constexpr uint16_t kText = rgb(0xee, 0xf1, 0xf4);
constexpr uint16_t kDim = rgb(0x8a, 0x91, 0x9c);
constexpr uint16_t kSugBg = rgb(0x23, 0x27, 0x2e);

// Long enough to outlast the 1 fps idle redraw with margin; released explicitly on exit.
constexpr uint32_t kLeaseMs = 2500;
constexpr uint32_t kPreviewMs = 320;

constexpr char KEY_SHIFT = '\x01';
constexpr char KEY_BKSP = '\x08';
const char *const kLetters[3] = {"qwertyuiop", "asdfghjkl", "\x01zxcvbnm\x08"};
const char *const kSymbols[3] = {"1234567890", "-/:;()$&@\"", "#%_?!'*+\x08"};
// Bottom row in key widths: layer, comma, space, period, send.
const float kBottomWeights[5] = {1.5f, 1.0f, 4.5f, 1.0f, 2.0f};
// Suggestions shown before a word is started, best first.
const char *const kStarters[3] = {"the", "you", "ok"};
// The best suggestion sits in the middle cell, flanked by the next two.
const uint8_t kCellOrder[3] = {1, 0, 2};

uint32_t fnv(const char *s, uint16_t n, uint32_t h = 2166136261u)
{
    while (n--) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

bool isWordChar(char c)
{
    return isalpha((unsigned char)c) || c == '\'';
}

bool rectsOverlap(int16_t ax, int16_t ay, int16_t aw, int16_t ah, int16_t bx, int16_t by, int16_t bw, int16_t bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}
} // namespace

TouchKeyboard::TouchKeyboard() : concurrency::OSThread("TouchKeyboard")
{
    disable(); // only runs to clear an expired key preview
}

int32_t TouchKeyboard::runOnce()
{
    // The preview has expired; ask the screen for a frame so draw() can take it down.
    if (screen)
        screen->forceDisplay(true); // on a TFT this just asks for a prompt frame
    return disable();
}

void TouchKeyboard::release()
{
    if (holding)
        tftReleaseDirectRows();
    holding = false;
    needFull = true;
    previewChar = 0;
    previewDrawn = false;
}

// ---------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------

void TouchKeyboard::computeLayout(int16_t width, int16_t height)
{
    const bool portrait = height > width;
    const float s = (float)min(width, height) / 320.0f; // 1.0 on the 3.5", 0.75 on the 2.8"

    lay.w = width;
    lay.h = height;
    lay.gap = max(3, (int)lroundf(4 * s));
    lay.radius = max(3, (int)lroundf(6 * s));
    lay.hdrH = max(24, (int)lroundf((portrait ? 32 : 28) * s));
    lay.closeW = max(40, (int)lroundf(width / 10.0f * 1.4f));
    lay.sugH = max(24, (int)lroundf((portrait ? 34 : 28) * s));
    // Keys take just under half the screen in portrait, half in landscape where the rows
    // would otherwise be too short to hit.
    const int keysH = (int)lroundf(height * (portrait ? 0.47f : 0.50f));
    lay.rowH = keysH / 4;
    lay.keysY = height - 4 * lay.rowH;
    lay.sugY = lay.keysY - lay.sugH;
    lay.boxY = lay.hdrH + lay.gap;
    lay.boxH = lay.sugY - lay.gap - lay.boxY;
    lay.keyW = (float)width / 10.0f;
    lay.largeKeys = lay.rowH >= 34 && lay.keyW >= 26;
}

const char *TouchKeyboard::rowKeys(uint8_t row) const
{
    return symbols ? kSymbols[row] : kLetters[row];
}

uint8_t TouchKeyboard::rowCount(uint8_t row) const
{
    return row < 3 ? (uint8_t)strlen(rowKeys(row)) : 5;
}

float TouchKeyboard::keyWeight(uint8_t row, uint8_t col) const
{
    if (row == 3)
        return kBottomWeights[col];
    const char ch = rowKeys(row)[col];
    return (ch == KEY_SHIFT || ch == KEY_BKSP) ? 1.5f : 1.0f; // bigger targets for the edit keys
}

float TouchKeyboard::rowStart(uint8_t row) const
{
    float units = 0;
    for (uint8_t c = 0; c < rowCount(row); c++)
        units += keyWeight(row, c);
    return (lay.w - units * lay.keyW) / 2; // rows narrower than the screen are centred
}

TouchKeyboard::KeyRect TouchKeyboard::closeRect() const
{
    const int16_t half = lay.gap / 2;
    return {(int16_t)(lay.w - lay.closeW - half), half, lay.closeW, (int16_t)(lay.hdrH - lay.gap)};
}

TouchKeyboard::KeyRect TouchKeyboard::keyRect(uint8_t row, uint8_t col) const
{
    float x, w;
    if (row < 3) {
        x = rowStart(row);
        for (uint8_t c = 0; c < col; c++)
            x += keyWeight(row, c) * lay.keyW;
        w = keyWeight(row, col) * lay.keyW;
    } else {
        x = 0;
        for (uint8_t c = 0; c < col; c++)
            x += kBottomWeights[c] * lay.keyW;
        w = kBottomWeights[col] * lay.keyW;
    }
    const int16_t half = lay.gap / 2;
    return {(int16_t)lroundf(x + half), (int16_t)(lay.keysY + row * lay.rowH + half), (int16_t)lroundf(w - lay.gap),
            (int16_t)(lay.rowH - lay.gap)};
}

// ---------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------

uint16_t TouchKeyboard::textWidth(const char *s, uint16_t n)
{
    uint16_t w = 0;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t c = (uint8_t)s[i];
        w += charW[(c >= 32 && c <= 126) ? c - 32 : 0];
    }
    return w;
}

void TouchKeyboard::wrapText(const char *text, uint16_t cursor)
{
    const int16_t pad = max(4, (int)lroundf(6 * lay.w / 320.0f));
    const int16_t lineH = tftDirectTextHeightF(TftFont::Small) + 3;
    const int16_t innerW = lay.w - 2 * lay.gap - 2 * pad - 2;
    visibleLines = (uint8_t)min<int>(kMaxLines, max(1, (lay.boxH - 2 * pad) / lineH));

    const uint16_t n = strlen(text);
    totalLines = 0;
    uint16_t i = 0;
    do {
        const uint16_t start = i;
        int32_t lastSpace = -1;
        uint16_t w = 0;
        while (i < n) {
            const uint8_t c = (uint8_t)text[i];
            const uint8_t cw = charW[(c >= 32 && c <= 126) ? c - 32 : 0];
            if (w + cw > innerW && i > start) {
                if (lastSpace >= start)
                    i = lastSpace + 1; // break after the last space; the word moves down
                break;
            }
            w += cw;
            if (c == ' ')
                lastSpace = i;
            i++;
        }
        lineStartAll[totalLines] = start;
        lineLenAll[totalLines] = (uint8_t)min<uint16_t>(i - start, 255);
        totalLines++;
    } while (i < n && totalLines < kMaxWrapped);

    // The cursor belongs to the last line that starts at or before it.
    cursorLine = 0;
    for (uint8_t l = 0; l < totalLines; l++)
        if (lineStartAll[l] <= cursor)
            cursorLine = l;

    // Scroll only as far as needed to keep the cursor line visible.
    if (cursorLine < firstLine)
        firstLine = cursorLine;
    else if (cursorLine >= firstLine + visibleLines)
        firstLine = cursorLine - visibleLines + 1;
    if (totalLines <= visibleLines)
        firstLine = 0;
    else if (firstLine > totalLines - visibleLines)
        firstLine = totalLines - visibleLines;
}

void TouchKeyboard::updateSuggestions(const char *text, uint16_t cursor)
{
    uint16_t b = cursor;
    while (b > 0 && isWordChar(text[b - 1]))
        b--;
    const uint16_t plen = min<uint16_t>(cursor - b, kWordMax - 1);

    // Capitalise to match what was typed, or at the start of a sentence.
    bool capital;
    if (plen > 0) {
        capital = isupper((unsigned char)text[b]);
    } else {
        int32_t k = (int32_t)b - 1;
        while (k >= 0 && text[k] == ' ')
            k--;
        capital = k < 0 || text[k] == '.' || text[k] == '!' || text[k] == '?';
    }

    char prefix[kWordMax];
    for (uint16_t i = 0; i < plen; i++)
        prefix[i] = (char)tolower((unsigned char)text[b + i]);

    uint8_t count = 0;
    if (plen == 0) {
        for (; count < kSuggestions; count++)
            strlcpy(sug[count], kStarters[count], kWordMax);
    } else {
        const char *w = kKeyboardWords;
        while (*w && count < kSuggestions) {
            const char *end = strchr(w, '\n');
            const uint16_t wl = end ? (uint16_t)(end - w) : (uint16_t)strlen(w);
            if (wl > plen && wl < kWordMax && strncmp(w, prefix, plen) == 0) {
                memcpy(sug[count], w, wl);
                sug[count][wl] = '\0';
                count++;
            }
            if (!end)
                break;
            w = end + 1;
        }
    }
    for (uint8_t i = count; i < kSuggestions; i++)
        sug[i][0] = '\0';

    for (uint8_t i = 0; i < count; i++) {
        // "i" and its contractions are always capitalised.
        const bool isI = sug[i][0] == 'i' && (sug[i][1] == '\0' || sug[i][1] == '\'');
        if (capital || isI)
            sug[i][0] = (char)toupper((unsigned char)sug[i][0]);
    }
}

// ---------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------

TouchKeyboard::DrawResult TouchKeyboard::draw(const char *text, uint16_t cursor, const char *header, uint16_t maxChars,
                                              int16_t width, int16_t height)
{
    if (!tftDirectAvailable())
        return DrawResult::Unavailable;

    // A banner or menu draws into the 1-bit buffer; give it the screen until it closes.
    if (NotificationRenderer::isOverlayBannerShowing() || NotificationRenderer::isMenuShowing()) {
        release();
        return DrawResult::Suppressed;
    }

    const uint32_t now = millis();
    if (!laidOut || width != lay.w || height != lay.h) {
        computeLayout(width, height);
        laidOut = true;
        needFull = true;
    }
    // If we went quiet long enough for the lease to lapse, the 1-bit UI has painted over us.
    if (now - lastDrawMs > kLeaseMs - 300)
        needFull = true;
    tftReserveDirectRows(0, height, kLeaseMs);
    holding = true;
    lastDrawMs = now;

    if (!charWReady) {
        char one[2] = {0, 0};
        for (uint8_t c = 32; c <= 126; c++) {
            one[0] = (char)c;
            charW[c - 32] = (uint8_t)tftDirectTextWidthF(one, TftFont::Small);
        }
        charWReady = true;
    }

    const uint16_t len = strlen(text);
    if (cursor > len)
        cursor = len;

    bool forceSuggestions = false;
    if (needFull) {
        tftDirectFillRect(0, 0, lay.w, lay.h, kBg);
        drawnHeader[0] = '\0';
        drawnLen = 0xFFFF;
        memset(drawnLineHash, 0, sizeof(drawnLineHash));
        previewDrawn = false;
        drawTextBoxFrame();
        drawKeys();
        forceSuggestions = true;
    } else if (previewDrawn && (now >= previewUntil || previewNew)) {
        erasePreview();
        forceSuggestions = true;
    }

    if (needFull || len != drawnLen || strcmp(header, drawnHeader) != 0)
        drawHeader(header, len, maxChars);

    wrapText(text, cursor);
    for (uint8_t slot = 0; slot < visibleLines; slot++) {
        const uint8_t line = firstLine + slot;
        uint32_t h = 0; // an empty slot
        if (line < totalLines) {
            h = fnv(text + lineStartAll[line], lineLenAll[line]);
            const uint16_t cur = (line == cursorLine) ? (uint16_t)(cursor - lineStartAll[line] + 1) : 0;
            h = fnv((const char *)&cur, sizeof(cur), h) | 1;
        }
        if (h != drawnLineHash[slot]) {
            drawLine(slot, text, cursor);
            drawnLineHash[slot] = h;
        }
    }

    updateSuggestions(text, cursor);
    drawSuggestions(forceSuggestions);

    if (!needFull && (shift != drawnShift || symbols != drawnSymbols))
        drawKeys();

    if (previewChar && !previewDrawn && now < previewUntil) {
        drawPreview();
        previewNew = false;
    }

    needFull = false;
    return DrawResult::Drawn;
}

void TouchKeyboard::drawHeader(const char *header, uint16_t len, uint16_t maxChars)
{
    const int16_t pad = lay.gap + 2;
    const int16_t ty = (lay.hdrH - tftDirectTextHeightF(TftFont::Small)) / 2;
    tftDirectFillRect(0, 0, lay.w, lay.hdrH, kHeader);

    const KeyRect x = closeRect();
    tftDirectFillRoundRect(x.x, x.y, x.w, x.h, lay.radius, kModKey);
    const int16_t xc = x.x + x.w / 2, yc = x.y + x.h / 2, arm = max(4, (int)(x.h * 0.22f));
    tftDirectLine(xc - arm, yc - arm, xc + arm, yc + arm, kText, 2);
    tftDirectLine(xc - arm, yc + arm, xc + arm, yc - arm, kText, 2);

    char count[16];
    snprintf(count, sizeof(count), "%u/%u", (unsigned)len, (unsigned)maxChars);
    const int16_t cw = tftDirectTextWidthF(count, TftFont::Small);
    const int16_t countX = x.x - pad - cw;
    tftDirectTextF(countX, ty, count, len >= maxChars ? kAccent : kDim, kHeader, TftFont::Small);

    // Truncate the destination so it never runs into the count.
    char label[48];
    strlcpy(label, header, sizeof(label));
    size_t n = strlen(label);
    while (n > 0 && tftDirectTextWidthF(label, TftFont::Small) > countX - 2 * pad)
        label[--n] = '\0';
    tftDirectTextF(pad, ty, label, kDim, kHeader, TftFont::Small);

    strlcpy(drawnHeader, header, sizeof(drawnHeader));
    drawnLen = len;
}

void TouchKeyboard::drawTextBoxFrame()
{
    const int16_t x = lay.gap, w = lay.w - 2 * lay.gap;
    tftDirectFillRoundRect(x, lay.boxY, w, lay.boxH, lay.radius, kBoxEdge);
    tftDirectFillRoundRect(x + 1, lay.boxY + 1, w - 2, lay.boxH - 2, max(1, lay.radius - 1), kBox);
}

void TouchKeyboard::drawLine(uint8_t slot, const char *text, uint16_t cursor)
{
    const int16_t pad = max(4, (int)lroundf(6 * lay.w / 320.0f));
    const int16_t lineH = tftDirectTextHeightF(TftFont::Small) + 3;
    const int16_t x = lay.gap + pad;
    const int16_t y = lay.boxY + pad + slot * lineH;

    tftDirectFillRect(lay.gap + 2, y, lay.w - 2 * lay.gap - 4, lineH, kBox);

    const uint8_t line = firstLine + slot;
    if (line >= totalLines)
        return;

    char buf[128];
    const uint16_t start = lineStartAll[line];
    const uint16_t n = min<uint16_t>(lineLenAll[line], sizeof(buf) - 1);
    memcpy(buf, text + start, n);
    buf[n] = '\0';
    tftDirectTextF(x, y + 1, buf, kText, kBox, TftFont::Small);

    if (line == cursorLine) {
        const int16_t cx = x + textWidth(text + start, cursor - start);
        tftDirectFillRect(cx, y + 1, 2, lineH - 3, kAccent);
    }
}

void TouchKeyboard::drawSuggestions(bool force)
{
    const int16_t cellW = lay.w / kSuggestions;
    for (uint8_t cell = 0; cell < kSuggestions; cell++) {
        const uint8_t idx = kCellOrder[cell];
        if (!force && strcmp(sug[idx], drawnSug[cell]) == 0)
            continue;
        const int16_t x = cell * cellW;
        const int16_t w = (cell == kSuggestions - 1) ? lay.w - x : cellW;
        tftDirectFillRect(x, lay.sugY, w, lay.sugH, kSugBg);
        if (cell < kSuggestions - 1)
            tftDirectFillRect(x + w - 1, lay.sugY + 6, 1, lay.sugH - 12, kBoxEdge);
        if (sug[idx][0])
            tftDirectTextF(x + w / 2, lay.sugY + lay.sugH / 2, sug[idx], cell == 1 ? kAccent : kText, kSugBg, TftFont::Small,
                           true);
        strlcpy(drawnSug[cell], sug[idx], kWordMax);
    }
}

void TouchKeyboard::drawKeys()
{
    tftDirectFillRect(0, lay.keysY, lay.w, lay.h - lay.keysY, kBg);
    for (uint8_t row = 0; row < 4; row++)
        for (uint8_t col = 0; col < rowCount(row); col++)
            drawKey(row, col);
    drawnShift = shift;
    drawnSymbols = symbols;
}

void TouchKeyboard::drawKey(uint8_t row, uint8_t col)
{
    const KeyRect r = keyRect(row, col);
    const int16_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    const TftFont font = lay.largeKeys ? TftFont::Large : TftFont::Small;
    char label[4] = {0, 0, 0, 0};

    if (row < 3) {
        const char ch = rowKeys(row)[col];
        if (ch == KEY_SHIFT) {
            const uint16_t bg = shift ? kAccent : kModKey, fg = shift ? kOnAccent : kText;
            tftDirectFillRoundRect(r.x, r.y, r.w, r.h, lay.radius, bg);
            const int16_t aw = r.w * 0.28f, ah = r.h * 0.24f, sw = max(2, (int)(r.w * 0.1f));
            tftDirectFillTriangle(cx, cy - ah, cx - aw, cy, cx + aw, cy, fg);
            tftDirectFillRect(cx - sw, cy, 2 * sw, ah, fg);
        } else if (ch == KEY_BKSP) {
            tftDirectFillRoundRect(r.x, r.y, r.w, r.h, lay.radius, kModKey);
            const int16_t bw = r.w * 0.24f, bh = r.h * 0.2f, tip = r.w * 0.16f;
            const int16_t left = cx - (bw + tip) / 2;
            tftDirectFillTriangle(left, cy, left + tip, cy - bh, left + tip, cy + bh, kText);
            tftDirectFillRect(left + tip, cy - bh, bw, 2 * bh + 1, kText);
            const int16_t xc = left + tip + bw / 2, xs = max(2, bh / 2);
            tftDirectLine(xc - xs, cy - xs, xc + xs, cy + xs, kModKey, 2);
            tftDirectLine(xc - xs, cy + xs, xc + xs, cy - xs, kModKey, 2);
        } else {
            tftDirectFillRoundRect(r.x, r.y, r.w, r.h, lay.radius, kKey);
            label[0] = (shift && !symbols) ? (char)toupper((unsigned char)ch) : ch;
            tftDirectTextF(cx, cy, label, kText, kKey, font, true);
        }
        return;
    }

    switch (col) {
    case 0:
        tftDirectFillRoundRect(r.x, r.y, r.w, r.h, lay.radius, kModKey);
        tftDirectTextF(cx, cy, symbols ? "abc" : "123", kText, kModKey, TftFont::Small, true);
        break;
    case 1:
    case 3:
        tftDirectFillRoundRect(r.x, r.y, r.w, r.h, lay.radius, kKey);
        label[0] = col == 1 ? ',' : '.';
        tftDirectTextF(cx, cy, label, kText, kKey, font, true);
        break;
    case 2:
        tftDirectFillRoundRect(r.x, r.y, r.w, r.h, lay.radius, kKey);
        break;
    default:
        tftDirectFillRoundRect(r.x, r.y, r.w, r.h, lay.radius, kAccent);
        tftDirectTextF(cx, cy, "Send", kOnAccent, kAccent, TftFont::Small, true);
        break;
    }
}

void TouchKeyboard::drawPreview()
{
    const KeyRect r = keyRect(previewRow, previewCol);
    const int16_t pw = max(36, (int)lroundf(lay.keyW * 1.5f));
    const int16_t ph = max(34, (int)lay.rowH);
    int16_t x = r.x + r.w / 2 - pw / 2;
    x = max<int16_t>(0, min<int16_t>(x, lay.w - pw));
    // Sit above the key, but never over the text box: a top-row preview overlaps its own key.
    const int16_t y = max<int16_t>(lay.sugY, r.y - ph - 2);

    tftDirectFillRoundRect(x, y, pw, ph, lay.radius, kAccent);
    const char label[2] = {previewChar, 0};
    tftDirectTextF(x + pw / 2, y + ph / 2, label, kOnAccent, kAccent, TftFont::Large, true);
    previewRect = {x, y, pw, ph};
    previewDrawn = true;
}

void TouchKeyboard::erasePreview()
{
    const KeyRect p = previewRect;
    tftDirectFillRect(p.x, p.y, p.w, p.h, kBg);
    // Repaint the keys the bubble covered; the suggestion bar is repainted by the caller.
    for (uint8_t row = 0; row < 4; row++)
        for (uint8_t col = 0; col < rowCount(row); col++) {
            const KeyRect k = keyRect(row, col);
            if (rectsOverlap(p.x, p.y, p.w, p.h, k.x - lay.gap, k.y - lay.gap, k.w + 2 * lay.gap, k.h + 2 * lay.gap))
                drawKey(row, col);
        }
    previewDrawn = false;
    if (millis() >= previewUntil)
        previewChar = 0;
}

// ---------------------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------------------

TouchKeyboard::Hit TouchKeyboard::hitTest(int16_t x, int16_t y, const char *text, uint16_t cursor)
{
    Hit hit;
    if (!laidOut)
        return hit;

    if (y >= lay.keysY) {
        // Nearest key, no dead zones: every pixel of the key area belongs to some key.
        const uint8_t row = (uint8_t)min(3, (y - lay.keysY) / lay.rowH);
        uint8_t col;
        if (row < 3) {
            const uint8_t n = rowCount(row);
            float edge = rowStart(row);
            for (col = 0; col < n - 1; col++) { // the outermost keys also own the margins
                edge += keyWeight(row, col) * lay.keyW;
                if (x < edge)
                    break;
            }
            const char ch = rowKeys(row)[col];
            if (ch == KEY_SHIFT) {
                hit.action = Action::Shift;
            } else if (ch == KEY_BKSP) {
                hit.action = Action::Backspace;
            } else {
                hit.action = Action::Char;
                hit.ch = (shift && !symbols) ? (char)toupper((unsigned char)ch) : ch;
            }
        } else {
            float edge = 0;
            for (col = 0; col < 4; col++) {
                edge += kBottomWeights[col] * lay.keyW;
                if (x < edge)
                    break;
            }
            static const Action kBottom[5] = {Action::Layer, Action::Char, Action::Space, Action::Char, Action::Send};
            hit.action = kBottom[col];
            if (col == 1 || col == 3)
                hit.ch = col == 1 ? ',' : '.';
        }

        if (hit.action == Action::Char) {
            // Show what was hit; a timer brings it down again. draw() erases any bubble
            // still up before drawing this one.
            previewNew = true;
            previewRow = row;
            previewCol = col;
            previewChar = hit.ch;
            previewUntil = millis() + kPreviewMs;
            enabled = true;
            setIntervalFromNow(kPreviewMs + 10);
        }
        return hit;
    }

    if (y < lay.hdrH) {
        // The close button takes the full header height and a generous width.
        if (x >= lay.w - lay.closeW - lay.gap)
            hit.action = Action::Cancel;
        return hit;
    }

    if (y >= lay.sugY) {
        const uint8_t cell = (uint8_t)min(kSuggestions - 1, x * kSuggestions / lay.w);
        const uint8_t idx = kCellOrder[cell];
        if (sug[idx][0]) {
            uint16_t b = min<uint16_t>(cursor, strlen(text));
            while (b > 0 && isWordChar(text[b - 1]))
                b--;
            hit.action = Action::Suggestion;
            hit.word = sug[idx];
            hit.cursor = b; // the partial word [b, cursor) is replaced
        }
        return hit;
    }

    if (y >= lay.boxY && y < lay.boxY + lay.boxH) {
        const int16_t pad = max(4, (int)lroundf(6 * lay.w / 320.0f));
        const int16_t lineH = tftDirectTextHeightF(TftFont::Small) + 3;
        const int16_t slot = max(0, min<int>(visibleLines - 1, (y - lay.boxY - pad) / lineH));
        const uint8_t line = firstLine + slot;
        hit.action = Action::Cursor;
        if (line >= totalLines) {
            hit.cursor = strlen(text);
            return hit;
        }
        // Land between the two characters nearest the tap.
        const int16_t rel = x - (lay.gap + pad);
        const uint16_t start = lineStartAll[line];
        uint16_t pos = start, w = 0;
        for (uint16_t i = 0; i < lineLenAll[line]; i++) {
            const uint8_t c = (uint8_t)text[start + i];
            const uint8_t cw = charW[(c >= 32 && c <= 126) ? c - 32 : 0];
            if (rel < w + cw / 2)
                break;
            w += cw;
            pos = start + i + 1;
        }
        // A wrapped line's trailing space belongs to it, but the cursor should stop before it.
        if (line + 1 < totalLines && pos == start + lineLenAll[line] && pos > start && text[pos - 1] == ' ')
            pos--;
        hit.cursor = pos;
    }
    return hit;
}

} // namespace graphics

#endif
