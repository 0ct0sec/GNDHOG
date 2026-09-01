#include "app.h"
#include "brand.h"
#include "mascot.h"

#include <algorithm>
#include <cstdio>

namespace bf {
namespace {

std::string stateText(SessionState s) {
    switch (s) {
    case SessionState::Disconnected: return "offline";
    case SessionState::EnteringCli:  return "opening CLI";
    case SessionState::Ready:        return "ready";
    case SessionState::Busy:         return "busy";
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
    "  from current arming faults and keeps the raw output.",
    "  It is a snapshot, not an airworthiness verdict.",
    "",
    "BACKUP AND RESTORE",
    "  Backup runs `diff all` and writes a file named the",
    "  way Betaflight Configurator names it, so the two",
    "  are interchangeable.",
    "  Restore sends the file line by line and waits for",
    "  each prompt. Nothing is kept until you run `save`.",
    "",
    "SAFETY",
    "  A command that can spin a motor or wipe settings",
    "  asks first. Disconnecting only closes the port -",
    "  `exit` is not sent, because on Betaflight that",
    "  reboots the flight controller.",
    "",
    "CONNECTION",
    "  Set the Host/Slave switch to HOST and plug the FC",
    "  into the USB-A port. It appears as ttyACM0.",
    "  A spare FC UART can be wired to Grove instead;",
    "  pick the port and baud on the connect screen.",
};
constexpr int kHelpLines = static_cast<int>(sizeof(kHelpText) / sizeof(kHelpText[0]));

} // namespace

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
    case Screen::Quick:  mid = "quick commands"; break;
    case Screen::Files:  mid = "backups"; break;
    case Screen::Diagnostics: mid = "field check"; break;
    case Screen::Keymap: mid = "keymap"; break;
    case Screen::Help:   mid = "help"; break;
    case Screen::About:  mid = "about"; break;
    case Screen::Terminal:
        if (!session_.connected()) {
            // The state chip already says "offline". Keep the middle label as
            // screen context instead of printing the same state twice.
            mid = "terminal";
        } else {
            mid = session_.device().substr(session_.device().find_last_of('/') + 1);
            if (!session_.craft().empty()) mid += " " + session_.craft();
            else if (!session_.board().empty()) mid += " " + session_.board();
        }
        break;
    }
    // Right-hand state chip.
    const std::string st = stateText(session_.state());
    Color chip = theme::textDim;
    if (session_.state() == SessionState::Ready) chip = theme::ok;
    else if (session_.state() == SessionState::Busy) chip = theme::warn;
    else if (session_.state() == SessionState::Failed) chip = theme::err;
    const int chipW = textWidth(st) + 8;
    const int chipX = s.w - chipW;
    fillRect(s, chipX, 0, chipW, kTopH, theme::panelHi);
    vLine(s, chipX, 0, kTopH, theme::rule);
    hLine(s, chipX + 1, kTopH - 1, chipW - 1, chip);
    const int midX = separatorX + 4;
    drawTextClipped(s, midX, 1, mid,
                    (chipX - kGlyphW - midX) / kGlyphW, theme::text);
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
                   int visibleRows) {
    const int cols = columns();
    if (items.empty()) {
        drawText(s, 4, kBodyY, "(nothing here)", theme::textDim);
        return;
    }
    for (int i = 0; i < visibleRows; ++i) {
        const int idx = st.top + i;
        if (idx >= static_cast<int>(items.size())) break;
        const int y = kBodyY + i * kGlyphH;
        const bool selected = (idx == st.sel);
        if (selected) {
            fillRect(s, 0, y, s.w - 3, kGlyphH, theme::panelHi);
            fillRect(s, 0, y, 2, kGlyphH, theme::accent);
        }
        const Color fg = items[static_cast<size_t>(idx)].enabled
                             ? (selected ? theme::accent : theme::text)
                             : theme::textDim;
        drawText(s, 2, y, selected ? ">" : " ", theme::accent);
        drawTextClipped(s, 2 + kGlyphW, y, items[static_cast<size_t>(idx)].label, cols - 3, fg);
    }
    drawScrollbar(s, s.w - 2, kBodyY, visibleRows * kGlyphH, st.top, visibleRows,
                  static_cast<int>(items.size()));
}

