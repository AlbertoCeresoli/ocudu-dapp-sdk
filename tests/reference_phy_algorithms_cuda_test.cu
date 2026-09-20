// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/class_a_cuda_contract.cuh"
#include "ocudu_dapp_sdk/reference_phy_algorithms.h"
#include "ocudu_dapp_sdk/reference_phy_algorithms_cuda.h"
#include <array>
#include <cmath>
#include <cstring>
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <initializer_list>
#include <vector>

using namespace ocudu::dapp::reference;

namespace {

uint32_t pack_cbf16(float real, float imaginary)
{
  const __nv_bfloat162 packed = __floats2bfloat162_rn(real, imaginary);
  uint32_t             result;
  static_assert(sizeof(result) == sizeof(packed));
  std::memcpy(&result, &packed, sizeof(result));
  return result;
}

float2 unpack_cbf16(uint32_t packed)
{
  __nv_bfloat162 value;
  static_assert(sizeof(value) == sizeof(packed));
  std::memcpy(&value, &packed, sizeof(value));
  return {__bfloat162float(value.x), __bfloat162float(value.y)};
}

float axis_level(uint32_t label, uint32_t bits)
{
  float amplitude = 1.0F;
  float scale     = 2.0F;
  for (uint32_t bit = bits; bit > 1; --bit) {
    amplitude = scale - (((label >> (bit - 1)) & 1U) == 0 ? 1.0F : -1.0F) * amplitude;
    scale *= 2.0F;
  }
  const float    sign       = (label & 1U) == 0 ? 1.0F : -1.0F;
  const uint32_t modulation = 1U << (2U * bits);
  return sign * amplitude / std::sqrt(2.0F * static_cast<float>(modulation - 1U) / 3.0F);
}

template <typename T>
ocudu_dapp_tensor_view_v1 device_tensor(T* pointer, uint32_t element, uint32_t access,
                                        std::initializer_list<uint64_t> dimensions, uint32_t layout = 0)
{
  ocudu_dapp_tensor_view_v1 result{};
  result.struct_size    = sizeof(result);
  result.abi_major      = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor      = OCUDU_DAPP_ABI_MINOR_V1;
  result.domain         = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  result.backend_id     = OCUDU_DAPP_BACKEND_CUDA_V1;
  result.element_type   = element;
  result.rank           = dimensions.size();
  result.access         = access;
  result.native_handle  = reinterpret_cast<uintptr_t>(pointer);
  result.layout_id      = layout;
  result.layout_version = 1;
  uint64_t elements     = 1;
  uint32_t index        = 0;
  for (uint64_t dimension : dimensions) {
    result.dimensions[index++] = dimension;
    elements *= dimension;
  }
  uint64_t stride = sizeof(T);
  for (uint32_t dimension = result.rank; dimension != 0; --dimension) {
    result.byte_strides[dimension - 1] = stride;
    stride *= result.dimensions[dimension - 1];
  }
  result.byte_length = elements * sizeof(T);
  return result;
}

template <typename T>
ocudu_dapp_tensor_view_v1 host_tensor(T* pointer, uint32_t element, uint32_t access,
                                      std::initializer_list<uint64_t> dimensions, uint32_t layout = 0)
{
  auto result       = device_tensor(pointer, element, access, dimensions, layout);
  result.domain     = OCUDU_DAPP_MEM_HOST_V1;
  result.backend_id = OCUDU_DAPP_BACKEND_HOST_V1;
  result.access |= OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  return result;
}

template <typename T>
T* allocate(size_t count)
{
  T* result = nullptr;
  EXPECT_EQ(cudaMalloc(&result, count * sizeof(T)), cudaSuccess);
  return result;
}

struct resident_fixture {
  resident_fixture()
  {
    EXPECT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
    EXPECT_EQ(initialize_equalizer_cuda_worker_state(equalizer_worker), OCUDU_DAPP_OK_V1);
    EXPECT_EQ(initialize_receiver_cuda_state(receiver_state), OCUDU_DAPP_OK_V1);
    EXPECT_EQ(initialize_receiver_cuda_worker_state(receiver_worker), OCUDU_DAPP_OK_V1);
    d_grid        = allocate<uint32_t>(grid.size());
    d_references  = allocate<float2>(references.size());
    d_coordinates = allocate<uint16_t>(coordinates.size());
    d_indices     = allocate<uint32_t>(indices.size());

    EXPECT_EQ(cudaMemcpyAsync(d_grid, grid.data(), grid.size() * sizeof(uint32_t), cudaMemcpyHostToDevice, stream),
              cudaSuccess);
    EXPECT_EQ(cudaMemcpyAsync(d_references, references.data(), references.size() * sizeof(float2),
                              cudaMemcpyHostToDevice, stream),
              cudaSuccess);
    EXPECT_EQ(cudaMemcpyAsync(d_coordinates, coordinates.data(), coordinates.size() * sizeof(uint16_t),
                              cudaMemcpyHostToDevice, stream),
              cudaSuccess);
    EXPECT_EQ(
        cudaMemcpyAsync(d_indices, indices.data(), indices.size() * sizeof(uint32_t), cudaMemcpyHostToDevice, stream),
        cudaSuccess);

    pusch.struct_size          = sizeof(pusch);
    pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
    pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
    pusch.nof_rx_ports         = 1;
    pusch.nof_layers           = 1;
    pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
    pusch.nof_symbols          = 2;
    pusch.nof_subcarriers      = 4;
    pusch.noise_variance_floor = 0.01F;
    pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
    pusch.nof_data_re          = static_cast<uint32_t>(indices.size());
    pusch.data_re_indices      = device_tensor(d_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                               {indices.size()}, OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);

    dmrs.struct_size = sizeof(dmrs);
    dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    dmrs.nof_pilots  = 1;
    dmrs.dmrs_type   = 1;
    dmrs.flags =
        OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
    dmrs.reference_symbols = device_tensor(d_references, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, 1});
    dmrs.coordinates       = device_tensor(d_coordinates, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, 2});

    invocation.struct_size      = sizeof(invocation);
    invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
    invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
    invocation.execution_stream = reinterpret_cast<uintptr_t>(stream);
    grid_view  = device_tensor(d_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, 2, 4},
                               OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
    pusch_view = {
        sizeof(pusch_view),        OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_PUSCH_V1, 1,
        {&pusch, sizeof(pusch), 0}};
    dmrs_view = {
        sizeof(dmrs_view),       OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_DMRS_V1, 1,
        {&dmrs, sizeof(dmrs), 0}};
  }

  ~resident_fixture()
  {
    cudaStreamSynchronize(stream);
    release_equalizer_cuda_worker_state(equalizer_worker);
    release_receiver_cuda_worker_state(receiver_worker);
    release_receiver_cuda_state(receiver_state);
    cudaFree(d_indices);
    cudaFree(d_coordinates);
    cudaFree(d_references);
    cudaFree(d_grid);
    cudaStreamDestroy(stream);
  }

  std::array<uint32_t, 8>             grid{pack_cbf16(2.0F, 0.0F),   pack_cbf16(2.0F, 2.0F),  pack_cbf16(-2.0F, 2.0F),
                               pack_cbf16(-2.0F, -2.0F), pack_cbf16(2.0F, -2.0F), pack_cbf16(2.0F, 2.0F),
                               pack_cbf16(-2.0F, 2.0F),  pack_cbf16(2.0F, -2.0F)};
  std::array<float2, 1>               references{{{1.0F, 0.0F}}};
  std::array<uint16_t, 2>             coordinates{{0, 0}};
  std::array<uint32_t, 7>             indices{{1, 2, 3, 4, 5, 6, 7}};
  cudaStream_t                        stream{};
  uint32_t*                           d_grid{};
  float2*                             d_references{};
  uint16_t*                           d_coordinates{};
  uint32_t*                           d_indices{};
  ocudu_dapp_pusch_metadata_v1        pusch{};
  ocudu_dapp_dmrs_metadata_v1         dmrs{};
  ocudu_dapp_invocation_v1            invocation{};
  ocudu_dapp_tensor_view_v1           grid_view{};
  ocudu_dapp_typed_metadata_v1        pusch_view{};
  ocudu_dapp_typed_metadata_v1        dmrs_view{};
  channel_estimator_cuda_state        channel_estimator_state{};
  channel_estimator_cuda_worker_state channel_estimator_worker{};
  equalizer_cuda_state                equalizer_state{};
  receiver_cuda_state                 receiver_state{};
  receiver_cuda_worker_state          receiver_worker{};
  equalizer_cuda_worker_state         equalizer_worker{};
};

