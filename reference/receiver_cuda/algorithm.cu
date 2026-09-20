// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/class_a_cuda_contract.cuh"
#include "ocudu_dapp_sdk/receiver_cuda.h"
#include "ocudu_dapp_sdk/receiver_symbol_storage.h"
#include "ocudu_dapp_sdk/ref_class_a_cuda.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cuda_fp16.h>

namespace ocudu::dapp::ref_class_a {
namespace {

namespace cuda_contract = ocudu::dapp::reference::cuda_contract;
using cuda_contract::channel_reduction_threads;
using cuda_contract::channel_setup_shared_bytes_for;
using cuda_contract::equalize;
using cuda_contract::estimate_channels_compact;
using cuda_contract::estimate_noise_compact;
using cuda_contract::inputs;
using cuda_contract::minimum_soft_demap_noise;
using cuda_contract::pusch_metadata;

constexpr uint32_t levels_per_order = 16;
constexpr uint32_t nof_orders       = 4;
constexpr uint32_t receiver_threads = 128;
constexpr float    invalid_re_noise = 1.0e6F;

const ocudu_dapp_receiver_metrics_v1* receiver_metrics(const ocudu_dapp_typed_metadata_v1& wrapper) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_receiver_metrics_v1, reserved);
  if (wrapper.struct_size < sizeof(wrapper) || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1 || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < minimum ||
      reinterpret_cast<uintptr_t>(wrapper.metadata.data) % alignof(ocudu_dapp_receiver_metrics_v1) != 0) {
    return nullptr;
  }
  const auto* metrics = static_cast<const ocudu_dapp_receiver_metrics_v1*>(wrapper.metadata.data);
  return metrics->struct_size >= minimum && metrics->struct_size <= wrapper.metadata.size &&
                 metrics->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 ? metrics : nullptr;
}

template <uint32_t Bits>
__device__ void demap_axis(const float* levels, float value, float noise, uint32_t axis, __half* llrs)
{
  static_assert(Bits >= 1 && Bits <= 4);
  levels += (Bits - 1U) * levels_per_order;
  float minimum[Bits][2];
#pragma unroll
  for (uint32_t bit = 0; bit != Bits; ++bit) {
    minimum[bit][0] = 1.0e30F;
    minimum[bit][1] = 1.0e30F;
  }
#pragma unroll
  for (uint32_t label = 0; label != (1U << Bits); ++label) {
    const float delta    = value - levels[label];
    const float distance = delta * delta;
#pragma unroll
    for (uint32_t bit = 0; bit != Bits; ++bit) {
      const uint32_t label_value = (label >> bit) & 1U;
      minimum[bit][label_value]  = fminf(minimum[bit][label_value], distance);
    }
  }
#pragma unroll
  for (uint32_t bit = 0; bit != Bits; ++bit) {
    llrs[axis + 2U * bit] = __float2half((minimum[bit][1] - minimum[bit][0]) / fmaxf(noise, minimum_soft_demap_noise));
  }
}

/// Compact CE only: H storage is [port][layer][sc], no full-plane expand.
template <uint32_t Layers>
__global__ void estimate_channel_and_noise_compact_kernel(inputs input, float2* compact_channel, float* noise)
{
  static_assert(Layers == 1 || Layers == 2);
  extern __shared__ unsigned char storage[];
  const uint32_t                  port   = blockIdx.x;
  const uint32_t                  nsc    = input.nof_subcarriers;
  float2*                         port_h = &compact_channel[port * Layers * nsc];
  float*                          phase  = &noise[input.nof_rx_ports + port * Layers];
  estimate_channels_compact<Layers>(input, port, storage, port_h, nsc, phase);
  __syncthreads();
  estimate_noise_compact<Layers>(input, port, port_h, phase, storage, &noise[port], 1);
}

