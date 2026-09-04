#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bf {

// A stand-in Meshtastic radio on a pseudo-terminal, so the whole connect /
// config-download / node-list / chat path can be exercised on a dev host with
// no LoRa hardware attached. It is a test fixture: nothing in the app depends
// on it, and it deliberately mixes plain console text into the framed stream
// the way real firmware does.
class SimMesh {
public:
    ~SimMesh();
    bool start(std::string& error);
    void stop();
    void pump();
    const std::string& devicePath() const { return slavePath_; }
    bool running() const { return master_ >= 0; }

    // Test knobs.
    void setRegionUnset(bool unset) { regionUnset_ = unset; }
    void setTxEnabled(bool enabled) { txEnabled_ = enabled; }
    // Routing error returned for the next direct message: 0 acknowledges it.
    void setAckError(uint32_t reason) { ackError_ = reason; }
    void setAckDelayMs(uint64_t ms) { ackDelayMs_ = ms; }
    // Spreads the config download over time, one frame per interval, the way a
    // radio with a large node database and a busy console really answers.
    void setConfigDripMs(uint64_t ms) { configDripMs_ = ms; }
    int configRequestsReceived() const { return configRequestsReceived_; }
    // Queues an inbound text message from one of the fixture nodes.
    void injectText(uint32_t from, uint32_t to, const std::string& text);
    void injectPosition(uint32_t from, double latitude, double longitude);

    uint32_t selfNodeNum() const;
    uint32_t hilltopNodeNum() const;
    uint32_t vanNodeNum() const;
    int textPacketsReceived() const { return textPacketsReceived_; }
    int positionPacketsReceived() const { return positionPacketsReceived_; }
    int heartbeatsReceived() const { return heartbeatsReceived_; }
    const std::string& lastTextReceived() const { return lastTextReceived_; }

private:
    struct DelayedAck {
        uint32_t requestId = 0;
        uint32_t from = 0;
        uint64_t dueMs = 0;
        bool wanted = false;
    };

    void handleToRadio(const std::string& body);
    void sendConfig(uint32_t configId);
    void emitFrame(const std::string& payload);
    void emitConsole(const std::string& text);
    void flush();
    // Packet ids for what the fixture radio "hears": unique is all they need
    // to be, and an LCG keeps a run reproducible.
    uint32_t nextPacketId() {
        packetSeed_ = packetSeed_ * 1664525u + 1013904223u;
        return packetSeed_;
    }

    int master_ = -1;
    std::string slavePath_;
    std::string in_;
    std::string out_;
    bool regionUnset_ = false;
    bool txEnabled_ = true;
    uint32_t ackError_ = 0;
    uint64_t ackDelayMs_ = 60;
    uint64_t configDripMs_ = 0;
    uint64_t nextConfigFrameMs_ = 0;
    int configRequestsReceived_ = 0;
    std::vector<std::string> pendingConfig_;
    std::vector<DelayedAck> acks_;
    int textPacketsReceived_ = 0;
    int positionPacketsReceived_ = 0;
    int heartbeatsReceived_ = 0;
    std::string lastTextReceived_;
    bool bannerSent_ = false;
    uint32_t packetSeed_ = 0x51ED2701u;
};

} // namespace bf
