// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_srs_channel_estimator.h"
#include "ocudu_dapp_sdk/srs_channel_estimator_vectors.h"
#include <cmath>
#include <gtest/gtest.h>
#include <initializer_list>

using namespace ocudu::dapp;
using namespace ocudu::dapp::sdk;

namespace {

ocudu_dapp_tensor_view_v1 host_tensor(void* data, uint32_t element_type, uint32_t element_size, uint32_t access,
                                      uint32_t layout_id, std::initializer_list<uint64_t> dimensions)
{
  ocudu_dapp_tensor_view_v1 view{};
  view.struct_size           = sizeof(view);
  view.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  view.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  view.domain                = OCUDU_DAPP_MEM_HOST_V1;
  view.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  view.element_type          = element_type;
  view.rank                  = dimensions.size();
  view.access                = access | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  view.allocation_id         = 1;
  view.allocation_generation = 1;
  view.lease_token           = 1;
  view.native_handle         = reinterpret_cast<uintptr_t>(data);
  view.layout_id             = layout_id;
  view.layout_version        = 1;
  uint64_t elements          = 1;
  uint32_t index             = 0;
  for (uint64_t dimension : dimensions) {
    view.dimensions[index++] = dimension;
    elements *= dimension;
  }
  uint64_t stride = element_size;
  for (uint32_t dimension = view.rank; dimension != 0; --dimension) {
    view.byte_strides[dimension - 1] = stride;
    stride *= view.dimensions[dimension - 1];
  }
  view.byte_length = elements * element_size;
  return view;
}

/// Host tensors over one vector's input and a fresh output.
struct invocation {
  explicit invocation(srs_vector& vector) : channel(vector.channel.size())
  {
    auto& srs                    = vector.metadata;
    input.invocation.struct_size = sizeof(input.invocation);
    input.invocation.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    input.rx_symbols = host_tensor(vector.rx_symbols.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(std::complex<float>),
                                   OCUDU_DAPP_ACCESS_READ_V1, OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1,
                                   {srs.nof_pilot_sets, srs.nof_rx_ports, srs.nof_symbols, srs.sequence_length});
    input.reference_symbols = host_tensor(
        vector.references.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(std::complex<float>), OCUDU_DAPP_ACCESS_READ_V1,
        OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1, {srs.nof_tx_ports, srs.sequence_length});
    input.srs.struct_size             = sizeof(input.srs);
    input.srs.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
    input.srs.metadata_schema_id      = OCUDU_DAPP_METADATA_SRS_CHANNEL_ESTIMATOR_V1;
    input.srs.metadata_schema_version = 1;
    input.srs.metadata                = {&srs, sizeof(srs), 0};
    output.channel_matrix =
        host_tensor(channel.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(std::complex<float>), OCUDU_DAPP_ACCESS_WRITE_V1,
                    OCUDU_DAPP_LAYOUT_SRS_CHANNEL_MATRIX_RX_TX_CF32_V1, {srs.nof_rx_ports, srs.nof_tx_ports});
    output.measurements =
        host_tensor(measurements.data(), OCUDU_DAPP_ELEMENT_F32_V1, sizeof(float), OCUDU_DAPP_ACCESS_WRITE_V1,
                    OCUDU_DAPP_LAYOUT_SRS_MEASUREMENTS_F32_V1, {OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1});
  }

