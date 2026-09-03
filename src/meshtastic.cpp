#include "meshtastic.h"
#include "protowire.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bf {
namespace {

// ToRadio
constexpr uint32_t kToRadioPacket = 1;
constexpr uint32_t kToRadioWantConfigId = 3;
constexpr uint32_t kToRadioDisconnect = 4;
constexpr uint32_t kToRadioHeartbeat = 7;

// FromRadio
constexpr uint32_t kFromRadioPacket = 2;
constexpr uint32_t kFromRadioMyInfo = 3;
constexpr uint32_t kFromRadioNodeInfo = 4;
constexpr uint32_t kFromRadioConfig = 5;
constexpr uint32_t kFromRadioLogRecord = 6;
constexpr uint32_t kFromRadioConfigCompleteId = 7;
constexpr uint32_t kFromRadioRebooted = 8;
constexpr uint32_t kFromRadioChannel = 10;
constexpr uint32_t kFromRadioMetadata = 13;

// MeshPacket
constexpr uint32_t kPacketFrom = 1;
constexpr uint32_t kPacketTo = 2;
constexpr uint32_t kPacketChannel = 3;
constexpr uint32_t kPacketDecoded = 4;
constexpr uint32_t kPacketEncrypted = 5;
constexpr uint32_t kPacketId = 6;
constexpr uint32_t kPacketRxTime = 7;
constexpr uint32_t kPacketRxSnr = 8;
constexpr uint32_t kPacketHopLimit = 9;
constexpr uint32_t kPacketWantAck = 10;
constexpr uint32_t kPacketRxRssi = 12;
constexpr uint32_t kPacketViaMqtt = 14;
constexpr uint32_t kPacketHopStart = 15;

// Data
constexpr uint32_t kDataPortnum = 1;
constexpr uint32_t kDataPayload = 2;
constexpr uint32_t kDataRequestId = 6;

// NodeInfo
constexpr uint32_t kNodeNum = 1;
constexpr uint32_t kNodeUser = 2;
constexpr uint32_t kNodePosition = 3;
constexpr uint32_t kNodeSnr = 4;
constexpr uint32_t kNodeLastHeard = 5;
constexpr uint32_t kNodeDeviceMetrics = 6;
constexpr uint32_t kNodeViaMqtt = 8;
constexpr uint32_t kNodeHopsAway = 9;
constexpr uint32_t kNodeIsFavorite = 10;

// User
constexpr uint32_t kUserId = 1;
constexpr uint32_t kUserLongName = 2;
constexpr uint32_t kUserShortName = 3;
constexpr uint32_t kUserHwModel = 5;
constexpr uint32_t kUserRole = 7;
constexpr uint32_t kUserUnmessagable = 9;

// Position
constexpr uint32_t kPositionLatitudeI = 1;
constexpr uint32_t kPositionLongitudeI = 2;
constexpr uint32_t kPositionAltitude = 3;
constexpr uint32_t kPositionTime = 4;
constexpr uint32_t kPositionLocationSource = 5;
constexpr uint32_t kPositionSatsInView = 19;
constexpr uint32_t kPositionPrecisionBits = 23;
// LocSource.LOC_EXTERNAL: the fix came from a receiver that is not the radio's
// own, which is exactly what a Cardputer Zero LoRa Cap GNSS is.
constexpr uint32_t kPositionSourceExternal = 3;

// DeviceMetrics, Routing, MyNodeInfo, DeviceMetadata, Config, Channel
constexpr uint32_t kMetricsBatteryLevel = 1;
constexpr uint32_t kMetricsVoltage = 2;
constexpr uint32_t kRoutingErrorReason = 3;
constexpr uint32_t kMyInfoNodeNum = 1;
constexpr uint32_t kMetadataFirmwareVersion = 1;
constexpr uint32_t kMetadataRole = 7;
constexpr uint32_t kMetadataHwModel = 9;
constexpr uint32_t kConfigPosition = 2;
constexpr uint32_t kConfigLora = 6;
constexpr uint32_t kLoraUsePreset = 1;
constexpr uint32_t kLoraModemPreset = 2;
constexpr uint32_t kLoraRegion = 7;
constexpr uint32_t kLoraHopLimit = 8;
constexpr uint32_t kLoraTxEnabled = 9;
constexpr uint32_t kPositionCfgFixed = 3;
constexpr uint32_t kPositionCfgGpsMode = 13;
constexpr uint32_t kChannelIndex = 1;
constexpr uint32_t kChannelSettings = 2;
constexpr uint32_t kChannelRole = 3;
constexpr uint32_t kChannelSettingsName = 3;

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6371000.0;

double toRadians(double degrees) { return degrees * kPi / 180.0; }
double toDegrees(double radians) { return radians * 180.0 / kPi; }


const char* stateText(MeshMessageState state) {
    switch (state) {
    case MeshMessageState::Received:  return "rx";
    case MeshMessageState::Queued:    return "queued";
    case MeshMessageState::Sent:      return "sent";
    case MeshMessageState::Delivered: return "ack";
    case MeshMessageState::Failed:    return "fail";
    }
    return "rx";
}

MeshMessageState stateFromText(const std::string& text) {
    if (text == "queued") return MeshMessageState::Queued;
    if (text == "sent") return MeshMessageState::Sent;
    if (text == "ack") return MeshMessageState::Delivered;
    if (text == "fail") return MeshMessageState::Failed;
    return MeshMessageState::Received;
}

uint32_t parseHex32(const std::string& text) {
    return static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 16));
}

} // namespace

