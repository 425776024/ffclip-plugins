#pragma once

#include "videocut/frame/MediaTime.h"
#include "videocut/text_composition/TextCompositionDocument.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace videocut::text_composition::detail {

inline std::int64_t SourceTickToUs(const std::int64_t tick) noexcept {
  const long double value =
      static_cast<long double>(tick) * 1'000'000.0L /
      static_cast<long double>(frame::kMediaTicksPerSecond);
  if (value <=
      static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
    return std::numeric_limits<std::int64_t>::min();
  if (value >=
      static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(std::llround(value));
}

inline std::int64_t SaturatingDifference(const std::int64_t value,
                                         const std::int64_t origin) noexcept {
  const auto difference =
      static_cast<long double>(value) - static_cast<long double>(origin);
  if (difference <=
      static_cast<long double>(std::numeric_limits<std::int64_t>::min()))
    return std::numeric_limits<std::int64_t>::min();
  if (difference >=
      static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(difference);
}

inline text::TimedTextSpanSemantic
ResolveTimedSemantic(const std::string &semantic) noexcept {
  if (semantic == "keyword")
    return text::TimedTextSpanSemantic::Keyword;
  if (semantic == "emphasis")
    return text::TimedTextSpanSemantic::Emphasis;
  if (semantic == "mention")
    return text::TimedTextSpanSemantic::Mention;
  return text::TimedTextSpanSemantic::KaraokeWord;
}

inline std::vector<text::TimedTextSpan>
ResolveTimedSpans(const TextCompositionDocument &document,
                  const std::int64_t sourceWindowBeginTicks,
                  const std::int64_t sourceWindowEndTicks) {
  std::vector<text::TimedTextSpan> result;
  if (!document.timedText)
    return result;
  const auto beginUs = SourceTickToUs(sourceWindowBeginTicks);
  const auto endUs = SourceTickToUs(sourceWindowEndTicks);
  const auto sliced = SliceTimedTextTrack(*document.timedText, beginUs, endUs);
  result.reserve(sliced.spans.size());
  for (const auto &span : sliced.spans) {
    text::TimedTextSpan resolved;
    resolved.spanId = span.spanId;
    resolved.semantic = ResolveTimedSemantic(span.semantic);
    resolved.paragraphId = span.paragraphId;
    resolved.runId = span.runId;
    resolved.utf8Begin = static_cast<std::size_t>(span.range.begin);
    resolved.utf8End = static_cast<std::size_t>(span.range.end);
    resolved.startOffsetUs = span.startOffsetUs;
    resolved.endOffsetUs = span.endOffsetUs;
    resolved.progressMode = span.progressMode;
    resolved.transitionEndOffsetUs = span.transitionEndOffsetUs;
    result.push_back(std::move(resolved));
  }
  return result;
}

} // namespace videocut::text_composition::detail
