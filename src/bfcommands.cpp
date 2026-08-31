#include "bfcommands.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace bf {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool isParamChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Commands present across Betaflight 4.3 through the 2026.x line. A command the
// running firmware does not have simply errors on the FC, exactly as it would
// if it were typed by hand.
const char* const kCommands[] = {
    "adjrange", "aux", "batch", "battery_profile", "beacon", "beeper", "bind_rx",
    "blackbox", "board_name", "bootloader", "color", "defaults", "diff", "dma",
    "dshotprog", "dump", "exit", "feature", "flash_erase", "flash_info",
    "flash_read", "flash_scan", "flash_write", "get", "gpspassthrough",
    "gyroregisters", "help", "led", "ledstrip_profile", "manufacturer_id", "map",
    "mcu_id", "mixer", "mmix", "mode_color", "motor", "msc", "name",
    "param_group_list", "playsound", "profile", "rateprofile", "rc_smoothing_info",
    "rebootenable", "resource", "rxfail", "rxrange", "save", "sd_info", "serial",
    "serialpassthrough", "servo", "set", "simplified_tuning", "smix", "status",
    "tasks", "timer", "version", "vtx", "vtx_info", "vtxtable",
};

// A starter parameter index so completion is useful before any dump is seen.
// The full set is learned from the FC's own output.
const char* const kSeedParams[] = {
    "acc_calibration", "acc_hardware", "acc_lpf_hz", "acc_trim_pitch", "acc_trim_roll",
    "align_board_pitch", "align_board_roll", "align_board_yaw", "anti_gravity_gain",
    "auto_profile_cell_count", "bat_capacity", "battery_meter", "beeper_dshot_beacon_tone",
    "blackbox_device", "blackbox_mode", "blackbox_sample_rate", "craft_name",
    "current_meter", "d_max_gain", "d_max_pitch", "d_max_roll", "d_max_yaw",
    "d_pitch", "d_roll", "d_yaw", "deadband", "debug_mode", "dshot_bidir",
    "dshot_bitbang", "dshot_idle_value", "dyn_idle_min_rpm", "dyn_notch_count",
    "dyn_notch_max_hz", "dyn_notch_min_hz", "dyn_notch_q", "f_pitch", "f_roll",
    "f_yaw", "failsafe_delay", "failsafe_procedure", "failsafe_throttle",
    "feedforward_averaging", "feedforward_jitter_factor", "feedforward_smooth_factor",
    "feedforward_transition", "gyro_hardware_lpf", "gyro_lpf1_dyn_max_hz",
    "gyro_lpf1_dyn_min_hz", "gyro_lpf1_static_hz", "gyro_lpf2_static_hz",
    "gyro_to_use", "i_pitch", "i_roll", "i_yaw", "ibata_offset", "ibata_scale",
    "imu_dcm_ki", "imu_dcm_kp", "iterm_relax", "iterm_relax_cutoff",
    "iterm_relax_type", "iterm_rotation", "iterm_windup", "led_inversion",
    "mag_hardware", "min_check", "max_check", "motor_output_limit", "motor_poles",
    "motor_pwm_freq", "motor_pwm_protocol", "osd_alt_alarm", "osd_cap_alarm",
    "osd_craft_name_pos", "osd_displayport_device", "osd_rssi_alarm",
    "osd_rssi_dbm_alarm", "osd_vbat_pos", "osd_warnings_pos", "p_pitch", "p_roll",
    "p_yaw", "pid_process_denom", "pitch_rc_rate", "pitch_srate",
    "pitch_expo", "profile_name", "rateprofile_name", "rates_type", "rc_smoothing",
    "rc_smoothing_auto_factor", "roll_rc_rate", "roll_srate", "roll_expo",
    "rpm_filter_harmonics", "rpm_filter_min_hz", "rpm_filter_q", "runaway_takeoff_prevention",
    "rx_min_usec", "rx_max_usec", "rx_spi_protocol", "serialrx_provider",
    "simplified_d_gain", "simplified_d_max_gain", "simplified_dterm_filter",
    "simplified_dterm_filter_multiplier", "simplified_feedforward_gain",
    "simplified_gyro_filter", "simplified_gyro_filter_multiplier", "simplified_i_gain",
    "simplified_master_multiplier", "simplified_pitch_d_gain", "simplified_pitch_pi_gain",
    "small_angle", "system_hse_mhz", "thr_expo", "thr_hover", "thr_mid",
    "throttle_boost", "throttle_limit_percent", "throttle_limit_type", "tpa_breakpoint",
    "tpa_mode", "tpa_rate", "vbat_max_cell_voltage", "vbat_min_cell_voltage",
    "vbat_scale", "vbat_warning_cell_voltage", "vcd_video_system", "vtx_band",
    "vtx_channel", "vtx_freq", "vtx_power", "yaw_deadband", "yaw_motors_reversed",
    "yaw_srate", "yaw_rc_rate", "yaw_expo",
};

