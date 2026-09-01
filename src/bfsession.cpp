#include "bfsession.h"
#include "input.h"

#include <algorithm>
#include <cctype>
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

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

} // namespace

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
    state_ = SessionState::EnteringCli;
    connectStartMs_ = nowMs();
    lastByteMs_ = connectStartMs_;
    cliAttempts_ = 1;
    // '#' is how Betaflight leaves MSP mode and enters the CLI. If it is
    // already in the CLI the same byte is just a comment, so this is safe to
    // send either way. One newline, never CRLF: Betaflight ends a line on
    // either character and prints a prompt for each, so CRLF gives two.
    port_.write("#\n");
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

void Session::poll(uint64_t now) {
    if (!port_.isOpen()) return;

    std::string err;
    if (!port_.flush(err)) {
        linkLost_ = true;
        term_.addLine("-- link lost (" + err + ") --", LineKind::Error);
        port_.close();
        state_ = SessionState::Disconnected;
        if (job_.active()) finishJob(false, "Link lost");
        return;
    }

    std::string incoming;
    const int n = port_.read(incoming);
    if (n < 0) {
        linkLost_ = true;
        term_.addLine("-- link lost (device disconnected) --", LineKind::Error);
        port_.close();
        state_ = SessionState::Disconnected;
        if (job_.active()) finishJob(false, "Link lost");
        return;
    }
    if (n > 0) {
        lastByteMs_ = now;
        const uint64_t linesBefore = term_.linesEver();
        term_.feed(incoming);
        // Cheap running harvest: every parameter name that scrolls past becomes
        // a completion candidate, so no extra query is ever needed.
        // Locate new lines by their monotonic arrival count. lineCount() can
        // shrink in the same feed when a full scrollback trims old rows.
        const uint64_t added = term_.linesEver() - linesBefore;
        const size_t survivingNew = static_cast<size_t>(
            std::min<uint64_t>(added, static_cast<uint64_t>(term_.lineCount())));
        const size_t firstNew = term_.lineCount() - survivingNew;
        for (size_t i = firstNew; i < term_.lineCount(); ++i) {
            const TermLine& l = term_.line(i);
            if (l.text.find('=') != std::string::npos) completer_.harvest(l.text);
            if (job_.kind == JobKind::Restore && isErrorLine(l.text)) ++restoreErrors_;
            if (firmware_.empty() && l.text.find("Betaflight /") != std::string::npos) {
                scanForIdentity(l.text);
            }
        }
    }

    switch (state_) {
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
                std::string captured = term_.captureSince();
                stripEchoedCommand(captured, captureCommand_);
                noteDumpText(captured);
                auto cb = captureDone_;
                captureDone_ = nullptr;
                finishJob(true, "Captured " + std::to_string(captured.size()) + " bytes");
                if (cb) cb(true, captured);
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
                auto cb = captureDone_;
                captureDone_ = nullptr;
                finishJob(false, "Timed out waiting for the FC");
                state_ = SessionState::Ready;
                std::string partial = term_.captureSince();
                stripEchoedCommand(partial, captureCommand_);
                if (cb) cb(false, partial);
            } else {
                term_.addLine("-- no response; returning to the prompt --", LineKind::Warn);
                state_ = SessionState::Ready;
            }
        }
        break;
    }

    case SessionState::Ready:
    case SessionState::Disconnected:
    case SessionState::Failed:
        break;
    }
}

} // namespace bf
