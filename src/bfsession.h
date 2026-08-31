#pragma once
#include "bfcommands.h"
#include "serialport.h"
#include "term.h"

#include <functional>
#include <string>
#include <vector>

namespace bf {

enum class SessionState {
    Disconnected,
    EnteringCli,   // '#' sent, waiting for the CLI banner or a prompt
    Ready,         // sitting at the "# " prompt
    Busy,          // a command is running
    Failed,
};

enum class JobKind { None, Capture, Restore };

struct JobStatus {
    JobKind kind = JobKind::None;
    std::string label;
    int done = 0;
    int total = 0;
    bool finished = false;
    bool ok = false;
    std::string message;
    int errorCount = 0;

    bool active() const { return kind != JobKind::None && !finished; }
    float fraction() const { return total > 0 ? static_cast<float>(done) / total : 0.0f; }
};

// Owns the serial link and the Betaflight CLI conversation on top of it.
//
// Betaflight's CLI echoes everything it receives and ends every response with a
// "# " prompt, so the command boundary is: the prompt is showing, at least one
// new line has arrived since the command was sent, and the link has been quiet
// briefly. The quiet period matters because a `diff` streams comment lines that
// momentarily look like a bare prompt while only "# " has arrived.
class Session {
public:
    explicit Session(Terminal& term, Completer& completer);

    bool connect(const std::string& device, int baud, std::string& error);
    void disconnect();
    bool connected() const { return port_.isOpen(); }
    SessionState state() const { return state_; }
    const std::string& device() const { return port_.device(); }
    int fd() const { return port_.fd(); }

    // Drives the state machine. Call every frame; never blocks.
    void poll(uint64_t now);

    // Queues a command exactly as typed. Returns false when the link is busy.
    bool send(const std::string& line);
    bool ready() const { return state_ == SessionState::Ready; }
    bool busy() const { return state_ == SessionState::Busy; }

    // Runs `command`, captures its full output, then hands it to `onDone`.
    bool startCapture(const std::string& command, const std::string& label,
                      std::function<void(bool ok, const std::string& text)> onDone);
    // Plays a saved backup back to the FC, one line at a time.
    bool startRestore(const std::vector<std::string>& lines, const std::string& label);
    void cancelJob();

    const JobStatus& job() const { return job_; }
    void clearFinishedJob();

    // Identity learned from `version` / `status`, empty until seen.
    const std::string& firmware() const { return firmware_; }
    const std::string& board() const { return board_; }
    const std::string& craft() const { return craft_; }
    void noteDumpText(const std::string& text);

    // Wall-clock ms since the last byte arrived, for the link indicator.
    uint64_t idleMs(uint64_t now) const { return now > lastByteMs_ ? now - lastByteMs_ : 0; }
    bool linkLost() const { return linkLost_; }

private:
    bool atPrompt() const;
    bool commandComplete(uint64_t now) const;
    void beginCommand(const std::string& line, uint64_t now);
    void pumpRestore(uint64_t now);
    void finishJob(bool ok, const std::string& message);
    void scanForIdentity(const std::string& text);

    Terminal& term_;
    Completer& completer_;
    SerialPort port_;
    SessionState state_ = SessionState::Disconnected;

    uint64_t lastByteMs_ = 0;
    uint64_t commandSentMs_ = 0;
    uint64_t linesAtSend_ = 0;
    uint64_t connectStartMs_ = 0;
    int cliAttempts_ = 0;
    bool linkLost_ = false;

    JobStatus job_;
    std::function<void(bool, const std::string&)> captureDone_;
    std::string captureCommand_;
    std::vector<std::string> restoreLines_;
    size_t restoreIndex_ = 0;
    int restoreErrors_ = 0;

    std::string firmware_, board_, craft_;
};

// Splits a backup file into the lines worth sending: blank lines and pure
// comments are dropped, since Betaflight ignores them and they triple the
// transfer time over a 46-key session the user is watching.
std::vector<std::string> restorableLines(const std::string& fileText);

// True when a line of FC output reports a rejected command.
bool isErrorLine(const std::string& line);

} // namespace bf
