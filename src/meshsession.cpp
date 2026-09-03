#include "meshsession.h"
#include "input.h"
#include "protowire.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace bf {
namespace {

// The resync burst the reference clients send: 32 bytes that cannot begin a
// frame, which flushes any half-parsed frame inside the radio and wakes a
// sleeping one. Then a short pause before the first real request.
constexpr int kWakeBytes = 32;
constexpr uint64_t kWakeSettleMs = 150;
// A config download is a few dozen small frames. Eight seconds is generous for
// a node database over 115200 and short enough to retry inside a bench session.
constexpr uint64_t kConfigTimeoutMs = 8000;
constexpr int kConfigMaxAttempts = 3;
// Three checksummed sentences and not one frame: the peer is a GNSS receiver.
// One would do, but a console line that quotes a sentence should not convict a
// radio that is about to answer; the third arrives a second later anyway.
constexpr int kNmeaVerdictSentences = 3;
constexpr size_t kMaxConsoleLine = 512;
// The firmware drops a client API connection that stops talking. The reference
// clients heartbeat every five minutes; so does this one.
constexpr uint64_t kHeartbeatIntervalMs = 300000;
// How long a direct message waits for its routing acknowledgement before it is
// reported as unacknowledged. Several hops with retransmits can take a while.
constexpr uint64_t kAckTimeoutMs = 60000;
constexpr size_t kMaxConversationMessages = 500;
constexpr size_t kMaxNodes = 250;

int64_t wallClockSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
}

} // namespace

std::string MeshRadioInfo::loraSummary() const {
    if (!haveLora) return "LoRa config not reported";
    std::string out = meshRegionName(region);
    if (usePreset) out += std::string("  ") + meshModemPresetName(modemPreset);
    out += txEnabled ? "  TX on" : "  TX off";
    if (region == 0) out += "  (no region: the radio stays mute)";
    return out;
}

MeshSession::MeshSession(Terminal& term) : term_(term) {}

// ---------------------------------------------------------------- lifecycle

bool MeshSession::connect(const std::string& device, int baud, std::string& error) {
    disconnect();
    if (!port_.open(device, baud, error)) {
        state_ = MeshState::Failed;
        return false;
    }
    term_.resetInputFragment();
    linkLost_ = false;
    state_ = MeshState::Waking;
    const uint64_t now = nowMs();
    lastByteMs_ = now;
    wakeSentMs_ = now;
    configProgressMs_ = now;
    lastHeartbeatMs_ = now;
    configAttempts_ = 0;
    configItems_ = 0;
    rxBuf_.clear();
    consoleLine_.clear();
    nmeaSentences_ = 0;
    framesSeen_ = 0;
    radio_ = MeshRadioInfo{};
    nodes_.clear();
    nodeIndex_.clear();
    pending_.clear();
    ++nodeSequence_;
    packetSeed_ = static_cast<uint32_t>(now) ^ 0x9E3779B9u;

    port_.write(std::string(static_cast<size_t>(kWakeBytes),
                            static_cast<char>(kMeshStart2)));
    term_.addLine("-- meshtastic: resyncing the serial framer --", LineKind::Local);
    return true;
}

void MeshSession::disconnect() {
    if (port_.isOpen() && state_ == MeshState::Ready) {
        // Tell the firmware the client is going away so it does not hold the
        // slot open waiting for a heartbeat that will never arrive.
        writeToRadio(encodeDisconnect());
        std::string err;
        port_.flush(err);
    }
    port_.close();
    state_ = MeshState::Disconnected;
    rxBuf_.clear();
    consoleLine_.clear();
    pending_.clear();
    configItems_ = 0;
}

void MeshSession::fail(const std::string& reason) {
    state_ = MeshState::Failed;
    term_.addLine("-- " + reason + " --", LineKind::Error);
    setNote(reason);
}

void MeshSession::setNote(const std::string& text) {
    note_ = text;
    ++noteSequence_;
}

// Console bytes arrive in whatever pieces the UART delivered them, so lines are
// reassembled before the checksum is tried: a fragment that starts with '$' is
// not a sentence, and a sentence split across two reads is still one.
void MeshSession::noteConsoleText(const std::string& text) {
    for (char c : text) {
        if (c != '\r' && c != '\n') {
            if (consoleLine_.size() < kMaxConsoleLine) consoleLine_.push_back(c);
            continue;
        }
        if (!consoleLine_.empty() && consoleLine_[0] == '$' &&
            consoleLine_.find('*') != std::string::npos && nmeaChecksumOk(consoleLine_)) {
            ++nmeaSentences_;
        }
        consoleLine_.clear();
    }
}

