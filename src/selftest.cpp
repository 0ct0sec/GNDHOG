#include "app.h"
#include "audio.h"
#include "brand.h"
#include "mascot.h"
#include "bfcommands.h"
#include "bfsession.h"
#include "font6x8.h"
#include "gfx.h"
#include "input.h"
#include "gnss.h"
#include "keys.h"
#include "meshsession.h"
#include "meshtastic.h"
#include "protowire.h"
#include "simfc.h"
#include "simmesh.h"
#include "storage.h"
#include "term.h"
#include "thermaltrip.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace bf {
namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool cond, const std::string& what) {
    ++gChecks;
    if (cond) return;
    ++gFailures;
    std::printf("  FAIL  %s\n", what.c_str());
}

void checkEq(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + " (got \"" + got + "\", want \"" + want + "\")");
}

void section(const char* name) { std::printf("%s\n", name); }

#if defined(__linux__)
bool fixtureDir(const std::string& path) {
    return ::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

bool fixtureFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return out.good();
}

std::string fixtureRead(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string content;
    std::getline(in, content, '\0');
    return content;
}

struct ThermalFixture {
    ThermalTripPaths paths;
    std::string root;
    std::string device = "/dev/ttyACM0";
    std::string muxPath;
    std::string railPath;
    bool ok = false;
};

ThermalFixture makeThermalFixture(const std::string& branch, bool muxUsb = true,
                                  bool railOn = true) {
    static int serial = 0;
    ThermalFixture f;
    f.root = "/tmp/bfcli-thermal-selftest-" + std::to_string(::getpid()) + "-" +
             std::to_string(++serial);
    f.paths.ttyClass = f.root + "/class/tty";
    f.paths.ledClass = f.root + "/class/leds";
    const std::string hub = f.root + "/devices/platform/usb/1-1";
    const std::string fc = hub + "/1-1." + branch;
    const std::string iface = fc + "/1-1." + branch + ":1.0";
    const std::string tty = iface + "/tty/ttyACM0";
    const std::string ttyClass = f.paths.ttyClass + "/ttyACM0";
    const std::string muxDir = f.paths.ledClass + "/ext_usb_gpio_fun";
    const std::string railDir = f.paths.ledClass + "/ext_5v_out";
    f.muxPath = muxDir + "/brightness";
    f.railPath = railDir + "/brightness";

    const std::string dirs[] = {
        f.root,
        f.root + "/class",
        f.paths.ttyClass,
        ttyClass,
        f.paths.ledClass,
        muxDir,
        railDir,
        f.root + "/devices",
        f.root + "/devices/platform",
        f.root + "/devices/platform/usb",
        hub,
        fc,
        iface,
        iface + "/tty",
        tty,
    };
    for (const std::string& dir : dirs) {
        if (!fixtureDir(dir)) return f;
    }
    if (!fixtureFile(hub + "/idVendor", "05e3\n") ||
        !fixtureFile(hub + "/idProduct", "0610\n") ||
        !fixtureFile(fc + "/idVendor", "0483\n") ||
        !fixtureFile(fc + "/idProduct", "5740\n") ||
        !fixtureFile(f.muxPath, muxUsb ? "1" : "0") ||
        !fixtureFile(f.railPath, railOn ? "1" : "0")) {
        return f;
    }
    if (::symlink(tty.c_str(), (ttyClass + "/device").c_str()) != 0) return f;
    f.ok = true;
    return f;
}
#endif

void testAudio() {
    section("audio");
    const HudCue cues[] = {
        HudCue::Startup, HudCue::Navigate, HudCue::Select, HudCue::Back,
        HudCue::Prompt, HudCue::Success, HudCue::Error, HudCue::LinkUp,
        HudCue::LinkDown, HudCue::Critical, HudCue::Command,
    };
    for (HudCue cue : cues) {
        const std::vector<std::int16_t> pcm = synthesizeHudCue(cue);
        int peak = 0;
        for (std::int16_t sample : pcm) peak = std::max(peak, std::abs(static_cast<int>(sample)));
        check(!pcm.empty() && pcm.size() % 2 == 0,
              "HUD cue is non-empty interleaved stereo PCM");
        check(peak > 100 && peak < 12000, "HUD cue has bounded non-silent sample amplitude");
        check(pcm.size() <= 48000U * 2U, "HUD cue stays below one second");
    }
    check(synthesizeHudCue(HudCue::Navigate).size() < 48000U * 2U / 10U,
          "navigation chirp stays below 100 ms");
    int drainCalls = 0;
    int stateReads = 0;
    const int drained = finishNonblockingAudioDrain(
        [&] { ++drainCalls; return -EAGAIN; },
        [&] { return ++stateReads < 3 ? 5 : 1; },
        [] { return true; }, [] {});
    check(drained == 0 && drainCalls == 1 && stateReads == 3,
          "nonblocking audio drain starts once and polls DRAINING to SETUP");
    check(finishNonblockingAudioDrain([] { return -EAGAIN; }, [] { return 5; },
                                      [] { return false; }, [] {}) == -ETIMEDOUT,
          "nonblocking audio drain has a bounded timeout");

#if defined(__linux__)
    const std::string root = "/tmp/bfcli-audio-selftest-" + std::to_string(::getpid());
    const std::string proc = root + "/proc";
    const std::string dev = root + "/dev";
    std::filesystem::remove_all(root);
    check(fixtureDir(root) && fixtureDir(proc) && fixtureDir(dev),
          "audio discovery fixture directories are created");
    check(fixtureFile(
              proc + "/cards",
              " 0 [vc4hdmi        ]: vc4-hdmi - vc4-hdmi\n"
              "                      vc4-hdmi\n"
              " 1 [ES8389Audio    ]: cardputerzero-a - ES8389-Audio\n"
              "                      ES8389-Audio\n") &&
              fixtureFile(dev + "/pcmC0D0p", "") &&
              fixtureFile(dev + "/pcmC1D0p", ""),
          "audio discovery fixture describes HDMI and ES8389 playback");
    const AudioDeviceInfo exact = discoverCardputerZeroAudio(proc, dev);
    check(exact.cardPresent && exact.playbackPresent && exact.cardNumber == 1 &&
              exact.cardId == "ES8389Audio" &&
              exact.pcmName == "default:CARD=ES8389Audio" &&
              exact.pulseSinkName == "alsa_output.platform-sound.stereo-fallback",
          "audio discovery selects exact ES8389 session and ALSA routes instead of HDMI");
    check(exact.mixerElements.size() == 2 && exact.mixerElements[0] == "DACL" &&
               exact.mixerElements[1] == "DACR",
          "ES8389 playback requires both DAC mixer channels");
    std::filesystem::remove(dev + "/pcmC1D0p");
    check(fixtureFile(
              proc + "/cards",
              " 1 [ES8388Audio    ]: cardputerzero-a - ES8388-Audio\n"
              " 2 [ES8389Audio    ]: cardputerzero-a - ES8389-Audio\n") &&
              fixtureFile(dev + "/pcmC2D0junkp", "") &&
              fixtureFile(dev + "/pcmC2D1p", ""),
          "audio discovery fixture includes a stale codec and malformed PCM node");
    const AudioDeviceInfo fallback = discoverCardputerZeroAudio(proc, dev);
    check(fallback.cardPresent && fallback.playbackPresent && fallback.cardNumber == 2 &&
              fallback.playbackDevice == 1 && fallback.pcmName == "hw:ES8389Audio,1",
          "audio discovery keeps an explicit route for a nonzero codec PCM");
    check(fixtureFile(proc + "/cards",
                      " 0 [vc4hdmi        ]: vc4-hdmi - vc4-hdmi\n") &&
              !discoverCardputerZeroAudio(proc, dev).cardPresent,
          "generic HDMI audio is never promoted to the Cardputer speaker backend");
    std::filesystem::remove_all(root);
#endif
}

// ---------------------------------------------------------------- keyboard

// Presses one physical key by scan code and the keycode that layer emits.
KeyEvent press(KeyDecoder& d, int scan, int code, uint64_t now = 1000) {
    KeyEvent ev;
    d.noteScan(scan);
    d.onKey(code, 1, now, ev);
    return ev;
}

void testKeys() {
    section("keys");
    KeyDecoder d;
    d.reset();

    // Base layer.
    checkEq(std::string(1, press(d, 0x28, 38).ch), "l", "base scan 0x28 -> l");
    checkEq(std::string(1, press(d, 0x20, 30).ch), "a", "base scan 0x20 -> a");
    checkEq(std::string(1, press(d, 0x44, 2).ch), "1", "base scan 0x44 -> 1");

    // The two characters Betaflight cannot be driven without.
    KeyEvent underscore = press(d, 0x28, 89);
    check(underscore.layer == Layer::Sym, "Sym+L is recognised as the Sym layer");
    checkEq(std::string(1, underscore.ch), "_", "Sym+L -> underscore");
    checkEq(std::string(1, press(d, 0x08, 94).ch), "=", "Sym+0 -> equals");

    // The rest of the Sym layer that has unambiguous keycodes.
    checkEq(std::string(1, press(d, 0x16, 74).ch), "-", "Sym+U -> minus");
    checkEq(std::string(1, press(d, 0x07, 53).ch), "/", "Sym+9 -> slash");
    checkEq(std::string(1, press(d, 0x06, 52).ch), ".", "Sym+8 -> dot");
    checkEq(std::string(1, press(d, 0x05, 51).ch), ",", "Sym+7 -> comma");
    checkEq(std::string(1, press(d, 0x24, 82).ch), "0", "Sym+G -> keypad 0");
    checkEq(std::string(1, press(d, 0x41, 78).ch), "+", "Sym+Shift -> plus");

    // Every Sym position that produces a character must be unique enough to be
    // worth showing on the keymap screen.
    int symChars = 0;
    for (int i = 0; i < descriptorCount(); ++i) {
        const KeyDescriptor* k = descriptorAt(i);
        if (k && d.symChar(k->scan) != 0) ++symChars;
    }
    check(symChars >= 28, "the Sym layer covers at least 28 characters");

    // Fn layer navigation.
    check(press(d, 0x23, 103).key == Key::Up, "Fn+F -> Up");
    check(press(d, 0x33, 108).key == Key::Down, "Fn+X -> Down");
    check(press(d, 0x32, 105).key == Key::Left, "Fn+Z -> Left");
    check(press(d, 0x34, 106).key == Key::Right, "Fn+C -> Right");
    check(press(d, 0x28, 104).key == Key::PageUp, "Fn+L -> PageUp");
    check(press(d, 0x09, 111).key == Key::Delete, "Fn+Backspace -> Delete");
    check(press(d, 0x44, 59).key == Key::F1, "Fn+1 -> F1");
    check(press(d, 0x08, 68).key == Key::F10, "Fn+0 -> F10");

    // Escape reaches userspace from every table.
    check(press(d, 0x40, 1).key == Key::Escape, "Esc -> Escape");

    // Shift via ASmux.
    d.reset();
    KeyEvent ignored;
    d.noteScan(0x41);
    d.onKey(42, 1, 1000, ignored);
    check(d.shift(), "ASmux press sets shift");
    checkEq(std::string(1, press(d, 0x20, 30).ch), "A", "Shift+A -> A");
    checkEq(std::string(1, press(d, 0x16, 74).ch), "_", "Shift+Sym+U -> underscore too");
    d.noteScan(0x41);
    d.onKey(42, 0, 1010, ignored);
    check(!d.shift(), "ASmux release clears shift");

    // A config override replaces one Sym position without a rebuild.
    d.setSymOverride(0x26, '#');
    checkEq(std::string(1, d.symChar(0x26)), "#", "sym override applies");

    // Software autorepeat: the v5 overlay provides none.
    d.reset();
    KeyEvent ev;
    d.noteScan(0x20);
    d.onKey(30, 1, 1000, ev);
    check(!d.pollRepeat(1399, ev), "no repeat before the 400ms delay");
    check(d.pollRepeat(1400, ev) && ev.repeat, "repeat fires at 400ms");
    check(d.pollRepeat(1470, ev), "repeat continues at 70ms");

    // A held key must not survive a screen change and answer the next dialog.
    d.releaseAll();
    check(!d.pollRepeat(1600, ev), "releaseAll stops autorepeat");

    // A plain USB keyboard sends no MSC_SCAN.
    KeyEvent usb = fromKeycodeOnly(30, false, false, false, false);
    checkEq(std::string(1, usb.ch), "a", "keycode-only fallback decodes a");
    usb = fromKeycodeOnly(12, true, false, false, false);
    checkEq(std::string(1, usb.ch), "_", "keycode-only Shift+minus -> underscore");
}