TEST(reference_phy_algorithms_cuda, equalizer_scratch_is_persistent_per_worker_and_bounded)
{
  equalizer_cuda_worker_state worker{};
  EXPECT_EQ(initialize_equalizer_cuda_worker_state(worker, 0), OCUDU_DAPP_INVALID_INPUT_V1);
  ASSERT_EQ(initialize_equalizer_cuda_worker_state(worker, 2), OCUDU_DAPP_OK_V1);
  ASSERT_NE(worker.noise_variances, nullptr);
  EXPECT_EQ(worker.nof_rx_ports, 2U);
  EXPECT_EQ(initialize_equalizer_cuda_worker_state(worker, 2), OCUDU_DAPP_CONFLICT_V1);
  release_equalizer_cuda_worker_state(worker);
  EXPECT_EQ(worker.noise_variances, nullptr);
  EXPECT_EQ(worker.nof_rx_ports, 0U);
  release_equalizer_cuda_worker_state(worker);
}

TEST(reference_phy_algorithms_cuda, fd_smoothing_shared_storage_is_bounded_for_full_nr_bandwidth)
{
  constexpr uint32_t nof_subcarriers      = 275U * 12U;
  constexpr uint32_t pilots_per_dmrs      = 275U * 6U;
  constexpr uint32_t nof_pilots           = 2U * pilots_per_dmrs;
  constexpr uint64_t two_dmrs_mask        = (uint64_t{1} << 2U) | (uint64_t{1} << 11U);
  constexpr uint32_t default_shared_limit = 48U * 1024U;
  using ocudu::dapp::reference::cuda_contract::channel_setup_shared_bytes_for;
  EXPECT_LE(channel_setup_shared_bytes_for(2, nof_subcarriers, nof_pilots, two_dmrs_mask, 1, nof_subcarriers),
            default_shared_limit);
  // Older hosts may omit the mask; the pattern-derived capacity remains
  // bounded and never causes a CPU fallback or an over-limit CUDA launch.
  EXPECT_LE(channel_setup_shared_bytes_for(2, nof_subcarriers, nof_pilots, 0, 1, nof_subcarriers),
            default_shared_limit);
}

TEST(reference_phy_algorithms_cuda, receiver_state_is_prepared_once_and_released_outside_invocation)
{
  receiver_cuda_state        state{};
  receiver_cuda_worker_state worker{};
  EXPECT_EQ(initialize_receiver_cuda_worker_state(worker, 0), OCUDU_DAPP_INVALID_INPUT_V1);
  ASSERT_EQ(initialize_receiver_cuda_worker_state(worker, 2), OCUDU_DAPP_OK_V1);
  ASSERT_NE(worker.storage, nullptr);
  EXPECT_EQ(worker.nof_rx_ports, 2U);
  EXPECT_EQ(worker.nof_layers, 2U);
  EXPECT_EQ(initialize_receiver_cuda_worker_state(worker, 2), OCUDU_DAPP_CONFLICT_V1);
  ASSERT_EQ(initialize_receiver_cuda_state(state), OCUDU_DAPP_OK_V1);
  ASSERT_NE(state.constellation_levels, nullptr);
  EXPECT_EQ(initialize_receiver_cuda_state(state), OCUDU_DAPP_CONFLICT_V1);
  release_receiver_cuda_state(state);
  EXPECT_EQ(state.constellation_levels, nullptr);
  release_receiver_cuda_state(state);
  release_receiver_cuda_worker_state(worker);
  EXPECT_EQ(worker.storage, nullptr);
  release_receiver_cuda_worker_state(worker);
}

