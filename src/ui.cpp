#include "app.h"
#include "brand.h"
#include "mascot.h"
#include "strutil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace bf {
namespace {

std::string meshStateText(MeshState s) {
    switch (s) {
    case MeshState::Disconnected: return "offline";
    case MeshState::Waking:       return "waking";
    case MeshState::Configuring:  return "node sync";
    case MeshState::Ready:        return "ready";
    case MeshState::Failed:       return "failed";
    }
    return "?";
}

std::string stateText(SessionState s) {
    switch (s) {
    case SessionState::Disconnected: return "offline";
    case SessionState::ProbingMsp:   return "checking VTX";
    case SessionState::AwaitingVtxChoice: return "VTX choice";
    case SessionState::ApplyingVtxGuard:  return "setting pit";
    case SessionState::EnteringCli:  return "opening CLI";
    case SessionState::Ready:        return "ready";
    case SessionState::Busy:         return "busy";
    case SessionState::Rebooting:    return "rebooting";
    case SessionState::Failed:       return "failed";
    }
    return "?";
}

Color colorFor(LineKind k) {
    switch (k) {
    case LineKind::Echo:  return theme::echo;
    case LineKind::Local: return theme::accent;
    case LineKind::Good:  return theme::ok;
    case LineKind::Warn:  return theme::warn;
    case LineKind::Error: return theme::err;
    case LineKind::Fc:
    default:              return theme::text;
    }
}

// The pack's own three-step alarm ladder, deliberately the same green/amber/red
// the state chip and the field check already use. Charging is not a level, so
// it is spelled with a leading "+" in the text rather than a fourth colour that
// would have to be learned.
Color colorFor(const BatteryReading& b) {
    if (!b.known()) return theme::textDim;
    if (b.percent < 15) return theme::err;
    if (b.percent < 40) return theme::warn;
    return theme::ok;
}

Color colorFor(DiagnosticLevel level) {
    switch (level) {
    case DiagnosticLevel::Pass:    return theme::ok;
    case DiagnosticLevel::Info:    return theme::text;
    case DiagnosticLevel::Warning: return theme::warn;
    case DiagnosticLevel::Failure: return theme::err;
    case DiagnosticLevel::Unknown: return theme::textDim;
    }
    return theme::textDim;
}

const char* markerFor(DiagnosticLevel level) {
    switch (level) {
    case DiagnosticLevel::Pass:    return "OK";
    case DiagnosticLevel::Info:    return "--";
    case DiagnosticLevel::Warning: return "!!";
    case DiagnosticLevel::Failure: return "XX";
    case DiagnosticLevel::Unknown: return "??";
    }
    return "??";
}

// Wraps `text` (which may contain explicit newlines) to `cols` columns.
std::vector<std::string> wrapText(const std::string& text, int cols) {
    std::vector<std::string> out;
    // A width of zero makes every cut a zero-length one, and the loop below
    // never shortens the paragraph. One column always makes progress.
    if (cols < 1) cols = 1;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string para = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        if (para.empty()) out.push_back("");
        while (!para.empty()) {
            if (static_cast<int>(para.size()) <= cols) {
                out.push_back(para);
                break;
            }
            size_t cut = para.rfind(' ', static_cast<size_t>(cols));
            if (cut == std::string::npos || cut == 0) cut = static_cast<size_t>(cols);
            out.push_back(para.substr(0, cut));
            para.erase(0, cut == static_cast<size_t>(cols) ? cut : cut + 1);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// "4m", "2h", "-": how long ago this station, or failing that the radio's own
// database, last heard a node.
std::string heardAgeText(const MeshNode& node, uint64_t now) {
    if (node.heardLocalMs != 0) return meshAgeText((now - node.heardLocalMs) / 1000);
    if (node.lastHeard != 0) {
        const int64_t age = static_cast<int64_t>(::time(nullptr)) -
                            static_cast<int64_t>(node.lastHeard);
        return age > 0 ? meshAgeText(static_cast<uint64_t>(age)) : "now";
    }
    return "-";
}

// "  snr 6.2  1 hop  87%": what the radio knows about a node, in the order
// the node list and the Locate screen both print it.
std::string nodeEvidenceText(const MeshNode& node) {
    std::string out;
    if (node.haveSnr) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "  snr %.1f", static_cast<double>(node.snr));
        out += buf;
    }
    if (node.haveHops) {
        out += "  " + std::to_string(node.hopsAway) + (node.hopsAway == 1 ? " hop" : " hops");
    }
    if (node.haveBattery) out += "  " + std::to_string(node.batteryLevel) + "%";
    return out;
}

const char* const kHelpText[] = {
    "GNDHOG ZERO - Betaflight CLI for Cardputer Zero",
    "",
    "TYPING SYMBOLS",
    "  Betaflight parameters are snake_case, so the two",
    "  characters you need most are placed under Sym:",
    "    Sym + L  ->  _        Sym + 0  ->  =",
    "  Hold Sym (bottom-right) and tap the key.",
    "  Menu > Keymap shows the whole Sym layer and lets",
    "  you test any key. If one is wrong on your unit,",
    "  fix it in config.ini - no rebuild needed.",
    "",
    "EDITING",
    "  Fn+Z / Fn+C   move left / right",
    "  Fn+K / Fn+N   start / end of line",
    "  Fn+Backspace  delete forward",
    "  Ctrl+U / K    kill to start / end",
    "  Ctrl+W        delete previous word",
    "  Ctrl+C        clear the line",
    "  Ctrl+L        clear the screen",
    "  Tab           complete a command or parameter",
    "",
    "HISTORY AND SCROLLING",
    "  Fn+F / Fn+X   previous / next command",
    "  Fn+L / Fn+M   scroll output up / down",
    "",
    "SHORTCUTS (hold Fn, tap a number)",
    "  Fn+1 help        Fn+2 field check Fn+3 version",
    "  Fn+4 diff        Fn+5 backup     Fn+6 backups",
    "  Fn+7 tasks       Fn+8 save       Fn+9 menu",
    "  Fn+0 disconnect",
    "",
    "FIELD CHECK",
    "  Reads status, tasks, and version without changing",
    "  FC configuration. It separates CLI/MSP blockers",
    "  from current arming faults and keeps raw output.",
    "  MCU temperature is watched every 30s while idle;",
    "  closing the serial link does not cut USB power.",
    "  It is a snapshot, not an airworthiness verdict.",
    "",
    "BACKUP AND RESTORE",
    "  Backup runs `diff all` and writes a file named",
    "  the way Betaflight Configurator names it, so the",
    "  two are interchangeable.",
    "  Restore sends the file line by line and waits for",
    "  each prompt. Nothing is kept until you `save`.",
    "",
    "SAFETY",
    "  A command that can spin a motor or wipe settings",
    "  asks first. A ready VTX can enter verified pit",
    "  mode before CLI. Disconnect can reboot the FC to",
    "  restore its saved flight state. Cancel leaves pit",
    "  mode active until the FC reboots or loses power.",
    "",
    "CONNECTION",
    "  Set the Host/Slave switch to HOST and plug the FC",
    "  into the USB-A port. It appears as ttyACM0.",
    "  A spare FC UART can be wired to Grove instead;",
    "  pick the port on the connect screen, and press B",
    "  to set the rate of the protocol Enter will open.",
    "  The picker proposes a protocol from the USB",
    "  identity. Press M to override it before Enter.",
    "",
    "BAUD RATES",
    "  The flight controller, the Meshtastic radio and",
    "  the GNSS receiver each keep their own rate, as",
    "  fc.baud, mesh.baud and gnss.baud in config.ini.",
    "  Set them in Menu > Connection & exit > Baud",
    "  rates, or with B on the port picker. USB CDC",
    "  negotiates its own rate and ignores these; a",
    "  wired UART does not, and a wrong rate is a port",
    "  that opens perfectly and then says nothing.",
    "  The receiver is the exception: a wire that talks",
    "  but not in NMEA is reopened at each rate in the",
    "  table, and the rate that answers is saved.",
    "",
    "BATTERY",
    "  The top bar draws this device's own pack: a",
    "  filled cell, the percentage, and a + while it is",
    "  charging. Green, then amber, then red. A machine",
    "  with no fuel gauge draws nothing at all rather",
    "  than showing a level it had to invent.",
    "",
    "MESHTASTIC",
    "  A Meshtastic radio (M5Stack Unit C6L and friends)",
    "  opens as a mesh link instead of a CLI. GNDHOG",
    "  downloads the node database, then lists every",
    "  radio it has heard, and how long ago it was.",
    "  Enter opens a conversation; type and press Enter",
    "  to send. A direct message waits for the mesh",
    "  routing ACK: + delivered, . waiting, ! rejected.",
    "  A broadcast is marked sent, because the mesh does",
    "  not acknowledge one.",
    "  Transcripts are kept per node under the data dir",
    "  and reload on the next launch.",
    "  Nothing is transmitted that you did not type. The",
    "  only automatic traffic is a config request and a",
    "  five-minute heartbeat, and neither leaves the USB",
    "  cable.",
    "",
    "MESH KEYS",
    "  Enter        open the highlighted conversation",
    "  F / Fn+4     locate: range, bearing, compass rose",
    "  M            saved marks",
    "  I            radio firmware, region, and preset",
    "  G            GNSS receiver status",
    "  P / Fn+5     transmit this station's own position",
    "  L            the radio's console log",
    "  N            back to the node list from the log",
    "  Tab          quick messages, from inside a chat",
    "",
    "LOCATE",
    "  Range and true bearing from this station's fix to",
    "  a node's last reported position, on a compass",
    "  rose. With the BMM150 calibrated the rose turns",
    "  heading-up and the arrow points where to walk;",
    "  otherwise it is north-up and the dot on the rim",
    "  is your GNSS track, real only once you move. The",
    "  age of their position is shown, because a tracker",
    "  that stopped reporting is a place, not a node.",
    "",
    "COMPASS",
    "  Menu > Position & GNSS > Compass reads the",
    "  BMM150 through the accelerometer for tilt. Press",
    "  C and turn the device slowly through a full",
    "  circle, then C again: that measures the board's",
    "  own magnetism. Press A while walking straight",
    "  with a GNSS fix to align the chip's x axis with",
    "  forward. Both are saved to config.ini, with",
    "  compass.declination (east positive) hand-set.",
    "",
    "MARKS",
    "  Menu > Position & GNSS > Mark this spot saves",
    "  your own fix under a name: the car, the launch",
    "  pad. M on the Locate screen saves that node's",
    "  last position instead, which outlives the",
    "  radio's memory of it. Marks stay on this device",
    "  and are never transmitted.",
    "",
    "QUICK MESSAGES",
    "  Tab in a chat opens canned lines; Enter sends",
    "  one. {pos} becomes your coordinate, or an honest",
    "  (no GNSS fix). Replace any slot in config.ini:",
    "    quickmsg.3 = Need a spotter at the gate",
    "",
    "SOS AND AUTO-SHARE",
    "  SOS broadcast sends a help request naming this",
    "  station, with its fix in the text and a position",
    "  packet, to everyone on the channel. It asks",
    "  first. Auto-share repeats your position every",
    "  2, 5 or 15 minutes for this session only; it is",
    "  never saved, and the next launch starts off.",
    "",
    "GNSS RECEIVER",
    "  The Cap LoRa-1262 GPS's AT6668, or any NMEA GPS",
    "  unit on the Grove socket, is read as NMEA 0183 on",
    "  /dev/serial0, 115200 by default. A fix gives range",
    "  and bearing to any node that reported a position,",
    "  and can be transmitted on request. With no",
    "  receiver or no fix, GNDHOG says so instead of",
    "  inventing a coordinate. Set gnss.device in",
    "  config.ini to move it, --gnss DEV to name it for",
    "  one launch, or --no-gnss to leave it closed.",
};
constexpr int kHelpLines = static_cast<int>(sizeof(kHelpText) / sizeof(kHelpText[0]));

} // namespace

