#include "videocut/vector/VectorCustomization.h"

#include "videocut/vector/VectorDigest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <exception>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace videocut::vector {
namespace {

using Json = nlohmann::json;

constexpr const char* kSvgSelectorPrefix = "svg:";
constexpr const char* kJsonSelectorPrefix = "json:";
constexpr const char* kJsonGradientSelectorPrefix = "json-gradient:";

std::string StableFieldIdentity(
    const char* prefix,
    const std::string& identity)
{
    const std::string digest = Sha256Digest(
        reinterpret_cast<const std::uint8_t*>(identity.data()),
        identity.size());
    return std::string(prefix) + "." + digest.substr(7, 16);
}

std::string AnimationGroupIdentity(const std::string& identity)
{
    return StableFieldIdentity("vector", identity);
}

const char* FieldRoleIdentity(const VectorFieldRole role) noexcept
{
    switch (role)
    {
    case VectorFieldRole::Fill: return "fill";
    case VectorFieldRole::Stroke: return "stroke";
    case VectorFieldRole::Text: return "text";
    case VectorFieldRole::Scalar: return "number";
    case VectorFieldRole::GradientStop: return "gradient_stop";
    case VectorFieldRole::Font: return "font";
    case VectorFieldRole::FillOpacity: return "fill_opacity";
    case VectorFieldRole::StrokeWidth: return "stroke_width";
    case VectorFieldRole::StrokeOpacity: return "stroke_opacity";
    case VectorFieldRole::CenterlineColor: return "centerline_color";
    case VectorFieldRole::CenterlineWidth: return "centerline_width";
    case VectorFieldRole::CenterlineOpacity: return "centerline_opacity";
    case VectorFieldRole::Roundness: return "roundness";
    case VectorFieldRole::ShadowColor: return "shadow_color";
    case VectorFieldRole::ShadowOpacity: return "shadow_opacity";
    case VectorFieldRole::ShadowDistance: return "shadow_distance";
    case VectorFieldRole::ShadowAngle: return "shadow_angle";
    case VectorFieldRole::GradientOpacity: return "gradient_opacity";
    case VectorFieldRole::GradientPosition: return "gradient_position";
    case VectorFieldRole::GradientAngle: return "gradient_angle";
    case VectorFieldRole::TextureOpacity: return "texture_opacity";
    case VectorFieldRole::SizeX: return "size_x";
    case VectorFieldRole::SizeY: return "size_y";
    case VectorFieldRole::GlobalOpacity: return "global_opacity";
    }
    return "field";
}

bool IsFiniteUnit(const float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

bool ValidColor(const VectorColor& color) noexcept
{
    return IsFiniteUnit(color.red) && IsFiniteUnit(color.green)
        && IsFiniteUnit(color.blue) && IsFiniteUnit(color.alpha);
}

bool ValidUtf8(const std::string& value) noexcept
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0;
    while (index < value.size())
    {
        const unsigned char first = bytes[index++];
        if (first <= 0x7fU)
            continue;
        std::uint32_t codePoint = 0;
        std::size_t continuation = 0;
        if (first >= 0xc2U && first <= 0xdfU)
        {
            codePoint = first & 0x1fU;
            continuation = 1;
        }
        else if (first >= 0xe0U && first <= 0xefU)
        {
            codePoint = first & 0x0fU;
            continuation = 2;
        }
        else if (first >= 0xf0U && first <= 0xf4U)
        {
            codePoint = first & 0x07U;
            continuation = 3;
        }
        else
            return false;
        if (continuation > value.size() - index)
            return false;
        for (std::size_t offset = 0; offset < continuation; ++offset)
        {
            const unsigned char next = bytes[index++];
            if ((next & 0xc0U) != 0x80U)
                return false;
            codePoint = (codePoint << 6U) | (next & 0x3fU);
        }
        if ((continuation == 1 && codePoint < 0x80U)
            || (continuation == 2 && codePoint < 0x800U)
            || (continuation == 3 && codePoint < 0x10000U)
            || codePoint > 0x10ffffU
            || (codePoint >= 0xd800U && codePoint <= 0xdfffU))
            return false;
    }
    return true;
}

bool ValidFontFamily(
    const std::string& value,
    const VectorLimits& limits) noexcept
{
    return !value.empty() && value.size() <= limits.maximumFontFamilyBytes
        && ValidUtf8(value)
        && std::none_of(value.begin(), value.end(), [](unsigned char character) {
               return character < 0x20U || character == 0x7fU;
           });
}

bool ValidFieldId(const std::string& value, const VectorLimits& limits) noexcept
{
    if (value.empty() || value.size() > limits.maximumFieldIdBytes)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_' || character == '-';
    });
}

bool ValidValue(
    const VectorEditableField& field,
    const VectorFieldValue& value,
    const VectorLimits& limits) noexcept
{
    if (value.kind != field.kind)
        return false;
    switch (field.kind)
    {
    case VectorFieldKind::Color:
        return ValidColor(value.color);
    case VectorFieldKind::Text:
        if (field.role == VectorFieldRole::Font)
            return ValidFontFamily(value.text, limits);
        return value.text.size() <= limits.maximumEditableTextBytes
            && ValidUtf8(value.text);
    case VectorFieldKind::Number:
        if (!std::isfinite(value.number))
            return false;
        if (field.minimum && value.number < *field.minimum)
            return false;
        if (field.maximum && value.number > *field.maximum)
            return false;
        return true;
    }
    return false;
}

bool ValidField(
    const VectorEditableField& field,
    const VectorLimits& limits) noexcept
{
    const bool colorRole =
        field.role == VectorFieldRole::Fill
        || field.role == VectorFieldRole::Stroke
        || field.role == VectorFieldRole::GradientStop
        || field.role == VectorFieldRole::CenterlineColor
        || field.role == VectorFieldRole::ShadowColor;
    const bool numberRole =
        field.role == VectorFieldRole::Scalar
        || field.role == VectorFieldRole::FillOpacity
        || field.role == VectorFieldRole::StrokeWidth
        || field.role == VectorFieldRole::StrokeOpacity
        || field.role == VectorFieldRole::CenterlineWidth
        || field.role == VectorFieldRole::CenterlineOpacity
        || field.role == VectorFieldRole::Roundness
        || field.role == VectorFieldRole::ShadowOpacity
        || field.role == VectorFieldRole::ShadowDistance
        || field.role == VectorFieldRole::ShadowAngle
        || field.role == VectorFieldRole::GradientOpacity
        || field.role == VectorFieldRole::GradientPosition
        || field.role == VectorFieldRole::GradientAngle
        || field.role == VectorFieldRole::TextureOpacity
        || field.role == VectorFieldRole::SizeX
        || field.role == VectorFieldRole::SizeY
        || field.role == VectorFieldRole::GlobalOpacity;
    const bool roleMatchesKind =
        (colorRole && field.kind == VectorFieldKind::Color)
        || (field.role == VectorFieldRole::Text
            && field.kind == VectorFieldKind::Text)
        || (numberRole && field.kind == VectorFieldKind::Number)
        || (field.role == VectorFieldRole::Font
            && field.kind == VectorFieldKind::Text);
    if (!ValidFieldId(field.id, limits)
        || !roleMatchesKind
        || field.label.size() > limits.maximumFieldLabelBytes
        || field.targets.empty()
        || field.targets.size() > limits.maximumFieldTargets
        || field.defaultValue.kind != field.kind
        || !ValidValue(field, field.defaultValue, limits))
        return false;
    if ((field.minimum && !std::isfinite(*field.minimum))
        || (field.maximum && !std::isfinite(*field.maximum))
        || (field.step && (!std::isfinite(*field.step) || *field.step <= 0.0))
        || (field.minimum && field.maximum && *field.minimum > *field.maximum))
        return false;
    std::unordered_set<std::uint8_t> components;
    for (const auto component : field.animatableComponents)
    {
        const bool kindMatches =
            (component == VectorFieldAnimatableComponent::Number
             && field.kind == VectorFieldKind::Number)
            || ((component == VectorFieldAnimatableComponent::Color
                 || component == VectorFieldAnimatableComponent::Alpha)
                && field.kind == VectorFieldKind::Color);
        if (!kindMatches
            || !components.insert(static_cast<std::uint8_t>(component)).second)
            return false;
    }
    if (field.animationGroupIdentity.size() > limits.maximumFieldIdBytes
        || (field.animatableComponents.empty()
            != field.animationGroupIdentity.empty())
        || (field.kind == VectorFieldKind::Text
            && (!field.animatableComponents.empty()
                || field.unit != VectorFieldUnit::Unitless))
        || (field.kind == VectorFieldKind::Color
            && field.unit != VectorFieldUnit::Unitless))
        return false;
    return std::all_of(
        field.targets.begin(), field.targets.end(), [&](const auto& target) {
            return !target.selector.empty()
                && target.selector.size() <= limits.maximumSelectorBytes
                && (target.selector.compare(0, 4, kSvgSelectorPrefix) == 0
                    || target.selector.compare(0, 5, kJsonSelectorPrefix) == 0
                    || target.selector.compare(
                           0, std::char_traits<char>::length(
                                  kJsonGradientSelectorPrefix),
                           kJsonGradientSelectorPrefix) == 0);
        });
}

std::uint8_t ColorByte(const float value) noexcept
{
    return static_cast<std::uint8_t>(
        std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

std::string ColorHex(const VectorColor& color, const bool alpha)
{
    std::ostringstream stream;
    stream << '#' << std::uppercase << std::hex << std::setfill('0')
           << std::setw(2) << static_cast<int>(ColorByte(color.red))
           << std::setw(2) << static_cast<int>(ColorByte(color.green))
           << std::setw(2) << static_cast<int>(ColorByte(color.blue));
    if (alpha || ColorByte(color.alpha) != 255)
        stream << std::setw(2) << static_cast<int>(ColorByte(color.alpha));
    return stream.str();
}

std::string LowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(
                character >= 'A' && character <= 'Z'
                    ? character + ('a' - 'A') : character);
        });
    return value;
}

