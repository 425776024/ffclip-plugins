#include "videocut/text_composition/TextEditing.h"

#include "CanonicalTextIdentity.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace videocut::text_composition {
namespace {

void Add(std::vector<Diagnostic> &diagnostics, std::string code,
         std::string subject, std::string message,
         const DiagnosticSeverity severity = DiagnosticSeverity::Error) {
  diagnostics.push_back({std::move(code), severity, "text-edit",
                         std::move(subject), std::move(message)});
}

void AppendTextDiagnostics(std::vector<Diagnostic> &destination,
                           const std::vector<text::Diagnostic> &source) {
  for (const auto &diagnostic : source) {
    destination.push_back(
        {"text_property." + diagnostic.code,
         static_cast<DiagnosticSeverity>(diagnostic.severity), "text-edit",
         diagnostic.subjectId, diagnostic.message});
  }
}

struct DecodedCodePoint final {
  std::uint32_t value{0U};
  std::size_t begin{0U};
  std::size_t end{0U};
};

bool DecodeUtf8(const std::string &value,
                std::vector<DecodedCodePoint> &decoded) {
  decoded.clear();
  if (!text::IsValidUtf8(value))
    return false;
  for (std::size_t offset = 0U; offset < value.size();) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::size_t length = 1U;
    std::uint32_t codePoint = first;
    if ((first & 0xe0U) == 0xc0U) {
      length = 2U;
      codePoint = first & 0x1fU;
    } else if ((first & 0xf0U) == 0xe0U) {
      length = 3U;
      codePoint = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
      length = 4U;
      codePoint = first & 0x07U;
    }
    for (std::size_t index = 1U; index < length; ++index)
      codePoint = (codePoint << 6U) |
                  (static_cast<unsigned char>(value[offset + index]) & 0x3fU);
    decoded.push_back({codePoint, offset, offset + length});
    offset += length;
  }
  return true;
}

bool CombiningCodePoint(const std::uint32_t value) noexcept {
  return (value >= 0x0300U && value <= 0x036fU) ||
         (value >= 0x1ab0U && value <= 0x1affU) ||
         (value >= 0x1dc0U && value <= 0x1dffU) ||
         (value >= 0x20d0U && value <= 0x20ffU) ||
         (value >= 0xfe20U && value <= 0xfe2fU) ||
         (value >= 0xfe00U && value <= 0xfe0fU) ||
         (value >= 0xe0100U && value <= 0xe01efU) ||
         (value >= 0x1f3fbU && value <= 0x1f3ffU) || value == 0x200dU;
}

bool RegionalIndicator(const std::uint32_t value) noexcept {
  return value >= 0x1f1e6U && value <= 0x1f1ffU;
}