// Right-aligned against `right`, which is the left edge of the state chip. The
// pictogram is 15px of the 6x8 lattice: a 13px shell, a 2px terminal, and an
// 11px column of fill that is never allowed to vanish while the pack still has
// charge -- an empty-looking shell at 3% is the one lie that matters here.
int App::drawBatteryIndicator(Surface& s, int right) {
    if (!battery_.available()) return right;
    const BatteryReading& b = battery_.reading();
    if (!b.present) return right;

    const Color fg = colorFor(b);
    const std::string text = b.shortText();
    const int textW = textWidth(text);
    const int x = right - 4 - textW - 2 - 15;
    if (x < 0) return right;

    const int y = 2;
    rect(s, x, y, 13, 7, theme::textDim);
    fillRect(s, x + 13, y + 2, 2, 3, theme::textDim);
    if (b.percent > 0) {
        const int fillW = std::max(1, std::min(11, (b.percent * 11 + 50) / 100));
        fillRect(s, x + 1, y + 1, fillW, 5, fg);
    }
    drawText(s, x + 15 + 2, 1, text, fg);
    return x;
}

void App::drawTopBar(Surface& s) {
    fillRect(s, 0, 0, s.w, kTopH, theme::panel);
    hLine(s, 0, kTopH, s.w, theme::rule);

    drawText(s, 2, 1, kAppName, theme::accent);
    const int separatorX = 2 + textWidth(kAppName) + 3;
    vLine(s, separatorX, 2, kTopH - 4, theme::rule);

    std::string mid;
    switch (screen_) {
    case Screen::Ports:  mid = "select port"; break;
    case Screen::Menu:   mid = "menu"; break;
    case Screen::Quick:  mid = "menu"; break;
    case Screen::Files:  mid = "backups"; break;
    case Screen::Diagnostics: mid = "field check"; break;
    case Screen::Keymap: mid = "keymap"; break;
    case Screen::Help:   mid = "help"; break;
    case Screen::About:  mid = "about"; break;
    case Screen::Nodes:  mid = "mesh nodes"; break;
    case Screen::Locate: mid = "locate"; break;
    case Screen::Marks:  mid = "marks"; break;
    case Screen::QuickMsg: mid = "quick messages"; break;
    case Screen::Compass: mid = "compass"; break;
    case Screen::Chat:
        mid = peerTitle(chatPeer_);
        break;
    case Screen::Terminal:
        if (meshMode()) {
            mid = mesh_.connected() ? baseName(mesh_.device()) + " radio log" : "radio log";
        } else if (!session_.connected()) {
            // The state chip already says "offline". Keep the middle label as
            // screen context instead of printing the same state twice.
            mid = "terminal";
        } else {
            mid = baseName(session_.device());
            if (!session_.craft().empty()) mid += " " + session_.craft();
            else if (!session_.board().empty()) mid += " " + session_.board();
        }
        break;
    }
    if (meshMode() && (screen_ == Screen::Nodes || screen_ == Screen::Chat) &&
        mesh_.radio().myNodeNum != 0) {
        const MeshNode* self = mesh_.findNode(mesh_.radio().myNodeNum);
        if (self && screen_ == Screen::Nodes) mid = self->label() + "  mesh nodes";
    }

    // Right-hand state chip.
    std::string st;
    Color chip = theme::textDim;
    if (meshMode()) {
        st = meshStateText(mesh_.state());
        if (mesh_.state() == MeshState::Ready) {
            // Ready but muted is not the same as ready. The chip is the only
            // place an operator looks before typing a message.
            chip = mesh_.radio().loraReady() ? theme::ok : theme::warn;
            if (!mesh_.radio().loraReady()) st = "no TX";
        } else if (mesh_.state() == MeshState::Failed) {
            chip = theme::err;
        } else if (mesh_.state() != MeshState::Disconnected) {
            chip = theme::warn;
        }
    } else {
        st = stateText(session_.state());
        if (session_.state() == SessionState::Ready) chip = theme::ok;
        else if (session_.state() == SessionState::Busy ||
                 session_.state() == SessionState::AwaitingVtxChoice ||
                 session_.state() == SessionState::ApplyingVtxGuard ||
                 session_.state() == SessionState::Rebooting) chip = theme::warn;
        else if (session_.state() == SessionState::Failed) chip = theme::err;
    }
    const int chipW = textWidth(st) + 8;
    const int chipX = s.w - chipW;
    fillRect(s, chipX, 0, chipW, kTopH, theme::panelHi);
    vLine(s, chipX, 0, kTopH, theme::rule);
    hLine(s, chipX + 1, kTopH - 1, chipW - 1, chip);
    const int batteryX = drawBatteryIndicator(s, chipX);
    const int midX = separatorX + 4;
    drawTextClipped(s, midX, 1, mid,
                    (batteryX - kGlyphW - midX) / kGlyphW, theme::text);
    drawText(s, chipX + 4, 1, st, chip);
}

void App::drawHintBar(Surface& s, const std::string& hints,
                      const std::string& action) {
    const int y = s.h - kHintH;
    fillRect(s, 0, y, s.w, kHintH, theme::panel);
    hLine(s, 0, y, s.w, theme::rule);
    const std::string shown = !status_.empty() ? status_ : hints;
    const Color c = !status_.empty() ? theme::accent : theme::textDim;
    int maxChars = columns() - 1;
    if (!action.empty()) {
        const int actionW = textWidth(action) + 8;
        const int actionX = std::max(0, s.w - actionW);
        fillRect(s, actionX, y + 1, actionW, kHintH - 1, theme::panelHi);
        vLine(s, actionX, y + 1, kHintH - 1, theme::rule);
        drawText(s, actionX + 4, y + 1, action, theme::accent);
        maxChars = std::max(0, (actionX - 4) / kGlyphW);
    }
    drawTextClipped(s, 2, y + 1, shown, maxChars, c);
}

