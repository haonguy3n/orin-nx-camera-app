#include "secure/SecureHandshake.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "camera/base/Unit.h"

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

namespace camera::secure {
namespace {


constexpr uint8_t kServerMagic[] = {'C', 'S', 'U', '2'};

constexpr size_t kMaxCertificateSize = 16 * 1024;
constexpr size_t kMaxSignatureSize = 1024;

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

camera::base::Expected<PkeyPtr, std::string> make_p256_key() {
    EVP_PKEY_CTX* raw = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (raw == nullptr)
        return camera::base::makeUnexpected(std::string("P-256 context creation failed"));
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        raw, EVP_PKEY_CTX_free);
    EVP_PKEY* key = nullptr;
    if (EVP_PKEY_keygen_init(raw) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(raw, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(raw, &key) <= 0)
        return camera::base::makeUnexpected(std::string("P-256 key generation failed"));
    return PkeyPtr(key, EVP_PKEY_free);
}

camera::base::Expected<std::array<uint8_t, 65>, std::string> public_point(EVP_PKEY* key) {
    std::array<uint8_t, 65> result{};
    size_t length = result.size();
    if (EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY,
                                        result.data(), result.size(), &length) <= 0 ||
        length != result.size() || result.front() != 0x04)
        return camera::base::makeUnexpected(std::string("invalid P-256 public key"));
    return result;
}

camera::base::Expected<PkeyPtr, std::string> peer_key(
    const std::array<uint8_t, 65>& point) {
    if (point.front() != 0x04)
        return camera::base::makeUnexpected(std::string("invalid P-256 point"));
    EC_KEY* raw_ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    if (raw_ec == nullptr)
        return camera::base::makeUnexpected(std::string("P-256 peer context failed"));
    std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> ec(raw_ec, EC_KEY_free);
    const unsigned char* cursor = point.data();
    if (o2i_ECPublicKey(&raw_ec, &cursor, point.size()) == nullptr ||
        cursor != point.data() + point.size())
        return camera::base::makeUnexpected(std::string("P-256 peer key creation failed"));
    // o2i_ECPublicKey may replace the EC_KEY allocation.
    ec.release();
    EVP_PKEY* key = EVP_PKEY_new();
    if (key == nullptr || EVP_PKEY_assign_EC_KEY(key, raw_ec) != 1) {
        EVP_PKEY_free(key);
        EC_KEY_free(raw_ec);
        return camera::base::makeUnexpected(std::string("P-256 peer key creation failed"));
    }
    return PkeyPtr(key, EVP_PKEY_free);
}

camera::base::Expected<std::array<uint8_t, 32>, std::string> shared_secret(
    EVP_PKEY* private_key, EVP_PKEY* peer) {
    EVP_PKEY_CTX* raw = EVP_PKEY_CTX_new(private_key, nullptr);
    if (raw == nullptr)
        return camera::base::makeUnexpected(std::string("ECDH context creation failed"));
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        raw, EVP_PKEY_CTX_free);
    std::array<uint8_t, 32> secret{};
    size_t length = secret.size();
    if (EVP_PKEY_derive_init(raw) <= 0 || EVP_PKEY_derive_set_peer(raw, peer) <= 0 ||
        EVP_PKEY_derive(raw, secret.data(), &length) <= 0 || length != secret.size())
        return camera::base::makeUnexpected(std::string("P-256 ECDH failed"));
    return secret;
}

std::vector<uint8_t> signature_input(const ClientHello& client,
                                     const ServerHello& server) {
    auto encoded = encode_client_hello(client);
    std::vector<uint8_t> data = encoded.hasValue() ? encoded.value()
                                                   : std::vector<uint8_t>{};
    data.insert(data.end(), server.nonce.begin(), server.nonce.end());
    data.insert(data.end(), server.public_key.begin(), server.public_key.end());
    return data;
}

camera::base::Expected<std::vector<uint8_t>, std::string> sign(EVP_PKEY* key,
                                                         const std::vector<uint8_t>& data) {
    MdCtxPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1 ||
        EVP_DigestSignUpdate(context.get(), data.data(), data.size()) != 1)
        return camera::base::makeUnexpected(std::string("handshake signature initialization failed"));
    size_t length = 0;
    if (EVP_DigestSignFinal(context.get(), nullptr, &length) != 1 || length > kMaxSignatureSize)
        return camera::base::makeUnexpected(std::string("handshake signature sizing failed"));
    std::vector<uint8_t> result(length);
    if (EVP_DigestSignFinal(context.get(), result.data(), &length) != 1)
        return camera::base::makeUnexpected(std::string("handshake signature failed"));
    result.resize(length);
    return result;
}

