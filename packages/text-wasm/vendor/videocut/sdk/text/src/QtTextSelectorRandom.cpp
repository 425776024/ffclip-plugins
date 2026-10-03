#include "QtTextSelectorRandom.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace videocut::text {
namespace {

constexpr std::size_t kWidth = 256;
constexpr std::uint32_t kMask = kWidth - 1;
constexpr double kStartDenominator = 281474976710656.0; // 256^6
constexpr double kSignificance = 4503599627370496.0;    // 2^52
constexpr double kOverflow = 9007199254740992.0;        // 2^53

std::u16string AsciiToUtf16(const std::string &value) {
  std::u16string result;
  result.reserve(value.size());
  for (const unsigned char byte : value)
    result.push_back(static_cast<char16_t>(byte));
  return result;
}

std::vector<std::uint8_t> MixKey(const std::u16string_view seed) {
  std::vector<std::uint8_t> key;
  std::int32_t smear = 0;
  std::size_t index = 0;
  for (const char16_t codeUnit : seed) {
    const std::size_t keyIndex = index & kMask;
    if (keyIndex >= key.size())
      key.resize(keyIndex + 1, 0);

    // JavaScript bitwise operators first apply ToInt32. An unassigned key slot
    // therefore contributes zero, exactly as the embedded mixkey function.
    smear ^= static_cast<std::int32_t>(key[keyIndex]) * 19;
    key[keyIndex] =
        static_cast<std::uint8_t>((static_cast<std::uint32_t>(smear) +
                                   static_cast<std::uint32_t>(codeUnit)) &
                                  kMask);
    ++index;
  }
  return key;
}

bool IsLineBreak(const std::vector<bool> &mask, const std::size_t index) {
  return index < mask.size() && mask[index];
}

void EnsureIndex(QtTextSelectorRandomMap &map, const std::size_t index) {
  if (index >= map.size())
    map.resize(index + 1);
}

int ParseExponent(const std::string_view representation,
                  const std::size_t exponentOffset) {
  if (exponentOffset == std::string_view::npos)
    return 0;
  std::size_t cursor = exponentOffset + 1;
  bool negative = false;
  if (cursor < representation.size() &&
      (representation[cursor] == '+' || representation[cursor] == '-')) {
    negative = representation[cursor] == '-';
    ++cursor;
  }
  int value = 0;
  for (; cursor < representation.size(); ++cursor) {
    const char digit = representation[cursor];
    if (digit < '0' || digit > '9')
      throw std::runtime_error("invalid floating-point exponent");
    value = value * 10 + (digit - '0');
  }
  return negative ? -value : value;
}

} // namespace

std::string QtTextSelectorJsNumberToString(const double value) {
  if (std::isnan(value))
    return "NaN";
  if (std::isinf(value))
    return std::signbit(value) ? "-Infinity" : "Infinity";
  if (value == 0.0)
    return "0";

  char buffer[128]{};
  const auto converted = std::to_chars(std::begin(buffer), std::end(buffer),
                                       value, std::chars_format::general);
  if (converted.ec != std::errc{})
    throw std::runtime_error("could not convert selector seed to decimal");

  std::string representation(buffer, converted.ptr);
  const bool negative = representation.front() == '-';
  const std::size_t mantissaOffset = negative ? 1U : 0U;
  const std::size_t exponentOffset = representation.find_first_of("eE");
  const std::size_t mantissaEnd = exponentOffset == std::string::npos
                                      ? representation.size()
                                      : exponentOffset;
  const std::size_t pointOffset = representation.find('.', mantissaOffset);
  const std::size_t decimalPosition =
      pointOffset != std::string::npos && pointOffset < mantissaEnd
          ? pointOffset - mantissaOffset
          : mantissaEnd - mantissaOffset;

  std::string digits;
  digits.reserve(mantissaEnd - mantissaOffset);
  for (std::size_t index = mantissaOffset; index < mantissaEnd; ++index) {
    if (representation[index] != '.')
      digits.push_back(representation[index]);
  }

  const std::size_t leadingZeroCount = static_cast<std::size_t>(
      std::find_if(digits.begin(), digits.end(),
                   [](const char digit) { return digit != '0'; }) -
      digits.begin());
  digits.erase(0, leadingZeroCount);
  if (digits.empty())
    return "0";

  const int exponent = ParseExponent(representation, exponentOffset);
  const int decimalIndex = static_cast<int>(decimalPosition) + exponent -
                           static_cast<int>(leadingZeroCount);
  const int digitCount = static_cast<int>(digits.size());

  std::string result;
  if (negative)
    result.push_back('-');

  // ECMAScript uses fixed notation for decimal positions (-5 .. 21), unlike
  // std::to_chars' %g-like presentation threshold. The significant digits are
  // already the shortest round-tripping sequence; only notation is changed.
  if (decimalIndex > 0 && decimalIndex <= 21) {
    if (decimalIndex >= digitCount) {
      result += digits;
      result.append(static_cast<std::size_t>(decimalIndex - digitCount), '0');
    } else {
      result.append(digits, 0, static_cast<std::size_t>(decimalIndex));
      result.push_back('.');
      result.append(digits, static_cast<std::size_t>(decimalIndex),
                    std::string::npos);
    }
    return result;
  }
  if (decimalIndex <= 0 && decimalIndex > -6) {
    result += "0.";
    result.append(static_cast<std::size_t>(-decimalIndex), '0');
    result += digits;
    return result;
  }

  result.push_back(digits.front());
  if (digits.size() > 1) {
    result.push_back('.');
    result.append(digits, 1, std::string::npos);
  }
  result.push_back('e');
  const int scientificExponent = decimalIndex - 1;
  if (scientificExponent >= 0)
    result.push_back('+');
  result += std::to_string(scientificExponent);
  return result;
}

