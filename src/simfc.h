#pragma once
#include <map>
#include <cstdint>
#include <string>
#include <vector>

namespace bf {

// A stand-in Betaflight flight controller on a pseudo-terminal, so the whole
// connect / CLI / backup / restore path can be exercised on a dev host with no
// hardware attached. It is a test fixture: nothing in the app depends on it.
class SimFc {
public:
    ~SimFc();
    bool start(std::string& error);
    void stop();
    void pump();
    const std::string& devicePath() const { return slavePath_; }
    bool running() const { return master_ >= 0; }
    void setVtxReady(bool ready) { vtxReady_ = ready; }
    void setPitModeSupported(bool supported) { pitModeSupported_ = supported; }
    void setCoreTemperatureC(int temperatureC) { coreTemperatureC_ = temperatureC; }

private:
    void respond(const std::string& command);
    void respondMsp(uint8_t command, const std::vector<uint8_t>& payload);
    void emitMsp(uint8_t command, const std::vector<uint8_t>& payload = {});
    void emit(const std::string& text);
    void flush();

    int master_ = -1;
    std::string slavePath_;
    std::string in_;
    std::string wireIn_;
    std::string out_;
    bool cliMode_ = false;
    uint8_t vtxPower_ = 3;
    bool vtxPitMode_ = false;
    bool vtxReady_ = true;
    bool pitModeSupported_ = true;
    int coreTemperatureC_ = 37;
    std::map<std::string, std::string> params_;
};

// The canned `diff all` body the simulator serves; also used as a test fixture.
const char* simDiffAll();

} // namespace bf