void MeshSession::writeToRadio(const std::string& payload) {
    port_.write(frameToRadio(payload));
}

uint32_t MeshSession::nextPacketId() {
    // A packet id only has to be unique among this client's in-flight packets;
    // the mesh matches acknowledgements against it. xorshift keeps it cheap and
    // keeps consecutive ids from being consecutive integers.
    packetSeed_ ^= packetSeed_ << 13;
    packetSeed_ ^= packetSeed_ >> 17;
    packetSeed_ ^= packetSeed_ << 5;
    if (packetSeed_ == 0) packetSeed_ = 0x1234567u;
    return packetSeed_;
}

// -------------------------------------------------------------- node table

size_t MeshSession::nodeSlotIndex(uint32_t num) {
    auto it = nodeIndex_.find(num);
    if (it != nodeIndex_.end()) return it->second;
    if (nodes_.size() >= kMaxNodes) {
        // Drop the least recently heard stranger rather than growing without a
        // bound on a device with 227 MB of RAM and a mesh that can be busy.
        size_t victim = 0;
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < nodes_.size(); ++i) {
            if (nodes_[i].isSelf) continue;
            if (nodes_[i].heardLocalMs <= oldest) {
                oldest = nodes_[i].heardLocalMs;
                victim = i;
            }
        }
        nodes_.erase(nodes_.begin() + static_cast<long>(victim));
        nodeIndex_.clear();
        for (size_t i = 0; i < nodes_.size(); ++i) nodeIndex_[nodes_[i].num] = i;
    }
    MeshNode fresh;
    fresh.num = num;
    fresh.isSelf = (radio_.myNodeNum != 0 && num == radio_.myNodeNum);
    nodes_.push_back(fresh);
    nodeIndex_[num] = nodes_.size() - 1;
    return nodes_.size() - 1;
}

MeshNode& MeshSession::nodeSlot(uint32_t num) { return nodes_[nodeSlotIndex(num)]; }

void MeshSession::sortNodes() {
    std::stable_sort(nodes_.begin(), nodes_.end(),
                     [](const MeshNode& a, const MeshNode& b) {
                         if (a.isSelf != b.isSelf) return a.isSelf;
                         if (a.heardLocalMs != b.heardLocalMs) {
                             return a.heardLocalMs > b.heardLocalMs;
                         }
                         if (a.lastHeard != b.lastHeard) return a.lastHeard > b.lastHeard;
                         return a.num < b.num;
                     });
    nodeIndex_.clear();
    for (size_t i = 0; i < nodes_.size(); ++i) nodeIndex_[nodes_[i].num] = i;
    ++nodeSequence_;
}

const MeshNode* MeshSession::findNode(uint32_t num) const {
    auto it = nodeIndex_.find(num);
    if (it == nodeIndex_.end()) return nullptr;
    return &nodes_[it->second];
}

void MeshSession::touchNode(uint32_t num, uint64_t now) {
    if (num == 0 || num == kMeshBroadcast) return;
    MeshNode& node = nodeSlot(num);
    node.heardLocalMs = now;
}

// ----------------------------------------------------------------- sending

bool MeshSession::sendText(uint32_t peer, const std::string& text, std::string& error) {
    if (!ready()) {
        error = "the radio is not ready";
        return false;
    }
    if (text.empty()) {
        error = "nothing to send";
        return false;
    }
    if (text.size() > kMeshMaxTextBytes) {
        error = "message is longer than " + std::to_string(kMeshMaxTextBytes) + " bytes";
        return false;
    }
    if (!radio_.loraReady()) {
        error = radio_.region == 0
                    ? "the radio has no LoRa region set, so it will not transmit"
                    : "the radio reports transmit disabled";
        return false;
    }
    const MeshNode* target = peer == kMeshBroadcast ? nullptr : findNode(peer);
    if (target && target->user.unmessagable) {
        error = target->title() + " advertises that it does not accept messages";
        return false;
    }

    const uint32_t packetId = nextPacketId();
    const bool direct = peer != kMeshBroadcast;
    writeToRadio(encodeTextPacket(peer, channelIndex_, packetId, text, direct));

    MeshMessage message;
    message.outgoing = true;
    message.peer = peer;
    message.from = radio_.myNodeNum;
    message.to = peer;
    message.id = packetId;
    message.channel = channelIndex_;
    message.stampUtc = wallClockSeconds();
    message.text = text;
    // A broadcast is never acknowledged, so calling it "queued" forever would
    // be a spinner that can only lie. Direct messages really do get an answer.
    message.state = direct ? MeshMessageState::Queued : MeshMessageState::Sent;
    if (!direct) message.note = "broadcast; the mesh does not acknowledge";
    appendMessage(peer, message);

    if (direct) {
        PendingTx pending;
        pending.peer = peer;
        pending.messageId = packetId;
        pending.sentMs = nowMs();
        pending_[packetId] = pending;
    }
    return true;
}

