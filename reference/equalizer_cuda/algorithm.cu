// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/class_a_cuda_contract.cuh"
#include "ocudu_dapp_sdk/ref_class_a_cuda.h"
#include <algorithm>

namespace ocudu::dapp::ref_class_a {
namespace {

namespace cuda_contract = ocudu::dapp::reference::cuda_contract;
using cuda_contract::channel_reduction_threads;
using cuda_contract::channel_setup_shared_bytes_for;
using cuda_contract::equalize;
using cuda_contract::estimate_channels_block;
using cuda_contract::estimate_noise_block;
using cuda_contract::inputs;

template <uint32_t Layers>
__global__ void estimate_channel_and_noise_kernel(inputs input, float2* channel, float* noise)
{
  static_assert(Layers == 1 || Layers == 2);
  extern __shared__ unsigned char storage[];
  const uint32_t                  port  = blockIdx.x;
  const uint32_t                  plane = input.nof_symbols * input.nof_subcarriers;
  estimate_channels_block<Layers>(input, port, storage, &channel[port * Layers * plane], plane);
  __syncthreads();
  estimate_noise_block<Layers>(input, port, &channel[port * Layers * plane], plane, storage, &noise[port], 1);
}

/// Data-RE-only equalization (compact output layout).
template <uint32_t Layers>
__global__ void equalize_compact_kernel(inputs input, const float2* channel, const float* port_noise, float2* equalized,
                                        float* noise)
{
  const uint32_t plane      = input.nof_symbols * input.nof_subcarriers;
  const uint32_t data_index = blockIdx.x * blockDim.x + threadIdx.x;
  if (data_index < input.nof_data_re) {
    const uint32_t index = input.data_indices[data_index];
    if (index < plane) {
      const auto result = equalize<Layers, false>(input, channel, plane, port_noise, index);
      for (uint32_t layer = 0; layer != Layers; ++layer) {
        equalized[data_index * Layers + layer] = result.symbols[layer];
        noise[data_index * Layers + layer]     = result.noise[layer];
      }
    } else {
      for (uint32_t layer = 0; layer != Layers; ++layer) {
        equalized[data_index * Layers + layer] = {0.0F, 0.0F};
        noise[data_index * Layers + layer]     = input.noise_variance_floor;
      }
    }
  }
}

/// Full-plane equalization (legacy ABI) — still only writes every RE, but CE already limited fill cost.
template <uint32_t Layers>
__global__ void equalize_kernel(inputs input, const float2* channel, const float* port_noise, float2* equalized,
                                float* noise)
{
  const uint32_t plane = input.nof_symbols * input.nof_subcarriers;
  const uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < plane) {
    const auto result = equalize<Layers, false>(input, channel, plane, port_noise, index);
    for (uint32_t layer = 0; layer != Layers; ++layer) {
      equalized[layer * plane + index] = result.symbols[layer];
      noise[layer * plane + index]     = result.noise[layer];
    }
  }
}

} // namespace

namespace cuda_contract = ocudu::dapp::reference::cuda_contract;

