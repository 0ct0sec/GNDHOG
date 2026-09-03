#pragma once
#include "audio.h"
#include "battery.h"
#include "bfcommands.h"
#include "bfsession.h"
#include "compass.h"
#include "diagnostics.h"
#include "display.h"
#include "gfx.h"
#include "gnss.h"
#include "input.h"
#include "marks.h"
#include "meshsession.h"
#include "meshtastic.h"
#include "quickmsg.h"
#include "serialport.h"
#include "storage.h"
#include "term.h"
#include "thermaltrip.h"

#include <functional>
#include <string>
#include <vector>

namespace bf {

// 320x170 landscape, 6x8 cell => 53 columns. The panel's real geometry is read
// from the driver at startup; these are the design values used when headless.
constexpr int kScreenW = 320;
constexpr int kScreenH = 170;
constexpr int kTopH    = 11;
constexpr int kBodyY   = 13;
constexpr int kHintH   = 10;
constexpr int kInputH  = 11;

enum class Screen {
    Ports,
    Terminal,
    Menu,
    Quick,
    Files,
    Diagnostics,
    Keymap,
    Help,
    About,
    Nodes,      // mesh: discovered radios and the broadcast channel
    Chat,       // mesh: one conversation
    Locate,     // mesh: range, bearing and a compass rose to a node or a mark
    Marks,      // mesh: saved places
    QuickMsg,   // mesh: canned messages for the open conversation
    Compass,    // the BMM150: heading, calibration, alignment
};

enum class MenuPage {
    Root,
    FlightController,
    BackupRestore,
    Mesh,
    MeshPosition,
    ControlsInfo,
    SoundDisplay,
    ConnectionExit,
    LinkSpeeds,
};

// Which protocol the open serial port is speaking. The two are mutually
// exclusive by construction: one port, one conversation, no guessing halfway
// through about which peer just answered.
enum class LinkMode { Betaflight, Meshtastic };

struct MenuItem {
    std::string label;
    std::string hint;
    int id = 0;
    bool enabled = true;
};

// Scroll/selection state shared by every list screen.
struct ListState {
    int sel = 0;
    int top = 0;

    void clamp(int count, int visible);
    void move(int delta, int count, int visible);
    void ensureVisible(int visible);
};

class App {
public:
    struct Options {
        std::string fbDevice = "/dev/fb0";
        std::string portOverride;      // --port
        // One port at a time, but three peers with three different opinions
        // about line rate. Each is remembered separately and each command-line
        // switch is a launch-wide override that must not rewrite the saved one.
        int fcBaud = 115200;           // --fc-baud (or --baud)
        int meshBaud = 115200;         // --mesh-baud (or --baud)
        int gnssBaud = 115200;         // --gnss-baud
        bool fcBaudSet = false;
        bool meshBaudSet = false;
        bool gnssBaudSet = false;
        bool headless = false;         // render offscreen only
        bool stdinKeys = false;        // read stdin instead of evdev
        bool simulate = false;         // talk to the built-in fake FC
        bool simulateMesh = false;     // talk to the built-in fake Meshtastic radio
        bool forceMesh = false;        // --mesh: open --port as a Meshtastic radio
        bool autoConnect = true;
        bool muteSound = false;        // --mute: session-only audio override
        bool showAbout = false;        // --about: start offline on the credits
        int frameLimit = 0;            // stop after N frames (preview/testing)
        std::string previewDir;        // dump each screen as PPM and exit
        // The GNSS receiver's UART: the cap's, or a GPS unit on the Grove
        // socket. Empty means "use the configured or default device"; a named
        // one is opened for this launch whatever the saved switch says.
        // --no-gnss suppresses the probe entirely.
        std::string gnssDevice;
        bool gnssEnabled = true;
    };

    int run(const Options& opt);

private:
    friend int runSelfTest();
    friend void testMeshApp();   // selftest.cpp: drives the mesh screens
    friend void testFieldTools(); // selftest.cpp: locate, marks, quick messages, SOS
    friend void testBattery();   // selftest.cpp: paints the gauge indicator
    friend void testLinkBaud();  // selftest.cpp: drives the saved link rates
    friend void testGnssBaudProbe(); // selftest.cpp: a receiver at the wrong rate
    // ---- lifecycle
    bool setup(const Options& opt, std::string& error);
    void teardown();
    void tick(uint64_t now);
    void tickMesh(uint64_t now);
    void pollGnss(uint64_t now);
    // Status-line expiry and the HUD audio watch, which both links need.
    void tickStatusTail(uint64_t now);
    void render();