TEST(reference_phy_algorithms_cuda, two_layer_ce_mmse_and_receiver_preserve_resident_tensor_layouts)
{
  constexpr uint32_t nof_ports       = 2;
  constexpr uint32_t nof_layers      = 2;
  constexpr uint32_t nof_symbols     = 1;
  constexpr uint32_t nof_subcarriers = 3;
  constexpr uint32_t nof_pilots      = 2;
  constexpr uint32_t data_index      = 2;

  // Correlated H = [[1, 0.5], [0.5, 1]], recovered from two OCC pilots.
  // The data RE is H * ([0.5+j0.25], [-0.25+j0.5]).
  std::array<uint32_t, nof_ports * nof_symbols * nof_subcarriers> grid{
      pack_cbf16(1.5F, 0.0F), pack_cbf16(0.5F, 0.0F),  pack_cbf16(0.375F, 0.5F),
      pack_cbf16(1.5F, 0.0F), pack_cbf16(-0.5F, 0.0F), pack_cbf16(0.0F, 0.625F)};
  std::array<float2, nof_layers * nof_pilots> references{{{1.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.0F}, {-1.0F, 0.0F}}};
  std::array<uint16_t, 2 * nof_pilots>        coordinates{{0, 0, 0, 1}};
  std::array<uint32_t, 1>                     indices{{data_index}};

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  auto* d_grid        = allocate<uint32_t>(grid.size());
  auto* d_references  = allocate<float2>(references.size());
  auto* d_coordinates = allocate<uint16_t>(coordinates.size());
  auto* d_indices     = allocate<uint32_t>(indices.size());
  auto* d_channel     = allocate<float2>(nof_ports * nof_layers * nof_symbols * nof_subcarriers);
  auto* d_ce_noise    = allocate<float>(nof_ports * nof_layers);
  auto* d_equalized   = allocate<float2>(nof_layers);
  auto* d_eq_noise    = allocate<float>(nof_layers);
  auto* d_rx_noise    = allocate<float>(nof_layers);
  auto* d_llrs        = allocate<__half>(nof_layers * 2);
  ASSERT_EQ(cudaMemcpyAsync(d_grid, grid.data(), sizeof(grid), cudaMemcpyHostToDevice, stream), cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_references, references.data(), sizeof(references), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_coordinates, coordinates.data(), sizeof(coordinates), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_indices, indices.data(), sizeof(indices), cudaMemcpyHostToDevice, stream), cudaSuccess);

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = nof_ports;
  pusch.nof_layers           = nof_layers;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = nof_symbols;
  pusch.nof_subcarriers      = nof_subcarriers;
  pusch.nof_data_re          = indices.size();
  pusch.noise_variance_floor = 0.2F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.data_re_indices      = device_tensor(d_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                             {indices.size()}, OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);
  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = nof_pilots;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reference_symbols =
      device_tensor(d_references, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_layers, nof_pilots});
  dmrs.coordinates =
      device_tensor(d_coordinates, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});
  ocudu_dapp_invocation_v1 invocation{};
  invocation.struct_size                 = sizeof(invocation);
  invocation.abi_major                   = OCUDU_DAPP_ABI_MAJOR_V1;
  invocation.abi_minor                   = OCUDU_DAPP_ABI_MINOR_V1;
  invocation.execution_stream            = reinterpret_cast<uintptr_t>(stream);
  auto                         grid_view = device_tensor(d_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                                         {nof_ports, nof_symbols, nof_subcarriers},
                                                         OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
  ocudu_dapp_typed_metadata_v1 pusch_view{
      sizeof(pusch_view),        OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_PUSCH_V1, 1,
      {&pusch, sizeof(pusch), 0}};
  ocudu_dapp_typed_metadata_v1 dmrs_view{
      sizeof(dmrs_view),       OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_DMRS_V1, 1,
      {&dmrs, sizeof(dmrs), 0}};

  ocudu_dapp_channel_estimator_input_v1  ce_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_channel_estimator_output_v1 ce_output{};
  ce_output.channel_estimates = device_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                              {nof_ports, nof_layers, nof_symbols, nof_subcarriers});
  ce_output.noise_variances =
      device_tensor(d_ce_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {nof_ports, nof_layers});
  channel_estimator_cuda_state        channel_estimator_state{};
  channel_estimator_cuda_worker_state channel_estimator_worker{};
  ASSERT_EQ(estimate_channel_cuda(ce_input, ce_output, channel_estimator_state, channel_estimator_worker),
            OCUDU_DAPP_OK_V1);

  ocudu_dapp_equalizer_input_v1  eq_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_equalizer_output_v1 eq_output{};
  eq_output.channel_estimates = ce_output.channel_estimates;
  eq_output.equalized_symbols = device_tensor(d_equalized, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                              {1, nof_layers}, OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
  eq_output.post_equalization_noise =
      device_tensor(d_eq_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, nof_layers},
                    OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
  equalizer_cuda_worker_state equalizer_worker{};
  equalizer_cuda_state        equalizer_state{};
  ASSERT_EQ(initialize_equalizer_cuda_worker_state(equalizer_worker, nof_ports), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(equalize_cuda(eq_input, eq_output, equalizer_state, equalizer_worker), OCUDU_DAPP_OK_V1);

  receiver_cuda_state        receiver_state{};
  receiver_cuda_worker_state receiver_worker{};
  ASSERT_EQ(initialize_receiver_cuda_state(receiver_state), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(initialize_receiver_cuda_worker_state(receiver_worker, nof_ports, nof_layers), OCUDU_DAPP_OK_V1);
  ocudu_dapp_receiver_input_v1  rx_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_receiver_output_v1 rx_output{};
  rx_output.llrs = device_tensor(d_llrs, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, nof_layers, 2},
                                 OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
  ocudu_dapp_receiver_metrics_v1 rx_metrics{};
  rx_metrics.struct_size = sizeof(rx_metrics);
  rx_metrics.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  rx_metrics.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  rx_metrics.post_equalization_noise =
      device_tensor(d_rx_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, nof_layers},
                    OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
  rx_output.metrics = {sizeof(rx_output.metrics),
                       OCUDU_DAPP_ABI_MAJOR_V1,
                       OCUDU_DAPP_ABI_MINOR_V1,
                       OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1,
                       1,
                       {&rx_metrics, sizeof(rx_metrics), 0}};
  ASSERT_EQ(receive_cuda(rx_input, rx_output, receiver_state, receiver_worker), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);

  std::array<float2, nof_ports * nof_layers * nof_symbols * nof_subcarriers> channel{};
  std::array<float2, nof_layers>                                             equalized{};
  std::array<float, nof_layers>                                              equalized_noise{};
  std::array<float, nof_layers>                                              receiver_noise{};
  std::array<__half, nof_layers * 2>                                         llrs{};
  ASSERT_EQ(cudaMemcpy(channel.data(), d_channel, sizeof(channel), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(equalized.data(), d_equalized, sizeof(equalized), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(equalized_noise.data(), d_eq_noise, sizeof(equalized_noise), cudaMemcpyDeviceToHost),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpy(llrs.data(), d_llrs, sizeof(llrs), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(receiver_noise.data(), d_rx_noise, sizeof(receiver_noise), cudaMemcpyDeviceToHost), cudaSuccess);
  const auto channel_at = [&](uint32_t port, uint32_t layer) {
    return channel[(port * nof_layers + layer) * nof_symbols * nof_subcarriers];
  };
  EXPECT_NEAR(channel_at(0, 0).x, 1.0F, 1.0e-5F);
  EXPECT_NEAR(channel_at(0, 1).x, 0.5F, 1.0e-5F);
  EXPECT_NEAR(channel_at(1, 0).x, 0.5F, 1.0e-5F);
  EXPECT_NEAR(channel_at(1, 1).x, 1.0F, 1.0e-5F);
  EXPECT_NEAR(equalized[0].x, 0.43846154F, 1.0e-5F);
  EXPECT_NEAR(equalized[0].y, 0.37307692F, 1.0e-5F);
  EXPECT_NEAR(equalized[1].x, -0.12692308F, 1.0e-5F);
  EXPECT_NEAR(equalized[1].y, 0.56153846F, 1.0e-5F);
  EXPECT_NEAR(equalized_noise[0], 0.35692308F, 1.0e-5F);
  EXPECT_NEAR(equalized_noise[1], 0.35692308F, 1.0e-5F);
  EXPECT_NE(rx_output.reserved0 & OCUDU_DAPP_RECEIVER_METRICS_VALID_V1, 0U);
  for (float value : receiver_noise) {
    EXPECT_TRUE(std::isfinite(value));
    EXPECT_NEAR(value, 0.35692308F, 1.0e-5F);
  }
  EXPECT_GT(__half2float(llrs[0]), 0.0F);
  EXPECT_GT(__half2float(llrs[1]), 0.0F);
  EXPECT_LT(__half2float(llrs[2]), 0.0F);
  EXPECT_GT(__half2float(llrs[3]), 0.0F);

  release_receiver_cuda_worker_state(receiver_worker);
  release_receiver_cuda_state(receiver_state);
  release_equalizer_cuda_worker_state(equalizer_worker);
  cudaFree(d_llrs);
  cudaFree(d_rx_noise);
  cudaFree(d_eq_noise);
  cudaFree(d_equalized);
  cudaFree(d_ce_noise);
  cudaFree(d_channel);
  cudaFree(d_indices);
  cudaFree(d_coordinates);
  cudaFree(d_references);
  cudaFree(d_grid);
  cudaStreamDestroy(stream);
}

TEST(reference_phy_algorithms_cuda, ce_equalizer_and_receiver_track_phase_rotation_and_dmrs_boost)
{
  constexpr uint32_t nof_symbols       = 10;
  constexpr uint32_t nof_subcarriers   = 4;
  constexpr uint32_t nof_pilots        = 4;
  constexpr uint32_t data_symbol_index = 5;
  constexpr uint32_t data_subcarrier   = 1;
  constexpr uint32_t data_index        = data_symbol_index * nof_subcarriers + data_subcarrier;
  constexpr float    phase_per_symbol  = 0.18F;
  constexpr float    dmrs_scaling      = 1.41421356237F;
  const float2       base_channel{1.5F, -0.4F};
  const float2       data_symbol{0.70710678F, 0.70710678F};
  const auto         rotate_value = [](float2 value, float phase) {
    const float c = std::cos(phase);
    const float s = std::sin(phase);
    return float2{value.x * c - value.y * s, value.x * s + value.y * c};
  };
  const auto multiply_value = [](float2 lhs, float2 rhs) {
    return float2{lhs.x * rhs.x - lhs.y * rhs.y, lhs.x * rhs.y + lhs.y * rhs.x};
  };

  std::array<uint32_t, nof_symbols * nof_subcarriers> grid{};
  for (uint32_t symbol = 0; symbol != nof_symbols; ++symbol) {
    const float  phase   = phase_per_symbol * static_cast<float>(static_cast<int32_t>(symbol) - 1);
    const float2 channel = rotate_value(base_channel, phase);
    const float2 sample  = multiply_value(channel, data_symbol);
    for (uint32_t sc = 0; sc != nof_subcarriers; ++sc) {
      grid[symbol * nof_subcarriers + sc] = pack_cbf16(sample.x, sample.y);
    }
  }
  std::array<float2, nof_pilots> references{
      {{dmrs_scaling, 0}, {dmrs_scaling, 0}, {dmrs_scaling, 0}, {dmrs_scaling, 0}}};
  std::array<uint16_t, nof_pilots * 2> coordinates{{1, 0, 1, 2, 8, 0, 8, 2}};
  for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
    const uint32_t symbol = coordinates[2 * pilot];
    const uint32_t sc     = coordinates[2 * pilot + 1];
    const float2   channel =
        rotate_value(base_channel, phase_per_symbol * static_cast<float>(static_cast<int32_t>(symbol) - 1));
    // Symmetric amplitude errors leave the fitted channel unchanged but create
    // a known non-zero residual while the boosted pilot reference must be
    // applied exactly once.
    const bool   high_amplitude         = (symbol == 1) == (sc == 0);
    const float  amplitude              = high_amplitude ? 1.25F : 0.75F;
    const float2 pilot_sample           = multiply_value(channel, references[pilot]);
    grid[symbol * nof_subcarriers + sc] = pack_cbf16(amplitude * pilot_sample.x, amplitude * pilot_sample.y);
  }
  std::array<uint32_t, 1> indices{{data_index}};

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  auto* d_grid        = allocate<uint32_t>(grid.size());
  auto* d_references  = allocate<float2>(references.size());
  auto* d_coordinates = allocate<uint16_t>(coordinates.size());
  auto* d_indices     = allocate<uint32_t>(indices.size());
  auto* d_channel     = allocate<float2>(grid.size());
  auto* d_ce_noise    = allocate<float>(1);
  auto* d_sched_noise = allocate<float>(1);
  auto* d_equalized   = allocate<float2>(1);
  auto* d_eq_noise    = allocate<float>(1);
  auto* d_llrs        = allocate<__half>(2);
  ASSERT_EQ(cudaMemcpyAsync(d_grid, grid.data(), sizeof(grid), cudaMemcpyHostToDevice, stream), cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_references, references.data(), sizeof(references), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_coordinates, coordinates.data(), sizeof(coordinates), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_indices, indices.data(), sizeof(indices), cudaMemcpyHostToDevice, stream), cudaSuccess);

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = 1;
  pusch.nof_layers           = 1;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = nof_symbols;
  pusch.nof_subcarriers      = nof_subcarriers;
  pusch.nof_data_re          = 1;
  pusch.noise_variance_floor = 1.0e-6F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.data_re_indices      = device_tensor(d_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1},
                                             OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);

  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = nof_pilots;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reserved[0] = (uint64_t{1} << 1U) | (uint64_t{1} << 8U);
  dmrs.reference_symbols =
      device_tensor(d_references, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_pilots});
  dmrs.coordinates =
      device_tensor(d_coordinates, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});

  ocudu_dapp_invocation_v1 invocation{};
  invocation.struct_size      = sizeof(invocation);
  invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  invocation.execution_stream = reinterpret_cast<uintptr_t>(stream);
  auto grid_view =
      device_tensor(d_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_symbols, nof_subcarriers},
                    OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
  ocudu_dapp_typed_metadata_v1 pusch_view{
      sizeof(pusch_view),        OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_PUSCH_V1, 1,
      {&pusch, sizeof(pusch), 0}};
  ocudu_dapp_typed_metadata_v1 dmrs_view{
      sizeof(dmrs_view),       OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_DMRS_V1, 1,
      {&dmrs, sizeof(dmrs), 0}};

  ocudu_dapp_channel_estimator_input_v1  ce_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_channel_estimator_output_v1 ce_output{};
  ce_output.channel_estimates = device_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                              {1, 1, nof_symbols, nof_subcarriers});
  ce_output.noise_variances = device_tensor(d_ce_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1});
  ce_output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_INDEX_V1] =
      OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_REQUEST_V1;
  ce_output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_HANDLE_INDEX_V1] =
      reinterpret_cast<uintptr_t>(d_sched_noise);
  ce_output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_BYTES_INDEX_V1] = sizeof(float);
  channel_estimator_cuda_state        ce_state{};
  channel_estimator_cuda_worker_state ce_worker{};
  ASSERT_EQ(estimate_channel_cuda(ce_input, ce_output, ce_state, ce_worker), OCUDU_DAPP_OK_V1);

  ocudu_dapp_equalizer_input_v1  eq_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_equalizer_output_v1 eq_output{};
  eq_output.channel_estimates       = ce_output.channel_estimates;
  eq_output.equalized_symbols       = device_tensor(d_equalized, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                                    {1, 1}, OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
  eq_output.post_equalization_noise = device_tensor(d_eq_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                                    {1, 1}, OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
  equalizer_cuda_state        eq_state{};
  equalizer_cuda_worker_state eq_worker{};
  ASSERT_EQ(initialize_equalizer_cuda_worker_state(eq_worker, 1), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(equalize_cuda(eq_input, eq_output, eq_state, eq_worker), OCUDU_DAPP_OK_V1);

  receiver_cuda_state        rx_state{};
  receiver_cuda_worker_state rx_worker{};
  ASSERT_EQ(initialize_receiver_cuda_state(rx_state), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(initialize_receiver_cuda_worker_state(rx_worker, 1, 1), OCUDU_DAPP_OK_V1);
  ocudu_dapp_receiver_input_v1  rx_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_receiver_output_v1 rx_output{};
  rx_output.llrs = device_tensor(d_llrs, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, 2},
                                 OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
  ASSERT_EQ(receive_cuda(rx_input, rx_output, rx_state, rx_worker), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);

  std::array<float2, nof_symbols * nof_subcarriers> channel{};
  float2                                            equalized{};
  float                                             ce_noise{};
  float                                             scheduler_noise{};
  std::array<__half, 2>                             llrs{};
  ASSERT_EQ(cudaMemcpy(channel.data(), d_channel, sizeof(channel), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(&equalized, d_equalized, sizeof(equalized), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(&ce_noise, d_ce_noise, sizeof(ce_noise), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(&scheduler_noise, d_sched_noise, sizeof(scheduler_noise), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(llrs.data(), d_llrs, sizeof(llrs), cudaMemcpyDeviceToHost), cudaSuccess);
  const float2 expected_channel =
      rotate_value(base_channel, phase_per_symbol * static_cast<float>(static_cast<int32_t>(data_symbol_index) - 1));
  EXPECT_NEAR(channel[data_index].x, expected_channel.x, 3.0e-2F);
  EXPECT_NEAR(channel[data_index].y, expected_channel.y, 3.0e-2F);
  EXPECT_NEAR(equalized.x, data_symbol.x, 3.0e-2F);
  EXPECT_NEAR(equalized.y, data_symbol.y, 3.0e-2F);
  EXPECT_EQ(ce_output.reserved[OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_INDEX_V1],
            OCUDU_DAPP_CHANNEL_ESTIMATOR_SCHED_NOISE_RESPONSE_V1);
  float residual = 0.0F;
  for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
    const uint32_t symbol    = coordinates[2 * pilot];
    const uint32_t sc        = coordinates[2 * pilot + 1];
    const float2   received  = unpack_cbf16(grid[symbol * nof_subcarriers + sc]);
    const float2   predicted = multiply_value(channel[symbol * nof_subcarriers + sc], references[pilot]);
    residual += (received.x - predicted.x) * (received.x - predicted.x) +
                (received.y - predicted.y) * (received.y - predicted.y);
  }
  // Match the conventional estimator's normalization after FD smoothing.
  const float expected_noise = residual / static_cast<float>(nof_pilots - 1U);
  EXPECT_GT(expected_noise, 100.0F * pusch.noise_variance_floor);
  EXPECT_NEAR(ce_noise, expected_noise, 2.0e-5F);
  EXPECT_FLOAT_EQ(scheduler_noise, ce_noise);
  EXPECT_GT(__half2float(llrs[0]), 0.0F);
  EXPECT_GT(__half2float(llrs[1]), 0.0F);

  release_receiver_cuda_worker_state(rx_worker);
  release_receiver_cuda_state(rx_state);
  release_equalizer_cuda_worker_state(eq_worker);
  cudaFree(d_llrs);
  cudaFree(d_eq_noise);
  cudaFree(d_equalized);
  cudaFree(d_sched_noise);
  cudaFree(d_ce_noise);
  cudaFree(d_channel);
  cudaFree(d_indices);
  cudaFree(d_coordinates);
  cudaFree(d_references);
  cudaFree(d_grid);
  cudaStreamDestroy(stream);
}

TEST(reference_phy_algorithms_cuda, fd_smoothing_rejects_pilot_noise_with_and_without_dmrs_boost)
{
  constexpr uint32_t                   nof_symbols     = 1;
  constexpr uint32_t                   nof_subcarriers = 48;
  constexpr uint32_t                   nof_pilots      = nof_subcarriers / 2;
  std::array<uint16_t, nof_pilots * 2> coordinates{};
  for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
    coordinates[2U * pilot]      = 0;
    coordinates[2U * pilot + 1U] = static_cast<uint16_t>(2U * pilot);
  }
  std::array<uint32_t, 1> indices{{1}};

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  auto* d_grid        = allocate<uint32_t>(nof_symbols * nof_subcarriers);
  auto* d_references  = allocate<float2>(nof_pilots);
  auto* d_coordinates = allocate<uint16_t>(coordinates.size());
  auto* d_indices     = allocate<uint32_t>(indices.size());
  auto* d_channel     = allocate<float2>(nof_symbols * nof_subcarriers);
  auto* d_noise       = allocate<float>(1);
  ASSERT_EQ(cudaMemcpyAsync(d_coordinates, coordinates.data(), sizeof(coordinates), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_indices, indices.data(), sizeof(indices), cudaMemcpyHostToDevice, stream), cudaSuccess);

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = 1;
  pusch.nof_layers           = 1;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = nof_symbols;
  pusch.nof_subcarriers      = nof_subcarriers;
  pusch.nof_data_re          = 1;
  pusch.noise_variance_floor = 1.0e-6F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.data_re_indices      = device_tensor(d_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1},
                                             OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);

  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = nof_pilots;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reserved[0] = 1U;
  dmrs.reference_symbols =
      device_tensor(d_references, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_pilots});
  dmrs.coordinates =
      device_tensor(d_coordinates, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});

  ocudu_dapp_invocation_v1 invocation{};
  invocation.struct_size      = sizeof(invocation);
  invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  invocation.execution_stream = reinterpret_cast<uintptr_t>(stream);
  const auto grid_view =
      device_tensor(d_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_symbols, nof_subcarriers},
                    OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
  ocudu_dapp_typed_metadata_v1 pusch_view{
      sizeof(pusch_view),        OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_PUSCH_V1, 1,
      {&pusch, sizeof(pusch), 0}};
  ocudu_dapp_typed_metadata_v1 dmrs_view{
      sizeof(dmrs_view),       OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_DMRS_V1, 1,
      {&dmrs, sizeof(dmrs), 0}};

  std::array<std::array<float2, nof_symbols * nof_subcarriers>, 2> estimates{};
  constexpr std::array<float, 2>                                   scaling{{1.0F, 1.41421356237F}};
  channel_estimator_cuda_state                                     state{};
  channel_estimator_cuda_worker_state                              worker{};
  for (uint32_t run = 0; run != scaling.size(); ++run) {
    std::array<uint32_t, nof_symbols * nof_subcarriers> grid{};
    std::array<float2, nof_pilots>                      references{};
    for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
      references[pilot]             = {scaling[run], 0.0F};
      const float alternating_error = (pilot & 1U) == 0U ? 0.35F : -0.35F;
      grid[2U * pilot]              = pack_cbf16(scaling[run] * (1.0F + alternating_error), 0.0F);
    }
    ASSERT_EQ(cudaMemcpyAsync(d_grid, grid.data(), sizeof(grid), cudaMemcpyHostToDevice, stream), cudaSuccess);
    ASSERT_EQ(cudaMemcpyAsync(d_references, references.data(), sizeof(references), cudaMemcpyHostToDevice, stream),
              cudaSuccess);
    ocudu_dapp_channel_estimator_input_v1  input{invocation, grid_view, pusch_view, dmrs_view};
    ocudu_dapp_channel_estimator_output_v1 output{};
    output.channel_estimates = device_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                             {1, 1, nof_symbols, nof_subcarriers});
    output.noise_variances   = device_tensor(d_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1});
    ASSERT_EQ(estimate_channel_cuda(input, output, state, worker), OCUDU_DAPP_OK_V1);
    ASSERT_EQ(cudaMemcpyAsync(estimates[run].data(), d_channel, sizeof(estimates[run]), cudaMemcpyDeviceToHost, stream),
              cudaSuccess);
    ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);

    // Same-input parity with ocudu/lib/phy/cuda/src/pusch_e2e.cu::kernel_fd_smoothing_fp16:
    // 15 pilot-domain taps, skipped out-of-band taps, then coefficient-mass
    // renormalization. Compute LS from the quantized CBF16 grid so this checks
    // the exact CUDA baseline arithmetic for both normal and boosted DM-RS.
    constexpr std::array<float, 15> cuda_baseline_filter{{-0.0391585909F, -0.0287990728F, 0.0000000000F, 0.0445245383F,
                                                          0.0970724536F, 0.1467045930F, 0.1821531663F, 0.1950058248F,
                                                          0.1821531663F, 0.1467045930F, 0.0970724536F, 0.0445245383F,
                                                          0.0000000000F, -0.0287990728F, -0.0391585909F}};
    std::array<float, nof_pilots>   raw_ls{};
    for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
      raw_ls[pilot] = unpack_cbf16(grid[2U * pilot]).x / scaling[run];
    }
    for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
      float expected = 0.0F;
      float mass     = 0.0F;
      for (uint32_t tap = 0; tap != cuda_baseline_filter.size(); ++tap) {
        const int32_t source = static_cast<int32_t>(pilot) + static_cast<int32_t>(tap) - 7;
        if (source < 0 || source >= static_cast<int32_t>(nof_pilots)) {
          continue;
        }
        expected += cuda_baseline_filter[tap] * raw_ls[static_cast<uint32_t>(source)];
        mass += cuda_baseline_filter[tap];
      }
      expected /= mass;
      EXPECT_NEAR(estimates[run][2U * pilot].x, expected, 2.0e-5F);
      EXPECT_NEAR(estimates[run][2U * pilot].y, 0.0F, 2.0e-5F);
    }
  }

  float filtered_mse = 0.0F;
  for (uint32_t sc = 0; sc != nof_subcarriers; ++sc) {
    filtered_mse += (estimates[0][sc].x - 1.0F) * (estimates[0][sc].x - 1.0F) + estimates[0][sc].y * estimates[0][sc].y;
    EXPECT_NEAR(estimates[0][sc].x, estimates[1][sc].x, 5.0e-3F);
    EXPECT_NEAR(estimates[0][sc].y, estimates[1][sc].y, 5.0e-3F);
  }
  filtered_mse /= static_cast<float>(nof_subcarriers);
  EXPECT_LT(filtered_mse, 0.02F);
  EXPECT_LT(filtered_mse, 0.2F * 0.35F * 0.35F);

  cudaFree(d_noise);
  cudaFree(d_channel);
  cudaFree(d_indices);
  cudaFree(d_coordinates);
  cudaFree(d_references);
  cudaFree(d_grid);
  cudaStreamDestroy(stream);
}

