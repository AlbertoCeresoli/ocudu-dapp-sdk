// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"

namespace ocudu::dapp::reference {

/// Instance-owned state shared by every SRS channel-estimator worker. The reference needs none.
struct srs_channel_estimator_cuda_state {
};

/// Device scratch owned by one worker: the least-squares estimates and the delay profiles of one occasion.
class srs_channel_estimator_cuda_worker_state
{
public:
  srs_channel_estimator_cuda_worker_state() noexcept;
  ~srs_channel_estimator_cuda_worker_state();
  srs_channel_estimator_cuda_worker_state(const srs_channel_estimator_cuda_worker_state&)            = delete;
  srs_channel_estimator_cuda_worker_state& operator=(const srs_channel_estimator_cuda_worker_state&) = delete;

  /// Whether the scratch was allocated.
  bool ready() const noexcept { return lse != nullptr && profile != nullptr; }

  /// CF32 [rx_port, tx_port, pilot].
  void* lse = nullptr;
  /// F32 [tx_port, delay bin].
  float* profile = nullptr;
};

/// \brief Queues the reference SRS channel estimator on the host-provided CUDA stream.
///
/// Computes what the CPU reference (reference_srs_channel_estimator.h) computes, on device tensors, without cuFFT:
/// the delay profile is evaluated only at the bins the time-alignment search reads. The outputs are complete when the
/// stream reaches the end of the queued work.
ocudu_dapp_status_v1 estimate_srs_channel_cuda(const ocudu_dapp_srs_channel_estimator_input_v1& input,
                                               ocudu_dapp_srs_channel_estimator_output_v1&      output,
                                               const srs_channel_estimator_cuda_state&          state,
                                               srs_channel_estimator_cuda_worker_state&         worker);

} // namespace ocudu::dapp::reference
