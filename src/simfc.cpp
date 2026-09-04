#include "simfc.h"
#include "bfsession.h"
#include "simpty.h"
#include "strutil.h"

#include <cerrno>
#include <cstdlib>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace bf {
namespace {

const char kDiffAll[] =
    "# version\r\n"
    "# Betaflight / STM32G47X (G473) 2026.6.0-alpha May 15 2026 / 06:14:55 (e92c10887) MSP API: 1.48\r\n"
    "# config rev: 6703db5\r\n"
    "\r\n"
    "# start the command batch\r\n"
    "batch start\r\n"
    "\r\n"
    "board_name BETAFPVG473_V2\r\n"
    "manufacturer_id BEFH\r\n"
    "\r\n"
    "# name: AIR65 C\r\n"
    "\r\n"
    "# feature\r\n"
    "feature OSD\r\n"
    "\r\n"
    "# aux\r\n"
    "aux 0 0 0 1700 2100 0 0\r\n"
    "aux 1 1 1 900 1300 0 0\r\n"
    "aux 2 2 1 1300 1700 0 0\r\n"
    "\r\n"
    "# master\r\n"
    "set gyro_lpf1_static_hz = 0\r\n"
    "set gyro_lpf2_static_hz = 550\r\n"
    "set dyn_notch_count = 1\r\n"
    "set dyn_notch_q = 400\r\n"
    "set dshot_bidir = ON\r\n"
    "set motor_pwm_protocol = DSHOT300\r\n"
    "set motor_poles = 12\r\n"
    "set align_board_yaw = -135\r\n"
    "set vbat_scale = 113\r\n"
    "set yaw_motors_reversed = ON\r\n"
    "set small_angle = 180\r\n"
    "set vtx_band = 5\r\n"
    "set vtx_channel = 1\r\n"
    "set craft_name = AIR65 C\r\n"
    "\r\n"
    "profile 0\r\n"
    "\r\n"
    "# profile 0\r\n"
    "set profile_name = GF 1207\r\n"
    "set p_pitch = 35\r\n"
    "set i_pitch = 59\r\n"
    "set d_pitch = 23\r\n"
    "set f_pitch = 42\r\n"
    "set p_roll = 33\r\n"
    "set i_roll = 57\r\n"
    "set d_roll = 21\r\n"
    "set f_roll = 40\r\n"
    "set tpa_rate = 60\r\n"
    "set tpa_breakpoint = 1180\r\n"
    "\r\n"
    "rateprofile 0\r\n"
    "\r\n"
    "# rateprofile 0\r\n"
    "set thr_mid = 28\r\n"
    "set thr_expo = 35\r\n"
    "set roll_srate = 58\r\n"
    "set pitch_srate = 58\r\n"
    "set yaw_srate = 50\r\n"
    "\r\n"
    "# end the command batch\r\n"
    "batch end\r\n";

} // namespace

const char* simDiffAll() { return kDiffAll; }

SimFc::~SimFc() { stop(); }

bool SimFc::start(std::string& error) {
    stop();
    return openSimPty(master_, slavePath_, error);
}

void SimFc::stop() {
#if defined(__linux__)
    if (master_ >= 0) ::close(master_);
#endif
    master_ = -1;
    slavePath_.clear();
    in_.clear();
    wireIn_.clear();
    out_.clear();
    cliMode_ = false;
    vtxPower_ = 3;
    vtxPitMode_ = false;
    vtxReady_ = true;
    pitModeSupported_ = true;
    coreTemperatureC_ = 37;
}

void SimFc::emit(const std::string& text) { out_ += text; }

void SimFc::flush() {
#if defined(__linux__)
    while (!out_.empty()) {
        const ssize_t n = ::write(master_, out_.data(), out_.size());
        if (n > 0) { out_.erase(0, static_cast<size_t>(n)); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EINTR) continue;
        return;
    }
#endif
}

void SimFc::emitMsp(uint8_t command, const std::vector<uint8_t>& payload) {
    emit(encodeMspFrame('>', command, payload));
}

void SimFc::respondMsp(uint8_t command, const std::vector<uint8_t>& payload) {
    if (command == 88) { // MSP_VTX_CONFIG
        const uint16_t frequency = 5658;
        emitMsp(command, {
            3,                 // SmartAudio
            5, 1,             // Raceband 1
            vtxPower_,
            static_cast<uint8_t>(vtxPitMode_ ? 1 : 0),
            static_cast<uint8_t>(frequency & 0xFF),
            static_cast<uint8_t>((frequency >> 8) & 0xFF),
            static_cast<uint8_t>(vtxReady_ ? 1 : 0),
            0,                 // low-power-disarm off
            0, 0,              // pit frequency
            1, 5, 8, 4,       // VTX table available, 4 power levels
        });
    } else if (command == 89 && payload.size() >= 4) { // MSP_SET_VTX_CONFIG
        vtxPower_ = payload[2];
        if (pitModeSupported_) vtxPitMode_ = payload[3] != 0;
        emitMsp(command);
    } else {
        // The FC's "no such command" is the same frame with '!' for its
        // direction and nothing in it.
        emit(encodeMspFrame('!', command, {}));
    }
}

