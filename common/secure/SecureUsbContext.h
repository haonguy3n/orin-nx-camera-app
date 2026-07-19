#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "camera/base/Expected.h"
#include "camera/base/Unit.h"
#include "secure/SecureHandshake.h"
#include "secure/SecureWire.h"

namespace camera::secure {

// An authenticated encrypted byte session over caller-owned I/O.  The
// callbacks deliberately use no GLib, libusb, or FunctionFS types, so the same
// core can sit over a USB endpoint, a socketpair in tests, or another stream.
class SecureUsbSession {
public:
    using Read = std::function<base::Expected<size_t, std::string>(
        uint8_t* buffer, size_t capacity)>;
    using Write = std::function<base::Expected<size_t, std::string>(
        const uint8_t* data, size_t size)>;

    struct Transport {
        Read read;
        Write write;
    };

    SecureUsbSession(const SecureUsbSession&) = delete;
    SecureUsbSession& operator=(const SecureUsbSession&) = delete;
    SecureUsbSession(SecureUsbSession&&) noexcept;
    SecureUsbSession& operator=(SecureUsbSession&&) noexcept;
    ~SecureUsbSession();

    // Thread-safe: concurrent producers are serialized in record-counter
    // order before bytes are handed to the transport.
    base::Expected<base::Unit, std::string> send(
        Channel channel, uint8_t stream, const uint8_t* data, size_t size);
    base::Expected<base::Unit, std::string> send(
        Channel channel, uint8_t stream, const std::vector<uint8_t>& data) {
        return send(channel, stream, data.data(), data.size());
    }

    // Blocking receive. Only one thread may call receive() at a time.
    base::Expected<WireMessage, std::string> receive();

    // Decode one complete framed record already read by an adapter. Only one
    // thread may call open_record() at a time. This is
    // the receive-side counterpart to send() for transports such as
    // FunctionFS that must inspect the byte stream for a replacement
    // ClientHello before handing records to the secure session.
    base::Expected<WireMessage, std::string> open_record(
        const std::vector<uint8_t>& wire);

private:
    friend class SecureUsbContext;
    SecureUsbSession(Transport transport, const SessionKeys& keys,
                     WireBuffer buffered_input);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Device-side secure-USB equivalent of SSLContext: create() loads and
// validates the identity once; accept() performs a handshake over supplied
// I/O and returns a ready encrypted session.
class SecureUsbContext {
public:
    struct Options {
        std::string certificate;
        std::string private_key;
    };

    [[nodiscard]] static base::Expected<SecureUsbContext, std::string>
    create(Options options);

    SecureUsbContext(const SecureUsbContext&) = delete;
    SecureUsbContext& operator=(const SecureUsbContext&) = delete;
    SecureUsbContext(SecureUsbContext&&) noexcept = default;
    SecureUsbContext& operator=(SecureUsbContext&&) noexcept = default;

    [[nodiscard]] base::Expected<SecureUsbSession, std::string>
    accept(SecureUsbSession::Transport transport) const;

    // Accept a ClientHello already separated by an adapter. The returned
    // session uses the transport for encrypted writes, while incoming records
    // are supplied through open_record(). Only the write callback is required
    // in this adapter-driven form.
    [[nodiscard]] base::Expected<SecureUsbSession, std::string>
    accept(SecureUsbSession::Transport transport,
           const std::vector<uint8_t>& client_hello) const;

private:
    struct Negotiated {
        std::vector<uint8_t> server_hello;
        SessionKeys keys;
    };

    explicit SecureUsbContext(DeviceHandshake handshake)
        : handshake_(std::move(handshake)) {}

    [[nodiscard]] base::Expected<Negotiated, std::string> negotiate(
        const std::vector<uint8_t>& client_hello) const;

    DeviceHandshake handshake_;
};

}  // namespace camera::secure