std::string TrimAscii(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseHexColor(const std::string& source, VectorColor& color)
{
    const std::string value = TrimAscii(source);
    if (value.empty() || value.front() != '#')
        return false;
    const auto nibble = [](const char character, std::uint8_t& output) {
        if (character >= '0' && character <= '9')
            output = static_cast<std::uint8_t>(character - '0');
        else if (character >= 'a' && character <= 'f')
            output = static_cast<std::uint8_t>(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F')
            output = static_cast<std::uint8_t>(character - 'A' + 10);
        else
            return false;
        return true;
    };
    const auto byte = [&](const char first, const char second,
                          std::uint8_t& output) {
        std::uint8_t high = 0;
        std::uint8_t low = 0;
        if (!nibble(first, high) || !nibble(second, low))
            return false;
        output = static_cast<std::uint8_t>((high << 4U) | low);
        return true;
    };
    std::array<std::uint8_t, 4> components{0, 0, 0, 255};
    if (value.size() == 4 || value.size() == 5)
    {
        for (std::size_t index = 0; index < value.size() - 1; ++index)
        {
            std::uint8_t component = 0;
            if (!nibble(value[index + 1], component))
                return false;
            components[index] = static_cast<std::uint8_t>(component * 17U);
        }
    }
    else if (value.size() == 7 || value.size() == 9)
    {
        for (std::size_t index = 0; index < (value.size() - 1) / 2; ++index)
            if (!byte(value[index * 2 + 1], value[index * 2 + 2], components[index]))
                return false;
    }
    else
        return false;
    color = {
        components[0] / 255.0F,
        components[1] / 255.0F,
        components[2] / 255.0F,
        components[3] / 255.0F,
    };
    return true;
}

std::string JsonPointerEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        if (character == '~')
            escaped.append("~0");
        else if (character == '/')
            escaped.append("~1");
        else
            escaped.push_back(character);
    }
    return escaped;
}

bool ColorFromJsonArray(const Json& value, VectorColor& color)
{
    if (!value.is_array() || value.size() < 3 || value.size() > 8)
        return false;
    std::array<double, 4> components{0.0, 0.0, 0.0, 1.0};
    for (std::size_t index = 0; index < std::min<std::size_t>(4, value.size()); ++index)
    {
        if (!value[index].is_number())
            return false;
        components[index] = value[index].get<double>();
        if (!std::isfinite(components[index]))
            return false;
    }
    const double maximum = *std::max_element(components.begin(), components.begin() + 3);
    const double scale = maximum > 1.0 && maximum <= 255.0 ? 255.0 : 1.0;
    color = {
        static_cast<float>(std::clamp(components[0] / scale, 0.0, 1.0)),
        static_cast<float>(std::clamp(components[1] / scale, 0.0, 1.0)),
        static_cast<float>(std::clamp(components[2] / scale, 0.0, 1.0)),
        static_cast<float>(std::clamp(components[3], 0.0, 1.0)),
    };
    return ValidColor(color);
}

bool RepresentativeLottieColor(const Json& value, VectorColor& color)
{
    if (ColorFromJsonArray(value, color))
        return true;
    if (value.is_object())
    {
        const auto start = value.find("s");
        if (start != value.end() && RepresentativeLottieColor(*start, color))
            return true;
        const auto end = value.find("e");
        if (end != value.end() && RepresentativeLottieColor(*end, color))
            return true;
        const auto key = value.find("k");
        if (key != value.end() && RepresentativeLottieColor(*key, color))
            return true;
    }
    if (value.is_array())
        for (const auto& item : value)
            if (RepresentativeLottieColor(item, color))
                return true;
    return false;
}

void AddColorField(
    std::vector<VectorEditableField>& fields,
    std::unordered_map<std::string, std::size_t>& colorFields,
    const VectorFieldRole role,
    const VectorColor& color,
    std::string label,
    std::string selector,
    const std::string& groupIdentity,
    const VectorLimits& limits)
{
    const std::string key = std::string(FieldRoleIdentity(role)) + ':'
        + groupIdentity + ':' + ColorHex(color, true);
    const auto existing = colorFields.find(key);
    if (existing != colorFields.end())
    {
        auto& field = fields[existing->second];
        if (field.targets.size() < limits.maximumFieldTargets)
            field.targets.push_back({std::move(selector)});
        return;
    }
    if (fields.size() >= limits.maximumEditableFields)
        return;
    VectorEditableField field;
    field.id = StableFieldIdentity(FieldRoleIdentity(role), key);
    field.label = std::move(label);
    field.kind = VectorFieldKind::Color;
    field.role = role;
    field.defaultValue.kind = VectorFieldKind::Color;
    field.defaultValue.color = color;
    field.targets.push_back({std::move(selector)});
    field.animatableComponents = {
        VectorFieldAnimatableComponent::Color,
        VectorFieldAnimatableComponent::Alpha,
    };
    field.unit = VectorFieldUnit::Unitless;
    field.animationGroupIdentity = AnimationGroupIdentity(groupIdentity);
    colorFields.emplace(key, fields.size());
    fields.push_back(std::move(field));
}

void AddFontField(
    std::vector<VectorEditableField>& fields,
    std::string defaultValue,
    std::string label,
    std::string selector,
    const VectorLimits& limits)
{
    if (fields.size() >= limits.maximumEditableFields)
        return;
    const std::string digest = Sha256Digest(
        reinterpret_cast<const std::uint8_t*>(selector.data()),
        selector.size());
    VectorEditableField field;
    field.id = "font." + digest.substr(7, 16);
    field.label = std::move(label);
    field.kind = VectorFieldKind::Text;
    field.role = VectorFieldRole::Font;
    field.defaultValue.kind = VectorFieldKind::Text;
    field.defaultValue.text = std::move(defaultValue);
    field.targets.push_back({std::move(selector)});
    fields.push_back(std::move(field));
}

void AddNumberField(
    std::vector<VectorEditableField>& fields,
    const double defaultValue,
    std::string label,
    std::string selector,
    const VectorFieldRole role,
    const VectorFieldUnit unit,
    const std::string& groupIdentity,
    const double minimum,
    const double maximum,
    const double step,
    const VectorLimits& limits)
{
    if (fields.size() >= limits.maximumEditableFields
        || !std::isfinite(defaultValue) || defaultValue < minimum
        || defaultValue > maximum)
        return;
    VectorEditableField field;
    field.id = StableFieldIdentity(
        FieldRoleIdentity(role), groupIdentity + ':' + selector);
    field.label = std::move(label);
    field.kind = VectorFieldKind::Number;
    field.role = role;
    field.defaultValue.kind = VectorFieldKind::Number;
    field.defaultValue.number = defaultValue;
    field.minimum = minimum;
    field.maximum = maximum;
    field.step = step;
    field.targets.push_back({std::move(selector)});
    field.animatableComponents = {
        VectorFieldAnimatableComponent::Number,
    };
    field.unit = unit;
    field.animationGroupIdentity = AnimationGroupIdentity(groupIdentity);
    fields.push_back(std::move(field));
}

bool RepresentativeLottieNumber(const Json& property, double& value)
{
    if (property.is_number())
    {
        value = property.get<double>();
        return std::isfinite(value);
    }
    if (property.is_object())
    {
        for (const char* key : {"k", "s", "e"})
        {
            const auto found = property.find(key);
            if (found != property.end()
                && RepresentativeLottieNumber(*found, value))
                return true;
        }
    }
    else if (property.is_array())
    {
        for (const auto& item : property)
            if (RepresentativeLottieNumber(item, value))
                return true;
    }
    return false;
}

bool RepresentativeLottieVectorComponent(
    const Json& property,
    const std::size_t component,
    double& value)
{
    if (property.is_object())
    {
        for (const char* key : {"k", "s", "e"})
        {
            const auto found = property.find(key);
            if (found != property.end()
                && RepresentativeLottieVectorComponent(
                    *found, component, value))
                return true;
        }
    }
    else if (property.is_array())
    {
        if (component < property.size() && property[component].is_number())
        {
            value = property[component].get<double>();
            return std::isfinite(value);
        }
        for (const auto& item : property)
            if (RepresentativeLottieVectorComponent(item, component, value))
                return true;
    }
    return false;
}

bool ContainsAsciiToken(std::string value, const char* token)
{
    value = LowerAscii(std::move(value));
    return value.find(token) != std::string::npos;
}

bool FindPackedGradientArray(
    const Json& value,
    const std::size_t stopCount,
    const std::size_t stopIndex,
    VectorColor& color)
{
    const std::size_t required = stopCount * 4;
    if (value.is_array() && value.size() >= required
        && stopIndex < stopCount)
    {
        const std::size_t base = stopIndex * 4;
        bool numeric = true;
        for (std::size_t index = 0; index < required; ++index)
            numeric = numeric && value[index].is_number();
        if (numeric)
        {
            const double position = value[base].get<double>();
            const double red = value[base + 1].get<double>();
            const double green = value[base + 2].get<double>();
            const double blue = value[base + 3].get<double>();
            if (std::isfinite(position) && std::isfinite(red)
                && std::isfinite(green) && std::isfinite(blue))
            {
                color = {
                    static_cast<float>(std::clamp(red, 0.0, 1.0)),
                    static_cast<float>(std::clamp(green, 0.0, 1.0)),
                    static_cast<float>(std::clamp(blue, 0.0, 1.0)),
                    1.0F,
                };
                return true;
            }
        }
    }
    if (value.is_object())
    {
        for (const char* key : {"k", "s", "e"})
        {
            const auto found = value.find(key);
            if (found != value.end()
                && FindPackedGradientArray(
                    *found, stopCount, stopIndex, color))
                return true;
        }
    }
    else if (value.is_array())
    {
        for (const auto& item : value)
            if (FindPackedGradientArray(item, stopCount, stopIndex, color))
                return true;
    }
    return false;
}

