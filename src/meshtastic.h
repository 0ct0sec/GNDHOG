#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf {

// Meshtastic's client API, as spoken over a serial port by the firmware on the
// M5Stack Unit C6L and every other supported radio. Frames are
// `0x94 0xC3 <len16 big endian> <protobuf>`, and anything that is not a frame
// is the device's own debug log sharing the same wire.
constexpr uint8_t kMeshStart1 = 0x94;
constexpr uint8_t kMeshStart2 = 0xC3;
constexpr size_t kMeshMaxFrame = 512;
constexpr uint32_t kMeshBroadcast = 0xFFFFFFFFu;
// The firmware reads this particular want_config_id as "send me no node
// database", which is the opposite of what a node picker needs.
constexpr uint32_t kMeshNodelessConfigId = 69420u;
// Text payloads are bounded by the LoRa frame, not by politeness.
constexpr size_t kMeshMaxTextBytes = 200;

enum class MeshPort : uint32_t {
    Unknown = 0,
    TextMessage = 1,
    Position = 3,
    NodeInfo = 4,
    Routing = 5,
    Admin = 6,
    Waypoint = 8,
    Detection = 10,
    Telemetry = 67,
    Traceroute = 70,
    NeighborInfo = 71,
};

struct MeshPosition {
    bool valid = false;              // a latitude and a longitude were present
    double latitude = 0.0;
    double longitude = 0.0;
    bool haveAltitude = false;
    int32_t altitudeM = 0;
    uint32_t time = 0;               // device clock, seconds since the epoch
    uint32_t satsInView = 0;
    uint32_t precisionBits = 0;      // nonzero means the sender blurred it

    std::string coordText() const;
};

struct MeshUser {
    std::string id;                  // "!a1b2c3d4"
    std::string longName;
    std::string shortName;
    uint32_t hwModel = 0;
    uint32_t role = 0;
    bool unmessagable = false;
};

// One entry of the radio's node database, plus the arrival evidence collected
// here. Nothing is inferred: a field that never arrived stays absent instead of
// becoming a plausible default.
struct MeshNode {
    uint32_t num = 0;
    MeshUser user;
    MeshPosition position;
    bool haveSnr = false;
    float snr = 0.0f;
    bool haveRssi = false;
    int32_t rssi = 0;
    uint32_t lastHeard = 0;          // device clock, seconds
    uint64_t heardLocalMs = 0;       // our own monotonic clock, 0 if never
    bool haveHops = false;
    uint32_t hopsAway = 0;
    bool viaMqtt = false;
    bool favorite = false;
    bool haveBattery = false;
    uint32_t batteryLevel = 0;
    bool haveVoltage = false;
    float voltage = 0.0f;
    bool isSelf = false;

    std::string idText() const;      // "!a1b2c3d4"
    std::string label() const;       // short name, else long name, else the id
    std::string title() const;       // long name, else the id
};

enum class MeshMessageState : uint8_t {
    Received,
    Queued,      // handed to the radio, an acknowledgement is still expected
    Sent,        // on the air, and no acknowledgement was ever going to come
    Delivered,   // the mesh returned a routing ACK for this packet id
    Failed,      // the mesh returned an error, or the ack never came
};

struct MeshMessage {
    bool outgoing = false;
    uint32_t peer = 0;               // conversation key: kMeshBroadcast or the other node
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t id = 0;
    uint32_t channel = 0;
    int64_t stampUtc = 0;            // wall clock when this app recorded it
    std::string text;
    MeshMessageState state = MeshMessageState::Received;
    std::string note;                // the routing error, when there was one
};

// ------------------------------------------------------------------ framing

// Wraps an encoded ToRadio in the serial framing.
std::string frameToRadio(const std::string& payload);

// Consumes `buf`, appending every complete frame body to `frames` and every
// byte that was never part of a frame to `log`. Incomplete trailing bytes stay
// in `buf` for the next read.
void extractMeshFrames(std::string& buf, std::vector<std::string>& frames,
                       std::string& log);

// ----------------------------------------------------------------- encoding

