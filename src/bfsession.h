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
    ProbingMsp,    // checking VTX capability before entering the CLI
    AwaitingVtxChoice,
    ApplyingVtxGuard,
    EnteringCli,   // '#' sent, waiting for the CLI banner or a prompt
    Ready,         // sitting at the "# " prompt
    Busy,          // a command is running
    Rebooting,     // `exit` queued so saved VTX state can reload
    Failed,
};

enum class VtxBenchMode { None, PitMode, Unconfirmed };

struct VtxStatus {
    bool valid = false;
    uint8_t deviceType = 0;
    uint8_t band = 0;
    uint8_t channel = 0;
    uint8_t power = 0;
    bool pitMode = false;
    uint16_t frequency = 0;
    bool deviceReady = false;
    uint8_t lowPowerDisarm = 0;
    bool tableAvailable = false;
    uint8_t powerLevels = 0;

    std::string deviceLabel() const;
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

    // Before entering CLI mode, Betaflight still runs its VTX control loop.
    // A capable device can therefore enter its purpose-built pit mode, then
    // hold that state while the CLI owns the FC.
    bool awaitingVtxChoice() const { return state_ == SessionState::AwaitingVtxChoice; }
    const VtxStatus& vtxStatus() const { return vtxOriginal_; }
    void enableVtxBenchGuard();
    void skipVtxBenchGuard();
    bool vtxBenchGuardActive() const { return vtxBenchMode_ != VtxBenchMode::None; }
    VtxBenchMode vtxBenchMode() const { return vtxBenchMode_; }
    const std::string& vtxGuardNote() const { return vtxGuardNote_; }
    uint64_t vtxGuardNoteSequence() const { return vtxGuardNoteSequence_; }

    // The guard is intentionally unsaved. Rebooting through `exit` reloads the
    // FC's stored VTX configuration and also leaves CLI mode flight-cleanly.
    bool restoreVtxAndDisconnect();

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

    bool coreTemperatureAvailable() const { return coreTemperatureAvailable_; }
    int coreTemperatureC() const { return coreTemperatureC_; }
    uint64_t coreTemperatureSequence() const { return coreTemperatureSequence_; }

private:
    enum class MspAction {
        None,
        ProbeVtx,
        SetPit,
        WaitPit,
        VerifyPit,
        SetPitOff,
        WaitPitOff,
        VerifyPitOff,
    };

    bool atPrompt() const;
    bool commandComplete(uint64_t now) const;
    void beginCommand(const std::string& line, uint64_t now);
    void pumpRestore(uint64_t now);
    void finishJob(bool ok, const std::string& message);
    void finishCapture(bool ok);
    void loseLink(const std::string& reason, const char* rebootNote);
    void noteVtxGuard(const std::string& note, LineKind kind);
    void scanForIdentity(const std::string& text);
    void beginCli(uint64_t now);
    void queueMsp(uint8_t command, const std::vector<uint8_t>& payload = {});
    void processMspInput(uint64_t now);
    void handleMspFrame(uint8_t direction, uint8_t command,
                        const std::vector<uint8_t>& payload, uint64_t now);
    bool parseVtxStatus(const std::vector<uint8_t>& payload, VtxStatus& status) const;
    std::vector<uint8_t> vtxSetPayload(uint8_t power, bool pitMode) const;
    void requestVtxStatus(MspAction action, uint64_t now);
    void startPitRollback(uint64_t now);
    void finishVtxGuard(VtxBenchMode mode, const std::string& note, uint64_t now);
    void noteCoreTemperature(const std::vector<TermLine>& lines);

    Terminal& term_;
    Completer& completer_;
    SerialPort port_;
    SessionState state_ = SessionState::Disconnected;

    uint64_t lastByteMs_ = 0;
    uint64_t commandSentMs_ = 0;
    uint64_t linesAtSend_ = 0;
    uint64_t connectStartMs_ = 0;
    uint64_t mspDeadlineMs_ = 0;
    uint64_t rebootStartedMs_ = 0;
    int cliAttempts_ = 0;
    bool linkLost_ = false;
    MspAction mspAction_ = MspAction::None;
    std::string mspInput_;

    VtxStatus vtxOriginal_;
    VtxBenchMode vtxBenchMode_ = VtxBenchMode::None;
    std::string vtxGuardNote_;
    uint64_t vtxGuardNoteSequence_ = 0;

    bool coreTemperatureAvailable_ = false;
    int coreTemperatureC_ = 0;
    uint64_t coreTemperatureSequence_ = 0;

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
