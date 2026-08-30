// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu_dapp_sdk/class_c_algorithms.h"

namespace ocudu::dapp::reference {

/// Maximum rx x tx port pairs the CUDA CIR reduction processes per invocation.
inline constexpr uint32_t max_srs_cuda_paths = 16;

/// Per-worker buffers for the CUDA SRS CIR preview: device accumulators for
/// per-path and aggregated tap powers plus pinned host mirrors for readback.
/// All owned by the state; one instance per worker, no concurrent invocations
/// on the same object.
struct srs_isac_cuda_worker_state {
  float* device_path_tap_power{};
  float* device_tap_power{};
  float* device_mean_power{};
  float* host_tap_power{};
  float* host_mean_power{};
};

/// Allocate the fixed-size device buffers and pinned host mirrors. Returns
/// OCUDU_DAPP_RESOURCE_V1 when a CUDA allocation fails.
ocudu_dapp_status_v1 initialize_srs_isac_cuda_worker_state(srs_isac_cuda_worker_state& state) noexcept;
/// Free every buffer (safe on an empty state).
void release_srs_isac_cuda_worker_state(srs_isac_cuda_worker_state& state) noexcept;

/// Forms the same bounded early-delay CIR preview as the CPU reference directly
/// from a device-resident [rx][tx][frequency] CF32 channel-estimate tensor.
///
/// The reference synchronizes the host-owned invocation stream only to make its
/// compact prepared result visible to the synchronous v1 callback transaction.
ocudu_dapp_status_v1 run_srs_cir_cuda(const ocudu_dapp_srs_isac_input_v1& input, double noise_variance,
                                      srs_isac_cuda_worker_state& worker, srs_cir_analysis& output) noexcept;

} // namespace ocudu::dapp::reference
