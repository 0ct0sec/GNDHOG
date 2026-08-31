#include "app.h"
#include "brand.h"
#include "serialport.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace bf {
int runSelfTest();   // selftest.cpp
}

namespace {

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
        else if (a == "--baud") opt.baud = std::atoi(next("--baud").c_str());
        else if (a == "--fb") opt.fbDevice = next("--fb");
        else if (a == "--headless") opt.headless = true;
        else if (a == "--stdin") opt.stdinKeys = true;
        else if (a == "--sim") opt.simulate = true;
        else if (a == "--no-autoconnect") opt.autoConnect = false;
        else if (a == "--frames") opt.frameLimit = std::atoi(next("--frames").c_str());
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

    bf::App app;
    return app.run(opt);
}
