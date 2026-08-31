#include "serialport.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#endif

namespace bf {

const int kBaudChoices[] = {115200, 57600, 38400, 19200, 9600, 230400, 250000, 460800, 921600};
const int kBaudChoiceCount = static_cast<int>(sizeof(kBaudChoices) / sizeof(kBaudChoices[0]));

namespace {

std::string readTrimmed(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string v;
    std::getline(f, v);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

#if defined(__linux__)
std::string resolve(const std::string& path) {
    char buf[PATH_MAX];
    const char* r = ::realpath(path.c_str(), buf);
    return r ? std::string(r) : path;
}

std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// Walk up from /sys/class/tty/<tty> to the USB device that owns it and read the
// identity attributes. Absence is reported as empty, never guessed at.
void fillUsbIdentity(PortInfo& p) {
    const std::string base = p.device.substr(p.device.find_last_of('/') + 1);
    std::string dir = "/sys/class/tty/" + base + "/device";
    for (int hop = 0; hop < 6; ++hop) {
        const std::string vid = readTrimmed(dir + "/idVendor");
        if (!vid.empty()) {
            p.vendorId = vid;
            p.productId = readTrimmed(dir + "/idProduct");
            p.product = readTrimmed(dir + "/product");
            p.manufacturer = readTrimmed(dir + "/manufacturer");
            return;
        }
        dir += "/..";
    }
}

speed_t baudConstant(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
#ifdef B250000
    case 250000: return B250000;
#endif
    default: return B115200;
    }
}
#endif // __linux__

} // namespace

std::string PortInfo::vidPid() const {
    if (vendorId.empty() || productId.empty()) return {};
    return vendorId + ":" + productId;
}

std::string PortInfo::label() const {
    const std::string base = device.substr(device.find_last_of('/') + 1);
    if (!product.empty()) return base + "  " + product;
    if (kind == "uart") return base + "  Grove/EXT UART";
    return base;
}

std::string PortInfo::detail() const {
    std::string d;
    const std::string vp = vidPid();
    if (!vp.empty()) d += vp;
    if (!manufacturer.empty()) d += (d.empty() ? "" : "  ") + manufacturer;
    if (isDfuId(vendorId, productId)) d += "  [DFU - no CLI]";
    else if (looksLikeFlightController()) d += "  [flight controller]";
    return d;
}

bool isKnownFcId(const std::string& vid, const std::string& pid) {
    const std::string v = lower(vid), p = lower(pid);
    // STM32 virtual COM port: the overwhelming majority of Betaflight targets.
    if (v == "0483" && p == "5740") return true;
    // AT32F435 targets ship the same CDC descriptor under Artery's VID.
    if (v == "2e3c" && p == "5740") return true;
    // Some GD32/APM32 clones enumerate here.
    if (v == "1209" && p == "5741") return true;
    return false;
}

bool isDfuId(const std::string& vid, const std::string& pid) {
    const std::string v = lower(vid), p = lower(pid);
    return (v == "0483" && p == "df11") || (v == "2e3c" && p == "df11");
}

std::vector<PortInfo> enumeratePorts() {
    std::vector<PortInfo> out;
#if defined(__linux__)
    // /dev/serial0 is a symlink to /dev/ttyAMA0 or /dev/ttyS0 on this board,
    // so compare where the names actually point: the picker should offer the
    // Grove UART once, not two or three times.
    std::vector<std::string> seen;
    auto addDevice = [&out, &seen](const std::string& dev, const std::string& kind) {
        struct stat st{};
        if (::stat(dev.c_str(), &st) != 0) return;
        const std::string real = resolve(dev);
        for (const std::string& s : seen) {
            if (s == real) return;
        }
        seen.push_back(real);
        PortInfo p;
        p.device = dev;
        p.kind = kind;
        if (kind == "usb") fillUsbIdentity(p);
        out.push_back(std::move(p));
    };

    // by-id first: the symlink name carries vendor/product/serial text and is
    // the only stable handle across replug.
    std::vector<std::pair<std::string, std::string>> byId;
    for (const std::string& name : listDir("/dev/serial/by-id")) {
        const std::string link = "/dev/serial/by-id/" + name;
        byId.emplace_back(resolve(link), link);
    }

    for (const std::string& name : listDir("/dev")) {
        if (name.rfind("ttyACM", 0) == 0 || name.rfind("ttyUSB", 0) == 0) {
            addDevice("/dev/" + name, "usb");
        }
    }
    // The Grove / EXT header UART, when the pinmux exposes it.
    for (const char* uart : {"/dev/serial0", "/dev/ttyAMA0", "/dev/ttyS0"}) {
        addDevice(uart, "uart");
    }

    for (PortInfo& p : out) {
        for (const auto& pair : byId) {
            if (pair.first == p.device) {
                p.byId = pair.second;
                break;
            }
        }
        if (isKnownFcId(p.vendorId, p.productId)) p.score = 100;
        else if (isDfuId(p.vendorId, p.productId)) p.score = 20;
        else if (lower(p.product).find("betaflight") != std::string::npos) p.score = 90;
        else if (p.device.rfind("/dev/ttyACM", 0) == 0) p.score = 70;
        else if (p.kind == "usb") p.score = 40;
        else p.score = 10;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const PortInfo& a, const PortInfo& b) { return a.score > b.score; });
#endif
    return out;
}

