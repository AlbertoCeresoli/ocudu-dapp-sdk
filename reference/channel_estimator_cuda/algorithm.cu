// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/channel_estimator_cuda.h"
#include "ocudu_dapp_sdk/class_a_cuda_contract.cuh"

namespace ocudu::dapp::reference {
namespace {

using namespace cuda_contract;

template <uint32_t Layers>
__global__ void estimate_channel_and_noise_kernel(inputs input, float2* channel, float* noise, float* scheduler_noise)
{
  static_assert(Layers == 1 || Layers == 2);
  extern __shared__ unsigned char storage[];
  const uint32_t                  port  = blockIdx.x;
  const uint32_t                  plane = input.nof_symbols * input.nof_subcarriers;
  // Compact LS + TD + FD fill, then expand only allocated PRB SCs into the full plane.
  estimate_channels_block<Layers>(input, port, storage, &channel[port * Layers * plane], plane);
  __syncthreads();
  estimate_noise_block<Layers>(input, port, &channel[port * Layers * plane], plane, storage, &noise[port * Layers],
                               Layers, scheduler_noise != nullptr ? &scheduler_noise[port * Layers] : nullptr);
}

} // namespace

ocudu_dapp_status_v1 estimate_channel_cuda(const ocudu_dapp_channel_estimator_input_v1& input,
                                           ocudu_dapp_channel_estimator_output_v1&      output,
                                           const channel_estimator_cuda_state&, channel_estimator_cuda_worker_state&)
{
  cuda_contract::inputs validated{};
  const bool scheduler_noise_requested = output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_INDEX_V1] ==
                                         OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_V1;
  float* scheduler_noise = scheduler_noise_requested
                               ? reinterpret_cast<float*>(static_cast<uintptr_t>(
                                     output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_HANDLE_INDEX_V1]))
                               : nullptr;
  if (!cuda_contract::validate_inputs(input, validated) ||
      !cuda_contract::device_tensor(output.channel_estimates, OCUDU_DAPP_ELEMENT_CF32_V1, 4, OCUDU_DAPP_ACCESS_WRITE_V1,
                                    sizeof(float2)) ||
      !cuda_contract::device_tensor(output.noise_variances, OCUDU_DAPP_ELEMENT_F32_V1, 2, OCUDU_DAPP_ACCESS_WRITE_V1,
                                    sizeof(float)) ||
      output.channel_estimates.dimensions[0] != validated.nof_rx_ports ||
      output.channel_estimates.dimensions[1] != validated.nof_layers ||
      output.channel_estimates.dimensions[2] != validated.nof_symbols ||
      output.channel_estimates.dimensions[3] != validated.nof_subcarriers ||
      output.noise_variances.dimensions[0] != validated.nof_rx_ports ||
      output.noise_variances.dimensions[1] != validated.nof_layers ||
      (scheduler_noise_requested &&
       (scheduler_noise == nullptr ||
        output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_BYTES_INDEX_V1] !=
            static_cast<uint64_t>(validated.nof_rx_ports) * validated.nof_layers * sizeof(float)))) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto*          channel = cuda_contract::pointer<float2>(output.channel_estimates);
  const uint32_t shared =
      channel_setup_shared_bytes_for(validated.nof_layers, validated.nof_subcarriers, validated.nof_pilots,
                                     validated.dmrs_symbol_mask, validated.dmrs_type, validated.alloc_nof_sc);
  if (validated.nof_layers == 1) {
    estimate_channel_and_noise_kernel<1>
        <<<validated.nof_rx_ports, channel_reduction_threads, shared, validated.stream>>>(
            validated, channel, cuda_contract::pointer<float>(output.noise_variances), scheduler_noise);
  } else {
    estimate_channel_and_noise_kernel<2>
        <<<validated.nof_rx_ports, channel_reduction_threads, shared, validated.stream>>>(
            validated, channel, cuda_contract::pointer<float>(output.noise_variances), scheduler_noise);
  }
  const auto status = cuda_contract::launch_status();
  if (status == OCUDU_DAPP_OK_V1 && scheduler_noise_requested) {
    output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_INDEX_V1] =
        OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_V1;
  }
  return status;
}

} // namespace ocudu::dapp::reference