std::vector<std::size_t> GraphemeBoundaries(const std::string &value) {
  std::vector<DecodedCodePoint> decoded;
  if (!DecodeUtf8(value, decoded))
    return {};
  std::vector<std::size_t> result{0U};
  std::size_t regionalRun = 0U;
  for (std::size_t index = 0U; index < decoded.size(); ++index) {
    bool boundary = index == 0U;
    if (index != 0U) {
      const auto current = decoded[index].value;
      const auto previous = decoded[index - 1U].value;
      boundary = !CombiningCodePoint(current) && previous != 0x200dU;
      if (RegionalIndicator(current) && RegionalIndicator(previous))
        boundary = regionalRun % 2U == 0U;
    }
    if (boundary && decoded[index].begin != 0U)
      result.push_back(decoded[index].begin);
    regionalRun = RegionalIndicator(decoded[index].value) ? regionalRun + 1U
                                                           : 0U;
  }
  result.push_back(value.size());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::string BoundaryDigest(const std::string &value) {
  return vector::Sha256Digest(
      reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
}

bool IsBoundary(const std::string &value, const std::uint64_t offset) {
  if (offset > value.size())
    return false;
  const auto boundaries = GraphemeBoundaries(value);
  return std::binary_search(boundaries.begin(), boundaries.end(),
                            static_cast<std::size_t>(offset));
}

struct RunAddress final {
  std::size_t paragraphIndex{0U};
  std::size_t runIndex{0U};
  std::uint64_t globalBegin{0U};
  std::uint64_t globalEnd{0U};
  std::uint64_t paragraphBegin{0U};
};

std::vector<RunAddress>
BuildRunAddresses(const TextCompositionDocument &document) {
  std::vector<RunAddress> result;
  std::uint64_t global = 0U;
  for (std::size_t paragraphIndex = 0U;
       paragraphIndex < document.content.size(); ++paragraphIndex) {
    const auto paragraphBegin = global;
    for (std::size_t runIndex = 0U;
         runIndex < document.content[paragraphIndex].runs.size(); ++runIndex) {
      const auto length =
          document.content[paragraphIndex].runs[runIndex].utf8Text.size();
      result.push_back({paragraphIndex, runIndex, global, global + length,
                        paragraphBegin});
      global += length;
    }
    if (paragraphIndex + 1U < document.content.size())
      ++global;
  }
  return result;
}

std::uint64_t FlattenedSize(const TextCompositionDocument &document) noexcept {
  std::uint64_t size = document.content.empty() ? 0U
                                                 : document.content.size() - 1U;
  for (const auto &paragraph : document.content)
    for (const auto &run : paragraph.runs)
      size += run.utf8Text.size();
  return size;
}

struct ResolvedPosition final {
  bool valid{false};
  TextDocumentPosition canonical{};
  std::size_t paragraphIndex{0U};
  std::size_t runIndex{0U};
  std::uint64_t global{0U};
};

ResolvedPosition ResolvePosition(const TextCompositionDocument &document,
                                 const TextDocumentPosition &position,
                                 std::vector<Diagnostic> &diagnostics) {
  ResolvedPosition result;
  const auto paragraph = std::find_if(
      document.content.begin(), document.content.end(),
      [&](const auto &candidate) {
        return candidate.paragraphId == position.paragraphId;
      });
  if (paragraph == document.content.end()) {
    Add(diagnostics, "text_edit.position.paragraph_missing",
        position.paragraphId, "position paragraph does not exist");
    return result;
  }
  result.paragraphIndex =
      static_cast<std::size_t>(paragraph - document.content.begin());
  const auto addresses = BuildRunAddresses(document);
  const auto paragraphAddresses = [&]() {
    std::vector<const RunAddress *> values;
    for (const auto &address : addresses)
      if (address.paragraphIndex == result.paragraphIndex)
        values.push_back(&address);
    return values;
  }();
  if (paragraphAddresses.empty()) {
    Add(diagnostics, "text_edit.position.paragraph_uneditable",
        position.paragraphId, "position paragraph has no editable run");
    return result;
  }
  const RunAddress *address = nullptr;
  std::uint64_t ownerOffset = position.ownerUtf8Offset;
  if (position.runId) {
    const auto found = std::find_if(
        paragraphAddresses.begin(), paragraphAddresses.end(),
        [&](const auto *candidate) {
          return paragraph->runs[candidate->runIndex].runId == *position.runId;
        });
    if (found == paragraphAddresses.end()) {
      Add(diagnostics, "text_edit.position.run_missing", *position.runId,
          "position run does not belong to its paragraph");
      return result;
    }
    address = *found;
  } else {
    std::uint64_t remaining = position.ownerUtf8Offset;
    for (const auto *candidate : paragraphAddresses) {
      const auto length = candidate->globalEnd - candidate->globalBegin;
      if (remaining <= length) {
        address = candidate;
        ownerOffset = remaining;
        break;
      }
      remaining -= length;
    }
    if (!address) {
      Add(diagnostics, "text_edit.position.paragraph_offset_invalid",
          position.paragraphId,
          "paragraph-local UTF-8 offset exceeds current content");
      return result;
    }
  }
  const auto &run = paragraph->runs[address->runIndex];
  if (!IsBoundary(run.utf8Text, ownerOffset)) {
    Add(diagnostics, "text_edit.position.grapheme_boundary_invalid", run.runId,
        "position must lie on a canonical grapheme boundary");
    return result;
  }
  const auto boundaries = GraphemeBoundaries(run.utf8Text);
  const auto grapheme = std::lower_bound(boundaries.begin(), boundaries.end(),
                                         ownerOffset);
  const auto graphemeOffset = static_cast<std::uint64_t>(
      std::distance(boundaries.begin(), grapheme));
  const auto digest = BoundaryDigest(run.utf8Text);
  const auto global = address->globalBegin + ownerOffset;
  if (position.globalUtf8Offset != global) {
    Add(diagnostics, "text_edit.position.global_offset_stale", run.runId,
        "position global UTF-8 offset does not match current content");
    return result;
  }
  if (position.graphemeOffset != graphemeOffset) {
    Add(diagnostics, "text_edit.position.grapheme_offset_stale", run.runId,
        "position grapheme offset does not match current content");
    return result;
  }
  if (!position.graphemeBoundaryDigest.empty() &&
      position.graphemeBoundaryDigest != digest) {
    Add(diagnostics, "text_edit.position.boundary_digest_stale", run.runId,
        "position grapheme-boundary digest is stale");
    return result;
  }
  result.valid = true;
  result.runIndex = address->runIndex;
  result.global = global;
  result.canonical = position;
  result.canonical.runId = run.runId;
  result.canonical.ownerUtf8Offset = ownerOffset;
  result.canonical.globalUtf8Offset = global;
  result.canonical.graphemeOffset = graphemeOffset;
  result.canonical.graphemeBoundaryDigest = digest;
  return result;
}

std::optional<TextDocumentPosition>
PositionAtGlobal(const TextCompositionDocument &document,
                 const std::uint64_t global, const text::TextAffinity affinity) {
  const auto addresses = BuildRunAddresses(document);
  if (addresses.empty() || global > FlattenedSize(document))
    return std::nullopt;
  const RunAddress *selected = nullptr;
  for (const auto &address : addresses) {
    if (global > address.globalBegin && global < address.globalEnd) {
      selected = &address;
      break;
    }
    if (global == address.globalBegin &&
        (affinity == text::TextAffinity::Downstream || !selected)) {
      selected = &address;
      if (affinity == text::TextAffinity::Downstream)
        break;
    }
    if (global == address.globalEnd) {
      selected = &address;
      if (affinity == text::TextAffinity::Upstream)
        break;
    }
  }
  if (!selected)
    selected = &addresses.back();
  const auto &paragraph = document.content[selected->paragraphIndex];
  const auto &run = paragraph.runs[selected->runIndex];
  const auto local = std::clamp<std::uint64_t>(
      global >= selected->globalBegin ? global - selected->globalBegin : 0U,
      0U, run.utf8Text.size());
  if (!IsBoundary(run.utf8Text, local))
    return std::nullopt;
  const auto boundaries = GraphemeBoundaries(run.utf8Text);
  const auto grapheme = std::lower_bound(boundaries.begin(), boundaries.end(),
                                         static_cast<std::size_t>(local));
  TextDocumentPosition result;
  result.paragraphId = paragraph.paragraphId;
  result.runId = run.runId;
  result.ownerUtf8Offset = local;
  result.globalUtf8Offset = selected->globalBegin + local;
  result.graphemeOffset = static_cast<std::uint64_t>(
      std::distance(boundaries.begin(), grapheme));
  result.affinity = affinity;
  result.graphemeBoundaryDigest = BoundaryDigest(run.utf8Text);
  return result;
}

struct ResolvedRange final {
  bool valid{false};
  ResolvedPosition begin{};
  ResolvedPosition end{};
};

ResolvedRange ResolveRange(const TextCompositionDocument &document,
                           const TextDocumentRange &range,
                           std::vector<Diagnostic> &diagnostics) {
  auto anchor = ResolvePosition(document, range.anchor, diagnostics);
  auto focus = ResolvePosition(document, range.focus, diagnostics);
  if (!anchor.valid || !focus.valid)
    return {};
  if (focus.global < anchor.global)
    std::swap(anchor, focus);
  return {true, std::move(anchor), std::move(focus)};
}

enum class AtomKind : std::uint8_t { Text = 0, ParagraphBreak };

struct TextAtom final {
  AtomKind kind{AtomKind::Text};
  std::string utf8;
  std::string runId;
  std::string locale;
  // Borrowed from the source document or the replacement's insertion style.
  // RebuildParagraphs consumes these references before document publication.
  const text::TextStyle *style{nullptr};
  std::string nextParagraphId;
  text::ParagraphStyle nextParagraphStyle{};
  std::string nextEmptyRunId;
  std::string nextEmptyRunLocale;
  const text::TextStyle *nextEmptyRunStyle{nullptr};
};

std::vector<TextAtom> FlattenAtoms(const TextCompositionDocument &document) {
  std::vector<TextAtom> atoms;
  for (std::size_t paragraphIndex = 0U;
       paragraphIndex < document.content.size(); ++paragraphIndex) {
    const auto &paragraph = document.content[paragraphIndex];
    for (const auto &run : paragraph.runs) {
      std::vector<DecodedCodePoint> decoded;
      DecodeUtf8(run.utf8Text, decoded);
      for (const auto &codePoint : decoded) {
        TextAtom atom;
        atom.utf8 = run.utf8Text.substr(codePoint.begin,
                                       codePoint.end - codePoint.begin);
        atom.runId = run.runId;
        atom.locale = run.locale;
        atom.style = &run.style;
        atoms.push_back(std::move(atom));
      }
    }
    if (paragraphIndex + 1U < document.content.size()) {
      const auto &next = document.content[paragraphIndex + 1U];
      TextAtom lineBreak;
      lineBreak.kind = AtomKind::ParagraphBreak;
      lineBreak.utf8 = "\n";
      lineBreak.nextParagraphId = next.paragraphId;
      lineBreak.nextParagraphStyle = next.style;
      if (!next.runs.empty()) {
        lineBreak.nextEmptyRunId = next.runs.front().runId;
        lineBreak.nextEmptyRunLocale = next.runs.front().locale;
        lineBreak.nextEmptyRunStyle = &next.runs.front().style;
      }
      atoms.push_back(std::move(lineBreak));
    }
  }
  return atoms;
}

std::optional<std::size_t> AtomIndexAtOffset(const std::vector<TextAtom> &atoms,
                                             const std::uint64_t offset) {
  std::uint64_t current = 0U;
  for (std::size_t index = 0U; index < atoms.size(); ++index) {
    if (current == offset)
      return index;
    current += atoms[index].utf8.size();
    if (current > offset)
      return std::nullopt;
  }
  return current == offset ? std::optional<std::size_t>(atoms.size())
                           : std::nullopt;
}

struct IdentityCursor final {
  const TextEditIdentitySet &identities;
  std::size_t paragraphIndex{0U};
  std::size_t runIndex{0U};

  std::optional<std::string> NextParagraph() {
    if (paragraphIndex >= identities.paragraphIds.size())
      return std::nullopt;
    return identities.paragraphIds[paragraphIndex++];
  }

  std::optional<std::string> NextRun() {
    if (runIndex >= identities.runIds.size())
      return std::nullopt;
    return identities.runIds[runIndex++];
  }
};

std::optional<std::string>
ContentSlotForParagraph(const TextCompositionDocument &document,
                        const std::string &paragraphId) {
  for (const auto &slot : document.contentSlots)
    if (std::find(slot.paragraphIds.begin(), slot.paragraphIds.end(),
                  paragraphId) != slot.paragraphIds.end())
      return slot.slotId;
  return std::nullopt;
}

text::TextStyle ResolveTypingStyle(
    const TextCompositionDocument &document, const text::TextStyle &base,
    const std::optional<text::TextPropertyPatch> &typingStyle,
    std::vector<Diagnostic> &diagnostics, bool &valid) {
  valid = true;
  if (!typingStyle)
    return base;
  TextCompositionDocument scratch = document;
  scratch.durationTicks = std::max<std::int64_t>(1, scratch.durationTicks);
  scratch.content = {text::RichTextParagraph{
      "typing.paragraph", {},
      {text::RichTextRun{"typing.run", {}, "und", base}}}};
  scratch.contentSlots = {
      text::TextContentSlot{"typing.slot", "typing", {"typing.paragraph"},
                            {"typing.run"}}};
  auto patch = *typingStyle;
  patch.baseRevision = 0U;
  for (auto &assignment : patch.assignments) {
    assignment.address.target.scope = text::TextPropertyScope::Run;
    assignment.address.target.contentSlotId = "typing.slot";
    assignment.address.target.paragraphIds = {"typing.paragraph"};
    assignment.address.target.runIds = {"typing.run"};
    assignment.address.target.range.reset();
    assignment.address.target.layerId.clear();
  }
  text::TextPropertyDocumentView view{
      scratch.contentSlots, scratch.content,
      scratch.presentation.authoredLayoutFrame,
      scratch.presentation.writingMode, scratch.presentation.appearance};
  const auto applied = text::ApplyTextPropertyPatch(view, patch);
  AppendTextDiagnostics(diagnostics, applied.diagnostics);
  valid = applied.valid;
  return valid ? scratch.content.front().runs.front().style : base;
}

struct RebuildResult final {
  bool valid{false};
  std::vector<text::RichTextParagraph> paragraphs;
  std::unordered_map<std::string, std::string> paragraphSlots;
  TextEditIdentitySet changed;
};

RebuildResult RebuildParagraphs(
    const TextCompositionDocument &before, const std::vector<TextAtom> &atoms,
    IdentityCursor &identities, const std::string &insertionSlot,
    std::vector<Diagnostic> &diagnostics) {
  RebuildResult result;
  if (before.content.empty()) {
    Add(diagnostics, "text_edit.document.empty", {},
        "editing requires one canonical paragraph");
    return result;
  }
  std::unordered_map<std::string, std::string> existingSlots;
  for (const auto &slot : before.contentSlots)
    for (const auto &paragraphId : slot.paragraphIds)
      existingSlots[paragraphId] = slot.slotId;
  std::unordered_set<std::string> usedParagraphIds;
  std::unordered_set<std::string> usedRunIds;
  text::RichTextParagraph current;
  current.paragraphId = before.content.front().paragraphId;
  current.style = before.content.front().style;
  result.paragraphSlots[current.paragraphId] =
      existingSlots.count(current.paragraphId)
          ? existingSlots[current.paragraphId]
          : insertionSlot;

  auto uniqueParagraph = [&](std::string candidate)
      -> std::optional<std::string> {
    if (!candidate.empty() && usedParagraphIds.insert(candidate).second)
      return candidate;
    while (const auto replacement = identities.NextParagraph()) {
      if (!replacement->empty() &&
          usedParagraphIds.insert(*replacement).second)
        return replacement;
    }
    return std::nullopt;
  };
  auto uniqueRun = [&](std::string candidate)
      -> std::optional<std::string> {
    if (!candidate.empty() && usedRunIds.insert(candidate).second)
      return candidate;
    while (const auto replacement = identities.NextRun()) {
      if (!replacement->empty() && usedRunIds.insert(*replacement).second)
        return replacement;
    }
    return std::nullopt;
  };
  auto firstParagraph = uniqueParagraph(current.paragraphId);
  if (!firstParagraph) {
    Add(diagnostics, "text_edit.identity.paragraph_exhausted", {},
        "paragraph reconstruction requires a deterministic identity");
    return result;
  }
  current.paragraphId = *firstParagraph;

  auto flushEmptyRun = [&](const TextAtom *lineBreak) -> bool {
    if (!current.runs.empty())
      return true;
    std::string candidate;
    std::string locale{"und"};
    text::TextStyle style{};
    if (lineBreak) {
      candidate = lineBreak->nextEmptyRunId;
      locale = lineBreak->nextEmptyRunLocale.empty()
                   ? std::string{"und"}
                   : lineBreak->nextEmptyRunLocale;
      if (lineBreak->nextEmptyRunStyle)
        style = *lineBreak->nextEmptyRunStyle;
    } else if (!before.content.front().runs.empty()) {
      candidate = before.content.front().runs.front().runId;
      locale = before.content.front().runs.front().locale;
      style = before.content.front().runs.front().style;
    }
    auto identity = uniqueRun(std::move(candidate));
    if (!identity) {
      Add(diagnostics, "text_edit.identity.empty_run_exhausted",
          current.paragraphId,
          "empty paragraph requires a deterministic run identity");
      return false;
    }
    current.runs.push_back({*identity, {}, std::move(locale), std::move(style)});
    result.changed.runIds.push_back(*identity);
    return true;
  };

  const text::TextStyle *lastStyle = nullptr;
  std::optional<std::string> currentStyleDigest;
  // Each matched source represents the current run's canonical style identity.
  // Consecutive code points from that source need no additional digest work.
  const auto sameStyle = [&](const text::TextStyle &style) {
    if (lastStyle == &style)
      return true;
    if (!currentStyleDigest)
      currentStyleDigest = detail::TextStyleDigest(current.runs.back().style);
    return *currentStyleDigest == detail::TextStyleDigest(style);
  };
  auto appendText = [&](const TextAtom &atom) -> bool {
    if (!current.runs.empty() &&
        current.runs.back().runId == atom.runId &&
        current.runs.back().locale == atom.locale &&
        sameStyle(*atom.style)) {
      current.runs.back().utf8Text += atom.utf8;
      lastStyle = atom.style;
      return true;
    }
    auto identity = uniqueRun(atom.runId);
    if (!identity) {
      Add(diagnostics, "text_edit.identity.run_exhausted", atom.runId,
          "run split requires a deterministic replacement identity");
      return false;
    }
    current.runs.push_back({*identity, atom.utf8, atom.locale, *atom.style});
    lastStyle = atom.style;
    currentStyleDigest.reset();
    if (*identity != atom.runId)
      result.changed.runIds.push_back(*identity);
    return true;
  };

  const TextAtom *emptyRunPrototype = nullptr;
  for (const auto &atom : atoms) {
    if (atom.kind == AtomKind::Text) {
      if (!appendText(atom))
        return result;
      continue;
    }
    if (!flushEmptyRun(emptyRunPrototype))
      return result;
    result.paragraphs.push_back(std::move(current));
    current = {};
    lastStyle = nullptr;
    currentStyleDigest.reset();
    auto identity = uniqueParagraph(atom.nextParagraphId);
    if (!identity) {
      Add(diagnostics, "text_edit.identity.paragraph_exhausted",
          atom.nextParagraphId,
          "paragraph split requires a deterministic replacement identity");
      return result;
    }
    current.paragraphId = *identity;
    current.style = atom.nextParagraphStyle;
    const auto existing = existingSlots.find(atom.nextParagraphId);
    result.paragraphSlots[current.paragraphId] =
        existing == existingSlots.end() ? insertionSlot : existing->second;
    if (*identity != atom.nextParagraphId)
      result.changed.paragraphIds.push_back(*identity);
    emptyRunPrototype = &atom;
  }
  if (!flushEmptyRun(emptyRunPrototype))
    return result;
  result.paragraphs.push_back(std::move(current));
  result.valid = true;
  return result;
}

void RebuildContentSlots(
    TextCompositionDocument &document,
    const std::unordered_map<std::string, std::string> &paragraphSlots,
    const std::string &fallbackSlot) {
  for (auto &slot : document.contentSlots) {
    slot.paragraphIds.clear();
    slot.runIds.clear();
  }
  auto findSlot = [&](const std::string &slotId) -> text::TextContentSlot * {
    const auto found = std::find_if(
        document.contentSlots.begin(), document.contentSlots.end(),
        [&](const auto &slot) { return slot.slotId == slotId; });
    return found == document.contentSlots.end() ? nullptr : &*found;
  };
  for (const auto &paragraph : document.content) {
    const auto mapped = paragraphSlots.find(paragraph.paragraphId);
    const auto slotId = mapped == paragraphSlots.end() ? fallbackSlot
                                                        : mapped->second;
    auto *slot = findSlot(slotId);
    if (!slot && !document.contentSlots.empty())
      slot = &document.contentSlots.front();
    if (!slot)
      continue;
    slot->paragraphIds.push_back(paragraph.paragraphId);
    for (const auto &run : paragraph.runs)
      slot->runIds.push_back(run.runId);
  }
}

struct GlobalRunRange final {
  std::string paragraphId;
  std::string runId;
  std::uint64_t begin{0U};
  std::uint64_t end{0U};
};

std::vector<GlobalRunRange>
GlobalRunRanges(const TextCompositionDocument &document) {
  std::vector<GlobalRunRange> result;
  for (const auto &address : BuildRunAddresses(document)) {
    const auto &paragraph = document.content[address.paragraphIndex];
    result.push_back({paragraph.paragraphId,
                      paragraph.runs[address.runIndex].runId,
                      address.globalBegin, address.globalEnd});
  }
  return result;
}

std::optional<text::TextUtf8Range>
GlobalRange(const std::vector<GlobalRunRange> &ranges,
            const std::string &paragraphId, const std::string &runId,
            const text::TextUtf8Range local) {
  const auto found = std::find_if(
      ranges.begin(), ranges.end(), [&](const auto &range) {
        return (paragraphId.empty() || range.paragraphId == paragraphId) &&
               range.runId == runId;
      });
  if (found == ranges.end() || local.end > found->end - found->begin)
    return std::nullopt;
  return text::TextUtf8Range{found->begin + local.begin,
                             found->begin + local.end};
}

struct LocalRange final {
  std::string paragraphId;
  std::string runId;
  text::TextUtf8Range range{};
};

std::optional<LocalRange>
LocalRangeForGlobal(const std::vector<GlobalRunRange> &ranges,
                    const text::TextUtf8Range global) {
  if (global.begin >= global.end)
    return std::nullopt;
  const auto found = std::find_if(
      ranges.begin(), ranges.end(), [&](const auto &range) {
        return global.begin >= range.begin && global.end <= range.end;
      });
  if (found == ranges.end())
    return std::nullopt;
  return LocalRange{found->paragraphId, found->runId,
                    {global.begin - found->begin, global.end - found->begin}};
}

std::optional<text::TextUtf8Range>
TransformRange(const text::TextUtf8Range value, const std::uint64_t begin,
               const std::uint64_t end,
               const std::uint64_t replacementBytes,
               const bool retainReplacement) {
  if (value.end <= begin)
    return value;
  if (value.begin >= end) {
    if (replacementBytes >= end - begin) {
      const auto delta = replacementBytes - (end - begin);
      return text::TextUtf8Range{value.begin + delta, value.end + delta};
    }
    const auto delta = (end - begin) - replacementBytes;
    return text::TextUtf8Range{value.begin - delta, value.end - delta};
  }
  const auto transformLeadingEdge = [&](const std::uint64_t offset) {
    if (offset <= begin)
      return offset;
    if (offset >= end) {
      if (replacementBytes >= end - begin)
        return offset + replacementBytes - (end - begin);
      return offset - ((end - begin) - replacementBytes);
    }
    return begin;
  };
  const auto transformTrailingEdge = [&](const std::uint64_t offset) {
    if (offset <= begin)
      return offset;
    if (offset >= end) {
      if (replacementBytes >= end - begin)
        return offset + replacementBytes - (end - begin);
      return offset - ((end - begin) - replacementBytes);
    }
    return begin + (retainReplacement ? replacementBytes : 0U);
  };
  const auto transformed = text::TextUtf8Range{
      transformLeadingEdge(value.begin), transformTrailingEdge(value.end)};
  if (transformed.begin >= transformed.end)
    return std::nullopt;
  return transformed;
}

void RebaseSemanticReferences(const TextCompositionDocument &before,
                              TextCompositionDocument &after,
                              const std::uint64_t begin,
                              const std::uint64_t end,
                              const std::uint64_t replacementBytes,
                              TextEditIdentitySet &changed) {
  const auto beforeRanges = GlobalRunRanges(before);
  const auto afterRanges = GlobalRunRanges(after);
  if (before.timedText) {
    TimedTextTrack rebased;
    rebased.clock = before.timedText->clock;
    for (const auto &span : before.timedText->spans) {
      const auto global = GlobalRange(beforeRanges, span.paragraphId,
                                      span.runId, span.range);
      const auto transformed =
          global ? TransformRange(*global, begin, end, replacementBytes, true)
                 : std::nullopt;
      const auto local = transformed
                             ? LocalRangeForGlobal(afterRanges, *transformed)
                             : std::nullopt;
      if (!local) {
        changed.spanIds.push_back(span.spanId);
        continue;
      }
      auto value = span;
      value.paragraphId = local->paragraphId;
      value.runId = local->runId;
      value.range = local->range;
      rebased.spans.push_back(std::move(value));
    }
    after.timedText = rebased.spans.empty()
                          ? std::optional<TimedTextTrack>{}
                          : std::optional<TimedTextTrack>{std::move(rebased)};
  }

  for (std::size_t index = 0U;
       index < before.decorations.size() && index < after.decorations.size();
       ++index) {
    const auto &source = before.decorations[index];
    auto &target = after.decorations[index];
    if (source.target.scope != DecorationTargetScope::Utf8Range)
      continue;
    const auto global = GlobalRange(beforeRanges, source.target.paragraphId,
                                    source.target.runId, source.target.range);
    const auto transformed =
        global ? TransformRange(*global, begin, end, replacementBytes, true)
               : std::nullopt;
    const auto local = transformed
                           ? LocalRangeForGlobal(afterRanges, *transformed)
                           : std::nullopt;
    if (!local) {
      target.enabled = false;
      continue;
    }
    target.target.paragraphId = local->paragraphId;
    target.target.runId = local->runId;
    target.target.range = local->range;
  }

  std::unordered_set<std::string> paragraphIds;
  std::unordered_set<std::string> runIds;
  for (const auto &paragraph : after.content) {
    paragraphIds.insert(paragraph.paragraphId);
    for (const auto &run : paragraph.runs)
      runIds.insert(run.runId);
  }
  std::unordered_set<std::string> spanIds;
  if (after.timedText)
    for (const auto &span : after.timedText->spans)
      spanIds.insert(span.spanId);
  const auto transformOffset = [&](const std::uint64_t offset) {
    if (offset <= begin)
      return offset;
    if (offset >= end) {
      if (replacementBytes >= end - begin)
        return offset + replacementBytes - (end - begin);
      return offset - ((end - begin) - replacementBytes);
    }
    return begin + replacementBytes;
  };
  const auto mapRunOwner = [&](const std::string &runId)
      -> std::optional<std::string> {
    const auto found = std::find_if(
        beforeRanges.begin(), beforeRanges.end(),
        [&](const auto &range) { return range.runId == runId; });
    if (found == beforeRanges.end() ||
        (found->begin < found->end && found->begin >= begin &&
         found->end <= end && replacementBytes == 0U))
      return std::nullopt;
    const auto position = PositionAtGlobal(
        after, std::min(transformOffset(found->begin), FlattenedSize(after)),
        text::TextAffinity::Downstream);
    return position && position->runId ? position->runId : std::nullopt;
  };
  const auto mapParagraphOwner = [&](const std::string &paragraphId)
      -> std::optional<std::string> {
    auto first = beforeRanges.end();
    auto last = beforeRanges.end();
    for (auto iterator = beforeRanges.begin(); iterator != beforeRanges.end();
         ++iterator) {
      if (iterator->paragraphId != paragraphId)
        continue;
      if (first == beforeRanges.end())
        first = iterator;
      last = iterator;
    }
    if (first == beforeRanges.end() ||
        (first->begin < last->end && first->begin >= begin &&
         last->end <= end && replacementBytes == 0U))
      return std::nullopt;
    const auto position = PositionAtGlobal(
        after, std::min(transformOffset(first->begin), FlattenedSize(after)),
        text::TextAffinity::Downstream);
    return position ? std::optional<std::string>{position->paragraphId}
                    : std::nullopt;
  };
  const auto rebaseOwners = [](std::vector<std::string> &identities,
                               const auto &current,
                               const auto &mapping) {
    std::vector<std::string> rebased;
    rebased.reserve(identities.size());
    for (const auto &identity : identities) {
      const auto resolved = current.count(identity) != 0U
                                ? std::optional<std::string>{identity}
                                : mapping(identity);
      if (resolved &&
          std::find(rebased.begin(), rebased.end(), *resolved) == rebased.end())
        rebased.push_back(*resolved);
    }
    identities = std::move(rebased);
  };
  for (auto &layer : after.animations.layers) {
    auto &target = layer.target;
    const auto sourceLayer = std::find_if(
        before.animations.layers.begin(), before.animations.layers.end(),
        [&](const auto &candidate) { return candidate.layerId == layer.layerId; });
    const auto beforeGlobal =
        sourceLayer != before.animations.layers.end() &&
                sourceLayer->target.scope == text::TextPropertyScope::Utf8Range &&
                sourceLayer->target.range &&
                sourceLayer->target.runIds.size() == 1U
            ? GlobalRange(
                  beforeRanges,
                  sourceLayer->target.paragraphIds.empty()
                      ? std::string{}
                      : sourceLayer->target.paragraphIds.front(),
                  sourceLayer->target.runIds.front(),
                  *sourceLayer->target.range)
            : std::nullopt;
    rebaseOwners(target.paragraphIds, paragraphIds, mapParagraphOwner);
    rebaseOwners(target.runIds, runIds, mapRunOwner);
    if (sourceLayer != before.animations.layers.end() &&
        sourceLayer->target.scope == text::TextPropertyScope::Utf8Range &&
        sourceLayer->target.range) {
      const auto transformed = beforeGlobal
                                   ? TransformRange(*beforeGlobal, begin, end,
                                                    replacementBytes, true)
                                   : std::nullopt;
      const auto local = transformed
                             ? LocalRangeForGlobal(afterRanges, *transformed)
                             : std::nullopt;
      if (local) {
        target.paragraphIds = {local->paragraphId};
        target.runIds = {local->runId};
        target.range = local->range;
      } else {
        layer.enabled = false;
      }
    }
    for (auto &animator : layer.animators) {
      rebaseOwners(animator.paragraphIds, paragraphIds, mapParagraphOwner);
      rebaseOwners(animator.runIds, runIds, mapRunOwner);
    }
    if (layer.timeDriver.timedRanges) {
      auto &ids = layer.timeDriver.timedRanges->spanIds;
      ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const auto &id) {
                  return spanIds.count(id) == 0U;
                }),
                ids.end());
    }
  }
}

