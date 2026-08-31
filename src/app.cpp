#include "app.h"
#include "simfc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace bf {
namespace {

enum MenuId {
    MenuBackupDiff = 1,
    MenuBackupDump,
    MenuRestore,
    MenuQuick,
    MenuFiles,
    MenuKeymap,
    MenuHelp,
    MenuBrightDown,
    MenuBrightUp,
    MenuDisconnect,
    MenuExit,
};

struct QuickCmd {
    const char* label;
    const char* cmd;
    const char* hint;
};

// Read-only queries first; the two that change the FC are last and confirmed.
const QuickCmd kQuick[] = {
    {"status",      "status",    "battery, sensors, arming flags"},
    {"version",     "version",   "firmware and target"},
    {"tasks",       "tasks",     "scheduler load"},
    {"diff",        "diff",      "settings that differ from default"},
    {"diff all",    "diff all",  "full diff, all profiles"},
    {"dump",        "dump",      "every setting, current profile"},
    {"dump all",    "dump all",  "every setting, all profiles"},
    {"serial",      "serial",    "UART assignment"},
    {"resource",    "resource",  "pin mapping"},
    {"vtx_info",    "vtx_info",  "video transmitter state"},
    {"vtxtable",    "vtxtable",  "band and power table"},
    {"mcu_id",      "mcu_id",    "unique chip id"},
    {"save",        "save",      "write to flash and reboot"},
    {"exit CLI",    "exit",      "leaves CLI - reboots the FC"},
};
constexpr int kQuickCount = static_cast<int>(sizeof(kQuick) / sizeof(kQuick[0]));

} // namespace

void ListState::clamp(int count, int visible) {
    if (count <= 0) { sel = 0; top = 0; return; }
    sel = std::max(0, std::min(sel, count - 1));
    if (visible <= 0) { top = 0; return; }
    if (top > sel) top = sel;
    if (sel >= top + visible) top = sel - visible + 1;
    top = std::max(0, std::min(top, std::max(0, count - visible)));
}

void ListState::move(int delta, int count, int visible) {
    sel += delta;
    clamp(count, visible);
}

void ListState::ensureVisible(int visible) {
    if (visible > 0 && sel < top) top = sel;
}

int App::columns() const { return display_.width() / kGlyphW; }

int App::bodyRows(bool withInput) const {
    const int bottom = display_.height() - kHintH - (withInput ? kInputH : 0);
    return std::max(1, (bottom - kBodyY) / kGlyphH);
}

void App::pushLocal(const std::string& text, LineKind kind) {
    term_.addLine(text, kind);
    if (term_.following()) term_.scrollToBottom(bodyRows(true));
    dirty_ = true;
}

void App::setScreen(Screen s) {
    if (screen_ == s) return;
    screen_ = s;
    // A key held across a screen change must not act on the new screen.
    keyboard_.releaseAll();
    dirty_ = true;
}

void App::confirm(const std::string& title, const std::string& body,
                  const std::string& yesLabel, std::function<void()> onYes) {
    modal_ = true;
    modalIsConfirm_ = true;
    modalTitle_ = title;
    modalBody_ = body;
    modalYes_ = yesLabel;
    modalAction_ = std::move(onYes);
    // Drop held keys so an autorepeat cannot answer a dialog the user has not
    // seen yet.
    keyboard_.releaseAll();
    dirty_ = true;
}

void App::notice(const std::string& title, const std::string& body) {
    modal_ = true;
    modalIsConfirm_ = false;
    modalTitle_ = title;
    modalBody_ = body;
    modalYes_.clear();
    modalAction_ = nullptr;
    keyboard_.releaseAll();
    dirty_ = true;
}

void App::closeModal() {
    modal_ = false;
    modalAction_ = nullptr;
    keyboard_.releaseAll();
    dirty_ = true;
}

// ------------------------------------------------------------------- setup