camera::base::Expected<bool, std::string> verify(EVP_PKEY* key,
                                          const std::vector<uint8_t>& data,
                                          const std::vector<uint8_t>& signature) {
    MdCtxPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1 ||
        EVP_DigestVerifyUpdate(context.get(), data.data(), data.size()) != 1)
        return camera::base::makeUnexpected(std::string("handshake verification initialization failed"));
    return EVP_DigestVerifyFinal(context.get(), signature.data(), signature.size()) == 1;
}

camera::base::Expected<X509Ptr, std::string> read_pem_certificate(const std::string& path) {
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "r"), fclose);
    if (!file)
        return camera::base::makeUnexpected(std::string("cannot read pinned certificate: ") + path);
    X509* cert = PEM_read_X509(file.get(), nullptr, nullptr, nullptr);
    if (cert == nullptr)
        return camera::base::makeUnexpected(std::string("invalid PEM certificate: ") + path);
    return X509Ptr(cert, X509_free);
}

// Verify the device certificate against the trust anchors in `path`.
//
// One path covers both trust models, so callers do not choose between them:
//   * path holds a CA certificate  -> any device it signed is accepted, which
//     is what lets a camera be swapped without re-pairing every host.
//   * path holds the device's own self-signed certificate -> it is its own
//     anchor and only that exact certificate verifies, i.e. pinning.
//
// PARTIAL_CHAIN additionally lets a CA-signed leaf be pinned directly, by
// treating a non-self-signed anchor as trusted on its own.
camera::base::Expected<camera::base::Unit, std::string> verify_against_trust_anchor(
    X509* presented, const std::string& path) {
    std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(
        X509_STORE_new(), X509_STORE_free);
    if (!store)
        return camera::base::makeUnexpected(std::string("cannot allocate certificate store"));
    if (X509_STORE_load_locations(store.get(), path.c_str(), nullptr) != 1)
        return camera::base::makeUnexpected(std::string("cannot read trust anchor: ") + path);
    X509_STORE_set_flags(store.get(), X509_V_FLAG_PARTIAL_CHAIN);

    std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> ctx(
        X509_STORE_CTX_new(), X509_STORE_CTX_free);
    if (!ctx || X509_STORE_CTX_init(ctx.get(), store.get(), presented, nullptr) != 1)
        return camera::base::makeUnexpected(std::string("cannot initialise certificate verification"));
    if (X509_verify_cert(ctx.get()) != 1) {
        const int reason = X509_STORE_CTX_get_error(ctx.get());
        return camera::base::makeUnexpected(
            std::string("secure USB device certificate is not trusted: ")
            + X509_verify_cert_error_string(reason));
    }
    return camera::base::unit;
}

camera::base::Expected<PkeyPtr, std::string> read_pem_private_key(const std::string& path) {
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "r"), fclose);
    if (!file)
        return camera::base::makeUnexpected(std::string("cannot read private key: ") + path);
    EVP_PKEY* key = PEM_read_PrivateKey(file.get(), nullptr, nullptr, nullptr);
    if (key == nullptr)
        return camera::base::makeUnexpected(std::string("invalid private key: ") + path);
    return PkeyPtr(key, EVP_PKEY_free);
}

}  // namespace

camera::base::Expected<std::vector<uint8_t>, std::string> encode_client_hello(const ClientHello& hello) {
    std::vector<uint8_t> wire(kClientHelloSize);
    std::copy(std::begin(kClientHelloMagic), std::end(kClientHelloMagic), wire.begin());
    std::copy(hello.nonce.begin(), hello.nonce.end(), wire.begin() + 4);
    std::copy(hello.public_key.begin(), hello.public_key.end(), wire.begin() + 36);
    return wire;
}

