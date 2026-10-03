// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_phy_algorithms.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <initializer_list>
#include <limits>

using namespace ocudu::dapp::reference;

namespace {

struct cf32 {
  float real;
  float imag;
};

cf32 multiply(cf32 lhs, cf32 rhs)
{
  return {lhs.real * rhs.real - lhs.imag * rhs.imag, lhs.real * rhs.imag + lhs.imag * rhs.real};
}

cf32 rotate(cf32 value, float phase)
{
  const float c = std::cos(phase);
  const float s = std::sin(phase);
  return {value.real * c - value.imag * s, value.real * s + value.imag * c};
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
ocudu_dapp_tensor_view_v1 tensor(T* data, uint32_t element_type, uint32_t access,
                                 std::initializer_list<uint64_t> dimensions)
{
  ocudu_dapp_tensor_view_v1 view{};
  view.struct_size           = sizeof(view);
  view.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  view.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  view.domain                = OCUDU_DAPP_MEM_HOST_V1;
  view.element_type          = element_type;
  view.rank                  = dimensions.size();
  view.access                = access | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  view.allocation_id         = 1;
  view.allocation_generation = 1;
  view.native_handle         = reinterpret_cast<uintptr_t>(data);
  uint64_t elements          = 1;
  uint32_t index             = 0;
  for (uint64_t dimension : dimensions) {
    view.dimensions[index++] = dimension;
    elements *= dimension;
  }
  uint64_t stride = sizeof(T);
  for (uint32_t dimension = view.rank; dimension != 0; --dimension) {
    view.byte_strides[dimension - 1] = stride;
    stride *= view.dimensions[dimension - 1];
  }
  view.byte_length = elements * sizeof(T);
  view.lease_token = 1;
  return view;
}

template <typename T>
ocudu_dapp_typed_metadata_v1 typed(uint32_t schema, T& value)
{
  ocudu_dapp_typed_metadata_v1 result{};
  result.struct_size             = sizeof(result);
  result.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  result.metadata_schema_id      = schema;
  result.metadata_schema_version = 1;
  result.metadata                = {&value, sizeof(value), 0};
  return result;
}

struct fixture {
  static constexpr uint32_t symbols     = 2;
  static constexpr uint32_t subcarriers = 4;
  static constexpr uint32_t pilots      = 2;

  std::array<cf32, symbols * subcarriers>    rx{};
  std::array<cf32, pilots>                   references{{{1, 0}, {1, 0}}};
  std::array<uint16_t, pilots * 2>           coordinates{{0, 0, 1, 0}};
  std::array<uint8_t, symbols * subcarriers> data_mask{{0, 1, 1, 1, 0, 1, 1, 1}};
  ocudu_dapp_pusch_metadata_v1               pusch{};
  ocudu_dapp_dmrs_metadata_v1                dmrs{};

  fixture()
  {
    constexpr float inverse_sqrt_two = 0.70710678118F;
    const cf32      channel{2.0F, 1.0F};
    const cf32      data{inverse_sqrt_two, inverse_sqrt_two};
    for (uint32_t symbol = 0; symbol != symbols; ++symbol) {
      rx[symbol * subcarriers] = channel;
      for (uint32_t subcarrier = 1; subcarrier != subcarriers; ++subcarrier) {
        rx[symbol * subcarriers + subcarrier] = multiply(channel, data);
      }
    }

    pusch.struct_size          = sizeof(pusch);
    pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
    pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
    pusch.nof_rx_ports         = 1;
    pusch.nof_layers           = 1;
    pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
    pusch.nof_symbols          = symbols;
    pusch.nof_subcarriers      = subcarriers;
    pusch.noise_variance_floor = 1.0e-3F;
    pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
    pusch.data_re_mask =
        tensor(data_mask.data(), OCUDU_DAPP_ELEMENT_U8_V1, OCUDU_DAPP_ACCESS_READ_V1, {symbols, subcarriers});

    dmrs.struct_size = sizeof(dmrs);
    dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    dmrs.nof_pilots  = pilots;
    dmrs.dmrs_type   = 1;
    dmrs.flags =
        OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
    dmrs.reference_symbols =
        tensor(references.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, pilots});
    dmrs.coordinates = tensor(coordinates.data(), OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {pilots, 2});
  }

  ocudu_dapp_tensor_view_v1 rx_view()
  {
    return tensor(rx.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, symbols, subcarriers});
  }
};

} // namespace

