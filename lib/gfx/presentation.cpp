#include "presentation.hpp"

#include "render_worker.hpp"
#include "resources.hpp"
#include "../internal.hpp"

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>

namespace aurora::gfx::presentation {
namespace {
using PresentClock = std::chrono::steady_clock;

struct PresentTimingSample {
  PresentClock::time_point presentedAt{};
  std::chrono::nanoseconds interval{};
  bool duplicated = false;
};

std::mutex g_timingMutex;
std::array<PresentTimingSample, 512> g_timingSamples{};
size_t g_timingWriteIndex = 0;
size_t g_timingSampleCount = 0;
std::optional<PresentClock::time_point> g_lastPresent;
uint64_t g_totalPresentCount = 0;

std::atomic_uint64_t g_scheduleBaseNanos = 0;
std::atomic_uint64_t g_scheduleIntervalNanos = 0;
std::atomic_bool g_producerPaced = true;

std::atomic<AuroraGuestWriteGenerationCallback> g_guestWriteGeneration = nullptr;
std::atomic<AuroraGuestWriteNotifyCallback> g_guestWriteNotify = nullptr;
} // namespace

void record_present(bool duplicated) noexcept {
  const auto now = PresentClock::now();
  std::lock_guard lock(g_timingMutex);
  std::chrono::nanoseconds interval{};
  if (g_lastPresent) {
    interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *g_lastPresent);
  }
  g_lastPresent = now;
  g_timingSamples[g_timingWriteIndex] = {
      .presentedAt = now,
      .interval = interval,
      .duplicated = duplicated,
  };
  g_timingWriteIndex = (g_timingWriteIndex + 1) % g_timingSamples.size();
  g_timingSampleCount = std::min(g_timingSampleCount + 1, g_timingSamples.size());
  ++g_totalPresentCount;
}

AuroraPresentTiming snapshot_timing() noexcept {
  constexpr auto Window = std::chrono::seconds(1);
  const auto cutoff = PresentClock::now() - Window;
  std::vector<double> milliseconds;
  uint64_t totalPresentCount = 0;
  size_t newMotionSamples = 0;
  {
    std::lock_guard lock(g_timingMutex);
    totalPresentCount = g_totalPresentCount;
    milliseconds.reserve(g_timingSampleCount);
    for (size_t i = 0; i < g_timingSampleCount; ++i) {
      const auto& sample = g_timingSamples[i];
      if (sample.presentedAt >= cutoff && sample.interval.count() > 0) {
        milliseconds.push_back(std::chrono::duration<double, std::milli>(sample.interval).count());
        if (!sample.duplicated) {
          ++newMotionSamples;
        }
      }
    }
  }

  AuroraPresentTiming result{
      .totalPresentCount = totalPresentCount,
      .sampleCount = static_cast<uint32_t>(milliseconds.size()),
  };
  if (milliseconds.empty()) {
    return result;
  }

  double totalMilliseconds = 0.0;
  for (const double value : milliseconds) {
    totalMilliseconds += value;
  }
  const auto count = static_cast<double>(milliseconds.size());
  result.averageFrameTimeMs = totalMilliseconds / count;
  result.framesPerSecond = result.averageFrameTimeMs > 0.0 ? 1000.0 / result.averageFrameTimeMs : 0.0;
  result.effectiveFramesPerSecond = result.framesPerSecond * (static_cast<double>(newMotionSamples) / count);
  for (const double value : milliseconds) {
    const double difference = value - result.averageFrameTimeMs;
    result.jitterMs += difference * difference;
  }
  result.jitterMs = std::sqrt(result.jitterMs / count);
  std::sort(milliseconds.begin(), milliseconds.end());
  result.p95FrameTimeMs = milliseconds[static_cast<size_t>(std::floor((count - 1.0) * 0.95))];
  return result;
}

Schedule present_schedule() noexcept {
  return {
      .baseNanos = g_scheduleBaseNanos.load(std::memory_order_acquire),
      .intervalNanos = g_scheduleIntervalNanos.load(std::memory_order_acquire),
  };
}

bool producer_paced() noexcept { return g_producerPaced.load(std::memory_order_acquire); }

uint64_t guest_write_generation(const void* hostPtr, size_t size) noexcept {
  const auto callback = g_guestWriteGeneration.load(std::memory_order_acquire);
  return callback != nullptr ? callback(hostPtr, size) : AURORA_GUEST_WRITE_UNTRACKED;
}

void notify_guest_write(const void* hostPtr, size_t size) noexcept {
  if (const auto callback = g_guestWriteNotify.load(std::memory_order_acquire)) {
    callback(hostPtr, size);
  }
}
} // namespace aurora::gfx::presentation

namespace {
std::atomic_bool g_skipUnreadyPipelines = true;
std::atomic_uint32_t g_frameInterpolationFps = 0;
} // namespace

void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback) {
  aurora::gfx::render_worker::set_producer_wait_callback(callback);
}

void aurora_wait_for_frame_worker() { aurora::gfx::render_worker::synchronize(); }

bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros) {
  return aurora::gfx::render_worker::synchronize_for(std::chrono::microseconds{timeoutMicros});
}

void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos) {
  using namespace aurora::gfx::presentation;
  g_scheduleBaseNanos.store(baseNanos, std::memory_order_release);
  g_scheduleIntervalNanos.store(intervalNanos, std::memory_order_release);
}

void aurora_report_producer_paced(bool paced) {
  aurora::gfx::presentation::g_producerPaced.store(paced, std::memory_order_release);
}

void aurora_get_present_timing(AuroraPresentTiming* timing) {
  if (timing != nullptr) {
    *timing = aurora::gfx::presentation::snapshot_timing();
  }
}

void aurora_set_skip_unready_pipelines(bool enabled) {
  g_skipUnreadyPipelines.store(enabled, std::memory_order_release);
  AuroraSetDrawsBlocking(!enabled);
}

bool aurora_get_skip_unready_pipelines() { return g_skipUnreadyPipelines.load(std::memory_order_acquire); }

uint32_t aurora_get_queued_pipeline_count() { return aurora_get_stats()->queuedPipelines; }

void aurora_set_disable_copy_filter(bool disabled) { aurora::g_config.disableCopyFilter = disabled; }

bool aurora_get_disable_copy_filter() { return aurora::g_config.disableCopyFilter; }

void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify) {
  aurora::gfx::presentation::g_guestWriteGeneration.store(generation, std::memory_order_release);
  aurora::gfx::presentation::g_guestWriteNotify.store(notify, std::memory_order_release);
}

void aurora_set_frame_interpolation_fps(uint32_t targetFps) {
  g_frameInterpolationFps.store(targetFps, std::memory_order_release);
}

uint32_t aurora_get_frame_interpolation_fps() { return g_frameInterpolationFps.load(std::memory_order_acquire); }

void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics) {
  if (diagnostics != nullptr) {
    *diagnostics = {.targetFps = g_frameInterpolationFps.load(std::memory_order_acquire)};
  }
}
