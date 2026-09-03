#include "bfsession.h"
#include "diagnostics.h"
#include "input.h"
#include "strutil.h"

#include <algorithm>
#include <sstream>

namespace bf {
namespace {

// How long the link must be quiet before a visible "# " counts as the prompt.
// A streaming `diff` can momentarily show a bare "# " while only the first two
// bytes of a comment line have arrived.
constexpr uint64_t kPromptQuietMs = 120;
constexpr uint64_t kCommandTimeoutMs = 15000;
constexpr uint64_t kRestoreLineTimeoutMs = 3000;
constexpr uint64_t kCliBannerTimeoutMs = 2500;
constexpr int kCliMaxAttempts = 3;
constexpr uint8_t kMspVtxConfig = 88;
constexpr uint8_t kMspSetVtxConfig = 89;
constexpr uint64_t kMspReplyTimeoutMs = 900;
constexpr uint64_t kVtxApplySettleMs = 700;
constexpr uint64_t kRebootDrainMs = 400;

} // namespace

std::string VtxStatus::deviceLabel() const {
    switch (deviceType) {
    case 1: return "RTC6705";
    case 3: return "SmartAudio";
    case 4: return "Tramp";
    case 5: return "MSP VTX";
    case 0: return "unsupported";
    case 0xFF: return "unknown";
    default: return "VTX type " + std::to_string(deviceType);
    }
}

bool isErrorLine(const std::string& line) {
    const std::string t = trim(line);
    if (t.empty()) return false;
    if (t.find("###ERROR") != std::string::npos) return true;
    if (t.rfind("Unknown command", 0) == 0) return true;
    if (t.rfind("Invalid name", 0) == 0) return true;
    if (t.rfind("Invalid value", 0) == 0) return true;
    if (t.rfind("Parse error", 0) == 0) return true;
    return false;
}

std::vector<std::string> restorableLines(const std::string& fileText) {
    std::vector<std::string> out;
    std::istringstream in(fileText);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        out.push_back(t);
    }
    return out;
}

namespace {

// Removes the command the CLI echoed back. Left in, a backup file would begin
// with "diff all" and restoring it would replay that dump mid-restore.
void stripEchoedCommand(std::string& text, const std::string& command) {
    const std::string want = trim(command);
    if (want.empty() || text.empty()) return;
    const size_t nl = text.find('\n');
    const std::string first = trim(text.substr(0, nl == std::string::npos ? text.size() : nl));
    if (first != want) return;
    text.erase(0, nl == std::string::npos ? text.size() : nl + 1);
}

} // namespace

Session::Session(Terminal& term, Completer& completer) : term_(term), completer_(completer) {}

bool Session::connect(const std::string& device, int baud, std::string& error) {
    disconnect();
    if (!port_.open(device, baud, error)) {
        state_ = SessionState::Failed;
        return false;
    }
    term_.resetInputFragment();
    linkLost_ = false;
    state_ = SessionState::ProbingMsp;
    connectStartMs_ = nowMs();
    lastByteMs_ = connectStartMs_;
    mspInput_.clear();
    mspAction_ = MspAction::ProbeVtx;
    mspDeadlineMs_ = connectStartMs_ + kMspReplyTimeoutMs;
    vtxOriginal_ = VtxStatus{};
    vtxBenchMode_ = VtxBenchMode::None;
    vtxGuardNote_.clear();
    coreTemperatureAvailable_ = false;
    coreTemperatureC_ = 0;
    queueMsp(kMspVtxConfig);
    return true;
}

void Session::disconnect() {
    // The port is simply closed. `exit` is deliberately not sent: on Betaflight
    // it reboots the flight controller, which is not what closing a terminal
    // should do. It is available as an explicit command instead.
    port_.close();
    state_ = SessionState::Disconnected;
    job_ = JobStatus{};
    captureDone_ = nullptr;
    restoreLines_.clear();
    restoreIndex_ = 0;
    firmware_.clear();
    board_.clear();
    craft_.clear();
    mspInput_.clear();
    mspAction_ = MspAction::None;
    vtxOriginal_ = VtxStatus{};
    vtxBenchMode_ = VtxBenchMode::None;
}

