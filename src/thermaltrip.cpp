#include "thermaltrip.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#endif

namespace bf {
namespace {

std::string readTrimmed(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string value;
    std::getline(f, value);
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
            value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

bool readSwitch(const std::string& path, bool& on) {
    const std::string text = readTrimmed(path);
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || value < 0) return false;
    on = value != 0;
    return true;
}

std::string basenameOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentOf(const std::string& path) {
    if (path.empty() || path == "/") return {};
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

bool internalHubNodeName(const std::string& name) {
    // USB core names a root-hub port device <bus>-<port>. The Cardputer Zero
    // GL852G is wired to root port 1 and was observed as 1-1 on the bench.
    const size_t dash = name.find('-');
    return dash != std::string::npos && name.find('.', dash) == std::string::npos &&
           name.substr(dash + 1) == "1";
}

bool isExtBranch(const std::string& hubName, const std::string& childName) {
    return childName == hubName + ".4";
}

#if defined(__linux__)
std::string resolvePath(const std::string& path) {
    char resolved[PATH_MAX];
    return ::realpath(path.c_str(), resolved) ? std::string(resolved) : std::string();
}
#endif

} // namespace

ThermalTrip::ThermalTrip(ThermalTripPaths paths) : paths_(std::move(paths)) {}

ThermalTripProbe ThermalTrip::probeDevice(const std::string& device) const {
    ThermalTripProbe out;
    out.device = device;
#if !defined(__linux__)
    out.reason = "thermal trip requires Linux sysfs";
    return out;
#else
    const std::string resolvedDevice = resolvePath(device);
    const std::string ttyName = basenameOf(resolvedDevice.empty() ? device : resolvedDevice);
    if (ttyName.empty()) {
        out.reason = "serial device name is unavailable";
        return out;
    }

    std::string cursor = resolvePath(paths_.ttyClass + "/" + ttyName + "/device");
    if (cursor.empty()) {
        out.reason = "serial device has no USB sysfs topology";
        return out;
    }

    std::string usbDevice;
    for (int hop = 0; hop < 12 && !cursor.empty(); ++hop) {
        const std::string vid = readTrimmed(cursor + "/idVendor");
        if (!vid.empty()) {
            usbDevice = cursor;
            out.usbDeviceFound = true;
            out.usbNode = basenameOf(cursor);
            const std::string pid = readTrimmed(cursor + "/idProduct");
            out.usbIdentity = pid.empty() ? vid : vid + ":" + pid;
            break;
        }
        cursor = parentOf(cursor);
    }
    if (usbDevice.empty()) {
        out.reason = "serial sysfs path has no USB device identity";
        return out;
    }

    // Walk from the FC (or its downstream hub) to the board's first/root-port
    // GL852G. Remember the child branch at each hop; only branch 4 reaches EXT.
    std::string child = usbDevice;
    cursor = parentOf(usbDevice);
    for (int hop = 0; hop < 12 && !cursor.empty() && cursor != "/"; ++hop) {
        const std::string vid = readTrimmed(cursor + "/idVendor");
        const std::string pid = readTrimmed(cursor + "/idProduct");
        const std::string hubName = basenameOf(cursor);
        if (vid == "05e3" && pid == "0610" && internalHubNodeName(hubName)) {
            out.extUsb4 = isExtBranch(hubName, basenameOf(child));
            break;
        }
        child = cursor;
        cursor = parentOf(cursor);
    }
    if (!out.extUsb4) {
        out.reason = "USB device is not on the verified EXT USB4 branch";
        return out;
    }

    const std::string muxPath = paths_.ledClass + "/ext_usb_gpio_fun/brightness";
    bool mux = false;
    if (!readSwitch(muxPath, mux)) {
        out.reason = "EXT USB/GPIO selector readback is unavailable";
        return out;
    }
    // Cardputer Zero drives this selector high for hub USB4 and low for the
    // GPIO23/GPIO26 function.
    out.usbMuxSelected = mux;
    if (!out.usbMuxSelected) {
        out.reason = "EXT pins are selected for GPIO, not hub USB4";
        return out;
    }

    const std::string railPath = paths_.ledClass + "/ext_5v_out/brightness";
    if (!readSwitch(railPath, out.ext5vOn)) {
        out.reason = "EXT 5V rail readback is unavailable";
        return out;
    }
    if (!out.ext5vOn) {
        out.reason = "EXT 5V rail is already off; another supply may power the FC";
        return out;
    }
    out.ext5vWritable = ::access(railPath.c_str(), W_OK) == 0;
    if (!out.ext5vWritable) {
        out.reason = "EXT 5V rail is not writable by this user";
        return out;
    }

    out.eligible = true;
    out.reason = "verified GL852G USB4 route, USB mux, and switched EXT 5V rail";
    return out;
#endif
}

const ThermalTripProbe& ThermalTrip::inspect(const std::string& device) {
    reset();
    boundDevice_ = device;
    probe_ = probeDevice(device);
    return probe_;
}

bool ThermalTrip::arm(std::string& error) {
    if (boundDevice_.empty()) {
        error = "no serial device is bound";
        return false;
    }
    const ThermalTripProbe current = probeDevice(boundDevice_);
    if (!current.eligible || current.usbNode != probe_.usbNode ||
        current.usbIdentity != probe_.usbIdentity) {
        probe_ = current;
        error = current.reason.empty() ? "EXT USB4 route changed before arming" : current.reason;
        return false;
    }
    probe_ = current;
    armed_ = true;
    latched_ = false;
    error.clear();
    return true;
}

void ThermalTrip::decline() { armed_ = false; }

void ThermalTrip::reset() {
    probe_ = ThermalTripProbe{};
    boundDevice_.clear();
    armed_ = false;
    latched_ = false;
}

bool ThermalTrip::cutPower(std::string& error) {
#if !defined(__linux__)
    error = "thermal trip requires Linux sysfs";
    return false;
#else
    if (!armed_) {
        error = "thermal trip is not armed";
        return false;
    }
    // Consume the arm before touching the rail. A failed write demands manual
    // unplugging; it must not become an unbounded write loop.
    armed_ = false;
    const ThermalTripProbe current = probeDevice(boundDevice_);
    if (!current.eligible || current.usbNode != probe_.usbNode ||
        current.usbIdentity != probe_.usbIdentity) {
        probe_ = current;
        error = current.reason.empty() ? "EXT USB4 route changed before cutoff" : current.reason;
        return false;
    }
    probe_ = current;

    const std::string railPath = paths_.ledClass + "/ext_5v_out/brightness";
    const int fd = ::open(railPath.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        error = railPath + ": " + std::strerror(errno);
        return false;
    }
    const char off = '0';
    const ssize_t written = ::write(fd, &off, 1);
    const int writeError = written == 1 ? 0 : (written < 0 ? errno : EIO);
    if (::close(fd) != 0 && writeError == 0) {
        error = railPath + ": close failed: " + std::strerror(errno);
        return false;
    }
    if (writeError != 0) {
        error = railPath + ": write failed: " + std::strerror(writeError);
        return false;
    }

    bool on = true;
    if (!readSwitch(railPath, on)) {
        error = "EXT 5V cutoff was written but readback failed";
        return false;
    }
    if (on) {
        error = "EXT 5V cutoff readback still reports on";
        return false;
    }
    probe_.ext5vOn = false;
    latched_ = true;
    error.clear();
    return true;
#endif
}

} // namespace bf