TEST(reference_phy_algorithms_cuda, multi_symbol_dmrs_jointly_resolves_two_layers)
{
  constexpr uint32_t                          nof_ports       = 2;
  constexpr uint32_t                          nof_layers      = 2;
  constexpr uint32_t                          nof_symbols     = 4;
  constexpr uint32_t                          nof_subcarriers = 3;
  constexpr uint32_t                          nof_pilots      = 4;
  constexpr uint32_t                          plane           = nof_symbols * nof_subcarriers;
  std::array<uint32_t, nof_ports * plane>     grid{};
  std::array<float2, nof_layers * nof_pilots> references{{{1.0F, 0.0F},
                                                          {1.0F, 0.0F},
                                                          {1.0F, 0.0F},
                                                          {1.0F, 0.0F},
                                                          {1.0F, 0.0F},
                                                          {1.0F, 0.0F},
                                                          {-1.0F, 0.0F},
                                                          {-1.0F, 0.0F}}};
  std::array<uint16_t, 2 * nof_pilots>        coordinates{{0, 0, 0, 2, 3, 0, 3, 2}};
  std::array<uint32_t, 1>                     indices{{4}};
  for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
    const uint32_t index = coordinates[2U * pilot] * nof_subcarriers + coordinates[2U * pilot + 1U];
    grid[index]          = pack_cbf16(references[pilot].x, references[pilot].y);
    grid[plane + index]  = pack_cbf16(references[nof_pilots + pilot].x, references[nof_pilots + pilot].y);
  }

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  auto* d_grid        = allocate<uint32_t>(grid.size());
  auto* d_references  = allocate<float2>(references.size());
  auto* d_coordinates = allocate<uint16_t>(coordinates.size());
  auto* d_indices     = allocate<uint32_t>(indices.size());
  auto* d_channel     = allocate<float2>(nof_ports * nof_layers * plane);
  auto* d_noise       = allocate<float>(nof_ports * nof_layers);
  ASSERT_EQ(cudaMemcpyAsync(d_grid, grid.data(), sizeof(grid), cudaMemcpyHostToDevice, stream), cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_references, references.data(), sizeof(references), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_coordinates, coordinates.data(), sizeof(coordinates), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_indices, indices.data(), sizeof(indices), cudaMemcpyHostToDevice, stream), cudaSuccess);

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = nof_ports;
  pusch.nof_layers           = nof_layers;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = nof_symbols;
  pusch.nof_subcarriers      = nof_subcarriers;
  pusch.nof_data_re          = indices.size();
  pusch.noise_variance_floor = 1.0e-6F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.data_re_indices      = device_tensor(d_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                             {indices.size()}, OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);
  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = nof_pilots;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reference_symbols =
      device_tensor(d_references, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_layers, nof_pilots});
  dmrs.coordinates =
      device_tensor(d_coordinates, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});
  ocudu_dapp_invocation_v1 invocation{};
  invocation.struct_size                 = sizeof(invocation);
  invocation.abi_major                   = OCUDU_DAPP_ABI_MAJOR_V1;
  invocation.abi_minor                   = OCUDU_DAPP_ABI_MINOR_V1;
  invocation.execution_stream            = reinterpret_cast<uintptr_t>(stream);
  auto                         grid_view = device_tensor(d_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                                         {nof_ports, nof_symbols, nof_subcarriers},
                                                         OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
  ocudu_dapp_typed_metadata_v1 pusch_view{
      sizeof(pusch_view),        OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_PUSCH_V1, 1,
      {&pusch, sizeof(pusch), 0}};
  ocudu_dapp_typed_metadata_v1 dmrs_view{
      sizeof(dmrs_view),       OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_DMRS_V1, 1,
      {&dmrs, sizeof(dmrs), 0}};

  ocudu_dapp_channel_estimator_input_v1  input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_channel_estimator_output_v1 output{};
  output.channel_estimates = device_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                           {nof_ports, nof_layers, nof_symbols, nof_subcarriers});
  output.noise_variances =
      device_tensor(d_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {nof_ports, nof_layers});
  channel_estimator_cuda_state        state{};
  channel_estimator_cuda_worker_state worker{};
  ASSERT_EQ(estimate_channel_cuda(input, output, state, worker), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);

  std::array<float2, nof_ports * nof_layers * plane> channel{};
  ASSERT_EQ(cudaMemcpy(channel.data(), d_channel, sizeof(channel), cudaMemcpyDeviceToHost), cudaSuccess);
  for (uint32_t port = 0; port != nof_ports; ++port) {
    for (uint32_t layer = 0; layer != nof_layers; ++layer) {
      const float expected = port == layer ? 1.0F : 0.0F;
      for (uint32_t index = 0; index != plane; ++index) {
        const float2 value = channel[(port * nof_layers + layer) * plane + index];
        EXPECT_NEAR(value.x, expected, 1.0e-5F);
        EXPECT_NEAR(value.y, 0.0F, 1.0e-5F);
      }
    }
  }

  cudaFree(d_noise);
  cudaFree(d_channel);
  cudaFree(d_indices);
  cudaFree(d_coordinates);
  cudaFree(d_references);
  cudaFree(d_grid);
  cudaStreamDestroy(stream);
}

