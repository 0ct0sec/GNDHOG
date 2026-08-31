#pragma once
#include <map>
#include <string>

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

private:
    void respond(const std::string& command);
    void emit(const std::string& text);
    void flush();

    int master_ = -1;
    std::string slavePath_;
    std::string in_;
    std::string out_;
    bool cliMode_ = false;
    std::map<std::string, std::string> params_;
};

// The canned `diff all` body the simulator serves; also used as a test fixture.
const char* simDiffAll();

} // namespace bf