void App::drawList(Surface& s, const std::vector<MenuItem>& items, ListState& st,
                   int visibleRows, bool showSelection) {
    const int cols = columns();
    if (items.empty()) {
        drawText(s, 4, kBodyY, "(nothing here)", theme::textDim);
        return;
    }
    for (int i = 0; i < visibleRows; ++i) {
        const int idx = st.top + i;
        if (idx >= static_cast<int>(items.size())) break;
        const int y = kBodyY + i * kGlyphH;
        const bool selected = showSelection && (idx == st.sel);
        if (selected) {
            fillRect(s, 0, y, s.w - 3, kGlyphH, theme::accent);
        }
        const Color fg = items[static_cast<size_t>(idx)].enabled
                             ? (selected ? theme::bg : theme::text)
                             : theme::textDim;
        drawTextClipped(s, 4, y, items[static_cast<size_t>(idx)].label,
                        cols - 4, fg);
        drawText(s, s.w - kGlyphW - 4, y, ">",
                 selected ? theme::bg : theme::accent);
    }
    drawScrollbar(s, s.w - 2, kBodyY, visibleRows * kGlyphH, st.top, visibleRows,
                  static_cast<int>(items.size()));
}

void App::drawMenuModal(Surface& s, const std::string& title,
                        const std::vector<MenuItem>& items, ListState& st) {
    constexpr int boxX = 14;
    constexpr int boxY = 17;
    const int boxW = s.w - 2 * boxX;
    const int boxH = s.h - kHintH - boxY - 4;
    const int rowStart = boxY + 22;
    const int visibleRows = std::max(1, (boxY + boxH - 4 - rowStart) / kGlyphH);

    fillRect(s, boxX, boxY, boxW, boxH, theme::accent);
    rect(s, boxX + 1, boxY + 1, boxW - 2, boxH - 2, theme::bg);

    const int titleMax = std::max(1, (boxW - 20) / kGlyphW);
    const std::string shown = static_cast<int>(title.size()) <= titleMax
                                  ? title
                                  : title.substr(0, static_cast<size_t>(titleMax - 1)) + "~";
    drawText(s, boxX + (boxW - textWidth(shown)) / 2, boxY + 5, shown, theme::bg);
    hLine(s, boxX + 10, boxY + 17, boxW - 20, theme::bg);

    if (items.empty()) {
        drawText(s, boxX + 10, rowStart, "(nothing here)", theme::bg);
        return;
    }

    st.clamp(static_cast<int>(items.size()), visibleRows);
    const int maxChars = std::max(1, (boxW - 20) / kGlyphW);
    for (int row = 0; row < visibleRows; ++row) {
        const int idx = st.top + row;
        if (idx >= static_cast<int>(items.size())) break;
        const int y = rowStart + row * kGlyphH;
        const bool selected = idx == st.sel;
        if (selected) {
            fillRect(s, boxX + 6, y, boxW - 12, kGlyphH, theme::bg);
        }
        const Color fg = items[static_cast<size_t>(idx)].enabled
                             ? (selected ? theme::accent : theme::bg)
                             : theme::accentDim;
        const std::string rowText = (selected ? "> " : "  ") +
                                    items[static_cast<size_t>(idx)].label;
        drawTextClipped(s, boxX + 10, y, rowText, maxChars, fg);
    }
}

void App::drawPorts(Surface& s) {
    const int rows = bodyRows(false);
    const int cols = columns();

    if (ports_.empty()) {
        drawText(s, 4, kBodyY, "No serial ports found.", theme::warn);
        drawText(s, 4, kBodyY + 2 * kGlyphH, "Set the Host/Slave switch to HOST,", theme::textDim);
        drawText(s, 4, kBodyY + 3 * kGlyphH, "plug the FC or radio into USB-A,", theme::textDim);
        drawText(s, 4, kBodyY + 4 * kGlyphH, "then press R to rescan.", theme::textDim);
    } else {
        for (int i = 0; i < rows; ++i) {
            const int idx = portList_.top + i;
            if (idx >= static_cast<int>(ports_.size())) break;
            const PortInfo& p = ports_[static_cast<size_t>(idx)];
            const int y = kBodyY + i * kGlyphH;
            const bool selected = (idx == portList_.sel);
            drawRowSelection(s, y, selected);
            drawText(s, 2, y, selected ? ">" : " ", theme::accent);
            Color fg = theme::text;
            std::string text = p.label();
            if (isGnssPort(p.device)) {
                // The receiver holds this node right now, so Enter will not
                // open a link on it while it keeps proving itself.
                text += gnss_.receiverPresent() ? "  GNSS live" : "  GNSS probe";
                fg = gnss_.receiverPresent() ? theme::accent : theme::textDim;
            } else {
                // A radio on USB and a receiver on the UART are listed
                // together, and "USB JTAG/serial debug unit" says nothing
                // about which of them is the radio. The role goes on the row.
                const std::string role = p.roleTag();
                if (!role.empty()) text += "  " + role;
                if (isDfuId(p.vendorId, p.productId)) fg = theme::warn;
                else if (p.prefersMeshtastic() && p.looksLikeMeshtastic()) fg = theme::echo;
                else if (p.looksLikeFlightController()) fg = theme::ok;
            }
            drawTextClipped(s, 2 + kGlyphW, y, text, cols - 3, fg);
        }
        drawScrollbar(s, s.w - 2, kBodyY, rows * kGlyphH, portList_.top, rows,
                      static_cast<int>(ports_.size()));
    }

    std::string hint = "R scan  M protocol  F files  G GNSS  H help  A about";
    bool selectedIsReceiver = false;
    if (!ports_.empty()) {
        const PortInfo& p = ports_[static_cast<size_t>(portList_.sel)];
        selectedIsReceiver = isGnssPort(p.device) && gnss_.receiverPresent();
        std::string detail = p.detail();
        if (selectedIsReceiver) {
            // What the wire is saying beats what the rate table would do to it.
            detail = gnss_.statusText(nowMs());
        } else if (p.kind == "uart") {
            // Name the peer the rate belongs to: the same wire carries a very
            // different conversation depending on which protocol Enter opens.
            detail += (detail.empty() ? "" : "  ") +
                      std::string(portLinkMode_ == LinkMode::Meshtastic ? "mesh " : "FC ") +
                      std::to_string(linkBaud(portLinkMode_)) + "  B changes";
        }
        if (!detail.empty()) hint = detail + (selectedIsReceiver ? "" : "  M protocol");
    }
    const std::string action =
        ports_.empty()       ? "Esc quit"
        : selectedIsReceiver ? "Enter GNSS"
        : (portLinkMode_ == LinkMode::Meshtastic ? "Enter mesh" : "Enter CLI");
    drawHintBar(s, hint, action);
}

void App::drawTerminal(Surface& s) {
    const int cols = columns();
    // A radio has no prompt, so its console gets the whole body instead of
    // leaving a row for an input line that could not do anything.
    const bool withInput = !meshMode();
    const int rows = bodyRows(withInput);
    term_.setWidth(cols - 1);   // leave a column for the scrollbar

    const int total = static_cast<int>(term_.rowCount());
    const int first = std::max(0, std::min(term_.scroll(), std::max(0, total - rows)));
    for (int i = 0; i < rows; ++i) {
        const int idx = first + i;
        if (idx >= total) break;
        drawText(s, 1, kBodyY + i * kGlyphH, term_.rowText(static_cast<size_t>(idx)),
                 colorFor(term_.rowKind(static_cast<size_t>(idx))));
    }
    drawScrollbar(s, s.w - 2, kBodyY, rows * kGlyphH, first, rows, total);

    if (!withInput) {
        std::string hints = term_.following() ? "N nodes  I radio  G GNSS  C clear"
                                              : "scrolled back - Fn+M returns to live";
        drawHintBar(s, hints, "Esc menu");
        return;
    }

    // A running job replaces the input line with a progress bar.
    const int inputY = s.h - kHintH - kInputH;
    hLine(s, 0, inputY, s.w, theme::rule);

    const JobStatus& job = session_.job();
    if (job.active()) {
        const std::string label = job.total > 0
                                      ? job.label + "  " + std::to_string(job.done) + "/" +
                                            std::to_string(job.total)
                                      : job.label;
        drawTextClipped(s, 2, inputY + 2, label, cols - 12, theme::accent);
        drawProgress(s, s.w - 66, inputY + 2, 64, 8,
                     job.total > 0 ? job.fraction() : 0.35f, theme::accent, theme::bg);
        drawHintBar(s, "", "Esc cancel");
        return;
    }

    // Completion candidates take over the body's last rows when present.
    if (!completions_.empty()) {
        const int colW = 11;
        const int perRow = std::max(1, (cols - 1) / colW);
        const int listRows = std::min(
            3, static_cast<int>((completions_.size() + perRow - 1) / perRow));
        const int y0 = inputY - listRows * kGlyphH;
        fillRect(s, 0, y0, s.w, listRows * kGlyphH, theme::panel);
        int col = 0, row = 0;
        for (const std::string& c : completions_) {
            if (row >= listRows) break;
            drawTextClipped(s, 2 + col * colW * kGlyphW, y0 + row * kGlyphH, c, colW - 1,
                            theme::textDim);
            if (++col >= perRow) { col = 0; ++row; }
        }
    }

    drawInputLine(s, 2, inputY + 2, editor_, cols - 3, "#",
                  session_.ready() ? theme::accent : theme::textDim, true);

    std::string hints;
    if (!completionNote_.empty()) hints = completionNote_ + "   ";
    if (!session_.connected()) hints += "not connected";
    else if (!term_.following()) hints += "scrolled back - Fn+M returns to live";
    else hints += "Tab complete  Fn+F history  Fn+1 help";
    drawHintBar(s, hints, "Esc menu");
}