TEST(reference_phy_algorithms, wideband_ls_channel_estimator_recovers_flat_channel)
{
  fixture                                                   instance;
  std::array<cf32, fixture::symbols * fixture::subcarriers> estimates{};
  std::array<float, 1>                                      noise{};
  ocudu_dapp_channel_estimator_input_v1                     input{};
  input.rx_grid = instance.rx_view();
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  ocudu_dapp_channel_estimator_output_v1 output{};
  output.channel_estimates = tensor(estimates.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                    {1, 1, fixture::symbols, fixture::subcarriers});
  output.noise_variances   = tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1});

  ASSERT_EQ(estimate_channel(input, output), OCUDU_DAPP_OK_V1);
  for (const cf32 value : estimates) {
    EXPECT_NEAR(value.real, 2.0F, 1.0e-6F);
    EXPECT_NEAR(value.imag, 1.0F, 1.0e-6F);
  }
  EXPECT_FLOAT_EQ(noise[0], instance.pusch.noise_variance_floor);
}

TEST(reference_phy_algorithms, fd_smoothing_rejects_pilot_noise_and_is_invariant_to_dmrs_boost)
{
  constexpr uint32_t                   nof_symbols     = 1;
  constexpr uint32_t                   nof_subcarriers = 48;
  constexpr uint32_t                   nof_pilots      = nof_subcarriers / 2;
  std::array<uint16_t, nof_pilots * 2> coordinates{};
  for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
    coordinates[2U * pilot]      = 0;
    coordinates[2U * pilot + 1U] = static_cast<uint16_t>(2U * pilot);
  }

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = 1;
  pusch.nof_layers           = 1;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = nof_symbols;
  pusch.nof_subcarriers      = nof_subcarriers;
  pusch.noise_variance_floor = 1.0e-6F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;

  std::array<std::array<cf32, nof_symbols * nof_subcarriers>, 2> estimates{};
  constexpr std::array<float, 2>                                 scaling{{1.0F, 1.41421356237F}};
  for (uint32_t run = 0; run != scaling.size(); ++run) {
    std::array<cf32, nof_symbols * nof_subcarriers> grid{};
    std::array<cf32, nof_pilots>                    references{};
    for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
      references[pilot]             = {scaling[run], 0.0F};
      const float alternating_error = (pilot & 1U) == 0U ? 0.35F : -0.35F;
      grid[2U * pilot]              = {scaling[run] * (1.0F + alternating_error), 0.0F};
    }

    ocudu_dapp_dmrs_metadata_v1 dmrs{};
    dmrs.struct_size = sizeof(dmrs);
    dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    dmrs.nof_pilots  = nof_pilots;
    dmrs.dmrs_type   = 1;
    dmrs.flags =
        OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
    dmrs.reference_symbols =
        tensor(references.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_pilots});
    dmrs.coordinates =
        tensor(coordinates.data(), OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});

    std::array<float, 1>                  noise{};
    ocudu_dapp_channel_estimator_input_v1 input{};
    input.rx_grid =
        tensor(grid.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_symbols, nof_subcarriers});
    input.pusch = typed(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
    input.dmrs  = typed(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);
    ocudu_dapp_channel_estimator_output_v1 output{};
    output.channel_estimates = tensor(estimates[run].data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                      {1, 1, nof_symbols, nof_subcarriers});
    output.noise_variances   = tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1});
    ASSERT_EQ(estimate_channel(input, output), OCUDU_DAPP_OK_V1);
  }

  float filtered_mse = 0.0F;
  for (uint32_t sc = 0; sc != nof_subcarriers; ++sc) {
    filtered_mse +=
        (estimates[0][sc].real - 1.0F) * (estimates[0][sc].real - 1.0F) + estimates[0][sc].imag * estimates[0][sc].imag;
    EXPECT_NEAR(estimates[0][sc].real, estimates[1][sc].real, 2.0e-6F);
    EXPECT_NEAR(estimates[0][sc].imag, estimates[1][sc].imag, 2.0e-6F);
  }
  filtered_mse /= static_cast<float>(nof_subcarriers);
  EXPECT_LT(filtered_mse, 0.02F);
  EXPECT_LT(filtered_mse, 0.2F * 0.35F * 0.35F);
}

