#include "camera/secure/KeySchedule.h"
#include <algorithm>
#include <memory>
#include <openssl/evp.h>
#include <openssl/kdf.h>
namespace camera::secure {
camera::base::Expected<SessionKeys, std::string> derive_session_keys(
    const std::array<uint8_t, 32>& shared_secret,
    const std::array<uint8_t, 32>& host_nonce,
    const std::array<uint8_t, 32>& device_nonce) {
    std::array<uint8_t, 64> salt{};
    std::copy(host_nonce.begin(), host_nonce.end(), salt.begin());
    std::copy(device_nonce.begin(), device_nonce.end(), salt.begin() + 32);

    EVP_PKEY_CTX* raw_context = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (raw_context == nullptr)
        return camera::base::makeUnexpected(std::string("HKDF context creation failed"));
    auto context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
        raw_context, EVP_PKEY_CTX_free);

    std::array<uint8_t, 88> material{};
    size_t material_length = material.size();
    constexpr char kInfo[] = "camera-secure-usb-v1";
    if (EVP_PKEY_derive_init(raw_context) <= 0 ||
        EVP_PKEY_CTX_set_hkdf_md(raw_context, EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_salt(raw_context, salt.data(), salt.size()) <= 0 ||
        EVP_PKEY_CTX_set1_hkdf_key(raw_context, shared_secret.data(),
                                   shared_secret.size()) <= 0 ||
        EVP_PKEY_CTX_add1_hkdf_info(
            raw_context, reinterpret_cast<const unsigned char*>(kInfo),
            sizeof(kInfo) - 1) <= 0 ||
        EVP_PKEY_derive(raw_context, material.data(), &material_length) <= 0 ||
        material_length != material.size()) {
        return camera::base::makeUnexpected(std::string("HKDF-SHA256 derivation failed"));
    }

    SessionKeys keys{};
    std::copy_n(material.begin(), keys.host_key.size(), keys.host_key.begin());
    std::copy_n(material.begin() + keys.host_key.size(), keys.device_key.size(),
                keys.device_key.begin());
    std::copy_n(material.begin() + keys.host_key.size() + keys.device_key.size(),
                keys.host_iv.size(), keys.host_iv.begin());
    std::copy_n(material.end() - keys.device_iv.size(), keys.device_iv.size(),
                keys.device_iv.begin());
    return keys;
}
}
