#pragma once
#include "gfx.h"

#include <cstdint>
#include <string>

namespace bf {

// The Cardputer Zero's panel presents a 320x170 landscape canvas through
// /dev/fb0 (panel-mipi-dbi-spi). Geometry and pixel format are always taken
// from the driver via FBIOGET_*SCREENINFO -- never assumed -- and the app draws
// into its own packed RGB565 back buffer that is converted at present() time.
class Display {
public:
    ~Display();

    // Opens the framebuffer. On failure the display stays headless: rendering
    // still works into the back buffer, which is what the host build and
    // --preview use.
    bool open(const std::string& device, std::string& error);
    void close();
    bool isOpen() const { return fd_ >= 0; }

    int width() const { return w_; }
    int height() const { return h_; }
    Surface surface() { return canvas_.surface(); }
    Canvas& canvas() { return canvas_; }

    // Copies the back buffer to the panel. No-op when headless.
    void present();

    // Sets the logical size when headless (host preview).
    void setHeadlessSize(int w, int h);

    const std::string& formatDescription() const { return format_; }

private:
    int fd_ = -1;
    uint8_t* map_ = nullptr;
    size_t mapLen_ = 0;
    int w_ = 320, h_ = 170;
    int bpp_ = 16;
    int lineLength_ = 0;
    int xOffset_ = 0, yOffset_ = 0;
    // Channel placement as reported by the driver, used to build each pixel
    // rather than assuming the usual 11/5/0 RGB565 layout.
    int rOff_ = 11, rLen_ = 5, gOff_ = 5, gLen_ = 6, bOff_ = 0, bLen_ = 5;
    bool plainRgb565_ = true;
    std::string format_;
    Canvas canvas_;
};

// sysfs backlight, discovered by walking /sys/class/backlight. The recorded
// range on this unit is 0..100, but max_brightness is always read rather than
// assumed, and a write is verified by reading back.
class Backlight {
public:
    bool discover();
    bool available() const { return !path_.empty(); }
    int max() const { return max_; }
    int get() const;
    bool setPercent(int percent);
    int percent() const;
    const std::string& path() const { return path_; }

private:
    std::string path_;
    int max_ = 0;
};

} // namespace bf
