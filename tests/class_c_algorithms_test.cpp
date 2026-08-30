// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/class_c_algorithms.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>

using namespace ocudu::dapp::reference;

namespace {

struct complex32 {
  float real;
  float imag;
};

struct complex_bf16 {
  uint16_t real;
  uint16_t imag;
};

uint16_t bf16(float value)
{
  uint32_t bits{};
  std::memcpy(&bits, &value, sizeof(bits));
  return static_cast<uint16_t>(bits >> 16U);
}

strided_complex_view contiguous(const complex32* data, uint64_t rows, uint64_t columns)
{
  strided_complex_view view{};
  view.data            = reinterpret_cast<const uint8_t*>(data);
  view.byte_length     = rows * columns * sizeof(complex32);
  view.rank            = 2;
  view.dimensions[0]   = rows;
  view.dimensions[1]   = columns;
  view.byte_strides[0] = columns * sizeof(complex32);
  view.byte_strides[1] = sizeof(complex32);
  return view;
}

strided_complex_view contiguous(const complex32* data, uint64_t ports, uint64_t symbols, uint64_t subcarriers)
{
  strided_complex_view view{};
  view.data            = reinterpret_cast<const uint8_t*>(data);
  view.byte_length     = ports * symbols * subcarriers * sizeof(complex32);
  view.rank            = 3;
  view.dimensions[0]   = ports;
  view.dimensions[1]   = symbols;
  view.dimensions[2]   = subcarriers;
  view.byte_strides[0] = symbols * subcarriers * sizeof(complex32);
  view.byte_strides[1] = subcarriers * sizeof(complex32);
  view.byte_strides[2] = sizeof(complex32);
  return view;
}

} // namespace

TEST(class_c_algorithms, radiometer_power_and_occupancy_are_deterministic)
{
  const std::array<complex32, 6> samples{{{1, 0}, {0, 2}, {0, 0}, {1, 1}, {3, 0}, {0, 1}}};
  spectrum_radiometer_result     result{};
  ASSERT_TRUE(run_spectrum_radiometer(contiguous(samples.data(), 2, 3), 2.0, 6.0, result));
  EXPECT_EQ(result.samples, 6U);
  EXPECT_DOUBLE_EQ(result.mean_power, 68.0 / 6.0);
  EXPECT_DOUBLE_EQ(result.peak_power, 36.0);
  EXPECT_DOUBLE_EQ(result.occupied_fraction, 0.5);
}

TEST(class_c_algorithms, radiometer_honors_noncontiguous_strides)
{
  std::array<uint8_t, 40>        storage{};
  const std::array<complex32, 4> values{{{1, 0}, {0, 2}, {3, 0}, {0, 4}}};
  std::memcpy(storage.data(), &values[0], sizeof(complex32));
  std::memcpy(storage.data() + 8, &values[1], sizeof(complex32));
  std::memcpy(storage.data() + 24, &values[2], sizeof(complex32));
  std::memcpy(storage.data() + 32, &values[3], sizeof(complex32));
  strided_complex_view view{};
  view.data            = storage.data();
  view.byte_length     = storage.size();
  view.rank            = 2;
  view.dimensions[0]   = 2;
  view.dimensions[1]   = 2;
  view.byte_strides[0] = 24;
  view.byte_strides[1] = 8;
  spectrum_radiometer_result result{};
  ASSERT_TRUE(run_spectrum_radiometer(view, 1.0, 8.0, result));
  EXPECT_DOUBLE_EQ(result.mean_power, 7.5);
  EXPECT_DOUBLE_EQ(result.occupied_fraction, 0.5);
}

TEST(class_c_algorithms, prb_radiometer_emits_bounded_binary_confidence)
{
  std::array<complex32, 48> samples{};
  for (uint32_t symbol = 0; symbol != 2; ++symbol) {
    for (uint32_t subcarrier = 0; subcarrier != 12; ++subcarrier) {
      samples[symbol * 24 + subcarrier] = {2.0F, 0.0F};
    }
  }
  std::array<uint8_t, 2> confidence{};
  uint32_t               nof_prbs = 0;
  ASSERT_TRUE(run_spectrum_prb_radiometer(contiguous(samples.data(), 1, 2, 24), 1.0, 1.0, 12, confidence.data(),
                                          confidence.size(), nof_prbs));
  EXPECT_EQ(nof_prbs, 2U);
  EXPECT_EQ(confidence, (std::array<uint8_t, 2>{255, 0}));

  EXPECT_FALSE(
      run_spectrum_prb_radiometer(contiguous(samples.data(), 1, 2, 24), 1.0, 1.0, 12, confidence.data(), 1, nof_prbs));
}