void App::drawPorts(Surface& s) {
    const int rows = bodyRows(false);
    const int cols = columns();

    if (ports_.empty()) {
        drawText(s, 4, kBodyY, "No serial ports found.", theme::warn);
        drawText(s, 4, kBodyY + 2 * kGlyphH, "Set the Host/Slave switch to HOST,", theme::textDim);
        drawText(s, 4, kBodyY + 3 * kGlyphH, "plug the FC into USB-A, power it on,", theme::textDim);
        drawText(s, 4, kBodyY + 4 * kGlyphH, "then press R to rescan.", theme::textDim);
    } else {
        for (int i = 0; i < rows; ++i) {
            const int idx = portList_.top + i;
            if (idx >= static_cast<int>(ports_.size())) break;
            const PortInfo& p = ports_[static_cast<size_t>(idx)];
            const int y = kBodyY + i * kGlyphH;
            const bool selected = (idx == portList_.sel);
            if (selected) {
                fillRect(s, 0, y, s.w - 3, kGlyphH, theme::panelHi);
                fillRect(s, 0, y, 2, kGlyphH, theme::accent);
            }
            drawText(s, 2, y, selected ? ">" : " ", theme::accent);
            const Color fg = p.looksLikeFlightController()
                                 ? theme::ok
                                 : (isDfuId(p.vendorId, p.productId) ? theme::warn : theme::text);
            drawTextClipped(s, 2 + kGlyphW, y, p.label(), cols - 3, fg);
        }
        drawScrollbar(s, s.w - 2, kBodyY, rows * kGlyphH, portList_.top, rows,
                      static_cast<int>(ports_.size()));
    }

    std::string hint = "R scan  F files  H help  A about";
    if (!ports_.empty()) {
        const PortInfo& p = ports_[static_cast<size_t>(portList_.sel)];
        std::string detail = p.detail();
        if (p.kind == "uart") {
            detail += (detail.empty() ? "" : "  ") +
                      std::string("baud ") + std::to_string(kBaudChoices[baudIndex_]) +
                      "  B changes";
        }
        if (!detail.empty()) hint = detail + "  A about";
    }
    drawHintBar(s, hint, ports_.empty() ? "Esc quit" : "Enter link  Esc quit");
}

void App::drawTerminal(Surface& s) {
    const int cols = columns();
    const int rows = bodyRows(true);
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

    // Input line with horizontal scrolling around the cursor.
    const std::string& text = editor_.text();
    const int avail = cols - 3;
    int scroll = 0;
    if (editor_.cursor() >= avail) scroll = editor_.cursor() - avail + 1;
    const std::string shown = text.substr(static_cast<size_t>(scroll),
                                          static_cast<size_t>(avail));
    const Color promptColor = session_.ready() ? theme::accent : theme::textDim;
    drawText(s, 2, inputY + 2, "#", promptColor);
    drawText(s, 2 + 2 * kGlyphW, inputY + 2, shown, theme::text);

    // Block cursor, drawn inverted over whatever character is under it.
    const int cx = 2 + (2 + editor_.cursor() - scroll) * kGlyphW;
    if (frame_ % 30 < 20) {
        const int ci = editor_.cursor();
        const char under = ci < static_cast<int>(text.size()) ? text[static_cast<size_t>(ci)] : ' ';
        fillRect(s, cx, inputY + 1, kGlyphW, kGlyphH + 1, theme::accent);
        drawChar(s, cx, inputY + 2, under, theme::bg);
    }

    std::string hints;
    if (!completionNote_.empty()) hints = completionNote_ + "   ";
    if (!session_.connected()) hints += "not connected";
    else if (!term_.following()) hints += "scrolled back - Fn+M returns to live";
    else hints += "Tab complete  Fn+F history  Fn+1 help";
    drawHintBar(s, hints, "Esc menu");
}