void testRawTerminalMode() {
    section("raw terminal lifecycle");
#if defined(__linux__)
    SimFc tty;
    std::string error;
    if (!tty.start(error)) {
        check(false, "test pty starts: " + error);
        return;
    }
    const int fd = ::open(tty.devicePath().c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        check(false, "test pty slave opens");
        return;
    }

    termios before{};
    const int flagsBefore = ::fcntl(fd, F_GETFL, 0);
    check(::tcgetattr(fd, &before) == 0 && flagsBefore >= 0,
          "test pty state is readable");
    {
        RawTerminalMode raw(true, fd);
        termios during{};
        const int flagsDuring = ::fcntl(fd, F_GETFL, 0);
        check(raw.active(), "raw mode activates on a tty");
        check(flagsDuring >= 0 && (flagsDuring & O_NONBLOCK) != 0,
              "raw mode makes input non-blocking");
        check(::tcgetattr(fd, &during) == 0 &&
                  (during.c_lflag & (ICANON | ECHO)) == 0,
              "raw mode disables canonical input and echo");
    }

    termios after{};
    const int flagsAfter = ::fcntl(fd, F_GETFL, 0);
    check(flagsAfter == flagsBefore, "raw mode restores descriptor flags");
    check(::tcgetattr(fd, &after) == 0 &&
              before.c_iflag == after.c_iflag &&
              before.c_oflag == after.c_oflag &&
              before.c_cflag == after.c_cflag &&
              before.c_lflag == after.c_lflag &&
              std::memcmp(before.c_cc, after.c_cc, sizeof(before.c_cc)) == 0,
          "raw mode restores terminal attributes");
    ::close(fd);
#else
    check(true, "raw terminal mode is Linux-only");
#endif
}

// ---------------------------------------------------------------- terminal

void testTerminal() {
    section("terminal");
    Terminal t;
    t.setWidth(10);

    t.feed("hello\r\nworld\r\n");
    check(t.lineCount() == 2, "CRLF splits into two lines");
    checkEq(t.line(0).text, "hello", "first line text");

    t.feed("abc");
    checkEq(t.partial(), "abc", "unterminated text stays partial");
    t.feed("\b\bZ");
    checkEq(t.partial(), "aZ", "backspace erases in the partial line");
    t.feed("\r\n");

    // ANSI sequences are dropped rather than printed as garbage.
    t.clear();
    t.feed("\x1b[2Kok\r\n");
    checkEq(t.line(0).text, "ok", "CSI sequence stripped");

    // Wrapping.
    t.clear();
    t.setWidth(10);
    t.feed("0123456789ABCDE\r\n");
    check(t.rowCount() == 2, "a 15-char line wraps into 2 rows at width 10");
    checkEq(t.rowText(0), "0123456789", "first wrapped row");
    checkEq(t.rowText(1), "ABCDE", "second wrapped row");

    // Re-wrapping on a width change.
    t.setWidth(5);
    check(t.rowCount() == 3, "narrowing to 5 columns re-wraps to 3 rows");

    // Capture only takes what arrived after the marker.
    t.clear();
    t.setWidth(53);
    t.feed("before\r\n");
    t.markCapture();
    t.feed("captured one\r\ncaptured two\r\n");
    const std::string cap = t.captureSince();
    check(cap.find("before") == std::string::npos, "capture excludes earlier output");
    check(cap.find("captured one") != std::string::npos, "capture includes later output");

    // Local app messages must not splice into a half-received FC line.
    t.clear();
    t.feed("partial");
    t.addLine("-- local --", LineKind::Local);
    check(t.lineCount() == 2, "partial line is flushed before a local message");
    checkEq(t.line(0).text, "partial", "partial content preserved");

    // The scrollback stays bounded.
    t.clear();
    t.setMaxLines(50);
    for (int i = 0; i < 500; ++i) t.addLine("line " + std::to_string(i), LineKind::Fc);
    check(t.lineCount() <= 50, "scrollback is trimmed to the cap");
    check(t.line(t.lineCount() - 1).text == "line 499", "newest line survives trimming");

    // Ctrl+L in the middle of a `diff` clears the view, but a command boundary
    // is counting arrivals, so the counter must not rewind under it.
    const uint64_t ever = t.linesEver();
    t.clear();
    check(t.linesEver() == ever, "clearing the screen does not rewind the line counter");
    t.addLine("after", LineKind::Fc);
    check(t.linesEver() == ever + 1, "the line counter keeps climbing after a clear");

    // Scrolled back, the viewport must keep its place when the buffer trims:
    // one dropped line can be several wrapped rows.
    t.clear();
    t.setWidth(10);
    t.setMaxLines(40);
    for (int i = 0; i < 40; ++i) {
        t.addLine("row" + std::to_string(i) + "-0123456789", LineKind::Fc);
    }
    t.scrollToBottom(6);
    t.scrollBy(-20, 6);
    const std::string atTop = t.rowText(static_cast<size_t>(t.scroll()));
    t.addLine("one more, which trims the oldest quarter", LineKind::Fc);
    checkEq(t.rowText(static_cast<size_t>(t.scroll())), atTop,
            "the viewport keeps its place across a trim");

    // A redraw cannot use lineCount() as its arrival signal: adding exactly
    // one trim batch leaves the bounded count unchanged despite fresh output.
    t.clear();
    while (t.lineCount() < 40) t.addLine("old", LineKind::Fc);
    const size_t boundedCount = t.lineCount();
    const uint64_t beforeBatch = t.linesEver();
    for (int i = 0; i < 11; ++i) t.addLine("replacement", LineKind::Fc);
    check(t.lineCount() == boundedCount, "a trim batch can leave lineCount unchanged");
    check(t.linesEver() == beforeBatch + 11, "the arrival counter exposes replacement lines");

    // Session bookkeeping consumes the completed batch, not only the tail
    // still resident in scrollback. Every rejected line must remain visible to
    // restore accounting even when one read is larger than the display buffer.
    std::string rejectedBurst;
    for (int i = 0; i < 60; ++i) rejectedBurst += "Invalid value\r\n";
    const std::vector<TermLine> completed = t.feed(rejectedBurst);
    int rejected = 0;
    for (const TermLine& line : completed) rejected += isErrorLine(line.text) ? 1 : 0;
    check(completed.size() == 60, "feed reports completed lines evicted by trimming");
    check(rejected == 60, "every evicted rejection remains available to the session");
}

void testEditor() {
    section("editor");
    LineEditor e;
    e.insert("set gyro_lpf1_static_hz");
    e.insert(" = 0");
    checkEq(e.text(), "set gyro_lpf1_static_hz = 0", "typed text");

    e.home();
    check(e.cursor() == 0, "home moves to column 0");
    e.end();
    e.killWordBack();
    checkEq(e.text(), "set gyro_lpf1_static_hz = ", "ctrl+w deletes the last word");

    e.setText("one two three");
    e.end();
    e.left(true);
    check(e.cursor() == 8, "word-left lands at the start of the last word");

    e.setText("abc");
    e.killToStart();
    checkEq(e.text(), "", "kill-to-start empties the line");

    // History.
    e.clear();
    e.insert("status");
    e.commit();
    e.insert("version");
    e.commit();
    check(e.historyPrev(), "history has entries");
    checkEq(e.text(), "version", "most recent command first");
    e.historyPrev();
    checkEq(e.text(), "status", "second step back");
    e.historyNext();
    e.historyNext();
    checkEq(e.text(), "", "walking forward restores the fresh line");

    // The word under the cursor drives completion.
    e.setText("set gyro_lpf");
    checkEq(e.wordPrefix(), "gyro_lpf", "word prefix at the cursor");
    e.replaceWord("gyro_lpf1_static_hz");
    checkEq(e.text(), "set gyro_lpf1_static_hz", "word replacement");

    // A quick command from the menu is recorded, but whatever was half-typed
    // when the menu opened is still there afterwards.
    e.clear();
    e.insert("set gyro_l");
    e.pushHistory("tasks");
    checkEq(e.text(), "set gyro_l", "a menu command leaves the input line alone");
    e.historyPrev();
    checkEq(e.text(), "tasks", "a menu command is still recorded in the history");
}

// -------------------------------------------------------------- completion

void testCompleter() {
    section("completion");
    Completer c;

    Completer::Result r = c.complete("vers", 4);
    check(r.candidates.size() == 1 && r.candidates[0] == "version", "command completion");

    r = c.complete("d", 1);
    check(r.candidates.size() > 1, "ambiguous command yields several candidates");
    check(r.commonPrefix.rfind("d", 0) == 0, "common prefix keeps the typed text");

    r = c.complete("set gyro_lpf1_s", 15);
    check(!r.candidates.empty(), "parameter completion after `set`");
    check(r.candidates[0].rfind("gyro_lpf1_s", 0) == 0, "candidate matches the prefix");

    // Learning from FC output, with no extra query.
    const size_t before = c.paramCount();
    c.harvest("set totally_made_up_param = 7\nset another_one = 3\n");
    check(c.paramCount() == before + 2, "harvest learns two new parameters");
    r = c.complete("set totally_", 12);
    check(r.candidates.size() == 1, "a harvested parameter completes");

    // Junk must not enter the index.
    c.harvest("Voltage: 4.12V (1S battery - OK)\n# comment = ignored\n");
    r = c.complete("set Volt", 8);
    check(r.candidates.empty(), "non-parameter text is not indexed");

    // An argument with no vocabulary is left alone.
    r = c.complete("aux 0 0", 7);
    check(r.candidates.empty(), "unknown argument position offers nothing");
}

