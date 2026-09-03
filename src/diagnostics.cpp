#include "diagnostics.h"

#include "bfsession.h"
#include "strutil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace bf {
namespace {

bool commandFailed(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (isErrorLine(line)) return true;
    }
    return false;
}

bool numberAfter(const std::string& text, const std::string& marker, double& value) {
    const std::string foldedText = upper(text);
    const std::string foldedMarker = upper(marker);
    const size_t found = foldedText.find(foldedMarker);
    if (found == std::string::npos) return false;
    const char* p = text.c_str() + found + marker.size();
    while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
    char* end = nullptr;
    const double parsed = std::strtod(p, &end);
    if (end == p || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

std::string compactNumber(double value, int decimals = 0) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::istringstream in(text);
    std::string word;
    while (in >> word) {
        word = upper(word);
        if (seen.insert(word).second) out.push_back(word);
    }
    return out;
}

std::string valueAfterLabel(const std::string& text, const std::string& label,
                            const std::vector<std::string>& followingLabels) {
    const std::string folded = upper(text);
    const size_t labelAt = folded.find(upper(label));
    if (labelAt == std::string::npos) return {};

    const size_t valueAt = labelAt + label.size();
    size_t valueEnd = text.size();
    for (const std::string& following : followingLabels) {
        const size_t found = folded.find(upper(following), valueAt);
        if (found != std::string::npos) valueEnd = std::min(valueEnd, found);
    }
    std::string value = trim(text.substr(valueAt, valueEnd - valueAt));
    while (!value.empty() && (value.back() == ',' || value.back() == ';')) value.pop_back();
    return trim(value);
}

bool gyroDetailIsPresent(const std::string& detail) {
    const std::string folded = upper(detail);
    return !detail.empty() && folded != "NONE" &&
           folded.find("NOT DETECTED") == std::string::npos;
}

bool hasFlag(const std::vector<std::string>& flags, const std::string& wanted) {
    return std::find(flags.begin(), flags.end(), wanted) != flags.end();
}

bool expectedHostFlag(const std::string& flag) {
    return flag == "CLI" || flag == "MSP";
}

struct FlagMeaning {
    DiagnosticLevel level;
    const char* action;
};

FlagMeaning meaningForFlag(const std::string& flag) {
    static const std::map<std::string, FlagMeaning> meanings = {
        {"NOGYRO",       {DiagnosticLevel::Failure, "no gyro detected; do not fly"}},
        {"FAILSAFE",     {DiagnosticLevel::Failure, "failsafe is active; restore the RX link"}},
        {"RXLOSS",       {DiagnosticLevel::Failure, "receiver link is missing"}},
        {"BADRX",        {DiagnosticLevel::Failure, "receiver signal is invalid; cycle ARM off"}},
        {"NOT_DISARMED", {DiagnosticLevel::Failure, "FC still reports an armed state"}},
        {"BOXFAILSAFE",  {DiagnosticLevel::Failure, "failsafe switch is active"}},
        {"RUNAWAY",      {DiagnosticLevel::Failure, "runaway protection is latched; inspect craft"}},
        {"RUNAWAY_TAKEOFF", {DiagnosticLevel::Failure, "runaway protection is latched; inspect craft"}},
        {"CRASH",        {DiagnosticLevel::Failure, "crash detection is latched; inspect craft"}},
        {"CRASH_DETECTED", {DiagnosticLevel::Failure, "crash detection is latched; inspect craft"}},
        {"THROTTLE",     {DiagnosticLevel::Warning, "lower throttle"}},
        {"ANGLE",        {DiagnosticLevel::Warning, "place the craft level"}},
        {"BOOTGRACE",    {DiagnosticLevel::Warning, "wait for boot grace to finish"}},
        {"NOPREARM",     {DiagnosticLevel::Warning, "activate PREARM before ARM"}},
        {"LOAD",         {DiagnosticLevel::Failure, "FC scheduler load is blocking arming"}},
        {"CALIB",        {DiagnosticLevel::Warning, "keep the craft still while sensors calibrate"}},
        {"CMS",          {DiagnosticLevel::Warning, "exit the on-screen configuration menu"}},
        {"BST",          {DiagnosticLevel::Warning, "BST host activity is blocking arming"}},
        {"PARALYZE",     {DiagnosticLevel::Failure, "PARALYZE mode is active"}},
        {"GPS",          {DiagnosticLevel::Warning, "GPS arming conditions are not met"}},
        {"RESCUE_SW",    {DiagnosticLevel::Warning, "GPS rescue switch is active"}},
        {"DSHOT_TELEM",  {DiagnosticLevel::Failure, "DSHOT telemetry is not healthy"}},
        {"RPMFILTER",     {DiagnosticLevel::Failure, "RPM filter lacks valid motor telemetry"}},
        {"REBOOT_REQD",  {DiagnosticLevel::Warning, "save and reboot before arming"}},
        {"REBOOT_REQ",   {DiagnosticLevel::Warning, "save and reboot before arming"}},
        {"DSHOT_BBANG",  {DiagnosticLevel::Failure, "DSHOT bitbang setup blocks arming"}},
        {"NO_ACC_CAL",   {DiagnosticLevel::Warning, "accelerometer needs calibration"}},
        {"ACC_CALIBRATION", {DiagnosticLevel::Warning, "accelerometer needs calibration"}},
        {"MOTOR_PROTO",  {DiagnosticLevel::Failure, "motor protocol is unavailable"}},
        {"MOTOR_PROTOCOL", {DiagnosticLevel::Failure, "motor protocol is unavailable"}},
        {"FLIP_SWITCH",  {DiagnosticLevel::Warning, "crash-flip switch is active"}},
        {"CRASHFLIP",    {DiagnosticLevel::Warning, "crash-flip switch is active"}},
        {"ALT_HOLD_SW",  {DiagnosticLevel::Warning, "altitude-hold switch is active"}},
        {"ALTHOLD",      {DiagnosticLevel::Warning, "altitude-hold switch is active"}},
        {"POS_HOLD_SW",  {DiagnosticLevel::Warning, "position-hold switch is active"}},
        {"POSHOLD",      {DiagnosticLevel::Warning, "position-hold switch is active"}},
        {"AUTOPILOT_SW", {DiagnosticLevel::Warning, "autopilot switch is active"}},
        {"AUTOPILOT",    {DiagnosticLevel::Warning, "autopilot switch is active"}},
        {"ARM_SWITCH",   {DiagnosticLevel::Warning, "move ARM switch off, then retry"}},
    };
    const auto it = meanings.find(flag);
    if (it != meanings.end()) return it->second;
    return {DiagnosticLevel::Warning, "unrecognized blocker; inspect raw status"};
}

void add(DiagnosticReport& report, DiagnosticLevel level,
         const std::string& title, const std::string& detail) {
    report.findings.push_back(DiagnosticFinding{level, title, detail});
}

struct StatusEvidence {
    bool armingSeen = false;
    bool gyroSeen = false;
    bool gyroPresent = false;
    std::string gyroDetail;
    bool batterySeen = false;
    std::string batteryDetail;
    std::string batteryState;
    bool i2cSeen = false;
    int i2cErrors = 0;
    bool cpuSeen = false;
    double cpuLoad = 0;
    bool cycleSeen = false;
    double cycleUs = 0;
    bool gyroRateSeen = false;
    double gyroRate = 0;
    bool rxRateSeen = false;
    double rxRate = 0;
    bool configSeen = false;
    std::string configState;
    bool gpsSeen = false;
    std::string gpsDetail;
    bool coreTemperatureSeen = false;
    int coreTemperatureC = 0;
};

StatusEvidence parseStatus(const std::string& text, DiagnosticReport& report) {
    StatusEvidence e;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        const std::string u = upper(t);
        int temperatureC = 0;
        if (parseCoreTemperatureC(t, temperatureC)) {
            e.coreTemperatureSeen = true;
            e.coreTemperatureC = temperatureC;
            report.coreTemperatureAvailable = true;
            report.coreTemperatureC = temperatureC;
        }
        if (startsWith(u, "ARMING DISABLE FLAGS:")) {
            e.armingSeen = true;
            report.armingFlags = words(trim(t.substr(t.find(':') + 1)));
        } else if (startsWith(u, "GYROS DETECTED:")) {
            e.gyroSeen = true;
            e.gyroDetail = trim(t.substr(t.find(':') + 1));
            e.gyroPresent = gyroDetailIsPresent(e.gyroDetail);
        } else if (!e.gyroSeen && u.find("GYRO:") != std::string::npos) {
            // Current Betaflight can put the gyro inventory on the shared
            // `DEVICES DETECTED:` line before ACC/BARO/MAG fields.
            e.gyroDetail = valueAfterLabel(
                t, "GYRO:", {"ACC:", "BARO:", "MAG:", "RANGEFINDER:", "OSD:"});
            e.gyroSeen = true;
            e.gyroPresent = gyroDetailIsPresent(e.gyroDetail);
        } else if (!e.gyroSeen && u.find("GYRO=") != std::string::npos) {
            const size_t start = u.find("GYRO=") + 5;
            const size_t end = t.find(',', start);
            e.gyroDetail = trim(t.substr(start, end == std::string::npos ? end : end - start));
            e.gyroSeen = true;
            e.gyroPresent = gyroDetailIsPresent(e.gyroDetail);
        }

        if (startsWith(u, "VOLTAGE:")) {
            e.batterySeen = true;
            e.batteryDetail = trim(t.substr(t.find(':') + 1));
            const size_t dash = e.batteryDetail.rfind(" - ");
            const size_t close = e.batteryDetail.rfind(')');
            if (dash != std::string::npos) {
                e.batteryState = upper(trim(e.batteryDetail.substr(
                    dash + 3, close == std::string::npos ? close : close - dash - 3)));
            }
        } else if (startsWith(u, "I2C ERRORS:")) {
            double value = 0;
            if (numberAfter(t, ":", value)) {
                e.i2cSeen = true;
                e.i2cErrors = static_cast<int>(value);
            }
        } else if (startsWith(u, "DEVICES DETECTED:") && u.find("I2C=") != std::string::npos) {
            const size_t i2c = u.find("I2C=");
            const size_t open = u.find('(', i2c);
            double value = 0;
            if (open != std::string::npos && numberAfter(t.substr(open), "(", value)) {
                e.i2cSeen = true;
                e.i2cErrors = static_cast<int>(value);
            }
        }

        double value = 0;
        if (startsWith(u, "CPU:") && numberAfter(t, ":", value)) {
            e.cpuSeen = true;
            e.cpuLoad = value;
        } else if (u.find("SYSTEM LOAD:") != std::string::npos &&
                   numberAfter(t, "System load:", value)) {
            e.cpuSeen = true;
            e.cpuLoad = value;
        }
        if (u.find("CYCLE TIME:") != std::string::npos && numberAfter(t, "cycle time:", value)) {
            e.cycleSeen = true;
            e.cycleUs = value;
        }
        if (u.find("GYRO RATE:") != std::string::npos && numberAfter(t, "GYRO rate:", value)) {
            e.gyroRateSeen = true;
            e.gyroRate = value;
        }
        if (u.find("RX RATE:") != std::string::npos && numberAfter(t, "RX rate:", value)) {
            e.rxRateSeen = true;
            e.rxRate = value;
        }

        if (startsWith(u, "CONFIG:")) {
            e.configSeen = true;
            const std::string rest = trim(t.substr(t.find(':') + 1));
            e.configState = upper(rest.substr(0, rest.find_first_of(" (,")));
        } else if (startsWith(u, "CONFIGURATION:")) {
            e.configSeen = true;
            const std::string rest = trim(t.substr(t.find(':') + 1));
            e.configState = upper(rest.substr(0, rest.find_first_of(" (,")));
        } else if (startsWith(u, "GPS:")) {
            e.gpsSeen = true;
            e.gpsDetail = trim(t.substr(t.find(':') + 1));
        }
    }
    return e;
}

