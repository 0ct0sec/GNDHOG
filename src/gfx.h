#pragma once
#include "font6x8.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf {

// The app always renders into a plain, tightly packed RGB565 surface. Whatever
// the real framebuffer's bitfields turn out to be is handled once, at blit
// time, in fbdev.cpp -- drawing code never has to care.
using Color = uint16_t;

constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<Color>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

namespace theme {
constexpr Color bg        = rgb(0x0B, 0x0F, 0x14);
constexpr Color panel     = rgb(0x16, 0x20, 0x2B);
constexpr Color panelHi   = rgb(0x22, 0x2F, 0x3D);
constexpr Color text      = rgb(0xC8, 0xD6, 0xE0);
constexpr Color textDim   = rgb(0x6B, 0x7E, 0x8C);
constexpr Color accent    = rgb(0xFF, 0x8C, 0x1A);   // Betaflight orange
constexpr Color accentDim = rgb(0x8A, 0x4C, 0x0E);
constexpr Color ok        = rgb(0x3A, 0xD0, 0x62);
constexpr Color warn      = rgb(0xFF, 0xC4, 0x3A);
constexpr Color err       = rgb(0xFF, 0x5A, 0x5A);
constexpr Color echo      = rgb(0x7F, 0xB2, 0xFF);   // lines the FC echoed back
constexpr Color rule      = rgb(0x2A, 0x38, 0x47);
constexpr Color black     = rgb(0, 0, 0);
} // namespace theme

struct Surface {
    Color* px = nullptr;
    int w = 0;
    int h = 0;

    bool valid() const { return px != nullptr && w > 0 && h > 0; }
    Color* row(int y) { return px + static_cast<size_t>(y) * w; }
};

// An owning surface, used for the back buffer and for host-side previews.
class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h) { resize(w, h); }
    void resize(int w, int h);
    Surface surface() { return Surface{store_.empty() ? nullptr : store_.data(), w_, h_}; }
    int width() const { return w_; }
    int height() const { return h_; }
    bool writePpm(const std::string& path) const;

private:
    std::vector<Color> store_;
    int w_ = 0;
    int h_ = 0;
};

void fill(Surface& s, Color c);
void fillRect(Surface& s, int x, int y, int w, int h, Color c);
// Pull every pixel halfway toward `toward`, preserving the image while lowering
// its contrast. Used for modal backdrops where scanline erasure is too noisy.
void dimSurface(Surface& s, Color toward);
void hLine(Surface& s, int x, int y, int w, Color c);
void vLine(Surface& s, int x, int y, int h, Color c);
void rect(Surface& s, int x, int y, int w, int h, Color c);
// Arbitrary-angle line and circles, for the compass rose on the Locate
// screen. Every pixel is clipped individually, so an endpoint off the surface
// costs nothing but the pixels that were never going to be visible.
void drawLine(Surface& s, int x0, int y0, int x1, int y1, Color c);
void drawCircle(Surface& s, int cx, int cy, int r, Color c);
void fillCircle(Surface& s, int cx, int cy, int r, Color c);

// Text. All coordinates are the glyph cell's top-left corner.
void drawChar(Surface& s, int x, int y, char c, Color fg);
void drawChar(Surface& s, int x, int y, char c, Color fg, Color bg);
int  drawText(Surface& s, int x, int y, const std::string& t, Color fg);
int  drawText(Surface& s, int x, int y, const std::string& t, Color fg, Color bg);
// Clipped to `maxChars`, appending an ellipsis when it had to cut.
int  drawTextClipped(Surface& s, int x, int y, const std::string& t, int maxChars, Color fg);
int  textWidth(const std::string& t);
// The same 6x8 glyphs at an integer multiple, for the one or two numbers an
// operator has to read at arm's length in sunlight. Returns the x after the
// last glyph, as drawText does.
int  drawTextScaled(Surface& s, int x, int y, const std::string& t, int scale, Color fg);

// Small chrome helpers shared by the screens.
void drawScrollbar(Surface& s, int x, int y, int h, int first, int visible, int total);
void drawProgress(Surface& s, int x, int y, int w, int h, float frac, Color fg, Color bg);

} // namespace bf
