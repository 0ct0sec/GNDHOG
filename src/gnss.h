#pragma once
#include "serialport.h"

#include <cstdint>
#include <map>
#include <string>

namespace bf {

// The M5Stack Cap LoRa868 / Cap LoRa-1262 carries an AT6668 GNSS receiver
// alongside its SX1262. On a Cardputer Zero the receiver reaches Linux as a
// plain UART speaking NMEA 0183 at 115200 8N1, so this is a line parser and a
// serial reader, not a driver.
struct GnssFix {
    bool valid = false;              // the receiver currently claims a fix
    bool everValid = false;          // it claimed one at least once this session
    double latitude = 0.0;
    double longitude = 0.0;
    bool haveAltitude = false;
    double altitudeM = 0.0;
    int satellitesUsed = 0;
    int satellitesInView = 0;        // the sum of every constellation's GSV total
    // One GSV total per talker (GP, GL, GA, BD, GQ...). The bench's AT6668
    // sends a set per constellation and the last set of each cycle said 00,
    // which used to zero a count that was five satellites high.
    std::map<std::string, int> inViewByTalker;
    double hdop = 0.0;
    bool haveSpeed = false;
    double speedKph = 0.0;
    bool haveCourse = false;
    double courseDeg = 0.0;
    std::string utc;                 // "hh:mm:ss" as the receiver reported it
    // The latest $--TXT payload, verbatim. The AT6668 on the bench used it for
    // "ANTENNA OPEN" while it sat indoors with no fix; it is shown, not judged.
    std::string receiverText;
    uint32_t utcSeconds = 0;         // epoch seconds, 0 until date and time agree
    uint64_t updatedMs = 0;          // our monotonic clock at the last good fix

    std::string coordText() const;
};

// True when the trailing *CS matches the XOR of the sentence body. A sentence
// with no checksum is accepted, because some receivers omit it; a sentence with
// a wrong one is not, because that is a corrupted line.
bool nmeaChecksumOk(const std::string& sentence);

// Applies one sentence to `fix`. Returns true when the sentence was understood.
bool parseNmeaSentence(const std::string& sentence, GnssFix& fix, uint64_t nowMs);

class Gnss {
public:
    bool open(const std::string& device, int baud, std::string& error);
    void close();
    bool isOpen() const { return port_.isOpen(); }
    int fd() const { return port_.fd(); }
    const std::string& device() const { return port_.device(); }

    void poll(uint64_t nowMs);

    const GnssFix& fix() const { return fix_; }
    // For the self-test and --preview only: installs a fix as if the receiver
    // had reported it, and counts as one sentence so the receiver reads as
    // present. The next real sentence overwrites it.
    void adoptFix(const GnssFix& fix, uint64_t nowMs);
    // Presence is proved by NMEA arriving, never by the device node opening: a
    // UART exists on this board whether or not a cap is clipped to it.
    bool receiverPresent() const { return sentences_ > 0; }
    int sentenceCount() const { return sentences_; }
    uint64_t lastSentenceMs() const { return lastSentenceMs_; }
    uint64_t openedMs() const { return openedMs_; }
    const std::string& lastError() const { return lastError_; }
    // A one-line summary for the status bar and the node screen header.
    std::string statusText(uint64_t nowMs) const;

private:
    void consumeLine(const std::string& line, uint64_t nowMs);

    SerialPort port_;
    std::string buf_;
    GnssFix fix_;
    int sentences_ = 0;
    uint64_t lastSentenceMs_ = 0;
    uint64_t openedMs_ = 0;
    std::string lastError_;
};

} // namespace bf
