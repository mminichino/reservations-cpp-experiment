#pragma once

#include <chrono>

namespace reservations {

/// Injectable clock so domain code never calls system_clock::now() directly.
class Clock {
 public:
  using Instant = std::chrono::system_clock::time_point;

  virtual ~Clock() = default;
  [[nodiscard]] virtual Instant Now() const = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] Instant Now() const override { return std::chrono::system_clock::now(); }
};

}  // namespace reservations
