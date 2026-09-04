#include "app.h"
#include "brand.h"
#include "simfc.h"
#include "simmesh.h"
#include "strutil.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
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
    MenuMeshNodes,
    MenuMeshBroadcast,
    MenuMeshRadioInfo,
    MenuMeshExport,
    MenuMeshClear,
    MenuGnssToggle,
    MenuGnssStatus,
    MenuMeshShare,
    MenuFcBaud,
    MenuMeshBaud,
    MenuGnssBaud,
    MenuMeshQuickMsg,
    MenuMeshSos,
    MenuMarkHere,
    MenuMarks,
    MenuAutoShare,
    MenuCompass,
    MenuOpenFlightController = 100,
    MenuOpenBackupRestore,
    MenuOpenMesh,
    MenuOpenMeshPosition,
    MenuOpenControlsInfo,
    MenuOpenSoundDisplay,
    MenuOpenConnectionExit,
    MenuOpenLinkSpeeds,
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
// How long the receiver probe listens at the saved rate before deciding, and
// at each further rate once the wire has proved it carries something.
constexpr uint64_t kGnssProbeWindowMs = 6000;
constexpr uint64_t kGnssRetryWindowMs = 3000;
// A wire that produced this much readable non-NMEA text is a console, not a
// receiver at the wrong rate, and the rate table is not walked for it.
constexpr int kGnssConsoleLines = 3;

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

void App::setupMenus() {
    controllerMenu_ = {
        {"Run field check", "no config writes; status, blockers, runtime", MenuFieldCheck, true},
        {"Quick commands", "common CLI queries", MenuQuick, true},
    };
    backupMenu_ = {
        {"Backup config to file", "runs `diff all`", MenuBackupDiff, true},
        {"Full dump to file", "runs `dump all`", MenuBackupDump, true},
        {"Restore from backup", "sends a saved file", MenuRestore, true},
        {"Saved backups", "view or delete", MenuFiles, true},
    };
    meshMenu_ = {
        {"Node list", "radios this device has heard", MenuMeshNodes, true},
        {"Broadcast channel", "message everyone on the channel", MenuMeshBroadcast, true},
        {"Quick messages", "canned lines for the open conversation", MenuMeshQuickMsg, true},
        {"Radio info", "firmware, region, preset, channel", MenuMeshRadioInfo, true},
        {"Export conversation", "write the open chat to a text file", MenuMeshExport, true},
        {"Clear conversation", "delete the open chat history", MenuMeshClear, true},
    };
    meshPositionMenu_ = {
        {"GNSS receiver", "NMEA on the Grove/EXT UART: the cap's, or a GPS unit",
         MenuGnssToggle, true},
        {"GNSS status", "fix, satellites, and coordinates", MenuGnssStatus, true},
        {"Compass", "BMM150 heading, calibration, alignment", MenuCompass, true},
        {"Share my position", "transmits this station's own fix", MenuMeshShare, true},
        {"Auto-share position: OFF", "session only; off again at the next launch",
         MenuAutoShare, true},
        {"SOS broadcast", "help request with your fix, to everyone", MenuMeshSos, true},
        {"Mark this spot", "save your fix as a place to walk back to", MenuMarkHere, true},
        {"Marks", "saved places: locate or delete", MenuMarks, true},
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
        {"Baud rates", "one line rate per peer, remembered", MenuOpenLinkSpeeds, true},
        {"Disconnect", "restore bench VTX state or close link", MenuDisconnect, true},
        {"Exit GNDHOG ZERO", "return to the launcher", MenuExit, true},
    };
    // Three peers, three rates. USB CDC negotiates its own and ignores these;
    // the Grove and EXT UARTs do not, which is where a wrong rate turns into
    // a port that opens perfectly and then says nothing at all.
    linkSpeedMenu_ = {
        {"Flight controller", "", MenuFcBaud, true},
        {"Meshtastic radio", "", MenuMeshBaud, true},
        {"GNSS receiver", "", MenuGnssBaud, true},
    };
    refreshLinkSpeedMenu();
    refreshMeshMenus();
    menuPage_ = MenuPage::Root;
    menuList_ = ListState{};
    submenuList_ = ListState{};

    quick_.clear();
    for (int i = 0; i < kQuickCount; ++i) {
        quick_.push_back(MenuItem{kQuick[i].label, kQuick[i].hint, i, true});
    }
    quickList_ = ListState{};
}

// The root menu keeps five categories in both link modes. Which five depends on
// what is actually plugged in: a Betaflight page on a mesh radio would be five
// dead ends wearing a flight controller's clothes.
void App::refreshMeshMenus() {
    if (meshMode()) {
        menu_ = {
            {"Mesh network", "nodes, messages, and radio identity", MenuOpenMesh, true},
            {"Position & GNSS", "fix, sharing, marks, and SOS", MenuOpenMeshPosition, true},
            {"Controls & info", "keymap, help, and build identity", MenuOpenControlsInfo, true},
            {"Sound & display", "HUD audio, volume, and brightness", MenuOpenSoundDisplay, true},
            {"Connection & exit", "close the radio link or return to launcher",
             MenuOpenConnectionExit, true},
        };
    } else {
        menu_ = {
            {"Flight controller", "checks and common CLI queries", MenuOpenFlightController, true},
            {"Backup & restore", "save, inspect, or restore configuration",
             MenuOpenBackupRestore, true},
            {"Controls & info", "keymap, help, and build identity", MenuOpenControlsInfo, true},
            {"Sound & display", "HUD audio, volume, and brightness", MenuOpenSoundDisplay, true},
            {"Connection & exit", "close the FC link or return to launcher",
             MenuOpenConnectionExit, true},
        };
    }

    for (MenuItem& item : meshPositionMenu_) {
        if (item.id == MenuGnssToggle) {
            item.label = opt_.gnssEnabled
                             ? std::string("GNSS receiver: ") + (gnssWanted_ ? "ON" : "OFF")
                             : std::string("GNSS receiver: OFF (--no-gnss)");
            if (!opt_.gnssEnabled) item.hint = "session override; restart without --no-gnss";
            else if (!gnssWanted_) item.hint = "receiver not opened this session";
            else if (!gnss_.isOpen()) item.hint = "no receiver on " + gnssDevice_;
            else if (!gnss_.receiverPresent()) item.hint = "open, waiting for NMEA";
            else if (gnss_.fix().valid) item.hint = gnss_.fix().coordText();
            else item.hint = "receiver present, searching for a fix";
        } else if (item.id == MenuAutoShare) {
            item.label = autoShareMinutes_ > 0
                             ? "Auto-share position: " + std::to_string(autoShareMinutes_) +
                                   " min"
                             : std::string("Auto-share position: OFF");
            item.hint = autoShareMinutes_ > 0
                            ? "Enter: 2 > 5 > 15 min > off; never saved"
                            : "session only; off again at the next launch";
        } else if (item.id == MenuMarks) {
            item.hint = marks_.empty()
                            ? std::string("no saved places yet")
                            : std::to_string(marks_.size()) + " saved: locate or delete";
        } else if (item.id == MenuCompass) {
            if (!compass_.available()) {
                item.label = "Compass: none found";
                item.hint = "no IIO magnetometer on this machine";
            } else if (!compass_.reading().valid) {
                item.label = "Compass: " + compass_.magnetometerName();
                item.hint = "open it to read a heading";
            } else {
                item.label = "Compass: " + formatHeading(compass_.reading().headingDeg) +
                             (compass_.calibration().hardIron ? "" : " uncalibrated");
                item.hint = compass_.calibration().hardIron
                                ? (compass_.calibration().aligned
                                       ? "calibrated and aligned; C or A redo either"
                                       : "calibrated; A while walking aligns forward")
                                : "C on the compass screen calibrates it";
            }
        }
    }
    for (MenuItem& item : meshMenu_) {
        if (item.id == MenuMeshBroadcast && !mesh_.radio().primaryChannel.empty()) {
            item.hint = "channel " + mesh_.radio().primaryChannel;
        } else if (item.id == MenuMeshQuickMsg) {
            item.hint = "sends to " + peerTitle(chatPeer_);
        }
    }
    dirty_ = true;
}

