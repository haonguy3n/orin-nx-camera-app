#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "camera/base/Expected.h"
namespace camera::secure {
struct SessionKeys { std::array<uint8_t,32> host_key, device_key; std::array<uint8_t,12> host_iv, device_iv; };
camera::base::Expected<SessionKeys,std::string> derive_session_keys(const std::array<uint8_t,32>& secret, const std::array<uint8_t,32>& host_nonce, const std::array<uint8_t,32>& device_nonce);
}