struct ReplacementResult final {
  bool valid{false};
  bool changed{false};
  std::uint64_t caretGlobal{0U};
  TextEditIdentitySet changedIdentities;
  std::vector<std::pair<std::string, std::string>> paragraphIdRemap;
  std::vector<std::pair<std::string, std::string>> runIdRemap;
};

void AppendChangedIdentities(TextEditIdentitySet &destination,
                             const TextEditIdentitySet &source) {
  destination.paragraphIds.insert(destination.paragraphIds.end(),
                                  source.paragraphIds.begin(),
                                  source.paragraphIds.end());
  destination.runIds.insert(destination.runIds.end(), source.runIds.begin(),
                            source.runIds.end());
  destination.spanIds.insert(destination.spanIds.end(), source.spanIds.begin(),
                             source.spanIds.end());
}

void AppendIdentityRemaps(
    std::vector<std::pair<std::string, std::string>> &destination,
    const std::vector<std::pair<std::string, std::string>> &source) {
  for (const auto &mapping : source) {
    for (auto &current : destination)
      if (current.second == mapping.first)
        current.second = mapping.second;
    const auto found = std::find_if(
        destination.begin(), destination.end(), [&](const auto &current) {
          return current.first == mapping.first;
        });
    if (found == destination.end())
      destination.push_back(mapping);
    else
      found->second = mapping.second;
  }
}

