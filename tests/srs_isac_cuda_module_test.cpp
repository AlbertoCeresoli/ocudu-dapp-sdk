// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cuda_runtime_api.h>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <string>

namespace {

struct capture {
  uint32_t                           calls{};
  ocudu_dapp_srs_isac_cir_preview_v1 preview{};
};

ocudu_dapp_status_v1 publish_prepared(void* context, uint32_t port, uint32_t schema, const void* object, uint32_t size)
{
  auto* output = static_cast<capture*>(context);
  if (output == nullptr || port != OCUDU_DAPP_SRS_ISAC_OUTPUT_CIR_PREVIEW_V1 ||
      schema != OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 || object == nullptr ||
      size != sizeof(ocudu_dapp_srs_isac_cir_preview_v1)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  output->preview = *static_cast<const ocudu_dapp_srs_isac_cir_preview_v1*>(object);
  ++output->calls;
  return OCUDU_DAPP_OK_V1;
}

TEST(srs_isac_cuda_module, consumes_device_channel_estimates_and_returns_a_bounded_cir_preview)
{
  const std::string path =
      std::getenv("OCUDU_SRS_ISAC_CUDA_MODULE") != nullptr ? std::getenv("OCUDU_SRS_ISAC_CUDA_MODULE") : "";
  ASSERT_FALSE(path.empty());
  void* library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(library, nullptr) << ::dlerror();
  auto query = reinterpret_cast<ocudu_dapp_query_v1_fn>(::dlsym(library, "ocudu_dapp_query_v1"));
  ASSERT_NE(query, nullptr);

  ocudu_dapp_host_info_v1 info{};
  info.struct_size  = sizeof(info);
  info.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
  info.abi_minor    = OCUDU_DAPP_ABI_MINOR_V1;
  info.backend_mask = uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;
  ocudu_dapp_module_v1 module{};
  ASSERT_EQ(query(&info, &module), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(module.nof_interfaces, 1U);
  ASSERT_EQ(module.interfaces[0].interface_id, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  ASSERT_EQ(module.interfaces[0].placement_mask, uint32_t{1} << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1));

  capture                captured{};
  ocudu_dapp_host_api_v1 host{};
  host.struct_size      = sizeof(host);
  host.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  host.host_context     = &captured;
  host.publish_prepared = publish_prepared;
  void* instance        = nullptr;
  ASSERT_EQ(module.create(&host, nullptr, &instance), OCUDU_DAPP_OK_V1);
  ASSERT_NE(instance, nullptr);
  ASSERT_EQ(module.prepare(instance, nullptr), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(module.start(instance), OCUDU_DAPP_OK_V1);
  const auto* api    = static_cast<const ocudu_dapp_srs_isac_interface_v1*>(module.interfaces[0].interface_vtable);
  void*       worker = nullptr;
  ASSERT_EQ(api->worker.create_worker_context(instance, 7, &worker), OCUDU_DAPP_OK_V1);

  constexpr uint32_t              frequencies = 8;
  std::array<float2, frequencies> channel{};
  for (auto& value : channel) {
    value = {1.0F, 0.0F};
  }
  float2*      device_channel = nullptr;
  cudaStream_t stream         = nullptr;
  ASSERT_EQ(cudaMalloc(reinterpret_cast<void**>(&device_channel), sizeof(channel)), cudaSuccess);
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(device_channel, channel.data(), sizeof(channel), cudaMemcpyHostToDevice, stream),
            cudaSuccess);

  ocudu_dapp_srs_isac_metadata_v1 metadata{};
  metadata.struct_size           = sizeof(metadata);
  metadata.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.srs_resource_id       = 3;
  metadata.srs_config_id         = 5;
  metadata.rx_ports              = 1;
  metadata.tx_ports              = 1;
  metadata.comb_size             = 2;
  metadata.first_subcarrier      = 12;
  metadata.subcarrier_spacing_hz = 30'000;
  metadata.frequency_points      = frequencies;
  metadata.noise_variance        = 0.25F;
  metadata.rsrp_db               = -8.0F;
  metadata.epre_db               = -7.0F;

  ocudu_dapp_srs_isac_input_v1 input{};
  input.invocation.struct_size                  = sizeof(input.invocation);
  input.invocation.abi_major                    = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor                    = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id                = 19;
  input.invocation.cell_id                      = 2;
  input.invocation.worker_id                    = 7;
  input.invocation.radio_time                   = {5, 3, 0, 0};
  input.invocation.execution_stream             = reinterpret_cast<uintptr_t>(stream);
  input.invocation.input_generation             = 19;
  input.channel_estimates.struct_size           = sizeof(input.channel_estimates);
  input.channel_estimates.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  input.channel_estimates.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  input.channel_estimates.domain                = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  input.channel_estimates.backend_id            = OCUDU_DAPP_BACKEND_CUDA_V1;
  input.channel_estimates.element_type          = OCUDU_DAPP_ELEMENT_CF32_V1;
  input.channel_estimates.rank                  = 3;
  input.channel_estimates.access                = OCUDU_DAPP_ACCESS_READ_V1;
  input.channel_estimates.allocation_id         = 1;
  input.channel_estimates.allocation_generation = 19;
  input.channel_estimates.native_handle         = reinterpret_cast<uintptr_t>(device_channel);
  input.channel_estimates.byte_length           = sizeof(channel);
  input.channel_estimates.dimensions[0]         = 1;
  input.channel_estimates.dimensions[1]         = 1;
  input.channel_estimates.dimensions[2]         = frequencies;
  input.channel_estimates.byte_strides[0]       = sizeof(channel);
  input.channel_estimates.byte_strides[1]       = sizeof(channel);
  input.channel_estimates.byte_strides[2]       = sizeof(float2);
  input.channel_estimates.layout_id             = OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1;
  input.channel_estimates.layout_version        = 1;
  input.channel_estimates.lease_token           = 19;
  input.srs_metadata.struct_size                = sizeof(input.srs_metadata);
  input.srs_metadata.abi_major                  = OCUDU_DAPP_ABI_MAJOR_V1;
  input.srs_metadata.abi_minor                  = OCUDU_DAPP_ABI_MINOR_V1;
  input.srs_metadata.metadata_schema_id         = OCUDU_DAPP_METADATA_SRS_ISAC_V1;
  input.srs_metadata.metadata_schema_version    = 1;
  input.srs_metadata.metadata                   = {&metadata, sizeof(metadata), 0};
  input.source_sequence                         = 19;

  EXPECT_EQ(api->publish(worker, &input), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(captured.calls, 1U);
  EXPECT_EQ(captured.preview.source_srs_sequence, 19U);
  EXPECT_EQ(captured.preview.srs_resource_id, 3U);
  EXPECT_EQ(captured.preview.srs_config_id, 5U);
  EXPECT_EQ(captured.preview.tap_count, frequencies);
  EXPECT_EQ(captured.preview.peak_delay_bin, 0U);
  EXPECT_NEAR(captured.preview.mean_channel_power, 1.0F, 1.0e-5F);
  EXPECT_NEAR(captured.preview.noise_corrected_power, 0.75F, 1.0e-5F);
  EXPECT_NEAR(captured.preview.tap_power[0], 1.0F, 1.0e-5F);
  for (uint32_t tap = 1; tap != frequencies; ++tap) {
    EXPECT_NEAR(captured.preview.tap_power[tap], 0.0F, 1.0e-5F);
  }

  input.channel_estimates.domain = OCUDU_DAPP_MEM_HOST_V1;
  EXPECT_EQ(api->publish(worker, &input), OCUDU_DAPP_INVALID_INPUT_V1);
  input.channel_estimates.domain    = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  input.channel_estimates.layout_id = 0;
  EXPECT_EQ(api->publish(worker, &input), OCUDU_DAPP_INVALID_INPUT_V1);
  input.channel_estimates.layout_id = OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1;
  input.invocation.execution_stream = 0;
  EXPECT_EQ(api->publish(worker, &input), OCUDU_DAPP_INVALID_INPUT_V1);
  EXPECT_EQ(captured.calls, 1U);

  api->worker.destroy_worker_context(instance, worker);
  EXPECT_EQ(module.stop(instance), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(module.quiesce(instance, 0), OCUDU_DAPP_OK_V1);
  module.destroy(instance);
  EXPECT_EQ(cudaStreamDestroy(stream), cudaSuccess);
  EXPECT_EQ(cudaFree(device_channel), cudaSuccess);
  EXPECT_EQ(::dlclose(library), 0);
}

} // namespace