const char* const kFeatures[] = {
    "RX_PPM", "INFLIGHT_ACC_CAL", "RX_SERIAL", "MOTOR_STOP", "SERVO_TILT",
    "SOFTSERIAL", "GPS", "RANGEFINDER", "TELEMETRY", "3D", "RX_PARALLEL_PWM",
    "RX_MSP", "RSSI_ADC", "LED_STRIP", "DISPLAY", "OSD", "CHANNEL_FORWARDING",
    "TRANSPONDER", "AIRMODE", "RX_SPI", "ESC_SENSOR", "ANTI_GRAVITY", "DYN_NOTCH",
};

} // namespace

Completer::Completer() {
    commands_.assign(std::begin(kCommands), std::end(kCommands));
    for (const char* p : kSeedParams) params_.insert(p);
    features_.assign(std::begin(kFeatures), std::end(kFeatures));
}

void Completer::addParam(const std::string& name) {
    if (name.empty() || name.size() > 64) return;
    for (char c : name) {
        if (!isParamChar(c)) return;
    }
    params_.insert(name);
}

void Completer::harvest(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        if (s.rfind("set ", 0) == 0) s = trim(s.substr(4));
        // Both `name = value` (dump) and `name = value` (get) reduce to this.
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        addParam(trim(s.substr(0, eq)));
    }
}

Completer::Result Completer::complete(const std::string& line, int cursor) const {
    Result r;
    const int cur = std::max(0, std::min<int>(cursor, static_cast<int>(line.size())));

    int start = cur;
    while (start > 0 && (isParamChar(line[static_cast<size_t>(start - 1)]) ||
                         line[static_cast<size_t>(start - 1)] == '-')) {
        --start;
    }
    r.prefix = line.substr(static_cast<size_t>(start), static_cast<size_t>(cur - start));

    // What comes before the word decides which vocabulary applies.
    const std::string before = lower(trim(line.substr(0, static_cast<size_t>(start))));
    const std::vector<std::string>* pool = nullptr;
    std::vector<std::string> scratch;

    if (before.empty()) {
        pool = &commands_;
    } else if (before == "set" || before == "get") {
        scratch.assign(params_.begin(), params_.end());
        pool = &scratch;
    } else if (before == "feature") {
        pool = &features_;
    } else {
        return r;   // an argument we have no vocabulary for: leave it alone
    }

    const std::string pfx = lower(r.prefix);
    for (const std::string& c : *pool) {
        if (lower(c).rfind(pfx, 0) == 0) r.candidates.push_back(c);
    }
    if (r.candidates.empty()) return r;

    r.commonPrefix = r.candidates.front();
    for (const std::string& c : r.candidates) {
        size_t i = 0;
        while (i < r.commonPrefix.size() && i < c.size() &&
               std::tolower(static_cast<unsigned char>(r.commonPrefix[i])) ==
                   std::tolower(static_cast<unsigned char>(c[i]))) {
            ++i;
        }
        r.commonPrefix.resize(i);
    }
    return r;
}

std::string commandWord(const std::string& line) {
    const std::string s = trim(line);
    const size_t sp = s.find_first_of(" \t");
    return lower(sp == std::string::npos ? s : s.substr(0, sp));
}

RiskNote riskFor(const std::string& line) {
    const std::string cmd = commandWord(line);
    const std::string rest = trim(line).size() > cmd.size()
                                 ? trim(trim(line).substr(cmd.size()))
                                 : std::string();

    if (cmd == "motor") {
        // `motor` with no argument only reports values; with a value it drives
        // an output, which is the case worth stopping for.
        if (!rest.empty() && rest.find(' ') != std::string::npos) {
            return {Risk::Motors, "Spins a motor. Props off?"};
        }
        return {Risk::None, ""};
    }
    if (cmd == "dshotprog") return {Risk::Motors, "Programs an ESC. Props off?"};
    if (cmd == "defaults")  return {Risk::Destructive, "Erases ALL settings on the FC."};
    if (cmd == "flash_erase") return {Risk::Destructive, "Erases the blackbox flash chip."};
    if (cmd == "bootloader" || cmd == "msc") {
        return {Risk::Destructive, "Reboots the FC out of CLI mode."};
    }
    if (cmd == "resource")  return {Risk::Writes, "Remaps a pin. Wrong values can brick the board."};
    if (cmd == "save")      return {Risk::Writes, "Writes settings to flash and reboots."};
    return {Risk::None, ""};
}

std::string craftNameFromDump(const std::string& dump) {
    std::istringstream in(dump);
    std::string line;
    std::string name;
    while (std::getline(in, line)) {
        const std::string s = trim(line);
        if (s.rfind("set craft_name", 0) == 0) {
            const size_t eq = s.find('=');
            if (eq != std::string::npos) name = trim(s.substr(eq + 1));
        } else if (s.rfind("# name:", 0) == 0 && name.empty()) {
            name = trim(s.substr(7));
        } else if (s.rfind("name ", 0) == 0 && name.empty()) {
            name = trim(s.substr(5));
        }
    }
    if (name == "-" || name == "EMPTY") name.clear();
    return name;
}

std::string boardNameFromDump(const std::string& dump) {
    std::istringstream in(dump);
    std::string line;
    while (std::getline(in, line)) {
        const std::string s = trim(line);
        if (s.rfind("board_name ", 0) == 0) return trim(s.substr(11));
    }
    return {};
}

} // namespace bf