camera::base::Expected<ClientHello, std::string> decode_client_hello(const std::vector<uint8_t>& wire) {
    if (wire.size() != kClientHelloSize ||
        !std::equal(std::begin(kClientHelloMagic), std::end(kClientHelloMagic), wire.begin()))
        return camera::base::makeUnexpected(std::string("invalid secure USB ClientHello"));
    ClientHello hello;
    std::copy_n(wire.begin() + 4, hello.nonce.size(), hello.nonce.begin());
    std::copy_n(wire.begin() + 36, hello.public_key.size(), hello.public_key.begin());
    return hello;
}

camera::base::Expected<std::vector<uint8_t>, std::string> encode_server_hello(const ServerHello& hello) {
    if (hello.certificate_der.empty() || hello.certificate_der.size() > kMaxCertificateSize ||
        hello.signature_der.empty() || hello.signature_der.size() > kMaxSignatureSize)
        return camera::base::makeUnexpected(std::string("invalid secure USB ServerHello fields"));
    const size_t fixed = 4 + 32 + 65 + 2 + 2;
    std::vector<uint8_t> wire(fixed + hello.certificate_der.size() + hello.signature_der.size());
    std::copy(std::begin(kServerMagic), std::end(kServerMagic), wire.begin());
    std::copy(hello.nonce.begin(), hello.nonce.end(), wire.begin() + 4);
    std::copy(hello.public_key.begin(), hello.public_key.end(), wire.begin() + 36);
    const size_t cert_offset = 101;
    wire[cert_offset] = static_cast<uint8_t>(hello.certificate_der.size() >> 8);
    wire[cert_offset + 1] = static_cast<uint8_t>(hello.certificate_der.size());
    wire[cert_offset + 2] = static_cast<uint8_t>(hello.signature_der.size() >> 8);
    wire[cert_offset + 3] = static_cast<uint8_t>(hello.signature_der.size());
    std::copy(hello.certificate_der.begin(), hello.certificate_der.end(), wire.begin() + fixed);
    std::copy(hello.signature_der.begin(), hello.signature_der.end(),
              wire.begin() + fixed + hello.certificate_der.size());
    return wire;
}

camera::base::Expected<ServerHello, std::string> decode_server_hello(const std::vector<uint8_t>& wire) {
    constexpr size_t fixed = 4 + 32 + 65 + 2 + 2;
    if (wire.size() < fixed ||
        !std::equal(std::begin(kServerMagic), std::end(kServerMagic), wire.begin()))
        return camera::base::makeUnexpected(std::string("invalid secure USB ServerHello"));
    const size_t cert_length = (static_cast<size_t>(wire[101]) << 8) | wire[102];
    const size_t signature_length = (static_cast<size_t>(wire[103]) << 8) | wire[104];
    if (cert_length == 0 || cert_length > kMaxCertificateSize || signature_length == 0 ||
        signature_length > kMaxSignatureSize || wire.size() != fixed + cert_length + signature_length)
        return camera::base::makeUnexpected(std::string("invalid secure USB ServerHello length"));
    ServerHello hello;
    std::copy_n(wire.begin() + 4, hello.nonce.size(), hello.nonce.begin());
    std::copy_n(wire.begin() + 36, hello.public_key.size(), hello.public_key.begin());
    hello.certificate_der.assign(wire.begin() + fixed, wire.begin() + fixed + cert_length);
    hello.signature_der.assign(wire.begin() + fixed + cert_length, wire.end());
    return hello;
}

ClientHandshake::ClientHandshake(void* key, ClientHello hello) : key_(key), hello_(hello) {}

ClientHandshake::~ClientHandshake() {
    EVP_PKEY_free(static_cast<EVP_PKEY*>(key_));
}

camera::base::Expected<std::unique_ptr<ClientHandshake>, std::string> ClientHandshake::create() {
    auto key = make_p256_key();
    if (!key.hasValue())
        return camera::base::makeUnexpected(key.error());
    ClientHello hello;
    if (RAND_bytes(hello.nonce.data(), hello.nonce.size()) != 1)
        return camera::base::makeUnexpected(std::string("secure USB random generation failed"));
    auto point = public_point(key.value().get());
    if (!point.hasValue())
        return camera::base::makeUnexpected(point.error());
    hello.public_key = point.value();
    return std::unique_ptr<ClientHandshake>(new ClientHandshake(key.value().release(), hello));
}

camera::base::Expected<std::vector<uint8_t>, std::string> ClientHandshake::client_hello() const {
    return encode_client_hello(hello_);
}

