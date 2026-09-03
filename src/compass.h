#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bf {

// The Cardputer Zero carries a Bosch BMM150 magnetometer on the auxiliary bus
// of its BMI270 IMU. The kernel presents both through the IIO sysfs class
// (bmc150_magn and bmi270 drivers), world-readable, so this is a handful of
// text files and some vector arithmetic, not a driver.
//
// A magnetometer is only a compass after two things nobody can skip: the
// hard-iron offset of the board it is soldered to (measured by turning the
// device through a circle), and the angle between the chip's x axis and the
// direction the operator calls "forward" (measured once against something
// known, such as the GNSS track while walking). Both are kept in config.ini.
struct CompassReading {
    bool valid = false;
    double headingDeg = 0.0;         // true heading, 0..360, every correction applied
    double magneticDeg = 0.0;        // before declination
    double fieldMicroTesla = 0.0;    // corrected field magnitude
    bool haveTilt = false;
    double tiltDeg = 0.0;            // between gravity and the board normal
    bool disturbed = false;          // the field is not the Earth's alone
    uint64_t sampledMs = 0;
};

struct CompassCalibration {
    bool hardIron = false;           // the offsets below were measured
    double xOff = 0.0, yOff = 0.0, zOff = 0.0;   // raw units
    double fieldNorm = 0.0;          // corrected magnitude during calibration, raw units
    bool aligned = false;            // mountOffsetDeg was set against a known heading
    double mountOffsetDeg = 0.0;
    double declinationDeg = 0.0;     // east positive; hand-set in config.ini
    bool mirror = false;             // a chip whose z points into the board
};

class Compass {
public:
    // Walks /sys/bus/iio/devices, or BFCLI_IIO_DIR when set, for a device
    // that reports in_magn_x_raw, and separately for one reporting
    // in_accel_x_raw. Absence is normal: the host build has neither.
    bool discover();
    bool discoverIn(const std::string& root);
    bool available() const { return !magnDir_.empty(); }
    bool haveAccelerometer() const { return !accelDir_.empty(); }
    const std::string& magnetometerName() const { return magnName_; }

    void setCalibration(const CompassCalibration& calibration) { cal_ = calibration; }
    const CompassCalibration& calibration() const { return cal_; }

    // Reads the sensors at most every kPollIntervalMs. Cheap enough to call
    // every frame; it returns early until the interval has passed.
    void poll(uint64_t nowMs);
    const CompassReading& reading() const { return reading_; }
    // A heading worth walking by: fresh, calibrated, level enough, and not
    // sitting on a car bonnet.
    bool usable(uint64_t nowMs) const;

    // ---- calibration
    void beginCalibration();
    bool calibrating() const { return calibrating_; }
    int calibrationSamples() const { return calSamples_; }
    // Fraction of a full turn the samples so far have covered.
    double calibrationCoverage() const;
    // False, with the calibration still running, until enough of a circle has
    // been seen. True installs the offsets.
    bool finishCalibration();
    void cancelCalibration();
    // Sets the mount offset so that the current magnetic heading reads
    // `trueHeadingDeg`. False when there is no fresh sample to align.
    bool alignTo(double trueHeadingDeg, uint64_t nowMs);

    std::string statusText(uint64_t nowMs) const;

    static constexpr uint64_t kPollIntervalMs = 200;
    static constexpr uint64_t kStaleMs = 2000;
    static constexpr int kCalibrationMinSamples = 40;
    static constexpr double kCalibrationMinCoverage = 0.75;
    // Two minutes of turning at the poll rate; older samples are dropped.
    static constexpr size_t kCalibrationMaxSamples = 600;

    // Heading, in degrees clockwise from magnetic north, of the board's x
    // axis, from a field vector and (optionally) a gravity vector in the same
    // frame. Exposed for the self-test.
    static double headingFromField(double mx, double my, double mz, bool haveGravity,
                                   double gx, double gy, double gz, bool mirror);
    // Applies a sysfs mount matrix ("1, 0, 0; 0, 1, 0; 0, 0, 1") in place.
    static bool parseMountMatrix(const std::string& text, double matrix[9]);

private:
    bool readVector(const std::string& dir, const char* prefix, double& x, double& y,
                    double& z) const;
    void applyMatrix(const double matrix[9], double& x, double& y, double& z) const;

    std::string magnDir_, accelDir_, magnName_;
    double magnScale_ = 1.0;         // raw -> gauss
    double magnMatrix_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double accelMatrix_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    CompassCalibration cal_;
    CompassReading reading_;
    uint64_t nextPollMs_ = 0;
    // Smoothing on the unit vector, so 359 and 1 average to 0, not 180.
    bool haveSmooth_ = false;
    double smoothCos_ = 0.0, smoothSin_ = 0.0;
    double lastRawX_ = 0.0, lastRawY_ = 0.0, lastRawZ_ = 0.0;
    bool haveLastRaw_ = false;

    bool calibrating_ = false;
    int calSamples_ = 0;
    double calMin_[3] = {0, 0, 0};
    double calMax_[3] = {0, 0, 0};
    // The horizontal samples are kept so coverage can be judged against the
    // centre of the whole turn, not the centre of however much of it had
    // been seen when each sample arrived.
    std::vector<std::pair<double, double>> calXY_;
};

} // namespace bf