std::vector<MenuItem>& App::currentMenuItems() {
    switch (menuPage_) {
    case MenuPage::FlightController: return controllerMenu_;
    case MenuPage::BackupRestore:    return backupMenu_;
    case MenuPage::Mesh:             return meshMenu_;
    case MenuPage::MeshPosition:     return meshPositionMenu_;
    case MenuPage::ControlsInfo:     return controlsMenu_;
    case MenuPage::SoundDisplay:     return settingsMenu_;
    case MenuPage::ConnectionExit:   return connectionMenu_;
    case MenuPage::LinkSpeeds:       return linkSpeedMenu_;
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
    currentMenuList().clamp(static_cast<int>(currentMenuItems().size()), bodyRows(false));
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
    if (screen_ == Screen::Menu) {
        refreshSoundMenu();
        refreshLinkSpeedMenu();
        refreshMeshMenus();
    }
    // A key held across a screen change must not act on the new screen.
    keyboard_.releaseAll();
    dirty_ = true;
}

void App::openReturnableScreen(Screen s) {
    // Rerunning an auxiliary screen (notably Field Check) must retain the
    // original caller instead of making the screen return to itself.
    if (screen_ != s) returnScreen_ = screen_;
    setScreen(s);
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

void App::prompt(const std::string& title, const std::string& body,
                 const std::string& initial, size_t maxBytes,
                 std::function<void(const std::string&)> onSubmit) {
    modal_ = true;
    modalIsConfirm_ = false;
    modalIsInput_ = true;
    modalTitle_ = title;
    modalBody_ = body;
    modalYes_.clear();
    modalAction_ = nullptr;
    modalCancelAction_ = nullptr;
    modalInputAction_ = std::move(onSubmit);
    modalInputMax_ = maxBytes;
    modalEditor_.setText(initial.size() > maxBytes ? initial.substr(0, maxBytes) : initial);
    keyboard_.releaseAll();
    audio_.play(HudCue::Prompt);
    dirty_ = true;
}

void App::closeModal() {
    modal_ = false;
    modalIsConfirm_ = false;
    modalIsInput_ = false;
    modalAction_ = nullptr;
    modalCancelAction_ = nullptr;
    modalInputAction_ = nullptr;
    modalEditor_.clear();
    keyboard_.releaseAll();
    dirty_ = true;
}

// ------------------------------------------------------------ shared moves

void App::showStatus(const std::string& text, uint64_t ms) {
    status_ = text;
    statusUntil_ = nowMs() + ms;
    dirty_ = true;
}

void App::openRootMenu() {
    openMenuPage(MenuPage::Root);
    menuList_.clamp(static_cast<int>(menu_.size()), bodyRows(false));
    setScreen(Screen::Menu);
}

void App::openHelp() {
    helpScroll_ = 0;
    openReturnableScreen(Screen::Help);
}

void App::openMarks() {
    markList_.clamp(static_cast<int>(marks_.size()), bodyRows(false));
    openReturnableScreen(Screen::Marks);
}

bool App::navigateList(ListState& st, const KeyEvent& e, int count, int rows) {
    int delta = 0;
    switch (e.key) {
    case Key::Up:       delta = -1; break;
    case Key::Down:     delta = +1; break;
    case Key::PageUp:   delta = -rows; break;
    case Key::PageDown: delta = +rows; break;
    default: return false;
    }
    st.move(delta, count, rows);
    audio_.play(HudCue::Navigate);
    dirty_ = true;
    return true;
}

bool App::requireFcReady() {
    if (session_.ready()) return true;
    notice("Not ready", "Connect to a flight controller first.");
    return false;
}

bool App::requireRadioReady() {
    if (meshMode() && mesh_.ready()) return true;
    notice("Not ready", "Connect a Meshtastic radio first.");
    return false;
}

bool App::marksFull() {
    if (marks_.size() < kMaxMarks) return false;
    notice("Marks full",
           std::to_string(kMaxMarks) + " places are saved already. Delete one from "
           "Menu > Position & GNSS > Marks before adding another.");
    return true;
}

void App::abortFieldCheck(const std::string& why) {
    diagnosticRunning_ = false;
    diagnosticError_ = why;
    diagnosticReport_ = buildDiagnosticReport(
        diagnosticStatus_, diagnosticTasks_, diagnosticVersion_);
    dirty_ = true;
}

void App::stopGnss() {
    gnss_.close();
    gnssProbeDeadlineMs_ = 0;
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
    loadMarks();
    quickMessages_ = loadQuickMessages(config_);

    soundVolume_ = std::clamp(config_.getInt("sound.volume", 70), 0, 100);
    soundEnabled_ = config_.getBool("sound.enabled", true) && !opt_.muteSound;
    audio_.setVolume(soundVolume_);
    audio_.setEnabled(soundEnabled_);
    audio_.start();
    lastAudioError_ = audio_.lastError();

    // Sym-layer corrections, e.g. `sym.0x28 = _`.
    for (const auto& kv : config_.all()) {
        if (!startsWith(kv.first, "sym.") || kv.second.empty()) continue;
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
            showStatus("framebuffer: " + dispErr, 6000);
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
            if (status_.empty()) showStatus("keyboard: " + kbErr + " (using stdin)", 6000);
        }
    } else {
        keyboard_.enableStdinFallback(true);
    }

    term_.setWidth(columns());
    editor_.loadHistory(storage_.loadHistory());

    // The receiver speaks NMEA on the Grove/EXT UART, which on this board is
    // /dev/serial0 whether the cap's AT6668 or a GPS unit on the Grove socket
    // is driving it; the config key exists because a USB receiver is somebody
    // else's Tuesday.
    gnssDevice_ = opt_.gnssDevice.empty() ? config_.get("gnss.device", "/dev/serial0")
                                          : opt_.gnssDevice;
    // A rate on the command line wins for this launch; otherwise the saved one
    // does. A hand-edited config that names a rate this libc has no termios
    // constant for is refused here rather than at open time, where it would
    // look like the peer had gone quiet.
    gnssBaud_ = opt_.gnssBaudSet ? opt_.gnssBaud : config_.getInt("gnss.baud", 115200);
    if (!isSupportedBaud(gnssBaud_)) gnssBaud_ = 115200;
    fcBaud_ = opt_.fcBaudSet ? opt_.fcBaud : config_.getInt("fc.baud", 115200);
    if (!isSupportedBaud(fcBaud_)) fcBaud_ = 115200;
    meshBaud_ = opt_.meshBaudSet ? opt_.meshBaud : config_.getInt("mesh.baud", 115200);
    if (!isSupportedBaud(meshBaud_)) meshBaud_ = 115200;
    // --gnss DEV names a receiver on purpose, so it is opened this launch even
    // if the saved switch is off: on the bench that switch was off, and the
    // flag was found doing nothing at all. Like --no-gnss it never rewrites
    // the saved choice; teardown knows.
    gnssWanted_ = opt_.gnssEnabled &&
                  (!opt_.gnssDevice.empty() || config_.getBool("gnss.enabled", true));

    // The pack is the operator's own clock in the field, so the indicator is
    // wired up before anything is connected. No gauge is a normal outcome.
    battery_.discover();
    battery_.poll(nowMs());
    // The magnetometer is read only when a screen will show the heading.
    // Discovery is a directory listing; absence is the host build's normal.
    compass_.discover();
    loadCompassCalibration();

    setupMenus();
    refreshSoundMenu();

    refreshPorts();
    refreshFiles();

    if (opt_.showAbout) {
        openReturnableScreen(Screen::About);
    } else if (!opt_.portOverride.empty()) {
        PortInfo forced;
        forced.device = opt_.portOverride;
        // Unknown to the scan means the operator named it deliberately, so the
        // baud they asked for is the baud they get.
        forced.kind = "uart";
        for (const PortInfo& p : ports_) {
            if (p.device == opt_.portOverride) forced = p;
        }
        const bool asMesh = opt_.forceMesh || forced.prefersMeshtastic();
        connectPort(forced, asMesh ? LinkMode::Meshtastic : LinkMode::Betaflight);
    } else if (opt_.autoConnect && !ports_.empty()) {
        // Auto-connect only on a strong identity claim. A generic USB bridge
        // gets the picker, not a protocol guess made on its behalf.
        const PortInfo& first = ports_.front();
        if (first.looksLikeFlightController() || first.meshScore >= 90) {
            portList_.sel = 0;
            portLinkModeForced_ = false;
            syncPortLinkMode();
            connectSelected();
        }
    }
    // The cap's receiver is this application's own peer, not a radio's. It used
    // to be opened only once a Meshtastic link was up, and on the bench there
    // was never going to be one: the only UART on the board was the AT6668.
    startGnss(true);
    return true;
}

void App::teardown() {
    storage_.saveHistory(editor_.history());
    flushMeshChats();
    config_.setInt("brightness", brightness_);
    if (!opt_.muteSound) config_.setBool("sound.enabled", soundEnabled_);
    config_.setInt("sound.volume", soundVolume_);
    // --no-gnss and --gnss DEV are launch-wide overrides, exactly like --mute:
    // they change this session and must not rewrite the saved choice. One
    // smoke test with --no-gnss should not disable the receiver forever, and
    // one launch that named a receiver should not switch it on forever. A
    // deliberate menu toggle saves itself as it happens, so nothing is lost.
    if (opt_.gnssEnabled && opt_.gnssDevice.empty()) {
        config_.setBool("gnss.enabled", gnssWanted_);
    }
    if (opt_.gnssDevice.empty()) config_.set("gnss.device", gnssDevice_);
    // Same contract as --mute and --no-gnss: a rate handed in on the command
    // line is this launch's business and never becomes the saved default.
    if (!opt_.gnssBaudSet) config_.setInt("gnss.baud", gnssBaud_);
    if (!opt_.fcBaudSet) config_.setInt("fc.baud", fcBaud_);
    if (!opt_.meshBaudSet) config_.setInt("mesh.baud", meshBaud_);
    std::string err;
    config_.save(storage_, err);
    mesh_.disconnect();
    gnss_.close();
    session_.disconnect();
    audio_.shutdown();
    keyboard_.close();
    display_.close();
}

// ------------------------------------------------------------------ actions

void App::refreshPorts() {
    ports_ = enumeratePorts();
    portList_.clamp(static_cast<int>(ports_.size()), bodyRows(false));
    syncPortLinkMode();
    dirty_ = true;
}

// The picker proposes the protocol the port's own USB identity argues for. An
// operator override sticks until the picker is left, because a hand-flashed
// board can present any identity it likes.
void App::syncPortLinkMode() {
    if (portLinkModeForced_) return;
    if (ports_.empty()) {
        portLinkMode_ = LinkMode::Betaflight;
        return;
    }
    const PortInfo& port = ports_[static_cast<size_t>(portList_.sel)];
    portLinkMode_ = port.prefersMeshtastic() ? LinkMode::Meshtastic : LinkMode::Betaflight;
}

void App::refreshFiles() {
    files_ = storage_.listBackups();
    fileList_.clamp(static_cast<int>(files_.size()), bodyRows(false));
    dirty_ = true;
}

// ------------------------------------------------------------------- mesh

bool App::linkConnected() const {
    return meshMode() ? mesh_.connected() : session_.connected();
}

int App::nodeRowCount() const {
    return 1 + static_cast<int>(mesh_.nodes().size());
}

uint32_t App::peerForNodeRow(int row) const {
    if (row <= 0) return kMeshBroadcast;
    const std::vector<MeshNode>& nodes = mesh_.nodes();
    const size_t index = static_cast<size_t>(row - 1);
    if (index >= nodes.size()) return kMeshBroadcast;
    return nodes[index].num;
}

std::string App::peerTitle(uint32_t peer) const {
    if (peer == kMeshBroadcast) {
        const std::string& channel = mesh_.radio().primaryChannel;
        return channel.empty() ? "Broadcast" : "Broadcast / " + channel;
    }
    const MeshNode* node = mesh_.findNode(peer);
    return node ? node->title() : meshNodeIdText(peer);
}

void App::loadMeshChats() {
    for (const std::string& name : storage_.listMeshChatFiles()) {
        uint32_t peer = 0;
        if (!meshChatPeerFromFileName(name, peer)) continue;
        std::string text, error;
        if (!storage_.readFile(storage_.meshDir() + "/" + name, text, error)) continue;
        mesh_.adoptConversation(peer, parseMeshChat(text));
    }
    chatSequenceSeen_ = mesh_.chatSequence();
}

void App::flushMeshChats() {
    for (uint32_t peer : mesh_.takeDirtyPeers()) {
        const std::vector<MeshMessage>* log = mesh_.conversation(peer);
        const std::string path = storage_.meshDir() + "/" + meshChatFileName(peer);
        std::string error;
        if (!log || log->empty()) continue;
        if (!storage_.writeAtomic(path, formatMeshChat(*log), error)) {
            // Losing the transcript is worth saying out loud; the message
            // itself already went out over the air either way.
            showStatus("chat not saved: " + error, 6000);
        }
    }
}

void App::startGnss(bool quiet) {
    if (!gnssWanted_ || gnss_.isOpen()) return;
    if (gnssDevice_.empty()) return;
    // The cap's receiver and a Grove-wired peer can be the same device node.
    // Whoever the operator deliberately connected keeps it.
    const std::string linkDevice = meshMode() ? mesh_.device() : session_.device();
    if (linkConnected() && sameDeviceNode(linkDevice, gnssDevice_)) {
        pushLocal("-- GNSS skipped: " + gnssDevice_ + " is the link port --", LineKind::Warn);
        return;
    }
    std::string error;
    if (!gnss_.open(gnssDevice_, gnssBaud_, error)) {
        // A node that is not there is a development host. A node that is there
        // and would not open is worth a line even at launch.
        if (!quiet || ::access(gnssDevice_.c_str(), F_OK) == 0) {
            pushLocal("-- no GNSS receiver on " + gnssDevice_ + ": " + error + " --",
                      LineKind::Warn);
        }
        gnssProbeDeadlineMs_ = 0;
        return;
    }
    gnssProbeDeadlineMs_ = nowMs() + kGnssProbeWindowMs;
    gnssProbeReported_ = false;
    gnssProbeFirstBaud_ = gnssBaud_;
    gnssProbeTried_.assign(1, gnssBaud_);
    pushLocal("-- listening for a GNSS receiver on " + gnssDevice_ + " @ " +
                  std::to_string(gnssBaud_) + " (NMEA 0183) --",
              LineKind::Local);
}

bool App::isGnssPort(const std::string& device) const {
    return gnss_.isOpen() && sameDeviceNode(gnss_.device(), device);
}

void App::toggleGnss() {
    if (!opt_.gnssEnabled) {
        // The same contract --mute has: a launch-wide override the menu cannot
        // quietly undo, and which says so rather than appearing to do nothing.
        gnssWanted_ = false;
        gnss_.close();
        showStatus("GNSS locked off by --no-gnss for this launch", 4000);
        refreshMeshMenus();
        return;
    }
    gnssWanted_ = !gnssWanted_;
    config_.setBool("gnss.enabled", gnssWanted_);
    if (gnssWanted_) {
        startGnss();
        showStatus(gnss_.isOpen() ? "GNSS receiver opened" : "GNSS receiver unavailable", 4000);
    } else {
        stopGnss();
        showStatus("GNSS receiver closed", 4000);
    }
    refreshMeshMenus();
}

void App::showGnssStatus() {
    const GnssFix& fix = gnss_.fix();
    std::string body;
    if (!gnssWanted_) {
        body = "The GNSS receiver is switched off for this session.";
    } else if (!gnss_.isOpen()) {
        body = "No receiver is open on " + gnssDevice_ + ".\n\n" +
               (gnss_.lastError().empty()
                    ? std::string("Fit a Cap LoRa-1262 GPS, or a GPS unit on the Grove "
                                  "socket, and restart the probe from this menu.")
                    : gnss_.lastError());
    } else if (!gnss_.receiverPresent()) {
        body = gnssDevice_ + " opened at " + std::to_string(gnssBaud_) +
               " but has sent no NMEA.\n\n"
               "That UART exists whether or not a receiver is wired to it, so this "
               "is reported as absent rather than as a receiver with no fix.";
    } else {
        body = "Receiver: " + gnssDevice_ + " @ " + std::to_string(gnssBaud_) + "\n" +
               "Sentences: " + std::to_string(gnss_.sentenceCount()) + "\n" +
               "Fix: " + (fix.valid ? "yes" : (fix.everValid ? "lost" : "searching")) + "\n" +
               "Position: " + fix.coordText() + "\n" +
               "Satellites: " + std::to_string(fix.satellitesUsed) + " used, " +
               std::to_string(fix.satellitesInView) + " in view";
        if (fix.haveAltitude) {
            body += "\nAltitude: " + std::to_string(static_cast<int>(fix.altitudeM)) + " m";
        }
        if (!fix.utc.empty()) body += "\nUTC: " + fix.utc;
        if (!fix.receiverText.empty()) body += "\nReceiver says: " + fix.receiverText;
    }
    notice("GNSS receiver", body, HudCue::Select);
}

void App::showRadioInfo() {
    const MeshRadioInfo& radio = mesh_.radio();
    if (!meshMode() || !mesh_.connected()) {
        notice("No radio", "Connect a Meshtastic radio first.");
        return;
    }
    std::string body =
        "Node: " + meshNodeIdText(radio.myNodeNum) + "\n" +
        "Firmware: " + (radio.firmwareVersion.empty() ? "not reported" : radio.firmwareVersion) +
        "\nRole: " + meshRoleName(radio.role) + "\n" +
        "LoRa: " + radio.loraSummary() + "\n" +
        "Channel: " + (radio.primaryChannel.empty() ? "not reported" : radio.primaryChannel) +
        "\nNodes heard: " + std::to_string(mesh_.nodes().size()) + "\n" +
        "Radio GNSS: " + (radio.havePositionConfig ? meshGpsModeName(radio.gpsMode)
                                                   : "not reported");
    notice("Meshtastic radio", body, HudCue::Select);
}

void App::openChat(uint32_t peer) {
    chatPeer_ = peer;
    chatEditor_.clear();
    chatRowsValid_ = false;
    chatFollow_ = true;
    chatScroll_ = 0;
    mesh_.markRead(peer);
    meshUnreadSeen_ = mesh_.totalUnread();
    openReturnableScreen(Screen::Chat);
}

void App::submitChatLine() {
    const std::string text = chatEditor_.text();
    if (text.empty()) return;
    std::string error;
    if (!mesh_.sendText(chatPeer_, text, error)) {
        notice("Not sent", error, HudCue::Error);
        return;
    }
    chatEditor_.commit();
    chatFollow_ = true;
    chatRowsValid_ = false;
    dirty_ = true;
}

void App::shareMyPosition(uint32_t peer) {
    if (!requireRadioReady()) return;
    if (!gnss_.receiverPresent()) {
        notice("No GNSS receiver",
               "This station has no GNSS receiver reporting on " + gnssDevice_ + ".\n\n"
               "Position sharing needs a receiver of its own: the Cap LoRa-1262 GPS, "
               "or a GPS unit on the Grove socket. The radio's own position is its "
               "business, not this application's.");
        return;
    }
    if (!gnss_.fix().valid) {
        notice("No fix yet",
               "The receiver is present but has no current fix.\n\n" +
                   gnss_.statusText(nowMs()) +
                   "\n\nGNDHOG will not transmit a stale or invented coordinate.");
        return;
    }
    confirm("Transmit position?",
            "Send this station's own GNSS fix to " + peerTitle(peer) + "?\n\n" +
                gnss_.fix().coordText() + "\n\n"
                "This keys the radio. Everyone who can decrypt the channel can read it.",
            "Send", [this, peer]() {
                std::string error;
                if (!mesh_.sendPosition(peer, gnss_.fix(), error)) {
                    notice("Not sent", error, HudCue::Error);
                    return;
                }
                showStatus("position transmitted", 4000);
            });
}

void App::exportConversation() {
    const std::vector<MeshMessage>* log = mesh_.conversation(chatPeer_);
    if (!log || log->empty()) {
        notice("Nothing to export", "That conversation has no messages yet.");
        return;
    }
    std::ostringstream out;
    out << "GNDHOG ZERO mesh conversation\n"
        << "peer: " << peerTitle(chatPeer_) << " (" << meshNodeIdText(chatPeer_) << ")\n"
        << "radio: " << meshNodeIdText(mesh_.radio().myNodeNum) << "  "
        << mesh_.radio().loraSummary() << "\n"
        << "exported: " << timestampCompact() << "\n\n";
    for (const MeshMessage& message : *log) {
        const MeshNode* from = mesh_.findNode(message.from);
        out << formatLocalTime(message.stampUtc, "%Y-%m-%d %H:%M:%S") << "  "
            << (message.outgoing ? "this station"
                                 : (from ? from->title() : meshNodeIdText(message.from)))
            << ": " << message.text;
        if (!message.note.empty()) out << "   [" << message.note << "]";
        out << "\n";
    }
    const std::string name = "chat_" +
                             (chatPeer_ == kMeshBroadcast ? std::string("broadcast")
                                                          : meshNodeIdText(chatPeer_).substr(1)) +
                             "_" + timestampCompact() + ".txt";
    const std::string path = storage_.meshDir() + "/" + name;
    std::string error;
    if (!storage_.writeAtomic(path, out.str(), error)) {
        notice("Could not save", error);
        return;
    }
    notice("Conversation exported", name + "\n\n" + storage_.meshDir(), HudCue::Success);
}

void App::clearConversation() {
    const std::vector<MeshMessage>* log = mesh_.conversation(chatPeer_);
    if (!log || log->empty()) {
        notice("Nothing to clear", "That conversation has no messages yet.");
        return;
    }
    const uint32_t peer = chatPeer_;
    const std::string title = peerTitle(peer);
    confirm("Clear conversation",
            title + "\n\n" + std::to_string(log->size()) +
                " message(s) are deleted from this device. Nothing is recalled from "
                "the mesh, and the other station keeps its own copy.",
            "Delete", [this, peer]() {
                mesh_.clearConversation(peer);
                std::string error;
                const std::string path = storage_.meshDir() + "/" + meshChatFileName(peer);
                storage_.deleteMeshChat(path, error);
                // The dirty list is deliberately left alone: another peer may
                // have an unwritten message in it, and this is not its problem.
                chatRowsValid_ = false;
                chatSequenceSeen_ = mesh_.chatSequence();
                showStatus("conversation cleared");
            });
}

void App::beginMeshSession() {
    mesh_.clearConversations();
    mesh_.takeDirtyPeers();
    loadMeshChats();
    nodeList_ = ListState{};
    chatPeer_ = kMeshBroadcast;
    chatRowsValid_ = false;
    chatEditor_.clear();
    nodeSequenceSeen_ = mesh_.nodeSequence();
    meshNoteSeen_ = mesh_.noteSequence();
    meshUnreadSeen_ = mesh_.totalUnread();
    meshFailureReported_ = false;
    startGnss();
    refreshMeshMenus();
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
    // A copy: connecting can rescan and reorder the vector under us.
    const PortInfo port = ports_[static_cast<size_t>(portList_.sel)];
    if (isDfuId(port.vendorId, port.productId)) {
        notice("DFU mode",
               "This device is in the bootloader, not the CLI. Power-cycle the "
               "flight controller and reconnect.");
        return;
    }
    connectPort(port, portLinkMode_);
}

void App::connectPort(const PortInfo& port, LinkMode mode) {
    const int baud = linkBaud(mode);
    std::string error;

    // A UART that has been proving itself with NMEA is the cap's receiver.
    // Opened as a link it would feed sentences to a CLI or, as the bench
    // showed, scroll them through a radio log as console chatter until the
    // config request had timed out three times. Show the receiver instead.
    if (isGnssPort(port.device)) {
        if (gnss_.receiverPresent()) {
            pushLocal("-- " + port.device + " is the GNSS receiver (" +
                          std::to_string(gnss_.sentenceCount()) +
                          " NMEA sentences); not opening it as a link --",
                      LineKind::Warn);
            showGnssStatus();
            return;
        }
        // Still probing and nothing has spoken: the operator's deliberate
        // choice of this port wins, and the probe starts over when the link
        // closes.
        stopGnss();
        pushLocal("-- GNSS probe released " + port.device + " for the link --",
                  LineKind::Local);
    }

    if (mode == LinkMode::Meshtastic) {
        // USB CDC ignores the line rate; a cap or Grove-wired radio does not.
        const int meshBaud = port.kind == "uart" ? baud : 115200;
        pushLocal("connecting to " + port.device + " as a Meshtastic radio" +
                      (port.kind == "uart" ? " @ " + std::to_string(meshBaud) : ""),
                  LineKind::Local);
        if (!mesh_.connect(port.device, meshBaud, error)) {
            pushLocal("connect failed: " + error, LineKind::Error);
            notice("Could not open port", error, HudCue::Error);
            return;
        }
        linkMode_ = LinkMode::Meshtastic;
        audio_.play(HudCue::LinkUp);
        beginMeshSession();
        setScreen(Screen::Nodes);
        return;
    }

    pushLocal("connecting to " + port.device + (port.kind == "uart"
                                                    ? " @ " + std::to_string(baud)
                                                    : ""),
              LineKind::Local);
    if (!session_.connect(port.device, baud, error)) {
        pushLocal("connect failed: " + error, LineKind::Error);
        notice("Could not open port", error, HudCue::Error);
        return;
    }
    linkMode_ = LinkMode::Betaflight;
    refreshMeshMenus();
    audio_.play(HudCue::LinkUp);
    beginConnectionSafety(port.device);
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
    showStatus(cut ? "thermal trip latched - EXT 5V is off" : "POWER CUT FAILED - unplug FC now",
               12000);

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
    if (meshMode()) {
        // The transcript on disk has to match what was on screen, so the last
        // change is written before the link and its node table go away.
        flushMeshChats();
        mesh_.disconnect();
        // Session-only by contract, and the session is over.
        autoShareMinutes_ = 0;
        lastAutoShareMs_ = 0;
        locateNode_ = 0;
        locateMark_ = -1;
        linkMode_ = LinkMode::Betaflight;
        portLinkModeForced_ = false;
        refreshMeshMenus();
    }
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
    // A link that had borrowed the receiver's UART has given it back, so the
    // probe starts over; a receiver on a port of its own was never closed.
    if (!exitAfter) startGnss();
    if (exitAfter) running_ = false;
    else setScreen(Screen::Ports);
}

void App::requestDisconnect(bool exitAfter) {
    // A mesh radio has no bench guard to unwind: nothing was changed on it, so
    // closing the port is the whole of the operation.
    if (meshMode()) {
        finishDisconnect(exitAfter);
        return;
    }
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
                showStatus("restoring VTX state via FC reboot", 5000);
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
    if (!requireFcReady()) return;
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
    if (!requireFcReady()) return;
    diagnosticReport_ = DiagnosticReport{};
    diagnosticStatus_.clear();
    diagnosticTasks_.clear();
    diagnosticVersion_.clear();
    diagnosticError_.clear();
    diagnosticList_ = ListState{};
    diagnosticStep_ = 0;
    diagnosticRunning_ = true;
    openReturnableScreen(Screen::Diagnostics);
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
        showStatus(diagnosticReport_.actionableBlockerCount() > 0
                       ? std::to_string(diagnosticReport_.actionableBlockerCount()) + " arming blocker(s)"
                       : std::string("field check complete"));
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
                abortFieldCheck(std::string("capture stopped while running ") + commands[step]);
                return;
            }
            ++diagnosticStep_;
            runFieldCheckStep();
        });
    if (!started) abortFieldCheck("FC became busy before the next read-only query");
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
    int shown = 0;
    for (const std::string& line : splitLines(text)) {
        if (shown == 4000) break;
        term_.addLine(line, LineKind::Fc);
        ++shown;
    }
    pushLocal("---- end of " + f.name + " ----", LineKind::Local);
    term_.scrollToBottom(bodyRows(true));
    setScreen(Screen::Terminal);
}