void DiscoverLottieNode(
    const Json& value,
    const std::string& path,
    std::vector<VectorEditableField>& fields,
    std::unordered_map<std::string, std::size_t>& colorFields,
    const std::unordered_map<std::string, std::string>& fontFamilies,
    const VectorLimits& limits,
    const bool inheritedShadowContext = false)
{
    if (fields.size() >= limits.maximumEditableFields)
        return;
    if (value.is_object())
    {
        const auto type = value.find("ty");
        const auto name = value.find("nm");
        const auto matchName = value.find("mn");
        const std::string baseLabel =
            name != value.end() && name->is_string()
            ? name->get<std::string>() : std::string("Value");
        const std::string semanticName = baseLabel + ' '
            + (matchName != value.end() && matchName->is_string()
                ? matchName->get<std::string>() : std::string{});
        const bool shadowContext = inheritedShadowContext
            || ContainsAsciiToken(semanticName, "shadow");
        const auto vectorGroup = [&](const char* suffix) {
            return std::string("lottie:") + path + ':' + suffix;
        };
        const auto discoverNumber =
            [&](const Json& owner, const std::string& ownerPath,
                const char* propertyName, const char* suffix,
                const VectorFieldRole role, const VectorFieldUnit unit,
                const std::string& groupIdentity,
                const double minimum, const double maximum,
                const double step) {
                const auto property = owner.find(propertyName);
                double number = 0.0;
                if (property != owner.end()
                    && RepresentativeLottieNumber(*property, number))
                {
                    AddNumberField(
                        fields, number, baseLabel + suffix,
                        std::string(kJsonSelectorPrefix) + ownerPath + "/"
                            + propertyName + "/k",
                        role, unit, groupIdentity,
                        minimum, maximum, step, limits);
                }
            };
        const auto discoverVectorComponent =
            [&](const Json& owner, const std::string& ownerPath,
                const char* propertyName, const std::size_t component,
                const char* suffix, const VectorFieldRole role,
                const VectorFieldUnit unit, const std::string& groupIdentity,
                const double minimum, const double maximum,
                const double step) {
                const auto property = owner.find(propertyName);
                double number = 0.0;
                if (property != owner.end()
                    && RepresentativeLottieVectorComponent(
                        *property, component, number))
                {
                    AddNumberField(
                        fields, number, baseLabel + suffix,
                        std::string(kJsonSelectorPrefix) + ownerPath + "/"
                            + propertyName + "/k/"
                            + std::to_string(component),
                        role, unit, groupIdentity,
                        minimum, maximum, step, limits);
                }
            };
        if (type != value.end() && type->is_string())
        {
            const std::string& shapeType = type->get_ref<const std::string&>();
            const bool centerline = ContainsAsciiToken(
                semanticName, "centerline");
            if (shapeType == "st")
            {
                discoverNumber(
                    value, path, "w", " width",
                    centerline ? VectorFieldRole::CenterlineWidth
                               : VectorFieldRole::StrokeWidth,
                    VectorFieldUnit::Pixels,
                    vectorGroup(centerline ? "centerline" : "stroke"),
                    0.0, 1000.0, 1.0);
            }
            if (shapeType == "rd")
            {
                discoverNumber(
                    value, path, "r", " roundness",
                    VectorFieldRole::Roundness, VectorFieldUnit::Pixels,
                    vectorGroup("roundness"), 0.0, 4096.0, 1.0);
            }
            if (shapeType == "fl")
            {
                discoverNumber(
                    value, path, "o", " opacity",
                    VectorFieldRole::FillOpacity, VectorFieldUnit::Percent,
                    vectorGroup("fill"), 0.0, 100.0, 1.0);
            }
            if (shapeType == "st")
            {
                discoverNumber(
                    value, path, "o", " opacity",
                    centerline ? VectorFieldRole::CenterlineOpacity
                               : VectorFieldRole::StrokeOpacity,
                    VectorFieldUnit::Percent,
                    vectorGroup(centerline ? "centerline" : "stroke"),
                    0.0, 100.0, 1.0);
            }
            if (shapeType == "gf" || shapeType == "gs")
            {
                const auto group = vectorGroup("gradient");
                discoverNumber(
                    value, path, "o", " opacity",
                    VectorFieldRole::GradientOpacity,
                    VectorFieldUnit::Percent, group,
                    0.0, 100.0, 1.0);
                discoverNumber(
                    value, path, "h", " position",
                    VectorFieldRole::GradientPosition,
                    VectorFieldUnit::Percent, group,
                    -100.0, 100.0, 1.0);
                discoverNumber(
                    value, path, "a", " angle",
                    VectorFieldRole::GradientAngle,
                    VectorFieldUnit::Degrees, group,
                    -360.0, 360.0, 1.0);
            }
            if (shapeType == "tr")
            {
                const auto group = vectorGroup("transform");
                discoverVectorComponent(
                    value, path, "s", 0, " width",
                    VectorFieldRole::SizeX, VectorFieldUnit::Percent,
                    group, 0.0, 10000.0, 1.0);
                discoverVectorComponent(
                    value, path, "s", 1, " height",
                    VectorFieldRole::SizeY, VectorFieldUnit::Percent,
                    group, 0.0, 10000.0, 1.0);
                discoverNumber(
                    value, path, "o", " opacity",
                    VectorFieldRole::GlobalOpacity,
                    VectorFieldUnit::Percent, group,
                    0.0, 100.0, 1.0);
            }
            if (shapeType == "tm")
            {
                const auto group = vectorGroup("trim");
                discoverNumber(
                    value, path, "s", " start", VectorFieldRole::Scalar,
                    VectorFieldUnit::Percent, group, 0.0, 100.0, 1.0);
                discoverNumber(
                    value, path, "e", " end", VectorFieldRole::Scalar,
                    VectorFieldUnit::Percent, group, 0.0, 100.0, 1.0);
                discoverNumber(
                    value, path, "o", " offset", VectorFieldRole::Scalar,
                    VectorFieldUnit::Degrees, group, -360.0, 360.0, 1.0);
            }
        }
        if (type != value.end() && type->is_string()
            && (type->get_ref<const std::string&>() == "gf"
                || type->get_ref<const std::string&>() == "gs"))
        {
            const auto gradient = value.find("g");
            if (gradient != value.end() && gradient->is_object())
            {
                const auto points = gradient->find("p");
                const auto property = gradient->find("k");
                if (points != gradient->end() && points->is_number_integer()
                    && property != gradient->end())
                {
                    const int declaredStops = points->get<int>();
                    const std::size_t stopCount = declaredStops > 0
                        ? std::min<std::size_t>(
                              static_cast<std::size_t>(declaredStops), 32)
                        : 0;
                    const auto name = value.find("nm");
                    const std::string label =
                        name != value.end() && name->is_string()
                        ? name->get<std::string>()
                        : std::string("Gradient");
                    for (std::size_t index = 0; index < stopCount; ++index)
                    {
                        VectorColor color;
                        if (!FindPackedGradientArray(
                                *property, stopCount, index, color))
                            continue;
                        AddColorField(
                            fields, colorFields,
                            VectorFieldRole::GradientStop, color,
                            label + " stop " + std::to_string(index + 1),
                            std::string(kJsonGradientSelectorPrefix) + path
                                + "/g:" + std::to_string(index),
                            vectorGroup("gradient"),
                            limits);
                    }
                }
            }
        }
        if (type != value.end() && type->is_string()
            && (type->get_ref<const std::string&>() == "fl"
                || type->get_ref<const std::string&>() == "st"))
        {
            const auto colorProperty = value.find("c");
            VectorColor color;
            if (colorProperty != value.end()
                && RepresentativeLottieColor(*colorProperty, color))
            {
                const bool centerline = ContainsAsciiToken(
                    semanticName, "centerline");
                const auto role = type->get_ref<const std::string&>() == "st"
                    ? (centerline ? VectorFieldRole::CenterlineColor
                                  : VectorFieldRole::Stroke)
                    : VectorFieldRole::Fill;
                std::string label;
                const auto name = value.find("nm");
                if (name != value.end() && name->is_string())
                    label = name->get<std::string>();
                AddColorField(
                    fields, colorFields, role, color, std::move(label),
                    std::string(kJsonSelectorPrefix) + path + "/c",
                    vectorGroup(centerline ? "centerline"
                                           : role == VectorFieldRole::Fill
                                               ? "fill" : "stroke"),
                    limits);
            }
        }
        if (shadowContext)
        {
            const auto property = value.find("v");
            const std::string propertyName = LowerAscii(semanticName);
            const auto group = vectorGroup("shadow");
            if (property != value.end())
            {
                VectorColor color;
                if (propertyName.find("color") != std::string::npos
                    && RepresentativeLottieColor(*property, color))
                {
                    AddColorField(
                        fields, colorFields, VectorFieldRole::ShadowColor,
                        color, baseLabel,
                        std::string(kJsonSelectorPrefix) + path + "/v",
                        group, limits);
                }
                else if (propertyName.find("opacity") != std::string::npos)
                {
                    discoverNumber(
                        value, path, "v", "", VectorFieldRole::ShadowOpacity,
                        VectorFieldUnit::Percent, group,
                        0.0, 100.0, 1.0);
                }
                else if (propertyName.find("distance") != std::string::npos)
                {
                    discoverNumber(
                        value, path, "v", "", VectorFieldRole::ShadowDistance,
                        VectorFieldUnit::Pixels, group,
                        0.0, 10000.0, 1.0);
                }
                else if (propertyName.find("direction") != std::string::npos
                         || propertyName.find("angle") != std::string::npos)
                {
                    discoverNumber(
                        value, path, "v", "", VectorFieldRole::ShadowAngle,
                        VectorFieldUnit::Degrees, group,
                        -360.0, 360.0, 1.0);
                }
            }
        }
        if (type != value.end() && type->is_number_integer()
            && type->get<int>() == 2)
        {
            const auto transform = value.find("ks");
            if (transform != value.end() && transform->is_object())
            {
                discoverNumber(
                    *transform, path + "/ks", "o", " opacity",
                    VectorFieldRole::TextureOpacity,
                    VectorFieldUnit::Percent,
                    vectorGroup("texture"), 0.0, 100.0, 1.0);
            }
        }
        if (type != value.end() && type->is_number_integer()
            && type->get<int>() == 5)
        {
            const auto text = value.find("t");
            std::vector<VectorFieldTarget> targets;
            std::vector<VectorFieldTarget> fontTargets;
            std::string defaultText;
            std::string defaultFont;
            if (text != value.end() && text->is_object())
            {
                const auto document = text->find("d");
                if (document != text->end() && document->is_object())
                {
                    const auto keyframes = document->find("k");
                    if (keyframes != document->end() && keyframes->is_array())
                    {
                        for (std::size_t index = 0;
                             index < keyframes->size()
                             && targets.size() < limits.maximumFieldTargets
                             && fontTargets.size() < limits.maximumFieldTargets;
                             ++index)
                        {
                            const auto& keyframe = (*keyframes)[index];
                            if (!keyframe.is_object())
                                continue;
                            const auto state = keyframe.find("s");
                            if (state == keyframe.end() || !state->is_object())
                                continue;
                            const std::string statePath =
                                std::string(kJsonSelectorPrefix) + path
                                + "/t/d/k/" + std::to_string(index) + "/s/";
                            const auto name = value.find("nm");
                            const std::string layerName =
                                name != value.end() && name->is_string()
                                    ? name->get<std::string>()
                                    : std::string("Text");
                            const auto discoverTextStyleColor =
                                [&](const char* property,
                                    const VectorFieldRole role,
                                    const char* suffix) {
                                    const auto colorProperty =
                                        state->find(property);
                                    VectorColor color;
                                    if (colorProperty != state->end()
                                        && RepresentativeLottieColor(
                                            *colorProperty, color))
                                    {
                                        AddColorField(
                                            fields, colorFields, role, color,
                                            layerName + suffix,
                                            statePath + property,
                                            vectorGroup(
                                                role == VectorFieldRole::Fill
                                                    ? "text_fill"
                                                    : "text_stroke"),
                                            limits);
                                    }
                                };
                            discoverTextStyleColor(
                                "fc", VectorFieldRole::Fill, " fill");
                            discoverTextStyleColor(
                                "sc", VectorFieldRole::Stroke, " stroke");
                            const auto font = state->find("f");
                            if (font != state->end() && font->is_string())
                            {
                                const std::string fontName =
                                    font->get<std::string>();
                                const auto family = fontFamilies.find(fontName);
                                const std::string candidate =
                                    family == fontFamilies.end()
                                    ? fontName : family->second;
                                if (ValidFontFamily(candidate, limits))
                                {
                                    if (fontTargets.empty())
                                        defaultFont = candidate;
                                    fontTargets.push_back({statePath + "f"});
                                }
                            }
                            const auto content = state->find("t");
                            if (content == state->end() || !content->is_string())
                                continue;
                            const std::string candidate = content->get<std::string>();
                            if (candidate.size() > limits.maximumEditableTextBytes
                                || !ValidUtf8(candidate))
                                continue;
                            if (targets.empty())
                                defaultText = candidate;
                            targets.push_back({
                                std::string(kJsonSelectorPrefix) + path
                                + "/t/d/k/" + std::to_string(index) + "/s/t",
                            });
                        }
                    }
                }
            }
            if (!targets.empty() && fields.size() < limits.maximumEditableFields)
            {
                VectorEditableField field;
                const std::string digest = Sha256Digest(
                    reinterpret_cast<const std::uint8_t*>(path.data()), path.size());
                field.id = "text." + digest.substr(7, 16);
                const auto name = value.find("nm");
                field.label = name != value.end() && name->is_string()
                    ? name->get<std::string>() : std::string{};
                field.kind = VectorFieldKind::Text;
                field.role = VectorFieldRole::Text;
                field.defaultValue.kind = VectorFieldKind::Text;
                field.defaultValue.text = std::move(defaultText);
                field.targets = std::move(targets);
                fields.push_back(std::move(field));
            }
            if (!fontTargets.empty()
                && fields.size() < limits.maximumEditableFields)
            {
                const auto name = value.find("nm");
                const std::string label = name != value.end() && name->is_string()
                    ? name->get<std::string>() + " font"
                    : std::string("Font");
                const std::string identity = path + "/font";
                const std::string digest = Sha256Digest(
                    reinterpret_cast<const std::uint8_t*>(identity.data()),
                    identity.size());
                VectorEditableField field;
                field.id = "font." + digest.substr(7, 16);
                field.label = label;
                field.kind = VectorFieldKind::Text;
                field.role = VectorFieldRole::Font;
                field.defaultValue.kind = VectorFieldKind::Text;
                field.defaultValue.text = std::move(defaultFont);
                field.targets = std::move(fontTargets);
                fields.push_back(std::move(field));
            }
        }

        for (auto item = value.begin(); item != value.end(); ++item)
            DiscoverLottieNode(
                item.value(), path + "/" + JsonPointerEscape(item.key()),
                fields, colorFields, fontFamilies, limits, shadowContext);
    }
    else if (value.is_array())
    {
        for (std::size_t index = 0; index < value.size(); ++index)
            DiscoverLottieNode(
                value[index], path + "/" + std::to_string(index),
                fields, colorFields, fontFamilies, limits,
                inheritedShadowContext);
    }
}

