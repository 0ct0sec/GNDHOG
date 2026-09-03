#include "compass.h"
#include "storage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

namespace bf {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool readNumber(const std::string& path, double& out) {
    const std::string text = readFirstLine(path);
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    out = value;
    return true;
}

bool fileExists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

double normalise(double deg) {
    deg = std::fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    return deg;
}

} // namespace

// ------------------------------------------------------------- discovery

bool Compass::discover() {
    const char* override = ::getenv("BFCLI_IIO_DIR");
    return discoverIn(override && *override ? override : "/sys/bus/iio/devices");
}

bool Compass::discoverIn(const std::string& root) {
    magnDir_.clear();
    accelDir_.clear();
    magnName_.clear();
    DIR* d = ::opendir(root.c_str());
    if (!d) return false;
    std::vector<std::string> entries;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        entries.push_back(name);
    }
    ::closedir(d);
    std::sort(entries.begin(), entries.end());
    for (const std::string& name : entries) {
        const std::string dir = root + "/" + name;
        if (magnDir_.empty() && fileExists(dir + "/in_magn_x_raw") &&
            fileExists(dir + "/in_magn_y_raw") && fileExists(dir + "/in_magn_z_raw")) {
            magnDir_ = dir;
            magnName_ = readFirstLine(dir + "/name");
            if (!readNumber(dir + "/in_magn_scale", magnScale_) || magnScale_ <= 0.0) {
                magnScale_ = 1.0;
            }
            parseMountMatrix(readFirstLine(dir + "/in_mount_matrix"), magnMatrix_);
        }
        if (accelDir_.empty() && fileExists(dir + "/in_accel_x_raw") &&
            fileExists(dir + "/in_accel_y_raw") && fileExists(dir + "/in_accel_z_raw")) {
            accelDir_ = dir;
            parseMountMatrix(readFirstLine(dir + "/in_mount_matrix"), accelMatrix_);
        }
    }
    return available();
}

bool Compass::parseMountMatrix(const std::string& text, double matrix[9]) {
    // "1, 0, 0; 0, 1, 0; 0, 0, 1" -- rows separated by ';', columns by ','.
    double parsed[9];
    int count = 0;
    std::string cell;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ';';
        if (c == ',' || c == ';') {
            if (count >= 9) return false;
            char* end = nullptr;
            const double value = std::strtod(cell.c_str(), &end);
            if (end == cell.c_str()) return false;
            parsed[count++] = value;
            cell.clear();
        } else if (c != ' ') {
            cell.push_back(c);
        }
    }
    if (count != 9) return false;
    for (int i = 0; i < 9; ++i) matrix[i] = parsed[i];
    return true;
}

// -------------------------------------------------------------- reading

bool Compass::readVector(const std::string& dir, const char* prefix, double& x, double& y,
                         double& z) const {
    const std::string base = dir + "/" + prefix;
    return readNumber(base + "_x_raw", x) && readNumber(base + "_y_raw", y) &&
           readNumber(base + "_z_raw", z);
}

void Compass::applyMatrix(const double m[9], double& x, double& y, double& z) const {
    const double nx = m[0] * x + m[1] * y + m[2] * z;
    const double ny = m[3] * x + m[4] * y + m[5] * z;
    const double nz = m[6] * x + m[7] * y + m[8] * z;
    x = nx;
    y = ny;
    z = nz;
}

