// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Product algorithm entry — here a thin call into open conventional Class-A.
//
// A commercial vendor replaces this body with proprietary code and may fall
// back to ref_class_a / equalize_cuda the same way commercial vendor packages do.
// Keep the signature stable so module_support.cpp never needs to change for
// algorithm swaps.

#include "ocudu_dapp_sdk/equalizer_cuda.h"

namespace ocudu::dapp::example {

ocudu_dapp_status_v1 equalize(const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output,
                              const ocudu::dapp::reference::equalizer_cuda_state&  state,
                              ocudu::dapp::reference::equalizer_cuda_worker_state& worker)
{
  // Open reference conventional path (install: ref_class_a via reference_api).
  return ocudu::dapp::reference::equalize_cuda(input, output, state, worker);
}

} // namespace ocudu::dapp::example