void Session::beginCli(uint64_t now) {
    if (!port_.isOpen()) return;
    mspInput_.clear();
    mspAction_ = MspAction::None;
    state_ = SessionState::EnteringCli;
    connectStartMs_ = now;
    lastByteMs_ = now;
    cliAttempts_ = 1;
    // '#' is how Betaflight leaves MSP mode and enters the CLI. If it is
    // already in the CLI the same byte is just a comment, so this is safe to
    // send either way. One newline, never CRLF: Betaflight ends a line on
    // either character and prints a prompt for each, so CRLF gives two.
    port_.write("#\n");
}

void Session::queueMsp(uint8_t command, const std::vector<uint8_t>& payload) {
    std::string frame;
    frame.reserve(payload.size() + 6);
    frame += "$M<";
    const uint8_t size = static_cast<uint8_t>(payload.size());
    frame.push_back(static_cast<char>(size));
    frame.push_back(static_cast<char>(command));
    uint8_t checksum = size ^ command;
    for (uint8_t byte : payload) {
        frame.push_back(static_cast<char>(byte));
        checksum ^= byte;
    }
    frame.push_back(static_cast<char>(checksum));
    port_.write(frame);
}

bool Session::parseVtxStatus(const std::vector<uint8_t>& payload, VtxStatus& status) const {
    if (payload.size() < 9) return false;
    status = VtxStatus{};
    status.valid = true;
    status.deviceType = payload[0];
    status.band = payload[1];
    status.channel = payload[2];
    status.power = payload[3];
    status.pitMode = payload[4] != 0;
    status.frequency = static_cast<uint16_t>(payload[5]) |
                       (static_cast<uint16_t>(payload[6]) << 8);
    status.deviceReady = payload[7] != 0;
    status.lowPowerDisarm = payload[8];
    if (payload.size() >= 15) {
        status.tableAvailable = payload[11] != 0;
        status.powerLevels = payload[14];
    }
    return true;
}

std::vector<uint8_t> Session::vtxSetPayload(uint8_t power, bool pitMode) const {
    uint16_t encoded = vtxOriginal_.frequency;
    if (vtxOriginal_.band > 0 && vtxOriginal_.channel > 0 &&
        vtxOriginal_.band <= 8 && vtxOriginal_.channel <= 8) {
        encoded = static_cast<uint16_t>((vtxOriginal_.band - 1) * 8 +
                                        (vtxOriginal_.channel - 1));
    }
    return {
        static_cast<uint8_t>(encoded & 0xFF),
        static_cast<uint8_t>((encoded >> 8) & 0xFF),
        power,
        static_cast<uint8_t>(pitMode ? 1 : 0),
    };
}

void Session::requestVtxStatus(MspAction action, uint64_t now) {
    mspAction_ = action;
    mspDeadlineMs_ = now + kMspReplyTimeoutMs;
    queueMsp(kMspVtxConfig);
}

void Session::noteVtxGuard(const std::string& note, LineKind kind) {
    vtxGuardNote_ = note;
    ++vtxGuardNoteSequence_;
    term_.addLine("-- " + note + " --", kind);
}

void Session::finishVtxGuard(VtxBenchMode mode, const std::string& note, uint64_t now) {
    vtxBenchMode_ = mode;
    noteVtxGuard(note, mode == VtxBenchMode::PitMode ? LineKind::Good : LineKind::Warn);
    beginCli(now);
}

void Session::startPitRollback(uint64_t now) {
    mspAction_ = MspAction::SetPitOff;
    mspDeadlineMs_ = now + kMspReplyTimeoutMs;
    queueMsp(kMspSetVtxConfig, vtxSetPayload(vtxOriginal_.power, false));
}

void Session::enableVtxBenchGuard() {
    if (state_ != SessionState::AwaitingVtxChoice) return;
    const uint64_t now = nowMs();
    state_ = SessionState::ApplyingVtxGuard;
    mspAction_ = MspAction::SetPit;
    mspDeadlineMs_ = now + kMspReplyTimeoutMs;
    // Pit mode is runtime state and does not alter the saved VTX power. Some
    // SmartAudio versions cannot enter it after power-up; in that case we fail
    // closed instead of modifying vtx_power behind the operator's back.
    queueMsp(kMspSetVtxConfig, vtxSetPayload(vtxOriginal_.power, true));
}