TEST(reference_phy_algorithms, single_layer_mrc_equalizer_recovers_data_symbols)
{
  fixture                                                    instance;
  std::array<cf32, fixture::symbols * fixture::subcarriers>  estimates{};
  std::array<cf32, fixture::symbols * fixture::subcarriers>  equalized{};
  std::array<float, fixture::symbols * fixture::subcarriers> noise{};
  ocudu_dapp_equalizer_input_v1                              input{};
  input.rx_grid = instance.rx_view();
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  ocudu_dapp_equalizer_output_v1 output{};
  output.channel_estimates       = tensor(estimates.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {1, 1, fixture::symbols, fixture::subcarriers});
  output.equalized_symbols       = tensor(equalized.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {1, fixture::symbols, fixture::subcarriers});
  output.post_equalization_noise = tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {1, fixture::symbols, fixture::subcarriers});

  ASSERT_EQ(equalize(input, output), OCUDU_DAPP_OK_V1);
  EXPECT_NEAR(equalized[1].real, std::sqrt(0.5F), 1.0e-6F);
  EXPECT_NEAR(equalized[1].imag, std::sqrt(0.5F), 1.0e-6F);
  EXPECT_GT(noise[1], 0.0F);
}

TEST(reference_phy_algorithms, ce_and_equalizer_track_common_phase_rotation_and_dmrs_boost)
{
  constexpr uint32_t nof_symbols      = 10;
  constexpr uint32_t nof_subcarriers  = 4;
  constexpr uint32_t nof_pilots       = 4;
  constexpr float    phase_per_symbol = 0.18F;
  constexpr float    dmrs_scaling     = 1.41421356237F;
  const cf32         base_channel{1.5F, -0.4F};
  const cf32         data_symbol{0.70710678F, 0.70710678F};

  std::array<cf32, nof_symbols * nof_subcarriers> grid{};
  for (uint32_t symbol = 0; symbol != nof_symbols; ++symbol) {
    const cf32 channel = rotate(base_channel, phase_per_symbol * static_cast<float>(static_cast<int32_t>(symbol) - 1));
    for (uint32_t sc = 0; sc != nof_subcarriers; ++sc) {
      grid[symbol * nof_subcarriers + sc] = multiply(channel, data_symbol);
    }
  }
  // Two pilots at each of two DM-RS symbols. Pilot order matches the live host contract.
  std::array<cf32, nof_pilots> references{{{dmrs_scaling, 0}, {dmrs_scaling, 0}, {dmrs_scaling, 0}, {dmrs_scaling, 0}}};
  std::array<uint16_t, nof_pilots * 2> coordinates{{1, 0, 1, 2, 8, 0, 8, 2}};
  for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
    const uint32_t symbol = coordinates[2 * pilot];
    const uint32_t sc     = coordinates[2 * pilot + 1];
    const cf32  channel = rotate(base_channel, phase_per_symbol * static_cast<float>(static_cast<int32_t>(symbol) - 1));
    const bool  high_amplitude          = (symbol == 1) == (sc == 0);
    const float amplitude               = high_amplitude ? 1.25F : 0.75F;
    const cf32  pilot_sample            = multiply(channel, references[pilot]);
    grid[symbol * nof_subcarriers + sc] = {amplitude * pilot_sample.real, amplitude * pilot_sample.imag};
  }

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = 1;
  pusch.nof_layers           = 1;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = nof_symbols;
  pusch.nof_subcarriers      = nof_subcarriers;
  pusch.noise_variance_floor = 1.0e-6F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;

  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = nof_pilots;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reference_symbols =
      tensor(references.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_pilots});
  dmrs.coordinates = tensor(coordinates.data(), OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});

  std::array<cf32, nof_symbols * nof_subcarriers>  channel{};
  std::array<cf32, nof_symbols * nof_subcarriers>  equalized{};
  std::array<float, nof_symbols * nof_subcarriers> post_eq_noise{};
  ocudu_dapp_equalizer_input_v1                    input{};
  input.rx_grid =
      tensor(grid.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, nof_symbols, nof_subcarriers});
  input.pusch = typed(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
  input.dmrs  = typed(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);
  std::array<float, 1>                  ce_noise{};
  ocudu_dapp_channel_estimator_input_v1 ce_input{};
  ce_input.rx_grid = input.rx_grid;
  ce_input.pusch   = input.pusch;
  ce_input.dmrs    = input.dmrs;
  ocudu_dapp_channel_estimator_output_v1 ce_output{};
  ce_output.channel_estimates = tensor(channel.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                       {1, 1, nof_symbols, nof_subcarriers});
  ce_output.noise_variances   = tensor(ce_noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1});
  ASSERT_EQ(estimate_channel(ce_input, ce_output), OCUDU_DAPP_OK_V1);

  float residual = 0.0F;
  for (uint32_t pilot = 0; pilot != nof_pilots; ++pilot) {
    const uint32_t symbol    = coordinates[2 * pilot];
    const uint32_t sc        = coordinates[2 * pilot + 1];
    const cf32     predicted = multiply(channel[symbol * nof_subcarriers + sc], references[pilot]);
    const cf32     received  = grid[symbol * nof_subcarriers + sc];
    residual += (received.real - predicted.real) * (received.real - predicted.real) +
                (received.imag - predicted.imag) * (received.imag - predicted.imag);
  }
  // Match the conventional estimator's normalization after FD smoothing.
  const float expected_noise = residual / static_cast<float>(nof_pilots - 1U);
  EXPECT_GT(expected_noise, 100.0F * pusch.noise_variance_floor);
  EXPECT_NEAR(ce_noise[0], expected_noise, 1.0e-5F);

  ocudu_dapp_equalizer_output_v1 output{};
  output.channel_estimates       = tensor(channel.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {1, 1, nof_symbols, nof_subcarriers});
  output.equalized_symbols       = tensor(equalized.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {1, nof_symbols, nof_subcarriers});
  output.post_equalization_noise = tensor(post_eq_noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {1, nof_symbols, nof_subcarriers});

  ASSERT_EQ(equalize(input, output), OCUDU_DAPP_OK_V1);
  for (uint32_t symbol = 0; symbol != nof_symbols; ++symbol) {
    const cf32 expected_channel =
        rotate(base_channel, phase_per_symbol * static_cast<float>(static_cast<int32_t>(symbol) - 1));
    EXPECT_NEAR(channel[symbol * nof_subcarriers + 1].real, expected_channel.real, 2.0e-5F);
    EXPECT_NEAR(channel[symbol * nof_subcarriers + 1].imag, expected_channel.imag, 2.0e-5F);
    EXPECT_NEAR(equalized[symbol * nof_subcarriers + 1].real, data_symbol.real, 2.0e-5F);
    EXPECT_NEAR(equalized[symbol * nof_subcarriers + 1].imag, data_symbol.imag, 2.0e-5F);
  }
}