// ------------------------------------------------------------------ values

std::string MeshPosition::coordText() const {
    if (!valid) return "no position";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.5f, %.5f", latitude, longitude);
    return buf;
}

std::string meshNodeIdText(uint32_t num) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "!%08x", num);
    return buf;
}

std::string MeshNode::idText() const { return meshNodeIdText(num); }

std::string MeshNode::label() const {
    if (!user.shortName.empty()) return user.shortName;
    if (!user.longName.empty()) return user.longName;
    return idText();
}

std::string MeshNode::title() const {
    if (!user.longName.empty()) return user.longName;
    if (!user.shortName.empty()) return user.shortName;
    return idText();
}

// ------------------------------------------------------------------ framing

std::string frameToRadio(const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 4);
    out.push_back(static_cast<char>(kMeshStart1));
    out.push_back(static_cast<char>(kMeshStart2));
    out.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
    out.push_back(static_cast<char>(payload.size() & 0xFF));
    out += payload;
    return out;
}

void extractMeshFrames(std::string& buf, std::vector<std::string>& frames,
                       std::string& log) {
    for (;;) {
        const size_t start = buf.find(static_cast<char>(kMeshStart1));
        if (start == std::string::npos) {
            // Nothing here can begin a frame, so all of it is the radio talking
            // to its own console.
            log += buf;
            buf.clear();
            return;
        }
        if (start > 0) {
            log.append(buf, 0, start);
            buf.erase(0, start);
        }
        if (buf.size() < 2) return;                 // wait for the second magic byte
        if (static_cast<uint8_t>(buf[1]) != kMeshStart2) {
            log.push_back(buf[0]);
            buf.erase(0, 1);
            continue;
        }
        if (buf.size() < 4) return;                 // wait for the length
        const size_t len = (static_cast<size_t>(static_cast<uint8_t>(buf[2])) << 8) |
                           static_cast<size_t>(static_cast<uint8_t>(buf[3]));
        if (len > kMeshMaxFrame) {
            // A length the protocol cannot produce means those two bytes were
            // log text that happened to spell the magic. Resync past them.
            log.push_back(buf[0]);
            buf.erase(0, 1);
            continue;
        }
        if (buf.size() < 4 + len) return;           // wait for the body
        frames.push_back(buf.substr(4, len));
        buf.erase(0, 4 + len);
    }
}

// ----------------------------------------------------------------- encoding

std::string encodeWantConfig(uint32_t configId) {
    pb::Writer w;
    w.varint(kToRadioWantConfigId, configId);
    return w.data();
}

std::string encodeHeartbeat() {
    pb::Writer w;
    w.emptyMessage(kToRadioHeartbeat);
    return w.data();
}

std::string encodeDisconnect() {
    pb::Writer w;
    w.boolean(kToRadioDisconnect, true);
    return w.data();
}