void App::drawMenu(Surface& s) {
    const bool quick = (screen_ == Screen::Quick);
    std::vector<MenuItem>& items = quick ? quick_ : menu_;
    ListState& st = quick ? quickList_ : menuList_;
    const int rows = bodyRows(false);
    st.clamp(static_cast<int>(items.size()), rows);
    drawList(s, items, st, rows);

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
        if (selected) {
            fillRect(s, 0, y, s.w - 3, kGlyphH, theme::panelHi);
            fillRect(s, 0, y, 2, kGlyphH, theme::accent);
        }
        drawText(s, 2, y, selected ? ">" : " ", theme::accent);
        // Trim the fixed BTFL_cli_ prefix: it is on every file and wastes width.
        std::string name = f.name;
        if (name.rfind("BTFL_cli_", 0) == 0) name = name.substr(9);
        if (name.size() > 11 && name.compare(name.size() - 11, 11, "_backup.txt") == 0) {
            name.resize(name.size() - 11);
        }
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
    Color summaryColor = theme::accent;
    if (diagnosticRunning_) {
        summary = "FIELD CHECK RUNNING";
    } else if (!diagnosticReport_.complete()) {
        summary = "INCOMPLETE";
        summaryColor = theme::warn;
    } else if (diagnosticReport_.actionableBlockerCount() > 0) {
        summary = "ARM BLOCKED";
        summaryColor = theme::err;
    } else if (diagnosticReport_.failureCount() > 0) {
        summary = "FAULT REPORTED";
        summaryColor = theme::err;
    } else if (diagnosticReport_.warningCount() > 0) {
        summary = "CHECK FINDINGS";
        summaryColor = theme::warn;
    } else {
        summary = "NO ACTIVE FAULTS";
        summaryColor = theme::ok;
    }
    drawText(s, 4, kBodyY + 2, summary, summaryColor);

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

    const std::string sub = !diagnosticError_.empty()
                                ? diagnosticError_
                                : "CLI snapshot only - inspect the craft props-off";
    drawTextClipped(s, 4, kBodyY + 13, sub, cols - 2,
                    diagnosticError_.empty() ? theme::textDim : theme::warn);

    const int count = static_cast<int>(diagnosticReport_.findings.size());
    diagnosticList_.clamp(count, rows);
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

void App::drawModal(Surface& s) {
    if (!modal_) return;
    const int cols = columns();
    const int innerCols = cols - 8;
    std::vector<std::string> body = wrapText(modalBody_, innerCols);
    if (body.size() > 9) body.resize(9);

    const int h = static_cast<int>(body.size()) * kGlyphH + 3 * kGlyphH + 10;
    const int w = s.w - 20;
    const int x = 10;
    const int y = std::max(2, (s.h - h) / 2);

    // Lower the backdrop contrast without turning text and rules into stripes.
    dimSurface(s, theme::bg);

    fillRect(s, x + 3, y + 3, w, h, theme::black);
    fillRect(s, x, y, w, h, theme::panel);
    rect(s, x, y, w, h, modalIsConfirm_ ? theme::accent : theme::rule);
    fillRect(s, x + 1, y + 1, w - 2, kGlyphH + 2, modalIsConfirm_ ? theme::accentDim : theme::panelHi);
    drawTextClipped(s, x + 4, y + 2, modalTitle_, innerCols, theme::text);

    int ty = y + kGlyphH + 6;
    for (const std::string& line : body) {
        Color c = theme::text;
        if (line.find("***") != std::string::npos) c = theme::err;
        drawTextClipped(s, x + 4, ty, line, innerCols, c);
        ty += kGlyphH;
    }

    const std::string prompt = modalIsConfirm_
                                   ? "Enter/Y " + modalYes_ + "     Esc/N cancel"
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
    }
    drawModal(s);
}

} // namespace bf
