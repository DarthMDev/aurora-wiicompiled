#pragma once

#include <aurora/gfx.h>

#include <cstddef>
#include <cstdint>

namespace aurora::gfx::presentation {
// Records a successful surface present. `duplicated` marks a present that repeated the previous
// image instead of showing new motion.
void record_present(bool duplicated) noexcept;
AuroraPresentTiming snapshot_timing() noexcept;

struct Schedule {
  uint64_t baseNanos = 0;
  uint64_t intervalNanos = 0;
};
// Schedule set by the producer for the frame currently being ended.
Schedule present_schedule() noexcept;
bool producer_paced() noexcept;

// Guest-RAM write generation for a host range, or AURORA_GUEST_WRITE_UNTRACKED.
uint64_t guest_write_generation(const void* hostPtr, size_t size) noexcept;
// Reports a write Aurora made to guest RAM.
void notify_guest_write(const void* hostPtr, size_t size) noexcept;
} // namespace aurora::gfx::presentation
