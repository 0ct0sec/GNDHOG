#include "term.h"

#include <algorithm>

namespace bf {

void Terminal::setWidth(int cols) {
    const int c = cols > 0 ? cols : 1;
    if (c == cols_) return;
    cols_ = c;
    rewrapAll();
}

void Terminal::clear() {
    lines_.clear();
    rows_.clear();
    resetInputFragment();
    scroll_ = 0;
    follow_ = true;
    capturing_ = false;
    captureFromLine_ = 0;
}

void Terminal::resetInputFragment() {
    partial_.clear();
    partialKind_ = LineKind::Fc;
    escape_.clear();
    inEscape_ = false;
}

void Terminal::appendRowsFor(int lineIndex) {
    const std::string& t = lines_[static_cast<size_t>(lineIndex)].text;
    if (t.empty()) {
        rows_.push_back(DisplayRow{lineIndex, 0, 0});
        return;
    }
    size_t off = 0;
    while (off < t.size()) {
        const size_t take = std::min(static_cast<size_t>(cols_), t.size() - off);
        rows_.push_back(DisplayRow{lineIndex, static_cast<uint16_t>(off),
                                   static_cast<uint16_t>(take)});
        off += take;
    }
}

void Terminal::rewrapAll() {
    rows_.clear();
    for (size_t i = 0; i < lines_.size(); ++i) appendRowsFor(static_cast<int>(i));
}

void Terminal::pushLine(const std::string& text, LineKind kind) {
    lines_.push_back(TermLine{text, kind});
    ++linesEver_;
    appendRowsFor(static_cast<int>(lines_.size()) - 1);

    if (lines_.size() > maxLines_) {
        // Drop the oldest quarter at once so trimming is amortised rather than
        // re-wrapping the whole buffer on every single line.
        const size_t drop = maxLines_ / 4 + 1;
        const size_t rowsBefore = rows_.size();
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<long>(drop));
        captureFromLine_ = captureFromLine_ > drop ? captureFromLine_ - drop : 0;
        rewrapAll();
        // Scroll is measured in display rows, not lines: a dropped line can be
        // several wrapped rows, so subtract what the rewrap actually removed.
        scroll_ = std::max(0, scroll_ - static_cast<int>(rowsBefore - rows_.size()));
    }
}

// Whatever the FC has sent of its current line goes into the scrollback as it
// stands, so that nothing is ever spliced into the middle of it.
void Terminal::flushPartial() {
    if (partial_.empty()) return;
    pushLine(partial_, partialKind_);
    partial_.clear();
}

void Terminal::addLine(const std::string& text, LineKind kind) {
    flushPartial();
    pushLine(text, kind);
}

std::vector<TermLine> Terminal::feed(const std::string& bytes) {
    std::vector<TermLine> completed;
    for (const char raw : bytes) {
        const unsigned char c = static_cast<unsigned char>(raw);

        if (inEscape_) {
            escape_ += static_cast<char>(c);
            // CSI runs until a byte in 0x40..0x7E; a lone ESC-x ends at once.
            const bool csi = escape_.size() > 1 && escape_[1] == '[';
            if ((csi && c >= 0x40 && c <= 0x7E && escape_.size() > 2) ||
                (!csi && escape_.size() >= 2)) {
                inEscape_ = false;
                escape_.clear();
            }
            if (escape_.size() > 32) {   // malformed: give up and resync
                inEscape_ = false;
                escape_.clear();
            }
            continue;
        }

        switch (c) {
        case 0x1B:
            inEscape_ = true;
            escape_ = "\x1b";
            break;
        case '\r':
            // Bare CR is a cursor return; the following LF ends the line. If no
            // LF arrives the next character simply overwrites from column 0,
            // which is how the CLI redraws its prompt.
            break;
        case '\n':
            completed.push_back(TermLine{partial_, partialKind_});
            pushLine(partial_, partialKind_);
            partial_.clear();
            partialKind_ = LineKind::Fc;
            break;
        case '\b':
        case 0x7F:
            if (!partial_.empty()) partial_.pop_back();
            break;
        case '\t':
            partial_.append(4 - (partial_.size() % 4), ' ');
            break;
        case 0x07:
            break;   // BEL: nothing audible here
        default:
            if (c >= 0x20 && c < 0x7F) partial_ += static_cast<char>(c);
            break;
        }
    }
    return completed;
}

std::string Terminal::rowText(size_t r) const {
    if (r >= rows_.size()) return {};
    const DisplayRow& row = rows_[r];
    return lines_[static_cast<size_t>(row.line)].text.substr(row.start, row.len);
}

LineKind Terminal::rowKind(size_t r) const {
    if (r >= rows_.size()) return LineKind::Fc;
    return lines_[static_cast<size_t>(rows_[r].line)].kind;
}

bool Terminal::atBottom(int visibleRows) const {
    const int maxScroll = std::max(0, static_cast<int>(rows_.size()) - visibleRows);
    return scroll_ >= maxScroll;
}