void App::drawMenu(Surface& s) {
    const bool quick = (screen_ == Screen::Quick);
    std::vector<MenuItem>& items = quick ? quick_ : currentMenuItems();
    ListState& st = quick ? quickList_ : currentMenuList();
    const int rows = bodyRows(false);
    if (!quick && menuPage_ == MenuPage::Root) {
        st.clamp(static_cast<int>(items.size()), rows);
        drawList(s, items, st, rows);
    } else {
        ListState backdrop = menuList_;
        backdrop.clamp(static_cast<int>(menu_.size()), rows);
        drawList(s, menu_, backdrop, rows, false);

        std::string title = "Quick commands";
        if (!quick) {
            switch (menuPage_) {
            case MenuPage::FlightController: title = "Flight controller"; break;
            case MenuPage::BackupRestore:    title = "Backup & restore"; break;
            case MenuPage::Mesh:             title = "Mesh network"; break;
            case MenuPage::MeshPosition:     title = "Position & GNSS"; break;
            case MenuPage::ControlsInfo:     title = "Controls & info"; break;
            case MenuPage::SoundDisplay:     title = "Sound & display"; break;
            case MenuPage::ConnectionExit:   title = "Connection & exit"; break;
            case MenuPage::LinkSpeeds:       title = "Baud rates"; break;
            case MenuPage::Root:             title = "Menu"; break;
            }
        }
        drawMenuModal(s, title, items, st);
    }

    std::string hint = "Enter select   Esc back";
    if (!items.empty()) {
        const std::string& h = items[static_cast<size_t>(st.sel)].hint;
        if (!h.empty()) hint = h;
    }
    drawHintBar(s, hint, "Esc back");
}

void App::drawFiles(Surface& s) {
    const int rows = bodyRows(false);
    const int cols = columns();
    fileList_.clamp(static_cast<int>(files_.size()), rows);

    if (files_.empty()) {
        drawText(s, 4, kBodyY, "No backups yet.", theme::warn);
        drawText(s, 4, kBodyY + 2 * kGlyphH, "Connect to a flight controller, then use",
                 theme::textDim);
        drawText(s, 4, kBodyY + 3 * kGlyphH, "Menu > Backup config to file (or Fn+5).",
                 theme::textDim);
        drawHintBar(s, storage_.backupDir(), "Esc back");
        return;
    }

    for (int i = 0; i < rows; ++i) {
        const int idx = fileList_.top + i;
        if (idx >= static_cast<int>(files_.size())) break;
        const BackupFile& f = files_[static_cast<size_t>(idx)];
        const int y = kBodyY + i * kGlyphH;
        const bool selected = (idx == fileList_.sel);
        drawRowSelection(s, y, selected);
        drawText(s, 2, y, selected ? ">" : " ", theme::accent);
        // Trim the fixed BTFL_cli_ prefix: it is on every file and wastes width.
        std::string name = f.name;
        if (startsWith(name, "BTFL_cli_")) name = name.substr(9);
        if (name.size() > 11 && endsWith(name, "_backup.txt")) name.resize(name.size() - 11);
        const std::string size = f.sizeText();
        drawTextClipped(s, 2 + kGlyphW, y, name, cols - 4 - static_cast<int>(size.size()),
                        selected ? theme::accent : theme::text);
        drawText(s, s.w - 4 - textWidth(size), y, size, theme::textDim);
    }
    drawScrollbar(s, s.w - 2, kBodyY, rows * kGlyphH, fileList_.top, rows,
                  static_cast<int>(files_.size()));

    const BackupFile& sel = files_[static_cast<size_t>(fileList_.sel)];
    drawHintBar(s, sel.dateText() + "   Enter restore   V view   D delete", "Esc back");
}

void App::drawDiagnostics(Surface& s) {
    const int cols = columns();
    constexpr int summaryH = 27;
    const int listY = kBodyY + summaryH + 2;
    const int rows = std::max(1, (s.h - kHintH - listY) / kGlyphH);

    fillRect(s, 0, kBodyY, s.w, summaryH, theme::panel);
    hLine(s, 0, kBodyY + summaryH - 1, s.w, theme::rule);

    std::string summary;
    std::string summaryCount;
    Color summaryColor = theme::accent;
    if (diagnosticRunning_) {
        summary = "FIELD CHECK RUNNING";
    } else if (!diagnosticReport_.complete()) {
        summary = "INCOMPLETE";
        summaryColor = theme::warn;
        const int missing = (diagnosticReport_.statusAvailable ? 0 : 1) +
                            (diagnosticReport_.tasksAvailable ? 0 : 1) +
                            (diagnosticReport_.versionAvailable ? 0 : 1);
        summaryCount = std::to_string(missing) + " MISSING";
    } else if (diagnosticReport_.actionableBlockerCount() > 0) {
        summary = "ARM BLOCKED";
        summaryColor = theme::err;
        summaryCount = std::to_string(diagnosticReport_.actionableBlockerCount()) + " BLOCK";
    } else if (diagnosticReport_.failureCount() > 0) {
        summary = "FAULT REPORTED";
        summaryColor = theme::err;
        summaryCount = std::to_string(diagnosticReport_.failureCount()) + " FAIL";
    } else if (diagnosticReport_.warningCount() > 0) {
        summary = "CHECK FINDINGS";
        summaryColor = theme::warn;
        summaryCount = std::to_string(diagnosticReport_.warningCount()) + " REVIEW";
    } else {
        summary = "NO ACTIVE FAULTS";
        summaryColor = theme::ok;
    }
    drawText(s, 4, kBodyY + 2, summary, summaryColor);
    if (!summaryCount.empty()) {
        drawText(s, s.w - textWidth(summaryCount) - 4, kBodyY + 2,
                 summaryCount, summaryColor);
    }

    if (diagnosticRunning_) {
        const int shownStep = std::min(3, diagnosticStep_ + 1);
        const std::string progress = std::to_string(shownStep) + "/3";
        drawText(s, s.w - textWidth(progress) - 4, kBodyY + 2, progress, theme::accent);
        const std::string detail = session_.job().label.empty()
                                       ? "waiting for flight controller"
                                       : session_.job().label;
        drawTextClipped(s, 4, kBodyY + 13, detail, cols - 2, theme::textDim);
        drawProgress(s, 4, listY + 10, s.w - 8, 9,
                     static_cast<float>(diagnosticStep_) / 3.0f, theme::accent, theme::bg);
        drawHintBar(s, "no config writes; raw output stays in terminal", "Esc cancel");
        return;
    }

    const int count = static_cast<int>(diagnosticReport_.findings.size());
    diagnosticList_.clamp(count, rows);
    std::string sub = !diagnosticError_.empty()
                          ? diagnosticError_
                          : "CLI snapshot only - inspect the craft props-off";
    Color subColor = diagnosticError_.empty() ? theme::textDim : theme::warn;
    if (diagnosticError_.empty() && count > 0) {
        const DiagnosticFinding& selected =
            diagnosticReport_.findings[static_cast<size_t>(diagnosticList_.sel)];
        const std::string rowText = selected.title + "  " + selected.detail;
        if (static_cast<int>(rowText.size()) > cols - 5) {
            // The row stays compact; selecting it exposes more of the detail
            // in the wider summary line instead of silently clipping it.
            sub = selected.detail;
            subColor = colorFor(selected.level);
        }
    }
    drawTextClipped(s, 4, kBodyY + 13, sub, cols - 2, subColor);

    for (int i = 0; i < rows; ++i) {
        const int index = diagnosticList_.top + i;
        if (index >= count) break;
        const DiagnosticFinding& finding = diagnosticReport_.findings[static_cast<size_t>(index)];
        const int y = listY + i * kGlyphH;
        const bool selected = index == diagnosticList_.sel;
        if (selected) fillRect(s, 0, y, s.w - 3, kGlyphH, theme::panelHi);
        const Color c = colorFor(finding.level);
        drawText(s, 2, y, markerFor(finding.level), c);
        drawTextClipped(s, 2 + 3 * kGlyphW, y,
                        finding.title + "  " + finding.detail, cols - 5,
                        selected ? c : (finding.level == DiagnosticLevel::Info ? theme::textDim : c));
    }
    drawScrollbar(s, s.w - 2, listY, rows * kGlyphH,
                  diagnosticList_.top, rows, count);
    drawHintBar(s, "R rerun  S save report  V raw terminal", "Esc back");
}