bool parseTasksLoad(const std::string& text, double& load) {
    std::istringstream in(text);
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        const std::string u = upper(t);
        if (!startsWith(u, "TOTAL")) continue;
        const size_t pct = t.rfind('%');
        if (pct == std::string::npos) continue;
        size_t start = pct;
        while (start > 0) {
            const char c = t[start - 1];
            if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) break;
            --start;
        }
        if (start == pct) continue;
        char* end = nullptr;
        const double value = std::strtod(t.c_str() + start, &end);
        if (end == t.c_str() + pct) {
            load = value;
            found = true;
        }
    }
    return found;
}

std::string firstVersionLine(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.rfind("# ", 0) == 0) t = trim(t.substr(2));
        if (t.find("Betaflight") != std::string::npos) return t;
    }
    return {};
}

} // namespace

bool parseCoreTemperatureC(const std::string& text, int& temperatureC) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string u = upper(line);
        const size_t label = u.find("CORE TEMP");
        if (label == std::string::npos) continue;
        const size_t separator = line.find_first_of("=:", label + 9);
        if (separator == std::string::npos) continue;
        const char* start = line.c_str() + separator + 1;
        char* end = nullptr;
        const double value = std::strtod(start, &end);
        if (end == start || !std::isfinite(value) || value < -100.0 || value > 250.0) continue;
        temperatureC = static_cast<int>(std::lround(value));
        return true;
    }
    return false;
}

