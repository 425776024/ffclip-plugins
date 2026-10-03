#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace videocut::text {

enum class TextStatusCode : std::uint8_t {
    Ok = 0,
    Empty,
    NotReady,
    Canceled,
    InvalidDocument,
    MissingResource,
    ChangedResource,
    UnsupportedFeature,
    BudgetExceeded,
    Failed,
};

enum class DiagnosticSeverity : std::uint8_t {
    Information = 0,
    Warning,
    Error,
};

struct Diagnostic final {
    std::string code;
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
    std::string stage;
    std::string subjectId;
    std::string message;
};

struct Color final {
    float red{0.0F};
    float green{0.0F};
    float blue{0.0F};
    float alpha{1.0F};
};

struct Rect final {
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};
};

struct Insets final {
    float left{0.0F};
    float top{0.0F};
    float right{0.0F};
    float bottom{0.0F};
};

enum class RenderQuality : std::uint8_t {
    PreviewDraft = 0,
    PreviewActive,
    PreviewPaused,
    Export,
};

/// Ordered property composition shared by static patches, automation layers
/// and template channel application.
enum class TextPropertyCombineMode : std::uint8_t {
    Replace = 0,
    Add,
    Multiply,
    MatrixConcat,
    ColorMix,
};

enum class TextInvalidationImpact : std::uint16_t {
    None = 0,
    Resources = 1U << 0U,
    Layout = 1U << 1U,
    GlyphMaterial = 1U << 2U,
    Backdrop = 1U << 3U,
    Animation = 1U << 4U,
    PostEffectState = 1U << 5U,
    Composite = 1U << 6U,
};

[[nodiscard]] constexpr TextInvalidationImpact
operator|(const TextInvalidationImpact left,
          const TextInvalidationImpact right) noexcept {
    return static_cast<TextInvalidationImpact>(
        static_cast<std::uint16_t>(left) |
        static_cast<std::uint16_t>(right));
}

constexpr TextInvalidationImpact &
operator|=(TextInvalidationImpact &left,
           const TextInvalidationImpact right) noexcept {
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool HasInvalidationImpact(
    const TextInvalidationImpact value,
    const TextInvalidationImpact expected) noexcept {
    return (static_cast<std::uint16_t>(value) &
            static_cast<std::uint16_t>(expected)) != 0U;
}

using CancelCheck = std::function<bool()>;

} // namespace videocut::text
