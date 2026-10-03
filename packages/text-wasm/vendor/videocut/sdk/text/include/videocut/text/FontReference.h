#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace videocut::text {

enum class FontSourceKind : std::uint8_t {
    Builtin = 0,
    ProjectManaged,
    System,
};

enum class FontSlant : std::uint8_t {
    Upright = 0,
    Italic,
    Oblique,
};

struct FontAxis final {
    std::string tag;
    float value{0.0F};
};

struct FontAxisRange final {
    std::string tag;
    float minimum{0.0F};
    float defaultValue{0.0F};
    float maximum{0.0F};
};

/// Portable persisted font identity. Imported files are admitted into a
/// project-managed asset before this value is authored.
struct FontReference final {
    FontSourceKind kind{FontSourceKind::Builtin};
    std::string family;
    std::string postscriptName;
    std::int32_t weight{400};
    std::int32_t width{5};
    FontSlant slant{FontSlant::Upright};
    std::uint32_t faceIndex{0};
    std::vector<FontAxis> variationAxes;

    std::string assetId;
    std::string platform;
    std::string digest;
    std::string faceFingerprint;

    bool allowSystemGlyphFallback{false};
};

} // namespace videocut::text
