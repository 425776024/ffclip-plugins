#include "videocut/vector/VectorDigest.h"
#include "videocut/base/Sha256.h"

namespace videocut::vector {
std::string Sha256Digest(const std::uint8_t *bytes, const std::size_t size) {
  base::Sha256 hasher;
  if (size > 0U)
    hasher.Update(bytes, size);
  return "sha256:" + base::Sha256::ToHex(hasher.Finalize());
}

} // namespace videocut::vector
