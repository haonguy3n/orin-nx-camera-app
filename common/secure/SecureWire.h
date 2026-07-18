#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "camera/folly/Expected.h"

namespace camera::secure {

enum class Channel : uint8_t {
    // Video is an H.265 elementary stream pushed by the device, one logical
    // stream per camera, tagged with the camera index.  It is deliberately
    // not an RTSP tunnel: running RTSP/RTP end-to-end over USB spends a
    // round trip per exchange on a link where round trips are the scarce
    // resource, and a single tunnelled TCP connection cannot carry two
    // cameras at once.
    Video = 1,
    Control = 2,
    Update = 3,
};

// One decoded record.  `stream` distinguishes concurrent streams sharing a
// channel -- the camera index for Video, 0 elsewhere.
struct WireMessage {
    Channel channel;
    uint8_t stream;
    std::vector<uint8_t> payload;
};

// USB bulk transfers are not a record protocol.  Each encrypted record is
// therefore prefixed with a four-byte network-order length.  The encrypted
// plaintext starts with the channel byte, allowing RTSP and JSON control to
// share one authenticated USB session.
//
// 64 KiB rather than 16 KiB: per-record cost is dominated by the USB
// round trip, not by the bytes, so larger records cut the number of
// transfers a video stream needs by four.
constexpr size_t kMaxPayload = 64 * 1024;
constexpr size_t kMaxWireRecord = kMaxPayload + 1 + 16;  // channel byte + AEAD tag

// Reassembles records from a bulk byte stream.
//
// A bulk transfer is NOT a record boundary: the peer's writes are split
// across transfers when they are large and coalesced into one transfer when
// they are small.  Treating each transfer as exactly one record works for
// short bursts like RTSP setup and then fails as soon as a channel pushes
// sustained data (a firmware upload), because the length check rejects the
// partial record and the caller tears down an otherwise healthy session.
class WireBuffer {
public:
    void append(const uint8_t* data, size_t size);

    // Moves the next complete record into `record` and returns true.  Returns
    // false when more bytes are needed; returns an error only for a length no
    // valid peer could have sent, which is a framing/authentication failure
    // and must end the session.
    folly::Expected<bool, std::string> next(std::vector<uint8_t>* record);

    // Drops zero padding sitting at a record boundary and returns how many
    // bytes went.  Safe because a length of 0 cannot begin a valid record
    // (the minimum is tag + channel byte = 17), so four zero bytes are never
    // a header.  Bytes are dropped one at a time, so a genuine small record
    // -- 74 bytes is 00 00 00 4a, three leading zeros -- is never eaten.
    size_t skip_zero_padding();

    // Raw access, for the handshake that shares this stream with the records.
    size_t size() const { return buffer_.size(); }
    const uint8_t* data() const { return buffer_.data(); }
    bool starts_with(const uint8_t* prefix, size_t length) const;
    void consume(size_t count);
    void clear() { buffer_.clear(); }

private:
    std::vector<uint8_t> buffer_;
};

folly::Expected<std::vector<uint8_t>, std::string> make_wire_record(
    Channel channel, uint8_t stream, const std::vector<uint8_t>& payload,
    class SecureRecord& record);

folly::Expected<WireMessage, std::string> open_wire_record(
    const std::vector<uint8_t>& wire, class SecureRecord& record);

}  // namespace camera::secure