void Session::skipVtxBenchGuard() {
    if (state_ != SessionState::AwaitingVtxChoice) return;
    noteVtxGuard("VTX left at its current flight setting", LineKind::Warn);
    beginCli(nowMs());
}

bool Session::restoreVtxAndDisconnect() {
    if (!vtxBenchGuardActive() || !ready()) return false;
    term_.addLine("-- restoring saved VTX state by rebooting the FC --", LineKind::Local);
    state_ = SessionState::Rebooting;
    rebootStartedMs_ = nowMs();
    port_.write("exit\n");
    return true;
}

bool Session::atPrompt() const {
    const std::string& p = term_.partial();
    return p == "# " || p == "#";
}

bool Session::commandComplete(uint64_t now) const {
    if (!atPrompt()) return false;
    if (term_.linesEver() <= linesAtSend_) return false;
    return idleMs(now) >= kPromptQuietMs;
}

void Session::beginCommand(const std::string& line, uint64_t now) {
    linesAtSend_ = term_.linesEver();
    commandSentMs_ = now;
    state_ = SessionState::Busy;
    port_.write(line + "\n");
}

bool Session::send(const std::string& line) {
    if (!port_.isOpen()) return false;
    if (state_ != SessionState::Ready) return false;
    beginCommand(line, nowMs());
    return true;
}

bool Session::startCapture(const std::string& command, const std::string& label,
                           std::function<void(bool, const std::string&)> onDone) {
    if (!ready()) return false;
    term_.markCapture();
    job_ = JobStatus{};
    job_.kind = JobKind::Capture;
    job_.label = label;
    job_.total = 0;
    captureDone_ = std::move(onDone);
    captureCommand_ = command;
    beginCommand(command, nowMs());
    return true;
}

bool Session::startRestore(const std::vector<std::string>& lines, const std::string& label) {
    if (!ready() || lines.empty()) return false;
    restoreLines_ = lines;
    restoreIndex_ = 0;
    restoreErrors_ = 0;
    job_ = JobStatus{};
    job_.kind = JobKind::Restore;
    job_.label = label;
    job_.total = static_cast<int>(lines.size());
    job_.done = 0;
    // The first line goes out here; the rest follow as each prompt returns.
    beginCommand(restoreLines_[restoreIndex_], nowMs());
    return true;
}

void Session::cancelJob() {
    if (!job_.active()) return;
    const bool wasRestore = job_.kind == JobKind::Restore;
    restoreLines_.clear();
    restoreIndex_ = 0;
    captureDone_ = nullptr;
    finishJob(false, wasRestore ? "Restore cancelled - the FC keeps whatever was already sent"
                                : "Cancelled");
}

void Session::finishJob(bool ok, const std::string& message) {
    job_.finished = true;
    job_.ok = ok;
    job_.message = message;
    job_.errorCount = (job_.kind == JobKind::Restore) ? restoreErrors_ : 0;
}

void Session::clearFinishedJob() {
    if (job_.finished) job_ = JobStatus{};
}

void Session::scanForIdentity(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (firmware_.empty() && t.rfind("# Betaflight /", 0) == 0) {
            firmware_ = trim(t.substr(2));
        } else if (firmware_.empty() && t.rfind("Betaflight /", 0) == 0) {
            firmware_ = t;
        }
        if (board_.empty() && t.rfind("board_name ", 0) == 0) board_ = trim(t.substr(11));
        if (craft_.empty() && t.rfind("# name:", 0) == 0) craft_ = trim(t.substr(7));
    }
}

void Session::noteDumpText(const std::string& text) {
    completer_.harvest(text);
    const std::string c = craftNameFromDump(text);
    if (!c.empty()) craft_ = c;
    const std::string b = boardNameFromDump(text);
    if (!b.empty()) board_ = b;
    scanForIdentity(text);
}

