#pragma once

#include <string>

namespace bf {

// The trip is intentionally tied to Cardputer Zero's internal GL852G hub and
// EXT USB4 branch. A serial device on USB-A, Grove UART, or an unknown topology
// is never treated as power-controllable.
struct ThermalTripPaths {
    std::string ttyClass = "/sys/class/tty";
    std::string ledClass = "/sys/class/leds";
};

struct ThermalTripProbe {
    bool usbDeviceFound = false;
    bool extUsb4 = false;
    bool usbMuxSelected = false;
    bool ext5vOn = false;
    bool ext5vWritable = false;
    bool eligible = false;
    std::string device;
    std::string usbNode;
    std::string usbIdentity;
    std::string reason;
};

class ThermalTrip {
public:
    explicit ThermalTrip(ThermalTripPaths paths = {});

    // Inspection and arming never change GPIO state. Arming rechecks the exact
    // topology and both switch states so a stale picker entry cannot qualify.
    const ThermalTripProbe& inspect(const std::string& device);
    bool arm(std::string& error);
    void decline();
    void reset();

    // One-way for the lifetime of this binding: write EXT5V off, verify
    // readback, and latch. There is deliberately no automatic re-enable API.
    bool cutPower(std::string& error);

    const ThermalTripProbe& probe() const { return probe_; }
    bool armed() const { return armed_; }
    bool latched() const { return latched_; }

private:
    ThermalTripProbe probeDevice(const std::string& device) const;

    ThermalTripPaths paths_;
    ThermalTripProbe probe_;
    std::string boundDevice_;
    bool armed_ = false;
    bool latched_ = false;
};

} // namespace bf
