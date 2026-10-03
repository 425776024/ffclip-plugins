#pragma once

#include "videocut/text/TextProperty.h"

namespace videocut::text::internal {

// Encodes an exclusively owned scratch value without copying its payload.
// Numeric normalization may modify value, including before a failure. The
// payload remains available for subsequent fields; output changes only on
// success. Public callers retain their source through the const-value API.
bool EncodeCanonicalTextPropertyValueInPlace(
    TextPropertyValue &value, std::vector<std::uint8_t> &output,
    const RichTextLimits &limits);

} // namespace videocut::text::internal
