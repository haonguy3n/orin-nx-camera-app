#include "camera/secure/SecureRecord.h"

#include <openssl/evp.h>

#include <limits>
#include <memory>

namespace camera::secure {

namespace {

constexpr size_t kTagLength = 16;
// Must not be below SecureWire's kMaxPayload, or every full-size record fails
// to seal and the session dies on the first large transfer.
constexpr size_t kMaxPlaintext = 64 * 1024;

std::string openssl_error(const char* operation) {
    return std::string(operation) + " failed";
}

}  // namespace

SecureRecord::SecureRecord(Key key, Iv iv) : key_(key), iv_(iv) {}

std::array<uint8_t, 12> SecureRecord::next_nonce() {
    auto nonce = iv_;
    const auto sequence = sequence_++;
    for (int i = 0; i < 8; ++i)
        nonce[4 + i] ^= static_cast<uint8_t>(sequence >> ((7 - i) * 8));
    return nonce;
}

folly::Expected<std::vector<uint8_t>, std::string> SecureRecord::seal(
    const uint8_t* plaintext, size_t length) {
    if (length > kMaxPlaintext || length > std::numeric_limits<int>::max())
        return folly::makeUnexpected(std::string("secure record too large"));

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr)
        return folly::makeUnexpected(openssl_error("EVP_CIPHER_CTX_new"));
    auto cleanup = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
        ctx, EVP_CIPHER_CTX_free);
    const auto nonce = next_nonce();
    std::vector<uint8_t> out(length + kTagLength);
    int written = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr,
                           nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce.size(),
                            nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce.data()) != 1 ||
        EVP_EncryptUpdate(ctx, out.data(), &written, plaintext,
                          static_cast<int>(length)) != 1)
        return folly::makeUnexpected(openssl_error("ChaCha20-Poly1305 encrypt"));
    int final = 0;
    if (EVP_EncryptFinal_ex(ctx, out.data() + written, &final) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, kTagLength,
                            out.data() + length) != 1)
        return folly::makeUnexpected(openssl_error("ChaCha20-Poly1305 finalize"));
    return out;
}

folly::Expected<std::vector<uint8_t>, std::string> SecureRecord::open(
    const uint8_t* ciphertext_and_tag, size_t length) {
    if (length < kTagLength || length - kTagLength > kMaxPlaintext ||
        length > std::numeric_limits<int>::max())
        return folly::makeUnexpected(std::string("invalid secure record length"));
    const size_t ciphertext_length = length - kTagLength;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr)
        return folly::makeUnexpected(openssl_error("EVP_CIPHER_CTX_new"));
    auto cleanup = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
        ctx, EVP_CIPHER_CTX_free);
    const auto nonce = next_nonce();
    std::vector<uint8_t> out(ciphertext_length);
    int written = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr,
                           nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce.size(),
                            nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_.data(), nonce.data()) != 1 ||
        EVP_DecryptUpdate(ctx, out.data(), &written, ciphertext_and_tag,
                          static_cast<int>(ciphertext_length)) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, kTagLength,
                            const_cast<uint8_t*>(ciphertext_and_tag + ciphertext_length)) != 1)
        return folly::makeUnexpected(openssl_error("ChaCha20-Poly1305 decrypt"));
    int final = 0;
    if (EVP_DecryptFinal_ex(ctx, out.data() + written, &final) != 1)
        return folly::makeUnexpected(std::string("secure record authentication failed"));
    return out;
}

}  // namespace camera::secure