QtTextSelectorSeedRandom::QtTextSelectorSeedRandom(const double numberSeed)
    : QtTextSelectorSeedRandom(
          AsciiToUtf16(QtTextSelectorJsNumberToString(numberSeed))) {}

QtTextSelectorSeedRandom::QtTextSelectorSeedRandom(
    const std::u16string_view seedString) {
  std::vector<std::uint8_t> key = MixKey(seedString);
  if (key.empty())
    key.push_back(0);

  for (std::size_t index = 0; index < state_.size(); ++index)
    state_[index] = static_cast<std::uint8_t>(index);

  std::uint32_t stateIndex = 0;
  for (std::size_t index = 0; index < state_.size(); ++index) {
    const std::uint8_t value = state_[index];
    stateIndex = (stateIndex + key[index % key.size()] + value) & kMask;
    state_[index] = state_[stateIndex];
    state_[stateIndex] = value;
  }

  // ARC4's constructor invokes g(width), discarding the first 256 bytes.
  Discard(kWidth);
}

std::uint64_t
QtTextSelectorSeedRandom::Generate(const std::size_t count) noexcept {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < count; ++index) {
    i_ = static_cast<std::uint8_t>(i_ + 1U);
    const std::uint8_t value = state_[i_];
    j_ = static_cast<std::uint8_t>(j_ + value);
    state_[i_] = state_[j_];
    state_[j_] = value;
    const std::uint8_t output =
        state_[static_cast<std::uint8_t>(state_[i_] + state_[j_])];
    result = result * kWidth + output;
  }
  return result;
}

void QtTextSelectorSeedRandom::Discard(const std::size_t count) noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    i_ = static_cast<std::uint8_t>(i_ + 1U);
    const std::uint8_t value = state_[i_];
    j_ = static_cast<std::uint8_t>(j_ + value);
    state_[i_] = state_[j_];
    state_[j_] = value;
  }
}

double QtTextSelectorSeedRandom::Next() noexcept {
  double numerator = static_cast<double>(Generate(6));
  double denominator = kStartDenominator;
  double extra = 0.0;
  while (numerator < kSignificance) {
    numerator = (numerator + extra) * static_cast<double>(kWidth);
    denominator *= static_cast<double>(kWidth);
    extra = static_cast<double>(Generate(1));
  }
  while (numerator >= kOverflow) {
    numerator /= 2.0;
    denominator /= 2.0;
    extra = static_cast<double>(static_cast<std::uint32_t>(extra) >> 1U);
  }
  return (numerator + extra) / denominator;
}

QtTextSelectorRandomMap BuildQtTextSelectorRandomMap(
    const double numberSeed,
    const std::vector<QtTextSelectorIndexRange> &ranges,
    const QtTextSelectorRandomBasis basedOn,
    const std::vector<bool> &lineBreakMask) {
  std::size_t initialSize = 0;
  for (const auto &range : ranges) {
    if (range.startIndex > range.endIndex)
      throw std::invalid_argument("selector range start exceeds end");
    initialSize = std::max(initialSize, range.endIndex);
    if (range.endIndex > range.startIndex &&
        range.startIndex >
            std::numeric_limits<std::size_t>::max() - (range.endIndex - 1U)) {
      throw std::overflow_error("selector range swap index overflows");
    }
  }

  QtTextSelectorRandomMap randomMap(initialSize);
  QtTextSelectorSeedRandom random(numberSeed);
  const bool letterMode = basedOn == QtTextSelectorRandomBasis::Letter;

  for (const auto &range : ranges) {
    for (std::size_t index = range.startIndex; index < range.endIndex;
         ++index) {
      if (!letterMode || !IsLineBreak(lineBreakMask, index))
        randomMap[index] = index;
    }

    const std::size_t count = range.endIndex - range.startIndex;
    for (std::size_t relativeIndex = count; relativeIndex-- > 1U;) {
      const double randomValue = random.Next();
      const std::size_t randomRelativeIndex = static_cast<std::size_t>(
          std::floor(randomValue * static_cast<double>(relativeIndex + 1U)));
      const std::size_t randomAbsoluteIndex =
          randomRelativeIndex + range.startIndex;
      if (letterMode && IsLineBreak(lineBreakMask, randomAbsoluteIndex)) {
        continue;
      }

      const std::size_t leftIndex = range.startIndex + relativeIndex;
      // This second +startIndex is intentional. It is present in the shipped
      // JavaScript after j has already been converted to an absolute index.
      const std::size_t rightIndex = range.startIndex + randomAbsoluteIndex;
      EnsureIndex(randomMap, rightIndex);
      if (leftIndex != rightIndex)
        std::swap(randomMap[leftIndex], randomMap[rightIndex]);
    }
  }
  return randomMap;
}

} // namespace videocut::text