void Session::pumpRestore(uint64_t now) {
    ++restoreIndex_;
    job_.done = static_cast<int>(restoreIndex_);
    if (restoreIndex_ >= restoreLines_.size()) {
        const std::string msg =
            restoreErrors_ > 0
                ? "Sent " + std::to_string(restoreLines_.size()) + " lines, " +
                      std::to_string(restoreErrors_) + " rejected - review before saving"
                : "Sent " + std::to_string(restoreLines_.size()) + " lines. Run `save` to keep them.";
        restoreLines_.clear();
        finishJob(restoreErrors_ == 0, msg);
        state_ = SessionState::Ready;
        return;
    }
    beginCommand(restoreLines_[restoreIndex_], now);
}

void Session::processMspInput(uint64_t now) {
    for (;;) {
        const size_t start = mspInput_.find("$M");
        if (start == std::string::npos) {
            if (mspInput_.size() > 2) mspInput_.erase(0, mspInput_.size() - 2);
            return;
        }
        if (start > 0) mspInput_.erase(0, start);
        if (mspInput_.size() < 6) return;
        const uint8_t direction = static_cast<uint8_t>(mspInput_[2]);
        if (direction != '>' && direction != '!') {
            mspInput_.erase(0, 1);
            continue;
        }
        const uint8_t size = static_cast<uint8_t>(mspInput_[3]);
        const size_t frameSize = static_cast<size_t>(size) + 6;
        if (mspInput_.size() < frameSize) return;
        const uint8_t command = static_cast<uint8_t>(mspInput_[4]);
        uint8_t checksum = size ^ command;
        std::vector<uint8_t> payload;
        payload.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            const uint8_t byte = static_cast<uint8_t>(mspInput_[5 + i]);
            payload.push_back(byte);
            checksum ^= byte;
        }
        const uint8_t received = static_cast<uint8_t>(mspInput_[frameSize - 1]);
        mspInput_.erase(0, frameSize);
        if (checksum != received) continue;
        handleMspFrame(direction, command, payload, now);
        if (state_ == SessionState::EnteringCli || state_ == SessionState::Disconnected) return;
    }
}

void Session::handleMspFrame(uint8_t direction, uint8_t command,
                             const std::vector<uint8_t>& payload, uint64_t now) {
    if (direction == '!') {
        if (mspAction_ == MspAction::SetPit) {
            finishVtxGuard(VtxBenchMode::None,
                           "VTX rejected the bench guard; power state was not changed", now);
        } else if (mspAction_ == MspAction::VerifyPit) {
            startPitRollback(now);
        } else if (mspAction_ == MspAction::SetPitOff ||
                   mspAction_ == MspAction::VerifyPitOff) {
            finishVtxGuard(VtxBenchMode::Unconfirmed,
                           "VTX state is unconfirmed; reboot restoration will be offered", now);
        } else if (mspAction_ == MspAction::ProbeVtx) {
            beginCli(now);
        }
        return;
    }

    if (mspAction_ == MspAction::ProbeVtx && command == kMspVtxConfig) {
        VtxStatus status;
        if (!parseVtxStatus(payload, status) || !status.deviceReady ||
            status.deviceType == 0 || status.deviceType == 0xFF) {
            beginCli(now);
            return;
        }
        const bool encodedChannelValid = status.band > 0 && status.band <= 8 &&
                                         status.channel > 0 && status.channel <= 8;
        const bool directFrequencyValid = status.band == 0 &&
                                          status.frequency >= 5000 && status.frequency <= 5999;
        if (!encodedChannelValid && !directFrequencyValid) {
            term_.addLine("-- VTX state cannot be preserved safely; bench guard skipped --",
                          LineKind::Warn);
            beginCli(now);
            return;
        }
        vtxOriginal_ = status;
        if (status.pitMode) {
            noteVtxGuard("VTX already reports pit mode", LineKind::Good);
            beginCli(now);
            return;
        }
        state_ = SessionState::AwaitingVtxChoice;
        mspAction_ = MspAction::None;
        term_.addLine("-- VTX detected: " + status.deviceLabel() +
                          ", power level " + std::to_string(status.power) + " --",
                      LineKind::Local);
        return;
    }

    if (command == kMspSetVtxConfig) {
        if (mspAction_ == MspAction::SetPit) {
            mspAction_ = MspAction::WaitPit;
            mspDeadlineMs_ = now + kVtxApplySettleMs;
        } else if (mspAction_ == MspAction::SetPitOff) {
            mspAction_ = MspAction::WaitPitOff;
            mspDeadlineMs_ = now + kVtxApplySettleMs;
        }
        return;
    }

    if (command != kMspVtxConfig) return;
    VtxStatus status;
    if (!parseVtxStatus(payload, status)) return;
    if (mspAction_ == MspAction::VerifyPit) {
        if (status.deviceReady && status.pitMode) {
            finishVtxGuard(VtxBenchMode::PitMode,
                           "VTX pit mode confirmed for this unsaved bench session", now);
        } else {
            startPitRollback(now);
        }
    } else if (mspAction_ == MspAction::VerifyPitOff) {
        if (status.deviceReady && status.pitMode) {
            finishVtxGuard(VtxBenchMode::PitMode,
                           "VTX pit mode confirmed after a delayed response", now);
        } else if (status.deviceReady) {
            finishVtxGuard(VtxBenchMode::None,
                           "VTX pit mode unsupported; unchanged state confirmed", now);
        } else {
            finishVtxGuard(VtxBenchMode::Unconfirmed,
                           "VTX state became unavailable; reboot restoration will be offered", now);
        }
    }
}

