#pragma once

#include <cstdint>

namespace videocut::text {

/// The single authored Text schema revision used by the current product.
/// Subobjects and render backends do not carry independent semantic versions.
inline constexpr std::uint32_t kTextSchemaRevision = 3U;

} // namespace videocut::text
