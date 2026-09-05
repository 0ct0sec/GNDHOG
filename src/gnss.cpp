#include "gnss.h"
#include "input.h"
#include "strutil.h"
#include "numutil.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace bf {
namespace {

// A receiver that is actually mute still lets the port open. Anything longer
// than this without a sentence is binary noise or a bare UART, and holding it
// would only grow a buffer nobody reads.
constexpr size_t kMaxLineBytes = 512;
constexpr size_t kMaxBufferBytes = 4096;

// Eight or more characters, all printable ASCII: a line a person could read.
bool legibleText(const std::string& line) {
    if (line.size() < 8) return false;
    for (char c : line) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u != '\t' && (u < 0x20 || u > 0x7e)) return false;
    }
    return true;
}

// ddmm.mmmm with a hemisphere letter. Degrees and minutes are not separated by
// anything, which is why this cannot be a plain strtod.
bool parseCoordinate(const std::string& value, const std::string& hemisphere,
                     bool latitude, double& out) {
    if (hemisphere.size() != 1 || value.find_first_not_of("0123456789.") != std::string::npos) {
        return false;
    }
    const char h = static_cast<char>(std::toupper(static_cast<unsigned char>(hemisphere[0])));
    if (latitude ? (h != 'N' && h != 'S') : (h != 'E' && h != 'W')) return false;
    const size_t dot = value.find('.');
    const size_t degreeDigits = (dot == std::string::npos ? value.size() : dot);
    if (degreeDigits != (latitude ? 4u : 5u) ||
        (dot != std::string::npos && dot + 1 == value.size())) return false;
    const std::string degreesText = value.substr(0, degreeDigits - 2);
    const std::string minutesText = value.substr(degreeDigits - 2);
    double degrees = 0.0, minutes = 0.0;
    if (!parseFiniteDouble(degreesText, degrees) || !parseFiniteDouble(minutesText, minutes) ||
        minutes >= 60.0) return false;
    double result = degrees + minutes / 60.0;
    if (result > (latitude ? 90.0 : 180.0)) return false;
    if (h == 'S' || h == 'W') result = -result;
    out = result;
    return true;
}

std::string formatClock(const std::string& hhmmss) {
    if (hhmmss.size() < 6) return {};
    for (int i = 0; i < 6; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(hhmmss[static_cast<size_t>(i)]))) return {};
    }
    return hhmmss.substr(0, 2) + ":" + hhmmss.substr(2, 2) + ":" + hhmmss.substr(4, 2);
}

// Days from 1970-01-01 for a proleptic Gregorian date. timegm() is a GNU
// extension and mktime() would drag the local timezone into a UTC timestamp.
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// ddmmyy plus hhmmss.sss, as RMC reports them.
bool epochFromNmea(const std::string& date, const std::string& time, uint32_t& out) {
    if (date.size() < 6 || time.size() < 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(date[static_cast<size_t>(i)]))) return false;
        if (!std::isdigit(static_cast<unsigned char>(time[static_cast<size_t>(i)]))) return false;
    }
    const int day = std::stoi(date.substr(0, 2));
    const int month = std::stoi(date.substr(2, 2));
    const int shortYear = std::stoi(date.substr(4, 2));
    const int hour = std::stoi(time.substr(0, 2));
    const int minute = std::stoi(time.substr(2, 2));
    const int second = std::stoi(time.substr(4, 2));
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    if (hour > 23 || minute > 59 || second > 60) return false;
    // NMEA's two-digit year rolls in 2080; this application will have other
    // problems by then, and guessing 19xx would be worse today.
    const int year = 2000 + shortYear;
    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month),
                                       static_cast<unsigned>(day));
    const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    if (seconds < 0) return false;
    out = static_cast<uint32_t>(seconds);
    return true;
}

} // namespace

std::string GnssFix::coordText() const {
    return everValid ? formatLatLon(latitude, longitude) : std::string("no fix");
}