bool App::setup(const Options& opt, std::string& error) {
    opt_ = opt;

    std::string err;
    if (!storage_.init(err)) {
        error = "storage: " + err;
        return false;
    }
    config_.load(storage_);

    // Sym-layer corrections, e.g. `sym.0x28 = _`.
    for (const auto& kv : config_.all()) {
        if (kv.first.rfind("sym.", 0) != 0 || kv.second.empty()) continue;
        try {
            const int scan = std::stoi(kv.first.substr(4), nullptr, 0);
            keyboard_.decoder().setSymOverride(scan, kv.second[0]);
        } catch (...) {
        }
    }

    if (!opt_.headless) {
        std::string dispErr;
        if (!display_.open(opt_.fbDevice, dispErr)) {
            // Headless is a fallback, not a failure: the app still runs for
            // previews and on a dev host with no panel.
            display_.setHeadlessSize(kScreenW, kScreenH);
            status_ = "framebuffer: " + dispErr;
            statusUntil_ = nowMs() + 6000;
        }
    } else {
        display_.setHeadlessSize(kScreenW, kScreenH);
    }

    if (backlight_.discover()) {
        brightness_ = config_.getInt("brightness", 100);
        backlight_.setPercent(brightness_);
    }

    if (!opt_.stdinKeys) {
        std::string kbErr;
        if (keyboard_.open(kbErr) == 0) {
            keyboard_.enableStdinFallback(true);
            if (status_.empty()) {
                status_ = "keyboard: " + kbErr + " (using stdin)";
                statusUntil_ = nowMs() + 6000;
            }
        }
    } else {
        keyboard_.enableStdinFallback(true);
    }

    term_.setWidth(columns());
    editor_.loadHistory(storage_.loadHistory());

    menu_ = {
        {"Backup config to file", "runs `diff all`", MenuBackupDiff, true},
        {"Full dump to file", "runs `dump all`", MenuBackupDump, true},
        {"Restore from backup...", "sends a saved file", MenuRestore, true},
        {"Quick commands...", "common CLI queries", MenuQuick, true},
        {"Saved backups...", "view or delete", MenuFiles, true},
        {"Keymap & key test", "find a symbol key", MenuKeymap, true},
        {"Help", "keys and workflow", MenuHelp, true},
        {"Brightness -", "", MenuBrightDown, true},
        {"Brightness +", "", MenuBrightUp, true},
        {"Disconnect", "close the serial port", MenuDisconnect, true},
        {"Exit bfcli", "return to the launcher", MenuExit, true},
    };
    quick_.clear();
    for (int i = 0; i < kQuickCount; ++i) {
        quick_.push_back(MenuItem{kQuick[i].label, kQuick[i].hint, i, true});
    }

    for (int i = 0; i < kBaudChoiceCount; ++i) {
        if (kBaudChoices[i] == opt_.baud) baudIndex_ = i;
    }

    refreshPorts();
    refreshFiles();

    if (!opt_.portOverride.empty()) {
        std::string cerr;
        if (session_.connect(opt_.portOverride, opt_.baud, cerr)) {
            setScreen(Screen::Terminal);
        } else {
            pushLocal("connect failed: " + cerr, LineKind::Error);
        }
    } else if (opt_.autoConnect && !ports_.empty() &&
               ports_.front().looksLikeFlightController()) {
        portList_.sel = 0;
        connectSelected();
    }
    return true;
}

void App::teardown() {
    storage_.saveHistory(editor_.history());
    config_.setInt("brightness", brightness_);
    std::string err;
    config_.save(storage_, err);
    session_.disconnect();
    keyboard_.close();
    display_.close();
}

// ------------------------------------------------------------------ actions

void App::refreshPorts() {
    ports_ = enumeratePorts();
    portList_.clamp(static_cast<int>(ports_.size()), bodyRows(false));
    dirty_ = true;
}

void App::refreshFiles() {
    files_ = storage_.listBackups();
    fileList_.clamp(static_cast<int>(files_.size()), bodyRows(false));
    dirty_ = true;
}