double Compass::headingFromField(double mx, double my, double mz, bool haveGravity,
                                 double gx, double gy, double gz, bool mirror) {
    // "Up" is where the accelerometer says gravity's reaction points. Without
    // one, the board is assumed level and its z axis is up.
    double ux = 0.0, uy = 0.0, uz = 1.0;
    if (haveGravity) {
        const double g = std::sqrt(gx * gx + gy * gy + gz * gz);
        if (g > 1e-6) {
            ux = gx / g;
            uy = gy / g;
            uz = gz / g;
        }
    }
    // Forward is the board's x axis, projected onto the horizontal plane.
    double fx = 1.0 - ux * ux, fy = -ux * uy, fz = -ux * uz;
    const double f = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (f < 1e-6) return 0.0;        // pointing straight up: no heading exists
    fx /= f;
    fy /= f;
    fz /= f;
    // Left is up cross forward, so a clockwise turn increases the heading.
    const double lx = uy * fz - uz * fy;
    const double ly = uz * fx - ux * fz;
    const double lz = ux * fy - uy * fx;
    // The horizontal field is the field with its vertical component removed;
    // the dot products below already ignore that component.
    const double along = mx * fx + my * fy + mz * fz;
    double left = mx * lx + my * ly + mz * lz;
    if (mirror) left = -left;
    return normalise(std::atan2(left, along) * 180.0 / kPi);
}

void Compass::poll(uint64_t nowMs) {
    if (!available() || nowMs < nextPollMs_) return;
    nextPollMs_ = nowMs + kPollIntervalMs;

    double mx = 0.0, my = 0.0, mz = 0.0;
    if (!readVector(magnDir_, "in_magn", mx, my, mz)) {
        reading_.valid = false;
        return;
    }
    applyMatrix(magnMatrix_, mx, my, mz);
    lastRawX_ = mx;
    lastRawY_ = my;
    lastRawZ_ = mz;
    haveLastRaw_ = true;

    if (calibrating_) {
        const double raw[3] = {mx, my, mz};
        for (int i = 0; i < 3; ++i) {
            if (calSamples_ == 0 || raw[i] < calMin_[i]) calMin_[i] = raw[i];
            if (calSamples_ == 0 || raw[i] > calMax_[i]) calMax_[i] = raw[i];
        }
        ++calSamples_;
        if (calXY_.size() >= kCalibrationMaxSamples) calXY_.erase(calXY_.begin());
        calXY_.emplace_back(mx, my);
    }

    double gx = 0.0, gy = 0.0, gz = 0.0;
    bool haveGravity = false;
    if (haveAccelerometer() && readVector(accelDir_, "in_accel", gx, gy, gz)) {
        applyMatrix(accelMatrix_, gx, gy, gz);
        haveGravity = true;
    }

    const double cx = mx - cal_.xOff, cy = my - cal_.yOff, cz = mz - cal_.zOff;
    const double magnetic = normalise(
        headingFromField(cx, cy, cz, haveGravity, gx, gy, gz, cal_.mirror) +
        cal_.mountOffsetDeg);
    const double rad = magnetic * kPi / 180.0;
    if (!haveSmooth_) {
        smoothCos_ = std::cos(rad);
        smoothSin_ = std::sin(rad);
        haveSmooth_ = true;
    } else {
        smoothCos_ = 0.6 * smoothCos_ + 0.4 * std::cos(rad);
        smoothSin_ = 0.6 * smoothSin_ + 0.4 * std::sin(rad);
    }
    const double smoothed = normalise(std::atan2(smoothSin_, smoothCos_) * 180.0 / kPi);

    reading_.valid = true;
    reading_.magneticDeg = smoothed;
    reading_.headingDeg = normalise(smoothed + cal_.declinationDeg);
    const double magnitude = std::sqrt(cx * cx + cy * cy + cz * cz);
    reading_.fieldMicroTesla = magnitude * magnScale_ * 100.0;   // gauss -> microtesla
    if (cal_.hardIron && cal_.fieldNorm > 0.0) {
        reading_.disturbed = std::fabs(magnitude - cal_.fieldNorm) > 0.35 * cal_.fieldNorm;
    } else {
        // Uncalibrated: only the Earth's plausible range can be checked.
        reading_.disturbed = reading_.fieldMicroTesla < 15.0 || reading_.fieldMicroTesla > 110.0;
    }
    reading_.haveTilt = haveGravity;
    if (haveGravity) {
        const double g = std::sqrt(gx * gx + gy * gy + gz * gz);
        const double cosTilt = g > 1e-6 ? std::fabs(gz) / g : 1.0;
        reading_.tiltDeg = std::acos(std::min(1.0, std::max(-1.0, cosTilt))) * 180.0 / kPi;
    }
    reading_.sampledMs = nowMs;
}

