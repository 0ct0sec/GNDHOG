#include "mascot.h"
#include <algorithm>

namespace bf {
static_assert(sizeof(kMascotBits) * 8 == kMascotSize * kMascotSize,
              "the mascot must contain one bit per pixel");

void drawMascot(Surface& s, int x, int y) {
    if (!s.valid()) return;
    for (int yy = std::max(0, -y); yy < kMascotSize && y + yy < s.h; ++yy) {
        for (int xx = std::max(0, -x); xx < kMascotSize && x + xx < s.w; ++xx) {
            const int bit = yy * kMascotSize + xx;
            const bool white = kMascotBits[bit / 8] & (0x80u >> (bit % 8));
            s.row(y + yy)[x + xx] = white ? rgb(255, 255, 255) : theme::black;
        }
    }
}
} // namespace bf