bool nmeaChecksumOk(const std::string& sentence) {
    if (sentence.size() < 2 || sentence[0] != '$') return false;
    const size_t star = sentence.rfind('*');
    if (star == std::string::npos) return true;      // no checksum offered
    if (sentence.size() < star + 3) return false;
    uint8_t sum = 0;
    for (size_t i = 1; i < star; ++i) sum ^= static_cast<uint8_t>(sentence[i]);
    const std::string want = sentence.substr(star + 1, 2);
    for (char c : want) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    const unsigned long got = std::strtoul(want.c_str(), nullptr, 16);
    return static_cast<unsigned long>(sum) == got;
}

bool parseNmeaSentence(const std::string& sentence, GnssFix& fix, uint64_t nowMs) {
    if (sentence.size() < 7 || sentence[0] != '$') return false;
    if (!nmeaChecksumOk(sentence)) return false;

    const size_t star = sentence.rfind('*');
    const std::string body = sentence.substr(1, (star == std::string::npos ? sentence.size() - 1
                                                                           : star - 1));
    const std::vector<std::string> f = splitFields(body, ',');
    if (f.empty() || f[0].size() != 5 || f[0].find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") !=
                                             std::string::npos) return false;
    // Talker IDs differ per constellation (GP, GL, GA, GB, GN); the last three
    // characters are the sentence type and the only part worth switching on.
    const std::string type = f[0].substr(f[0].size() - 3);
    // A fix is the coordinates plus the moment this station saw them; the
    // receiver's own clock is kept as it arrived, whenever it arrived.
    const auto adoptCoords = [&](double lat, double lon) {
        fix.latitude = lat;
        fix.longitude = lon;
        fix.valid = true;
        fix.everValid = true;
        fix.updatedMs = nowMs;
    };
    const auto noteClock = [&](const std::string& hhmmss) {
        const std::string clock = formatClock(hhmmss);
        if (!clock.empty()) fix.utc = clock;
    };

    if (type == "GGA") {
        if (f.size() < 10) return false;
        int quality = 0;
        parseInteger(f[6], quality);
        double lat = 0.0, lon = 0.0;
        const bool haveCoords = parseCoordinate(f[2], f[3], true, lat) &&
                                parseCoordinate(f[4], f[5], false, lon);
        if (quality > 0 && haveCoords) {
            adoptCoords(lat, lon);
        } else {
            fix.valid = false;
        }
        int used = 0;
        fix.satellitesUsed = parseInteger(f[7], used) && used >= 0 && used <= 999 ? used : 0;
        double hdop = 0.0;
        fix.hdop = parseFiniteDouble(f[8], hdop) && hdop >= 0.0 ? hdop : 0.0;
        double altitude = 0.0;
        fix.haveAltitude = parseFiniteDouble(f[9], altitude) &&
                           altitude >= std::numeric_limits<int32_t>::min() &&
                           altitude <= std::numeric_limits<int32_t>::max();
        if (fix.haveAltitude) {
            fix.altitudeM = altitude;
        }
        noteClock(f[1]);
        return true;
    }

    if (type == "RMC") {
        if (f.size() < 10) return false;
        const bool active = f[2].size() == 1 &&
                            std::toupper(static_cast<unsigned char>(f[2][0])) == 'A';
        double lat = 0.0, lon = 0.0;
        const bool haveCoords = parseCoordinate(f[3], f[4], true, lat) &&
                                parseCoordinate(f[5], f[6], false, lon);
        if (active && haveCoords) {
            adoptCoords(lat, lon);
            uint32_t epoch = 0;
            if (epochFromNmea(f[9], f[1], epoch)) fix.utcSeconds = epoch;
        } else {
            fix.valid = false;
        }
        double speedKnots = 0.0;
        fix.haveSpeed = parseFiniteDouble(f[7], speedKnots) && speedKnots >= 0.0 &&
                        std::isfinite(speedKnots * 1.852);
        if (fix.haveSpeed) {
            fix.speedKph = speedKnots * 1.852;
        }
        double course = 0.0;
        fix.haveCourse = parseFiniteDouble(f[8], course) && course >= 0.0 && course < 360.0;
        if (fix.haveCourse) {
            fix.courseDeg = course;
        }
        noteClock(f[1]);
        return true;
    }

    if (type == "GSV") {
        if (f.size() < 4) return false;
        int inView = 0;
        if (parseInteger(f[3], inView) && inView >= 0 && inView <= 999) {
            // Field 3 is that constellation's total, so the sky is the sum
            // over talkers, not whichever talker spoke last.
            fix.inViewByTalker[f[0].substr(0, f[0].size() - 3)] = inView;
            int total = 0;
            for (const auto& entry : fix.inViewByTalker) total += entry.second;
            fix.satellitesInView = total;
        }
        return true;
    }

    if (type == "TXT") {
        // $GPTXT,01,01,01,ANTENNA OPEN*25: total, number, severity, text. The
        // text is the receiver talking about itself, kept for the status screen.
        if (f.size() < 5) return false;
        fix.receiverText = f[4];
        return true;
    }

    // GSA, VTG and the rest are well-formed and simply not needed here.
    return true;
}

