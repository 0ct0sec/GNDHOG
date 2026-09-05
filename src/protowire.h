#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace bf {
namespace pb {

// Protobuf wire format, by hand. The alternative was libprotobuf or a nanopb
// code-generation step, and this application links exactly libc, libdl and
// pthread. The wire format is small enough that carrying a code generator to
// read nine message types would be the more expensive decision.
enum class WireType : uint8_t {
    Varint = 0,
    Fixed64 = 1,
    Bytes = 2,
    StartGroup = 3,
    EndGroup = 4,
    Fixed32 = 5,
};

// A forward-only reader over one encoded message. Every read is bounds checked
// and a malformed byte stops iteration rather than walking off the buffer, so a
// truncated or hostile frame costs one dropped message and nothing else.
// Unknown fields are skipped, which is what keeps this tolerant of firmware
// newer than the build that is reading it.
class Reader {
public:
    Reader() = default;
    Reader(const char* data, size_t size) : data_(data), size_(size) {}
    explicit Reader(const std::string& s) : Reader(s.data(), s.size()) {}
    // A Reader borrows its bytes and never owns them, so constructing one from
    // a temporary would leave it pointing at a string that died at the end of
    // the full expression. Deleting the overload turns that into a compile
    // error instead of a use-after-scope the sanitizer has to find later.
    explicit Reader(std::string&&) = delete;

    // Advances to the next field. False at the end of the message, and also
    // false the moment anything does not decode.
    bool next();

    uint32_t field() const { return field_; }
    WireType type() const { return type_; }
    bool ok() const { return ok_; }

    // Accessors validate the expected type and mark the reader failed on a
    // mismatch. A fabricated zero could otherwise look like an ACK or a fix
    // at the equator. Unknown fields remain safe to skip without an accessor.
    uint64_t varint();
    bool boolean() { return varint() != 0; }
    uint32_t u32();
    int32_t i32();          // int32 / enum: a varint in two's complement
    int32_t s32();          // sint32: zigzag
    uint32_t fixed32();
    int32_t sfixed32();
    float f32();
    std::string bytes();
    Reader sub();           // nested message inside a length-delimited field

private:
    bool readVarint(uint64_t& out);
    bool expect(WireType type);

    const char* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    bool ok_ = true;

    uint32_t field_ = 0;
    WireType type_ = WireType::Varint;
    uint64_t value_ = 0;              // varint and fixed payloads
    const char* payload_ = nullptr;   // length-delimited payload
    size_t payloadSize_ = 0;
};

// The matching encoder. Fields must be written in whatever order the caller
// likes; protobuf does not care, and neither does the firmware.
class Writer {
public:
    void varint(uint32_t field, uint64_t value);
    void boolean(uint32_t field, bool value);
    void i32(uint32_t field, int32_t value);   // sign extended, as protobuf requires
    void fixed32(uint32_t field, uint32_t value);
    void sfixed32(uint32_t field, int32_t value);
    void f32(uint32_t field, float value);
    void bytes(uint32_t field, const std::string& value);
    void message(uint32_t field, const Writer& value) { bytes(field, value.data()); }
    // A present-but-empty submessage, which is how an empty Heartbeat travels.
    void emptyMessage(uint32_t field) { bytes(field, std::string()); }

    const std::string& data() const { return out_; }
    void clear() { out_.clear(); }

private:
    void tag(uint32_t field, WireType type);
    void rawVarint(uint64_t value);

    std::string out_;
};

} // namespace pb
} // namespace bf