  ocudu_dapp_srs_channel_estimator_input_v1              input{};
  ocudu_dapp_srs_channel_estimator_output_v1             output{};
  std::vector<std::complex<float>>                       channel;
  std::array<float, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1> measurements{};
};

double relative_error(const std::vector<std::complex<float>>& expected, const std::vector<std::complex<float>>& actual)
{
  double error = 0.0;
  double norm  = 0.0;
  for (size_t index = 0; index != expected.size(); ++index) {
    error += std::norm(std::complex<double>(actual[index]) - std::complex<double>(expected[index]));
    norm += std::norm(std::complex<double>(expected[index]));
  }
  return std::sqrt(error / norm);
}

std::vector<srs_vector> vectors()
{
  return read_srs_vectors(OCUDU_DAPP_SRS_VECTORS);
}

TEST(reference_srs_channel_estimator, reproduces_the_gnb_host_estimator_on_every_golden_occasion)
{
  auto                cases = vectors();
  const srs_tolerance tolerance;
  ASSERT_GE(cases.size(), 20U);
  for (size_t index = 0; index != cases.size(); ++index) {
    SCOPED_TRACE("case " + std::to_string(index));
    auto&      vector = cases[index];
    invocation call(vector);
    ASSERT_EQ(reference::estimate_srs_channel(call.input, call.output), OCUDU_DAPP_OK_V1);
    EXPECT_LT(relative_error(vector.channel, call.channel), tolerance.channel);
    const auto& expected = vector.measurements;
    const auto& actual   = call.measurements;
    EXPECT_NEAR(actual[OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1], expected[OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1],
                tolerance.power_db);
    EXPECT_NEAR(actual[OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1], expected[OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1],
                tolerance.power_db);
    const float noise = expected[OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1];
    EXPECT_NEAR(actual[OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1], noise, tolerance.noise * noise);
    const double resolution = vector.time_alignment[1];
    EXPECT_NEAR(actual[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1], vector.time_alignment[0],
                tolerance.time_alignment * resolution);
    EXPECT_FLOAT_EQ(actual[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_RESOLUTION_S_V1], static_cast<float>(resolution));
    EXPECT_FLOAT_EQ(actual[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MIN_S_V1],
                    static_cast<float>(vector.time_alignment[2]));
    EXPECT_FLOAT_EQ(actual[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MAX_S_V1],
                    static_cast<float>(vector.time_alignment[3]));
    EXPECT_TRUE(std::isnan(actual[7]));
  }
}

TEST(reference_srs_channel_estimator, search_window_matches_the_host_dft_estimator)
{
  // 156 comb-4 pilots at 30 kHz: 256-point IDFT, sampling 30.72 MHz; half CP 1.171875 us -> 36 taps, the cyclic-shift
  // range 1/(12 * 30 kHz * 4) -> 21 taps.
  ocudu_dapp_srs_channel_estimator_metadata_v1 srs{};
  srs.sequence_length       = 156;
  srs.comb_size             = 4;
  srs.subcarrier_spacing_hz = 30000;
  srs.numerology            = 1;
  srs.max_time_alignment_s  = 1.0 / (12.0 * 30000.0 * 4.0);
  auto search               = reference::make_srs_time_alignment_search(srs);
  EXPECT_EQ(search.dft_size, 256U);
  EXPECT_DOUBLE_EQ(search.sampling_rate_hz, 30.72e6);
  EXPECT_EQ(search.max_samples, 21U);
  EXPECT_TRUE(search.fractional);
  // Without the cyclic-shift limit the half cyclic prefix bounds the search exactly at 36 taps.
  srs.max_time_alignment_s = 1.0;
  EXPECT_EQ(reference::make_srs_time_alignment_search(srs).max_samples, 36U);
}

TEST(reference_srs_channel_estimator, fractional_peak_follows_the_host_curve_fit)
{
  const float symmetric[5] = {1.0F, 2.0F, 3.0F, 2.0F, 1.0F};
  EXPECT_FLOAT_EQ(reference::srs_fractional_peak(symmetric, 5), 0.0F);
  const float late[3] = {1.0F, 3.0F, 2.0F};
  // -0.5 * (0.5 * (2 - 1)) / (1 - 6 + 2) / 2: the peak sits a sixth of a tap late.
  EXPECT_NEAR(reference::srs_fractional_peak(late, 3), 1.0F / 6.0F, 1e-6F);
  const float flat[3] = {1.0F, 1.0F, 1.0F};
  EXPECT_FLOAT_EQ(reference::srs_fractional_peak(flat, 3), 0.0F);
  EXPECT_FLOAT_EQ(reference::srs_fractional_peak(symmetric, 4), 0.0F);
}

TEST(reference_srs_channel_estimator, rejects_malformed_invocations_without_writing)
{
  auto cases = vectors();
  ASSERT_FALSE(cases.empty());
  const auto corruptions = {
      +[](invocation& call, srs_vector&) { call.input.rx_symbols.layout_id = 0; },
      +[](invocation& call, srs_vector&) { call.output.channel_matrix.dimensions[1] += 1; },
      +[](invocation& call, srs_vector&) { call.output.measurements.domain = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1; },
      +[](invocation& call, srs_vector&) { call.input.srs.metadata_schema_id = 0; },
      +[](invocation&, srs_vector& vector) { vector.metadata.n_cs_max = 7; },
      +[](invocation&, srs_vector& vector) { vector.metadata.flags = OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1; },
      +[](invocation&, srs_vector& vector) { vector.metadata.subcarrier_spacing_hz = 20000; },
  };
  for (const auto corrupt : corruptions) {
    auto       vector = cases.front();
    invocation call(vector);
    corrupt(call, vector);
    call.measurements.fill(-7.0F);
    EXPECT_EQ(reference::estimate_srs_channel(call.input, call.output), OCUDU_DAPP_INVALID_INPUT_V1);
    EXPECT_EQ(call.measurements[0], -7.0F);
  }
}

} // namespace
