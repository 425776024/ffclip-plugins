#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace videocut::vector {

enum class VectorStatusCode : std::uint8_t {
  Ok = 0,
  Empty,
  NotReady,
  Canceled,
  InvalidDocument,
  MissingResource,
  UnsupportedFeature,
  BudgetExceeded,
  Failed,
  DeadlineExceeded,
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

struct Rect final {
  float x{0.0F};
  float y{0.0F};
  float width{0.0F};
  float height{0.0F};
};

enum class RenderQuality : std::uint8_t {
  PreviewDraft = 0,
  PreviewActive,
  PreviewPaused,
  Export,
};

using CancelCheck = std::function<bool()>;

} // namespace videocut::vector