int DiagnosticReport::failureCount() const {
    return static_cast<int>(std::count_if(findings.begin(), findings.end(),
        [](const DiagnosticFinding& f) { return f.level == DiagnosticLevel::Failure; }));
}

int DiagnosticReport::warningCount() const {
    return static_cast<int>(std::count_if(findings.begin(), findings.end(),
        [](const DiagnosticFinding& f) {
            return f.level == DiagnosticLevel::Warning || f.level == DiagnosticLevel::Unknown;
        }));
}

int DiagnosticReport::actionableBlockerCount() const {
    return static_cast<int>(std::count_if(armingFlags.begin(), armingFlags.end(),
        [](const std::string& flag) { return !expectedHostFlag(flag); }));
}

const char* diagnosticLevelName(DiagnosticLevel level) {
    switch (level) {
    case DiagnosticLevel::Pass:    return "PASS";
    case DiagnosticLevel::Info:    return "INFO";
    case DiagnosticLevel::Warning: return "WARN";
    case DiagnosticLevel::Failure: return "FAIL";
    case DiagnosticLevel::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

DiagnosticReport buildDiagnosticReport(const std::string& statusText,
                                       const std::string& tasksText,
                                       const std::string& versionText) {
    DiagnosticReport report;
    report.statusAvailable = !statusText.empty() && !commandFailed(statusText);
    report.tasksAvailable = !tasksText.empty() && !commandFailed(tasksText);
    report.versionAvailable = !versionText.empty() && !commandFailed(versionText);

    StatusEvidence status;
    if (report.statusAvailable) status = parseStatus(statusText, report);

    if (!report.statusAvailable) {
        add(report, DiagnosticLevel::Unknown, "Status", "no complete FC status response");
    } else if (!status.armingSeen) {
        add(report, DiagnosticLevel::Unknown, "Arming", "disable flags were not reported");
    } else {
        int actionable = 0;
        for (const std::string& flag : report.armingFlags) {
            if (expectedHostFlag(flag)) continue;
            ++actionable;
            const FlagMeaning meaning = meaningForFlag(flag);
            add(report, meaning.level, "ARM " + flag, meaning.action);
        }
        if (actionable == 0) {
            const bool hostBlocked = hasFlag(report.armingFlags, "CLI") || hasFlag(report.armingFlags, "MSP");
            add(report, DiagnosticLevel::Pass, "Arming",
                hostBlocked ? "only CLI/MSP host blockers are active" : "no disable flags reported");
        }
    }

    if (status.gyroSeen) {
        add(report, status.gyroPresent ? DiagnosticLevel::Pass : DiagnosticLevel::Failure,
            "Gyro", status.gyroPresent ? status.gyroDetail : "no gyro detected; do not fly");
    } else if (!hasFlag(report.armingFlags, "NOGYRO")) {
        add(report, DiagnosticLevel::Unknown, "Gyro", "detection evidence unavailable");
    }

    if (status.rxRateSeen && !(status.rxRate == 0 && hasFlag(report.armingFlags, "RXLOSS"))) {
        add(report, status.rxRate > 0 ? DiagnosticLevel::Pass : DiagnosticLevel::Warning,
            "RX", compactNumber(status.rxRate) + " Hz reported");
    }

    if (status.batterySeen) {
        DiagnosticLevel level = DiagnosticLevel::Info;
        if (status.batteryState == "OK") level = DiagnosticLevel::Pass;
        else if (status.batteryState.find("CRITICAL") != std::string::npos) level = DiagnosticLevel::Failure;
        else if (!status.batteryState.empty() && status.batteryState.find("NOT PRESENT") == std::string::npos) {
            level = DiagnosticLevel::Warning;
        }
        add(report, level, "Battery", status.batteryDetail);
    } else if (report.statusAvailable) {
        add(report, DiagnosticLevel::Unknown, "Battery", "voltage state was not reported");
    }

    if (status.i2cSeen) {
        add(report, status.i2cErrors == 0 ? DiagnosticLevel::Pass : DiagnosticLevel::Warning,
            "I2C", status.i2cErrors == 0
                       ? "0 errors since boot"
                       : std::to_string(status.i2cErrors) + " cumulative errors since boot");
    }

    if (status.coreTemperatureSeen) {
        DiagnosticLevel level = DiagnosticLevel::Pass;
        std::string detail = std::to_string(status.coreTemperatureC) + "C MCU core";
        if (status.coreTemperatureC >= 80) {
            level = DiagnosticLevel::Failure;
            detail += "; critical heat - unplug USB/battery power and cool the stack";
        } else if (status.coreTemperatureC >= kDefaultCoreTemperatureAlarmC) {
            level = DiagnosticLevel::Warning;
            detail += "; at/above Betaflight's 70C default alarm";
        } else {
            detail += "; below Betaflight's 70C default alarm";
        }
        add(report, level, "Core temp", detail);
    }

    if (status.configSeen && status.configState != "CONFIGURED") {
        add(report, DiagnosticLevel::Warning, "Config", status.configState + " reported by FC");
    }

    if (status.gpsSeen) {
        const std::string gpsUpper = upper(status.gpsDetail);
        if (gpsUpper.find("NOT ENABLED") == std::string::npos) {
            const bool problem = gpsUpper.find("NOT CONNECTED") != std::string::npos ||
                                 gpsUpper.find("NO PORT") != std::string::npos ||
                                 gpsUpper.find("NOT CONFIGURED") != std::string::npos;
            add(report, problem ? DiagnosticLevel::Warning : DiagnosticLevel::Info,
                "GPS", status.gpsDetail);
        }
    }

    double tasksLoad = 0;
    const bool tasksLoadSeen = report.tasksAvailable && parseTasksLoad(tasksText, tasksLoad);
    if (!report.tasksAvailable) {
        add(report, DiagnosticLevel::Unknown, "Tasks", "no complete scheduler response");
    }
    if (status.cpuSeen || status.cycleSeen || status.gyroRateSeen || tasksLoadSeen) {
        std::string detail;
        if (status.cpuSeen) detail += "CPU " + compactNumber(status.cpuLoad) + "%";
        if (tasksLoadSeen) detail += (detail.empty() ? "" : ", ") +
                                     std::string("tasks ") + compactNumber(tasksLoad, 1) + "%";
        if (status.cycleSeen) detail += (detail.empty() ? "" : ", ") +
                                       compactNumber(status.cycleUs) + "us loop";
        if (status.gyroRateSeen) detail += (detail.empty() ? "" : ", ") +
                                          compactNumber(status.gyroRate) + "Hz gyro";
        add(report, DiagnosticLevel::Info, "Runtime", detail);
    } else if (report.statusAvailable && report.tasksAvailable) {
        add(report, DiagnosticLevel::Info, "Runtime", "task rates returned; load stats disabled");
    }

    const std::string firmware = firstVersionLine(versionText);
    if (!report.versionAvailable || firmware.empty()) {
        add(report, DiagnosticLevel::Unknown, "Firmware", "version identity unavailable");
    } else {
        add(report, DiagnosticLevel::Info, "Firmware", firmware);
    }

    return report;
}

std::string formatDiagnosticReport(const DiagnosticReport& report,
                                   const std::string& statusText,
                                   const std::string& tasksText,
                                   const std::string& versionText) {
    std::string summary;
    if (!report.complete()) summary = "INCOMPLETE";
    else if (report.actionableBlockerCount() > 0) summary = "ARM BLOCKED";
    else if (report.failureCount() > 0) summary = "FAULT REPORTED";
    else if (report.warningCount() > 0) summary = "CHECK FINDINGS";
    else summary = "NO ACTIVE FAULTS";

    std::ostringstream out;
    out << "# GNDHOG ZERO FIELD CHECK\n"
        << "# Summary: " << summary << "\n"
        << "# Queries: status=" << (report.statusAvailable ? "ok" : "unavailable")
        << ", tasks=" << (report.tasksAvailable ? "ok" : "unavailable")
        << ", version=" << (report.versionAvailable ? "ok" : "unavailable") << "\n"
        << "# Evidence: captured CLI responses follow; no configuration writes\n"
        << "# Note: Betaflight's tasks command resets transient max-time statistics after printing.\n"
        << "# This is not an airworthiness verdict. Inspect the craft props-off.\n"
        << "#\n";
    for (const DiagnosticFinding& finding : report.findings) {
        out << "# [" << diagnosticLevelName(finding.level) << "] "
            << finding.title << ": " << finding.detail << "\n";
    }
    out << "\n# --- status (raw) ---\n" << statusText
        << (statusText.empty() || statusText.back() == '\n' ? "" : "\n")
        << "\n# --- tasks (raw) ---\n" << tasksText
        << (tasksText.empty() || tasksText.back() == '\n' ? "" : "\n")
        << "\n# --- version (raw) ---\n" << versionText
        << (versionText.empty() || versionText.back() == '\n' ? "" : "\n");
    return out.str();
}

} // namespace bf
