#include "protowire.h"

#include <cstring>

namespace bf {
namespace pb {

bool Reader::readVarint(uint64_t& out) {
    uint64_t result = 0;
    int shift = 0;
    // Ten groups is the whole range of a 64-bit varint. An eleventh byte means
    // the stream is not what it claims to be.
    for (int i = 0; i < 10; ++i) {
        if (pos_ >= size_) return false;
        const uint8_t byte = static_cast<uint8_t>(data_[pos_++]);
        // Nine groups already filled 63 bits. Only bit 0 fits in the tenth;
        // silently discarding the rest can turn a corrupt length into zero.
        if (i == 9 && byte > 1) return false;
        result |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            out = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool Reader::next() {
    payload_ = nullptr;
    payloadSize_ = 0;
    value_ = 0;
    if (!ok_ || pos_ >= size_) return false;

    uint64_t tag = 0;
    if (!readVarint(tag) || (tag >> 3) > 0x1FFFFFFFu) { ok_ = false; return false; }
    field_ = static_cast<uint32_t>(tag >> 3);
    type_ = static_cast<WireType>(tag & 0x07);
    if (field_ == 0) { ok_ = false; return false; }

    switch (type_) {
    case WireType::Varint:
        if (!readVarint(value_)) { ok_ = false; return false; }
        return true;
    case WireType::Fixed64: {
        if (size_ - pos_ < 8) { ok_ = false; return false; }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(static_cast<uint8_t>(data_[pos_ + static_cast<size_t>(i)]))
                 << (8 * i);
        }
        value_ = v;
        pos_ += 8;
        return true;
    }
    case WireType::Fixed32: {
        if (size_ - pos_ < 4) { ok_ = false; return false; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(static_cast<uint8_t>(data_[pos_ + static_cast<size_t>(i)]))
                 << (8 * i);
        }
        value_ = v;
        pos_ += 4;
        return true;
    }
    case WireType::Bytes: {
        uint64_t len = 0;
        if (!readVarint(len)) { ok_ = false; return false; }
        if (len > size_ - pos_) { ok_ = false; return false; }
        payload_ = data_ + pos_;
        payloadSize_ = static_cast<size_t>(len);
        pos_ += payloadSize_;
        return true;
    }
    case WireType::StartGroup:
    case WireType::EndGroup:
    default:
        // Groups were removed from the language two major versions ago and
        // Meshtastic does not use them. Stopping is more honest than guessing
        // at a length that is not on the wire.
        ok_ = false;
        return false;
    }
}

bool Reader::expect(WireType type) {
    if (type_ != type) ok_ = false;
    return ok_;
}

uint64_t Reader::varint() {
    return expect(WireType::Varint) ? value_ : 0;
}

uint32_t Reader::u32() {
    const uint64_t value = varint();
    if (value > UINT32_MAX) { ok_ = false; return 0; }
    return static_cast<uint32_t>(value);
}

int32_t Reader::i32() { return static_cast<int32_t>(static_cast<uint32_t>(varint())); }

int32_t Reader::s32() {
    const uint32_t v = u32();
    return static_cast<int32_t>((v >> 1) ^ (~(v & 1) + 1));
}

uint32_t Reader::fixed32() {
    return expect(WireType::Fixed32) ? static_cast<uint32_t>(value_) : 0;
}

int32_t Reader::sfixed32() { return static_cast<int32_t>(fixed32()); }

float Reader::f32() {
    const uint32_t bits = fixed32();
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

std::string Reader::bytes() {
    if (!expect(WireType::Bytes) || payload_ == nullptr) return {};
    return std::string(payload_, payloadSize_);
}

Reader Reader::sub() {
    if (!expect(WireType::Bytes) || payload_ == nullptr) return Reader();
    return Reader(payload_, payloadSize_);
}

void Writer::rawVarint(uint64_t value) {
    for (;;) {
        const uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) {
            out_.push_back(static_cast<char>(byte | 0x80));
        } else {
            out_.push_back(static_cast<char>(byte));
            return;
        }
    }
}

void Writer::tag(uint32_t field, WireType type) {
    rawVarint((static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(type));
}

void Writer::varint(uint32_t field, uint64_t value) {
    tag(field, WireType::Varint);
    rawVarint(value);
}

void Writer::boolean(uint32_t field, bool value) { varint(field, value ? 1 : 0); }

void Writer::i32(uint32_t field, int32_t value) {
    // A negative int32 is sign extended to 64 bits before encoding; truncating
    // it to four groups produces a value the other side reads as enormous.
    varint(field, static_cast<uint64_t>(static_cast<int64_t>(value)));
}

void Writer::fixed32(uint32_t field, uint32_t value) {
    tag(field, WireType::Fixed32);
    for (int i = 0; i < 4; ++i) {
        out_.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void Writer::sfixed32(uint32_t field, int32_t value) {
    fixed32(field, static_cast<uint32_t>(value));
}

void Writer::f32(uint32_t field, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    fixed32(field, bits);
}

void Writer::bytes(uint32_t field, const std::string& value) {
    tag(field, WireType::Bytes);
    rawVarint(value.size());
    out_ += value;
}

} // namespace pb
} // namespace bf
