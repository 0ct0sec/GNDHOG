#pragma once

#include <string>
#include <vector>

namespace bf {

// Betaflight's own default OSD warning threshold. The status command exposes
// the MCU die temperature, not a VTX temperature sensor.
constexpr int kDefaultCoreTemperatureAlarmC = 70;

enum class DiagnosticLevel {
    Pass,
    Info,
    Warning,
    Failure,
    Unknown,
};

struct DiagnosticFinding {
    DiagnosticLevel level = DiagnosticLevel::Info;
    std::string title;
    std::string detail;
};

// Evidence-bounded interpretation of three non-configuration Betaflight CLI
// queries. `tasks` resets transient max-time statistics after printing them;
// none of the queries change settings or flash. This intentionally does not
// claim that a craft is airworthy: status/tasks can
// expose current blockers and runtime evidence, but cannot inspect props,
// solder joints, motor direction, control surfaces, or behavior in flight.
struct DiagnosticReport {
    std::vector<DiagnosticFinding> findings;
    std::vector<std::string> armingFlags;
    bool statusAvailable = false;
    bool tasksAvailable = false;
    bool versionAvailable = false;
    bool coreTemperatureAvailable = false;
    int coreTemperatureC = 0;

    int failureCount() const;
    int warningCount() const;
    int actionableBlockerCount() const;
    bool complete() const { return statusAvailable && tasksAvailable && versionAvailable; }
};

// Extracts `Core temp=41degC` (and spacing/case variants) from status output.
// Returns false when the target does not expose an MCU temperature.
bool parseCoreTemperatureC(const std::string& text, int& temperatureC);

DiagnosticReport buildDiagnosticReport(const std::string& statusText,
                                       const std::string& tasksText,
                                       const std::string& versionText);

// Human-readable, comment-led report followed by the untouched command
// responses. Suitable for saving and sharing without pretending the summary
// is stronger evidence than the FC transcript underneath it.
std::string formatDiagnosticReport(const DiagnosticReport& report,
                                   const std::string& statusText,
                                   const std::string& tasksText,
                                   const std::string& versionText);

const char* diagnosticLevelName(DiagnosticLevel level);

} // namespace bf