    // ---- input
    void handleKey(const KeyEvent& e);
    void onPortsKey(const KeyEvent& e);
    void onTerminalKey(const KeyEvent& e);
    void onMenuKey(const KeyEvent& e);
    void onFilesKey(const KeyEvent& e);
    void onDiagnosticsKey(const KeyEvent& e);
    void onKeymapKey(const KeyEvent& e);
    void onHelpKey(const KeyEvent& e);
    void onAboutKey(const KeyEvent& e);
    void onNodesKey(const KeyEvent& e);
    void onChatKey(const KeyEvent& e);
    void onLocateKey(const KeyEvent& e);
    void onMarksKey(const KeyEvent& e);
    void onCompassKey(const KeyEvent& e);
    bool handleModalKey(const KeyEvent& e);

    // ---- screens
    void drawTopBar(Surface& s);
    // Returns the x of the indicator's left edge, or `right` when there is no
    // pack to draw -- the top bar hands the reclaimed width back to the title.
    int drawBatteryIndicator(Surface& s, int right);
    void drawHintBar(Surface& s, const std::string& hints,
                     const std::string& action = {});
    void drawPorts(Surface& s);
    void drawTerminal(Surface& s);
    void drawMenu(Surface& s);
    void drawFiles(Surface& s);
    void drawDiagnostics(Surface& s);
    void drawKeymap(Surface& s);
    void drawHelp(Surface& s);
    void drawAbout(Surface& s);
    void drawNodes(Surface& s);
    void drawChat(Surface& s);
    void drawLocate(Surface& s);
    void drawMarks(Surface& s);
    void drawQuickMsg(Surface& s);
    void drawCompassScreen(Surface& s);
    // The rose. Every angle is drawn relative to `rotationDeg`, the true
    // bearing at the top: zero is north-up, the operator's own heading is
    // heading-up. The target arrow is a true bearing; the dot on the rim is
    // the direction this station is facing or moving.
    void drawCompass(Surface& s, int cx, int cy, int r, bool haveBearing,
                     double bearingDeg, bool haveHeading, double headingDeg,
                     double rotationDeg);
    void drawModal(Surface& s);
    void drawList(Surface& s, const std::vector<MenuItem>& items, ListState& st,
                  int visibleRows, bool showSelection = true);
    void drawMenuModal(Surface& s, const std::string& title,
                       const std::vector<MenuItem>& items, ListState& st);
    // One editable line with the block cursor: the terminal's, the chat's and
    // the input dialog's. `tallCursor` is the terminal and chat variant that
    // overhangs the row by a pixel.
    void drawInputLine(Surface& s, int x, int y, const LineEditor& editor, int avail,
                       const char* prompt, Color promptColor, bool tallCursor);
    // The highlight behind the selected row of a list screen.
    void drawRowSelection(Surface& s, int y, bool selected);

    void setupMenus();
    std::vector<MenuItem>& currentMenuItems();
    ListState& currentMenuList();
    void openMenuPage(MenuPage page);

    // ---- actions
    void refreshPorts();
    void syncPortLinkMode();
    void connectSelected();
    void connectPort(const PortInfo& port, LinkMode mode);
    void beginConnectionSafety(const std::string& device);
    void performThermalTrip(int temperatureC);
    void requestDisconnect(bool exitAfter = false);
    void finishDisconnect(bool exitAfter = false);
    void submitLine();
    void doComplete();
    void runBackup(const std::string& command, const std::string& label);
    void runFieldCheck();
    void runFieldCheckStep();
    void saveFieldCheck();
    void refreshFiles();
    void viewFile(const BackupFile& f);
    void restoreFile(const BackupFile& f);
    void deleteFile(const BackupFile& f);
    void applyQuick(int id);
    void applyMenu(int id);
    void adjustBrightness(int delta);
    // The rate the picker will use for the protocol it is about to open.
    int& linkBaudFor(LinkMode mode);
    int linkBaud(LinkMode mode) const;
    void cycleLinkBaud(LinkMode mode);
    void cycleGnssBaud();
    // The rate the receiver probe tries next, or 0 once the table is spent.
    int nextGnssProbeBaud() const;
    void refreshLinkSpeedMenu();
    void adjustSoundVolume(int delta);
    void toggleSound();
    void refreshSoundMenu();
    void pushLocal(const std::string& text, LineKind kind = LineKind::Local);

