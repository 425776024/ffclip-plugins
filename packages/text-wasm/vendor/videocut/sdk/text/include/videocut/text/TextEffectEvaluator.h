#pragma once

#include "videocut/text/TextEffectFramePlan.h"
#include "videocut/text/TextEffectProgramIR.h"
#include "videocut/text/TextExecutionEvidence.h"

#include <cstdint>
#include <string>
#include <vector>

namespace videocut::text {

/// Snapshot-admitted metadata needed to resolve animated decorations without
/// consulting a package, filesystem or renderer cache during frame sampling.
struct TextEffectEvaluationResource final {
  std::string resourceId;
  std::string assetId;
  std::string digest;
  float intrinsicWidth{0.0F};
  float intrinsicHeight{0.0F};
  std::int64_t durationUs{0};
};

struct TextEffectEvaluationStateInput final {
  std::string stateId;
  std::uint64_t revision{0U};
  std::int64_t elapsedUs{0};
  std::uint64_t randomSeed{0U};
};

struct TextEffectEvaluationHistoryInput final {
  std::string historyId;
  std::uint64_t revision{0U};
};

/// Complete immutable input for one clip-local absolute-time sample. The
/// caller retains ownership of authored animation/timed-span vectors for the
/// duration of EvaluateTextEffects; the evaluator stores no references.
struct TextEffectEvaluationRequest final {
  const TextAnimationStack *animations{nullptr};
  const std::vector<TimedTextSpan> *timedSpans{nullptr};
  TextEffectFrameInput frame{};
  ReferenceCanvas referenceCanvas{};
  std::vector<TextEffectEvaluationResource> resources;
  TextEffectFrameBounds bounds{};
  TextEffectFrameCacheIdentities cacheIdentities{};
  std::int64_t clipLocalTimeUs{0};
  std::int64_t clipDurationUs{0};
  /// Fixed source-design -> reference-canvas bridge for effect-program
  /// distances and matrices. The renderer must not project them a second time.
  float sourceToReferenceScale{1.0F};
  /// Native Letter component that owns Effect Program scalar inputs. SDFText
  /// exposes its writer font scalar; legacy text keeps the reference-canvas
  /// font-size domain.
  TextSourceCreationComponent sourceCreationComponent{
      TextSourceCreationComponent::LegacyText};
  bool suppressAnimation{false};
  std::vector<TextEffectEvaluationStateInput> stateInputs;
  std::vector<TextEffectEvaluationHistoryInput> historyInputs;
  /// Optional caller-owned diagnostics; absent during normal playback.
  TextExecutionEvidence *executionEvidence{nullptr};
};

struct TextEffectEvaluationResult final {
  bool valid{false};
  TextEffectFramePlan framePlan{};
  std::vector<Diagnostic> diagnostics;
};

/// Resolves authored components into the sole column-major transform consumed
/// by every renderer. Bounds use the same top-left text-local coordinate
/// domain as TextEffectFrameInput.
std::optional<TextEffectTransformPlan> ResolveTextEffectTransformPlan(
    const TextEffectTransformComponents &components,
    const TextEffectRect &tightAnchorBounds,
    const TextEffectRect &layoutAnchorBounds,
    const TextEffectRect &projectionBounds) noexcept;

/// Conjugates a centered, Y-up Qt Effect Program matrix into the top-left,
/// Y-down reference-canvas domain used by layout and renderer execution.
bool ProjectTextEffectTransformPlanToReference(
    TextEffectTransformPlan &plan, float sourceToReferenceScale,
    const ReferenceCanvas &referenceCanvas) noexcept;

/// Re-resolves only decomposed Qt Effect Program transforms against final
/// shaped geometry. Ordinary animator and direct-matrix plans are untouched.
bool RetargetTextEffectProgramTransforms(
    TextEffectFramePlan &plan, const TextEffectFrameInput &finalFrame,
    const ReferenceCanvas &referenceCanvas, float sourceToReferenceScale,
    std::string &error) noexcept;

/// Composes transforms in authored order: each later transform observes the
/// geometry produced by the preceding transform.
std::optional<TextEffectMatrix4x4> ComposeTextEffectTransforms(
    const std::vector<TextEffectTransformPlan> &transforms) noexcept;

bool EvaluateTextEffectProgram(const TextEffectProgramIR &program,
                               const TextEffectFrameInput &input,
                               TextEffectFramePlan &output,
                               std::string &error,
                               TextProgramExecutionEvidence *evidence = nullptr) noexcept;

/// Sole authored animation/program sampler. It is deterministic for an
/// absolute clip-local time and never reads package data, a wall clock,
/// renderer state or previous-frame history.
TextEffectEvaluationResult
EvaluateTextEffects(const TextEffectEvaluationRequest &request) noexcept;

} // namespace videocut::text
