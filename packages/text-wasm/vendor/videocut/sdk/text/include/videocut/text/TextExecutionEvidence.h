#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videocut::text {

struct TextExecutedPostEffectPass final {
    std::string chain;
    std::string stage;
    std::size_t ordinal{0};
    std::size_t passCount{0};
    int width{0};
    int height{0};
    std::uint32_t surfaceSemantics{0};
    std::string executor;
    std::uint32_t executorVersion{0};
    std::string pixelFormat;
};

struct TextNamedParameterConsumptionEvidence final {
    bool recorded{false};
    std::size_t declaredParameterCount{0};
    std::size_t consumedParameterCount{0};
    std::vector<std::string> unconsumedParameters;
    bool verified{false};
};

struct TextPostEffectExecutionEvidence final {
    std::string nodeId;
    std::uint64_t renderGroupInstanceId{0};
    std::size_t plannedPassCount{0};
    int pageWidth{0};
    int pageHeight{0};
    bool topologyValidated{false};
    std::string error;
    std::vector<TextExecutedPostEffectPass> passes;
    TextNamedParameterConsumptionEvidence parameters;
};

struct TextExecutionParameterIdentity final {
    std::uint32_t kind{0};
    std::uint32_t domain{0};
    std::uint32_t slot{0};
    std::optional<std::uint64_t> stableUnitId;
};

struct TextExecutionNodeEvidence final {
    std::string nodeId;
    bool active{false};
    /// Collected after the existing node branch finishes. An inactive node
    /// forwards its input; it must not be described as an executed effect.
    bool completed{false};
    /// Admission is not consumption evidence. Consumers must not infer zero
    /// unconsumed parameters from this count.
    std::size_t declaredParameterCount{0};
    std::size_t consumedParameterCount{0};
    std::vector<TextExecutionParameterIdentity> unconsumedParameters;
    bool parameterConsumptionVerified{false};
};

struct TextParticleExecutionEvidence final {
    std::string nodeId;
    float timeSeconds{0};
    std::size_t activeInstances{0};
    std::size_t withinCycle{0};
    std::size_t alive{0};
    std::size_t sourceCovered{0};
    std::size_t withinFrontier{0};
    std::size_t submittedDraws{0};
    /// Bounds intersection is not pixel coverage or a visual acceptance.
    std::size_t intersectingDrawBounds{0};
};

/// Emitted only after the native interpreter has executed a complete stage
/// for nonempty input. This proves evaluation, not downstream pixel influence.
struct TextProgramStageExecutionEvidence final {
    std::string stageId;
    std::size_t evaluatedUnits{0};
    std::size_t evaluatedInstructions{0};
    std::size_t appliedOutputs{0};
    std::size_t publishedBindings{0};
    std::vector<std::string> capabilities;
};

struct TextProgramExecutionEvidence final {
    std::string programId;
    bool completed{false};
    std::vector<TextProgramStageExecutionEvidence> stages;
};

/// Successful GPU material submissions, grouped without retaining resources
/// or reading pixels back. A submitted draw is not a visibility assertion.
struct TextMaterialExecutionEvidence final {
    std::string layerId;
    std::string capability;
    std::string executor;
    std::size_t submittedDraws{0};
};

/// Successful native sampling followed by projection into the frame plan.
/// Counts identify executed authored inputs; they do not assert pixel influence.
struct TextAnimatorExecutionEvidence final {
    std::string animatorId;
    std::size_t sampledGroups{0};
    std::size_t projectedUnits{0};
    std::size_t sampledSelectors{0};
    std::size_t sampledTracks{0};
};

struct TextAnimationLayerExecutionEvidence final {
    std::string layerId;
    double progress{0.0};
    std::size_t targetUnits{0};
    bool renderGroupBoundsResolved{false};
    std::size_t projectedDecorations{0};
    std::vector<std::string> sampledTimedSpanIds;
    std::vector<TextAnimatorExecutionEvidence> animators;
};

/// Optional CPU-side metadata from the existing render graph and quantized
/// post-effect boundaries. It neither reads GPU pixels nor covers every
/// material/shaping/parameter consumer in the renderer.
struct TextExecutionEvidence final {
    bool completed{false};
    bool frameCacheReused{false};
    /// Covers frame-plan execution-node samples only, not independently
    /// authored post-effect parameters or every renderer capability.
    bool executionGraphParameterConsumptionVerified{false};
    std::vector<TextExecutionNodeEvidence> nodes;
    std::vector<TextPostEffectExecutionEvidence> postEffects;
    std::vector<TextParticleExecutionEvidence> particles;
    std::vector<TextProgramExecutionEvidence> programs;
    std::vector<TextMaterialExecutionEvidence> materials;
    std::vector<TextAnimationLayerExecutionEvidence> animationLayers;
};

} // namespace videocut::text
