#include "videocut/base/Sha256.h"

#include <algorithm>
#include <cstring>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <limits>
#endif

namespace videocut::base {
#if !defined(__APPLE__)
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
}};

constexpr std::uint32_t RotateRight(std::uint32_t value,
                                    std::uint32_t amount) noexcept {
  return (value >> amount) | (value << (32U - amount));
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{{0x6a09e667U,
              0xbb67ae85U,
              0x3c6ef372U,
              0xa54ff53aU,
              0x510e527fU,
              0x9b05688cU,
              0x1f83d9abU,
              0x5be0cd19U}} {}

void Sha256::Transform(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16U; ++index) {
    const std::size_t offset = index * 4U;
    schedule[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                      (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                      static_cast<std::uint32_t>(block[offset + 3U]);
  }
  for (std::size_t index = 16U; index < schedule.size(); ++index) {
    const std::uint32_t s0 = RotateRight(schedule[index - 15U], 7U) ^
                             RotateRight(schedule[index - 15U], 18U) ^
                             (schedule[index - 15U] >> 3U);
    const std::uint32_t s1 = RotateRight(schedule[index - 2U], 17U) ^
                             RotateRight(schedule[index - 2U], 19U) ^
                             (schedule[index - 2U] >> 10U);
    schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < schedule.size(); ++index) {
    const std::uint32_t sum1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^
                               RotateRight(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temporary1 =
        h + sum1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t sum0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^
                               RotateRight(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temporary2 = sum0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}
#else
Sha256::Sha256() noexcept {
  static_assert(CC_SHA256_DIGEST_LENGTH == 32);
  (void)CC_SHA256_Init(&context_);
}
#endif

void Sha256::Update(const void* data, std::size_t size) noexcept {
  if (finalized_ || size == 0U) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
#if defined(__APPLE__)
  while (size > 0U) {
    const auto chunk = std::min(
        size, static_cast<std::size_t>(std::numeric_limits<CC_LONG>::max()));
    (void)CC_SHA256_Update(&context_, bytes, static_cast<CC_LONG>(chunk));
    bytes += chunk;
    size -= chunk;
  }
#else
  bitCount_ += static_cast<std::uint64_t>(size) * 8U;

  while (size > 0U) {
    const std::size_t available = buffer_.size() - buffered_;
    const std::size_t copied = std::min(size, available);
    std::memcpy(buffer_.data() + buffered_, bytes, copied);
    buffered_ += copied;
    bytes += copied;
    size -= copied;
    if (buffered_ == buffer_.size()) {
      Transform(buffer_.data());
      buffered_ = 0U;
    }
  }
#endif
}

std::array<std::uint8_t, 32> Sha256::Finalize() noexcept {
#if defined(__APPLE__)
  if (!finalized_) {
    (void)CC_SHA256_Final(digest_.data(), &context_);
    finalized_ = true;
  }
  return digest_;
#else
  if (!finalized_) {
    buffer_[buffered_++] = 0x80U;
    if (buffered_ > 56U) {
      std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                buffer_.end(), 0U);
      Transform(buffer_.data());
      buffered_ = 0U;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
              buffer_.begin() + 56, 0U);
    for (std::size_t index = 0; index < 8U; ++index) {
      const auto shift = static_cast<std::uint32_t>((7U - index) * 8U);
      buffer_[56U + index] =
          static_cast<std::uint8_t>((bitCount_ >> shift) & 0xffU);
    }
    Transform(buffer_.data());
    finalized_ = true;
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < state_.size(); ++index) {
    digest[index * 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
    digest[index * 4U + 1U] = static_cast<std::uint8_t>(state_[index] >> 16U);
    digest[index * 4U + 2U] = static_cast<std::uint8_t>(state_[index] >> 8U);
    digest[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index]);
  }
  return digest;
#endif
}

std::string Sha256::HexDigest(std::string_view data) {
  Sha256 hash;
  hash.Update(data.data(), data.size());
  return ToHex(hash.Finalize());
}

std::string Sha256::ToHex(const std::array<std::uint8_t, 32>& digest) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string value;
  value.resize(digest.size() * 2U);
  for (std::size_t index = 0; index < digest.size(); ++index) {
    value[index * 2U] = kHex[digest[index] >> 4U];
    value[index * 2U + 1U] = kHex[digest[index] & 0x0fU];
  }
  return value;
}

bool Sha256::IsCanonicalDigest(std::string_view value) noexcept {
  constexpr std::string_view prefix{"sha256:"};
  return value.size() == prefix.size() + 64U &&
         value.substr(0U, prefix.size()) == prefix &&
         std::all_of(value.begin() +
                         static_cast<std::ptrdiff_t>(prefix.size()),
                     value.end(), [](const char byte) noexcept {
                       return (byte >= '0' && byte <= '9') ||
                              (byte >= 'a' && byte <= 'f');
                     });
}

}  // namespace videocut::base
