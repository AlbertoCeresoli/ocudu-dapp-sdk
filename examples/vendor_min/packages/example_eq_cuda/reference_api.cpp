// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Standalone-only: host wrappers onto installed ref_class_a static lib.
//
// In-tree SDK builds link ref_equalizer_cuda_reference_api instead (see
// examples/vendor_min/CMakeLists.txt). Vendor machines that only install
// ref_*_cuda_algorithm archives compile this TU so algorithm.cpp can keep
// calling reference::equalize_cuda without ODR clashes on commercial symbols.

#include "ocudu_dapp_sdk/equalizer_cuda.h"
#include "ocudu_dapp_sdk/ref_class_a_cuda.h"

namespace ocudu::dapp::reference {

ocudu_dapp_status_v1 initialize_equalizer_cuda_worker_state(equalizer_cuda_worker_state& state,
                                                            uint32_t                     max_nof_rx_ports) noexcept
{
  ref_class_a::equalizer_worker_state shared{state.noise_variances, state.nof_rx_ports};
  const auto                          status = ref_class_a::initialize_equalizer_worker(shared, max_nof_rx_ports);
  state.noise_variances                      = shared.noise_variances;
  state.nof_rx_ports                         = shared.nof_rx_ports;
  return status;
}

void release_equalizer_cuda_worker_state(equalizer_cuda_worker_state& state) noexcept
{
  ref_class_a::equalizer_worker_state shared{state.noise_variances, state.nof_rx_ports};
  ref_class_a::release_equalizer_worker(shared);
  state.noise_variances = shared.noise_variances;
  state.nof_rx_ports    = shared.nof_rx_ports;
}

ocudu_dapp_status_v1 equalize_cuda(const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output,
                                   const equalizer_cuda_state&, equalizer_cuda_worker_state&                   worker)
{
  ref_class_a::equalizer_worker_state shared{worker.noise_variances, worker.nof_rx_ports};
  return ref_class_a::equalize(input, output, shared);
}

} // namespace ocudu::dapp::reference