void App::restoreFile(const BackupFile& f) {
    if (!requireFcReady()) return;
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
        showStatus("deleted " + f.name);
    });
}

void App::applyQuick(int id) {
    if (id < 0 || id >= kQuickCount) return;
    const QuickCmd& q = kQuick[id];
    const std::string cmd = q.cmd;

    auto run = [this, cmd]() {
        setScreen(Screen::Terminal);
        if (!requireFcReady()) return;
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
        showStatus("brightness " + std::to_string(brightness_) + "%", 2000);
    } else {
        showStatus(backlight_.available() ? "brightness write rejected"
                                          : "no backlight control available", 2000);
    }
}

int& App::linkBaudFor(LinkMode mode) {
    return mode == LinkMode::Meshtastic ? meshBaud_ : fcBaud_;
}

int App::linkBaud(LinkMode mode) const {
    return mode == LinkMode::Meshtastic ? meshBaud_ : fcBaud_;
}

void App::cycleLinkBaud(LinkMode mode) {
    int& baud = linkBaudFor(mode);
    baud = nextBaudChoice(baud);
    const char* who = mode == LinkMode::Meshtastic ? "radio" : "flight controller";
    // The rate is applied when the port is opened, so saying so is the
    // difference between a setting and an apparently ignored keystroke.
    showStatus(std::string(who) + " baud " + std::to_string(baud) +
               (linkConnected() && linkMode_ == mode ? " - applies on the next connect" : ""));
    refreshLinkSpeedMenu();
}

