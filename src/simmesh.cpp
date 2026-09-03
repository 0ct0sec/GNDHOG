#include "simmesh.h"
#include "input.h"
#include "meshtastic.h"
#include "protowire.h"
#include "simpty.h"

#include <cmath>
#include <ctime>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace bf {
namespace {

constexpr uint32_t kSelfNum = 0x33445566u;
constexpr uint32_t kHilltopNum = 0xA1B2C3D4u;
constexpr uint32_t kVanNum = 0x0BADF00Du;

// Greenwich and a point about 1.4 km north-east of it, so distance and bearing
// have something real to compute.
constexpr double kHilltopLat = 51.48180;
constexpr double kHilltopLon = 0.00420;

std::string positionPayload(double latitude, double longitude, uint32_t timeSeconds,
                            int32_t altitudeM, uint32_t sats) {
    pb::Writer p;
    p.sfixed32(1, static_cast<int32_t>(std::lround(latitude * 1e7)));
    p.sfixed32(2, static_cast<int32_t>(std::lround(longitude * 1e7)));
    p.i32(3, altitudeM);
    p.fixed32(4, timeSeconds);
    p.varint(19, sats);
    return p.data();
}

std::string userPayload(uint32_t num, const char* longName, const char* shortName,
                        uint32_t role) {
    pb::Writer u;
    u.bytes(1, meshNodeIdText(num));
    u.bytes(2, longName);
    u.bytes(3, shortName);
    u.varint(5, 9);                 // some HardwareModel; the app never trusts it
    u.varint(7, role);
    return u.data();
}

std::string deviceMetricsPayload(uint32_t battery, float voltage) {
    pb::Writer m;
    m.varint(1, battery);
    m.f32(2, voltage);
    return m.data();
}

std::string nodeInfoPayload(uint32_t num, const char* longName, const char* shortName,
                            uint32_t role, uint32_t lastHeard, float snr,
                            bool havePosition, double latitude, double longitude,
                            bool haveHops, uint32_t hopsAway, uint32_t battery,
                            float voltage) {
    pb::Writer n;
    n.varint(1, num);
    n.bytes(2, userPayload(num, longName, shortName, role));
    if (havePosition) {
        n.bytes(3, positionPayload(latitude, longitude, lastHeard, 42, 9));
    }
    n.f32(4, snr);
    n.fixed32(5, lastHeard);
    n.bytes(6, deviceMetricsPayload(battery, voltage));
    if (haveHops) n.varint(9, hopsAway);
    return n.data();
}

std::string wrapFromRadio(uint32_t field, const std::string& payload) {
    pb::Writer f;
    f.bytes(field, payload);
    return f.data();
}

std::string meshPacketPayload(uint32_t from, uint32_t to, uint32_t packetId,
                              uint32_t port, const std::string& payload,
                              uint32_t requestId, float snr, uint32_t hopStart,
                              uint32_t hopLimit) {
    pb::Writer data;
    data.varint(1, port);
    data.bytes(2, payload);
    if (requestId != 0) data.fixed32(6, requestId);

    pb::Writer packet;
    packet.fixed32(1, from);
    packet.fixed32(2, to);
    packet.message(4, data);
    packet.fixed32(6, packetId);
    packet.fixed32(7, static_cast<uint32_t>(::time(nullptr)));
    packet.f32(8, snr);
    packet.varint(9, hopLimit);
    packet.varint(15, hopStart);
    return packet.data();
}

} // namespace

uint32_t SimMesh::selfNodeNum() const { return kSelfNum; }
uint32_t SimMesh::hilltopNodeNum() const { return kHilltopNum; }
uint32_t SimMesh::vanNodeNum() const { return kVanNum; }

SimMesh::~SimMesh() { stop(); }

bool SimMesh::start(std::string& error) {
    stop();
    if (!openSimPty(master_, slavePath_, error)) return false;
    in_.clear();
    out_.clear();
    acks_.clear();
    bannerSent_ = false;
    textPacketsReceived_ = 0;
    positionPacketsReceived_ = 0;
    heartbeatsReceived_ = 0;
    lastTextReceived_.clear();
    return true;
}