template <uint32_t ModulationOrder, uint32_t Layers>
__global__ void receiver_kernel(inputs input, const float2* compact_channel, const float* port_noise,
                                const float* phase_slopes, const float* levels, __half* llrs, float* post_eq_noise,
                                float2* equalized_symbols)
{
  static_assert(Layers == 1 || Layers == 2);
  const uint32_t data_index = blockIdx.x * blockDim.x + threadIdx.x;
  if (data_index >= input.nof_data_re) {
    return;
  }
  const uint32_t plane = input.nof_symbols * input.nof_subcarriers;
  const uint32_t nsc   = input.nof_subcarriers;
  const uint32_t index = input.data_indices[data_index];
  if (index >= plane) {
    for (uint32_t value = 0; value != Layers * ModulationOrder; ++value) {
      llrs[(data_index * Layers * ModulationOrder) + value] = __float2half(0.0F);
    }
    if (post_eq_noise != nullptr) {
      for (uint32_t layer = 0; layer != Layers; ++layer) {
        post_eq_noise[data_index * Layers + layer] = invalid_re_noise;
      }
    }
    if (equalized_symbols != nullptr) {
      for (uint32_t layer = 0; layer != Layers; ++layer) {
        equalized_symbols[data_index * Layers + layer] = make_float2(0.0F, 0.0F);
      }
    }
    return;
  }
  // Compact H: channel_stride = nsc, CompactH = true.
  const auto result = equalize<Layers, true>(input, compact_channel, nsc, port_noise, index, phase_slopes);
  for (uint32_t layer = 0; layer != Layers; ++layer) {
    __half* output = llrs + (data_index * Layers + layer) * ModulationOrder;
    demap_axis<ModulationOrder / 2U>(levels, result.symbols[layer].x, result.noise[layer], 0, output);
    demap_axis<ModulationOrder / 2U>(levels, result.symbols[layer].y, result.noise[layer], 1, output);
    if (post_eq_noise != nullptr) {
      post_eq_noise[data_index * Layers + layer] = result.noise[layer];
    }
    if (equalized_symbols != nullptr) {
      equalized_symbols[data_index * Layers + layer] = result.symbols[layer];
    }
  }
}

template <uint32_t Layers>
void launch_receiver(const inputs& input, uint32_t modulation, const float2* channel, const float* noise,
                     const float* phase_slopes, const float* levels, __half* llrs, float* post_eq_noise,
                     float2* equalized_symbols)
{
  const uint32_t blocks = (input.nof_data_re + receiver_threads - 1U) / receiver_threads;
  switch (modulation) {
    case OCUDU_DAPP_MODULATION_QPSK_V1:
      receiver_kernel<2, Layers><<<blocks, receiver_threads, 0, input.stream>>>(input, channel, noise, phase_slopes,
                                                                                levels, llrs, post_eq_noise, equalized_symbols);
      break;
    case OCUDU_DAPP_MODULATION_QAM16_V1:
      receiver_kernel<4, Layers><<<blocks, receiver_threads, 0, input.stream>>>(input, channel, noise, phase_slopes,
                                                                                levels, llrs, post_eq_noise, equalized_symbols);
      break;
    case OCUDU_DAPP_MODULATION_QAM64_V1:
      receiver_kernel<6, Layers><<<blocks, receiver_threads, 0, input.stream>>>(input, channel, noise, phase_slopes,
                                                                                levels, llrs, post_eq_noise, equalized_symbols);
      break;
    case OCUDU_DAPP_MODULATION_QAM256_V1:
      receiver_kernel<8, Layers><<<blocks, receiver_threads, 0, input.stream>>>(input, channel, noise, phase_slopes,
                                                                                levels, llrs, post_eq_noise, equalized_symbols);
      break;
  }
}

} // namespace

namespace cuda_contract = ocudu::dapp::reference::cuda_contract;