void testRiskAndParsing() {
    section("commands");
    check(riskFor("motor 1 1100").risk == Risk::Motors, "motor with a value is a motor risk");
    check(riskFor("motor").risk == Risk::None, "bare `motor` only reports values");
    check(riskFor("defaults").risk == Risk::Destructive, "defaults is destructive");
    check(riskFor("flash_erase").risk == Risk::Destructive, "flash_erase is destructive");
    check(riskFor("save").risk == Risk::Writes, "save writes");
    check(riskFor("status").risk == Risk::None, "status is safe");
    checkEq(commandWord("  SET foo = 1"), "set", "command word is lowercased and trimmed");

    for (int i = 0; i < kBaudChoiceCount; ++i) {
        check(isSupportedBaud(kBaudChoices[i]),
              "offered baud rate has an exact termios mapping");
    }
    check(!isSupportedBaud(1200), "1200 baud is not offered to Betaflight");
    check(!isSupportedBaud(12345), "an arbitrary baud rate is rejected");

    check(isErrorLine("###ERROR IN diff: bad###"), "###ERROR detected");
    check(isErrorLine("Unknown command, try 'help'"), "unknown command detected");
    check(isErrorLine("Invalid name"), "invalid name detected");
    check(!isErrorLine("set p_pitch = 35"), "a normal line is not an error");

    const std::string dump = simDiffAll();
    checkEq(craftNameFromDump(dump), "AIR65 C", "craft name from a dump");
    checkEq(boardNameFromDump(dump), "BETAFPVG473_V2", "board name from a dump");

    const std::vector<std::string> lines = restorableLines(dump);
    check(!lines.empty(), "restorable lines extracted");
    for (const std::string& l : lines) {
        check(!l.empty() && l[0] != '#', "no blank or comment lines are sent");
    }
    check(lines.front() == "batch start", "restore starts with the batch");
    check(lines.back() == "batch end", "restore ends with the batch");
}

void testDiagnostics() {
    section("field diagnostics");
    const std::string modernStatus =
        "MCU G473 Clock=168MHz, Vref=3.30V, Core temp=41degC\n"
        "STACK: 2048b (0x20020000) / 740b\n"
        "CONFIG: CONFIGURED (4012b / 32768b)\n"
        "DEVICES DETECTED: GYRO: (1) ICM42688P enabled locked dma "
        "ACC: ICM42688P BARO: DPS310 I2C=1 (0 errors)\n"
        "GPS: NOT ENABLED\n"
        "Cpu:37%, Cycle Time: 125, gyro RATE: 8000, rx Rate: 250, System rate: 10\n"
        "Voltage: 16.42V (4S battery - OK)\n"
        "Arming disable flags: CLI MSP\n";
    const std::string modernTasks =
        "Task list rate/hz max/us avg/us maxload avgload total/ms\n"
        "02 - (GYRO) 8000 18 12 14.4% 9.6% 1948\n"
        "Total                                             42.5%\n";
    const std::string modernVersion =
        "# Betaflight / STM32G47X (G473) 2026.6.0 MSP API: 1.48\n"
        "# board: manufacturer_id: BEFH, board_name: BETAFPVG473_V2\n";
    DiagnosticReport report = buildDiagnosticReport(modernStatus, modernTasks, modernVersion);
    check(report.complete(), "modern status/tasks/version form a complete report");
    check(report.actionableBlockerCount() == 0, "CLI and MSP are expected host blockers");
    check(report.failureCount() == 0 && report.warningCount() == 0,
          "healthy explicit evidence is not promoted to a fault");
    check(report.findings.size() >= 6, "health report covers arming, gyro, RX, battery, bus, runtime, and firmware");
    check(report.coreTemperatureAvailable && report.coreTemperatureC == 41,
          "status exposes the FC MCU core temperature without calling it VTX temperature");
    bool inlineGyro = false;
    bool tolerantRuntime = false;
    for (const DiagnosticFinding& finding : report.findings) {
        if (finding.title == "Gyro" && finding.level == DiagnosticLevel::Pass &&
            finding.detail.find("ICM42688P") != std::string::npos &&
            finding.detail.find("ACC:") == std::string::npos) {
            inlineGyro = true;
        }
        if (finding.title == "Runtime" && finding.detail.find("125us loop") != std::string::npos &&
            finding.detail.find("8000Hz gyro") != std::string::npos) {
            tolerantRuntime = true;
        }
    }
    check(inlineGyro, "current inline device inventory yields a bounded gyro finding");
    check(tolerantRuntime, "runtime labels are parsed without depending on capitalization");
    int parsedTemperature = 0;
    check(parseCoreTemperatureC("MCU H743, Core Temp: 70.4 degC", parsedTemperature) &&
              parsedTemperature == 70,
          "core-temperature parser accepts spacing, case, colon, and decimal variants");

    std::string hotStatus = modernStatus;
    const size_t temperatureAt = hotStatus.find("Core temp=41degC");
    hotStatus.replace(temperatureAt, std::strlen("Core temp=41degC"), "Core temp=82degC");
    DiagnosticReport hotReport = buildDiagnosticReport(hotStatus, modernTasks, modernVersion);
    bool criticalTemperature = false;
    for (const DiagnosticFinding& finding : hotReport.findings) {
        if (finding.title == "Core temp" && finding.level == DiagnosticLevel::Failure) {
            criticalTemperature = true;
        }
    }
    check(criticalTemperature, "an 82C MCU reading is a critical field-check finding");

    const std::string legacyStatus =
        "Configuration: CONFIGURED, size: 3957, max available: 16384\n"
        "Gyros detected: none\n"
        "System load: 96, cycle time: 125, GYRO rate: 0, RX rate: 0\n"
        "Voltage: 13.20V (4S battery - CRITICAL)\n"
        "I2C Errors: 7\n"
        "Arming disable flags: NOGYRO RXLOSS THROTTLE CLI MSP ARM_SWITCH\n";
    const std::string legacyTasks =
        "Task list rate/hz max/us avg/us maxload avgload total/ms late run reqd/us\n"
        "Total (excluding SERIAL)                                96.4%\n";
    report = buildDiagnosticReport(legacyStatus, legacyTasks, modernVersion);
    check(report.actionableBlockerCount() == 4, "four non-host arming blockers are retained");
    check(report.failureCount() >= 4, "gyro, RX, battery, and blocker failures are explicit");
    check(report.warningCount() >= 3, "situational blockers and cumulative I2C errors are warnings");

    report = buildDiagnosticReport(modernStatus, "Unknown command, try 'help'\n", "");
    check(!report.complete(), "missing or rejected query makes the report incomplete");
    check(report.warningCount() >= 2, "missing scheduler and firmware evidence stays visible");

    const std::string saved = formatDiagnosticReport(
        report, modernStatus, "Unknown command, try 'help'\n", "");
    check(saved.find("not an airworthiness verdict") != std::string::npos,
          "saved report states its evidence boundary");
    check(saved.find("# Queries: status=ok, tasks=unavailable, version=unavailable") !=
              std::string::npos,
          "saved report identifies exactly which query evidence is unavailable");
    check(saved.find("# --- status (raw) ---") != std::string::npos &&
              saved.find("Arming disable flags: CLI MSP") != std::string::npos,
          "saved report preserves the raw FC evidence");

    const std::string duplicateFlags =
        "Gyros detected: gyro 1 locked\n"
        "Voltage: 0.00V (0S battery - NOT PRESENT)\n"
        "Arming disable flags: RXLOSS RXLOSS CLI MSP\n";
    report = buildDiagnosticReport(duplicateFlags, modernTasks, modernVersion);
    int rxLossFindings = 0;
    for (const DiagnosticFinding& finding : report.findings) {
        if (finding.title == "ARM RXLOSS") ++rxLossFindings;
    }
    check(report.actionableBlockerCount() == 1 && rxLossFindings == 1,
          "duplicate blocker tokens do not inflate field-check counts or findings");
}

// ------------------------------------------------------------ thermal trip

void testThermalTrip() {
    section("thermal trip");
#if defined(__linux__)
    ThermalFixture ext = makeThermalFixture("4");
    check(ext.ok, "synthetic EXT USB4 sysfs fixture is created");
    if (ext.ok) {
        ThermalTrip trip(ext.paths);
        const ThermalTripProbe& probe = trip.inspect(ext.device);
        check(probe.usbDeviceFound && probe.extUsb4 && probe.usbMuxSelected &&
                  probe.ext5vOn && probe.ext5vWritable && probe.eligible,
              "verified GL852G branch 4 plus mux and EXT5V readback qualifies");
        checkEq(probe.usbIdentity, "0483:5740", "trip binds the FC USB identity");
        std::string error;
        check(trip.arm(error), "verified EXT USB4 route arms: " + error);
        check(trip.cutPower(error), "armed trip cuts the synthetic EXT rail: " + error);
        check(trip.latched() && !trip.armed(), "successful cutoff latches and consumes its arm");
        checkEq(fixtureRead(ext.railPath), "0", "cutoff readback is physically represented as off");
        check(!trip.cutPower(error), "a latched trip cannot write the rail a second time");
        checkEq(fixtureRead(ext.railPath), "0", "there is no automatic re-enable path");
    }

    ThermalFixture usbA = makeThermalFixture("2");
    check(usbA.ok, "synthetic USB-A branch fixture is created");
    if (usbA.ok) {
        ThermalTrip trip(usbA.paths);
        const ThermalTripProbe& probe = trip.inspect(usbA.device);
        std::string error;
        check(probe.usbDeviceFound && !probe.extUsb4 && !probe.eligible,
              "a normal USB-A hub branch remains monitor-only");
        check(!trip.arm(error), "USB-A cannot arm the EXT rail cutoff");
        checkEq(fixtureRead(usbA.railPath), "1", "rejecting USB-A does not touch EXT power");
    }

    ThermalFixture gpioMode = makeThermalFixture("4", false, true);
    check(gpioMode.ok, "synthetic EXT GPIO-mode fixture is created");
    if (gpioMode.ok) {
        ThermalTrip trip(gpioMode.paths);
        const ThermalTripProbe& probe = trip.inspect(gpioMode.device);
        check(probe.extUsb4 && !probe.usbMuxSelected && !probe.eligible,
              "branch 4 cannot qualify while EXT pins report GPIO mode");
    }

    ThermalFixture changed = makeThermalFixture("4");
    check(changed.ok, "synthetic route-change fixture is created");
    if (changed.ok) {
        ThermalTrip trip(changed.paths);
        trip.inspect(changed.device);
        std::string error;
        check(trip.arm(error), "route-change fixture initially arms: " + error);
        check(fixtureFile(changed.muxPath, "0"), "test can move EXT selector away from USB");
        check(!trip.cutPower(error), "trip refuses a route that changed after arming");
        check(!trip.armed() && !trip.latched(), "failed revalidation consumes the stale arm");
        checkEq(fixtureRead(changed.railPath), "1", "failed revalidation leaves the rail untouched");
    }
#else
    check(true, "thermal trip is Linux-only");
#endif
}

// ----------------------------------------------------------------- storage

void testStorage() {
    section("storage");
    const std::string dir = "/tmp/bfcli-selftest";
    ::setenv("BFCLI_DATA_DIR", dir.c_str(), 1);
    Storage s;
    std::string err;
    check(s.init(err), "storage init: " + err);

    const std::string path = s.backupDir() + "/BTFL_cli_test_backup.txt";
    check(s.writeAtomic(path, "hello\nworld\n", err), "atomic write: " + err);
    std::string back;
    check(s.readFile(path, back, err), "read back: " + err);
    checkEq(back, "hello\nworld\n", "content round-trips");

    const std::vector<BackupFile> files = s.listBackups();
    check(!files.empty(), "the backup is listed");

    // A path outside the backup directory must be refused.
    check(!s.deleteBackup("/etc/passwd", err), "delete outside the backup dir is refused");
    check(s.deleteBackup(path, err), "delete inside the backup dir works");

    const std::string name = s.makeBackupName("AIR65 C", "BETAFPVG473_V2");
    check(name.rfind("BTFL_cli_AIR65_C_", 0) == 0, "backup name matches Configurator's shape");
    check(name.find("_BETAFPVG473_V2_backup.txt") != std::string::npos,
          "backup name carries the board");
    checkEq(sanitizeForFilename("a/b:c d"), "abc_d", "filename sanitising");
    const std::string diagnostic = s.makeDiagnosticName("AIR65 C", "BETAFPVG473_V2");
    check(diagnostic.rfind("GNDHOG_fieldcheck_AIR65_C_", 0) == 0 &&
              diagnostic.find("_BETAFPVG473_V2.txt") != std::string::npos,
          "field checks get a separate non-restorable filename");
    check(s.diagnosticDir() != s.backupDir(), "field checks stay outside the restore picker");
}