void Terminal::scrollBy(int delta, int visibleRows) {
    const int maxScroll = std::max(0, static_cast<int>(rows_.size()) - visibleRows);
    scroll_ = std::max(0, std::min(maxScroll, scroll_ + delta));
    follow_ = (scroll_ >= maxScroll);
}

void Terminal::scrollToBottom(int visibleRows) {
    scroll_ = std::max(0, static_cast<int>(rows_.size()) - visibleRows);
    follow_ = true;
}

void Terminal::markCapture() {
    flushPartial();
    capturing_ = true;
    captureFromLine_ = lines_.size();
}

std::string Terminal::captureSince() const {
    if (!capturing_) return {};
    std::string out;
    for (size_t i = captureFromLine_; i < lines_.size(); ++i) {
        if (lines_[i].kind != LineKind::Fc && lines_[i].kind != LineKind::Echo) continue;
        out += lines_[i].text;
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------- LineEditor

void LineEditor::setText(const std::string& t) {
    text_ = t;
    cursor_ = static_cast<int>(text_.size());
}

void LineEditor::clear() {
    text_.clear();
    cursor_ = 0;
    historyPos_ = -1;
}

void LineEditor::insert(char c) {
    text_.insert(static_cast<size_t>(cursor_), 1, c);
    ++cursor_;
}

void LineEditor::insert(const std::string& s) {
    text_.insert(static_cast<size_t>(cursor_), s);
    cursor_ += static_cast<int>(s.size());
}

void LineEditor::backspace() {
    if (cursor_ <= 0) return;
    text_.erase(static_cast<size_t>(cursor_ - 1), 1);
    --cursor_;
}

void LineEditor::del() {
    if (cursor_ >= static_cast<int>(text_.size())) return;
    text_.erase(static_cast<size_t>(cursor_), 1);
}

namespace {
bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}
} // namespace

void LineEditor::left(bool word) {
    if (cursor_ <= 0) return;
    if (!word) { --cursor_; return; }
    while (cursor_ > 0 && !isWordChar(text_[static_cast<size_t>(cursor_ - 1)])) --cursor_;
    while (cursor_ > 0 && isWordChar(text_[static_cast<size_t>(cursor_ - 1)])) --cursor_;
}

void LineEditor::right(bool word) {
    const int n = static_cast<int>(text_.size());
    if (cursor_ >= n) return;
    if (!word) { ++cursor_; return; }
    while (cursor_ < n && isWordChar(text_[static_cast<size_t>(cursor_)])) ++cursor_;
    while (cursor_ < n && !isWordChar(text_[static_cast<size_t>(cursor_)])) ++cursor_;
}

void LineEditor::home() { cursor_ = 0; }
void LineEditor::end() { cursor_ = static_cast<int>(text_.size()); }

void LineEditor::killToEnd() { text_.erase(static_cast<size_t>(cursor_)); }

void LineEditor::killToStart() {
    text_.erase(0, static_cast<size_t>(cursor_));
    cursor_ = 0;
}

void LineEditor::killWordBack() {
    const int start = cursor_;
    left(true);
    text_.erase(static_cast<size_t>(cursor_), static_cast<size_t>(start - cursor_));
}

std::string LineEditor::commit() {
    std::string out = text_;
    pushHistory(out);
    clear();
    return out;
}

void LineEditor::pushHistory(const std::string& line) {
    if (line.empty() || (!history_.empty() && history_.back() == line)) return;
    history_.push_back(line);
    if (history_.size() > 200) history_.erase(history_.begin());
}

void LineEditor::loadHistory(std::vector<std::string> h) { history_ = std::move(h); }

bool LineEditor::historyPrev() {
    if (history_.empty()) return false;
    if (historyPos_ == -1) {
        stash_ = text_;
        historyPos_ = static_cast<int>(history_.size()) - 1;
    } else if (historyPos_ > 0) {
        --historyPos_;
    } else {
        return false;
    }
    setText(history_[static_cast<size_t>(historyPos_)]);
    return true;
}

bool LineEditor::historyNext() {
    if (historyPos_ == -1) return false;
    ++historyPos_;
    if (historyPos_ >= static_cast<int>(history_.size())) {
        historyPos_ = -1;
        setText(stash_);
    } else {
        setText(history_[static_cast<size_t>(historyPos_)]);
    }
    return true;
}

std::string LineEditor::wordPrefix() const {
    int start = cursor_;
    while (start > 0 && isWordChar(text_[static_cast<size_t>(start - 1)])) --start;
    return text_.substr(static_cast<size_t>(start), static_cast<size_t>(cursor_ - start));
}

void LineEditor::replaceWord(const std::string& replacement) {
    int start = cursor_;
    while (start > 0 && isWordChar(text_[static_cast<size_t>(start - 1)])) --start;
    text_.erase(static_cast<size_t>(start), static_cast<size_t>(cursor_ - start));
    cursor_ = start;
    insert(replacement);
}

} // namespace bf
