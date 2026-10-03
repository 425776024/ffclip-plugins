#pragma once

#include "videocut/text/TextLayerAppearance.h"
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace videocut::skia_runtime::internal {

class IdentityBuilder final {
public:
  template <typename Value> void Add(const Value &value) {
    if constexpr (std::is_enum_v<Value>) {
      using Underlying = std::underlying_type_t<Value>;
      Add(static_cast<Underlying>(value));
    } else if constexpr (std::is_arithmetic_v<Value>) {
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
      for (std::size_t index = 0U; index < sizeof(Value); ++index) {
        hash_ ^= bytes[index];
        hash_ *= 1099511628211ULL;
      }
      hash_ ^= 0xfeU;
      hash_ *= 1099511628211ULL;
    } else {
      std::ostringstream stream;
      stream << value;
      AddString(stream.str());
    }
  }

  void AddString(const std::string_view value) {
    for (const auto character : value) {
      hash_ ^= static_cast<std::uint8_t>(character);
      hash_ *= 1099511628211ULL;
    }
    hash_ ^= 0xffU;
    hash_ *= 1099511628211ULL;
  }

  void AddColor(const text::Color &color) {
    Add(color.red);
    Add(color.green);
    Add(color.blue);
    Add(color.alpha);
  }

  [[nodiscard]] std::string Finish() const {
    std::ostringstream stream;
    stream << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
           << hash_;
    return stream.str();
  }

  [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
  std::uint64_t hash_{1469598103934665603ULL};
};

} // namespace videocut::skia_runtime::internal
