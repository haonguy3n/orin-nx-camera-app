#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "camera/base/Expected.h"
#include "camera/secure/KeySchedule.h"

namespace camera::secure {

// Wire format is deliberately fixed-width before variable certificate fields.
// It is transported as a control packet by the USB bulk multiplexer.
struct ClientHello {
    std::array<uint8_t, 32> nonce{};
    std::array<uint8_t, 65> public_key{};  // uncompressed P-256 point
};

struct ServerHello {
    std::array<uint8_t, 32> nonce{};
    std::array<uint8_t, 65> public_key{};
    std::vector<uint8_t> certificate_der;
    std::vector<uint8_t> signature_der;
};

camera::base::Expected<std::vector<uint8_t>, std::string> encode_client_hello(
    const ClientHello& hello);
camera::base::Expected<ClientHello, std::string> decode_client_hello(
    const std::vector<uint8_t>& wire);
camera::base::Expected<std::vector<uint8_t>, std::string> encode_server_hello(
    const ServerHello& hello);
camera::base::Expected<ServerHello, std::string> decode_server_hello(
    const std::vector<uint8_t>& wire);

class ClientHandshake {
public:
    static camera::base::Expected<std::unique_ptr<ClientHandshake>, std::string> create();
    ~ClientHandshake();

    ClientHandshake(const ClientHandshake&) = delete;
    ClientHandshake& operator=(const ClientHandshake&) = delete;

    camera::base::Expected<std::vector<uint8_t>, std::string> client_hello() const;

    // The device certificate must verify against the trust anchors in the
    // given PEM file, which may be either a CA certificate (any device it
    // signed is accepted) or the device's own self-signed certificate (only
    // that exact device is accepted, i.e. pinning).  Failure to verify is an
    // authentication failure; callers must not downgrade to the unencrypted
    // transport on that failure.
    camera::base::Expected<SessionKeys, std::string> complete(
        const std::vector<uint8_t>& server_hello,
        const std::string& trust_anchor_pem) const;

private:
    ClientHandshake(void* key, ClientHello hello);
    void* key_ = nullptr;  // EVP_PKEY, opaque in this public header
    ClientHello hello_;
};

// ClientHello framing, public so the transport can recognise a handshake at a
// point in the byte stream where it would otherwise parse a record length.
// The host may re-send one at any time to restart the session.
inline constexpr uint8_t kClientHelloMagic[] = {'C', 'S', 'U', '1'};
inline constexpr size_t kClientHelloSize = sizeof(kClientHelloMagic) + 32 + 65;

class DeviceHandshake {
public:
    // Loads and validates the device identity once, like a TLS context.  A
    // successfully-created handshake is ready to answer any number of client
    // hellos; malformed credentials are therefore a startup error rather
    // than a failure discovered by the first connecting host.
    static camera::base::Expected<DeviceHandshake, std::string> create(
        const std::string& certificate_pem,
        const std::string& private_key_pem);

    ~DeviceHandshake();
    DeviceHandshake(const DeviceHandshake&) = delete;
    DeviceHandshake& operator=(const DeviceHandshake&) = delete;
    DeviceHandshake(DeviceHandshake&& other) noexcept;
    DeviceHandshake& operator=(DeviceHandshake&& other) noexcept;

    // Produces a signed ServerHello and directional session keys using the
    // identity loaded by create().
    camera::base::Expected<std::pair<std::vector<uint8_t>, SessionKeys>, std::string>
    respond(const std::vector<uint8_t>& client_hello) const;

private:
    DeviceHandshake(void* signing_key, std::vector<uint8_t> certificate_der);

    void* signing_key_ = nullptr;  // EVP_PKEY, opaque in this public header
    std::vector<uint8_t> certificate_der_;
};

}  // namespace camera::secure