namespace {

std::string wrapPacket(uint32_t to, uint32_t channel, uint32_t packetId, bool wantAck,
                       const pb::Writer& data) {
    pb::Writer packet;
    // `from` is deliberately absent: the radio stamps its own node number, and
    // a client that fills it in can only get it wrong.
    packet.fixed32(kPacketTo, to);
    if (channel != 0) packet.varint(kPacketChannel, channel);
    packet.message(kPacketDecoded, data);
    packet.fixed32(kPacketId, packetId);
    if (wantAck) packet.boolean(kPacketWantAck, true);

    pb::Writer toRadio;
    toRadio.message(kToRadioPacket, packet);
    return toRadio.data();
}

} // namespace

std::string encodeTextPacket(uint32_t to, uint32_t channel, uint32_t packetId,
                             const std::string& text, bool wantAck) {
    pb::Writer data;
    data.varint(kDataPortnum, static_cast<uint64_t>(MeshPort::TextMessage));
    data.bytes(kDataPayload, text.size() > kMeshMaxTextBytes
                                 ? text.substr(0, kMeshMaxTextBytes)
                                 : text);
    return wrapPacket(to, channel, packetId, wantAck, data);
}

std::string encodePositionPacket(uint32_t to, uint32_t channel, uint32_t packetId,
                                 double latitude, double longitude,
                                 bool haveAltitude, int32_t altitudeM,
                                 uint32_t timeSeconds, uint32_t satsInView) {
    pb::Writer position;
    position.sfixed32(kPositionLatitudeI,
                      static_cast<int32_t>(std::lround(latitude * 1e7)));
    position.sfixed32(kPositionLongitudeI,
                      static_cast<int32_t>(std::lround(longitude * 1e7)));
    if (haveAltitude) position.i32(kPositionAltitude, altitudeM);
    if (timeSeconds != 0) position.fixed32(kPositionTime, timeSeconds);
    position.varint(kPositionLocationSource, kPositionSourceExternal);
    if (satsInView != 0) position.varint(kPositionSatsInView, satsInView);

    pb::Writer data;
    data.varint(kDataPortnum, static_cast<uint64_t>(MeshPort::Position));
    data.bytes(kDataPayload, position.data());
    // A position broadcast is informational; asking every hop for an ack would
    // put more traffic in the air than the position is worth.
    return wrapPacket(to, channel, packetId, to != kMeshBroadcast, data);
}

// ----------------------------------------------------------------- decoding

bool decodeMeshUser(const std::string& payload, MeshUser& out) {
    out = MeshUser{};
    pb::Reader r(payload);
    while (r.next()) {
        switch (r.field()) {
        case kUserId: out.id = r.bytes(); break;
        case kUserLongName: out.longName = r.bytes(); break;
        case kUserShortName: out.shortName = r.bytes(); break;
        case kUserHwModel: out.hwModel = r.u32(); break;
        case kUserRole: out.role = r.u32(); break;
        case kUserUnmessagable: out.unmessagable = r.boolean(); break;
        default: break;
        }
    }
    return r.ok();
}

bool decodeMeshPosition(const std::string& payload, MeshPosition& out) {
    out = MeshPosition{};
    bool haveLat = false, haveLon = false;
    pb::Reader r(payload);
    while (r.next()) {
        switch (r.field()) {
        case kPositionLatitudeI:
            out.latitude = static_cast<double>(r.sfixed32()) / 1e7;
            haveLat = true;
            break;
        case kPositionLongitudeI:
            out.longitude = static_cast<double>(r.sfixed32()) / 1e7;
            haveLon = true;
            break;
        case kPositionAltitude:
            out.altitudeM = r.i32();
            out.haveAltitude = true;
            break;
        case kPositionTime: out.time = r.fixed32(); break;
        case kPositionSatsInView: out.satsInView = r.u32(); break;
        case kPositionPrecisionBits: out.precisionBits = r.u32(); break;
        default: break;
        }
    }
    // Presence, not plausibility: a node at 0,0 that actually reported 0,0 is
    // still a report, and a node that reported nothing is not at the equator.
    out.valid = r.ok() && haveLat && haveLon;
    return r.ok();
}

bool decodeMeshRouting(const std::string& payload, bool& haveError, uint32_t& errorReason) {
    haveError = false;
    errorReason = 0;
    pb::Reader r(payload);
    while (r.next()) {
        if (r.field() == kRoutingErrorReason) {
            haveError = true;
            errorReason = r.u32();
        }
    }
    return r.ok();
}