void App::drawKeymap(Surface& s) {
    const int cols = columns();
    int y = kBodyY;

    drawText(s, 2, y, "Press any key to identify it:", theme::accent);
    y += kGlyphH + 2;

    if (haveLastKey_) {
        char buf[128];
        const KeyDescriptor* d = lastKey_.scan >= 0 ? descriptorForScan(lastKey_.scan) : nullptr;
        std::snprintf(buf, sizeof(buf), "scan 0x%02X  code %d  layer %s  key %s",
                      lastKey_.scan >= 0 ? lastKey_.scan : 0, lastKey_.code,
                      layerName(lastKey_.layer), keyName(lastKey_.key));
        drawTextClipped(s, 2, y, buf, cols - 1, theme::text);
        y += kGlyphH;

        std::string produced = "produces: ";
        if (lastKey_.key == Key::Char) {
            produced += std::string("'") + lastKey_.ch + "'";
        } else {
            produced += keyName(lastKey_.key);
        }
        if (d) produced += std::string("   physical key: ") + d->label;
        if (lastKey_.shift) produced += "  +shift";
        if (lastKey_.ctrl) produced += "  +ctrl";
        drawTextClipped(s, 2, y, produced, cols - 1, theme::ok);
    } else {
        drawText(s, 2, y, "(waiting)", theme::textDim);
        y += kGlyphH;
    }
    y += kGlyphH + 2;
    hLine(s, 0, y - 2, s.w, theme::rule);

    drawText(s, 2, y, "SYM LAYER - hold Sym and tap:", theme::accent);
    y += kGlyphH + 1;

    // Compact grid of every Sym position that produces a character.
    const int cellW = 9;
    const int perRow = std::max(1, (cols - 1) / cellW);
    int cell = 0;
    for (int i = 0; i < descriptorCount(); ++i) {
        const KeyDescriptor* d = descriptorAt(i);
        if (!d) continue;
        const char c = keyboard_.decoder().symChar(d->scan);
        if (c == 0) continue;
        const int row = cell / perRow;
        const int yy = y + row * kGlyphH;
        if (yy + kGlyphH > s.h - kHintH) break;
        const int xx = 2 + (cell % perRow) * cellW * kGlyphW;
        // Highlight the two that matter most for Betaflight parameters.
        const bool star = (c == '_' || c == '=');
        std::string txt = std::string(d->label) + "=" + c;
        drawTextClipped(s, xx, yy, txt, cellW - 1, star ? theme::accent : theme::textDim);
        ++cell;
    }

    drawHintBar(s, "Override: sym.0x28 = _ in config.ini", "Esc back");
}

void App::drawHelp(Surface& s) {
    const int rows = bodyRows(false);
    const int cols = columns();
    helpScroll_ = std::max(0, std::min(helpScroll_, std::max(0, kHelpLines - rows)));
    for (int i = 0; i < rows; ++i) {
        const int idx = helpScroll_ + i;
        if (idx >= kHelpLines) break;
        const std::string line = kHelpText[idx];
        Color c = theme::text;
        if (!line.empty() && line[0] != ' ') c = theme::accent;
        drawTextClipped(s, 2, kBodyY + i * kGlyphH, line, cols - 2, c);
    }
    drawScrollbar(s, s.w - 2, kBodyY, rows * kGlyphH, helpScroll_, rows, kHelpLines);
    drawHintBar(s, "Fn+F / Fn+X scroll", "Esc back");
}

void App::drawAbout(Surface& s) {
    drawMascot(s, 4, 18);
    drawText(s, 128, 22, kAppName, theme::accent);
    drawText(s, 128, 37, "Betaflight field terminal", theme::textDim);
    drawText(s, 128, 54, std::string("author: ") + kAuthor, theme::text);
    drawText(s, 128, 71, std::string("commit: ") + kBuildCommit, theme::ok);
    drawText(s, 128, 91, "Cardputer Zero / ARM64", theme::textDim);
    drawText(s, 128, 108, "props off. shell on.", theme::accent);
    hLine(s, 4, 133, s.w - 8, theme::rule);
    drawText(s, 4, 137, kAboutHeading, theme::accent);
    drawText(s, 4, 149, kAboutJoke, theme::text);
    drawHintBar(s, "Enter returns", "Esc back");
}

void App::drawNodes(Surface& s) {
    const int rows = bodyRows(false);
    const int count = nodeRowCount();
    nodeList_.clamp(count, rows);

    const std::vector<MeshNode>& nodes = mesh_.nodes();
    const uint64_t now = nowMs();
    const GnssFix& fix = gnss_.fix();

    for (int i = 0; i < rows; ++i) {
        const int index = nodeList_.top + i;
        if (index >= count) break;
        const int y = kBodyY + i * kGlyphH;
        const bool selected = index == nodeList_.sel;
        drawRowSelection(s, y, selected);

        const uint32_t peer = peerForNodeRow(index);
        const int unread = mesh_.unread(peer);
        drawText(s, 2, y, unread > 0 ? "*" : (selected ? ">" : " "),
                 unread > 0 ? theme::warn : theme::accent);

        std::string tag, title, right;
        Color fg = theme::text;
        if (index == 0) {
            tag = "ALL ";
            const std::string& channel = mesh_.radio().primaryChannel;
            title = channel.empty() ? "Broadcast" : "Broadcast - " + channel;
            const std::vector<MeshMessage>* log = mesh_.conversation(kMeshBroadcast);
            if (log && !log->empty()) right = std::to_string(log->size()) + " msg";
            fg = theme::accent;
        } else {
            const MeshNode& node = nodes[static_cast<size_t>(index - 1)];
            tag = node.label();
            tag.resize(4, ' ');
            title = node.title();
            if (node.isSelf) {
                title += "  (this radio)";
                fg = theme::ok;
            } else if (node.heardLocalMs == 0 && node.lastHeard == 0) {
                // In the database because the radio listed it, not because we
                // have heard it ourselves.
                fg = theme::textDim;
            }
            // "Heard N ago" is a fact. "Online" would be a guess about a radio
            // that may simply have nothing to say.
            right = heardAgeText(node, now);
        }

        drawText(s, 2 + kGlyphW, y, tag, selected ? theme::accent : theme::textDim);
        const int titleX = 2 + 6 * kGlyphW;
        const int rightW = static_cast<int>(right.size());
        const int titleMax = std::max(1, (s.w - 6 - titleX) / kGlyphW - rightW - 1);
        drawTextClipped(s, titleX, y, title, titleMax, fg);
        if (!right.empty()) {
            drawText(s, s.w - 6 - textWidth(right), y, right, theme::textDim);
        }
    }
    drawScrollbar(s, s.w - 2, kBodyY, rows * kGlyphH, nodeList_.top, rows, count);

    // While the database is still arriving, say so rather than presenting a
    // one-row list as if that were the whole mesh.
    if (!mesh_.ready() && count < rows) {
        const int y = kBodyY + count * kGlyphH;
        const char* note = mesh_.state() == MeshState::Failed
                               ? "(the radio did not answer)"
                               : "(downloading the node database...)";
        drawText(s, 2 + 6 * kGlyphW, y, note,
                 mesh_.state() == MeshState::Failed ? theme::err : theme::textDim);
    }

    // The hint bar carries the evidence for the highlighted row.
    std::string hint;
    const uint32_t peer = peerForNodeRow(nodeList_.sel);
    if (nodeList_.sel == 0) {
        hint = mesh_.radio().loraSummary();
    } else {
        const MeshNode* node = mesh_.findNode(peer);
        if (node) {
            hint = node->idText() + nodeEvidenceText(*node);
            if (node->viaMqtt) hint += "  mqtt";
            // Range needs two real fixes: ours from the cap, theirs from the mesh.
            if (fix.valid && node->position.valid) {
                const double metres = meshDistanceM(fix.latitude, fix.longitude,
                                                    node->position.latitude,
                                                    node->position.longitude);
                const double bearing = meshBearingDeg(fix.latitude, fix.longitude,
                                                      node->position.latitude,
                                                      node->position.longitude);
                hint += "  " + meshRangeText(metres) + " " + meshCompassPoint(bearing);
            } else if (node->position.valid) {
                hint += "  " + node->position.coordText();
            }
        }
    }
    if (hint.empty()) hint = gnss_.statusText(now);
    drawHintBar(s, hint, count > 1 || nodeList_.sel == 0 ? "Enter chat" : "Esc menu");
}

