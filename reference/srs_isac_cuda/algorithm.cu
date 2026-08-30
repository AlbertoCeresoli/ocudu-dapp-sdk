// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/srs_isac_cuda.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cuda_runtime_api.h>
#include <limits>

namespace ocudu::dapp::reference {
namespace {

constexpr uint32_t threads_per_block = 256;

__global__ void path_tap_power_kernel(const float2* channel, uint32_t frequency_points, uint32_t rows, uint32_t taps,
                                      float* path_tap_power)
{
  const uint32_t tap = blockIdx.x;
  const uint32_t row = blockIdx.y;
  if (tap >= taps || row >= rows) {
    return;
  }
  __shared__ float real[threads_per_block];
  __shared__ float imag[threads_per_block];
  float            local_real = 0.0F;
  float            local_imag = 0.0F;
  constexpr float  two_pi     = 6.2831853071795864769F;
  for (uint32_t frequency = threadIdx.x; frequency < frequency_points; frequency += blockDim.x) {
    const float2 value = channel[row * frequency_points + frequency];
    const float  angle =
        two_pi * static_cast<float>(tap) * static_cast<float>(frequency) / static_cast<float>(frequency_points);
    float sine;
    float cosine;
    sincosf(angle, &sine, &cosine);
    local_real += value.x * cosine - value.y * sine;
    local_imag += value.x * sine + value.y * cosine;
  }
  real[threadIdx.x] = local_real;
  imag[threadIdx.x] = local_imag;
  __syncthreads();
  for (uint32_t stride = blockDim.x / 2; stride != 0; stride /= 2) {
    if (threadIdx.x < stride) {
      real[threadIdx.x] += real[threadIdx.x + stride];
      imag[threadIdx.x] += imag[threadIdx.x + stride];
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    const float scale                = 1.0F / static_cast<float>(frequency_points);
    const float re                   = real[0] * scale;
    const float im                   = imag[0] * scale;
    path_tap_power[tap * rows + row] = re * re + im * im;
  }
}

__global__ void reduce_tap_power_kernel(const float* path_tap_power, uint32_t rows, uint32_t taps, float* tap_power)
{
  const uint32_t tap = threadIdx.x + blockIdx.x * blockDim.x;
  if (tap >= taps) {
    return;
  }
  float sum = 0.0F;
  for (uint32_t row = 0; row != rows; ++row) {
    sum += path_tap_power[tap * rows + row];
  }
  tap_power[tap] = sum / static_cast<float>(rows);
}

__global__ void mean_power_kernel(const float2* channel, uint64_t samples, float* mean_power)
{
  __shared__ float partial[threads_per_block];
  double           sum = 0.0;
  for (uint64_t sample = threadIdx.x; sample < samples; sample += blockDim.x) {
    const float2 value = channel[sample];
    sum += static_cast<double>(value.x) * value.x + static_cast<double>(value.y) * value.y;
  }
  partial[threadIdx.x] = static_cast<float>(sum);
  __syncthreads();
  for (uint32_t stride = blockDim.x / 2; stride != 0; stride /= 2) {
    if (threadIdx.x < stride) {
      partial[threadIdx.x] += partial[threadIdx.x + stride];
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    *mean_power = partial[0] / static_cast<float>(samples);
  }
}

bool valid_channel_tensor(const ocudu_dapp_tensor_view_v1& tensor, uint32_t& rows, uint32_t& frequencies) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_tensor_view_v1, reserved);
  if (tensor.struct_size < minimum || tensor.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      tensor.domain != OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1 || tensor.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1 ||
      tensor.element_type != OCUDU_DAPP_ELEMENT_CF32_V1 || tensor.rank != 3 ||
      (tensor.access & OCUDU_DAPP_ACCESS_READ_V1) == 0 || tensor.native_handle == 0 || tensor.byte_offset != 0 ||
      tensor.layout_id != OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1 || tensor.layout_version != 1 ||
      tensor.allocation_id == 0 || tensor.allocation_generation == 0 || tensor.lease_token == 0 ||
      tensor.dimensions[0] == 0 || tensor.dimensions[0] > 4 || tensor.dimensions[1] == 0 || tensor.dimensions[1] > 4 ||
      tensor.dimensions[2] == 0 || tensor.dimensions[2] > std::numeric_limits<uint32_t>::max() ||
      tensor.byte_strides[2] != sizeof(float2) || tensor.byte_strides[1] != tensor.dimensions[2] * sizeof(float2) ||
      tensor.byte_strides[0] != tensor.dimensions[1] * tensor.byte_strides[1]) {
    return false;
  }
  const uint64_t path_count = tensor.dimensions[0] * tensor.dimensions[1];
  const uint64_t samples    = path_count * tensor.dimensions[2];
  if (path_count > max_srs_cuda_paths || samples > std::numeric_limits<uint64_t>::max() / sizeof(float2) ||
      tensor.byte_length != samples * sizeof(float2)) {
    return false;
  }
  rows        = static_cast<uint32_t>(path_count);
  frequencies = static_cast<uint32_t>(tensor.dimensions[2]);
  return true;
}

} // namespace

ocudu_dapp_status_v1 initialize_srs_isac_cuda_worker_state(srs_isac_cuda_worker_state& state) noexcept
{
  state                            = {};
  constexpr std::size_t path_bytes = max_reference_cir_taps * max_srs_cuda_paths * sizeof(float);
  constexpr std::size_t tap_bytes  = max_reference_cir_taps * sizeof(float);
  if (cudaMalloc(&state.device_path_tap_power, path_bytes) != cudaSuccess ||
      cudaMalloc(&state.device_tap_power, tap_bytes) != cudaSuccess ||
      cudaMalloc(&state.device_mean_power, sizeof(float)) != cudaSuccess ||
      cudaHostAlloc(&state.host_tap_power, tap_bytes, cudaHostAllocDefault) != cudaSuccess ||
      cudaHostAlloc(&state.host_mean_power, sizeof(float), cudaHostAllocDefault) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    release_srs_isac_cuda_worker_state(state);
    return OCUDU_DAPP_RESOURCE_V1;
  }
  return OCUDU_DAPP_OK_V1;
}

void release_srs_isac_cuda_worker_state(srs_isac_cuda_worker_state& state) noexcept
{
  if (state.host_mean_power != nullptr) {
    static_cast<void>(cudaFreeHost(state.host_mean_power));
  }
  if (state.host_tap_power != nullptr) {
    static_cast<void>(cudaFreeHost(state.host_tap_power));
  }
  if (state.device_mean_power != nullptr) {
    static_cast<void>(cudaFree(state.device_mean_power));
  }
  if (state.device_tap_power != nullptr) {
    static_cast<void>(cudaFree(state.device_tap_power));
  }
  if (state.device_path_tap_power != nullptr) {
    static_cast<void>(cudaFree(state.device_path_tap_power));
  }
  state = {};
}

ocudu_dapp_status_v1 run_srs_cir_cuda(const ocudu_dapp_srs_isac_input_v1& input, double noise_variance,
                                      srs_isac_cuda_worker_state& worker, srs_cir_analysis& output) noexcept
{
  output               = {};
  uint32_t rows        = 0;
  uint32_t frequencies = 0;
  if (worker.device_path_tap_power == nullptr || worker.device_tap_power == nullptr ||
      worker.device_mean_power == nullptr || worker.host_tap_power == nullptr || worker.host_mean_power == nullptr ||
      input.invocation.execution_stream == 0 || !std::isfinite(noise_variance) || noise_variance < 0.0 ||
      !valid_channel_tensor(input.channel_estimates, rows, frequencies)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const uint32_t taps    = std::min(frequencies, max_reference_cir_taps);
  const uint64_t samples = static_cast<uint64_t>(rows) * frequencies;
  auto*          stream  = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(input.invocation.execution_stream));
  const auto* channel = reinterpret_cast<const float2*>(static_cast<uintptr_t>(input.channel_estimates.native_handle));
  path_tap_power_kernel<<<dim3(taps, rows), threads_per_block, 0, stream>>>(channel, frequencies, rows, taps,
                                                                            worker.device_path_tap_power);
  reduce_tap_power_kernel<<<1, threads_per_block, 0, stream>>>(worker.device_path_tap_power, rows, taps,
                                                               worker.device_tap_power);
  mean_power_kernel<<<1, threads_per_block, 0, stream>>>(channel, samples, worker.device_mean_power);
  if (cudaGetLastError() != cudaSuccess ||
      cudaMemcpyAsync(worker.host_tap_power, worker.device_tap_power, taps * sizeof(float), cudaMemcpyDeviceToHost,
                      stream) != cudaSuccess ||
      cudaMemcpyAsync(worker.host_mean_power, worker.device_mean_power, sizeof(float), cudaMemcpyDeviceToHost,
                      stream) != cudaSuccess ||
      cudaStreamSynchronize(stream) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return OCUDU_DAPP_DEVICE_ERROR_V1;
  }
  output.samples               = samples;
  output.channel_rows          = rows;
  output.tap_count             = taps;
  output.mean_channel_power    = *worker.host_mean_power;
  output.noise_corrected_power = std::max(0.0, output.mean_channel_power - noise_variance);
  for (uint32_t tap = 0; tap != taps; ++tap) {
    const double value = worker.host_tap_power[tap];
    if (!std::isfinite(value) || value < 0.0) {
      return OCUDU_DAPP_INVALID_OUTPUT_V1;
    }
    output.tap_power[tap] = value;
    if (value > output.peak_cir_power) {
      output.peak_cir_power = value;
      output.peak_delay_bin = tap;
    }
  }
  return std::isfinite(output.mean_channel_power) && output.mean_channel_power >= 0.0 ? OCUDU_DAPP_OK_V1
                                                                                      : OCUDU_DAPP_INVALID_OUTPUT_V1;
}

} // namespace ocudu::dapp::reference