bool decodeMeshDeviceMetrics(const std::string& payload, bool& haveBattery,
                             uint32_t& batteryLevel, bool& haveVoltage, float& voltage) {
    haveBattery = false;
    haveVoltage = false;
    batteryLevel = 0;
    voltage = 0.0f;
    pb::Reader r(payload);
    while (r.next()) {
        if (r.field() == kMetricsBatteryLevel) {
            batteryLevel = r.u32();
            haveBattery = true;
        } else if (r.field() == kMetricsVoltage) {
            voltage = r.f32();
            haveVoltage = true;
        }
    }
    return r.ok();
}

namespace {

bool decodeNodeInfo(const std::string& payload, MeshNode& out) {
    out = MeshNode{};
    pb::Reader r(payload);
    while (r.next()) {
        switch (r.field()) {
        case kNodeNum: out.num = r.u32(); break;
        case kNodeUser: decodeMeshUser(r.bytes(), out.user); break;
        case kNodePosition: decodeMeshPosition(r.bytes(), out.position); break;
        case kNodeSnr: out.snr = r.f32(); out.haveSnr = true; break;
        case kNodeLastHeard: out.lastHeard = r.fixed32(); break;
        case kNodeDeviceMetrics:
            decodeMeshDeviceMetrics(r.bytes(), out.haveBattery, out.batteryLevel,
                                    out.haveVoltage, out.voltage);
            break;
        case kNodeViaMqtt: out.viaMqtt = r.boolean(); break;
        case kNodeHopsAway: out.hopsAway = r.u32(); out.haveHops = true; break;
        case kNodeIsFavorite: out.favorite = r.boolean(); break;
        default: break;
        }
    }
    return r.ok();
}

void decodeData(const std::string& payload, MeshFromRadio& out) {
    pb::Reader r(payload);
    while (r.next()) {
        switch (r.field()) {
        case kDataPortnum: out.port = static_cast<MeshPort>(r.u32()); break;
        case kDataPayload: out.payload = r.bytes(); break;
        case kDataRequestId: out.requestId = r.fixed32(); break;
        default: break;
        }
    }
}

void decodePacket(const std::string& payload, MeshFromRadio& out) {
    pb::Reader r(payload);
    while (r.next()) {
        switch (r.field()) {
        case kPacketFrom: out.from = r.fixed32(); break;
        case kPacketTo: out.to = r.fixed32(); break;
        case kPacketChannel: out.channel = r.u32(); break;
        case kPacketDecoded: decodeData(r.bytes(), out); break;
        case kPacketEncrypted: out.encrypted = true; break;
        case kPacketId: out.packetId = r.fixed32(); break;
        case kPacketRxTime: out.rxTime = r.fixed32(); break;
        case kPacketRxSnr: out.rxSnr = r.f32(); out.haveSnr = true; break;
        case kPacketHopLimit: out.hopLimitPacket = r.u32(); break;
        case kPacketRxRssi: out.rxRssi = r.i32(); out.haveRssi = true; break;
        case kPacketViaMqtt: out.viaMqtt = r.boolean(); break;
        case kPacketHopStart: out.hopStart = r.u32(); break;
        default: break;
        }
    }
}

void decodeConfig(const std::string& payload, MeshFromRadio& out) {
    pb::Reader r(payload);
    while (r.next()) {
        if (r.field() == kConfigLora) {
            out.haveLoraConfig = true;
            pb::Reader lora = r.sub();
            while (lora.next()) {
                switch (lora.field()) {
                case kLoraUsePreset: out.usePreset = lora.boolean(); break;
                case kLoraModemPreset: out.modemPreset = lora.u32(); break;
                case kLoraRegion: out.region = lora.u32(); break;
                case kLoraHopLimit: out.hopLimit = lora.u32(); break;
                case kLoraTxEnabled: out.txEnabled = lora.boolean(); break;
                default: break;
                }
            }
        } else if (r.field() == kConfigPosition) {
            out.havePositionConfig = true;
            pb::Reader pos = r.sub();
            while (pos.next()) {
                if (pos.field() == kPositionCfgGpsMode) out.gpsMode = pos.u32();
                else if (pos.field() == kPositionCfgFixed) out.fixedPosition = pos.boolean();
            }
        }
    }
}

void decodeChannel(const std::string& payload, MeshFromRadio& out) {
    pb::Reader r(payload);
    while (r.next()) {
        switch (r.field()) {
        case kChannelIndex: out.channelIndex = r.u32(); break;
        case kChannelRole: out.channelRole = r.u32(); break;
        case kChannelSettings: {
            pb::Reader settings = r.sub();
            while (settings.next()) {
                if (settings.field() == kChannelSettingsName) out.channelName = settings.bytes();
            }
            break;
        }
        default: break;
        }
    }
}

void decodeMetadata(const std::string& payload, MeshFromRadio& out) {
    pb::Reader r(payload);
    while (r.next()) {
        switch (r.field()) {
        case kMetadataFirmwareVersion: out.firmwareVersion = r.bytes(); break;
        case kMetadataRole: out.role = r.u32(); break;
        case kMetadataHwModel: out.hwModel = r.u32(); break;
        default: break;
        }
    }
}

} // namespace

