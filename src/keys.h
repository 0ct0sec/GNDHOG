#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace bf {

enum class Key : uint8_t {
    None = 0,
    Char,        // printable ASCII in KeyEvent::ch
    Enter, Backspace, Tab, Escape, Delete,
    Up, Down, Left, Right,
    Home, End, PageUp, PageDown, Insert,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Help, BrightUp, BrightDown,
};

enum class Layer : uint8_t { Unknown, Common, Base, Sym, Fn };

struct KeyEvent {
    Key  key    = Key::None;
    char ch     = 0;
    bool shift  = false;
    bool ctrl   = false;
    bool alt    = false;
    bool caps   = false;
    bool repeat = false;
    int  scan   = -1;    // raw MSC_SCAN, -1 when the device does not send one
    int  code   = 0;     // raw evdev keycode
    Layer layer = Layer::Unknown;

    bool valid() const { return key != Key::None; }
    // A control chord such as Ctrl+C -- `ch` still carries the base letter.
    bool isCtrlChar() const { return ctrl && key == Key::Char; }
};

// One physical Cardputer Zero key and the three keycodes the kernel driver may
// emit for it. Taken verbatim from the v5 overlay's base/sym/fn tables: Fn
// (0x42) and Sym (0x43) select the table inside the driver, and ASmux (0x41)
// arrives as Shift or Caps Lock.
struct KeyDescriptor {
    uint8_t scan;
    const char* label;
    uint16_t base;
    uint16_t sym;
    uint16_t fn;
};

const KeyDescriptor* descriptorForScan(int scan);
int descriptorCount();
const KeyDescriptor* descriptorAt(int index);
Layer layerFor(int scan, int code);
const char* layerName(Layer l);
const char* keyName(Key k);

// Default character produced by the Sym layer for a given physical key. Several
// of these are inferences: the v5 overlay routes a handful of Sym positions
// through placeholder keycodes (KEY_RO, KEY_MUHENKAN, the Japanese IME block)
// that carry no US-layout meaning, so this table assigns them the symbols a
// Betaflight user actually needs. Anything wrong on a given unit can be
// corrected in the config file without a rebuild -- see KeyDecoder::setSymOverride.
char defaultSymChar(int scan);

class KeyDecoder {
public:
    void reset();
    void noteScan(int scan);                                   // MSC_SCAN arrived
    bool onKey(int code, int value, uint64_t nowMs, KeyEvent& out);
    bool pollRepeat(uint64_t nowMs, KeyEvent& out);            // software autorepeat
    void releaseAll();                                         // focus loss / resync

    void setSymOverride(int scan, char ch) { symOverride_[scan] = ch; }
    char symChar(int scan) const;
    const std::map<int, char>& symOverrides() const { return symOverride_; }

    bool shift() const { return shift_; }
    bool ctrl() const { return ctrl_; }
    bool alt() const { return alt_; }
    bool caps() const { return caps_; }

private:
    KeyEvent make(int code, int scan, bool repeated) const;
    void updateModifier(int code, bool pressed);

    std::map<int, char> symOverride_;
    bool shift_ = false, ctrl_ = false, alt_ = false, caps_ = false;
    int pendingScan_ = -1;
    int heldScan_ = -1;
    int heldCode_ = -1;
    uint64_t nextRepeatMs_ = 0;
};

// Fallback path for a plain USB keyboard plugged into the hub: no MSC_SCAN, so
// the keycode alone has to carry the meaning.
KeyEvent fromKeycodeOnly(int code, bool shift, bool caps, bool ctrl, bool alt);

} // namespace bf