TEST(reference_phy_algorithms, two_layer_mmse_handles_correlated_channels_and_reports_effective_noise)
{
  constexpr uint32_t nof_ports       = 2;
  constexpr uint32_t nof_layers      = 2;
  constexpr uint32_t nof_symbols     = 1;
  constexpr uint32_t nof_subcarriers = 3;
  constexpr uint32_t nof_pilots      = 2;
  constexpr uint32_t data_index      = 2;

  // H = [[1, 0.5], [0.5, 1]]. The two OCC pilots recover the correlated
  // columns independently. The data RE is H * ([0.5+j0.25], [-0.25+j0.5]).
  std::array<cf32, nof_ports * nof_symbols * nof_subcarriers> grid{
      {{1.5F, 0.0F}, {0.5F, 0.0F}, {0.375F, 0.5F}, {1.5F, 0.0F}, {-0.5F, 0.0F}, {0.0F, 0.625F}}};
  std::array<cf32, nof_layers * nof_pilots> references{{{1.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.0F}, {-1.0F, 0.0F}}};
  std::array<uint16_t, 2 * nof_pilots>      coordinates{{0, 0, 0, 1}};
  std::array<uint8_t, nof_symbols * nof_subcarriers> data_mask{{0, 0, 1}};

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = nof_ports;
  pusch.nof_layers           = nof_layers;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = nof_symbols;
  pusch.nof_subcarriers      = nof_subcarriers;
  pusch.nof_data_re          = 1;
  pusch.noise_variance_floor = 0.2F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.data_re_mask =
      tensor(data_mask.data(), OCUDU_DAPP_ELEMENT_U8_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_symbols, nof_subcarriers});

  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = nof_pilots;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reference_symbols =
      tensor(references.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_layers, nof_pilots});
  dmrs.coordinates = tensor(coordinates.data(), OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {nof_pilots, 2});

  std::array<cf32, nof_ports * nof_layers * nof_symbols * nof_subcarriers> channel{};
  std::array<cf32, nof_layers * nof_symbols * nof_subcarriers>             equalized{};
  std::array<float, nof_layers * nof_symbols * nof_subcarriers>            noise{};
  ocudu_dapp_equalizer_input_v1                                            input{};
  input.rx_grid = tensor(grid.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1,
                         {nof_ports, nof_symbols, nof_subcarriers});
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);
  ocudu_dapp_equalizer_output_v1 output{};
  output.channel_estimates       = tensor(channel.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {nof_ports, nof_layers, nof_symbols, nof_subcarriers});
  output.equalized_symbols       = tensor(equalized.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {nof_layers, nof_symbols, nof_subcarriers});
  output.post_equalization_noise = tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                                          {nof_layers, nof_symbols, nof_subcarriers});

  ASSERT_EQ(equalize(input, output), OCUDU_DAPP_OK_V1);
  constexpr float expected_noise = 0.35692308F;
  EXPECT_NEAR(equalized[data_index].real, 0.43846154F, 1.0e-5F);
  EXPECT_NEAR(equalized[data_index].imag, 0.37307692F, 1.0e-5F);
  EXPECT_NEAR(equalized[nof_symbols * nof_subcarriers + data_index].real, -0.12692308F, 1.0e-5F);
  EXPECT_NEAR(equalized[nof_symbols * nof_subcarriers + data_index].imag, 0.56153846F, 1.0e-5F);
  EXPECT_NEAR(noise[data_index], expected_noise, 1.0e-5F);
  EXPECT_NEAR(noise[nof_symbols * nof_subcarriers + data_index], expected_noise, 1.0e-5F);
  // A ZF implementation would recover the original symbols exactly here.
  EXPECT_GT(std::abs(equalized[data_index].real - 0.5F), 0.05F);
}

