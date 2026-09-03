#pragma once
#include "gnss.h"
#include "meshtastic.h"
#include "serialport.h"
#include "term.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf {

enum class MeshState {
    Disconnected,
    Waking,        // resync bytes written, waiting for the line to settle
    Configuring,   // want_config sent, the node database is streaming in
    Ready,
    Failed,
};

// Everything the radio said about itself during the config download. Every
// field is either something the device reported or an explicit "not reported":
// none of it is assumed from the fact that a serial port opened.
struct MeshRadioInfo {
    uint32_t myNodeNum = 0;
    std::string firmwareVersion;
    uint32_t hwModel = 0;
    uint32_t role = 0;

    bool haveLora = false;
    uint32_t region = 0;
    uint32_t modemPreset = 0;
    bool usePreset = true;
    uint32_t hopLimit = 0;
    bool txEnabled = false;

    bool havePositionConfig = false;
    uint32_t gpsMode = 0;              // PositionConfig.GpsMode
    bool fixedPosition = false;

    std::string primaryChannel;
    int channelCount = 0;

    // The radio can only put something in the air once it has a region and
    // transmit is enabled. An unset region is how Meshtastic ships, and it is
    // deliberately mute until somebody chooses one.
    bool loraReady() const { return haveLora && region != 0 && txEnabled; }
    std::string loraSummary() const;
};

// Owns the serial link to a Meshtastic radio and the client-API conversation on
// top of it: the config download, the node database, the chat log, and the
// heartbeat that keeps the firmware from dropping an idle client.
//
// Framed protobuf and the radio's own debug console share one wire. Bytes that
// are not part of a frame are handed to the terminal verbatim, so the log stays
// readable instead of being discarded as noise.
class MeshSession {
public:
    explicit MeshSession(Terminal& term);

    bool connect(const std::string& device, int baud, std::string& error);
    void disconnect();
    bool connected() const { return port_.isOpen(); }
    MeshState state() const { return state_; }
    bool ready() const { return state_ == MeshState::Ready; }
    const std::string& device() const { return port_.device(); }
    int fd() const { return port_.fd(); }
    bool linkLost() const { return linkLost_; }

    // Drives the state machine. Call every frame; never blocks.
    void poll(uint64_t now);

    const MeshRadioInfo& radio() const { return radio_; }
    // Sorted for display: this radio first, then by how recently each node was
    // heard. Never claims a node is "online" -- only when it was last heard.
    const std::vector<MeshNode>& nodes() const { return nodes_; }
    const MeshNode* findNode(uint32_t num) const;
    uint64_t nodeSequence() const { return nodeSequence_; }
    int configItems() const { return configItems_; }

    // ---- chat
    // `peer` is kMeshBroadcast or the other node's number. Returns false and
    // fills `error` when the radio cannot legally or physically send.
    bool sendText(uint32_t peer, const std::string& text, std::string& error);
    bool sendPosition(uint32_t peer, const GnssFix& fix, std::string& error);
    const std::vector<MeshMessage>* conversation(uint32_t peer) const;
    std::vector<uint32_t> conversationPeers() const;
    // Restores a saved conversation without marking it dirty again.
    void adoptConversation(uint32_t peer, std::vector<MeshMessage> messages);
    uint64_t chatSequence() const { return chatSequence_; }
    int unread(uint32_t peer) const;
    int totalUnread() const;
    void markRead(uint32_t peer);
    // Conversations changed since the last call, for the writer in App::tick.
    std::vector<uint32_t> takeDirtyPeers();
    void clearConversations();
    void clearConversation(uint32_t peer);

    uint32_t channelIndex() const { return channelIndex_; }
    void setChannelIndex(uint32_t index) { channelIndex_ = index; }

    // Checksummed NMEA 0183 sentences seen on the wire, and frames seen. A
    // Meshtastic radio never opens with sentences; a GNSS receiver on the same
    // Grove UART sends nothing else. This is how the bench's "radio" was
    // recognised for what it was.
    int nmeaSentences() const { return nmeaSentences_; }
    int framesSeen() const { return framesSeen_; }

    // The most recent protocol event worth putting in the status bar.
    const std::string& note() const { return note_; }
    uint64_t noteSequence() const { return noteSequence_; }

private:
    struct PendingTx {
        uint32_t peer = 0;
        uint32_t messageId = 0;
        uint64_t sentMs = 0;
    };

    void writeToRadio(const std::string& payload);
    void handleFrame(const std::string& body, uint64_t now);
    void handlePacket(const MeshFromRadio& frame, uint64_t now);
    void touchNode(uint32_t num, uint64_t now);
    size_t nodeSlotIndex(uint32_t num);
    MeshNode& nodeSlot(uint32_t num);
    void sortNodes();
    void setNote(const std::string& text);
    void fail(const std::string& reason);
    void appendMessage(uint32_t peer, MeshMessage message);
    void markDirty(uint32_t peer);
    void resolvePending(uint32_t packetId, bool delivered, const std::string& reason,
                        uint64_t now);
    uint32_t nextPacketId();
    void noteConsoleText(const std::string& text);

    Terminal& term_;
    SerialPort port_;
    MeshState state_ = MeshState::Disconnected;

    std::string rxBuf_;
    std::string consoleLine_;        // the console's current line, for the NMEA check
    int nmeaSentences_ = 0;
    int framesSeen_ = 0;
    uint64_t lastByteMs_ = 0;
    uint64_t wakeSentMs_ = 0;
    uint64_t configSentMs_ = 0;
    // When the last piece of the config download arrived. The retry deadline is
    // measured from here, not from configSentMs_: a large node database can take
    // longer to stream than the deadline, and re-asking restarts it.
    uint64_t configProgressMs_ = 0;
    uint64_t lastHeartbeatMs_ = 0;
    uint32_t configId_ = 0;
    int configAttempts_ = 0;
    int configItems_ = 0;
    bool linkLost_ = false;

    MeshRadioInfo radio_;
    std::vector<MeshNode> nodes_;
    std::map<uint32_t, size_t> nodeIndex_;
    uint64_t nodeSequence_ = 0;

    std::map<uint32_t, std::vector<MeshMessage>> chat_;
    std::map<uint32_t, int> unread_;
    std::map<uint32_t, PendingTx> pending_;
    std::vector<uint32_t> dirty_;
    uint64_t chatSequence_ = 0;
    uint32_t channelIndex_ = 0;
    uint32_t packetSeed_ = 0;

    std::string note_;
    uint64_t noteSequence_ = 0;
};

} // namespace bf