ocudu_dapp_status_v1 initialize_receiver(receiver_state& state, const float* levels) noexcept
{
  if (state.constellation_levels != nullptr) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  ocudu::dapp::reference::receiver_cuda_model_artifact_v1 default_artifact;
  if (levels == nullptr) {
    default_artifact = ocudu::dapp::reference::make_receiver_cuda_model_artifact_v1();
    levels           = default_artifact.constellation_levels;
  }
  float*           device_levels = nullptr;
  constexpr size_t level_bytes   = levels_per_order * nof_orders * sizeof(float);
  if (cudaMalloc(&device_levels, level_bytes) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return OCUDU_DAPP_RESOURCE_V1;
  }
  if (cudaMemcpy(device_levels, levels, level_bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    static_cast<void>(cudaFree(device_levels));
    return OCUDU_DAPP_RESOURCE_V1;
  }
  state.constellation_levels = device_levels;
  return OCUDU_DAPP_OK_V1;
}

void release_receiver(receiver_state& state) noexcept
{
  if (state.constellation_levels != nullptr) {
    static_cast<void>(cudaFree(const_cast<float*>(state.constellation_levels)));
    state.constellation_levels = nullptr;
  }
}

ocudu_dapp_status_v1 initialize_receiver_worker(receiver_worker_state& state, uint32_t max_nof_rx_ports,
                                                uint32_t max_nof_layers) noexcept
{
  if (state.storage != nullptr || max_nof_rx_ports == 0 || max_nof_rx_ports > 64 || max_nof_layers == 0 ||
      max_nof_layers > 2) {
    return state.storage != nullptr ? OCUDU_DAPP_CONFLICT_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
  }
  // Seed allocation; compact H is sized on first invocation from grid width.
  const size_t bytes = max_nof_rx_ports * sizeof(float);
  if (cudaMalloc(&state.storage, bytes) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return OCUDU_DAPP_RESOURCE_V1;
  }
  state.nof_rx_ports  = max_nof_rx_ports;
  state.nof_layers    = max_nof_layers;
  state.storage_bytes = bytes;
  return OCUDU_DAPP_OK_V1;
}

void release_receiver_worker(receiver_worker_state& state) noexcept
{
  if (state.storage != nullptr) {
    static_cast<void>(cudaFree(state.storage));
    state.storage       = nullptr;
    state.nof_rx_ports  = 0;
    state.nof_layers    = 0;
    state.storage_bytes = 0;
  }
}

ocudu_dapp_status_v1 receive(const ocudu_dapp_receiver_input_v1& input, ocudu_dapp_receiver_output_v1& output,
                             const receiver_state& state, receiver_worker_state& worker) noexcept
{
  output.reserved0 = 0;
  cuda_contract::inputs validated{};
  const auto*           pusch             = cuda_contract::pusch_metadata(input.pusch);
  const auto*           metrics           = receiver_metrics(output.metrics);
  const bool            metrics_requested = output.metrics.metadata.data != nullptr;
  const auto* symbol_output = metrics != nullptr &&
                                     metrics->struct_size >= OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_MIN_SIZE_V1 &&
                                     metrics->equalized_symbols.struct_size != 0
                                 ? &metrics->equalized_symbols : nullptr;
  if (state.constellation_levels == nullptr || worker.storage == nullptr ||
      !cuda_contract::validate_inputs(input, validated) || validated.nof_rx_ports > worker.nof_rx_ports ||
      validated.nof_layers > worker.nof_layers || pusch == nullptr ||
      (pusch->modulation != OCUDU_DAPP_MODULATION_QPSK_V1 && pusch->modulation != OCUDU_DAPP_MODULATION_QAM16_V1 &&
       pusch->modulation != OCUDU_DAPP_MODULATION_QAM64_V1 && pusch->modulation != OCUDU_DAPP_MODULATION_QAM256_V1)) {
    return OCUDU_DAPP_UNSUPPORTED_V1;
  }
  if (!cuda_contract::device_tensor(output.llrs, OCUDU_DAPP_ELEMENT_F16_V1, 3, OCUDU_DAPP_ACCESS_WRITE_V1,
                                    sizeof(__half)) ||
      output.llrs.layout_id != OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1 ||
      output.llrs.dimensions[0] < validated.nof_data_re || output.llrs.dimensions[1] != validated.nof_layers ||
      output.llrs.dimensions[2] != pusch->modulation ||
      (metrics_requested &&
       (metrics == nullptr ||
        !cuda_contract::device_tensor(metrics->post_equalization_noise, OCUDU_DAPP_ELEMENT_F32_V1, 2,
                                      OCUDU_DAPP_ACCESS_WRITE_V1, sizeof(float)) ||
        metrics->post_equalization_noise.layout_id != OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1 ||
        metrics->post_equalization_noise.dimensions[0] < validated.nof_data_re ||
        metrics->post_equalization_noise.dimensions[1] != validated.nof_layers))) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  if (symbol_output != nullptr &&
      (!cuda_contract::device_tensor(*symbol_output, OCUDU_DAPP_ELEMENT_CF32_V1, 2,
                                     OCUDU_DAPP_ACCESS_WRITE_V1, sizeof(float2)) ||
       symbol_output->abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
       symbol_output->layout_id != OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1 ||
       symbol_output->layout_version != 1 || symbol_output->device_ordinal != output.llrs.device_ordinal ||
       symbol_output->dimensions[0] != validated.nof_data_re ||
       symbol_output->dimensions[1] != validated.nof_layers ||
       !ocudu::dapp::reference::receiver_symbol_storage_is_disjoint(
           *symbol_output, input, output, *pusch, *cuda_contract::dmrs_metadata(input.dmrs),
           metrics->post_equalization_noise))) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }

  // Compact H only: [port][layer][sc] — no full symbol×sc plane.
  const size_t channel_bytes =
      static_cast<size_t>(validated.nof_rx_ports) * validated.nof_layers * validated.nof_subcarriers * sizeof(float2);
  const size_t noise_bytes = static_cast<size_t>(validated.nof_rx_ports) * sizeof(float);
  const size_t phase_bytes = static_cast<size_t>(validated.nof_rx_ports) * validated.nof_layers * sizeof(float);
  const size_t need_bytes  = channel_bytes + noise_bytes + phase_bytes;
  if (need_bytes > worker.storage_bytes) {
    void* grown = nullptr;
    if (cudaMalloc(&grown, need_bytes) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return OCUDU_DAPP_RESOURCE_V1;
    }
    static_cast<void>(cudaFree(worker.storage));
    worker.storage       = grown;
    worker.storage_bytes = need_bytes;
  }

  auto* channel = static_cast<float2*>(worker.storage);
  auto* noise   = reinterpret_cast<float*>(static_cast<unsigned char*>(worker.storage) + channel_bytes);
  auto* phase_slopes =
      reinterpret_cast<float*>(static_cast<unsigned char*>(worker.storage) + channel_bytes + noise_bytes);
  auto* llrs          = cuda_contract::pointer<__half>(output.llrs);
  auto* post_eq_noise = metrics != nullptr ? cuda_contract::pointer<float>(metrics->post_equalization_noise) : nullptr;
  auto* equalized_symbols = symbol_output != nullptr ? cuda_contract::pointer<float2>(*symbol_output) : nullptr;
  const uint32_t shared =
      channel_setup_shared_bytes_for(validated.nof_layers, validated.nof_subcarriers, validated.nof_pilots,
                                     validated.dmrs_symbol_mask, validated.dmrs_type, validated.alloc_nof_sc);

  // Compact CE (no plane expand) + multi-block demap over data REs only.
  if (validated.nof_layers == 1) {
    estimate_channel_and_noise_compact_kernel<1>
        <<<validated.nof_rx_ports, channel_reduction_threads, shared, validated.stream>>>(validated, channel, noise);
    launch_receiver<1>(validated, pusch->modulation, channel, noise, phase_slopes, state.constellation_levels, llrs,
                       post_eq_noise, equalized_symbols);
  } else {
    estimate_channel_and_noise_compact_kernel<2>
        <<<validated.nof_rx_ports, channel_reduction_threads, shared, validated.stream>>>(validated, channel, noise);
    launch_receiver<2>(validated, pusch->modulation, channel, noise, phase_slopes, state.constellation_levels, llrs,
                       post_eq_noise, equalized_symbols);
  }
  const auto status = cuda_contract::launch_status();
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  if (metrics != nullptr) {
    output.reserved0 |= OCUDU_DAPP_RECEIVER_METRICS_VALID_V1;
  }
  if (equalized_symbols != nullptr) {
    output.reserved0 |= OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1;
  }
  return status;
}

} // namespace ocudu::dapp::ref_class_a
