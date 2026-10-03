#pragma once

#include "videocut/text/RichTextDocument.h"

namespace videocut::text::internal {

// Synchronous validation borrows the authored fields and never retains them.
// Callers keep ownership, including any private candidate awaiting publication.
struct RichTextValidationView final {
  const ReferenceCanvas &referenceCanvas;
  const LayoutBox &layoutBox;
  TextWritingMode writingMode;
  const std::vector<TextContentSlot> &slots;
  const std::vector<RichTextParagraph> &paragraphs;
};

RichTextValidationResult ValidateRichText(const RichTextValidationView &view,
                                        const RichTextLimits &limits);

} // namespace videocut::text::internal
