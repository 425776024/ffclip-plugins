#pragma once

#include "videocut/text/TextEffectFramePlan.h"
#include "videocut/text/TextExecutionEvidence.h"

#include <cstdint>
#include <optional>
#include <string>

namespace videocut::skia_runtime::internal {

// Shared Page attachment geometry, independent of the material algorithm:
// (unpadded canvas width, height, extra RT width, height), ReferencePixels.
// SourcePixels bindings are converted by the evaluator before reaching here.
inline constexpr std::uint32_t kTextExecutionSourceCanvasSlot = 31U;

// History NodeScalar/Page/Unitless: 0 (or absent) composites current over previous;
// 1 stores the current input unchanged after an explicit feedback material.
inline constexpr std::uint32_t kTextExecutionHistoryWriteModeSlot = 1U;

// Parameter admission and lookup are independent of GPU resources and
// rendering.
bool ExecutionGraphCapabilityMatchesKind(
    text::TextEffectExecutionNodeKind kind,
    text::TextEffectExecutionCapability capability) noexcept;

bool ValidateExecutionGraphParameters(
    const text::TextEffectExecutionNodeFramePlan &node, std::string &error);

bool ValidateClosedExecutionMaterialParameters(
    const text::TextEffectExecutionNodeFramePlan &node, std::string &error);

// Resolve the stable unit first, then fall back to the page value. Scene
// parameters use slot zero, under the same domain precedence as materials.
const text::TextEffectExecutionParameterSample *FindTextExecutionParameter(
    const text::TextEffectExecutionNodeFramePlan &node,
    text::TextEffectExecutionParameterKind kind, std::uint32_t slot,
    std::optional<std::uint64_t> stableUnitId = std::nullopt) noexcept;

class TextExecutionParameterConsumptionScope final {
public:
  TextExecutionParameterConsumptionScope(
      const text::TextEffectExecutionNodeFramePlan &node, bool enabled);
  ~TextExecutionParameterConsumptionScope();
  TextExecutionParameterConsumptionScope(
      const TextExecutionParameterConsumptionScope &) = delete;
  TextExecutionParameterConsumptionScope &operator=(
      const TextExecutionParameterConsumptionScope &) = delete;
  // Earlier coverage must come from this same node/frame before graph drawing,
  // e.g. the source attachment that has already consumed its canvas parameters.
  void Finish(text::TextExecutionNodeEvidence &evidence,
              const text::TextExecutionNodeEvidence *earlier = nullptr) const;
  static void Record(const text::TextEffectExecutionNodeFramePlan &node,
                     const text::TextEffectExecutionParameterSample &sample) noexcept;

private:
  static thread_local TextExecutionParameterConsumptionScope *active_;
  TextExecutionParameterConsumptionScope *previous_{nullptr};
  const text::TextEffectExecutionNodeFramePlan &node_;
  bool enabled_{false};
  std::vector<bool> consumed_;
};

// Execution-only access. Admission and cache hashing must keep using Find
// or direct reads, so neither can manufacture consumption evidence.
const text::TextEffectExecutionParameterSample *ConsumeTextExecutionParameter(
    const text::TextEffectExecutionNodeFramePlan &node,
    text::TextEffectExecutionParameterKind kind, std::uint32_t slot,
    std::optional<std::uint64_t> stableUnitId = std::nullopt) noexcept;

void RecordTextExecutionParameterConsumption(
    const text::TextEffectExecutionNodeFramePlan &node,
    const text::TextEffectExecutionParameterSample &sample) noexcept;

// Active only during post-effect execution, never during plan validation.
class TextPostEffectParameterConsumptionScope final {
public:
  TextPostEffectParameterConsumptionScope(
      const text::TextEffectPostEffectNode &effect, bool enabled);
  ~TextPostEffectParameterConsumptionScope();
  TextPostEffectParameterConsumptionScope(
      const TextPostEffectParameterConsumptionScope &) = delete;
  TextPostEffectParameterConsumptionScope &operator=(
      const TextPostEffectParameterConsumptionScope &) = delete;
  static void Record(const text::TextEffectPostEffectNode &effect,
                     const text::TextPostEffectParameter &parameter) noexcept;
  void Finish(text::TextNamedParameterConsumptionEvidence &evidence,
              bool completed) const;

private:
  static thread_local TextPostEffectParameterConsumptionScope *active_;
  TextPostEffectParameterConsumptionScope *previous_{nullptr};
  const text::TextEffectPostEffectNode &effect_;
  bool enabled_{false};
  std::vector<bool> consumed_;
};

} // namespace videocut::skia_runtime::internal