void testGraphics() {
    section("graphics");
    check(glyph(' ')[0] == 0 && glyph(' ')[4] == 0, "space is blank");
    check(glyph('_')[0] == 0x40, "underscore sits on the baseline");
    check(glyph('A')[0] == 0x7E, "A has its left stem");

    Canvas c(20, 10);
    Surface s = c.surface();
    fill(s, theme::bg);
    check(s.px[0] == theme::bg, "fill writes the background");
    fillRect(s, 5, 5, 3, 3, theme::accent);
    check(s.row(5)[5] == theme::accent, "fillRect writes inside");
    check(s.row(4)[5] == theme::bg, "fillRect does not write outside");
    // Clipping must not corrupt memory outside the surface.
    fillRect(s, -100, -100, 500, 500, theme::ok);
    check(s.row(0)[0] == theme::ok, "an oversized rect clips to the surface");
    check(rgb(255, 255, 255) == 0xFFFF, "white packs to 0xFFFF");
    check(rgb(0, 0, 0) == 0x0000, "black packs to 0x0000");

    Canvas dimmed(2, 1);
    Surface ds = dimmed.surface();
    fill(ds, rgb(255, 255, 255));
    dimSurface(ds, theme::black);
    check(ds.px[0] == 0x8410 && ds.px[1] == 0x8410,
          "modal dimming blends every pixel instead of dropping scanlines");
}

// ------------------------------------------------- end-to-end against a pty

bool spin(SimFc& sim, Session& s, int timeoutMs, const std::function<bool()>& until) {
    const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeoutMs);
    while (nowMs() < deadline) {
        sim.pump();
        s.poll(nowMs());
        if (until()) return true;
        sleepMs(4);
    }
    return false;
}

void testSession() {
    section("session (simulated FC over a pty)");
    SimFc sim;
    std::string err;
    if (!sim.start(err)) {
        std::printf("  SKIP  simulator unavailable: %s\n", err.c_str());
        return;
    }

    Terminal term;
    term.setWidth(53);
    Completer completer;
    Session s(term, completer);

    check(s.connect(sim.devicePath(), 115200, err), "connect to the pty: " + err);
    check(spin(sim, s, 2000, [&] { return s.awaitingVtxChoice(); }),
          "MSP preflight detects a controllable VTX before entering CLI");
    check(s.vtxStatus().deviceReady && s.vtxStatus().power == 3,
          "VTX preflight retains ready state and configured power");
    s.enableVtxBenchGuard();
    check(spin(sim, s, 4000, [&] { return s.ready(); }),
          "verified VTX pit mode continues into the CLI prompt");
    check(s.vtxBenchGuardActive() && s.vtxBenchMode() == VtxBenchMode::PitMode,
          "bench guard is active only after the VTX confirms pit mode");

    // A plain command must complete and return to the prompt.
    check(s.send("status"), "send status");
    check(spin(sim, s, 4000, [&] { return s.ready(); }), "status completes");
    bool sawVoltage = false;
    for (size_t i = 0; i < term.lineCount(); ++i) {
        if (term.line(i).text.find("Voltage:") != std::string::npos) sawVoltage = true;
    }
    check(sawVoltage, "status output reached the terminal");
    check(s.coreTemperatureAvailable() && s.coreTemperatureC() == 37 &&
              s.coreTemperatureSequence() > 0,
          "live status output updates the connection temperature monitor");

    // Capture: this is the case where a streaming `diff` shows comment lines
    // that momentarily look like a bare prompt.
    bool done = false, ok = false;
    std::string captured;
    check(s.startCapture("diff all", "test", [&](bool o, const std::string& text) {
              done = true;
              ok = o;
              captured = text;
          }),
          "capture starts");
    check(spin(sim, s, 8000, [&] { return done; }), "capture finishes");
    check(ok, "capture reports success");
    check(captured.rfind("# version", 0) == 0,
          "capture starts at the FC response, not the echoed command");
    check(captured.find("set craft_name") != std::string::npos,
          "captured text contains the whole dump");
    check(captured.find("batch end") != std::string::npos, "capture ran to the end");
    checkEq(craftNameFromDump(captured), "AIR65 C", "craft name parsed from the capture");
    check(completer.paramCount() > 100, "the capture was harvested for completion");

    // Restore: play the captured config back line by line.
    const std::vector<std::string> lines = restorableLines(captured);
    check(spin(sim, s, 2000, [&] { return s.ready(); }), "ready before restore");
    check(s.startRestore(lines, "test restore"), "restore starts");
    check(spin(sim, s, 20000, [&] { return s.job().finished; }), "restore finishes");
    check(s.job().ok, "restore reports success: " + s.job().message);
    check(s.job().done == static_cast<int>(lines.size()), "every line was sent");

    check(s.restoreVtxAndDisconnect(), "guarded disconnect queues an FC reboot");
    check(spin(sim, s, 2000, [&] { return !s.connected(); }),
          "guarded disconnect drains exit and closes the port");
    check(!s.vtxBenchGuardActive(), "reboot-backed disconnect clears guard ownership");

    // The old FC left a prompt in Terminal::partial(). A new, silent port must
    // prove its own prompt instead of inheriting that fragment and claiming it
    // is ready.
    SimFc silent;
    check(silent.start(err), "start a silent reconnect target: " + err);
    check(s.connect(silent.devicePath(), 115200, err), "open the silent reconnect target");
    const uint64_t reconnectDeadline = nowMs() + 400;
    while (nowMs() < reconnectDeadline) {
        s.poll(nowMs());
        sleepMs(4);
    }
    check(!s.ready(), "a reconnect cannot reuse the previous FC prompt");
    s.disconnect();

    // A write can discover a detached USB serial device before read() does.
    // The failed flush must transition the session to link-lost immediately;
    // otherwise it remains "connected" until an unrelated command timeout.
    SimFc dropped;
    check(dropped.start(err), "start a link-loss target: " + err);
    check(s.connect(dropped.devicePath(), 115200, err), "connect to the link-loss target");
    check(spin(dropped, s, 4000, [&] {
        if (s.awaitingVtxChoice()) s.skipVtxBenchGuard();
        return s.ready();
    }),
          "link-loss target reaches the CLI prompt");
    dropped.stop();
    check(s.send("status"), "queue a command after the peer disappears");
    const uint64_t lossDeadline = nowMs() + 400;
    while (nowMs() < lossDeadline && s.connected()) {
        s.poll(nowMs());
        sleepMs(4);
    }
    check(s.linkLost(), "a failed serial write reports link loss");
    check(!s.connected(), "a failed serial write closes the dead port");
}

void testVtxGuardFailurePaths() {
    section("VTX guard fail-closed paths");
    Terminal term;
    term.setWidth(53);
    Completer completer;
    Session session(term, completer);
    std::string error;

    SimFc noPit;
    check(noPit.start(error), "non-pit VTX simulator starts: " + error);
    noPit.setPitModeSupported(false);
    check(session.connect(noPit.devicePath(), 115200, error),
          "connect to a VTX that cannot enter pit mode");
    check(spin(noPit, session, 2000, [&] { return session.awaitingVtxChoice(); }),
          "unsupported pit mode is still an operator choice, not an assumption");
    session.enableVtxBenchGuard();
    check(spin(noPit, session, 4000, [&] { return session.ready(); }),
          "failed pit confirmation continues safely into CLI");
    check(!session.vtxBenchGuardActive() &&
              session.vtxGuardNote().find("unchanged state confirmed") != std::string::npos,
          "unconfirmed pit mode is never labelled active");
    session.disconnect();

    SimFc offlineVtx;
    check(offlineVtx.start(error), "offline-VTX simulator starts: " + error);
    offlineVtx.setVtxReady(false);
    check(session.connect(offlineVtx.devicePath(), 115200, error),
          "connect when Betaflight reports no ready VTX");
    check(spin(offlineVtx, session, 4000, [&] { return session.ready(); }),
          "an unready VTX does not block CLI entry with a false prompt");
    check(!session.awaitingVtxChoice() && !session.vtxBenchGuardActive(),
          "an unready VTX remains untouched");
    session.disconnect();
}

// A full scrollback is the normal state after a session or two, and it is the
// one that used to break: trimming renumbers the buffer, so a command boundary
// that compared raw line counts could never fire again.
void testFullScrollback() {
    section("commands with a full scrollback");
    SimFc sim;
    std::string err;
    if (!sim.start(err)) {
        std::printf("  SKIP  simulator unavailable: %s\n", err.c_str());
        return;
    }

    Terminal term;
    term.setWidth(53);
    term.setMaxLines(40);
    Completer completer;
    Session s(term, completer);

    check(s.connect(sim.devicePath(), 115200, err), "connect to the pty: " + err);
    check(spin(sim, s, 4000, [&] {
        if (s.awaitingVtxChoice()) s.skipVtxBenchGuard();
        return s.ready();
    }), "reaches the CLI prompt");

    // Fill the scrollback to the cap, exactly as a long session does.
    while (term.lineCount() < 40) term.addLine("old output", LineKind::Fc);

    check(s.send("status"), "send status with the scrollback already full");
    check(spin(sim, s, 4000, [&] { return s.ready(); }),
          "a command completes even when every new line trims an old one");

    bool done = false, ok = false;
    std::string captured;
    check(s.startCapture("diff all", "test", [&](bool o, const std::string& text) {
              done = true;
              ok = o;
              captured = text;
          }),
          "capture starts on a full scrollback");
    check(spin(sim, s, 8000, [&] { return done; }), "capture finishes on a full scrollback");
    check(ok, "capture reports success on a full scrollback");
    check(captured.find("batch end") != std::string::npos,
          "the tail of the dump is still captured");

    // A restore response can push the buffer over its cap. Even when that
    // trims old rows and makes lineCount() shrink, the rejection must still be
    // counted rather than reported as a successful restore.
    while (term.lineCount() < 40) term.addLine("old output", LineKind::Fc);
    check(s.startRestore({"definitely_not_a_betaflight_command"}, "rejected restore"),
          "rejected restore starts on a full scrollback");
    check(spin(sim, s, 4000, [&] { return s.job().finished; }),
          "rejected restore finishes on a full scrollback");
    check(!s.job().ok, "restore rejection survives scrollback trimming");
    check(s.job().errorCount == 1, "trimmed restore response counts one rejection");

    s.disconnect();
}

// ---------------------------------------------------------------- protobuf