void CollectOwnerRemaps(
    const TextCompositionDocument &before,
    const TextCompositionDocument &after, const std::uint64_t begin,
    const std::uint64_t end, const std::uint64_t replacementBytes,
    std::vector<std::pair<std::string, std::string>> &paragraphRemap,
    std::vector<std::pair<std::string, std::string>> &runRemap) {
  const auto beforeRanges = GlobalRunRanges(before);
  std::unordered_set<std::string> afterParagraphIds;
  std::unordered_set<std::string> afterRunIds;
  for (const auto &paragraph : after.content) {
    afterParagraphIds.insert(paragraph.paragraphId);
    for (const auto &run : paragraph.runs)
      afterRunIds.insert(run.runId);
  }
  const auto transformOffset = [&](const std::uint64_t offset) {
    if (offset <= begin)
      return offset;
    if (offset >= end) {
      if (replacementBytes >= end - begin)
        return offset + replacementBytes - (end - begin);
      return offset - ((end - begin) - replacementBytes);
    }
    return begin + replacementBytes;
  };
  const auto mapRange = [&](const std::uint64_t ownerBegin,
                            const std::uint64_t ownerEnd)
      -> std::optional<TextDocumentPosition> {
    if (ownerBegin < ownerEnd && ownerBegin >= begin && ownerEnd <= end &&
        replacementBytes == 0U)
      return std::nullopt;
    return PositionAtGlobal(
        after, std::min(transformOffset(ownerBegin), FlattenedSize(after)),
        text::TextAffinity::Downstream);
  };
  for (const auto &paragraph : before.content) {
    if (afterParagraphIds.count(paragraph.paragraphId) != 0U)
      continue;
    auto first = beforeRanges.end();
    auto last = beforeRanges.end();
    for (auto iterator = beforeRanges.begin(); iterator != beforeRanges.end();
         ++iterator) {
      if (iterator->paragraphId != paragraph.paragraphId)
        continue;
      if (first == beforeRanges.end())
        first = iterator;
      last = iterator;
    }
    if (first == beforeRanges.end())
      continue;
    const auto mapped = mapRange(first->begin, last->end);
    if (mapped && mapped->paragraphId != paragraph.paragraphId)
      paragraphRemap.emplace_back(paragraph.paragraphId, mapped->paragraphId);
  }
  for (const auto &range : beforeRanges) {
    if (afterRunIds.count(range.runId) != 0U)
      continue;
    const auto mapped = mapRange(range.begin, range.end);
    if (mapped && mapped->runId && *mapped->runId != range.runId)
      runRemap.emplace_back(range.runId, *mapped->runId);
  }
}

ReplacementResult ReplaceGlobalRange(
    TextCompositionDocument &document, const std::uint64_t begin,
    const std::uint64_t end, const std::string &replacement,
    const TextEditIdentitySet &replacementIdentities,
    const std::optional<text::TextPropertyPatch> &typingStyle,
    std::vector<Diagnostic> &diagnostics,
    const TextDocumentPosition *insertionOwner = nullptr,
    std::vector<std::optional<text::TextPropertyTarget>> *propertyTargets = nullptr) {
  ReplacementResult result;
  if (begin > end || end > FlattenedSize(document) ||
      !text::IsValidUtf8(replacement) ||
      replacement.find('\r') != std::string::npos) {
    Add(diagnostics, "text_edit.replacement.invalid", {},
        "replacement range and UTF-8 payload must be canonical");
    return result;
  }
  auto atoms = FlattenAtoms(document);
  const auto beginIndex = AtomIndexAtOffset(atoms, begin);
  const auto endIndex = AtomIndexAtOffset(atoms, end);
  if (!beginIndex || !endIndex) {
    Add(diagnostics, "text_edit.replacement.boundary_invalid", {},
        "replacement endpoints must lie on canonical grapheme boundaries");
    return result;
  }
  const auto position = insertionOwner
                            ? std::optional<TextDocumentPosition>{
                                  *insertionOwner}
                            : PositionAtGlobal(document, begin,
                                               text::TextAffinity::Upstream);
  if (!position || !position->runId) {
    Add(diagnostics, "text_edit.replacement.style_unavailable", {},
        "replacement insertion style cannot be resolved");
    return result;
  }
  const auto paragraph = std::find_if(
      document.content.begin(), document.content.end(), [&](const auto &value) {
        return value.paragraphId == position->paragraphId;
      });
  const auto run = paragraph == document.content.end()
                       ? std::vector<text::RichTextRun>::const_iterator{}
                       : std::find_if(
                             paragraph->runs.begin(), paragraph->runs.end(),
                             [&](const auto &value) {
                               return value.runId == *position->runId;
                             });
  if (paragraph == document.content.end() || run == paragraph->runs.end())
    return result;
  bool typingValid = false;
  const auto insertionStyle = ResolveTypingStyle(
      document, run->style, typingStyle, diagnostics, typingValid);
  if (!typingValid)
    return result;
  const auto insertionSlot =
      ContentSlotForParagraph(document, paragraph->paragraphId)
          .value_or(document.contentSlots.empty()
                        ? std::string{}
                        : document.contentSlots.front().slotId);

  IdentityCursor identities{replacementIdentities};
  std::vector<TextAtom> inserted;
  std::vector<DecodedCodePoint> decoded;
  DecodeUtf8(replacement, decoded);
  std::string insertionRunId = run->runId;
  if (detail::TextStyleDigest(insertionStyle) !=
      detail::TextStyleDigest(run->style)) {
    const auto next = identities.NextRun();
    if (!next || next->empty()) {
      Add(diagnostics, "text_edit.identity.styled_run_required", run->runId,
          "typing-style insertion requires a deterministic run identity");
      return result;
    }
    insertionRunId = *next;
  }
  for (const auto &codePoint : decoded) {
    const auto bytes = replacement.substr(codePoint.begin,
                                          codePoint.end - codePoint.begin);
    if (bytes != "\n") {
      TextAtom atom;
      atom.utf8 = bytes;
      atom.runId = insertionRunId;
      atom.locale = run->locale;
      atom.style = &insertionStyle;
      inserted.push_back(std::move(atom));
      continue;
    }
    const auto paragraphId = identities.NextParagraph();
    const auto trailingRunId = identities.NextRun();
    if (!paragraphId || paragraphId->empty() || !trailingRunId ||
        trailingRunId->empty()) {
      Add(diagnostics, "text_edit.identity.paragraph_split_required", {},
          "each inserted paragraph separator requires paragraph and run IDs");
      return result;
    }
    TextAtom lineBreak;
    lineBreak.kind = AtomKind::ParagraphBreak;
    lineBreak.utf8 = "\n";
    lineBreak.nextParagraphId = *paragraphId;
    lineBreak.nextParagraphStyle = paragraph->style;
    lineBreak.nextEmptyRunId = *trailingRunId;
    lineBreak.nextEmptyRunLocale = run->locale;
    lineBreak.nextEmptyRunStyle = &insertionStyle;
    inserted.push_back(std::move(lineBreak));
    insertionRunId = *trailingRunId;
  }

  std::vector<TextAtom> replaced;
  replaced.reserve(atoms.size() - (*endIndex - *beginIndex) + inserted.size());
  replaced.insert(replaced.end(), atoms.begin(), atoms.begin() + *beginIndex);
  replaced.insert(replaced.end(), inserted.begin(), inserted.end());
  replaced.insert(replaced.end(), atoms.begin() + *endIndex, atoms.end());
  const auto before = document;
  auto rebuilt = RebuildParagraphs(before, replaced, identities, insertionSlot,
                                   diagnostics);
  if (!rebuilt.valid)
    return result;
  document.content = std::move(rebuilt.paragraphs);
  RebuildContentSlots(document, rebuilt.paragraphSlots, insertionSlot);
  RebaseSemanticReferences(before, document, begin, end, replacement.size(),
                           rebuilt.changed);
  const auto validation = ValidateTextCompositionDocument(document);
  if (!validation.valid) {
    document = before;
    diagnostics.insert(diagnostics.end(), validation.diagnostics.begin(),
                       validation.diagnostics.end());
    return result;
  }
  result.valid = true;
  result.changed = begin != end || !replacement.empty();
  result.caretGlobal = begin + replacement.size();
  result.changedIdentities = std::move(rebuilt.changed);
  CollectOwnerRemaps(before, document, begin, end, replacement.size(),
                     result.paragraphIdRemap, result.runIdRemap);
  if (propertyTargets && !propertyTargets->empty()) {
    const auto beforeRanges = GlobalRunRanges(before);
    const auto afterRanges = GlobalRunRanges(document);
    for (auto &target : *propertyTargets) {
      if (!target) continue;
      if (!target->range) {
        if (target->runIds.empty() && target->paragraphIds.empty()) continue;
        const auto selectedBefore = text::ResolveTextPropertyRunIds(
            *target, before.contentSlots, before.content);
        const auto remap = [](auto &ids, const auto &mappings) {
          for (auto &id : ids) {
            const auto found = std::find_if(mappings.begin(), mappings.end(),
                [&](const auto &mapping) { return mapping.first == id; });
            if (found != mappings.end()) id = found->second;
          }
          std::unordered_set<std::string> seen;
          ids.erase(std::remove_if(ids.begin(), ids.end(),
              [&](const auto &id) { return !seen.insert(id).second; }), ids.end());
        };
        remap(target->runIds, result.runIdRemap);
        remap(target->paragraphIds, result.paragraphIdRemap);
        const auto selectedAfter = text::ResolveTextPropertyRunIds(
            *target, document.contentSlots, document.content);
        std::vector<text::TextUtf8Range> expected;
        std::vector<text::TextUtf8Range> actual;
        for (const auto &range : beforeRanges) {
          if (selectedBefore.count(range.runId) == 0U) continue;
          const bool insertsIntoOwner = begin == end && insertionOwner &&
              insertionOwner->runId == range.runId;
          const auto transformed = insertsIntoOwner
              ? std::optional<text::TextUtf8Range>{{range.begin,
                                                   range.end + replacement.size()}}
              : TransformRange({range.begin, range.end}, begin, end,
                               replacement.size(), true);
          if (transformed && transformed->begin < transformed->end)
            expected.push_back(*transformed);
        }
        for (const auto &range : afterRanges)
          if (selectedAfter.count(range.runId) != 0U && range.begin < range.end)
            actual.push_back({range.begin, range.end});
        const auto normalize = [](const auto &ranges) {
          std::vector<text::TextUtf8Range> result;
          for (const auto &range : ranges) {
            if (!result.empty() && range.begin <= result.back().end)
              result.back().end = std::max(result.back().end, range.end);
            else result.push_back(range);
          }
          return result;
        };
        if (normalize(expected) != normalize(actual)) {
          document = before;
          result.valid = false;
          Add(diagnostics, "text_edit.automation_owner_scope_changed", {},
              "text edit cannot preserve the animated owner's exact text scope");
          return result;
        }
        continue;
      }
      const auto global = target->runIds.size() == 1U
          ? GlobalRange(beforeRanges, {}, target->runIds.front(), *target->range)
          : std::nullopt;
      if (!global) continue;
      const auto transformed =
          TransformRange(*global, begin, end, replacement.size(), true);
      const auto local = transformed
          ? LocalRangeForGlobal(afterRanges, *transformed)
          : std::nullopt;
      if (!local) {
        if (transformed) {
          document = before;
          result.valid = false;
          Add(diagnostics, "text_edit.automation_range_split", target->runIds.front(),
              "text edit would split an animated range across run owners");
          return result;
        }
        target.reset();
        continue;
      }
      target->runIds = {local->runId};
      if (!target->paragraphIds.empty()) target->paragraphIds = {local->paragraphId};
      target->range = local->range;
    }
  }
  return result;
}