bool Gnss::open(const std::string& device, int baud, std::string& error) {
    close();
    if (!port_.open(device, baud, error)) {
        lastError_ = error;
        return false;
    }
    lastError_.clear();
    buf_.clear();
    fix_ = GnssFix{};
    baud_ = baud;
    bytes_ = 0;
    legibleLines_ = 0;
    sentences_ = 0;
    lastSentenceMs_ = 0;
    openedMs_ = nowMs();
    return true;
}

void Gnss::close() {
    port_.close();
    buf_.clear();
    baud_ = 0;
    openedMs_ = 0;
}

void Gnss::adoptFix(const GnssFix& fix, uint64_t nowMs) {
    fix_ = fix;
    if (fix_.valid) {
        fix_.everValid = true;
        fix_.updatedMs = nowMs;
    }
    ++sentences_;
    lastSentenceMs_ = nowMs;
}

void Gnss::consumeLine(const std::string& line, uint64_t nowMs) {
    if (line.empty() || line[0] != '$') {
        if (legibleText(line)) ++legibleLines_;
        return;
    }
    if (!parseNmeaSentence(line, fix_, nowMs)) return;
    // Presence is proved by a sentence that carried a checksum and passed it.
    // parseNmeaSentence tolerates a missing one for the receivers that omit
    // it, but a wire sampled at the wrong rate throws up the odd '$' followed
    // by whatever, and that must not count as a receiver.
    if (line.rfind('*') == std::string::npos) return;
    ++sentences_;
    lastSentenceMs_ = nowMs;
}

void Gnss::poll(uint64_t nowMs) {
    if (!port_.isOpen()) return;
    std::string incoming;
    const int n = port_.read(incoming);
    if (n < 0) {
        lastError_ = "GNSS serial read failed";
        close();
        return;
    }
    if (n == 0) return;
    bytes_ += static_cast<size_t>(n);
    buf_ += incoming;
    if (buf_.size() > kMaxBufferBytes) {
        // Not NMEA. Keep the tail so a real sentence can still resynchronise.
        buf_.erase(0, buf_.size() - kMaxLineBytes);
    }
    for (;;) {
        const size_t nl = buf_.find_first_of("\r\n");
        if (nl == std::string::npos) {
            if (buf_.size() > kMaxLineBytes) buf_.clear();
            return;
        }
        const std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        consumeLine(line, nowMs);
    }
}

std::string Gnss::statusText(uint64_t nowMs) const {
    if (!port_.isOpen()) {
        return lastError_.empty() ? "GNSS off" : "GNSS: " + lastError_;
    }
    if (sentences_ == 0) return "GNSS: no NMEA yet";
    if (!fix_.everValid) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "GNSS: searching, %d sats in view",
                      fix_.satellitesInView);
        return buf;
    }
    char buf[96];
    const uint64_t ageMs = nowMs > fix_.updatedMs ? nowMs - fix_.updatedMs : 0;
    std::snprintf(buf, sizeof(buf), "GNSS %s  %s  %d sats%s",
                  fix_.valid ? "fix" : "stale", fix_.coordText().c_str(),
                  fix_.satellitesUsed,
                  (!fix_.valid || ageMs > 15000) ? "  (no update)" : "");
    return buf;
}

} // namespace bf
