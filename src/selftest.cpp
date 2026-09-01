#include "app.h"
#include "brand.h"
#include "mascot.h"
#include "bfcommands.h"
#include "bfsession.h"
#include "font6x8.h"
#include "gfx.h"
#include "input.h"
#include "keys.h"
#include "simfc.h"
#include "storage.h"
#include "term.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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
    check(spin(sim, s, 4000, [&] { return s.ready(); }),
          "reaches the CLI prompt after sending '#'");

    // A plain command must complete and return to the prompt.
    check(s.send("status"), "send status");
    check(spin(sim, s, 4000, [&] { return s.ready(); }), "status completes");
    bool sawVoltage = false;
    for (size_t i = 0; i < term.lineCount(); ++i) {
        if (term.line(i).text.find("Voltage:") != std::string::npos) sawVoltage = true;
    }
    check(sawVoltage, "status output reached the terminal");

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

    s.disconnect();
    check(!s.connected(), "disconnect closes the port");

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
    check(spin(dropped, s, 4000, [&] { return s.ready(); }),
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
    check(spin(sim, s, 4000, [&] { return s.ready(); }), "reaches the CLI prompt");

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

} // namespace

int runSelfTest() {
    std::printf("%s self-test (commit %s)\n\n", kAppName, kBuildCommit);
    testKeys();
    testTerminal();
    testEditor();
    testCompleter();
    testRiskAndParsing();
    testStorage();
    testGraphics();
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
        KeyEvent key;
        key.key = Key::Char;
        key.ch = 'a';
        app.handleKey(key);
        check(app.screen_ == Screen::About, "A opens About without a flight controller");
        check(!app.session_.connected(), "About does not connect a flight controller");
        app.render();
        Surface s = app.display_.surface();
        bool frameless = true;
        for (int y = 0; y < kMascotSize; ++y) {
            for (int x = 0; x < kMascotSize; ++x) {
                const bool outerEdge = x < 5 || x >= kMascotSize - 5 ||
                                       y < 5 || y >= kMascotSize - 5;
                const bool corner = (x < 14 || x >= kMascotSize - 14) &&
                                    (y < 14 || y >= kMascotSize - 14);
                if (!outerEdge && !corner) continue;
                const int bit = y * kMascotSize + x;
                frameless &= (kMascotBits[bit / 8] & (0x80u >> (bit % 8))) != 0;
            }
        }
        check(frameless, "mascot has no surrounding badge frame");
        bool identical = true;
        for (int y = 0; y < kMascotSize; ++y) {
            for (int x = 0; x < kMascotSize; ++x) {
                const int bit = y * kMascotSize + x;
                const Color want = kMascotBits[bit / 8] & (0x80u >> (bit % 8))
                                       ? theme::bg : theme::accent;
                identical &= s.row(18 + y)[4 + x] == want;
            }
        }
        check(identical, "About renders orange ink and transparent white without overlap");
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
        app.returnScreen_ = Screen::Menu;
        app.setScreen(Screen::About);
        key.key = Key::Enter;
        app.handleKey(key);
        check(app.screen_ == Screen::Menu, "About returns to its menu entry screen");
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
                    if (!(kMascotBits[bit / 8] & (0x80u >> (bit % 8)))) want = theme::accent;
                }
                transparentAndClipped &= small.row(y)[x] == want;
            }
        }
        check(transparentAndClipped, "mascot clips and preserves an arbitrary background");
    }
    testSession();
    testFullScrollback();
    std::printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}

} // namespace bf
