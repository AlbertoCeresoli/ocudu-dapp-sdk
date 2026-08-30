// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Thin open-reference wrappers onto ref_class_a (kept out of the installable
// Class-A CUDA core so commercial receive_cuda entry points do not ODR-clash).

#include "ocudu_dapp_sdk/receiver_cuda.h"
#include "ocudu_dapp_sdk/ref_class_a_cuda.h"

namespace ocudu::dapp::reference {

ocudu_dapp_status_v1 initialize_receiver_cuda_state(receiver_cuda_state& state, const float* levels) noexcept
{
  ref_class_a::receiver_state shared{state.constellation_levels};
  const auto                  status = ref_class_a::initialize_receiver(shared, levels);
  state.constellation_levels         = shared.constellation_levels;
  return status;
}

void release_receiver_cuda_state(receiver_cuda_state& state) noexcept
{
  ref_class_a::receiver_state shared{state.constellation_levels};
  ref_class_a::release_receiver(shared);
  state.constellation_levels = shared.constellation_levels;
}

ocudu_dapp_status_v1 initialize_receiver_cuda_worker_state(receiver_cuda_worker_state& state, uint32_t max_nof_rx_ports,
                                                           uint32_t max_nof_layers) noexcept
{
  ref_class_a::receiver_worker_state shared{state.storage, state.nof_rx_ports, state.nof_layers, state.storage_bytes};
  const auto status   = ref_class_a::initialize_receiver_worker(shared, max_nof_rx_ports, max_nof_layers);
  state.storage       = shared.storage;
  state.nof_rx_ports  = shared.nof_rx_ports;
  state.nof_layers    = shared.nof_layers;
  state.storage_bytes = shared.storage_bytes;
  return status;
}

void release_receiver_cuda_worker_state(receiver_cuda_worker_state& state) noexcept
{
  ref_class_a::receiver_worker_state shared{state.storage, state.nof_rx_ports, state.nof_layers, state.storage_bytes};
  ref_class_a::release_receiver_worker(shared);
  state.storage       = shared.storage;
  state.nof_rx_ports  = shared.nof_rx_ports;
  state.nof_layers    = shared.nof_layers;
  state.storage_bytes = shared.storage_bytes;
}

ocudu_dapp_status_v1 receive_cuda(const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output,
                                  const receiver_cuda_state& state, receiver_cuda_worker_state& worker)
{
  ref_class_a::receiver_state        shared_state{state.constellation_levels};
  ref_class_a::receiver_worker_state shared_worker{worker.storage, worker.nof_rx_ports, worker.nof_layers,
                                                   worker.storage_bytes};
  const auto                         status = ref_class_a::receive(input, output, shared_state, shared_worker);
  worker.storage                            = shared_worker.storage;
  worker.nof_rx_ports                       = shared_worker.nof_rx_ports;
  worker.nof_layers                         = shared_worker.nof_layers;
  worker.storage_bytes                      = shared_worker.storage_bytes;
  return status;
}

} // namespace ocudu::dapp::reference