void CollectChangedIdentities(const TextCompositionDocument &before,
                              const TextCompositionDocument &after,
                              TextEditIdentitySet &changed) {
  std::unordered_map<std::string, std::string> beforeParagraphs;
  std::unordered_map<std::string, std::string> afterParagraphs;
  std::unordered_map<std::string, std::string> beforeRuns;
  std::unordered_map<std::string, std::string> afterRuns;
  const auto paragraphIdentity = [](const text::RichTextParagraph &paragraph) {
    return detail::MakeIdentity("videocut.text-edit.paragraph", [&](auto &writer) {
      detail::Encode(writer, paragraph);
    });
  };
  const auto runIdentity = [](const text::RichTextRun &run) {
    return detail::MakeIdentity("videocut.text-edit.run", [&](auto &writer) {
      detail::Encode(writer, run);
    });
  };
  for (const auto &paragraph : before.content) {
    beforeParagraphs.emplace(paragraph.paragraphId,
                             paragraphIdentity(paragraph));
    for (const auto &run : paragraph.runs)
      beforeRuns.emplace(run.runId, runIdentity(run));
  }
  for (const auto &paragraph : after.content) {
    afterParagraphs.emplace(paragraph.paragraphId,
                            paragraphIdentity(paragraph));
    for (const auto &run : paragraph.runs)
      afterRuns.emplace(run.runId, runIdentity(run));
  }
  for (const auto &[id, identity] : beforeParagraphs) {
    const auto found = afterParagraphs.find(id);
    if (found == afterParagraphs.end() || found->second != identity)
      changed.paragraphIds.push_back(id);
  }
  for (const auto &[id, identity] : afterParagraphs) {
    const auto found = beforeParagraphs.find(id);
    if (found == beforeParagraphs.end() || found->second != identity)
      changed.paragraphIds.push_back(id);
  }
  for (const auto &[id, identity] : beforeRuns) {
    const auto found = afterRuns.find(id);
    if (found == afterRuns.end() || found->second != identity)
      changed.runIds.push_back(id);
  }
  for (const auto &[id, identity] : afterRuns) {
    const auto found = beforeRuns.find(id);
    if (found == beforeRuns.end() || found->second != identity)
      changed.runIds.push_back(id);
  }
  const auto spanIdentities = [](const TextCompositionDocument &document) {
    std::unordered_map<std::string, std::string> result;
    if (!document.timedText)
      return result;
    for (const auto &span : document.timedText->spans) {
      result.emplace(
          span.spanId,
          detail::MakeIdentity("videocut.text-edit.timed-span",
                               [&](auto &writer) {
                                 writer.String(span.spanId);
                                 writer.String(span.paragraphId);
                                 writer.String(span.runId);
                                 detail::Encode(writer, span.range);
                                 writer.Signed(span.startOffsetUs);
                                 writer.Signed(span.endOffsetUs);
                                 writer.String(span.semantic);
                                 writer.Enumeration(span.progressMode);
                                 writer.Signed(span.transitionEndOffsetUs);
                               }));
    }
    return result;
  };
  const auto beforeSpans = spanIdentities(before);
  const auto afterSpans = spanIdentities(after);
  for (const auto &[id, identity] : beforeSpans) {
    const auto found = afterSpans.find(id);
    if (found == afterSpans.end() || found->second != identity)
      changed.spanIds.push_back(id);
  }
  for (const auto &[id, identity] : afterSpans) {
    const auto found = beforeSpans.find(id);
    if (found == beforeSpans.end() || found->second != identity)
      changed.spanIds.push_back(id);
  }
  const auto unique = [](std::vector<std::string> &values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
  };
  unique(changed.paragraphIds);
  unique(changed.runIds);
  unique(changed.spanIds);
}

TextEditReceipt MakeReceipt(const TextCompositionDocument &document,
                            const TextEditTransaction &transaction,
                            const TextEditIdentitySet &changed,
                            const std::optional<TextDocumentRange> &selection,
                            const std::optional<TextDocumentRange> &composing,
                            const std::vector<std::pair<std::string, std::string>>
                                &paragraphRemap = {},
                            const std::vector<std::pair<std::string, std::string>>
                                &runRemap = {}) {
  TextEditReceipt receipt;
  receipt.documentRevision = transaction.baseRevision + 1U;
  receipt.textDigest = detail::TextContentDigest(document);
  receipt.canonicalSelection = selection;
  receipt.canonicalComposingRange = composing;
  receipt.changedIdentities = changed;
  receipt.paragraphIdRemap = paragraphRemap;
  receipt.runIdRemap = runRemap;
  return receipt;
}

struct TransactionState final {
  std::optional<text::TextPropertyPatch> typingStyle;
  std::optional<TextDocumentRange> selection;
  std::optional<TextDocumentRange> composing;
  std::vector<std::optional<text::TextPropertyTarget>> propertyTargets;
};

bool RebaseStyleTargets(
    const TextCompositionDocument &before,
    const TextCompositionDocument &after,
    std::vector<std::optional<text::TextPropertyTarget>> &targets,
    std::vector<Diagnostic> &diagnostics) {
  const auto oldRuns = GlobalRunRanges(before);
  const auto newRuns = GlobalRunRanges(after);
  for (auto &target : targets) {
    if (!target || target->runIds.empty()) continue;
    if (target->range) {
      const auto global = target->runIds.size() == 1U
          ? GlobalRange(oldRuns, {}, target->runIds.front(), *target->range)
          : std::nullopt;
      if (!global) continue;
      const auto local = LocalRangeForGlobal(newRuns, *global);
      if (!local) {
        Add(diagnostics, "text_edit.automation_range_split", target->runIds.front(),
            "style edit would split an animated range across run owners");
        return false;
      }
      target->runIds = {local->runId};
      if (!target->paragraphIds.empty()) target->paragraphIds = {local->paragraphId};
      target->range = local->range;
      continue;
    }
    std::vector<std::string> mappedIds;
    for (const auto &id : target->runIds) {
      const auto old = std::find_if(oldRuns.begin(), oldRuns.end(),
          [&](const auto &run) { return run.runId == id; });
      if (old == oldRuns.end()) {
        mappedIds.push_back(id);
        continue;
      }
      for (const auto &run : newRuns)
        if (run.paragraphId == old->paragraphId &&
            run.begin >= old->begin && run.end <= old->end &&
            (run.begin < run.end || run.runId == old->runId))
          mappedIds.push_back(run.runId);
    }
    target->runIds = std::move(mappedIds);
  }
  return true;
}