void testProtobufWire() {
    section("protobuf wire format");

    pb::Writer w;
    w.varint(1, 300);
    w.boolean(2, true);
    w.i32(3, -7);
    w.fixed32(4, 0xDEADBEEFu);
    w.sfixed32(5, -1234567);
    w.f32(6, 2.5f);
    w.bytes(7, "hello");
    pb::Writer inner;
    inner.varint(1, 42);
    w.message(8, inner);
    w.emptyMessage(9);

    int seen = 0;
    bool ok = true;
    pb::Reader r(w.data());
    while (r.next()) {
        ++seen;
        switch (r.field()) {
        case 1: ok &= r.varint() == 300; break;
        case 2: ok &= r.boolean(); break;
        case 3: ok &= r.i32() == -7; break;
        case 4: ok &= r.fixed32() == 0xDEADBEEFu; break;
        case 5: ok &= r.sfixed32() == -1234567; break;
        case 6: ok &= r.f32() == 2.5f; break;
        case 7: ok &= r.bytes() == "hello"; break;
        case 8: {
            pb::Reader sub = r.sub();
            ok &= sub.next() && sub.varint() == 42;
            break;
        }
        case 9: ok &= r.bytes().empty(); break;
        default: ok = false; break;
        }
    }
    check(seen == 9 && ok && r.ok(), "every wire type survives an encode/decode round trip");

    // A negative int32 is sign extended to ten groups; truncating it to four
    // makes the peer read an enormous positive number instead.
    pb::Writer negative;
    negative.i32(1, -1);
    check(negative.data().size() == 11, "a negative int32 is encoded sign extended");

    // Truncation must stop the reader, not read past the buffer. The reader
    // borrows its bytes, so the truncated copy has to outlive it.
    for (size_t cut = 1; cut < w.data().size(); ++cut) {
        const std::string truncated = w.data().substr(0, cut);
        pb::Reader partial(truncated);
        while (partial.next()) {
        }
    }
    check(true, "every truncation of a message is parsed without reading past the end");

    const std::string endless("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 11);
    pb::Reader bogus(endless);
    check(!bogus.next() && !bogus.ok(), "an over-long varint is rejected instead of wrapping");

    // Unknown fields, which is how this survives a firmware newer than itself.
    pb::Writer future;
    future.varint(1, 5);
    future.bytes(4242, "a field this build has never heard of");
    future.varint(2, 9);
    pb::Reader forward(future.data());
    int known = 0;
    while (forward.next()) {
        if (forward.field() == 1 || forward.field() == 2) ++known;
    }
    check(known == 2 && forward.ok(), "unknown fields are skipped, not fatal");

    const std::string groupTag("\x0B", 1);      // start-group tag
    pb::Reader group(groupTag);
    check(!group.next() && !group.ok(), "a group tag stops the reader rather than guessing");
}

// ----------------------------------------------------------- mesh framing

void testMeshFraming() {
    section("meshtastic serial framing");

    const std::string frame = frameToRadio("payload");
    check(frame.size() == 11 && static_cast<uint8_t>(frame[0]) == 0x94 &&
              static_cast<uint8_t>(frame[1]) == 0xC3 && frame[2] == 0 && frame[3] == 7,
          "a frame is 0x94 0xC3 then a big-endian length");

    // Real firmware writes its console log down the same wire.
    std::string buf = "INFO | booted\r\n" + frameToRadio("one") + "DEBUG | noise" +
                      frameToRadio("two");
    std::vector<std::string> frames;
    std::string log;
    extractMeshFrames(buf, frames, log);
    check(frames.size() == 2 && frames[0] == "one" && frames[1] == "two",
          "frames are recovered from between console lines");
    check(log == "INFO | booted\r\nDEBUG | noise",
          "text that was never part of a frame is preserved as log output");
    check(buf.empty(), "a fully consumed buffer is left empty");

    // A frame split across two reads must not be lost or half-delivered.
    const std::string whole = frameToRadio("split me");
    std::string partial = whole.substr(0, 6);
    frames.clear();
    log.clear();
    extractMeshFrames(partial, frames, log);
    check(frames.empty() && log.empty() && partial.size() == 6,
          "an incomplete frame is held, not guessed at");
    partial += whole.substr(6);
    extractMeshFrames(partial, frames, log);
    check(frames.size() == 1 && frames[0] == "split me",
          "the held fragment completes on the next read");

    // 0x94 0xC3 can occur in log text. A length the protocol cannot produce is
    // the tell, and the parser has to resynchronise rather than stall forever.
    std::string bogus;
    bogus.push_back(static_cast<char>(0x94));
    bogus.push_back(static_cast<char>(0xC3));
    bogus.push_back(static_cast<char>(0xFF));
    bogus.push_back(static_cast<char>(0xFF));
    bogus += frameToRadio("after");
    frames.clear();
    log.clear();
    extractMeshFrames(bogus, frames, log);
    check(frames.size() == 1 && frames[0] == "after",
          "an impossible length resynchronises to the next real frame");

    std::string lone;
    lone.push_back(static_cast<char>(0x94));
    lone += "not a frame";
    frames.clear();
    log.clear();
    extractMeshFrames(lone, frames, log);
    check(frames.empty() && log.size() == 12,
          "a stray magic byte in log text is delivered as log text");

    check(kMeshMaxFrame == 512, "the frame cap matches the client API's maximum");
}

// ------------------------------------------------------------- mesh codec

void testMeshCodec() {
    section("meshtastic protobuf messages");

    // want_config_id is field 3, a varint.
    const std::string want = encodeWantConfig(0x1234u);
    pb::Reader wantReader(want);
    check(wantReader.next() && wantReader.field() == 3 && wantReader.varint() == 0x1234u,
          "want_config_id is encoded on field 3");
    check(kMeshNodelessConfigId == 69420u,
          "the nodeless config id is known so it can be avoided");

    const std::string beat = encodeHeartbeat();
    pb::Reader beatReader(beat);
    check(beatReader.next() && beatReader.field() == 7 && beatReader.bytes().empty(),
          "a heartbeat is an empty submessage on field 7");

    // A text packet, read back the way the firmware would.
    const std::string packet = encodeTextPacket(0xA1B2C3D4u, 0, 0x55667788u, "hello mesh", true);
    uint32_t to = 0, id = 0, port = 0;
    bool wantAck = false, sawFrom = false;
    std::string payload;
    pb::Reader toRadio(packet);
    check(toRadio.next() && toRadio.field() == 1, "a packet rides on ToRadio field 1");
    pb::Reader meshPacket = toRadio.sub();
    while (meshPacket.next()) {
        switch (meshPacket.field()) {
        case 1: sawFrom = true; break;
        case 2: to = meshPacket.fixed32(); break;
        case 6: id = meshPacket.fixed32(); break;
        case 10: wantAck = meshPacket.boolean(); break;
        case 4: {
            pb::Reader data = meshPacket.sub();
            while (data.next()) {
                if (data.field() == 1) port = data.u32();
                else if (data.field() == 2) payload = data.bytes();
            }
            break;
        }
        default: break;
        }
    }
    check(to == 0xA1B2C3D4u && id == 0x55667788u && wantAck && port == 1 &&
              payload == "hello mesh",
          "a text packet carries destination, id, ack request and TEXT_MESSAGE_APP");
    check(!sawFrom, "the client never stamps `from`; the radio owns its own node number");

    const std::string overlong(kMeshMaxTextBytes + 50, 'x');
    const std::string clipped = encodeTextPacket(1, 0, 2, overlong, false);
    check(clipped.find(std::string(kMeshMaxTextBytes + 1, 'x')) == std::string::npos,
          "an over-long message is clipped to the LoRa payload limit");

    // Position, through the encoder and back out of the decoder.
    const std::string positionPacket =
        encodePositionPacket(kMeshBroadcast, 0, 3, 51.50722, -0.12750, true, 35, 1700000000u, 9);
    std::string positionPayload;
    pb::Reader positionRadio(positionPacket);
    positionRadio.next();
    pb::Reader positionMesh = positionRadio.sub();
    while (positionMesh.next()) {
        if (positionMesh.field() != 4) continue;
        pb::Reader data = positionMesh.sub();
        while (data.next()) {
            if (data.field() == 2) positionPayload = data.bytes();
        }
    }
    MeshPosition position;
    check(decodeMeshPosition(positionPayload, position) && position.valid,
          "an encoded position decodes back");
    check(std::abs(position.latitude - 51.50722) < 1e-6 &&
              std::abs(position.longitude + 0.12750) < 1e-6,
          "latitude and longitude survive the 1e7 fixed-point round trip");
    check(position.haveAltitude && position.altitudeM == 35 && position.satsInView == 9,
          "altitude and satellite count survive with them");

    MeshPosition absent;
    check(decodeMeshPosition(std::string(), absent) && !absent.valid,
          "a position with no coordinates is absent, not a fix at the equator");

    bool haveError = false;
    uint32_t reason = 0;
    pb::Writer routing;
    routing.varint(3, 0);
    check(decodeMeshRouting(routing.data(), haveError, reason) && haveError && reason == 0,
          "a routing ACK is an error_reason of NONE that is actually present");
    haveError = true;
    check(decodeMeshRouting(std::string(), haveError, reason) && !haveError,
          "a routing message with no error field is not a delivery report");

    checkEq(meshNodeIdText(0x0BADF00Du), "!0badf00d", "node ids render the Meshtastic way");
    checkEq(meshAgeText(10), "now", "a fresh contact reads as now");
    checkEq(meshAgeText(3700), "1h", "an hour-old contact reads in hours");
    checkEq(std::string(meshCompassPoint(91.0)), "E", "bearings map onto compass points");
    checkEq(std::string(meshCompassPoint(359.0)), "N", "the compass wraps at north");

    // Greenwich to a point due north of it.
    const double metres = meshDistanceM(51.4779, 0.0, 51.4869, 0.0);
    check(metres > 990.0 && metres < 1010.0, "great-circle distance is metres, not degrees");
    const double bearing = meshBearingDeg(51.4779, 0.0, 51.4869, 0.0);
    check(bearing < 1.0 || bearing > 359.0, "due north reads as a bearing of zero");
    checkEq(meshRangeText(1400.0), "1.4km", "ranges over a kilometre are shown in kilometres");
}

void testMeshChatFiles() {
    section("mesh conversation files");

    std::vector<MeshMessage> messages;
    MeshMessage in;
    in.peer = 0xA1B2C3D4u;
    in.from = 0xA1B2C3D4u;
    in.to = kMeshBroadcast;
    in.id = 0x11223344u;
    in.stampUtc = 1700000000;
    in.text = "line one\nline two\twith a tab";
    in.state = MeshMessageState::Received;
    messages.push_back(in);

    MeshMessage out;
    out.outgoing = true;
    out.peer = 0xA1B2C3D4u;
    out.from = 0x33445566u;
    out.to = 0xA1B2C3D4u;
    out.id = 0x55667788u;
    out.stampUtc = 1700000060;
    out.text = "understood";
    out.state = MeshMessageState::Delivered;
    messages.push_back(out);

    MeshMessage pending = out;
    pending.id = 0x99AABBCCu;
    pending.text = "still going";
    pending.state = MeshMessageState::Queued;
    messages.push_back(pending);

    const std::vector<MeshMessage> back = parseMeshChat(formatMeshChat(messages));
    check(back.size() == 3, "every record survives the transcript round trip");
    if (back.size() == 3) {
        checkEq(back[0].text, in.text, "embedded newlines and tabs are escaped, not split");
        check(!back[0].outgoing && back[0].from == in.from && back[0].id == in.id,
              "direction and identifiers survive");
        check(back[1].outgoing && back[1].state == MeshMessageState::Delivered,
              "a delivered message stays delivered");
        check(back[2].state == MeshMessageState::Failed &&
                  back[2].note.find("unresolved") != std::string::npos,
              "a message still awaiting an ack at shutdown is not reloaded as delivered");
    }

    checkEq(meshChatFileName(kMeshBroadcast), "broadcast.chat", "the broadcast log has a name");
    checkEq(meshChatFileName(0x0BADF00Du), "node-0badf00d.chat", "per-node logs are named by id");
    uint32_t peer = 0;
    check(meshChatPeerFromFileName("node-0badf00d.chat", peer) && peer == 0x0BADF00Du,
          "a conversation file names the peer it belongs to");
    check(meshChatPeerFromFileName("broadcast.chat", peer) && peer == kMeshBroadcast,
          "the broadcast file maps back to the broadcast address");
    check(!meshChatPeerFromFileName("node-nothex.chat", peer) &&
              !meshChatPeerFromFileName("notes.txt", peer) &&
              !meshChatPeerFromFileName("node-0badf00d.chat.bak", peer),
          "anything that is not a conversation file is refused");
}

// -------------------------------------------------------------------- GNSS

void testGnss() {
    section("LoRa Cap GNSS (NMEA 0183)");

    GnssFix fix;
    const std::string gga =
        "$GNGGA,123519.00,5130.4332,N,00007.6500,W,1,09,0.9,45.4,M,46.9,M,,*56";
    check(nmeaChecksumOk(gga), "a well-formed GGA passes its checksum");
    check(parseNmeaSentence(gga, fix, 1000) && fix.valid,
          "a GGA with a fix quality above zero is a fix");
    check(std::abs(fix.latitude - 51.507220) < 1e-5 &&
              std::abs(fix.longitude + 0.1275) < 1e-5,
          "ddmm.mmmm degrees and minutes are converted, not read as decimal degrees");
    check(fix.satellitesUsed == 9 && fix.haveAltitude &&
              std::abs(fix.altitudeM - 45.4) < 0.01,
          "satellites and altitude come across");

    // A corrupted line must be dropped rather than believed.
    std::string corrupt = gga;
    corrupt[20] = '9';
    GnssFix untouched;
    check(!parseNmeaSentence(corrupt, untouched, 1000) && !untouched.valid,
          "a sentence that fails its checksum is discarded");

    GnssFix rmcFix;
    const std::string rmc =
        "$GNRMC,123519.00,A,5130.4332,N,00007.6500,W,0.5,54.7,230326,,,A*6F";
    check(nmeaChecksumOk(rmc), "the RMC fixture checksum is right");
    check(parseNmeaSentence(rmc, rmcFix, 2000) && rmcFix.valid,
          "an active RMC is a fix");
    check(rmcFix.utcSeconds > 1774000000u && rmcFix.utcSeconds < 1900000000u,
          "date and time combine into a UTC timestamp without touching the local zone");
    checkEq(rmcFix.utc, "12:35:19", "the receiver's own clock is reported as it arrived");

    // A void RMC clears the fix but keeps the evidence that one existed.
    GnssFix lost = rmcFix;
    std::string built = "GNRMC,123529.00,V,,,,,,,230326,,,N";
    uint8_t sum = 0;
    for (char c : built) sum ^= static_cast<uint8_t>(c);
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X", sum);
    check(parseNmeaSentence("$" + built + tail, lost, 3000),
          "a void RMC is a well-formed sentence");
    check(!lost.valid && lost.everValid,
          "losing the fix is reported as lost, not as never having had one");

    GnssFix view;
    std::string gsvBody = "GPGSV,3,1,11,01,45,090,32";
    sum = 0;
    for (char c : gsvBody) sum ^= static_cast<uint8_t>(c);
    std::snprintf(tail, sizeof(tail), "*%02X", sum);
    check(parseNmeaSentence("$" + gsvBody + tail, view, 4000) && view.satellitesInView == 11,
          "GSV supplies the satellites-in-view count while searching");

    check(!parseNmeaSentence("garbage", view, 5000) &&
              !parseNmeaSentence("$", view, 5000),
          "non-NMEA input is rejected without reading past the end");
}

// --------------------------------------------------------- mesh session

bool spinMesh(SimMesh& sim, MeshSession& mesh, int timeoutMs,
              const std::function<bool()>& until) {
    const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeoutMs);
    while (nowMs() < deadline) {
        sim.pump();
        mesh.poll(nowMs());
        if (until()) return true;
        sleepMs(4);
    }
    return false;
}

