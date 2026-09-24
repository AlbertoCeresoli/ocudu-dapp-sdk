// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_srs_channel_estimator.h"
#include "ocudu_dapp_sdk/srs_channel_estimator_cuda.h"
#include "ocudu_dapp_sdk/srs_channel_estimator_vectors.h"
#include <cmath>
#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <initializer_list>

using namespace ocudu::dapp;
using namespace ocudu::dapp::sdk;

namespace {

ocudu_dapp_tensor_view_v1 tensor(void* data, uint32_t domain, uint32_t element_type, uint32_t element_size,
                                 uint32_t access, uint32_t layout_id, std::initializer_list<uint64_t> dimensions)
{
  const bool                host = domain == OCUDU_DAPP_MEM_HOST_V1;
  ocudu_dapp_tensor_view_v1 view{};
  view.struct_size           = sizeof(view);
  view.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  view.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  view.domain                = domain;
  view.backend_id            = host ? OCUDU_DAPP_BACKEND_HOST_V1 : OCUDU_DAPP_BACKEND_CUDA_V1;
  view.element_type          = element_type;
  view.rank                  = dimensions.size();
  view.access                = access | (host ? OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1 : 0);
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

/// Outputs of one estimate.
struct estimate {
  std::vector<std::complex<float>>                       channel;
  std::array<float, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1> measurements{};
};

/// Input and output descriptors over \p rx / \p reference / \p channel / \p measurements in \p domain.
void describe(srs_vector& vector, uint32_t domain, void* rx, void* reference, void* channel, void* measurements,
              uint64_t stream, ocudu_dapp_srs_channel_estimator_input_v1& input,
              ocudu_dapp_srs_channel_estimator_output_v1& output)
{
  auto& srs                         = vector.metadata;
  input.invocation.struct_size      = sizeof(input.invocation);
  input.invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.execution_stream = stream;
  input.rx_symbols = tensor(rx, domain, OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(float2), OCUDU_DAPP_ACCESS_READ_V1,
                            OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1,
                            {srs.nof_pilot_sets, srs.nof_rx_ports, srs.nof_symbols, srs.sequence_length});
  input.reference_symbols =
      tensor(reference, domain, OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(float2), OCUDU_DAPP_ACCESS_READ_V1,
             OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1, {srs.nof_tx_ports, srs.sequence_length});
  input.srs.struct_size             = sizeof(input.srs);
  input.srs.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  input.srs.metadata_schema_id      = OCUDU_DAPP_METADATA_SRS_CHANNEL_ESTIMATOR_V1;
  input.srs.metadata_schema_version = 1;
  input.srs.metadata                = {&srs, sizeof(srs), 0};
  output.channel_matrix =
      tensor(channel, domain, OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(float2), OCUDU_DAPP_ACCESS_WRITE_V1,
             OCUDU_DAPP_LAYOUT_SRS_CHANNEL_MATRIX_RX_TX_CF32_V1, {srs.nof_rx_ports, srs.nof_tx_ports});
  output.measurements =
      tensor(measurements, domain, OCUDU_DAPP_ELEMENT_F32_V1, sizeof(float), OCUDU_DAPP_ACCESS_WRITE_V1,
             OCUDU_DAPP_LAYOUT_SRS_MEASUREMENTS_F32_V1, {OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1});
}

class reference_srs_channel_estimator_cuda : public ::testing::Test
{
protected:
  void SetUp() override
  {
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
      static_cast<void>(cudaGetLastError());
      GTEST_SKIP() << "no CUDA device";
    }
    ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
    ASSERT_TRUE(worker.ready());
  }

  void TearDown() override
  {
    if (stream != nullptr) {
      cudaStreamDestroy(stream);
    }
  }

  estimate run_cuda(srs_vector& vector)
  {
    const size_t rx_bytes        = vector.rx_symbols.size() * sizeof(float2);
    const size_t reference_bytes = vector.references.size() * sizeof(float2);
    const size_t channel_bytes   = vector.channel.size() * sizeof(float2);
    const size_t value_bytes     = OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1 * sizeof(float);
    void *       rx = nullptr, *reference = nullptr, *channel = nullptr, *values = nullptr;
    EXPECT_EQ(cudaMalloc(&rx, rx_bytes), cudaSuccess);
    EXPECT_EQ(cudaMalloc(&reference, reference_bytes), cudaSuccess);
    EXPECT_EQ(cudaMalloc(&channel, channel_bytes), cudaSuccess);
    EXPECT_EQ(cudaMalloc(&values, value_bytes), cudaSuccess);
    EXPECT_EQ(cudaMemcpy(rx, vector.rx_symbols.data(), rx_bytes, cudaMemcpyHostToDevice), cudaSuccess);
    EXPECT_EQ(cudaMemcpy(reference, vector.references.data(), reference_bytes, cudaMemcpyHostToDevice), cudaSuccess);

    ocudu_dapp_srs_channel_estimator_input_v1  input{};
    ocudu_dapp_srs_channel_estimator_output_v1 output{};
    describe(vector, OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1, rx, reference, channel, values,
             reinterpret_cast<uintptr_t>(stream), input, output);
    estimate result;
    result.channel.resize(vector.channel.size());
    EXPECT_EQ(reference::estimate_srs_channel_cuda(input, output, state, worker), OCUDU_DAPP_OK_V1);
    EXPECT_EQ(cudaStreamSynchronize(stream), cudaSuccess);
    EXPECT_EQ(cudaMemcpy(result.channel.data(), channel, channel_bytes, cudaMemcpyDeviceToHost), cudaSuccess);
    EXPECT_EQ(cudaMemcpy(result.measurements.data(), values, value_bytes, cudaMemcpyDeviceToHost), cudaSuccess);
    for (void* pointer : {rx, reference, channel, values}) {
      cudaFree(pointer);
    }
    return result;
  }

  static estimate run_cpu(srs_vector& vector)
  {
    estimate                                   result;
    ocudu_dapp_srs_channel_estimator_input_v1  input{};
    ocudu_dapp_srs_channel_estimator_output_v1 output{};
    result.channel.resize(vector.channel.size());
    describe(vector, OCUDU_DAPP_MEM_HOST_V1, vector.rx_symbols.data(), vector.references.data(), result.channel.data(),
             result.measurements.data(), 0, input, output);
    EXPECT_EQ(reference::estimate_srs_channel(input, output), OCUDU_DAPP_OK_V1);
    return result;
  }

  cudaStream_t                                       stream = nullptr;
  reference::srs_channel_estimator_cuda_state        state;
  reference::srs_channel_estimator_cuda_worker_state worker;
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

void expect_close(const estimate& actual, const std::vector<std::complex<float>>& channel,
                  const std::array<float, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1>& expected, double resolution,
                  const srs_tolerance& tolerance)
{
  EXPECT_LT(relative_error(channel, actual.channel), tolerance.channel);
  for (const uint32_t index : {OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1, OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1}) {
    EXPECT_NEAR(actual.measurements[index], expected[index], tolerance.power_db) << "measurement " << index;
  }
  const float noise = expected[OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1];
  EXPECT_NEAR(actual.measurements[OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1], noise, tolerance.noise * noise);
  EXPECT_NEAR(actual.measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1],
              expected[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1], tolerance.time_alignment * resolution);
  for (const uint32_t index :
       {OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_RESOLUTION_S_V1, OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MIN_S_V1,
        OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MAX_S_V1}) {
    EXPECT_FLOAT_EQ(actual.measurements[index], expected[index]) << "measurement " << index;
  }
  EXPECT_TRUE(std::isnan(actual.measurements[7]));
}

TEST_F(reference_srs_channel_estimator_cuda, reproduces_the_gnb_host_estimator_on_every_golden_occasion)
{
  auto cases = read_srs_vectors(OCUDU_DAPP_SRS_VECTORS);
  ASSERT_GE(cases.size(), 20U);
  for (size_t index = 0; index != cases.size(); ++index) {
    SCOPED_TRACE("case " + std::to_string(index));
    expect_close(run_cuda(cases[index]), cases[index].channel, cases[index].measurements,
                 cases[index].time_alignment[1], srs_tolerance{});
  }
}

TEST_F(reference_srs_channel_estimator_cuda, agrees_with_the_cpu_reference)
{
  auto cases = read_srs_vectors(OCUDU_DAPP_SRS_VECTORS);
  for (size_t index = 0; index != cases.size(); ++index) {
    SCOPED_TRACE("case " + std::to_string(index));
    const auto cpu = run_cpu(cases[index]);
    expect_close(run_cuda(cases[index]), cpu.channel, cpu.measurements, cases[index].time_alignment[1],
                 srs_tolerance{});
  }
}

TEST_F(reference_srs_channel_estimator_cuda, rejects_host_tensors_and_a_missing_stream)
{
  auto                                       cases  = read_srs_vectors(OCUDU_DAPP_SRS_VECTORS);
  auto                                       vector = cases.front();
  estimate                                   result;
  ocudu_dapp_srs_channel_estimator_input_v1  input{};
  ocudu_dapp_srs_channel_estimator_output_v1 output{};
  result.channel.resize(vector.channel.size());
  describe(vector, OCUDU_DAPP_MEM_HOST_V1, vector.rx_symbols.data(), vector.references.data(), result.channel.data(),
           result.measurements.data(), reinterpret_cast<uintptr_t>(stream), input, output);
  EXPECT_EQ(reference::estimate_srs_channel_cuda(input, output, state, worker), OCUDU_DAPP_INVALID_INPUT_V1);
  input.invocation.execution_stream = 0;
  EXPECT_EQ(reference::estimate_srs_channel_cuda(input, output, state, worker), OCUDU_DAPP_INVALID_INPUT_V1);
}

} // namespace
