// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ocudu::dapp::reference {

/// Bounds quiet-period authority independently from interference-map output.
/// One intent may be attempted per cell during the lifetime of the preceding
/// intent, even when several producer shards report the same interference.
class spectrum_quiet_authority
{
public:
  static constexpr std::chrono::milliseconds intent_lifetime{500};
  static constexpr std::size_t               maximum_cells = 64;

  bool try_admit(uint32_t cell_id, uint64_t monotonic_time_ns) noexcept
  {
    if (cell_id >= next_allowed_ns.size() || monotonic_time_ns == 0) {
      return false;
    }
    const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(intent_lifetime).count();
    if (interval <= 0 || monotonic_time_ns > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(interval)) {
      return false;
    }
    const uint64_t next     = monotonic_time_ns + static_cast<uint64_t>(interval);
    uint64_t       observed = next_allowed_ns[cell_id].load(std::memory_order_relaxed);
    while (observed <= monotonic_time_ns) {
      if (next_allowed_ns[cell_id].compare_exchange_weak(observed, next, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  static uint64_t expiry(uint64_t monotonic_time_ns) noexcept
  {
    const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(intent_lifetime).count();
    return monotonic_time_ns != 0 && interval > 0 &&
                   monotonic_time_ns <= std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(interval)
               ? monotonic_time_ns + static_cast<uint64_t>(interval)
               : 0;
  }

private:
  std::array<std::atomic<uint64_t>, maximum_cells> next_allowed_ns{};
};

} // namespace ocudu::dapp::reference