void testMeshSession() {
    section("meshtastic session (simulated radio over a pty)");
    SimMesh sim;
    std::string error;
    if (!sim.start(error)) {
        std::printf("  SKIP  mesh simulator unavailable: %s\n", error.c_str());
        return;
    }

    Terminal term;
    term.setWidth(53);
    MeshSession mesh(term);

    check(mesh.connect(sim.devicePath(), 115200, error), "connect to the pty radio: " + error);
    check(spinMesh(sim, mesh, 5000, [&] { return mesh.ready(); }),
          "the config download runs to config_complete_id");
    check(mesh.radio().myNodeNum == sim.selfNodeNum(),
          "my_info supplies the attached radio node number");
    checkEq(mesh.radio().firmwareVersion, "2.7.11.abcdef1",
            "device metadata carries the firmware version");
    check(mesh.radio().loraReady() && mesh.radio().region == 3,
          "the LoRa config is read back and reports a region it can transmit in");
    checkEq(mesh.radio().primaryChannel, "LongFast",
            "the primary channel name comes from the channel record");
    check(mesh.nodes().size() == 3, "all three fixture nodes reach the node database");
    check(!mesh.nodes().empty() && mesh.nodes().front().isSelf,
          "the attached radio sorts to the top of its own node list");

    const MeshNode* hilltop = mesh.findNode(sim.hilltopNodeNum());
    check(hilltop != nullptr && hilltop->user.shortName == "HILL" &&
              hilltop->user.longName == "HILLTOP RELAY",
          "a node user record is decoded");
    check(hilltop != nullptr && hilltop->position.valid &&
              hilltop->position.latitude > 51.4 && hilltop->position.latitude < 51.5,
          "a node position is decoded from its node info");
    check(hilltop != nullptr && hilltop->haveBattery && hilltop->batteryLevel == 87,
          "device metrics are decoded alongside it");
    check(hilltop != nullptr && hilltop->haveHops && hilltop->hopsAway == 1,
          "hops away is reported when the radio supplies it");

    bool sawConsole = false;
    for (size_t i = 0; i < term.lineCount(); ++i) {
        if (term.line(i).text.find("Sending our nodeinfo") != std::string::npos) sawConsole = true;
    }
    check(sawConsole, "console text interleaved with frames still reaches the terminal");

    // A direct message stays queued until the mesh answers for it.
    check(mesh.sendText(sim.hilltopNodeNum(), "on my way", error),
          "a direct message is accepted: " + error);
    const std::vector<MeshMessage>* direct = mesh.conversation(sim.hilltopNodeNum());
    check(direct != nullptr && direct->size() == 1 &&
              direct->back().state == MeshMessageState::Queued,
          "a direct message starts queued, never pre-emptively delivered");
    check(spinMesh(sim, mesh, 4000,
                   [&] {
                       const std::vector<MeshMessage>* log =
                           mesh.conversation(sim.hilltopNodeNum());
                       return log && !log->empty() &&
                              log->back().state == MeshMessageState::Delivered;
                   }),
          "a routing ACK for that packet id marks it delivered");
    checkEq(sim.lastTextReceived(), "on my way", "the radio received what was typed");

    // A rejection has to say so out loud.
    sim.setAckError(1);   // NO_ROUTE
    check(mesh.sendText(sim.vanNodeNum(), "anyone there", error),
          "a second direct message is accepted");
    check(spinMesh(sim, mesh, 4000,
                   [&] {
                       const std::vector<MeshMessage>* log = mesh.conversation(sim.vanNodeNum());
                       return log && !log->empty() &&
                              log->back().state == MeshMessageState::Failed;
                   }),
          "a routing error marks the message failed");
    const std::vector<MeshMessage>* rejected = mesh.conversation(sim.vanNodeNum());
    check(rejected != nullptr && !rejected->empty() &&
              rejected->back().note.find("no route") != std::string::npos,
          "the failure carries the reason the mesh gave");

    // Broadcasts are not acknowledged, and must not claim to be.
    sim.setAckError(0);
    check(mesh.sendText(kMeshBroadcast, "net check", error), "a broadcast is accepted");
    const std::vector<MeshMessage>* broadcast = mesh.conversation(kMeshBroadcast);
    check(broadcast != nullptr && !broadcast->empty() &&
              broadcast->back().state == MeshMessageState::Sent &&
              broadcast->back().note.find("does not acknowledge") != std::string::npos,
          "a broadcast is recorded as sent, never as delivered");

    sim.injectText(sim.hilltopNodeNum(), sim.selfNodeNum(), "roger, gate is open");
    check(spinMesh(sim, mesh, 3000,
                   [&] {
                       const std::vector<MeshMessage>* log =
                           mesh.conversation(sim.hilltopNodeNum());
                       return log && log->size() >= 2 && !log->back().outgoing;
                   }),
          "an inbound direct message lands in that node conversation");
    check(mesh.unread(sim.hilltopNodeNum()) > 0,
          "an unread count is kept until the conversation is opened");
    mesh.markRead(sim.hilltopNodeNum());
    check(mesh.unread(sim.hilltopNodeNum()) == 0 && mesh.totalUnread() == 0,
          "opening a conversation clears its unread count");

    const size_t broadcastBefore = broadcast ? broadcast->size() : 0;
    sim.injectText(sim.vanNodeNum(), kMeshBroadcast, "van 2 rolling");
    check(spinMesh(sim, mesh, 3000,
                   [&] {
                       const std::vector<MeshMessage>* log = mesh.conversation(kMeshBroadcast);
                       return log && log->size() > broadcastBefore;
                   }),
          "a message addressed to everyone lands in the broadcast conversation");

    sim.injectPosition(sim.vanNodeNum(), 51.5007, -0.1246);
    check(spinMesh(sim, mesh, 3000,
                   [&] {
                       const MeshNode* van = mesh.findNode(sim.vanNodeNum());
                       return van && van->position.valid;
                   }),
          "a position packet updates the sender entry in the node table");

    // A dead peer has to surface as a lost link, not a silent session.
    sim.stop();
    mesh.sendText(kMeshBroadcast, "still there?", error);
    const uint64_t lossDeadline = nowMs() + 1000;
    while (nowMs() < lossDeadline && mesh.connected()) {
        mesh.poll(nowMs());
        sleepMs(4);
    }
    check(mesh.linkLost() && !mesh.connected(),
          "a failed write to a vanished radio reports link loss and closes the port");
    mesh.disconnect();

    // A radio with no region cannot legally transmit, and must refuse.
    SimMesh mute;
    if (mute.start(error)) {
        mute.setRegionUnset(true);
        Terminal muteTerm;
        muteTerm.setWidth(53);
        MeshSession muteSession(muteTerm);
        check(muteSession.connect(mute.devicePath(), 115200, error),
              "connect to a region-less radio: " + error);
        check(spinMesh(mute, muteSession, 5000, [&] { return muteSession.ready(); }),
              "a region-less radio still completes its config download");
        check(!muteSession.radio().loraReady(),
              "a radio with no region is not reported as ready to transmit");
        std::string why;
        check(!muteSession.sendText(kMeshBroadcast, "hello", why) &&
                  why.find("region") != std::string::npos,
              "sending is refused with the reason instead of queued into a mute radio");
        check(muteSession.conversation(kMeshBroadcast) == nullptr,
              "a refused message is not written into the transcript");
        muteSession.disconnect();
    }
}

} // namespace

// ------------------------------------------------------------- mesh app

