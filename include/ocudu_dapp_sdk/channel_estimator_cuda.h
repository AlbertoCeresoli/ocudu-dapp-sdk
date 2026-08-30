// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/channel_estimator.h"

namespace ocudu::dapp::reference {

/// Instance-owned state shared by every admitted channel-estimator worker.
///
/// The reference algorithm needs no weights. A copied dApp can extend this
/// type with immutable prepared model state or atomically published handles.
struct channel_estimator_cuda_state {
};

/// Mutable state owned by one channel-estimator worker/lane.
///
/// A copied dApp can keep stream-local scratch or history here without a lock.
struct channel_estimator_cuda_worker_state {
};

/// Queue the standalone channel-estimation reference on the host-provided CUDA stream.
ocudu_dapp_status_v1 estimate_channel_cuda(const ocudu_dapp_channel_estimator_input_v1& input,
                                           ocudu_dapp_channel_estimator_output_v1&      output,
                                           const channel_estimator_cuda_state&          state,
                                           channel_estimator_cuda_worker_state&         worker);

} // namespace ocudu::dapp::reference
