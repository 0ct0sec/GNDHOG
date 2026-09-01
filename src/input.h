#pragma once
#include "keys.h"

#include <cstddef>
#include <string>
#include <vector>

namespace bf {

struct InputDeviceInfo {
    std::string path;
    std::string name;
    bool isCardputer = false;   // the TCA8418 built-in keyboard
};

// Opens every keyboard-capable evdev node and merges them into one key stream.
// The built-in TCA8418 is found by name, never by a hard-coded event number,
// and its name is re-checked after opening. A plain USB keyboard on the hub
// works too: it sends no MSC_SCAN, so those events fall back to keycode-only
// decoding.
class Keyboard {
public:
    ~Keyboard();

    // Discovers and opens devices. Returns the number opened; zero is not an
    // error (the app can still be driven from stdin on a dev host).
    int open(std::string& error);
    void close();
    bool anyOpen() const { return !devices_.empty(); }

    const std::vector<InputDeviceInfo>& devices() const { return infos_; }
    std::vector<int> fds() const;

    // Drains all ready events. Call after poll() reports readable, or on a
    // timer -- reads are non-blocking either way.
    void pump(uint64_t nowMs, std::vector<KeyEvent>& out);
    // Software autorepeat tick; the v5 overlay does not enable the kernel's.
    void pumpRepeat(uint64_t nowMs, std::vector<KeyEvent>& out);

    void releaseAll() { decoder_.releaseAll(); }
    KeyDecoder& decoder() { return decoder_; }

    // Reads stdin as a fallback when no evdev node could be opened.
    void enableStdinFallback(bool on) { stdinFallback_ = on; }
    void pumpStdin(std::vector<KeyEvent>& out);

private:
    struct Device {
        int fd = -1;
        bool cardputer = false;
    };
    std::vector<Device> devices_;
    std::vector<InputDeviceInfo> infos_;
    KeyDecoder decoder_;
    bool stdinFallback_ = false;
};

// Milliseconds from CLOCK_MONOTONIC.
uint64_t nowMs();
// Sleeps without burning CPU; used to pace the frame loop.
void sleepMs(int ms);

// Puts the terminal into raw mode for the stdin fallback, restoring it on exit.
class RawTerminalMode {
public:
    explicit RawTerminalMode(bool enable, int fd = 0);
    ~RawTerminalMode();
    RawTerminalMode(const RawTerminalMode&) = delete;
    RawTerminalMode& operator=(const RawTerminalMode&) = delete;

    bool active() const { return active_; }

private:
    bool active_ = false;
    int fd_ = -1;
    int savedFlags_ = -1;
    alignas(std::max_align_t) unsigned char saved_[64] = {0};
};

} // namespace bf