std::string encodeWantConfig(uint32_t configId);
std::string encodeHeartbeat();
std::string encodeDisconnect();
std::string encodeTextPacket(uint32_t to, uint32_t channel, uint32_t packetId,
                             const std::string& text, bool wantAck);
std::string encodePositionPacket(uint32_t to, uint32_t channel, uint32_t packetId,
                                 double latitude, double longitude,
                                 bool haveAltitude, int32_t altitudeM,
                                 uint32_t timeSeconds, uint32_t satsInView);

// ----------------------------------------------------------------- decoding

struct MeshFromRadio {
    enum class Kind {
        Unknown,
        Packet,
        MyInfo,
        NodeInfo,
        Config,
        ConfigComplete,
        Metadata,
        Channel,
        Rebooted,
        LogRecord,
    };

    Kind kind = Kind::Unknown;

    // MyInfo
    uint32_t myNodeNum = 0;

    // NodeInfo
    MeshNode node;

    // ConfigComplete
    uint32_t configCompleteId = 0;

    // Metadata
    std::string firmwareVersion;
    uint32_t hwModel = 0;
    uint32_t role = 0;

    // Config: only the two variants this application reads
    bool haveLoraConfig = false;
    uint32_t region = 0;
    uint32_t modemPreset = 0;
    bool usePreset = true;
    uint32_t hopLimit = 0;
    bool txEnabled = false;
    bool havePositionConfig = false;
    uint32_t gpsMode = 0;
    bool fixedPosition = false;

    // Channel
    uint32_t channelIndex = 0;
    std::string channelName;
    uint32_t channelRole = 0;

    // Packet
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t packetId = 0;
    uint32_t channel = 0;
    uint32_t hopLimitPacket = 0;
    uint32_t hopStart = 0;
    bool haveSnr = false;
    float rxSnr = 0.0f;
    bool haveRssi = false;
    int32_t rxRssi = 0;
    uint32_t rxTime = 0;
    bool viaMqtt = false;
    bool encrypted = false;          // the radio could not decrypt it for us
    MeshPort port = MeshPort::Unknown;
    std::string payload;
    uint32_t requestId = 0;

    // LogRecord
    std::string logText;
};

bool decodeMeshFromRadio(const std::string& body, MeshFromRadio& out);
bool decodeMeshPosition(const std::string& payload, MeshPosition& out);
bool decodeMeshUser(const std::string& payload, MeshUser& out);
// Routing reports an ACK as error_reason == NONE, so presence and value both
// matter: a routing packet carrying no error field is not a delivery report.
bool decodeMeshRouting(const std::string& payload, bool& haveError, uint32_t& errorReason);
bool decodeMeshDeviceMetrics(const std::string& payload, bool& haveBattery,
                             uint32_t& batteryLevel, bool& haveVoltage, float& voltage);

// ------------------------------------------------------------------ helpers

std::string meshNodeIdText(uint32_t num);
const char* meshRegionName(uint32_t region);
const char* meshModemPresetName(uint32_t preset);
const char* meshRoleName(uint32_t role);
const char* meshRoutingErrorText(uint32_t reason);
const char* meshGpsModeName(uint32_t mode);
const char* meshPortName(MeshPort port);
// "now", "4m", "2h", "3d": deliberately short, the column is narrow.
std::string meshAgeText(uint64_t seconds);

double meshDistanceM(double lat1, double lon1, double lat2, double lon2);
double meshBearingDeg(double lat1, double lon1, double lat2, double lon2);
const char* meshCompassPoint(double bearingDeg);
std::string meshRangeText(double metres);

// -------------------------------------------------------- chat persistence

// One tab-separated record per message, so a conversation can be read with
// `cat` and copied off the device without a parser. Text is escaped, because a
// message containing a newline must not become two records.
std::string formatMeshChat(const std::vector<MeshMessage>& messages);
std::vector<MeshMessage> parseMeshChat(const std::string& text);
std::string meshChatFileName(uint32_t peer);
bool meshChatPeerFromFileName(const std::string& name, uint32_t& peer);

} // namespace bf
