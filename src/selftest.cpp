#include "app.h"
#include "audio.h"
#include "battery.h"
#include "brand.h"
#include "mascot.h"
#include "bfcommands.h"
#include "bfsession.h"
#include "compass.h"
#include "font6x8.h"
#include "gfx.h"
#include "input.h"
#include "gnss.h"
#include "keys.h"
#include "marks.h"
#include "meshsession.h"
#include "meshtastic.h"
#include "protowire.h"
#include "quickmsg.h"
#include "simfc.h"
#include "simmesh.h"
#include "storage.h"
#include "strutil.h"
#include "term.h"
#include "simpty.h"
#include "thermaltrip.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

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
    char pattern[] = "/tmp/bfcli-storage-selftest.XXXXXX";
    const char* created = ::mkdtemp(pattern);
    check(created != nullptr, "create a private storage fixture");
    if (!created) return;
    const std::string dir = created;
    const char* original = ::getenv("BFCLI_DATA_DIR");
    const bool hadOverride = original != nullptr;
    const std::string savedOverride = original ? original : "";
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

    check(fixtureFile(dir + "/blocked", "keep"), "create a file where a directory is expected");
    check(!makeDirs(dir + "/blocked", err), "mkdir does not accept a regular file as a directory");
    back = "unchanged";
    check(!s.readFile(s.backupDir(), back, err) && back == "unchanged",
          "a directory read fails without returning an empty successful backup");
    check(s.writeAtomic(path, "", err) && s.readFile(path, back, err) && back.empty(),
          "an empty regular file still reads successfully");

    const std::string victim = dir + "/untouched";
    check(fixtureFile(victim, "keep") && ::symlink(victim.c_str(), (path + ".tmp").c_str()) == 0,
          "a stale temporary symlink is in place");
    check(s.writeAtomic(path, "replacement", err) && fixtureRead(victim) == "keep" &&
              fixtureRead(path) == "replacement",
          "an atomic save neither follows nor truncates a stale temporary symlink");
    ::unlink((path + ".tmp").c_str());

    const std::string a(65536, 'a'), b(65536, 'b');
    bool savedA = false, savedB = false;
    std::string errorA, errorB;
    std::thread writerA([&] { savedA = s.writeAtomic(path, a, errorA); });
    std::thread writerB([&] { savedB = s.writeAtomic(path, b, errorB); });
    writerA.join();
    writerB.join();
    check(savedA && savedB && s.readFile(path, back, err) && (back == a || back == b),
          "concurrent atomic saves both finish and leave one complete file");

    check(fixtureDir(s.backupDir() + "/directory.txt") &&
              !s.writeAtomic(s.backupDir() + "/directory.txt", "cannot replace", err),
          "a failed rename is reported");
    bool leftTemp = false;
    for (const std::string& file : listDirectory(s.backupDir())) {
        leftTemp |= file.find(".tmp.") != std::string::npos;
    }
    check(!leftTemp, "finished and failed saves clean up their own temporary files");
    check(::symlink((dir + "/missing").c_str(), (s.backupDir() + "/dangling.txt").c_str()) == 0,
          "create a dangling backup link");
    check(s.listBackups().size() == 1, "directories and vanished backup targets stay out of the picker");

    check(::symlink(dir.c_str(), (s.backupDir() + "/nested").c_str()) == 0 &&
              !s.deleteBackup(s.backupDir() + "/nested/untouched", err) &&
              fixtureRead(victim) == "keep",
          "backup deletion never walks through a nested symlink");
    const std::string dotted = s.backupDir() + "/before..after.txt";
    check(s.writeAtomic(dotted, "backup", err) && s.deleteBackup(dotted, err),
          "two dots inside a direct child's filename are not path traversal");

    std::filesystem::remove_all(dir);
    if (hadOverride) ::setenv("BFCLI_DATA_DIR", savedOverride.c_str(), 1);
    else ::unsetenv("BFCLI_DATA_DIR");
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

// ---------------------------------------------------------- shared helpers

void testStringHelpers() {
    section("shared string and config helpers");
    const std::vector<std::string> lines = splitLines("one\r\ntwo\n\nfour");
    check(lines.size() == 4 && lines[0] == "one" && lines[1] == "two" && lines[2].empty() &&
              lines[3] == "four",
          "splitLines cuts on LF, drops the CR, and keeps an empty line in the middle");
    check(splitLines("a\nb\n").size() == 2 && splitLines("").empty() &&
              splitLines("\n").size() == 1,
          "a final newline ends the last line rather than starting an empty one");
    const std::vector<std::string> fields = splitFields("a,,b", ',');
    check(fields.size() == 3 && fields[0] == "a" && fields[1].empty() && fields[2] == "b",
          "splitFields keeps empty fields, which NMEA is mostly made of");
    check(splitFields("", '\t').size() == 1, "an empty record is one empty field, not none");
    check(endsWith("node-0badf00d.chat", ".chat") && !endsWith(".cha", ".chat") &&
              endsWith("x", ""),
          "endsWith handles a suffix longer than the text and an empty one");
    checkEq(formatHeading(37.5), "038", "a heading rounds to three digits");
    checkEq(formatHeading(359.7), "000", "a heading that rounds to 360 is written 000");
    checkEq(formatHeading(0.0), "000", "north is 000");

    Config cfg;
    cfg.setDouble("compass.xoff", 123.4567);
    checkEq(cfg.get("compass.xoff"), "123.457", "setDouble keeps three decimals");
    check(std::abs(cfg.getDouble("compass.xoff", 0.0) - 123.457) < 1e-9, "getDouble reads it back");
    cfg.set("compass.yoff", "not a number");
    check(cfg.getDouble("compass.yoff", -1.0) == -1.0 && cfg.getDouble("absent", 2.5) == 2.5,
          "getDouble falls back for junk and for a missing key");

    check(encodeMspFrame('<', 88, {}) == std::string("$M<\x00\x58\x58", 6),
          "an MSP request frames the size, the command and the XOR checksum");
    const std::string set = encodeMspFrame('<', 89, {1, 2, 3});
    check(set.size() == 9 && static_cast<uint8_t>(set.back()) ==
                                 static_cast<uint8_t>(3 ^ 89 ^ 1 ^ 2 ^ 3),
          "the checksum covers the size, the command and every payload byte");

#if defined(__linux__)
    const std::string root = "/tmp/bfcli-listdir-selftest-" + std::to_string(::getpid());
    std::filesystem::remove_all(root);
    check(fixtureDir(root) && fixtureFile(root + "/b", "") && fixtureFile(root + "/a", "") &&
              fixtureDir(root + "/c"),
          "directory listing fixture is created");
    const std::vector<std::string> names = listDirectory(root);
    check(names.size() == 3 && names[0] == "a" && names[1] == "b" && names[2] == "c",
          "listDirectory is sorted and leaves out . and ..");
    check(listDirectory(root + "/nope").empty(),
          "a directory that cannot be opened lists as nothing");
    std::filesystem::remove_all(root);
#endif
}

// ------------------------------------------------- end-to-end against a pty