TextEditResult ApplyTransaction(
    TextCompositionDocument &document, const TextEditTransaction &transaction,
    TransactionState &state, const TextCompositionLimits &limits) {
  TextEditResult result;
  if (transaction.sessionId.empty()) {
    Add(result.diagnostics, "text_edit.session_id_invalid", {},
        "transaction requires a non-empty session identity");
    return result;
  }
  const auto before = document;
  const auto beforeDigest = detail::TextContentDigest(document);
  if (!transaction.beforeDigest.empty() &&
      transaction.beforeDigest != beforeDigest) {
    Add(result.diagnostics, "text_edit.before_digest_stale",
        transaction.sessionId,
        "transaction text digest does not match current canonical content");
    return result;
  }
  auto operations = transaction.operations;
  if (transaction.input) {
    const auto &input = *transaction.input;
    if (!operations.empty() || transaction.selectionAfter ||
        transaction.composingAfter || transaction.beforeDigest.empty() ||
        !text::IsValidUtf8(input.utf8Text) ||
        input.utf8Text.find('\r') != std::string::npos) {
      Add(result.diagnostics, "text_edit.input.invalid", {},
          "input requires a digest fence and cannot mix canonical operations");
      return result;
    }
    std::string source;
    for (const auto &paragraph : document.content) {
      if (&paragraph != &document.content.front()) source += '\n';
      for (const auto &run : paragraph.runs) source += run.utf8Text;
    }
    if (source != input.utf8Text) {
      const auto &target = input.utf8Text;
      const auto sourceBoundaries = GraphemeBoundaries(source);
      const auto targetBoundaries = GraphemeBoundaries(target);
      const auto boundary = [](const auto &boundaries, std::size_t offset) {
        return std::binary_search(boundaries.begin(), boundaries.end(), offset);
      };
      std::size_t caretSuffix = 0;
      if (input.selection.anchor == input.selection.focus &&
          input.selection.focus <= target.size()) {
        const auto tail = target.size() - input.selection.focus;
        if (tail <= source.size() &&
            source.compare(source.size() - tail, tail, target,
                           target.size() - tail, tail) == 0 &&
            boundary(sourceBoundaries, source.size() - tail) &&
            boundary(targetBoundaries, target.size() - tail))
          caretSuffix = tail;
      }
      std::size_t begin = 0;
      while (begin < source.size() - caretSuffix &&
             begin < target.size() - caretSuffix &&
             source[begin] == target[begin]) ++begin;
      while (begin && (!boundary(sourceBoundaries, begin) ||
                       !boundary(targetBoundaries, begin))) --begin;
      std::size_t suffix = caretSuffix;
      while (suffix < source.size() - begin &&
             suffix < target.size() - begin &&
             source[source.size() - suffix - 1] ==
                 target[target.size() - suffix - 1]) ++suffix;
      while (suffix &&
             (!boundary(sourceBoundaries, source.size() - suffix) ||
              !boundary(targetBoundaries, target.size() - suffix))) --suffix;
      const auto anchor = PositionAtGlobal(document, begin,
          begin == source.size() - suffix ? text::TextAffinity::Upstream
                                         : text::TextAffinity::Downstream);
      const auto focus = PositionAtGlobal(document, source.size() - suffix,
                                          text::TextAffinity::Downstream);
      if (!anchor || !focus) {
        Add(result.diagnostics, "text_edit.input.boundary_invalid", {},
            "input replacement must resolve to canonical run boundaries");
        return result;
      }
      auto replacement = target.substr(begin, target.size() - suffix - begin);
      TextEditIdentitySet identities;
      const auto identity = "text-input-" + BoundaryDigest(
          anchor->paragraphId + ":" + transaction.sessionId + ":" +
          std::to_string(transaction.sequence));
      const auto paragraphCount = static_cast<std::size_t>(
          std::count(replacement.begin(), replacement.end(), '\n'));
      for (std::size_t index = 0; index < paragraphCount; ++index)
        identities.paragraphIds.push_back(identity + "-p" + std::to_string(index));
      // A split can also retain the old run on either side of the insertion.
      for (std::size_t index = 0; index < paragraphCount + 2; ++index)
        identities.runIds.push_back(identity + "-r" + std::to_string(index));
      operations.emplace_back(ReplaceTextEdit{
          {*anchor, *focus}, std::move(replacement), std::move(identities)});
    }
  }
  TextEditIdentitySet changed;
  std::vector<std::pair<std::string, std::string>> paragraphRemap;
  std::vector<std::pair<std::string, std::string>> runRemap;
  std::optional<std::uint64_t> implicitCaret;
  for (const auto &operation : operations) {
    bool operationValid = true;
    std::visit(
        [&](const auto &typed) {
          using Operation = std::decay_t<decltype(typed)>;
          if constexpr (std::is_same_v<Operation, InsertTextEdit>) {
            const auto position =
                ResolvePosition(document, typed.position, result.diagnostics);
            if (!position.valid) {
              operationValid = false;
              return;
            }
            auto replacement = ReplaceGlobalRange(
                document, position.global, position.global, typed.utf8Text,
                typed.replacementIdentities, state.typingStyle,
                result.diagnostics, &position.canonical, &state.propertyTargets);
            operationValid = replacement.valid;
            if (replacement.valid) {
              implicitCaret = replacement.caretGlobal;
              AppendChangedIdentities(changed,
                                      replacement.changedIdentities);
              AppendIdentityRemaps(paragraphRemap,
                                   replacement.paragraphIdRemap);
              AppendIdentityRemaps(runRemap, replacement.runIdRemap);
            }
          } else if constexpr (std::is_same_v<Operation, DeleteTextEdit>) {
            const auto range =
                ResolveRange(document, typed.range, result.diagnostics);
            if (!range.valid) {
              operationValid = false;
              return;
            }
            auto replacement = ReplaceGlobalRange(
                document, range.begin.global, range.end.global, {}, {},
                state.typingStyle, result.diagnostics,
                &range.begin.canonical, &state.propertyTargets);
            operationValid = replacement.valid;
            if (replacement.valid) {
              implicitCaret = replacement.caretGlobal;
              AppendChangedIdentities(changed,
                                      replacement.changedIdentities);
              AppendIdentityRemaps(paragraphRemap,
                                   replacement.paragraphIdRemap);
              AppendIdentityRemaps(runRemap, replacement.runIdRemap);
            }
          } else if constexpr (std::is_same_v<Operation, ReplaceTextEdit>) {
            const auto range =
                ResolveRange(document, typed.range, result.diagnostics);
            if (!range.valid) {
              operationValid = false;
              return;
            }
            auto replacement = ReplaceGlobalRange(
                document, range.begin.global, range.end.global, typed.utf8Text,
                typed.replacementIdentities, state.typingStyle,
                result.diagnostics, &range.begin.canonical, &state.propertyTargets);
            operationValid = replacement.valid;
            if (replacement.valid) {
              implicitCaret = replacement.caretGlobal;
              AppendChangedIdentities(changed,
                                      replacement.changedIdentities);
              AppendIdentityRemaps(paragraphRemap,
                                   replacement.paragraphIdRemap);
              AppendIdentityRemaps(runRemap, replacement.runIdRemap);
            }
          } else if constexpr (std::is_same_v<Operation,
                                              SplitParagraphEdit>) {
            const auto position =
                ResolvePosition(document, typed.position, result.diagnostics);
            if (!position.valid) {
              operationValid = false;
              return;
            }
            TextEditIdentitySet identities;
            identities.paragraphIds = {typed.newParagraphId};
            identities.runIds = {typed.trailingRunId};
            auto replacement = ReplaceGlobalRange(
                document, position.global, position.global, "\n", identities,
                state.typingStyle, result.diagnostics,
                &position.canonical, &state.propertyTargets);
            operationValid = replacement.valid;
            if (replacement.valid) {
              implicitCaret = replacement.caretGlobal;
              AppendChangedIdentities(changed,
                                      replacement.changedIdentities);
              AppendIdentityRemaps(paragraphRemap,
                                   replacement.paragraphIdRemap);
              AppendIdentityRemaps(runRemap, replacement.runIdRemap);
            }
          } else if constexpr (std::is_same_v<Operation,
                                              MergeParagraphEdit>) {
            const auto leading = std::find_if(
                document.content.begin(), document.content.end(),
                [&](const auto &paragraph) {
                  return paragraph.paragraphId == typed.leadingParagraphId;
                });
            const auto trailing = std::find_if(
                document.content.begin(), document.content.end(),
                [&](const auto &paragraph) {
                  return paragraph.paragraphId == typed.trailingParagraphId;
                });
            if (leading == document.content.end() ||
                trailing == document.content.end() || trailing != leading + 1) {
              Add(result.diagnostics, "text_edit.merge.non_adjacent",
                  typed.trailingParagraphId,
                  "paragraph merge requires adjacent current identities");
              operationValid = false;
              return;
            }
            std::uint64_t boundary = 0U;
            for (auto iterator = document.content.begin();
                 iterator <= leading; ++iterator) {
              for (const auto &run : iterator->runs)
                boundary += run.utf8Text.size();
              if (iterator != leading)
                ++boundary;
            }
            auto replacement = ReplaceGlobalRange(
                document, boundary, boundary + 1U, {}, {}, state.typingStyle,
                result.diagnostics, nullptr, &state.propertyTargets);
            operationValid = replacement.valid;
            if (replacement.valid) {
              implicitCaret = replacement.caretGlobal;
              AppendChangedIdentities(changed,
                                      replacement.changedIdentities);
              AppendIdentityRemaps(paragraphRemap,
                                   replacement.paragraphIdRemap);
              AppendIdentityRemaps(runRemap, replacement.runIdRemap);
              changed.paragraphIds.push_back(typed.trailingParagraphId);
            }
          } else if constexpr (std::is_same_v<Operation,
                                              ApplyTextPropertyEdit>) {
            if (typed.typingStyle) {
              const auto validation =
                  text::ValidateTextPropertyPatch(typed.patch, limits.richText);
              AppendTextDiagnostics(result.diagnostics,
                                    validation.diagnostics);
              operationValid = validation.valid;
              if (operationValid)
                state.typingStyle = typed.patch;
              return;
            }
            const auto beforeStyle = state.propertyTargets.empty()
                ? std::optional<TextCompositionDocument>{}
                : std::optional<TextCompositionDocument>{document};
            text::TextPropertyDocumentView view{
                document.contentSlots, document.content,
                document.presentation.authoredLayoutFrame,
                document.presentation.writingMode,
                document.presentation.appearance};
            const auto applied =
                text::ApplyTextPropertyPatch(view, typed.patch, limits.richText);
            AppendTextDiagnostics(result.diagnostics, applied.diagnostics);
            operationValid = applied.valid;
            if (operationValid && beforeStyle)
              operationValid = RebaseStyleTargets(*beforeStyle, document,
                  state.propertyTargets, result.diagnostics);
            if (operationValid) {
              changed.paragraphIds.insert(changed.paragraphIds.end(),
                                          applied.changedParagraphIds.begin(),
                                          applied.changedParagraphIds.end());
              changed.runIds.insert(changed.runIds.end(),
                                    applied.changedRunIds.begin(),
                                    applied.changedRunIds.end());
            }
          } else if constexpr (std::is_same_v<Operation,
                                              SetTextSelectionEdit>) {
            const auto range =
                ResolveRange(document, typed.selection, result.diagnostics);
            operationValid = range.valid;
            if (range.valid)
              state.selection = TextDocumentRange{range.begin.canonical,
                                                  range.end.canonical};
          } else if constexpr (std::is_same_v<Operation,
                                              SetTextComposingRangeEdit>) {
            if (!typed.composing) {
              state.composing.reset();
              return;
            }
            const auto range =
                ResolveRange(document, *typed.composing, result.diagnostics);
            operationValid = range.valid;
            if (range.valid)
              state.composing = TextDocumentRange{range.begin.canonical,
                                                  range.end.canonical};
          }
        },
        operation);
    if (!operationValid) {
      document = before;
      return result;
    }
  }
  if (transaction.input) {
    const auto resolveInputRange = [&](const TextInputRange &range)
        -> std::optional<TextDocumentRange> {
      auto anchor = PositionAtGlobal(document, range.anchor,
                                      text::TextAffinity::Downstream);
      auto focus = PositionAtGlobal(document, range.focus,
                                     text::TextAffinity::Downstream);
      if (!anchor || !focus || anchor->globalUtf8Offset != range.anchor ||
          focus->globalUtf8Offset != range.focus) return std::nullopt;
      return TextDocumentRange{*anchor, *focus};
    };
    state.selection = resolveInputRange(transaction.input->selection);
    state.composing = transaction.input->composing
        ? resolveInputRange(*transaction.input->composing) : std::nullopt;
    if (!state.selection || (transaction.input->composing && !state.composing)) {
      document = before;
      Add(result.diagnostics, "text_edit.input.selection_invalid", {},
          "input selection must resolve in the resulting canonical document");
      return result;
    }
  } else if (transaction.selectionAfter) {
    const auto resolved = ResolveRange(document, *transaction.selectionAfter,
                                       result.diagnostics);
    if (!resolved.valid) {
      document = before;
      return result;
    }
    state.selection =
        TextDocumentRange{resolved.begin.canonical, resolved.end.canonical};
  } else if (implicitCaret) {
    const auto caret = PositionAtGlobal(document, *implicitCaret,
                                        text::TextAffinity::Downstream);
    if (caret)
      state.selection = TextDocumentRange{*caret, *caret};
  }
  if (transaction.composingAfter) {
    const auto resolved = ResolveRange(document, *transaction.composingAfter,
                                       result.diagnostics);
    if (!resolved.valid) {
      document = before;
      return result;
    }
    state.composing =
        TextDocumentRange{resolved.begin.canonical, resolved.end.canonical};
  }
  const auto validation = ValidateTextCompositionDocument(document, limits);
  if (!validation.valid) {
    document = before;
    result.diagnostics.insert(result.diagnostics.end(),
                              validation.diagnostics.begin(),
                              validation.diagnostics.end());
    return result;
  }
  CollectChangedIdentities(before, document, changed);
  result.valid = true;
  result.changed = ComputeTextCompositionDocumentIdentity(before) !=
                   ComputeTextCompositionDocumentIdentity(document);
  result.receipt =
      MakeReceipt(document, transaction, changed, state.selection,
                  state.composing, paragraphRemap, runRemap);
  return result;
}

std::optional<text::TextMaterial>
MaterialFromBinding(const text::TextMaterialBinding &binding) {
  if (const auto *literal = std::get_if<text::LiteralTextMaterial>(&binding))
    return literal->material;
  if (const auto *slot = std::get_if<text::EditableTextStyleSlot>(&binding))
    return slot->fallback;
  return std::nullopt;
}

text::Color RepresentativeMaterialColor(
    const text::TextMaterial &material) noexcept {
  return std::visit(
      [](const auto &value) -> text::Color {
        using Material = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Material, text::SolidTextMaterial>) {
          return value.color;
        } else if constexpr (
            std::is_same_v<Material, text::LinearGradientTextMaterial> ||
            std::is_same_v<Material, text::RadialGradientTextMaterial>) {
          return value.stops.empty()
                     ? text::Color{1.0F, 1.0F, 1.0F, 1.0F}
                     : value.stops.front().color;
        } else {
          return value.underlayColor.value_or(
              text::Color{1.0F, 1.0F, 1.0F, value.opacity});
        }
      },
      material);
}

float MaterialOpacity(const text::TextMaterial &material) noexcept {
  return std::visit(
      [](const auto &value) -> float {
        using Material = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Material, text::SolidTextMaterial>) {
          return value.color.alpha;
        } else if constexpr (
            std::is_same_v<Material, text::LinearGradientTextMaterial> ||
            std::is_same_v<Material, text::RadialGradientTextMaterial>) {
          return value.stops.empty() ? 1.0F : value.stops.front().color.alpha;
        } else {
          return value.opacity;
        }
      },
      material);
}

const text::TextGlyphMaterialLayer *FindGlyphLayer(
    const text::RichTextRun &run, const std::string &layerId) noexcept {
  const auto found = std::find_if(
      run.style.materials.layers.begin(), run.style.materials.layers.end(),
      [&](const auto &layer) {
        return std::visit(
            [&](const auto &typed) {
              return layerId.empty() || typed.layerId == layerId;
            },
            layer);
      });
  return found == run.style.materials.layers.end() ? nullptr : &*found;
}

template <typename Layer>
const Layer *FindGlyphLayer(const text::RichTextRun &run,
                            const std::string &layerId) noexcept {
  for (const auto &layer : run.style.materials.layers) {
    const auto *typed = std::get_if<Layer>(&layer);
    if (typed && (layerId.empty() || typed->layerId == layerId))
      return typed;
  }
  return nullptr;
}

std::optional<text::TextMaterial> GlyphLayerMaterial(
    const text::TextGlyphMaterialLayer *layer) {
  if (!layer)
    return std::nullopt;
  return std::visit(
      [](const auto &typed) { return MaterialFromBinding(typed.material); },
      *layer);
}

std::vector<const text::RichTextRun *>
SelectedRuns(const TextCompositionDocument &document,
             const text::TextPropertyTarget &target) {
  const auto runIds = text::ResolveTextPropertyRunIds(
      target, document.contentSlots, document.content);
  std::vector<const text::RichTextRun *> result;
  for (const auto &paragraph : document.content) {
    for (const auto &run : paragraph.runs) {
      if (runIds.count(run.runId) != 0U &&
          (!target.range || text::TextPropertyRangeMatches(run.utf8Text, *target.range)))
        result.push_back(&run);
    }
  }
  return result;
}

