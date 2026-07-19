#include "secure/SecureUsbContext.h"

#include <algorithm>
#include <array>
#include <utility>

#include "camera/secure/SecureRecord.h"

namespace camera::secure {

namespace {

base::Expected<base::Unit, std::string> write_fully(
    const SecureUsbSession::Write& write, const uint8_t* data, size_t size) {
    while (size != 0) {
        auto written = write(data, size);
        if (!written)
            return base::makeUnexpected(written.error());
        if (*written == 0 || *written > size)
            return base::makeUnexpected(
                std::string("secure USB transport made no write progress"));
        data += *written;
        size -= *written;
    }
    return base::unit;
}

}  // namespace

struct SecureUsbSession::Impl {
    Impl(Transport io, const SessionKeys& keys, WireBuffer buffered_input)
        : transport(std::move(io)),
          encrypt(keys.device_key, keys.device_iv),
          decrypt(keys.host_key, keys.host_iv),
          input(std::move(buffered_input)) {}

    Transport transport;
    SecureRecord encrypt;
    SecureRecord decrypt;
    WireBuffer input;
    std::mutex send_mutex;
};

SecureUsbSession::SecureUsbSession(Transport transport, const SessionKeys& keys,
                                   WireBuffer buffered_input)
    : impl_(std::make_unique<Impl>(std::move(transport), keys,
                                   std::move(buffered_input))) {}

SecureUsbSession::~SecureUsbSession() = default;
SecureUsbSession::SecureUsbSession(SecureUsbSession&&) noexcept = default;
SecureUsbSession& SecureUsbSession::operator=(SecureUsbSession&&) noexcept = default;

base::Expected<base::Unit, std::string> SecureUsbSession::send(
    Channel channel, uint8_t stream, const uint8_t* data, size_t size) {
    if (size != 0 && data == nullptr)
        return base::makeUnexpected(
            std::string("secure USB send received null data"));
    std::lock_guard<std::mutex> lock(impl_->send_mutex);
    std::vector<uint8_t> payload;
    if (size != 0)
        payload.assign(data, data + size);
    auto wire = make_wire_record(channel, stream, payload, impl_->encrypt);
    if (!wire)
        return base::makeUnexpected(wire.error());
    return write_fully(impl_->transport.write, wire->data(), wire->size());
}

base::Expected<WireMessage, std::string> SecureUsbSession::receive() {
    if (!impl_->transport.read)
        return base::makeUnexpected(
            std::string("secure USB session has no read callback"));
    std::array<uint8_t, 16 * 1024> chunk{};
    for (;;) {
        impl_->input.skip_zero_padding();
        std::vector<uint8_t> record;
        auto framed = impl_->input.next(&record);
        if (!framed)
            return base::makeUnexpected(framed.error());
        if (*framed)
            return open_record(record);

        auto received = impl_->transport.read(chunk.data(), chunk.size());
        if (!received)
            return base::makeUnexpected(received.error());
        if (*received == 0)
            return base::makeUnexpected(
                std::string("secure USB transport reached end of stream"));
        if (*received > chunk.size())
            return base::makeUnexpected(
                std::string("secure USB transport returned an invalid read size"));
        impl_->input.append(chunk.data(), *received);
    }
}

base::Expected<WireMessage, std::string> SecureUsbSession::open_record(
    const std::vector<uint8_t>& wire) {
    return open_wire_record(wire, impl_->decrypt);
}

base::Expected<SecureUsbContext, std::string>
SecureUsbContext::create(Options options) {
    if (options.certificate.empty() || options.private_key.empty())
        return base::makeUnexpected(
            std::string("secure USB needs device certificate and private key"));
    auto handshake = DeviceHandshake::create(options.certificate,
                                              options.private_key);
    if (!handshake)
        return base::makeUnexpected("secure USB identity: " + handshake.error());
    return SecureUsbContext(std::move(*handshake));
}

base::Expected<SecureUsbContext::Negotiated, std::string>
SecureUsbContext::negotiate(const std::vector<uint8_t>& client_hello) const {
    auto response = handshake_.respond(client_hello);
    if (!response)
        return base::makeUnexpected(response.error());
    return Negotiated{std::move(response->first), response->second};
}

base::Expected<SecureUsbSession, std::string>
SecureUsbContext::accept(SecureUsbSession::Transport transport) const {
    if (!transport.read || !transport.write)
        return base::makeUnexpected(
            std::string("secure USB accept needs read and write callbacks"));

    WireBuffer input;
    std::array<uint8_t, 16 * 1024> chunk{};
    while (input.size() < kClientHelloSize) {
        auto received = transport.read(chunk.data(), chunk.size());
        if (!received)
            return base::makeUnexpected(received.error());
        if (*received == 0)
            return base::makeUnexpected(
                std::string("secure USB transport ended during handshake"));
        if (*received > chunk.size())
            return base::makeUnexpected(
                std::string("secure USB transport returned an invalid read size"));
        input.append(chunk.data(), *received);
    }
    if (!input.starts_with(kClientHelloMagic, sizeof(kClientHelloMagic)))
        return base::makeUnexpected(std::string("invalid secure USB ClientHello"));

    const std::vector<uint8_t> hello(input.data(),
                                     input.data() + kClientHelloSize);
    input.consume(kClientHelloSize);
    auto session = accept(std::move(transport), hello);
    if (!session)
        return base::makeUnexpected(session.error());
    // The ordinary accept path may have read the first encrypted record in
    // the same bulk/socket read as the ClientHello. Preserve it in the
    // returned session rather than dropping it at the handshake boundary.
    session->impl_->input = std::move(input);
    return std::move(*session);
}

base::Expected<SecureUsbSession, std::string>
SecureUsbContext::accept(SecureUsbSession::Transport transport,
                         const std::vector<uint8_t>& client_hello) const {
    if (!transport.write)
        return base::makeUnexpected(
            std::string("secure USB accept needs a write callback"));
    auto negotiated = negotiate(client_hello);
    if (!negotiated)
        return base::makeUnexpected(negotiated.error());
    if (auto written = write_fully(transport.write,
                                   negotiated->server_hello.data(),
                                   negotiated->server_hello.size());
        !written)
        return base::makeUnexpected(written.error());

    return SecureUsbSession(std::move(transport), negotiated->keys,
                            WireBuffer{});
}

}  // namespace camera::secure