bool MeshSession::sendPosition(uint32_t peer, const GnssFix& fix, std::string& error) {
    if (!ready()) {
        error = "the radio is not ready";
        return false;
    }
    if (!fix.valid) {
        error = "the LoRa Cap GNSS has no current fix";
        return false;
    }
    if (!radio_.loraReady()) {
        error = radio_.region == 0
                    ? "the radio has no LoRa region set, so it will not transmit"
                    : "the radio reports transmit disabled";
        return false;
    }
    const uint32_t packetId = nextPacketId();
    writeToRadio(encodePositionPacket(
        peer, channelIndex_, packetId, fix.latitude, fix.longitude,
        fix.haveAltitude, static_cast<int32_t>(fix.altitudeM), fix.utcSeconds,
        static_cast<uint32_t>(fix.satellitesUsed)));
    term_.addLine("-- sent this station's GNSS position to " +
                      (peer == kMeshBroadcast ? std::string("the mesh")
                                              : meshNodeIdText(peer)) +
                      " --",
                  LineKind::Local);
    setNote("position transmitted");
    return true;
}

// -------------------------------------------------------------------- chat

void MeshSession::markDirty(uint32_t peer) {
    if (std::find(dirty_.begin(), dirty_.end(), peer) == dirty_.end()) {
        dirty_.push_back(peer);
    }
    ++chatSequence_;
}

void MeshSession::appendMessage(uint32_t peer, MeshMessage message) {
    std::vector<MeshMessage>& log = chat_[peer];
    log.push_back(std::move(message));
    if (log.size() > kMaxConversationMessages) {
        log.erase(log.begin(),
                  log.begin() + static_cast<long>(log.size() - kMaxConversationMessages));
    }
    markDirty(peer);
}

const std::vector<MeshMessage>* MeshSession::conversation(uint32_t peer) const {
    auto it = chat_.find(peer);
    return it == chat_.end() ? nullptr : &it->second;
}

std::vector<uint32_t> MeshSession::conversationPeers() const {
    std::vector<uint32_t> peers;
    peers.reserve(chat_.size());
    for (const auto& kv : chat_) peers.push_back(kv.first);
    return peers;
}

void MeshSession::adoptConversation(uint32_t peer, std::vector<MeshMessage> messages) {
    if (messages.empty()) return;
    if (messages.size() > kMaxConversationMessages) {
        messages.erase(messages.begin(),
                       messages.begin() +
                           static_cast<long>(messages.size() - kMaxConversationMessages));
    }
    chat_[peer] = std::move(messages);
    ++chatSequence_;
}

void MeshSession::clearConversations() {
    chat_.clear();
    unread_.clear();
    dirty_.clear();
    ++chatSequence_;
}

void MeshSession::clearConversation(uint32_t peer) {
    chat_.erase(peer);
    unread_.erase(peer);
    // Anything still in flight for that peer has nowhere to land now.
    for (auto it = pending_.begin(); it != pending_.end();) {
        it = (it->second.peer == peer) ? pending_.erase(it) : std::next(it);
    }
    ++chatSequence_;
}

int MeshSession::unread(uint32_t peer) const {
    auto it = unread_.find(peer);
    return it == unread_.end() ? 0 : it->second;
}

int MeshSession::totalUnread() const {
    int total = 0;
    for (const auto& kv : unread_) total += kv.second;
    return total;
}

void MeshSession::markRead(uint32_t peer) {
    auto it = unread_.find(peer);
    if (it == unread_.end() || it->second == 0) return;
    it->second = 0;
    ++chatSequence_;
}

std::vector<uint32_t> MeshSession::takeDirtyPeers() {
    std::vector<uint32_t> out;
    out.swap(dirty_);
    return out;
}

