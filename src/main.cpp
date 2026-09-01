#include "app.h"
#include "brand.h"
#include "serialport.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace bf {
int runSelfTest();   // selftest.cpp
}

namespace {

bool parsePositiveInt(const std::string& text, const char* option, int& value) {
    int parsed = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end || parsed <= 0) {
        std::fprintf(stderr,
                     "%s: %s needs a positive decimal integer (got `%s`)\n",
                     bf::kAppName, option, text.c_str());
        return false;
    }
    value = parsed;
    return true;
}

void printUnsupportedBaud(int baud) {
    std::fprintf(stderr, "%s: unsupported baud rate %d (choose", bf::kAppName, baud);
    for (int i = 0; i < bf::kBaudChoiceCount; ++i) {
        std::fprintf(stderr, "%s%d", i == 0 ? " " : ", ", bf::kBaudChoices[i]);
    }
    std::fprintf(stderr, ")\n");
}

void usage() {
    std::printf(
        "GNDHOG ZERO - Betaflight CLI for the M5Stack Cardputer Zero\n"
        "\n"
        "Usage: bfcli [options]\n"
        "\n"
        "  --port DEV        connect to DEV instead of showing the port picker\n"
        "  --baud N          baud rate for a UART port (default 115200)\n"
        "  --fb DEV          framebuffer device (default /dev/fb0)\n"
        "  --headless        render offscreen only, no framebuffer\n"
        "  --stdin           read keys from stdin instead of evdev\n"
        "  --sim             talk to a built-in simulated flight controller\n"
        "  --mute            disable HUD sounds for this launch\n"
        "  --sound-test      play one bounded HUD startup cue and exit\n"
        "  --no-autoconnect  always show the port picker at startup\n"
        "  --frames N        exit after N rendered frames\n"
        "  --preview DIR     write one PPM per screen to DIR and exit\n"
        "  --list-ports      print the detected serial ports and exit\n"
        "  --selftest        run the built-in checks and exit\n"
        "  --about           open the About screen without connecting an FC\n"
        "  --version         print the author and source commit, then exit\n"
        "  --help            this text\n"
        "\n"
        "Data (backups, history, config) lives under $BFCLI_DATA_DIR, or\n"
        "$XDG_DATA_HOME/bfcli, defaulting to ~/.local/share/bfcli.\n");
}

int listPorts() {
    const std::vector<bf::PortInfo> ports = bf::enumeratePorts();
    if (ports.empty()) {
        std::printf("no serial ports found\n");
        return 1;
    }
    for (const bf::PortInfo& p : ports) {
        std::printf("%-20s %-34s %s\n", p.device.c_str(), p.product.c_str(),
                    p.detail().c_str());
        if (!p.byId.empty()) std::printf("  by-id: %s\n", p.byId.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bf::App::Options opt;
    bool soundTest = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: %s needs a value\n", bf::kAppName, what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--version") {
            std::printf("%s | author %s | commit %s\n", bf::kAppName,
                        bf::kAuthor, bf::kBuildCommit);
            return 0;
        }
        else if (a == "--about") opt.showAbout = true;
        else if (a == "--selftest") return bf::runSelfTest();
        else if (a == "--list-ports") return listPorts();
        else if (a == "--port") opt.portOverride = next("--port");
        else if (a == "--baud") {
            if (!parsePositiveInt(next("--baud"), "--baud", opt.baud)) return 2;
            if (!bf::isSupportedBaud(opt.baud)) {
                printUnsupportedBaud(opt.baud);
                return 2;
            }
        }
        else if (a == "--fb") opt.fbDevice = next("--fb");
        else if (a == "--headless") opt.headless = true;
        else if (a == "--stdin") opt.stdinKeys = true;
        else if (a == "--sim") opt.simulate = true;
        else if (a == "--mute") opt.muteSound = true;
        else if (a == "--sound-test") soundTest = true;
        else if (a == "--no-autoconnect") opt.autoConnect = false;
        else if (a == "--frames") {
            if (!parsePositiveInt(next("--frames"), "--frames", opt.frameLimit)) return 2;
        }
        else if (a == "--preview") {
            opt.previewDir = next("--preview");
            opt.headless = true;
            opt.autoConnect = false;
        } else {
            std::fprintf(stderr, "%s: unknown option %s (try --help)\n",
                         bf::kAppName, a.c_str());
            return 2;
        }
    }

    if (soundTest) {
        bf::Audio audio;
        audio.setEnabled(true);
        audio.setVolume(70);
        if (!audio.start()) {
            std::fprintf(stderr, "%s: HUD audio unavailable: %s\n",
                         bf::kAppName, audio.lastError().c_str());
            return 1;
        }
        audio.play(bf::HudCue::Startup);
        const bool finished = audio.waitIdle(3000);
        const std::string error = audio.lastError();
        const std::string backend = audio.backendName();
        audio.shutdown();
        if (!finished || !error.empty()) {
            std::fprintf(stderr, "%s: HUD sound test failed: %s\n", bf::kAppName,
                         !error.empty() ? error.c_str() : "playback deadline expired");
            return 1;
        }
        std::printf("HUD sound test passed: %s, volume 70%%\n", backend.c_str());
        return 0;
    }

    bf::App app;
    return app.run(opt);
}
