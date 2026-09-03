#include "display.h"
#include "storage.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace bf {

Display::~Display() { close(); }

void Display::setHeadlessSize(int w, int h) {
    if (isOpen()) return;
    w_ = w;
    h_ = h;
    canvas_.resize(w_, h_);
    format_ = "headless RGB565";
}

bool Display::open(const std::string& device, std::string& error) {
#if defined(__linux__)
    close();
    fd_ = ::open(device.c_str(), O_RDWR);
    if (fd_ < 0) {
        error = device + ": " + std::strerror(errno);
        if (errno == EACCES) error += " (add the user to the video group)";
        return false;
    }

    fb_var_screeninfo var{};
    fb_fix_screeninfo fix{};
    if (::ioctl(fd_, FBIOGET_VSCREENINFO, &var) != 0 ||
        ::ioctl(fd_, FBIOGET_FSCREENINFO, &fix) != 0) {
        error = std::string("FBIOGET_SCREENINFO: ") + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    w_ = static_cast<int>(var.xres);
    h_ = static_cast<int>(var.yres);
    bpp_ = static_cast<int>(var.bits_per_pixel);
    lineLength_ = static_cast<int>(fix.line_length);
    xOffset_ = static_cast<int>(var.xoffset);
    yOffset_ = static_cast<int>(var.yoffset);
    rOff_ = static_cast<int>(var.red.offset);   rLen_ = static_cast<int>(var.red.length);
    gOff_ = static_cast<int>(var.green.offset); gLen_ = static_cast<int>(var.green.length);
    bOff_ = static_cast<int>(var.blue.offset);  bLen_ = static_cast<int>(var.blue.length);

    if (bpp_ != 16) {
        error = "unsupported framebuffer depth: " + std::to_string(bpp_) +
                " bits/pixel (this app renders RGB565)";
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    plainRgb565_ = (rOff_ == 11 && rLen_ == 5 && gOff_ == 5 && gLen_ == 6 &&
                    bOff_ == 0 && bLen_ == 5);

    mapLen_ = fix.smem_len ? fix.smem_len
                           : static_cast<size_t>(lineLength_) * var.yres_virtual;
    void* m = ::mmap(nullptr, mapLen_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
        error = std::string("mmap: ") + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    map_ = static_cast<uint8_t*>(m);

    std::ostringstream fmt;
    fmt << w_ << "x" << h_ << " " << bpp_ << "bpp stride=" << lineLength_
        << (plainRgb565_ ? " RGB565" : " custom-bitfields");
    format_ = fmt.str();

    canvas_.resize(w_, h_);
    return true;
#else
    error = "framebuffer output is only implemented on Linux";
    canvas_.resize(w_, h_);
    return false;
#endif
}

void Display::close() {
#if defined(__linux__)
    if (map_ && map_ != MAP_FAILED) ::munmap(map_, mapLen_);
    if (fd_ >= 0) ::close(fd_);
#endif
    map_ = nullptr;
    mapLen_ = 0;
    fd_ = -1;
}

void Display::present() {
#if defined(__linux__)
    if (fd_ < 0 || !map_) return;
    const Color* src = const_cast<Canvas&>(canvas_).surface().px;
    if (!src) return;

    const size_t rowBytes = static_cast<size_t>(w_) * 2;
    for (int y = 0; y < h_; ++y) {
        const size_t offset = static_cast<size_t>(y + yOffset_) * lineLength_ +
                              static_cast<size_t>(xOffset_) * 2;
        // A driver whose stride or pan offset disagrees with yres must cost a
        // missing row, not a write past the mapping.
        if (offset + rowBytes > mapLen_) break;
        uint8_t* dstRow = map_ + offset;
        const Color* srcRow = src + static_cast<size_t>(y) * w_;
        if (plainRgb565_) {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(w_) * 2);
            continue;
        }
        // Rebuild each pixel using the driver's own channel placement.
        uint16_t* d = reinterpret_cast<uint16_t*>(dstRow);
        for (int x = 0; x < w_; ++x) {
            const Color c = srcRow[x];
            const unsigned r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            const unsigned rv = rLen_ >= 5 ? (r << (rLen_ - 5)) : (r >> (5 - rLen_));
            const unsigned gv = gLen_ >= 6 ? (g << (gLen_ - 6)) : (g >> (6 - gLen_));
            const unsigned bv = bLen_ >= 5 ? (b << (bLen_ - 5)) : (b >> (5 - bLen_));
            d[x] = static_cast<uint16_t>((rv << rOff_) | (gv << gOff_) | (bv << bOff_));
        }
    }
#endif
}

// ---------------------------------------------------------------- Backlight

bool Backlight::discover() {
#if defined(__linux__)
    DIR* d = ::opendir("/sys/class/backlight");
    if (!d) return false;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string dir = "/sys/class/backlight/" + name;
        const std::string maxs = readFirstLine(dir + "/max_brightness");
        if (maxs.empty()) continue;
        try {
            max_ = std::stoi(maxs);
        } catch (...) {
            continue;
        }
        if (max_ <= 0) continue;
        path_ = dir;
        break;
    }
    ::closedir(d);
    return available();
#else
    return false;
#endif
}

int Backlight::get() const {
    if (!available()) return -1;
    const std::string v = readFirstLine(path_ + "/brightness");
    try {
        return std::stoi(v);
    } catch (...) {
        return -1;
    }
}

int Backlight::percent() const {
    const int v = get();
    if (v < 0 || max_ <= 0) return -1;
    return v * 100 / max_;
}

bool Backlight::setPercent(int percent) {
    if (!available()) return false;
    if (percent < 1) percent = 1;      // never blank the panel entirely
    if (percent > 100) percent = 100;
    // A panel whose max_brightness is small rounds a low percentage down to
    // zero, which is a dark screen with no way back. Keep one step of light.
    const int value = std::max(1, percent * max_ / 100);
    std::ofstream f(path_ + "/brightness");
    if (!f) return false;
    f << value;
    f.close();
    // The driver may clamp; a write only counts if it reads back.
    return get() == value;
}

} // namespace bf