TEST(reference_phy_algorithms_cuda, queues_ce_eq_and_receiver_on_the_inherited_stream)
{
  resident_fixture fixture;
  fixture.pusch.flags |= OCUDU_DAPP_PUSCH_DC_OVERLAP_V1;
  float2* d_channel   = allocate<float2>(8);
  float*  d_ce_noise  = allocate<float>(1);
  float2* d_equalized = allocate<float2>(8);
  float*  d_eq_noise  = allocate<float>(8);
  __half* d_llrs      = allocate<__half>(14);

  ocudu_dapp_channel_estimator_input_v1  ce_input{fixture.invocation, fixture.grid_view, fixture.pusch_view,
                                                 fixture.dmrs_view};
  ocudu_dapp_channel_estimator_output_v1 ce_output{};
  ce_output.channel_estimates =
      device_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, 2, 4});
  ce_output.noise_variances = device_tensor(d_ce_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1});
  EXPECT_EQ(
      estimate_channel_cuda(ce_input, ce_output, fixture.channel_estimator_state, fixture.channel_estimator_worker),
      OCUDU_DAPP_OK_V1);

  ocudu_dapp_equalizer_input_v1  eq_input{fixture.invocation, fixture.grid_view, fixture.pusch_view, fixture.dmrs_view};
  ocudu_dapp_equalizer_output_v1 eq_output{};
  eq_output.channel_estimates = ce_output.channel_estimates;
  eq_output.equalized_symbols =
      device_tensor(d_equalized, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 2, 4});
  eq_output.post_equalization_noise =
      device_tensor(d_eq_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 2, 4});
  EXPECT_EQ(equalize_cuda(eq_input, eq_output, fixture.equalizer_state, fixture.equalizer_worker), OCUDU_DAPP_OK_V1);

  ocudu_dapp_receiver_input_v1  rx_input{fixture.invocation, fixture.grid_view, fixture.pusch_view, fixture.dmrs_view};
  ocudu_dapp_receiver_output_v1 rx_output{};
  rx_output.llrs = device_tensor(d_llrs, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {7, 1, 2},
                                 OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
  EXPECT_EQ(receive_cuda(rx_input, rx_output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(cudaStreamSynchronize(fixture.stream), cudaSuccess);

  std::array<float2, 8>  channel{};
  std::array<float2, 8>  equalized{};
  std::array<__half, 14> llrs{};
  EXPECT_EQ(cudaMemcpy(channel.data(), d_channel, sizeof(channel), cudaMemcpyDeviceToHost), cudaSuccess);
  EXPECT_EQ(cudaMemcpy(equalized.data(), d_equalized, sizeof(equalized), cudaMemcpyDeviceToHost), cudaSuccess);
  EXPECT_EQ(cudaMemcpy(llrs.data(), d_llrs, sizeof(llrs), cudaMemcpyDeviceToHost), cudaSuccess);
  for (const float2 value : channel) {
    EXPECT_NEAR(value.x, 2.0F, 1.0e-5F);
    EXPECT_NEAR(value.y, 0.0F, 1.0e-5F);
  }
  EXPECT_GT(equalized[1].x, 0.9F);
  EXPECT_GT(equalized[1].y, 0.9F);
  EXPECT_GT(__half2float(llrs[0]), 0.0F);
  EXPECT_GT(__half2float(llrs[1]), 0.0F);
  EXPECT_LT(__half2float(llrs[2]), 0.0F);
  EXPECT_GT(__half2float(llrs[3]), 0.0F);

  cudaFree(d_llrs);
  cudaFree(d_eq_noise);
  cudaFree(d_equalized);
  cudaFree(d_ce_noise);
  cudaFree(d_channel);
}

TEST(reference_phy_algorithms_cuda, all_pilots_drive_multiport_ls_and_mrc_outputs)
{
  constexpr uint32_t                                              nof_ports       = 2;
  constexpr uint32_t                                              nof_symbols     = 2;
  constexpr uint32_t                                              nof_subcarriers = 4;
  constexpr uint32_t                                              nof_pilots      = 3;
  constexpr uint32_t                                              data_index      = 7;
  std::array<uint32_t, nof_ports * nof_symbols * nof_subcarriers> grid{};
  std::array<float2, nof_ports * nof_symbols * nof_subcarriers>   host_grid{};
  const auto set_grid = [&](uint32_t index, float real, float imaginary) {
    host_grid[index] = {real, imaginary};
    grid[index]      = pack_cbf16(real, imaginary);
  };
  set_grid(0, 1.0F, 0.0F);
  set_grid(1, 2.0F, 0.0F);
  set_grid(2, 3.0F, 0.0F);
  set_grid(data_index, 1.0F, 1.0F);
  const uint32_t port1 = nof_symbols * nof_subcarriers;
  set_grid(port1, 0.0F, 0.5F);
  set_grid(port1 + 1, 0.0F, 1.0F);
  set_grid(port1 + 2, 0.0F, 1.5F);
  set_grid(port1 + data_index, -0.5F, 0.5F);
  std::array<float2, nof_pilots>       references{{{1.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.0F}}};
  std::array<uint16_t, 2 * nof_pilots> coordinates{{0, 0, 0, 1, 0, 2}};
  std::array<uint32_t, 1>              indices{{data_index}};

  cudaStream_t stream{};
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  auto* d_grid        = allocate<uint32_t>(grid.size());
  auto* d_references  = allocate<float2>(references.size());
  auto* d_coordinates = allocate<uint16_t>(coordinates.size());
  auto* d_indices     = allocate<uint32_t>(indices.size());
  auto* d_channel     = allocate<float2>(grid.size());
  auto* d_ce_noise    = allocate<float>(nof_ports);
  auto* d_equalized   = allocate<float2>(1);
  auto* d_eq_noise    = allocate<float>(1);
  auto* d_llrs        = allocate<__half>(2);
  ASSERT_EQ(cudaMemcpyAsync(d_grid, grid.data(), sizeof(grid), cudaMemcpyHostToDevice, stream), cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_references, references.data(), sizeof(references), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_coordinates, coordinates.data(), sizeof(coordinates), cudaMemcpyHostToDevice, stream),
            cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(d_indices, indices.data(), sizeof(indices), cudaMemcpyHostToDevice, stream), cudaSuccess);

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = nof_ports;
  pusch.nof_layers           = 1;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = nof_symbols;
  pusch.nof_subcarriers      = nof_subcarriers;
  pusch.nof_data_re          = 1;
  pusch.noise_variance_floor = 0.01F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.data_re_indices      = device_tensor(d_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1},
                                             OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);
  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = nof_pilots;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reference_symbols =
      device_tensor(d_references, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_pilots});
  dmrs.coordinates =
      device_tensor(d_coordinates, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});
  ocudu_dapp_invocation_v1 invocation{};
  invocation.struct_size                 = sizeof(invocation);
  invocation.abi_major                   = OCUDU_DAPP_ABI_MAJOR_V1;
  invocation.abi_minor                   = OCUDU_DAPP_ABI_MINOR_V1;
  invocation.execution_stream            = reinterpret_cast<uintptr_t>(stream);
  auto                         grid_view = device_tensor(d_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                                         {nof_ports, nof_symbols, nof_subcarriers},
                                                         OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
  ocudu_dapp_typed_metadata_v1 pusch_view{
      sizeof(pusch_view),        OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_PUSCH_V1, 1,
      {&pusch, sizeof(pusch), 0}};
  ocudu_dapp_typed_metadata_v1 dmrs_view{
      sizeof(dmrs_view),       OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_DMRS_V1, 1,
      {&dmrs, sizeof(dmrs), 0}};

  ocudu_dapp_channel_estimator_input_v1  ce_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_channel_estimator_output_v1 ce_output{};
  ce_output.channel_estimates = device_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                              {nof_ports, 1, nof_symbols, nof_subcarriers});
  ce_output.noise_variances =
      device_tensor(d_ce_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {nof_ports, 1});
  channel_estimator_cuda_state        channel_estimator_state{};
  channel_estimator_cuda_worker_state channel_estimator_worker{};
  ASSERT_EQ(estimate_channel_cuda(ce_input, ce_output, channel_estimator_state, channel_estimator_worker),
            OCUDU_DAPP_OK_V1);

  ocudu_dapp_equalizer_input_v1  eq_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_equalizer_output_v1 eq_output{};
  eq_output.channel_estimates       = ce_output.channel_estimates;
  eq_output.equalized_symbols       = device_tensor(d_equalized, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                                    {1, 1}, OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
  eq_output.post_equalization_noise = device_tensor(d_eq_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                                    {1, 1}, OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
  equalizer_cuda_worker_state equalizer_worker{};
  equalizer_cuda_state        equalizer_state{};
  ASSERT_EQ(initialize_equalizer_cuda_worker_state(equalizer_worker, nof_ports), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(equalize_cuda(eq_input, eq_output, equalizer_state, equalizer_worker), OCUDU_DAPP_OK_V1);

  receiver_cuda_state        receiver_state{};
  receiver_cuda_worker_state receiver_worker{};
  ASSERT_EQ(initialize_receiver_cuda_state(receiver_state), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(initialize_receiver_cuda_worker_state(receiver_worker), OCUDU_DAPP_OK_V1);
  ocudu_dapp_receiver_input_v1  rx_input{invocation, grid_view, pusch_view, dmrs_view};
  ocudu_dapp_receiver_output_v1 rx_output{};
  rx_output.llrs = device_tensor(d_llrs, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, 2},
                                 OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
  ASSERT_EQ(receive_cuda(rx_input, rx_output, receiver_state, receiver_worker), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess);

  std::array<float2, nof_ports * nof_symbols * nof_subcarriers> channel{};
  std::array<float, nof_ports>                                  noise{};
  float2                                                        equalized{};
  std::array<__half, 2>                                         llrs{};
  ASSERT_EQ(cudaMemcpy(channel.data(), d_channel, sizeof(channel), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(noise.data(), d_ce_noise, sizeof(noise), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(&equalized, d_equalized, sizeof(equalized), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(llrs.data(), d_llrs, sizeof(llrs), cudaMemcpyDeviceToHost), cudaSuccess);
  // CE is frequency-selective: the three pilots establish H={1,2,3} on
  // port 0 and H=j*{0.5,1,1.5} on port 1, then the edge value is held out
  // to data_index. The old expectations below predated the FD interpolation
  // path and incorrectly asserted one wideband average for every subcarrier.
  EXPECT_NEAR(channel[0].x, 1.0F, 1.0e-5F);
  EXPECT_NEAR(channel[0].y, 0.0F, 1.0e-5F);
  EXPECT_NEAR(channel[port1].x, 0.0F, 1.0e-5F);
  EXPECT_NEAR(channel[port1].y, 0.5F, 1.0e-5F);
  EXPECT_NEAR(noise[0], pusch.noise_variance_floor, 1.0e-5F);
  EXPECT_NEAR(noise[1], pusch.noise_variance_floor, 1.0e-5F);
  EXPECT_NEAR(equalized.x, 1.0F / 3.0F, 1.0e-5F);
  EXPECT_NEAR(equalized.y, 1.0F / 3.0F, 1.0e-5F);
  EXPECT_GT(__half2float(llrs[0]), 0.0F);
  EXPECT_GT(__half2float(llrs[1]), 0.0F);

  std::array<uint8_t, nof_symbols * nof_subcarriers> data_mask{};
  data_mask[data_index]  = 1;
  auto cpu_pusch         = pusch;
  cpu_pusch.data_re_mask = host_tensor(data_mask.data(), OCUDU_DAPP_ELEMENT_U8_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                       {nof_symbols, nof_subcarriers});
  auto cpu_dmrs          = dmrs;
  cpu_dmrs.reference_symbols =
      host_tensor(references.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_pilots});
  cpu_dmrs.coordinates =
      host_tensor(coordinates.data(), OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});
  auto cpu_grid = host_tensor(host_grid.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1,
                              {nof_ports, nof_symbols, nof_subcarriers});
  ocudu_dapp_typed_metadata_v1 cpu_pusch_view{sizeof(cpu_pusch_view),
                                              OCUDU_DAPP_ABI_MAJOR_V1,
                                              OCUDU_DAPP_ABI_MINOR_V1,
                                              OCUDU_DAPP_METADATA_PUSCH_V1,
                                              1,
                                              {&cpu_pusch, sizeof(cpu_pusch), 0}};
  ocudu_dapp_typed_metadata_v1 cpu_dmrs_view{
      sizeof(cpu_dmrs_view),           OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1, OCUDU_DAPP_METADATA_DMRS_V1, 1,
      {&cpu_dmrs, sizeof(cpu_dmrs), 0}};

  std::array<float2, nof_ports * nof_symbols * nof_subcarriers> cpu_channel{};
  std::array<float, nof_ports>                                  cpu_noise{};
  ocudu_dapp_channel_estimator_input_v1  cpu_ce_input{{}, cpu_grid, cpu_pusch_view, cpu_dmrs_view};
  ocudu_dapp_channel_estimator_output_v1 cpu_ce_output{};
  cpu_ce_output.channel_estimates =
      host_tensor(cpu_channel.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                  {nof_ports, 1, nof_symbols, nof_subcarriers});
  cpu_ce_output.noise_variances =
      host_tensor(cpu_noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {nof_ports, 1});
  ASSERT_EQ(estimate_channel(cpu_ce_input, cpu_ce_output), OCUDU_DAPP_OK_V1);
  for (uint32_t index = 0; index != channel.size(); ++index) {
    EXPECT_NEAR(channel[index].x, cpu_channel[index].x, 1.0e-5F);
    EXPECT_NEAR(channel[index].y, cpu_channel[index].y, 1.0e-5F);
  }
  for (uint32_t port = 0; port != nof_ports; ++port) {
    EXPECT_NEAR(noise[port], cpu_noise[port], 1.0e-5F);
  }

  std::array<float2, nof_symbols * nof_subcarriers> cpu_equalized{};
  std::array<float, nof_symbols * nof_subcarriers>  cpu_eq_noise{};
  ocudu_dapp_equalizer_input_v1                     cpu_eq_input{{}, cpu_grid, cpu_pusch_view, cpu_dmrs_view};
  ocudu_dapp_equalizer_output_v1                    cpu_eq_output{};
  cpu_eq_output.channel_estimates       = cpu_ce_output.channel_estimates;
  cpu_eq_output.equalized_symbols       = host_tensor(cpu_equalized.data(), OCUDU_DAPP_ELEMENT_CF32_V1,
                                                      OCUDU_DAPP_ACCESS_WRITE_V1, {1, nof_symbols, nof_subcarriers});
  cpu_eq_output.post_equalization_noise = host_tensor(cpu_eq_noise.data(), OCUDU_DAPP_ELEMENT_F32_V1,
                                                      OCUDU_DAPP_ACCESS_WRITE_V1, {1, nof_symbols, nof_subcarriers});
  ASSERT_EQ(equalize(cpu_eq_input, cpu_eq_output), OCUDU_DAPP_OK_V1);
  EXPECT_NEAR(equalized.x, cpu_equalized[data_index].x, 1.0e-5F);
  EXPECT_NEAR(equalized.y, cpu_equalized[data_index].y, 1.0e-5F);
  float gpu_eq_noise{};
  ASSERT_EQ(cudaMemcpy(&gpu_eq_noise, d_eq_noise, sizeof(gpu_eq_noise), cudaMemcpyDeviceToHost), cudaSuccess);
  EXPECT_NEAR(gpu_eq_noise, cpu_eq_noise[data_index], 1.0e-5F);

  std::array<int8_t, 2>         cpu_llrs{};
  ocudu_dapp_receiver_input_v1  cpu_rx_input{{}, cpu_grid, cpu_pusch_view, cpu_dmrs_view};
  ocudu_dapp_receiver_output_v1 cpu_rx_output{};
  cpu_rx_output.llrs =
      host_tensor(cpu_llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {cpu_llrs.size()});
  ASSERT_EQ(receive(cpu_rx_input, cpu_rx_output), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(__half2float(llrs[0]) > 0.0F, cpu_llrs[0] > 0);
  EXPECT_EQ(__half2float(llrs[1]) > 0.0F, cpu_llrs[1] > 0);

  release_receiver_cuda_worker_state(receiver_worker);
  release_receiver_cuda_state(receiver_state);
  release_equalizer_cuda_worker_state(equalizer_worker);
  cudaFree(d_llrs);
  cudaFree(d_eq_noise);
  cudaFree(d_equalized);
  cudaFree(d_ce_noise);
  cudaFree(d_channel);
  cudaFree(d_indices);
  cudaFree(d_coordinates);
  cudaFree(d_references);
  cudaFree(d_grid);
  cudaStreamDestroy(stream);
}

TEST(reference_phy_algorithms_cuda, rejects_host_tensors_and_unsupported_modulation_without_enqueuing_work)
{
  resident_fixture              fixture;
  __half*                       d_llrs  = allocate<__half>(14);
  float*                        d_noise = allocate<float>(7);
  ocudu_dapp_receiver_input_v1  input{fixture.invocation, fixture.grid_view, fixture.pusch_view, fixture.dmrs_view};
  ocudu_dapp_receiver_output_v1 output{};
  output.llrs          = device_tensor(d_llrs, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {7, 1, 2},
                                       OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
  input.rx_grid.domain = OCUDU_DAPP_MEM_HOST_V1;
  EXPECT_NE(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
  input.rx_grid            = fixture.grid_view;
  fixture.pusch.modulation = OCUDU_DAPP_MODULATION_UNSPECIFIED_V1;
  EXPECT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_UNSUPPORTED_V1);
  fixture.pusch.modulation = OCUDU_DAPP_MODULATION_QPSK_V1;
  fixture.dmrs.dmrs_type   = 0;
  EXPECT_NE(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
  fixture.dmrs.dmrs_type     = 1;
  fixture.pusch.start_symbol = 1;
  EXPECT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_UNSUPPORTED_V1);
  fixture.pusch.start_symbol     = 0;
  fixture.pusch.start_subcarrier = 1;
  EXPECT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_UNSUPPORTED_V1);
  fixture.pusch.start_subcarrier = 0;

  ocudu_dapp_receiver_metrics_v1 metrics{};
  metrics.struct_size             = sizeof(metrics);
  metrics.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  metrics.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  metrics.post_equalization_noise = device_tensor(d_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                                  {6, 1}, OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
  output.metrics                  = {sizeof(output.metrics),
                                     OCUDU_DAPP_ABI_MAJOR_V1,
                                     OCUDU_DAPP_ABI_MINOR_V1,
                                     OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1,
                                     1,
                                     {&metrics, sizeof(metrics), 0}};
  EXPECT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_INVALID_OUTPUT_V1);
  metrics.post_equalization_noise.dimensions[0] = 7;
  metrics.post_equalization_noise.layout_id     = 0;
  EXPECT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_INVALID_OUTPUT_V1);

  // Existing callers may omit the optional metric request entirely.
  output.metrics = {};
  ASSERT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(cudaStreamSynchronize(fixture.stream), cudaSuccess);
  EXPECT_EQ(output.reserved0, 0U);
  cudaFree(d_noise);
  cudaFree(d_llrs);
}

TEST(reference_phy_algorithms_cuda, receiver_supports_all_nr_qam_orders)
{
  resident_fixture fixture;
  __half*          d_llrs = allocate<__half>(fixture.indices.size() * 8);
  float* d_noise = allocate<float>(fixture.indices.size());
  float2* d_symbols = allocate<float2>(fixture.indices.size());
  ocudu_dapp_receiver_metrics_v1 metrics{};
  metrics.struct_size = sizeof(metrics);
  metrics.abi_major = OCUDU_DAPP_ABI_MAJOR_V1;
  metrics.post_equalization_noise = device_tensor(d_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
      {fixture.indices.size(), 1}, OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);

  ocudu_dapp_receiver_input_v1 input{fixture.invocation, fixture.grid_view, fixture.pusch_view, fixture.dmrs_view};
  for (auto modulation : {OCUDU_DAPP_MODULATION_QPSK_V1, OCUDU_DAPP_MODULATION_QAM16_V1, OCUDU_DAPP_MODULATION_QAM64_V1,
                          OCUDU_DAPP_MODULATION_QAM256_V1}) {
    const uint32_t axis_bits = static_cast<uint32_t>(modulation) / 2U;
    const uint32_t real_bits = 0b1010U & ((1U << axis_bits) - 1U);
    const uint32_t imag_bits = 0b0101U & ((1U << axis_bits) - 1U);
    fixture.grid[1] = pack_cbf16(2.0F * axis_level(real_bits, axis_bits), 2.0F * axis_level(imag_bits, axis_bits));
    ASSERT_EQ(cudaMemcpyAsync(fixture.d_grid, fixture.grid.data(), fixture.grid.size() * sizeof(uint32_t),
                              cudaMemcpyHostToDevice, fixture.stream),
              cudaSuccess);
    fixture.pusch.modulation = modulation;
    ocudu_dapp_receiver_output_v1 output{};
    output.llrs = device_tensor(d_llrs, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                {fixture.indices.size(), 1, static_cast<uint64_t>(modulation)},
                                OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
    ASSERT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
    ASSERT_EQ(cudaStreamSynchronize(fixture.stream), cudaSuccess);

    std::vector<__half> llrs(fixture.indices.size() * modulation);
    ASSERT_EQ(cudaMemcpy(llrs.data(), d_llrs, llrs.size() * sizeof(__half), cudaMemcpyDeviceToHost), cudaSuccess);
    for (uint32_t bit = 0; bit != axis_bits; ++bit) {
      EXPECT_EQ(__half2float(llrs[2U * bit]) > 0.0F, ((real_bits >> bit) & 1U) == 0);
      EXPECT_EQ(__half2float(llrs[2U * bit + 1U]) > 0.0F, ((imag_bits >> bit) & 1U) == 0);
    }
    metrics.equalized_symbols = device_tensor(d_symbols, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
        {fixture.indices.size(), 1}, OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
    output.metrics = {sizeof(output.metrics), OCUDU_DAPP_ABI_MAJOR_V1, OCUDU_DAPP_ABI_MINOR_V1,
                      OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1, 1, {&metrics, sizeof(metrics), 0}};
    ASSERT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
    ASSERT_NE(output.reserved0 & OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1, 0U);
    ASSERT_EQ(cudaStreamSynchronize(fixture.stream), cudaSuccess);
    std::vector<__half> with_symbols(llrs.size());
    std::vector<float2> symbols(fixture.indices.size());
    ASSERT_EQ(cudaMemcpy(with_symbols.data(), d_llrs, llrs.size() * sizeof(__half), cudaMemcpyDeviceToHost), cudaSuccess);
    EXPECT_EQ(std::memcmp(llrs.data(), with_symbols.data(), llrs.size() * sizeof(__half)), 0);
    ASSERT_EQ(cudaMemcpy(symbols.data(), d_symbols, symbols.size() * sizeof(float2), cudaMemcpyDeviceToHost), cudaSuccess);
    for (size_t i = 0; i != symbols.size(); ++i) {
      const auto received = unpack_cbf16(fixture.grid[fixture.indices[i]]);
      EXPECT_NEAR(symbols[i].x, received.x / 2.0F, 1.e-5F);
      EXPECT_NEAR(symbols[i].y, received.y / 2.0F, 1.e-5F);
    }
    // Aliased auxiliary storage must reject before any enqueue.
    metrics.equalized_symbols.native_handle = output.llrs.native_handle;
    EXPECT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_INVALID_OUTPUT_V1);
    EXPECT_EQ(output.reserved0, 0U);
    metrics.equalized_symbols.native_handle = reinterpret_cast<uintptr_t>(d_symbols) + sizeof(float);
    EXPECT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_INVALID_OUTPUT_V1);
    metrics.equalized_symbols = {};
    ASSERT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
    EXPECT_EQ(output.reserved0 & OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1, 0U);
    ASSERT_EQ(cudaStreamSynchronize(fixture.stream), cudaSuccess);
  }
  cudaFree(d_symbols);
  cudaFree(d_noise);
  cudaFree(d_llrs);
}

TEST(reference_phy_algorithms_cuda, receiver_uses_the_prepared_instance_model)
{
  resident_fixture                                      fixture;
  std::array<float, receiver_cuda_model_level_count_v1> scaled_levels{};
  for (uint32_t bits = 1; bits <= 4; ++bits) {
    for (uint32_t label = 0; label != (1U << bits); ++label) {
      scaled_levels[(bits - 1U) * 16U + label] = 0.75F * axis_level(label, bits);
    }
  }
  receiver_cuda_state scaled_state{};
  ASSERT_EQ(initialize_receiver_cuda_state(scaled_state, scaled_levels.data()), OCUDU_DAPP_OK_V1);

  fixture.pusch.modulation                     = OCUDU_DAPP_MODULATION_QAM16_V1;
  __half*                       default_output = allocate<__half>(fixture.indices.size() * 4);
  __half*                       scaled_output  = allocate<__half>(fixture.indices.size() * 4);
  ocudu_dapp_receiver_input_v1  input{fixture.invocation, fixture.grid_view, fixture.pusch_view, fixture.dmrs_view};
  ocudu_dapp_receiver_output_v1 output{};
  output.llrs = device_tensor(default_output, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                              {fixture.indices.size(), 1, 4}, OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
  ASSERT_EQ(receive_cuda(input, output, fixture.receiver_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
  output.llrs.native_handle = reinterpret_cast<uintptr_t>(scaled_output);
  ASSERT_EQ(receive_cuda(input, output, scaled_state, fixture.receiver_worker), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(cudaStreamSynchronize(fixture.stream), cudaSuccess);

  std::array<__half, 28> default_llrs{};
  std::array<__half, 28> scaled_llrs{};
  ASSERT_EQ(cudaMemcpy(default_llrs.data(), default_output, sizeof(default_llrs), cudaMemcpyDeviceToHost), cudaSuccess);
  ASSERT_EQ(cudaMemcpy(scaled_llrs.data(), scaled_output, sizeof(scaled_llrs), cudaMemcpyDeviceToHost), cudaSuccess);
  EXPECT_NE(__half2float(default_llrs[2]), __half2float(scaled_llrs[2]));

  release_receiver_cuda_state(scaled_state);
  cudaFree(scaled_output);
  cudaFree(default_output);
}

TEST(reference_phy_algorithms_cuda, equalizer_writes_compact_data_re_outputs)
{
  resident_fixture fixture;
  float2*          d_channel   = allocate<float2>(8);
  float2*          d_equalized = allocate<float2>(fixture.indices.size());
  float*           d_noise     = allocate<float>(fixture.indices.size());

  ocudu_dapp_equalizer_input_v1  input{fixture.invocation, fixture.grid_view, fixture.pusch_view, fixture.dmrs_view};
  ocudu_dapp_equalizer_output_v1 output{};
  output.channel_estimates =
      device_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, 2, 4});
  output.equalized_symbols =
      device_tensor(d_equalized, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {fixture.indices.size(), 1},
                    OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
  output.post_equalization_noise =
      device_tensor(d_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {fixture.indices.size(), 1},
                    OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
  EXPECT_EQ(equalize_cuda(input, output, fixture.equalizer_state, fixture.equalizer_worker), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(cudaStreamSynchronize(fixture.stream), cudaSuccess);

  std::array<float2, 7> equalized{};
  EXPECT_EQ(cudaMemcpy(equalized.data(), d_equalized, sizeof(equalized), cudaMemcpyDeviceToHost), cudaSuccess);
  EXPECT_GT(equalized[0].x, 0.9F);
  EXPECT_GT(equalized[0].y, 0.9F);
  EXPECT_LT(equalized[1].x, -0.9F);
  EXPECT_GT(equalized[1].y, 0.9F);

  cudaFree(d_noise);
  cudaFree(d_equalized);
  cudaFree(d_channel);
}

} // namespace