void App::cycleGnssBaud() {
    gnssBaud_ = nextBaudChoice(gnssBaud_);
    std::string text = "GNSS baud " + std::to_string(gnssBaud_);
    // Unlike the link rates, this one owns its port outright, so it can be
    // proved immediately instead of being promised for the next connect.
    if (gnssWanted_ && gnss_.isOpen()) {
        stopGnss();
        startGnss();
        if (!gnss_.isOpen()) text += " - receiver did not reopen";
    }
    showStatus(text);
    refreshLinkSpeedMenu();
    refreshMeshMenus();
}

// 9600 first: it is what most receivers that are not at 115200 ship at, and
// the table's own order would only reach it fourth.
int App::nextGnssProbeBaud() const {
    const auto tried = [this](int baud) {
        return std::find(gnssProbeTried_.begin(), gnssProbeTried_.end(), baud) !=
               gnssProbeTried_.end();
    };
    if (!tried(9600)) return 9600;
    for (int i = 0; i < kBaudChoiceCount; ++i) {
        if (!tried(kBaudChoices[i])) return kBaudChoices[i];
    }
    return 0;
}

void App::refreshLinkSpeedMenu() {
    for (MenuItem& item : linkSpeedMenu_) {
        switch (item.id) {
        case MenuFcBaud:
            item.label = "Flight controller: " + std::to_string(fcBaud_);
            item.hint = "USB CDC ignores this; a wired UART does not";
            break;
        case MenuMeshBaud:
            item.label = "Meshtastic radio: " + std::to_string(meshBaud_);
            item.hint = "used when the radio is wired, not on USB";
            break;
        case MenuGnssBaud:
            item.label = "GNSS receiver: " + std::to_string(gnssBaud_);
            item.hint = gnss_.isOpen() ? "Enter reopens the receiver at the next rate"
                                       : "a wrong rate is probed and corrected";
            break;
        default: break;
        }
    }
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
        showStatus("HUD sounds locked off by --mute for this launch", 4000);
        refreshSoundMenu();
        return;
    }
    soundEnabled_ = !soundEnabled_;
    audio_.setEnabled(soundEnabled_);
    if (soundEnabled_ && !audio_.available()) audio_.start();
    if (soundEnabled_ && audio_.available()) audio_.play(HudCue::Startup);
    const bool unavailable = soundEnabled_ && (!audio_.available() || !audio_.lastError().empty());
    showStatus(unavailable ? "HUD audio unavailable: " + audio_.lastError()
                           : std::string(soundEnabled_ ? "HUD sounds on" : "HUD sounds muted"),
               4000);
    refreshSoundMenu();
}

