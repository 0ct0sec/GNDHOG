#include "input.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace bf {

uint64_t nowMs() {
    timespec ts{};
#if defined(CLOCK_MONOTONIC)
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    ::clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull + ts.tv_nsec / 1000000ull;
}

void sleepMs(int ms) {
    if (ms <= 0) return;
    timespec ts{ms / 1000, static_cast<long>(ms % 1000) * 1000000L};
    ::nanosleep(&ts, nullptr);
}

Keyboard::~Keyboard() { close(); }

std::vector<int> Keyboard::fds() const {
    std::vector<int> out;
    out.reserve(devices_.size());
    for (const Device& d : devices_) out.push_back(d.fd);
    return out;
}

int Keyboard::open(std::string& error) {
#if defined(__linux__)
    close();
    DIR* dir = ::opendir("/dev/input");
    if (!dir) {
        error = std::string("/dev/input: ") + std::strerror(errno);
        return 0;
    }
    std::vector<std::string> nodes;
    while (dirent* e = ::readdir(dir)) {
        const std::string name = e->d_name;
        if (name.rfind("event", 0) == 0) nodes.push_back("/dev/input/" + name);
    }
    ::closedir(dir);
    std::sort(nodes.begin(), nodes.end());

    int denied = 0;
    for (const std::string& node : nodes) {
        const int fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (errno == EACCES) ++denied;
            continue;
        }
        // Keep only devices that actually produce letter keys, so mice, lid
        // switches and power buttons are not mistaken for keyboards.
        unsigned long keyBits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1] = {0};
        if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0) {
            ::close(fd);
            continue;
        }
        auto hasKey = [&keyBits](int code) {
            return (keyBits[code / (8 * sizeof(unsigned long))] >>
                    (code % (8 * sizeof(unsigned long)))) & 1ul;
        };
        if (!hasKey(KEY_A) || !hasKey(KEY_Z) || !hasKey(KEY_ENTER)) {
            ::close(fd);
            continue;
        }

        char nameBuf[256] = {0};
        if (::ioctl(fd, EVIOCGNAME(sizeof(nameBuf) - 1), nameBuf) < 0) nameBuf[0] = '\0';
        std::string devName = nameBuf;
        std::string lowerName = devName;
        for (char& c : lowerName) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

        Device d;
        d.fd = fd;
        d.cardputer = lowerName.find("tca8418") != std::string::npos;
        devices_.push_back(d);
        infos_.push_back(InputDeviceInfo{node, devName, d.cardputer});
    }

    if (devices_.empty()) {
        error = denied > 0
                    ? "no readable keyboard (permission denied; add the user to the input group)"
                    : "no keyboard-capable /dev/input/event* device found";
    }
    return static_cast<int>(devices_.size());
#else
    error = "evdev input is only available on Linux";
    return 0;
#endif
}

void Keyboard::close() {
#if defined(__linux__)
    for (Device& d : devices_) {
        if (d.fd >= 0) ::close(d.fd);
    }
#endif
    devices_.clear();
    infos_.clear();
    decoder_.reset();
}

void Keyboard::pump(uint64_t now, std::vector<KeyEvent>& out) {
#if defined(__linux__)
    input_event ev{};
    for (Device& d : devices_) {
        for (;;) {
            const ssize_t n = ::read(d.fd, &ev, sizeof(ev));
            if (n != static_cast<ssize_t>(sizeof(ev))) break;

            if (ev.type == EV_MSC && ev.code == MSC_SCAN) {
                decoder_.noteScan(static_cast<int>(ev.value));
                continue;
            }
            if (ev.type != EV_KEY) continue;

            KeyEvent out_ev;
            if (decoder_.onKey(static_cast<int>(ev.code), ev.value, now, out_ev)) {
                out.push_back(out_ev);
            }
        }
    }
#else
    (void)now;
    (void)out;
#endif
}

void Keyboard::pumpRepeat(uint64_t now, std::vector<KeyEvent>& out) {
    KeyEvent ev;
    while (decoder_.pollRepeat(now, ev)) out.push_back(ev);
}

void Keyboard::pumpStdin(std::vector<KeyEvent>& out) {
#if defined(__linux__)
    if (!stdinFallback_) return;
    char buf[64];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return;
    for (ssize_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        KeyEvent e;
        // Decode the handful of escape sequences a dev terminal sends.
        if (c == 0x1b && i + 2 < n && buf[i + 1] == '[') {
            switch (buf[i + 2]) {
            case 'A': e.key = Key::Up; break;
            case 'B': e.key = Key::Down; break;
            case 'C': e.key = Key::Right; break;
            case 'D': e.key = Key::Left; break;
            case 'H': e.key = Key::Home; break;
            case 'F': e.key = Key::End; break;
            case '5': e.key = Key::PageUp; break;
            case '6': e.key = Key::PageDown; break;
            default: break;
            }
            if (e.valid()) {
                i += 2;
                if (buf[i] == '5' || buf[i] == '6') ++i;   // consume the trailing ~
                out.push_back(e);
                continue;
            }
        }
        if (c == 0x1b) { e.key = Key::Escape; }
        else if (c == '\r' || c == '\n') { e.key = Key::Enter; }
        else if (c == 0x7F || c == '\b') { e.key = Key::Backspace; }
        else if (c == '\t') { e.key = Key::Tab; }
        else if (c < 0x20) {
            e.key = Key::Char;
            e.ctrl = true;
            e.ch = static_cast<char>('a' + c - 1);
        } else if (c < 0x7F) {
            e.key = Key::Char;
            e.ch = static_cast<char>(c);
        }
        if (e.valid()) out.push_back(e);
    }
#else
    (void)out;
#endif
}

// ------------------------------------------------------------ raw tty mode

RawTerminalMode::RawTerminalMode(bool enable, int fd) : fd_(fd) {
#if defined(__linux__)
    static_assert(sizeof(termios) <= sizeof(saved_), "termios does not fit");
    if (!enable || fd_ < 0 || !::isatty(fd_)) return;
    termios* saved = reinterpret_cast<termios*>(saved_);
    if (::tcgetattr(fd_, saved) != 0) return;
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) return;
    termios raw = *saved;
    ::cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(fd_, TCSANOW, &raw) != 0) return;
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::tcsetattr(fd_, TCSANOW, saved);
        return;
    }
    savedFlags_ = flags;
    active_ = true;
#else
    (void)enable;
    (void)fd;
#endif
}

RawTerminalMode::~RawTerminalMode() {
#if defined(__linux__)
    if (!active_) return;
    ::tcsetattr(fd_, TCSANOW, reinterpret_cast<termios*>(saved_));
    if (savedFlags_ >= 0) ::fcntl(fd_, F_SETFL, savedFlags_);
#endif
}

} // namespace bf