TEST(class_c_algorithms, planned_ul_energy_is_excluded_from_interference_detection)
{
  std::array<complex32, 48> samples{};
  for (uint32_t subcarrier = 0; subcarrier != 12; ++subcarrier) {
    samples[subcarrier]           = {2.0F, 0.0F};
    samples[24 + 12 + subcarrier] = {2.0F, 0.0F};
  }
  const std::array<uint8_t, 4> plan{{OCUDU_DAPP_UL_USE_PUSCH_V1, OCUDU_DAPP_UL_USE_PUSCH_V1,
                                     OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1}};
  const expected_ul_use_view   expected{plan.data(), 2, 2, 9};
  std::array<uint8_t, 2>       confidence{};
  uint32_t                     nof_prbs = 0;
  spectrum_radiometer_result   combined{};
  ASSERT_TRUE(run_spectrum_prb_radiometer(contiguous(samples.data(), 1, 2, 24), expected, 1.0, 1.0, 12,
                                          confidence.data(), confidence.size(), nof_prbs, &combined));
  EXPECT_EQ(nof_prbs, 2U);
  EXPECT_EQ(confidence, (std::array<uint8_t, 2>{0, 255}));
  EXPECT_EQ(combined.samples, samples.size());
  EXPECT_DOUBLE_EQ(combined.mean_power, 2.0);
  EXPECT_DOUBLE_EQ(combined.peak_power, 4.0);
  EXPECT_DOUBLE_EQ(combined.occupied_fraction, 0.5);
}

TEST(class_c_algorithms, radiometer_consumes_native_complex_bf16_without_conversion_storage)
{
  const std::array<complex_bf16, 4> samples{
      {{bf16(1), bf16(0)}, {bf16(0), bf16(2)}, {bf16(1), bf16(1)}, {bf16(0), bf16(0)}}};
  strided_complex_view view{};
  view.data            = reinterpret_cast<const uint8_t*>(samples.data());
  view.byte_length     = sizeof(samples);
  view.element_type    = OCUDU_DAPP_ELEMENT_CBF16_V1;
  view.rank            = 2;
  view.dimensions[0]   = 2;
  view.dimensions[1]   = 2;
  view.byte_strides[0] = 2 * sizeof(complex_bf16);
  view.byte_strides[1] = sizeof(complex_bf16);
  spectrum_radiometer_result result{};
  ASSERT_TRUE(run_spectrum_radiometer(view, 1.0, 1.5, result));
  EXPECT_DOUBLE_EQ(result.mean_power, 7.0 / 4.0);
  EXPECT_DOUBLE_EQ(result.peak_power, 4.0);
  EXPECT_DOUBLE_EQ(result.occupied_fraction, 0.5);
}

TEST(class_c_algorithms, srs_placeholder_forms_a_bounded_cir)
{
  std::array<complex32, 8> samples{};
  samples.fill({1, 0});
  srs_cir_analysis result{};
  ASSERT_TRUE(estimate_srs_cir_placeholder(contiguous(samples.data(), 2, 4), 0.25, result));
  EXPECT_EQ(result.samples, 8U);
  EXPECT_EQ(result.channel_rows, 2U);
  EXPECT_EQ(result.tap_count, 4U);
  EXPECT_EQ(result.peak_delay_bin, 0U);
  EXPECT_DOUBLE_EQ(result.mean_channel_power, 1.0);
  EXPECT_DOUBLE_EQ(result.noise_corrected_power, 0.75);
  EXPECT_DOUBLE_EQ(result.peak_cir_power, 1.0);
  EXPECT_DOUBLE_EQ(result.tap_power[0], 1.0);
}

TEST(class_c_algorithms, srs_preview_is_bounded_pointer_free_and_canonical)
{
  std::array<complex32, 8> samples{};
  samples.fill({1, 0});
  srs_cir_analysis analysis{};
  ASSERT_TRUE(estimate_srs_cir_placeholder(contiguous(samples.data(), 2, 4), 0.25, analysis));

  ocudu_dapp_srs_isac_cir_preview_v1 preview{};
  ASSERT_TRUE(make_srs_cir_preview(analysis, 0.25F, preview));
  EXPECT_EQ(preview.struct_size, sizeof(preview));
  EXPECT_EQ(preview.abi_major, OCUDU_DAPP_ABI_MAJOR_V1);
  EXPECT_EQ(preview.tap_count, 4U);
  EXPECT_EQ(preview.peak_delay_bin, 0U);
  EXPECT_FLOAT_EQ(preview.mean_channel_power, 1.0F);
  EXPECT_FLOAT_EQ(preview.noise_corrected_power, 0.75F);
  EXPECT_FLOAT_EQ(preview.peak_cir_power, 1.0F);
  EXPECT_FLOAT_EQ(preview.noise_variance, 0.25F);
  EXPECT_TRUE(std::all_of(preview.tap_power + preview.tap_count,
                          preview.tap_power + OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1,
                          [](float value) { return value == 0.0F; }));

  analysis.tap_power[0] = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(make_srs_cir_preview(analysis, 0.25F, preview));
}