void App::connectSelected() {
    if (ports_.empty()) return;
    const PortInfo& p = ports_[static_cast<size_t>(portList_.sel)];
    if (isDfuId(p.vendorId, p.productId)) {
        notice("DFU mode",
               "This device is in the bootloader, not the CLI. Power-cycle the "
               "flight controller and reconnect.");
        return;
    }
    const int baud = kBaudChoices[baudIndex_];
    std::string err;
    pushLocal("connecting to " + p.device + (p.kind == "uart"
                                                 ? " @ " + std::to_string(baud)
                                                 : ""),
              LineKind::Local);
    if (!session_.connect(p.device, baud, err)) {
        pushLocal("connect failed: " + err, LineKind::Error);
        notice("Could not open port", err);
        return;
    }
    setScreen(Screen::Terminal);
}

void App::doDisconnect() {
    session_.disconnect();
    pushLocal("-- disconnected --", LineKind::Warn);
    refreshPorts();
    setScreen(Screen::Ports);
}

void App::submitLine() {
    const std::string line = editor_.text();
    completions_.clear();
    completionNote_.clear();
    if (line.empty()) {
        if (session_.ready()) session_.send("");
        return;
    }
    if (!session_.connected()) {
        pushLocal("not connected", LineKind::Error);
        return;
    }

    const RiskNote risk = riskFor(line);
    auto fire = [this]() {
        const std::string text = editor_.commit();
        if (!session_.send(text)) {
            pushLocal("busy - wait for the current command to finish", LineKind::Warn);
        }
        term_.scrollToBottom(bodyRows(true));
    };

    if (risk.risk == Risk::Motors || risk.risk == Risk::Destructive) {
        confirm(risk.risk == Risk::Motors ? "Props off?" : "Are you sure?",
                risk.message + "\n\n> " + line, "Send", fire);
        return;
    }
    fire();
}

void App::doComplete() {
    const Completer::Result r = completer_.complete(editor_.text(), editor_.cursor());
    completions_.clear();
    completionNote_.clear();
    if (r.candidates.empty()) {
        completionNote_ = "no match";
        dirty_ = true;
        return;
    }
    if (r.candidates.size() == 1) {
        editor_.replaceWord(r.candidates.front());
        // A completed command is almost always followed by an argument.
        if (editor_.cursor() == static_cast<int>(editor_.text().size())) editor_.insert(' ');
        dirty_ = true;
        return;
    }
    if (r.commonPrefix.size() > r.prefix.size()) {
        editor_.replaceWord(r.commonPrefix);
    }
    completions_ = r.candidates;
    if (completions_.size() > 40) completions_.resize(40);
    completionNote_ = std::to_string(r.candidates.size()) + " matches";
    dirty_ = true;
}

void App::runBackup(const std::string& command, const std::string& label) {
    if (!session_.ready()) {
        notice("Not ready", "Connect to a flight controller first.");
        return;
    }
    pushLocal("-- " + label + " --", LineKind::Local);
    const bool started = session_.startCapture(
        command, label, [this, label](bool ok, const std::string& text) {
            if (!ok || text.empty()) {
                notice("Backup failed",
                       "The flight controller did not return a complete response.");
                return;
            }
            const std::string craft = craftNameFromDump(text);
            const std::string board = boardNameFromDump(text);
            const std::string name = storage_.makeBackupName(craft, board);
            const std::string path = storage_.backupDir() + "/" + name;
            std::string err;
            if (!storage_.writeAtomic(path, text, err)) {
                notice("Could not save", err);
                return;
            }
            refreshFiles();
            notice("Saved", name + "\n\n" + humanBytes(text.size()) + " in " +
                                storage_.backupDir());
        });
    if (!started) notice("Busy", "A command is already running.");
}