void SimMesh::stop() {
#if defined(__linux__)
    if (master_ >= 0) ::close(master_);
#endif
    master_ = -1;
    slavePath_.clear();
    in_.clear();
    out_.clear();
    acks_.clear();
}

void SimMesh::emitFrame(const std::string& payload) {
    out_ += frameToRadio(payload);
}

void SimMesh::emitConsole(const std::string& text) { out_ += text; }

void SimMesh::flush() {
#if defined(__linux__)
    while (!out_.empty()) {
        const ssize_t n = ::write(master_, out_.data(), out_.size());
        if (n > 0) {
            out_.erase(0, static_cast<size_t>(n));
            continue;
        }
        break;
    }
#endif
}

void SimMesh::sendConfig(uint32_t configId) {
    const uint32_t now = static_cast<uint32_t>(::time(nullptr));

    // With a drip configured the frames are queued and released one per
    // interval by pump(); otherwise they all go out at once.
    std::vector<std::string> frames;
    auto push = [&](const std::string& payload) { frames.push_back(payload); };

    pb::Writer myInfo;
    myInfo.varint(1, kSelfNum);
    push(wrapFromRadio(3, myInfo.data()));

    pb::Writer metadata;
    metadata.bytes(1, "2.7.11.abcdef1");
    metadata.varint(7, 0);
    metadata.varint(9, 9);
    push(wrapFromRadio(13, metadata.data()));

    push(wrapFromRadio(4, nodeInfoPayload(kSelfNum, "GNDHOG BENCH", "GNDH", 0,
                                          now, 0.0f, false, 0, 0, false, 0,
                                          96, 4.05f)));
    // Real firmware interleaves its own console output with the frames; if this
    // application cannot survive that, it cannot survive a real radio.
    emitConsole("INFO  | Sending our nodeinfo to mesh\r\n");
    push(wrapFromRadio(4, nodeInfoPayload(kHilltopNum, "HILLTOP RELAY", "HILL", 2,
                                          now - 120, 6.25f, true, kHilltopLat,
                                          kHilltopLon, true, 1, 87, 3.98f)));
    push(wrapFromRadio(4, nodeInfoPayload(kVanNum, "VAN 2", "VAN2", 0,
                                          now - 5400, -3.5f, false, 0, 0,
                                          true, 2, 42, 3.62f)));

    pb::Writer lora;
    lora.boolean(1, true);
    lora.varint(2, 0);                                   // LONG_FAST
    lora.varint(7, regionUnset_ ? 0u : 3u);              // EU_868 unless muted
    lora.varint(8, 3);
    lora.boolean(9, txEnabled_);
    pb::Writer loraConfig;
    loraConfig.message(6, lora);
    push(wrapFromRadio(5, loraConfig.data()));

    pb::Writer position;
    position.boolean(3, false);
    position.varint(13, 2);                              // GpsMode NOT_PRESENT
    pb::Writer positionConfig;
    positionConfig.message(2, position);
    push(wrapFromRadio(5, positionConfig.data()));

    pb::Writer settings;
    settings.bytes(3, "LongFast");
    pb::Writer channel;
    channel.varint(1, 0);
    channel.message(2, settings);
    channel.varint(3, 1);                                // PRIMARY
    push(wrapFromRadio(10, channel.data()));

    pb::Writer complete;
    complete.varint(7, configId);
    push(complete.data());

    if (configDripMs_ == 0) {
        for (const std::string& payload : frames) emitFrame(payload);
        return;
    }
    // A re-request abandons whatever is still queued, exactly as the firmware
    // restarts the download when a client asks again.
    pendingConfig_ = std::move(frames);
    nextConfigFrameMs_ = nowMs() + configDripMs_;
}

