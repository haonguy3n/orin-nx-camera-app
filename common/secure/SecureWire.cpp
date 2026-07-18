#include "secure/SecureWire.h"

#include <algorithm>
#include <cstdio>

#include "camera/secure/SecureRecord.h"

namespace camera::secure {

void WireBuffer::append(const uint8_t* data, size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
}

bool WireBuffer::starts_with(const uint8_t* prefix, size_t length) const {
    // A shorter buffer is not a mismatch, only an undecided one: the caller
    // must wait for more bytes rather than treat it as a record.
    if (buffer_.size() < length)
        return false;
    return std::equal(prefix, prefix + length, buffer_.begin());
}

size_t WireBuffer::skip_zero_padding() {
    size_t skipped = 0;
    while (buffer_.size() >= 4 && buffer_[0] == 0 && buffer_[1] == 0
           && buffer_[2] == 0 && buffer_[3] == 0) {
        buffer_.erase(buffer_.begin());
        ++skipped;
    }
    return skipped;
}

void WireBuffer::consume(size_t count) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + std::min(count, buffer_.size()));
}

folly::Expected<bool, std::string> WireBuffer::next(std::vector<uint8_t>* record) {
    if (buffer_.size() < 4)
        return false;
    const uint32_t length = (static_cast<uint32_t>(buffer_[0]) << 24) |
                            (static_cast<uint32_t>(buffer_[1]) << 16) |
                            (static_cast<uint32_t>(buffer_[2]) << 8) | buffer_[3];
    // Reject before reserving anything: an attacker-supplied length is the
    // one field read prior to authentication.
    if (length > kMaxWireRecord || length < 16 + 1) {
        // Include the offending bytes. A desynchronised stream is impossible
        // to diagnose from the length alone, and the leading bytes usually
        // name the cause outright -- "CSU1"/"CSU2" here means a handshake
        // message is being read where a record was expected.
        char detail[128];
        const size_t shown = std::min<size_t>(8, buffer_.size());
        int offset = std::snprintf(detail, sizeof(detail),
                                   "invalid secure USB record length %u (bytes:", length);
        for (size_t i = 0; i < shown && offset > 0 && offset < static_cast<int>(sizeof(detail)); ++i) {
            offset += std::snprintf(detail + offset, sizeof(detail) - offset,
                                    " %02x", buffer_[i]);
        }
        if (offset > 0 && offset < static_cast<int>(sizeof(detail)))
            std::snprintf(detail + offset, sizeof(detail) - offset, ")");
        return folly::makeUnexpected(std::string(detail));
    }
    if (buffer_.size() < 4 + length)
        return false;
    record->assign(buffer_.begin(), buffer_.begin() + 4 + length);
    // ponytail: erase-from-front is O(remaining) per record; a ring buffer if
    // profiling ever shows this copy mattering next to the USB transfer.
    buffer_.erase(buffer_.begin(), buffer_.begin() + 4 + length);
    return true;
}

folly::Expected<std::vector<uint8_t>, std::string> make_wire_record(
    Channel channel, uint8_t stream, const std::vector<uint8_t>& payload,
    SecureRecord& record) {
    if (payload.size() + 2 > kMaxPayload)
        return folly::makeUnexpected(std::string("secure USB payload too large"));
    std::vector<uint8_t> plaintext;
    plaintext.reserve(payload.size() + 2);
    plaintext.push_back(static_cast<uint8_t>(channel));
    plaintext.push_back(stream);
    plaintext.insert(plaintext.end(), payload.begin(), payload.end());
    auto encrypted = record.seal(plaintext.data(), plaintext.size());
    if (!encrypted.hasValue())
        return folly::makeUnexpected(encrypted.error());
    if (encrypted.value().size() > kMaxWireRecord)
        return folly::makeUnexpected(std::string("secure USB encrypted record too large"));
    const uint32_t length = static_cast<uint32_t>(encrypted.value().size());
    std::vector<uint8_t> wire(4);
    wire[0] = static_cast<uint8_t>(length >> 24);
    wire[1] = static_cast<uint8_t>(length >> 16);
    wire[2] = static_cast<uint8_t>(length >> 8);
    wire[3] = static_cast<uint8_t>(length);
    wire.insert(wire.end(), encrypted.value().begin(), encrypted.value().end());
    return wire;
}

folly::Expected<WireMessage, std::string> open_wire_record(
    const std::vector<uint8_t>& wire, SecureRecord& record) {
    if (wire.size() < 4 + 16 + 1)
        return folly::makeUnexpected(std::string("truncated secure USB record"));
    const uint32_t length = (static_cast<uint32_t>(wire[0]) << 24) |
                            (static_cast<uint32_t>(wire[1]) << 16) |
                            (static_cast<uint32_t>(wire[2]) << 8) | wire[3];
    if (length > kMaxWireRecord || wire.size() != 4 + length)
        return folly::makeUnexpected(std::string("invalid secure USB record length"));
    auto plaintext = record.open(wire.data() + 4, length);
    if (!plaintext.hasValue())
        return folly::makeUnexpected(plaintext.error());
    if (plaintext.value().size() < 2)
        return folly::makeUnexpected(std::string("secure USB record has no header"));
    const auto channel = static_cast<Channel>(plaintext.value()[0]);
    if (channel != Channel::Video && channel != Channel::Control && channel != Channel::Update)
        return folly::makeUnexpected(std::string("unknown secure USB channel"));
    return WireMessage{channel, plaintext.value()[1],
                       std::vector<uint8_t>(plaintext.value().begin() + 2,
                                            plaintext.value().end())};
}

}  // namespace camera::secure