void App::viewFile(const BackupFile& f) {
    std::string text, err;
    if (!storage_.readFile(f.path, text, err)) {
        notice("Could not read", err);
        return;
    }
    pushLocal("---- " + f.name + " ----", LineKind::Local);
    size_t pos = 0;
    int shown = 0;
    while (pos < text.size() && shown < 4000) {
        const size_t nl = text.find('\n', pos);
        const std::string line =
            text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        term_.addLine(line, LineKind::Fc);
        ++shown;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    pushLocal("---- end of " + f.name + " ----", LineKind::Local);
    term_.scrollToBottom(bodyRows(true));
    setScreen(Screen::Terminal);
}

void App::restoreFile(const BackupFile& f) {
    if (!session_.ready()) {
        notice("Not ready", "Connect to a flight controller first.");
        return;
    }
    std::string text, err;
    if (!storage_.readFile(f.path, text, err)) {
        notice("Could not read", err);
        return;
    }
    const std::vector<std::string> lines = restorableLines(text);
    if (lines.empty()) {
        notice("Nothing to send", "That file has no CLI commands in it.");
        return;
    }
    const std::string board = boardNameFromDump(text);
    std::string body = std::to_string(lines.size()) + " lines from\n" + f.name;
    if (!board.empty()) {
        body += "\n\nfile board: " + board;
        if (!session_.board().empty() && session_.board() != board) {
            body += "\nFC board:   " + session_.board() + "\n*** BOARDS DIFFER ***";
        }
    }
    body += "\n\nSettings are written to the FC. Nothing is\nkept until you run `save`.";

    confirm("Restore config", body, "Send", [this, lines, f]() {
        setScreen(Screen::Terminal);
        pushLocal("-- restoring " + f.name + " --", LineKind::Local);
        if (!session_.startRestore(lines, "Restoring " + f.name)) {
            notice("Busy", "A command is already running.");
        }
    });
}

void App::deleteFile(const BackupFile& f) {
    confirm("Delete backup", f.name + "\n\nThis cannot be undone.", "Delete", [this, f]() {
        std::string err;
        if (!storage_.deleteBackup(f.path, err)) {
            notice("Could not delete", err);
            return;
        }
        refreshFiles();
        status_ = "deleted " + f.name;
        statusUntil_ = nowMs() + 3000;
    });
}

void App::applyQuick(int id) {
    if (id < 0 || id >= kQuickCount) return;
    const QuickCmd& q = kQuick[id];
    const std::string cmd = q.cmd;

    auto run = [this, cmd]() {
        setScreen(Screen::Terminal);
        if (!session_.ready()) {
            notice("Not ready", "Connect to a flight controller first.");
            return;
        }
        editor_.setText(cmd);
        const std::string text = editor_.commit();
        session_.send(text);
        term_.scrollToBottom(bodyRows(true));
    };

    if (cmd == "save") {
        confirm("Save to flash",
                "Writes the current settings to the FC and\nreboots it. The CLI "
                "link will drop.", "Save", run);
        return;
    }
    if (cmd == "exit") {
        confirm("Exit CLI",
                "On Betaflight this reboots the flight\ncontroller. The serial "
                "link will drop.", "Exit CLI", run);
        return;
    }
    run();
}

void App::adjustBrightness(int delta) {
    brightness_ = std::max(5, std::min(100, brightness_ + delta));
    if (backlight_.available() && backlight_.setPercent(brightness_)) {
        status_ = "brightness " + std::to_string(brightness_) + "%";
    } else {
        status_ = backlight_.available() ? "brightness write rejected"
                                         : "no backlight control available";
    }
    statusUntil_ = nowMs() + 2000;
    dirty_ = true;
}

void App::applyMenu(int id) {
    switch (id) {
    case MenuBackupDiff:
        setScreen(Screen::Terminal);
        runBackup("diff all", "Backup (diff all)");
        break;
    case MenuBackupDump:
        setScreen(Screen::Terminal);
        runBackup("dump all", "Full dump");
        break;
    case MenuRestore:
    case MenuFiles:
        refreshFiles();
        setScreen(Screen::Files);
        break;
    case MenuQuick:
        setScreen(Screen::Quick);
        break;
    case MenuKeymap:
        setScreen(Screen::Keymap);
        break;
    case MenuHelp:
        helpScroll_ = 0;
        setScreen(Screen::Help);
        break;
    case MenuBrightDown: adjustBrightness(-10); break;
    case MenuBrightUp:   adjustBrightness(+10); break;
    case MenuDisconnect: doDisconnect(); break;
    case MenuExit:       running_ = false; break;
    default: break;
    }
}

// -------------------------------------------------------------------- input

bool App::handleModalKey(const KeyEvent& e) {
    if (!modal_) return false;
    // A repeat cannot answer a dialog; only a deliberate fresh press.
    if (e.repeat) return true;

    if (!modalIsConfirm_) {
        if (e.key == Key::Enter || e.key == Key::Escape || e.key == Key::Char) closeModal();
        return true;
    }
    const bool yes = (e.key == Key::Char && (e.ch == 'y' || e.ch == 'Y')) ||
                     e.key == Key::Enter;
    const bool no = (e.key == Key::Char && (e.ch == 'n' || e.ch == 'N')) ||
                    e.key == Key::Escape;
    if (yes) {
        auto action = modalAction_;
        closeModal();
        if (action) action();
    } else if (no) {
        closeModal();
    }
    return true;
}

void App::onPortsKey(const KeyEvent& e) {
    const int rows = bodyRows(false);
    const int n = static_cast<int>(ports_.size());
    switch (e.key) {
    case Key::Up:   portList_.move(-1, n, rows); dirty_ = true; break;
    case Key::Down: portList_.move(+1, n, rows); dirty_ = true; break;
    case Key::Enter: connectSelected(); break;
    case Key::Escape: running_ = false; break;
    case Key::F1: helpScroll_ = 0; setScreen(Screen::Help); break;
    case Key::Char:
        if (e.ch == 'r' || e.ch == 'R') {
            refreshPorts();
            status_ = "rescanned: " + std::to_string(ports_.size()) + " ports";
            statusUntil_ = nowMs() + 2000;
        } else if (e.ch == 'b' || e.ch == 'B') {
            baudIndex_ = (baudIndex_ + 1) % kBaudChoiceCount;
            dirty_ = true;
        } else if (e.ch == 'f' || e.ch == 'F') {
            refreshFiles();
            setScreen(Screen::Files);
        } else if (e.ch == 'h' || e.ch == 'H' || e.ch == '?') {
            helpScroll_ = 0;
            setScreen(Screen::Help);
        } else if (e.ch == 'q' || e.ch == 'Q') {
            running_ = false;
        }
        break;
    default: break;
    }
}

void App::onTerminalKey(const KeyEvent& e) {
    const int rows = bodyRows(true);

    if (e.ctrl && e.key == Key::Char) {
        switch (e.ch) {
        case 'l': term_.clear(); dirty_ = true; return;
        case 'u': editor_.killToStart(); dirty_ = true; return;
        case 'k': editor_.killToEnd(); dirty_ = true; return;
        case 'w': editor_.killWordBack(); dirty_ = true; return;
        case 'a': editor_.home(); dirty_ = true; return;
        case 'e': editor_.end(); dirty_ = true; return;
        case 'c':
            editor_.clear();
            completions_.clear();
            completionNote_.clear();
            dirty_ = true;
            return;
        default: break;
        }
    }

    switch (e.key) {
    case Key::Enter:     submitLine(); dirty_ = true; return;
    case Key::Backspace: editor_.backspace(); dirty_ = true; return;
    case Key::Delete:    editor_.del(); dirty_ = true; return;
    case Key::Left:      editor_.left(e.ctrl); dirty_ = true; return;
    case Key::Right:     editor_.right(e.ctrl); dirty_ = true; return;
    case Key::Home:      editor_.home(); dirty_ = true; return;
    case Key::End:       editor_.end(); dirty_ = true; return;
    case Key::Tab:       doComplete(); return;
    case Key::Up:
        // Up/Down walk the history; PageUp/PageDown scroll the output.
        editor_.historyPrev();
        dirty_ = true;
        return;
    case Key::Down:
        editor_.historyNext();
        dirty_ = true;
        return;
    case Key::PageUp:   term_.scrollBy(-(rows - 1), rows); dirty_ = true; return;
    case Key::PageDown: term_.scrollBy(rows - 1, rows); dirty_ = true; return;
    case Key::Escape:
        if (session_.job().active()) {
            session_.cancelJob();
            return;
        }
        menuList_.clamp(static_cast<int>(menu_.size()), bodyRows(false));
        setScreen(Screen::Menu);
        return;
    case Key::Help:      helpScroll_ = 0; setScreen(Screen::Help); return;
    case Key::BrightUp:  adjustBrightness(+10); return;
    case Key::BrightDown: adjustBrightness(-10); return;
    case Key::F1: helpScroll_ = 0; setScreen(Screen::Help); return;
    case Key::F2: if (session_.ready()) session_.send("status"); return;
    case Key::F3: if (session_.ready()) session_.send("version"); return;
    case Key::F4: if (session_.ready()) session_.send("diff"); return;
    case Key::F5: runBackup("diff all", "Backup (diff all)"); return;
    case Key::F6: refreshFiles(); setScreen(Screen::Files); return;
    case Key::F7: if (session_.ready()) session_.send("tasks"); return;
    case Key::F8:
        applyQuick(12);   // save, with its confirmation
        return;
    case Key::F9:
        menuList_.clamp(static_cast<int>(menu_.size()), bodyRows(false));
        setScreen(Screen::Menu);
        return;
    case Key::F10: doDisconnect(); return;
    case Key::Char:
        editor_.insert(e.ch);
        completions_.clear();
        completionNote_.clear();
        dirty_ = true;
        return;
    default: return;
    }
}

void App::onMenuKey(const KeyEvent& e) {
    const bool quick = (screen_ == Screen::Quick);
    std::vector<MenuItem>& items = quick ? quick_ : menu_;
    ListState& st = quick ? quickList_ : menuList_;
    const int rows = bodyRows(false);
    const int n = static_cast<int>(items.size());

    switch (e.key) {
    case Key::Up:   st.move(-1, n, rows); dirty_ = true; break;
    case Key::Down: st.move(+1, n, rows); dirty_ = true; break;
    case Key::PageUp:   st.move(-rows, n, rows); dirty_ = true; break;
    case Key::PageDown: st.move(+rows, n, rows); dirty_ = true; break;
    case Key::Enter:
        if (n == 0) break;
        if (quick) applyQuick(items[static_cast<size_t>(st.sel)].id);
        else applyMenu(items[static_cast<size_t>(st.sel)].id);
        break;
    case Key::Escape:
        setScreen(quick ? Screen::Menu : Screen::Terminal);
        break;
    default: break;
    }
}

void App::onFilesKey(const KeyEvent& e) {
    const int rows = bodyRows(false);
    const int n = static_cast<int>(files_.size());
    switch (e.key) {
    case Key::Up:   fileList_.move(-1, n, rows); dirty_ = true; break;
    case Key::Down: fileList_.move(+1, n, rows); dirty_ = true; break;
    case Key::PageUp:   fileList_.move(-rows, n, rows); dirty_ = true; break;
    case Key::PageDown: fileList_.move(+rows, n, rows); dirty_ = true; break;
    case Key::Enter:
        if (n > 0) restoreFile(files_[static_cast<size_t>(fileList_.sel)]);
        break;
    case Key::Escape:
        setScreen(session_.connected() ? Screen::Terminal : Screen::Ports);
        break;
    case Key::Char:
        if (n == 0) break;
        if (e.ch == 'v' || e.ch == 'V') viewFile(files_[static_cast<size_t>(fileList_.sel)]);
        else if (e.ch == 'd' || e.ch == 'D') deleteFile(files_[static_cast<size_t>(fileList_.sel)]);
        else if (e.ch == 'r' || e.ch == 'R') refreshFiles();
        break;
    default: break;
    }
}

void App::onKeymapKey(const KeyEvent& e) {
    // Everything except Escape is swallowed so the tester can show it.
    if (e.key == Key::Escape && !e.repeat) {
        setScreen(Screen::Terminal);
        return;
    }
    const int rows = bodyRows(false) - 6;
    if (e.key == Key::Up) keymapList_.move(-1, descriptorCount(), rows);
    else if (e.key == Key::Down) keymapList_.move(+1, descriptorCount(), rows);
    dirty_ = true;
}

void App::onHelpKey(const KeyEvent& e) {
    switch (e.key) {
    case Key::Up:       helpScroll_ = std::max(0, helpScroll_ - 1); dirty_ = true; break;
    case Key::Down:     ++helpScroll_; dirty_ = true; break;
    case Key::PageUp:   helpScroll_ = std::max(0, helpScroll_ - 8); dirty_ = true; break;
    case Key::PageDown: helpScroll_ += 8; dirty_ = true; break;
    case Key::Escape:
    case Key::Enter:
        setScreen(session_.connected() ? Screen::Terminal : Screen::Ports);
        break;
    default: break;
    }
}

void App::handleKey(const KeyEvent& e) {
    lastKey_ = e;
    haveLastKey_ = true;
    if (screen_ == Screen::Keymap) dirty_ = true;

    if (handleModalKey(e)) return;

    switch (screen_) {
    case Screen::Ports:    onPortsKey(e); break;
    case Screen::Terminal: onTerminalKey(e); break;
    case Screen::Menu:
    case Screen::Quick:    onMenuKey(e); break;
    case Screen::Files:    onFilesKey(e); break;
    case Screen::Keymap:   onKeymapKey(e); break;
    case Screen::Help:     onHelpKey(e); break;
    }
}

// --------------------------------------------------------------- main loop

void App::tick(uint64_t now) {
    const size_t linesBefore = term_.lineCount();
    session_.poll(now);
    if (term_.lineCount() != linesBefore || session_.busy()) {
        if (term_.following()) term_.scrollToBottom(bodyRows(true));
        dirty_ = true;
    }

    const JobStatus& job = session_.job();
    if (job.finished) {
        const bool wasRestore = job.kind == JobKind::Restore;
        const std::string message = job.message;
        const bool ok = job.ok;
        // A capture reports through its own callback; only a restore or a
        // failure needs a dialog here.
        if (wasRestore || !ok) {
            if (!modal_) {
                notice(ok ? "Restore complete" : "Finished with problems", message);
            }
        }
        pushLocal("-- " + message + " --", ok ? LineKind::Good : LineKind::Warn);
        session_.clearFinishedJob();
    }

    if (session_.linkLost() && screen_ == Screen::Terminal && !modal_) {
        refreshPorts();
    }
    if (!status_.empty() && now > statusUntil_) {
        status_.clear();
        dirty_ = true;
    }
}

int App::run(const Options& opt) {
    std::string error;
    if (!setup(opt, error)) {
        std::fprintf(stderr, "bfcli: %s\n", error.c_str());
        return 1;
    }

    if (!opt.previewDir.empty()) {
        // Render one frame of every screen for host-side inspection. Nothing is
        // connected and no key is synthesised, so this is purely a paint test.
        const struct { Screen screen; const char* name; } kShots[] = {
            {Screen::Ports, "01-ports"},   {Screen::Terminal, "02-terminal"},
            {Screen::Menu, "03-menu"},     {Screen::Quick, "04-quick"},
            {Screen::Files, "05-files"},   {Screen::Keymap, "06-keymap"},
            {Screen::Help, "07-help"},
        };
        status_.clear();   // show each screen's real hint bar, not a startup notice
        pushLocal("# Betaflight / STM32G47X (G473) 2026.6.0-alpha MSP API: 1.48", LineKind::Fc);
        pushLocal("Voltage: 4.12V (1S battery - OK)", LineKind::Fc);
        pushLocal("Arming disable flags: RXLOSS CLI", LineKind::Fc);
        pushLocal("-- CLI ready --", LineKind::Good);
        editor_.setText("set gyro_lpf1_static_hz = 0");
        int written = 0;
        for (const auto& shot : kShots) {
            screen_ = shot.screen;
            render();
            const std::string path = opt.previewDir + "/" + shot.name + ".ppm";
            if (display_.canvas().writePpm(path)) ++written;
        }
        // One more with a confirmation dialog up, since that is its own layout.
        screen_ = Screen::Terminal;
        confirm("Props off?", "Spins a motor. Props off?\n\n> motor 1 1100", "Send", nullptr);
        render();
        if (display_.canvas().writePpm(opt.previewDir + "/08-confirm.ppm")) ++written;
        closeModal();
        std::printf("wrote %d previews to %s\n", written, opt.previewDir.c_str());
        teardown();
        return written > 0 ? 0 : 1;
    }

    SimFc sim;
    if (opt.simulate) {
        std::string simErr;
        if (!sim.start(simErr)) {
            std::fprintf(stderr, "bfcli: simulator: %s\n", simErr.c_str());
            teardown();
            return 1;
        }
        std::string cerr;
        pushLocal("simulated flight controller on " + sim.devicePath(), LineKind::Local);
        session_.connect(sim.devicePath(), 115200, cerr);
        setScreen(Screen::Terminal);
    }

    RawTerminalMode rawTty(opt.stdinKeys || !keyboard_.anyOpen());

    const uint64_t frameMs = 33;   // ~30 fps, matching the reference UI target
    uint64_t nextFrame = nowMs();

    while (running_) {
        const uint64_t now = nowMs();

        // Wait on input and serial together so the UI stays responsive without
        // ever spinning: a slow `dump` never blocks the frame loop.
        std::vector<pollfd> pfds;
        for (int fd : keyboard_.fds()) pfds.push_back(pollfd{fd, POLLIN, 0});
        if (session_.connected()) pfds.push_back(pollfd{session_.fd(), POLLIN, 0});
        if (!keyboard_.anyOpen() || opt.stdinKeys) {
            pfds.push_back(pollfd{STDIN_FILENO, POLLIN, 0});
        }
        int waitMs = static_cast<int>(nextFrame > now ? nextFrame - now : 0);
        if (session_.busy() || session_.job().active()) waitMs = std::min(waitMs, 10);
        if (!pfds.empty()) ::poll(pfds.data(), pfds.size(), waitMs);
        else sleepMs(waitMs);

        std::vector<KeyEvent> events;
        keyboard_.pump(nowMs(), events);
        keyboard_.pumpStdin(events);
        keyboard_.pumpRepeat(nowMs(), events);
        for (const KeyEvent& e : events) {
            handleKey(e);
            if (!running_) break;
        }

        if (opt.simulate) sim.pump();
        tick(nowMs());

        if (nowMs() >= nextFrame) {
            if (dirty_) {
                render();
                display_.present();
                dirty_ = false;
            }
            nextFrame = nowMs() + frameMs;
            ++frame_;
        }
        if (opt.frameLimit > 0 && frame_ >= static_cast<uint64_t>(opt.frameLimit)) break;
    }

    teardown();
    return 0;
}

int writePreviews(const std::string& dir) {
    App::Options opt;
    opt.headless = true;
    opt.autoConnect = false;
    opt.previewDir = dir;
    App app;
    return app.run(opt);
}

} // namespace bf
