// Native secure-USB record layer.  seal() emits ciphertext || 16-byte
// Poly1305 tag; record length is framed by SecureWire, not here.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "camera/base/Expected.h"

namespace camera::secure {

class SecureRecord {
public:
    using Key = std::array<uint8_t, 32>;
    using Iv = std::array<uint8_t, 12>;

    SecureRecord(Key key, Iv iv);

    camera::base::Expected<std::vector<uint8_t>, std::string> seal(
        const uint8_t* plaintext, size_t length);
    camera::base::Expected<std::vector<uint8_t>, std::string> open(
        const uint8_t* ciphertext_and_tag, size_t length);

private:
    std::array<uint8_t, 12> next_nonce();

    Key key_;
    Iv iv_;
    uint64_t sequence_ = 0;
};

}  // namespace camera::secure