SerialPort::~SerialPort() { close(); }

bool SerialPort::open(const std::string& device, int baud, std::string& error) {
    close();
#if defined(__linux__)
    // O_NOCTTY so a serial console never becomes our controlling terminal;
    // O_NONBLOCK so a port with no carrier cannot stall the UI thread.
    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        error = device + ": " + std::strerror(errno);
        if (errno == EACCES) error += " (add the user to the dialout group)";
        return false;
    }

    termios tio{};
    if (::tcgetattr(fd_, &tio) != 0) {
        error = std::string("tcgetattr: ") + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    ::cfmakeraw(&tio);
    // 8N1, no flow control, ignore modem lines. CLOCAL matters: without it a
    // port with no DCD blocks. HUPCL is cleared so closing does not pulse DTR,
    // which some targets read as a reset request.
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | CRTSCTS | HUPCL | PARENB);
    tio.c_cflag = (tio.c_cflag & ~static_cast<tcflag_t>(CSIZE)) | CS8;
    tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    // Never negotiate 1200 baud: Betaflight treats that as "reboot to DFU".
    const int safeBaud = (baud == 1200) ? 115200 : baud;
    const speed_t sp = baudConstant(safeBaud);
    ::cfsetispeed(&tio, sp);
    ::cfsetospeed(&tio, sp);

    if (::tcsetattr(fd_, TCSANOW, &tio) != 0) {
        error = std::string("tcsetattr: ") + std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    ::tcflush(fd_, TCIOFLUSH);
    device_ = device;
    outBuf_.clear();
    hangup_ = false;
    return true;
#else
    (void)baud;
    error = "serial ports are only implemented on Linux";
    return false;
#endif
}

void SerialPort::close() {
#if defined(__linux__)
    if (fd_ >= 0) ::close(fd_);
#endif
    fd_ = -1;
    device_.clear();
    outBuf_.clear();
}

int SerialPort::read(std::string& out) {
#if defined(__linux__)
    if (fd_ < 0) return -1;
    char buf[2048];
    int total = 0;
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            total += static_cast<int>(n);
            if (n < static_cast<ssize_t>(sizeof(buf))) break;
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        hangup_ = true;   // EIO/ENXIO: the FC was unplugged mid-session
        return -1;
    }
    return total;
#else
    (void)out;
    return -1;
#endif
}

void SerialPort::write(const std::string& data) { outBuf_ += data; }

bool SerialPort::flush(std::string& error) {
#if defined(__linux__)
    if (fd_ < 0) return false;
    while (!outBuf_.empty()) {
        const ssize_t n = ::write(fd_, outBuf_.data(), outBuf_.size());
        if (n > 0) {
            outBuf_.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;  // retry next tick
        if (n < 0 && errno == EINTR) continue;
        error = std::string("write: ") + std::strerror(errno);
        hangup_ = true;
        return false;
    }
    return true;
#else
    (void)error;
    return false;
#endif
}

} // namespace bf