bool Compass::usable(uint64_t nowMs) const {
    if (!available() || !reading_.valid || !cal_.hardIron) return false;
    if (nowMs - reading_.sampledMs > kStaleMs) return false;
    if (reading_.disturbed) return false;
    if (reading_.haveTilt && reading_.tiltDeg > 40.0) return false;
    return true;
}

// ---------------------------------------------------------- calibration

void Compass::beginCalibration() {
    calibrating_ = true;
    calSamples_ = 0;
    for (int i = 0; i < 3; ++i) calMin_[i] = calMax_[i] = 0.0;
    calXY_.clear();
}

double Compass::calibrationCoverage() const {
    if (calXY_.empty()) return 0.0;
    // Twelve 30-degree sectors around the centre of everything seen so far.
    // Early samples are re-judged against the final centre, so a turn that
    // started with a poor estimate is still counted as the full turn it was.
    const double cx = (calMin_[0] + calMax_[0]) / 2.0;
    const double cy = (calMin_[1] + calMax_[1]) / 2.0;
    bool sector[12] = {false};
    for (const auto& xy : calXY_) {
        const double angle = normalise(std::atan2(xy.second - cy, xy.first - cx) * 180.0 / kPi);
        sector[static_cast<int>(angle / 30.0) % 12] = true;
    }
    int seen = 0;
    for (bool s : sector) seen += s ? 1 : 0;
    return seen / 12.0;
}

bool Compass::finishCalibration() {
    if (!calibrating_) return false;
    if (calSamples_ < kCalibrationMinSamples || calibrationCoverage() < kCalibrationMinCoverage) {
        return false;
    }
    cal_.xOff = (calMin_[0] + calMax_[0]) / 2.0;
    cal_.yOff = (calMin_[1] + calMax_[1]) / 2.0;
    cal_.zOff = (calMin_[2] + calMax_[2]) / 2.0;
    // The horizontal axes were swept through a circle; their half-spans are
    // the field's radius. z was only tilted through, so it is left out.
    cal_.fieldNorm = ((calMax_[0] - calMin_[0]) + (calMax_[1] - calMin_[1])) / 4.0;
    cal_.hardIron = true;
    calibrating_ = false;
    calXY_.clear();
    haveSmooth_ = false;
    return true;
}

void Compass::cancelCalibration() {
    calibrating_ = false;
    calXY_.clear();
}

bool Compass::alignTo(double trueHeadingDeg, uint64_t nowMs) {
    if (!reading_.valid || nowMs - reading_.sampledMs > kStaleMs) return false;
    // The reading already includes the old mount offset; take it back out.
    const double uncorrected = normalise(reading_.magneticDeg - cal_.mountOffsetDeg);
    cal_.mountOffsetDeg = normalise(trueHeadingDeg - cal_.declinationDeg - uncorrected);
    if (cal_.mountOffsetDeg > 180.0) cal_.mountOffsetDeg -= 360.0;
    cal_.aligned = true;
    haveSmooth_ = false;
    return true;
}

std::string Compass::statusText(uint64_t nowMs) const {
    if (!available()) return "no magnetometer";
    if (!reading_.valid) return "compass: no sample yet";
    char buf[96];
    if (!cal_.hardIron) {
        std::snprintf(buf, sizeof(buf), "compass %03d uncalibrated", static_cast<int>(reading_.headingDeg + 0.5) % 360);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "compass %03d%s%s%s",
                  static_cast<int>(reading_.headingDeg + 0.5) % 360,
                  reading_.disturbed ? " disturbed" : "",
                  reading_.haveTilt && reading_.tiltDeg > 40.0 ? " tilted" : "",
                  nowMs - reading_.sampledMs > kStaleMs ? " stale" : "");
    return buf;
}

} // namespace bf