struct SvgAttribute final {
    std::string name;
    std::string value;
    std::size_t valueOffset{0};
    std::size_t valueLength{0};
};

std::vector<SvgAttribute> ParseSvgAttributes(
    const std::string& source,
    const std::size_t begin,
    const std::size_t end)
{
    std::vector<SvgAttribute> attributes;
    std::size_t cursor = begin;
    while (cursor < end && source[cursor] != '<')
        ++cursor;
    if (cursor < end)
        ++cursor;
    while (cursor < end && !std::isspace(static_cast<unsigned char>(source[cursor]))
           && source[cursor] != '>' && source[cursor] != '/')
        ++cursor;
    while (cursor < end)
    {
        while (cursor < end
               && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        if (cursor >= end || source[cursor] == '>' || source[cursor] == '/')
            break;
        const std::size_t nameBegin = cursor;
        while (cursor < end && !std::isspace(static_cast<unsigned char>(source[cursor]))
               && source[cursor] != '=' && source[cursor] != '>'
               && source[cursor] != '/')
            ++cursor;
        std::string name = LowerAscii(source.substr(nameBegin, cursor - nameBegin));
        while (cursor < end
               && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        if (cursor >= end || source[cursor] != '=')
            continue;
        ++cursor;
        while (cursor < end
               && std::isspace(static_cast<unsigned char>(source[cursor])))
            ++cursor;
        if (cursor >= end)
            break;
        const char quote = source[cursor] == '\'' || source[cursor] == '"'
            ? source[cursor++] : '\0';
        const std::size_t valueBegin = cursor;
        if (quote != '\0')
            while (cursor < end && source[cursor] != quote)
                ++cursor;
        else
            while (cursor < end
                   && !std::isspace(static_cast<unsigned char>(source[cursor]))
                   && source[cursor] != '>')
                ++cursor;
        attributes.push_back({
            std::move(name), source.substr(valueBegin, cursor - valueBegin),
            valueBegin, cursor - valueBegin,
        });
        if (quote != '\0' && cursor < end)
            ++cursor;
    }
    return attributes;
}

const SvgAttribute* FindSvgAttribute(
    const std::vector<SvgAttribute>& attributes,
    const std::string& name) noexcept
{
    const auto found = std::find_if(
        attributes.begin(), attributes.end(), [&](const auto& attribute) {
            return attribute.name == name;
        });
    return found == attributes.end() ? nullptr : &*found;
}

std::string XmlUnescape(std::string value)
{
    const std::array<std::pair<const char*, const char*>, 5> entities{{
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
        {"&apos;", "'"}, {"&amp;", "&"},
    }};
    for (const auto& entity : entities)
    {
        std::size_t offset = 0;
        while ((offset = value.find(entity.first, offset)) != std::string::npos)
        {
            value.replace(offset, std::char_traits<char>::length(entity.first),
                          entity.second);
            offset += std::char_traits<char>::length(entity.second);
        }
    }
    return value;
}

std::string XmlEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '&': escaped.append("&amp;"); break;
        case '<': escaped.append("&lt;"); break;
        case '>': escaped.append("&gt;"); break;
        case '"': escaped.append("&quot;"); break;
        case '\'': escaped.append("&apos;"); break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

std::string SvgSelector(
    const std::size_t offset,
    const std::size_t length,
    const std::size_t tagOffset = std::string::npos,
    const std::string& attribute = {})
{
    std::string selector = std::string(kSvgSelectorPrefix)
        + std::to_string(offset) + ':' + std::to_string(length);
    if (tagOffset != std::string::npos && !attribute.empty())
        selector += ':' + std::to_string(tagOffset) + ':' + attribute;
    return selector;
}

bool ParseSvgNumber(
    const std::string& source,
    double& value,
    VectorFieldUnit& unit)
{
    std::string text = TrimAscii(source);
    unit = VectorFieldUnit::Unitless;
    if (text.size() >= 2
        && LowerAscii(text.substr(text.size() - 2)) == "px")
    {
        unit = VectorFieldUnit::Pixels;
        text.resize(text.size() - 2);
    }
    else if (text.size() >= 3
             && LowerAscii(text.substr(text.size() - 3)) == "deg")
    {
        unit = VectorFieldUnit::Degrees;
        text.resize(text.size() - 3);
    }
    else if (!text.empty() && text.back() == '%')
    {
        unit = VectorFieldUnit::Percent;
        text.pop_back();
    }
    if (text.empty())
        return false;
    // libc++ floating from_chars requires macOS 26. Keep the declared macOS
    // 15 baseline and locale-independent decimal parsing for SVG controls.
    // from_chars rejects leading '+' and whitespace; preserve that grammar.
    if (text.front() == '+' || std::isspace(static_cast<unsigned char>(text.front())))
        return false;
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    return !input.fail() && input.eof() && std::isfinite(value);
}

void DiscoverSvg(
    const std::string& source,
    std::vector<VectorEditableField>& fields,
    const VectorLimits& limits)
{
    std::unordered_map<std::string, std::size_t> colorFields;
    std::size_t cursor = 0;
    while (cursor < source.size() && fields.size() < limits.maximumEditableFields)
    {
        const std::size_t tagBegin = source.find('<', cursor);
        if (tagBegin == std::string::npos || tagBegin + 1 >= source.size())
            break;
        if (source[tagBegin + 1] == '/' || source[tagBegin + 1] == '!'
            || source[tagBegin + 1] == '?')
        {
            cursor = tagBegin + 2;
            continue;
        }
        std::size_t tagEnd = tagBegin + 1;
        char quote = '\0';
        for (; tagEnd < source.size(); ++tagEnd)
        {
            const char character = source[tagEnd];
            if (quote != '\0')
            {
                if (character == quote)
                    quote = '\0';
                continue;
            }
            if (character == '\'' || character == '"')
                quote = character;
            else if (character == '>')
                break;
        }
        if (tagEnd >= source.size())
            break;
        std::size_t nameBegin = tagBegin + 1;
        while (nameBegin < tagEnd
               && std::isspace(static_cast<unsigned char>(source[nameBegin])))
            ++nameBegin;
        std::size_t nameEnd = nameBegin;
        while (nameEnd < tagEnd
               && !std::isspace(static_cast<unsigned char>(source[nameEnd]))
               && source[nameEnd] != '/' && source[nameEnd] != '>')
            ++nameEnd;
        const std::string tagName = LowerAscii(
            source.substr(nameBegin, nameEnd - nameBegin));
        const auto attributes = ParseSvgAttributes(source, tagBegin, tagEnd);
        const auto* id = FindSvgAttribute(attributes, "id");
        const auto* customLabel = FindSvgAttribute(attributes, "data-vc-label");
        const std::string nodeLabel = customLabel ? customLabel->value
            : id ? id->value : std::string{};
        const auto* explicitRole = FindSvgAttribute(attributes, "data-vc-role");
        const std::string semanticRole = LowerAscii(
            explicitRole ? explicitRole->value : nodeLabel);
        const bool centerline = semanticRole.find("centerline")
            != std::string::npos;
        const bool shadow = semanticRole.find("shadow") != std::string::npos
            || tagName == "feflood";
        const auto vectorGroup = [&](const char* suffix) {
            return std::string("svg:")
                + (id ? id->value : std::to_string(tagBegin)) + ':' + suffix;
        };
        const auto selector = [&](const SvgAttribute& attribute) {
            return SvgSelector(
                attribute.valueOffset, attribute.valueLength,
                tagBegin, attribute.name);
        };
        for (const auto role : {VectorFieldRole::Fill, VectorFieldRole::Stroke})
        {
            const char* attributeName = role == VectorFieldRole::Fill
                ? "fill" : "stroke";
            const auto* attribute = FindSvgAttribute(attributes, attributeName);
            VectorColor color;
            if (attribute && ParseHexColor(attribute->value, color))
            {
                const auto discoveredRole = role == VectorFieldRole::Stroke
                    && centerline ? VectorFieldRole::CenterlineColor : role;
                AddColorField(
                    fields, colorFields, discoveredRole, color, nodeLabel,
                    selector(*attribute),
                    vectorGroup(discoveredRole == VectorFieldRole::Fill
                        ? "fill" : centerline ? "centerline" : "stroke"),
                    limits);
            }
        }
        if (tagName == "stop")
        {
            const auto* attribute = FindSvgAttribute(attributes, "stop-color");
            VectorColor color;
            if (attribute && ParseHexColor(attribute->value, color))
                AddColorField(
                    fields, colorFields, VectorFieldRole::GradientStop, color,
                    nodeLabel,
                    selector(*attribute), vectorGroup("gradient"),
                    limits);
        }
        if (shadow)
        {
            const auto* attribute = FindSvgAttribute(attributes, "flood-color");
            VectorColor color;
            if (attribute && ParseHexColor(attribute->value, color))
                AddColorField(
                    fields, colorFields, VectorFieldRole::ShadowColor, color,
                    nodeLabel.empty() ? std::string("Shadow color") : nodeLabel,
                    selector(*attribute), vectorGroup("shadow"), limits);
        }
        const auto discoverNumber =
            [&](const char* attributeName, const char* suffix,
                const VectorFieldRole role, const VectorFieldUnit fallbackUnit,
                const std::string& groupIdentity,
                const double minimum, const double maximum,
                const double step) {
                const auto* attribute = FindSvgAttribute(
                    attributes, attributeName);
                double number = 0.0;
                VectorFieldUnit unit = fallbackUnit;
                if (!attribute
                    || !ParseSvgNumber(attribute->value, number, unit))
                    return;
                if (unit == VectorFieldUnit::Unitless)
                    unit = fallbackUnit;
                double resolvedMinimum = minimum;
                double resolvedMaximum = maximum;
                double resolvedStep = step;
                if (unit == VectorFieldUnit::Percent
                    && fallbackUnit == VectorFieldUnit::Ratio)
                {
                    resolvedMinimum = minimum * 100.0;
                    resolvedMaximum = maximum * 100.0;
                    resolvedStep = step * 100.0;
                }
                AddNumberField(
                    fields, number,
                    nodeLabel.empty() ? std::string(attributeName)
                                      : nodeLabel + suffix,
                    selector(*attribute), role, unit, groupIdentity,
                    resolvedMinimum, resolvedMaximum, resolvedStep, limits);
            };
        discoverNumber(
            "fill-opacity", " opacity", VectorFieldRole::FillOpacity,
            VectorFieldUnit::Ratio, vectorGroup("fill"),
            0.0, 1.0, 0.01);
        discoverNumber(
            "stroke-width", " width",
            centerline ? VectorFieldRole::CenterlineWidth
                       : VectorFieldRole::StrokeWidth,
            VectorFieldUnit::Pixels,
            vectorGroup(centerline ? "centerline" : "stroke"),
            0.0, 10000.0, 1.0);
        discoverNumber(
            "stroke-opacity", " opacity",
            centerline ? VectorFieldRole::CenterlineOpacity
                       : VectorFieldRole::StrokeOpacity,
            VectorFieldUnit::Ratio,
            vectorGroup(centerline ? "centerline" : "stroke"),
            0.0, 1.0, 0.01);
        if (tagName == "rect")
        {
            discoverNumber(
                "rx", " roundness", VectorFieldRole::Roundness,
                VectorFieldUnit::Pixels, vectorGroup("roundness"),
                0.0, 10000.0, 1.0);
            discoverNumber(
                "ry", " roundness", VectorFieldRole::Roundness,
                VectorFieldUnit::Pixels, vectorGroup("roundness"),
                0.0, 10000.0, 1.0);
        }
        if (tagName == "stop")
        {
            discoverNumber(
                "stop-opacity", " opacity",
                VectorFieldRole::GradientOpacity, VectorFieldUnit::Ratio,
                vectorGroup("gradient"), 0.0, 1.0, 0.01);
            discoverNumber(
                "offset", " position", VectorFieldRole::GradientPosition,
                VectorFieldUnit::Ratio, vectorGroup("gradient"),
                0.0, 1.0, 0.01);
        }
        if (shadow)
            discoverNumber(
                "flood-opacity", " opacity", VectorFieldRole::ShadowOpacity,
                VectorFieldUnit::Ratio, vectorGroup("shadow"),
                0.0, 1.0, 0.01);
        if (tagName == "image")
        {
            discoverNumber(
                "opacity", " opacity", VectorFieldRole::TextureOpacity,
                VectorFieldUnit::Ratio, vectorGroup("texture"),
                0.0, 1.0, 0.01);
            discoverNumber(
                "width", " width", VectorFieldRole::SizeX,
                VectorFieldUnit::Pixels, vectorGroup("size"),
                0.0, 100000.0, 1.0);
            discoverNumber(
                "height", " height", VectorFieldRole::SizeY,
                VectorFieldUnit::Pixels, vectorGroup("size"),
                0.0, 100000.0, 1.0);
        }
        else if (tagName == "svg" || tagName == "g")
            discoverNumber(
                "opacity", " opacity", VectorFieldRole::GlobalOpacity,
                VectorFieldUnit::Ratio, vectorGroup("global"),
                0.0, 1.0, 0.01);
        if (const auto* attribute = FindSvgAttribute(attributes, "font-family"))
        {
            const std::string family = TrimAscii(attribute->value);
            if (ValidFontFamily(family, limits))
                AddFontField(
                    fields, family,
                    nodeLabel.empty() ? std::string("Font") : nodeLabel,
                    selector(*attribute),
                    limits);
        }
        if (tagName == "text" || tagName == "tspan")
        {
            const std::string closeToken = "</" + tagName;
            const std::size_t close = source.find(closeToken, tagEnd + 1);
            if (close != std::string::npos)
            {
                const std::size_t contentBegin = tagEnd + 1;
                const std::size_t contentLength = close - contentBegin;
                if (source.find('<', contentBegin) >= close)
                {
                    std::string text = XmlUnescape(
                        source.substr(contentBegin, contentLength));
                    if (text.size() <= limits.maximumEditableTextBytes
                        && ValidUtf8(text))
                    {
                        VectorEditableField field;
                        const auto* explicitId = FindSvgAttribute(
                            attributes, "data-vc-text-field");
                        if (explicitId && ValidFieldId(explicitId->value, limits))
                            field.id = explicitId->value;
                        else
                        {
                            const std::string identity = std::to_string(contentBegin);
                            const std::string digest = Sha256Digest(
                                reinterpret_cast<const std::uint8_t*>(identity.data()),
                                identity.size());
                            field.id = "text." + digest.substr(7, 16);
                        }
                        field.label = nodeLabel;
                        field.kind = VectorFieldKind::Text;
                        field.role = VectorFieldRole::Text;
                        field.defaultValue.kind = VectorFieldKind::Text;
                        field.defaultValue.text = std::move(text);
                        field.targets.push_back({
                            SvgSelector(contentBegin, contentLength),
                        });
                        fields.push_back(std::move(field));
                    }
                }
            }
        }
        cursor = tagEnd + 1;
    }
}

void FinalizeLabels(std::vector<VectorEditableField>& fields)
{
    std::size_t fillIndex = 0;
    std::size_t strokeIndex = 0;
    std::size_t textIndex = 0;
    for (auto& field : fields)
    {
        if (!field.label.empty())
            continue;
        switch (field.role)
        {
        case VectorFieldRole::Fill:
            field.label = "Fill " + std::to_string(++fillIndex);
            break;
        case VectorFieldRole::Stroke:
            field.label = "Stroke " + std::to_string(++strokeIndex);
            break;
        case VectorFieldRole::Text:
            field.label = "Text " + std::to_string(++textIndex);
            break;
        case VectorFieldRole::Scalar:
            field.label = "Value";
            break;
        case VectorFieldRole::GradientStop:
            field.label = "Gradient stop " + std::to_string(++fillIndex);
            break;
        case VectorFieldRole::Font:
            field.label = "Font";
            break;
        default:
            field.label = "Property";
            break;
        }
    }
}

bool ParseSvgSelector(
    const std::string& selector,
    std::size_t& offset,
    std::size_t& length) noexcept
{
    if (selector.compare(0, 4, kSvgSelectorPrefix) != 0)
        return false;
    const auto separator = selector.find(':', 4);
    if (separator == std::string::npos)
        return false;
    const char* offsetBegin = selector.data() + 4;
    const char* offsetEnd = selector.data() + separator;
    const char* lengthBegin = offsetEnd + 1;
    const auto metadata = selector.find(':', separator + 1);
    const char* lengthEnd = selector.data()
        + (metadata == std::string::npos ? selector.size() : metadata);
    const auto offsetResult = std::from_chars(offsetBegin, offsetEnd, offset);
    const auto lengthResult = std::from_chars(lengthBegin, lengthEnd, length);
    return offsetResult.ec == std::errc{} && offsetResult.ptr == offsetEnd
        && lengthResult.ec == std::errc{} && lengthResult.ptr == lengthEnd;
}

struct HsvColor final {
    float hue{0.0F};
    float saturation{0.0F};
    float value{0.0F};
};

HsvColor ToHsv(const VectorColor& color) noexcept
{
    const float maximum = std::max({color.red, color.green, color.blue});
    const float minimum = std::min({color.red, color.green, color.blue});
    const float delta = maximum - minimum;
    HsvColor result;
    result.value = maximum;
    result.saturation = maximum > 0.0F ? delta / maximum : 0.0F;
    if (delta <= 1.0e-6F)
        return result;
    if (maximum == color.red)
        result.hue = (color.green - color.blue) / delta;
    else if (maximum == color.green)
        result.hue = 2.0F + (color.blue - color.red) / delta;
    else
        result.hue = 4.0F + (color.red - color.green) / delta;
    result.hue /= 6.0F;
    if (result.hue < 0.0F)
        result.hue += 1.0F;
    return result;
}

VectorColor FromHsv(const HsvColor& hsv, const float alpha) noexcept
{
    const float hue = hsv.hue - std::floor(hsv.hue);
    const float scaled = hue * 6.0F;
    const int sector = static_cast<int>(std::floor(scaled)) % 6;
    const float fraction = scaled - std::floor(scaled);
    const float p = hsv.value * (1.0F - hsv.saturation);
    const float q = hsv.value * (1.0F - fraction * hsv.saturation);
    const float t = hsv.value * (1.0F - (1.0F - fraction) * hsv.saturation);
    switch (sector)
    {
    case 0: return {hsv.value, t, p, alpha};
    case 1: return {q, hsv.value, p, alpha};
    case 2: return {p, hsv.value, t, alpha};
    case 3: return {p, q, hsv.value, alpha};
    case 4: return {t, p, hsv.value, alpha};
    default: return {hsv.value, p, q, alpha};
    }
}

VectorColor RemapColor(
    const VectorColor& sample,
    const VectorColor& source,
    const VectorColor& target) noexcept
{
    const HsvColor sampleHsv = ToHsv(sample);
    const HsvColor sourceHsv = ToHsv(source);
    const HsvColor targetHsv = ToHsv(target);
    HsvColor remapped = sampleHsv;
    remapped.hue = sourceHsv.saturation > 1.0e-5F
        ? sampleHsv.hue + targetHsv.hue - sourceHsv.hue
        : targetHsv.hue;
    remapped.saturation = std::clamp(
        sourceHsv.saturation > 1.0e-5F
            ? sampleHsv.saturation * targetHsv.saturation
                / sourceHsv.saturation
            : sampleHsv.saturation + targetHsv.saturation,
        0.0F, 1.0F);
    remapped.value = std::clamp(
        sourceHsv.value > 1.0e-5F
            ? sampleHsv.value * targetHsv.value / sourceHsv.value
            : sampleHsv.value + targetHsv.value,
        0.0F, 1.0F);
    const float alpha = std::clamp(
        source.alpha > 1.0e-5F
            ? sample.alpha * target.alpha / source.alpha
            : sample.alpha + target.alpha,
        0.0F, 1.0F);
    return FromHsv(remapped, alpha);
}

bool SetLottieColorArray(
    Json& value,
    const VectorColor& source,
    const VectorColor& target)
{
    if (!value.is_array() || value.size() < 3 || value.size() > 4)
        return false;
    const bool numeric = std::all_of(
        value.begin(), value.begin() + std::min<std::size_t>(4, value.size()),
        [](const auto& item) { return item.is_number(); });
    if (!numeric)
        return false;
    VectorColor sample;
    if (!ColorFromJsonArray(value, sample))
        return false;
    const VectorColor color = RemapColor(sample, source, target);
    value[0] = color.red;
    value[1] = color.green;
    value[2] = color.blue;
    if (value.size() >= 4)
        value[3] = color.alpha;
    else
        value.push_back(color.alpha);
    return true;
}

bool ApplyLottieColorValue(
    Json& value,
    const VectorColor& source,
    const VectorColor& target)
{
    if (SetLottieColorArray(value, source, target))
        return true;
    bool applied = false;
    if (value.is_object())
    {
        for (const char* component : {"k", "s", "e"})
        {
            auto found = value.find(component);
            if (found != value.end())
                applied = ApplyLottieColorValue(
                    *found, source, target) || applied;
        }
    }
    else if (value.is_array())
        for (auto& item : value)
            applied = ApplyLottieColorValue(
                item, source, target) || applied;
    return applied;
}

bool ApplyLottieColor(
    Json& property,
    const VectorColor& source,
    const VectorColor& target)
{
    if (!property.is_object())
        return false;
    auto key = property.find("k");
    return key != property.end()
        && ApplyLottieColorValue(*key, source, target);
}

bool ApplyLottieNumberValue(
    Json& value,
    const double source,
    const double target)
{
    if (value.is_number())
    {
        const double current = value.get<double>();
        if (!std::isfinite(current))
            return false;
        value = current + target - source;
        return true;
    }
    bool applied = false;
    if (value.is_object())
    {
        for (const char* key : {"k", "s", "e"})
        {
            auto found = value.find(key);
            if (found != value.end())
                applied = ApplyLottieNumberValue(
                    *found, source, target) || applied;
        }
    }
    else if (value.is_array())
        for (auto& item : value)
            applied = ApplyLottieNumberValue(
                item, source, target) || applied;
    return applied;
}

bool ApplyLottieVectorComponentValue(
    Json& value,
    const std::size_t component,
    const double source,
    const double target)
{
    if (value.is_array()
        && component < value.size() && value[component].is_number())
    {
        const double current = value[component].get<double>();
        if (!std::isfinite(current))
            return false;
        value[component] = current + target - source;
        return true;
    }
    bool applied = false;
    if (value.is_object())
    {
        for (const char* key : {"k", "s", "e"})
        {
            auto found = value.find(key);
            if (found != value.end())
                applied = ApplyLottieVectorComponentValue(
                    *found, component, source, target) || applied;
        }
    }
    else if (value.is_array())
        for (auto& item : value)
            applied = ApplyLottieVectorComponentValue(
                item, component, source, target) || applied;
    return applied;
}

bool ApplyPackedGradientStop(
    Json& value,
    const std::size_t stopCount,
    const std::size_t stopIndex,
    const VectorColor& source,
    const VectorColor& target)
{
    const std::size_t required = stopCount * 4;
    if (value.is_array() && value.size() >= required
        && stopIndex < stopCount)
    {
        bool numeric = true;
        for (std::size_t index = 0; index < required; ++index)
            numeric = numeric && value[index].is_number();
        if (numeric)
        {
            const std::size_t base = stopIndex * 4;
            VectorColor sample{
                static_cast<float>(value[base + 1].get<double>()),
                static_cast<float>(value[base + 2].get<double>()),
                static_cast<float>(value[base + 3].get<double>()),
                1.0F,
            };
            sample.red = std::clamp(sample.red, 0.0F, 1.0F);
            sample.green = std::clamp(sample.green, 0.0F, 1.0F);
            sample.blue = std::clamp(sample.blue, 0.0F, 1.0F);
            const VectorColor remapped = RemapColor(sample, source, target);
            value[base + 1] = remapped.red;
            value[base + 2] = remapped.green;
            value[base + 3] = remapped.blue;
            return true;
        }
    }
    bool applied = false;
    if (value.is_object())
    {
        for (const char* key : {"k", "s", "e"})
        {
            auto found = value.find(key);
            if (found != value.end())
                applied = ApplyPackedGradientStop(
                    *found, stopCount, stopIndex, source, target) || applied;
        }
    }
    else if (value.is_array())
        for (auto& item : value)
            applied = ApplyPackedGradientStop(
                item, stopCount, stopIndex, source, target) || applied;
    return applied;
}

bool ApplyLottieGradientStop(
    Json& gradient,
    const std::size_t stopIndex,
    const VectorColor& source,
    const VectorColor& target)
{
    if (!gradient.is_object())
        return false;
    const auto points = gradient.find("p");
    auto property = gradient.find("k");
    if (points == gradient.end() || !points->is_number_integer()
        || property == gradient.end())
        return false;
    const int declaredStops = points->get<int>();
    if (declaredStops <= 0 || declaredStops > 32)
        return false;
    return ApplyPackedGradientStop(
        *property, static_cast<std::size_t>(declaredStops), stopIndex,
        source, target);
}

bool EnsureLottieFont(Json& source, const std::string& family)
{
    if (!source.is_object())
        return false;
    Json& fonts = source["fonts"];
    if (!fonts.is_object())
        fonts = Json::object();
    Json& list = fonts["list"];
    if (!list.is_array())
        list = Json::array();
    for (auto& font : list)
    {
        if (!font.is_object())
            continue;
        const auto name = font.find("fName");
        const auto currentFamily = font.find("fFamily");
        if ((name != font.end() && name->is_string()
             && name->get_ref<const std::string&>() == family)
            || (currentFamily != font.end() && currentFamily->is_string()
                && currentFamily->get_ref<const std::string&>() == family))
        {
            font["fName"] = family;
            font["fFamily"] = family;
            return true;
        }
    }
    list.push_back({
        {"fName", family},
        {"fFamily", family},
        {"fStyle", "Regular"},
        {"ascent", 75},
    });
    return true;
}

bool ParseGradientSelector(
    const std::string& selector,
    std::string& path,
    std::size_t& stopIndex) noexcept
{
    const std::size_t prefixLength =
        std::char_traits<char>::length(kJsonGradientSelectorPrefix);
    if (selector.compare(0, prefixLength, kJsonGradientSelectorPrefix) != 0)
        return false;
    const auto separator = selector.rfind(':');
    if (separator == std::string::npos || separator < prefixLength)
        return false;
    path = selector.substr(prefixLength, separator - prefixLength);
    const char* begin = selector.data() + separator + 1;
    const char* end = selector.data() + selector.size();
    const auto result = std::from_chars(begin, end, stopIndex);
    return result.ec == std::errc{} && result.ptr == end;
}

struct SvgReplacement final {
    std::size_t offset{0};
    std::size_t length{0};
    std::string value;
};

std::string SvgNumberText(
    const double value,
    const VectorFieldUnit unit)
{
    std::ostringstream stream;
    stream << std::setprecision(12) << value;
    switch (unit)
    {
    case VectorFieldUnit::Percent: stream << '%'; break;
    case VectorFieldUnit::Pixels: stream << "px"; break;
    case VectorFieldUnit::Degrees: stream << "deg"; break;
    case VectorFieldUnit::Unitless:
    case VectorFieldUnit::Ratio:
        break;
    }
    return stream.str();
}

} // namespace

const VectorEditableField* FindVectorEditableField(
    const VectorDocument& document,
    const std::string& fieldId) noexcept
{
    const auto found = std::find_if(
        document.editableFields.begin(), document.editableFields.end(),
        [&](const auto& field) { return field.id == fieldId; });
    return found == document.editableFields.end() ? nullptr : &*found;
}

const VectorFieldOverride* FindVectorFieldOverride(
    const VectorDocument& document,
    const std::string& fieldId) noexcept
{
    const auto found = std::find_if(
        document.overrides.begin(), document.overrides.end(),
        [&](const auto& value) { return value.fieldId == fieldId; });
    return found == document.overrides.end() ? nullptr : &*found;
}

VectorFieldValue EffectiveVectorFieldValue(
    const VectorDocument& document,
    const VectorEditableField& field,
    const VectorLimits& limits)
{
    const auto* value = FindVectorFieldOverride(document, field.id);
    return value && ValidValue(field, value->value, limits)
        ? value->value : field.defaultValue;
}

void SanitizeVectorOverrides(
    VectorDocument& document,
    const VectorLimits& limits)
{
    std::unordered_set<std::string> fieldIds;
    std::vector<VectorEditableField> fields;
    fields.reserve(std::min(document.editableFields.size(), limits.maximumEditableFields));
    for (auto& field : document.editableFields)
    {
        if (fields.size() >= limits.maximumEditableFields
            || !ValidField(field, limits) || !fieldIds.insert(field.id).second)
            continue;
        fields.push_back(std::move(field));
    }
    document.editableFields = std::move(fields);

    std::unordered_set<std::string> overrideIds;
    std::vector<VectorFieldOverride> overrides;
    overrides.reserve(std::min(document.overrides.size(), document.editableFields.size()));
    for (auto& value : document.overrides)
    {
        const auto* field = FindVectorEditableField(document, value.fieldId);
        if (!field || !overrideIds.insert(value.fieldId).second
            || !ValidValue(*field, value.value, limits)
            || value.value == field->defaultValue)
            continue;
        overrides.push_back(std::move(value));
    }
    document.overrides = std::move(overrides);
}

bool SetVectorFieldOverride(
    VectorDocument& document,
    const std::string& fieldId,
    const VectorFieldValue* value,
    bool& changed,
    const VectorLimits& limits)
{
    changed = false;
    const auto* field = FindVectorEditableField(document, fieldId);
    if (!field || (value && !ValidValue(*field, *value, limits)))
        return false;
    auto found = std::find_if(
        document.overrides.begin(), document.overrides.end(),
        [&](const auto& current) { return current.fieldId == fieldId; });
    const bool reset = !value || *value == field->defaultValue;
    if (reset)
    {
        if (found != document.overrides.end())
        {
            document.overrides.erase(found);
            changed = true;
        }
        return true;
    }
    if (found != document.overrides.end())
    {
        if (found->value == *value)
            return true;
        found->value = *value;
        changed = true;
        return true;
    }
    document.overrides.push_back({fieldId, *value});
    changed = true;
    return true;
}

bool ResetAllVectorFieldOverrides(
    VectorDocument& document,
    bool& changed) noexcept
{
    changed = !document.overrides.empty();
    document.overrides.clear();
    return true;
}

bool FinalizeVectorRuntimeOverrides(
    VectorRuntimeOverrideSet& overrides,
    std::string& error)
{
    error.clear();
    std::sort(
        overrides.values.begin(), overrides.values.end(),
        [](const auto& left, const auto& right) {
            if (left.fieldId != right.fieldId)
                return left.fieldId < right.fieldId;
            return static_cast<std::uint8_t>(left.component)
                < static_cast<std::uint8_t>(right.component);
        });

    const auto appendUnsigned = [](std::vector<std::uint8_t>& target,
                                   std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            target.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto appendDouble = [&](std::vector<std::uint8_t>& target,
                                  const double value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        appendUnsigned(target, bits);
    };

    std::vector<std::uint8_t> canonical;
    canonical.reserve(overrides.values.size() * 64U);
    const VectorRuntimeFieldOverride* previous = nullptr;
    for (auto& sampled : overrides.values)
    {
        if (sampled.fieldId.empty())
        {
            error = "vector runtime override field identity is empty";
            return false;
        }
        if (previous && previous->fieldId == sampled.fieldId
            && previous->component == sampled.component)
        {
            error = "vector runtime override component is duplicated";
            return false;
        }
        switch (sampled.component)
        {
        case VectorRuntimeOverrideComponent::Number:
        case VectorRuntimeOverrideComponent::Alpha:
            if (sampled.value.kind != VectorFieldKind::Number
                || !std::isfinite(sampled.value.number))
            {
                error = "vector runtime scalar override is invalid";
                return false;
            }
            break;
        case VectorRuntimeOverrideComponent::Color:
            // Color automation owns RGB only. Canonicalize the intentionally
            // ignored channel so digest identity follows rendered semantics.
            sampled.value.color.alpha = 1.0F;
            if (sampled.value.kind != VectorFieldKind::Color
                || !ValidColor(sampled.value.color))
            {
                error = "vector runtime color override is invalid";
                return false;
            }
            break;
        default:
            error = "vector runtime override component is invalid";
            return false;
        }

        appendUnsigned(canonical, sampled.fieldId.size());
        canonical.insert(canonical.end(), sampled.fieldId.begin(),
                         sampled.fieldId.end());
        canonical.push_back(static_cast<std::uint8_t>(sampled.component));
        if (sampled.component == VectorRuntimeOverrideComponent::Color)
        {
            appendDouble(canonical, sampled.value.color.red);
            appendDouble(canonical, sampled.value.color.green);
            appendDouble(canonical, sampled.value.color.blue);
        }
        else
        {
            appendDouble(canonical, sampled.value.number);
        }
        previous = &sampled;
    }

    overrides.digest = canonical.empty()
        ? std::string{}
        : Sha256Digest(canonical.data(), canonical.size());
    return true;
}

bool ApplyVectorRuntimeOverrides(
    const VectorDocument& authored,
    const VectorRuntimeOverrideSet& overrides,
    VectorDocument& resolved,
    std::string& error,
    const VectorLimits& limits)
{
    auto normalized = overrides;
    if (!FinalizeVectorRuntimeOverrides(normalized, error))
        return false;
    if (!overrides.digest.empty() && overrides.digest != normalized.digest)
    {
        error = "vector runtime override digest does not match its values";
        return false;
    }

    resolved = authored;
    for (const auto& sampled : normalized.values)
    {
        const auto* field = FindVectorEditableField(resolved, sampled.fieldId);
        if (!field)
        {
            error = "vector runtime override field is absent: "
                + sampled.fieldId;
            return false;
        }
        const auto declaredComponent = [&]() {
            switch (sampled.component)
            {
            case VectorRuntimeOverrideComponent::Number:
                return VectorFieldAnimatableComponent::Number;
            case VectorRuntimeOverrideComponent::Color:
                return VectorFieldAnimatableComponent::Color;
            case VectorRuntimeOverrideComponent::Alpha:
                return VectorFieldAnimatableComponent::Alpha;
            }
            return VectorFieldAnimatableComponent::Number;
        }();
        if (std::find(field->animatableComponents.begin(),
                      field->animatableComponents.end(), declaredComponent)
            == field->animatableComponents.end())
        {
            error = "vector runtime override component is not animatable: "
                + sampled.fieldId;
            return false;
        }

        auto value = EffectiveVectorFieldValue(resolved, *field, limits);
        switch (sampled.component)
        {
        case VectorRuntimeOverrideComponent::Number:
            if (field->kind != VectorFieldKind::Number)
            {
                error = "vector runtime Number target is not numeric: "
                    + sampled.fieldId;
                return false;
            }
            value = sampled.value;
            if (field->minimum)
                value.number = std::max(value.number, *field->minimum);
            if (field->maximum)
                value.number = std::min(value.number, *field->maximum);
            break;
        case VectorRuntimeOverrideComponent::Color:
            if (field->kind != VectorFieldKind::Color)
            {
                error = "vector runtime Color target is not a color: "
                    + sampled.fieldId;
                return false;
            }
            value.color.red = sampled.value.color.red;
            value.color.green = sampled.value.color.green;
            value.color.blue = sampled.value.color.blue;
            break;
        case VectorRuntimeOverrideComponent::Alpha:
            if (field->kind != VectorFieldKind::Color
                || sampled.value.number < 0.0
                || sampled.value.number > 1.0)
            {
                error = "vector runtime Alpha target/value is invalid: "
                    + sampled.fieldId;
                return false;
            }
            value.color.alpha = static_cast<float>(sampled.value.number);
            break;
        default:
            error = "vector runtime override component is invalid: "
                + sampled.fieldId;
            return false;
        }
        if (!ValidValue(*field, value, limits))
        {
            error = "vector runtime override is outside the field contract: "
                + sampled.fieldId;
            return false;
        }
        bool changed = false;
        if (!SetVectorFieldOverride(resolved, field->id, &value, changed,
                                    limits))
        {
            error = "vector runtime override could not be composed: "
                + sampled.fieldId;
            return false;
        }
    }
    error.clear();
    return true;
}

bool DiscoverVectorEditableFields(
    VectorDocument& document,
    std::string& error,
    const VectorLimits& limits)
{
    error.clear();
    if (!document.source.bytes || document.source.bytes->empty())
    {
        error = "vector source bytes are unavailable for field discovery";
        return false;
    }
    try
    {
        std::vector<VectorEditableField> discovered;
        if (document.source.kind == VectorSourceKind::Svg)
        {
            const std::string source(
                reinterpret_cast<const char*>(document.source.bytes->data()),
                document.source.bytes->size());
            DiscoverSvg(source, discovered, limits);
        }
        else if (document.source.kind == VectorSourceKind::LottieJson)
        {
            const auto& bytes = *document.source.bytes;
            const Json source = Json::parse(bytes.begin(), bytes.end());
            std::unordered_map<std::string, std::size_t> colorFields;
            std::unordered_map<std::string, std::string> fontFamilies;
            const auto fonts = source.find("fonts");
            if (fonts != source.end() && fonts->is_object())
            {
                const auto list = fonts->find("list");
                if (list != fonts->end() && list->is_array())
                {
                    for (const auto& font : *list)
                    {
                        if (!font.is_object())
                            continue;
                        const auto name = font.find("fName");
                        const auto family = font.find("fFamily");
                        if (name != font.end() && name->is_string()
                            && family != font.end() && family->is_string())
                            fontFamilies.emplace(
                                name->get<std::string>(),
                                family->get<std::string>());
                    }
                }
            }
            DiscoverLottieNode(
                source, {}, discovered, colorFields, fontFamilies, limits);
        }
        else
        {
            error = "editable fields are unsupported for this vector source kind";
            return false;
        }
        FinalizeLabels(discovered);
        auto overrides = std::move(document.overrides);
        for (auto& field : discovered)
        {
            if (!field.animatableComponents.empty())
                continue;
            if (field.kind == VectorFieldKind::Number)
            {
                field.animatableComponents = {
                    VectorFieldAnimatableComponent::Number};
            }
            else if (field.kind == VectorFieldKind::Color)
            {
                field.animatableComponents = {
                    VectorFieldAnimatableComponent::Color,
                    VectorFieldAnimatableComponent::Alpha};
            }
            if (!field.animatableComponents.empty())
                field.animationGroupIdentity = AnimationGroupIdentity(
                    "legacy:" + field.id);
        }
        document.editableFields = std::move(discovered);
        document.overrides = std::move(overrides);
        SanitizeVectorOverrides(document, limits);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

std::shared_ptr<const std::vector<std::uint8_t>> BuildVectorInstanceSource(
    const VectorDocument& document,
    std::string& error,
    const VectorLimits& limits)
{
    error.clear();
    if (!document.source.bytes || document.source.bytes->empty())
    {
        error = "vector source bytes are unavailable";
        return {};
    }
    if (document.overrides.empty())
        return document.source.bytes;
    try
    {
        if (document.source.kind == VectorSourceKind::LottieJson)
        {
            const auto& bytes = *document.source.bytes;
            Json source = Json::parse(bytes.begin(), bytes.end());
            for (const auto& authored : document.overrides)
            {
                const auto* field = FindVectorEditableField(document, authored.fieldId);
                if (!field || !ValidValue(*field, authored.value, limits))
                    continue;
                if (field->role == VectorFieldRole::Font
                    && !EnsureLottieFont(source, authored.value.text))
                {
                    error = "Lottie font table is unavailable";
                    return document.source.bytes;
                }
                for (const auto& target : field->targets)
                {
                    bool applied = false;
                    std::string gradientPath;
                    std::size_t gradientStopIndex = 0;
                    if (ParseGradientSelector(
                            target.selector, gradientPath, gradientStopIndex))
                    {
                        Json& selected = source.at(
                            Json::json_pointer(gradientPath));
                        applied = field->kind == VectorFieldKind::Color
                            && field->role == VectorFieldRole::GradientStop
                            && ApplyLottieGradientStop(
                                selected, gradientStopIndex,
                                field->defaultValue.color,
                                authored.value.color);
                    }
                    else if (target.selector.compare(
                                 0, 5, kJsonSelectorPrefix) == 0)
                    {
                        const std::string selectedPath =
                            target.selector.substr(5);
                        Json& selected = source.at(
                            Json::json_pointer(selectedPath));
                        switch (field->kind)
                        {
                        case VectorFieldKind::Color:
                            applied = ApplyLottieColor(
                                selected, field->defaultValue.color,
                                authored.value.color)
                                || ApplyLottieColorValue(
                                    selected, field->defaultValue.color,
                                    authored.value.color);
                            break;
                        case VectorFieldKind::Text:
                            if (selected.is_string())
                            {
                                selected = authored.value.text;
                                applied = true;
                            }
                            break;
                        case VectorFieldKind::Number:
                            if (selected.is_number())
                            {
                                selected = authored.value.number;
                                applied = true;
                            }
                            else
                            {
                                const auto componentMarker =
                                    selectedPath.rfind("/k/");
                                if (componentMarker != std::string::npos)
                                {
                                    std::size_t component = 0;
                                    const char* begin = selectedPath.data()
                                        + componentMarker + 3;
                                    const char* end = selectedPath.data()
                                        + selectedPath.size();
                                    const auto parsed = std::from_chars(
                                        begin, end, component);
                                    if (parsed.ec == std::errc{}
                                        && parsed.ptr == end)
                                    {
                                        Json& property = source.at(
                                            Json::json_pointer(
                                                selectedPath.substr(
                                                    0, componentMarker)));
                                        applied = ApplyLottieVectorComponentValue(
                                            property, component,
                                            field->defaultValue.number,
                                            authored.value.number);
                                    }
                                }
                                else
                                    applied = ApplyLottieNumberValue(
                                        selected,
                                        field->defaultValue.number,
                                        authored.value.number);
                            }
                            break;
                        }
                    }
                    else
                    {
                        error = "vector field selector does not match Lottie";
                        return document.source.bytes;
                    }
                    if (!applied)
                    {
                        error = "Lottie editable field target is stale";
                        return document.source.bytes;
                    }
                }
            }
            const std::string encoded = source.dump();
            return std::make_shared<const std::vector<std::uint8_t>>(
                encoded.begin(), encoded.end());
        }
        if (document.source.kind == VectorSourceKind::Svg)
        {
            std::string source(
                reinterpret_cast<const char*>(document.source.bytes->data()),
                document.source.bytes->size());
            std::vector<SvgReplacement> replacements;
            for (const auto& authored : document.overrides)
            {
                const auto* field = FindVectorEditableField(document, authored.fieldId);
                if (!field || !ValidValue(*field, authored.value, limits))
                    continue;
                std::string replacement;
                switch (field->kind)
                {
                case VectorFieldKind::Color:
                    replacement = ColorHex(authored.value.color, false);
                    break;
                case VectorFieldKind::Text:
                    replacement = XmlEscape(authored.value.text);
                    break;
                case VectorFieldKind::Number:
                    replacement = SvgNumberText(
                        authored.value.number, field->unit);
                    break;
                }
                for (const auto& target : field->targets)
                {
                    std::size_t offset = 0;
                    std::size_t length = 0;
                    if (!ParseSvgSelector(target.selector, offset, length)
                        || offset > source.size() || length > source.size() - offset)
                    {
                        error = "SVG editable field target is stale";
                        return document.source.bytes;
                    }
                    replacements.push_back({offset, length, replacement});
                }
            }
            std::sort(
                replacements.begin(), replacements.end(),
                [](const auto& left, const auto& right) {
                    return left.offset > right.offset;
                });
            std::size_t previousOffset = source.size();
            for (const auto& replacement : replacements)
            {
                if (replacement.offset + replacement.length > previousOffset)
                {
                    error = "SVG editable field targets overlap";
                    return document.source.bytes;
                }
                source.replace(replacement.offset, replacement.length,
                               replacement.value);
                previousOffset = replacement.offset;
            }
            return std::make_shared<const std::vector<std::uint8_t>>(
                source.begin(), source.end());
        }
        error = "editable fields are unsupported for this vector source kind";
        return document.source.bytes;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return document.source.bytes;
    }
}

} // namespace videocut::vector
