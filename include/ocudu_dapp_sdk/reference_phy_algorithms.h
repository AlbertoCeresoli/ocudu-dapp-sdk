// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"

namespace ocudu::dapp::reference {

/// Host-path Class-A CE/EQ/RX references (OCUDU-aligned average CE + ZF).
/// Same algorithmic structure as the CUDA Class-A contract (LS, average_pairs,
/// TD average, FD fill, per-RE ZF); independently implemented on host CF32 tensors.
///
/// Hot-path notes (PUSCH deadline): H is stored per-(port,layer,SC) only (symbol-
/// independent after TD average); pilot TD average is O(nof_pilots); soft demap
/// uses precomputed PAM levels; thread_local workspaces avoid per-invoke malloc.
/// **Always build with CMAKE_BUILD_TYPE=Release (-O3).** Unoptimized host RX was
/// ~10–20× slower and opened the circuit breaker under OTA speed tests; Release
/// full-BW 256-QAM RX is ~0.4 ms (microbench) and stays under deadline OTA.
ocudu_dapp_status_v1 estimate_channel(const ocudu_dapp_channel_estimator_input_v1& input,
                                      ocudu_dapp_channel_estimator_output_v1&      output);

ocudu_dapp_status_v1 equalize(const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output);

ocudu_dapp_status_v1 receive(const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output);

} // namespace ocudu::dapp::reference
