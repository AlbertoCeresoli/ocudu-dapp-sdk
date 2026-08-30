// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/equalizer.h"

namespace ocudu::dapp::reference {

/// Instance-owned state shared by every admitted CE+EQ worker.
///
/// The reference algorithm needs no weights. A copied dApp can extend this
/// type with immutable prepared model state or atomically published handles.
struct equalizer_cuda_state {
};

/// Per-worker CUDA scratch prepared outside invocation for concurrent equalizer lanes.
struct equalizer_cuda_worker_state {
  float*   noise_variances{};
  uint32_t nof_rx_ports{};
};

ocudu_dapp_status_v1 initialize_equalizer_cuda_worker_state(equalizer_cuda_worker_state& state,
                                                            uint32_t max_nof_rx_ports = 64) noexcept;
void                 release_equalizer_cuda_worker_state(equalizer_cuda_worker_state& state) noexcept;

/// Queue the standalone CE+EQ reference on the host-provided CUDA stream.
ocudu_dapp_status_v1 equalize_cuda(const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output,
                                   const equalizer_cuda_state& state, equalizer_cuda_worker_state& worker);

} // namespace ocudu::dapp::reference
