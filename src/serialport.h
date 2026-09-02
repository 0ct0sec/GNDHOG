#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bf {

// A candidate serial port plus whatever identity the kernel exposes for it.
// Device-node numbers are never treated as stable: ports are ranked by USB
// identity, and a saved favourite is matched on by-id path or VID:PID first.
struct PortInfo {
    std::string device;      // /dev/ttyACM0
    std::string byId;        // /dev/serial/by-id/... when present
    std::string vendorId;    // "0483"
    std::string productId;   // "5740"
    std::string product;     // "STM32 Virtual ComPort"
    std::string manufacturer;
    std::string kind;        // "usb" | "uart"
    int score = 0;           // higher = more likely to be a flight controller
    int meshScore = 0;       // higher = more likely to be a Meshtastic radio

    std::string vidPid() const;
    std::string label() const;        // short line for the picker
    std::string detail() const;       // second line for the picker
    bool looksLikeFlightController() const { return score >= 60; }
    bool looksLikeMeshtastic() const { return meshScore >= 60; }
    // The picker ranks on whichever identity is the stronger claim, so a mesh
    // radio is not buried under a generic CDC device that scored as an FC.
    int rank() const { return score > meshScore ? score : meshScore; }
    // True when this port is more likely a radio than a flight controller.
    bool prefersMeshtastic() const { return meshScore > score; }
};

// Enumerates /dev/serial/by-id, /dev/ttyACM*, /dev/ttyUSB* and the Grove UART.
// Read-only: nothing is opened, no bus is probed.
std::vector<PortInfo> enumeratePorts();

// True for USB IDs known to be Betaflight-capable flight controllers.
bool isKnownFcId(const std::string& vid, const std::string& pid);
// True for the USB identities a Meshtastic radio arrives with: an ESP32's own
// USB-serial-JTAG, or one of the common bridge chips. A bridge is a weaker
// claim than a native Espressif device, because everything uses those.
bool isKnownMeshId(const std::string& vid, const std::string& pid);
bool isSerialBridgeId(const std::string& vid, const std::string& pid);
// True for the DFU/bootloader IDs -- reachable, but not a CLI.
bool isDfuId(const std::string& vid, const std::string& pid);

class SerialPort {
public:
    ~SerialPort();
    SerialPort() = default;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // baud is ignored by USB CDC-ACM but matters for the Grove UART.
    bool open(const std::string& device, int baud, std::string& error);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    const std::string& device() const { return device_; }

    // Non-blocking. read() returns bytes appended to `out`, or -1 on a hard
    // error (including the FC unplugging, which surfaces as EIO/ENXIO).
    int read(std::string& out);
    // Queues bytes; call flush() (or let the app loop do it) to drain.
    void write(const std::string& data);
    bool flush(std::string& error);
    size_t pendingOut() const { return outBuf_.size(); }
    bool hadHangup() const { return hangup_; }

private:
    int fd_ = -1;
    std::string device_;
    std::string outBuf_;
    bool hangup_ = false;
};

// Baud rates offered for the Grove/EXT UART path.
extern const int kBaudChoices[];
extern const int kBaudChoiceCount;
// True only when the requested rate has an exact termios mapping. Unsupported
// values must never be silently negotiated as 115200.
bool isSupportedBaud(int baud);

} // namespace bf