void App::adjustSoundVolume(int delta) {
    soundVolume_ = std::clamp(soundVolume_ + delta, 0, 100);
    audio_.setVolume(soundVolume_);
    if (soundEnabled_) audio_.play(HudCue::Select);
    showStatus("HUD volume " + std::to_string(soundVolume_) + "%" +
                   (audio_.available() ? "" : " (audio unavailable)"),
               2500);
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
    case MenuOpenMesh:
        openMenuPage(MenuPage::Mesh);
        break;
    case MenuOpenMeshPosition:
        refreshMeshMenus();
        openMenuPage(MenuPage::MeshPosition);
        break;
    case MenuMeshNodes:
        openReturnableScreen(Screen::Nodes);
        break;
    case MenuMeshBroadcast:
        openChat(kMeshBroadcast);
        break;
    case MenuMeshRadioInfo:
        showRadioInfo();
        break;
    case MenuMeshExport:
        exportConversation();
        break;
    case MenuMeshClear:
        clearConversation();
        break;
    case MenuGnssToggle:
        toggleGnss();
        break;
    case MenuGnssStatus:
        showGnssStatus();
        break;
    case MenuMeshShare:
        shareMyPosition(chatPeer_);
        break;
    case MenuMeshQuickMsg:
        openQuickMessages();
        break;
    case MenuMeshSos:
        sendSos();
        break;
    case MenuMarkHere:
        markHere();
        break;
    case MenuMarks:
        openMarks();
        break;
    case MenuAutoShare:
        cycleAutoShare();
        break;
    case MenuCompass:
        openReturnableScreen(Screen::Compass);
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
    case MenuOpenLinkSpeeds:
        refreshLinkSpeedMenu();
        openMenuPage(MenuPage::LinkSpeeds);
        break;
    case MenuFcBaud:   cycleLinkBaud(LinkMode::Betaflight); break;
    case MenuMeshBaud: cycleLinkBaud(LinkMode::Meshtastic); break;
    case MenuGnssBaud: cycleGnssBaud(); break;
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
        openReturnableScreen(Screen::Files);
        break;
    case MenuQuick:
        setScreen(Screen::Quick);
        break;
    case MenuKeymap:
        openReturnableScreen(Screen::Keymap);
        break;
    case MenuHelp:
        openHelp();
        break;
    case MenuAbout:
        openReturnableScreen(Screen::About);
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


// ------------------------------------------------------------- field tools

std::string App::myStationLabel() const {
    const MeshNode* self = mesh_.findNode(mesh_.radio().myNodeNum);
    if (self && !self->label().empty()) return self->label();
    return meshNodeIdText(mesh_.radio().myNodeNum);
}

Screen App::escapeTarget(Screen self, Screen fallback) const {
    if (returnScreen_ == self) return fallback;
    // A screen that was reached through Locate hands back to Locate's own
    // parent, not to Locate, or Escape would walk in a circle of two.
    if (returnScreen_ == Screen::Locate && self != Screen::Chat) return fallback;
    return returnScreen_;
}

void App::openLocateNode(uint32_t node) {
    if (node == 0 || node == kMeshBroadcast) {
        showStatus("the broadcast channel is not a place");
        return;
    }
    locateNode_ = node;
    locateMark_ = -1;
    audio_.play(HudCue::Select);
    openReturnableScreen(Screen::Locate);
}

void App::openLocateMark(int index) {
    if (index < 0 || index >= static_cast<int>(marks_.size())) return;
    locateMark_ = index;
    locateNode_ = 0;
    audio_.play(HudCue::Select);
    openReturnableScreen(Screen::Locate);
}

void App::loadMarks() {
    std::string text, error;
    marks_.clear();
    if (storage_.readFile(storage_.marksPath(), text, error)) marks_ = parseMarks(text);
    markList_.clamp(static_cast<int>(marks_.size()), bodyRows(false));
}

bool App::saveMarks() {
    std::string error;
    if (storage_.writeAtomic(storage_.marksPath(), formatMarks(marks_), error)) return true;
    showStatus("marks not saved: " + error, 6000);
    return false;
}

void App::addMark(Mark mark) {
    mark.name = cleanMarkName(mark.name);
    if (mark.name.empty()) mark.name = "Mark " + std::to_string(marks_.size() + 1);
    if (mark.stampUtc == 0) mark.stampUtc = static_cast<int64_t>(std::time(nullptr));
    if (marksFull()) return;
    marks_.push_back(std::move(mark));
    const bool saved = saveMarks();
    markList_.sel = static_cast<int>(marks_.size()) - 1;
    markList_.clamp(static_cast<int>(marks_.size()), bodyRows(false));
    if (saved) {
        showStatus("marked " + marks_.back().name);
        audio_.play(HudCue::Success);
    }
    refreshMeshMenus();
}

void App::markHere() {
    const GnssFix fix = gnss_.fix();
    if (!gnss_.receiverPresent()) {
        notice("No GNSS receiver",
               "A mark is this station's own fix, and there is no receiver reporting on " +
                   gnssDevice_ + ".");
        return;
    }
    if (!fix.valid) {
        notice("No fix yet",
               "The receiver is present but has no current fix.\n\n" +
                   gnss_.statusText(nowMs()) +
                   "\n\nGNDHOG will not save a stale or invented coordinate as a place.");
        return;
    }
    if (marksFull()) return;
    // The fix is captured when the key is pressed. The operator is naming the
    // spot they were standing on, not wherever they wander while typing.
    prompt("Mark this spot", fix.coordText() + "\n\nName this place:",
           "Mark " + std::to_string(marks_.size() + 1), kMaxMarkNameBytes,
           [this, fix](const std::string& name) {
               Mark mark;
               mark.name = name;
               mark.latitude = fix.latitude;
               mark.longitude = fix.longitude;
               mark.haveAltitude = fix.haveAltitude;
               mark.altitudeM = static_cast<int32_t>(fix.altitudeM);
               mark.source = "gnss";
               addMark(std::move(mark));
           });
}

void App::markNodePosition(uint32_t nodeNum) {
    const MeshNode* node = mesh_.findNode(nodeNum);
    if (!node || !node->position.valid) {
        notice("No position",
               "That node has not reported a position, so there is nothing to mark.");
        return;
    }
    if (marksFull()) return;
    // Copied now: the node table can evict or update the entry while the
    // dialog is open, and the point of a mark is that it does not move.
    const MeshPosition position = node->position;
    const std::string id = node->idText();
    prompt("Mark " + node->label() + "'s position",
           position.coordText() + "\n\nThis is the last position " + node->title() +
               " reported. It stays on this device after the radio forgets it.\n\n"
               "Name this place:",
           node->label(), kMaxMarkNameBytes, [this, position, id](const std::string& name) {
               Mark mark;
               mark.name = name;
               mark.latitude = position.latitude;
               mark.longitude = position.longitude;
               mark.haveAltitude = position.haveAltitude;
               mark.altitudeM = position.altitudeM;
               mark.source = id;
               addMark(std::move(mark));
           });
}

void App::deleteMark(int index) {
    if (index < 0 || index >= static_cast<int>(marks_.size())) return;
    const Mark mark = marks_[static_cast<size_t>(index)];
    confirm("Delete mark",
            mark.name + "\n" + mark.coordText() +
                "\n\nThe saved place is removed from this device. Nothing was ever "
                "transmitted, so there is nothing else to undo.",
            "Delete", [this, index]() {
                if (index >= static_cast<int>(marks_.size())) return;
                const std::string name = marks_[static_cast<size_t>(index)].name;
                marks_.erase(marks_.begin() + index);
                saveMarks();
                markList_.clamp(static_cast<int>(marks_.size()), bodyRows(false));
                if (locateMark_ == index) locateMark_ = -1;
                else if (locateMark_ > index) --locateMark_;
                showStatus("deleted mark " + name);
                refreshMeshMenus();
                if (screen_ == Screen::Locate) setScreen(Screen::Marks);
            });
}

void App::openQuickMessages() {
    if (quickMessages_.empty()) {
        notice("No quick messages",
               "Every quickmsg.N slot in config.ini is blank. Remove those lines to get "
               "the built-in set back.");
        return;
    }
    quickMsgMenu_.clear();
    const std::string hint = "Enter sends to " + peerTitle(chatPeer_);
    for (size_t i = 0; i < quickMessages_.size(); ++i) {
        quickMsgMenu_.push_back(MenuItem{quickMessages_[i], hint, static_cast<int>(i), true});
    }
    quickMsgList_.clamp(static_cast<int>(quickMsgMenu_.size()), bodyRows(false));
    openReturnableScreen(Screen::QuickMsg);
}

void App::sendQuickMessage(int index) {
    if (index < 0 || index >= static_cast<int>(quickMessages_.size())) return;
    const std::string text =
        expandQuickMessage(quickMessages_[static_cast<size_t>(index)], gnss_.fix());
    std::string error;
    if (!mesh_.sendText(chatPeer_, text, error)) {
        notice("Not sent", error, HudCue::Error);
        return;
    }
    audio_.play(HudCue::Command);
    chatFollow_ = true;
    chatRowsValid_ = false;
    mesh_.markRead(chatPeer_);
    setScreen(Screen::Chat);
}

void App::sendSos() {
    if (!requireRadioReady()) return;
    const GnssFix fix = gnss_.fix();
    std::string text = "SOS from " + myStationLabel() + ": need help";
    if (fix.valid) {
        text += " at " + fix.coordText();
        if (fix.haveAltitude) {
            text += " alt " + std::to_string(static_cast<int>(fix.altitudeM)) + "m";
        }
        if (!fix.utc.empty()) text += " " + fix.utc + " UTC";
    } else if (fix.everValid) {
        // Old is not the same as invented. A last known position, labelled
        // as such, is what a search party asks for first.
        text += ". No current fix; last known " + fix.coordText();
        if (!fix.utc.empty()) text += " at " + fix.utc + " UTC";
    } else {
        text += ". No GNSS position";
    }
    const std::string& channel = mesh_.radio().primaryChannel;
    std::string body = "Broadcast to everyone on " +
                       (channel.empty() ? std::string("the primary channel") : channel) +
                       ":\n\n" + text + "\n\n";
    body += fix.valid ? "A position packet goes with it."
                      : "No position packet: there is no current fix to put in one.";
    confirm("Send SOS?", body, "Send SOS", [this, text, fix]() {
        std::string error;
        if (!mesh_.sendText(kMeshBroadcast, text, error)) {
            notice("SOS not sent", error, HudCue::Error);
            return;
        }
        if (fix.valid) {
            std::string positionError;
            mesh_.sendPosition(kMeshBroadcast, fix, positionError);
        }
        audio_.play(HudCue::Success);
        openChat(kMeshBroadcast);
        // A broadcast is not acknowledged, so the only proof of rescue is a
        // reply. The conversation stays open to receive one.
        showStatus("SOS sent - repeat from the menu if nobody answers", 8000);
    });
}

void App::cycleAutoShare() {
    if (!requireRadioReady()) return;
    if (autoShareMinutes_ == 0) {
        if (!gnss_.receiverPresent()) {
            notice("No GNSS receiver",
                   "Auto-share transmits this station's own fix, and there is no receiver "
                   "reporting on " + gnssDevice_ + ".");
            return;
        }
        confirm("Auto-share position?",
                "Transmit this station's GNSS fix to the whole mesh every 2 minutes, "
                "whenever there is a current fix, until it is switched off or GNDHOG "
                "exits.\n\nThis is never saved: the next launch starts with it off.",
                "Switch on", [this]() {
                    autoShareMinutes_ = 2;
                    lastAutoShareMs_ = 0;
                    showStatus("auto-share every 2 min - first fix goes out now", 4000);
                    refreshMeshMenus();
                });
        return;
    }
    // 2 -> 5 -> 15 -> off. Every step is one press; off is never more than
    // three away, and the label says which it is.
    if (autoShareMinutes_ < 5) autoShareMinutes_ = 5;
    else if (autoShareMinutes_ < 15) autoShareMinutes_ = 15;
    else autoShareMinutes_ = 0;
    showStatus(autoShareMinutes_ > 0
                   ? "auto-share every " + std::to_string(autoShareMinutes_) + " min"
                   : std::string("auto-share off"));
    refreshMeshMenus();
}

void App::tickAutoShare(uint64_t now) {
    if (autoShareMinutes_ <= 0 || !mesh_.ready()) return;
    const uint64_t interval = static_cast<uint64_t>(autoShareMinutes_) * 60000u;
    if (lastAutoShareMs_ != 0 && now - lastAutoShareMs_ < interval) return;
    // No fix, no packet. The timer simply keeps waiting for one; a beacon
    // that repeats a coordinate it no longer has is a lie on a schedule.
    if (!gnss_.fix().valid) return;
    std::string error;
    if (!mesh_.sendPosition(kMeshBroadcast, gnss_.fix(), error)) {
        autoShareMinutes_ = 0;
        showStatus("auto-share off: " + error, 6000);
        refreshMeshMenus();
        return;
    }
    lastAutoShareMs_ = now;
}


// ------------------------------------------------------------------ compass

void App::loadCompassCalibration() {
    CompassCalibration cal;
    cal.hardIron = config_.getBool("compass.calibrated", false);
    cal.xOff = config_.getDouble("compass.xoff", 0.0);
    cal.yOff = config_.getDouble("compass.yoff", 0.0);
    cal.zOff = config_.getDouble("compass.zoff", 0.0);
    cal.fieldNorm = config_.getDouble("compass.norm", 0.0);
    cal.aligned = config_.getBool("compass.aligned", false);
    cal.mountOffsetDeg = config_.getDouble("compass.offset", 0.0);
    // Hand-set: the local magnetic declination, east positive, and a chip
    // that turns out to be mounted with its z axis into the board.
    cal.declinationDeg = config_.getDouble("compass.declination", 0.0);
    cal.mirror = config_.getBool("compass.mirror", false);
    compass_.setCalibration(cal);
}

void App::saveCompassCalibration() {
    const CompassCalibration& cal = compass_.calibration();
    config_.setBool("compass.calibrated", cal.hardIron);
    config_.setDouble("compass.xoff", cal.xOff);
    config_.setDouble("compass.yoff", cal.yOff);
    config_.setDouble("compass.zoff", cal.zOff);
    config_.setDouble("compass.norm", cal.fieldNorm);
    config_.setBool("compass.aligned", cal.aligned);
    config_.setDouble("compass.offset", cal.mountOffsetDeg);
    std::string error;
    if (!config_.save(storage_, error)) {
        showStatus("compass calibration not saved: " + error, 6000);
    }
}

void App::pollCompass(uint64_t now) {
    if (!compass_.available()) return;
    // Six sysfs reads per sample, five times a second: cheap, but only worth
    // it while something on screen is going to use the answer.
    const bool wanted = screen_ == Screen::Locate || screen_ == Screen::Compass ||
                        compass_.calibrating() ||
                        (screen_ == Screen::Menu && menuPage_ == MenuPage::MeshPosition);
    if (!wanted) return;
    compass_.poll(now);
    if (compass_.reading().sampledMs != compassSampleSeen_) {
        compassSampleSeen_ = compass_.reading().sampledMs;
        if (screen_ == Screen::Menu) refreshMeshMenus();
        dirty_ = true;
    }
}

void App::toggleCompassCalibration() {
    if (!compass_.available()) {
        notice("No magnetometer", "No IIO device on this machine reports a magnetic field.");
        return;
    }
    if (!compass_.calibrating()) {
        compass_.beginCalibration();
        showStatus("turn the device slowly through a full circle, then press C", 8000);
        return;
    }
    if (!compass_.finishCalibration()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "keep turning: %d samples, %d%% of a circle so far",
                      compass_.calibrationSamples(),
                      static_cast<int>(compass_.calibrationCoverage() * 100.0 + 0.5));
        showStatus(buf, 4000);
        return;
    }
    saveCompassCalibration();
    audio_.play(HudCue::Success);
    showStatus("compass calibrated; press A while walking to align it", 6000);
    refreshMeshMenus();
}

void App::alignCompassToTrack() {
    if (!compass_.available()) {
        notice("No magnetometer", "No IIO device on this machine reports a magnetic field.");
        return;
    }
    const GnssFix& fix = gnss_.fix();
    const bool moving = fix.valid && fix.haveCourse && fix.haveSpeed && fix.speedKph >= 2.5;
    if (!moving) {
        notice("Walk first",
               "Alignment sets the chip's forward direction from the GNSS track, so it "
               "needs a fix and a straight walk of a few steps at more than 2.5 km/h, "
               "holding the device pointed the way you are going.\n\n" +
                   gnss_.statusText(nowMs()));
        return;
    }
    if (!compass_.alignTo(fix.courseDeg, nowMs())) {
        notice("No compass sample", "The magnetometer has not produced a fresh reading.");
        return;
    }
    saveCompassCalibration();
    audio_.play(HudCue::Success);
    showStatus("compass aligned to track " + formatHeading(fix.courseDeg), 5000);
    refreshMeshMenus();
}

// -------------------------------------------------------------------- input

