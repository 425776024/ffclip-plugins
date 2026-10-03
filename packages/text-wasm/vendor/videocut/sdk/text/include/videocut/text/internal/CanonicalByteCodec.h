#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace videocut::text::internal {

// Bounded, little-endian byte IO and structural value transfers. Each document
// format owns its domain marker, field order, validation and collection limits.
class CanonicalByteWriter final {
public:
  static constexpr bool kReading = false;

  CanonicalByteWriter(const std::size_t maximumBytes,
                      const std::size_t maximumStringBytes)
      : maximumBytes_(maximumBytes), maximumStringBytes_(maximumStringBytes) {}

  bool Boolean(bool &value) { return Byte(value ? 1U : 0U); }

  bool Unsigned(std::uint64_t &value) {
    for (unsigned shift = 0U; shift != 64U; shift += 8U) {
      if (!Byte(static_cast<std::uint8_t>((value >> shift) & 0xffU)))
        return false;
    }
    return true;
  }

  bool Signed(std::int64_t &value) {
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return Unsigned(bits);
  }

  bool Float(float &value) {
    if (!std::isfinite(value))
      return false;
    value = value == 0.0F ? 0.0F : value;
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    auto encoded = static_cast<std::uint64_t>(bits);
    return Unsigned(encoded);
  }

  bool Double(double &value) {
    if (!std::isfinite(value))
      return false;
    value = value == 0.0 ? 0.0 : value;
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return Unsigned(bits);
  }

  bool String(std::string &value) {
    if (value.size() > maximumStringBytes_)
      return false;
    auto size = static_cast<std::uint64_t>(value.size());
    if (!Unsigned(size) || value.size() > Remaining())
      return false;
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  bool Bytes(std::vector<std::uint8_t> &value) {
    auto size = static_cast<std::uint64_t>(value.size());
    if (!Unsigned(size) || value.size() > Remaining())
      return false;
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  bool Count(std::uint64_t &value, const std::size_t maximum) {
    return value <= maximum && Unsigned(value);
  }

  [[nodiscard]] bool ok() const noexcept {
    return bytes_.size() <= maximumBytes_;
  }

  [[nodiscard]] std::vector<std::uint8_t> Take() { return std::move(bytes_); }

private:
  [[nodiscard]] std::size_t Remaining() const noexcept {
    return bytes_.size() <= maximumBytes_ ? maximumBytes_ - bytes_.size() : 0U;
  }

  bool Byte(const std::uint8_t value) {
    if (bytes_.size() >= maximumBytes_)
      return false;
    bytes_.push_back(value);
    return true;
  }

  std::size_t maximumBytes_{0U};
  std::size_t maximumStringBytes_{0U};
  std::vector<std::uint8_t> bytes_;
};

class CanonicalByteReader final {
public:
  static constexpr bool kReading = true;

  CanonicalByteReader(const std::vector<std::uint8_t> &bytes,
                      const std::size_t maximumBytes,
                      const std::size_t maximumStringBytes)
      : bytes_(bytes), maximumStringBytes_(maximumStringBytes),
        valid_(bytes.size() <= maximumBytes) {}

  bool Boolean(bool &value) {
    std::uint8_t byte = 0U;
    if (!Byte(byte) || byte > 1U)
      return false;
    value = byte != 0U;
    return true;
  }

  bool Unsigned(std::uint64_t &value) {
    value = 0U;
    for (unsigned shift = 0U; shift != 64U; shift += 8U) {
      std::uint8_t byte = 0U;
      if (!Byte(byte))
        return false;
      value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return true;
  }

  bool Signed(std::int64_t &value) {
    std::uint64_t bits = 0U;
    if (!Unsigned(bits))
      return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
  }

  bool Float(float &value) {
    std::uint64_t encoded = 0U;
    if (!Unsigned(encoded) ||
        encoded > std::numeric_limits<std::uint32_t>::max())
      return false;
    const auto bits = static_cast<std::uint32_t>(encoded);
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
  }

  bool Double(double &value) {
    std::uint64_t bits = 0U;
    if (!Unsigned(bits))
      return false;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
  }

  bool String(std::string &value) {
    std::uint64_t size = 0U;
    if (!Unsigned(size) || size > maximumStringBytes_ || size > Remaining())
      return false;
    value.assign(reinterpret_cast<const char *>(bytes_.data() + offset_),
                 static_cast<std::size_t>(size));
    offset_ += static_cast<std::size_t>(size);
    return true;
  }

  bool Bytes(std::vector<std::uint8_t> &value) {
    std::uint64_t size = 0U;
    if (!Unsigned(size) || size > Remaining())
      return false;
    value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                 bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += static_cast<std::size_t>(size);
    return true;
  }

  bool Count(std::uint64_t &value, const std::size_t maximum) {
    return Unsigned(value) && value <= maximum;
  }

  [[nodiscard]] bool Complete() const noexcept {
    return valid_ && offset_ == bytes_.size();
  }

private:
  [[nodiscard]] std::size_t Remaining() const noexcept {
    return valid_ && offset_ <= bytes_.size() ? bytes_.size() - offset_ : 0U;
  }

  bool Byte(std::uint8_t &value) {
    if (!valid_ || offset_ >= bytes_.size())
      return false;
    value = bytes_[offset_++];
    return true;
  }

  const std::vector<std::uint8_t> &bytes_;
  std::size_t maximumStringBytes_{0U};
  std::size_t offset_{0U};
  bool valid_{false};
};

template <typename Archive, typename Enum>
bool TransferEnum(Archive &archive, Enum &value, const Enum maximum) {
  static_assert(std::is_enum_v<Enum>);
  std::uint64_t encoded = static_cast<std::uint64_t>(value);
  if (!archive.Unsigned(encoded) ||
      encoded > static_cast<std::uint64_t>(maximum))
    return false;
  if constexpr (Archive::kReading)
    value = static_cast<Enum>(encoded);
  return true;
}

template <typename Archive, typename Value, typename Transfer>
bool TransferBoundedVector(Archive &archive, std::vector<Value> &values,
                           const std::size_t maximum, Transfer &&transfer) {
  std::uint64_t count = static_cast<std::uint64_t>(values.size());
  if (!archive.Count(count, maximum))
    return false;
  if constexpr (Archive::kReading)
    values.resize(static_cast<std::size_t>(count));
  for (auto &value : values) {
    if (!transfer(value))
      return false;
  }
  return true;
}

template <typename Archive, typename Value, typename Transfer>
bool TransferOptional(Archive &archive, std::optional<Value> &value,
                      Transfer &&transfer) {
  bool present = value.has_value();
  if (!archive.Boolean(present))
    return false;
  if constexpr (Archive::kReading) {
    if (present)
      value.emplace();
    else
      value.reset();
  }
  return !present || transfer(*value);
}

} // namespace videocut::text::internal