void SimMesh::handleToRadio(const std::string& body) {
    pb::Reader r(body);
    while (r.next()) {
        if (r.field() == 3) {                            // want_config_id
            ++configRequestsReceived_;
            sendConfig(r.u32());
            continue;
        }
        if (r.field() == 7) {                            // heartbeat
            ++heartbeatsReceived_;
            continue;
        }
        if (r.field() != 1) continue;                    // packet

        uint32_t to = 0, packetId = 0, port = 0;
        bool wantAck = false;
        std::string payload;
        pb::Reader packet = r.sub();
        while (packet.next()) {
            switch (packet.field()) {
            case 2: to = packet.fixed32(); break;
            case 6: packetId = packet.fixed32(); break;
            case 10: wantAck = packet.boolean(); break;
            case 4: {
                pb::Reader data = packet.sub();
                while (data.next()) {
                    if (data.field() == 1) port = data.u32();
                    else if (data.field() == 2) payload = data.bytes();
                }
                break;
            }
            default: break;
            }
        }
        if (port == static_cast<uint32_t>(MeshPort::TextMessage)) {
            ++textPacketsReceived_;
            lastTextReceived_ = payload;
        } else if (port == static_cast<uint32_t>(MeshPort::Position)) {
            ++positionPacketsReceived_;
        }
        if (wantAck && packetId != 0) {
            DelayedAck ack;
            ack.requestId = packetId;
            ack.from = to;
            ack.dueMs = nowMs() + ackDelayMs_;
            ack.wanted = true;
            acks_.push_back(ack);
        }
    }
}

void SimMesh::injectText(uint32_t from, uint32_t to, const std::string& text) {
    packetSeed_ = packetSeed_ * 1664525u + 1013904223u;
    emitFrame(wrapFromRadio(
        2, meshPacketPayload(from, to, packetSeed_,
                             static_cast<uint32_t>(MeshPort::TextMessage), text, 0,
                             5.5f, 3, 2)));
    flush();
}

void SimMesh::injectPosition(uint32_t from, double latitude, double longitude) {
    packetSeed_ = packetSeed_ * 1664525u + 1013904223u;
    emitFrame(wrapFromRadio(
        2, meshPacketPayload(from, kMeshBroadcast, packetSeed_,
                             static_cast<uint32_t>(MeshPort::Position),
                             positionPayload(latitude, longitude,
                                             static_cast<uint32_t>(::time(nullptr)), 12, 8),
                             0, 4.0f, 3, 3)));
    flush();
}

void SimMesh::pump() {
#if defined(__linux__)
    if (master_ < 0) return;

    if (!bannerSent_) {
        bannerSent_ = true;
        emitConsole("\r\n//\\ E S H T /\\ S T / C\r\nINFO  | Booted, wake reason 0\r\n");
    }

    char buf[1024];
    for (;;) {
        const ssize_t n = ::read(master_, buf, sizeof(buf));
        if (n > 0) {
            in_.append(buf, static_cast<size_t>(n));
            continue;
        }
        break;
    }

    std::vector<std::string> frames;
    std::string ignored;
    extractMeshFrames(in_, frames, ignored);
    for (const std::string& frame : frames) handleToRadio(frame);

    const uint64_t now = nowMs();
    if (!pendingConfig_.empty() && now >= nextConfigFrameMs_) {
        emitFrame(pendingConfig_.front());
        pendingConfig_.erase(pendingConfig_.begin());
        nextConfigFrameMs_ = now + configDripMs_;
    }

    for (size_t i = 0; i < acks_.size();) {
        if (acks_[i].dueMs > now) {
            ++i;
            continue;
        }
        pb::Writer routing;
        routing.varint(3, ackError_);
        packetSeed_ = packetSeed_ * 1664525u + 1013904223u;
        emitFrame(wrapFromRadio(
            2, meshPacketPayload(acks_[i].from, kSelfNum, packetSeed_,
                                 static_cast<uint32_t>(MeshPort::Routing),
                                 routing.data(), acks_[i].requestId, 7.0f, 3, 3)));
        acks_.erase(acks_.begin() + static_cast<long>(i));
    }

    flush();
#endif
}

} // namespace bf