void App::rebuildChatRows() {
    const int cols = columns();
    const int width = std::max(8, cols - 6);
    chatRows_.clear();
    chatRowsPeer_ = chatPeer_;
    chatRowsCols_ = cols;
    chatRowsSequence_ = mesh_.chatSequence();
    chatRowsValid_ = true;

    const std::vector<MeshMessage>* log = mesh_.conversation(chatPeer_);
    if (!log || log->empty()) {
        chatRows_.push_back({"No messages yet.", LineKind::Local});
        chatRows_.push_back({"", LineKind::Local});
        chatRows_.push_back({"Type below and press Enter. Nothing is sent", LineKind::Fc});
        chatRows_.push_back({"until you do, and nothing is stored on the", LineKind::Fc});
        chatRows_.push_back({"radio: this transcript lives on the SD card.", LineKind::Fc});
        return;
    }

    int64_t previousStamp = 0;
    for (const MeshMessage& message : *log) {
        // A time separator only where the conversation actually paused, so a
        // fast exchange stays dense on an eighteen-row screen.
        if (message.stampUtc > 0 && message.stampUtc - previousStamp > 300) {
            chatRows_.push_back({"-- " + formatLocalTime(message.stampUtc, "%a %H:%M") + " --",
                                 LineKind::Local});
        }
        if (message.stampUtc > 0) previousStamp = message.stampUtc;

        std::string tag;
        LineKind kind = LineKind::Fc;
        if (message.outgoing) {
            char marker = '.';
            switch (message.state) {
            case MeshMessageState::Delivered: marker = '+'; kind = LineKind::Echo; break;
            case MeshMessageState::Sent:      marker = '>'; kind = LineKind::Echo; break;
            case MeshMessageState::Failed:    marker = '!'; kind = LineKind::Error; break;
            case MeshMessageState::Queued:    marker = '.'; kind = LineKind::Warn; break;
            case MeshMessageState::Received:  marker = ' '; kind = LineKind::Echo; break;
            }
            tag = std::string("me") + marker + " ";
        } else {
            const MeshNode* from = mesh_.findNode(message.from);
            tag = from ? from->label() : meshNodeIdText(message.from).substr(1, 4);
            tag.resize(4, ' ');
            kind = LineKind::Fc;
        }

        const std::vector<std::string> wrapped = wrapText(message.text, width);
        bool first = true;
        for (const std::string& piece : wrapped) {
            chatRows_.push_back({(first ? tag + " " : std::string(5, ' ')) + piece, kind});
            first = false;
        }
        if (wrapped.empty()) chatRows_.push_back({tag + " ", kind});
        if (!message.note.empty() && message.state == MeshMessageState::Failed) {
            for (const std::string& piece : wrapText("! " + message.note, width)) {
                chatRows_.push_back({std::string(5, ' ') + piece, LineKind::Error});
            }
        }
    }
}

void App::drawChat(Surface& s) {
    const int cols = columns();
    const int rows = bodyRows(true);
    if (!chatRowsValid_ || chatRowsPeer_ != chatPeer_ || chatRowsCols_ != cols ||
        chatRowsSequence_ != mesh_.chatSequence()) {
        rebuildChatRows();
    }

    const int total = static_cast<int>(chatRows_.size());
    const int maxScroll = std::max(0, total - rows);
    if (chatFollow_) chatScroll_ = maxScroll;
    chatScroll_ = std::max(0, std::min(chatScroll_, maxScroll));

    for (int i = 0; i < rows; ++i) {
        const int index = chatScroll_ + i;
        if (index >= total) break;
        const ChatRow& row = chatRows_[static_cast<size_t>(index)];
        drawTextClipped(s, 1, kBodyY + i * kGlyphH, row.text, cols - 1, colorFor(row.kind));
    }
    drawScrollbar(s, s.w - 2, kBodyY, rows * kGlyphH, chatScroll_, rows, total);

    const int inputY = s.h - kHintH - kInputH;
    hLine(s, 0, inputY, s.w, theme::rule);

    const std::string& text = chatEditor_.text();
    const bool sendable = mesh_.ready() && mesh_.radio().loraReady();
    drawInputLine(s, 2, inputY + 2, chatEditor_, cols - 3, ">",
                  sendable ? theme::accent : theme::textDim, true);

    std::string hints;
    if (!mesh_.connected()) {
        hints = "radio disconnected - this is the saved transcript";
    } else if (!mesh_.ready()) {
        hints = "radio still syncing";
    } else if (!mesh_.radio().loraReady()) {
        hints = mesh_.radio().region == 0 ? "no LoRa region set - the radio will not transmit"
                                          : "transmit is disabled on this radio";
    } else {
        hints = std::to_string(text.size()) + "/" + std::to_string(kMeshMaxTextBytes) +
                "  Tab quick msgs";
        if (chatPeer_ == kMeshBroadcast) hints += "  no ack";
        // Only offer the key that can actually do something right now.
        if (gnss_.receiverPresent()) hints += "  Fn+5 pos";
    }
    drawHintBar(s, hints, "Esc back");
}

// ------------------------------------------------------------ field tools

void App::drawCompass(Surface& s, int cx, int cy, int r, bool haveBearing,
                      double bearingDeg, bool haveHeading, double headingDeg,
                      double rotationDeg) {
    constexpr double kPi = 3.14159265358979323846;
    // Bearings are clockwise from north; the screen's y axis points down; the
    // whole rose is turned so that `rotationDeg` sits at the top.
    auto px = [&](double deg, double len) {
        return cx + static_cast<int>(std::lround(std::sin((deg - rotationDeg) * kPi / 180.0) * len));
    };
    auto py = [&](double deg, double len) {
        return cy - static_cast<int>(std::lround(std::cos((deg - rotationDeg) * kPi / 180.0) * len));
    };

    drawCircle(s, cx, cy, r, theme::rule);
    for (int i = 0; i < 8; ++i) {
        const double deg = i * 45.0;
        const int len = (i % 2 == 0) ? 5 : 3;
        drawLine(s, px(deg, r - len), py(deg, r - len), px(deg, r), py(deg, r), theme::textDim);
    }
    // The cardinal letters ride the rim, so a heading-up rose keeps N where
    // north actually is rather than where the label used to be.
    const struct { double deg; const char* label; Color color; } kCardinals[] = {
        {0.0, "N", theme::accent}, {90.0, "E", theme::textDim},
        {180.0, "S", theme::textDim}, {270.0, "W", theme::textDim},
    };
    for (const auto& c : kCardinals) {
        drawText(s, px(c.deg, r + 9) - 3, py(c.deg, r + 9) - 4, c.label, c.color);
    }

    // The direction this station is facing (magnetometer) or moving (GNSS
    // track), on the rim. Nothing is drawn when neither is known: a
    // stationary receiver has no heading and nothing here pretends it has.
    if (haveHeading) fillCircle(s, px(headingDeg, r), py(headingDeg, r), 3, theme::ok);

    if (!haveBearing) {
        drawText(s, cx - 3, cy - 4, "?", theme::textDim);
        return;
    }
    const int tipX = px(bearingDeg, r - 6);
    const int tipY = py(bearingDeg, r - 6);
    // A three-pixel shaft reads from arm's length; one pixel does not.
    const double screenRad = (bearingDeg - rotationDeg) * kPi / 180.0;
    const int ox = static_cast<int>(std::lround(std::cos(screenRad)));
    const int oy = static_cast<int>(std::lround(std::sin(screenRad)));
    drawLine(s, cx, cy, tipX, tipY, theme::accent);
    drawLine(s, cx + ox, cy + oy, tipX + ox, tipY + oy, theme::accent);
    drawLine(s, cx - ox, cy - oy, tipX - ox, tipY - oy, theme::accent);
    for (double side : {150.0, -150.0}) {
        drawLine(s, tipX, tipY, tipX + (px(bearingDeg + side, 10) - cx),
                 tipY + (py(bearingDeg + side, 10) - cy), theme::accent);
    }
    fillCircle(s, cx, cy, 2, theme::accent);
}

