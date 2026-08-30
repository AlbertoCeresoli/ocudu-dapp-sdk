// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/spectrum_cuda.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cuda_runtime_api.h>
#include <limits>

namespace ocudu::dapp::reference {
namespace {

constexpr uint32_t threads_per_prb     = 128;
constexpr uint32_t subcarriers_per_prb = 12;
constexpr uint32_t maximum_plan_cells  = 14 * OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1;

__device__ float bfloat16_to_float(uint16_t value)
{
  return __uint_as_float(static_cast<uint32_t>(value) << 16U);
}

__global__ void radiometer_kernel(const uint32_t* grid, uint32_t ports, uint32_t symbols, uint32_t subcarriers,
                                  const uint8_t* expected_use, float scale_squared, uint8_t* confidence)
{
  __shared__ float    power[threads_per_prb];
  __shared__ uint32_t count[threads_per_prb];
  const uint32_t      prb              = blockIdx.x;
  float               local_power      = 0.0F;
  uint32_t            local_count      = 0;
  const uint32_t      samples_per_port = symbols * subcarriers;
  const uint32_t      samples_per_prb  = ports * symbols * subcarriers_per_prb;
  for (uint32_t item = threadIdx.x; item < samples_per_prb; item += blockDim.x) {
    const uint32_t subcarrier_offset = item % subcarriers_per_prb;
    const uint32_t row               = item / subcarriers_per_prb;
    const uint32_t symbol            = row % symbols;
    const uint32_t port              = row / symbols;
    if (expected_use != nullptr &&
        expected_use[symbol * (subcarriers / subcarriers_per_prb) + prb] != OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1) {
      continue;
    }
    const uint32_t packed =
        grid[port * samples_per_port + symbol * subcarriers + prb * subcarriers_per_prb + subcarrier_offset];
    const float real = bfloat16_to_float(static_cast<uint16_t>(packed));
    const float imag = bfloat16_to_float(static_cast<uint16_t>(packed >> 16U));
    local_power += real * real + imag * imag;
    ++local_count;
  }
  power[threadIdx.x] = local_power;
  count[threadIdx.x] = local_count;
  __syncthreads();
  for (uint32_t stride = blockDim.x / 2; stride != 0; stride /= 2) {
    if (threadIdx.x < stride) {
      power[threadIdx.x] += power[threadIdx.x + stride];
      count[threadIdx.x] += count[threadIdx.x + stride];
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    const float mean = count[0] == 0 ? 0.0F : power[0] * scale_squared / static_cast<float>(count[0]);
    confidence[prb]  = isfinite(mean) && mean > 1.0F ? UINT8_MAX : 0;
  }
}

bool valid_grid(const ocudu_dapp_tensor_view_v1& grid, uint64_t& samples) noexcept
{
  if (grid.struct_size < offsetof(ocudu_dapp_tensor_view_v1, reserved) || grid.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      grid.domain != OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1 || grid.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1 ||
      grid.element_type != OCUDU_DAPP_ELEMENT_CBF16_V1 || grid.rank != 3 ||
      (grid.access & OCUDU_DAPP_ACCESS_READ_V1) == 0 || grid.native_handle == 0 || grid.byte_offset != 0 ||
      grid.allocation_id == 0 || grid.allocation_generation == 0 || grid.lease_token == 0 ||
      grid.layout_id != OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1 || grid.layout_version != 1 ||
      grid.dimensions[0] == 0 || grid.dimensions[1] == 0 || grid.dimensions[1] > 14 || grid.dimensions[2] == 0 ||
      grid.dimensions[2] % subcarriers_per_prb != 0 ||
      grid.dimensions[2] / subcarriers_per_prb > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 ||
      grid.byte_strides[2] != sizeof(uint32_t) || grid.byte_strides[1] != grid.dimensions[2] * sizeof(uint32_t) ||
      grid.byte_strides[0] != grid.dimensions[1] * grid.byte_strides[1] ||
      grid.dimensions[0] > std::numeric_limits<uint64_t>::max() / grid.dimensions[1] ||
      grid.dimensions[0] * grid.dimensions[1] > std::numeric_limits<uint64_t>::max() / grid.dimensions[2]) {
    return false;
  }
  samples = grid.dimensions[0] * grid.dimensions[1] * grid.dimensions[2];
  return grid.byte_length == samples * sizeof(uint32_t);
}

bool valid_expected_use(const ocudu_dapp_tensor_view_v1& mask, uint32_t symbols, uint32_t prbs) noexcept
{
  return mask.struct_size >= offsetof(ocudu_dapp_tensor_view_v1, reserved) &&
         mask.abi_major == OCUDU_DAPP_ABI_MAJOR_V1 && mask.domain == OCUDU_DAPP_MEM_HOST_V1 &&
         mask.backend_id == OCUDU_DAPP_BACKEND_HOST_V1 && mask.element_type == OCUDU_DAPP_ELEMENT_U8_V1 &&
         mask.rank == 2 &&
         (mask.access & (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1)) ==
             (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) &&
         mask.native_handle != 0 && mask.byte_offset == 0 && mask.dimensions[0] == symbols &&
         mask.dimensions[1] == prbs && mask.byte_strides[0] == prbs && mask.byte_strides[1] == 1 &&
         mask.byte_length == static_cast<uint64_t>(symbols) * prbs &&
         mask.layout_id == OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1 && mask.layout_version == 1 &&
         mask.allocation_id != 0 && mask.allocation_generation != 0 && mask.lease_token != 0;
}

} // namespace

ocudu_dapp_status_v1 initialize_spectrum_cuda_worker_state(spectrum_cuda_worker_state& state) noexcept
{
  state = {};
  if (cudaMalloc(&state.device_expected_use, maximum_plan_cells) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return OCUDU_DAPP_RESOURCE_V1;
  }
  if (cudaMalloc(&state.device_confidence, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1) != cudaSuccess) {
    static_cast<void>(cudaFree(state.device_expected_use));
    state = {};
    static_cast<void>(cudaGetLastError());
    return OCUDU_DAPP_RESOURCE_V1;
  }
  if (cudaHostAlloc(&state.host_confidence, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1, cudaHostAllocDefault) != cudaSuccess) {
    static_cast<void>(cudaFree(state.device_confidence));
    static_cast<void>(cudaFree(state.device_expected_use));
    state = {};
    static_cast<void>(cudaGetLastError());
    return OCUDU_DAPP_RESOURCE_V1;
  }
  return OCUDU_DAPP_OK_V1;
}

void release_spectrum_cuda_worker_state(spectrum_cuda_worker_state& state) noexcept
{
  if (state.host_confidence != nullptr) {
    static_cast<void>(cudaFreeHost(state.host_confidence));
  }
  if (state.device_expected_use != nullptr) {
    static_cast<void>(cudaFree(state.device_expected_use));
  }
  if (state.device_confidence != nullptr) {
    static_cast<void>(cudaFree(state.device_confidence));
  }
  if (state.device_grid_snap != nullptr) {
    static_cast<void>(cudaFree(state.device_grid_snap));
  }
  state = {};
}

ocudu_dapp_status_v1 run_spectrum_radiometer_cuda(const ocudu_dapp_spectrum_input_v1& input, float amplitude_scale,
                                                  spectrum_cuda_worker_state& worker,
                                                  spectrum_cuda_result&       output) noexcept
{
  output           = {};
  uint64_t samples = 0;
  if (worker.device_expected_use == nullptr || worker.device_confidence == nullptr ||
      worker.host_confidence == nullptr || input.invocation.execution_stream == 0 || !std::isfinite(amplitude_scale) ||
      amplitude_scale <= 0.0F || !valid_grid(input.uplink_resource_grid, samples)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const uint32_t symbols      = input.uplink_resource_grid.dimensions[1];
  const uint32_t prbs         = input.uplink_resource_grid.dimensions[2] / subcarriers_per_prb;
  const bool     plan_present = input.scheduled_re_mask.struct_size != 0;
  if (plan_present && !valid_expected_use(input.scheduled_re_mask, symbols, prbs)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto           stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(input.invocation.execution_stream));
  const uint8_t* expected_use = nullptr;
  if (plan_present) {
    const auto* source =
        reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(input.scheduled_re_mask.native_handle));
    if (cudaMemcpyAsync(worker.device_expected_use, source, input.scheduled_re_mask.byte_length, cudaMemcpyHostToDevice,
                        stream) != cudaSuccess) {
      return OCUDU_DAPP_DEVICE_ERROR_V1;
    }
    expected_use = worker.device_expected_use;
  }
  const auto* live_grid =
      reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(input.uplink_resource_grid.native_handle));
  {
    cudaPointerAttributes attrs{};
    if (cudaPointerGetAttributes(&attrs, live_grid) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return OCUDU_DAPP_DEVICE_ERROR_V1;
    }
    if (attrs.type == cudaMemoryTypeHost || attrs.type == cudaMemoryTypeUnregistered) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
  }
  const size_t grid_bytes = static_cast<size_t>(samples) * sizeof(uint32_t);
  if (worker.device_grid_snap == nullptr || worker.device_grid_snap_bytes < grid_bytes) {
    if (worker.device_grid_snap != nullptr) {
      static_cast<void>(cudaFree(worker.device_grid_snap));
      worker.device_grid_snap       = nullptr;
      worker.device_grid_snap_bytes = 0;
    }
    if (cudaMalloc(reinterpret_cast<void**>(&worker.device_grid_snap), grid_bytes) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return OCUDU_DAPP_RESOURCE_V1;
    }
    worker.device_grid_snap_bytes = grid_bytes;
  }
  if (cudaMemcpyAsync(worker.device_grid_snap, live_grid, grid_bytes, cudaMemcpyDeviceToDevice, stream) !=
      cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return OCUDU_DAPP_DEVICE_ERROR_V1;
  }
  radiometer_kernel<<<prbs, threads_per_prb, 0, stream>>>(worker.device_grid_snap,
                                                          input.uplink_resource_grid.dimensions[0], symbols,
                                                          input.uplink_resource_grid.dimensions[2], expected_use,
                                                          amplitude_scale * amplitude_scale, worker.device_confidence);
  if (cudaGetLastError() != cudaSuccess ||
      cudaMemcpyAsync(worker.host_confidence, worker.device_confidence, prbs, cudaMemcpyDeviceToHost, stream) !=
          cudaSuccess ||
      cudaStreamSynchronize(stream) != cudaSuccess) {
    return OCUDU_DAPP_DEVICE_ERROR_V1;
  }
  output.confidence    = worker.host_confidence;
  output.nof_prbs      = prbs;
  output.samples       = samples;
  output.occupied_prbs = static_cast<uint32_t>(
      std::count_if(worker.host_confidence, worker.host_confidence + prbs, [](uint8_t value) { return value != 0; }));
  return OCUDU_DAPP_OK_V1;
}

} // namespace ocudu::dapp::reference