std::vector<const text::RichTextParagraph *>
SelectedParagraphs(const TextCompositionDocument &document,
                   const text::TextPropertyTarget &target) {
  const auto ids = text::ResolveTextPropertyParagraphIds(
      target, document.contentSlots, document.content);
  std::vector<const text::RichTextParagraph *> result;
  for (const auto &paragraph : document.content)
    if (ids.count(paragraph.paragraphId) != 0U)
      result.push_back(&paragraph);
  return result;
}

std::optional<text::TextPropertyValue>
RunProperty(const text::RichTextRun &run, const text::TextPropertyId property,
            const text::TextPropertyTarget &target) {
  switch (property) {
  case text::TextPropertyId::FontReference:
    return run.style.font.primary;
  case text::TextPropertyId::FontFamily:
    return run.style.font.family;
  case text::TextPropertyId::FontPostscriptName:
    return run.style.font.postscriptName;
  case text::TextPropertyId::FontWeight:
    return static_cast<std::int64_t>(run.style.font.weight);
  case text::TextPropertyId::FontWidth:
    return static_cast<std::int64_t>(run.style.font.width);
  case text::TextPropertyId::FontSlant:
    return static_cast<std::int64_t>(run.style.font.slant);
  case text::TextPropertyId::FontVariationAxes:
    return run.style.font.variationAxes;
  case text::TextPropertyId::FontFeatures:
    return run.style.font.features;
  case text::TextPropertyId::FontSize:
    return static_cast<double>(run.style.fontSize);
  case text::TextPropertyId::LetterSpacing:
    return static_cast<double>(run.style.letterSpacing);
  case text::TextPropertyId::WordSpacing:
    return static_cast<double>(run.style.wordSpacing);
  case text::TextPropertyId::BaselineShift:
    return static_cast<double>(run.style.baselineShift);
  case text::TextPropertyId::UnderlineEnabled:
    return run.style.decoration.underline.enabled;
  case text::TextPropertyId::UnderlineStyle:
    return static_cast<std::int64_t>(run.style.decoration.underline.style);
  case text::TextPropertyId::UnderlineMaterial:
    return MaterialFromBinding(run.style.decoration.underline.material);
  case text::TextPropertyId::UnderlineThickness:
    return static_cast<double>(run.style.decoration.underline.thickness);
  case text::TextPropertyId::UnderlineOffset:
    return static_cast<double>(run.style.decoration.underline.offset);
  case text::TextPropertyId::UnderlineSkipInk:
    return run.style.decoration.underline.skipInk;
  case text::TextPropertyId::StrikeThroughEnabled:
    return run.style.decoration.strikeThrough.enabled;
  case text::TextPropertyId::StrikeThroughStyle:
    return static_cast<std::int64_t>(run.style.decoration.strikeThrough.style);
  case text::TextPropertyId::StrikeThroughMaterial:
    return MaterialFromBinding(run.style.decoration.strikeThrough.material);
  case text::TextPropertyId::StrikeThroughThickness:
    return static_cast<double>(run.style.decoration.strikeThrough.thickness);
  case text::TextPropertyId::StrikeThroughOffset:
    return static_cast<double>(run.style.decoration.strikeThrough.offset);
  case text::TextPropertyId::GlyphMaterialStack:
  case text::TextPropertyId::GlyphDecorativeMaterialStack:
    return run.style.materials;
  case text::TextPropertyId::GlyphFill:
  case text::TextPropertyId::GlyphFillColor:
  case text::TextPropertyId::GlyphFillOpacity:
  case text::TextPropertyId::GlyphFillGradientStartColor:
  case text::TextPropertyId::GlyphFillGradientEndColor:
  case text::TextPropertyId::GlyphFillGradientStartOffset:
  case text::TextPropertyId::GlyphFillGradientEndOffset:
  case text::TextPropertyId::GlyphFillGradientAngle:
  case text::TextPropertyId::GlyphFillGradientCenterX:
  case text::TextPropertyId::GlyphFillGradientCenterY:
  case text::TextPropertyId::GlyphFillGradientRadius: {
    const auto *fill = FindGlyphLayer<text::TextFillLayer>(run, target.layerId);
    const auto material = fill ? MaterialFromBinding(fill->material)
                               : std::nullopt;
    if (!material)
      return std::nullopt;
    if (property == text::TextPropertyId::GlyphFill)
      return *material;
    if (property == text::TextPropertyId::GlyphFillColor)
      return RepresentativeMaterialColor(*material);
    if (property == text::TextPropertyId::GlyphFillOpacity)
      return static_cast<double>(MaterialOpacity(*material));
    if (property == text::TextPropertyId::GlyphFillGradientAngle) {
      const auto *linear =
          std::get_if<text::LinearGradientTextMaterial>(&*material);
      if (!linear)
        return std::nullopt;
      return std::atan2(static_cast<double>(linear->endY - linear->startY),
                        static_cast<double>(linear->endX - linear->startX)) *
             180.0 / 3.14159265358979323846;
    }
    if (property == text::TextPropertyId::GlyphFillGradientCenterX ||
        property == text::TextPropertyId::GlyphFillGradientCenterY ||
        property == text::TextPropertyId::GlyphFillGradientRadius) {
      const auto *radial =
          std::get_if<text::RadialGradientTextMaterial>(&*material);
      if (!radial)
        return std::nullopt;
      if (property == text::TextPropertyId::GlyphFillGradientCenterX)
        return static_cast<double>(radial->centerX);
      if (property == text::TextPropertyId::GlyphFillGradientCenterY)
        return static_cast<double>(radial->centerY);
      return static_cast<double>(radial->radius);
    }
    const std::vector<text::GradientStop> *stops = nullptr;
    if (const auto *linear =
            std::get_if<text::LinearGradientTextMaterial>(&*material))
      stops = &linear->stops;
    if (const auto *radial =
            std::get_if<text::RadialGradientTextMaterial>(&*material))
      stops = &radial->stops;
    if (!stops || stops->empty())
      return std::nullopt;
    const bool start =
        property == text::TextPropertyId::GlyphFillGradientStartColor ||
        property == text::TextPropertyId::GlyphFillGradientStartOffset;
    const auto &stop = start ? stops->front() : stops->back();
    if (property == text::TextPropertyId::GlyphFillGradientStartColor ||
        property == text::TextPropertyId::GlyphFillGradientEndColor)
      return stop.color;
    return static_cast<double>(stop.offset);
  }
  case text::TextPropertyId::GlyphStrokeStack: {
    std::vector<text::TextStrokeLayer> layers;
    for (const auto &layer : run.style.materials.layers)
      if (const auto *stroke = std::get_if<text::TextStrokeLayer>(&layer))
        layers.push_back(*stroke);
    return layers;
  }
  case text::TextPropertyId::GlyphShadowStack: {
    std::vector<text::TextShadowLayer> layers;
    for (const auto &layer : run.style.materials.layers)
      if (const auto *shadow = std::get_if<text::TextShadowLayer>(&layer))
        layers.push_back(*shadow);
    return layers;
  }
  case text::TextPropertyId::GlyphGlowStack: {
    std::vector<text::TextGlowLayer> layers;
    for (const auto &layer : run.style.materials.layers)
      if (const auto *glow = std::get_if<text::TextGlowLayer>(&layer))
        layers.push_back(*glow);
    return layers;
  }
  case text::TextPropertyId::GlyphStrokeColor:
  case text::TextPropertyId::GlyphStrokeWidth:
  case text::TextPropertyId::GlyphStrokeOpacity: {
    const auto *layer =
        FindGlyphLayer<text::TextStrokeLayer>(run, target.layerId);
    const auto material = layer ? MaterialFromBinding(layer->material)
                                : std::nullopt;
    if (!layer || !material)
      return std::nullopt;
    if (property == text::TextPropertyId::GlyphStrokeColor)
      return RepresentativeMaterialColor(*material);
    if (property == text::TextPropertyId::GlyphStrokeOpacity)
      return static_cast<double>(MaterialOpacity(*material));
    return static_cast<double>(layer->width);
  }
  case text::TextPropertyId::GlyphShadowColor:
  case text::TextPropertyId::GlyphShadowOpacity:
  case text::TextPropertyId::GlyphShadowOffsetX:
  case text::TextPropertyId::GlyphShadowOffsetY:
  case text::TextPropertyId::GlyphShadowBlur:
  case text::TextPropertyId::GlyphShadowSpread:
  case text::TextPropertyId::GlyphShadowDistance:
  case text::TextPropertyId::GlyphShadowAngle: {
    const auto *layer =
        FindGlyphLayer<text::TextShadowLayer>(run, target.layerId);
    const auto material = layer ? MaterialFromBinding(layer->material)
                                : std::nullopt;
    if (!layer || !material)
      return std::nullopt;
    if (property == text::TextPropertyId::GlyphShadowColor)
      return RepresentativeMaterialColor(*material);
    if (property == text::TextPropertyId::GlyphShadowOpacity)
      return static_cast<double>(MaterialOpacity(*material));
    if (property == text::TextPropertyId::GlyphShadowOffsetX)
      return static_cast<double>(layer->offsetX);
    if (property == text::TextPropertyId::GlyphShadowOffsetY)
      return static_cast<double>(layer->offsetY);
    if (property == text::TextPropertyId::GlyphShadowBlur)
      return static_cast<double>(layer->blurRadius);
    if (property == text::TextPropertyId::GlyphShadowSpread)
      return static_cast<double>(layer->spread);
    if (property == text::TextPropertyId::GlyphShadowDistance)
      return static_cast<double>(layer->thicknessDistance);
    return static_cast<double>(layer->thicknessAngleDegrees);
  }
  case text::TextPropertyId::GlyphGlowColor:
  case text::TextPropertyId::GlyphGlowOpacity:
  case text::TextPropertyId::GlyphGlowRadius:
  case text::TextPropertyId::GlyphGlowSpread:
  case text::TextPropertyId::GlyphGlowDirectionX:
  case text::TextPropertyId::GlyphGlowDirectionY: {
    const auto *layer = FindGlyphLayer<text::TextGlowLayer>(run, target.layerId);
    const auto material = layer ? MaterialFromBinding(layer->material)
                                : std::nullopt;
    if (!layer || !material)
      return std::nullopt;
    if (property == text::TextPropertyId::GlyphGlowColor)
      return RepresentativeMaterialColor(*material);
    if (property == text::TextPropertyId::GlyphGlowOpacity)
      return static_cast<double>(MaterialOpacity(*material));
    if (property == text::TextPropertyId::GlyphGlowRadius)
      return static_cast<double>(layer->radius);
    if (property == text::TextPropertyId::GlyphGlowSpread)
      return static_cast<double>(layer->spread);
    if (property == text::TextPropertyId::GlyphGlowDirectionX)
      return static_cast<double>(layer->directionX);
    return static_cast<double>(layer->directionY);
  }
  case text::TextPropertyId::SemanticMaterialChannel: {
    const auto material =
        GlyphLayerMaterial(FindGlyphLayer(run, target.layerId));
    return material ? std::optional<text::TextPropertyValue>{*material}
                    : std::nullopt;
  }
  case text::TextPropertyId::RunBackground:
    return run.style.background;
  default:
    return std::nullopt;
  }
}