void App::drawLocate(Surface& s) {
    const uint64_t now = nowMs();
    const GnssFix& fix = gnss_.fix();
    const int textX = 108;
    const int textCols = std::max(1, (s.w - textX - 2) / kGlyphW);

    // Resolve the target. Everything below is drawn from these, so a node
    // and a mark get the same screen and the same honesty about what is
    // missing.
    std::string title, subtitle, positionAge, evidence;
    bool havePosition = false, haveAltitude = false;
    double latitude = 0.0, longitude = 0.0, altitudeM = 0.0;
    const bool isMark = locateMark_ >= 0 && locateMark_ < static_cast<int>(marks_.size());
    if (locateNode_ != 0) {
        const MeshNode* node = mesh_.findNode(locateNode_);
        if (!node) {
            title = meshNodeIdText(locateNode_);
            subtitle = "no longer in the radio's node table";
        } else {
            title = node->title();
            subtitle = node->idText() + "  " + meshRoleName(node->user.role);
            havePosition = node->position.valid;
            latitude = node->position.latitude;
            longitude = node->position.longitude;
            haveAltitude = node->position.haveAltitude;
            altitudeM = node->position.altitudeM;
            if (havePosition) {
                if (node->positionLocalMs != 0) {
                    positionAge = "their fix heard " +
                                  meshAgeText((now - node->positionLocalMs) / 1000) + " ago";
                } else if (node->position.time != 0) {
                    const int64_t age = static_cast<int64_t>(::time(nullptr)) -
                                        static_cast<int64_t>(node->position.time);
                    positionAge = age > 0 ? "their fix stamped " +
                                                meshAgeText(static_cast<uint64_t>(age)) + " ago"
                                          : std::string("their fix stamped now");
                } else {
                    positionAge = "their fix age unknown (node db)";
                }
            }
            evidence = "heard " + heardAgeText(*node, now) + nodeEvidenceText(*node);
        }
    } else if (isMark) {
        const Mark& mark = marks_[static_cast<size_t>(locateMark_)];
        title = mark.name;
        subtitle = "mark from " + (mark.source.empty() ? std::string("?") : mark.source);
        havePosition = true;
        latitude = mark.latitude;
        longitude = mark.longitude;
        haveAltitude = mark.haveAltitude;
        altitudeM = mark.altitudeM;
        if (mark.stampUtc > 0) {
            positionAge = "marked " + formatLocalTime(mark.stampUtc, "%a %d %b %H:%M");
        }
    } else {
        title = "Nothing to locate";
        subtitle = "F on a node, or Enter on a mark";
    }

    const bool haveRange = fix.valid && havePosition;
    double metres = 0.0, bearing = 0.0;
    if (haveRange) {
        metres = meshDistanceM(fix.latitude, fix.longitude, latitude, longitude);
        bearing = meshBearingDeg(fix.latitude, fix.longitude, latitude, longitude);
    }
    // A course over ground only means something while moving. Below a slow
    // walk the receiver reports whatever it drifted through last.
    const bool moving = fix.valid && fix.haveCourse && fix.haveSpeed && fix.speedKph >= 1.5;
    // Where this station is pointing, from the magnetometer once it has been
    // calibrated, else where it is moving, from the receiver. With a real
    // heading the rose turns heading-up and the arrow says where to walk.
    bool haveHeading = false;
    double heading = 0.0;
    const char* headingSource = "";
    const bool headingUp = compass_.usable(now);
    if (headingUp) {
        haveHeading = true;
        heading = compass_.reading().headingDeg;
        headingSource = "mag";
    } else if (moving) {
        haveHeading = true;
        heading = fix.courseDeg;
        headingSource = "track";
    }

    drawCompass(s, 52, 90, 38, haveRange, bearing, haveHeading, heading,
                headingUp ? heading : 0.0);
    drawText(s, 8, 142, "-> them", theme::accent);
    drawText(s, 62, 142, "o you", haveHeading ? theme::ok : theme::textDim);
    drawText(s, 8, 151, headingUp ? "rose: heading up" : "rose: north up", theme::textDim);

    int y = kBodyY;
    drawTextClipped(s, textX, y, title, textCols, theme::accent);
    y += kGlyphH;
    drawTextClipped(s, textX, y, subtitle, textCols, theme::textDim);
    y += kGlyphH + 3;

    // The two numbers that get read at arm's length, in sunlight.
    std::string range;
    Color rangeColor = theme::accent;
    if (haveRange) {
        range = meshRangeText(metres);
    } else if (!havePosition) {
        range = "NO POS";
        rangeColor = theme::warn;
    } else {
        range = "NO FIX";
        rangeColor = theme::warn;
    }
    drawTextScaled(s, textX, y, range, 3, rangeColor);
    y += 3 * kGlyphH + 2;
    drawTextScaled(s, textX, y, haveRange ? meshBearingText(bearing) : std::string("--- --"), 2,
                   haveRange ? theme::text : theme::textDim);
    y += 2 * kGlyphH + 3;

    std::string turn;
    Color turnColor = theme::text;
    if (haveRange && haveHeading) {
        turn = "you " + formatHeading(heading) + " " + headingSource + "  " +
               meshTurnText(meshRelativeTurnDeg(bearing, heading));
    } else if (haveRange && compass_.available() && !compass_.calibration().hardIron) {
        turn = "compass needs calibrating (menu)";
        turnColor = theme::textDim;
    } else if (haveRange) {
        turn = "walk a few steps for turn advice";
        turnColor = theme::textDim;
    }
    drawTextClipped(s, textX, y, turn, textCols, turnColor);
    y += kGlyphH + 2;

    std::string altitude;
    if (havePosition && haveAltitude && fix.valid && fix.haveAltitude) {
        altitude = "alt " + meshAltitudeDiffText(altitudeM - fix.altitudeM) + "  theirs " +
                   std::to_string(static_cast<int>(altitudeM)) + "m";
    } else if (havePosition && haveAltitude) {
        altitude = "alt theirs " + std::to_string(static_cast<int>(altitudeM)) + "m";
    } else if (havePosition) {
        altitude = "alt not reported";
    }
    drawTextClipped(s, textX, y, altitude, textCols, theme::textDim);
    y += kGlyphH;

    drawTextClipped(s, textX, y,
                    havePosition ? "them " + formatLatLon(latitude, longitude)
                                 : std::string("no position reported"),
                    textCols, havePosition ? theme::text : theme::warn);
    y += kGlyphH;
    drawTextClipped(s, textX, y, positionAge, textCols, theme::textDim);
    y += kGlyphH + 2;

    if (fix.valid) {
        drawTextClipped(s, textX, y,
                        "me   " + fix.coordText() + "  " +
                            std::to_string(fix.satellitesUsed) + " sat",
                        textCols, theme::text);
        y += kGlyphH;
        std::string mine = "my fix " + meshAgeText((now - fix.updatedMs) / 1000);
        if (fix.haveSpeed) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "  %.1f km/h", fix.speedKph);
            mine += buf;
        }
        drawTextClipped(s, textX, y, mine, textCols, theme::textDim);
    } else {
        drawTextClipped(s, textX, y, gnss_.statusText(now), textCols, theme::warn);
        y += kGlyphH;
    }
    y += kGlyphH + 2;
    drawTextClipped(s, textX, y, evidence, textCols, theme::textDim);

    std::string hint;
    if (locateNode_ != 0) hint = "Enter chat  P share my fix  M mark theirs";
    else if (isMark) hint = "D delete  M mark here";
    else hint = "F on a node, or Enter on a mark";
    drawHintBar(s, hint, "Esc back");
}


void App::drawCompassScreen(Surface& s) {
    const uint64_t now = nowMs();
    const int textX = 108;
    const int textCols = std::max(1, (s.w - textX - 2) / kGlyphW);
    const CompassReading& reading = compass_.reading();
    const CompassCalibration& cal = compass_.calibration();
    const bool fresh = reading.valid && now - reading.sampledMs <= Compass::kStaleMs;

    // Heading-up, so the N rides the rim like a real compass card. No target
    // here: this screen is about trusting the needle, not following it.
    drawCompass(s, 52, 90, 38, false, 0.0, fresh, reading.headingDeg,
                fresh ? reading.headingDeg : 0.0);
    drawText(s, 8, 142, "o you", fresh ? theme::ok : theme::textDim);
    drawText(s, 8, 151, fresh ? "rose: heading up" : "rose: north up", theme::textDim);

    int y = kBodyY;
    if (!compass_.available()) {
        drawTextClipped(s, textX, y, "No magnetometer found", textCols, theme::warn);
        y += kGlyphH;
        drawTextClipped(s, textX, y, "no in_magn_*_raw under IIO here", textCols,
                        theme::textDim);
        y += kGlyphH + 4;
        drawTextClipped(s, textX, y, "The Cardputer Zero exposes its", textCols, theme::textDim);
        y += kGlyphH;
        drawTextClipped(s, textX, y, "BMM150 under /sys/bus/iio/devices;", textCols, theme::textDim);
        y += kGlyphH;
        drawTextClipped(s, textX, y, "Locate uses the GNSS track instead.", textCols,
                        theme::textDim);
        drawHintBar(s, "G GNSS status", "Esc back");
        return;
    }

    std::string title = compass_.magnetometerName().empty() ? "magnetometer"
                                                            : compass_.magnetometerName();
    title += " via IIO";
    if (compass_.haveAccelerometer()) title += " + accel tilt";
    drawTextClipped(s, textX, y, title, textCols, theme::accent);
    y += kGlyphH + 3;

    drawTextScaled(s, textX, y, fresh ? formatHeading(reading.headingDeg) : std::string("---"),
                   3, fresh ? theme::accent : theme::textDim);
    y += 3 * kGlyphH + 2;

    char buf[64];
    std::string kind;
    if (!fresh) kind = reading.valid ? "stale sample" : "no sample yet";
    else if (cal.declinationDeg != 0.0) {
        std::snprintf(buf, sizeof(buf), "true; declination %+.1f", cal.declinationDeg);
        kind = buf;
    } else kind = "magnetic (compass.declination = 0)";
    drawTextClipped(s, textX, y, kind, textCols, theme::textDim);
    y += kGlyphH + 2;

    if (fresh) {
        std::snprintf(buf, sizeof(buf), "field %.0f uT", reading.fieldMicroTesla);
        std::string field = buf;
        if (reading.disturbed) field += "  DISTURBED";
        if (reading.haveTilt) {
            std::snprintf(buf, sizeof(buf), "  %s %.0f", reading.tiltDeg > 40.0 ? "TILTED" : "tilt",
                          reading.tiltDeg);
            field += buf;
        }
        drawTextClipped(s, textX, y, field, textCols,
                        reading.disturbed || (reading.haveTilt && reading.tiltDeg > 40.0)
                            ? theme::warn : theme::text);
    }
    y += kGlyphH + 3;

    drawTextClipped(s, textX, y,
                    cal.hardIron ? "hard iron: calibrated" : "hard iron: NOT calibrated",
                    textCols, cal.hardIron ? theme::ok : theme::warn);
    y += kGlyphH;
    std::snprintf(buf, sizeof(buf), "mount offset %+.0f  %s", cal.mountOffsetDeg,
                  cal.aligned ? "aligned" : "not aligned");
    drawTextClipped(s, textX, y, buf, textCols, cal.aligned ? theme::ok : theme::textDim);
    y += kGlyphH + 3;

    if (compass_.calibrating()) {
        std::snprintf(buf, sizeof(buf), "turning: %d samples  %d%% of circle",
                      compass_.calibrationSamples(),
                      static_cast<int>(compass_.calibrationCoverage() * 100.0 + 0.5));
        drawTextClipped(s, textX, y, buf, textCols, theme::accent);
        y += kGlyphH;
    }
    drawTextClipped(s, textX, y, "C: turn a full circle, then C again", textCols, theme::textDim);
    y += kGlyphH;
    drawTextClipped(s, textX, y, "A: walking straight, with a fix", textCols, theme::textDim);
    y += kGlyphH;
    drawTextClipped(s, textX, y, "config.ini: compass.declination", textCols, theme::textDim);

    drawHintBar(s, compass_.calibrating() ? "turn slowly through a full circle, then C"
                                          : "C calibrate  A align to GNSS track  G GNSS",
                "Esc back");
}