ocudu_dapp_status_v1 initialize_equalizer_worker(equalizer_worker_state& state, uint32_t max_nof_rx_ports) noexcept
{
  if (state.noise_variances != nullptr || max_nof_rx_ports == 0 || max_nof_rx_ports > 64) {
    return state.noise_variances != nullptr ? OCUDU_DAPP_CONFLICT_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (cudaMalloc(&state.noise_variances, max_nof_rx_ports * sizeof(float)) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return OCUDU_DAPP_RESOURCE_V1;
  }
  state.nof_rx_ports = max_nof_rx_ports;
  return OCUDU_DAPP_OK_V1;
}

void release_equalizer_worker(equalizer_worker_state& state) noexcept
{
  if (state.noise_variances != nullptr) {
    static_cast<void>(cudaFree(state.noise_variances));
    state.noise_variances = nullptr;
    state.nof_rx_ports    = 0;
  }
}

ocudu_dapp_status_v1 equalize(const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output,
                              equalizer_worker_state& worker) noexcept
{
  cuda_contract::inputs validated{};
  if (worker.noise_variances == nullptr || !cuda_contract::validate_inputs(input, validated) ||
      validated.nof_rx_ports > worker.nof_rx_ports ||
      !cuda_contract::device_tensor(output.channel_estimates, OCUDU_DAPP_ELEMENT_CF32_V1, 4, OCUDU_DAPP_ACCESS_WRITE_V1,
                                    sizeof(float2)) ||
      output.channel_estimates.dimensions[0] != validated.nof_rx_ports ||
      output.channel_estimates.dimensions[1] != validated.nof_layers ||
      output.channel_estimates.dimensions[2] != validated.nof_symbols ||
      output.channel_estimates.dimensions[3] != validated.nof_subcarriers) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }

  const bool compact =
      cuda_contract::device_tensor(output.equalized_symbols, OCUDU_DAPP_ELEMENT_CF32_V1, 2, OCUDU_DAPP_ACCESS_WRITE_V1,
                                   sizeof(float2)) &&
      cuda_contract::device_tensor(output.post_equalization_noise, OCUDU_DAPP_ELEMENT_F32_V1, 2,
                                   OCUDU_DAPP_ACCESS_WRITE_V1, sizeof(float)) &&
      output.equalized_symbols.layout_id == OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1 &&
      output.post_equalization_noise.layout_id == OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1 &&
      output.equalized_symbols.dimensions[0] >= validated.nof_data_re &&
      output.equalized_symbols.dimensions[1] == validated.nof_layers &&
      output.post_equalization_noise.dimensions[0] >= validated.nof_data_re &&
      output.post_equalization_noise.dimensions[1] == validated.nof_layers;

  auto*          channel = cuda_contract::pointer<float2>(output.channel_estimates);
  const uint32_t shared =
      channel_setup_shared_bytes_for(validated.nof_layers, validated.nof_subcarriers, validated.nof_pilots,
                                     validated.dmrs_symbol_mask, validated.dmrs_type, validated.alloc_nof_sc);

  if (compact) {
    // Two launches, both data-RE-aware: (1) compact CE + expand-to-plane for ABI channel
    // estimates, (2) multi-block EQ over nof_data_re only (not the full grid plane).
    auto* eq_sym = cuda_contract::pointer<float2>(output.equalized_symbols);
    auto* eq_n   = cuda_contract::pointer<float>(output.post_equalization_noise);
    if (validated.nof_layers == 1) {
      estimate_channel_and_noise_kernel<1>
          <<<validated.nof_rx_ports, channel_reduction_threads, shared, validated.stream>>>(validated, channel,
                                                                                            worker.noise_variances);
      equalize_compact_kernel<1><<<(validated.nof_data_re + 127) / 128, 128, 0, validated.stream>>>(
          validated, channel, worker.noise_variances, eq_sym, eq_n);
    } else {
      estimate_channel_and_noise_kernel<2>
          <<<validated.nof_rx_ports, channel_reduction_threads, shared, validated.stream>>>(validated, channel,
                                                                                            worker.noise_variances);
      equalize_compact_kernel<2><<<(validated.nof_data_re + 127) / 128, 128, 0, validated.stream>>>(
          validated, channel, worker.noise_variances, eq_sym, eq_n);
    }
    return cuda_contract::launch_status();
  }

  if (!cuda_contract::device_tensor(output.equalized_symbols, OCUDU_DAPP_ELEMENT_CF32_V1, 3, OCUDU_DAPP_ACCESS_WRITE_V1,
                                    sizeof(float2)) ||
      !cuda_contract::device_tensor(output.post_equalization_noise, OCUDU_DAPP_ELEMENT_F32_V1, 3,
                                    OCUDU_DAPP_ACCESS_WRITE_V1, sizeof(float)) ||
      output.equalized_symbols.dimensions[0] != validated.nof_layers ||
      output.equalized_symbols.dimensions[1] != validated.nof_symbols ||
      output.equalized_symbols.dimensions[2] != validated.nof_subcarriers ||
      output.post_equalization_noise.dimensions[0] != validated.nof_layers ||
      output.post_equalization_noise.dimensions[1] != validated.nof_symbols ||
      output.post_equalization_noise.dimensions[2] != validated.nof_subcarriers) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const uint32_t count = validated.nof_symbols * validated.nof_subcarriers;
  if (validated.nof_layers == 1) {
    estimate_channel_and_noise_kernel<1>
        <<<validated.nof_rx_ports, channel_reduction_threads, shared, validated.stream>>>(validated, channel,
                                                                                          worker.noise_variances);
    equalize_kernel<1><<<(count + 127) / 128, 128, 0, validated.stream>>>(
        validated, channel, worker.noise_variances, cuda_contract::pointer<float2>(output.equalized_symbols),
        cuda_contract::pointer<float>(output.post_equalization_noise));
  } else {
    estimate_channel_and_noise_kernel<2>
        <<<validated.nof_rx_ports, channel_reduction_threads, shared, validated.stream>>>(validated, channel,
                                                                                          worker.noise_variances);
    equalize_kernel<2><<<(count + 127) / 128, 128, 0, validated.stream>>>(
        validated, channel, worker.noise_variances, cuda_contract::pointer<float2>(output.equalized_symbols),
        cuda_contract::pointer<float>(output.post_equalization_noise));
  }
  return cuda_contract::launch_status();
}

} // namespace ocudu::dapp::ref_class_a