    // ---- mesh
    bool meshMode() const { return linkMode_ == LinkMode::Meshtastic; }
    bool linkConnected() const;
    void beginMeshSession();
    void loadMeshChats();
    void flushMeshChats();
    void openChat(uint32_t peer);
    void submitChatLine();
    // The recipient is passed in rather than read from chatPeer_: the node list
    // shares with the highlighted row, and borrowing the open conversation to
    // carry that would silently repoint Export and Clear at it.
    void shareMyPosition(uint32_t peer);
    void showRadioInfo();
    void showGnssStatus();
    void toggleGnss();
    // `quiet` drops the terminal line when the node cannot be opened: at
    // launch on a development host /dev/serial0 does not exist, and that is
    // not news. The GNSS status page still reports the reason.
    void startGnss(bool quiet = false);
    // True when `device` is the UART the receiver currently holds open, under
    // whichever name the picker used for it.
    bool isGnssPort(const std::string& device) const;
    void exportConversation();
    void clearConversation();
    void rebuildChatRows();
    void refreshMeshMenus();
    // Row 0 of the node screen is the broadcast channel; the rest are radios.
    int nodeRowCount() const;
    uint32_t peerForNodeRow(int row) const;
    std::string peerTitle(uint32_t peer) const;
    // This radio's own short name, for messages that name their sender.
    std::string myStationLabel() const;

    // ---- field tools: locate, marks, quick messages, SOS, auto-share
    void openLocateNode(uint32_t node);
    void openLocateMark(int index);
    // Where Escape goes from a screen that may have been reached from itself
    // (Locate opens Chat, Chat returns to Locate, Locate must not return to
    // Locate). `fallback` is the screen's natural parent.
    Screen escapeTarget(Screen self, Screen fallback) const;
    void loadMarks();
    bool saveMarks();
    void addMark(Mark mark);
    void markHere();
    void markNodePosition(uint32_t node);
    void deleteMark(int index);
    void openQuickMessages();
    void sendQuickMessage(int index);
    void sendSos();
    void cycleAutoShare();
    void tickAutoShare(uint64_t now);

    // ---- compass
    void loadCompassCalibration();
    void saveCompassCalibration();
    void pollCompass(uint64_t now);
    void toggleCompassCalibration();
    void alignCompassToTrack();

    // ---- the moves every screen makes
    // Puts `text` on the hint bar for `ms` and schedules a repaint.
    void showStatus(const std::string& text, uint64_t ms = 3000);
    // The root menu from any screen: page, selection, screen.
    void openRootMenu();
    void openHelp();
    void openMarks();
    // Up/Down/PageUp/PageDown on a list, with the cue and the repaint. False
    // for any other key, so the caller goes on to its own bindings.
    bool navigateList(ListState& st, const KeyEvent& e, int count, int rows);
    // The two "not ready" refusals, each with its dialog; true when ready.
    bool requireFcReady();
    bool requireRadioReady();
    // True, with the dialog shown, when no more places can be saved.
    bool marksFull();
    // Ends a field check early with `why` on the screen and a report built
    // from whatever had arrived.
    void abortFieldCheck(const std::string& why);
    // Closes the receiver and forgets the probe that was running on it.
    void stopGnss();

    // ---- modal helpers
    void confirm(const std::string& title, const std::string& body,
                 const std::string& yesLabel, std::function<void()> onYes,
                 std::function<void()> onNo = nullptr);
    void notice(const std::string& title, const std::string& body,
                HudCue cue = HudCue::Error);
    // A one-line text entry. Enter hands the text to `onSubmit`; Escape
    // discards it. Empty text is submitted as empty and the caller decides.
    void prompt(const std::string& title, const std::string& body,
                const std::string& initial, size_t maxBytes,
                std::function<void(const std::string&)> onSubmit);
    void closeModal();
    void setScreen(Screen s);
    void openReturnableScreen(Screen s);

    int bodyRows(bool withInput) const;
    int columns() const;

    // ---- state
    Options opt_;
    Display display_;
    Backlight backlight_;
    Keyboard keyboard_;
    Audio audio_;
    Storage storage_;
    Battery battery_;
    Config config_;
    Terminal term_;
    Completer completer_;
    LineEditor editor_;
    Session session_{term_, completer_};
    MeshSession mesh_{term_};
    Gnss gnss_;
    Compass compass_;
    ThermalTrip thermalTrip_;

    Screen screen_ = Screen::Ports;
    // Auxiliary screens return to the exact surface that launched them. This
    // keeps menu-owned tools inside their category while preserving terminal
    // and port-picker shortcuts.
    Screen returnScreen_ = Screen::Terminal;
    bool running_ = true;
    bool dirty_ = true;
    uint64_t frame_ = 0;

