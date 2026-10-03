#include "videocut/frame/FrameProvenance.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <tuple>

namespace videocut::frame {
namespace {

constexpr std::size_t kMaximumEffectExecutionsPerFrame = 4096U;
constexpr std::size_t kMaximumEffectScopeBytes = 64U;
constexpr std::size_t kMaximumPlanFingerprintBytes = 256U;

bool IsCanonicalScope(const std::string &scope) noexcept {
  if (scope.empty() || scope.size() > kMaximumEffectScopeBytes ||
      scope.front() < 'a' || scope.front() > 'z') {
    return false;
  }
  return std::all_of(scope.begin(), scope.end(), [](const char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '_';
  });
}

bool IsCanonicalSha256(const std::string &value) noexcept {
  constexpr std::size_t kPrefixBytes = 7U;
  constexpr std::size_t kHexBytes = 64U;
  if (value.size() != kPrefixBytes + kHexBytes ||
      value.compare(0U, kPrefixBytes, "sha256:") != 0) {
    return false;
  }
  return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(kPrefixBytes),
                     value.end(), [](const char byte) {
                       return (byte >= '0' && byte <= '9') ||
                              (byte >= 'a' && byte <= 'f');
                     });
}

bool IsBoundedVisibleText(const std::string &value,
                          const std::size_t maximumBytes) noexcept {
  if (value.empty() || value.size() > maximumBytes)
    return false;
  return std::all_of(value.begin(), value.end(), [](const char byte) {
    const auto encoded = static_cast<unsigned char>(byte);
    return encoded >= 0x21U && encoded <= 0x7eU;
  });
}

} // namespace

void AppendUniqueFrameProvenance(const FrameProvenance *input,
                                 FrameProvenance &output) {
  if (!input)
    return;
  for (const auto &source : input->sources) {
    const bool duplicate = std::any_of(
        output.sources.begin(), output.sources.end(),
        [&source](const auto &existing) {
          return existing.request_serial == source.request_serial &&
                 existing.session_generation == source.session_generation &&
                 existing.lane_generation == source.lane_generation &&
                 existing.decoded_stream_pts == source.decoded_stream_pts &&
                 existing.clip_id == source.clip_id &&
                 existing.track_id == source.track_id;
        });
    if (!duplicate)
      output.sources.push_back(source);
  }
  for (const auto &effect : input->effect_executions) {
    const bool duplicate = std::any_of(
        output.effect_executions.begin(), output.effect_executions.end(),
        [&effect](const auto &existing) {
          return existing.scope == effect.scope &&
                 existing.owner_id == effect.owner_id &&
                 existing.secondary_owner_id == effect.secondary_owner_id &&
                 existing.plan_fingerprint == effect.plan_fingerprint &&
                 existing.execution_fingerprint == effect.execution_fingerprint;
        });
    if (!duplicate)
      output.effect_executions.push_back(effect);
  }
}

bool ValidateFrameEffectExecutionProvenance(
    const std::vector<FrameEffectExecutionProvenance> &executions,
    std::string &error) {
  error.clear();
  if (executions.size() > kMaximumEffectExecutionsPerFrame) {
    error = "effect execution provenance exceeds the per-frame limit";
    return false;
  }

  using ApplicationIdentity =
      std::tuple<std::string, std::int64_t, std::int64_t>;
  std::set<ApplicationIdentity> applications;
  for (const auto &execution : executions) {
    if (!IsCanonicalScope(execution.scope)) {
      error = "effect execution provenance scope is not canonical";
      return false;
    }
    if (execution.owner_id < -1 || execution.secondary_owner_id < -1) {
      error = "effect execution provenance owner identity is invalid";
      return false;
    }
    if (!IsBoundedVisibleText(execution.plan_fingerprint,
                              kMaximumPlanFingerprintBytes)) {
      error = "effect execution plan fingerprint is invalid";
      return false;
    }
    if (!IsCanonicalSha256(execution.execution_fingerprint)) {
      error = "effect execution fingerprint is not canonical sha256";
      return false;
    }
    if (!applications
             .emplace(execution.scope, execution.owner_id,
                      execution.secondary_owner_id)
             .second) {
      error = "effect execution provenance contains an ambiguous application";
      return false;
    }
  }
  return true;
}

} // namespace videocut::frame