void testMeshApp() {
    section("mesh screens, menus and saved conversations");
    SimMesh sim;
    std::string error;
    if (!sim.start(error)) {
        std::printf("  SKIP  mesh simulator unavailable: %s\n", error.c_str());
        return;
    }

    const std::string dataDir = "/tmp/bfcli-mesh-selftest-" + std::to_string(::getpid());
    const char* previous = ::getenv("BFCLI_DATA_DIR");
    const std::string saved = previous ? previous : "";
    ::setenv("BFCLI_DATA_DIR", dataDir.c_str(), 1);
    uint32_t chatPeer = 0;

    {
        App app;
        app.display_.setHeadlessSize(kScreenW, kScreenH);
        check(app.storage_.init(error), "mesh app storage initializes: " + error);
        app.setupMenus();
        check(app.menu_.size() == 5 && app.menu_.front().label == "Flight controller",
              "the root menu is flight-controller shaped until a radio is opened");

        check(app.mesh_.connect(sim.devicePath(), 115200, error),
              "the app opens the radio: " + error);
        app.linkMode_ = LinkMode::Meshtastic;
        app.beginMeshSession();
        app.setScreen(Screen::Nodes);
        const uint64_t ready = nowMs() + 6000;
        while (nowMs() < ready && !app.mesh_.ready()) {
            sim.pump();
            app.tick(nowMs());
            sleepMs(4);
        }
        check(app.mesh_.ready(), "the app drives the config download through to ready");

        // The five-category contract holds in both link modes; only the first
        // two categories change to match what is actually plugged in.
        check(app.menu_.size() == 5 && app.menu_.front().label == "Mesh network",
              "the mesh root keeps five categories and leads with the radio");
        const MenuPage pages[] = {
            MenuPage::Mesh,         MenuPage::MeshPosition,  MenuPage::ControlsInfo,
            MenuPage::SoundDisplay, MenuPage::ConnectionExit,
        };
        std::set<int> ids;
        int actions = 0;
        bool labelsFit = true;
        const int labelColumns = (kScreenW - 48) / kGlyphW;
        for (MenuPage page : pages) {
            app.openMenuPage(page);
            for (const MenuItem& item : app.currentMenuItems()) {
                ++actions;
                ids.insert(item.id);
                labelsFit &= static_cast<int>(item.label.size()) <= labelColumns;
            }
        }
        check(actions == 18 && ids.size() == 18,
              "every mesh action has exactly one category owner");
        check(labelsFit, "every mesh menu label fits the 6x8 grid");
        app.openMenuPage(MenuPage::Root);

        app.setScreen(Screen::Nodes);
        check(app.nodeRowCount() == 4,
              "the node screen lists the broadcast channel plus every radio heard");
        check(app.peerForNodeRow(0) == kMeshBroadcast, "row zero is the broadcast channel");
        app.render();
        check(app.display_.surface().valid(), "the node list renders offscreen");

        KeyEvent down;
        down.key = Key::Down;
        app.handleKey(down);
        KeyEvent enter;
        enter.key = Key::Enter;
        app.handleKey(enter);
        check(app.screen_ == Screen::Chat, "Enter on a node row opens its conversation");
        chatPeer = app.chatPeer_;
        check(chatPeer != kMeshBroadcast, "the second row is a radio, not the broadcast channel");

        for (char c : std::string("ping")) {
            KeyEvent key;
            key.key = Key::Char;
            key.ch = c;
            app.handleKey(key);
        }
        app.handleKey(enter);
        const uint64_t sent = nowMs() + 4000;
        while (nowMs() < sent) {
            sim.pump();
            app.tick(nowMs());
            const std::vector<MeshMessage>* log = app.mesh_.conversation(chatPeer);
            if (log && !log->empty() && log->back().state == MeshMessageState::Delivered) break;
            sleepMs(4);
        }
        const std::vector<MeshMessage>* log = app.mesh_.conversation(chatPeer);
        check(log != nullptr && !log->empty() && log->back().text == "ping" &&
                  log->back().state == MeshMessageState::Delivered,
              "typing into the chat screen sends the message and then confirms it");
        app.render();
        check(app.display_.surface().valid(), "the conversation renders offscreen");
        check(!app.chatRows_.empty(), "the conversation produced wrapped display rows");

        // The transcript is written as it happens, not only at shutdown.
        const std::string path = app.storage_.meshDir() + "/" + meshChatFileName(chatPeer);
        std::string text, readError;
        check(app.storage_.readFile(path, text, readError),
              "the conversation is written to the data directory: " + readError);
        const std::vector<MeshMessage> reloaded = parseMeshChat(text);
        check(!reloaded.empty() && reloaded.back().text == "ping",
              "the saved transcript reads back as the same conversation");

        KeyEvent escape;
        escape.key = Key::Escape;
        app.handleKey(escape);
        check(app.screen_ == Screen::Nodes,
              "Escape leaves a conversation for the node list it came from");

        // The radio log stays reachable and keeps the firmware own output.
        app.setScreen(Screen::Terminal);
        app.render();
        check(app.display_.surface().valid(), "the radio log renders offscreen");
        KeyEvent nodesKey;
        nodesKey.key = Key::Char;
        nodesKey.ch = 'n';
        app.handleKey(nodesKey);
        check(app.screen_ == Screen::Nodes, "N returns from the radio log to the node list");

        // Disconnecting hands the menus back to the flight controller.
        app.requestDisconnect(false);
        check(!app.mesh_.connected() && app.screen_ == Screen::Ports,
              "disconnecting a radio returns to the port picker");
        check(app.menu_.front().label == "Flight controller",
              "the root menu goes back to its flight-controller shape");
    }

    // A second launch has to find the conversation where the first left it.
    {
        App app;
        app.display_.setHeadlessSize(kScreenW, kScreenH);
        check(app.storage_.init(error), "a second app instance initializes storage");
        app.loadMeshChats();
        const std::vector<MeshMessage>* log = app.mesh_.conversation(chatPeer);
        bool foundPing = false;
        if (log) {
            for (const MeshMessage& message : *log) {
                if (message.text == "ping") foundPing = true;
            }
        }
        check(foundPing, "a saved conversation is reloaded on the next launch");
    }

    if (saved.empty()) ::unsetenv("BFCLI_DATA_DIR");
    else ::setenv("BFCLI_DATA_DIR", saved.c_str(), 1);
}