bool decodeMeshFromRadio(const std::string& body, MeshFromRadio& out) {
    out = MeshFromRadio{};
    pb::Reader r(body);
    while (r.next()) {
        switch (r.field()) {
        case kFromRadioPacket:
            out.kind = MeshFromRadio::Kind::Packet;
            decodePacket(r.bytes(), out);
            break;
        case kFromRadioMyInfo: {
            out.kind = MeshFromRadio::Kind::MyInfo;
            pb::Reader info = r.sub();
            while (info.next()) {
                if (info.field() == kMyInfoNodeNum) out.myNodeNum = info.u32();
            }
            break;
        }
        case kFromRadioNodeInfo:
            out.kind = MeshFromRadio::Kind::NodeInfo;
            decodeNodeInfo(r.bytes(), out.node);
            break;
        case kFromRadioConfig:
            out.kind = MeshFromRadio::Kind::Config;
            decodeConfig(r.bytes(), out);
            break;
        case kFromRadioLogRecord: {
            out.kind = MeshFromRadio::Kind::LogRecord;
            pb::Reader record = r.sub();
            while (record.next()) {
                if (record.field() == 1) out.logText = record.bytes();
            }
            break;
        }
        case kFromRadioConfigCompleteId:
            out.kind = MeshFromRadio::Kind::ConfigComplete;
            out.configCompleteId = r.u32();
            break;
        case kFromRadioRebooted:
            out.kind = MeshFromRadio::Kind::Rebooted;
            break;
        case kFromRadioChannel:
            out.kind = MeshFromRadio::Kind::Channel;
            decodeChannel(r.bytes(), out);
            break;
        case kFromRadioMetadata:
            out.kind = MeshFromRadio::Kind::Metadata;
            decodeMetadata(r.bytes(), out);
            break;
        default:
            break;
        }
    }
    return r.ok();
}

// ------------------------------------------------------------------ helpers

const char* meshRegionName(uint32_t region) {
    switch (region) {
    case 0: return "UNSET";
    case 1: return "US";
    case 2: return "EU_433";
    case 3: return "EU_868";
    case 4: return "CN";
    case 5: return "JP";
    case 6: return "ANZ";
    case 7: return "KR";
    case 8: return "TW";
    case 9: return "RU";
    case 10: return "IN";
    case 11: return "NZ_865";
    case 12: return "TH";
    case 13: return "LORA_24";
    case 14: return "UA_433";
    case 15: return "UA_868";
    case 16: return "MY_433";
    case 17: return "MY_919";
    case 18: return "SG_923";
    case 19: return "PH_433";
    case 20: return "PH_868";
    case 21: return "PH_915";
    case 22: return "ANZ_433";
    case 23: return "KZ_433";
    case 24: return "KZ_863";
    case 25: return "NP_865";
    case 26: return "BR_902";
    default: return "region?";
    }
}

const char* meshModemPresetName(uint32_t preset) {
    switch (preset) {
    case 0: return "LONG_FAST";
    case 1: return "LONG_SLOW";
    case 2: return "VERY_LONG_SLOW";
    case 3: return "MEDIUM_SLOW";
    case 4: return "MEDIUM_FAST";
    case 5: return "SHORT_SLOW";
    case 6: return "SHORT_FAST";
    case 7: return "LONG_MODERATE";
    case 8: return "SHORT_TURBO";
    case 9: return "LONG_TURBO";
    default: return "preset?";
    }
}