void MeshSession::resolvePending(uint32_t packetId, bool delivered,
                                 const std::string& reason, uint64_t now) {
    (void)now;
    auto it = pending_.find(packetId);
    if (it == pending_.end()) return;
    const uint32_t peer = it->second.peer;
    pending_.erase(it);

    auto chatIt = chat_.find(peer);
    if (chatIt == chat_.end()) return;
    for (auto message = chatIt->second.rbegin(); message != chatIt->second.rend(); ++message) {
        if (message->id != packetId || !message->outgoing) continue;
        message->state = delivered ? MeshMessageState::Delivered : MeshMessageState::Failed;
        message->note = delivered ? std::string() : reason;
        markDirty(peer);
        return;
    }
}

// ---------------------------------------------------------------- receiving

void MeshSession::handlePacket(const MeshFromRadio& frame, uint64_t now) {
    if (frame.encrypted) {
        // Somebody else's channel. Counting it as traffic is honest; pretending
        // to have read it is not.
        touchNode(frame.from, now);
        return;
    }
    touchNode(frame.from, now);
    // Indices, not references: nodeSlot() can grow or evict from the vector,
    // and a reference held across that call is a use-after-free waiting for a
    // busy mesh to find it.
    {
        const size_t index = nodeSlotIndex(frame.from);
        MeshNode& sender = nodes_[index];
        if (frame.haveSnr) {
            sender.snr = frame.rxSnr;
            sender.haveSnr = true;
        }
        if (frame.haveRssi) {
            sender.rssi = frame.rxRssi;
            sender.haveRssi = true;
        }
        if (frame.rxTime != 0) sender.lastHeard = frame.rxTime;
        if (frame.hopStart != 0 && frame.hopStart >= frame.hopLimitPacket) {
            sender.hopsAway = frame.hopStart - frame.hopLimitPacket;
            sender.haveHops = true;
        }
        sender.viaMqtt = frame.viaMqtt;
    }

    switch (frame.port) {
    case MeshPort::TextMessage: {
        const uint32_t peer = frame.to == kMeshBroadcast ? kMeshBroadcast : frame.from;
        MeshMessage message;
        message.outgoing = false;
        message.peer = peer;
        message.from = frame.from;
        message.to = frame.to;
        message.id = frame.packetId;
        message.channel = frame.channel;
        message.stampUtc = wallClockSeconds();
        message.text = frame.payload;
        message.state = MeshMessageState::Received;
        appendMessage(peer, message);
        ++unread_[peer];
        const MeshNode* from = findNode(frame.from);
        setNote((from ? from->label() : meshNodeIdText(frame.from)) + ": message");
        break;
    }
    case MeshPort::NodeInfo: {
        MeshUser user;
        if (decodeMeshUser(frame.payload, user)) nodeSlot(frame.from).user = user;
        break;
    }
    case MeshPort::Position: {
        MeshPosition position;
        if (decodeMeshPosition(frame.payload, position) && position.valid) {
            MeshNode& node = nodeSlot(frame.from);
            node.position = position;
            // Heard by this station, now. The sender's own stamp may be zero
            // (a tracker with no clock) and this is the age that matters when
            // somebody is walking towards it.
            node.positionLocalMs = now;
        }
        break;
    }
    case MeshPort::Telemetry: {
        bool haveBattery = false, haveVoltage = false;
        uint32_t battery = 0;
        float voltage = 0.0f;
        // Telemetry wraps its variant; DeviceMetrics is field 2.
        pb::Reader reader(frame.payload);
        while (reader.next()) {
            if (reader.field() != 2) continue;
            if (decodeMeshDeviceMetrics(reader.bytes(), haveBattery, battery,
                                        haveVoltage, voltage)) {
                MeshNode& node = nodeSlot(frame.from);
                if (haveBattery) {
                    node.batteryLevel = battery;
                    node.haveBattery = true;
                }
                if (haveVoltage) {
                    node.voltage = voltage;
                    node.haveVoltage = true;
                }
            }
        }
        break;
    }
    case MeshPort::Routing: {
        bool haveError = false;
        uint32_t reason = 0;
        if (!decodeMeshRouting(frame.payload, haveError, reason) || !haveError) break;
        if (frame.requestId == 0) break;
        const bool delivered = reason == 0;
        resolvePending(frame.requestId, delivered, meshRoutingErrorText(reason), now);
        if (!delivered) {
            term_.addLine(std::string("-- mesh returned \"") + meshRoutingErrorText(reason) +
                              "\" for a sent message --",
                          LineKind::Warn);
            setNote(std::string("not delivered: ") + meshRoutingErrorText(reason));
        }
        break;
    }
    default:
        break;
    }
    sortNodes();
}

