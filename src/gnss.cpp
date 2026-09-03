#include "gnss.h"
#include "input.h"

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

std::vector<std::string> splitFields(const std::string& body) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t comma = body.find(',', start);
        if (comma == std::string::npos) {
            out.push_back(body.substr(start));
            return out;
        }
        out.push_back(body.substr(start, comma - start));
        start = comma + 1;
    }
}

// ddmm.mmmm with a hemisphere letter. Degrees and minutes are not separated by
// anything, which is why this cannot be a plain strtod.
bool parseCoordinate(const std::string& value, const std::string& hemisphere, double& out) {
    if (value.size() < 3 || hemisphere.empty()) return false;
    const size_t dot = value.find('.');
    const size_t degreeDigits = (dot == std::string::npos ? value.size() : dot);
    if (degreeDigits < 3) return false;
    const std::string degreesText = value.substr(0, degreeDigits - 2);
    const std::string minutesText = value.substr(degreeDigits - 2);
    char* end = nullptr;
    const double degrees = std::strtod(degreesText.c_str(), &end);
    if (end == degreesText.c_str()) return false;
    const double minutes = std::strtod(minutesText.c_str(), &end);
    if (end == minutesText.c_str()) return false;
    double result = degrees + minutes / 60.0;
    const char h = static_cast<char>(std::toupper(static_cast<unsigned char>(hemisphere[0])));
    if (h == 'S' || h == 'W') result = -result;
    else if (h != 'N' && h != 'E') return false;
    out = result;
    return true;
}

bool parseDouble(const std::string& value, double& out) {
    if (value.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str()) return false;
    out = parsed;
    return true;
}

bool parseInt(const std::string& value, int& out) {
    if (value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str()) return false;
    out = static_cast<int>(parsed);
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
    if (!everValid) return "no fix";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.5f, %.5f", latitude, longitude);
    return buf;
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
    const std::vector<std::string> f = splitFields(body);
    if (f.empty() || f[0].size() < 5) return false;
    // Talker IDs differ per constellation (GP, GL, GA, GB, GN); the last three
    // characters are the sentence type and the only part worth switching on.
    const std::string type = f[0].substr(f[0].size() - 3);

    if (type == "GGA") {
        if (f.size() < 10) return false;
        int quality = 0;
        parseInt(f[6], quality);
        double lat = 0.0, lon = 0.0;
        const bool haveCoords = parseCoordinate(f[2], f[3], lat) &&
                                parseCoordinate(f[4], f[5], lon);
        if (quality > 0 && haveCoords) {
            fix.latitude = lat;
            fix.longitude = lon;
            fix.valid = true;
            fix.everValid = true;
            fix.updatedMs = nowMs;
        } else if (quality == 0) {
            fix.valid = false;
        }
        parseInt(f[7], fix.satellitesUsed);
        parseDouble(f[8], fix.hdop);
        double altitude = 0.0;
        if (f.size() > 9 && parseDouble(f[9], altitude)) {
            fix.altitudeM = altitude;
            fix.haveAltitude = true;
        }
        const std::string clock = formatClock(f[1]);
        if (!clock.empty()) fix.utc = clock;
        return true;
    }

    if (type == "RMC") {
        if (f.size() < 10) return false;
        const bool active = !f[2].empty() &&
                            std::toupper(static_cast<unsigned char>(f[2][0])) == 'A';
        double lat = 0.0, lon = 0.0;
        const bool haveCoords = parseCoordinate(f[3], f[4], lat) &&
                                parseCoordinate(f[5], f[6], lon);
        if (active && haveCoords) {
            fix.latitude = lat;
            fix.longitude = lon;
            fix.valid = true;
            fix.everValid = true;
            fix.updatedMs = nowMs;
            uint32_t epoch = 0;
            if (epochFromNmea(f[9], f[1], epoch)) fix.utcSeconds = epoch;
        } else if (!active) {
            fix.valid = false;
        }
        double speedKnots = 0.0;
        if (parseDouble(f[7], speedKnots)) {
            fix.speedKph = speedKnots * 1.852;
            fix.haveSpeed = true;
        }
        double course = 0.0;
        if (parseDouble(f[8], course)) {
            fix.courseDeg = course;
            fix.haveCourse = true;
        }
        const std::string clock = formatClock(f[1]);
        if (!clock.empty()) fix.utc = clock;
        return true;
    }

    if (type == "GSV") {
        if (f.size() < 4) return false;
        int inView = 0;
        if (parseInt(f[3], inView)) fix.satellitesInView = inView;
        return true;
    }

    // GSA, VTG, TXT and the rest are well-formed and simply not needed here.
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
    sentences_ = 0;
    lastSentenceMs_ = 0;
    openedMs_ = nowMs();
    return true;
}

void Gnss::close() {
    port_.close();
    buf_.clear();
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
    if (line.empty() || line[0] != '$') return;
    if (!parseNmeaSentence(line, fix_, nowMs)) return;
    ++sentences_;
    lastSentenceMs_ = nowMs;
}

void Gnss::poll(uint64_t nowMs) {
    if (!port_.isOpen()) return;
    std::string incoming;
    const int n = port_.read(incoming);
    if (n < 0) {
        lastError_ = "GNSS serial read failed";
        port_.close();
        buf_.clear();
        return;
    }
    if (n == 0) return;
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
