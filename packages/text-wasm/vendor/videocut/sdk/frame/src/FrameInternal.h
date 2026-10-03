#pragma once

#include <cstdint>

namespace videocut::frame::detail {

[[nodiscard]] std::uint64_t NextContentId() noexcept;

} // namespace videocut::frame::detail