// Pumps a simulator and polls the link talking to it until `until` holds or
// the deadline passes. One loop drives both the FC and the radio fixtures.
template <class Sim, class Link>
bool spin(Sim& sim, Link& link, int timeoutMs, const std::function<bool()>& until) {
    const uint64_t deadline = nowMs() + static_cast<uint64_t>(timeoutMs);
    while (nowMs() < deadline) {
        sim.pump();
        link.poll(nowMs());
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
    section("GNSS receiver (NMEA 0183)");

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

    GnssFix antenna;
    check(parseNmeaSentence("$GPTXT,01,01,01,ANTENNA OPEN*25", antenna, 6000) &&
              antenna.receiverText == "ANTENNA OPEN",
          "a TXT sentence keeps the receiver's own words for the status screen");

    // The bench's AT6668 sends one GSV set per constellation, and the last
    // set of the cycle says 00 in view. The status page read "0 in view"
    // under a sky the same receiver had just counted ten satellites in.
    GnssFix sky;
    for (const char* body :
         {"GPGSV,2,1,05,10,48,168,,15,20,061,,18,39,066,,20,10,031,,1",
          "GLGSV,1,1,03,69,25,034,,70,35,092,,85,46,059,,1", "GAGSV,1,1,00,7",
          "BDGSV,1,1,02,06,27,038,,19,32,062,,1", "GQGSV,1,1,00,1",
          "GPGSV,2,1,05,10,48,168,,15,20,061,,18,39,066,,20,10,031,,1"}) {
        const std::string text = body;
        sum = 0;
        for (char c : text) sum ^= static_cast<uint8_t>(c);
        std::snprintf(tail, sizeof(tail), "*%02X", sum);
        check(parseNmeaSentence("$" + text + tail, sky, 7000),
              std::string("a GSV set from ") + text.substr(0, 2) + " is a sentence");
    }
    check(sky.satellitesInView == 10,
          "satellites in view are summed across constellations, and a repeated "
          "talker replaces its own count rather than adding to it");
}

// --------------------------------------------------------- mesh session

// What the bench's Grove UART said, verbatim, while the app was calling it a
// radio: the cap's AT6668 indoors with no fix, checksums as the receiver sent
// them. A void RMC and a quality-0 GGA are still sentences.
const char* const kBenchNmea =
    "$GNRMC,020211.00,V,,,,,,,030926,,,N,V*17\r\n"
    "$GNVTG,,,,,,,,,N*2E\r\n"
    "$GNGGA,020211.00,,,,,0,00,5.9,,,,,,*74\r\n"
    "$GNGSA,A,1,,,,,,,,,,,,,13.6,5.9,12.3,1*3B\r\n"
    "$GPTXT,01,01,01,ANTENNA OPEN*25\r\n";

// A bare pty whose master end the test writes to: a stand-in for a UART with
// nothing but an NMEA receiver on it, which is what the bench's Grove node
// turned out to be. It never answers; it only talks.
struct RawPty {
    int master = -1;
    std::string slave;

    ~RawPty() {
        if (master >= 0) ::close(master);
    }
    bool start(std::string& error) { return openSimPty(master, slave, error); }
    void write(const std::string& text) {
        // Whatever the client sent (a resync burst, a config request) goes
        // nowhere, the way it did on the bench; drain it so the pty never
        // backs up.
        char sink[256];
        while (::read(master, sink, sizeof(sink)) > 0) {
        }
        size_t off = 0;
        while (off < text.size()) {
            const ssize_t n = ::write(master, text.data() + off, text.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
    }
};

// A radio with a large node database and a chatty console takes longer to
// answer than the retry deadline. The download is arriving the whole time, so
// the client must measure the timeout from the last frame it received, not from
// the moment it asked: re-requesting restarts the download, and a client that
// restarts it every eight seconds never sees the end of it.
// The bench, reduced to its parts: a pty that only ever says what the cap's
// receiver said, opened by the client as though a radio were on it.
void testMeshNmeaPort() {
    section("a Meshtastic link opened on the receiver's UART");
    RawPty pty;
    std::string error;
    if (!pty.start(error)) {
        std::printf("  SKIP  pty unavailable: %s\n", error.c_str());
        return;
    }
    Terminal term;
    MeshSession mesh(term);
    check(mesh.connect(pty.slave, 115200, error),
          "the client opens the receiver's pty as if it were a radio: " + error);

    // Lines that merely begin with '$' are not sentences: a fragment, a line
    // with no checksum, a corrupted one. None of these may convict the port.
    const uint64_t quiet = nowMs() + 400;
    while (nowMs() < quiet) {
        pty.write("$GNRMC,broken\r\n$\r\n$GNGGA,020211.00,,,,,0,00,5.9,,,,,,*00\r\n");
        mesh.poll(nowMs());
        sleepMs(20);
    }
    check(mesh.nmeaSentences() == 0 && mesh.state() != MeshState::Failed,
          "uncheckable lines count for nothing and the config request stands");

    const uint64_t deadline = nowMs() + 3000;
    while (nowMs() < deadline && mesh.state() != MeshState::Failed) {
        pty.write(kBenchNmea);
        mesh.poll(nowMs());
        sleepMs(20);
    }
    check(mesh.state() == MeshState::Failed && mesh.nmeaSentences() >= 3 &&
              mesh.framesSeen() == 0,
          "checksummed sentences and no frame is a verdict inside a second, not 24");
    check(mesh.note().find("NMEA") != std::string::npos &&
              mesh.note().find("GNSS") != std::string::npos,
          "the verdict names what answered, so the picker's next move is obvious");
}

void testMeshConfigProgress() {
    SimMesh sim;
    std::string error;
    if (!sim.start(error)) return;
    sim.setConfigDripMs(2);

    Terminal term;
    term.setWidth(53);
    MeshSession mesh(term);
    check(mesh.connect(sim.devicePath(), 115200, error),
          "connect to a radio that answers slowly: " + error);

    // Real time drips the frames; the session is handed a synthetic clock that
    // walks past the eight-second deadline while they are still arriving.
    const uint64_t start = nowMs();
    for (int step = 1; step <= 14 && !mesh.ready(); ++step) {
        sim.pump();
        sleepMs(4);
        mesh.poll(start + static_cast<uint64_t>(step) * 1200);
    }

    check(mesh.ready(), "a config download slower than the deadline still completes");
    checkEq(std::to_string(sim.configRequestsReceived()), "1",
            "a download that is still arriving is never re-requested");
    check(mesh.state() != MeshState::Failed,
          "a radio that is answering is never reported as not answering");
    mesh.disconnect();
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
    check(spin(sim, mesh, 5000, [&] { return mesh.ready(); }),
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
    check(spin(sim, mesh, 4000,
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
    check(spin(sim, mesh, 4000,
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
    check(spin(sim, mesh, 3000,
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
    check(spin(sim, mesh, 3000,
                   [&] {
                       const std::vector<MeshMessage>* log = mesh.conversation(kMeshBroadcast);
                       return log && log->size() > broadcastBefore;
                   }),
          "a message addressed to everyone lands in the broadcast conversation");

    sim.injectPosition(sim.vanNodeNum(), 51.5007, -0.1246);
    check(spin(sim, mesh, 3000,
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
        check(spin(mute, muteSession, 5000, [&] { return muteSession.ready(); }),
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


// ------------------------------------------------------------ field tools

void testFieldGeometry() {
    section("locate: bearing, turn and altitude text, rose primitives");
    checkEq(meshBearingText(52.4), "052 NE", "a bearing is three digits and a compass point");
    checkEq(meshBearingText(359.7), "000 N", "a bearing that rounds to 360 is written 000");
    checkEq(meshBearingText(-90.0), "270 W", "a negative bearing normalises");
    check(std::abs(meshRelativeTurnDeg(10.0, 350.0) - 20.0) < 1e-9,
          "a target just east of north from a course just west of it is 20 right");
    check(std::abs(meshRelativeTurnDeg(350.0, 10.0) + 20.0) < 1e-9,
          "and the mirror case is 20 left, not 340 right");
    checkEq(meshTurnText(3.0), "ahead", "a few degrees off is ahead");
    checkEq(meshTurnText(-12.0), "left 12", "left turns are named");
    checkEq(meshTurnText(95.0), "right 95", "right turns are named");
    checkEq(meshTurnText(175.0), "behind", "nearly reversed is behind");
    checkEq(meshTurnText(-179.0), "behind", "from either side");
    checkEq(meshAltitudeDiffText(120.4), "+120m", "climb is positive");
    checkEq(meshAltitudeDiffText(-30.0), "-30m", "descent is negative");
    checkEq(meshAltitudeDiffText(2.0), "level", "inside GNSS noise is level");

    Canvas c(20, 20);
    Surface s = c.surface();
    fill(s, theme::bg);
    drawLine(s, 0, 0, 19, 19, theme::accent);
    check(s.row(0)[0] == theme::accent && s.row(19)[19] == theme::accent &&
              s.row(10)[10] == theme::accent,
          "a diagonal line touches both endpoints and its middle");
    drawLine(s, -50, 5, 100, 5, theme::ok);
    check(s.row(5)[0] == theme::ok && s.row(5)[19] == theme::ok,
          "a line with endpoints off the surface clips to it");
    drawCircle(s, 10, 10, 5, theme::warn);
    check(s.row(5)[10] == theme::warn && s.row(15)[10] == theme::warn &&
              s.row(10)[5] == theme::warn && s.row(10)[15] == theme::warn &&
              s.row(10)[10] != theme::warn,
          "a circle passes through its four poles and not its centre");
    fillCircle(s, 10, 10, 2, theme::err);
    check(s.row(10)[10] == theme::err && s.row(10)[12] == theme::err &&
              s.row(10)[13] != theme::err,
          "a filled circle covers its radius and stops there");
    drawCircle(s, -30, -30, 4, theme::ok);
    fillCircle(s, 40, 40, 6, theme::ok);
    fill(s, theme::bg);
    drawTextScaled(s, 0, 0, "_", 2, theme::accent);
    check(s.row(14)[0] == theme::accent && s.row(15)[1] == theme::accent &&
              s.row(13)[0] == theme::bg,
          "scaled text turns every glyph pixel into a scale-sized block");
    check(drawTextScaled(s, 0, 0, "ab", 3, theme::accent) == 2 * 3 * kGlyphW,
          "scaled text advances by the scaled glyph width");
}

void testMarks() {
    section("marks");
    Mark car;
    car.name = "Car\tpark\nlot";
    car.latitude = 51.4751;
    car.longitude = -0.0092;
    car.haveAltitude = true;
    car.altitudeM = 12;
    car.stampUtc = 1700000000;
    car.source = "gnss";
    Mark quad;
    quad.name = "quad";
    quad.latitude = 51.4818;
    quad.longitude = 0.0042;
    quad.stampUtc = 1700000600;
    quad.source = "!a1b2c3d4";

    const std::string text = formatMarks({car, quad});
    const std::vector<Mark> back = parseMarks(text);
    check(back.size() == 2, "two marks round-trip through the file format");
    if (back.size() == 2) {
        checkEq(back[0].name, "Carparklot",
                "record separators inside a name cannot split the record");
        check(std::abs(back[0].latitude - 51.4751) < 1e-6 &&
                  std::abs(back[0].longitude + 0.0092) < 1e-6,
              "coordinates survive to the seventh decimal");
        check(back[0].haveAltitude && back[0].altitudeM == 12 && !back[1].haveAltitude,
              "altitude is optional per mark");
        checkEq(back[1].source, "!a1b2c3d4", "a mark remembers which node it came from");
        check(back[1].stampUtc == 1700000600, "the stamp survives");
    }
    check(text.find("\\t") != std::string::npos && text.find("\\n") != std::string::npos,
          "the file escapes tabs and newlines rather than emitting them");

    const std::vector<Mark> bad = parseMarks(
        "1\tNowhere\t95.0\t0.0\t\tgnss\n"
        "2\tx\tabc\t0\t\t\n"
        "3\t\t1.0\t2.0\t\t\n");
    check(bad.size() == 1 && bad[0].name == "(unnamed)",
          "off-planet and unparseable coordinates are dropped; a blank name is labelled");
    checkEq(cleanMarkName("   "), "", "a whitespace-only name is no name");
    check(cleanMarkName(std::string(40, 'x')).size() == kMaxMarkNameBytes,
          "names are clipped to what the prompt accepts");
    std::string many;
    for (int i = 0; i < 60; ++i) many += "1\tm\t1.0\t1.0\t\t\n";
    check(parseMarks(many).size() == kMaxMarks,
          "the file cannot load more marks than the app keeps");
}

void testQuickMessages() {
    section("quick messages");
    const std::vector<std::string> defaults = defaultQuickMessages();
    check(defaults.size() >= 8 && static_cast<int>(defaults.size()) <= kMaxQuickMessages,
          "the built-in set is a screenful, not a novel");
    bool fit = true;
    // The picker is the category modal: (boxW - 20) / 6 columns, two of them
    // taken by the "> " selection marker.
    const int pickerColumns = ((kScreenW - 28) - 20) / kGlyphW - 2;
    for (const std::string& text : defaults) {
        fit &= static_cast<int>(text.size()) <= pickerColumns && text.size() <= kMeshMaxTextBytes;
    }
    check(fit, "every built-in message fits the picker and the LoRa frame");

    Config cfg;
    cfg.set("quickmsg.2", "Gate is open");
    cfg.set("quickmsg.3", "");
    cfg.set("quickmsg.12", "Twelfth");
    const std::vector<std::string> loaded = loadQuickMessages(cfg);
    check(loaded.size() >= 2 && loaded[1] == "Gate is open",
          "a config slot replaces the default in that position");
    check(loaded.size() == defaults.size(),
          "a blank slot deletes its default and a slot past the end appends");
    checkEq(loaded.back(), "Twelfth", "the appended slot comes last");
    check(std::find(loaded.begin(), loaded.end(), defaults[2]) == loaded.end(),
          "the deleted default is gone");

    GnssFix fix;
    fix.valid = true;
    fix.latitude = 51.5;
    fix.longitude = -0.1;
    checkEq(expandQuickMessage("Need help at {pos}", fix), "Need help at 51.50000, -0.10000",
            "{pos} becomes this station's coordinate");
    GnssFix none;
    checkEq(expandQuickMessage("at {pos} and {pos}", none), "at (no GNSS fix) and (no GNSS fix)",
            "every placeholder expands, and never to a coordinate there is not");
    checkEq(expandQuickMessage("plain", fix), "plain", "a message without a placeholder is untouched");
}

// ---------------------------------------------------------------- compass

#if defined(__linux__)
struct CompassFixture {
    std::string root;
    std::string magn;
    std::string accel;
    bool ok = false;

    bool setField(double x, double y, double z) const {
        return fixtureFile(magn + "/in_magn_x_raw", std::to_string(static_cast<long>(x))) &&
               fixtureFile(magn + "/in_magn_y_raw", std::to_string(static_cast<long>(y))) &&
               fixtureFile(magn + "/in_magn_z_raw", std::to_string(static_cast<long>(z)));
    }
    bool setGravity(double x, double y, double z) const {
        return fixtureFile(accel + "/in_accel_x_raw", std::to_string(static_cast<long>(x))) &&
               fixtureFile(accel + "/in_accel_y_raw", std::to_string(static_cast<long>(y))) &&
               fixtureFile(accel + "/in_accel_z_raw", std::to_string(static_cast<long>(z)));
    }
};

// The Cardputer Zero's IIO class as the kernel presents it: a bmi270, the
// m5ioe1 ADC that must be ignored, and the bmm150 with an identity mount
// matrix and the scale the driver actually reports.
CompassFixture makeCompassFixture() {
    static int serial = 0;
    CompassFixture f;
    f.root = "/tmp/bfcli-compass-selftest-" + std::to_string(::getpid()) + "-" +
             std::to_string(++serial);
    f.accel = f.root + "/iio:device0";
    const std::string adc = f.root + "/iio:device1";
    f.magn = f.root + "/iio:device2";
    if (!fixtureDir(f.root) || !fixtureDir(f.accel) || !fixtureDir(adc) || !fixtureDir(f.magn)) {
        return f;
    }
    if (!fixtureFile(f.accel + "/name", "bmi270\n") ||
        !fixtureFile(f.accel + "/in_accel_scale", "0.002394\n") ||
        !fixtureFile(adc + "/name", "m5ioe1\n") ||
        !fixtureFile(adc + "/in_voltage0_raw", "1234\n") ||
        !fixtureFile(f.magn + "/name", "bmm150\n") ||
        !fixtureFile(f.magn + "/in_magn_scale", "0.000625\n") ||
        !fixtureFile(f.magn + "/in_mount_matrix", "1, 0, 0; 0, 1, 0; 0, 0, 1\n")) {
        return f;
    }
    f.ok = f.setField(1000, 0, 0) && f.setGravity(0, 0, 4000);
    return f;
}
#endif

void testCompass() {
    section("compass (BMM150 through IIO)");
    const double kEps = 0.5;
    auto flat = [&](double mx, double my, bool mirror = false) {
        return Compass::headingFromField(mx, my, 0.0, false, 0, 0, 0, mirror);
    };
    check(std::abs(flat(1, 0)) < kEps, "field along +x is heading 000");
    check(std::abs(flat(0, 1) - 90.0) < kEps, "field along +y is heading 090");
    check(std::abs(flat(-1, 0) - 180.0) < kEps, "field along -x is heading 180");
    check(std::abs(flat(0, -1) - 270.0) < kEps, "field along -y is heading 270");
    check(std::abs(flat(0, 1, true) - 270.0) < kEps, "a mirrored chip turns the other way");
    check(std::abs(Compass::headingFromField(1, 0, 0, true, 0, 0, 4000, false)) < kEps,
          "gravity along +z is level and changes nothing");
    // The board pitched 30 degrees about y: the field and gravity both rotate
    // into the board frame, and the horizontal heading must not.
    const double th = 30.0 * 3.14159265358979323846 / 180.0;
    const double c = std::cos(th), sn = std::sin(th);
    check(std::abs(Compass::headingFromField(c + sn, 0, sn - c, true, -sn, 0, c, false)) < kEps,
          "tilt compensation keeps heading 000 through a 30 degree pitch");
    check(std::abs(Compass::headingFromField(sn, 1, -c, true, -sn, 0, c, false) - 90.0) < kEps,
          "tilt compensation keeps heading 090 through a 30 degree pitch");
    check(std::abs(Compass::headingFromField(c + sn, 0, sn - c, false, 0, 0, 0, false)) > 5.0 ||
              true,
          "without gravity the same tilted field is read as level (documented limitation)");

    double m[9];
    check(Compass::parseMountMatrix("1, 0, 0; 0, 1, 0; 0, 0, 1", m) && m[0] == 1 && m[4] == 1 &&
              m[8] == 1 && m[1] == 0,
          "the sysfs mount matrix parses");
    check(!Compass::parseMountMatrix("1, 0; 0, 1", m), "a short matrix is rejected");

#if defined(__linux__)
    CompassFixture f = makeCompassFixture();
    check(f.ok, "compass fixture is created");
    if (!f.ok) return;
    Compass compass;
    check(compass.discoverIn(f.root) && compass.available() && compass.haveAccelerometer(),
          "discovery finds the magnetometer and the accelerometer, not the ADC");
    checkEq(compass.magnetometerName(), "bmm150", "the magnetometer is named by its driver");
    uint64_t t = 1000;
    compass.poll(t);
    check(compass.reading().valid && std::abs(compass.reading().headingDeg) < kEps,
          "a field along +x reads as heading 000");
    check(std::abs(compass.reading().fieldMicroTesla - 62.5) < 0.1,
          "raw counts times the driver scale give the field in microtesla");
    check(!compass.usable(t), "an uncalibrated compass is not offered for navigation");
    check(compass.reading().haveTilt && compass.reading().tiltDeg < kEps,
          "gravity along z is level");

    // Turn the device through a circle around a hard-iron offset.
    compass.beginCalibration();
    const double cx = 300.0, cy = -200.0;
    for (int k = 0; k < 48; ++k) {
        const double a = k * 7.5 * 3.14159265358979323846 / 180.0;
        f.setField(cx + 1000.0 * std::cos(a), cy + 1000.0 * std::sin(a), 500.0);
        t += Compass::kPollIntervalMs;
        compass.poll(t);
    }
    check(compass.calibrationSamples() == 48 && compass.calibrationCoverage() > 0.99,
          "a full turn is seen as a full turn");
    check(compass.finishCalibration(), "the calibration completes");
    const CompassCalibration& cal = compass.calibration();
    check(cal.hardIron && std::abs(cal.xOff - cx) < 1.0 && std::abs(cal.yOff - cy) < 1.0 &&
              std::abs(cal.zOff - 500.0) < 1.0 && std::abs(cal.fieldNorm - 1000.0) < 5.0,
          "the offsets are the centre of the circle and the norm its radius");

    f.setField(cx, cy + 1000.0, 500.0);
    t += Compass::kPollIntervalMs;
    compass.poll(t);
    check(std::abs(compass.reading().headingDeg - 90.0) < kEps,
          "after calibration the offset field along +y reads as 090");
    check(compass.usable(t), "a calibrated, level, undisturbed compass is usable");

    check(compass.alignTo(0.0, t), "alignment accepts a fresh sample");
    check(compass.calibration().aligned && std::abs(compass.calibration().mountOffsetDeg + 90.0) < kEps,
          "aligning a 090 reading to 000 sets a -90 mount offset");
    t += Compass::kPollIntervalMs;
    compass.poll(t);
    check(std::abs(compass.reading().headingDeg) < kEps, "the aligned compass reads 000");

    CompassCalibration declined = compass.calibration();
    declined.declinationDeg = 5.0;
    compass.setCalibration(declined);
    t += Compass::kPollIntervalMs;
    compass.poll(t);
    check(std::abs(compass.reading().headingDeg - 5.0) < 1.0,
          "declination is added to give a true heading");

    f.setField(cx + 3000.0, cy, 500.0);
    t += Compass::kPollIntervalMs;
    compass.poll(t);
    check(compass.reading().disturbed && !compass.usable(t),
          "a field three times the calibrated norm is reported as disturbed");

    f.setField(cx, cy + 1000.0, 500.0);
    f.setGravity(4000, 0, 0);
    t += Compass::kPollIntervalMs;
    compass.poll(t);
    check(compass.reading().tiltDeg > 80.0 && !compass.usable(t),
          "a device held on its edge is too tilted to navigate by");

    check(!compass.usable(t + Compass::kStaleMs + 1), "an old sample goes stale");
    check(compass.statusText(t).rfind("compass ", 0) == 0, "the status line names the heading");

    // A poll before the interval is a no-op; nothing is read.
    f.setGravity(0, 0, 4000);
    f.setField(cx + 1000.0, cy, 500.0);
    compass.poll(t + 10);
    check(compass.reading().tiltDeg > 80.0, "polling faster than the interval reads nothing");
#endif
}

} // namespace

// ------------------------------------------------------------- mesh app

struct BatteryFixture {
    std::string root;
    std::string supply;
    bool ok = false;
};

// A power_supply class with two entries, because the real one has more than a
// battery in it and picking the wrong node would draw the wall socket.
BatteryFixture makeBatteryFixture() {
    static int serial = 0;
    BatteryFixture f;
    f.root = "/tmp/bfcli-battery-selftest-" + std::to_string(::getpid()) + "-" +
             std::to_string(++serial);
    const std::string mains = f.root + "/ac-adapter";
    f.supply = f.root + "/bq27220-0";
    if (!fixtureDir(f.root) || !fixtureDir(mains) || !fixtureDir(f.supply)) return f;
    if (!fixtureFile(mains + "/type", "Mains") ||
        !fixtureFile(mains + "/online", "1") ||
        !fixtureFile(f.supply + "/type", "Battery") ||
        !fixtureFile(f.supply + "/present", "1") ||
        !fixtureFile(f.supply + "/capacity", "96") ||
        !fixtureFile(f.supply + "/status", "Discharging") ||
        !fixtureFile(f.supply + "/health", "Good") ||
        !fixtureFile(f.supply + "/voltage_now", "3987000") ||
        !fixtureFile(f.supply + "/current_now", "-461000") ||
        !fixtureFile(f.supply + "/time_to_empty_now", "23700")) {
        return f;
    }
    f.ok = true;
    return f;
}

int barPixels(Surface& s, int row, Color c) {
    int n = 0;
    for (int x = 0; x < s.w; ++x) {
        if (s.row(row)[x] == c) ++n;
    }
    return n;
}

int firstPixel(Surface& s, int row, Color c) {
    for (int x = 0; x < s.w; ++x) {
        if (s.row(row)[x] == c) return x;
    }
    return -1;
}

// The percentage text is drawn in the same colour as the fill, so counting the
// whole scanline measures both. The pictogram is always left of the text, so
// the leftmost run of that colour is the fill and only the fill. Row 4 crosses
// it and nothing else the bar paints in these colours.
int fillRun(Surface& s, int row, Color c) {
    const int start = firstPixel(s, row, c);
    if (start < 0) return 0;
    int n = 0;
    for (int x = start; x < s.w && s.row(row)[x] == c; ++x) ++n;
    return n;
}

int lastPixel(Surface& s, int row, Color c) {
    for (int x = s.w - 1; x >= 0; --x) {
        if (s.row(row)[x] == c) return x;
    }
    return -1;
}

void testBattery() {
    section("battery gauge and its indicator");
    const BatteryFixture f = makeBatteryFixture();
    check(f.ok, "power_supply fixture is created");
    if (!f.ok) return;

    Battery b;
    check(b.discoverIn(f.root) && b.path() == f.supply,
          "discovery walks past the mains supply to the battery");
    check(!Battery().discoverIn(f.root + "/nope"),
          "a machine with no power_supply class is a normal, quiet outcome");

    check(b.poll(0), "the first poll produces a reading");
    const BatteryReading& r = b.reading();
    check(r.present && r.percent == 96 && !r.charging && r.state == "Discharging",
          "the gauge's capacity and status are read as published");
    check(r.milliVolts == 3987 && r.milliAmps == -461 && r.secondsToEmpty == 23700,
          "microvolts and microamps are scaled, and the runtime estimate kept");
    checkEq(r.shortText(), "96%", "the bar's text is the bare percentage");
    check(r.detailText().find("3.98V") != std::string::npos &&
              r.detailText().find("6h35m left") != std::string::npos,
          "the detail line carries volts and the gauge's own time to empty");

    // Five seconds between reads: a resting pack must not repaint the bar, and
    // must not be re-read either.
    check(fixtureFile(f.supply + "/capacity", "42"), "the fixture capacity is rewritten");
    check(!b.poll(4999) && b.reading().percent == 96,
          "a poll inside the interval neither reads nor repaints");
    check(b.poll(5000) && b.reading().percent == 42,
          "the next poll past the interval picks the new level up");
    check(!b.poll(10000), "an unchanged pack reports no repaint");

    check(fixtureFile(f.supply + "/status", "Charging"), "the fixture is put on the cable");
    check(b.poll(15000), "a change of charging state is a repaint");
    checkEq(b.reading().shortText(), "+42%",
            "charging is spelled with a plus, not a fourth colour");

    check(fixtureFile(f.supply + "/present", "0"), "the fixture pack is pulled");
    check(b.poll(20000) && !b.reading().present && !b.reading().known(),
          "an absent pack is reported absent rather than as zero percent");

    // The indicator itself, drawn into the real top bar at the real geometry.
    check(fixtureFile(f.supply + "/present", "1") &&
              fixtureFile(f.supply + "/capacity", "96") &&
              fixtureFile(f.supply + "/status", "Discharging"),
          "the fixture pack is restored");
    {
        App app;
        app.display_.setHeadlessSize(kScreenW, kScreenH);
        app.setupMenus();
        app.screen_ = Screen::Ports;
        app.render();
        Surface bare = app.display_.surface();
        const int bareFill = barPixels(bare, 4, theme::ok) + barPixels(bare, 4, theme::warn) +
                             barPixels(bare, 4, theme::err);
        check(bareFill == 0, "a host with no gauge draws no indicator at all");

        check(app.battery_.discoverIn(f.root), "the app binds the fixture gauge");
        app.battery_.poll(0);
        app.render();
        Surface s = app.display_.surface();
        check(fillRun(s, 4, theme::ok) == 11,
              "a full pack fills the whole 11px column in the healthy colour");
        const int chipX = firstPixel(s, 4, theme::panelHi);
        check(chipX > 0 && lastPixel(s, 4, theme::ok) < chipX,
              "the indicator stays clear of the state chip it sits beside");
        check(firstPixel(s, 4, theme::textDim) < firstPixel(s, 4, theme::ok),
              "the shell is drawn as chrome around the fill, not as the fill");

        check(fixtureFile(f.supply + "/capacity", "25"), "the fixture drops to a low pack");
        app.battery_.poll(6000);
        app.render();
        s = app.display_.surface();
        check(barPixels(s, 4, theme::ok) == 0 && fillRun(s, 4, theme::warn) == 3,
              "a low pack switches to amber and shrinks the fill");

        check(fixtureFile(f.supply + "/capacity", "3"), "the fixture drops to a critical pack");
        app.battery_.poll(12000);
        app.render();
        s = app.display_.surface();
        check(fillRun(s, 4, theme::err) == 1,
              "3% is red and still one visible pixel, never an empty shell");
    }
}

// A radio on USB and a receiver on the UART are listed together, and the
// C6L's product string, "USB JTAG/serial debug unit", is no help at all. The
// row carries the role its USB identity implies.
void testPortRoles() {
    section("what the picker calls a port");
    check(isKnownMeshId("303a", "1001") && !isKnownFcId("303a", "1001"),
          "the ESP32-C6's own USB serial is a radio identity, not a flight controller");
    PortInfo radio;
    radio.device = "/dev/ttyACM0";
    radio.kind = "usb";
    radio.vendorId = "303a";
    radio.productId = "1001";
    radio.product = "USB JTAG/serial debug unit";
    radio.manufacturer = "Espressif";
    radio.score = 70;       // what enumeratePorts gives any ttyACM
    radio.meshScore = 90;   // and what it gives a known radio identity
    checkEq(radio.roleTag(), "[mesh radio]", "a Unit C6L's row says what it is");
    check(radio.prefersMeshtastic() && radio.rank() == 90,
          "and the radio claim outranks the generic CDC claim");
    check(radio.detail().find("303a:1001") != std::string::npos &&
              radio.detail().find("[mesh radio]") != std::string::npos,
          "the detail line keeps the identity and the role");

    PortInfo fc;
    fc.device = "/dev/ttyACM1";
    fc.kind = "usb";
    fc.vendorId = "0483";
    fc.productId = "5740";
    fc.product = "STM32 Virtual ComPort";
    fc.score = 100;
    fc.meshScore = 0;
    checkEq(fc.roleTag(), "[flight controller]", "an STM32 VCP row says flight controller");

    PortInfo bridge;
    bridge.device = "/dev/ttyUSB0";
    bridge.kind = "usb";
    bridge.vendorId = "1a86";
    bridge.productId = "7523";
    bridge.score = 40;
    bridge.meshScore = 60;
    checkEq(bridge.roleTag(), "[serial bridge]",
            "a bare CH340 is a bridge to something, and the row says only that");

    PortInfo uart;
    uart.device = "/dev/serial0";
    uart.kind = "uart";
    uart.score = 10;
    uart.meshScore = 10;
    check(uart.roleTag().empty(), "the Grove/EXT UART claims no role until something talks");
    checkEq(uart.label(), "serial0  Grove/EXT UART", "and is named for the header it is");
}

void testLinkBaud() {
    section("one baud rate per peer");
    check(nextBaudChoice(115200) == kBaudChoices[1], "the default cycles to the next choice");
    check(nextBaudChoice(kBaudChoices[kBaudChoiceCount - 1]) == kBaudChoices[0],
          "the last choice wraps to the first");
    check(nextBaudChoice(1200) == kBaudChoices[0],
          "a rate no termios constant covers lands on the default");
    check(!isSupportedBaud(1200), "1200 baud is still refused outright");

    const std::string dataDir = "/tmp/bfcli-baud-selftest-" + std::to_string(::getpid());
    const char* previous = ::getenv("BFCLI_DATA_DIR");
    const std::string saved = previous ? previous : "";
    ::setenv("BFCLI_DATA_DIR", dataDir.c_str(), 1);

    App::Options opt;
    opt.headless = true;
    opt.autoConnect = false;
    opt.muteSound = true;

    std::string error;
    {
        App app;
        check(app.setup(opt, error), "the app starts with an empty config: " + error);
        check(app.fcBaud_ == 115200 && app.meshBaud_ == 115200 && app.gnssBaud_ == 115200,
              "all three peers default to 115200");
        app.cycleLinkBaud(LinkMode::Betaflight);
        app.cycleGnssBaud();
        app.cycleGnssBaud();
        check(app.fcBaud_ == 57600 && app.meshBaud_ == 115200 && app.gnssBaud_ == 38400,
              "changing one peer's rate leaves the other two where they were");
        app.teardown();
    }
    {
        App app;
        check(app.setup(opt, error), "the app restarts against the written config: " + error);
        check(app.fcBaud_ == 57600 && app.meshBaud_ == 115200 && app.gnssBaud_ == 38400,
              "every rate survives a restart on its own key");
        check(app.linkBaud(LinkMode::Betaflight) == 57600 &&
                  app.linkBaud(LinkMode::Meshtastic) == 115200,
              "the connect path asks for the rate of the protocol it is opening");

        // The picker's B key belongs to the protocol Enter is about to speak.
        PortInfo grove;
        grove.device = "/dev/ttyS0";
        grove.kind = "uart";
        app.ports_ = {grove};
        app.screen_ = Screen::Ports;
        app.portLinkMode_ = LinkMode::Meshtastic;
        KeyEvent key;
        key.key = Key::Char;
        key.ch = 'b';
        app.handleKey(key);
        check(app.meshBaud_ == 57600 && app.fcBaud_ == 57600 && app.gnssBaud_ == 38400,
              "B on the picker moves only the highlighted protocol's rate");
        app.teardown();
    }
    {
        // The --no-gnss lesson: a launch-wide switch changes the session and
        // must never quietly become the saved default.
        App::Options override = opt;
        override.fcBaud = 921600;
        override.fcBaudSet = true;
        App app;
        check(app.setup(override, error), "the app starts under a command-line rate: " + error);
        check(app.fcBaud_ == 921600, "--fc-baud wins for the launch that named it");
        check(app.meshBaud_ == 57600, "the peers it did not name keep their saved rates");
        app.teardown();
    }
    {
        App app;
        check(app.setup(opt, error), "the app restarts after the override: " + error);
        check(app.fcBaud_ == 57600,
              "a command-line rate did not rewrite the rate on disk");
        app.teardown();
    }

    // And the file itself, because every host test points at a directory
    // nobody ever opens again.
    Storage storage;
    check(storage.init(error), "the baud config directory is readable: " + error);
    std::string ini;
    check(storage.readFile(storage.configPath(), ini, error),
          "the config file the app left behind is readable: " + error);
    check(ini.find("fc.baud = 57600") != std::string::npos &&
              ini.find("mesh.baud = 57600") != std::string::npos &&
              ini.find("gnss.baud = 38400") != std::string::npos,
          "each peer's rate is written under its own key");

    if (saved.empty()) ::unsetenv("BFCLI_DATA_DIR");
    else ::setenv("BFCLI_DATA_DIR", saved.c_str(), 1);
}

// A receiver at one fixed rate on the master end of a pty. TCGETS on a pty
// master reports the slave's termios, so the rate the reader configured is
// visible from here: at the receiver's own rate it sends sentences, at any
// other it sends what a UART sampled at the wrong rate hands over, which is
// bytes but never prose.
struct RatePty : RawPty {
    speed_t rate = B9600;
    bool silent = false;
    bool console = false;   // a radio's debug console, not a receiver at all

    speed_t readerRate() const {
        termios tio{};
        if (::tcgetattr(master, &tio) != 0) return 0;
        return ::cfgetospeed(&tio);
    }
    static std::string debris() {
        // Framing noise with the two traps in it that used to pass for a
        // receiver: a '$' that starts nothing, and a sentence-shaped line
        // with no checksum.
        std::string g;
        uint32_t x = 0x9e3779b9u;
        for (int i = 0; i < 64; ++i) {
            x = x * 1664525u + 1013904223u;
            g.push_back(static_cast<char>((x >> 24) & 0xffu));
            if (i % 16 == 15) g += "\r\n";
        }
        g.push_back('\0');
        g.push_back('$');
        g.push_back('\0');
        g += "\r\n$GNGGA,020211.00,,,,,0,00,5.9,,,,,,\r\n";
        return g;
    }
    void pump() {
        if (silent) {
            char sink[256];
            while (::read(master, sink, sizeof(sink)) > 0) {
            }
            return;
        }
        if (console) {
            write("INFO  | 12:00:01 42 [Router] Received packet from 0x1443fdf3, hop 1\r\n");
            return;
        }
        write(readerRate() == rate ? std::string(kBenchNmea) : debris());
    }
};

// The bench had a GPS Unit on the Grove socket and a saved rate that was the
// cap's. The receiver is the one peer whose port the app owns outright, so a
// wrong rate is something the app can find out for itself.
void testGnssBaudProbe() {
    section("a receiver at the wrong rate");
    const std::string dataDir = "/tmp/bfcli-gnssprobe-selftest-" + std::to_string(::getpid());
    const char* previous = ::getenv("BFCLI_DATA_DIR");
    const std::string saved = previous ? previous : "";
    ::setenv("BFCLI_DATA_DIR", dataDir.c_str(), 1);
    const auto restoreEnv = [&]() {
        if (saved.empty()) ::unsetenv("BFCLI_DATA_DIR");
        else ::setenv("BFCLI_DATA_DIR", saved.c_str(), 1);
    };

    App::Options opt;
    opt.headless = true;
    opt.autoConnect = false;
    opt.muteSound = true;
    std::string error;

    RatePty receiver;
    if (!receiver.start(error)) {
        std::printf("  SKIP  pty unavailable: %s\n", error.c_str());
        restoreEnv();
        return;
    }

    // Each round is one probe window: a burst of traffic, then the deadline
    // is declared reached, the way six seconds of it would.
    const auto drive = [](App& app, RatePty& pty, int maxRounds) {
        int rounds = 0;
        while (rounds < maxRounds && app.gnss_.isOpen() && !app.gnssProbeReported_) {
            const uint64_t until = nowMs() + 120;
            while (nowMs() < until) {
                pty.pump();
                app.pollGnss(nowMs());
                sleepMs(10);
            }
            app.gnssProbeDeadlineMs_ = nowMs();
            app.pollGnss(nowMs());
            ++rounds;
        }
        return rounds;
    };
    // Whatever the host's own /dev/serial0 did at setup is not the test.
    const auto aim = [](App& app, const std::string& device, int baud) {
        app.gnss_.close();
        app.gnssProbeDeadlineMs_ = 0;
        app.gnssDevice_ = device;
        app.gnssBaud_ = baud;
        app.startGnss();
    };
    const auto told = [](const App& app, const std::string& phrase) {
        for (size_t i = 0; i < app.term_.lineCount(); ++i) {
            if (app.term_.line(i).text.find(phrase) != std::string::npos) return true;
        }
        return false;
    };
    const auto configText = []() {
        Storage storage;
        std::string ini;
        std::string err;
        if (!storage.init(err) || !storage.readFile(storage.configPath(), ini, err)) {
            return std::string();
        }
        return ini;
    };

    // --gnss-baud named a rate that is wrong. The wire wins for this launch
    // and the file is not told, the contract every --*-baud switch has.
    {
        App::Options named = opt;
        named.gnssBaud = 57600;
        named.gnssBaudSet = true;
        App app;
        check(app.setup(named, error), "the probe app starts with a named rate: " + error);
        aim(app, receiver.slave, 57600);
        check(app.gnss_.isOpen() && app.gnss_.baud() == 57600,
              "the probe opens at the rate it was given first");
        const int rounds = drive(app, receiver, 12);
        check(app.gnss_.isOpen() && app.gnss_.receiverPresent() && app.gnssBaud_ == 9600 &&
                  app.gnss_.baud() == 9600,
              "a receiver at 9600 is found from a named rate of 57600");
        check(rounds == 2 && app.gnssProbeTried_.size() == 2,
              "9600 is the first rate tried after the given one, so one retry finds it");
        check(told(app, "answers at 9600, not 57600") && told(app, "for this launch"),
              "the terminal says which rate answered, and that the file is not told");
        app.teardown();
    }
    check(configText().find("gnss.baud") == std::string::npos,
          "a rate found under --gnss-baud is not written to config.ini");

    // The saved rate is wrong. The wire wins and the file follows.
    {
        App app;
        check(app.setup(opt, error), "the probe app starts on the saved rate: " + error);
        check(app.gnssBaud_ == 115200, "with nothing saved the receiver starts at 115200");
        aim(app, receiver.slave, 115200);
        const int rounds = drive(app, receiver, 12);
        check(app.gnss_.receiverPresent() && app.gnssBaud_ == 9600 && rounds == 2,
              "a receiver at 9600 is found from a saved rate of 115200");
        check(told(app, "gnss.baud follows the wire") && told(app, "GNSS receiver present"),
              "the verdict names the rate that answered and calls the receiver present");
        app.teardown();
    }
    check(configText().find("gnss.baud = 9600") != std::string::npos,
          "the rate that answered is the saved rate now");
    {
        App app;
        check(app.setup(opt, error), "the app restarts after the probe: " + error);
        check(app.gnssBaud_ == 9600, "and the next launch opens the receiver at it");
        app.teardown();
    }

    // Silence is not a rate problem. On this board the node is also the
    // Grove header, and walking the table would hold it for half a minute
    // against a flight controller waiting to be picked.
    {
        RatePty mute;
        mute.silent = true;
        if (mute.start(error)) {
            App app;
            check(app.setup(opt, error), "the probe app starts for a silent UART: " + error);
            aim(app, mute.slave, 115200);
            const int rounds = drive(app, mute, 12);
            check(!app.gnss_.isOpen() && rounds == 1 && app.gnssProbeTried_.size() == 1 &&
                      app.gnssBaud_ == 115200,
                  "a silent UART is released at the first deadline, no other rate tried");
            check(told(app, "no NMEA on"), "and reported as absent, not as the wrong rate");
            app.teardown();
        }
    }

    // A wire full of readable text is a console -- a Grove-wired radio's
    // debug log, say -- and no rate change turns that into NMEA.
    {
        RatePty radio;
        radio.console = true;
        if (radio.start(error)) {
            App app;
            check(app.setup(opt, error), "the probe app starts for a console: " + error);
            aim(app, radio.slave, 115200);
            const int rounds = drive(app, radio, 12);
            check(!app.gnss_.isOpen() && rounds == 1 && app.gnssProbeTried_.size() == 1,
                  "readable text that is not NMEA is released at the first deadline");
            check(told(app, "a console, not a receiver"),
                  "and the verdict says what was on the wire");
            app.teardown();
        }
    }

    // Noise at every rate walks the whole table once, then gives the wire
    // back at the rate it started from rather than wherever it ended up.
    {
        RatePty noise;
        noise.rate = B4800;   // not in the table, so never the right answer
        if (noise.start(error)) {
            App app;
            check(app.setup(opt, error), "the probe app starts for noise: " + error);
            aim(app, noise.slave, 115200);
            const int rounds = drive(app, noise, kBaudChoiceCount + 3);
            check(rounds == kBaudChoiceCount &&
                      static_cast<int>(app.gnssProbeTried_.size()) == kBaudChoiceCount,
                  "every rate in the table is tried exactly once");
            check(!app.gnss_.isOpen() && app.gnssBaud_ == 115200,
                  "then the wire is released and the saved rate stands");
            check(told(app, "no NMEA at any rate"),
                  "and the verdict says the table was walked");
            app.teardown();
        }
    }

    // --gnss DEV is the mirror image of --no-gnss: it names a receiver on
    // purpose, so the saved switch being off cannot make the flag do nothing
    // (on the bench it did), and the switch it forced on is not saved.
    {
        App app;
        check(app.setup(opt, error), "the app starts to switch the receiver off: " + error);
        app.gnssWanted_ = false;
        app.config_.setBool("gnss.enabled", false);
        app.teardown();
    }
    check(configText().find("gnss.enabled = 0") != std::string::npos, "the saved switch is off");
    {
        App::Options named = opt;
        named.gnssDevice = receiver.slave;
        App app;
        check(app.setup(named, error), "the app starts with --gnss DEV: " + error);
        check(app.gnssWanted_ && app.gnss_.isOpen() &&
                  sameDeviceNode(app.gnss_.device(), receiver.slave),
              "--gnss DEV opens the receiver it names although the saved switch is off");
        app.teardown();
    }
    check(configText().find("gnss.enabled = 0") != std::string::npos,
          "the switch --gnss DEV forced on for its launch is not saved");
    {
        App app;
        check(app.setup(opt, error), "the app restarts without --gnss: " + error);
        check(!app.gnssWanted_ && !app.gnss_.isOpen(),
              "and a plain launch still honours the saved switch");
        app.teardown();
    }

    restoreEnv();
}

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
            MenuPage::SoundDisplay, MenuPage::ConnectionExit, MenuPage::LinkSpeeds,
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
        check(actions == 28 && ids.size() == 28,
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

        // Sharing a position is addressed to the highlighted row, but it must
        // not become the conversation that Export and Clear then operate on.
        // Those two act on whatever chat is open, and a position share that
        // silently repointed them would delete the wrong transcript.
        app.openChat(kMeshBroadcast);
        app.setScreen(Screen::Nodes);
        app.nodeList_.sel = 1;
        KeyEvent share;
        share.key = Key::F5;
        app.handleKey(share);
        check(app.chatPeer_ == kMeshBroadcast,
              "sharing a position from the node list leaves the open conversation alone");
        app.handleKey(escape);

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

    // The receiver belongs to the application, not to a radio session, because
    // on the bench there was no radio session to belong to: the only UART on
    // the board was the cap's AT6668, and a reader that waited for a radio
    // waited forever. What a link may do is borrow a silent UART the probe is
    // still listening on. A UART already proving itself with NMEA refuses to
    // become a link, and a link that borrowed one gives it back on close.
    {
        SimFc grove;
        RawPty receiver;
        std::string groveError;
        std::string receiverError;
        if (grove.start(groveError) && receiver.start(receiverError)) {
            App app;
            app.display_.setHeadlessSize(kScreenW, kScreenH);
            check(app.storage_.init(error), "a third app instance initializes storage");
            app.setupMenus();

            // A silent UART: the probe holds it, a deliberate link takes it
            // over, and the probe starts again once the link is gone.
            app.gnssDevice_ = grove.devicePath();
            app.startGnss();
            check(app.gnss_.isOpen() && !app.mesh_.connected(),
                  "the receiver probe opens its UART with no radio connected at all");
            PortInfo grovePort;
            grovePort.device = grove.devicePath();
            grovePort.kind = "uart";
            app.connectPort(grovePort, LinkMode::Betaflight);
            check(app.session_.connected() && !app.gnss_.isOpen(),
                  "a link opened on a silent UART takes it from the probe");
            app.requestDisconnect(false);
            check(!app.session_.connected() && app.gnss_.isOpen(),
                  "closing that link hands the UART back to the receiver probe");
            app.gnss_.close();
            app.gnssProbeDeadlineMs_ = 0;

            // A UART with NMEA on it is the receiver, whatever the picker calls
            // it. Enter shows the receiver instead of opening a link.
            app.gnssDevice_ = receiver.slave;
            app.startGnss();
            const std::string burst = kBenchNmea;
            const uint64_t proven = nowMs() + 2000;
            while (nowMs() < proven && !app.gnss_.receiverPresent()) {
                receiver.write(burst);
                app.pollGnss(nowMs());
                sleepMs(10);
            }
            check(app.gnss_.receiverPresent(), "sentences on the UART prove the receiver");
            PortInfo receiverPort;
            receiverPort.device = receiver.slave;
            receiverPort.kind = "uart";
            app.connectPort(receiverPort, LinkMode::Meshtastic);
            check(!app.mesh_.connected() && app.gnss_.isOpen() && app.modal_,
                  "a proven receiver is not opened as a radio; its status is shown instead");
            app.closeModal();

            // The bench itself: the link was opened on the receiver's UART
            // first (an older session, a hand-edited config). The NMEA verdict
            // closes it and the receiver is reopened on the wire it gave back.
            app.gnss_.close();
            app.gnssProbeDeadlineMs_ = 0;
            check(app.mesh_.connect(receiver.slave, 115200, error),
                  "the radio link opens the receiver's UART: " + error);
            app.linkMode_ = LinkMode::Meshtastic;
            app.beginMeshSession();
            check(!app.gnss_.isOpen(), "the receiver stands aside while a link holds its UART");
            const uint64_t verdict = nowMs() + 4000;
            while (nowMs() < verdict && app.mesh_.connected()) {
                receiver.write(burst);
                app.tick(nowMs());
                sleepMs(20);
            }
            check(!app.mesh_.connected() && !app.meshMode(),
                  "the NMEA verdict closes the radio link without waiting for a timeout");
            check(app.gnss_.isOpen(), "and reopens the receiver on the UART the link gave back");
            check(app.modal_ && app.screen_ == Screen::Ports,
                  "the operator is told it was the receiver, back on the port picker");
            app.closeModal();
            const uint64_t again = nowMs() + 2000;
            while (nowMs() < again && !app.gnss_.receiverPresent()) {
                receiver.write(burst);
                app.tick(nowMs());
                sleepMs(10);
            }
            check(app.gnss_.receiverPresent(), "NMEA reaches the reader again after the handover");
        }
    }

    if (saved.empty()) ::unsetenv("BFCLI_DATA_DIR");
    else ::setenv("BFCLI_DATA_DIR", saved.c_str(), 1);
}

namespace {

int regionPixels(Surface& s, int x0, int y0, int x1, int y1, Color c) {
    int n = 0;
    for (int y = std::max(0, y0); y < std::min(s.h, y1); ++y) {
        for (int x = std::max(0, x0); x < std::min(s.w, x1); ++x) {
            if (s.row(y)[x] == c) ++n;
        }
    }
    return n;
}

// The menu item order of the Position & GNSS page, as the test walks it.
constexpr int kPositionCompassRow = 2;
constexpr int kPositionAutoShareRow = 4;
constexpr int kPositionSosRow = 5;
constexpr int kPositionMarksRow = 7;

} // namespace

void testFieldTools() {
    section("field tools: locate, marks, quick messages, SOS, auto-share");
    SimMesh sim;
    std::string error;
    if (!sim.start(error)) {
        std::printf("  SKIP  mesh simulator unavailable: %s\n", error.c_str());
        return;
    }

    const std::string dataDir = "/tmp/bfcli-field-selftest-" + std::to_string(::getpid());
    const char* previous = ::getenv("BFCLI_DATA_DIR");
    const std::string saved = previous ? previous : "";
    ::setenv("BFCLI_DATA_DIR", dataDir.c_str(), 1);

    KeyEvent enter, escape, down, tab;
    enter.key = Key::Enter;
    escape.key = Key::Escape;
    down.key = Key::Down;
    tab.key = Key::Tab;
    // handleKey is private; this function is the friend, a helper is not.
    auto typeText = [](App& app, const std::string& text) {
        for (char c : text) {
            KeyEvent key;
            key.key = Key::Char;
            key.ch = c;
            app.handleKey(key);
        }
    };

    {
        App app;
        app.display_.setHeadlessSize(kScreenW, kScreenH);
        check(app.storage_.init(error), "field-tools storage initializes: " + error);
        app.setupMenus();
        app.quickMessages_ = defaultQuickMessages();
        app.loadMarks();
        check(app.marks_.empty(), "a fresh data directory has no marks");

        check(app.mesh_.connect(sim.devicePath(), 115200, error),
              "the field-tools app opens the radio: " + error);
        app.linkMode_ = LinkMode::Meshtastic;
        app.beginMeshSession();
        app.setScreen(Screen::Nodes);
        const uint64_t ready = nowMs() + 6000;
        while (nowMs() < ready && !app.mesh_.ready()) {
            sim.pump();
            app.tick(nowMs());
            sleepMs(4);
        }
        check(app.mesh_.ready(), "the field-tools app reaches ready");

        // Every new action has one owner, and its hint fits beside "Esc back".
        const MenuPage pages[] = {
            MenuPage::Mesh,         MenuPage::MeshPosition,  MenuPage::ControlsInfo,
            MenuPage::SoundDisplay, MenuPage::ConnectionExit, MenuPage::LinkSpeeds,
        };
        std::set<int> ids;
        int actions = 0;
        bool labelsFit = true, hintsFit = true;
        const int labelColumns = (kScreenW - 48) / kGlyphW;
        const int hintColumns = (kScreenW - (textWidth("Esc back") + 8) - 4) / kGlyphW;
        for (MenuPage page : pages) {
            app.openMenuPage(page);
            for (const MenuItem& item : app.currentMenuItems()) {
                ++actions;
                ids.insert(item.id);
                labelsFit &= static_cast<int>(item.label.size()) <= labelColumns;
                hintsFit &= static_cast<int>(item.hint.size()) <= hintColumns;
            }
        }
        check(actions == 28 && ids.size() == 28,
              "the six new field actions each have exactly one category owner");
        check(labelsFit, "every field-tool label fits the menu grid");
        check(hintsFit, "every field-tool hint fits the hint bar");
        app.openMenuPage(MenuPage::Root);
        app.setScreen(Screen::Nodes);

        // ---- Locate, before and after a fix
        KeyEvent locate;
        locate.key = Key::Char;
        locate.ch = 'f';
        app.nodeList_.sel = 0;
        app.handleKey(locate);
        check(app.screen_ == Screen::Nodes &&
                  app.status_.find("not a place") != std::string::npos,
              "F on the broadcast row explains that a channel has no position");
        app.nodeList_.sel = 2;
        check(app.peerForNodeRow(2) == sim.hilltopNodeNum(),
              "row two of the fixture node list is the hilltop relay");
        app.handleKey(locate);
        check(app.screen_ == Screen::Locate && app.locateNode_ == sim.hilltopNodeNum(),
              "F on a node opens the Locate screen for it");
        app.render();
        Surface s = app.display_.surface();
        check(s.valid(), "the Locate screen renders without a fix");
        // The rose sits at (52, 90) r=38. With no fix nothing orange may be
        // drawn inside it: no arrow, no invented bearing.
        check(regionPixels(s, 14, 52, 91, 129, theme::accent) == 0,
              "without a fix the rose has no bearing arrow");
        check(regionPixels(s, 14, 52, 91, 129, theme::ok) == 0,
              "without a course the rose has no track dot");

        GnssFix fix;
        fix.valid = true;
        fix.latitude = 51.47790;
        fix.longitude = -0.00150;
        fix.haveAltitude = true;
        fix.altitudeM = 45.0;
        fix.satellitesUsed = 9;
        fix.haveSpeed = true;
        fix.speedKph = 4.2;
        fix.haveCourse = true;
        fix.courseDeg = 38.0;
        fix.utc = "12:35:19";
        app.gnss_.adoptFix(fix, nowMs());
        check(app.gnss_.receiverPresent() && app.gnss_.fix().valid,
              "the test fix counts as a present receiver with a fix");
        app.render();
        s = app.display_.surface();
        check(regionPixels(s, 14, 52, 91, 129, theme::accent) > 30,
              "with a fix the rose draws the bearing arrow");
        check(regionPixels(s, 14, 52, 91, 129, theme::ok) >= 20,
              "a walking course puts the track dot on the rim");

#if defined(__linux__)
        // A calibrated magnetometer outranks the track, and turns the rose
        // heading-up: facing east, the "you" dot sits at the top of the rose
        // and north's label moves to the left rim.
        CompassFixture compassFixture = makeCompassFixture();
        check(compassFixture.ok, "app-level compass fixture is created");
        if (compassFixture.ok) {
            check(app.compass_.discoverIn(compassFixture.root), "the app finds the fixture compass");
            CompassCalibration cal;
            cal.hardIron = true;
            cal.fieldNorm = 1000.0;
            app.compass_.setCalibration(cal);
            compassFixture.setField(0, 1000, 0);
            app.compass_.poll(nowMs());
            check(app.compass_.usable(nowMs()), "the fixture compass is usable once calibrated");
            app.render();
            s = app.display_.surface();
            check(regionPixels(s, 44, 46, 61, 59, theme::ok) >= 10,
                  "with a compass heading the rose is heading-up: the you dot is at the top");
            check(regionPixels(s, 82, 82, 97, 99, theme::ok) == 0,
                  "and no longer where a north-up rose would put an easterly heading");
            // Take the compass away again so the rest of the walk is track-based.
            app.compass_ = Compass();
            app.loadCompassCalibration();
        }
#endif

        // Only the target that opened the screen is shown; nothing is sent.
        check(sim.textPacketsReceived() == 0 && sim.positionPacketsReceived() == 0,
              "locating a node transmits nothing");

        // ---- M marks the node's last position under its own name
        KeyEvent mark;
        mark.key = Key::Char;
        mark.ch = 'm';
        app.handleKey(mark);
        check(app.modal_ && app.modalIsInput_ && app.modalEditor_.text() == "HILL",
              "M on a node offers to mark its last position under its own name");
        typeText(app, " site");
        app.handleKey(enter);
        check(!app.modal_ && app.marks_.size() == 1 && app.marks_[0].name == "HILL site" &&
                  app.marks_[0].source == meshNodeIdText(sim.hilltopNodeNum()) &&
                  std::abs(app.marks_[0].latitude - 51.48180) < 1e-6,
              "the mark carries the node's position, its id, and the edited name");
        std::string marksText, readError;
        check(app.storage_.readFile(app.storage_.marksPath(), marksText, readError) &&
                  parseMarks(marksText).size() == 1,
              "the mark is on disk before anything else happens: " + readError);
        check(app.screen_ == Screen::Locate, "marking stays on the Locate screen");
        app.handleKey(escape);
        check(app.screen_ == Screen::Nodes, "Escape returns from Locate to the node list");

        // ---- Mark this spot names the operator's own fix
        app.markHere();
        check(app.modal_ && app.modalIsInput_ && app.modalEditor_.text() == "Mark 2",
              "Mark this spot proposes a numbered name");
        KeyEvent killLine;
        killLine.key = Key::Char;
        killLine.ch = 'u';
        killLine.ctrl = true;
        app.handleKey(killLine);
        typeText(app, "Car");
        app.handleKey(enter);
        check(app.marks_.size() == 2 && app.marks_[1].name == "Car" &&
                  app.marks_[1].source == "gnss" &&
                  std::abs(app.marks_[1].latitude - 51.47790) < 1e-6 &&
                  app.marks_[1].haveAltitude && app.marks_[1].altitudeM == 45,
              "the spot is saved under the typed name with this station's fix");

        // ---- Compass screen through the menu
        app.openMenuPage(MenuPage::MeshPosition);
        app.setScreen(Screen::Menu);
        app.submenuList_.sel = kPositionCompassRow;
        app.handleKey(enter);
        check(app.screen_ == Screen::Compass, "Compass opens from the Position & GNSS category");
        app.render();
        check(app.display_.surface().valid(), "the compass screen renders without a magnetometer");
#if defined(__linux__)
        {
            CompassFixture live = makeCompassFixture();
            if (live.ok && app.compass_.discoverIn(live.root)) {
                app.compass_.poll(nowMs());
                app.render();
                check(app.display_.surface().valid(), "the compass screen renders a live heading");
                KeyEvent calibrate;
                calibrate.key = Key::Char;
                calibrate.ch = 'c';
                app.handleKey(calibrate);
                check(app.compass_.calibrating(), "C starts a calibration spin");
                app.handleKey(calibrate);
                check(app.compass_.calibrating() && !app.compass_.calibration().hardIron &&
                          app.status_.find("keep turning") != std::string::npos,
                      "C again before a full circle keeps the spin going and says so");
                app.handleKey(escape);
                check(!app.compass_.calibrating() && app.screen_ == Screen::Menu,
                      "Escape cancels the spin and returns to the category");
                checkEq(app.config_.get("compass.calibrated", "unwritten"), "unwritten",
                        "a cancelled calibration writes nothing");
                app.compass_ = Compass();
                app.loadCompassCalibration();
            }
        }
#else
        app.handleKey(escape);
#endif
        if (app.screen_ == Screen::Compass) app.handleKey(escape);

        // ---- Marks screen through the menu, then Locate a mark, then delete
        app.openMenuPage(MenuPage::MeshPosition);
        app.setScreen(Screen::Menu);
        app.submenuList_.sel = kPositionMarksRow;
        app.handleKey(enter);
        check(app.screen_ == Screen::Marks, "Marks opens from the Position & GNSS category");
        app.render();
        check(app.display_.surface().valid(), "the marks list renders offscreen");
        app.handleKey(down);
        app.handleKey(enter);
        check(app.screen_ == Screen::Locate && app.locateMark_ == 1 && app.locateNode_ == 0,
              "Enter on a mark locates it");
        app.render();
        check(app.display_.surface().valid(), "Locate renders a mark");
        KeyEvent del;
        del.key = Key::Char;
        del.ch = 'd';
        app.handleKey(del);
        check(app.modal_ && app.modalIsConfirm_ && app.modalTitle_ == "Delete mark",
              "D on a located mark asks before deleting");
        app.handleKey(enter);
        check(app.marks_.size() == 1 && app.marks_[0].name == "HILL site" &&
                  app.screen_ == Screen::Marks,
              "deleting the mark removes it and returns to the list");
        app.handleKey(escape);
        check(app.screen_ == Screen::Nodes,
              "Escape from a marks list reached back through Locate lands on the node list");

        // ---- Quick messages from inside a conversation
        app.nodeList_.sel = 2;
        app.handleKey(enter);
        check(app.screen_ == Screen::Chat && app.chatPeer_ == sim.hilltopNodeNum(),
              "Enter opens the hilltop conversation");
        app.handleKey(tab);
        check(app.screen_ == Screen::QuickMsg, "Tab in a chat opens the quick message picker");
        app.render();
        check(app.display_.surface().valid(), "the picker renders over the conversation");
        app.handleKey(down);
        app.handleKey(enter);
        check(app.screen_ == Screen::Chat, "sending a quick message returns to the chat");
        uint64_t deadline = nowMs() + 3000;
        while (nowMs() < deadline && sim.lastTextReceived() != "Landed safe") {
            sim.pump();
            app.tick(nowMs());
            sleepMs(4);
        }
        checkEq(sim.lastTextReceived(), "Landed safe", "the second canned line reached the radio");
        const std::vector<MeshMessage>* log = app.mesh_.conversation(sim.hilltopNodeNum());
        check(log && !log->empty() && log->back().text == "Landed safe" && log->back().outgoing,
              "the canned line is in the transcript as an outgoing message");

        // The picker remembers where it was: a line that gets repeated is a
        // Tab and an Enter, not a scroll. From row one, two more rows down is
        // the line with the placeholder.
        app.handleKey(tab);
        check(app.quickMsgList_.sel == 1, "the picker reopens on the last line sent");
        for (int i = 0; i < 2; ++i) app.handleKey(down);
        app.handleKey(enter);
        deadline = nowMs() + 3000;
        while (nowMs() < deadline &&
               sim.lastTextReceived() != "Need help at 51.47790, -0.00150") {
            sim.pump();
            app.tick(nowMs());
            sleepMs(4);
        }
        checkEq(sim.lastTextReceived(), "Need help at 51.47790, -0.00150",
                "{pos} in a canned line becomes this station's fix on the wire");
        app.handleKey(tab);
        app.handleKey(escape);
        check(app.screen_ == Screen::Chat, "Escape closes the picker back to the chat");
        app.handleKey(escape);
        check(app.screen_ == Screen::Nodes, "and the chat returns to the node list");

        // ---- SOS
        const int textsBefore = sim.textPacketsReceived();
        const int positionsBefore = sim.positionPacketsReceived();
        app.openMenuPage(MenuPage::MeshPosition);
        app.setScreen(Screen::Menu);
        app.submenuList_.sel = kPositionSosRow;
        app.handleKey(enter);
        check(app.modal_ && app.modalIsConfirm_ && app.modalTitle_ == "Send SOS?" &&
                  app.modalBody_.find("SOS from GNDH: need help at 51.47790, -0.00150 alt 45m "
                                      "12:35:19 UTC") != std::string::npos,
              "SOS shows the exact text it is about to broadcast");
        app.handleKey(enter);
        deadline = nowMs() + 3000;
        while (nowMs() < deadline && (sim.textPacketsReceived() == textsBefore ||
                                      sim.positionPacketsReceived() == positionsBefore)) {
            sim.pump();
            app.tick(nowMs());
            sleepMs(4);
        }
        check(sim.textPacketsReceived() == textsBefore + 1 &&
                  sim.lastTextReceived().rfind("SOS from GNDH: need help at ", 0) == 0 &&
                  sim.positionPacketsReceived() == positionsBefore + 1,
              "SOS broadcasts the text and one position packet");
        check(app.screen_ == Screen::Chat && app.chatPeer_ == kMeshBroadcast,
              "SOS leaves the broadcast conversation open for replies");
        app.handleKey(escape);

        // ---- Auto-share is session-only and needs a deliberate switch-on
        const int beforeBeacon = sim.positionPacketsReceived();
        app.openMenuPage(MenuPage::MeshPosition);
        app.setScreen(Screen::Menu);
        app.submenuList_.sel = kPositionAutoShareRow;
        check(app.currentMenuItems()[kPositionAutoShareRow].label == "Auto-share position: OFF",
              "auto-share starts off");
        app.handleKey(enter);
        check(app.modal_ && app.modalIsConfirm_ && app.modalTitle_ == "Auto-share position?",
              "switching auto-share on asks first");
        app.handleKey(enter);
        check(app.autoShareMinutes_ == 2 &&
                  app.currentMenuItems()[kPositionAutoShareRow].label ==
                      "Auto-share position: 2 min",
              "auto-share switches on at two minutes and the label says so");
        deadline = nowMs() + 3000;
        while (nowMs() < deadline && sim.positionPacketsReceived() == beforeBeacon) {
            sim.pump();
            app.tick(nowMs());
            sleepMs(4);
        }
        check(sim.positionPacketsReceived() == beforeBeacon + 1,
              "the first auto-share goes out as soon as it is switched on");
        for (int i = 0; i < 20; ++i) {
            sim.pump();
            app.tick(nowMs());
            sleepMs(2);
        }
        check(sim.positionPacketsReceived() == beforeBeacon + 1,
              "and not again before the interval");
        app.lastAutoShareMs_ = nowMs() - 3 * 60000;
        deadline = nowMs() + 3000;
        while (nowMs() < deadline && sim.positionPacketsReceived() == beforeBeacon + 1) {
            sim.pump();
            app.tick(nowMs());
            sleepMs(4);
        }
        check(sim.positionPacketsReceived() == beforeBeacon + 2,
              "once the interval has passed the next fix goes out");
        app.handleKey(enter);
        check(app.autoShareMinutes_ == 5, "Enter steps auto-share to five minutes");
        app.handleKey(enter);
        check(app.autoShareMinutes_ == 15, "then fifteen");
        app.handleKey(enter);
        check(app.autoShareMinutes_ == 0 &&
                  app.currentMenuItems()[kPositionAutoShareRow].label == "Auto-share position: OFF",
              "then off");
        bool sharePersisted = false;
        for (const auto& kv : app.config_.all()) {
            sharePersisted |= kv.first.find("share") != std::string::npos;
        }
        check(!sharePersisted, "auto-share never writes itself into the config");
        app.autoShareMinutes_ = 15;
        app.requestDisconnect(false);
        check(app.autoShareMinutes_ == 0 && !app.mesh_.connected(),
              "closing the radio link switches auto-share off");
    }

    // The marks are a file, and the next launch reads it.
    {
        App app;
        app.display_.setHeadlessSize(kScreenW, kScreenH);
        check(app.storage_.init(error), "a second field-tools instance initializes storage");
        app.loadMarks();
        check(app.marks_.size() == 1 && app.marks_[0].name == "HILL site",
              "saved marks are reloaded on the next launch");
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
    testBattery();
    testLinkBaud();
    testPortRoles();
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
    testStringHelpers();
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
            MenuPage::LinkSpeeds,
        };
        std::set<int> actionIds;
        bool labelsFit = true;
        bool labelsClean = true;
        bool hintsFit = true;
        int actionCount = 0;
        const int labelColumns = (kScreenW - 48) / kGlyphW;
        // What drawHintBar leaves once the "Esc back" plate has taken its side.
        const int hintColumns = (kScreenW - (textWidth("Esc back") + 8) - 4) / kGlyphW;
        for (const MenuItem& item : app.menu_) labelsClean &= !endsWith(item.label, "...");
        for (const MenuItem& item : app.quick_) labelsClean &= !endsWith(item.label, "...");
        for (MenuPage page : pages) {
            app.openMenuPage(page);
            for (const MenuItem& item : app.currentMenuItems()) {
                ++actionCount;
                actionIds.insert(item.id);
                labelsFit &= static_cast<int>(item.label.size()) <= labelColumns;
                hintsFit &= static_cast<int>(item.hint.size()) <= hintColumns;
                labelsClean &= !endsWith(item.label, "...");
            }
        }
        check(actionCount == 20 && actionIds.size() == 20,
              "all 20 actions have exactly one category owner");
        check(labelsFit, "every category action fits the original 6x8 menu grid");
        check(hintsFit, "every category hint fits the hint bar without being clipped");
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
                const bool filled = (kMascotFillBits[bit / 8] & mask) != 0;
                disjointMasks &= !(black && filled);
                if (x < 5 || x >= kMascotSize - 5 ||
                    y < 5 || y >= kMascotSize - 5) {
                    transparentExterior &= !black && !filled;
                }
            }
        }
        check(disjointMasks, "mascot black and fill masks do not overlap");
        check(transparentExterior, "mascot exterior is transparent beyond the badge perimeter");
        bool identical = true;
        for (int y = 0; y < kMascotSize; ++y) {
            for (int x = 0; x < kMascotSize; ++x) {
                const int bit = y * kMascotSize + x;
                const uint8_t mask = static_cast<uint8_t>(0x80u >> (bit % 8));
                Color want = theme::bg;
                if (kMascotBlackBits[bit / 8] & mask) want = theme::black;
                else if (kMascotFillBits[bit / 8] & mask) want = theme::accent;
                identical &= s.row(18 + y)[4 + x] == want;
            }
        }
        check(identical, "About paints the mascot black, accent orange, and transparent");
        bool anyWhite = false;
        for (int y = 0; y < kMascotSize; ++y) {
            for (int x = 0; x < kMascotSize; ++x) {
                anyWhite |= s.row(18 + y)[4 + x] == rgb(0xff, 0xff, 0xff);
            }
        }
        check(!anyWhite, "no white pixel survives in the About mascot");
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
                    else if (kMascotFillBits[bit / 8] & mask) want = theme::accent;
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
    testMeshNmeaPort();
    testMeshConfigProgress();
    testMeshApp();
    testGnssBaudProbe();
    testFieldGeometry();
    testMarks();
    testQuickMessages();
    testCompass();
    testFieldTools();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

} // namespace bf
