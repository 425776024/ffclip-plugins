#pragma once

#include "videocut/base/BaseApi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace videocut::base {

class VIDEOCUT_BASE_API Sha256 final {
 public:
  Sha256() noexcept;

  void Update(const void* data, std::size_t size) noexcept;
  [[nodiscard]] std::array<std::uint8_t, 32> Finalize() noexcept;

  [[nodiscard]] static std::string HexDigest(std::string_view data);
  [[nodiscard]] static std::string ToHex(
      const std::array<std::uint8_t, 32>& digest);

  // Requires "sha256:" followed by exactly 64 lowercase hexadecimal digits.
  [[nodiscard]] static bool IsCanonicalDigest(std::string_view value) noexcept;

 private:
#if defined(__APPLE__)
  CC_SHA256_CTX context_{};
  std::array<std::uint8_t, 32> digest_{};
#else
  void Transform(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t bitCount_{0};
  std::size_t buffered_{0};
#endif
  bool finalized_{false};
};

}  // namespace videocut::base