TEST(reference_phy_algorithms, receiver_emits_max_log_qpsk_llrs_for_data_res)
{
  fixture                      instance;
  std::array<int8_t, 12>       llrs{};
  std::array<float, 6>         noise{};
  ocudu_dapp_receiver_input_v1 input{};
  input.rx_grid = instance.rx_view();
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  ocudu_dapp_receiver_output_v1 output{};
  output.llrs = tensor(llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {llrs.size()});
  ocudu_dapp_receiver_metrics_v1 metrics{};
  metrics.struct_size = sizeof(metrics);
  metrics.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  metrics.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  metrics.post_equalization_noise =
      tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {noise.size(), 1});
  metrics.post_equalization_noise.layout_id      = OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1;
  metrics.post_equalization_noise.layout_version = 1;
  output.metrics                                 = typed(OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1, metrics);

  ASSERT_EQ(receive(input, output), OCUDU_DAPP_OK_V1);
  for (int8_t llr : llrs) {
    EXPECT_GT(llr, 0);
  }
  EXPECT_GT(output.confidence_q16, 0U);
  EXPECT_NE(output.reserved0 & OCUDU_DAPP_RECEIVER_METRICS_VALID_V1, 0U);
  for (float value : noise) {
    EXPECT_TRUE(std::isfinite(value));
    EXPECT_GT(value, 0.0F);
  }
}

