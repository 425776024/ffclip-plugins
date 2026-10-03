#pragma once

#include <functional>
#include <cstddef>
#include <string>

namespace videocut::skia_runtime::internal {

// Synchronous encoding handoff to the owning Skia lane. The owner commits the
// native command and retains its completion receipt for frame publication.
// Empty callbacks are reserved for explicit standalone/reference consumers.
using NativeCommandSubmission =
    std::function<bool(void *command, std::size_t retainedBytes, std::string &error)>;

struct NativeRgba8TextureTarget final {
  void *texture{nullptr};
  void *commandQueue{nullptr};
  NativeCommandSubmission submit;
};

} // namespace videocut::skia_runtime::internal