bool App::handleModalKey(const KeyEvent& e) {
    if (!modal_) return false;

    if (modalIsInput_) {
        // Editing keys may repeat; the two that decide the dialog may not.
        switch (e.key) {
        case Key::Enter: {
            if (e.repeat) return true;
            auto action = modalInputAction_;
            const std::string text = modalEditor_.text();
            audio_.play(HudCue::Select);
            closeModal();
            if (action) action(text);
            return true;
        }
        case Key::Escape:
            if (e.repeat) return true;
            audio_.play(HudCue::Back);
            closeModal();
            return true;
        case Key::Backspace: modalEditor_.backspace(); break;
        case Key::Delete:    modalEditor_.del(); break;
        case Key::Left:      modalEditor_.left(e.ctrl); break;
        case Key::Right:     modalEditor_.right(e.ctrl); break;
        case Key::Home:      modalEditor_.home(); break;
        case Key::End:       modalEditor_.end(); break;
        case Key::Char:
            if (e.ctrl) {
                if (e.ch == 'u') modalEditor_.killToStart();
                else if (e.ch == 'k') modalEditor_.killToEnd();
                else if (e.ch == 'w') modalEditor_.killWordBack();
                break;
            }
            if (modalEditor_.text().size() < modalInputMax_) modalEditor_.insert(e.ch);
            break;
        default: break;
        }
        dirty_ = true;
        return true;
    }

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
    case Key::Up:
        portList_.move(-1, n, rows);
        portLinkModeForced_ = false;
        syncPortLinkMode();
        audio_.play(HudCue::Navigate);
        dirty_ = true;
        break;
    case Key::Down:
        portList_.move(+1, n, rows);
        portLinkModeForced_ = false;
        syncPortLinkMode();
        audio_.play(HudCue::Navigate);
        dirty_ = true;
        break;
    case Key::Enter: audio_.play(HudCue::Select); connectSelected(); break;
    case Key::Escape: audio_.play(HudCue::Back); running_ = false; break;
    case Key::F1: openHelp(); break;
    case Key::Char:
        if (e.ch == 'r' || e.ch == 'R') {
            refreshPorts();
            showStatus("rescanned: " + std::to_string(ports_.size()) + " ports", 2000);
        } else if (e.ch == 'b' || e.ch == 'B') {
            // The picker already knows which protocol Enter will speak, so B
            // adjusts that peer's rate and leaves the other two alone.
            cycleLinkBaud(portLinkMode_);
        } else if (e.ch == 'm' || e.ch == 'M') {
            // The identity is a strong hint, not a verdict. A radio flashed
            // onto an FC-shaped board is still a radio.
            portLinkMode_ = portLinkMode_ == LinkMode::Meshtastic ? LinkMode::Betaflight
                                                                 : LinkMode::Meshtastic;
            portLinkModeForced_ = true;
            audio_.play(HudCue::Select);
            showStatus(portLinkMode_ == LinkMode::Meshtastic
                           ? "Enter opens this port as a Meshtastic radio"
                           : "Enter opens this port as a Betaflight CLI");
        } else if (e.ch == 'f' || e.ch == 'F') {
            refreshFiles();
            openReturnableScreen(Screen::Files);
        } else if (e.ch == 'g' || e.ch == 'G') {
            // The receiver no longer waits for a radio, so its status cannot
            // either: on a bench with nothing but a cap, this is the screen.
            audio_.play(HudCue::Select);
            showGnssStatus();
        } else if (e.ch == 'h' || e.ch == 'H' || e.ch == '?') {
            openHelp();
        } else if (e.ch == 'a' || e.ch == 'A') {
            openReturnableScreen(Screen::About);
        } else if (e.ch == 'q' || e.ch == 'Q') {
            running_ = false;
        }
        break;
    default: break;
    }
}

void App::onTerminalKey(const KeyEvent& e) {
    const int rows = bodyRows(!meshMode());

    // With a radio attached there is no prompt to type at: this screen is the
    // firmware's own console, so it scrolls and gets out of the way.
    if (meshMode()) {
        switch (e.key) {
        case Key::Up:       term_.scrollBy(-1, rows); dirty_ = true; return;
        case Key::Down:     term_.scrollBy(+1, rows); dirty_ = true; return;
        case Key::PageUp:   term_.scrollBy(-(rows - 1), rows); dirty_ = true; return;
        case Key::PageDown: term_.scrollBy(rows - 1, rows); dirty_ = true; return;
        case Key::Escape:
        case Key::F9:
            audio_.play(HudCue::Back);
            openRootMenu();
            return;
        case Key::Help:
        case Key::F1: openHelp(); return;
        case Key::F2: showRadioInfo(); return;
        case Key::F3:
        case Key::Enter: setScreen(Screen::Nodes); return;
        case Key::F6: showGnssStatus(); return;
        case Key::F10: requestDisconnect(false); return;
        case Key::BrightUp:   adjustBrightness(+10); return;
        case Key::BrightDown: adjustBrightness(-10); return;
        case Key::Char:
            if (e.ch == 'n' || e.ch == 'N') setScreen(Screen::Nodes);
            else if (e.ch == 'i' || e.ch == 'I') showRadioInfo();
            else if (e.ch == 'g' || e.ch == 'G') showGnssStatus();
            else if (e.ch == 'c' || e.ch == 'C') { term_.clear(); dirty_ = true; }
            return;
        default: return;
        }
    }

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
        audio_.play(HudCue::Back);
        openRootMenu();
        return;
    case Key::Help:      openHelp(); return;
    case Key::BrightUp:  adjustBrightness(+10); return;
    case Key::BrightDown: adjustBrightness(-10); return;
    case Key::F1: openHelp(); return;
    case Key::F2: runFieldCheck(); return;
    case Key::F3: if (session_.ready()) session_.send("version"); return;
    case Key::F4: if (session_.ready()) session_.send("diff"); return;
    case Key::F5: runBackup("diff all", "Backup (diff all)"); return;
    case Key::F6: refreshFiles(); openReturnableScreen(Screen::Files); return;
    case Key::F7: if (session_.ready()) session_.send("tasks"); return;
    case Key::F8:
        applyQuick(12);   // save, with its confirmation
        return;
    case Key::F9:
        openRootMenu();
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
    const bool quickMsg = (screen_ == Screen::QuickMsg);
    std::vector<MenuItem>& items = quick ? quick_ : quickMsg ? quickMsgMenu_ : currentMenuItems();
    ListState& st = quick ? quickList_ : quickMsg ? quickMsgList_ : currentMenuList();
    const int rows = bodyRows(false);
    const int n = static_cast<int>(items.size());

    if (navigateList(st, e, n, rows)) return;
    switch (e.key) {
    case Key::Enter:
        if (n == 0) break;
        if (quick) {
            audio_.play(HudCue::Select);
            applyQuick(items[static_cast<size_t>(st.sel)].id);
        } else if (quickMsg) {
            sendQuickMessage(items[static_cast<size_t>(st.sel)].id);
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
        } else if (quickMsg) {
            setScreen(escapeTarget(Screen::QuickMsg, Screen::Chat));
        } else if (menuPage_ == MenuPage::LinkSpeeds) {
            // The only page two levels deep. Escape gives back the category it
            // was opened from rather than dropping the operator at the root.
            openMenuPage(MenuPage::ConnectionExit);
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
    if (navigateList(fileList_, e, n, rows)) return;
    switch (e.key) {
    case Key::Enter:
        audio_.play(HudCue::Select);
        if (n > 0) restoreFile(files_[static_cast<size_t>(fileList_.sel)]);
        break;
    case Key::Escape:
        audio_.play(HudCue::Back);
        setScreen(returnScreen_);
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
            abortFieldCheck("cancelled by operator");
            pushLocal("-- field check cancelled --", LineKind::Warn);
            setScreen(returnScreen_);
        }
        return;
    }

    if (navigateList(diagnosticList_, e, n, rows)) return;
    switch (e.key) {
    case Key::Escape:
    case Key::Enter:
        audio_.play(HudCue::Back);
        setScreen(returnScreen_);
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
        setScreen(returnScreen_);
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
        setScreen(returnScreen_);
        break;
    default: break;
    }
}

void App::onNodesKey(const KeyEvent& e) {
    const int rows = bodyRows(false);
    const int n = nodeRowCount();

    if (navigateList(nodeList_, e, n, rows)) return;
    switch (e.key) {
    case Key::Enter:
        audio_.play(HudCue::Select);
        openChat(peerForNodeRow(nodeList_.sel));
        break;
    case Key::Escape:
        audio_.play(HudCue::Back);
        // Opened from a menu category, Escape goes back to that category, the
        // way every other menu-owned screen behaves. Reached as the mesh home
        // screen, it opens the menu instead.
        if (returnScreen_ == Screen::Menu) setScreen(Screen::Menu);
        else openRootMenu();
        break;
    case Key::F1: openHelp(); break;
    case Key::F2: showRadioInfo(); break;
    case Key::F4: openLocateNode(peerForNodeRow(nodeList_.sel)); break;
    case Key::F5:
        shareMyPosition(peerForNodeRow(nodeList_.sel));
        break;
    case Key::F6: showGnssStatus(); break;
    case Key::F9:
        openRootMenu();
        break;
    case Key::F10: requestDisconnect(false); break;
    case Key::Char:
        if (e.ch == 'i' || e.ch == 'I') showRadioInfo();
        else if (e.ch == 'g' || e.ch == 'G') showGnssStatus();
        else if (e.ch == 'l' || e.ch == 'L') {
            term_.scrollToBottom(bodyRows(false));
            openReturnableScreen(Screen::Terminal);
        } else if (e.ch == 'p' || e.ch == 'P') {
            shareMyPosition(peerForNodeRow(nodeList_.sel));
        } else if (e.ch == 'f' || e.ch == 'F') {
            openLocateNode(peerForNodeRow(nodeList_.sel));
        } else if (e.ch == 'm' || e.ch == 'M') {
            openMarks();
        }
        break;
    default: break;
    }
}

void App::onLocateKey(const KeyEvent& e) {
    switch (e.key) {
    case Key::Escape:
        audio_.play(HudCue::Back);
        setScreen(escapeTarget(Screen::Locate,
                               locateMark_ >= 0 ? Screen::Marks : Screen::Nodes));
        return;
    case Key::Enter:
        if (locateNode_ != 0) {
            audio_.play(HudCue::Select);
            openChat(locateNode_);
        }
        return;
    case Key::F1: openHelp(); return;
    case Key::F2: showRadioInfo(); return;
    case Key::F3: openReturnableScreen(Screen::Nodes); return;
    case Key::F5:
        if (locateNode_ != 0) shareMyPosition(locateNode_);
        return;
    case Key::F6: showGnssStatus(); return;
    case Key::F9:
        openRootMenu();
        return;
    case Key::F10: requestDisconnect(false); return;
    case Key::BrightUp:   adjustBrightness(+10); return;
    case Key::BrightDown: adjustBrightness(-10); return;
    case Key::Char:
        if (e.ch == 'p' || e.ch == 'P') {
            if (locateNode_ != 0) shareMyPosition(locateNode_);
        } else if (e.ch == 'm' || e.ch == 'M') {
            if (locateNode_ != 0) markNodePosition(locateNode_);
            else markHere();
        } else if (e.ch == 'd' || e.ch == 'D') {
            if (locateMark_ >= 0) deleteMark(locateMark_);
        } else if (e.ch == 'g' || e.ch == 'G') {
            showGnssStatus();
        } else if (e.ch == 'n' || e.ch == 'N') {
            openReturnableScreen(Screen::Nodes);
        }
        return;
    default: return;
    }
}

void App::onCompassKey(const KeyEvent& e) {
    switch (e.key) {
    case Key::Escape:
        if (e.repeat) return;
        audio_.play(HudCue::Back);
        if (compass_.calibrating()) {
            compass_.cancelCalibration();
            showStatus("compass calibration cancelled");
        }
        setScreen(escapeTarget(Screen::Compass, Screen::Nodes));
        return;
    case Key::F1: openHelp(); return;
    case Key::F6: showGnssStatus(); return;
    case Key::F9:
        openRootMenu();
        return;
    case Key::Char:
        if (e.repeat) return;
        if (e.ch == 'c' || e.ch == 'C') toggleCompassCalibration();
        else if (e.ch == 'a' || e.ch == 'A') alignCompassToTrack();
        else if (e.ch == 'g' || e.ch == 'G') showGnssStatus();
        return;
    default: return;
    }
}

void App::onMarksKey(const KeyEvent& e) {
    const int rows = bodyRows(false);
    const int n = static_cast<int>(marks_.size());
    if (navigateList(markList_, e, n, rows)) return;
    switch (e.key) {
    case Key::Enter:
        if (n > 0) openLocateMark(markList_.sel);
        break;
    case Key::Escape:
        audio_.play(HudCue::Back);
        setScreen(escapeTarget(Screen::Marks, Screen::Nodes));
        break;
    case Key::F1: openHelp(); break;
    case Key::F6: showGnssStatus(); break;
    case Key::F9:
        openRootMenu();
        break;
    case Key::Char:
        if (e.ch == 'n' || e.ch == 'N') markHere();
        else if (e.ch == 'd' || e.ch == 'D') { if (n > 0) deleteMark(markList_.sel); }
        else if (e.ch == 'g' || e.ch == 'G') showGnssStatus();
        break;
    default: break;
    }
}

void App::onChatKey(const KeyEvent& e) {
    const int rows = bodyRows(true);

    if (e.ctrl && e.key == Key::Char) {
        switch (e.ch) {
        case 'u': chatEditor_.killToStart(); dirty_ = true; return;
        case 'k': chatEditor_.killToEnd(); dirty_ = true; return;
        case 'w': chatEditor_.killWordBack(); dirty_ = true; return;
        case 'a': chatEditor_.home(); dirty_ = true; return;
        case 'e': chatEditor_.end(); dirty_ = true; return;
        case 'c': chatEditor_.clear(); dirty_ = true; return;
        default: break;
        }
    }

    switch (e.key) {
    case Key::Enter:
        audio_.play(HudCue::Command);
        submitChatLine();
        dirty_ = true;
        return;
    case Key::Tab:
        // Every printable key types; Tab is the one that does not, so it is
        // the one key a cold thumb can find for a canned line.
        audio_.play(HudCue::Select);
        openQuickMessages();
        return;
    case Key::F4:
        openLocateNode(chatPeer_);
        return;
    case Key::Backspace: chatEditor_.backspace(); dirty_ = true; return;
    case Key::Delete:    chatEditor_.del(); dirty_ = true; return;
    case Key::Left:      chatEditor_.left(e.ctrl); dirty_ = true; return;
    case Key::Right:     chatEditor_.right(e.ctrl); dirty_ = true; return;
    case Key::Home:      chatEditor_.home(); dirty_ = true; return;
    case Key::End:       chatEditor_.end(); dirty_ = true; return;
    case Key::Up:        chatEditor_.historyPrev(); dirty_ = true; return;
    case Key::Down:      chatEditor_.historyNext(); dirty_ = true; return;
    case Key::PageUp:
        chatScroll_ = std::max(0, chatScroll_ - (rows - 1));
        chatFollow_ = false;
        dirty_ = true;
        return;
    case Key::PageDown: {
        const int total = static_cast<int>(chatRows_.size());
        chatScroll_ = std::min(std::max(0, total - rows), chatScroll_ + (rows - 1));
        if (chatScroll_ >= std::max(0, total - rows)) chatFollow_ = true;
        dirty_ = true;
        return;
    }
    case Key::Escape:
        audio_.play(HudCue::Back);
        setScreen(escapeTarget(Screen::Chat, Screen::Nodes));
        return;
    case Key::F1: openHelp(); return;
    case Key::F2: showRadioInfo(); return;
    case Key::F3: openReturnableScreen(Screen::Nodes); return;
    case Key::F5: shareMyPosition(chatPeer_); return;
    case Key::F6: showGnssStatus(); return;
    case Key::F9:
        openRootMenu();
        return;
    case Key::F10: requestDisconnect(false); return;
    case Key::BrightUp:   adjustBrightness(+10); return;
    case Key::BrightDown: adjustBrightness(-10); return;
    case Key::Char:
        chatEditor_.insert(e.ch);
        dirty_ = true;
        return;
    default: return;
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
    case Screen::Nodes:    onNodesKey(e); break;
    case Screen::Chat:     onChatKey(e); break;
    case Screen::Locate:   onLocateKey(e); break;
    case Screen::Marks:    onMarksKey(e); break;
    case Screen::QuickMsg: onMenuKey(e); break;
    case Screen::Compass:  onCompassKey(e); break;
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
    pollGnss(now);
    pollCompass(now);
    if (meshMode()) {
        tickMesh(now);
        return;
    }

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
                    showStatus("EXT USB4 thermal trip armed at 80C", 6000);
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
                        showStatus("FC temperature unavailable");
                        return;
                    }
                    int temperatureC = 0;
                    if (!parseCoreTemperatureC(text, temperatureC)) {
                        // Older targets that omit the field should not have
                        // their terminal filled with an unproductive query.
                        nextTemperatureCheckMs_ = 0;
                        showStatus("FC does not report core temperature - watch stopped", 5000);
                    }
                })) {
            nextTemperatureCheckMs_ = now + kTemperatureCheckRetryMs;
            showStatus("temperature watch deferred - FC busy");
        }
    }

    if (session_.coreTemperatureSequence() != lastTemperatureSequence_) {
        lastTemperatureSequence_ = session_.coreTemperatureSequence();
        const int temperatureC = session_.coreTemperatureC();
        showStatus("FC core " + std::to_string(temperatureC) + "C", 5000);
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
        abortFieldCheck("serial link lost during field check");
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
            showStatus(session_.vtxBenchGuardActive()
                           ? "link lost - pit mode clears only when the FC reboots"
                           : "link lost - Esc for the menu to reconnect",
                       6000);
        }
    } else {
        linkLossHandled_ = false;
    }
    tickStatusTail(now);
}

