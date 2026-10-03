#include "videocut/text/TextEffectEvaluator.h"
#include "TextAnimatorSampler.h"
#include "TextEffectEllipticLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace videocut::text {
namespace {

constexpr std::size_t kMaximumFrameUnits = 65'536U;
constexpr std::size_t kMaximumExecutionParameterSamples = 1'048'576U;

using ColumnUnitCountMap = std::unordered_map<std::size_t, std::size_t>;
using RowUnitCountMap = std::unordered_map<std::size_t, std::size_t>;
struct RowInitialGeometry final {
  double minimumX{std::numeric_limits<double>::infinity()};
  double maximumX{-std::numeric_limits<double>::infinity()};
  double left{std::numeric_limits<double>::infinity()};
  double right{-std::numeric_limits<double>::infinity()};
  double bottom{std::numeric_limits<double>::infinity()};
  double top{-std::numeric_limits<double>::infinity()};
};
using RowInitialGeometryMap =
    std::unordered_map<std::size_t, RowInitialGeometry>;

struct SequentialRandomContext final {
  std::uint32_t state{1U};
  std::vector<double> samples;
  std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> shuffleRanks;
};

struct CurrentPositionState final {
  double x{0.0};
  double y{0.0};
  bool valid{false};
};

struct TransformAccumulator final {
  TextEffectTransformComponents components{};
  TextEffectMatrix4x4 directMatrix{};
  bool hasDecomposedOutput{false};
  bool hasDirectMatrixOutput{false};
};

using Matrix4 = std::array<float, 16>;

float StoreQtTextBinary32(const float value) noexcept {
  volatile float stored = value;
  return stored;
}

float QtTextBinary32Add(const float left, const float right) noexcept {
  return StoreQtTextBinary32(StoreQtTextBinary32(left) +
                             StoreQtTextBinary32(right));
}

float QtTextBinary32Subtract(const float left, const float right) noexcept {
  return StoreQtTextBinary32(StoreQtTextBinary32(left) -
                             StoreQtTextBinary32(right));
}

float QtTextBinary32Multiply(const float left, const float right) noexcept {
  return StoreQtTextBinary32(StoreQtTextBinary32(left) *
                             StoreQtTextBinary32(right));
}

float QtTextBinary32Divide(const float numerator,
                           const float denominator) noexcept {
  return StoreQtTextBinary32(StoreQtTextBinary32(numerator) /
                             StoreQtTextBinary32(denominator));
}

float QtTextDegreesToRadians(const float degrees) noexcept {
  constexpr float kPi = 3.14159265358979323846F;
  const float halfTurns = QtTextBinary32Divide(degrees, 360.0F);
  return QtTextBinary32Multiply(
      QtTextBinary32Add(halfTurns, halfTurns), kPi);
}

Matrix4 MultiplyMatrix(const Matrix4 &left, const Matrix4 &right) noexcept {
  Matrix4 result{};
  for (std::size_t column = 0U; column < 4U; ++column) {
    for (std::size_t row = 0U; row < 4U; ++row) {
      float value = QtTextBinary32Multiply(
          left[row], right[column * 4U]);
      value = QtTextBinary32Add(
          value, QtTextBinary32Multiply(left[4U + row],
                                        right[column * 4U + 1U]));
      value = QtTextBinary32Add(
          value, QtTextBinary32Multiply(left[8U + row],
                                        right[column * 4U + 2U]));
      value = QtTextBinary32Add(
          value, QtTextBinary32Multiply(left[12U + row],
                                        right[column * 4U + 3U]));
      result[column * 4U + row] = value;
    }
  }
  return result;
}

Matrix4 ConcatQtTextMatrix(const Matrix4 &left,
                           const Matrix4 &right) noexcept {
  Matrix4 result{};
  for (std::size_t column = 0U; column < 4U; ++column) {
    for (std::size_t row = 0U; row < 4U; ++row) {
      float value = QtTextBinary32Multiply(
          left[12U + row], right[column * 4U + 3U]);
      value = QtTextBinary32Add(
          QtTextBinary32Multiply(left[8U + row],
                                 right[column * 4U + 2U]),
          value);
      value = QtTextBinary32Add(
          QtTextBinary32Multiply(left[4U + row],
                                 right[column * 4U + 1U]),
          value);
      result[column * 4U + row] = QtTextBinary32Add(
          QtTextBinary32Multiply(left[row], right[column * 4U]), value);
    }
  }
  return result;
}

Matrix4 TranslationMatrix(const float x, const float y,
                          const float z) noexcept {
  Matrix4 result{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                 0.0F, 0.0F, 1.0F, 0.0F, x,    y,    z,    1.0F};
  result[12] = StoreQtTextBinary32(x);
  result[13] = StoreQtTextBinary32(y);
  result[14] = StoreQtTextBinary32(z);
  return result;
}

Matrix4 ScaleMatrix(const float x, const float y, const float z) noexcept {
  return {StoreQtTextBinary32(x), 0.0F, 0.0F, 0.0F,
          0.0F, StoreQtTextBinary32(y), 0.0F, 0.0F,
          0.0F, 0.0F, StoreQtTextBinary32(z), 0.0F,
          0.0F, 0.0F, 0.0F, 1.0F};
}

Matrix4 RotationXMatrix(const float radians) noexcept {
  const float cosine = StoreQtTextBinary32(std::cos(radians));
  const float sine = StoreQtTextBinary32(std::sin(radians));
  return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, cosine, sine, 0.0F,
          0.0F, -sine, cosine, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

Matrix4 RotationYMatrix(const float radians) noexcept {
  const float cosine = StoreQtTextBinary32(std::cos(radians));
  const float sine = StoreQtTextBinary32(std::sin(radians));
  return {cosine, 0.0F, -sine, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
          sine,   0.0F, cosine, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

Matrix4 RotationZMatrix(const float radians) noexcept {
  const float cosine = StoreQtTextBinary32(std::cos(radians));
  const float sine = StoreQtTextBinary32(std::sin(radians));
  return {cosine, sine, 0.0F, 0.0F, -sine, cosine, 0.0F, 0.0F,
          0.0F,   0.0F, 1.0F, 0.0F, 0.0F,  0.0F,   0.0F, 1.0F};
}

Matrix4 ShearMatrix(const float x, const float y) noexcept {
  return {1.0F, StoreQtTextBinary32(y), 0.0F, 0.0F,
          StoreQtTextBinary32(x), 1.0F, 0.0F, 0.0F,
          0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

bool FiniteRect(const TextEffectRect &rect) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
         std::isfinite(rect.width) && std::isfinite(rect.height) &&
         rect.width >= 0.0F && rect.height >= 0.0F;
}

bool NonEmptyRect(const TextEffectRect &rect) noexcept {
  return FiniteRect(rect) && rect.width > 0.0F && rect.height > 0.0F;
}

TextEffectRect UnionRect(const TextEffectRect &left,
                         const TextEffectRect &right) noexcept {
  if (!NonEmptyRect(left))
    return right;
  if (!NonEmptyRect(right))
    return left;
  const float minimumX = std::min(left.x, right.x);
  const float minimumY = std::min(left.y, right.y);
  const float maximumX =
      std::max(left.x + left.width, right.x + right.width);
  const float maximumY =
      std::max(left.y + left.height, right.y + right.height);
  return {minimumX, minimumY, maximumX - minimumX, maximumY - minimumY};
}

TextEffectRect AnchorBounds(const TextEffectFrameInput &input,
                            const std::size_t begin, const std::size_t end,
                            const bool tight) noexcept {
  TextEffectRect result{};
  for (auto index = begin; index < end && index < input.units.size(); ++index) {
    const auto &unit = input.units[index];
    const auto &candidate =
        tight && NonEmptyRect(unit.tightRect) ? unit.tightRect : unit.rect;
    result = UnionRect(result, candidate);
  }
  return result;
}

bool IsNormalUnitTopologyInput(const TextEffectInput input) noexcept {
  return input == TextEffectInput::IsNormalUnit ||
         input == TextEffectInput::NormalUnitIndex ||
         input == TextEffectInput::NormalUnitCount;
}

bool IsCurrentPositionInput(const TextEffectInput input) noexcept {
  return input == TextEffectInput::CurrentPositionX ||
         input == TextEffectInput::CurrentPositionY ||
         input == TextEffectInput::PreviousCurrentPositionX ||
         input == TextEffectInput::PreviousCurrentPositionY ||
         input == TextEffectInput::NextCurrentPositionX ||
         input == TextEffectInput::NextCurrentPositionY ||
         input == TextEffectInput::HasCurrentPosition ||
         input == TextEffectInput::HasPreviousCurrentPosition ||
         input == TextEffectInput::HasNextCurrentPosition;
}

template <typename Predicate>
bool ProgramUsesInput(const TextEffectProgramIR &program,
                      Predicate predicate) noexcept {
  return std::any_of(
      program.stages.begin(), program.stages.end(),
      [&](const TextEffectProgramStageIR &stage) noexcept {
        return std::any_of(
            stage.instructions.begin(), stage.instructions.end(),
            [&](const TextEffectInstruction &instruction) noexcept {
              return instruction.input && predicate(*instruction.input);
            });
      });
}

bool StagePublishesCurrentPosition(
    const TextEffectProgramStageIR &stage) noexcept {
  return std::any_of(
      stage.outputs.begin(), stage.outputs.end(),
      [](const TextEffectOutputBinding &binding) noexcept {
        return binding.output == TextEffectOutput::CurrentPositionX ||
               binding.output == TextEffectOutput::CurrentPositionY ||
               binding.output == TextEffectOutput::CurrentPositionValid;
      });
}

void SetEvaluationError(std::string &error,
                        const std::string_view message) noexcept {
  try {
    error.assign(message.data(), message.size());
  } catch (...) {
    error.clear();
  }
}

void ResetFramePlan(TextEffectFramePlan &output) noexcept {
  output.programId.clear();
  output.sourceCreationComponent = TextSourceCreationComponent::LegacyText;
  output.sampledProperties.clear();
  output.units.clear();
  output.layoutMutations.clear();
  output.glyphMaterialPasses.clear();
  output.backdropPasses.clear();
  output.decorationPasses.clear();
  output.postEffectNodes.clear();
  output.compositeOrder.clear();
  output.resources.clear();
  output.stateTransitions.clear();
  output.executionParameters.clear();
  output.executionGraph.nodes.clear();
  output.bounds = {};
  output.cacheIdentities = {};
}

void StoreQtTextEffectFramePlanBinary32(
    TextEffectFramePlan &framePlan) noexcept {
  const auto storeColor = [](Color &color) noexcept {
    color.red = StoreQtTextBinary32(color.red);
    color.green = StoreQtTextBinary32(color.green);
    color.blue = StoreQtTextBinary32(color.blue);
    color.alpha = StoreQtTextBinary32(color.alpha);
  };
  for (auto &unit : framePlan.units) {
    if (unit.opacity)
      *unit.opacity = StoreQtTextBinary32(*unit.opacity);
    if (unit.instanceColor)
      storeColor(*unit.instanceColor);
    if (unit.absoluteFontSize)
      *unit.absoluteFontSize = StoreQtTextBinary32(*unit.absoluteFontSize);
    if (unit.sdfBlurRadius)
      *unit.sdfBlurRadius = StoreQtTextBinary32(*unit.sdfBlurRadius);
    for (auto &transform : unit.transforms) {
      for (auto &component : transform.localToText.columnMajor)
        component = StoreQtTextBinary32(component);
      transform.tightAnchorBounds.x =
          StoreQtTextBinary32(transform.tightAnchorBounds.x);
      transform.tightAnchorBounds.y =
          StoreQtTextBinary32(transform.tightAnchorBounds.y);
      transform.tightAnchorBounds.width =
          StoreQtTextBinary32(transform.tightAnchorBounds.width);
      transform.tightAnchorBounds.height =
          StoreQtTextBinary32(transform.tightAnchorBounds.height);
      transform.layoutAnchorBounds.x =
          StoreQtTextBinary32(transform.layoutAnchorBounds.x);
      transform.layoutAnchorBounds.y =
          StoreQtTextBinary32(transform.layoutAnchorBounds.y);
      transform.layoutAnchorBounds.width =
          StoreQtTextBinary32(transform.layoutAnchorBounds.width);
      transform.layoutAnchorBounds.height =
          StoreQtTextBinary32(transform.layoutAnchorBounds.height);
    }
  }
  for (auto &mutation : framePlan.layoutMutations)
    mutation.value = StoreQtTextBinary32(mutation.value);
  for (auto &parameter : framePlan.executionParameters) {
    for (auto &component : parameter.values)
      component = StoreQtTextBinary32(component);
  }
}

bool BuildQtBinary32TextEffectProgram(
    const TextEffectProgramIR &source, TextEffectProgramIR &binary32Program,
    std::string &error) {
  constexpr std::uint32_t kTextEffectRegisterBudget = 512U;
  binary32Program = {};
  error.clear();
  try {
    binary32Program.programId = source.programId;
    binary32Program.randomSeed = source.randomSeed;
    binary32Program.stages.reserve(source.stages.size());
    for (const auto &sourceStage : source.stages) {
      const auto instructionCount = sourceStage.instructions.size();
      if (instructionCount > kTextEffectRegisterBudget / 2U) {
        error = "text effect binary32 lowering exceeds register budget";
        binary32Program = {};
        return false;
      }
      TextEffectProgramStageIR stage;
      stage.stageId = sourceStage.stageId;
      stage.kind = sourceStage.kind;
      stage.instructions.reserve(instructionCount * 2U);
      stage.outputs = sourceStage.outputs;
      stage.executionParameterBindings =
          sourceStage.executionParameterBindings;
      std::vector<std::uint32_t> loweredRegisters(
          sourceStage.registerCount,
          std::numeric_limits<std::uint32_t>::max());
      for (const auto &sourceInstruction : sourceStage.instructions) {
        if (sourceInstruction.outputRegister >= loweredRegisters.size()) {
          error = "text effect binary32 lowering found invalid destination";
          binary32Program = {};
          return false;
        }
        auto instruction = sourceInstruction;
        for (auto &inputRegister : instruction.inputRegisters) {
          if (inputRegister >= loweredRegisters.size() ||
              loweredRegisters[inputRegister] ==
                  std::numeric_limits<std::uint32_t>::max()) {
            error = "text effect binary32 lowering found invalid dataflow";
            binary32Program = {};
            return false;
          }
          inputRegister = loweredRegisters[inputRegister];
        }
        if (instruction.opcode == TextEffectOpcode::SeededRandom &&
            instruction.immediates.empty()) {
          instruction.immediates.push_back(
              static_cast<double>(sourceInstruction.outputRegister));
        } else if ((instruction.opcode ==
                        TextEffectOpcode::SampleHoldNoise ||
                    instruction.opcode ==
                        TextEffectOpcode::PageSampleHoldNoise) &&
                   instruction.immediates.size() == 1U) {
          instruction.immediates.push_back(
              static_cast<double>(sourceInstruction.outputRegister));
        }
        const auto rawRegister =
            static_cast<std::uint32_t>(stage.instructions.size());
        instruction.outputRegister = rawRegister;
        stage.instructions.push_back(std::move(instruction));

        TextEffectInstruction store;
        store.opcode = TextEffectOpcode::Float32;
        store.outputRegister =
            static_cast<std::uint32_t>(stage.instructions.size());
        store.inputRegisters.push_back(rawRegister);
        loweredRegisters[sourceInstruction.outputRegister] =
            store.outputRegister;
        stage.instructions.push_back(std::move(store));
      }
      stage.registerCount =
          static_cast<std::uint32_t>(stage.instructions.size());
      for (auto &output : stage.outputs) {
        if (output.registerIndex >= loweredRegisters.size() ||
            loweredRegisters[output.registerIndex] ==
                std::numeric_limits<std::uint32_t>::max()) {
          error = "text effect binary32 lowering found invalid output";
          binary32Program = {};
          return false;
        }
        output.registerIndex = loweredRegisters[output.registerIndex];
      }
      for (auto &binding : stage.executionParameterBindings) {
        for (auto &registerIndex : binding.registerIndices) {
          if (registerIndex >= loweredRegisters.size() ||
              loweredRegisters[registerIndex] ==
                  std::numeric_limits<std::uint32_t>::max()) {
            error =
                "text effect binary32 lowering found invalid execution parameter";
            binary32Program = {};
            return false;
          }
          registerIndex = loweredRegisters[registerIndex];
        }
      }
      binary32Program.stages.push_back(std::move(stage));
    }
  } catch (...) {
    error = "text effect binary32 lowering allocation failed";
    binary32Program = {};
    return false;
  }
  if (!ValidateTextEffectProgramIR(binary32Program, &error)) {
    binary32Program = {};
    return false;
  }
  return true;
}

std::uint64_t MixBits(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

std::uint64_t DoubleBits(const double value) noexcept {
  std::uint64_t result = 0U;
  static_assert(sizeof(result) == sizeof(value));
  const double canonicalValue = value == 0.0 ? 0.0 : value;
  std::memcpy(&result, &canonicalValue, sizeof(result));
  return result;
}

double UnitRandom(const TextEffectProgramIR &program,
                  const TextEffectUnitInput &unit, const double seed,
                  const double channel) noexcept {
  const auto bits =
      MixBits(program.randomSeed ^ MixBits(unit.stableUnitId) ^
              MixBits(DoubleBits(seed)) ^ MixBits(DoubleBits(channel)));
  return static_cast<double>(bits >> 11U) * (1.0 / 9007199254740992.0);
}

double PageRandom(const TextEffectProgramIR &program, const double seed,
                  const double channel) noexcept {
  constexpr std::uint64_t kPageDomain = 0x706167652d726e67ULL;
  const auto bits =
      MixBits(program.randomSeed ^ kPageDomain ^ MixBits(DoubleBits(seed)) ^
              MixBits(DoubleBits(channel)));
  return static_cast<double>(bits >> 11U) * (1.0 / 9007199254740992.0);
}

constexpr std::uint32_t kParkMillerModulus = 2'147'483'647U;
constexpr std::uint32_t kParkMillerMultiplier = 16'807U;

std::uint32_t NormalizeParkMillerSeed(const std::uint64_t seed) noexcept {
  const auto normalized =
      static_cast<std::uint32_t>(seed % kParkMillerModulus);
  return normalized == 0U ? 1U : normalized;
}

double NextSequentialRandom(SequentialRandomContext &context) noexcept {
  context.state = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(context.state) * kParkMillerMultiplier) %
      kParkMillerModulus);
  // Lua 5.1 normalizes rand() % RAND_MAX by RAND_MAX. Park-Miller produces
  // states in [1, RAND_MAX), so the result is strictly below one.
  return static_cast<double>(context.state) /
         static_cast<double>(kParkMillerModulus);
}

bool ResolveSequentialRandom(SequentialRandomContext &context,
                             const double callIndexValue,
                             double &value) noexcept {
  constexpr double kMaximumExactInteger = 9'007'199'254'740'991.0;
  if (!std::isfinite(callIndexValue) || callIndexValue < 0.0 ||
      callIndexValue > kMaximumExactInteger ||
      std::floor(callIndexValue) != callIndexValue) {
    return false;
  }
  const auto callIndex = static_cast<std::uint64_t>(callIndexValue);
  // A text program is bounded to the frame-unit budget; reject pathological
  // authored indexes rather than allowing an unbounded sequence expansion.
  constexpr std::uint64_t kMaximumSequentialCalls =
      static_cast<std::uint64_t>(kMaximumFrameUnits) * 4U;
  if (callIndex >= kMaximumSequentialCalls)
    return false;
  try {
    while (context.samples.size() <= callIndex)
      context.samples.push_back(NextSequentialRandom(context));
  } catch (...) {
    return false;
  }
  value = context.samples[static_cast<std::size_t>(callIndex)];
  return true;
}

bool ResolveSeededShuffleRank(SequentialRandomContext &context,
                              const double unitIndexValue,
                              const double unitCountValue,
                              const double startCallIndexValue,
                              double &value) noexcept {
  if (!std::isfinite(unitIndexValue) || !std::isfinite(unitCountValue) ||
      !std::isfinite(startCallIndexValue) ||
      unitIndexValue < 0.0 || unitCountValue < 1.0 ||
      startCallIndexValue < 0.0 ||
      std::floor(unitIndexValue) != unitIndexValue ||
      std::floor(unitCountValue) != unitCountValue ||
      std::floor(startCallIndexValue) != startCallIndexValue ||
      unitCountValue > static_cast<double>(kMaximumFrameUnits) ||
      unitIndexValue >= unitCountValue) {
    return false;
  }
  const auto unitIndex = static_cast<std::size_t>(unitIndexValue);
  const auto unitCount = static_cast<std::size_t>(unitCountValue);
  const auto startCallIndex =
      static_cast<std::uint64_t>(startCallIndexValue);
  constexpr std::uint64_t kMaximumSequentialCalls =
      static_cast<std::uint64_t>(kMaximumFrameUnits) * 4U;
  if (startCallIndex >= kMaximumSequentialCalls ||
      unitCount - 1U > kMaximumSequentialCalls - startCallIndex) {
    return false;
  }
  try {
    const auto cacheKey =
        (startCallIndex << 32U) | static_cast<std::uint64_t>(unitCount);
    auto found = context.shuffleRanks.find(cacheKey);
    if (found == context.shuffleRanks.end()) {
      std::vector<std::uint32_t> ranks(unitCount);
      for (std::size_t index = 0U; index < unitCount; ++index)
        ranks[index] = static_cast<std::uint32_t>(index);
      for (std::size_t right = 1U; right < unitCount; ++right) {
        double random = 0.0;
        if (!ResolveSequentialRandom(context,
                                     static_cast<double>(startCallIndex +
                                                         right - 1U),
                                     random)) {
          return false;
        }
        const auto left = std::min(
            static_cast<std::size_t>(
                std::floor(random * static_cast<double>(right + 1U))),
            right);
        std::swap(ranks[left], ranks[right]);
      }
      found =
          context.shuffleRanks.emplace(cacheKey, std::move(ranks)).first;
    }
    value = static_cast<double>(found->second[unitIndex]);
  } catch (...) {
    return false;
  }
  return true;
}

bool ResolveSeededReverseShuffleRank(SequentialRandomContext &context,
                                     const double listIndexValue,
                                     const double unitCountValue,
                                     const double startCallIndexValue,
                                     double &value) noexcept {
  if (!std::isfinite(listIndexValue) || !std::isfinite(unitCountValue) ||
      !std::isfinite(startCallIndexValue) || listIndexValue < 0.0 ||
      unitCountValue < 1.0 || startCallIndexValue < 0.0 ||
      std::floor(listIndexValue) != listIndexValue ||
      std::floor(unitCountValue) != unitCountValue ||
      std::floor(startCallIndexValue) != startCallIndexValue ||
      unitCountValue > static_cast<double>(kMaximumFrameUnits) ||
      listIndexValue >= unitCountValue) {
    return false;
  }
  const auto listIndex = static_cast<std::size_t>(listIndexValue);
  const auto unitCount = static_cast<std::size_t>(unitCountValue);
  const auto startCallIndex = static_cast<std::uint64_t>(startCallIndexValue);
  constexpr std::uint64_t kMaximumSequentialCalls =
      static_cast<std::uint64_t>(kMaximumFrameUnits) * 4U;
  if (startCallIndex >= kMaximumSequentialCalls ||
      unitCount > kMaximumSequentialCalls - startCallIndex) {
    return false;
  }
  try {
    constexpr std::uint64_t kReverseShuffleCacheBit = 1ULL << 63U;
    const auto cacheKey = kReverseShuffleCacheBit |
                          (startCallIndex << 32U) |
                          static_cast<std::uint64_t>(unitCount);
    auto found = context.shuffleRanks.find(cacheKey);
    if (found == context.shuffleRanks.end()) {
      std::vector<std::uint32_t> ranks(unitCount);
      for (std::size_t index = 0U; index < unitCount; ++index)
        ranks[index] = static_cast<std::uint32_t>(index);
      for (std::size_t remaining = unitCount; remaining > 0U; --remaining) {
        double random = 0.0;
        if (!ResolveSequentialRandom(
                context,
                static_cast<double>(startCallIndex + unitCount - remaining),
                random)) {
          return false;
        }
        const auto selected = std::min(
            static_cast<std::size_t>(
                std::floor(random * static_cast<double>(remaining))),
            remaining - 1U);
        std::swap(ranks[selected], ranks[remaining - 1U]);
      }
      found = context.shuffleRanks.emplace(cacheKey, std::move(ranks)).first;
    }
    value = static_cast<double>(found->second[listIndex]);
  } catch (...) {
    return false;
  }
  return true;
}

bool ResolveDurationLane(const double laneValue, const std::size_t laneCount,
                         std::size_t &lane) noexcept {
  if (!std::isfinite(laneValue) || laneValue < 0.0 ||
      laneValue > static_cast<double>(laneCount) ||
      std::floor(laneValue) != laneValue) {
    return false;
  }
  lane = static_cast<std::size_t>(laneValue);
  return true;
}

double DurationPrefix(const std::vector<double> &durations,
                      const std::size_t lane) noexcept {
  double prefix = 0.0;
  for (std::size_t index = 0U; index < lane; ++index)
    prefix += durations[index];
  return prefix;
}

template <typename Scalar>
Scalar CubicBezierCoordinate(const Scalar q, const Scalar second,
                              const Scalar third) noexcept {
  if constexpr (std::is_same_v<Scalar, double>) {
    // Preserve the source controller's binary64 expression order.
    return 3.0 * second * (1.0 - q) * (1.0 - q) * q +
           3.0 * third * (1.0 - q) * q * q + q * q * q;
  }
  const float p = QtTextBinary32Subtract(1.0F, q);
  const float p2 = QtTextBinary32Multiply(p, p);
  const float q2 = QtTextBinary32Multiply(q, q);
  const float q3 = QtTextBinary32Multiply(q2, q);
  float value = QtTextBinary32Multiply(3.0F, second);
  value = QtTextBinary32Multiply(value, p2);
  value = QtTextBinary32Multiply(value, q);
  float thirdTerm = QtTextBinary32Multiply(3.0F, third);
  thirdTerm = QtTextBinary32Multiply(thirdTerm, p);
  thirdTerm = QtTextBinary32Multiply(thirdTerm, q2);
  return QtTextBinary32Add(QtTextBinary32Add(value, thirdTerm), q3);
}

template <typename Scalar>
double EvaluateCubicBezierTimingInSourcePrecision(
    const double x, const double x1, const double y1,
    const double x2, const double y2) noexcept {
  const auto round = [](const Scalar value) -> Scalar {
    if constexpr (std::is_same_v<Scalar, float>)
      return StoreQtTextBinary32(value);
    return value;
  };
  const Scalar targetX = round(static_cast<Scalar>(x));
  const Scalar controlX1 = round(static_cast<Scalar>(x1));
  const Scalar controlX2 = round(static_cast<Scalar>(x2));
  const Scalar tolerance = std::is_same_v<Scalar, double>
      ? static_cast<Scalar>(0.0001) : static_cast<Scalar>(0.00001F);
  Scalar lower = 0;
  Scalar upper = 1;
  do {
    const Scalar middle = round(round(lower + upper) * static_cast<Scalar>(0.5));
    const Scalar sampledX =
        CubicBezierCoordinate(middle, controlX1, controlX2);
    if (sampledX > targetX)
      upper = middle;
    else
      lower = middle;
  } while (round(upper - lower) >= tolerance);

  const Scalar t = round(round(lower + upper) * static_cast<Scalar>(0.5));
  return static_cast<double>(CubicBezierCoordinate(
      t, round(static_cast<Scalar>(y1)), round(static_cast<Scalar>(y2))));
}

double EvaluateCubicBezierTiming(const double x, const double x1,
                                 const double y1, const double x2,
                                 const double y2,
                                 const bool sourceBinary64 = false) noexcept {
  return sourceBinary64
      ? EvaluateCubicBezierTimingInSourcePrecision<double>(x, x1, y1, x2, y2)
      : EvaluateCubicBezierTimingInSourcePrecision<float>(x, x1, y1, x2, y2);
}

const TextEffectUnitInput *Neighbor(const TextEffectFrameInput &input,
                                    const std::size_t unitIndex,
                                    const int direction) noexcept {
  if (direction < 0)
    return unitIndex == 0U ? nullptr : &input.units[unitIndex - 1U];
  return unitIndex + 1U >= input.units.size() ? nullptr
                                              : &input.units[unitIndex + 1U];
}

double RectInput(const TextEffectRect &rect,
                 const TextEffectInput input) noexcept {
  switch (input) {
  case TextEffectInput::RectX:
  case TextEffectInput::RowRectX:
  case TextEffectInput::TextRectX:
  case TextEffectInput::PreviousRectX:
  case TextEffectInput::NextRectX:
    return rect.x;
  case TextEffectInput::RectY:
  case TextEffectInput::RowRectY:
  case TextEffectInput::TextRectY:
  case TextEffectInput::PreviousRectY:
  case TextEffectInput::NextRectY:
    return rect.y;
  case TextEffectInput::RectWidth:
  case TextEffectInput::RowRectWidth:
  case TextEffectInput::TextRectWidth:
  case TextEffectInput::PreviousRectWidth:
  case TextEffectInput::NextRectWidth:
    return rect.width;
  case TextEffectInput::RectHeight:
  case TextEffectInput::RowRectHeight:
  case TextEffectInput::TextRectHeight:
  case TextEffectInput::PreviousRectHeight:
  case TextEffectInput::NextRectHeight:
    return rect.height;
  default:
    return 0.0;
  }
}

double ResolveInput(const TextEffectFrameInput &frame,
                    const RowUnitCountMap &rowUnitCounts,
                    const ColumnUnitCountMap &columnUnitCounts,
                    const RowInitialGeometryMap &rowPositionSpans,
                    const std::array<float, 2> &maximumNormalUnitExtent,
                    const std::vector<CurrentPositionState> &currentPositions,
                    const std::size_t unitIndex,
                    const TextEffectInput input) noexcept {
  const auto &unit = frame.units[unitIndex];
  const auto *previous = Neighbor(frame, unitIndex, -1);
  const auto *next = Neighbor(frame, unitIndex, 1);
  const auto previousIndex = unitIndex == 0U ? unitIndex : unitIndex - 1U;
  const auto nextIndex =
      unitIndex + 1U >= frame.units.size() ? unitIndex : unitIndex + 1U;
  const auto rowRect = unit.row < frame.rowRects.size()
                           ? frame.rowRects[unit.row]
                           : TextEffectRect{};
  switch (input) {
  case TextEffectInput::Progress:
    return frame.progress;
  case TextEffectInput::TimeUs:
    return static_cast<double>(frame.timeUs);
  case TextEffectInput::SourceToOutputScaleX:
  case TextEffectInput::SourceToOutputScaleY:
    return frame.sourceToOutputScale
               ? (*frame.sourceToOutputScale)[
                     input == TextEffectInput::SourceToOutputScaleX ? 0U : 1U]
               : std::numeric_limits<double>::quiet_NaN();
  case TextEffectInput::SourceOriginOutputX:
  case TextEffectInput::SourceOriginOutputY:
    return frame.sourceOriginOutput
               ? (*frame.sourceOriginOutput)[
                     input == TextEffectInput::SourceOriginOutputX ? 0U : 1U]
               : std::numeric_limits<double>::quiet_NaN();
  case TextEffectInput::AnimationDurationUs:
    return frame.animationDurationUs && *frame.animationDurationUs > 0
               ? static_cast<double>(*frame.animationDurationUs)
               : std::numeric_limits<double>::quiet_NaN();
  case TextEffectInput::FirstUnitFontSize:
    return frame.units.front().fontSize;
  case TextEffectInput::FirstUnitColorRed:
    return frame.units.front().instanceColor.red;
  case TextEffectInput::FirstUnitColorGreen:
    return frame.units.front().instanceColor.green;
  case TextEffectInput::FirstUnitColorBlue:
    return frame.units.front().instanceColor.blue;
  case TextEffectInput::LastUnitColorRed:
    return frame.units.back().instanceColor.red;
  case TextEffectInput::LastUnitColorGreen:
    return frame.units.back().instanceColor.green;
  case TextEffectInput::LastUnitColorBlue:
    return frame.units.back().instanceColor.blue;
  case TextEffectInput::FirstUnitColorAlpha:
    return frame.units.front().instanceColor.alpha;
  case TextEffectInput::FirstUnitRectWidth:
    return frame.units.front().rect.width;
  case TextEffectInput::FirstUnitRectHeight:
    return frame.units.front().rect.height;
  case TextEffectInput::MaxNormalUnitRectWidth:
    return maximumNormalUnitExtent[0];
  case TextEffectInput::MaxNormalUnitRectHeight:
    return maximumNormalUnitExtent[1];
  case TextEffectInput::IsVerticalWriting:
    if (!frame.writingMode)
      return std::numeric_limits<double>::quiet_NaN();
    switch (*frame.writingMode) {
    case TextWritingMode::Horizontal:
      return 0.0;
    case TextWritingMode::VerticalRightToLeft:
    case TextWritingMode::VerticalLeftToRight:
      return 1.0;
    }
    return std::numeric_limits<double>::quiet_NaN();
  case TextEffectInput::RowInitialBoundsX:
  case TextEffectInput::RowInitialBoundsY:
  case TextEffectInput::RowInitialBoundsWidth:
  case TextEffectInput::RowInitialBoundsHeight: {
    const auto &bounds = rowPositionSpans.at(unit.row);
    if (input == TextEffectInput::RowInitialBoundsX) return bounds.left;
    if (input == TextEffectInput::RowInitialBoundsY) return bounds.bottom;
    if (input == TextEffectInput::RowInitialBoundsWidth)
      return bounds.right - bounds.left;
    return bounds.top - bounds.bottom;
  }
  case TextEffectInput::UnitIndex:
    return static_cast<double>(unit.index);
  case TextEffectInput::RenderIndex:
    return static_cast<double>(unit.renderIndex);
  case TextEffectInput::UnitType:
    return static_cast<double>(unit.type);
  case TextEffectInput::RowIndex:
    return static_cast<double>(unit.row);
  case TextEffectInput::IndexInRow:
    return static_cast<double>(unit.indexInRow);
  case TextEffectInput::UnitCount:
    return static_cast<double>(frame.units.size());
  case TextEffectInput::RowCount:
    return static_cast<double>(frame.rowRects.size());
  case TextEffectInput::FontSize:
    return unit.fontSize;
  case TextEffectInput::RectX:
  case TextEffectInput::RectY:
  case TextEffectInput::RectWidth:
  case TextEffectInput::RectHeight:
    return RectInput(unit.rect, input);
  case TextEffectInput::InitialPositionX:
    return unit.initialPositionX;
  case TextEffectInput::InitialPositionY:
    return unit.initialPositionY;
  case TextEffectInput::InstanceColorRed:
    return unit.instanceColor.red;
  case TextEffectInput::InstanceColorGreen:
    return unit.instanceColor.green;
  case TextEffectInput::InstanceColorBlue:
    return unit.instanceColor.blue;
  case TextEffectInput::InstanceColorAlpha:
    return unit.instanceColor.alpha;
  case TextEffectInput::RowRectX:
  case TextEffectInput::RowRectY:
  case TextEffectInput::RowRectWidth:
  case TextEffectInput::RowRectHeight:
    return RectInput(rowRect, input);
  case TextEffectInput::RowInitialPositionSpanX:
    if (const auto found = rowPositionSpans.find(unit.row);
        found != rowPositionSpans.end()) {
      return std::isfinite(found->second.minimumX)
                 ? found->second.maximumX - found->second.minimumX : 0.0;
    }
    return 0.0;
  case TextEffectInput::TextRectX:
  case TextEffectInput::TextRectY:
  case TextEffectInput::TextRectWidth:
  case TextEffectInput::TextRectHeight:
    return RectInput(frame.textRect, input);
  case TextEffectInput::CanvasRectWidth:
    return frame.canvasRect ? frame.canvasRect->width
                            : std::numeric_limits<double>::quiet_NaN();
  case TextEffectInput::CanvasRectHeight:
    return frame.canvasRect ? frame.canvasRect->height
                            : std::numeric_limits<double>::quiet_NaN();
  case TextEffectInput::OutputWidth:
  case TextEffectInput::OutputHeight:
    if (frame.outputSize) {
      const auto value = (*frame.outputSize)[
          input == TextEffectInput::OutputWidth ? 0U : 1U];
      if (std::isfinite(value) && value > 0.0F)
        return value;
    }
    return std::numeric_limits<double>::quiet_NaN();
  case TextEffectInput::HasPreviousUnit:
    return previous ? 1.0 : 0.0;
  case TextEffectInput::PreviousInitialPositionX:
    return previous ? previous->initialPositionX : unit.initialPositionX;
  case TextEffectInput::PreviousInitialPositionY:
    return previous ? previous->initialPositionY : unit.initialPositionY;
  case TextEffectInput::PreviousRectX:
  case TextEffectInput::PreviousRectY:
  case TextEffectInput::PreviousRectWidth:
  case TextEffectInput::PreviousRectHeight:
    return RectInput(previous ? previous->rect : unit.rect, input);
  case TextEffectInput::HasNextUnit:
    return next ? 1.0 : 0.0;
  case TextEffectInput::NextInitialPositionX:
    return next ? next->initialPositionX : unit.initialPositionX;
  case TextEffectInput::NextInitialPositionY:
    return next ? next->initialPositionY : unit.initialPositionY;
  case TextEffectInput::NextRectX:
  case TextEffectInput::NextRectY:
  case TextEffectInput::NextRectWidth:
  case TextEffectInput::NextRectHeight:
    return RectInput(next ? next->rect : unit.rect, input);
  case TextEffectInput::RowUnitCount:
    if (const auto found = rowUnitCounts.find(unit.row);
        found != rowUnitCounts.end()) {
      return static_cast<double>(found->second);
    }
    return 0.0;
  case TextEffectInput::ColumnUnitCount:
    if (const auto found = columnUnitCounts.find(unit.indexInRow);
        found != columnUnitCounts.end()) {
      return static_cast<double>(found->second);
    }
    return 0.0;
  case TextEffectInput::CurrentPositionX:
    return currentPositions[unitIndex].x;
  case TextEffectInput::CurrentPositionY:
    return currentPositions[unitIndex].y;
  case TextEffectInput::PreviousCurrentPositionX:
    return previous && currentPositions[previousIndex].valid
               ? currentPositions[previousIndex].x
               : currentPositions[unitIndex].x;
  case TextEffectInput::PreviousCurrentPositionY:
    return previous && currentPositions[previousIndex].valid
               ? currentPositions[previousIndex].y
               : currentPositions[unitIndex].y;
  case TextEffectInput::NextCurrentPositionX:
    return next && currentPositions[nextIndex].valid
               ? currentPositions[nextIndex].x
               : currentPositions[unitIndex].x;
  case TextEffectInput::NextCurrentPositionY:
    return next && currentPositions[nextIndex].valid
               ? currentPositions[nextIndex].y
               : currentPositions[unitIndex].y;
  case TextEffectInput::HasCurrentPosition:
    return currentPositions[unitIndex].valid ? 1.0 : 0.0;
  case TextEffectInput::HasPreviousCurrentPosition:
    return previous && currentPositions[previousIndex].valid ? 1.0 : 0.0;
  case TextEffectInput::HasNextCurrentPosition:
    return next && currentPositions[nextIndex].valid ? 1.0 : 0.0;
  case TextEffectInput::IsNormalUnit:
    return unit.normalUnitIndex ? 1.0 : 0.0;
  case TextEffectInput::NormalUnitIndex:
    // The count is an explicit out-of-range sentinel for auxiliary units. It
    // keeps eager Select operands finite while is_normal_unit remains the
    // authoritative membership predicate.
    return static_cast<double>(
        unit.normalUnitIndex.value_or(frame.normalUnitCount.value_or(0U)));
  case TextEffectInput::NormalUnitCount:
    return static_cast<double>(frame.normalUnitCount.value_or(0U));
  case TextEffectInput::UnicodeCodepoint:
    return static_cast<double>(unit.unicodeCodepoint);
  }
  return 0.0;
}

bool EvaluateInstruction(
    const TextEffectProgramIR &program,
    const TextEffectInstruction &instruction, const TextEffectFrameInput &frame,
    const RowUnitCountMap &rowUnitCounts,
    const ColumnUnitCountMap &columnUnitCounts,
    const RowInitialGeometryMap &rowPositionSpans,
    const std::array<float, 2> &maximumNormalUnitExtent,
    const std::vector<CurrentPositionState> &currentPositions,
    SequentialRandomContext &sequentialRandom,
    internal::EllipticRowLayoutCache &ellipticLayout,
    const std::size_t unitIndex, std::vector<double> &registers,
    std::string &error) noexcept {
  const auto fail = [&](const std::string_view message) noexcept {
    SetEvaluationError(error, message);
    return false;
  };
  const auto in = [&](const std::size_t index) {
    return registers[instruction.inputRegisters[index]];
  };
  double value = 0.0;
  switch (instruction.opcode) {
  case TextEffectOpcode::Constant:
    value = instruction.immediates[0];
    break;
  case TextEffectOpcode::EllipticRowLayout:
    if (!internal::EvaluateEllipticRowLayout(
            frame, {in(0U), in(1U), in(2U), in(3U)}, unitIndex,
            static_cast<std::size_t>(instruction.immediates[0]),
            ellipticLayout, value, error))
      return false;
    break;
  case TextEffectOpcode::Input:
    value = ResolveInput(frame, rowUnitCounts, columnUnitCounts,
                         rowPositionSpans, maximumNormalUnitExtent,
                         currentPositions, unitIndex, *instruction.input);
    break;
  case TextEffectOpcode::Add:
    value = in(0U) + in(1U);
    break;
  case TextEffectOpcode::Subtract:
    value = in(0U) - in(1U);
    break;
  case TextEffectOpcode::Multiply:
    value = in(0U) * in(1U);
    break;
  case TextEffectOpcode::Divide:
    if (in(1U) == 0.0)
      return fail("text effect division by zero");
    value = in(0U) / in(1U);
    break;
  case TextEffectOpcode::Minimum:
    value = std::min(in(0U), in(1U));
    break;
  case TextEffectOpcode::Maximum:
    value = std::max(in(0U), in(1U));
    break;
  case TextEffectOpcode::Clamp:
    value =
        std::clamp(in(0U), std::min(in(1U), in(2U)), std::max(in(1U), in(2U)));
    break;
  case TextEffectOpcode::Mix:
    value = in(0U) + (in(1U) - in(0U)) * in(2U);
    break;
  case TextEffectOpcode::Power:
    value = std::pow(in(0U), in(1U));
    break;
  case TextEffectOpcode::Sine:
    value = std::sin(in(0U));
    break;
  case TextEffectOpcode::Cosine:
    value = std::cos(in(0U));
    break;
  case TextEffectOpcode::Absolute:
    value = std::fabs(in(0U));
    break;
  case TextEffectOpcode::Floor:
    value = std::floor(in(0U));
    break;
  case TextEffectOpcode::Ceil:
    value = std::ceil(in(0U));
    break;
  case TextEffectOpcode::Modulo:
    if (in(1U) == 0.0)
      return fail("text effect modulo by zero");
    value = std::fmod(in(0U), in(1U));
    break;
  case TextEffectOpcode::Negate:
    value = -in(0U);
    break;
  case TextEffectOpcode::Float32:
    if (in(0U) < -static_cast<double>(std::numeric_limits<float>::max()) ||
        in(0U) > static_cast<double>(std::numeric_limits<float>::max())) {
      return fail("text effect float32 conversion is outside range");
    }
    value = static_cast<double>(static_cast<float>(in(0U)));
    break;
  case TextEffectOpcode::Step:
    value = in(1U) < in(0U) ? 0.0 : 1.0;
    break;
  case TextEffectOpcode::SquareRoot:
    if (in(0U) < 0.0)
      return fail("text effect square-root input is negative");
    value = std::sqrt(in(0U));
    break;
  case TextEffectOpcode::ArcSine:
    if (in(0U) < -1.0 || in(0U) > 1.0)
      return fail("text effect arc-sine input is outside [-1, 1]");
    value = std::asin(in(0U));
    break;
  case TextEffectOpcode::ArcTangent:
    value = std::atan(in(0U));
    break;
  case TextEffectOpcode::ArcTangent2:
    value = std::atan2(in(0U), in(1U));
    break;
  case TextEffectOpcode::Exponential:
    value = std::exp(in(0U));
    break;
  case TextEffectOpcode::LogOnePlus:
    if (in(0U) <= -1.0)
      return fail("text effect log-one-plus input is outside its domain");
    value = std::log1p(in(0U));
    break;
  case TextEffectOpcode::LessThan:
    value = in(0U) < in(1U) ? 1.0 : 0.0;
    break;
  case TextEffectOpcode::LessThanOrEqual:
    value = in(0U) <= in(1U) ? 1.0 : 0.0;
    break;
  case TextEffectOpcode::Equal:
    value = in(0U) == in(1U) ? 1.0 : 0.0;
    break;
  case TextEffectOpcode::Select:
    value = in(0U) != 0.0 ? in(1U) : in(2U);
    break;
  case TextEffectOpcode::SeededRandom:
    value = UnitRandom(program, frame.units[unitIndex],
                       instruction.inputRegisters.empty() ? 0.0 : in(0U),
                       instruction.immediates.empty()
                           ? static_cast<double>(instruction.outputRegister)
                           : instruction.immediates[0]);
    break;
  case TextEffectOpcode::ValueNoise2D: {
    const double x = std::floor(in(0U));
    const double y = std::floor(in(1U));
    const double fx = in(0U) - x;
    const double fy = in(1U) - y;
    const auto hash = [&](const double px, const double py) {
      const double n = std::sin(px * instruction.immediates[0] +
                                py * instruction.immediates[1]) *
                       instruction.immediates[2];
      return n - std::floor(n);
    };
    const auto mix = [](const double a, const double b, const double t) {
      return a * (1.0 - t) + b * t;
    };
    value = mix(mix(hash(x, y), hash(x + 1.0, y), fx),
                mix(hash(x, y + 1.0), hash(x + 1.0, y + 1.0), fx), fy);
    break;
  }
  case TextEffectOpcode::SampleHoldNoise: {
    const double frequency = instruction.immediates[0];
    if (!(frequency > 0.0))
      return fail("text effect sample-hold frequency is invalid");
    const double bucket = std::floor(in(0U) * frequency);
    if (!std::isfinite(bucket))
      return fail("text effect sample-hold bucket is not finite");
    const double channel =
        instruction.immediates.size() > 1U
            ? instruction.immediates[1]
            : static_cast<double>(instruction.outputRegister);
    value = UnitRandom(program, frame.units[unitIndex], bucket, channel);
    break;
  }
  case TextEffectOpcode::PageSampleHoldNoise: {
    const double frequency = instruction.immediates[0];
    if (!(frequency > 0.0))
      return fail("text effect page sample-hold frequency is invalid");
    const double bucket = std::floor(in(0U) * frequency);
    if (!std::isfinite(bucket))
      return fail("text effect page sample-hold bucket is not finite");
    const double channel =
        instruction.immediates.size() > 1U
            ? instruction.immediates[1]
            : static_cast<double>(instruction.outputRegister);
    value = PageRandom(program, bucket, channel);
    break;
  }
  case TextEffectOpcode::DurationPrefix: {
    std::size_t lane = 0U;
    if (!ResolveDurationLane(in(0U), instruction.immediates.size(), lane))
      return fail("text effect duration-prefix lane is invalid");
    value = DurationPrefix(instruction.immediates, lane);
    break;
  }
  case TextEffectOpcode::DurationLaneProgress: {
    std::size_t lane = 0U;
    if (!ResolveDurationLane(in(1U), instruction.immediates.size(), lane))
      return fail("text effect duration-progress lane is invalid");
    if (lane == instruction.immediates.size()) {
      // The normal-unit sentinel is deliberately safe under eager Select.
      value = 0.0;
      break;
    }
    const double prefix = DurationPrefix(instruction.immediates, lane);
    value = std::clamp((in(0U) - prefix) / instruction.immediates[lane],
                       0.0, 1.0);
    break;
  }
  case TextEffectOpcode::CubicBezierTiming: {
    const bool dynamicControls = instruction.inputRegisters.size() == 5U;
    const double x1 = dynamicControls ? in(1U) : instruction.immediates[0];
    const double y1 = dynamicControls ? in(2U) : instruction.immediates[1];
    const double x2 = dynamicControls ? in(3U) : instruction.immediates[2];
    const double y2 = dynamicControls ? in(4U) : instruction.immediates[3];
    if (x1 < 0.0 || x1 > 1.0 || x2 < 0.0 || x2 > 1.0)
      return fail("text effect cubic-bezier x controls are invalid");
    value = EvaluateCubicBezierTiming(
        in(0U), x1, y1, x2, y2,
        instruction.immediates.size() == (dynamicControls ? 1U : 5U) &&
            instruction.immediates.back() == 1.0);
    break;
  }
  case TextEffectOpcode::SequentialRandom:
    if (!ResolveSequentialRandom(sequentialRandom, in(0U), value))
      return fail("text effect sequential-random call index is invalid");
    break;
  case TextEffectOpcode::SeededShuffleRank:
    if (!ResolveSeededShuffleRank(
            sequentialRandom, in(0U), in(1U),
            instruction.immediates.empty() ? 0.0
                                           : instruction.immediates[0],
            value))
      return fail("text effect seeded-shuffle inputs are invalid");
    break;
  case TextEffectOpcode::SeededReverseShuffleRank:
    if (!ResolveSeededReverseShuffleRank(sequentialRandom, in(0U), in(1U),
                                         in(2U), value)) {
      return fail("text effect reverse-shuffle inputs are invalid");
    }
    break;
  case TextEffectOpcode::CubicBezierCurve: {
    const auto &knots = instruction.immediates;
    const double time = in(0U);
    double previousTime = knots[0];
    double previousValue = knots[1];
    value = previousValue;
    if (time <= previousTime)
      break;
    for (std::size_t index = 2U; index < knots.size(); index += 6U) {
      const double endTime = knots[index];
      const double endValue = knots[index + 1U];
      value = endValue;
      if (time < endTime) {
        const double progress = (time - previousTime) / (endTime - previousTime);
        const double eased = EvaluateCubicBezierTiming(
            progress, knots[index + 2U], knots[index + 3U],
            knots[index + 4U], knots[index + 5U]);
        value = previousValue * (1.0 - eased) + endValue * eased;
        break;
      }
      previousTime = endTime;
      previousValue = endValue;
    }
    break;
  }
  case TextEffectOpcode::ImmediateLookup: {
    const double index = in(0U);
    if (index < 0.0 || std::floor(index) != index ||
        index >= static_cast<double>(instruction.immediates.size())) {
      return fail("text effect immediate-lookup index is invalid");
    }
    value = instruction.immediates[static_cast<std::size_t>(index)];
    break;
  }
  }
  if (!std::isfinite(value))
    return fail("text effect instruction produced a non-finite value");
  registers[instruction.outputRegister] = value;
  return true;
}

TransformAccumulator &
EnsureTransform(std::vector<TransformAccumulator> &transforms,
                const std::uint32_t index, const std::size_t unitIndex,
                const std::size_t unitCount) {
  const auto previousSize = transforms.size();
  if (previousSize <= index) {
    transforms.resize(static_cast<std::size_t>(index) + 1U);
    for (auto created = previousSize; created < transforms.size(); ++created) {
      transforms[created].components.anchorRangeBegin =
          std::min(unitIndex, unitCount);
      transforms[created].components.anchorRangeEnd =
          std::min(unitIndex + 1U, unitCount);
    }
  }
  return transforms[index];
}

bool ApplyOutput(const TextEffectOutputBinding &binding, const double value,
                 const TextEffectUnitInput &input,
                 TextEffectUnitFramePlan &unit, const std::size_t unitIndex,
                 const std::size_t unitCount,
                 std::vector<TransformAccumulator> &transforms,
                 CurrentPositionState *currentPosition, std::string &error) {
  const auto fail = [&](const std::string_view message) noexcept {
    SetEvaluationError(error, message);
    return false;
  };
  if (!std::isfinite(value) ||
      value < -static_cast<double>(std::numeric_limits<float>::max()) ||
      value > static_cast<double>(std::numeric_limits<float>::max())) {
    return fail("text effect output is outside float range");
  }
  const auto scalar = static_cast<float>(value);
  switch (binding.output) {
  case TextEffectOutput::CurrentPositionX:
    if (!currentPosition)
      return fail("text effect current-position output has no stage buffer");
    currentPosition->x = value;
    return true;
  case TextEffectOutput::CurrentPositionY:
    if (!currentPosition)
      return fail("text effect current-position output has no stage buffer");
    currentPosition->y = value;
    return true;
  case TextEffectOutput::CurrentPositionValid:
    if (!currentPosition)
      return fail("text effect current-position output has no stage buffer");
    currentPosition->valid = value != 0.0;
    return true;
  case TextEffectOutput::Opacity:
    // TextPro stores animator opacity in the Letter instance-color channel
    // without normalizing it first. Cubic/sine overshoot is therefore allowed
    // to exceed one (and can be observed in Qt's captured vertex packet); the
    // RGBA render target performs the eventual saturation. Clamping here
    // weakens antialiased edges before a RenderGroup post effect sees them.
    unit.opacity = scalar;
    return true;
  case TextEffectOutput::InstanceColorRed:
  case TextEffectOutput::InstanceColorGreen:
  case TextEffectOutput::InstanceColorBlue:
  case TextEffectOutput::InstanceColorAlpha: {
    auto color = unit.instanceColor.value_or(input.instanceColor);
    const auto channel = std::clamp(scalar, 0.0F, 1.0F);
    if (binding.output == TextEffectOutput::InstanceColorRed)
      color.red = channel;
    else if (binding.output == TextEffectOutput::InstanceColorGreen)
      color.green = channel;
    else if (binding.output == TextEffectOutput::InstanceColorBlue)
      color.blue = channel;
    else
      color.alpha = channel;
    unit.instanceColor = color;
    return true;
  }
  case TextEffectOutput::AbsoluteFontSize:
    if (scalar < 0.0F)
      return fail("text effect absolute font size is negative");
    unit.absoluteFontSize = scalar;
    return true;
  case TextEffectOutput::BlurRadius:
    if (scalar < 0.0F || scalar > 512.0F)
      return fail("text effect blur radius is outside its budget");
    unit.sdfBlurRadius = scalar;
    return true;
  case TextEffectOutput::ReplacementCodepoint:
    if (value < 0.0 || value > 65'535.0 || std::floor(value) != value ||
        (value >= 0xD800 && value <= 0xDFFF)) {
      return fail("text effect replacement codepoint is invalid");
    }
    unit.replacementCodepoint = static_cast<std::uint32_t>(value);
    return true;
  default:
    break;
  }

  auto &accumulator =
      EnsureTransform(transforms, binding.transformIndex, unitIndex, unitCount);
  if (binding.output >= TextEffectOutput::MatrixC0R0 &&
      binding.output <= TextEffectOutput::MatrixC3R3) {
    if (accumulator.hasDecomposedOutput) {
      return fail("text effect transform mixes matrix and decomposed outputs");
    }
    accumulator.hasDirectMatrixOutput = true;
    const auto component = static_cast<std::size_t>(binding.output) -
                           static_cast<std::size_t>(TextEffectOutput::MatrixC0R0);
    accumulator.directMatrix.columnMajor[component] = scalar;
    return true;
  }
  if (accumulator.hasDirectMatrixOutput) {
    return fail("text effect transform mixes matrix and decomposed outputs");
  }
  accumulator.hasDecomposedOutput = true;
  auto &transform = accumulator.components;
  switch (binding.output) {
  case TextEffectOutput::OffsetX:
    transform.offsetX = scalar;
    break;
  case TextEffectOutput::OffsetY:
    transform.offsetY = scalar;
    break;
  case TextEffectOutput::OffsetZ:
    transform.offsetZ = scalar;
    break;
  case TextEffectOutput::RotationX:
    transform.rotationX = scalar;
    break;
  case TextEffectOutput::RotationY:
    transform.rotationY = scalar;
    break;
  case TextEffectOutput::RotationZ:
    transform.rotationZ = scalar;
    break;
  case TextEffectOutput::ShearX:
    if (std::fabs(scalar) > 3'600.0F)
      return fail("text effect shear X is outside its budget");
    transform.shearX = scalar;
    break;
  case TextEffectOutput::ShearY:
    if (std::fabs(scalar) > 3'600.0F)
      return fail("text effect shear Y is outside its budget");
    transform.shearY = scalar;
    break;
  case TextEffectOutput::ScaleX:
    transform.scaleX = scalar;
    break;
  case TextEffectOutput::ScaleY:
    transform.scaleY = scalar;
    break;
  case TextEffectOutput::ScaleZ:
    transform.scaleZ = scalar;
    break;
  case TextEffectOutput::AnchorX:
    transform.anchorX = scalar;
    break;
  case TextEffectOutput::AnchorY:
    transform.anchorY = scalar;
    break;
  case TextEffectOutput::AnchorZ:
    transform.anchorZ = scalar;
    break;
  case TextEffectOutput::AnchorRangeBegin:
    transform.anchorRangeBegin = static_cast<std::size_t>(
        std::clamp(std::floor(value), 0.0, static_cast<double>(unitCount)));
    break;
  case TextEffectOutput::AnchorRangeEnd:
    transform.anchorRangeEnd = static_cast<std::size_t>(
        std::clamp(std::ceil(value), 0.0, static_cast<double>(unitCount)));
    break;
  default:
    return fail("text effect output binding is unsupported");
  }
  return true;
}

} // namespace

std::optional<TextEffectTransformPlan> ResolveTextEffectTransformPlan(
    const TextEffectTransformComponents &components,
    const TextEffectRect &tightAnchorBounds,
    const TextEffectRect &layoutAnchorBounds,
    const TextEffectRect &projectionBounds) noexcept {
  const std::array<float, 18> scalars{
      components.offsetX,
      components.offsetY,
      components.offsetZ,
      components.rotationX,
      components.rotationY,
      components.rotationZ,
      components.shearX,
      components.shearY,
      components.scaleX,
      components.scaleY,
      components.scaleZ,
      components.anchorX,
      components.anchorY,
      components.anchorZ,
      components.projection.fieldOfViewDegrees,
      components.projection.vanishingPointX,
      components.projection.vanishingPointY,
      0.0F,
  };
  if (!std::all_of(scalars.begin(), scalars.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !FiniteRect(tightAnchorBounds) || !FiniteRect(layoutAnchorBounds) ||
      !FiniteRect(projectionBounds) ||
      std::fabs(components.shearX) > 3'600.0F ||
      std::fabs(components.shearY) > 3'600.0F ||
      std::fabs(components.scaleX) > 1'024.0F ||
      std::fabs(components.scaleY) > 1'024.0F ||
      std::fabs(components.scaleZ) > 1'024.0F) {
    return std::nullopt;
  }
  const auto anchorBounds = NonEmptyRect(tightAnchorBounds)
                                ? tightAnchorBounds
                                : layoutAnchorBounds;
  if (!NonEmptyRect(anchorBounds))
    return std::nullopt;

  const float halfWidth =
      QtTextBinary32Multiply(anchorBounds.width, 0.5F);
  const float halfHeight =
      QtTextBinary32Multiply(anchorBounds.height, 0.5F);
  const float anchorX = QtTextBinary32Add(
      QtTextBinary32Add(anchorBounds.x, halfWidth),
      QtTextBinary32Multiply(components.anchorX, halfWidth));
  const float anchorY = QtTextBinary32Add(
      QtTextBinary32Add(anchorBounds.y, halfHeight),
      QtTextBinary32Multiply(components.anchorY, halfHeight));
  const float anchorZ = StoreQtTextBinary32(components.anchorZ);
  const float shearX = std::clamp(
      StoreQtTextBinary32(
          std::tan(QtTextDegreesToRadians(components.shearX))),
      -64.0F, 64.0F);
  const float shearY = std::clamp(
      StoreQtTextBinary32(
          std::tan(QtTextDegreesToRadians(components.shearY))),
      -64.0F, 64.0F);

  // Recovered ExtraMatrix order: T(offset) * T(anchor) * Ry * Rx * Rz *
  // Shear * Scale * T(-anchor). Every concat observes a binary32 store.
  Matrix4 matrix = TranslationMatrix(components.offsetX, components.offsetY,
                                     components.offsetZ);
  matrix = ConcatQtTextMatrix(matrix,
                              TranslationMatrix(anchorX, anchorY, anchorZ));
  matrix = ConcatQtTextMatrix(
      matrix, RotationYMatrix(QtTextDegreesToRadians(components.rotationY)));
  matrix = ConcatQtTextMatrix(
      matrix, RotationXMatrix(QtTextDegreesToRadians(components.rotationX)));
  matrix = ConcatQtTextMatrix(
      matrix, RotationZMatrix(QtTextDegreesToRadians(components.rotationZ)));
  matrix = ConcatQtTextMatrix(matrix, ShearMatrix(shearX, shearY));
  matrix = ConcatQtTextMatrix(
      matrix,
      ScaleMatrix(components.scaleX, components.scaleY, components.scaleZ));
  matrix = ConcatQtTextMatrix(
      matrix, TranslationMatrix(StoreQtTextBinary32(-anchorX),
                                StoreQtTextBinary32(-anchorY),
                                StoreQtTextBinary32(-anchorZ)));

  if (components.projection.kind == TextProjectionKind::Perspective3D) {
    if (components.projection.fieldOfViewDegrees <= 0.0F ||
        components.projection.fieldOfViewDegrees >= 180.0F ||
        !NonEmptyRect(projectionBounds)) {
      return std::nullopt;
    }
    const float fieldOfView =
        QtTextDegreesToRadians(components.projection.fieldOfViewDegrees);
    const float focal = std::max(
        1.0F, projectionBounds.height * 0.5F / std::tan(fieldOfView * 0.5F));
    Matrix4 perspective{1.0F, 0.0F, 0.0F, 0.0F,
                        0.0F, 1.0F, 0.0F, 0.0F,
                        0.0F, 0.0F, 1.0F, -1.0F / focal,
                        0.0F, 0.0F, 0.0F, 1.0F};
    const float vanishingX =
        projectionBounds.x + projectionBounds.width *
                                 components.projection.vanishingPointX;
    const float vanishingY =
        projectionBounds.y + projectionBounds.height *
                                 components.projection.vanishingPointY;
    matrix = ConcatQtTextMatrix(
        TranslationMatrix(-vanishingX, -vanishingY, 0.0F), matrix);
    matrix = ConcatQtTextMatrix(perspective, matrix);
    matrix = ConcatQtTextMatrix(
        TranslationMatrix(vanishingX, vanishingY, 0.0F), matrix);
  }
  if (!std::all_of(matrix.begin(), matrix.end(),
                   [](const float value) { return std::isfinite(value); })) {
    return std::nullopt;
  }
  TextEffectTransformPlan result;
  result.localToText.columnMajor = matrix;
  result.tightAnchorBounds = tightAnchorBounds;
  result.layoutAnchorBounds = layoutAnchorBounds;
  return result;
}

namespace {

struct QtTextQuaternionF32 final {
  float x{0.0F};
  float y{0.0F};
  float z{0.0F};
  float w{1.0F};
};

void QtTextSinCosHalfF32(const float angle, float &sine,
                         float &cosine) noexcept {
  const float half = QtTextBinary32Multiply(angle, 0.5F);
  sine = StoreQtTextBinary32(std::sin(half));
  cosine = StoreQtTextBinary32(std::cos(half));
}

QtTextQuaternionF32 MultiplyQtTextQuaternionF32(
    const QtTextQuaternionF32 &left,
    const QtTextQuaternionF32 &right) noexcept {
  QtTextQuaternionF32 result;
  result.x = QtTextBinary32Subtract(
      QtTextBinary32Add(
          QtTextBinary32Add(QtTextBinary32Multiply(left.w, right.x),
                            QtTextBinary32Multiply(left.x, right.w)),
          QtTextBinary32Multiply(left.y, right.z)),
      QtTextBinary32Multiply(left.z, right.y));
  result.y = QtTextBinary32Subtract(
      QtTextBinary32Add(
          QtTextBinary32Add(QtTextBinary32Multiply(left.w, right.y),
                            QtTextBinary32Multiply(left.y, right.w)),
          QtTextBinary32Multiply(left.z, right.x)),
      QtTextBinary32Multiply(left.x, right.z));
  result.z = QtTextBinary32Subtract(
      QtTextBinary32Add(
          QtTextBinary32Add(QtTextBinary32Multiply(left.w, right.z),
                            QtTextBinary32Multiply(left.z, right.w)),
          QtTextBinary32Multiply(left.x, right.y)),
      QtTextBinary32Multiply(left.y, right.x));
  result.w = QtTextBinary32Subtract(
      QtTextBinary32Subtract(
          QtTextBinary32Subtract(QtTextBinary32Multiply(left.w, right.w),
                                 QtTextBinary32Multiply(left.x, right.x)),
          QtTextBinary32Multiply(left.y, right.y)),
      QtTextBinary32Multiply(left.z, right.z));
  return result;
}

QtTextQuaternionF32 QtTextEulerToQuaternionF32(
    const float rotationX, const float rotationY,
    const float rotationZ) noexcept {
  float sinX = 0.0F;
  float cosX = 1.0F;
  float sinY = 0.0F;
  float cosY = 1.0F;
  float sinZ = 0.0F;
  float cosZ = 1.0F;
  QtTextSinCosHalfF32(rotationX, sinX, cosX);
  QtTextSinCosHalfF32(rotationY, sinY, cosY);
  QtTextSinCosHalfF32(rotationZ, sinZ, cosZ);
  const QtTextQuaternionF32 qX{sinX, 0.0F, 0.0F, cosX};
  const QtTextQuaternionF32 qY{0.0F, sinY, 0.0F, cosY};
  const QtTextQuaternionF32 qZ{0.0F, 0.0F, sinZ, cosZ};
  return MultiplyQtTextQuaternionF32(
      MultiplyQtTextQuaternionF32(qY, qX), qZ);
}

Matrix4 QtTextTrsMatrixF32(const float translationX,
                           const float translationY,
                           const float translationZ,
                           const QtTextQuaternionF32 &rotation,
                           const float scaleX, const float scaleY,
                           const float scaleZ) noexcept {
  const float twoX = QtTextBinary32Add(rotation.x, rotation.x);
  const float twoY = QtTextBinary32Add(rotation.y, rotation.y);
  const float twoZ = QtTextBinary32Add(rotation.z, rotation.z);
  const float xx = QtTextBinary32Multiply(rotation.x, twoX);
  const float yy = QtTextBinary32Multiply(rotation.y, twoY);
  const float zz = QtTextBinary32Multiply(rotation.z, twoZ);
  const float xy = QtTextBinary32Multiply(rotation.x, twoY);
  const float xz = QtTextBinary32Multiply(rotation.x, twoZ);
  const float yz = QtTextBinary32Multiply(rotation.y, twoZ);
  const float wx = QtTextBinary32Multiply(twoX, rotation.w);
  const float wy = QtTextBinary32Multiply(twoY, rotation.w);
  const float wz = QtTextBinary32Multiply(rotation.w, twoZ);

  Matrix4 result{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
  result[0] = QtTextBinary32Subtract(1.0F, QtTextBinary32Add(yy, zz));
  result[1] = QtTextBinary32Add(xy, wz);
  result[2] = QtTextBinary32Subtract(xz, wy);
  result[4] = QtTextBinary32Subtract(xy, wz);
  result[5] = QtTextBinary32Subtract(1.0F, QtTextBinary32Add(xx, zz));
  result[6] = QtTextBinary32Add(yz, wx);
  result[8] = QtTextBinary32Add(xz, wy);
  result[9] = QtTextBinary32Subtract(yz, wx);
  result[10] = QtTextBinary32Subtract(1.0F, QtTextBinary32Add(xx, yy));
  for (std::size_t row = 0U; row < 3U; ++row) {
    result[row] = QtTextBinary32Multiply(result[row], scaleX);
    result[4U + row] = QtTextBinary32Multiply(result[4U + row], scaleY);
    result[8U + row] = QtTextBinary32Multiply(result[8U + row], scaleZ);
  }
  result[12] = StoreQtTextBinary32(translationX);
  result[13] = StoreQtTextBinary32(translationY);
  result[14] = StoreQtTextBinary32(translationZ);
  return result;
}

std::optional<TextEffectTransformPlan> ResolveQtTextProgramTransformPlan(
    const TextEffectTransformComponents &components,
    const TextEffectRect &tightAnchorBounds,
    const TextEffectRect &layoutAnchorBounds,
    const TextEffectRect &projectionBounds) noexcept {
  if (components.projection.kind != TextProjectionKind::Planar2D ||
      components.shearX != 0.0F || components.shearY != 0.0F) {
    auto frameComponents = components;
    const auto anchorBounds = NonEmptyRect(tightAnchorBounds)
                                  ? tightAnchorBounds
                                  : layoutAnchorBounds;
    frameComponents.anchorZ = QtTextBinary32Multiply(
        frameComponents.anchorZ,
        QtTextBinary32Multiply(anchorBounds.height, 0.5F));
    return ResolveTextEffectTransformPlan(
        frameComponents, tightAnchorBounds, layoutAnchorBounds,
        projectionBounds);
  }
  const auto anchorBounds = NonEmptyRect(tightAnchorBounds)
                                ? tightAnchorBounds
                                : layoutAnchorBounds;
  if (!NonEmptyRect(anchorBounds))
    return std::nullopt;
  const std::array<float, 12> scalars{
      components.offsetX, components.offsetY, components.offsetZ,
      components.rotationX, components.rotationY, components.rotationZ,
      components.scaleX, components.scaleY, components.scaleZ,
      components.anchorX, components.anchorY, components.anchorZ};
  if (!std::all_of(scalars.begin(), scalars.end(),
                   [](const float value) { return std::isfinite(value); })) {
    return std::nullopt;
  }

  const float halfWidth =
      QtTextBinary32Multiply(anchorBounds.width, 0.5F);
  const float halfHeight =
      QtTextBinary32Multiply(anchorBounds.height, 0.5F);
  const float anchorX = QtTextBinary32Add(
      QtTextBinary32Add(anchorBounds.x, halfWidth),
      QtTextBinary32Multiply(components.anchorX, halfWidth));
  const float anchorY = QtTextBinary32Add(
      QtTextBinary32Add(anchorBounds.y, halfHeight),
      QtTextBinary32Multiply(components.anchorY, halfHeight));
  const float anchorZ =
      QtTextBinary32Multiply(components.anchorZ, halfHeight);
  const auto rotation = QtTextEulerToQuaternionF32(
      QtTextDegreesToRadians(components.rotationX),
      QtTextDegreesToRadians(components.rotationY),
      QtTextDegreesToRadians(components.rotationZ));
  const auto trs = QtTextTrsMatrixF32(
      components.offsetX, components.offsetY, components.offsetZ, rotation,
      components.scaleX, components.scaleY, components.scaleZ);
  auto matrix = MultiplyMatrix(TranslationMatrix(anchorX, anchorY, anchorZ),
                               trs);
  matrix = MultiplyMatrix(
      matrix, TranslationMatrix(StoreQtTextBinary32(-anchorX),
                                StoreQtTextBinary32(-anchorY),
                                StoreQtTextBinary32(-anchorZ)));
  if (!std::all_of(matrix.begin(), matrix.end(),
                   [](const float value) { return std::isfinite(value); })) {
    return std::nullopt;
  }
  TextEffectTransformPlan result;
  result.localToText.columnMajor = matrix;
  result.tightAnchorBounds = tightAnchorBounds;
  result.layoutAnchorBounds = layoutAnchorBounds;
  return result;
}

std::pair<float, float> ReferencePointToQtTextProgramSource(
    float x, float y, const ReferenceCanvas &referenceCanvas,
    float sourceToReferenceScale) noexcept;

TextEffectRect ReferenceRectToQtTextProgramSource(
    const TextEffectRect &rect, const ReferenceCanvas &referenceCanvas,
    float sourceToReferenceScale) noexcept;

std::optional<TextEffectTransformPlan>
ResolveQtTextProgramTransformPlanForReference(
    const TextEffectTransformComponents &components,
    const TextEffectRect &tightAnchorBounds,
    const TextEffectRect &layoutAnchorBounds,
    const TextEffectRect &projectionBounds,
    const float sourceToReferenceScale,
    const ReferenceCanvas &referenceCanvas,
    const bool legacyLetterRotation) noexcept {
  if (components.projection.kind != TextProjectionKind::Planar2D ||
      components.shearX != 0.0F || components.shearY != 0.0F) {
    const auto sourceTight = ReferenceRectToQtTextProgramSource(
        tightAnchorBounds, referenceCanvas, sourceToReferenceScale);
    const auto sourceLayout = ReferenceRectToQtTextProgramSource(
        layoutAnchorBounds, referenceCanvas, sourceToReferenceScale);
    const auto sourceProjection = ReferenceRectToQtTextProgramSource(
        projectionBounds, referenceCanvas, sourceToReferenceScale);
    auto result = ResolveQtTextProgramTransformPlan(
        components, sourceTight, sourceLayout, sourceProjection);
    if (!result || !ProjectTextEffectTransformPlanToReference(
                       *result, sourceToReferenceScale, referenceCanvas)) {
      return std::nullopt;
    }
    return result;
  }
  const auto anchorBounds = NonEmptyRect(tightAnchorBounds)
                                ? tightAnchorBounds
                                : layoutAnchorBounds;
  if (!NonEmptyRect(anchorBounds) ||
      !std::isfinite(sourceToReferenceScale) ||
      sourceToReferenceScale <= 0.0F) {
    return std::nullopt;
  }
  const std::array<float, 12> scalars{
      components.offsetX, components.offsetY, components.offsetZ,
      components.rotationX, components.rotationY, components.rotationZ,
      components.scaleX, components.scaleY, components.scaleZ,
      components.anchorX, components.anchorY, components.anchorZ};
  if (!std::all_of(scalars.begin(), scalars.end(),
                   [](const float value) { return std::isfinite(value); })) {
    return std::nullopt;
  }

  const float halfWidth =
      QtTextBinary32Multiply(anchorBounds.width, 0.5F);
  const float halfHeight =
      QtTextBinary32Multiply(anchorBounds.height, 0.5F);
  const float right =
      QtTextBinary32Add(anchorBounds.x, anchorBounds.width);
  const float bottom =
      QtTextBinary32Add(anchorBounds.y, anchorBounds.height);
  const float centerX = QtTextBinary32Multiply(
      QtTextBinary32Add(anchorBounds.x, right), 0.5F);
  const float centerY = QtTextBinary32Multiply(
      QtTextBinary32Add(anchorBounds.y, bottom), 0.5F);
  const float anchorX = QtTextBinary32Add(
      centerX, QtTextBinary32Multiply(components.anchorX, halfWidth));
  const float anchorY = QtTextBinary32Subtract(
      centerY, QtTextBinary32Multiply(components.anchorY, halfHeight));
  const float anchorZ =
      QtTextBinary32Multiply(components.anchorZ, halfHeight);
  const auto rotation = QtTextEulerToQuaternionF32(
      StoreQtTextBinary32(-QtTextDegreesToRadians(components.rotationX)),
      QtTextDegreesToRadians(components.rotationY),
      // Legacy Letter's planar Z setter uses the clockwise presentation
      // convention. SDFText and world-space physics use Y-up Euler angles.
      // Keep the recovered Legacy conversion when sharing this projection.
      StoreQtTextBinary32(QtTextDegreesToRadians(components.rotationZ) *
                         (legacyLetterRotation ? 1.0F : -1.0F)));
  const auto trs = QtTextTrsMatrixF32(
      QtTextBinary32Multiply(components.offsetX, sourceToReferenceScale),
      StoreQtTextBinary32(-QtTextBinary32Multiply(
          components.offsetY, sourceToReferenceScale)),
      QtTextBinary32Multiply(components.offsetZ, sourceToReferenceScale),
      rotation, components.scaleX, components.scaleY, components.scaleZ);
  auto matrix = MultiplyMatrix(TranslationMatrix(anchorX, anchorY, anchorZ),
                               trs);
  matrix = MultiplyMatrix(
      matrix, TranslationMatrix(StoreQtTextBinary32(-anchorX),
                                StoreQtTextBinary32(-anchorY),
                                StoreQtTextBinary32(-anchorZ)));
  if (!std::all_of(matrix.begin(), matrix.end(),
                   [](const float value) { return std::isfinite(value); })) {
    return std::nullopt;
  }
  TextEffectTransformPlan result;
  result.localToText.columnMajor = matrix;
  result.tightAnchorBounds = tightAnchorBounds;
  result.layoutAnchorBounds = layoutAnchorBounds;
  return result;
}

} // namespace

bool ProjectTextEffectTransformPlanToReference(
    TextEffectTransformPlan &plan,
    const float sourceToReferenceScale,
    const ReferenceCanvas &referenceCanvas) noexcept {
  if (!std::isfinite(sourceToReferenceScale) ||
      sourceToReferenceScale <= 0.0F ||
      !std::isfinite(referenceCanvas.width) ||
      !std::isfinite(referenceCanvas.height) ||
      referenceCanvas.width <= 0.0F || referenceCanvas.height <= 0.0F ||
      !std::all_of(plan.localToText.columnMajor.begin(),
                   plan.localToText.columnMajor.end(),
                   [](const float value) { return std::isfinite(value); })) {
    return false;
  }
  const float storedScale = StoreQtTextBinary32(sourceToReferenceScale);
  const float inverseScale = QtTextBinary32Divide(1.0F, storedScale);
  const float centerX =
      QtTextBinary32Multiply(referenceCanvas.width, 0.5F);
  const float centerY =
      QtTextBinary32Multiply(referenceCanvas.height, 0.5F);
  const auto sourceToReference = MultiplyMatrix(
      TranslationMatrix(centerX, centerY, 0.0F),
      ScaleMatrix(storedScale, StoreQtTextBinary32(-storedScale),
                  storedScale));
  const auto referenceToSource = MultiplyMatrix(
      ScaleMatrix(inverseScale, StoreQtTextBinary32(-inverseScale),
                  inverseScale),
      TranslationMatrix(StoreQtTextBinary32(-centerX),
                        StoreQtTextBinary32(-centerY), 0.0F));
  plan.localToText.columnMajor = MultiplyMatrix(
      sourceToReference,
      MultiplyMatrix(plan.localToText.columnMajor, referenceToSource));
  const auto projectBounds = [&](TextEffectRect &rect) {
    rect.x = QtTextBinary32Add(
        centerX, QtTextBinary32Multiply(rect.x, storedScale));
    rect.y = QtTextBinary32Subtract(
        centerY,
        QtTextBinary32Multiply(
            QtTextBinary32Add(rect.y, rect.height), storedScale));
    rect.width = QtTextBinary32Multiply(rect.width, storedScale);
    rect.height = QtTextBinary32Multiply(rect.height, storedScale);
  };
  projectBounds(plan.tightAnchorBounds);
  projectBounds(plan.layoutAnchorBounds);
  return std::all_of(plan.localToText.columnMajor.begin(),
                     plan.localToText.columnMajor.end(),
                     [](const float value) { return std::isfinite(value); });
}

std::optional<TextEffectMatrix4x4> ComposeTextEffectTransforms(
    const std::vector<TextEffectTransformPlan> &transforms) noexcept {
  TextEffectMatrix4x4 result;
  for (const auto &transform : transforms) {
    if (!std::all_of(transform.localToText.columnMajor.begin(),
                     transform.localToText.columnMajor.end(),
                     [](const float value) { return std::isfinite(value); })) {
      return std::nullopt;
    }
    result.columnMajor = MultiplyMatrix(transform.localToText.columnMajor,
                                        result.columnMajor);
  }
  return result;
}

bool RetargetTextEffectProgramTransforms(
    TextEffectFramePlan &plan, const TextEffectFrameInput &finalFrame,
    const ReferenceCanvas &referenceCanvas,
    const float sourceToReferenceScale, std::string &error) noexcept {
  error.clear();
  if (!std::isfinite(sourceToReferenceScale) ||
      sourceToReferenceScale <= 0.0F ||
      !std::isfinite(referenceCanvas.width) ||
      !std::isfinite(referenceCanvas.height) ||
      referenceCanvas.width <= 0.0F || referenceCanvas.height <= 0.0F) {
    error = "text effect program retarget bridge is invalid";
    return false;
  }
  try {
    std::unordered_map<std::uint64_t, const TextEffectUnitInput *> units;
    units.reserve(finalFrame.units.size());
    for (const auto &unit : finalFrame.units) {
      if (!units.emplace(unit.stableUnitId, &unit).second) {
        error = "text effect program retarget frame has duplicate units";
        return false;
      }
    }
    for (auto &unitPlan : plan.units) {
      for (auto &transform : unitPlan.transforms) {
        if (!transform.programRetarget)
          continue;
        const auto descriptor = *transform.programRetarget;
        if (!descriptor.anchorToReferenceCanvas &&
            descriptor.anchorStableUnitIds.empty()) {
          error = "text effect program retarget anchor set is empty";
          return false;
        }
        TextEffectRect tightBounds{};
        TextEffectRect layoutBounds{};
        if (descriptor.anchorToReferenceCanvas) {
          tightBounds = {0.0F, 0.0F, referenceCanvas.width,
                         referenceCanvas.height};
          layoutBounds = tightBounds;
        } else {
          std::unordered_set<std::uint64_t> anchorIds;
          anchorIds.reserve(descriptor.anchorStableUnitIds.size());
          for (const auto stableUnitId : descriptor.anchorStableUnitIds) {
            if (!anchorIds.insert(stableUnitId).second) {
              error = "text effect program retarget anchor set is ambiguous";
              return false;
            }
            const auto found = units.find(stableUnitId);
            if (found == units.end()) {
              error = "text effect program retarget anchor is missing";
              return false;
            }
            const auto &unit = *found->second;
            // Program retargeting is resolved in the same geometry domain that
            // the private Qt program consumed.  Product frames carry a
            // dedicated programRect for this purpose; falling back to rect
            // keeps older callers source-compatible without mixing layout
            // geometry into the program anchor.
            const auto programBounds =
                NonEmptyRect(unit.programRect) ? unit.programRect : unit.rect;
            // Keep the same two anchor channels used during program
            // evaluation. The script-visible padded Letter rect is not the
            // uniform tight row anchor used by scale/rotation transforms.
            tightBounds = UnionRect(
                tightBounds,
                NonEmptyRect(unit.tightRect) ? unit.tightRect : programBounds);
            layoutBounds = UnionRect(layoutBounds, programBounds);
          }
        }
        if (!NonEmptyRect(tightBounds) || !NonEmptyRect(layoutBounds)) {
          error = "text effect program retarget anchor bounds are empty";
          return false;
        }
        auto resolved = ResolveQtTextProgramTransformPlanForReference(
            descriptor.sourceComponents, tightBounds, layoutBounds,
            finalFrame.textRect, sourceToReferenceScale, referenceCanvas,
            plan.sourceCreationComponent == TextSourceCreationComponent::LegacyText);
        if (!resolved) {
          error = "text effect program final transform is invalid";
          return false;
        }
        resolved->programRetarget = descriptor;
        transform = std::move(*resolved);
      }
    }
    return true;
  } catch (...) {
    error = "text effect program retarget allocation failed";
    return false;
  }
}

bool EvaluateTextEffectProgram(const TextEffectProgramIR &program,
                               const TextEffectFrameInput &input,
                               TextEffectFramePlan &output,
                               std::string &error,
                               TextProgramExecutionEvidence *evidence) noexcept {
  ResetFramePlan(output);
  if (evidence)
    *evidence = {};
  error.clear();
  const auto fail = [&](const std::string_view message) noexcept {
    SetEvaluationError(error, message);
    ResetFramePlan(output);
    return false;
  };
  try {
    // Qt's SegmentJS program keeps source-number intermediates in
    // double precision and narrows only at the authored material/property
    // boundaries.  The IR carries those boundaries explicitly as Float32
    // instructions.  Inserting an extra Float32 after every instruction
    // perturbs trigonometric/random offset programs even when their final
    // opacity happens to round to the same value; captured Letter vertices
    // expose that error directly.  Execute the authored dataflow as-is and
    // let explicit Float32 opcodes own every narrowing point.
    const auto &evaluatedProgram = program;
    SequentialRandomContext sequentialRandom;
    sequentialRandom.state = NormalizeParkMillerSeed(program.randomSeed);
    if (!std::isfinite(input.progress) || input.progress < 0.0 ||
        input.progress > 1.0 || input.units.size() > kMaximumFrameUnits ||
        input.rowRects.size() > kMaximumFrameUnits ||
        input.tightRowRects.size() != input.rowRects.size()) {
      return fail("text effect frame input is invalid");
    }

    const auto validRect = [](const TextEffectRect &rect) noexcept {
      return std::isfinite(rect.x) && std::isfinite(rect.y) &&
             std::isfinite(rect.width) && std::isfinite(rect.height) &&
             rect.width >= 0.0F && rect.height >= 0.0F &&
             std::isfinite(rect.x + rect.width) &&
             std::isfinite(rect.y + rect.height);
    };
    if (!validRect(input.textRect) || !validRect(input.tightTextRect) ||
        (input.canvasRect && !validRect(*input.canvasRect)) ||
        !std::all_of(input.rowRects.begin(), input.rowRects.end(), validRect) ||
        !std::all_of(input.tightRowRects.begin(), input.tightRowRects.end(),
                     validRect)) {
      return fail("text effect frame bounds are invalid");
    }

    output.programId = evaluatedProgram.programId;
    output.units.reserve(input.units.size());
    std::unordered_set<std::uint64_t> stableUnitIds;
    stableUnitIds.reserve(input.units.size());
    for (std::size_t unitIndex = 0U; unitIndex < input.units.size();
         ++unitIndex) {
      const auto &unit = input.units[unitIndex];
      if (unit.index != unitIndex || unit.renderIndex >= input.units.size() ||
          unit.row >= input.rowRects.size() ||
          unit.indexInRow >= input.units.size() ||
          !std::isfinite(unit.fontSize) || unit.fontSize < 0.0F ||
          !validRect(unit.rect) || !validRect(unit.tightRect) ||
          !validRect(unit.programRect) ||
          !std::isfinite(unit.initialPositionX) ||
          !std::isfinite(unit.initialPositionY) ||
          !std::isfinite(unit.instanceColor.red) ||
          !std::isfinite(unit.instanceColor.green) ||
          !std::isfinite(unit.instanceColor.blue) ||
          !std::isfinite(unit.instanceColor.alpha) ||
          unit.instanceColor.red < 0.0F || unit.instanceColor.red > 1.0F ||
          unit.instanceColor.green < 0.0F || unit.instanceColor.green > 1.0F ||
          unit.instanceColor.blue < 0.0F || unit.instanceColor.blue > 1.0F ||
          unit.instanceColor.alpha < 0.0F || unit.instanceColor.alpha > 1.0F ||
          unit.unicodeCodepoint > 0x10FFFFU ||
          (unit.unicodeCodepoint >= 0xD800U &&
           unit.unicodeCodepoint <= 0xDFFFU) ||
          !stableUnitIds.insert(unit.stableUnitId).second) {
        return fail("text effect unit geometry is invalid");
      }
      TextEffectUnitFramePlan unitPlan;
      unitPlan.stableUnitId = unit.stableUnitId;
      output.units.push_back(std::move(unitPlan));
    }

    const bool usesNormalUnitTopology =
        ProgramUsesInput(evaluatedProgram,
                         [](const TextEffectInput value) noexcept {
          return IsNormalUnitTopologyInput(value);
        });
    const bool suppliesNormalUnitTopology =
        input.normalUnitCount.has_value() ||
        std::any_of(input.units.begin(), input.units.end(),
                    [](const TextEffectUnitInput &unit) noexcept {
                      return unit.normalUnitIndex.has_value();
                    });
    if (usesNormalUnitTopology || suppliesNormalUnitTopology) {
      if (!input.normalUnitCount ||
          *input.normalUnitCount > input.units.size()) {
        return fail("text effect normal-unit topology is missing or invalid");
      }
      std::size_t expectedNormalIndex = 0U;
      for (const auto &unit : input.units) {
        if (!unit.normalUnitIndex)
          continue;
        if (*unit.normalUnitIndex != expectedNormalIndex ||
            expectedNormalIndex >= *input.normalUnitCount) {
          return fail(
              "text effect normal-unit topology is not dense and ordered");
        }
        ++expectedNormalIndex;
      }
      if (expectedNormalIndex != *input.normalUnitCount) {
        return fail(
            "text effect normal-unit count does not match its topology");
      }
    }

    RowUnitCountMap rowUnitCounts;
    rowUnitCounts.reserve(input.rowRects.size());
    RowInitialGeometryMap rowInitialPositionSpans;
    rowInitialPositionSpans.reserve(input.rowRects.size());
    float maximumFontSize = 0.0F;
    std::array<float, 2> maximumNormalUnitExtent{};
    for (const auto &unit : input.units) {
      maximumFontSize = std::max(maximumFontSize, unit.fontSize);
      auto &range = rowInitialPositionSpans[unit.row];
      const double x = unit.initialPositionX;
      const double y = unit.initialPositionY;
      range.left = std::min(range.left, x - unit.rect.width * 0.5);
      range.right = std::max(range.right, x + unit.rect.width * 0.5);
      range.bottom = std::min(range.bottom, y - unit.rect.height * 0.5);
      range.top = std::max(range.top, y + unit.rect.height * 0.5);
      if (unit.type == 0) {
        maximumNormalUnitExtent[0] =
            std::max(maximumNormalUnitExtent[0], unit.rect.width);
        maximumNormalUnitExtent[1] =
            std::max(maximumNormalUnitExtent[1], unit.rect.height);
        ++rowUnitCounts[unit.row];
        range.minimumX = std::min(range.minimumX, x);
        range.maximumX = std::max(range.maximumX, x);
      }
    }

    ColumnUnitCountMap columnUnitCounts;
    const bool usesColumnUnitCount =
        std::any_of(evaluatedProgram.stages.begin(),
                    evaluatedProgram.stages.end(),
                    [](const TextEffectProgramStageIR &stage) noexcept {
                      return stage.kind == TextEffectStageKind::ColumnGroup;
                    });
    if (usesColumnUnitCount) {
      columnUnitCounts.reserve(input.units.size());
      for (const auto &unit : input.units) {
        if (unit.type == 0)
          ++columnUnitCounts[unit.indexInRow];
      }
    }

    const bool usesCurrentPositionDataflow =
        ProgramUsesInput(evaluatedProgram,
                         [](const TextEffectInput value) noexcept {
                           return IsCurrentPositionInput(value);
                         }) ||
        std::any_of(evaluatedProgram.stages.begin(),
                    evaluatedProgram.stages.end(),
                    StagePublishesCurrentPosition);
    std::vector<CurrentPositionState> currentPositions;
    if (usesCurrentPositionDataflow) {
      currentPositions.reserve(input.units.size());
      for (const auto &unit : input.units) {
        currentPositions.push_back(
            {unit.initialPositionX, unit.initialPositionY});
      }
    }
    std::vector<std::vector<TransformAccumulator>> transformAccumulators(
        input.units.size());
    std::vector<std::optional<float>> letterSpacingOffsets(input.units.size());

    if (input.units.empty() &&
        std::any_of(evaluatedProgram.stages.begin(),
                    evaluatedProgram.stages.end(), [](const auto &stage) {
                      return !stage.executionParameterBindings.empty();
                    })) {
      return fail(
          "text effect execution parameters require an evaluation unit");
    }

    const auto evaluateStage = [&](const TextEffectProgramStageIR &stage) {
      std::vector<double> registers(stage.registerCount, 0.0);
      internal::EllipticRowLayoutCache ellipticLayout;
      std::vector<std::optional<std::size_t>> pageSampleIndexes(
          stage.executionParameterBindings.size());
      const bool publishesCurrentPosition =
          StagePublishesCurrentPosition(stage);
      auto nextCurrentPositions = publishesCurrentPosition
                                      ? currentPositions
                                      : std::vector<CurrentPositionState>{};
      for (std::size_t unitIndex = 0U; unitIndex < input.units.size();
           ++unitIndex) {
        std::fill(registers.begin(), registers.end(), 0.0);
        for (const auto &instruction : stage.instructions) {
          if (!EvaluateInstruction(evaluatedProgram, instruction, input,
                                   rowUnitCounts, columnUnitCounts,
                                   rowInitialPositionSpans,
                                   maximumNormalUnitExtent,
                                   currentPositions,
                                   sequentialRandom,
                                   ellipticLayout,
                                   unitIndex, registers, error)) {
            return false;
          }
        }
        for (const auto &binding : stage.outputs) {
          if (binding.output == TextEffectOutput::LetterSpacingOffsetEm ||
              binding.output == TextEffectOutput::LetterSpacingOffsetMaxEm) {
            const float em =
                binding.output == TextEffectOutput::LetterSpacingOffsetMaxEm
                    ? maximumFontSize : input.units[unitIndex].fontSize;
            const double offset = registers[binding.registerIndex] * em;
            if (!std::isfinite(offset) ||
                std::fabs(offset) > std::numeric_limits<float>::max())
              return fail("text effect letter-spacing offset is invalid");
            letterSpacingOffsets[unitIndex] = static_cast<float>(offset);
            continue;
          }
          if (!ApplyOutput(binding, registers[binding.registerIndex],
                           input.units[unitIndex], output.units[unitIndex],
                           unitIndex, input.units.size(),
                           transformAccumulators[unitIndex],
                           publishesCurrentPosition
                               ? &nextCurrentPositions[unitIndex]
                               : nullptr,
                           error)) {
            return false;
          }
        }
        for (std::size_t bindingIndex = 0U;
             bindingIndex < stage.executionParameterBindings.size();
             ++bindingIndex) {
          const auto &binding =
              stage.executionParameterBindings[bindingIndex];
          std::vector<float> values;
          values.reserve(binding.registerIndices.size());
          for (const auto registerIndex : binding.registerIndices) {
            const double component = registers[registerIndex];
            if (!std::isfinite(component) ||
                component <
                    -static_cast<double>(std::numeric_limits<float>::max()) ||
                component >
                    static_cast<double>(std::numeric_limits<float>::max())) {
              return fail(
                  "text effect execution parameter is outside float range");
            }
            values.push_back(StoreQtTextBinary32(
                static_cast<float>(component)));
          }
          if (binding.parameter ==
                  TextEffectExecutionParameterKind::PostEffectProgress &&
              (values[0] < 0.0F || values[0] > 1.0F)) {
            return fail("text effect post progress is outside [0, 1]");
          }
          if (binding.parameter ==
                  TextEffectExecutionParameterKind::PostEffectBlurRadius &&
              values[0] < 0.0F) {
            return fail("text effect post blur radius is negative");
          }
          if (binding.parameter ==
                  TextEffectExecutionParameterKind::SceneOpacity &&
              (values[0] < 0.0F || values[0] > 1.0F)) {
            return fail("text effect scene opacity is outside [0, 1]");
          }

          if (binding.domain == TextEffectExecutionParameterDomain::Page) {
            if (!pageSampleIndexes[bindingIndex]) {
              if (output.executionParameters.size() >=
                  kMaximumExecutionParameterSamples) {
                return fail(
                    "text effect execution parameter sample budget exceeded");
              }
              pageSampleIndexes[bindingIndex] =
                  output.executionParameters.size();
              output.executionParameters.push_back(
                  {binding.nodeId, binding.parameter, binding.domain,
                   binding.valueSpace, binding.slot, std::nullopt,
                   std::move(values)});
            } else if (output.executionParameters
                           [*pageSampleIndexes[bindingIndex]]
                               .values != values) {
              return fail(
                  "text effect page parameter depends on a unit value");
            }
          } else {
            if (output.executionParameters.size() >=
                kMaximumExecutionParameterSamples) {
              return fail(
                  "text effect execution parameter sample budget exceeded");
            }
            output.executionParameters.push_back(
                {binding.nodeId, binding.parameter, binding.domain,
                 binding.valueSpace, binding.slot,
                 input.units[unitIndex].stableUnitId, std::move(values)});
          }
        }
      }
      if (publishesCurrentPosition)
        currentPositions.swap(nextCurrentPositions);
      if (evidence && !input.units.empty()) {
        TextProgramStageExecutionEvidence executed;
        executed.stageId = stage.stageId;
        executed.evaluatedUnits = input.units.size();
        executed.evaluatedInstructions = stage.instructions.size() * input.units.size();
        executed.appliedOutputs = stage.outputs.size() * input.units.size();
        const auto record = [&](const std::string_view prefix,
                                const std::string_view name) {
          std::string capability(prefix);
          capability.append(name);
          if (std::find(executed.capabilities.begin(), executed.capabilities.end(),
                        capability) == executed.capabilities.end())
            executed.capabilities.push_back(std::move(capability));
        };
        // The interpreter above is straight-line: every instruction and
        // output was evaluated for every unit before this success boundary.
        record("effect-stage:", TextEffectStageKindName(stage.kind));
        for (const auto &instruction : stage.instructions) {
          record("effect-opcode:", TextEffectOpcodeName(instruction.opcode));
          if (instruction.opcode == TextEffectOpcode::Input && instruction.input)
            record("effect-input:", TextEffectInputName(*instruction.input));
        }
        for (const auto &binding : stage.outputs)
          record("effect-output:", TextEffectOutputName(binding.output));
        for (const auto &binding : stage.executionParameterBindings) {
          executed.publishedBindings += binding.domain ==
              TextEffectExecutionParameterDomain::Page ? 1U : input.units.size();
          record("effect-binding-target:", TextEffectExecutionParameterKindName(binding.parameter));
          record("effect-binding-domain:", TextEffectExecutionParameterDomainName(binding.domain));
          record("effect-binding-space:", TextEffectExecutionParameterSpaceName(binding.valueSpace));
        }
        evidence->stages.push_back(std::move(executed));
      }
      return true;
    };

    for (const auto &stage : evaluatedProgram.stages) {
      if (!evaluateStage(stage)) {
        ResetFramePlan(output);
        return false;
      }
    }

    for (std::size_t unitIndex = 0U; unitIndex < output.units.size();
         ++unitIndex) {
      auto &unit = output.units[unitIndex];
      if (letterSpacingOffsets[unitIndex]) {
        output.layoutMutations.push_back(
            {TextEffectLayoutMutationKind::LetterSpacing, unit.stableUnitId,
             TextPropertyCombineMode::Add, *letterSpacingOffsets[unitIndex]});
      }
      if (unit.absoluteFontSize) {
        output.layoutMutations.push_back(
            {TextEffectLayoutMutationKind::AbsoluteFontSize,
             unit.stableUnitId, TextPropertyCombineMode::Replace,
             *unit.absoluteFontSize});
      }
      for (const auto &accumulator : transformAccumulators[unitIndex]) {
        const auto &components = accumulator.components;
        if (components.anchorRangeBegin >= components.anchorRangeEnd ||
            components.anchorRangeEnd > input.units.size()) {
          return fail("text effect transform anchor range is invalid");
        }
        const auto tightBounds =
            AnchorBounds(input, components.anchorRangeBegin,
                         components.anchorRangeEnd, true);
        const auto layoutBounds =
            AnchorBounds(input, components.anchorRangeBegin,
                         components.anchorRangeEnd, false);
        if (accumulator.hasDirectMatrixOutput) {
          const auto &matrix = accumulator.directMatrix.columnMajor;
          if (!std::all_of(matrix.begin(), matrix.end(), [](const float value) {
                return std::isfinite(value) && std::fabs(value) <= 1.0e9F;
              }) ||
              std::fabs(matrix[3]) + std::fabs(matrix[7]) +
                      std::fabs(matrix[11]) + std::fabs(matrix[15]) <
                  1.0e-8F) {
            return fail("text effect direct transform matrix is invalid");
          }
          unit.transforms.push_back(
              {accumulator.directMatrix, tightBounds, layoutBounds});
          continue;
        }
        const auto resolved = ResolveQtTextProgramTransformPlan(
            components, tightBounds, layoutBounds, input.textRect);
        if (!resolved)
          return fail("text effect decomposed transform is invalid");
        auto transform = *resolved;
        TextEffectProgramTransformRetargetPlan retarget;
        retarget.sourceComponents = components;
        retarget.anchorStableUnitIds.reserve(
            components.anchorRangeEnd - components.anchorRangeBegin);
        for (auto anchorIndex = components.anchorRangeBegin;
             anchorIndex < components.anchorRangeEnd; ++anchorIndex) {
          retarget.anchorStableUnitIds.push_back(
              input.units[anchorIndex].stableUnitId);
        }
        transform.programRetarget = std::move(retarget);
        unit.transforms.push_back(std::move(transform));
      }
    }
    StoreQtTextEffectFramePlanBinary32(output);
    if (evidence) {
      evidence->programId = program.programId;
      evidence->completed = true;
    }
  } catch (...) {
    return fail("text effect frame evaluation allocation failed");
  }
  return true;
}

namespace {

void AddEvaluationDiagnostic(TextEffectEvaluationResult &result,
                             std::string code, std::string subject,
                             std::string message) {
  result.diagnostics.push_back(
      {std::move(code), DiagnosticSeverity::Error, "text-effect-evaluation",
       std::move(subject), std::move(message)});
}

bool ValidEvaluationRect(const TextEffectRect &rect) noexcept {
  return FiniteRect(rect) && std::isfinite(rect.x + rect.width) &&
         std::isfinite(rect.y + rect.height);
}

TextEffectRect ExpandRect(const TextEffectRect &rect, const float left,
                          const float top, const float right,
                          const float bottom) noexcept {
  if (!NonEmptyRect(rect))
    return rect;
  return {rect.x - left, rect.y - top,
          rect.width + left + right, rect.height + top + bottom};
}

float RectCenterX(const TextEffectRect &rect) noexcept {
  return rect.x + rect.width * 0.5F;
}

float RectCenterY(const TextEffectRect &rect) noexcept {
  return rect.y + rect.height * 0.5F;
}

TextEffectRect ScaleRect(const TextEffectRect &rect,
                         const float scale) noexcept {
  return {QtTextBinary32Multiply(rect.x, scale),
          QtTextBinary32Multiply(rect.y, scale),
          QtTextBinary32Multiply(rect.width, scale),
          QtTextBinary32Multiply(rect.height, scale)};
}

bool ValidEvaluationFrame(const TextEffectFrameInput &frame,
                          std::string &error) {
  if (!std::isfinite(frame.progress) || frame.progress < 0.0 ||
      frame.progress > 1.0 || frame.units.size() > kMaximumFrameUnits ||
      frame.rowRects.size() > kMaximumFrameUnits ||
      frame.tightRowRects.size() != frame.rowRects.size() ||
      !ValidEvaluationRect(frame.textRect) ||
      !ValidEvaluationRect(frame.tightTextRect) ||
      (frame.canvasRect && !ValidEvaluationRect(*frame.canvasRect)) ||
      !std::all_of(frame.rowRects.begin(), frame.rowRects.end(),
                   ValidEvaluationRect) ||
      !std::all_of(frame.tightRowRects.begin(), frame.tightRowRects.end(),
                   ValidEvaluationRect)) {
    error = "text effect evaluation geometry is invalid";
    return false;
  }
  std::unordered_set<std::uint64_t> identities;
  identities.reserve(frame.units.size());
  for (std::size_t index = 0U; index < frame.units.size(); ++index) {
    const auto &unit = frame.units[index];
    if (unit.index != index || unit.renderIndex >= frame.units.size() ||
        unit.row >= frame.rowRects.size() ||
        unit.visualLineIndex >= frame.rowRects.size() ||
        unit.indexInRow >= frame.units.size() ||
        !std::isfinite(unit.fontSize) || unit.fontSize < 0.0F ||
        !ValidEvaluationRect(unit.rect) ||
        !ValidEvaluationRect(unit.tightRect) ||
        !std::isfinite(unit.initialPositionX) ||
        !std::isfinite(unit.initialPositionY) ||
        !std::isfinite(unit.baselineY) ||
        unit.documentUtf8Begin > unit.documentUtf8End ||
        unit.documentWordUtf8Begin > unit.documentWordUtf8End ||
        (!unit.contentSlotId.empty() && !IsValidUtf8(unit.contentSlotId)) ||
        (!unit.paragraphId.empty() && !IsValidUtf8(unit.paragraphId)) ||
        (!unit.runId.empty() && !IsValidUtf8(unit.runId)) ||
        !identities.emplace(unit.stableUnitId).second) {
      error = "text effect evaluation unit identity or geometry is invalid";
      return false;
    }
  }
  const bool suppliesNormalTopology =
      frame.normalUnitCount.has_value() ||
      std::any_of(frame.units.begin(), frame.units.end(),
                  [](const auto &unit) {
                    return unit.normalUnitIndex.has_value();
                  });
  if (suppliesNormalTopology) {
    if (!frame.normalUnitCount ||
        *frame.normalUnitCount > frame.units.size()) {
      error = "text effect normal-unit topology is invalid";
      return false;
    }
    std::size_t expected = 0U;
    for (const auto &unit : frame.units) {
      if (!unit.normalUnitIndex)
        continue;
      if (*unit.normalUnitIndex != expected ||
          expected >= *frame.normalUnitCount) {
        error = "text effect normal-unit topology is not dense";
        return false;
      }
      ++expected;
    }
    if (expected != *frame.normalUnitCount) {
      error = "text effect normal-unit count is inconsistent";
      return false;
    }
  }
  return true;
}

bool TargetContainsUnit(const TextPropertyTarget &target,
                        const TextEffectUnitInput &unit) {
  if (!target.contentSlotId.empty() &&
      target.contentSlotId != unit.contentSlotId) {
    return false;
  }
  if (!target.paragraphIds.empty() &&
      std::find(target.paragraphIds.begin(), target.paragraphIds.end(),
                unit.paragraphId) == target.paragraphIds.end()) {
    return false;
  }
  if (!target.runIds.empty() &&
      std::find(target.runIds.begin(), target.runIds.end(), unit.runId) ==
          target.runIds.end()) {
    return false;
  }
  if (target.range &&
      (target.range->end <= unit.documentUtf8Begin ||
       target.range->begin >= unit.documentUtf8End)) {
    return false;
  }
  // layerId identifies a material/backdrop layer, not a drawable unit.  The
  // layer pass applies that identity after unit targeting; filtering here
  // would make every non-empty layer target match zero units because units do
  // not carry a layer-id field.
  return true;
}

bool AnimatorContainsUnit(const TextAnimatorSpec &animator,
                          const TextEffectUnitInput &unit) {
  if (animator.paragraphIds.empty() && animator.runIds.empty())
    return true;
  return std::find(animator.paragraphIds.begin(), animator.paragraphIds.end(),
                   unit.paragraphId) != animator.paragraphIds.end() ||
         std::find(animator.runIds.begin(), animator.runIds.end(),
                   unit.runId) != animator.runIds.end();
}

TextEffectRect UnitSetBounds(const TextEffectFrameInput &frame,
                             const std::vector<std::size_t> &indexes,
                             const bool tight = true) noexcept {
  TextEffectRect result{};
  for (const auto index : indexes) {
    if (index >= frame.units.size())
      continue;
    result = UnionRect(result,
                       tight ? frame.units[index].tightRect
                             : frame.units[index].rect);
  }
  return result;
}

struct EvaluationUnitGroup final {
  std::vector<std::size_t> indexes;
  TextEffectRect bounds{};
  TextEffectRect tightBounds{};
  std::uint64_t wordBegin{0U};
  std::uint64_t wordEnd{0U};
  std::size_t line{0U};
  std::size_t representativeIndex{std::numeric_limits<std::size_t>::max()};
};

std::vector<std::size_t>
VisualEvaluationOrder(const std::vector<std::size_t> &indexes,
                      const TextEffectFrameInput &frame) {
  auto result = indexes;
  std::stable_sort(result.begin(), result.end(), [&](const auto left,
                                                      const auto right) {
    if (left >= frame.units.size() || right >= frame.units.size())
      return left < right;
    const auto &lhs = frame.units[left];
    const auto &rhs = frame.units[right];
    if (lhs.visualLineIndex != rhs.visualLineIndex)
      return lhs.visualLineIndex < rhs.visualLineIndex;
    if (lhs.rect.x != rhs.rect.x)
      return lhs.rect.x < rhs.rect.x;
    if (lhs.documentUtf8Begin != rhs.documentUtf8Begin)
      return lhs.documentUtf8Begin < rhs.documentUtf8Begin;
    return lhs.index < rhs.index;
  });
  return result;
}

std::vector<EvaluationUnitGroup>
BuildEvaluationUnitGroups(const TextAnimatorSpec &animator,
                          const std::vector<std::size_t> &targetIndexes,
                          const TextEffectFrameInput &frame) {
  std::vector<EvaluationUnitGroup> result;
  const auto basis = animator.selectors.empty()
                         ? TextUnitBasis::All
                         : animator.selectors.front().basedOn;
  const auto visualOrder = VisualEvaluationOrder(targetIndexes, frame);
  using WordRange = std::pair<std::uint64_t, std::uint64_t>;
  std::map<WordRange, std::size_t> wordGroups;
  for (const auto index : visualOrder) {
    if (index >= frame.units.size())
      continue;
    const auto &unit = frame.units[index];
    auto found = result.end();
    if (basis == TextUnitBasis::All && !result.empty()) {
      found = result.begin();
    } else if (basis == TextUnitBasis::Word) {
      if (!result.empty() &&
          result.back().wordBegin == unit.documentWordUtf8Begin &&
          result.back().wordEnd == unit.documentWordUtf8End) {
        found = std::prev(result.end());
      } else {
        const auto [entry, inserted] = wordGroups.try_emplace(
            WordRange{unit.documentWordUtf8Begin, unit.documentWordUtf8End},
            result.size());
        if (!inserted)
          found = result.begin() + entry->second;
      }
    } else if (basis == TextUnitBasis::Line && !result.empty() &&
               result.back().line == unit.visualLineIndex) {
      // Valid target units are sorted by visualLineIndex first, so each line
      // is contiguous. Word groups may reappear on later lines and use ordinals
      // above to preserve first occurrence across result vector growth.
      found = std::prev(result.end());
    }
    if (found == result.end()) {
      result.push_back({});
      found = std::prev(result.end());
      found->wordBegin = unit.documentWordUtf8Begin;
      found->wordEnd = unit.documentWordUtf8End;
      found->line = unit.visualLineIndex;
      found->representativeIndex = index;
    }
    found->indexes.push_back(index);
    // Selector topology is advance/layout based. Tight ink remains the
    // independently resolved transform-anchor domain below.
    found->bounds = UnionRect(found->bounds, unit.rect);
    found->tightBounds = UnionRect(found->tightBounds, unit.tightRect);
  }
  return result;
}

class EvaluationAnimatorGeometry final {
public:
  struct AnchorBounds final {
    TextEffectRect tight{};
    TextEffectRect layout{};
  };

  EvaluationAnimatorGeometry(const TextAnimatorSpec &animator,
                             const std::vector<std::size_t> &targetIndexes,
                             const TextEffectFrameInput &frame)
      : frame_(frame),
        basis_(animator.anchorBasis == TextAnimatorAnchorBasis::Page
                   ? TextAnimatorAnchorBasis::Page
               : animator.anchor == TextUnitAnchor::LineBox
                   ? TextAnimatorAnchorBasis::Line
                   : animator.anchorBasis) {
    // Fold in target order, not selector visual order: UnionRect rounding is
    // order-sensitive. Word anchors also include paragraph identity.
    for (const auto index : targetIndexes) {
      if (index >= frame.units.size())
        continue;
      const auto &unit = frame.units[index];
      scopeBounds = UnionRect(scopeBounds, unit.rect);
      const auto &positionBounds = NonEmptyRect(unit.programRect)
                                       ? unit.programRect
                                   : NonEmptyRect(unit.rect) ? unit.rect
                                                             : unit.tightRect;
      positionBasisBounds = UnionRect(positionBasisBounds, positionBounds);
      if (basis_ == TextAnimatorAnchorBasis::Page) {
        page_.tight = UnionRect(page_.tight, UnitBounds(unit).tight);
      } else if (basis_ == TextAnimatorAnchorBasis::Line) {
        Accumulate(lines_[unit.visualLineIndex], unit);
      } else if (basis_ == TextAnimatorAnchorBasis::Word) {
        Accumulate(words_[WordKeyFor(unit)], unit);
      }
    }
    page_.layout = scopeBounds;
    if (!NonEmptyRect(positionBasisBounds))
      positionBasisBounds = scopeBounds;
  }

  AnchorBounds anchors(const std::size_t unitIndex) const noexcept {
    if (unitIndex >= frame_.units.size())
      return {};
    const auto &unit = frame_.units[unitIndex];
    auto result = UnitBounds(unit);
    const AnchorBounds *aggregate = nullptr;
    if (basis_ == TextAnimatorAnchorBasis::Page) {
      aggregate = &page_;
    } else if (basis_ == TextAnimatorAnchorBasis::Line) {
      const auto found = lines_.find(unit.visualLineIndex);
      if (found != lines_.end())
        aggregate = &found->second;
    } else if (basis_ == TextAnimatorAnchorBasis::Word) {
      const auto found = words_.find(WordKeyFor(unit));
      if (found != words_.end())
        aggregate = &found->second;
    }
    // Letter identities are unique and targetIndexes contains each source at
    // most once. Empty aggregates retain the original per-unit fallback.
    if (aggregate) {
      if (NonEmptyRect(aggregate->tight))
        result.tight = aggregate->tight;
      if (NonEmptyRect(aggregate->layout))
        result.layout = aggregate->layout;
    }
    return result;
  }

  TextEffectRect scopeBounds{};
  TextEffectRect positionBasisBounds{};

private:
  using WordKey =
      std::tuple<std::string_view, std::uint64_t, std::uint64_t>;

  static WordKey WordKeyFor(const TextEffectUnitInput &unit) noexcept {
    return {unit.paragraphId, unit.documentWordUtf8Begin,
            unit.documentWordUtf8End};
  }

  static AnchorBounds UnitBounds(const TextEffectUnitInput &unit) noexcept {
    return {NonEmptyRect(unit.tightRect) ? unit.tightRect : unit.rect, unit.rect};
  }

  static void Accumulate(AnchorBounds &bounds,
                         const TextEffectUnitInput &unit) noexcept {
    bounds.tight = UnionRect(bounds.tight, UnitBounds(unit).tight);
    bounds.layout = UnionRect(bounds.layout, unit.rect);
  }

  // One animator evaluation borrows the immutable frame; word-key views and
  // these aggregates never escape that lifetime or survive into another frame.
  const TextEffectFrameInput &frame_;
  TextAnimatorAnchorBasis basis_;
  AnchorBounds page_{};
  std::unordered_map<std::size_t, AnchorBounds> lines_;
  std::map<WordKey, AnchorBounds> words_;
};

float ResolveEvaluationBaselineAnchor(
    const TextAnimatorSpec &animator, const std::size_t unitIndex,
    const TextEffectRect &anchorBounds,
    const TextEffectFrameInput &frame) noexcept {
  if (animator.anchor != TextUnitAnchor::Baseline ||
      unitIndex >= frame.units.size()) {
    return 0.0F;
  }
  if (!NonEmptyRect(anchorBounds))
    return 0.0F;
  return (frame.units[unitIndex].baselineY - RectCenterY(anchorBounds)) *
         2.0F / anchorBounds.height;
}

TextUnitAnimationSample ResolveEvaluationAnimatorPosition(
    const TextAnimatorSpec &animator, TextUnitAnimationSample sample,
    const TextEffectRect &positionBasisBounds,
    const TextEffectRect &scopeBounds, const TextEffectUnitInput &unit,
    const ReferenceCanvas &referenceCanvas) noexcept {
  switch (animator.positionMode) {
  case TextPositionMode::AbsolutePixels:
    break;
  case TextPositionMode::TextBoundsOffset:
    sample.positionX *= positionBasisBounds.width * 0.5F;
    // TextBoundsOffset is authored in TextPro's Y-up coordinate space while
    // the evaluated frame geometry is already in the renderer's Y-down
    // domain.  Keep this conversion scoped to the normalized bounds mode:
    // absolute-pixel and distance modes already resolve directly in frame
    // coordinates and must not be conjugated a second time.
    sample.positionY *= -positionBasisBounds.height * 0.5F;
    break;
  case TextPositionMode::DistanceFromCenter:
  case TextPositionMode::SpaceX:
  case TextPositionMode::SpaceY: {
    const double centerX =
        static_cast<double>(referenceCanvas.width) * 0.5 +
        static_cast<double>(sample.positionX) * scopeBounds.width * 0.5;
    const double centerY =
        static_cast<double>(referenceCanvas.height) * 0.5 +
        static_cast<double>(sample.positionY) * scopeBounds.height * 0.5;
    const float offsetX = static_cast<float>(
        (static_cast<double>(unit.initialPositionX) - centerX) *
        sample.distanceFromCenter);
    const float offsetY = static_cast<float>(
        (static_cast<double>(unit.initialPositionY) - centerY) *
        sample.distanceFromCenter);
    sample.positionX = animator.positionMode == TextPositionMode::SpaceY
                           ? 0.0F
                           : offsetX;
    sample.positionY = animator.positionMode == TextPositionMode::SpaceX
                           ? 0.0F
                           : offsetY;
    break;
  }
  }
  return sample;
}

void MergeEvaluationUnit(TextEffectUnitFramePlan &destination,
                         const TextEffectUnitFramePlan &source,
                         const TextPropertyCombineMode mode) {
  const auto combineScalar = [mode](const float current,
                                    const float incoming) noexcept {
    switch (mode) {
    case TextPropertyCombineMode::Replace:
      return incoming;
    case TextPropertyCombineMode::Add:
      return QtTextBinary32Add(current, incoming);
    case TextPropertyCombineMode::Multiply:
    case TextPropertyCombineMode::MatrixConcat:
    case TextPropertyCombineMode::ColorMix:
      return QtTextBinary32Multiply(current, incoming);
    }
    return incoming;
  };
  if (source.opacity) {
    if (!destination.opacity ||
        mode == TextPropertyCombineMode::Replace) {
      destination.opacity = source.opacity;
    } else {
      destination.opacity =
          combineScalar(*destination.opacity, *source.opacity);
    }
  }
  // Color samples are already authored-material-relative (see the animator
  // block below), so keep the complete sampled color rather than applying a
  // second channel-wise arithmetic operation here.
  if (source.instanceColor)
    destination.instanceColor = source.instanceColor;
  if (source.absoluteFontSize)
    destination.absoluteFontSize = source.absoluteFontSize;
  if (source.replacementCodepoint)
    destination.replacementCodepoint = source.replacementCodepoint;
  if (source.sdfBlurRadius) {
    if (!destination.sdfBlurRadius ||
        mode == TextPropertyCombineMode::Replace) {
      destination.sdfBlurRadius = source.sdfBlurRadius;
    } else {
      destination.sdfBlurRadius =
          std::max(0.0F, combineScalar(*destination.sdfBlurRadius,
                                       *source.sdfBlurRadius));
    }
  }
  destination.transforms.insert(destination.transforms.end(),
                                source.transforms.begin(),
                                source.transforms.end());
}

void MergeAnimatorEvaluationUnit(TextEffectUnitFramePlan &destination,
                                 const TextEffectUnitFramePlan &source) {
  // Text_BaseSelector animators within one Studio animation layer are a
  // stack, not independent layer replacements. Qt concatenates their
  // transforms, multiplies alpha, and accumulates scalar blur before the
  // containing layer's combine mode is applied to earlier layers. Using the
  // layer combine mode for every animator makes a Replace layer retain only
  // the last selector's alpha while accidentally keeping all transforms.
  if (source.opacity) {
    destination.opacity = QtTextBinary32Multiply(
        destination.opacity.value_or(1.0F), *source.opacity);
  }
  if (source.instanceColor)
    destination.instanceColor = source.instanceColor;
  if (source.absoluteFontSize)
    destination.absoluteFontSize = source.absoluteFontSize;
  if (source.replacementCodepoint)
    destination.replacementCodepoint = source.replacementCodepoint;
  if (source.sdfBlurRadius) {
    destination.sdfBlurRadius = std::max(
        0.0F, QtTextBinary32Add(destination.sdfBlurRadius.value_or(0.0F),
                                *source.sdfBlurRadius));
  }
  destination.transforms.insert(destination.transforms.end(),
                                source.transforms.begin(),
                                source.transforms.end());
}

// Unit samples are merged separately with the layer's animator stacking rules.
void AppendEvaluationPlanNonUnitData(TextEffectFramePlan &destination,
                                     const TextEffectFramePlan &source) {
  destination.sampledProperties.insert(destination.sampledProperties.end(),
                                       source.sampledProperties.begin(),
                                       source.sampledProperties.end());
  destination.layoutMutations.insert(destination.layoutMutations.end(),
                                     source.layoutMutations.begin(),
                                     source.layoutMutations.end());
  destination.glyphMaterialPasses.insert(
      destination.glyphMaterialPasses.end(),
      source.glyphMaterialPasses.begin(), source.glyphMaterialPasses.end());
  destination.backdropPasses.insert(destination.backdropPasses.end(),
                                    source.backdropPasses.begin(),
                                    source.backdropPasses.end());
  destination.decorationPasses.insert(destination.decorationPasses.end(),
                                      source.decorationPasses.begin(),
                                      source.decorationPasses.end());
  destination.postEffectNodes.insert(destination.postEffectNodes.end(),
                                     source.postEffectNodes.begin(),
                                     source.postEffectNodes.end());
  destination.compositeOrder.insert(destination.compositeOrder.end(),
                                    source.compositeOrder.begin(),
                                    source.compositeOrder.end());
  destination.resources.insert(destination.resources.end(),
                               source.resources.begin(),
                               source.resources.end());
  destination.stateTransitions.insert(destination.stateTransitions.end(),
                                      source.stateTransitions.begin(),
                                      source.stateTransitions.end());
  destination.executionParameters.insert(
      destination.executionParameters.end(),
      source.executionParameters.begin(), source.executionParameters.end());
  destination.executionGraph.nodes.insert(
      destination.executionGraph.nodes.end(),
      source.executionGraph.nodes.begin(), source.executionGraph.nodes.end());
}

struct ProgramSourceFrameResult final {
  TextEffectFrameInput frame;
  std::vector<std::vector<std::size_t>> sourceIndexesByUnit;
};

std::pair<float, float> ReferencePointToQtTextProgramSource(
    const float x, const float y, const ReferenceCanvas &referenceCanvas,
    const float sourceToReferenceScale) noexcept {
  const float inverse =
      QtTextBinary32Divide(1.0F, sourceToReferenceScale);
  const float centerX =
      QtTextBinary32Multiply(referenceCanvas.width, 0.5F);
  const float centerY =
      QtTextBinary32Multiply(referenceCanvas.height, 0.5F);
  return {
      QtTextBinary32Multiply(QtTextBinary32Subtract(x, centerX), inverse),
      QtTextBinary32Multiply(QtTextBinary32Subtract(centerY, y), inverse)};
}

TextEffectRect ReferenceRectToQtTextProgramSource(
    const TextEffectRect &rect, const ReferenceCanvas &referenceCanvas,
    const float sourceToReferenceScale) noexcept {
  if (!NonEmptyRect(rect))
    return {};
  const auto lowerLeft = ReferencePointToQtTextProgramSource(
      rect.x, QtTextBinary32Add(rect.y, rect.height), referenceCanvas,
      sourceToReferenceScale);
  const float inverse =
      QtTextBinary32Divide(1.0F, sourceToReferenceScale);
  return {lowerLeft.first, lowerLeft.second,
          QtTextBinary32Multiply(rect.width, inverse),
          QtTextBinary32Multiply(rect.height, inverse)};
}

ProgramSourceFrameResult ProgramSourceFrame(
    const TextEffectEvaluationRequest &request,
    const std::vector<EvaluationUnitGroup> &groups, const double progress,
    const std::int64_t clipLocalTimeUs, const std::int64_t animationDurationUs) {
  ProgramSourceFrameResult result;
  result.frame.progress = progress;
  result.frame.timeUs = clipLocalTimeUs;
  result.frame.animationDurationUs = animationDurationUs;
  result.frame.outputSize = request.frame.outputSize;
  result.frame.sourceToOutputScale = request.frame.sourceToOutputScale;
  result.frame.sourceOriginOutput = request.frame.sourceOriginOutput;
  result.frame.writingMode = request.frame.writingMode;
  // SDFText exposes the shaped advance/font-em canvas through text.rect.
  // The document wrapping box and padded Letter union are different inputs;
  // neither can substitute for that canvas in material/animation uniforms.
  // Legacy Text instead exposes the union of its padded Letter rectangles.
  // Restore that native program contract below, after grouping the Letters;
  // the paragraph wrapping box is not an animation geometry input.
  const bool legacyText = request.sourceCreationComponent ==
                          TextSourceCreationComponent::LegacyText;
  if (!legacyText) {
    result.frame.textRect = ReferenceRectToQtTextProgramSource(
        request.frame.canvasRect.value_or(request.frame.textRect),
        request.referenceCanvas, request.sourceToReferenceScale);
    result.frame.tightTextRect = ReferenceRectToQtTextProgramSource(
        request.frame.tightTextRect, request.referenceCanvas,
        request.sourceToReferenceScale);
  }
  if (request.frame.canvasRect) {
    result.frame.canvasRect = ReferenceRectToQtTextProgramSource(
        *request.frame.canvasRect, request.referenceCanvas,
        request.sourceToReferenceScale);
  }
  result.frame.units.reserve(groups.size());
  result.sourceIndexesByUnit.reserve(groups.size());
  std::unordered_map<std::size_t, std::size_t> nextIndexInRow;
  std::size_t nextRenderIndex = 0U;
  for (std::size_t groupIndex = 0U; groupIndex < groups.size();
       ++groupIndex) {
    const auto &group = groups[groupIndex];
    if (group.indexes.empty() ||
        group.representativeIndex >= request.frame.units.size()) {
      continue;
    }
    const auto &representative =
        request.frame.units[group.representativeIndex];
    TextEffectRect sourceBounds{};
    for (const auto sourceIndex : group.indexes) {
      if (sourceIndex >= request.frame.units.size())
        continue;
      const auto &source = request.frame.units[sourceIndex];
      sourceBounds = UnionRect(
          sourceBounds, NonEmptyRect(source.programRect) ? source.programRect
                                                        : source.rect);
    }
    if (!NonEmptyRect(sourceBounds))
      sourceBounds = group.bounds;
    // The legacy Qt Effect Program ABI exposed one padded Letter/script rect
    // per program unit. It had no independent tight-ink input, so transform
    // anchor ranges and selector geometry both consumed that same rect. Keep
    // richer product-frame tight bounds outside this private projection frame.
    const auto tightBounds = sourceBounds;
    const auto row = representative.visualLineIndex;
    if (result.frame.rowRects.size() <= row) {
      result.frame.rowRects.resize(row + 1U);
      result.frame.tightRowRects.resize(row + 1U);
    }

    TextEffectUnitInput unit;
    unit.stableUnitId =
        static_cast<std::uint64_t>(result.sourceIndexesByUnit.size());
    unit.index = result.frame.units.size();
    unit.renderIndex = nextRenderIndex;
    unit.type = representative.type;
    unit.unicodeCodepoint = representative.unicodeCodepoint;
    unit.row = row;
    unit.indexInRow = nextIndexInRow[row]++;
    unit.fontSize = QtTextBinary32Divide(
        representative.fontSize, request.sourceToReferenceScale);
    unit.tightRect = ReferenceRectToQtTextProgramSource(
        tightBounds, request.referenceCanvas,
        request.sourceToReferenceScale);
    unit.rect = ReferenceRectToQtTextProgramSource(
        sourceBounds, request.referenceCanvas,
        request.sourceToReferenceScale);
    unit.programRect = unit.rect;
    const auto initialPosition = ReferencePointToQtTextProgramSource(
        representative.initialPositionX, representative.initialPositionY,
        request.referenceCanvas, request.sourceToReferenceScale);
    unit.initialPositionX = initialPosition.first;
    unit.initialPositionY = initialPosition.second;
    unit.instanceColor = representative.instanceColor;
    unit.contentSlotId = representative.contentSlotId;
    unit.paragraphId = representative.paragraphId;
    unit.runId = representative.runId;
    unit.documentUtf8Begin = representative.documentUtf8Begin;
    unit.documentUtf8End = representative.documentUtf8End;
    unit.documentWordUtf8Begin = representative.documentWordUtf8Begin;
    unit.documentWordUtf8End = representative.documentWordUtf8End;
    unit.visualLineIndex = row;
    unit.baselineY = ReferencePointToQtTextProgramSource(
                         0.0F, representative.baselineY,
                         request.referenceCanvas,
                         request.sourceToReferenceScale)
                         .second;
    for (const auto sourceIndex : group.indexes) {
      if (sourceIndex >= request.frame.units.size())
        continue;
      const auto &source = request.frame.units[sourceIndex];
      unit.documentUtf8Begin =
          std::min(unit.documentUtf8Begin, source.documentUtf8Begin);
      unit.documentUtf8End =
          std::max(unit.documentUtf8End, source.documentUtf8End);
      if (source.unicodeCodepoint != unit.unicodeCodepoint)
        unit.unicodeCodepoint = 0U;
    }
    if (unit.type == 0) {
      unit.normalUnitIndex = nextRenderIndex;
      ++nextRenderIndex;
    }
    if (legacyText) {
      result.frame.textRect = UnionRect(result.frame.textRect, unit.rect);
      result.frame.tightTextRect =
          UnionRect(result.frame.tightTextRect, unit.tightRect);
    }
    result.frame.units.push_back(std::move(unit));
    result.sourceIndexesByUnit.push_back(group.indexes);
    result.frame.rowRects[row] = UnionRect(
        result.frame.rowRects[row], ReferenceRectToQtTextProgramSource(
                                        sourceBounds, request.referenceCanvas,
                                        request.sourceToReferenceScale));
    result.frame.tightRowRects[row] = UnionRect(
        result.frame.tightRowRects[row],
        ReferenceRectToQtTextProgramSource(
            tightBounds, request.referenceCanvas,
            request.sourceToReferenceScale));
  }
  result.frame.normalUnitCount = nextRenderIndex;
  return result;
}

template <typename PublishUnit>
TextEffectFramePlan ExpandProgramPlanToSourceUnits(
    TextEffectFramePlan result,
    const ProgramSourceFrameResult &programInput,
    const TextEffectFrameInput &sourceFrame, PublishUnit &&publishUnit) {
  auto units = std::move(result.units);
  const auto layoutMutations = std::move(result.layoutMutations);
  const auto executionParameters = std::move(result.executionParameters);
  result.units.clear();
  result.layoutMutations.clear();
  result.executionParameters.clear();
  for (const auto &parameter : executionParameters) {
    if (parameter.domain == TextEffectExecutionParameterDomain::Page) {
      result.executionParameters.push_back(parameter);
      continue;
    }
    if (!parameter.stableUnitId ||
        *parameter.stableUnitId >=
            programInput.sourceIndexesByUnit.size()) {
      continue;
    }
    for (const auto sourceIndex :
         programInput.sourceIndexesByUnit[*parameter.stableUnitId]) {
      if (sourceIndex >= sourceFrame.units.size())
        continue;
      auto reboundParameter = parameter;
      reboundParameter.stableUnitId =
          sourceFrame.units[sourceIndex].stableUnitId;
      result.executionParameters.push_back(std::move(reboundParameter));
    }
  }
  // ProgramSourceFrame assigns dense positional identities, and successful
  // program evaluation preserves that order for units and layout mutations.
  // Publish each rebound sample with its source position; the caller merges
  // it immediately instead of storing and looking up an expanded unit array.
  std::size_t nextMutation = 0U;
  for (std::size_t programUnitIndex = 0U;
       programUnitIndex < programInput.sourceIndexesByUnit.size();
       ++programUnitIndex) {
    const auto stableProgramUnitId =
        static_cast<std::uint64_t>(programUnitIndex);
    auto &rebound = units[programUnitIndex];
    const auto mutationBegin = nextMutation;
    while (nextMutation < layoutMutations.size() &&
           layoutMutations[nextMutation].stableUnitId == stableProgramUnitId) {
      ++nextMutation;
    }
    // Anchors depend on the program group, not the source unit receiving it.
    // Reuse this owned sample; publishUnit consumes it without retaining refs.
    for (auto &transform : rebound.transforms) {
      if (!transform.programRetarget)
        continue;
      std::vector<std::uint64_t> reboundAnchorIds;
      for (const auto programStableUnitId :
           transform.programRetarget->anchorStableUnitIds) {
        if (programStableUnitId >= programInput.sourceIndexesByUnit.size()) {
          reboundAnchorIds.clear();
          break;
        }
        for (const auto anchorSourceIndex :
             programInput.sourceIndexesByUnit[programStableUnitId]) {
          if (anchorSourceIndex >= sourceFrame.units.size()) {
            reboundAnchorIds.clear();
            break;
          }
          const auto anchorStableUnitId =
              sourceFrame.units[anchorSourceIndex].stableUnitId;
          if (std::find(reboundAnchorIds.begin(), reboundAnchorIds.end(),
                        anchorStableUnitId) == reboundAnchorIds.end()) {
            reboundAnchorIds.push_back(anchorStableUnitId);
          }
        }
        if (reboundAnchorIds.empty())
          break;
      }
      transform.programRetarget->anchorStableUnitIds =
          std::move(reboundAnchorIds);
    }
    for (const auto sourceIndex :
         programInput.sourceIndexesByUnit[programUnitIndex]) {
      if (sourceIndex >= sourceFrame.units.size())
        continue;
      rebound.stableUnitId = sourceFrame.units[sourceIndex].stableUnitId;
      publishUnit(sourceIndex, rebound);
      for (auto mutationIndex = mutationBegin; mutationIndex < nextMutation;
           ++mutationIndex) {
        auto reboundMutation = layoutMutations[mutationIndex];
        reboundMutation.stableUnitId =
            sourceFrame.units[sourceIndex].stableUnitId;
        result.layoutMutations.push_back(std::move(reboundMutation));
      }
    }
  }
  return result;
}

bool ProjectProgramPlan(TextEffectFramePlan &plan,
                        const TextEffectExecutionGraphFramePlan &executionGraph,
                        const float sourceToReferenceScale,
                        const ReferenceCanvas &referenceCanvas) noexcept {
  for (auto &mutation : plan.layoutMutations) {
    if (mutation.kind == TextEffectLayoutMutationKind::LetterSpacing ||
        mutation.kind == TextEffectLayoutMutationKind::AbsoluteFontSize) {
      mutation.value *= sourceToReferenceScale;
      if (!std::isfinite(mutation.value))
        return false;
    }
  }
  for (auto &unit : plan.units) {
    // Program font sizes use the same source units as their input. Both the
    // unit sample and the layout mutation must return to shaping coordinates.
    if (unit.absoluteFontSize) {
      *unit.absoluteFontSize *= sourceToReferenceScale;
      if (!std::isfinite(*unit.absoluteFontSize))
        return false;
    }
    if (unit.sdfBlurRadius)
      unit.sdfBlurRadius = *unit.sdfBlurRadius * sourceToReferenceScale;
    for (auto &transform : unit.transforms) {
      if (!ProjectTextEffectTransformPlanToReference(
              transform, sourceToReferenceScale, referenceCanvas)) {
        return false;
      }
    }
  }
  const auto findExecutionNode =
      [&](const auto &self,
          const std::vector<TextEffectExecutionNodeFramePlan> &nodes,
          const std::string &nodeId)
      -> const TextEffectExecutionNodeFramePlan * {
    for (const auto &node : nodes) {
      if (node.nodeId == nodeId)
        return &node;
      if (const auto *found = self(self, node.children, nodeId))
        return found;
    }
    return nullptr;
  };
  for (auto &parameter : plan.executionParameters) {
    if (parameter.valueSpace !=
        TextEffectExecutionParameterSpace::SourcePixels) {
      continue;
    }
    const auto *target = findExecutionNode(
        findExecutionNode, executionGraph.nodes, parameter.nodeId);
    if (target &&
        target->capability == TextEffectExecutionCapability::MediaParticle &&
        parameter.parameter == TextEffectExecutionParameterKind::NodeVector4 &&
        parameter.values.size() == 4U) {
      if (parameter.slot == 0U) {
        // Particle emitters carry a source-space center followed by positive
        // half-extents. Points need the centered Qt source transform, while
        // extents need scale only.
        const auto center = ReferencePointToQtTextProgramSource(
            referenceCanvas.width * 0.5F,
            referenceCanvas.height * 0.5F, referenceCanvas,
            sourceToReferenceScale);
        parameter.values[0] = QtTextBinary32Add(
            referenceCanvas.width * 0.5F,
            QtTextBinary32Multiply(
                QtTextBinary32Subtract(parameter.values[0], center.first),
                sourceToReferenceScale));
        parameter.values[1] = QtTextBinary32Subtract(
            referenceCanvas.height * 0.5F,
            QtTextBinary32Multiply(
                QtTextBinary32Subtract(parameter.values[1], center.second),
                sourceToReferenceScale));
        parameter.values[2] = QtTextBinary32Multiply(
            parameter.values[2], sourceToReferenceScale);
        parameter.values[3] = QtTextBinary32Multiply(
            parameter.values[3], sourceToReferenceScale);
        parameter.valueSpace =
            TextEffectExecutionParameterSpace::ReferencePixels;
        continue;
      }
      if (parameter.slot == 2U || parameter.slot == 3U) {
        // Velocity/acceleration are x/y vectors followed by non-negative
        // random amplitudes. Qt source Y is up; reference Y is down.
        parameter.values[0] = QtTextBinary32Multiply(
            parameter.values[0], sourceToReferenceScale);
        parameter.values[1] = QtTextBinary32Multiply(
            -parameter.values[1], sourceToReferenceScale);
        parameter.values[2] = QtTextBinary32Multiply(
            parameter.values[2], sourceToReferenceScale);
        parameter.values[3] = QtTextBinary32Multiply(
            parameter.values[3], sourceToReferenceScale);
        parameter.valueSpace =
            TextEffectExecutionParameterSpace::ReferencePixels;
        continue;
      }
      if (parameter.slot == 4U) {
        for (auto &component : parameter.values)
          component = QtTextBinary32Multiply(component,
                                             sourceToReferenceScale);
        parameter.valueSpace =
            TextEffectExecutionParameterSpace::ReferencePixels;
        continue;
      }
    }
    if (parameter.parameter ==
            TextEffectExecutionParameterKind::SceneTranslation &&
        parameter.values.size() == 2U) {
      parameter.values[0] = QtTextBinary32Multiply(
          parameter.values[0], sourceToReferenceScale);
      parameter.values[1] = QtTextBinary32Multiply(
          -parameter.values[1], sourceToReferenceScale);
      parameter.valueSpace =
          TextEffectExecutionParameterSpace::ReferencePixels;
      continue;
    }
    for (auto &component : parameter.values) {
      component = QtTextBinary32Multiply(component, sourceToReferenceScale);
      if (!std::isfinite(component))
        return false;
    }
    parameter.valueSpace =
        TextEffectExecutionParameterSpace::ReferencePixels;
  }
  return true;
}

TextEffectRect FitDecorationRect(
    const TextEffectRect &target, const float intrinsicWidth,
    const float intrinsicHeight, const TextAnimatedDecorationFit fit) noexcept {
  if (!NonEmptyRect(target) || intrinsicWidth <= 0.0F ||
      intrinsicHeight <= 0.0F || fit == TextAnimatedDecorationFit::Stretch)
    return target;
  const float widthScale = target.width / intrinsicWidth;
  const float heightScale = target.height / intrinsicHeight;
  float scale = 1.0F;
  switch (fit) {
  case TextAnimatedDecorationFit::Inherit:
  case TextAnimatedDecorationFit::Contain:
    scale = std::min(widthScale, heightScale);
    break;
  case TextAnimatedDecorationFit::Cover:
    scale = std::max(widthScale, heightScale);
    break;
  case TextAnimatedDecorationFit::Stretch:
    return target;
  case TextAnimatedDecorationFit::Native:
    scale = 1.0F;
    break;
  case TextAnimatedDecorationFit::FitWidth:
    scale = widthScale;
    break;
  case TextAnimatedDecorationFit::FitHeight:
    scale = heightScale;
    break;
  case TextAnimatedDecorationFit::FitLongSide:
    scale = std::max(widthScale, heightScale);
    break;
  case TextAnimatedDecorationFit::FitShortSide:
    scale = std::min(widthScale, heightScale);
    break;
  }
  if (!std::isfinite(scale) || scale <= 0.0F)
    return target;
  const float width = intrinsicWidth * scale;
  const float height = intrinsicHeight * scale;
  return {RectCenterX(target) - width * 0.5F,
          RectCenterY(target) - height * 0.5F, width, height};
}

float ResolveDecorationAssetProgress(
    const TextAnimationPlaybackMode playback, const float progress) noexcept {
  float result = std::clamp(progress, 0.0F, 1.0F);
  switch (playback) {
  case TextAnimationPlaybackMode::Loop:
    if (result >= 1.0F)
      result = 0.0F;
    break;
  case TextAnimationPlaybackMode::PingPong:
    result = result <= 0.5F ? result * 2.0F : (1.0F - result) * 2.0F;
    break;
  case TextAnimationPlaybackMode::Once:
  case TextAnimationPlaybackMode::Hold:
    break;
  }
  return std::clamp(result, 0.0F, 1.0F);
}

std::int64_t ResolveDecorationAssetTime(
    const std::int64_t durationUs, const TextAnimationPlaybackMode playback,
    const float progress) noexcept {
  if (durationUs <= 0)
    return 0;
  const long double coordinate =
      static_cast<long double>(ResolveDecorationAssetProgress(playback,
                                                               progress)) *
      static_cast<long double>(durationUs);
  const auto sampled = coordinate >= static_cast<long double>(durationUs)
                           ? durationUs - 1
                           : static_cast<std::int64_t>(std::floor(coordinate));
  return std::clamp<std::int64_t>(sampled, 0, durationUs - 1);
}

struct EvaluationDecorationBasis final {
  float xx{1.0F};
  float xy{0.0F};
  float yx{0.0F};
  float yy{1.0F};
};

EvaluationDecorationBasis ResolveEvaluationDecorationBasis(
    const TextDecorationAnimationSample &sample) noexcept {
  constexpr double degreesToRadians =
      3.14159265358979323846264338327950288 / 180.0;
  const double rx = static_cast<double>(sample.rotationXDegrees) *
                    degreesToRadians;
  const double ry = static_cast<double>(sample.rotationYDegrees) *
                    degreesToRadians;
  const double rz =
      static_cast<double>(sample.rotationDegrees) * degreesToRadians;
  const double cosineX = std::cos(rx);
  const double sineX = std::sin(rx);
  const double cosineY = std::cos(ry);
  const double sineY = std::sin(ry);
  const double cosineZ = std::cos(rz);
  const double sineZ = std::sin(rz);
  return {
      static_cast<float>(cosineZ * cosineY * sample.scaleX),
      static_cast<float>((cosineZ * sineY * sineX - sineZ * cosineX) *
                         sample.scaleY),
      static_cast<float>(sineZ * cosineY * sample.scaleX),
      static_cast<float>((sineZ * sineY * sineX + cosineZ * cosineX) *
                         sample.scaleY),
  };
}

bool ResolveEvaluationRenderGroupBounds(
    const TextRenderGroupSpec &spec, const TextEffectFrameInput &frame,
    const std::vector<std::size_t> &targetIndexes,
    TextEffectRect &fixedBounds, TextEffectRect &expandedBounds) {
  fixedBounds = {};
  expandedBounds = {};
  const auto visualOrder = VisualEvaluationOrder(targetIndexes, frame);
  if (visualOrder.empty())
    return true;

  TextRenderGroupUnitTopology topology;
  topology.letterCount = visualOrder.size();
  const auto appendRange = [](std::vector<TextRenderGroupIndexRange> &ranges,
                              const std::size_t begin,
                              const std::size_t end) {
    if (begin < end) {
      ranges.push_back({static_cast<std::int64_t>(begin),
                        static_cast<std::int64_t>(end)});
    }
  };
  std::size_t lineBegin = 0U;
  std::size_t wordBegin = 0U;
  for (std::size_t index = 1U; index <= visualOrder.size(); ++index) {
    const bool atEnd = index == visualOrder.size();
    const auto &previous = frame.units[visualOrder[index - 1U]];
    if (atEnd || frame.units[visualOrder[index]].visualLineIndex !=
                     previous.visualLineIndex) {
      appendRange(topology.lineRanges, lineBegin, index);
      lineBegin = index;
    }
    if (atEnd ||
        frame.units[visualOrder[index]].paragraphId != previous.paragraphId ||
        frame.units[visualOrder[index]].documentWordUtf8Begin !=
            previous.documentWordUtf8Begin ||
        frame.units[visualOrder[index]].documentWordUtf8End !=
            previous.documentWordUtf8End) {
      appendRange(topology.wordRanges, wordBegin, index);
      wordBegin = index;
    }
  }

  const auto ranges = ResolveTextRenderGroupTopology(spec, topology);
  for (const auto &range : ranges) {
    TextEffectRect source{};
    const auto end = std::min(range.endIndex, visualOrder.size());
    for (auto index = range.startIndex; index < end; ++index) {
      const auto unitIndex = visualOrder[index];
      const auto &unit = frame.units[unitIndex];
      const auto &fixedBounds = NonEmptyRect(unit.programRect)
                                    ? unit.programRect
                                : NonEmptyRect(unit.tightRect) ? unit.tightRect
                                                               : unit.rect;
      source = UnionRect(source, fixedBounds);
    }
    if (!NonEmptyRect(source))
      continue;
    fixedBounds = UnionRect(fixedBounds, source);
    const auto resolved = ResolveTextRenderGroupFrame(
        range,
        {source.x, source.y, source.width, source.height},
        spec.expandRatioX, spec.expandRatioY);
    if (!resolved)
      return false;
    const auto &expanded = resolved->expandedRect;
    expandedBounds = UnionRect(
        expandedBounds,
        {static_cast<float>(expanded.originX),
         static_cast<float>(expanded.originY),
         static_cast<float>(expanded.extentWidth),
         static_cast<float>(expanded.extentHeight)});
  }
  return ValidEvaluationRect(fixedBounds) &&
         ValidEvaluationRect(expandedBounds);
}

const TextEffectEvaluationResource *FindEvaluationResource(
    const TextEffectEvaluationRequest &request,
    const TextDecorationAnimationSpec &decoration) noexcept {
  const auto found = std::find_if(
      request.resources.begin(), request.resources.end(),
      [&](const auto &resource) {
        return (!resource.resourceId.empty() &&
                resource.resourceId == decoration.decorationId) ||
               (!resource.assetId.empty() &&
                resource.assetId == decoration.assetId);
      });
  return found == request.resources.end() ? nullptr : &*found;
}

std::optional<TextEffectRect>
TransformEvaluationRect(const TextEffectRect &rect,
                        const TextEffectMatrix4x4 &matrix) noexcept {
  if (!ValidEvaluationRect(rect))
    return std::nullopt;
  TextEffectRect result{};
  bool initialized = false;
  const std::array<std::pair<float, float>, 4> corners{
      std::pair<float, float>{rect.x, rect.y},
      {rect.x + rect.width, rect.y},
      {rect.x + rect.width, rect.y + rect.height},
      {rect.x, rect.y + rect.height}};
  for (const auto &[x, y] : corners) {
    const auto &m = matrix.columnMajor;
    const float homogeneous = m[3] * x + m[7] * y + m[15];
    if (!std::isfinite(homogeneous) || std::fabs(homogeneous) < 1.0e-8F)
      return std::nullopt;
    const float projectedX = (m[0] * x + m[4] * y + m[12]) / homogeneous;
    const float projectedY = (m[1] * x + m[5] * y + m[13]) / homogeneous;
    if (!std::isfinite(projectedX) || !std::isfinite(projectedY))
      return std::nullopt;
    if (!initialized) {
      result = {projectedX, projectedY, 0.0F, 0.0F};
      initialized = true;
      continue;
    }
    const float minimumX = std::min(result.x, projectedX);
    const float minimumY = std::min(result.y, projectedY);
    const float maximumX = std::max(result.x + result.width, projectedX);
    const float maximumY = std::max(result.y + result.height, projectedY);
    result = {minimumX, minimumY, maximumX - minimumX,
              maximumY - minimumY};
  }
  return initialized ? std::optional<TextEffectRect>{result} : std::nullopt;
}

bool ResolveEvaluationBounds(const TextEffectEvaluationRequest &request,
                             TextEffectFramePlan &plan,
                             std::string &error) noexcept {
  auto bounds = request.bounds;
  const std::array<TextEffectRect, 5> authoredBounds{
      bounds.layoutBounds, bounds.controlBounds, bounds.inkBounds,
      bounds.visualBounds, bounds.expandedRenderTargetBounds};
  if (!std::all_of(authoredBounds.begin(), authoredBounds.end(),
                   ValidEvaluationRect)) {
    error = "text effect authored bounds are invalid";
    return false;
  }
  if (!NonEmptyRect(bounds.layoutBounds))
    bounds.layoutBounds = request.frame.textRect;
  if (!NonEmptyRect(bounds.controlBounds))
    bounds.controlBounds = bounds.layoutBounds;
  if (!NonEmptyRect(bounds.inkBounds))
    bounds.inkBounds = request.frame.tightTextRect;
  TextEffectRect visual = NonEmptyRect(bounds.visualBounds)
                              ? bounds.visualBounds
                              : bounds.inkBounds;
  visual = UnionRect(visual, plan.bounds.visualBounds);
  for (std::size_t unitIndex = 0U; unitIndex < plan.units.size(); ++unitIndex) {
    const auto &unitPlan = plan.units[unitIndex];
    // Absolute zero is the authored hidden-unit state used by staggered Qt
    // programs. It remains a real shaping mutation, but contributes no glyph
    // geometry to visual or render-target bounds.
    if (unitPlan.absoluteFontSize && *unitPlan.absoluteFontSize == 0.0F)
      continue;
    TextEffectRect unitBounds = request.frame.units[unitIndex].tightRect;
    if (!unitPlan.transforms.empty()) {
      const auto composed = ComposeTextEffectTransforms(unitPlan.transforms);
      if (!composed) {
        error = "text effect frame transform composition is invalid";
        return false;
      }
      const auto transformed = TransformEvaluationRect(unitBounds, *composed);
      if (!transformed) {
        error = "text effect frame transform bounds are invalid";
        return false;
      }
      unitBounds = *transformed;
    }
    if (unitPlan.sdfBlurRadius) {
      unitBounds = ExpandRect(unitBounds, *unitPlan.sdfBlurRadius,
                              *unitPlan.sdfBlurRadius,
                              *unitPlan.sdfBlurRadius,
                              *unitPlan.sdfBlurRadius);
    }
    visual = UnionRect(visual, unitBounds);
  }
  for (const auto &decoration : plan.decorationPasses) {
    auto source = ExpandRect(decoration.bounds,
                             decoration.sourceOutsets.left,
                             decoration.sourceOutsets.top,
                             decoration.sourceOutsets.right,
                             decoration.sourceOutsets.bottom);
    const auto transformed =
        TransformEvaluationRect(source, decoration.transform.localToText);
    if (!transformed) {
      error = "text effect decoration bounds are invalid";
      return false;
    }
    visual = UnionRect(visual, *transformed);
  }
  float postPadding = 0.0F;
  for (const auto &node : plan.postEffectNodes)
    postPadding += std::max(0.0F, node.paddingPx);
  if (postPadding > 0.0F)
    visual = ExpandRect(visual, postPadding, postPadding, postPadding,
                        postPadding);
  if (!NonEmptyRect(visual))
    visual = bounds.layoutBounds;
  bounds.visualBounds = visual;
  auto expanded = UnionRect(bounds.expandedRenderTargetBounds,
                            plan.bounds.expandedRenderTargetBounds);
  expanded = UnionRect(expanded, visual);
  if (NonEmptyRect(expanded)) {
    const float left = std::floor(expanded.x);
    const float top = std::floor(expanded.y);
    const float right = std::ceil(expanded.x + expanded.width) + 1.0F;
    const float bottom = std::ceil(expanded.y + expanded.height) + 1.0F;
    expanded = {left, top, right - left, bottom - top};
  }
  if (!ValidEvaluationRect(expanded)) {
    error = "text effect expanded render-target bounds are invalid";
    return false;
  }
  bounds.expandedRenderTargetBounds = expanded;
  plan.bounds = bounds;
  return true;
}

bool ValidatePostEffectDag(const TextEffectFramePlan &plan,
                           std::string &error) {
  std::unordered_map<std::string, std::size_t> indexes;
  indexes.reserve(plan.postEffectNodes.size());
  for (std::size_t index = 0U; index < plan.postEffectNodes.size(); ++index) {
    if (!indexes.emplace(plan.postEffectNodes[index].nodeId, index).second) {
      error = "text effect post DAG contains a duplicate node identity";
      return false;
    }
  }
  std::vector<std::uint8_t> colors(plan.postEffectNodes.size(), 0U);
  std::function<bool(std::size_t)> visit = [&](const std::size_t index) {
    if (colors[index] == 1U)
      return false;
    if (colors[index] == 2U)
      return true;
    colors[index] = 1U;
    for (const auto &input : plan.postEffectNodes[index].inputIds) {
      const auto found = indexes.find(input);
      if (found == indexes.end()) {
        error = "text effect post DAG contains a dangling input identity";
        return false;
      }
      if (!visit(found->second))
        return false;
    }
    colors[index] = 2U;
    return true;
  };
  for (std::size_t index = 0U; index < colors.size(); ++index) {
    if (!visit(index)) {
      if (error.empty())
        error = "text effect post DAG contains a cycle";
      return false;
    }
  }
  return true;
}

struct PhysicsBodyState final {
  std::size_t unitIndex{0U};
  double initialX{0.0};
  double initialY{0.0};
  double x{0.0};
  double y{0.0};
  double previousX{0.0};
  double previousY{0.0};
  double accelerationX{0.0};
  double accelerationY{0.0};
  double radius{0.0};
  double width{0.0};
  double height{0.0};
  double rotation{0.0};
  double previousRotation{0.0};
  double angularVelocity{0.0};
};

struct PhysicsBoundary final {
  double left{0.0};
  double right{0.0};
  double top{0.0};
  double bottom{0.0};
};

struct PhysicsCell final {
  std::int64_t x{0};
  std::int64_t y{0};

  bool operator==(const PhysicsCell &other) const noexcept {
    return x == other.x && y == other.y;
  }
};

struct PhysicsCellHash final {
  std::size_t operator()(const PhysicsCell &cell) const noexcept {
    const auto x = static_cast<std::uint64_t>(cell.x);
    const auto y = static_cast<std::uint64_t>(cell.y);
    const auto mixed = x ^ (y + 0x9e3779b97f4a7c15ULL + (x << 6U) +
                            (x >> 2U));
    return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
  }
};

double PhysicsSeededRandom(const TextEffectPhysicsSpec &spec,
                           const std::uint64_t seed) noexcept {
  const double x = std::sin(static_cast<double>(spec.randomSeed + seed) +
                            static_cast<double>(spec.randomPhase)) *
                   static_cast<double>(spec.randomScale);
  return x - std::floor(x);
}

bool PhysicsCellCoordinate(const double value, const double cellSize,
                           std::int64_t &coordinate) noexcept {
  const double cell = std::floor(value / cellSize);
  if (!std::isfinite(cell) ||
      cell < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      cell > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  coordinate = static_cast<std::int64_t>(cell);
  return true;
}

bool EvaluatePhysicsStatePair(
    const TextEffectPhysicsSpec &physics,
    const TextEffectCollisionSpec &collision, const std::int64_t effectTimeUs,
    const TextEffectEvaluationRequest &request, TextEffectFramePlan &plan,
    std::string &error) {
  std::vector<PhysicsBodyState> bodies;
  bodies.reserve(request.frame.units.size());
  double centerX = 0.0;
  double centerY = 0.0;
  double boundsLeft = std::numeric_limits<double>::infinity();
  double boundsRight = -std::numeric_limits<double>::infinity();
  double boundsTop = std::numeric_limits<double>::infinity();
  double boundsBottom = -std::numeric_limits<double>::infinity();

  for (std::size_t unitIndex = 0U;
       unitIndex < request.frame.units.size(); ++unitIndex) {
    const auto &unit = request.frame.units[unitIndex];
    if (unit.type != 0)
      continue;
    const auto initial = ReferencePointToQtTextProgramSource(
        unit.initialPositionX, unit.initialPositionY, request.referenceCanvas,
        request.sourceToReferenceScale);
    const auto authoredRect = NonEmptyRect(unit.programRect)
                                  ? unit.programRect
                                  : unit.rect;
    const auto sourceRect = ReferenceRectToQtTextProgramSource(
        authoredRect, request.referenceCanvas,
        request.sourceToReferenceScale);
    const double fallbackSize =
        static_cast<double>(unit.fontSize) /
        static_cast<double>(request.sourceToReferenceScale);
    const double width = sourceRect.width > 0.0F
                             ? static_cast<double>(sourceRect.width)
                             : fallbackSize;
    const double height = sourceRect.height > 0.0F
                              ? static_cast<double>(sourceRect.height)
                              : fallbackSize;
    if (!std::isfinite(initial.first) || !std::isfinite(initial.second) ||
        !std::isfinite(width) || !std::isfinite(height) || width <= 0.0 ||
        height <= 0.0) {
      SetEvaluationError(error,
                         "text effect physics glyph geometry is invalid");
      return false;
    }
    PhysicsBodyState body;
    body.unitIndex = unitIndex;
    body.initialX = initial.first;
    body.initialY = initial.second;
    body.x = body.previousX = body.initialX;
    body.y = body.previousY = body.initialY;
    body.width = width;
    body.height = height;
    body.radius = std::min(width, height) * 0.5;
    bodies.push_back(body);
    centerX += body.initialX;
    centerY += body.initialY;
    boundsLeft = std::min(boundsLeft, body.initialX - width * 0.5);
    boundsRight = std::max(boundsRight, body.initialX + width * 0.5);
    boundsTop = std::min(boundsTop, body.initialY - height * 0.5);
    boundsBottom = std::max(boundsBottom, body.initialY + height * 0.5);
  }
  if (bodies.empty())
    return true;

  centerX /= static_cast<double>(bodies.size());
  centerY /= static_cast<double>(bodies.size());
  PhysicsBoundary box{
      centerX * static_cast<double>(collision.boxCenterXScale) -
          static_cast<double>(collision.fixedBoxWidth) * 0.5,
      centerX * static_cast<double>(collision.boxCenterXScale) +
          static_cast<double>(collision.fixedBoxWidth) * 0.5,
      centerY - static_cast<double>(collision.fixedBoxHeight) * 0.5,
      centerY + static_cast<double>(collision.fixedBoxHeight) * 0.5};

  double minimumLeft = std::numeric_limits<double>::infinity();
  double maximumRight = -std::numeric_limits<double>::infinity();
  double minimumTop = std::numeric_limits<double>::infinity();
  double averageWidth = 0.0;
  double averageHeight = 0.0;
  for (const auto &body : bodies) {
    minimumLeft = std::min(minimumLeft, body.x - body.radius);
    maximumRight = std::max(maximumRight, body.x + body.radius);
    minimumTop = std::min(minimumTop, body.y - body.radius);
    averageWidth += body.width;
    averageHeight += body.height;
  }
  averageWidth /= static_cast<double>(bodies.size());
  averageHeight /= static_cast<double>(bodies.size());
  if (collision.expandHorizontalToContent &&
      maximumRight - minimumLeft > box.right - box.left) {
    box.left = minimumLeft - averageWidth;
    box.right = maximumRight + averageWidth;
  }
  if (collision.expandTopToContent && minimumTop < box.top)
    box.top = minimumTop - averageHeight;

  const double stepSeconds =
      1.0 / static_cast<double>(physics.fixedStepHz);
  const double effectSeconds =
      std::max(0.0, static_cast<double>(effectTimeUs) / 1'000'000.0);
  if (!std::isfinite(effectSeconds) || effectSeconds > 60.0) {
    SetEvaluationError(error,
                       "text effect physics replay duration is invalid");
    return false;
  }
  const double explosionDelay =
      static_cast<double>(physics.explosionDelayUs) / 1'000'000.0;
  const double explosionDuration =
      static_cast<double>(physics.explosionForceDurationUs) / 1'000'000.0;
  const double rampDuration =
      static_cast<double>(physics.explosionRampUpUs) / 1'000'000.0;
  constexpr double kPi = 3.14159265358979323846264338327950288;

  std::vector<double> angleCosines(bodies.size(), 1.0);
  std::vector<double> angleSines(bodies.size(), 0.0);
  std::vector<double> speedRandoms(bodies.size(), 0.0);
  std::vector<double> rotationDirections(bodies.size(), -1.0);
  for (std::size_t index = 0U; index < bodies.size(); ++index) {
    const double angleOffset =
        (PhysicsSeededRandom(
             physics,
             static_cast<std::uint64_t>(index) * physics.angleSeedStride) -
         0.5) *
        static_cast<double>(physics.explosionAngleRangeDegrees) * kPi /
        180.0;
    angleCosines[index] = std::cos(angleOffset);
    angleSines[index] = std::sin(angleOffset);
    speedRandoms[index] = PhysicsSeededRandom(
        physics,
        static_cast<std::uint64_t>(index) * physics.speedSeedStride);
    rotationDirections[index] =
        PhysicsSeededRandom(
            physics,
            static_cast<std::uint64_t>(index) * physics.rotationSeedStride) >
                0.5
            ? 1.0
            : -1.0;
  }

  const auto solveBodyCollisions = [&]() {
    if (bodies.size() <= 1U)
      return true;
    double sizeSum = 0.0;
    for (const auto &body : bodies)
      sizeSum += std::min(body.width, body.height);
    const double cellSize = std::max(
        static_cast<double>(collision.minimumCellSize),
        sizeSum / static_cast<double>(bodies.size()) *
            static_cast<double>(collision.cellSizeScale));
    if (!std::isfinite(cellSize) || cellSize <= 0.0)
      return false;
    std::unordered_map<PhysicsCell, std::vector<std::size_t>,
                       PhysicsCellHash>
        grid;
    grid.reserve(bodies.size());
    for (std::size_t index = 0U; index < bodies.size(); ++index) {
      PhysicsCell cell;
      if (!PhysicsCellCoordinate(bodies[index].x, cellSize, cell.x) ||
          !PhysicsCellCoordinate(bodies[index].y, cellSize, cell.y)) {
        return false;
      }
      grid[cell].push_back(index);
    }
    std::unordered_set<std::uint64_t> processed;
    processed.reserve(bodies.size() * 4U);
    for (std::size_t firstIndex = 0U; firstIndex < bodies.size();
         ++firstIndex) {
      auto &first = bodies[firstIndex];
      PhysicsCell centerCell;
      if (!PhysicsCellCoordinate(first.x, cellSize, centerCell.x) ||
          !PhysicsCellCoordinate(first.y, cellSize, centerCell.y)) {
        return false;
      }
      for (std::int64_t offsetX = -1; offsetX <= 1; ++offsetX) {
        for (std::int64_t offsetY = -1; offsetY <= 1; ++offsetY) {
          const PhysicsCell cell{centerCell.x + offsetX,
                                 centerCell.y + offsetY};
          const auto found = grid.find(cell);
          if (found == grid.end())
            continue;
          for (const auto secondIndex : found->second) {
            if (secondIndex <= firstIndex)
              continue;
            const auto pairIdentity =
                static_cast<std::uint64_t>(firstIndex) *
                    static_cast<std::uint64_t>(bodies.size()) +
                static_cast<std::uint64_t>(secondIndex);
            if (!processed.emplace(pairIdentity).second)
              continue;
            auto &second = bodies[secondIndex];
            const double deltaX = first.x - second.x;
            const double deltaY = first.y - second.y;
            const double squaredDistance =
                deltaX * deltaX + deltaY * deltaY;
            const double minimumDistance =
                (std::min(first.width, first.height) +
                 std::min(second.width, second.height)) *
                0.5 * static_cast<double>(collision.collisionBoundaryScale);
            if (squaredDistance >= minimumDistance * minimumDistance)
              continue;
            const double distance =
                std::sqrt(squaredDistance) > 0.0
                    ? std::sqrt(squaredDistance)
                    : 1.0e-6;
            const double normalX = deltaX / distance;
            const double normalY = deltaY / distance;
            const double overlap = minimumDistance - distance;
            first.x += normalX * overlap * 0.5;
            first.y += normalY * overlap * 0.5;
            second.x -= normalX * overlap * 0.5;
            second.y -= normalY * overlap * 0.5;
            const double firstVelocityX = first.x - first.previousX;
            const double firstVelocityY = first.y - first.previousY;
            const double secondVelocityX = second.x - second.previousX;
            const double secondVelocityY = second.y - second.previousY;
            const double relativeVelocityX =
                firstVelocityX - secondVelocityX;
            const double relativeVelocityY =
                firstVelocityY - secondVelocityY;
            const double tangentX = -normalY;
            const double tangentY = normalX;
            const double tangentialForce =
                relativeVelocityX * tangentX +
                relativeVelocityY * tangentY;
            const double torque =
                tangentialForce * overlap *
                static_cast<double>(collision.collisionTorqueScale);
            first.angularVelocity += torque;
            second.angularVelocity -= torque;
            const double normalVelocity =
                relativeVelocityX * normalX +
                relativeVelocityY * normalY;
            if (normalVelocity < 0.0) {
              const double impulse =
                  -(1.0 +
                    static_cast<double>(collision.collisionRestitution)) *
                  normalVelocity * 0.5;
              const double impulseX = normalX * impulse;
              const double impulseY = normalY * impulse;
              first.previousX = first.x - firstVelocityX - impulseX;
              first.previousY = first.y - firstVelocityY - impulseY;
              second.previousX = second.x - secondVelocityX + impulseX;
              second.previousY = second.y - secondVelocityY + impulseY;
            }
          }
        }
      }
    }
    return true;
  };

  const auto applyBoundaries = [&]() {
    const double sideVelocityScale =
        static_cast<double>(collision.wallDamping) *
        static_cast<double>(collision.sideWallVelocityScale);
    const double topVelocityScale =
        static_cast<double>(collision.wallDamping) *
        static_cast<double>(collision.topWallVelocityScale);
    for (auto &body : bodies) {
      const double velocityX = body.x - body.previousX;
      const double velocityY = body.y - body.previousY;
      const double speed =
          std::sqrt(velocityX * velocityX + velocityY * velocityY);
      if (collision.collideLeft && body.x - body.radius < box.left) {
        body.x = box.left + body.radius;
        body.previousX = body.x + velocityX * sideVelocityScale;
        const double impactPoint =
            (body.y - box.top) / (box.bottom - box.top);
        body.angularVelocity +=
            velocityY * speed *
            static_cast<double>(collision.wallTorqueScale) *
            (impactPoint - 0.5);
      }
      if (collision.collideRight && body.x + body.radius > box.right) {
        body.x = box.right - body.radius;
        body.previousX = body.x + velocityX * sideVelocityScale;
        const double impactPoint =
            (body.y - box.top) / (box.bottom - box.top);
        body.angularVelocity -=
            velocityY * speed *
            static_cast<double>(collision.wallTorqueScale) *
            (impactPoint - 0.5);
      }
      if (collision.collideTop && body.y - body.radius < box.top) {
        body.y = box.top + body.radius;
        body.previousY = body.y + velocityY * topVelocityScale;
        const double impactPoint =
            (body.x - box.left) / (box.right - box.left);
        body.angularVelocity -=
            velocityX * speed *
            static_cast<double>(collision.wallTorqueScale) *
            (impactPoint - 0.5);
      }
      if (collision.collideBottom && body.y + body.radius > box.bottom) {
        body.y = box.bottom - body.radius;
        body.previousY =
            body.y + velocityY *
                         static_cast<double>(collision.bottomWallDamping);
        const double impactPoint =
            (body.x - box.left) / (box.right - box.left);
        body.angularVelocity +=
            velocityX * speed *
            static_cast<double>(collision.wallTorqueScale) *
            (impactPoint - 0.5);
      }
    }
  };

  const auto updateBodies = [&]() {
    const double squaredStep = stepSeconds * stepSeconds;
    for (auto &body : bodies) {
      const double velocityX = body.x - body.previousX;
      const double velocityY = body.y - body.previousY;
      const double positionX = body.x;
      const double positionY = body.y;
      body.previousX = positionX;
      body.previousY = positionY;
      body.x = positionX + velocityX + body.accelerationX * squaredStep;
      body.y = positionY + velocityY + body.accelerationY * squaredStep;
      body.previousRotation = body.rotation;
      body.rotation += body.angularVelocity * stepSeconds;
      body.angularVelocity *= static_cast<double>(physics.angularDamping);
      body.accelerationX = 0.0;
      body.accelerationY = 0.0;
    }
  };

  const auto updateEngine = [&]() {
    for (auto &body : bodies) {
      body.accelerationX += static_cast<double>(physics.gravityX);
      body.accelerationY += static_cast<double>(physics.gravityY);
    }
    if (!solveBodyCollisions())
      return false;
    applyBoundaries();
    updateBodies();
    return true;
  };

  double accumulatedSeconds = 0.0;
  while (accumulatedSeconds < std::min(effectSeconds, explosionDelay))
    accumulatedSeconds += stepSeconds;
  if (effectSeconds > explosionDelay) {
    const double originX = (boundsLeft + boundsRight) * 0.5;
    const double originY =
        (boundsTop + boundsBottom) * 0.5 +
        (boundsBottom - boundsTop) *
            static_cast<double>(physics.explosionOriginYShiftFactor);
    std::vector<double> accelerationMagnitudes(bodies.size(), 0.0);
    for (std::size_t index = 0U; index < bodies.size(); ++index) {
      accelerationMagnitudes[index] =
          static_cast<double>(physics.explosionAccelerationMin) +
          speedRandoms[index] *
              (static_cast<double>(physics.explosionAccelerationMax) -
               static_cast<double>(physics.explosionAccelerationMin));
      bodies[index].angularVelocity +=
          rotationDirections[index] * accelerationMagnitudes[index] *
          static_cast<double>(physics.initialAngularVelocityScale);
    }
    while (accumulatedSeconds < effectSeconds) {
      const double elapsed = accumulatedSeconds - explosionDelay;
      if (accumulatedSeconds >= explosionDelay &&
          elapsed <= explosionDuration) {
        const double ramp = elapsed < rampDuration
                                ? elapsed / rampDuration
                                : 1.0;
        for (std::size_t index = 0U; index < bodies.size(); ++index) {
          auto &body = bodies[index];
          double directionX = body.x - originX;
          double directionY = body.y - originY;
          const double length =
              std::sqrt(directionX * directionX +
                        directionY * directionY);
          const double divisor = length > 0.0 ? length : 1.0e-6;
          directionX /= divisor;
          directionY /= divisor;
          const double rotatedX =
              directionX * angleCosines[index] -
              directionY * angleSines[index];
          const double rotatedY =
              directionX * angleSines[index] +
              directionY * angleCosines[index];
          body.accelerationX +=
              rotatedX * accelerationMagnitudes[index] * ramp;
          body.accelerationY +=
              rotatedY * accelerationMagnitudes[index] * ramp;
        }
      }
      if (!updateEngine()) {
        SetEvaluationError(error,
                           "text effect physics collision grid is invalid");
        return false;
      }
      accumulatedSeconds += stepSeconds;
    }
  }

  for (const auto &body : bodies) {
    const std::array<double, 3U> values{body.x, body.y, body.rotation};
    if (!std::all_of(values.begin(), values.end(), [](const double value) {
          return std::isfinite(value) &&
                 std::fabs(value) <=
                     static_cast<double>(std::numeric_limits<float>::max());
        })) {
      SetEvaluationError(error,
                         "text effect physics result is not finite");
      return false;
    }
    const auto &unit = request.frame.units[body.unitIndex];
    TextEffectTransformComponents components;
    components.offsetX = static_cast<float>(body.x - body.initialX);
    components.offsetY = static_cast<float>(body.y - body.initialY);
    components.rotationZ = static_cast<float>(body.rotation);
    const auto transform = ResolveQtTextProgramTransformPlanForReference(
        components, unit.tightRect, unit.rect, request.frame.textRect,
        request.sourceToReferenceScale, request.referenceCanvas, false);
    if (!transform) {
      SetEvaluationError(error,
                         "text effect physics transform is invalid");
      return false;
    }
    auto &destination = plan.units[body.unitIndex];
    destination.transforms.insert(destination.transforms.begin(), *transform);
  }
  return true;
}

bool EvaluatePhysicsExecutionGraph(
    const TextEffectEvaluationRequest &request, TextEffectFramePlan &plan,
    std::string &error) {
  std::vector<TextEffectExecutionNodeFramePlan *> orderedNodes;
  std::function<void(std::vector<TextEffectExecutionNodeFramePlan> &)>
      appendNodes;
  appendNodes = [&](std::vector<TextEffectExecutionNodeFramePlan> &nodes) {
    for (auto &node : nodes) {
      orderedNodes.push_back(&node);
      appendNodes(node.children);
    }
  };
  appendNodes(plan.executionGraph.nodes);

  std::unordered_set<const TextEffectExecutionNodeFramePlan *> usedCollisions;
  for (auto *physicsNode : orderedNodes) {
    if (physicsNode->capability !=
            TextEffectExecutionCapability::StatePhysics ||
        !physicsNode->physicsSpec) {
      continue;
    }
    TextEffectExecutionNodeFramePlan *collisionNode = nullptr;
    for (auto *candidate : orderedNodes) {
      if (candidate->capability !=
              TextEffectExecutionCapability::StateCollision ||
          !candidate->collisionSpec ||
          std::find(candidate->inputIds.begin(), candidate->inputIds.end(),
                    physicsNode->nodeId) == candidate->inputIds.end()) {
        continue;
      }
      if (collisionNode) {
        SetEvaluationError(
            error,
            "text effect physics state has multiple collision solvers");
        return false;
      }
      collisionNode = candidate;
    }
    if (!collisionNode ||
        !usedCollisions.emplace(collisionNode).second) {
      SetEvaluationError(error,
                         "text effect physics state is not closed");
      return false;
    }
    if (!physicsNode->active || !collisionNode->active)
      continue;
    if (!EvaluatePhysicsStatePair(
            *physicsNode->physicsSpec, *collisionNode->collisionSpec,
            physicsNode->effectTimeUs, request, plan, error)) {
      return false;
    }
  }
  for (const auto *node : orderedNodes) {
    if (node->capability == TextEffectExecutionCapability::StateCollision &&
        node->collisionSpec && usedCollisions.count(node) == 0U) {
      SetEvaluationError(error,
                         "text effect collision state is not closed");
      return false;
    }
  }
  return true;
}

bool BindExecutionParametersToGraph(TextEffectFramePlan &plan,
                                    std::string &error) {
  std::unordered_map<std::string, TextEffectExecutionNodeFramePlan *> nodes;
  std::function<bool(std::vector<TextEffectExecutionNodeFramePlan> &)>
      indexNodes;
  indexNodes = [&](std::vector<TextEffectExecutionNodeFramePlan> &entries) {
    for (auto &node : entries) {
      if (node.nodeId.empty() ||
          !nodes.emplace(node.nodeId, &node).second) {
        error =
            "text effect execution graph has an invalid node identity";
        return false;
      }
      if (!indexNodes(node.children))
        return false;
    }
    return true;
  };
  if (!indexNodes(plan.executionGraph.nodes))
    return false;

  std::unordered_set<std::uint64_t> unitIds;
  unitIds.reserve(plan.units.size());
  for (const auto &unit : plan.units)
    unitIds.emplace(unit.stableUnitId);

  std::unordered_set<std::string> boundKeys;
  boundKeys.reserve(plan.executionParameters.size());
  for (const auto &parameter : plan.executionParameters) {
    const auto found = nodes.find(parameter.nodeId);
    if (found == nodes.end()) {
      error = "text effect execution parameter target is missing";
      return false;
    }
    auto &node = *found->second;
    const bool targetsPostEffect =
        IsPostEffectExecutionParameter(parameter.parameter);
    const bool targetsMaterial =
        IsMaterialExecutionParameter(parameter.parameter);
    const bool targetsScene =
        IsSceneExecutionParameter(parameter.parameter);
    const bool targetsNode =
        IsNodeExecutionParameter(parameter.parameter);
    const bool materialNode =
        node.kind == TextEffectExecutionNodeKind::MaterialPass &&
        (node.capability ==
             TextEffectExecutionCapability::MaterialColorPass ||
         node.capability ==
             TextEffectExecutionCapability::MaterialDepthPass ||
         node.capability ==
             TextEffectExecutionCapability::MaterialProgramPass ||
         (node.capability >=
              TextEffectExecutionCapability::MaterialAlphaModulate &&
          node.capability <= TextEffectExecutionCapability::
                                 MaterialNoiseThresholdDissolve));
    const bool sceneNode =
        node.kind == TextEffectExecutionNodeKind::Scene &&
        (node.capability == TextEffectExecutionCapability::SceneClone ||
         (node.capability == TextEffectExecutionCapability::SceneEntity &&
          parameter.domain == TextEffectExecutionParameterDomain::Page));
    const bool parameterizedNode =
        node.kind == TextEffectExecutionNodeKind::Operator ||
        node.kind == TextEffectExecutionNodeKind::MediaInput ||
        node.kind == TextEffectExecutionNodeKind::History;
    if ((targetsPostEffect &&
         (node.kind != TextEffectExecutionNodeKind::PostEffectPass ||
          !node.postEffectKind)) ||
        (targetsMaterial && !materialNode) ||
        (targetsScene && !sceneNode) ||
        (targetsNode && !parameterizedNode) ||
        (!targetsPostEffect && !targetsMaterial && !targetsScene &&
         !targetsNode) ||
        parameter.values.size() !=
            TextEffectExecutionParameterComponentCount(parameter.parameter) ||
        TextEffectExecutionParameterDomainName(parameter.domain).empty() ||
        TextEffectExecutionParameterSpaceName(parameter.valueSpace).empty() ||
        parameter.valueSpace ==
            TextEffectExecutionParameterSpace::SourcePixels ||
        (targetsScene && parameter.slot != 0U) ||
        (parameter.domain == TextEffectExecutionParameterDomain::Page &&
         parameter.stableUnitId) ||
        (parameter.domain == TextEffectExecutionParameterDomain::PerUnit &&
         (!parameter.stableUnitId ||
          unitIds.count(*parameter.stableUnitId) == 0U)) ||
        !std::all_of(parameter.values.begin(), parameter.values.end(),
                     [](const float value) { return std::isfinite(value); })) {
      error = "text effect execution parameter sample is invalid";
      return false;
    }
    if (parameter.parameter ==
            TextEffectExecutionParameterKind::PostEffectProgress &&
        (parameter.values[0] < 0.0F || parameter.values[0] > 1.0F)) {
      error = "text effect execution post progress is outside [0, 1]";
      return false;
    }
    if (parameter.parameter ==
            TextEffectExecutionParameterKind::PostEffectBlurRadius &&
        parameter.values[0] < 0.0F) {
      error = "text effect execution post blur radius is negative";
      return false;
    }
    if (parameter.parameter ==
            TextEffectExecutionParameterKind::SceneOpacity &&
        (parameter.values[0] < 0.0F || parameter.values[0] > 1.0F)) {
      error = "text effect execution scene opacity is outside [0, 1]";
      return false;
    }

    std::string key;
    try {
      key = std::to_string(parameter.nodeId.size()) + ":" +
            parameter.nodeId + ":" +
            std::to_string(static_cast<unsigned>(parameter.parameter)) +
            ":" + std::to_string(parameter.slot) + ":";
      if (parameter.stableUnitId)
        key += std::to_string(*parameter.stableUnitId);
      else
        key += "page";
    } catch (...) {
      error = "text effect execution parameter key allocation failed";
      return false;
    }
    if (!boundKeys.emplace(std::move(key)).second) {
      error = "text effect execution parameter is bound more than once";
      return false;
    }
    node.parameters.push_back(parameter);
    if (parameter.parameter ==
        TextEffectExecutionParameterKind::PostEffectProgress) {
      node.progress = parameter.values[0];
    }
  }
  for (const auto &entry : nodes) {
    const auto &nodeId = entry.first;
    const auto *node = entry.second;
    const auto hasParameter = [&](const TextEffectExecutionParameterKind kind,
                                  const std::uint32_t slot,
                                  const TextEffectExecutionParameterSpace
                                      space) {
      return std::count_if(
                 node->parameters.begin(), node->parameters.end(),
                 [&](const auto &parameter) {
                   return parameter.parameter == kind &&
                          parameter.slot == slot &&
                          parameter.valueSpace == space;
                 }) == 1;
    };
    switch (node->capability) {
    case TextEffectExecutionCapability::MaterialRadialDecayHsvGlow:
      if (node->parameters.size() != 2U ||
          !hasParameter(TextEffectExecutionParameterKind::MaterialScalar, 0U,
                        TextEffectExecutionParameterSpace::Unitless) ||
          !hasParameter(TextEffectExecutionParameterKind::MaterialVector2, 1U,
                        TextEffectExecutionParameterSpace::Unitless)) {
        error = "text radial-decay HSV glow parameters are incomplete: " +
                nodeId;
        return false;
      }
      break;
    case TextEffectExecutionCapability::MaterialFixedNineTapAxisBlur:
      if (node->parameters.size() != 2U ||
          !hasParameter(TextEffectExecutionParameterKind::MaterialScalar, 0U,
                        TextEffectExecutionParameterSpace::ReferencePixels) ||
          !hasParameter(TextEffectExecutionParameterKind::MaterialVector2, 1U,
                        TextEffectExecutionParameterSpace::Unitless)) {
        error = "text fixed nine-tap blur parameters are incomplete: " +
                nodeId;
        return false;
      }
      break;
    case TextEffectExecutionCapability::MaterialHsvOriginalOverBlur:
      if (!node->parameters.empty() || node->inputIds.size() != 2U) {
        error = "text HSV original-over-blur contract is incomplete: " +
                nodeId;
        return false;
      }
      break;
    case TextEffectExecutionCapability::MaterialNoiseThresholdDissolve:
      if (node->parameters.size() != 3U ||
          !hasParameter(TextEffectExecutionParameterKind::MaterialScalar, 0U,
                        TextEffectExecutionParameterSpace::Unitless) ||
          !hasParameter(TextEffectExecutionParameterKind::MaterialScalar, 1U,
                        TextEffectExecutionParameterSpace::Unitless) ||
          !hasParameter(TextEffectExecutionParameterKind::MaterialVector2, 0U,
                        TextEffectExecutionParameterSpace::Unitless) ||
          node->inputIds.size() != 2U) {
        error = "text noise-threshold dissolve contract is incomplete: " +
                nodeId;
        return false;
      }
      break;
    default:
      break;
    }
  }
  return true;
}

} // namespace

TextEffectEvaluationResult
EvaluateTextEffects(const TextEffectEvaluationRequest &request) noexcept {
  TextEffectEvaluationResult result;
  try {
    std::string error;
    if (!std::isfinite(request.sourceToReferenceScale) ||
        request.sourceToReferenceScale <= 0.0F ||
        !std::isfinite(request.referenceCanvas.width) ||
        !std::isfinite(request.referenceCanvas.height) ||
        request.referenceCanvas.width <= 0.0F ||
        request.referenceCanvas.height <= 0.0F ||
        !ValidEvaluationFrame(request.frame, error)) {
      AddEvaluationDiagnostic(result, "text_effect.request_invalid", {},
                              error.empty()
                                  ? "text effect evaluation request is invalid"
                                  : std::move(error));
      return result;
    }
    const std::int64_t clipDurationUs =
        std::max<std::int64_t>(0, request.clipDurationUs);
    const std::int64_t clipLocalTimeUs =
        clipDurationUs > 0
            ? std::clamp(request.clipLocalTimeUs, std::int64_t{0},
                         clipDurationUs)
            : 0;
    std::unordered_set<std::string> resourceIds;
    std::unordered_map<std::string, const TextEffectEvaluationResource *>
        resourcesById;
    for (const auto &resource : request.resources) {
      if ((resource.resourceId.empty() && resource.assetId.empty()) ||
          (!resource.resourceId.empty() &&
           (!IsValidUtf8(resource.resourceId) ||
            !resourceIds.emplace(resource.resourceId).second)) ||
          (!resource.assetId.empty() && !IsValidUtf8(resource.assetId)) ||
          (!resource.digest.empty() && !IsValidUtf8(resource.digest)) ||
          !std::isfinite(resource.intrinsicWidth) ||
          !std::isfinite(resource.intrinsicHeight) ||
          resource.intrinsicWidth < 0.0F ||
          resource.intrinsicHeight < 0.0F || resource.durationUs < 0) {
        AddEvaluationDiagnostic(
            result, "text_effect.resource_closure_invalid",
            resource.resourceId,
            "text effect evaluation resource closure is invalid");
        return result;
      }
      if (!resource.resourceId.empty())
        resourcesById.emplace(resource.resourceId, &resource);
    }
    std::unordered_map<std::string, const TextEffectEvaluationStateInput *>
        statesById;
    for (const auto &state : request.stateInputs) {
      if (state.stateId.empty() || state.stateId.size() > 512U ||
          !IsValidUtf8(state.stateId) || state.revision == 0U ||
          state.elapsedUs < 0 ||
          !statesById.emplace(state.stateId, &state).second) {
        AddEvaluationDiagnostic(result, "text_effect.state_closure_invalid",
                                state.stateId,
                                "text effect state closure is invalid");
        return result;
      }
    }
    std::unordered_map<std::string, const TextEffectEvaluationHistoryInput *>
        historiesById;
    for (const auto &history : request.historyInputs) {
      if (history.historyId.empty() || history.historyId.size() > 512U ||
          !IsValidUtf8(history.historyId) ||
          !historiesById.emplace(history.historyId, &history).second) {
        AddEvaluationDiagnostic(result,
                                "text_effect.history_closure_invalid",
                                history.historyId,
                                "text effect history closure is invalid");
        return result;
      }
    }

    result.framePlan.programId = "authored.text.effects";
    result.framePlan.sourceCreationComponent = request.sourceCreationComponent;
    result.framePlan.cacheIdentities = request.cacheIdentities;
    // ValidEvaluationFrame establishes unique identities and dense indexes.
    // Keep this source order throughout evaluation: animator/program samples
    // only update source units, so physics and bounds can reuse their indexes.
    result.framePlan.units.reserve(request.frame.units.size());
    for (const auto &unit : request.frame.units) {
      TextEffectUnitFramePlan unitPlan;
      unitPlan.stableUnitId = unit.stableUnitId;
      result.framePlan.units.push_back(std::move(unitPlan));
    }

    const std::vector<TimedTextSpan> emptyTimedSpans;
    const auto &timedSpans = request.timedSpans ? *request.timedSpans
                                                : emptyTimedSpans;
    if (request.animations) {
      if (!ValidateTextAnimationStack(*request.animations,
                                      !timedSpans.empty(), &error)) {
        AddEvaluationDiagnostic(result, "text_effect.animation_invalid", {},
                                std::move(error));
        return result;
      }
      if (!request.suppressAnimation &&
          !request.animations->layers.empty() &&
          clipDurationUs <= 0) {
        AddEvaluationDiagnostic(
            result, "text_effect.duration_invalid", {},
            "authored text animation requires a positive clip duration");
        return result;
      }
    }

    if (request.animations && !request.suppressAnimation) {
      struct PostEffectOwnerSample final {
        const TextPostEffectSpec *effect;
        TextAnimationLayerEvaluationSample sample;
        std::int64_t durationUs;
        bool enabled;
      };
      struct LayerOwnerSample final {
        TextAnimationLayerEvaluationSample sample;
        std::int64_t durationUs;
        bool enabled;
        TextAnimationTimeDriverKind clockKind;
      };
      std::unordered_map<std::string, PostEffectOwnerSample> postEffectOwners;
      std::unordered_map<std::string, LayerOwnerSample> layerOwners;
      const auto phaseDurations = ResolveTextAnimationDurations(
          *request.animations, clipDurationUs);
      for (const auto &layer : request.animations->layers) {
        const auto sample = SampleTextAnimationLayerEvaluation(
            *request.animations, layer, clipLocalTimeUs, clipDurationUs,
            timedSpans);
        const auto duration = layer.timeDriver.durationUs > 0
                                  ? layer.timeDriver.durationUs
                                  : clipDurationUs;
        layerOwners.emplace(layer.layerId,
                            LayerOwnerSample{sample, duration, layer.enabled,
                                             layer.timeDriver.kind});
        for (const auto &effect : layer.postEffects) {
          if (!postEffectOwners.emplace(
                  effect.effectId,
                  PostEffectOwnerSample{&effect, sample, duration, layer.enabled})
                   .second) {
            AddEvaluationDiagnostic(
                result, "text_effect.post_owner_ambiguous", effect.effectId,
                "execution post-effect must have one authored layer owner");
            return result;
          }
        }
      }
      std::function<bool(const std::vector<TextEffectExecutionNode> &,
                         std::vector<TextEffectExecutionNodeFramePlan> &)>
          projectNodes;
      projectNodes = [&](const std::vector<TextEffectExecutionNode> &nodes,
                         std::vector<TextEffectExecutionNodeFramePlan> &plans) {
        plans.reserve(nodes.size());
        for (const auto &node : nodes) {
          TextEffectExecutionNodeFramePlan plan;
          plan.nodeId = node.nodeId;
          plan.kind = node.kind;
          plan.capability = node.capability;
          plan.inputIds = node.inputIds;
          plan.resourceIds = node.resourceIds;
          plan.postEffectKind = node.postEffectKind;
          plan.hasAuthoredTimeDriver = node.timeDriver.has_value();
          plan.stateId = node.stateId;
          plan.historyId = node.historyId;
          plan.physicsSpec = node.physicsSpec;
          plan.collisionSpec = node.collisionSpec;
          plan.staticAffine = node.staticAffine;
          plan.camera = node.camera;
          for (const auto &resourceId : node.resourceIds) {
            const auto found = resourcesById.find(resourceId);
            if (found == resourcesById.end() ||
                found->second->assetId.empty() ||
                found->second->digest.empty()) {
              AddEvaluationDiagnostic(
                  result, "text_effect.resource_missing", node.nodeId,
                  "execution graph references an unadmitted resource");
              return false;
            }
          }
          if (node.kind == TextEffectExecutionNodeKind::MediaInput &&
              node.resourceIds.empty()) {
            AddEvaluationDiagnostic(
                result, "text_effect.media_input_missing", node.nodeId,
                "media input node requires an admitted resource");
            return false;
          }
          if (!node.stateId.empty()) {
            const auto found = statesById.find(node.stateId);
            if (found == statesById.end()) {
              AddEvaluationDiagnostic(result, "text_effect.state_missing",
                                      node.nodeId,
                                      "execution graph state is unavailable");
              return false;
            }
            plan.stateRevision = found->second->revision;
            plan.stateElapsedUs = found->second->elapsedUs;
            plan.randomSeed = node.randomSeed != 0U
                                  ? node.randomSeed
                                  : found->second->randomSeed;
          }
          if (!node.historyId.empty()) {
            const auto found = historiesById.find(node.historyId);
            if (found == historiesById.end()) {
              AddEvaluationDiagnostic(
                  result, "text_effect.history_missing", node.nodeId,
                  "execution graph history is unavailable");
              return false;
            }
            plan.historyRevision = found->second->revision;
          }
          if (node.timeDriver) {
            TextAnimationLayerSpec clockLayer;
            clockLayer.layerId = node.nodeId;
            clockLayer.timeDriver = *node.timeDriver;
            TextAnimationStack clockStack;
            clockStack.layers.push_back(clockLayer);
            auto nodeTimeUs = clipLocalTimeUs;
            if (node.timeDriver->kind ==
                    TextAnimationTimeDriverKind::ClipLocal &&
                !node.ownerLayerId.empty()) {
              const auto owner = layerOwners.find(node.ownerLayerId);
              if (owner != layerOwners.end()) {
                // A graph node owned by an animation phase starts at that
                // phase's boundary. Its ClipLocal driver plays within the
                // phase, including when an exit phase starts near clip end.
                if (owner->second.clockKind ==
                    TextAnimationTimeDriverKind::ExitPhase)
                  nodeTimeUs -= clipDurationUs - phaseDurations.exitUs;
                else if (owner->second.clockKind ==
                         TextAnimationTimeDriverKind::LoopPhase)
                  nodeTimeUs -= phaseDurations.enterUs;
              }
            }
            const auto sample = SampleTextAnimationLayerEvaluation(
                clockStack, clockStack.layers.front(), nodeTimeUs,
                clipDurationUs, timedSpans);
            plan.progress = sample.progress;
            plan.active = sample.active;
            const auto effectDurationUs = node.timeDriver->durationUs > 0
                                              ? node.timeDriver->durationUs
                                              : clipDurationUs;
            plan.effectTimeUs = static_cast<std::int64_t>(std::llround(
                sample.progress * static_cast<double>(effectDurationUs)));
          } else {
            plan.effectTimeUs = clipLocalTimeUs;
            plan.progress = clipDurationUs > 0
                                ? static_cast<double>(clipLocalTimeUs) /
                                      static_cast<double>(clipDurationUs)
                                : 0.0;
          }
          if (!node.ownerLayerId.empty()) {
            const auto owner = layerOwners.find(node.ownerLayerId);
            if (owner == layerOwners.end()) {
              AddEvaluationDiagnostic(result, "text_effect.layer_owner_missing",
                                      node.nodeId,
                                      "execution node requires its authored layer owner");
              return false;
            }
            const auto &sample = owner->second;
            plan.active = plan.active && sample.enabled && sample.sample.active;
            if (!node.timeDriver) {
              plan.progress = sample.sample.progress;
              plan.effectTimeUs = static_cast<std::int64_t>(std::llround(
                  sample.sample.progress * static_cast<double>(sample.durationUs)));
            }
          }
          if (node.kind == TextEffectExecutionNodeKind::PostEffectPass) {
            const auto owner = postEffectOwners.find(node.nodeId);
            if (owner == postEffectOwners.end() || !node.postEffectKind ||
                owner->second.effect->kind != *node.postEffectKind) {
              AddEvaluationDiagnostic(
                  result, "text_effect.post_owner_missing", node.nodeId,
                  "execution post-effect requires its authored parameter owner");
              return false;
            }
            // Parameter projection below omits inactive animation layers. The
            // graph must sample the same owner instead of executing a dangling
            // post node before an exit phase starts or after a once phase ends.
            const auto &sample = owner->second;
            plan.active = plan.active && sample.enabled && sample.sample.active;
            if (!node.timeDriver) {
              plan.progress = sample.sample.progress;
              plan.effectTimeUs = static_cast<std::int64_t>(std::llround(
                  sample.sample.progress * static_cast<double>(sample.durationUs)));
            }
          }
          if (!projectNodes(node.children, plan.children))
            return false;
          plans.push_back(std::move(plan));
        }
        return true;
      };
      if (!projectNodes(request.animations->executionGraph.nodes,
                        result.framePlan.executionGraph.nodes)) {
        return result;
      }
    }

    if (request.animations && !request.suppressAnimation) {
      const auto &animations = *request.animations;
      const auto timedProperties = SampleTextTimedPropertyPatches(
          animations, clipLocalTimeUs, clipDurationUs, timedSpans);
      const auto appendTimedProperty =
          [&](const TextTimedPropertySample &sample,
              TextPropertyAssignment assignment) {
            const auto *descriptor =
                DescribeTextProperty(assignment.address.property);
            const bool materialLayer =
                descriptor &&
                (descriptor->legalScopeMask &
                 TextPropertyScopeBit(TextPropertyScope::Utf8Range)) == 0U &&
                (descriptor->legalScopeMask &
                 TextPropertyScopeBit(
                     TextPropertyScope::GlyphMaterialLayer)) != 0U;
            assignment.address.target.scope =
                materialLayer ? TextPropertyScope::GlyphMaterialLayer
                              : TextPropertyScope::Utf8Range;
            assignment.address.target.contentSlotId.clear();
            assignment.address.target.paragraphIds.clear();
            assignment.address.target.runIds = {sample.runId};
            assignment.address.target.range =
                TextUtf8Range{sample.utf8Begin, sample.utf8End};
            if (!materialLayer)
              assignment.address.target.layerId.clear();
            result.framePlan.sampledProperties.push_back(
                std::move(assignment));
          };
      for (const auto &sample : timedProperties) {
        for (const auto &assignment : sample.patch.assignments)
          appendTimedProperty(sample, assignment);
        if (sample.opacity) {
          TextPropertyAssignment opacity;
          opacity.address.property = TextPropertyId::GlyphFillOpacity;
          opacity.value = static_cast<double>(*sample.opacity);
          appendTimedProperty(sample, std::move(opacity));
        }
      }

      for (std::size_t layerIndex = 0U;
           layerIndex < animations.layers.size(); ++layerIndex) {
        const auto &layer = animations.layers[layerIndex];
        if (!layer.enabled)
          continue;
        const auto layerSample = SampleTextAnimationLayerEvaluation(
            animations, layer, clipLocalTimeUs, clipDurationUs, timedSpans);
        result.framePlan.stateTransitions.push_back(
            {layer.layerId, layerSample.active, layerSample.progress});
        if (!layerSample.active)
          continue;

        TextAnimationLayerExecutionEvidence layerEvidence;
        if (request.executionEvidence) {
          layerEvidence.layerId = layer.layerId;
          layerEvidence.progress = layerSample.progress;
          for (const auto &sample : timedProperties) {
            if (sample.layerId == layer.layerId &&
                (!sample.patch.assignments.empty() || sample.opacity))
              layerEvidence.sampledTimedSpanIds.push_back(sample.spanId);
          }
        }

        std::vector<std::size_t> targetIndexes;
        for (std::size_t index = 0U; index < request.frame.units.size();
             ++index) {
          if (TargetContainsUnit(layer.target, request.frame.units[index]))
            targetIndexes.push_back(index);
        }
        if (targetIndexes.empty() &&
            layer.target.scope == TextPropertyScope::Composition) {
          targetIndexes.reserve(request.frame.units.size());
          for (std::size_t index = 0U; index < request.frame.units.size();
               ++index) {
            targetIndexes.push_back(index);
          }
        }
        auto layerBounds = UnitSetBounds(request.frame, targetIndexes);
        if (request.executionEvidence)
          layerEvidence.targetUnits = targetIndexes.size();
        if (!NonEmptyRect(layerBounds))
          layerBounds = request.frame.tightTextRect;
        auto layerLayoutBounds =
            UnitSetBounds(request.frame, targetIndexes, false);
        if (!NonEmptyRect(layerLayoutBounds))
          layerLayoutBounds = request.frame.textRect;

        std::optional<TextEffectRenderGroupExecutionPlan> renderGroupPlan;
        if (layer.renderGroup && !layer.postEffects.empty()) {
          TextEffectRenderGroupExecutionPlan execution;
          execution.layerId = layer.layerId;
          execution.renderGroupInstanceId = layerIndex + 1U;
          execution.spec = *layer.renderGroup;
          TextEffectRect expandedBounds{};
          if (!ResolveEvaluationRenderGroupBounds(
                  *layer.renderGroup, request.frame, targetIndexes,
                  execution.fixedGeometryBounds, expandedBounds)) {
            AddEvaluationDiagnostic(
                result, "text_effect.render_group_bounds_invalid",
                layer.layerId,
                "text render-group frame geometry could not be resolved");
            return result;
          }
          execution.visualSourceBounds = execution.fixedGeometryBounds;
          result.framePlan.bounds.visualBounds = UnionRect(
              result.framePlan.bounds.visualBounds, expandedBounds);
          result.framePlan.bounds.expandedRenderTargetBounds = UnionRect(
              result.framePlan.bounds.expandedRenderTargetBounds,
              expandedBounds);
          if (!targetIndexes.empty()) {
            execution.fixedUnitRange.begin =
                *std::min_element(targetIndexes.begin(), targetIndexes.end());
            execution.fixedUnitRange.end =
                *std::max_element(targetIndexes.begin(), targetIndexes.end()) +
                1U;
          }
          renderGroupPlan = std::move(execution);
          if (request.executionEvidence)
            layerEvidence.renderGroupBoundsResolved =
                !targetIndexes.empty() && NonEmptyRect(expandedBounds);
        }

        std::unordered_set<std::uint64_t> layerAnimationUnitIds;
        const auto mergeLayerAnimationUnit =
            [&](TextEffectUnitFramePlan &destination,
                const TextEffectUnitFramePlan &sampled) {
              if (layerAnimationUnitIds.emplace(sampled.stableUnitId).second) {
                MergeEvaluationUnit(destination, sampled, layer.combineMode);
              } else {
                MergeAnimatorEvaluationUnit(destination, sampled);
              }
            };

        std::optional<TextLayerAnimationSample> layerTrackSample;
        std::optional<TextEffectTransformPlan> layerTrackTransform;
        float layerTrackOffsetX = 0.0F;
        float layerTrackOffsetY = 0.0F;
        if (layer.layerTrack) {
          layerTrackSample = SampleTextLayerAnimationClips(
              {*layer.layerTrack}, clipLocalTimeUs, clipDurationUs);
          layerTrackOffsetX =
              layerTrackSample->positionX *
              (request.referenceCanvas.width +
               layerBounds.width * std::fabs(layerTrackSample->scaleX)) *
              0.5F;
          layerTrackOffsetY =
              layerTrackSample->positionY *
              (request.referenceCanvas.height +
               layerBounds.height * std::fabs(layerTrackSample->scaleY)) *
              0.5F;
          if (!targetIndexes.empty()) {
            TextEffectTransformComponents components;
            components.offsetX = layerTrackOffsetX;
            components.offsetY = layerTrackOffsetY;
            components.scaleX = layerTrackSample->scaleX;
            components.scaleY = layerTrackSample->scaleY;
            components.rotationZ = layerTrackSample->rotationDegrees;
            layerTrackTransform = ResolveTextEffectTransformPlan(
                components, layerBounds, layerLayoutBounds,
                request.frame.textRect);
            if (!layerTrackTransform) {
              AddEvaluationDiagnostic(
                  result, "text_effect.layer_transform_invalid",
                  layer.layerId, "text layer animation transform is invalid");
              return result;
            }
            for (const auto index : targetIndexes) {
              TextEffectUnitFramePlan sampled;
              sampled.stableUnitId =
                  request.frame.units[index].stableUnitId;
              sampled.opacity = layerTrackSample->opacity;
              sampled.transforms.push_back(*layerTrackTransform);
              mergeLayerAnimationUnit(result.framePlan.units[index], sampled);
            }
          }
        }

        for (const auto &animator : layer.animators) {
          std::vector<std::size_t> animatorIndexes;
          for (const auto index : targetIndexes) {
            if (AnimatorContainsUnit(animator, request.frame.units[index]))
              animatorIndexes.push_back(index);
          }
          const auto groups = BuildEvaluationUnitGroups(
              animator, animatorIndexes, request.frame);
          TextAnimatorSampler animatorSampler(animator, groups.size(),
                                               layerSample.progress);
          TextAnimatorExecutionEvidence animatorEvidence;
          const EvaluationAnimatorGeometry geometry(
              animator, animatorIndexes, request.frame);
          for (std::size_t groupIndex = 0U; groupIndex < groups.size();
               ++groupIndex) {
            const auto &group = groups[groupIndex];
            const auto selectorSample =
                animatorSampler.Sample(groupIndex);
            if (request.executionEvidence) {
              ++animatorEvidence.sampledGroups;
              animatorEvidence.sampledSelectors += animator.selectors.size();
              animatorEvidence.sampledTracks += animator.tracks.size();
            }
            for (const auto unitIndex : group.indexes) {
              const auto sample = ResolveEvaluationAnimatorPosition(
                  animator, selectorSample, geometry.positionBasisBounds,
                  geometry.scopeBounds,
                  request.frame.units[unitIndex], request.referenceCanvas);
              TextEffectUnitFramePlan sampled;
              sampled.stableUnitId =
                  request.frame.units[unitIndex].stableUnitId;
              sampled.opacity = sample.opacity;
              if (std::any_of(animator.tracks.begin(), animator.tracks.end(),
                              [](const auto &track) {
                                return track.property ==
                                       TextAnimatedProperty::BlurRadius;
                              })) {
                sampled.sdfBlurRadius = std::max(0.0F, sample.blurRadius);
              }
              if (sample.fillColor && sample.fillColorInfluence > 0.0F) {
                const float influence =
                    std::clamp(sample.fillColorInfluence, 0.0F, 1.0F);
                const auto mix = [influence](const float from,
                                             const float to) {
                  return from + (to - from) * influence;
                };
                const auto authoredColor =
                    request.frame.units[unitIndex].instanceColor;
                sampled.instanceColor =
                    Color{mix(authoredColor.red, sample.fillColor->red),
                          mix(authoredColor.green, sample.fillColor->green),
                          mix(authoredColor.blue, sample.fillColor->blue),
                          mix(authoredColor.alpha, sample.fillColor->alpha)};
              }
              const auto anchorBounds = geometry.anchors(unitIndex);
              const auto &tightAnchorBounds = anchorBounds.tight;
              const auto &layoutAnchorBounds = anchorBounds.layout;
              TextEffectTransformComponents components;
              components.offsetX = sample.positionX;
              components.offsetY = sample.positionY;
              components.offsetZ = sample.positionZ;
              components.scaleX = sample.scaleX;
              components.scaleY = sample.scaleY;
              components.rotationX = -sample.rotationX;
              components.rotationY = sample.rotationY;
              components.rotationZ = sample.rotationZ;
              components.shearX = sample.shearX;
              components.shearY = sample.shearY;
              components.anchorX = sample.anchorOffsetX;
              components.anchorY =
                  -sample.anchorOffsetY + ResolveEvaluationBaselineAnchor(
                                              animator, unitIndex,
                                              tightAnchorBounds,
                                              request.frame);
              components.anchorRangeBegin = 0U;
              components.anchorRangeEnd = 1U;
              components.projection = animator.projection;
              const auto transform = ResolveTextEffectTransformPlan(
                  components, tightAnchorBounds, layoutAnchorBounds,
                  request.frame.textRect);
              if (!transform) {
                AddEvaluationDiagnostic(
                    result, "text_effect.animator_transform_invalid",
                    animator.animatorId,
                    "text animator transform is invalid");
                return result;
              }
              sampled.transforms.push_back(*transform);
              mergeLayerAnimationUnit(result.framePlan.units[unitIndex],
                                      sampled);
              if (request.executionEvidence)
                ++animatorEvidence.projectedUnits;
              if (sample.tracking != 0.0F) {
                result.framePlan.layoutMutations.push_back(
                    {TextEffectLayoutMutationKind::LetterSpacing,
                     sampled.stableUnitId, layer.combineMode,
                     sample.tracking});
              }
            }
          }

          if (!animator.effectProgramId.empty()) {
            const auto *program = FindTextEffectProgram(
                animations.effectPrograms, animator.effectProgramId);
            if (!program) {
              AddEvaluationDiagnostic(
                  result, "text_effect.program_missing", animator.animatorId,
                  "text animator references a missing effect program");
              return result;
            }
            auto programInput = ProgramSourceFrame(
                request, groups, layerSample.progress, clipLocalTimeUs,
                layerSample.durationUs);
            if (std::getenv("VIDEOCUT_TRACE_TEXT_EFFECT_FRAMEPLAN") !=
                nullptr) {
              const auto sourceTextRect = ReferenceRectToQtTextProgramSource(
                  request.frame.textRect, request.referenceCanvas,
                  request.sourceToReferenceScale);
              const auto sourceTightTextRect =
                  ReferenceRectToQtTextProgramSource(
                      request.frame.tightTextRect, request.referenceCanvas,
                      request.sourceToReferenceScale);
              std::fprintf(
                  stderr,
                  "[VIDEOCUT_TEXT_EFFECT_PROGRAM_SOURCE] program=%s "
                  "text_rect=[%.9g %.9g %.9g %.9g] "
                  "tight_text_rect=[%.9g %.9g %.9g %.9g]\n",
                  program->programId.c_str(), sourceTextRect.x,
                  sourceTextRect.y, sourceTextRect.width,
                  sourceTextRect.height, sourceTightTextRect.x,
                  sourceTightTextRect.y, sourceTightTextRect.width,
                  sourceTightTextRect.height);
              for (const auto &unit : programInput.frame.units) {
                std::fprintf(
                    stderr,
                    "[VIDEOCUT_TEXT_EFFECT_PROGRAM_INPUT] program=%s "
                    "time_us=%lld unit=%zu row=%zu index_in_row=%zu "
                    "initial=[%.9g %.9g] rect=[%.9g %.9g %.9g %.9g] "
                    "text_rect=[%.9g %.9g %.9g %.9g] font=%.9g "
                    "source_to_reference=%.9g\n",
                    program->programId.c_str(),
                    static_cast<long long>(clipLocalTimeUs), unit.index,
                    unit.row, unit.indexInRow, unit.initialPositionX,
                    unit.initialPositionY, unit.rect.x, unit.rect.y,
                    unit.rect.width, unit.rect.height,
                    programInput.frame.textRect.x,
                    programInput.frame.textRect.y,
                    programInput.frame.textRect.width,
                    programInput.frame.textRect.height, unit.fontSize,
                    request.sourceToReferenceScale);
              }
            }
            TextEffectFramePlan sampledProgram;
            TextProgramExecutionEvidence programEvidence;
            if (!EvaluateTextEffectProgram(*program, programInput.frame,
                                           sampledProgram, error,
                                           request.executionEvidence ? &programEvidence : nullptr) ||
                !ProjectProgramPlan(sampledProgram,
                                    result.framePlan.executionGraph,
                                    request.sourceToReferenceScale,
                                    request.referenceCanvas)) {
              AddEvaluationDiagnostic(
                  result, "text_effect.program_evaluation_failed",
                  program->programId,
                  error.empty() ? "text effect program projection is invalid"
                                : std::move(error));
              return result;
            }
            if (request.executionEvidence)
              request.executionEvidence->programs.push_back(std::move(programEvidence));
            const bool traceProgram =
                std::getenv("VIDEOCUT_TRACE_TEXT_EFFECT_FRAMEPLAN") != nullptr;
            if (traceProgram) {
              for (const auto &parameter : sampledProgram.executionParameters) {
                std::fprintf(
                    stderr,
                    "[VIDEOCUT_TEXT_EFFECT_PARAMETER] program=%s time_us=%lld "
                    "node=%s kind=%.*s slot=%u space=%.*s values=",
                    program->programId.c_str(),
                    static_cast<long long>(clipLocalTimeUs),
                    parameter.nodeId.c_str(),
                    static_cast<int>(TextEffectExecutionParameterKindName(
                                         parameter.parameter)
                                         .size()),
                    TextEffectExecutionParameterKindName(parameter.parameter)
                        .data(),
                    parameter.slot,
                    static_cast<int>(TextEffectExecutionParameterSpaceName(
                                         parameter.valueSpace)
                                         .size()),
                    TextEffectExecutionParameterSpaceName(parameter.valueSpace)
                        .data());
                for (const auto value : parameter.values)
                  std::fprintf(stderr, " %.9g", value);
                std::fprintf(stderr, "\n");
              }
            }
            const auto publishUnit =
                [&](const std::size_t sourceIndex,
                    const TextEffectUnitFramePlan &unit) {
              if (traceProgram) {
                std::fprintf(
                    stderr,
                    "[VIDEOCUT_TEXT_EFFECT_FRAMEPLAN] program=%s time_us=%lld "
                    "unit=%llu opacity=%g color=[%g %g %g %g] font=%g "
                    "blur=%g transforms=%zu\n",
                    program->programId.c_str(),
                    static_cast<long long>(clipLocalTimeUs),
                    static_cast<unsigned long long>(unit.stableUnitId),
                    unit.opacity.value_or(-1.0F),
                    unit.instanceColor ? unit.instanceColor->red : -1.0F,
                    unit.instanceColor ? unit.instanceColor->green : -1.0F,
                    unit.instanceColor ? unit.instanceColor->blue : -1.0F,
                    unit.instanceColor ? unit.instanceColor->alpha : -1.0F,
                    unit.absoluteFontSize.value_or(-1.0F),
                    unit.sdfBlurRadius.value_or(-1.0F),
                    unit.transforms.size());
              }
              mergeLayerAnimationUnit(
                  result.framePlan.units[sourceIndex], unit);
            };
            auto reboundProgram = ExpandProgramPlanToSourceUnits(
                std::move(sampledProgram), programInput, request.frame,
                publishUnit);
            AppendEvaluationPlanNonUnitData(result.framePlan, reboundProgram);
          }
          if (request.executionEvidence && animatorEvidence.projectedUnits > 0U) {
            animatorEvidence.animatorId = animator.animatorId;
            layerEvidence.animators.push_back(std::move(animatorEvidence));
          }
        }

        for (const auto &decoration : layer.decorations) {
          const auto *resource = FindEvaluationResource(request, decoration);
          if (!resource || resource->assetId.empty() ||
              resource->digest.empty() || resource->intrinsicWidth <= 0.0F ||
              resource->intrinsicHeight <= 0.0F) {
            AddEvaluationDiagnostic(
                result, "text_effect.decoration_resource_missing",
                decoration.decorationId,
                "animated decoration is outside the admitted resource closure");
            return result;
          }
          const auto sample = SampleTextDecorationAnimation(
              decoration, static_cast<float>(layerSample.progress));
          auto basedBounds = layerBounds;
          switch (decoration.extentSpace) {
          case TextDecorationExtentSpace::TextLocal:
            break;
          case TextDecorationExtentSpace::CanvasWidth:
            basedBounds = {0.0F, basedBounds.y,
                           request.referenceCanvas.width, basedBounds.height};
            break;
          case TextDecorationExtentSpace::CanvasHeight:
            basedBounds = {basedBounds.x, 0.0F, basedBounds.width,
                           request.referenceCanvas.height};
            break;
          case TextDecorationExtentSpace::CanvasFull:
            basedBounds = {0.0F, 0.0F, request.referenceCanvas.width,
                           request.referenceCanvas.height};
            break;
          }
          if (sample.anchor == TextAnimatedDecorationAnchor::CanvasCenter) {
            basedBounds.x += request.referenceCanvas.width * 0.5F -
                             RectCenterX(basedBounds);
            basedBounds.y += request.referenceCanvas.height * 0.5F -
                             RectCenterY(basedBounds);
          }
          const float expandedWidth =
              basedBounds.width * std::max(0.0F, decoration.expandRatioX);
          const float expandedHeight =
              basedBounds.height * std::max(0.0F, decoration.expandRatioY);
          const float expandX = (expandedWidth - basedBounds.width) * 0.5F;
          const float expandY = (expandedHeight - basedBounds.height) * 0.5F;
          const TextEffectRect expandedBounds{
              RectCenterX(basedBounds) - expandedWidth * 0.5F,
              RectCenterY(basedBounds) - expandedHeight * 0.5F,
              expandedWidth, expandedHeight};
          const auto bounds = FitDecorationRect(
              expandedBounds, resource->intrinsicWidth,
              resource->intrinsicHeight, sample.fit);
          const auto assetDuration = resource->durationUs > 0
                                         ? resource->durationUs
                                         : clipDurationUs;
          const auto assetTimeUs = ResolveDecorationAssetTime(
              assetDuration, decoration.playback, sample.assetProgress);

          TextEffectDecorationPass pass;
          pass.passId =
              layer.layerId + ":decoration:" + decoration.decorationId;
          pass.layerId = layer.layerId;
          pass.decorationId = decoration.decorationId;
          pass.assetId = resource->assetId;
          if (!targetIndexes.empty()) {
            pass.basedRange.begin =
                *std::min_element(targetIndexes.begin(), targetIndexes.end());
            pass.basedRange.end =
                *std::max_element(targetIndexes.begin(), targetIndexes.end()) +
                1U;
          }
          pass.basedRect = basedBounds;
          pass.sourceIntrinsicSize = {resource->intrinsicWidth,
                                      resource->intrinsicHeight};
          switch (sample.fit) {
          case TextAnimatedDecorationFit::FitWidth:
            pass.fitMode = TextBackdropFitMode::Width;
            break;
          case TextAnimatedDecorationFit::FitHeight:
            pass.fitMode = TextBackdropFitMode::Height;
            break;
          case TextAnimatedDecorationFit::FitLongSide:
            pass.fitMode = TextBackdropFitMode::LongSide;
            break;
          case TextAnimatedDecorationFit::FitShortSide:
            pass.fitMode = TextBackdropFitMode::ShortSide;
            break;
          default:
            pass.fitMode = TextBackdropFitMode::Stretch;
            break;
          }
          pass.sourcePivotX = 0.5F + sample.pivotX * 0.5F;
          pass.sourcePivotY = 0.5F + sample.pivotY * 0.5F;
          pass.expand = {expandX, expandY, expandX, expandY};
          pass.sourceOutsets = {decoration.sourceOutsets.left,
                                decoration.sourceOutsets.top,
                                decoration.sourceOutsets.right,
                                decoration.sourceOutsets.bottom};
          pass.bounds = bounds;
          const auto basis = ResolveEvaluationDecorationBasis(sample);
          float offsetX = sample.offsetX +
                          sample.relativeOffsetX * expandedBounds.width *
                              0.5F;
          float offsetY = sample.offsetY -
                          sample.relativeOffsetY * expandedBounds.height *
                              0.5F;
          // VideoAnimSeq exposes Sprite2D pivot coordinates on its fixed
          // [-1,+1] logical mesh. Resolve that source-local displacement
          // before emitting the final top-left-domain matrix.
          const float intrinsicAspect =
              resource->intrinsicWidth / resource->intrinsicHeight;
          const float pivotLocalX =
              sample.pivotX * intrinsicAspect * bounds.width * 0.5F;
          const float pivotLocalY =
              -sample.pivotY * bounds.height * 0.5F;
          offsetX += basis.xx * pivotLocalX + basis.xy * pivotLocalY;
          offsetY += basis.yx * pivotLocalX + basis.yy * pivotLocalY;
          float opacity = sample.opacity;
          // Qt Sprite2D flattens X/Y rotations to the upper-left 2x2 basis
          // before raster composition.  Keeping the generic 4x4 TRS here
          // leaves Z terms in otherwise orthographic decorations and makes a
          // captured raster impossible to place (for example rotationY=-180).
          TextEffectTransformPlan transform;
          const float centerX = RectCenterX(bounds);
          const float centerY = RectCenterY(bounds);
          transform.localToText.columnMajor = {
              basis.xx,
              basis.yx,
              0.0F,
              0.0F,
              basis.xy,
              basis.yy,
              0.0F,
              0.0F,
              0.0F,
              0.0F,
              1.0F,
              0.0F,
              centerX + offsetX - basis.xx * centerX - basis.xy * centerY,
              centerY + offsetY - basis.yx * centerX - basis.yy * centerY,
              0.0F,
              1.0F,
          };
          transform.tightAnchorBounds = bounds;
          transform.layoutAnchorBounds = bounds;
          if (!std::all_of(transform.localToText.columnMajor.begin(),
                           transform.localToText.columnMajor.end(),
                           [](const float value) {
                             return std::isfinite(value);
                           })) {
            AddEvaluationDiagnostic(
                result, "text_effect.decoration_transform_invalid",
                decoration.decorationId,
                "text decoration transform is invalid");
            return result;
          }
          if (layerTrackTransform) {
            if (decoration.inherit == TextDecorationTransformInherit::Full) {
              const auto composed = ComposeTextEffectTransforms(
                  {transform, *layerTrackTransform});
              if (!composed) {
                AddEvaluationDiagnostic(
                    result, "text_effect.decoration_inherit_invalid",
                    decoration.decorationId,
                    "text decoration inherited transform is invalid");
                return result;
              }
              transform.localToText = *composed;
              opacity *= layerTrackSample->opacity;
            } else {
              transform.localToText.columnMajor = MultiplyMatrix(
                  TranslationMatrix(layerTrackOffsetX, layerTrackOffsetY,
                                    0.0F),
                  transform.localToText.columnMajor);
            }
          }
          pass.transform = transform;
          pass.opacity = std::clamp(opacity, 0.0F, 1.0F);
          pass.assetTimeUs = assetTimeUs;
          result.framePlan.decorationPasses.push_back(pass);
          if (request.executionEvidence)
            ++layerEvidence.projectedDecorations;
          if (renderGroupPlan) {
            if (pass.effectScope == TextEffectDecorationEffectScope::
                                        InsideRenderGroupBehindText) {
              renderGroupPlan->behindDecorationInputIds.push_back(pass.passId);
            } else if (pass.effectScope == TextEffectDecorationEffectScope::
                                               InsideRenderGroupInFrontOfText) {
              renderGroupPlan->frontDecorationInputIds.push_back(pass.passId);
            }
          }
          result.framePlan.resources.push_back(
              {resource->resourceId, resource->assetId, resource->digest,
               assetTimeUs});
          result.framePlan.compositeOrder.push_back(
              {pass.passId, TextEffectCompositeItemKind::Decoration,
               pass.zOrder, TextBlendMode::SourceOver});
        }

        const auto effectDurationUs = layer.timeDriver.durationUs > 0
                                          ? layer.timeDriver.durationUs
                                          : clipDurationUs;
        std::string previousPostEffectId;
        for (std::size_t effectIndex = 0U;
             effectIndex < layer.postEffects.size(); ++effectIndex) {
          const auto &effect = layer.postEffects[effectIndex];
          TextEffectPostEffectNode node;
          node.nodeId = effect.effectId;
          node.layerId = layer.layerId;
          node.effectNodeInstanceId =
              ((static_cast<std::uint64_t>(layerIndex) + 1U) << 32U) |
              (static_cast<std::uint64_t>(effectIndex) + 1U);
          node.kind = effect.kind;
          node.inputIds = effect.inputIds;
          if (node.inputIds.empty() && !previousPostEffectId.empty())
            node.inputIds = {previousPostEffectId};
          node.parameters = effect.parameters;
          node.amount = static_cast<float>(SampleTextKeyframeCurve(
              effect.amountKeyframes, layerSample.progress, effect.amount));
          node.paddingPx = effect.paddingPx;
          node.effectTimeUs = static_cast<std::int64_t>(std::llround(
              layerSample.progress * static_cast<double>(effectDurationUs)));
          node.renderGroup = renderGroupPlan;
          result.framePlan.postEffectNodes.push_back(std::move(node));
          result.framePlan.stateTransitions.push_back(
              {effect.effectId, layerSample.active, layerSample.progress});
          previousPostEffectId = effect.effectId;
        }
        if (request.executionEvidence)
          request.executionEvidence->animationLayers.push_back(
              std::move(layerEvidence));
      }

      if (!ValidatePostEffectDag(result.framePlan, error)) {
        AddEvaluationDiagnostic(result, "text_effect.post_dag_invalid", {},
                                std::move(error));
        return result;
      }
      std::unordered_set<std::string> consumedPostNodes;
      for (const auto &node : result.framePlan.postEffectNodes) {
        for (const auto &input : node.inputIds)
          consumedPostNodes.emplace(input);
      }
      for (const auto &node : result.framePlan.postEffectNodes) {
        if (consumedPostNodes.count(node.nodeId) == 0U) {
          result.framePlan.compositeOrder.push_back(
              {node.nodeId, TextEffectCompositeItemKind::PostEffect, 0,
               TextBlendMode::SourceOver});
        }
      }
    }

    if (!EvaluatePhysicsExecutionGraph(request, result.framePlan, error)) {
      AddEvaluationDiagnostic(result, "text_effect.physics_invalid", {},
                              std::move(error));
      return result;
    }

    if (!BindExecutionParametersToGraph(result.framePlan, error)) {
      AddEvaluationDiagnostic(result,
                              "text_effect.execution_parameter_invalid", {},
                              std::move(error));
      return result;
    }

    if (!ResolveEvaluationBounds(request, result.framePlan, error)) {
      AddEvaluationDiagnostic(result, "text_effect.bounds_invalid", {},
                              std::move(error));
      return result;
    }
    result.valid = true;
    return result;
  } catch (...) {
    result.framePlan = {};
    AddEvaluationDiagnostic(result, "text_effect.evaluation_failed", {},
                            "text effect evaluation allocation failed");
    return result;
  }
}

} // namespace videocut::text
