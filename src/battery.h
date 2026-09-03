#pragma once
#include <cstdint>
#include <string>

namespace bf {

// The Cardputer Zero carries a TI BQ27220 fuel gauge, which the kernel already
// presents as an ordinary power_supply class device. Nothing below is specific
// to that part: the first supply whose `type` reads Battery wins, and every
// attribute is optional, because a gauge that cannot answer is not the same
// thing as a gauge that is not there.
struct BatteryReading {
    bool present = false;
    int percent = -1;         // -1 when the gauge reports no capacity
    bool charging = false;
    bool full = false;
    int milliVolts = 0;       // 0 when unknown
    int milliAmps = 0;        // signed: negative while discharging
    int secondsToEmpty = 0;   // 0 when unknown
    std::string state;        // the driver's own word: Charging, Discharging...
    std::string health;

    bool known() const { return present && percent >= 0; }
    // What the top bar prints beside the pictogram: "96%", "+96%" on the cable,
    // "?" for a gauge that is present but silent about capacity.
    std::string shortText() const;
    // One line for the menu and the About screen.
    std::string detailText() const;
};

class Battery {
public:
    // Walks /sys/class/power_supply, or BFCLI_POWER_SUPPLY_DIR when set, for
    // the first Battery-type supply. Absence is a normal outcome: the host
    // build and any bench Pi without a pack simply draw no indicator.
    bool discover();
    bool discoverIn(const std::string& root);
    bool available() const { return !path_.empty(); }
    const std::string& path() const { return path_; }

    // Re-reads sysfs at most every kPollIntervalMs. Returns true only when
    // something the UI actually draws changed, so a resting pack costs no
    // repaints -- voltage and current jitter every read and are deliberately
    // not part of that comparison.
    bool poll(uint64_t nowMs);
    BatteryReading read() const;
    const BatteryReading& reading() const { return reading_; }

    static constexpr uint64_t kPollIntervalMs = 5000;

private:
    std::string path_;
    BatteryReading reading_;
    uint64_t nextPollMs_ = 0;
};

} // namespace bf