void App::drawMarks(Surface& s) {
    const int rows = bodyRows(false);
    const int cols = columns();
    markList_.clamp(static_cast<int>(marks_.size()), rows);

    if (marks_.empty()) {
        drawText(s, 4, kBodyY, "No saved places yet.", theme::warn);
        drawText(s, 4, kBodyY + 2 * kGlyphH, "N marks this station's own GNSS fix.",
                 theme::textDim);
        drawText(s, 4, kBodyY + 3 * kGlyphH, "M on the Locate screen marks a node's fix.",
                 theme::textDim);
        drawText(s, 4, kBodyY + 5 * kGlyphH, "Marks stay on this device. Nothing is sent.",
                 theme::textDim);
        drawHintBar(s, "N new mark", "Esc back");
        return;
    }

    const GnssFix& fix = gnss_.fix();
    for (int i = 0; i < rows; ++i) {
        const int idx = markList_.top + i;
        if (idx >= static_cast<int>(marks_.size())) break;
        const Mark& mark = marks_[static_cast<size_t>(idx)];
        const int y = kBodyY + i * kGlyphH;
        const bool selected = (idx == markList_.sel);
        drawRowSelection(s, y, selected);
        drawText(s, 2, y, selected ? ">" : " ", theme::accent);
        std::string right;
        if (fix.valid) {
            right = meshRangeText(meshDistanceM(fix.latitude, fix.longitude, mark.latitude,
                                                mark.longitude)) +
                    " " +
                    meshCompassPoint(meshBearingDeg(fix.latitude, fix.longitude, mark.latitude,
                                                    mark.longitude));
        } else {
            right = mark.coordText();
        }
        drawTextClipped(s, 2 + kGlyphW, y, mark.name,
                        cols - 4 - static_cast<int>(right.size()),
                        selected ? theme::accent : theme::text);
        drawText(s, s.w - 4 - textWidth(right), y, right, theme::textDim);
    }
    drawScrollbar(s, s.w - 2, kBodyY, rows * kGlyphH, markList_.top, rows,
                  static_cast<int>(marks_.size()));

    // The coordinate is on the row when there is no fix and on the Locate
    // screen always; the hint bar has room for the keys, not for both.
    drawHintBar(s, "Enter locate  N new mark  D delete", "Esc back");
}

void App::drawQuickMsg(Surface& s) {
    // The conversation it will send into stays visible behind the picker.
    drawChat(s);
    drawMenuModal(s, "Quick messages", quickMsgMenu_, quickMsgList_);
    std::string hint = "Enter select   Esc back";
    if (!quickMsgMenu_.empty()) {
        const std::string& h = quickMsgMenu_[static_cast<size_t>(quickMsgList_.sel)].hint;
        if (!h.empty()) hint = h;
    }
    drawHintBar(s, hint, "Esc back");
}

void App::drawInputLine(Surface& s, int x, int y, const LineEditor& editor, int avail,
                        const char* prompt, Color promptColor, bool tallCursor) {
    // Horizontal scrolling keeps the cursor on screen for a line longer than
    // the row; the block cursor is drawn inverted over whatever is under it.
    const std::string& text = editor.text();
    int scroll = 0;
    if (editor.cursor() >= avail) scroll = editor.cursor() - avail + 1;
    const std::string shown = text.substr(static_cast<size_t>(scroll),
                                          static_cast<size_t>(avail));
    drawText(s, x, y, prompt, promptColor);
    drawText(s, x + 2 * kGlyphW, y, shown, theme::text);
    if (frame_ % 30 >= 20) return;
    const int ci = editor.cursor();
    const char under = ci < static_cast<int>(text.size()) ? text[static_cast<size_t>(ci)] : ' ';
    const int cx = x + (2 + ci - scroll) * kGlyphW;
    if (tallCursor) fillRect(s, cx, y - 1, kGlyphW, kGlyphH + 1, theme::accent);
    else fillRect(s, cx, y, kGlyphW, kGlyphH, theme::accent);
    drawChar(s, cx, y, under, theme::bg);
}

void App::drawRowSelection(Surface& s, int y, bool selected) {
    if (!selected) return;
    fillRect(s, 0, y, s.w - 3, kGlyphH, theme::panelHi);
    fillRect(s, 0, y, 2, kGlyphH, theme::accent);
}

void App::drawModal(Surface& s) {
    if (!modal_) return;
    const int cols = columns();
    const int innerCols = cols - 8;
    std::vector<std::string> body = wrapText(modalBody_, innerCols);
    if (body.size() > 9) body.resize(9);

    const int inputH = modalIsInput_ ? kGlyphH + 6 : 0;
    const int h = static_cast<int>(body.size()) * kGlyphH + 3 * kGlyphH + 10 + inputH;
    const int w = s.w - 20;
    const int x = 10;
    const int y = std::max(2, (s.h - h) / 2);

    // Lower the backdrop contrast without turning text and rules into stripes.
    dimSurface(s, theme::bg);

    fillRect(s, x + 3, y + 3, w, h, theme::black);
    fillRect(s, x, y, w, h, theme::panel);
    rect(s, x, y, w, h, modalIsConfirm_ || modalIsInput_ ? theme::accent : theme::rule);
    fillRect(s, x + 1, y + 1, w - 2, kGlyphH + 2,
             modalIsConfirm_ || modalIsInput_ ? theme::accentDim : theme::panelHi);
    drawTextClipped(s, x + 4, y + 2, modalTitle_, innerCols, theme::text);

    int ty = y + kGlyphH + 6;
    for (const std::string& line : body) {
        Color c = theme::text;
        if (line.find("***") != std::string::npos) c = theme::err;
        drawTextClipped(s, x + 4, ty, line, innerCols, c);
        ty += kGlyphH;
    }

    if (modalIsInput_) {
        // One editable line, with the same block cursor as the terminal so
        // it is obviously the thing the keyboard is talking to.
        const int iy = ty + 2;
        fillRect(s, x + 4, iy, w - 8, kGlyphH + 2, theme::bg);
        rect(s, x + 4, iy, w - 8, kGlyphH + 2, theme::accent);
        drawInputLine(s, x + 8, iy + 1, modalEditor_, innerCols - 3, ">", theme::accent, false);
    }

    const std::string prompt = modalIsConfirm_
                                   ? "Enter/Y " + modalYes_ + "     Esc/N cancel"
                                   : modalIsInput_ ? "Enter save     Esc cancel"
                                                   : "Enter or Esc to close";
    const int promptY = y + h - kGlyphH - 3;
    hLine(s, x + 1, promptY - 3, w - 2, theme::rule);
    drawText(s, x + 4, promptY, prompt, theme::accent);
}

void App::render() {
    Surface s = display_.surface();
    if (!s.valid()) return;
    fill(s, theme::bg);
    drawTopBar(s);

    switch (screen_) {
    case Screen::Ports:    drawPorts(s); break;
    case Screen::Terminal: drawTerminal(s); break;
    case Screen::Menu:
    case Screen::Quick:    drawMenu(s); break;
    case Screen::Files:    drawFiles(s); break;
    case Screen::Diagnostics: drawDiagnostics(s); break;
    case Screen::Keymap:   drawKeymap(s); break;
    case Screen::Help:     drawHelp(s); break;
    case Screen::About:    drawAbout(s); break;
    case Screen::Nodes:    drawNodes(s); break;
    case Screen::Chat:     drawChat(s); break;
    case Screen::Locate:   drawLocate(s); break;
    case Screen::Marks:    drawMarks(s); break;
    case Screen::QuickMsg: drawQuickMsg(s); break;
    case Screen::Compass:  drawCompassScreen(s); break;
    }
    drawModal(s);
}

} // namespace bf