void MeshSession::handleFrame(const std::string& body, uint64_t now) {
    MeshFromRadio frame;
    if (!decodeMeshFromRadio(body, frame)) {
        term_.addLine("-- meshtastic: dropped a malformed frame --", LineKind::Warn);
        return;
    }

    // Every piece of the download that lands is evidence the radio is answering.
    // Recorded around the whole switch so no future config-bearing kind can be
    // added without also counting as progress.
    const int itemsBefore = configItems_;

    switch (frame.kind) {
    case MeshFromRadio::Kind::MyInfo:
        radio_.myNodeNum = frame.myNodeNum;
        for (MeshNode& node : nodes_) node.isSelf = (node.num == frame.myNodeNum);
        ++configItems_;
        break;

    case MeshFromRadio::Kind::Metadata:
        radio_.firmwareVersion = frame.firmwareVersion;
        radio_.hwModel = frame.hwModel;
        radio_.role = frame.role;
        ++configItems_;
        break;

    case MeshFromRadio::Kind::NodeInfo: {
        MeshNode& node = nodeSlot(frame.node.num);
        const uint64_t heard = node.heardLocalMs;
        const uint32_t num = node.num;
        // A database record that repeats the position we already heard live
        // keeps the moment we heard it; a different coordinate is news from
        // the radio's memory and its arrival time here is unknown.
        const bool samePosition = node.position.valid && frame.node.position.valid &&
                                  node.position.latitude == frame.node.position.latitude &&
                                  node.position.longitude == frame.node.position.longitude;
        const uint64_t positionHeard = samePosition ? node.positionLocalMs : 0;
        node = frame.node;
        node.num = num;
        node.heardLocalMs = heard;
        node.positionLocalMs = positionHeard;
        node.isSelf = (radio_.myNodeNum != 0 && num == radio_.myNodeNum);
        ++configItems_;
        break;
    }

    case MeshFromRadio::Kind::Config:
        if (frame.haveLoraConfig) {
            radio_.haveLora = true;
            radio_.region = frame.region;
            radio_.modemPreset = frame.modemPreset;
            radio_.usePreset = frame.usePreset;
            radio_.hopLimit = frame.hopLimit;
            radio_.txEnabled = frame.txEnabled;
        }
        if (frame.havePositionConfig) {
            radio_.havePositionConfig = true;
            radio_.gpsMode = frame.gpsMode;
            radio_.fixedPosition = frame.fixedPosition;
        }
        ++configItems_;
        break;

    case MeshFromRadio::Kind::Channel:
        ++radio_.channelCount;
        // Role 1 is PRIMARY. Its name is what the operator sees on every phone.
        if (frame.channelRole == 1) {
            radio_.primaryChannel = frame.channelName.empty() ? "(default)" : frame.channelName;
            channelIndex_ = frame.channelIndex;
        }
        ++configItems_;
        break;

    case MeshFromRadio::Kind::ConfigComplete:
        if (frame.configCompleteId != configId_) break;
        state_ = MeshState::Ready;
        sortNodes();
        term_.addLine("-- meshtastic ready: " + std::to_string(nodes_.size()) +
                          " node(s), " + radio_.loraSummary() + " --",
                      radio_.loraReady() ? LineKind::Good : LineKind::Warn);
        if (!radio_.loraReady()) {
            term_.addLine("-- this radio will not transmit until a region is set --",
                          LineKind::Warn);
        }
        setNote("mesh ready");
        lastHeartbeatMs_ = now;
        break;

    case MeshFromRadio::Kind::Rebooted:
        term_.addLine("-- the radio rebooted; requesting its node database again --",
                      LineKind::Warn);
        state_ = MeshState::Configuring;
        configAttempts_ = 0;
        configItems_ = 0;
        configId_ = nextPacketId();
        if (configId_ == kMeshNodelessConfigId) ++configId_;
        configSentMs_ = now;
        writeToRadio(encodeWantConfig(configId_));
        break;

    case MeshFromRadio::Kind::LogRecord:
        if (!frame.logText.empty()) term_.addLine(frame.logText, LineKind::Fc);
        break;

    case MeshFromRadio::Kind::Packet:
        handlePacket(frame, now);
        break;

    case MeshFromRadio::Kind::Unknown:
        break;
    }

    if (configItems_ != itemsBefore) configProgressMs_ = now;
}