TEST(reference_phy_algorithms, receiver_optional_symbols_preserve_llrs_noise_and_unsliced_qam)
{
  fixture                      instance;
  std::array<int8_t, 48>       llrs{};
  std::array<float, 6>         noise{};
  std::array<cf32, 8>          symbols{}; // Two trailing canaries must remain untouched.
  ocudu_dapp_receiver_input_v1 input{};
  input.rx_grid = instance.rx_view();
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  ocudu_dapp_receiver_metrics_v1 metrics{};
  metrics.struct_size             = sizeof(metrics);
  metrics.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  metrics.post_equalization_noise = tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {6, 1});
  metrics.post_equalization_noise.layout_id      = OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1;
  metrics.post_equalization_noise.layout_version = 1;
  ocudu_dapp_receiver_output_v1 output{};
  output.metrics = typed(OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1, metrics);
  for (auto qm : {OCUDU_DAPP_MODULATION_QPSK_V1, OCUDU_DAPP_MODULATION_QAM16_V1, OCUDU_DAPP_MODULATION_QAM64_V1,
                  OCUDU_DAPP_MODULATION_QAM256_V1}) {
    instance.pusch.modulation = qm;
    const cf32 data{axis_level(0, qm / 2) + 0.025F, axis_level(1, qm / 2) - 0.015F};
    for (size_t i = 0; i != instance.rx.size(); ++i) {
      if (instance.data_mask[i]) {
        instance.rx[i] = multiply({2.0F, 1.0F}, data);
      }
    }
    output.llrs               = tensor(llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {6U * qm});
    metrics.equalized_symbols = {};
    ASSERT_EQ(receive(input, output), OCUDU_DAPP_OK_V1);
    const auto expected_llrs  = llrs;
    const auto expected_noise = noise;
    metrics.equalized_symbols = tensor(symbols.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {6, 1});
    metrics.equalized_symbols.layout_id      = OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1;
    metrics.equalized_symbols.layout_version = 1;
    symbols.fill({123.0F, -456.0F});
    ASSERT_EQ(receive(input, output), OCUDU_DAPP_OK_V1);
    EXPECT_NE(output.reserved0 & OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1, 0U);
    EXPECT_EQ(llrs, expected_llrs);
    EXPECT_EQ(noise, expected_noise);
    for (unsigned i = 0; i != 6; ++i) {
      EXPECT_NEAR(symbols[i].real, data.real, 1.e-5F);
      EXPECT_NEAR(symbols[i].imag, data.imag, 1.e-5F);
    }
    EXPECT_EQ(symbols[6].real, 123.0F);
    EXPECT_EQ(symbols[7].imag, -456.0F);
    // Reusing the output must not retain the previous symbol-valid bit.
    metrics.equalized_symbols = {};
    ASSERT_EQ(receive(input, output), OCUDU_DAPP_OK_V1);
    EXPECT_EQ(output.reserved0 & OCUDU_DAPP_RECEIVER_EQUALIZED_SYMBOLS_VALID_V1, 0U);
  }
}

TEST(reference_phy_algorithms, receiver_accepts_actual_legacy_prefix_without_reading_symbol_tail)
{
  struct legacy_metrics {
    uint32_t                  struct_size;
    uint16_t                  abi_major;
    uint16_t                  abi_minor;
    ocudu_dapp_tensor_view_v1 post_equalization_noise;
    uint64_t                  reserved[4];
  } metrics{};
  static_assert(sizeof(metrics) == offsetof(ocudu_dapp_receiver_metrics_v1, equalized_symbols));
  fixture                      instance;
  std::array<int8_t, 12>       llrs{};
  std::array<float, 6>         noise{};
  ocudu_dapp_receiver_input_v1 input{};
  input.rx_grid                   = instance.rx_view();
  input.pusch                     = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs                      = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  metrics.struct_size             = sizeof(metrics);
  metrics.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  metrics.post_equalization_noise = tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {6, 1});
  metrics.post_equalization_noise.layout_id = OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1;
  ocudu_dapp_receiver_output_v1 output{};
  output.llrs    = tensor(llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {12});
  output.metrics = typed(OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1, metrics);
  ASSERT_EQ(receive(input, output), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(output.reserved0, OCUDU_DAPP_RECEIVER_METRICS_VALID_V1);
  metrics.struct_size = sizeof(ocudu_dapp_receiver_metrics_v1);
  EXPECT_EQ(receive(input, output), OCUDU_DAPP_INVALID_OUTPUT_V1);
  EXPECT_EQ(output.reserved0, 0U);
}

