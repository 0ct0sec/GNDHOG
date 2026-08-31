#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf {

enum class LineKind : uint8_t {
    Fc,       // plain output from the flight controller
    Echo,     // the command line the FC echoed back
    Local,    // a message from this app, never sent over the wire
    Good,
    Warn,
    Error,
};

struct TermLine {
    std::string text;
    LineKind kind = LineKind::Fc;
};

// One rendered row: a slice of a logical line after wrapping.
struct DisplayRow {
    int line = 0;
    uint16_t start = 0;
    uint16_t len = 0;
};

// Scrollback plus the byte-stream decoder that feeds it. Betaflight's CLI is
// line oriented and echoes what it receives, so this only has to handle CR/LF
// pairs, destructive backspace, and the occasional ANSI sequence.
class Terminal {
public:
    void setWidth(int cols);
    int width() const { return cols_; }
    void setMaxLines(size_t n) { maxLines_ = n; }

    void feed(const std::string& bytes);       // raw serial input
    void addLine(const std::string& text, LineKind kind);
    void clear();

    size_t lineCount() const { return lines_.size(); }
    const TermLine& line(size_t i) const { return lines_[i]; }
    // Text of the line currently being assembled (no newline seen yet).
    const std::string& partial() const { return partial_; }

    const std::vector<DisplayRow>& rows() const { return rows_; }
    size_t rowCount() const { return rows_.size(); }
    std::string rowText(size_t r) const;
    LineKind rowKind(size_t r) const;

    // Scroll position, expressed as the first visible display row.
    int scroll() const { return scroll_; }
    void scrollBy(int delta, int visibleRows);
    void scrollToBottom(int visibleRows);
    bool atBottom(int visibleRows) const;
    void setFollow(bool f) { follow_ = f; }
    bool following() const { return follow_; }

    // Everything received since the marker was set, used to capture the output
    // of one command (a `diff all`, say) without disturbing the display.
    void markCapture();
    std::string captureSince() const;

private:
    void pushLine(const std::string& text, LineKind kind);
    void rewrapLast();
    void rewrapAll();
    void appendRowsFor(int lineIndex);

    std::vector<TermLine> lines_;
    std::vector<DisplayRow> rows_;
    std::string partial_;
    LineKind partialKind_ = LineKind::Fc;
    std::string escape_;
    bool inEscape_ = false;

    int cols_ = 53;
    size_t maxLines_ = 3000;
    int scroll_ = 0;
    bool follow_ = true;

    bool capturing_ = false;
    size_t captureFromLine_ = 0;
};

// The input line: editing, history, and completion state.
class LineEditor {
public:
    const std::string& text() const { return text_; }
    void setText(const std::string& t);
    int cursor() const { return cursor_; }

    void insert(char c);
    void insert(const std::string& s);
    void backspace();
    void del();
    void left(bool word);
    void right(bool word);
    void home();
    void end();
    void killToEnd();
    void killToStart();
    void killWordBack();
    void clear();

    // Returns the finished line and pushes it onto the history.
    std::string commit();
    bool historyPrev();
    bool historyNext();
    const std::vector<std::string>& history() const { return history_; }
    void loadHistory(std::vector<std::string> h);

    // The word under the cursor, used as the completion prefix.
    std::string wordPrefix() const;
    void replaceWord(const std::string& replacement);

private:
    std::string text_;
    int cursor_ = 0;
    std::vector<std::string> history_;
    int historyPos_ = -1;        // -1 == editing a fresh line
    std::string stash_;          // the fresh line, parked during history browsing
};

} // namespace bf
