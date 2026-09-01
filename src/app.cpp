#include "app.h"
#include "brand.h"
#include "simfc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <poll.h>
#include <unistd.h>

namespace bf {
namespace {

enum MenuId {
    MenuFieldCheck = 1,
    MenuBackupDiff,
    MenuBackupDump,
    MenuRestore,
    MenuQuick,
    MenuFiles,
    MenuKeymap,
    MenuHelp,
    MenuAbout,
    MenuSoundToggle,
    MenuSoundDown,
    MenuSoundUp,
    MenuBrightDown,
    MenuBrightUp,
    MenuDisconnect,
    MenuExit,
    MenuOpenFlightController = 100,
    MenuOpenBackupRestore,
    MenuOpenControlsInfo,
    MenuOpenSoundDisplay,
    MenuOpenConnectionExit,
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

// A connection-time sample cannot catch the common bench case where an AIO
// stack warms for a few minutes before crossing its alarm threshold. `status`
// is read-only, so repeat it at a low cadence whenever the CLI is otherwise
// idle. The raw response remains visible instead of hiding safety evidence.
constexpr uint64_t kTemperatureCheckIntervalMs = 30000;
constexpr uint64_t kTemperatureCheckRetryMs = 1000;

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

int App::menuRows() const {
    const int bottom = display_.height() - kHintH;
    return std::max(1, (bottom - kBodyY) / kMenuRowH);
}

void App::setupMenus() {
    menu_ = {
        {"Flight controller...", "checks and common CLI queries", MenuOpenFlightController, true},
        {"Backup & restore...", "save, inspect, or restore configuration", MenuOpenBackupRestore, true},
        {"Controls & info...", "keymap, help, and build identity", MenuOpenControlsInfo, true},
        {"Sound & display...", "HUD audio, volume, and brightness", MenuOpenSoundDisplay, true},
        {"Connection & exit...", "close the FC link or return to launcher", MenuOpenConnectionExit, true},
    };
    controllerMenu_ = {
        {"Run field check", "no config writes; status, blockers, runtime", MenuFieldCheck, true},
        {"Quick commands...", "common CLI queries", MenuQuick, true},
    };
    backupMenu_ = {
        {"Backup config to file", "runs `diff all`", MenuBackupDiff, true},
        {"Full dump to file", "runs `dump all`", MenuBackupDump, true},
        {"Restore from backup...", "sends a saved file", MenuRestore, true},
        {"Saved backups...", "view or delete", MenuFiles, true},
    };
    controlsMenu_ = {
        {"Keymap & key test", "find a symbol key", MenuKeymap, true},
        {"Help", "keys and workflow", MenuHelp, true},
        {"About GNDHOG ZERO", "0ct0 / build / ground crew", MenuAbout, true},
    };
    settingsMenu_ = {
        {"HUD sounds", "fighter-HUD cues", MenuSoundToggle, true},
        {"HUD volume -", "", MenuSoundDown, true},
        {"HUD volume +", "", MenuSoundUp, true},
        {"Brightness -", "", MenuBrightDown, true},
        {"Brightness +", "", MenuBrightUp, true},
    };
    connectionMenu_ = {
        {"Disconnect", "restore bench VTX state or close link", MenuDisconnect, true},
        {"Exit GNDHOG ZERO", "return to the launcher", MenuExit, true},
    };
    menuPage_ = MenuPage::Root;
    menuList_ = ListState{};
    submenuList_ = ListState{};

    quick_.clear();
    for (int i = 0; i < kQuickCount; ++i) {
        quick_.push_back(MenuItem{kQuick[i].label, kQuick[i].hint, i, true});
    }
    quickList_ = ListState{};
}

std::vector<MenuItem>& App::currentMenuItems() {
    switch (menuPage_) {
    case MenuPage::FlightController: return controllerMenu_;
    case MenuPage::BackupRestore:    return backupMenu_;
    case MenuPage::ControlsInfo:     return controlsMenu_;
    case MenuPage::SoundDisplay:     return settingsMenu_;
    case MenuPage::ConnectionExit:   return connectionMenu_;
    case MenuPage::Root:
    default:                         return menu_;
    }
}

ListState& App::currentMenuList() {
    return menuPage_ == MenuPage::Root ? menuList_ : submenuList_;
}

void App::openMenuPage(MenuPage page) {
    if (menuPage_ == page) return;
    menuPage_ = page;
    if (page != MenuPage::Root) submenuList_ = ListState{};
    currentMenuList().clamp(static_cast<int>(currentMenuItems().size()), menuRows());
    keyboard_.releaseAll();
    dirty_ = true;
}

void App::pushLocal(const std::string& text, LineKind kind) {
    term_.addLine(text, kind);
    if (term_.following()) term_.scrollToBottom(bodyRows(true));
    dirty_ = true;
}

void App::setScreen(Screen s) {
    if (screen_ == s) return;
    screen_ = s;
    if (screen_ == Screen::Menu) refreshSoundMenu();
    // A key held across a screen change must not act on the new screen.
    keyboard_.releaseAll();
    dirty_ = true;
}

void App::confirm(const std::string& title, const std::string& body,
                  const std::string& yesLabel, std::function<void()> onYes,
                  std::function<void()> onNo) {
    modal_ = true;
    modalIsConfirm_ = true;
    modalTitle_ = title;
    modalBody_ = body;
    modalYes_ = yesLabel;
    modalAction_ = std::move(onYes);
    modalCancelAction_ = std::move(onNo);
    // Drop held keys so an autorepeat cannot answer a dialog the user has not
    // seen yet.
    keyboard_.releaseAll();
    audio_.play(HudCue::Prompt);
    dirty_ = true;
}

void App::notice(const std::string& title, const std::string& body, HudCue cue) {
    modal_ = true;
    modalIsConfirm_ = false;
    modalTitle_ = title;
    modalBody_ = body;
    modalYes_.clear();
    modalAction_ = nullptr;
    modalCancelAction_ = nullptr;
    keyboard_.releaseAll();
    audio_.play(cue);
    dirty_ = true;
}

void App::closeModal() {
    modal_ = false;
    modalAction_ = nullptr;
    modalCancelAction_ = nullptr;
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

    soundVolume_ = std::clamp(config_.getInt("sound.volume", 70), 0, 100);
    soundEnabled_ = config_.getBool("sound.enabled", true) && !opt_.muteSound;
    audio_.setVolume(soundVolume_);
    audio_.setEnabled(soundEnabled_);
    audio_.start();
    lastAudioError_ = audio_.lastError();

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

    setupMenus();
    refreshSoundMenu();

    for (int i = 0; i < kBaudChoiceCount; ++i) {
        if (kBaudChoices[i] == opt_.baud) baudIndex_ = i;
    }

    refreshPorts();
    refreshFiles();

    if (opt_.showAbout) {
        returnScreen_ = Screen::Ports;
        setScreen(Screen::About);
    } else if (!opt_.portOverride.empty()) {
        std::string cerr;
        if (session_.connect(opt_.portOverride, opt_.baud, cerr)) {
            audio_.play(HudCue::LinkUp);
            beginConnectionSafety(opt_.portOverride);
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
    if (!opt_.muteSound) config_.setBool("sound.enabled", soundEnabled_);
    config_.setInt("sound.volume", soundVolume_);
    std::string err;
    config_.save(storage_, err);
    session_.disconnect();
    audio_.shutdown();
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

void App::beginConnectionSafety(const std::string& device) {
    nextTemperatureCheckMs_ = nowMs();
    temperatureMonitorStarted_ = false;
    temperatureAlarmLevel_ = 0;
    temperatureWarningPending_ = false;
    temperatureWarningC_ = 0;
    lastTemperatureSequence_ = session_.coreTemperatureSequence();
    thermalTripAttempted_ = false;

    const ThermalTripProbe& probe = thermalTrip_.inspect(device);
    thermalTripPromptPending_ = probe.eligible;
    if (probe.eligible) {
        pushLocal("-- verified EXT USB4 and switched EXT 5V; thermal trip can be armed --",
                  LineKind::Good);
    } else if (probe.usbDeviceFound) {
        pushLocal("-- temperature watch only: " + probe.reason + " --", LineKind::Warn);
    }
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
        notice("Could not open port", err, HudCue::Error);
        return;
    }
    audio_.play(HudCue::LinkUp);
    beginConnectionSafety(p.device);
    setScreen(Screen::Terminal);
}

void App::performThermalTrip(int temperatureC) {
    thermalTripAttempted_ = true;
    temperatureWarningPending_ = false;
    temperatureWarningC_ = 0;
    nextTemperatureCheckMs_ = 0;

    const ThermalTripProbe before = thermalTrip_.probe();
    const bool pitWasActive = session_.vtxBenchGuardActive();
    const std::string stamp = timestampCompact();
    const std::string name = "GNDHOG_thermal_trip_" + stamp + ".txt";
    const std::string path = storage_.diagnosticDir() + "/" + name;

    auto incidentBody = [&](const std::string& result) {
        std::ostringstream out;
        out << "GNDHOG ZERO THERMAL TRIP INCIDENT\n"
            << "recorded: " << stamp << "\n"
            << "trigger: fresh Betaflight FC MCU core sample >= 80 C\n"
            << "sample: " << temperatureC << " C\n"
            << "serial device: " << before.device << "\n"
            << "USB identity: " << before.usbIdentity << "\n"
            << "USB node: " << before.usbNode << "\n"
            << "route: internal GL852G branch 4 / EXT USB4\n"
            << "selector readback: USB\n"
            << "EXT 5V before trip: on\n"
            << "result: " << result << "\n"
            << "serial action: close without VTX restore\n"
            << "warning: battery power is separate and may keep the stack energized\n"
            << "warning: no automatic rail re-enable is performed\n";
        if (pitWasActive) {
            out << "VTX guard: pit mode was active; emergency power cutoff took priority\n";
        }
        return out.str();
    };

    std::string recordError;
    const bool preRecorded = storage_.writeAtomic(path, incidentBody("cut requested"), recordError);

    std::string cutError;
    const bool cut = thermalTrip_.cutPower(cutError);
    std::string finalRecordError;
    const std::string result = cut ? "EXT 5V cut and read back off"
                                   : "POWER CUT FAILED: " + cutError;
    const bool finalRecorded = storage_.writeAtomic(path, incidentBody(result), finalRecordError);

    diagnosticRunning_ = false;
    session_.disconnect();
    setScreen(Screen::Terminal);
    status_ = cut ? "thermal trip latched - EXT 5V is off"
                  : "POWER CUT FAILED - unplug FC now";
    statusUntil_ = nowMs() + 12000;

    std::string recordNote;
    if (finalRecorded) {
        recordNote = "\n\nIncident: " + name;
        pushLocal("-- thermal incident: " + path + " --", LineKind::Local);
    } else if (preRecorded) {
        recordNote = "\n\nIncident (pre-cut record): " + name;
        pushLocal("-- thermal incident final update failed: " + finalRecordError + " --",
                  LineKind::Warn);
    } else {
        const std::string& why = finalRecordError.empty() ? recordError : finalRecordError;
        recordNote = "\n\nIncident write failed: " + why;
    }

    if (cut) {
        pushLocal("-- THERMAL TRIP: EXT 5V cut, read back OFF, serial closed --",
                  LineKind::Error);
        notice("THERMAL TRIP - EXT 5V OFF",
               "FC MCU " + std::to_string(temperatureC) +
                   "C: GNDHOG cut verified EXT USB4 5 V, read it OFF, and closed serial.\n"
                   "Why: stop USB-fed bench heating.\n"
                   "Battery can still power the stack. Unplug FC and battery now.\n"
                   "Rail stays off. Do not exit or reboot before unplugging; launcher may "
                   "restore it." +
                   recordNote,
               HudCue::Critical);
        if (pitWasActive) {
            pushLocal("-- VTX restore skipped; thermal cutoff took priority --", LineKind::Warn);
        }
    } else {
        pushLocal("-- POWER CUT FAILED: " + cutError + " --", LineKind::Error);
        notice("POWER CUT FAILED - UNPLUG NOW",
               "FC MCU " + std::to_string(temperatureC) +
                   "C: GNDHOG could not verify EXT 5 V OFF.\n"
                   "Cut error: " + cutError +
                   "\nSerial closed, but power stays on. UNPLUG FC USB AND BATTERY NOW." +
                   recordNote,
               HudCue::Critical);
    }
    dirty_ = true;
}

void App::finishDisconnect(bool exitAfter) {
    diagnosticRunning_ = false;
    session_.disconnect();
    pushLocal("-- disconnected --", LineKind::Warn);
    nextTemperatureCheckMs_ = 0;
    temperatureMonitorStarted_ = false;
    temperatureAlarmLevel_ = 0;
    temperatureWarningPending_ = false;
    temperatureWarningC_ = 0;
    thermalTrip_.reset();
    thermalTripPromptPending_ = false;
    thermalTripAttempted_ = false;
    refreshPorts();
    if (exitAfter) running_ = false;
    else setScreen(Screen::Ports);
}

void App::requestDisconnect(bool exitAfter) {
    if (!session_.connected()) {
        finishDisconnect(exitAfter);
        return;
    }
    if (!session_.vtxBenchGuardActive()) {
        finishDisconnect(exitAfter);
        return;
    }
    if (!session_.ready()) {
        notice("VTX guard busy",
               "Wait for the current FC command to finish before restoring the VTX state.");
        return;
    }

    const std::string guardState = session_.vtxBenchMode() == VtxBenchMode::PitMode
                                       ? "Pit mode is active for bench work."
                                       : "The VTX state could not be confirmed after the pit-mode request.";
    confirm("Restore VTX state?",
            guardState + " Restore the saved flight state?\n\n"
            "Restore exits CLI, discards unsaved CLI changes, and reboots the FC. Cancel closes "
            "the link and leaves pit mode active until the FC is rebooted or power-cycled.",
            "Restore", [this, exitAfter]() {
                if (!session_.restoreVtxAndDisconnect()) {
                    notice("Could not restore", "The FC is busy. Wait for the prompt and try again.");
                    return;
                }
                disconnectAfterVtxRestore_ = true;
                exitAfterVtxRestore_ = exitAfter;
                status_ = "restoring VTX state via FC reboot";
                statusUntil_ = nowMs() + 5000;
            },
            [this, exitAfter]() { finishDisconnect(exitAfter); });
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
        // Commit only once the line is actually on the wire. Committing first
        // threw the text away whenever the FC was still busy with a `diff`.
        if (!session_.send(editor_.text())) {
            pushLocal("busy - wait for the current command to finish", LineKind::Warn);
            dirty_ = true;
            return;
        }
        editor_.commit();
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
                                storage_.backupDir(),
                   HudCue::Success);
        });
    if (!started) notice("Busy", "A command is already running.");
}

void App::runFieldCheck() {
    if (!session_.ready()) {
        notice("Not ready", "Connect to a flight controller first.");
        return;
    }
    diagnosticReport_ = DiagnosticReport{};
    diagnosticStatus_.clear();
    diagnosticTasks_.clear();
    diagnosticVersion_.clear();
    diagnosticError_.clear();
    diagnosticList_ = ListState{};
    diagnosticStep_ = 0;
    diagnosticRunning_ = true;
    setScreen(Screen::Diagnostics);
    pushLocal("-- field check: status / tasks / version (no config writes) --", LineKind::Local);
    runFieldCheckStep();
}

void App::runFieldCheckStep() {
    static const char* const commands[] = {"status", "tasks", "version"};
    static const char* const labels[] = {"Reading FC status", "Reading scheduler", "Reading firmware"};
    constexpr int commandCount = static_cast<int>(sizeof(commands) / sizeof(commands[0]));

    if (diagnosticStep_ >= commandCount) {
        diagnosticRunning_ = false;
        diagnosticReport_ = buildDiagnosticReport(
            diagnosticStatus_, diagnosticTasks_, diagnosticVersion_);
        status_ = diagnosticReport_.actionableBlockerCount() > 0
                      ? std::to_string(diagnosticReport_.actionableBlockerCount()) + " arming blocker(s)"
                      : "field check complete";
        statusUntil_ = nowMs() + 3000;
        audio_.play(diagnosticReport_.actionableBlockerCount() > 0
                        ? HudCue::Error
                        : HudCue::Success);
        dirty_ = true;
        return;
    }

    const int step = diagnosticStep_;
    const bool started = session_.startCapture(
        commands[step], labels[step], [this, step](bool ok, const std::string& text) {
            if (step == 0) diagnosticStatus_ = text;
            else if (step == 1) diagnosticTasks_ = text;
            else diagnosticVersion_ = text;

            if (!ok) {
                diagnosticRunning_ = false;
                diagnosticError_ = "capture stopped while running " +
                                   std::string(step == 0 ? "status" : step == 1 ? "tasks" : "version");
                diagnosticReport_ = buildDiagnosticReport(
                    diagnosticStatus_, diagnosticTasks_, diagnosticVersion_);
                dirty_ = true;
                return;
            }
            ++diagnosticStep_;
            runFieldCheckStep();
        });
    if (!started) {
        diagnosticRunning_ = false;
        diagnosticError_ = "FC became busy before the next read-only query";
        diagnosticReport_ = buildDiagnosticReport(
            diagnosticStatus_, diagnosticTasks_, diagnosticVersion_);
        dirty_ = true;
    }
}

void App::saveFieldCheck() {
    if (diagnosticRunning_ || diagnosticReport_.findings.empty()) return;
    const std::string name = storage_.makeDiagnosticName(session_.craft(), session_.board());
    const std::string path = storage_.diagnosticDir() + "/" + name;
    std::string body = formatDiagnosticReport(
        diagnosticReport_, diagnosticStatus_, diagnosticTasks_, diagnosticVersion_);
    if (!diagnosticError_.empty()) body.insert(0, "# Capture note: " + diagnosticError_ + "\n");
    std::string err;
    if (!storage_.writeAtomic(path, body, err)) {
        notice("Could not save", err);
        return;
    }
    notice("Field check saved", name + "\n\n" + storage_.diagnosticDir(), HudCue::Success);
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
        if (!session_.send(cmd)) {
            pushLocal("busy - wait for the current command to finish", LineKind::Warn);
            return;
        }
        // It belongs in the history, but the input line may hold something the
        // user typed before opening the menu.
        editor_.pushHistory(cmd);
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

void App::refreshSoundMenu() {
    const std::string audioError = audio_.lastError();
    for (MenuItem& item : settingsMenu_) {
        if (item.id == MenuSoundToggle) {
            item.label = opt_.muteSound
                             ? "HUD sounds: OFF (--mute)"
                             : std::string("HUD sounds: ") + (soundEnabled_ ? "ON" : "OFF");
            if (opt_.muteSound) {
                item.hint = "session override; restart without --mute";
            } else if (audio_.available() && audioError.empty()) {
                item.hint = audio_.backendName();
            } else {
                item.hint = "silent fallback: " +
                            (audioError.empty() ? "backend stopped" : audioError);
            }
        } else if (item.id == MenuSoundDown || item.id == MenuSoundUp) {
            item.hint = "level " + std::to_string(soundVolume_) + "%";
        }
    }
    dirty_ = true;
}

void App::toggleSound() {
    if (opt_.muteSound) {
        soundEnabled_ = false;
        audio_.setEnabled(false);
        status_ = "HUD sounds locked off by --mute for this launch";
        statusUntil_ = nowMs() + 4000;
        refreshSoundMenu();
        return;
    }
    soundEnabled_ = !soundEnabled_;
    audio_.setEnabled(soundEnabled_);
    if (soundEnabled_ && !audio_.available()) audio_.start();
    if (soundEnabled_ && audio_.available()) audio_.play(HudCue::Startup);
    status_ = soundEnabled_ ? "HUD sounds on" : "HUD sounds muted";
    if (soundEnabled_ && (!audio_.available() || !audio_.lastError().empty())) {
        status_ = "HUD audio unavailable: " + audio_.lastError();
    }
    statusUntil_ = nowMs() + 4000;
    refreshSoundMenu();
}

void App::adjustSoundVolume(int delta) {
    soundVolume_ = std::clamp(soundVolume_ + delta, 0, 100);
    audio_.setVolume(soundVolume_);
    if (soundEnabled_) audio_.play(HudCue::Select);
    status_ = "HUD volume " + std::to_string(soundVolume_) + "%";
    if (!audio_.available()) status_ += " (audio unavailable)";
    statusUntil_ = nowMs() + 2500;
    refreshSoundMenu();
}

void App::applyMenu(int id) {
    switch (id) {
    case MenuOpenFlightController:
        openMenuPage(MenuPage::FlightController);
        break;
    case MenuOpenBackupRestore:
        openMenuPage(MenuPage::BackupRestore);
        break;
    case MenuOpenControlsInfo:
        openMenuPage(MenuPage::ControlsInfo);
        break;
    case MenuOpenSoundDisplay:
        openMenuPage(MenuPage::SoundDisplay);
        break;
    case MenuOpenConnectionExit:
        openMenuPage(MenuPage::ConnectionExit);
        break;
    case MenuFieldCheck:
        runFieldCheck();
        break;
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
    case MenuAbout:
        returnScreen_ = Screen::Menu;
        setScreen(Screen::About);
        break;
    case MenuSoundToggle: toggleSound(); break;
    case MenuSoundDown:   adjustSoundVolume(-10); break;
    case MenuSoundUp:     adjustSoundVolume(+10); break;
    case MenuBrightDown: adjustBrightness(-10); break;
    case MenuBrightUp:   adjustBrightness(+10); break;
    case MenuDisconnect: requestDisconnect(false); break;
    case MenuExit:       requestDisconnect(true); break;
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
        audio_.play(HudCue::Select);
        auto action = modalAction_;
        closeModal();
        if (action) action();
    } else if (no) {
        audio_.play(HudCue::Back);
        auto action = modalCancelAction_;
        closeModal();
        if (action) action();
    }
    return true;
}

void App::onPortsKey(const KeyEvent& e) {
    const int rows = bodyRows(false);
    const int n = static_cast<int>(ports_.size());
    switch (e.key) {
    case Key::Up:   portList_.move(-1, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Down: portList_.move(+1, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Enter: audio_.play(HudCue::Select); connectSelected(); break;
    case Key::Escape: audio_.play(HudCue::Back); running_ = false; break;
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
        } else if (e.ch == 'a' || e.ch == 'A') {
            returnScreen_ = Screen::Ports;
            setScreen(Screen::About);
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
    case Key::Enter:     audio_.play(HudCue::Command); submitLine(); dirty_ = true; return;
    case Key::Backspace: editor_.backspace(); dirty_ = true; return;
    case Key::Delete:    editor_.del(); dirty_ = true; return;
    case Key::Left:      editor_.left(e.ctrl); dirty_ = true; return;
    case Key::Right:     editor_.right(e.ctrl); dirty_ = true; return;
    case Key::Home:      editor_.home(); dirty_ = true; return;
    case Key::End:       editor_.end(); dirty_ = true; return;
    case Key::Tab:       audio_.play(HudCue::Select); doComplete(); return;
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
        openMenuPage(MenuPage::Root);
        menuList_.clamp(static_cast<int>(menu_.size()), menuRows());
        audio_.play(HudCue::Back);
        setScreen(Screen::Menu);
        return;
    case Key::Help:      helpScroll_ = 0; setScreen(Screen::Help); return;
    case Key::BrightUp:  adjustBrightness(+10); return;
    case Key::BrightDown: adjustBrightness(-10); return;
    case Key::F1: helpScroll_ = 0; setScreen(Screen::Help); return;
    case Key::F2: runFieldCheck(); return;
    case Key::F3: if (session_.ready()) session_.send("version"); return;
    case Key::F4: if (session_.ready()) session_.send("diff"); return;
    case Key::F5: runBackup("diff all", "Backup (diff all)"); return;
    case Key::F6: refreshFiles(); setScreen(Screen::Files); return;
    case Key::F7: if (session_.ready()) session_.send("tasks"); return;
    case Key::F8:
        applyQuick(12);   // save, with its confirmation
        return;
    case Key::F9:
        openMenuPage(MenuPage::Root);
        menuList_.clamp(static_cast<int>(menu_.size()), menuRows());
        setScreen(Screen::Menu);
        return;
    case Key::F10: requestDisconnect(false); return;
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
    std::vector<MenuItem>& items = quick ? quick_ : currentMenuItems();
    ListState& st = quick ? quickList_ : currentMenuList();
    const int rows = menuRows();
    const int n = static_cast<int>(items.size());

    switch (e.key) {
    case Key::Up:   st.move(-1, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Down: st.move(+1, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::PageUp:   st.move(-rows, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::PageDown: st.move(+rows, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Enter:
        if (n == 0) break;
        if (quick) {
            audio_.play(HudCue::Select);
            applyQuick(items[static_cast<size_t>(st.sel)].id);
        } else {
            const int id = items[static_cast<size_t>(st.sel)].id;
            if (id != MenuSoundToggle && id != MenuSoundDown && id != MenuSoundUp) {
                audio_.play(HudCue::Select);
            }
            applyMenu(id);
        }
        break;
    case Key::Escape:
        audio_.play(HudCue::Back);
        if (quick) {
            setScreen(Screen::Menu);
        } else if (menuPage_ != MenuPage::Root) {
            openMenuPage(MenuPage::Root);
        } else {
            setScreen(Screen::Terminal);
        }
        break;
    default: break;
    }
}

void App::onFilesKey(const KeyEvent& e) {
    const int rows = bodyRows(false);
    const int n = static_cast<int>(files_.size());
    switch (e.key) {
    case Key::Up:   fileList_.move(-1, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Down: fileList_.move(+1, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::PageUp:   fileList_.move(-rows, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::PageDown: fileList_.move(+rows, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Enter:
        audio_.play(HudCue::Select);
        if (n > 0) restoreFile(files_[static_cast<size_t>(fileList_.sel)]);
        break;
    case Key::Escape:
        audio_.play(HudCue::Back);
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

void App::onDiagnosticsKey(const KeyEvent& e) {
    const int rows = std::max(1, bodyRows(false) - 4);
    const int n = static_cast<int>(diagnosticReport_.findings.size());

    if (diagnosticRunning_) {
        if (e.key == Key::Escape && !e.repeat) {
            session_.cancelJob();
            session_.clearFinishedJob();
            diagnosticRunning_ = false;
            diagnosticError_ = "cancelled by operator";
            diagnosticReport_ = buildDiagnosticReport(
                diagnosticStatus_, diagnosticTasks_, diagnosticVersion_);
            pushLocal("-- field check cancelled --", LineKind::Warn);
            setScreen(Screen::Terminal);
        }
        return;
    }

    switch (e.key) {
    case Key::Up:       diagnosticList_.move(-1, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Down:     diagnosticList_.move(+1, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::PageUp:   diagnosticList_.move(-rows, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::PageDown: diagnosticList_.move(+rows, n, rows); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Escape:
    case Key::Enter:
        audio_.play(HudCue::Back);
        setScreen(Screen::Terminal);
        break;
    case Key::Char:
        if (e.ch == 'r' || e.ch == 'R') runFieldCheck();
        else if (e.ch == 's' || e.ch == 'S') saveFieldCheck();
        else if (e.ch == 'v' || e.ch == 'V') {
            term_.scrollToBottom(bodyRows(true));
            setScreen(Screen::Terminal);
        }
        break;
    default: break;
    }
}

void App::onKeymapKey(const KeyEvent& e) {
    // Everything except Escape is swallowed so the tester can show it.
    if (e.key == Key::Escape && !e.repeat) {
        audio_.play(HudCue::Back);
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
    case Key::Up:       helpScroll_ = std::max(0, helpScroll_ - 1); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Down:     ++helpScroll_; audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::PageUp:   helpScroll_ = std::max(0, helpScroll_ - 8); audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::PageDown: helpScroll_ += 8; audio_.play(HudCue::Navigate); dirty_ = true; break;
    case Key::Escape:
    case Key::Enter:
        audio_.play(HudCue::Back);
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
    case Screen::Diagnostics: onDiagnosticsKey(e); break;
    case Screen::Keymap:   onKeymapKey(e); break;
    case Screen::Help:     onHelpKey(e); break;
    case Screen::About:    onAboutKey(e); break;
    }
}

void App::onAboutKey(const KeyEvent& e) {
    if (!e.repeat && (e.key == Key::Escape || e.key == Key::Enter)) {
        audio_.play(HudCue::Back);
        setScreen(returnScreen_);
    }
}

// --------------------------------------------------------------- main loop

void App::tick(uint64_t now) {
    const uint64_t arrivalsBefore = term_.linesEver();
    session_.poll(now);
    if (term_.linesEver() != arrivalsBefore || session_.busy()) {
        if (term_.following()) term_.scrollToBottom(bodyRows(true));
        dirty_ = true;
    }

    if (session_.awaitingVtxChoice() && !modal_) {
        const VtxStatus& vtx = session_.vtxStatus();
        std::string power = "power level " + std::to_string(vtx.power);
        if (vtx.powerLevels > 0) power += "/" + std::to_string(vtx.powerLevels);
        confirm("Bench VTX guard?",
                vtx.deviceLabel() + " reports " + power + ".\n\n"
                "Try verified pit mode before entering CLI? This is the VTX's purpose-built "
                "bench state; no setting is saved. Unsupported hardware is left unchanged.",
                "Use pit", [this]() { session_.enableVtxBenchGuard(); },
                [this]() { session_.skipVtxBenchGuard(); });
    }

    if (thermalTripPromptPending_ && session_.ready() && !modal_) {
        thermalTripPromptPending_ = false;
        confirm("Arm EXT thermal trip?",
                "Verified: this FC is on EXT USB4 with switched EXT 5 V.\n"
                "Arm a one-shot 80C trip? It records an incident, cuts and verifies EXT 5 V "
                "off, then closes serial without another prompt.\n"
                "Battery power is separate. There is no automatic re-enable.",
                "Arm trip",
                [this]() {
                    std::string error;
                    if (!thermalTrip_.arm(error)) {
                        pushLocal("-- thermal trip not armed: " + error + " --", LineKind::Error);
                        notice("Thermal trip unavailable",
                               error + "\n\nTemperature watch remains warning-only. Unplug USB and "
                                       "battery manually if the stack gets hot.");
                        return;
                    }
                    pushLocal("-- EXT USB4 thermal trip ARMED at 80C --", LineKind::Good);
                    status_ = "EXT USB4 thermal trip armed at 80C";
                    statusUntil_ = nowMs() + 6000;
                },
                [this]() {
                    thermalTrip_.decline();
                    pushLocal("-- EXT cutoff declined; temperature watch is warning-only --",
                              LineKind::Warn);
                });
    }

    if (nextTemperatureCheckMs_ != 0 && now >= nextTemperatureCheckMs_ &&
        session_.ready() && !session_.job().active() && !modal_ &&
        !thermalTripPromptPending_) {
        nextTemperatureCheckMs_ = now + kTemperatureCheckIntervalMs;
        if (!temperatureMonitorStarted_) {
            temperatureMonitorStarted_ = true;
            pushLocal("-- temperature watch: status now, then every 30s while idle --",
                      LineKind::Local);
        }
        if (!session_.startCapture(
                "status", "Watching FC temperature",
                [this](bool ok, const std::string& text) {
                    if (!ok) {
                        status_ = "FC temperature unavailable";
                        statusUntil_ = nowMs() + 3000;
                        return;
                    }
                    int temperatureC = 0;
                    if (!parseCoreTemperatureC(text, temperatureC)) {
                        // Older targets that omit the field should not have
                        // their terminal filled with an unproductive query.
                        nextTemperatureCheckMs_ = 0;
                        status_ = "FC does not report core temperature - watch stopped";
                        statusUntil_ = nowMs() + 5000;
                    }
                })) {
            nextTemperatureCheckMs_ = now + kTemperatureCheckRetryMs;
            status_ = "temperature watch deferred - FC busy";
            statusUntil_ = now + 3000;
        }
    }

    if (session_.coreTemperatureSequence() != lastTemperatureSequence_) {
        lastTemperatureSequence_ = session_.coreTemperatureSequence();
        const int temperatureC = session_.coreTemperatureC();
        status_ = "FC core " + std::to_string(temperatureC) + "C";
        statusUntil_ = now + 5000;
        const int alarmLevel = temperatureC >= 80
                                   ? 2
                                   : temperatureC >= kDefaultCoreTemperatureAlarmC ? 1 : 0;
        if (alarmLevel > temperatureAlarmLevel_) {
            temperatureAlarmLevel_ = alarmLevel;
            temperatureWarningPending_ = true;
            temperatureWarningC_ = temperatureC;
        } else if (temperatureWarningPending_ && temperatureC > temperatureWarningC_) {
            // A different modal can delay the notice. Retain the hottest
            // observed sample instead of presenting a later cooled value.
            temperatureWarningC_ = temperatureC;
        } else if (temperatureC < kDefaultCoreTemperatureAlarmC - 5) {
            temperatureAlarmLevel_ = 0;
        }
        dirty_ = true;
        if (temperatureC >= 80 && thermalTrip_.armed() && !thermalTripAttempted_) {
            performThermalTrip(temperatureC);
            return;
        }
    }

    if (temperatureWarningPending_ && !modal_) {
        temperatureWarningPending_ = false;
        const int temperatureC = temperatureWarningC_;
        temperatureWarningC_ = 0;
        const bool critical = temperatureC >= 80;
        notice(critical ? "CRITICAL FC TEMPERATURE" : "FC temperature warning",
               "FC MCU core is " + std::to_string(temperatureC) +
                   "C. Betaflight's default alarm is 70C.\n\n" +
                   (critical ? "Automatic cutoff is not armed for this connection. " : "") +
                   "This is not a VTX temperature sensor. Stop bench work, unplug FC USB and "
                   "battery power, and let the stack cool. Closing the serial link does not "
                   "remove USB power.",
               critical ? HudCue::Critical : HudCue::Error);
    }

    if (disconnectAfterVtxRestore_ && !session_.connected()) {
        const bool exitAfter = exitAfterVtxRestore_;
        disconnectAfterVtxRestore_ = false;
        exitAfterVtxRestore_ = false;
        finishDisconnect(exitAfter);
        return;
    }

    const JobStatus& job = session_.job();
    if (diagnosticRunning_ && !session_.connected()) {
        diagnosticRunning_ = false;
        diagnosticError_ = "serial link lost during field check";
        diagnosticReport_ = buildDiagnosticReport(
            diagnosticStatus_, diagnosticTasks_, diagnosticVersion_);
        dirty_ = true;
    }
    if (job.finished) {
        const bool wasRestore = job.kind == JobKind::Restore;
        const std::string message = job.message;
        const bool ok = job.ok;
        // A capture reports through its own callback; only a restore or a
        // failure needs a dialog here.
        if (wasRestore || !ok) {
            if (!modal_) {
                notice(ok ? "Restore complete" : "Finished with problems", message,
                       ok ? HudCue::Success : HudCue::Error);
            }
        }
        if (ok && !wasRestore) audio_.play(HudCue::Success);
        pushLocal("-- " + message + " --", ok ? LineKind::Good : LineKind::Warn);
        session_.clearFinishedJob();
    }

    // Act on an unplug once. Unlatched, this re-enumerated /dev, /sys and
    // /dev/serial/by-id on every frame for as long as the terminal stayed up.
    if (session_.linkLost()) {
        if (!linkLossHandled_) {
            linkLossHandled_ = true;
            audio_.play(HudCue::LinkDown);
            refreshPorts();
            status_ = session_.vtxBenchGuardActive()
                          ? "link lost - pit mode clears only when the FC reboots"
                          : "link lost - Esc for the menu to reconnect";
            statusUntil_ = now + 6000;
            dirty_ = true;
        }
    } else {
        linkLossHandled_ = false;
    }
    const std::string audioError = audio_.lastError();
    if (audioError != lastAudioError_) {
        lastAudioError_ = audioError;
        refreshSoundMenu();
        if (!audioError.empty() && soundEnabled_ && !opt_.muteSound) {
            status_ = "HUD audio unavailable: " + audioError;
            statusUntil_ = now + 6000;
        }
    }
    if (!status_.empty() && now > statusUntil_) {
        status_.clear();
        dirty_ = true;
    }
}

int App::run(const Options& opt) {
    std::string error;
    if (!setup(opt, error)) {
        std::fprintf(stderr, "%s: %s\n", kAppName, error.c_str());
        return 1;
    }

    if (!opt.previewDir.empty()) {
        // Render one frame of every screen for host-side inspection. Nothing is
        // connected and no key is synthesised, so this is purely a paint test.
        const struct { Screen screen; MenuPage menuPage; const char* name; } kShots[] = {
            {Screen::Ports, MenuPage::Root, "01-ports"},
            {Screen::Terminal, MenuPage::Root, "02-terminal"},
            {Screen::Menu, MenuPage::Root, "03-menu"},
            {Screen::Menu, MenuPage::FlightController, "04-menu-flight-controller"},
            {Screen::Menu, MenuPage::BackupRestore, "05-menu-backup-restore"},
            {Screen::Menu, MenuPage::ControlsInfo, "06-menu-controls-info"},
            {Screen::Menu, MenuPage::SoundDisplay, "07-menu-sound-display"},
            {Screen::Menu, MenuPage::ConnectionExit, "08-menu-connection-exit"},
            {Screen::Quick, MenuPage::FlightController, "09-quick"},
            {Screen::Files, MenuPage::Root, "10-files"},
            {Screen::Keymap, MenuPage::Root, "11-keymap"},
            {Screen::Help, MenuPage::Root, "12-help"},
            {Screen::Diagnostics, MenuPage::Root, "13-field-check"},
            {Screen::About, MenuPage::Root, "14-about"},
        };
        status_.clear();   // show each screen's real hint bar, not a startup notice
        pushLocal("# Betaflight / STM32G47X (G473) 2026.6.0-alpha MSP API: 1.48", LineKind::Fc);
        pushLocal("Voltage: 4.12V (1S battery - OK)", LineKind::Fc);
        pushLocal("Arming disable flags: RXLOSS CLI", LineKind::Fc);
        pushLocal("-- CLI ready --", LineKind::Good);
        editor_.setText("set gyro_lpf1_static_hz = 0");
        diagnosticStatus_ =
            "GYRO: (1) ICM42688P enabled locked dma\n"
            "DEVICES DETECTED: SPI=2, I2C=1 (0 errors)\n"
            "CPU:37%, cycle time: 125, GYRO rate: 8000, RX rate: 0, System rate: 10\n"
            "Voltage: 4.12V (1S battery - OK)\n"
            "Arming disable flags: RXLOSS CLI\n";
        diagnosticTasks_ = "Task list\nTotal                                             42.5%\n";
        diagnosticVersion_ =
            "# Betaflight / STM32G47X (G473) 2026.6.0-alpha MSP API: 1.48\n";
        diagnosticReport_ = buildDiagnosticReport(
            diagnosticStatus_, diagnosticTasks_, diagnosticVersion_);
        int written = 0;
        for (const auto& shot : kShots) {
            screen_ = shot.screen;
            if (shot.screen == Screen::Menu) menuPage_ = shot.menuPage;
            render();
            const std::string path = opt.previewDir + "/" + shot.name + ".ppm";
            if (display_.canvas().writePpm(path)) ++written;
        }
        // One more with a confirmation dialog up, since that is its own layout.
        screen_ = Screen::Terminal;
        confirm("Props off?", "Spins a motor. Props off?\n\n> motor 1 1100", "Send", nullptr);
        render();
        if (display_.canvas().writePpm(opt.previewDir + "/15-confirm.ppm")) ++written;
        closeModal();
        confirm("Bench VTX guard?",
                "SmartAudio reports power level 3/4.\n\nTry verified pit mode before entering "
                "CLI? This is the VTX's purpose-built bench state; no setting is saved. "
                "Unsupported hardware is left unchanged.",
                "Use pit", nullptr);
        render();
        if (display_.canvas().writePpm(opt.previewDir + "/16-vtx-guard.ppm")) ++written;
        closeModal();
        notice("CRITICAL FC TEMPERATURE",
               "FC MCU core is 82C. Betaflight's default alarm is 70C.\n\n"
               "This is not a VTX temperature sensor. Stop bench work, unplug FC USB and "
               "battery power, and let the stack cool. Closing the serial link does not "
               "remove USB power.");
        render();
        if (display_.canvas().writePpm(opt.previewDir + "/17-temperature-warning.ppm")) ++written;
        closeModal();
        confirm("Restore VTX state?",
                "Pit mode is active for bench work. Restore the saved flight state?\n\n"
                "Restore exits CLI, discards unsaved CLI changes, and reboots the FC. Cancel "
                "closes the link and leaves pit mode active until the FC is rebooted or "
                "power-cycled.",
                "Restore", nullptr);
        render();
        if (display_.canvas().writePpm(opt.previewDir + "/18-vtx-restore.ppm")) ++written;
        closeModal();
        std::printf("wrote %d previews to %s\n", written, opt.previewDir.c_str());
        teardown();
        return written > 0 ? 0 : 1;
    }

    audio_.play(HudCue::Startup);

    SimFc sim;
    if (opt.simulate && !opt.showAbout) {
        std::string simErr;
        if (!sim.start(simErr)) {
            std::fprintf(stderr, "%s: simulator: %s\n", kAppName, simErr.c_str());
            teardown();
            return 1;
        }
        std::string cerr;
        pushLocal("simulated flight controller on " + sim.devicePath(), LineKind::Local);
        session_.connect(sim.devicePath(), 115200, cerr);
        audio_.play(HudCue::LinkUp);
        beginConnectionSafety(sim.devicePath());
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