const char* meshRoleName(uint32_t role) {
    switch (role) {
    case 0: return "CLIENT";
    case 1: return "CLIENT_MUTE";
    case 2: return "ROUTER";
    case 3: return "ROUTER_CLIENT";
    case 4: return "REPEATER";
    case 5: return "TRACKER";
    case 6: return "SENSOR";
    case 7: return "TAK";
    case 8: return "CLIENT_HIDDEN";
    case 9: return "LOST_AND_FOUND";
    case 10: return "TAK_TRACKER";
    case 11: return "ROUTER_LATE";
    case 12: return "CLIENT_BASE";
    default: return "role?";
    }
}

const char* meshRoutingErrorText(uint32_t reason) {
    switch (reason) {
    case 0: return "delivered";
    case 1: return "no route";
    case 2: return "got NAK";
    case 3: return "timeout";
    case 4: return "no interface";
    case 5: return "max retransmit";
    case 6: return "no channel";
    case 7: return "too large";
    case 8: return "no response";
    case 9: return "duty cycle limit";
    case 32: return "bad request";
    case 33: return "not authorized";
    case 34: return "PKI failed";
    case 35: return "unknown public key";
    case 38: return "rate limited";
    default: return "rejected";
    }
}

const char* meshGpsModeName(uint32_t mode) {
    switch (mode) {
    case 0: return "disabled";
    case 1: return "enabled";
    case 2: return "not present";
    default: return "unknown";
    }
}

const char* meshPortName(MeshPort port) {
    switch (port) {
    case MeshPort::TextMessage: return "text";
    case MeshPort::Position: return "position";
    case MeshPort::NodeInfo: return "nodeinfo";
    case MeshPort::Routing: return "routing";
    case MeshPort::Admin: return "admin";
    case MeshPort::Waypoint: return "waypoint";
    case MeshPort::Detection: return "detection";
    case MeshPort::Telemetry: return "telemetry";
    case MeshPort::Traceroute: return "traceroute";
    case MeshPort::NeighborInfo: return "neighborinfo";
    case MeshPort::Unknown: return "unknown";
    }
    return "app";
}

std::string meshAgeText(uint64_t seconds) {
    char buf[32];
    if (seconds < 45) return "now";
    if (seconds < 3600) {
        std::snprintf(buf, sizeof(buf), "%lum", static_cast<unsigned long>(seconds / 60));
    } else if (seconds < 86400) {
        std::snprintf(buf, sizeof(buf), "%luh", static_cast<unsigned long>(seconds / 3600));
    } else {
        std::snprintf(buf, sizeof(buf), "%lud", static_cast<unsigned long>(seconds / 86400));
    }
    return buf;
}

double meshDistanceM(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = toRadians(lat1);
    const double phi2 = toRadians(lat2);
    const double dPhi = toRadians(lat2 - lat1);
    const double dLambda = toRadians(lon2 - lon1);
    const double a = std::sin(dPhi / 2) * std::sin(dPhi / 2) +
                     std::cos(phi1) * std::cos(phi2) *
                         std::sin(dLambda / 2) * std::sin(dLambda / 2);
    return 2.0 * kEarthRadiusM * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

double meshBearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = toRadians(lat1);
    const double phi2 = toRadians(lat2);
    const double dLambda = toRadians(lon2 - lon1);
    const double y = std::sin(dLambda) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2) -
                     std::sin(phi1) * std::cos(phi2) * std::cos(dLambda);
    double deg = toDegrees(std::atan2(y, x));
    if (deg < 0.0) deg += 360.0;
    return deg;
}

const char* meshCompassPoint(double bearingDeg) {
    static const char* const kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    if (!(bearingDeg >= 0.0) && !(bearingDeg <= 0.0)) return "?";   // NaN
    double normalised = std::fmod(bearingDeg, 360.0);
    if (normalised < 0.0) normalised += 360.0;
    const int index = static_cast<int>((normalised + 22.5) / 45.0) % 8;
    return kPoints[index];
}