void App::tickStatusTail(uint64_t now) {
    // Throttled inside Battery: a resting pack is read every five seconds and
    // only repaints the bar when a number the operator can see has moved.
    if (battery_.poll(now)) dirty_ = true;

    const std::string audioError = audio_.lastError();
    if (audioError != lastAudioError_) {
        lastAudioError_ = audioError;
        refreshSoundMenu();
        if (!audioError.empty() && soundEnabled_ && !opt_.muteSound) {
            showStatus("HUD audio unavailable: " + audioError, 6000);
        }
    }
    if (!status_.empty() && now > statusUntil_) {
        status_.clear();
        dirty_ = true;
    }
}

// The receiver is independent of whichever radio or flight controller is on
// the other port, so it is polled in both link modes. The probe that follows
// the read decides whether the UART carries a receiver at all, and at which
// rate: a wire that talks but never in NMEA is reopened at the next rate in
// the table, a rate that answers is kept as gnss.baud, and a silent wire is
// given back at the first deadline, because on this board that node is also
// the Grove header and a flight controller may be waiting on it.
void App::pollGnss(uint64_t now) {
    if (!gnss_.isOpen()) return;
    const bool hadFix = gnss_.fix().valid;
    const int before = gnss_.sentenceCount();
    gnss_.poll(now);
    if (gnss_.sentenceCount() != before || gnss_.fix().valid != hadFix) dirty_ = true;

    if (gnssProbeDeadlineMs_ == 0 || gnssProbeReported_ || now < gnssProbeDeadlineMs_) return;
    const std::string rate = std::to_string(gnssBaud_);
    if (gnss_.receiverPresent()) {
        gnssProbeReported_ = true;
        if (gnssBaud_ != gnssProbeFirstBaud_) {
            pushLocal("-- " + gnssDevice_ + " answers at " + rate + ", not " +
                          std::to_string(gnssProbeFirstBaud_) +
                          (opt_.gnssBaudSet ? "; using it for this launch --"
                                            : "; gnss.baud follows the wire --"),
                      LineKind::Good);
            refreshLinkSpeedMenu();
        }
        pushLocal("-- GNSS receiver present on " + gnssDevice_ + " @ " + rate + ": " +
                      std::to_string(gnss_.sentenceCount()) + " NMEA sentences --",
                  LineKind::Good);
    } else if (gnss_.bytesSeen() > 0 && gnss_.legibleLines() < kGnssConsoleLines &&
               nextGnssProbeBaud() != 0) {
        // Bytes and no sentence: a receiver at some other rate. Readable text
        // would have meant a console, which no rate change turns into NMEA.
        const int next = nextGnssProbeBaud();
        pushLocal("-- " + gnssDevice_ + " @ " + rate + ": " +
                      std::to_string(gnss_.bytesSeen()) + " bytes, no NMEA; trying " +
                      std::to_string(next) + " --",
                  LineKind::Local);
        gnss_.close();
        gnssBaud_ = next;
        gnssProbeTried_.push_back(next);
        std::string error;
        if (gnss_.open(gnssDevice_, gnssBaud_, error)) {
            gnssProbeDeadlineMs_ = now + kGnssRetryWindowMs;
        } else {
            gnssProbeReported_ = true;
            gnssProbeDeadlineMs_ = 0;
            gnssBaud_ = gnssProbeFirstBaud_;
            pushLocal("-- GNSS receiver did not reopen: " + error + " --", LineKind::Warn);
        }
        refreshLinkSpeedMenu();
    } else {
        gnssProbeReported_ = true;
        const size_t bytes = gnss_.bytesSeen();
        const bool console = gnss_.legibleLines() >= kGnssConsoleLines;
        gnss_.close();
        if (console) {
            pushLocal("-- " + gnssDevice_ + " is sending text, not NMEA: a console, not a "
                      "receiver; treating the GNSS receiver as absent --",
                      LineKind::Warn);
        } else if (bytes > 0) {
            pushLocal("-- " + gnssDevice_ + ": bytes but no NMEA at any rate in the table; "
                      "treating the GNSS receiver as absent --",
                      LineKind::Warn);
        } else {
            pushLocal("-- no NMEA on " + gnssDevice_ +
                          "; treating the GNSS receiver as absent --",
                      LineKind::Warn);
        }
        if (gnssBaud_ != gnssProbeFirstBaud_) {
            gnssBaud_ = gnssProbeFirstBaud_;
            refreshLinkSpeedMenu();
        }
    }
    refreshMeshMenus();
}

