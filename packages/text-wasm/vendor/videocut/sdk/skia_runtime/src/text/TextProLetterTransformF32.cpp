#include "text/TextProLetterTransformF32.h"

#include <cmath>
#include <cstddef>

namespace videocut::skia_runtime::internal {
namespace {

struct QuaternionF32 final {
  float x{0.0F};
  float y{0.0F};
  float z{0.0F};
  float w{1.0F};
};

float StoreF32(const float value) noexcept {
  volatile float stored = value;
  return stored;
}

float AddF32(const float left, const float right) noexcept {
  volatile float result = StoreF32(left) + StoreF32(right);
  return result;
}

float SubF32(const float left, const float right) noexcept {
  volatile float result = StoreF32(left) - StoreF32(right);
  return result;
}

float MulF32(const float left, const float right) noexcept {
  volatile float result = StoreF32(left) * StoreF32(right);
  return result;
}

bool IsFinite(const TextProLetterVector3F32 &value) noexcept {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool IsFinite(const TextProLetterMatrixF32 &value) noexcept {
  for (const float component : value.values) {
    if (!std::isfinite(component))
      return false;
  }
  return true;
}

void SinCosHalfF32(const float angle, float &sine, float &cosine) noexcept {
  const float half = MulF32(angle, 0.5F);
#if defined(__APPLE__)
  ::__sincosf(half, &sine, &cosine);
#else
  sine = std::sin(half);
  cosine = std::cos(half);
#endif
  sine = StoreF32(sine);
  cosine = StoreF32(cosine);
}

QuaternionF32 MultiplyQuaternionF32(const QuaternionF32 &left,
                                    const QuaternionF32 &right) noexcept {
  QuaternionF32 result;
  result.x = SubF32(
      AddF32(AddF32(MulF32(left.w, right.x), MulF32(left.x, right.w)),
             MulF32(left.y, right.z)),
      MulF32(left.z, right.y));
  result.y = SubF32(
      AddF32(AddF32(MulF32(left.w, right.y), MulF32(left.y, right.w)),
             MulF32(left.z, right.x)),
      MulF32(left.x, right.z));
  result.z = SubF32(
      AddF32(AddF32(MulF32(left.w, right.z), MulF32(left.z, right.w)),
             MulF32(left.x, right.y)),
      MulF32(left.y, right.x));
  result.w = SubF32(
      SubF32(SubF32(MulF32(left.w, right.w), MulF32(left.x, right.x)),
             MulF32(left.y, right.y)),
      MulF32(left.z, right.z));
  return result;
}

QuaternionF32 EulerToQuaternionF32(
    const TextProLetterVector3F32 &rotation) noexcept {
  float sinX = 0.0F;
  float cosX = 1.0F;
  float sinY = 0.0F;
  float cosY = 1.0F;
  float sinZ = 0.0F;
  float cosZ = 1.0F;
  SinCosHalfF32(rotation.x, sinX, cosX);
  SinCosHalfF32(rotation.y, sinY, cosY);
  SinCosHalfF32(rotation.z, sinZ, cosZ);
  const QuaternionF32 qX{sinX, 0.0F, 0.0F, cosX};
  const QuaternionF32 qY{0.0F, sinY, 0.0F, cosY};
  const QuaternionF32 qZ{0.0F, 0.0F, sinZ, cosZ};
  return MultiplyQuaternionF32(MultiplyQuaternionF32(qY, qX), qZ);
}

TextProLetterMatrixF32 IdentityF32() noexcept {
  TextProLetterMatrixF32 result;
  result.values[0] = 1.0F;
  result.values[5] = 1.0F;
  result.values[10] = 1.0F;
  result.values[15] = 1.0F;
  return result;
}

TextProLetterMatrixF32 TranslationF32(
    const TextProLetterVector3F32 &translation) noexcept {
  auto result = IdentityF32();
  result.values[12] = StoreF32(translation.x);
  result.values[13] = StoreF32(translation.y);
  result.values[14] = StoreF32(translation.z);
  return result;
}

TextProLetterMatrixF32 SetTrsF32(
    const TextProLetterVector3F32 &translation,
    const QuaternionF32 &rotation,
    const TextProLetterVector3F32 &scale) noexcept {
  const float twoX = AddF32(rotation.x, rotation.x);
  const float twoY = AddF32(rotation.y, rotation.y);
  const float twoZ = AddF32(rotation.z, rotation.z);
  const float xx = MulF32(rotation.x, twoX);
  const float yy = MulF32(rotation.y, twoY);
  const float zz = MulF32(rotation.z, twoZ);
  const float xy = MulF32(rotation.x, twoY);
  const float xz = MulF32(rotation.x, twoZ);
  const float yz = MulF32(rotation.y, twoZ);
  const float wx = MulF32(twoX, rotation.w);
  const float wy = MulF32(twoY, rotation.w);
  const float wz = MulF32(rotation.w, twoZ);

  auto result = IdentityF32();
  result.values[0] = SubF32(1.0F, AddF32(yy, zz));
  result.values[1] = AddF32(xy, wz);
  result.values[2] = SubF32(xz, wy);
  result.values[4] = SubF32(xy, wz);
  result.values[5] = SubF32(1.0F, AddF32(xx, zz));
  result.values[6] = AddF32(yz, wx);
  result.values[8] = AddF32(xz, wy);
  result.values[9] = SubF32(yz, wx);
  result.values[10] = SubF32(1.0F, AddF32(xx, yy));

  for (std::size_t row = 0U; row < 3U; ++row) {
    result.values[row] = MulF32(result.values[row], scale.x);
    result.values[4U + row] = MulF32(result.values[4U + row], scale.y);
    result.values[8U + row] = MulF32(result.values[8U + row], scale.z);
  }
  result.values[12] = StoreF32(translation.x);
  result.values[13] = StoreF32(translation.y);
  result.values[14] = StoreF32(translation.z);
  return result;
}

} // namespace

std::optional<TextProLetterMatrixF32>
ConcatTextProLetterTransformF32(const TextProLetterMatrixF32 &left,
                                const TextProLetterMatrixF32 &right) noexcept {
  if (!IsFinite(left) || !IsFinite(right))
    return std::nullopt;
  TextProLetterMatrixF32 result;
  for (std::size_t row = 0U; row < 4U; ++row) {
    for (std::size_t column = 0U; column < 4U; ++column) {
      float value = MulF32(left.values[row], right.values[column * 4U]);
      value = AddF32(
          value,
          MulF32(left.values[4U + row], right.values[column * 4U + 1U]));
      value = AddF32(
          value,
          MulF32(left.values[8U + row], right.values[column * 4U + 2U]));
      value = AddF32(
          value,
          MulF32(left.values[12U + row], right.values[column * 4U + 3U]));
      result.values[column * 4U + row] = value;
    }
  }
  return IsFinite(result) ? std::optional{result} : std::nullopt;
}

std::optional<TextProLetterMatrixF32>
BuildTextProLetterTransformF32(
    const TextProLetterTransformInputF32 &input) noexcept {
  if (!IsFinite(input.translation) || !IsFinite(input.rotationRadians) ||
      !IsFinite(input.scale) || !IsFinite(input.pivot)) {
    return std::nullopt;
  }
  const auto trs = SetTrsF32(input.translation,
                             EulerToQuaternionF32(input.rotationRadians),
                             input.scale);
  const auto pre = TranslationF32(input.pivot);
  const TextProLetterVector3F32 negativePivot{
      StoreF32(-input.pivot.x), StoreF32(-input.pivot.y),
      StoreF32(-input.pivot.z)};
  const auto post = TranslationF32(negativePivot);
  const auto preTrs = ConcatTextProLetterTransformF32(pre, trs);
  if (!preTrs)
    return std::nullopt;
  return ConcatTextProLetterTransformF32(*preTrs, post);
}

std::optional<TextProLetterVertexF32>
TransformTextProLetterPointF32(const TextProLetterMatrixF32 &transform,
                               const TextProLetterVector3F32 &point) noexcept {
  if (!IsFinite(transform) || !IsFinite(point) ||
      transform.values[3] != 0.0F || transform.values[7] != 0.0F ||
      transform.values[11] != 0.0F || transform.values[15] != 1.0F) {
    return std::nullopt;
  }
  const auto component = [&](const std::size_t row) noexcept {
    float value = MulF32(transform.values[row], point.x);
    value = AddF32(value, MulF32(transform.values[4U + row], point.y));
    value = AddF32(value, MulF32(transform.values[8U + row], point.z));
    return AddF32(value, transform.values[12U + row]);
  };
  TextProLetterVertexF32 result{component(0U), component(1U), component(2U)};
  return IsFinite(TextProLetterVector3F32{result.x, result.y, result.z})
             ? std::optional{result}
             : std::nullopt;
}

} // namespace videocut::skia_runtime::internal
