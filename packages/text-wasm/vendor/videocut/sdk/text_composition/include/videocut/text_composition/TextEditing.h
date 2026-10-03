#pragma once

#include "videocut/text/TextProperty.h"
#include "videocut/text_composition/TextCompositionDocument.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace videocut::text_composition {

struct TextDocumentPosition final {
  std::string paragraphId;
  std::optional<std::string> runId;
  std::uint64_t ownerUtf8Offset{0};
  std::uint64_t globalUtf8Offset{0};
  std::uint64_t graphemeOffset{0};
  text::TextAffinity affinity{text::TextAffinity::Downstream};
  std::string graphemeBoundaryDigest;
};

struct TextDocumentRange final {
  TextDocumentPosition anchor{};
  TextDocumentPosition focus{};
};

struct TextEditIdentitySet final {
  std::vector<std::string> paragraphIds;
  std::vector<std::string> runIds;
  std::vector<std::string> spanIds;
};

struct InsertTextEdit final {
  TextDocumentPosition position{};
  std::string utf8Text;
  TextEditIdentitySet replacementIdentities{};
};

struct DeleteTextEdit final {
  TextDocumentRange range{};
};

struct ReplaceTextEdit final {
  TextDocumentRange range{};
  std::string utf8Text;
  TextEditIdentitySet replacementIdentities{};
};

struct SplitParagraphEdit final {
  TextDocumentPosition position{};
  std::string newParagraphId;
  std::string trailingRunId;
};

struct MergeParagraphEdit final {
  std::string leadingParagraphId;
  std::string trailingParagraphId;
};

struct ApplyTextPropertyEdit final {
  text::TextPropertyPatch patch{};
  bool typingStyle{false};
};

struct SetTextSelectionEdit final {
  TextDocumentRange selection{};
};

struct SetTextComposingRangeEdit final {
  std::optional<TextDocumentRange> composing;
};

using TextEditOperation =
    std::variant<InsertTextEdit, DeleteTextEdit, ReplaceTextEdit,
                 SplitParagraphEdit, MergeParagraphEdit,
                 ApplyTextPropertyEdit, SetTextSelectionEdit,
                 SetTextComposingRangeEdit>;

enum class TextEditTransactionPhase : std::uint8_t {
  Begin = 0,
  Preview,
  Commit,
  Undo,
  Redo,
  Finalize,
  Cancel,
};

struct TextInputRange final {
  std::uint64_t anchor{0};
  std::uint64_t focus{0};
};

struct TextInput final {
  std::string utf8Text;
  TextInputRange selection;
  std::optional<TextInputRange> composing;
};

struct TextEditTransaction final {
  std::string sessionId;
  std::uint64_t sequence{0};
  std::uint64_t baseRevision{0};
  std::string beforeDigest;
  TextEditTransactionPhase phase{TextEditTransactionPhase::Preview};
  std::vector<TextEditOperation> operations;
  std::optional<TextDocumentRange> selectionAfter;
  std::optional<TextDocumentRange> composingAfter;
  std::optional<TextInput> input;
};

struct TextEditReceipt final {
  std::uint64_t documentRevision{0};
  std::string textDigest;
  std::optional<TextDocumentRange> canonicalSelection;
  std::optional<TextDocumentRange> canonicalComposingRange;
  TextEditIdentitySet changedIdentities{};
  std::vector<std::pair<std::string, std::string>> paragraphIdRemap;
  std::vector<std::pair<std::string, std::string>> runIdRemap;
};

struct TextEditResult final {
  bool valid{false};
  bool changed{false};
  std::optional<TextEditReceipt> receipt;
  std::vector<Diagnostic> diagnostics;
};

/// Exact disposable edit map for one authored run. The global begin includes
/// the single `\n` separator between adjacent paragraphs. Boundary offsets
/// and their digest are produced by the same implementation that validates
/// TextEditTransaction positions, so clients never recreate Unicode
/// segmentation or stable owner identities.
struct TextEditRunProjection final {
  std::string paragraphId;
  std::string runId;
  std::uint64_t globalUtf8Begin{0};
  std::uint64_t utf8ByteLength{0};
  std::vector<std::uint64_t> graphemeBoundaryOffsets;
  std::string graphemeBoundaryDigest;
};

/// SHA-256 over the canonical flattened UTF-8 content (paragraphs separated
/// by one newline), matching renderer TextLayoutSnapshot::textDigest.
std::string ComputeTextContentDigest(
    const TextCompositionDocument &document);

/// Projects the complete ordered run/boundary closure used to author exact
/// grapheme- and owner-fenced edit positions.
std::vector<TextEditRunProjection> ProjectTextEditRuns(
    const TextCompositionDocument &document);

/// Rebases supplied range targets atomically; null entries represent deleted
/// ranges. A range that would span multiple run owners rejects the edit.
TextEditResult ApplyTextEditTransaction(
    TextCompositionDocument &document, const TextEditTransaction &transaction,
    const TextCompositionLimits &limits = {},
    std::vector<std::optional<text::TextPropertyTarget>> *propertyTargets = nullptr);

std::vector<text::TextPropertyState> QueryTextPropertyState(
    const TextCompositionDocument &document,
    const text::TextPropertyTarget &target);

} // namespace videocut::text_composition
