#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace videocut::vector {

/// Returns the canonical lower-case `sha256:<hex>` identity for immutable
/// asset bytes.
std::string Sha256Digest(const std::uint8_t *bytes, std::size_t size);

} // namespace videocut::vector