std::optional<text::TextPropertyValue>
ParagraphProperty(const text::RichTextParagraph &paragraph,
                  const text::TextPropertyId property) {
  const auto &style = paragraph.style;
  switch (property) {
  case text::TextPropertyId::ParagraphBackground:
    return style.background;
  case text::TextPropertyId::ParagraphAlignment:
    return static_cast<std::int64_t>(style.alignment);
  case text::TextPropertyId::ParagraphDirection:
    return static_cast<std::int64_t>(style.direction);
  case text::TextPropertyId::ParagraphLocale:
    return style.locale;
  case text::TextPropertyId::ParagraphLineHeight:
    return static_cast<double>(style.lineHeight);
  case text::TextPropertyId::ParagraphMaximumLines:
    return style.maximumLines
               ? text::TextPropertyValue{static_cast<std::int64_t>(
                     *style.maximumLines)}
               : text::TextPropertyValue{std::monostate{}};
  case text::TextPropertyId::ParagraphOverflow:
    return static_cast<std::int64_t>(style.overflow);
  case text::TextPropertyId::ParagraphWrap:
    return static_cast<std::int64_t>(style.wrap);
  case text::TextPropertyId::ParagraphLineBreakPolicy:
    return static_cast<std::int64_t>(style.lineBreakPolicy);
  case text::TextPropertyId::ParagraphHyphenation:
    return static_cast<std::int64_t>(style.hyphenation);
  case text::TextPropertyId::ParagraphFirstLineIndent:
    return static_cast<double>(style.firstLineIndent);
  case text::TextPropertyId::ParagraphStartIndent:
    return static_cast<double>(style.startIndent);
  case text::TextPropertyId::ParagraphEndIndent:
    return static_cast<double>(style.endIndent);
  case text::TextPropertyId::ParagraphSpacingBefore:
    return static_cast<double>(style.spacingBefore);
  case text::TextPropertyId::ParagraphSpacingAfter:
    return static_cast<double>(style.spacingAfter);
  case text::TextPropertyId::ParagraphHangingPunctuation:
    return style.hangingPunctuation;
  case text::TextPropertyId::ParagraphTabStops:
    return style.tabStops;
  default:
    return std::nullopt;
  }
}

const text::TextBackdropLayer *FindBackdropLayer(
    const TextCompositionDocument &document,
    const text::TextPropertyTarget &target) noexcept {
  const auto found = std::find_if(
      document.presentation.appearance.backdrops.layers.begin(),
      document.presentation.appearance.backdrops.layers.end(),
      [&](const auto &layer) {
        return target.layerId.empty() || layer.layerId == target.layerId;
      });
  return found == document.presentation.appearance.backdrops.layers.end()
             ? nullptr
             : &*found;
}

std::optional<text::TextMaterial>
BackdropMaterial(const text::TextBackdropLayer &layer) {
  if (const auto *rounded =
          std::get_if<text::RoundedRectBackdrop>(&layer.source))
    return MaterialFromBinding(rounded->fill);
  if (layer.materialOverride)
    return MaterialFromBinding(*layer.materialOverride);
  return std::visit(
      [](const auto &source) -> std::optional<text::TextMaterial> {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, text::RoundedRectBackdrop>) {
          return std::nullopt;
        } else {
          return text::TextMaterial{text::SolidTextMaterial{
              source.fallbackColor}};
        }
      },
      layer.source);
}

std::optional<text::TextPropertyValue>
CompositionProperty(const TextCompositionDocument &document,
                    const text::TextPropertyId property,
                    const text::TextPropertyTarget &target) {
  switch (property) {
  case text::TextPropertyId::LayoutFrame:
    return document.presentation.authoredLayoutFrame;
  case text::TextPropertyId::LayoutSizingMode:
    return static_cast<std::int64_t>(
        document.presentation.authoredLayoutFrame.sizingMode);
  case text::TextPropertyId::LayoutVerticalAlignment:
    return static_cast<std::int64_t>(
        document.presentation.authoredLayoutFrame.verticalAlignment);
  case text::TextPropertyId::LayoutPadding:
    return document.presentation.authoredLayoutFrame.padding;
  case text::TextPropertyId::LayoutPixelSnap:
    return document.presentation.authoredLayoutFrame.pixelSnap;
  case text::TextPropertyId::WritingMode:
    return document.presentation.writingMode;
  case text::TextPropertyId::BackdropStack:
    return document.presentation.appearance.backdrops;
  case text::TextPropertyId::BackdropMaterial:
  case text::TextPropertyId::BackdropEnabled:
  case text::TextPropertyId::BackdropFillColor:
  case text::TextPropertyId::BackdropOpacity:
  case text::TextPropertyId::BackdropPadding:
  case text::TextPropertyId::BackdropCornerRadius:
  case text::TextPropertyId::BackdropWidth:
  case text::TextPropertyId::BackdropHeight:
  case text::TextPropertyId::BackdropOffsetX:
  case text::TextPropertyId::BackdropOffsetY:
  case text::TextPropertyId::BackdropScaleX:
  case text::TextPropertyId::BackdropScaleY:
  case text::TextPropertyId::BackdropRotation: {
    const auto *layer = FindBackdropLayer(document, target);
    if (!layer)
      return std::nullopt;
    if (property == text::TextPropertyId::BackdropEnabled)
      return layer->enabled;
    const auto material = BackdropMaterial(*layer);
    if (property == text::TextPropertyId::BackdropMaterial)
      return material ? std::optional<text::TextPropertyValue>{*material}
                      : std::nullopt;
    if (property == text::TextPropertyId::BackdropFillColor)
      return material ? std::optional<text::TextPropertyValue>{
                            RepresentativeMaterialColor(*material)}
                      : std::nullopt;
    if (property == text::TextPropertyId::BackdropOpacity)
      return static_cast<double>(layer->transform.opacity);
    if (property == text::TextPropertyId::BackdropPadding)
      return layer->padding;
    if (property == text::TextPropertyId::BackdropOffsetX)
      return static_cast<double>(layer->transform.offsetX);
    if (property == text::TextPropertyId::BackdropOffsetY)
      return static_cast<double>(layer->transform.offsetY);
    if (property == text::TextPropertyId::BackdropScaleX)
      return static_cast<double>(layer->transform.scaleX);
    if (property == text::TextPropertyId::BackdropScaleY)
      return static_cast<double>(layer->transform.scaleY);
    if (property == text::TextPropertyId::BackdropRotation)
      return static_cast<double>(layer->transform.rotationDegrees);
    const auto *rounded =
        std::get_if<text::RoundedRectBackdrop>(&layer->source);
    if (!rounded)
      return std::nullopt;
    if (property == text::TextPropertyId::BackdropCornerRadius)
      return static_cast<double>(rounded->cornerRadius);
    if (property == text::TextPropertyId::BackdropWidth)
      return rounded->authoredWidth
                 ? std::optional<text::TextPropertyValue>{
                       static_cast<double>(*rounded->authoredWidth)}
                 : std::nullopt;
    return rounded->authoredHeight
               ? std::optional<text::TextPropertyValue>{
                     static_cast<double>(*rounded->authoredHeight)}
               : std::nullopt;
  }
  case text::TextPropertyId::FrameBackdrop:
  case text::TextPropertyId::BubbleBackdrop:
  case text::TextPropertyId::CustomBackdrop: {
    const auto expected = property == text::TextPropertyId::FrameBackdrop
                              ? text::TextBackdropChannel::Frame
                              : property == text::TextPropertyId::BubbleBackdrop
                                    ? text::TextBackdropChannel::Bubble
                                    : text::TextBackdropChannel::Custom;
    const auto found = std::find_if(
        document.presentation.appearance.backdrops.layers.begin(),
        document.presentation.appearance.backdrops.layers.end(),
        [&](const auto &layer) {
          return layer.channel == expected &&
                 (target.layerId.empty() || layer.layerId == target.layerId);
        });
    return found == document.presentation.appearance.backdrops.layers.end()
               ? std::nullopt
               : std::optional<text::TextPropertyValue>{*found};
  }
  case text::TextPropertyId::Bend:
    return document.presentation.appearance.bend;
  case text::TextPropertyId::Path:
    return document.presentation.appearance.path;
  case text::TextPropertyId::SdfMaterial:
    return document.presentation.appearance.sdfMaterial;
  case text::TextPropertyId::GlobalAlpha:
    return static_cast<double>(document.presentation.appearance.globalAlpha);
  default:
    return std::nullopt;
  }
}

std::string PropertyValueIdentity(const text::TextPropertyValue &value) {
  return detail::MakeIdentity("videocut.text.property-value", [&](auto &writer) {
    detail::Encode(writer, value);
  });
}

} // namespace

std::string ComputeTextContentDigest(
    const TextCompositionDocument &document) {
  return detail::TextContentDigest(document);
}

std::vector<TextEditRunProjection> ProjectTextEditRuns(
    const TextCompositionDocument &document) {
  std::vector<TextEditRunProjection> result;
  const auto addresses = BuildRunAddresses(document);
  result.reserve(addresses.size());
  for (const auto &address : addresses) {
    const auto &paragraph = document.content[address.paragraphIndex];
    const auto &run = paragraph.runs[address.runIndex];
    TextEditRunProjection projection;
    projection.paragraphId = paragraph.paragraphId;
    projection.runId = run.runId;
    projection.globalUtf8Begin = address.globalBegin;
    projection.utf8ByteLength = run.utf8Text.size();
    const auto boundaries = GraphemeBoundaries(run.utf8Text);
    projection.graphemeBoundaryOffsets.reserve(boundaries.size());
    for (const auto boundary : boundaries)
      projection.graphemeBoundaryOffsets.push_back(boundary);
    projection.graphemeBoundaryDigest = BoundaryDigest(run.utf8Text);
    result.push_back(std::move(projection));
  }
  return result;
}

TextEditResult ApplyTextEditTransaction(
    TextCompositionDocument &document, const TextEditTransaction &transaction,
    const TextCompositionLimits &limits,
    std::vector<std::optional<text::TextPropertyTarget>> *propertyTargets) {
  if (transaction.phase != TextEditTransactionPhase::Commit &&
      transaction.phase != TextEditTransactionPhase::Finalize) {
    TextEditResult result;
    Add(result.diagnostics, "text_edit.persistent_phase_invalid",
        transaction.sessionId,
        "the stateless persistence adapter accepts only commit or finalize");
    return result;
  }
  // The Workspace owns the IME draft, selection and undo history. Both phases
  // operate only on the candidate supplied by that owner: Commit advances the
  // candidate, while Finalize validates and publishes that same final sample.
  // An empty Finalize is therefore a valid no-op receipt and never requires a
  // second native TextEditSession/document owner.
  TransactionState state;
  if (propertyTargets) state.propertyTargets = *propertyTargets;
  auto result = ApplyTransaction(document, transaction, state, limits);
  if (result.valid && propertyTargets)
    *propertyTargets = std::move(state.propertyTargets);
  return result;
}

std::vector<text::TextPropertyState> QueryTextPropertyState(
    const TextCompositionDocument &document,
    const text::TextPropertyTarget &target) {
  std::vector<text::TextPropertyState> result;
  const auto runs = SelectedRuns(document, target);
  const auto paragraphs = SelectedParagraphs(document, target);
  const auto maximum =
      static_cast<std::uint16_t>(text::TextPropertyId::BackdropEnabled);
  for (std::uint16_t raw = 0U; raw <= maximum; ++raw) {
    const auto property = static_cast<text::TextPropertyId>(raw);
    const auto *descriptor = text::DescribeTextProperty(property);
    if (!descriptor ||
        (descriptor->legalScopeMask & text::TextPropertyScopeBit(target.scope)) ==
            0U)
      continue;
    std::vector<text::TextPropertyValue> values;
    if (const auto composition = CompositionProperty(document, property, target))
      values.push_back(*composition);
    if (values.empty()) {
      for (const auto *paragraph : paragraphs)
        if (const auto value = ParagraphProperty(*paragraph, property))
          values.push_back(*value);
    }
    if (values.empty()) {
      for (const auto *run : runs)
        if (const auto value = RunProperty(*run, property, target))
          values.push_back(*value);
    }
    text::TextPropertyState state;
    state.address = {property, target};
    if (values.empty()) {
      state.state = text::TextPropertyStateKind::Unavailable;
    } else {
      const auto identity = PropertyValueIdentity(values.front());
      const bool mixed = std::any_of(
          values.begin() + 1, values.end(), [&](const auto &value) {
            return PropertyValueIdentity(value) != identity;
          });
      state.state = mixed ? text::TextPropertyStateKind::Mixed
                          : text::TextPropertyStateKind::Uniform;
      if (!mixed)
        state.value = values.front();
    }
    result.push_back(std::move(state));
  }
  return result;
}

} // namespace videocut::text_composition
