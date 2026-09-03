#include "battery.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace bf {
namespace {

std::string readTrimmed(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string v;
    std::getline(f, v);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
}

// Every numeric attribute is optional and any of them may be an empty file on a
// gauge that has not finished its first conversion. `fallback` is what the UI
// gets told when the kernel will not commit.
long readNumber(const std::string& path, long fallback) {
    const std::string v = readTrimmed(path);
    if (v.empty()) return fallback;
    try {
        return std::stol(v);
    } catch (...) {
        return fallback;
    }
}

std::string hoursMinutes(int seconds) {
    if (seconds <= 0) return {};
    const int minutes = seconds / 60;
    if (minutes < 60) return std::to_string(minutes) + "m";
    return std::to_string(minutes / 60) + "h" + std::to_string(minutes % 60) + "m";
}

std::string volts(int milliVolts) {
    if (milliVolts <= 0) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d.%02dV", milliVolts / 1000, (milliVolts % 1000) / 10);
    return buf;
}

} // namespace

std::string BatteryReading::shortText() const {
    if (!present) return {};
    if (percent < 0) return charging ? "+?" : "?";
    return (charging ? "+" : "") + std::to_string(percent) + "%";
}

std::string BatteryReading::detailText() const {
    if (!present) return "no pack detected";
    std::string out = percent >= 0 ? std::to_string(percent) + "%" : "level unknown";
    if (!state.empty()) out += " " + state;
    const std::string v = volts(milliVolts);
    if (!v.empty()) out += ", " + v;
    // Time-to-empty is the gauge's own estimate and only exists while the pack
    // is actually being drained; a charging estimate is a different register
    // this app does not pretend to have.
    const std::string left = charging ? std::string() : hoursMinutes(secondsToEmpty);
    if (!left.empty()) out += ", " + left + " left";
    return out;
}

bool Battery::discover() {
    const char* env = ::getenv("BFCLI_POWER_SUPPLY_DIR");
    return discoverIn((env && *env) ? env : "/sys/class/power_supply");
}

bool Battery::discoverIn(const std::string& root) {
    path_.clear();
    reading_ = BatteryReading{};
    nextPollMs_ = 0;
#if defined(__linux__)
    DIR* d = ::opendir(root.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        names.push_back(name);
    }
    ::closedir(d);
    // readdir order is whatever the filesystem feels like. A machine with two
    // supplies must pick the same one across launches, or the indicator would
    // change meaning at random.
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::string dir = root + "/" + name;
        if (readTrimmed(dir + "/type") != "Battery") continue;
        // A mains adapter and a UPS both live in this class; only a supply that
        // can report a charge level is something to draw a battery for.
        if (readTrimmed(dir + "/capacity").empty() &&
            readTrimmed(dir + "/capacity_level").empty()) {
            continue;
        }
        path_ = dir;
        break;
    }
#else
    (void)root;
#endif
    return available();
}

BatteryReading Battery::read() const {
    BatteryReading r;
    if (!available()) return r;

    // `present` is optional; a gauge that does not publish it is taken at its
    // word that the pack it is reporting on exists.
    const std::string presentText = readTrimmed(path_ + "/present");
    r.present = presentText.empty() || presentText != "0";
    if (!r.present) return r;

    const long capacity = readNumber(path_ + "/capacity", -1);
    if (capacity >= 0) r.percent = static_cast<int>(std::min(capacity, 100L));

    r.state = readTrimmed(path_ + "/status");
    r.health = readTrimmed(path_ + "/health");
    r.charging = (r.state == "Charging");
    r.full = (r.state == "Full");
    // A full pack still sitting on the cable is not discharging, and drawing it
    // as if it were is the one reading an operator would act on wrongly.
    if (r.full && r.percent < 0) r.percent = 100;

    const long uV = readNumber(path_ + "/voltage_now", 0);
    r.milliVolts = static_cast<int>(uV / 1000);
    const long uA = readNumber(path_ + "/current_now", 0);
    r.milliAmps = static_cast<int>(uA / 1000);
    r.secondsToEmpty = static_cast<int>(readNumber(path_ + "/time_to_empty_now", 0));
    if (r.secondsToEmpty < 0) r.secondsToEmpty = 0;
    return r;
}

bool Battery::poll(uint64_t nowMs) {
    if (!available()) return false;
    if (nextPollMs_ != 0 && nowMs < nextPollMs_) return false;
    nextPollMs_ = nowMs + kPollIntervalMs;

    const BatteryReading fresh = read();
    const bool drawn = fresh.present != reading_.present ||
                       fresh.percent != reading_.percent ||
                       fresh.charging != reading_.charging ||
                       fresh.full != reading_.full;
    reading_ = fresh;
    return drawn;
}

} // namespace bf
