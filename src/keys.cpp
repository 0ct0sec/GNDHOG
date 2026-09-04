#include "keys.h"

#include <array>
#include <cstring>

namespace bf {
namespace {

// Linux input-event codes are a stable userspace ABI; the subset used here is
// spelled out so the decoder also builds and tests on a non-Linux host.
enum : int {
    KEsc = 1, K1 = 2, K0 = 11, KMinus = 12, KEqual = 13, KBackspace = 14, KTab = 15,
    KQ = 16, KP_ = 25, KEnter = 28, KLeftCtrl = 29, KA = 30, KL = 38,
    KSemicolon = 39, KApostrophe = 40, KGrave = 41, KLeftShift = 42, KBackslash = 43,
    KZ = 44, KM = 50, KComma = 51, KDot = 52, KSlash = 53, KRightShift = 54,
    KKpAsterisk = 55, KLeftAlt = 56, KSpace = 57, KCapsLock = 58,
    KF1 = 59, KF10 = 68, KNumLock = 69, KScrollLock = 70,
    KKp7 = 71, KKp8 = 72, KKp9 = 73, KKpMinus = 74, KKp4 = 75, KKp5 = 76, KKp6 = 77,
    KKpPlus = 78, KKp1 = 79, KKp2 = 80, KKp3 = 81, KKp0 = 82, KKpDot = 83,
    KZenkaku = 85, K102nd = 86, KF11 = 87, KF12 = 88, KRo = 89, KKatakana = 90,
    KHiragana = 91, KHenkan = 92, KKatakanaHiragana = 93, KMuhenkan = 94,
    KKpEnter = 96, KRightCtrl = 97, KKpSlash = 98, KRightAlt = 100,
    KHome = 102, KUp = 103, KPageUp = 104, KLeft = 105, KRight = 106, KEnd = 107,
    KDown = 108, KPageDown = 109, KInsert = 110, KDelete = 111,
    KHelp = 138, KBrightDown = 224, KBrightUp = 225,
};

constexpr uint64_t kRepeatDelayMs = 400;
constexpr uint64_t kRepeatIntervalMs = 70;

// Exact 46-key matrix from the v5 overlay (scan, label, base, sym, fn).
constexpr std::array<KeyDescriptor, 46> kKeys{{
    {0x00, "2", 3, 27, 60},      {0x01, "3", 4, 39, 61},
    {0x02, "4", 5, 40, 62},      {0x03, "5", 6, 41, 63},
    {0x04, "6", 7, 43, 64},      {0x05, "7", 8, 51, 65},
    {0x06, "8", 9, 52, 66},      {0x07, "9", 10, 53, 67},
    {0x08, "0", 11, 94, 68},     {0x09, "bksp", 14, 14, 111},
    {0x10, "q", 16, 55, 164},    {0x11, "w", 17, 69, 165},
    {0x12, "e", 18, 70, 163},    {0x13, "r", 19, 71, 19},
    {0x14, "t", 20, 72, 20},     {0x15, "y", 21, 73, 21},
    {0x16, "u", 22, 74, 224},    {0x17, "i", 23, 75, 225},
    {0x18, "o", 24, 76, 87},     {0x19, "p", 25, 77, 88},
    {0x20, "a", 30, 79, 113},    {0x21, "s", 31, 80, 114},
    {0x22, "d", 32, 81, 115},    {0x23, "f", 33, 0, 103},
    {0x24, "g", 34, 82, 34},     {0x25, "h", 35, 83, 138},
    {0x26, "j", 36, 85, 99},     {0x27, "k", 37, 86, 102},
    {0x28, "l", 38, 89, 104},    {0x29, "enter", 28, 28, 28},
    {0x30, "ctrl", 29, 29, 29},  {0x31, "alt", 56, 56, 56},
    {0x32, "z", 44, 0, 105},     {0x33, "x", 45, 0, 108},
    {0x34, "c", 46, 0, 106},     {0x35, "v", 47, 90, 47},
    {0x36, "b", 48, 91, 110},    {0x37, "n", 49, 92, 107},
    {0x38, "m", 50, 93, 109},    {0x39, "space", 57, 57, 57},
    {0x40, "esc", 1, 1, 1},      {0x41, "shift", 42, 78, 42},
    {0x42, "fn", 0, 0, 0},       {0x43, "sym", 0, 0, 0},
    {0x44, "1", 2, 26, 59},      {0x49, "tab", 15, 15, 15},
}};

bool isModifier(int code) {
    return code == KLeftShift || code == KRightShift || code == KLeftCtrl ||
           code == KRightCtrl || code == KLeftAlt || code == KRightAlt ||
           code == KCapsLock;
}

// US-layout shifted form, applied on top of whichever layer produced the char.
char shiftChar(char c) {
    if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
    switch (c) {
    case '1': return '!';  case '2': return '@';  case '3': return '#';
    case '4': return '$';  case '5': return '%';  case '6': return '^';
    case '7': return '&';  case '8': return '*';  case '9': return '(';
    case '0': return ')';  case '-': return '_';  case '=': return '+';
    case '[': return '{';  case ']': return '}';  case ';': return ':';
    case '\'': return '"'; case '`': return '~';  case '\\': return '|';
    case ',': return '<';  case '.': return '>';  case '/': return '?';
    default: return c;
    }
}

// Base-layer / plain-USB-keyboard keycode to ASCII.
char baseChar(int code) {
    static const char kQwerty[] = "qwertyuiop";
    static const char kHome[]   = "asdfghjkl";
    static const char kBottom[] = "zxcvbnm";
    if (code >= KQ && code <= KP_) return kQwerty[code - KQ];
    if (code >= KA && code <= KL)  return kHome[code - KA];
    if (code >= KZ && code <= KM)  return kBottom[code - KZ];
    if (code >= K1 && code <= K0)  return "1234567890"[code - K1];
    switch (code) {
    case KSpace: return ' ';
    case KMinus: return '-';
    case KEqual: return '=';
    case KSemicolon: return ';';
    case KApostrophe: return '\'';
    case KGrave: return '`';
    case KBackslash: return '\\';
    case KComma: return ',';
    case KDot: return '.';
    case KSlash: return '/';
    case 26: return '[';
    case 27: return ']';
    // Keypad codes carry their face value; the Sym layer leans on these.
    case KKpAsterisk: return '*';
    case KKpMinus: return '-';
    case KKpPlus: return '+';
    case KKpSlash: return '/';
    case KKpDot: return '.';
    case KKp0: return '0';
    default:
        if (code >= KKp7 && code <= KKp9) return static_cast<char>('7' + code - KKp7);
        if (code >= KKp4 && code <= KKp6) return static_cast<char>('4' + code - KKp4);
        if (code >= KKp1 && code <= KKp3) return static_cast<char>('1' + code - KKp1);
        return 0;
    }
}

// The base layer's character with Shift and Caps Lock applied: Caps only
// turns letters, and Shift on a letter under Caps turns it back.
char shiftedBaseChar(int code, bool shift, bool caps) {
    const char c = baseChar(code);
    if (!c) return 0;
    const bool upper = (c >= 'a' && c <= 'z') ? (shift != caps) : shift;
    return upper ? shiftChar(c) : c;
}

Key specialForCode(int code) {
    switch (code) {
    case KEnter: case KKpEnter: return Key::Enter;
    case KBackspace: return Key::Backspace;
    case KTab: return Key::Tab;
    case KEsc: return Key::Escape;
    case KDelete: return Key::Delete;
    case KUp: return Key::Up;
    case KDown: return Key::Down;
    case KLeft: return Key::Left;
    case KRight: return Key::Right;
    case KHome: return Key::Home;
    case KEnd: return Key::End;
    case KPageUp: return Key::PageUp;
    case KPageDown: return Key::PageDown;
    case KInsert: return Key::Insert;
    case KHelp: return Key::Help;
    case KBrightUp: return Key::BrightUp;
    case KBrightDown: return Key::BrightDown;
    case KF11: return Key::F11;
    case KF12: return Key::F12;
    default:
        if (code >= KF1 && code <= KF10) {
            return static_cast<Key>(static_cast<int>(Key::F1) + (code - KF1));
        }
        return Key::None;
    }
}

} // namespace

const KeyDescriptor* descriptorForScan(int scan) {
    for (const KeyDescriptor& k : kKeys) {
        if (k.scan == scan) return &k;
    }
    return nullptr;
}

int descriptorCount() { return static_cast<int>(kKeys.size()); }

const KeyDescriptor* descriptorAt(int index) {
    if (index < 0 || index >= static_cast<int>(kKeys.size())) return nullptr;
    return &kKeys[static_cast<size_t>(index)];
}

Layer layerFor(int scan, int code) {
    const KeyDescriptor* d = descriptorForScan(scan);
    if (!d || code == 0) return Layer::Unknown;
    const int hits = (d->base == code) + (d->sym == code) + (d->fn == code);
    if (hits > 1) return Layer::Common;
    if (d->base == code) return Layer::Base;
    if (d->sym == code) return Layer::Sym;
    if (d->fn == code) return Layer::Fn;
    return Layer::Unknown;
}

const char* layerName(Layer l) {
    switch (l) {
    case Layer::Common: return "common";
    case Layer::Base:   return "base";
    case Layer::Sym:    return "sym";
    case Layer::Fn:     return "fn";
    default:            return "unknown";
    }
}

const char* keyName(Key k) {
    switch (k) {
    case Key::Char: return "char";
    case Key::Enter: return "enter";
    case Key::Backspace: return "backspace";
    case Key::Tab: return "tab";
    case Key::Escape: return "esc";
    case Key::Delete: return "del";
    case Key::Up: return "up";
    case Key::Down: return "down";
    case Key::Left: return "left";
    case Key::Right: return "right";
    case Key::Home: return "home";
    case Key::End: return "end";
    case Key::PageUp: return "pgup";
    case Key::PageDown: return "pgdn";
    case Key::Insert: return "ins";
    case Key::Help: return "help";
    case Key::BrightUp: return "bright+";
    case Key::BrightDown: return "bright-";
    case Key::F1: return "F1";   case Key::F2: return "F2";
    case Key::F3: return "F3";   case Key::F4: return "F4";
    case Key::F5: return "F5";   case Key::F6: return "F6";
    case Key::F7: return "F7";   case Key::F8: return "F8";
    case Key::F9: return "F9";   case Key::F10: return "F10";
    case Key::F11: return "F11"; case Key::F12: return "F12";
    default: return "none";
    }
}

char defaultSymChar(int scan) {
    switch (scan) {
    // Sym positions whose keycode already has an unambiguous US meaning.
    case 0x44: return '[';   // KEY_LEFTBRACE
    case 0x00: return ']';   // KEY_RIGHTBRACE
    case 0x01: return ';';   // KEY_SEMICOLON
    case 0x02: return '\'';  // KEY_APOSTROPHE
    case 0x03: return '`';   // KEY_GRAVE
    case 0x04: return '\\';  // KEY_BACKSLASH
    case 0x05: return ',';   // KEY_COMMA
    case 0x06: return '.';   // KEY_DOT
    case 0x07: return '/';   // KEY_SLASH
    case 0x10: return '*';   // KEY_KPASTERISK
    case 0x13: return '7';   case 0x14: return '8';   case 0x15: return '9';
    case 0x16: return '-';   // KEY_KPMINUS
    case 0x17: return '4';   case 0x18: return '5';   case 0x19: return '6';
    case 0x20: return '1';   case 0x21: return '2';   case 0x22: return '3';
    case 0x24: return '0';   // KEY_KP0
    case 0x25: return '.';   // KEY_KPDOT
    case 0x41: return '+';   // KEY_KPPLUS (ASmux in the Sym table)
    // Assigned positions. The overlay routes these through placeholder keycodes
    // (KEY_RO, KEY_MUHENKAN, the Japanese IME block) that carry no US-layout
    // meaning, so the symbols Betaflight needs most go where they are easiest
    // to reach. Correct any of them in config.ini without a rebuild.
    case 0x28: return '_';   // Sym+L  (KEY_RO)       -- every param is snake_case
    case 0x08: return '=';   // Sym+0  (KEY_MUHENKAN) -- `set x = y`
    case 0x26: return ':';   // Sym+J  (KEY_ZENKAKUHANKAKU)
    case 0x27: return '"';   // Sym+K  (KEY_102ND)
    case 0x11: return '{';   // Sym+W  (KEY_NUMLOCK)
    case 0x12: return '}';   // Sym+E  (KEY_SCROLLLOCK)
    case 0x35: return '<';   // Sym+V  (KEY_KATAKANA)
    case 0x36: return '>';   // Sym+B  (KEY_HIRAGANA)
    case 0x37: return '?';   // Sym+N  (KEY_HENKAN)
    case 0x38: return '|';   // Sym+M  (KEY_KATAKANAHIRAGANA)
    default: return 0;
    }
}

char KeyDecoder::symChar(int scan) const {
    const auto it = symOverride_.find(scan);
    if (it != symOverride_.end()) return it->second;
    return defaultSymChar(scan);
}

void KeyDecoder::reset() {
    shift_ = ctrl_ = alt_ = caps_ = false;
    pendingScan_ = heldScan_ = heldCode_ = -1;
    nextRepeatMs_ = 0;
}

void KeyDecoder::releaseAll() {
    // Modifier and held-key state must not survive a disconnect or a screen
    // change, or a still-held key can answer a dialog the user never saw.
    shift_ = ctrl_ = alt_ = false;
    heldScan_ = heldCode_ = -1;
    nextRepeatMs_ = 0;
}

void KeyDecoder::noteScan(int scan) { pendingScan_ = scan; }

void KeyDecoder::updateModifier(int code, bool pressed) {
    if (code == KLeftShift || code == KRightShift) shift_ = pressed;
    else if (code == KLeftCtrl || code == KRightCtrl) ctrl_ = pressed;
    else if (code == KLeftAlt || code == KRightAlt) alt_ = pressed;
    else if (code == KCapsLock && pressed) caps_ = !caps_;
}

bool KeyDecoder::onKey(int code, int value, uint64_t nowMs, KeyEvent& out) {
    const int scan = pendingScan_;
    pendingScan_ = -1;

    if (value == 0) {
        updateModifier(code, false);
        const bool matches = (scan >= 0 && scan == heldScan_) ||
                             (scan < 0 && code == heldCode_);
        if (matches) {
            heldScan_ = heldCode_ = -1;
            nextRepeatMs_ = 0;
        }
        return false;
    }
    if (value != 1 && value != 2) return false;
    if (value == 1) updateModifier(code, true);
    if (isModifier(code)) return false;

    if (value == 1) {
        heldScan_ = scan;
        heldCode_ = code;
        nextRepeatMs_ = nowMs + kRepeatDelayMs;
    } else {
        nextRepeatMs_ = nowMs + kRepeatIntervalMs;
    }
    out = make(code, scan, value == 2);
    return out.valid();
}

bool KeyDecoder::pollRepeat(uint64_t nowMs, KeyEvent& out) {
    // The v5 overlay does not enable kernel autorepeat, so supply our own while
    // still accepting kernel value=2 events if a future overlay sends them.
    if (heldCode_ < 0 || nextRepeatMs_ == 0 || nowMs < nextRepeatMs_) return false;
    do {
        nextRepeatMs_ += kRepeatIntervalMs;
    } while (nextRepeatMs_ <= nowMs);
    out = make(heldCode_, heldScan_, true);
    return out.valid();
}

KeyEvent KeyDecoder::make(int code, int scan, bool repeated) const {
    KeyEvent e;
    e.scan = scan;
    e.code = code;
    e.shift = shift_;
    e.ctrl = ctrl_;
    e.alt = alt_;
    e.caps = caps_;
    e.repeat = repeated;
    e.layer = layerFor(scan, code);

    // Escape must survive every layer, so specials are checked before chars.
    const Key special = specialForCode(code);
    if (special != Key::None) {
        e.key = special;
        return e;
    }

    char c = 0;
    if (e.layer == Layer::Sym) {
        c = symChar(scan);
        if (c && shift_) c = shiftChar(c);
    } else {
        c = shiftedBaseChar(code, shift_, caps_);
    }
    if (c) {
        e.key = Key::Char;
        e.ch = c;
    }
    return e;
}

KeyEvent fromKeycodeOnly(int code, bool shift, bool caps, bool ctrl, bool alt) {
    KeyEvent e;
    e.code = code;
    e.shift = shift;
    e.caps = caps;
    e.ctrl = ctrl;
    e.alt = alt;
    const Key special = specialForCode(code);
    if (special != Key::None) {
        e.key = special;
        return e;
    }
    const char c = shiftedBaseChar(code, shift, caps);
    if (c) {
        e.key = Key::Char;
        e.ch = c;
    }
    return e;
}

} // namespace bf