camera::base::Expected<SessionKeys, std::string> ClientHandshake::complete(
    const std::vector<uint8_t>& wire, const std::string& pinned_certificate) const {
    auto hello = decode_server_hello(wire);
    if (!hello.hasValue())
        return camera::base::makeUnexpected(hello.error());
    const unsigned char* cursor = hello.value().certificate_der.data();
    X509Ptr presented(d2i_X509(nullptr, &cursor, hello.value().certificate_der.size()), X509_free);
    if (!presented || cursor != hello.value().certificate_der.data() + hello.value().certificate_der.size())
        return camera::base::makeUnexpected(std::string("invalid device certificate in handshake"));
    auto trusted = verify_against_trust_anchor(presented.get(), pinned_certificate);
    if (!trusted.hasValue())
        return camera::base::makeUnexpected(trusted.error());
    PkeyPtr signing_key(X509_get_pubkey(presented.get()), EVP_PKEY_free);
    if (!signing_key)
        return camera::base::makeUnexpected(std::string("device certificate has no public key"));
    auto valid = verify(signing_key.get(), signature_input(hello_, hello.value()),
                        hello.value().signature_der);
    if (!valid.hasValue())
        return camera::base::makeUnexpected(valid.error());
    if (!valid.value())
        return camera::base::makeUnexpected(std::string("secure USB handshake signature rejected"));
    auto peer = peer_key(hello.value().public_key);
    if (!peer.hasValue())
        return camera::base::makeUnexpected(peer.error());
    auto secret = shared_secret(static_cast<EVP_PKEY*>(key_), peer.value().get());
    if (!secret.hasValue())
        return camera::base::makeUnexpected(secret.error());
    return derive_session_keys(secret.value(), hello_.nonce, hello.value().nonce);
}

camera::base::Expected<std::pair<std::vector<uint8_t>, SessionKeys>, std::string>
DeviceHandshake::respond(const std::vector<uint8_t>& client_wire,
                         const std::string& certificate_path,
                         const std::string& private_key_path) {
    auto client = decode_client_hello(client_wire);
    if (!client.hasValue())
        return camera::base::makeUnexpected(client.error());
    auto certificate = read_pem_certificate(certificate_path);
    auto signing_key = read_pem_private_key(private_key_path);
    auto ephemeral = make_p256_key();
    if (!certificate.hasValue() || !signing_key.hasValue() || !ephemeral.hasValue())
        return camera::base::makeUnexpected(!certificate.hasValue() ? certificate.error()
                                      : !signing_key.hasValue() ? signing_key.error()
                                                                 : ephemeral.error());
    ServerHello server;
    if (RAND_bytes(server.nonce.data(), server.nonce.size()) != 1)
        return camera::base::makeUnexpected(std::string("secure USB random generation failed"));
    auto point = public_point(ephemeral.value().get());
    if (!point.hasValue())
        return camera::base::makeUnexpected(point.error());
    server.public_key = point.value();
    int cert_length = i2d_X509(certificate.value().get(), nullptr);
    if (cert_length <= 0 || static_cast<size_t>(cert_length) > kMaxCertificateSize)
        return camera::base::makeUnexpected(std::string("device certificate DER encoding failed"));
    server.certificate_der.resize(cert_length);
    unsigned char* cert_cursor = server.certificate_der.data();
    if (i2d_X509(certificate.value().get(), &cert_cursor) != cert_length)
        return camera::base::makeUnexpected(std::string("device certificate DER encoding failed"));
    auto signature = sign(signing_key.value().get(), signature_input(client.value(), server));
    if (!signature.hasValue())
        return camera::base::makeUnexpected(signature.error());
    server.signature_der = signature.value();
    auto wire = encode_server_hello(server);
    if (!wire.hasValue())
        return camera::base::makeUnexpected(wire.error());
    auto peer = peer_key(client.value().public_key);
    if (!peer.hasValue())
        return camera::base::makeUnexpected(peer.error());
    auto secret = shared_secret(ephemeral.value().get(), peer.value().get());
    if (!secret.hasValue())
        return camera::base::makeUnexpected(secret.error());
    auto keys = derive_session_keys(secret.value(), client.value().nonce, server.nonce);
    if (!keys.hasValue())
        return camera::base::makeUnexpected(keys.error());
    return std::make_pair(wire.value(), keys.value());
}

}  // namespace camera::secure