// -------------------------------------------------------------------- poll

void MeshSession::poll(uint64_t now) {
    if (!port_.isOpen()) return;

    std::string err;
    if (!port_.flush(err)) {
        linkLost_ = true;
        term_.addLine("-- link lost (" + err + ") --", LineKind::Error);
        port_.close();
        state_ = MeshState::Disconnected;
        return;
    }

    std::string incoming;
    const int n = port_.read(incoming);
    if (n < 0) {
        linkLost_ = true;
        term_.addLine("-- link lost (device disconnected) --", LineKind::Error);
        port_.close();
        state_ = MeshState::Disconnected;
        return;
    }
    if (n > 0) {
        lastByteMs_ = now;
        rxBuf_ += incoming;
        std::vector<std::string> frames;
        std::string log;
        extractMeshFrames(rxBuf_, frames, log);
        // The radio's own console shares this wire. Keeping it visible is the
        // difference between "nothing happened" and "the radio said why".
        if (!log.empty()) {
            term_.feed(log);
            noteConsoleText(log);
        }
        framesSeen_ += static_cast<int>(frames.size());
        for (const std::string& frame : frames) handleFrame(frame, now);

        // What the bench's Grove UART actually had on it was the cap's GNSS
        // receiver, and its NMEA scrolled through here as "console output" for
        // three config timeouts before anyone was told. A wire that has only
        // ever produced checksummed sentences is not a radio; say so now.
        if (framesSeen_ == 0 && nmeaSentences_ >= kNmeaVerdictSentences &&
            (state_ == MeshState::Waking || state_ == MeshState::Configuring)) {
            fail("this port is speaking NMEA 0183: a GNSS receiver, not a Meshtastic radio");
            return;
        }
    }

    switch (state_) {
    case MeshState::Waking:
        if (now - wakeSentMs_ < kWakeSettleMs) break;
        state_ = MeshState::Configuring;
        configAttempts_ = 1;
        configId_ = nextPacketId();
        if (configId_ == kMeshNodelessConfigId) ++configId_;
        configSentMs_ = now;
        writeToRadio(encodeWantConfig(configId_));
        term_.addLine("-- meshtastic: requesting the node database --", LineKind::Local);
        break;

    case MeshState::Configuring: {
        // Measured from the last thing that arrived. A radio still streaming its
        // node database has not gone quiet, however long the whole download
        // takes, and asking again would only send it back to the beginning.
        const uint64_t lastProgress = std::max(configSentMs_, configProgressMs_);
        if (now - lastProgress < kConfigTimeoutMs) break;
        if (configAttempts_ >= kConfigMaxAttempts) {
            fail(configItems_ > 0
                     ? "the radio stopped partway through the config download after " +
                           std::to_string(configItems_) + " item(s)"
                     : std::string("no Meshtastic device answered. Is this the right "
                                   "port, and is the firmware running?"));
            break;
        }
        ++configAttempts_;
        configItems_ = 0;
        configId_ = nextPacketId();
        if (configId_ == kMeshNodelessConfigId) ++configId_;
        configSentMs_ = now;
        writeToRadio(encodeWantConfig(configId_));
        term_.addLine("-- meshtastic: no reply, asking again (attempt " +
                          std::to_string(configAttempts_) + ") --",
                      LineKind::Warn);
        break;
    }

    case MeshState::Ready: {
        if (now - lastHeartbeatMs_ >= kHeartbeatIntervalMs) {
            lastHeartbeatMs_ = now;
            writeToRadio(encodeHeartbeat());
        }
        // Give up on an acknowledgement rather than leaving a message spinning
        // forever. Nothing is allocated on the common path where none is due.
        if (!pending_.empty()) {
            std::vector<uint32_t> expired;
            for (const auto& kv : pending_) {
                if (now - kv.second.sentMs >= kAckTimeoutMs) expired.push_back(kv.first);
            }
            for (uint32_t packetId : expired) {
                resolvePending(packetId, false, "no acknowledgement in 60s", now);
            }
        }
        break;
    }

    case MeshState::Disconnected:
    case MeshState::Failed:
        break;
    }
}

} // namespace bf