int runSelfTest() {
    std::printf("%s self-test (commit %s)\n\n", kAppName, kBuildCommit);
    testAudio();
    testKeys();
    testRawTerminalMode();
    testTerminal();
    testEditor();
    testCompleter();
    testRiskAndParsing();
    testDiagnostics();
    testThermalTrip();
    testStorage();
#if defined(__linux__)
    {
        ThermalFixture action = makeThermalFixture("4");
        check(action.ok, "app-level thermal trip fixture is created");
        if (action.ok) {
            App app;
            app.display_.setHeadlessSize(kScreenW, kScreenH);
            std::string error;
            check(app.storage_.init(error), "app-level trip incident storage initializes: " + error);
            app.thermalTrip_ = ThermalTrip(action.paths);
            app.thermalTrip_.inspect(action.device);
            check(app.thermalTrip_.arm(error), "app-level thermal trip arms: " + error);
            app.performThermalTrip(82);
            check(app.thermalTrip_.latched() && !app.session_.connected(),
                  "critical action cuts power and closes serial");
            check(app.modal_ && app.modalTitle_ == "THERMAL TRIP - EXT 5V OFF" &&
                      app.modalBody_.find("Why: stop USB-fed bench heating") != std::string::npos &&
                      app.modalBody_.find("Battery can still power") != std::string::npos &&
                      app.modalBody_.find("Incident:") != std::string::npos,
                  "trip notice tells the operator what happened, why, and what remains powered");
            checkEq(fixtureRead(action.railPath), "0", "app-level trip leaves EXT 5V off");
            app.render();
            check(app.display_.surface().valid(), "thermal-trip incident notice renders offscreen");
        }
    }
#endif
    testGraphics();
    section("menu hierarchy and typography");
    {
        App app;
        app.display_.setHeadlessSize(kScreenW, kScreenH);
        app.setupMenus();
        check(app.menu_.size() == 5, "main menu exposes five coherent categories");

        const MenuPage pages[] = {
            MenuPage::FlightController,
            MenuPage::BackupRestore,
            MenuPage::ControlsInfo,
            MenuPage::SoundDisplay,
            MenuPage::ConnectionExit,
        };
        std::set<int> actionIds;
        bool labelsFit = true;
        bool labelsClean = true;
        int actionCount = 0;
        const int labelColumns = (kScreenW - 48) / kGlyphW;
        for (const MenuItem& item : app.menu_) {
            labelsClean &= item.label.size() < 3 ||
                           item.label.compare(item.label.size() - 3, 3, "...") != 0;
        }
        for (const MenuItem& item : app.quick_) {
            labelsClean &= item.label.size() < 3 ||
                           item.label.compare(item.label.size() - 3, 3, "...") != 0;
        }
        for (MenuPage page : pages) {
            app.openMenuPage(page);
            for (const MenuItem& item : app.currentMenuItems()) {
                ++actionCount;
                actionIds.insert(item.id);
                labelsFit &= static_cast<int>(item.label.size()) <= labelColumns;
                labelsClean &= item.label.size() < 3 ||
                               item.label.compare(item.label.size() - 3, 3, "...") != 0;
            }
        }
        check(actionCount == 16 && actionIds.size() == 16,
              "all 16 actions have exactly one category owner");
        check(labelsFit, "every category action fits the original 6x8 menu grid");
        check(labelsClean, "menu labels do not use trailing dot-dot-dot affordances");
        check(app.bodyRows(false) == 18, "menu typography restores eighteen 6x8 rows");

        app.openMenuPage(MenuPage::Root);
        app.screen_ = Screen::Menu;
        app.render();
        Surface menuSurface = app.display_.surface();
        check(menuSurface.row(kBodyY + kGlyphH - 1)[300] == theme::accent &&
                  menuSurface.row(kBodyY + kGlyphH)[300] == theme::bg,
              "root selection plate follows the original 8px row lattice");

        KeyEvent key;
        key.key = Key::Enter;
        app.handleKey(key);
        check(app.screen_ == Screen::Menu &&
                  app.menuPage_ == MenuPage::FlightController &&
                  app.currentMenuItems().size() == 2,
              "selecting a category opens its submenu");
        app.render();
        menuSurface = app.display_.surface();
        check(menuSurface.row(17)[14] == theme::accent &&
                  menuSurface.row(18)[15] == theme::bg &&
                  menuSurface.row(39)[20] == theme::bg &&
                  menuSurface.row(47)[20] == theme::accent,
              "category submenu is an inverted bordered modal over the root");

        key.key = Key::Down;
        app.handleKey(key);
        key.key = Key::Enter;
        app.handleKey(key);
        check(app.screen_ == Screen::Quick &&
                  app.menuPage_ == MenuPage::FlightController,
              "nested quick commands open from the controller modal");
        key.key = Key::Escape;
        app.handleKey(key);
        check(app.screen_ == Screen::Menu &&
                  app.menuPage_ == MenuPage::FlightController,
              "Escape returns from quick commands to its category modal");
        app.handleKey(key);
        check(app.screen_ == Screen::Menu && app.menuPage_ == MenuPage::Root,
              "Escape returns from a category to the main menu");
        app.handleKey(key);
        check(app.screen_ == Screen::Terminal,
              "Escape from the main menu returns to the terminal");

        app.openMenuPage(MenuPage::ControlsInfo);
        app.screen_ = Screen::Menu;
        app.submenuList_ = ListState{};
        key.key = Key::Enter;
        app.handleKey(key);
        check(app.screen_ == Screen::Keymap,
              "keymap opens from the controls category");
        key.key = Key::Escape;
        app.handleKey(key);
        check(app.screen_ == Screen::Menu && app.menuPage_ == MenuPage::ControlsInfo,
              "Escape returns from keymap to its controls category");

        app.submenuList_.sel = 1;
        key.key = Key::Enter;
        app.handleKey(key);
        check(app.screen_ == Screen::Help,
              "help opens from the controls category");
        key.key = Key::Escape;
        app.handleKey(key);
        check(app.screen_ == Screen::Menu && app.menuPage_ == MenuPage::ControlsInfo,
              "Escape returns from help to its controls category");

        app.openMenuPage(MenuPage::BackupRestore);
        app.openReturnableScreen(Screen::Files);
        key.key = Key::Escape;
        app.handleKey(key);
        check(app.screen_ == Screen::Menu && app.menuPage_ == MenuPage::BackupRestore,
              "Escape returns from saved backups to its backup category");
    }
    section("brand and About");
    checkEq(kAppName, "GNDHOG ZERO", "final project name");
    checkEq(kAuthor, "0ct0", "author credit");
    check(!std::string(kBuildCommit).empty(), "build identity is present");
    check(128 + textWidth(std::string("commit: ") + kBuildCommit) <= kScreenW,
          "build identity fits beside the mascot, including dirty suffix");
    check(4 + textWidth(kAboutJoke) <= kScreenW, "complete FPV joke fits on screen");
    {
        App app;
        app.display_.setHeadlessSize(kScreenW, kScreenH);
        app.opt_.muteSound = true;
        app.soundEnabled_ = false;
        app.audio_.setEnabled(false);
        app.toggleSound();
        check(!app.soundEnabled_ && !app.audio_.enabled() &&
                  app.status_.find("locked off by --mute") != std::string::npos,
              "--mute remains a launch-wide override when the menu toggle is pressed");

        // Same contract for the GNSS switch. One smoke test launched with
        // --no-gnss must not disable the receiver for every launch after it.
        app.opt_.gnssEnabled = false;
        app.gnssWanted_ = true;
        app.toggleGnss();
        check(!app.gnssWanted_ &&
                  app.status_.find("locked off by --no-gnss") != std::string::npos,
              "--no-gnss is a launch-wide override the menu toggle cannot undo");
        checkEq(app.config_.get("gnss.enabled", "unwritten"), "unwritten",
                "a session GNSS override never writes itself into the saved config");
        app.opt_.gnssEnabled = true;
        app.gnssWanted_ = false;
        app.toggleGnss();
        check(app.gnssWanted_ && app.config_.getBool("gnss.enabled", false),
              "a deliberate menu toggle does save the GNSS choice");
        KeyEvent key;
        key.key = Key::Char;
        key.ch = 'a';
        app.handleKey(key);
        check(app.screen_ == Screen::About, "A opens About without a flight controller");
        check(!app.session_.connected(), "About does not connect a flight controller");
        app.render();
        Surface s = app.display_.surface();
        bool transparentExterior = true;
        bool disjointMasks = true;
        for (int y = 0; y < kMascotSize; ++y) {
            for (int x = 0; x < kMascotSize; ++x) {
                const int bit = y * kMascotSize + x;
                const uint8_t mask = static_cast<uint8_t>(0x80u >> (bit % 8));
                const bool black = (kMascotBlackBits[bit / 8] & mask) != 0;
                const bool white = (kMascotWhiteBits[bit / 8] & mask) != 0;
                disjointMasks &= !(black && white);
                if (x < 5 || x >= kMascotSize - 5 ||
                    y < 5 || y >= kMascotSize - 5) {
                    transparentExterior &= !black && !white;
                }
            }
        }
        check(disjointMasks, "mascot black and white masks do not overlap");
        check(transparentExterior, "mascot exterior is transparent beyond the badge perimeter");
        bool identical = true;
        for (int y = 0; y < kMascotSize; ++y) {
            for (int x = 0; x < kMascotSize; ++x) {
                const int bit = y * kMascotSize + x;
                const uint8_t mask = static_cast<uint8_t>(0x80u >> (bit % 8));
                Color want = theme::bg;
                if (kMascotBlackBits[bit / 8] & mask) want = theme::black;
                else if (kMascotWhiteBits[bit / 8] & mask) want = rgb(0xff, 0xff, 0xff);
                identical &= s.row(18 + y)[4 + x] == want;
            }
        }
        check(identical, "About preserves black, white, and transparent mascot pixels");
        const std::string footerAction = "Esc back";
        app.drawHintBar(s, std::string(200, 'x'), footerAction);
        const int actionX = kScreenW - textWidth(footerAction) - 8;
        check(s.row(kScreenH - kHintH + 2)[actionX + 4] == theme::accent &&
                  s.row(kScreenH - 1)[kScreenW - 1] == theme::panelHi,
              "footer action stays visible when contextual guidance is too long");
        key.key = Key::Escape;
        key.repeat = true;
        app.handleKey(key);
        check(app.screen_ == Screen::About, "held Escape cannot dismiss About");
        key.repeat = false;
        app.handleKey(key);
        check(app.screen_ == Screen::Ports, "About returns to its offline entry screen");
        app.screen_ = Screen::Menu;
        // Exercise the menu return route without opening hardware or changing
        // saved settings. Menu selection itself is covered in the UI preview.
        app.openReturnableScreen(Screen::About);
        key.key = Key::Enter;
        app.handleKey(key);
        check(app.screen_ == Screen::Menu, "About returns to its menu entry screen");
        bool cancelActionRan = false;
        app.confirm("Choice", "Cancel callback check", "Yes", nullptr,
                    [&]() { cancelActionRan = true; });
        key.key = Key::Escape;
        app.handleKey(key);
        check(cancelActionRan && !app.modal_,
              "a deliberate modal cancel can continue a paused connection workflow");
        Canvas clipped(16, 16);
        Surface small = clipped.surface();
        fill(small, theme::echo);  // Deliberately not the About background.
        drawMascot(small, -100, -100);
        drawMascot(small, 20, 20);
        drawMascot(small, -200, -200);
        bool transparentAndClipped = true;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                Color want = theme::echo;
                if (x < 12 && y < 12) {
                    const int bit = (y + 100) * kMascotSize + x + 100;
                    const uint8_t mask = static_cast<uint8_t>(0x80u >> (bit % 8));
                    if (kMascotBlackBits[bit / 8] & mask) want = theme::black;
                    else if (kMascotWhiteBits[bit / 8] & mask) want = rgb(0xff, 0xff, 0xff);
                }
                transparentAndClipped &= small.row(y)[x] == want;
            }
        }
        check(transparentAndClipped, "mascot clips and preserves an arbitrary background");
    }
    {
        SimFc sim;
        std::string error;
        check(sim.start(error), "VTX-guard simulator starts: " + error);
        if (error.empty()) {
            sim.setCoreTemperatureC(72);
            App app;
            app.display_.setHeadlessSize(kScreenW, kScreenH);
            app.nextTemperatureCheckMs_ = nowMs();
            check(app.session_.connect(sim.devicePath(), 115200, error),
                  "VTX-guard app connects: " + error);
            app.setScreen(Screen::Terminal);
            const uint64_t promptDeadline = nowMs() + 3000;
            while (nowMs() < promptDeadline && !app.modal_) {
                sim.pump();
                app.tick(nowMs());
                sleepMs(4);
            }
            check(app.modal_ && app.modalTitle_ == "Bench VTX guard?",
                  "a ready controllable VTX pauses connection for an operator choice");
            KeyEvent enter;
            enter.key = Key::Enter;
            app.handleKey(enter);
            const uint64_t readyDeadline = nowMs() + 5000;
            while (nowMs() < readyDeadline && !app.session_.ready()) {
                sim.pump();
                app.tick(nowMs());
                sleepMs(4);
            }
            check(app.session_.ready() && app.session_.vtxBenchGuardActive(),
                  "accepted pit mode is verified before CLI becomes ready");
            check(app.modal_ && app.modalTitle_ == "FC temperature warning" &&
                      app.session_.coreTemperatureC() == 72,
                  "the automatic status capture opens the 70C MCU-temperature warning");
            app.handleKey(enter);

            const uint64_t warmSequence = app.session_.coreTemperatureSequence();
            sim.setCoreTemperatureC(82);
            app.nextTemperatureCheckMs_ = nowMs();
            const uint64_t criticalDeadline = nowMs() + 4000;
            while (nowMs() < criticalDeadline &&
                   (app.session_.coreTemperatureSequence() == warmSequence ||
                    !app.session_.ready())) {
                sim.pump();
                app.tick(nowMs());
                sleepMs(4);
            }
            check(app.session_.coreTemperatureSequence() > warmSequence,
                  "the idle temperature watch repeats without waiting for a reconnect");
            check(app.modal_ && app.modalTitle_ == "CRITICAL FC TEMPERATURE" &&
                      app.session_.coreTemperatureC() == 82,
                  "a later rise from warning to 80C escalates to critical");
            app.handleKey(enter);
            app.requestDisconnect(false);
            check(app.modal_ && app.modalTitle_ == "Restore VTX state?",
                  "guarded disconnect asks before restoring flight state");
            app.handleKey(enter);
            const uint64_t disconnectDeadline = nowMs() + 3000;
            while (nowMs() < disconnectDeadline && app.session_.connected()) {
                sim.pump();
                app.tick(nowMs());
                sleepMs(4);
            }
            check(!app.session_.connected() && app.screen_ == Screen::Ports,
                  "restoration drains the reboot request and returns to the port picker");
        }
    }
    {
        SimFc sim;
        std::string error;
        check(sim.start(error), "field-check simulator starts: " + error);
        if (error.empty()) {
            App app;
            app.display_.setHeadlessSize(kScreenW, kScreenH);
            check(app.session_.connect(sim.devicePath(), 115200, error),
                  "field-check app connects: " + error);
            check(spin(sim, app.session_, 4000, [&] {
                if (app.session_.awaitingVtxChoice()) app.session_.skipVtxBenchGuard();
                return app.session_.ready();
            }),
                  "field-check app reaches the prompt");
            app.setupMenus();
            app.openMenuPage(MenuPage::FlightController);
            app.setScreen(Screen::Menu);
            app.runFieldCheck();
            const uint64_t deadline = nowMs() + 8000;
            while (nowMs() < deadline && app.diagnosticRunning_) {
                sim.pump();
                app.tick(nowMs());
                sleepMs(4);
            }
            check(!app.diagnosticRunning_, "field check finishes all read-only queries");
            check(app.diagnosticReport_.complete(), "field check produces a complete report");
            check(app.screen_ == Screen::Diagnostics, "field check stays on its summary screen");
            app.render();
            check(app.display_.surface().valid(), "field-check summary renders offscreen");
            KeyEvent escape;
            escape.key = Key::Escape;
            app.handleKey(escape);
            check(app.screen_ == Screen::Menu &&
                      app.menuPage_ == MenuPage::FlightController,
                  "field-check summary returns to its controller category");
            app.session_.disconnect();
        }
    }
    testSession();
    testVtxGuardFailurePaths();
    testFullScrollback();
    testProtobufWire();
    testMeshFraming();
    testMeshCodec();
    testMeshChatFiles();
    testGnss();
    testMeshSession();
    testMeshApp();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

} // namespace bf
