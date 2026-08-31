#pragma once
#include "bfcommands.h"
#include "bfsession.h"
#include "display.h"
#include "gfx.h"
#include "input.h"
#include "serialport.h"
#include "storage.h"
#include "term.h"

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
    Keymap,
    Help,
    About,
};

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
        int baud = 115200;
        bool headless = false;         // render offscreen only
        bool stdinKeys = false;        // read stdin instead of evdev
        bool simulate = false;         // talk to the built-in fake FC
        bool autoConnect = true;
        bool showAbout = false;        // --about: start offline on the credits
        int frameLimit = 0;            // stop after N frames (preview/testing)
        std::string previewDir;        // dump each screen as PPM and exit
    };

    int run(const Options& opt);

private:
    friend int runSelfTest();
    // ---- lifecycle
    bool setup(const Options& opt, std::string& error);
    void teardown();
    void tick(uint64_t now);
    void render();

    // ---- input
    void handleKey(const KeyEvent& e);
    void onPortsKey(const KeyEvent& e);
    void onTerminalKey(const KeyEvent& e);
    void onMenuKey(const KeyEvent& e);
    void onFilesKey(const KeyEvent& e);
    void onKeymapKey(const KeyEvent& e);
    void onHelpKey(const KeyEvent& e);
    void onAboutKey(const KeyEvent& e);
    bool handleModalKey(const KeyEvent& e);

    // ---- screens
    void drawTopBar(Surface& s);
    void drawHintBar(Surface& s, const std::string& hints);
    void drawPorts(Surface& s);
    void drawTerminal(Surface& s);
    void drawMenu(Surface& s);
    void drawFiles(Surface& s);
    void drawKeymap(Surface& s);
    void drawHelp(Surface& s);
    void drawAbout(Surface& s);
    void drawModal(Surface& s);
    void drawList(Surface& s, const std::vector<MenuItem>& items, ListState& st,
                  int visibleRows);

    // ---- actions
    void refreshPorts();
    void connectSelected();
    void doDisconnect();
    void submitLine();
    void doComplete();
    void runBackup(const std::string& command, const std::string& label);
    void refreshFiles();
    void viewFile(const BackupFile& f);
    void restoreFile(const BackupFile& f);
    void deleteFile(const BackupFile& f);
    void applyQuick(int id);
    void applyMenu(int id);
    void adjustBrightness(int delta);
    void pushLocal(const std::string& text, LineKind kind = LineKind::Local);

    // ---- modal helpers
    void confirm(const std::string& title, const std::string& body,
                 const std::string& yesLabel, std::function<void()> onYes);
    void notice(const std::string& title, const std::string& body);
    void closeModal();
    void setScreen(Screen s);

    int bodyRows(bool withInput) const;
    int columns() const;

    // ---- state
    Options opt_;
    Display display_;
    Backlight backlight_;
    Keyboard keyboard_;
    Storage storage_;
    Config config_;
    Terminal term_;
    Completer completer_;
    LineEditor editor_;
    Session session_{term_, completer_};

    Screen screen_ = Screen::Ports;
    Screen returnScreen_ = Screen::Terminal;
    bool running_ = true;
    bool dirty_ = true;
    uint64_t frame_ = 0;

    std::vector<PortInfo> ports_;
    ListState portList_;
    std::vector<BackupFile> files_;
    ListState fileList_;
    std::vector<MenuItem> menu_;
    ListState menuList_;
    std::vector<MenuItem> quick_;
    ListState quickList_;
    ListState keymapList_;
    int helpScroll_ = 0;
    int baudIndex_ = 0;
    int brightness_ = 100;

    // Completion feedback shown under the input line.
    std::vector<std::string> completions_;
    std::string completionNote_;

    // Key tester (Keymap screen) shows the last raw event as received.
    KeyEvent lastKey_;
    bool haveLastKey_ = false;

    // Modal dialog.
    bool modal_ = false;
    bool modalIsConfirm_ = false;
    std::string modalTitle_, modalBody_, modalYes_;
    std::function<void()> modalAction_;

    std::string status_;
    uint64_t statusUntil_ = 0;
    bool linkLossHandled_ = false;
};

// Renders every screen once into `dir` as PPM files, for host-side inspection.
int writePreviews(const std::string& dir);

} // namespace bf