void Session::noteCoreTemperature(const std::vector<TermLine>& lines) {
    for (const TermLine& line : lines) {
        int temperatureC = 0;
        if (!parseCoreTemperatureC(line.text, temperatureC)) continue;
        coreTemperatureAvailable_ = true;
        coreTemperatureC_ = temperatureC;
        ++coreTemperatureSequence_;
    }
}

// A write or read failure is the FC going away. Mid-reboot that is the
// expected end of a VTX restore; anywhere else it is a lost link, and a job
// in flight is finished as failed so nothing keeps waiting on it.
void Session::loseLink(const std::string& reason, const char* rebootNote) {
    if (state_ == SessionState::Rebooting) {
        term_.addLine(std::string("-- ") + rebootNote + " --", LineKind::Good);
        disconnect();
        return;
    }
    linkLost_ = true;
    term_.addLine("-- link lost (" + reason + ") --", LineKind::Error);
    port_.close();
    state_ = SessionState::Disconnected;
    if (job_.active()) finishJob(false, "Link lost");
}

// Hands the captured output to whoever asked for it. A complete capture is
// also mined for identity and completion candidates; a partial one is
// delivered as it stands, marked failed, and never learned from.
void Session::finishCapture(bool ok) {
    std::string captured = term_.captureSince();
    stripEchoedCommand(captured, captureCommand_);
    if (ok) noteDumpText(captured);
    auto cb = std::move(captureDone_);
    captureDone_ = nullptr;
    finishJob(ok, ok ? "Captured " + std::to_string(captured.size()) + " bytes"
                     : std::string("Timed out waiting for the FC"));
    if (cb) cb(ok, captured);
}