    std::vector<PortInfo> ports_;
    ListState portList_;
    std::vector<BackupFile> files_;
    ListState fileList_;
    ListState diagnosticList_;
    std::vector<MenuItem> menu_;
    ListState menuList_;
    std::vector<MenuItem> controllerMenu_;
    std::vector<MenuItem> backupMenu_;
    std::vector<MenuItem> meshMenu_;
    std::vector<MenuItem> meshPositionMenu_;
    std::vector<MenuItem> controlsMenu_;
    std::vector<MenuItem> settingsMenu_;
    std::vector<MenuItem> connectionMenu_;
    std::vector<MenuItem> linkSpeedMenu_;
    ListState submenuList_;
    MenuPage menuPage_ = MenuPage::Root;
    std::vector<MenuItem> quick_;
    ListState quickList_;
    ListState keymapList_;
    int helpScroll_ = 0;
    int fcBaud_ = 115200;
    int meshBaud_ = 115200;

    // ---- mesh state
    LinkMode linkMode_ = LinkMode::Betaflight;
    // The protocol the port picker will use for the highlighted row. It starts
    // from the port's own USB identity and the operator can override it.
    LinkMode portLinkMode_ = LinkMode::Betaflight;
    bool portLinkModeForced_ = false;
    ListState nodeList_;
    uint64_t nodeSequenceSeen_ = 0;
    uint64_t chatSequenceSeen_ = 0;
    uint64_t meshNoteSeen_ = 0;
    uint32_t chatPeer_ = kMeshBroadcast;
    LineEditor chatEditor_;
    struct ChatRow {
        std::string text;
        LineKind kind = LineKind::Fc;
    };
    std::vector<ChatRow> chatRows_;
    uint64_t chatRowsSequence_ = 0;
    uint32_t chatRowsPeer_ = 0;
    int chatRowsCols_ = 0;
    bool chatRowsValid_ = false;
    int chatScroll_ = 0;
    bool chatFollow_ = true;
    std::string gnssDevice_;
    int gnssBaud_ = 115200;
    bool gnssWanted_ = true;
    uint64_t gnssProbeDeadlineMs_ = 0;
    bool gnssProbeReported_ = false;
    int gnssProbeFirstBaud_ = 0;         // the rate the probe started from
    std::vector<int> gnssProbeTried_;    // rates tried so far in this probe
    bool meshFailureReported_ = false;
    int meshUnreadSeen_ = 0;

    // ---- field tools state
    std::vector<Mark> marks_;
    ListState markList_;
    std::vector<std::string> quickMessages_;
    std::vector<MenuItem> quickMsgMenu_;
    ListState quickMsgList_;
    // Exactly one of these names the Locate target: a node number, or an
    // index into marks_. Zero and -1 mean "not that kind of target".
    uint32_t locateNode_ = 0;
    int locateMark_ = -1;
    // Session-only. Never written to config.ini: the next launch starts with
    // it off, so nothing is ever transmitted that somebody did not switch on
    // during this very session.
    int autoShareMinutes_ = 0;
    uint64_t lastAutoShareMs_ = 0;
    // Ages on the node list and the Locate screen tick over without a key
    // press; this paces those repaints.
    uint64_t lastAgeRepaintMs_ = 0;
    uint64_t compassSampleSeen_ = 0;

    int brightness_ = 100;
    int soundVolume_ = 70;
    bool soundEnabled_ = true;
    std::string lastAudioError_;

    // Read-only status/tasks/version capture and its evidence-bounded summary.
    DiagnosticReport diagnosticReport_;
    std::string diagnosticStatus_, diagnosticTasks_, diagnosticVersion_;
    std::string diagnosticError_;
    int diagnosticStep_ = 0;
    bool diagnosticRunning_ = false;

    // Completion feedback shown under the input line.
    std::vector<std::string> completions_;
    std::string completionNote_;

    // Key tester (Keymap screen) shows the last raw event as received.
    KeyEvent lastKey_;
    bool haveLastKey_ = false;

    // Modal dialog.
    bool modal_ = false;
    bool modalIsConfirm_ = false;
    bool modalIsInput_ = false;
    std::string modalTitle_, modalBody_, modalYes_;
    std::function<void()> modalAction_;
    std::function<void()> modalCancelAction_;
    LineEditor modalEditor_;
    size_t modalInputMax_ = 0;
    std::function<void(const std::string&)> modalInputAction_;

    std::string status_;
    uint64_t statusUntil_ = 0;
    bool linkLossHandled_ = false;
    uint64_t nextTemperatureCheckMs_ = 0;
    bool temperatureMonitorStarted_ = false;
    int temperatureAlarmLevel_ = 0;
    bool temperatureWarningPending_ = false;
    int temperatureWarningC_ = 0;
    uint64_t lastTemperatureSequence_ = 0;
    bool thermalTripPromptPending_ = false;
    bool thermalTripAttempted_ = false;
    bool disconnectAfterVtxRestore_ = false;
    bool exitAfterVtxRestore_ = false;
};

// Renders every screen once into `dir` as PPM files, for host-side inspection.
int writePreviews(const std::string& dir);

} // namespace bf