TEST(class_c_algorithms, srs_cir_peak_tracks_a_frequency_domain_delay)
{
  std::array<complex32, 8> samples{};
  constexpr double         two_pi = 6.283185307179586476925286766559;
  for (std::size_t frequency = 0; frequency != samples.size(); ++frequency) {
    const double angle = -two_pi * 2.0 * static_cast<double>(frequency) / samples.size();
    samples[frequency] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
  }
  srs_cir_analysis result{};
  ASSERT_TRUE(estimate_srs_cir_placeholder(contiguous(samples.data(), 1, samples.size()), 0.0, result));
  EXPECT_EQ(result.peak_delay_bin, 2U);
  EXPECT_NEAR(result.peak_cir_power, 1.0, 1e-6);
}

TEST(class_c_algorithms, malformed_and_nonfinite_inputs_are_rejected)
{
  const std::array<complex32, 1> samples{{{1, 0}}};
  auto                           view = contiguous(samples.data(), 1, 1);
  view.byte_length                    = 7;
  spectrum_radiometer_result spectrum{};
  EXPECT_FALSE(run_spectrum_radiometer(view, 1.0, 0.0, spectrum));
  view.byte_length = sizeof(complex32);
  srs_cir_analysis srs{};
  EXPECT_FALSE(estimate_srs_cir_placeholder(view, -1.0, srs));
}

TEST(class_c_algorithms, host_tensor_adapter_enforces_memory_contract)
{
  const std::array<complex32, 2> samples{{{1, 0}, {0, 1}}};
  ocudu_dapp_tensor_view_v1      tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
  tensor.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  tensor.rank                  = 1;
  tensor.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.allocation_id         = 1;
  tensor.allocation_generation = 1;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(samples.data());
  tensor.byte_length           = sizeof(samples);
  tensor.dimensions[0]         = samples.size();
  tensor.byte_strides[0]       = sizeof(complex32);
  tensor.lease_token           = 1;
  strided_complex_view view{};
  ASSERT_TRUE(make_host_complex_view(tensor, view));
  EXPECT_EQ(view.data, reinterpret_cast<const uint8_t*>(samples.data()));
  tensor.domain = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  EXPECT_FALSE(make_host_complex_view(tensor, view));
}

TEST(class_c_algorithms, expected_use_adapter_enforces_layout_generation_and_categories)
{
  std::array<uint8_t, 4> plan{{OCUDU_DAPP_UL_USE_PUSCH_V1, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1, OCUDU_DAPP_UL_USE_SRS_V1,
                               OCUDU_DAPP_UL_USE_REFERENCE_SIGNAL_V1}};
  ocudu_dapp_tensor_view_v1 tensor{};
  tensor.struct_size           = sizeof(tensor);
  tensor.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  tensor.domain                = OCUDU_DAPP_MEM_HOST_V1;
  tensor.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  tensor.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
  tensor.rank                  = 2;
  tensor.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  tensor.allocation_id         = 1;
  tensor.allocation_generation = 7;
  tensor.native_handle         = reinterpret_cast<uintptr_t>(plan.data());
  tensor.byte_length           = plan.size();
  tensor.dimensions[0]         = 2;
  tensor.dimensions[1]         = 2;
  tensor.byte_strides[0]       = 2;
  tensor.byte_strides[1]       = 1;
  tensor.layout_id             = OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1;
  tensor.layout_version        = 1;
  tensor.lease_token           = 7;
  expected_ul_use_view view{};
  ASSERT_TRUE(make_host_expected_ul_use_view(tensor, 7, view));
  EXPECT_EQ(view.data, plan.data());
  EXPECT_EQ(view.symbols, 2U);
  EXPECT_EQ(view.prbs, 2U);
  plan[0] = UINT8_MAX;
  EXPECT_FALSE(make_host_expected_ul_use_view(tensor, 7, view));
}
