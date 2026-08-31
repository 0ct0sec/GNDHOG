#include "gfx.h"
#include "font6x8.h"

#include <algorithm>
#include <cstdio>

namespace bf {

void Canvas::resize(int w, int h) {
    w_ = w > 0 ? w : 0;
    h_ = h > 0 ? h : 0;
    store_.assign(static_cast<size_t>(w_) * h_, theme::black);
}

bool Canvas::writePpm(const std::string& path) const {
    if (w_ <= 0 || h_ <= 0) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w_, h_);
    std::vector<unsigned char> line(static_cast<size_t>(w_) * 3);
    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            const Color c = store_[static_cast<size_t>(y) * w_ + x];
            // Replicate the high bits into the low ones so 5/6-bit channels
            // span the full 0..255 range instead of topping out at 248/252.
            const unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
            line[x * 3 + 0] = static_cast<unsigned char>((r5 << 3) | (r5 >> 2));
            line[x * 3 + 1] = static_cast<unsigned char>((g6 << 2) | (g6 >> 4));
            line[x * 3 + 2] = static_cast<unsigned char>((b5 << 3) | (b5 >> 2));
        }
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
    return true;
}

void fill(Surface& s, Color c) {
    if (!s.valid()) return;
    std::fill(s.px, s.px + static_cast<size_t>(s.w) * s.h, c);
}

void fillRect(Surface& s, int x, int y, int w, int h, Color c) {
    if (!s.valid()) return;
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(s.w, x + w), y1 = std::min(s.h, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        Color* p = s.row(yy);
        std::fill(p + x0, p + x1, c);
    }
}

void hLine(Surface& s, int x, int y, int w, Color c) { fillRect(s, x, y, w, 1, c); }
void vLine(Surface& s, int x, int y, int h, Color c) { fillRect(s, x, y, 1, h, c); }

void rect(Surface& s, int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    hLine(s, x, y, w, c);
    hLine(s, x, y + h - 1, w, c);
    vLine(s, x, y, h, c);
    vLine(s, x + w - 1, y, h, c);
}

void drawChar(Surface& s, int x, int y, char c, Color fg) {
    if (!s.valid()) return;
    const uint8_t* g = glyph(c);
    for (int col = 0; col < kGlyphCols; ++col) {
        const int px = x + col;
        if (px < 0 || px >= s.w) continue;
        const uint8_t bits = g[col];
        if (!bits) continue;
        for (int rowIdx = 0; rowIdx < 7; ++rowIdx) {
            if (!(bits & (1u << rowIdx))) continue;
            const int py = y + rowIdx + 1;   // +1 for the leading row
            if (py < 0 || py >= s.h) continue;
            s.row(py)[px] = fg;
        }
    }
}

void drawChar(Surface& s, int x, int y, char c, Color fg, Color bg) {
    fillRect(s, x, y, kGlyphW, kGlyphH, bg);
    drawChar(s, x, y, c, fg);
}

int drawText(Surface& s, int x, int y, const std::string& t, Color fg) {
    int cx = x;
    for (char c : t) {
        drawChar(s, cx, y, c, fg);
        cx += kGlyphW;
    }
    return cx;
}

int drawText(Surface& s, int x, int y, const std::string& t, Color fg, Color bg) {
    fillRect(s, x, y, static_cast<int>(t.size()) * kGlyphW, kGlyphH, bg);
    return drawText(s, x, y, t, fg);
}

int drawTextClipped(Surface& s, int x, int y, const std::string& t, int maxChars, Color fg) {
    if (maxChars <= 0) return x;
    if (static_cast<int>(t.size()) <= maxChars) return drawText(s, x, y, t, fg);
    if (maxChars <= 1) return drawText(s, x, y, std::string(1, '~'), fg);
    return drawText(s, x, y, t.substr(0, static_cast<size_t>(maxChars) - 1) + "~", fg);
}

int textWidth(const std::string& t) { return static_cast<int>(t.size()) * kGlyphW; }

void drawScrollbar(Surface& s, int x, int y, int h, int first, int visible, int total) {
    if (h <= 0) return;
    if (total <= visible || total <= 0) {
        vLine(s, x, y, h, theme::rule);
        return;
    }
    fillRect(s, x, y, 2, h, theme::rule);
    int thumb = std::max(6, h * visible / total);
    thumb = std::min(thumb, h);
    const int span = total - visible;
    const int pos = span > 0 ? (h - thumb) * std::min(first, span) / span : 0;
    fillRect(s, x, y + pos, 2, thumb, theme::accentDim);
}

void drawProgress(Surface& s, int x, int y, int w, int h, float frac, Color fg, Color bg) {
    if (w <= 2 || h <= 2) return;
    fillRect(s, x, y, w, h, bg);
    rect(s, x, y, w, h, theme::rule);
    frac = std::max(0.0f, std::min(1.0f, frac));
    const int inner = static_cast<int>((w - 2) * frac);
    if (inner > 0) fillRect(s, x + 1, y + 1, inner, h - 2, fg);
}

} // namespace bf
