// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Shared open Class-A CUDA algorithms (LS CE + ZF EQ + max-log demap).
// Stable symbol namespace for vendor packages — no clash with proprietary
// equalize_cuda / receive_cuda entry points.
//
// Prefer this header from commercial trees. Open reference modules still expose
// ocudu::dapp::reference::equalize_cuda as thin wrappers (same code path).

#pragma once

#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include <cstddef>

namespace ocudu::dapp::ref_class_a {

/// Per-worker device scratch for the equalizer path. One instance per worker /
/// CUDA stream; not safe for concurrent \ref equalize calls on the same object.
struct equalizer_worker_state {
  /// Device buffer of \c nof_rx_ports per-port noise variances
  /// (owned; allocated by \ref initialize_equalizer_worker).
  float* noise_variances{};
  /// Capacity of \c noise_variances in ports.
  uint32_t nof_rx_ports{};
};

/// Shared read-only receiver state. Kernels only read it, so one instance may
/// serve many workers concurrently once initialized.
struct receiver_state {
  /// Device copy of the demapper constellation-level artifact
  /// (owned; allocated by \ref initialize_receiver).
  const float* constellation_levels{};
};

/// Per-worker device scratch for the full-receiver path. One instance per
/// worker / CUDA stream; not safe for concurrent \ref receive calls.
struct receiver_worker_state {
  /// Single owned device allocation holding compact channel estimates
  /// [port][layer][subcarrier], per-port noise, and per-port/layer phase slopes.
  void* storage{};
  /// Port / layer capacities sized at \ref initialize_receiver_worker.
  uint32_t nof_rx_ports{};
  uint32_t nof_layers{};
  /// Bytes allocated in `storage` (channel plane + per-port noise).
  /// \ref receive grows the allocation on demand when a grant needs more.
  size_t storage_bytes{};
};

/// Allocate device scratch for up to \p max_nof_rx_ports receive ports.
/// Returns OCUDU_DAPP_RESOURCE_V1 when the device allocation fails.
ocudu_dapp_status_v1 initialize_equalizer_worker(equalizer_worker_state& state,
                                                 uint32_t                max_nof_rx_ports = 64) noexcept;
/// Free the worker's device scratch (safe on an empty state).
void release_equalizer_worker(equalizer_worker_state& state) noexcept;

/// LS channel estimation + ZF equalization of one PUSCH grant. Validates the
/// ABI tensors (device-resident, cf32/f32, expected layouts) and launches the
/// kernels on the CUDA stream carried by \p input; work is asynchronous and
/// complete when that stream is synchronized by the host.
ocudu_dapp_status_v1 equalize(const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output,
                              equalizer_worker_state& worker) noexcept;

/// Copy the constellation-level artifact to the device (\p levels == nullptr
/// selects the built-in reference artifact). Fails if \p state is already
/// initialized.
ocudu_dapp_status_v1 initialize_receiver(receiver_state& state, const float* levels = nullptr) noexcept;
/// Free the device constellation levels (safe on an empty state).
void release_receiver(receiver_state& state) noexcept;
/// Allocate the worker's device scratch for up to \p max_nof_rx_ports ports and
/// \p max_nof_layers layers.
ocudu_dapp_status_v1 initialize_receiver_worker(receiver_worker_state& state, uint32_t max_nof_rx_ports = 64,
                                                uint32_t max_nof_layers = 2) noexcept;
/// Free the worker's device scratch (safe on an empty state).
void release_receiver_worker(receiver_worker_state& state) noexcept;

/// Full open receiver: compact CE + max-log demap to f16 LLRs (QPSK-256QAM)
/// with optional post-EQ-noise metrics. Kernels are launched on the CUDA
/// stream carried by \p input (asynchronous, like \ref equalize). \p state is
/// read-only and shareable; \p worker is per-invocation-exclusive.
ocudu_dapp_status_v1 receive(const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output,
                             const receiver_state& state, receiver_worker_state& worker) noexcept;

} // namespace ocudu::dapp::ref_class_a
