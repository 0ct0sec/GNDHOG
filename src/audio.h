#pragma once

#include <cstdint>
#include <cerrno>
#include <memory>
#include <string>
#include <vector>

namespace bf {

enum class HudCue : std::uint8_t {
    Startup,
    Navigate,
    Select,
    Back,
    Prompt,
    Success,
    Error,
    LinkUp,
    LinkDown,
    Critical,
    Command,
};

struct AudioDeviceInfo {
    bool cardPresent = false;
    bool playbackPresent = false;
    int cardNumber = -1;
    int playbackDevice = -1;
    std::string cardId;
    std::string cardName;
    std::string pcmName;
    std::string mixerName;
    std::vector<std::string> mixerElements;
};

// Exact-card discovery deliberately ignores HDMI and unrelated USB audio.
AudioDeviceInfo discoverCardputerZeroAudio(
    const std::string& procAsoundRoot = "/proc/asound",
    const std::string& devSndRoot = "/dev/snd");

// Deterministic stereo 48 kHz PCM used by both playback and self-tests.
std::vector<std::int16_t> synthesizeHudCue(HudCue cue);

// Start ALSA's nonblocking drain once, then observe DRAINING -> SETUP. Calling
// drain again after EAGAIN can restart this codec driver's silence tail.
template <class StartDrain, class ReadState, class CanWait, class Pause>
int finishNonblockingAudioDrain(StartDrain startDrain, ReadState readState,
                                CanWait canWait, Pause pause) {
    const int started = startDrain();
    if (started != -EAGAIN) return started;
    for (;;) {
        const int state = readState();
        if (state == 1) return 0;       // SND_PCM_STATE_SETUP
        if (state != 5) return -EIO;   // SND_PCM_STATE_DRAINING
        if (!canWait()) return -ETIMEDOUT;
        pause();
    }
}

class Audio {
public:
    Audio();
    ~Audio();

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool start(const std::string& procAsoundRoot = "/proc/asound",
               const std::string& devSndRoot = "/dev/snd");
    void shutdown();
    void setEnabled(bool enabled);
    void setVolume(int percent);
    void play(HudCue cue);
    bool waitIdle(int timeoutMs);

    bool available() const;
    bool enabled() const;
    int volume() const;
    std::string backendName() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bf