void SimFc::respond(const std::string& raw) {
    const std::string cmd = trim(raw);

    if (!cliMode_) {
        if (startsWith(cmd, "#")) {
            cliMode_ = true;
            emit("\r\nEntering CLI Mode, type 'exit' to return, or 'help'\r\n\r\n# ");
        }
        return;
    }
    if (cmd.empty() || cmd[0] == '#') {
        emit("\r\n# ");
        return;
    }

    if (cmd == "version") {
        emit("# Betaflight / STM32G47X (G473) 2026.6.0-alpha May 15 2026 / 06:14:55 "
             "(e92c10887) MSP API: 1.48\r\n");
    } else if (cmd == "status") {
        emit("MCU G473 Clock=170MHz, Vref=3.30V, Core temp=" +
             std::to_string(coreTemperatureC_) + "degC\r\n"
             "Stack size: 2048, Stack address: 0x2001c000\r\n"
             "Configuration: CONFIGURED, size: 3421, max available: 32768\r\n"
             "Gyros detected: gyro 1\r\n"
             "System Uptime: 84 seconds, Current Time: 2026-08-31T13:59:05.000+00:00\r\n"
             "Voltage: 4.12V (1S battery - OK)\r\n"
             "Arming disable flags: RXLOSS CLI\r\n");
    } else if (cmd == "tasks") {
        emit("Task list             rate/hz  max/us  avg/us maxload avgload  total/ms\r\n"
             "00 - (      SYSTEM)        10       9       1    0.1%    0.0%         3\r\n"
             "01 - (        MAIN)      4000      21       4    8.4%    1.6%       412\r\n"
             "03 - (        GYRO)      8000      12       6    9.6%    4.8%       901\r\n");
    } else if (cmd == "diff" || cmd == "diff all" || cmd == "dump" || cmd == "dump all") {
        emit(kDiffAll);
    } else if (cmd == "mcu_id") {
        emit("mcu_id 0033002f3438510e33393838\r\n");
    } else if (cmd == "save") {
        emit("Writing to on-board flash\r\n");
        cliMode_ = false;   // the FC reboots; the CLI goes away
        return;
    } else if (cmd == "exit") {
        emit("Leaving CLI mode, unsaved changes lost.\r\n");
        cliMode_ = false;
        return;
    } else if (startsWith(cmd, "set ")) {
        const std::string body = trim(cmd.substr(4));
        const size_t eq = body.find('=');
        if (eq == std::string::npos) {
            emit("Invalid value\r\n");
        } else {
            const std::string name = trim(body.substr(0, eq));
            const std::string value = trim(body.substr(eq + 1));
            params_[name] = value;
            emit(name + " set to " + value + "\r\n");
        }
    } else if (startsWith(cmd, "get ")) {
        const std::string name = trim(cmd.substr(4));
        const auto it = params_.find(name);
        emit(it == params_.end() ? "Invalid name\r\n"
                                 : name + " = " + it->second + "\r\n");
    } else if (cmd == "batch start" || cmd == "batch end" ||
               startsWith(cmd, "profile") || startsWith(cmd, "rateprofile") ||
               startsWith(cmd, "battery_profile") || startsWith(cmd, "feature") ||
               startsWith(cmd, "aux ") || startsWith(cmd, "board_name") ||
               startsWith(cmd, "manufacturer_id") || startsWith(cmd, "vtxtable")) {
        // Accepted silently, as the real CLI does inside a batch.
    } else {
        emit("Unknown command, try 'help'\r\n");
    }
    emit("# ");
}

void SimFc::pump() {
#if defined(__linux__)
    if (master_ < 0) return;
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(master_, buf, sizeof(buf));
        if (n <= 0) break;
        wireIn_.append(buf, static_cast<size_t>(n));
        for (;;) {
            if (wireIn_.empty()) break;
            if (wireIn_[0] == '$') {
                if (wireIn_.size() < 3) break;
                if (wireIn_.compare(0, 3, "$M<") == 0) {
                    if (wireIn_.size() < 6) break;
                    const uint8_t size = static_cast<uint8_t>(wireIn_[3]);
                    const size_t frameSize = static_cast<size_t>(size) + 6;
                    if (wireIn_.size() < frameSize) break;
                    const uint8_t command = static_cast<uint8_t>(wireIn_[4]);
                    uint8_t checksum = size ^ command;
                    std::vector<uint8_t> payload;
                    payload.reserve(size);
                    for (size_t i = 0; i < size; ++i) {
                        const uint8_t byte = static_cast<uint8_t>(wireIn_[5 + i]);
                        payload.push_back(byte);
                        checksum ^= byte;
                    }
                    const uint8_t received = static_cast<uint8_t>(wireIn_[frameSize - 1]);
                    wireIn_.erase(0, frameSize);
                    if (checksum == received) respondMsp(command, payload);
                    continue;
                }
            }

            const char c = wireIn_.front();
            wireIn_.erase(0, 1);
            // The real CLI echoes every byte it receives.
            if (c == '\n') {
                emit("\r\n");
                const std::string line = in_;
                in_.clear();
                respond(line);
            } else if (c != '\r') {
                in_ += c;
                emit(std::string(1, c));
            }
        }
        if (n < static_cast<ssize_t>(sizeof(buf))) break;
    }
    flush();
#endif
}

} // namespace bf
