#pragma once

#include "videocut/vector/VectorDocument.h"

#include <cstdint>
#include <string>
#include <vector>

namespace videocut::vector {

enum class VectorRuntimeOverrideComponent : std::uint8_t {
    Number = 1,
    Color = 2,
    Alpha = 3,
};

struct VectorRuntimeFieldOverride final {
    std::string fieldId;
    VectorRuntimeOverrideComponent component{
        VectorRuntimeOverrideComponent::Number};
    VectorFieldValue value{};
};

/// One immutable, item-local vector-field sample. `values` contains component
/// overrides rather than authored document overrides: Color replaces RGB and
/// Alpha replaces only alpha when it is composed over the authored baseline.
struct VectorRuntimeOverrideSet final {
    std::vector<VectorRuntimeFieldOverride> values;
    std::string digest;

    bool empty() const noexcept { return values.empty(); }
};

/// Deterministically discovers editable fields from the immutable source.
/// Existing overrides are reconciled by field ID and invalid values fall back
/// to the newly discovered defaults.
bool DiscoverVectorEditableFields(
    VectorDocument& document,
    std::string& error,
    const VectorLimits& limits = {});

/// Drops malformed field definitions plus unknown, mismatched and invalid
/// override values. This function is the recovery boundary for corrupted or
/// stale project data: rendering continues with immutable source defaults
/// instead of rejecting the whole clip.
void SanitizeVectorOverrides(
    VectorDocument& document,
    const VectorLimits& limits = {});

const VectorEditableField* FindVectorEditableField(
    const VectorDocument& document,
    const std::string& fieldId) noexcept;

const VectorFieldOverride* FindVectorFieldOverride(
    const VectorDocument& document,
    const std::string& fieldId) noexcept;

VectorFieldValue EffectiveVectorFieldValue(
    const VectorDocument& document,
    const VectorEditableField& field,
    const VectorLimits& limits = {});

/// Applies or removes one clip-local override. Missing fields and invalid values
/// return false without mutation; a null value resets to the field default.
bool SetVectorFieldOverride(
    VectorDocument& document,
    const std::string& fieldId,
    const VectorFieldValue* value,
    bool& changed,
    const VectorLimits& limits = {});

bool ResetAllVectorFieldOverrides(
    VectorDocument& document,
    bool& changed) noexcept;

/// Sorts, validates and fingerprints one sampled override set. The digest is
/// canonical and is suitable for render/fingerprint cache keys.
bool FinalizeVectorRuntimeOverrides(
    VectorRuntimeOverrideSet& overrides,
    std::string& error);

/// Composes sampled component values over static authored overrides without
/// mutating the authored VectorDocument. Number replaces the scalar, Color
/// replaces RGB while retaining baseline alpha, and Alpha retains baseline
/// RGB.
bool ApplyVectorRuntimeOverrides(
    const VectorDocument& authored,
    const VectorRuntimeOverrideSet& overrides,
    VectorDocument& resolved,
    std::string& error,
    const VectorLimits& limits = {});

/// Builds renderer-instance bytes without mutating the immutable asset bytes.
/// Any selector/value failure returns the original source and
/// reports a recoverable warning through `error`.
std::shared_ptr<const std::vector<std::uint8_t>> BuildVectorInstanceSource(
    const VectorDocument& document,
    std::string& error,
    const VectorLimits& limits = {});

} // namespace videocut::vector