std::string meshRangeText(double metres) {
    char buf[24];
    if (metres < 1000.0) {
        std::snprintf(buf, sizeof(buf), "%dm", static_cast<int>(metres + 0.5));
    } else if (metres < 100000.0) {
        std::snprintf(buf, sizeof(buf), "%.1fkm", metres / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0fkm", metres / 1000.0);
    }
    return buf;
}

std::string meshBearingText(double bearingDeg) {
    if (!(bearingDeg >= 0.0) && !(bearingDeg <= 0.0)) return "---";   // NaN
    double normalised = std::fmod(bearingDeg, 360.0);
    if (normalised < 0.0) normalised += 360.0;
    int whole = static_cast<int>(normalised + 0.5);
    if (whole >= 360) whole = 0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d %s", whole, meshCompassPoint(normalised));
    return buf;
}

double meshRelativeTurnDeg(double bearingDeg, double courseDeg) {
    double turn = std::fmod(bearingDeg - courseDeg, 360.0);
    if (turn > 180.0) turn -= 360.0;
    if (turn <= -180.0) turn += 360.0;
    return turn;
}

std::string meshTurnText(double relativeDeg) {
    const int whole = static_cast<int>(std::fabs(relativeDeg) + 0.5);
    if (whole <= 5) return "ahead";
    if (whole >= 170) return "behind";
    return std::string(relativeDeg < 0.0 ? "left " : "right ") + std::to_string(whole);
}

std::string meshAltitudeDiffText(double metres) {
    const int whole = static_cast<int>(std::fabs(metres) + 0.5);
    if (whole < 5) return "level";
    return std::string(metres < 0.0 ? "-" : "+") + std::to_string(whole) + "m";
}

std::string meshEscapeField(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string meshUnescapeField(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) {
            out.push_back(in[i]);
            continue;
        }
        switch (in[++i]) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '\\': out.push_back('\\'); break;
        default: out.push_back(in[i]); break;
        }
    }
    return out;
}

// -------------------------------------------------------- chat persistence

std::string formatMeshChat(const std::vector<MeshMessage>& messages) {
    std::string out =
        "# GNDHOG ZERO mesh conversation v1\n"
        "# stamp\tdir\tfrom\tto\tid\tchannel\tstate\ttext\tnote\n";
    char head[96];
    for (const MeshMessage& m : messages) {
        std::snprintf(head, sizeof(head), "%lld\t%s\t%08x\t%08x\t%08x\t%u\t%s\t",
                      static_cast<long long>(m.stampUtc), m.outgoing ? "out" : "in",
                      m.from, m.to, m.id, m.channel, stateText(m.state));
        out += head;
        out += meshEscapeField(m.text);
        out.push_back('\t');
        out += meshEscapeField(m.note);
        out.push_back('\n');
    }
    return out;
}

std::vector<MeshMessage> parseMeshChat(const std::string& text) {
    std::vector<MeshMessage> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> fields;
        size_t start = 0;
        for (;;) {
            const size_t tab = line.find('\t', start);
            if (tab == std::string::npos) {
                fields.push_back(line.substr(start));
                break;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (fields.size() < 8) continue;

        MeshMessage m;
        m.stampUtc = std::strtoll(fields[0].c_str(), nullptr, 10);
        m.outgoing = fields[1] == "out";
        m.from = parseHex32(fields[2]);
        m.to = parseHex32(fields[3]);
        m.id = parseHex32(fields[4]);
        m.channel = static_cast<uint32_t>(std::strtoul(fields[5].c_str(), nullptr, 10));
        m.state = stateFromText(fields[6]);
        m.text = meshUnescapeField(fields[7]);
        if (fields.size() > 8) m.note = meshUnescapeField(fields[8]);
        // A message that was still waiting for an ack when the app closed did
        // not become delivered by being written to disk.
        if (m.outgoing && m.state == MeshMessageState::Queued) {
            m.state = MeshMessageState::Failed;
            if (m.note.empty()) m.note = "unresolved at shutdown";
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::string meshChatFileName(uint32_t peer) {
    if (peer == kMeshBroadcast) return "broadcast.chat";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "node-%08x.chat", peer);
    return buf;
}

bool meshChatPeerFromFileName(const std::string& name, uint32_t& peer) {
    if (name == "broadcast.chat") {
        peer = kMeshBroadcast;
        return true;
    }
    if (name.rfind("node-", 0) != 0) return false;
    const size_t dot = name.rfind(".chat");
    if (dot == std::string::npos || dot != name.size() - 5) return false;
    const std::string hex = name.substr(5, dot - 5);
    if (hex.size() != 8) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    peer = parseHex32(hex);
    return true;
}

} // namespace bf