void App::tickMesh(uint64_t now) {
    const uint64_t arrivalsBefore = term_.linesEver();
    mesh_.poll(now);
    if (term_.linesEver() != arrivalsBefore) {
        if (term_.following()) term_.scrollToBottom(bodyRows(false));
        dirty_ = true;
    }

    if (mesh_.nodeSequence() != nodeSequenceSeen_) {
        nodeSequenceSeen_ = mesh_.nodeSequence();
        dirty_ = true;
    }

    if (mesh_.chatSequence() != chatSequenceSeen_) {
        chatSequenceSeen_ = mesh_.chatSequence();
        chatRowsValid_ = false;
        dirty_ = true;
        if (screen_ == Screen::Chat) mesh_.markRead(chatPeer_);
        flushMeshChats();
    }

    const int unread = mesh_.totalUnread();
    if (unread > meshUnreadSeen_) audio_.play(HudCue::Prompt);
    meshUnreadSeen_ = unread;

    if (mesh_.noteSequence() != meshNoteSeen_) {
        meshNoteSeen_ = mesh_.noteSequence();
        showStatus(mesh_.note(), 5000);
    }

    tickAutoShare(now);

    // "heard 4m ago" and "my fix 12s" go stale sitting still. The node list
    // ticks every ten seconds; the Locate screen, which somebody is walking
    // behind, every second.
    if (screen_ == Screen::Locate || screen_ == Screen::Nodes || screen_ == Screen::Marks ||
        screen_ == Screen::Compass) {
        const uint64_t period = screen_ == Screen::Nodes ? 10000 : 1000;
        if (now - lastAgeRepaintMs_ >= period) {
            lastAgeRepaintMs_ = now;
            dirty_ = true;
        }
    }

    if (mesh_.state() == MeshState::Failed && !meshFailureReported_ && !modal_) {
        meshFailureReported_ = true;
        audio_.play(HudCue::Error);
        if (mesh_.nmeaSentences() > 0) {
            // The bench case: the cap's receiver on the node the picker offered
            // as a Grove UART. Close the link and give the wire back to the
            // reader that parses it, which finishDisconnect restarts.
            const std::string device = mesh_.device();
            const bool ownDevice = sameDeviceNode(device, gnssDevice_);
            pushLocal("-- " + device + " answered with NMEA 0183, not the client API: "
                      "that is a GNSS receiver --",
                      LineKind::Warn);
            finishDisconnect(false);
            notice("GNSS receiver, not a radio",
                   device + " is speaking NMEA 0183: that is a GNSS "
                   "receiver, and the radio link on it has been closed.\n\n" +
                       (ownDevice
                            ? std::string("The receiver is being read again; "
                                          "G shows its status.")
                            : "It is not the configured receiver (" + gnssDevice_ +
                                  "). Launch with --gnss " + device +
                                  " or set gnss.device to read it."),
                   HudCue::Error);
        } else {
            notice("No Meshtastic radio",
                   "The device on " + mesh_.device() +
                       " did not answer the client API.\n\n"
                       "Check that this is the radio's port, that the firmware is running, "
                       "and that nothing else already holds the connection.",
                   HudCue::Error);
        }
    }

    if (mesh_.linkLost()) {
        if (!linkLossHandled_) {
            linkLossHandled_ = true;
            audio_.play(HudCue::LinkDown);
            flushMeshChats();
            refreshPorts();
            showStatus("radio link lost - Esc for the menu to reconnect", 6000);
        }
    } else {
        linkLossHandled_ = false;
    }

    tickStatusTail(now);
}

int App::run(const Options& opt) {
    std::string error;
    if (!setup(opt, error)) {
        std::fprintf(stderr, "%s: %s\n", kAppName, error.c_str());
        return 1;
    }

    if (!opt.previewDir.empty()) {
        // Every writePpm below opens a file by path, and fopen does not build
        // the directory it was handed. Without this the whole run reports
        // "wrote 0 previews" and never says which of the two things went
        // wrong: the painting, or a directory that was never there.
        std::string previewDirError;
        if (!makeDirs(opt.previewDir, previewDirError)) {
            std::fprintf(stderr, "%s: --preview %s: %s\n", kAppName,
                         opt.previewDir.c_str(), previewDirError.c_str());
            teardown();
            return 1;
        }
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
            {Screen::Menu, MenuPage::LinkSpeeds, "09-menu-link-speeds"},
            {Screen::Quick, MenuPage::FlightController, "10-quick"},
            {Screen::Files, MenuPage::Root, "11-files"},
            {Screen::Keymap, MenuPage::Root, "12-keymap"},
            {Screen::Help, MenuPage::Root, "13-help"},
            {Screen::Diagnostics, MenuPage::Root, "14-field-check"},
            {Screen::About, MenuPage::Root, "15-about"},
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
        if (display_.canvas().writePpm(opt.previewDir + "/16-confirm.ppm")) ++written;
        closeModal();
        confirm("Bench VTX guard?",
                "SmartAudio reports power level 3/4.\n\nTry verified pit mode before entering "
                "CLI? This is the VTX's purpose-built bench state; no setting is saved. "
                "Unsupported hardware is left unchanged.",
                "Use pit", nullptr);
        render();
        if (display_.canvas().writePpm(opt.previewDir + "/17-vtx-guard.ppm")) ++written;
        closeModal();
        notice("CRITICAL FC TEMPERATURE",
               "FC MCU core is 82C. Betaflight's default alarm is 70C.\n\n"
               "This is not a VTX temperature sensor. Stop bench work, unplug FC USB and "
               "battery power, and let the stack cool. Closing the serial link does not "
               "remove USB power.");
        render();
        if (display_.canvas().writePpm(opt.previewDir + "/18-temperature-warning.ppm")) ++written;
        closeModal();
        confirm("Restore VTX state?",
                "Pit mode is active for bench work. Restore the saved flight state?\n\n"
                "Restore exits CLI, discards unsaved CLI changes, and reboots the FC. Cancel "
                "closes the link and leaves pit mode active until the FC is rebooted or "
                "power-cycled.",
                "Restore", nullptr);
        render();
        if (display_.canvas().writePpm(opt.previewDir + "/19-vtx-restore.ppm")) ++written;
        closeModal();

        // The mesh screens are worth seeing with real traffic in them, so the
        // preview drives the simulated radio rather than painting invented
        // node rows that no decoder ever produced.
        SimMesh previewMesh;
        std::string previewError;
        if (previewMesh.start(previewError) &&
            mesh_.connect(previewMesh.devicePath(), 115200, previewError)) {
            const uint64_t ready = nowMs() + 4000;
            while (nowMs() < ready && !mesh_.ready()) {
                previewMesh.pump();
                mesh_.poll(nowMs());
                sleepMs(4);
            }
        }
        if (mesh_.ready()) {
            linkMode_ = LinkMode::Meshtastic;
            refreshMeshMenus();
            // The Betaflight fixture lines above belong to the other shots.
            term_.clear();
            chatPeer_ = previewMesh.hilltopNodeNum();
            previewMesh.injectText(previewMesh.hilltopNodeNum(), mesh_.radio().myNodeNum,
                                   "gate is open, road is clear past the ford");
            std::string sendError;
            mesh_.sendText(chatPeer_, "rolling in ten, keep the channel", sendError);
            const uint64_t settle = nowMs() + 1200;
            while (nowMs() < settle) {
                previewMesh.pump();
                mesh_.poll(nowMs());
                sleepMs(4);
            }
            chatRowsValid_ = false;
            chatFollow_ = true;

            // A fix from a spot south-west of the fixture's hilltop relay, so
            // the Locate screen has a real range and bearing to draw, and a
            // walking course so the turn advice has something to say.
            GnssFix previewFix;
            previewFix.valid = true;
            previewFix.latitude = 51.47790;
            previewFix.longitude = -0.00150;
            previewFix.haveAltitude = true;
            previewFix.altitudeM = 45.0;
            previewFix.satellitesUsed = 9;
            previewFix.satellitesInView = 14;
            previewFix.hdop = 0.9;
            previewFix.haveSpeed = true;
            previewFix.speedKph = 4.2;
            previewFix.haveCourse = true;
            previewFix.courseDeg = 38.0;
            previewFix.utc = "12:35:19";
            gnss_.adoptFix(previewFix, nowMs());
            Mark previewMark;
            previewMark.name = "Car";
            previewMark.latitude = 51.47510;
            previewMark.longitude = -0.00920;
            previewMark.haveAltitude = true;
            previewMark.altitudeM = 12;
            previewMark.stampUtc = static_cast<int64_t>(std::time(nullptr)) - 5400;
            previewMark.source = "gnss";
            // In memory only: the preview must not write into the operator's
            // real marks file.
            marks_.push_back(previewMark);
            locateNode_ = previewMesh.hilltopNodeNum();
            locateMark_ = -1;
            quickMessages_ = defaultQuickMessages();
            refreshMeshMenus();

            const struct { Screen screen; MenuPage page; const char* name; } kMeshShots[] = {
                {Screen::Nodes, MenuPage::Root, "20-mesh-nodes"},
                {Screen::Chat, MenuPage::Root, "21-mesh-chat"},
                {Screen::Menu, MenuPage::Root, "22-mesh-menu"},
                {Screen::Menu, MenuPage::Mesh, "23-mesh-network"},
                {Screen::Menu, MenuPage::MeshPosition, "24-mesh-position-gnss"},
                {Screen::Terminal, MenuPage::Root, "25-mesh-radio-log"},
                {Screen::Locate, MenuPage::Root, "26-mesh-locate"},
                {Screen::Marks, MenuPage::Root, "27-mesh-marks"},
            };
            for (const auto& shot : kMeshShots) {
                screen_ = shot.screen;
                menuPage_ = shot.page;
                render();
                if (display_.canvas().writePpm(opt.previewDir + "/" + shot.name + ".ppm")) {
                    ++written;
                }
            }
            screen_ = Screen::Chat;
            openQuickMessages();
            render();
            if (display_.canvas().writePpm(opt.previewDir + "/28-mesh-quick-messages.ppm")) {
                ++written;
            }
            screen_ = Screen::Locate;
            markNodePosition(locateNode_);
            render();
            if (display_.canvas().writePpm(opt.previewDir + "/29-mesh-mark-prompt.ppm")) {
                ++written;
            }
            closeModal();
            screen_ = Screen::Compass;
            render();
            if (display_.canvas().writePpm(opt.previewDir + "/30-compass.ppm")) ++written;
            marks_.clear();
            menuPage_ = MenuPage::Root;
            mesh_.disconnect();
        } else if (!previewError.empty()) {
            std::printf("mesh preview skipped: %s\n", previewError.c_str());
        }

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

    SimMesh simMesh;
    if (opt.simulateMesh && !opt.showAbout) {
        std::string simErr;
        if (!simMesh.start(simErr)) {
            std::fprintf(stderr, "%s: mesh simulator: %s\n", kAppName, simErr.c_str());
            teardown();
            return 1;
        }
        std::string cerr;
        pushLocal("simulated Meshtastic radio on " + simMesh.devicePath(), LineKind::Local);
        if (mesh_.connect(simMesh.devicePath(), 115200, cerr)) {
            linkMode_ = LinkMode::Meshtastic;
            audio_.play(HudCue::LinkUp);
            beginMeshSession();
            setScreen(Screen::Nodes);
        } else {
            pushLocal("mesh connect failed: " + cerr, LineKind::Error);
        }
    }

    RawTerminalMode rawTty(opt.stdinKeys || !keyboard_.anyOpen());

    const uint64_t frameMs = 33;   // ~30 fps, matching the reference UI target
    uint64_t nextFrame = nowMs();

    // Reused across frames: thirty times a second is no place to allocate.
    std::vector<pollfd> pfds;
    std::vector<KeyEvent> events;

    while (running_) {
        const uint64_t now = nowMs();

        // Wait on input and serial together so the UI stays responsive without
        // ever spinning: a slow `dump` never blocks the frame loop.
        pfds.clear();
        for (int fd : keyboard_.fds()) pfds.push_back(pollfd{fd, POLLIN, 0});
        if (session_.connected()) pfds.push_back(pollfd{session_.fd(), POLLIN, 0});
        if (mesh_.connected()) pfds.push_back(pollfd{mesh_.fd(), POLLIN, 0});
        if (gnss_.isOpen()) pfds.push_back(pollfd{gnss_.fd(), POLLIN, 0});
        if (!keyboard_.anyOpen() || opt.stdinKeys) {
            pfds.push_back(pollfd{STDIN_FILENO, POLLIN, 0});
        }
        int waitMs = static_cast<int>(nextFrame > now ? nextFrame - now : 0);
        if (session_.busy() || session_.job().active()) waitMs = std::min(waitMs, 10);
        if (mesh_.connected() && !mesh_.ready()) waitMs = std::min(waitMs, 10);
        if (!pfds.empty()) ::poll(pfds.data(), pfds.size(), waitMs);
        else sleepMs(waitMs);

        events.clear();
        keyboard_.pump(nowMs(), events);
        keyboard_.pumpStdin(events);
        keyboard_.pumpRepeat(nowMs(), events);
        for (const KeyEvent& e : events) {
            handleKey(e);
            if (!running_) break;
        }

        if (opt.simulate) sim.pump();
        if (opt.simulateMesh) simMesh.pump();
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
