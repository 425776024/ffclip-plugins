#pragma once

#include <array>
#include <optional>

namespace videocut::skia_runtime::internal {

struct TextProLetterVector3F32 final {
  float x{0.0F};
  float y{0.0F};
  float z{0.0F};
};

struct TextProLetterTransformInputF32 final {
  TextProLetterVector3F32 translation{};
  TextProLetterVector3F32 rotationRadians{};
  TextProLetterVector3F32 scale{1.0F, 1.0F, 1.0F};
  TextProLetterVector3F32 pivot{};
};

/// Column-major 4x4 binary32 matrix, matching AmazingEngine::Matrix4x4f.
struct TextProLetterMatrixF32 final {
  std::array<float, 16> values{};
};

struct TextProLetterVertexF32 final {
  float x{0.0F};
  float y{0.0F};
  float z{0.0F};
};

/// Builds T(pivot) * SetTRS(translation, Euler(rotation), scale) * T(-pivot).
/// Euler composition follows the recovered TextPro order (qY * qX) * qZ.
std::optional<TextProLetterMatrixF32>
BuildTextProLetterTransformF32(
    const TextProLetterTransformInputF32 &input) noexcept;

/// Returns left * right with one binary32 rounding after every multiply and
/// every left-associated add.
std::optional<TextProLetterMatrixF32>
ConcatTextProLetterTransformF32(const TextProLetterMatrixF32 &left,
                                const TextProLetterMatrixF32 &right) noexcept;

/// Applies the recovered TextPro affine point ABI. Matrices that require a
/// perspective divide are rejected because the captured CPU Letter packet
/// path has no such ABI; perspective division occurs later in Metal.
std::optional<TextProLetterVertexF32>
TransformTextProLetterPointF32(const TextProLetterMatrixF32 &transform,
                               const TextProLetterVector3F32 &point) noexcept;

} // namespace videocut::skia_runtime::internal
