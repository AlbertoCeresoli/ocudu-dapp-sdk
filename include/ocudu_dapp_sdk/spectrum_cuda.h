// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <cstddef>
#include <cstdint>

namespace ocudu::dapp::reference {

/// Per-worker buffers for the CUDA spectrum radiometer, all owned by the state
/// and sized for the ABI maxima. One instance per worker; not safe for
/// concurrent invocations on the same object.
struct spectrum_cuda_worker_state {
  /// Device copy of the symbol x PRB expected-UL-use map.
  uint8_t* device_expected_use{};
  /// Device per-PRB confidence output.
  uint8_t* device_confidence{};
  /// Pinned host mirror of the confidence output.
  uint8_t* host_confidence{};
  /// Owned device snapshot of the live grid (grown on demand) and its size.
  uint32_t* device_grid_snap{};
  size_t    device_grid_snap_bytes{};
};

/// Radiometer output. `confidence` points into the worker's pinned host buffer
/// (one U8 per PRB) and is overwritten by the next invocation.
struct spectrum_cuda_result {
  const uint8_t* confidence{};
  uint32_t       nof_prbs{};
  /// Grid samples inspected.
  uint64_t samples{};
  /// PRBs reported occupied (non-zero confidence).
  uint32_t occupied_prbs{};
};

/// Allocate the worker's device buffers and pinned host mirror. Returns
/// OCUDU_DAPP_RESOURCE_V1 when a CUDA allocation fails.
ocudu_dapp_status_v1 initialize_spectrum_cuda_worker_state(spectrum_cuda_worker_state& state) noexcept;
/// Free every buffer (safe on an empty state).
void release_spectrum_cuda_worker_state(spectrum_cuda_worker_state& state) noexcept;

/// Runs a simple per-PRB radiometer on a CUDA grid ordered on the supplied
/// execution stream. The live grid is Device-to-Device copied onto an owned
/// snapshot first so the kernel never reads the host's managed UL pointer.
///
/// This reference synchronizes the host-owned invocation stream only to make its
/// small prepared result visible to the synchronous v1 callback transaction.
ocudu_dapp_status_v1 run_spectrum_radiometer_cuda(const ocudu_dapp_spectrum_input_v1& input, float amplitude_scale,
                                                  spectrum_cuda_worker_state& worker,
                                                  spectrum_cuda_result&       output) noexcept;

} // namespace ocudu::dapp::reference
