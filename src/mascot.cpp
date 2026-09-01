#include "mascot.h"
#include <algorithm>

namespace bf {
static_assert(sizeof(kMascotBlackBits) * 8 == kMascotSize * kMascotSize,
              "the mascot black mask must contain one bit per pixel");
static_assert(sizeof(kMascotWhiteBits) * 8 == kMascotSize * kMascotSize,
              "the mascot white mask must contain one bit per pixel");

void drawMascot(Surface& s, int x, int y) {
    if (!s.valid()) return;
    for (int yy = std::max(0, -y); yy < kMascotSize && y + yy < s.h; ++yy) {
        for (int xx = std::max(0, -x); xx < kMascotSize && x + xx < s.w; ++xx) {
            const int bit = yy * kMascotSize + xx;
            const uint8_t mask = static_cast<uint8_t>(0x80u >> (bit % 8));
            if (kMascotBlackBits[bit / 8] & mask) {
                s.row(y + yy)[x + xx] = theme::black;
            } else if (kMascotWhiteBits[bit / 8] & mask) {
                s.row(y + yy)[x + xx] = rgb(0xff, 0xff, 0xff);
            }
        }
    }
}
} // namespace bf