TEST(reference_phy_algorithms, receiver_rejects_malformed_or_aliased_symbol_requests_before_writing)
{
  for (unsigned probe = 0; probe != 15; ++probe) {
    SCOPED_TRACE(probe);
    fixture                      instance;
    std::array<int8_t, 12>       llrs{};
    std::array<float, 6>         noise{};
    std::array<cf32, 6>          symbols{};
    ocudu_dapp_receiver_input_v1 input{};
    input.rx_grid = instance.rx_view();
    input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
    input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
    ocudu_dapp_receiver_metrics_v1 metrics{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    metrics.post_equalization_noise =
        tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {6, 1});
    metrics.post_equalization_noise.layout_id = OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1;
    auto& v                                   = metrics.equalized_symbols;
    v                = tensor(symbols.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {6, 1});
    v.layout_id      = OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1;
    v.layout_version = 1;
    ocudu_dapp_receiver_output_v1 output{};
    output.llrs    = tensor(llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {12});
    output.metrics = typed(OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1, metrics);
    switch (probe) {
      case 0:
        v.element_type = OCUDU_DAPP_ELEMENT_F32_V1;
        break;
      case 1:
        v.layout_id = 0;
        break;
      case 2:
        v.layout_version = 2;
        break;
      case 3:
        v.dimensions[0] = 5;
        break;
      case 4:
        v.dimensions[1] = 2;
        break;
      case 5:
        v.byte_length = 1;
        break;
      case 6:
        v.native_handle = output.llrs.native_handle;
        break;
      case 7:
        v.native_handle = metrics.post_equalization_noise.native_handle;
        break;
      case 8:
        v.native_handle = input.rx_grid.native_handle;
        break;
      case 9:
        v.native_handle = reinterpret_cast<uintptr_t>(&metrics);
        break;
      case 10:
        v.native_handle = std::numeric_limits<uintptr_t>::max() - 3;
        break;
      case 11:
        v.access = OCUDU_DAPP_ACCESS_READ_V1;
        break;
      case 12:
        v.domain = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
        break;
      case 13:
        --output.metrics.metadata.size;
        break;
      case 14:
        output.metrics.metadata.data = reinterpret_cast<const unsigned char*>(&metrics) + 1;
        break;
    }
    llrs.fill(31);
    ASSERT_EQ(receive(input, output), OCUDU_DAPP_INVALID_OUTPUT_V1);
    for (auto value : llrs)
      EXPECT_EQ(value, 31);
    EXPECT_EQ(output.reserved0, 0U);
  }
}

TEST(reference_phy_algorithms, receiver_prefers_compact_data_re_indices)
{
  fixture                       instance;
  std::array<uint32_t, 2>       indices{{1, 7}};
  std::array<int8_t, 4>         llrs{};
  ocudu_dapp_receiver_input_v1  input{};
  ocudu_dapp_receiver_output_v1 output{};

  instance.data_mask.fill(0);
  instance.pusch.data_re_indices =
      tensor(indices.data(), OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1, {indices.size()});
  instance.pusch.nof_data_re = indices.size();
  input.rx_grid              = instance.rx_view();
  input.pusch                = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs                 = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  output.llrs                = tensor(llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {llrs.size()});

  ASSERT_EQ(receive(input, output), OCUDU_DAPP_OK_V1);
  for (int8_t llr : llrs) {
    EXPECT_GT(llr, 0);
  }
  EXPECT_GT(output.confidence_q16, 0U);
}

TEST(reference_phy_algorithms, receiver_metrics_are_optional_and_malformed_requests_are_rejected)
{
  fixture                      instance;
  std::array<int8_t, 12>       llrs{};
  std::array<float, 6>         noise{};
  ocudu_dapp_receiver_input_v1 input{};
  input.rx_grid = instance.rx_view();
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  ocudu_dapp_receiver_output_v1 output{};
  output.llrs = tensor(llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {llrs.size()});

  // Empty metrics preserve compatibility with existing v1 callers.
  ASSERT_EQ(receive(input, output), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(output.reserved0, 0U);

  ocudu_dapp_receiver_metrics_v1 metrics{};
  metrics.struct_size = sizeof(metrics);
  metrics.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  metrics.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  metrics.post_equalization_noise =
      tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {noise.size() - 1, 1});
  metrics.post_equalization_noise.layout_id      = OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1;
  metrics.post_equalization_noise.layout_version = 1;
  output.metrics                                 = typed(OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1, metrics);
  EXPECT_EQ(receive(input, output), OCUDU_DAPP_INVALID_OUTPUT_V1);

  metrics.post_equalization_noise.dimensions[0] = noise.size();
  metrics.post_equalization_noise.layout_id     = 0;
  EXPECT_EQ(receive(input, output), OCUDU_DAPP_INVALID_OUTPUT_V1);
}