void Session::poll(uint64_t now) {
    if (!port_.isOpen()) return;

    std::string err;
    if (!port_.flush(err)) {
        loseLink(err, "FC reboot requested; saved VTX state will reload");
        return;
    }

    std::string incoming;
    const int n = port_.read(incoming);
    if (n < 0) {
        loseLink("device disconnected", "FC reboot observed; saved VTX state will reload");
        return;
    }
    if (n > 0) {
        lastByteMs_ = now;
        if (state_ == SessionState::ProbingMsp ||
            state_ == SessionState::AwaitingVtxChoice ||
            state_ == SessionState::ApplyingVtxGuard) {
            mspInput_ += incoming;
            processMspInput(now);
            incoming.clear();
        }
    }
    if (!incoming.empty()) {
        const std::vector<TermLine> completed = term_.feed(incoming);
        noteCoreTemperature(completed);
        // Cheap running harvest: every parameter name that scrolls past becomes
        // a completion candidate, so no extra query is ever needed.
        // Inspect the batch itself rather than the bounded display buffer: a
        // large read can evict an early response line before feed() returns.
        for (const TermLine& l : completed) {
            if (l.text.find('=') != std::string::npos) completer_.harvest(l.text);
            if (job_.kind == JobKind::Restore && isErrorLine(l.text)) ++restoreErrors_;
            if (firmware_.empty() && l.text.find("Betaflight /") != std::string::npos) {
                scanForIdentity(l.text);
            }
        }
    }

    switch (state_) {
    case SessionState::ProbingMsp:
        if (now >= mspDeadlineMs_) beginCli(now);
        break;

    case SessionState::AwaitingVtxChoice:
        break;

    case SessionState::ApplyingVtxGuard:
        if (now < mspDeadlineMs_) break;
        switch (mspAction_) {
        case MspAction::SetPit:
        case MspAction::WaitPit:
            requestVtxStatus(MspAction::VerifyPit, now);
            break;
        case MspAction::VerifyPit:
            startPitRollback(now);
            break;
        case MspAction::SetPitOff:
        case MspAction::WaitPitOff:
            requestVtxStatus(MspAction::VerifyPitOff, now);
            break;
        case MspAction::VerifyPitOff:
            finishVtxGuard(VtxBenchMode::Unconfirmed,
                           "VTX state is unconfirmed; reboot restoration will be offered", now);
            break;
        default:
            finishVtxGuard(VtxBenchMode::None,
                           "VTX bench guard stopped without confirmation", now);
            break;
        }
        break;

    case SessionState::EnteringCli: {
        if (atPrompt() && idleMs(now) >= kPromptQuietMs) {
            state_ = SessionState::Ready;
            term_.addLine("-- CLI ready --", LineKind::Good);
            break;
        }
        if (now - connectStartMs_ > kCliBannerTimeoutMs) {
            if (cliAttempts_ < kCliMaxAttempts) {
                ++cliAttempts_;
                connectStartMs_ = now;
                port_.write("#\n");
            } else {
                state_ = SessionState::Failed;
                term_.addLine("-- no CLI prompt. Is this a Betaflight FC, and is it powered? --",
                              LineKind::Error);
            }
        }
        break;
    }

    case SessionState::Busy: {
        if (commandComplete(now)) {
            state_ = SessionState::Ready;
            if (job_.kind == JobKind::Capture && !job_.finished) {
                finishCapture(true);
            } else if (job_.kind == JobKind::Restore && !job_.finished) {
                pumpRestore(now);
            }
            break;
        }
        // Stuck means nothing has arrived for a while, not merely that the
        // command is taking its time: a `dump all` down the Grove UART at 9600
        // baud runs for half a minute and is perfectly healthy the whole way.
        // The min() also covers a command sent after a long idle spell, where
        // the last byte is already older than the limit.
        const uint64_t limit =
            (job_.kind == JobKind::Restore) ? kRestoreLineTimeoutMs : kCommandTimeoutMs;
        const uint64_t stalled = std::min(now - commandSentMs_, idleMs(now));
        if (stalled > limit) {
            if (job_.kind == JobKind::Restore && !job_.finished) {
                // A line that never came back is recorded and the run continues;
                // stopping halfway would leave a more confusing config behind.
                ++restoreErrors_;
                state_ = SessionState::Ready;
                pumpRestore(now);
            } else if (job_.kind == JobKind::Capture && !job_.finished) {
                state_ = SessionState::Ready;
                finishCapture(false);
            } else {
                term_.addLine("-- no response; returning to the prompt --", LineKind::Warn);
                state_ = SessionState::Ready;
            }
        }
        break;
    }

    case SessionState::Rebooting:
        if (port_.pendingOut() == 0 && now - rebootStartedMs_ >= kRebootDrainMs) {
            term_.addLine("-- FC reboot requested; saved VTX state will reload --", LineKind::Good);
            disconnect();
        }
        break;

    case SessionState::Ready:
    case SessionState::Disconnected:
    case SessionState::Failed:
        break;
    }
}

} // namespace bf