TEST(reference_phy_algorithms, receiver_uses_canonical_interleaved_bits_for_all_nr_qam_orders)
{
  fixture                      instance;
  std::array<int8_t, 6 * 8>    llrs{};
  ocudu_dapp_receiver_input_v1 input{};
  input.rx_grid = instance.rx_view();
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);

  const cf32 channel{2.0F, 1.0F};
  for (auto modulation : {OCUDU_DAPP_MODULATION_QPSK_V1, OCUDU_DAPP_MODULATION_QAM16_V1, OCUDU_DAPP_MODULATION_QAM64_V1,
                          OCUDU_DAPP_MODULATION_QAM256_V1}) {
    const uint32_t axis_bits = static_cast<uint32_t>(modulation) / 2U;
    const uint32_t real_bits = 0b1010U & ((1U << axis_bits) - 1U);
    const uint32_t imag_bits = 0b0101U & ((1U << axis_bits) - 1U);
    const cf32     symbol{axis_level(real_bits, axis_bits), axis_level(imag_bits, axis_bits)};
    for (uint32_t grid_index = 0; grid_index != instance.rx.size(); ++grid_index) {
      if (instance.data_mask[grid_index] != 0) {
        instance.rx[grid_index] = multiply(channel, symbol);
      }
    }
    instance.pusch.modulation = modulation;
    ocudu_dapp_receiver_output_v1 output{};
    output.llrs = tensor(llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, OCUDU_DAPP_ACCESS_WRITE_V1,
                         {6U * static_cast<uint32_t>(modulation)});
    ASSERT_EQ(receive(input, output), OCUDU_DAPP_OK_V1);
    for (uint32_t bit = 0; bit != axis_bits; ++bit) {
      EXPECT_EQ(llrs[2U * bit] > 0, ((real_bits >> bit) & 1U) == 0);
      EXPECT_EQ(llrs[2U * bit + 1U] > 0, ((imag_bits >> bit) & 1U) == 0);
    }
  }
}

TEST(reference_phy_algorithms, rejects_an_output_with_the_wrong_shape)
{
  fixture                               instance;
  std::array<cf32, 1>                   estimates{};
  std::array<float, 1>                  noise{};
  ocudu_dapp_channel_estimator_input_v1 input{};
  input.rx_grid = instance.rx_view();
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  ocudu_dapp_channel_estimator_output_v1 output{};
  output.channel_estimates =
      tensor(estimates.data(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, 1, 1});
  output.noise_variances = tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1});

  EXPECT_EQ(estimate_channel(input, output), OCUDU_DAPP_INVALID_OUTPUT_V1);
}

TEST(reference_phy_algorithms, rejects_incomplete_or_unknown_class_a_metadata_flags)
{
  fixture                               instance;
  ocudu_dapp_channel_estimator_input_v1 input{};
  input.rx_grid = instance.rx_view();
  input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, instance.pusch);
  input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, instance.dmrs);
  ocudu_dapp_channel_estimator_output_v1 output{};

  instance.dmrs.flags = 0;
  EXPECT_EQ(estimate_channel(input, output), OCUDU_DAPP_INVALID_INPUT_V1);
  instance.dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  instance.dmrs.dmrs_type = 0;
  EXPECT_EQ(estimate_channel(input, output), OCUDU_DAPP_INVALID_INPUT_V1);
  instance.dmrs.dmrs_type = 1;
  instance.pusch.flags |= UINT32_C(1) << 31U;
  EXPECT_EQ(estimate_channel(input, output), OCUDU_DAPP_INVALID_INPUT_V1);
}
