// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime_api.h>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <string>

namespace {

uint16_t bfloat16(float value)
{
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return static_cast<uint16_t>(bits >> 16U);
}

uint32_t pack(float real, float imag)
{
  return static_cast<uint32_t>(bfloat16(real)) | (static_cast<uint32_t>(bfloat16(imag)) << 16U);
}

struct capture {
  uint32_t               calls{};
  std::array<uint8_t, 2> confidence{};
  uint64_t               source_sequence{};
};

ocudu_dapp_status_v1 publish_prepared(void* context, uint32_t port, uint32_t schema, const void* object, uint32_t size)
{
  auto* output = static_cast<capture*>(context);
  if (output == nullptr || port != OCUDU_DAPP_SPECTRUM_OUTPUT_INTERFERENCE_MAP_V1 ||
      schema != OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1 || object == nullptr ||
      size != sizeof(ocudu_dapp_spectrum_interference_map_v1)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto& map = *static_cast<const ocudu_dapp_spectrum_interference_map_v1*>(object);
  if (map.nof_prbs != output->confidence.size() || map.prb_confidence.native_handle == 0) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  std::memcpy(output->confidence.data(), reinterpret_cast<const void*>(map.prb_confidence.native_handle),
              output->confidence.size());
  output->source_sequence = map.source_grid_sequence;
  ++output->calls;
  return OCUDU_DAPP_OK_V1;
}

TEST(spectrum_cuda_module, consumes_a_device_grid_and_returns_the_plan_aware_radiometer_map)
{
  const std::string path =
      std::getenv("OCUDU_SPECTRUM_CUDA_MODULE") != nullptr ? std::getenv("OCUDU_SPECTRUM_CUDA_MODULE") : "";
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
  ASSERT_EQ(module.interfaces[0].interface_id, OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  ASSERT_EQ(module.interfaces[0].capability_bits, OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1);

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
  const auto* api    = static_cast<const ocudu_dapp_spectrum_interface_v1*>(module.interfaces[0].interface_vtable);
  void*       worker = nullptr;
  ASSERT_EQ(api->worker.create_worker_context(instance, 4, &worker), OCUDU_DAPP_OK_V1);

  std::array<uint32_t, 48> grid{};
  std::fill_n(grid.begin(), 12, pack(2.0F, 0.0F));
  std::fill_n(grid.begin() + 12, 12, pack(0.0F, 0.0F));
  std::fill_n(grid.begin() + 24, 12, pack(10.0F, 0.0F));
  std::fill_n(grid.begin() + 36, 12, pack(0.0F, 0.0F));
  uint32_t*    device_grid = nullptr;
  cudaStream_t stream      = nullptr;
  ASSERT_EQ(cudaMalloc(reinterpret_cast<void**>(&device_grid), sizeof(grid)), cudaSuccess);
  ASSERT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
  ASSERT_EQ(cudaMemcpyAsync(device_grid, grid.data(), sizeof(grid), cudaMemcpyHostToDevice, stream), cudaSuccess);

  std::array<uint8_t, 4>               plan{{OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1,
                                             OCUDU_DAPP_UL_USE_PUSCH_V1, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1}};
  ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
  metadata.struct_size            = sizeof(metadata);
  metadata.abi_major              = OCUDU_DAPP_ABI_MAJOR_V1;
  metadata.abi_minor              = OCUDU_DAPP_ABI_MINOR_V1;
  metadata.rx_ports               = 1;
  metadata.symbols                = 2;
  metadata.subcarriers            = 24;
  metadata.amplitude_scale        = 1.0F;
  metadata.flags                  = OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1;
  metadata.source_plan_generation = 19;

  ocudu_dapp_spectrum_input_v1 input{};
  input.invocation.struct_size                     = sizeof(input.invocation);
  input.invocation.abi_major                       = OCUDU_DAPP_ABI_MAJOR_V1;
  input.invocation.abi_minor                       = OCUDU_DAPP_ABI_MINOR_V1;
  input.invocation.invocation_id                   = 19;
  input.invocation.cell_id                         = 2;
  input.invocation.worker_id                       = 4;
  input.invocation.radio_time                      = {5, 3, 0, 0};
  input.invocation.execution_stream                = reinterpret_cast<uintptr_t>(stream);
  input.invocation.input_generation                = 19;
  input.uplink_resource_grid.struct_size           = sizeof(input.uplink_resource_grid);
  input.uplink_resource_grid.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  input.uplink_resource_grid.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  input.uplink_resource_grid.domain                = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  input.uplink_resource_grid.backend_id            = OCUDU_DAPP_BACKEND_CUDA_V1;
  input.uplink_resource_grid.element_type          = OCUDU_DAPP_ELEMENT_CBF16_V1;
  input.uplink_resource_grid.rank                  = 3;
  input.uplink_resource_grid.access                = OCUDU_DAPP_ACCESS_READ_V1;
  input.uplink_resource_grid.allocation_id         = 1;
  input.uplink_resource_grid.allocation_generation = 19;
  input.uplink_resource_grid.native_handle         = reinterpret_cast<uintptr_t>(device_grid);
  input.uplink_resource_grid.byte_length           = sizeof(grid);
  input.uplink_resource_grid.dimensions[0]         = 1;
  input.uplink_resource_grid.dimensions[1]         = 2;
  input.uplink_resource_grid.dimensions[2]         = 24;
  input.uplink_resource_grid.byte_strides[0]       = sizeof(grid);
  input.uplink_resource_grid.byte_strides[1]       = 24 * sizeof(uint32_t);
  input.uplink_resource_grid.byte_strides[2]       = sizeof(uint32_t);
  input.uplink_resource_grid.layout_id             = OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1;
  input.uplink_resource_grid.layout_version        = 1;
  input.uplink_resource_grid.lease_token           = 19;
  input.scheduled_re_mask.struct_size              = sizeof(input.scheduled_re_mask);
  input.scheduled_re_mask.abi_major                = OCUDU_DAPP_ABI_MAJOR_V1;
  input.scheduled_re_mask.abi_minor                = OCUDU_DAPP_ABI_MINOR_V1;
  input.scheduled_re_mask.domain                   = OCUDU_DAPP_MEM_HOST_V1;
  input.scheduled_re_mask.backend_id               = OCUDU_DAPP_BACKEND_HOST_V1;
  input.scheduled_re_mask.element_type             = OCUDU_DAPP_ELEMENT_U8_V1;
  input.scheduled_re_mask.rank                     = 2;
  input.scheduled_re_mask.access                   = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  input.scheduled_re_mask.allocation_id            = 2;
  input.scheduled_re_mask.allocation_generation    = 19;
  input.scheduled_re_mask.native_handle            = reinterpret_cast<uintptr_t>(plan.data());
  input.scheduled_re_mask.byte_length              = plan.size();
  input.scheduled_re_mask.dimensions[0]            = 2;
  input.scheduled_re_mask.dimensions[1]            = 2;
  input.scheduled_re_mask.byte_strides[0]          = 2;
  input.scheduled_re_mask.byte_strides[1]          = 1;
  input.scheduled_re_mask.layout_id                = OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1;
  input.scheduled_re_mask.layout_version           = 1;
  input.scheduled_re_mask.lease_token              = 19;
  input.slot_metadata.struct_size                  = sizeof(input.slot_metadata);
  input.slot_metadata.abi_major                    = OCUDU_DAPP_ABI_MAJOR_V1;
  input.slot_metadata.abi_minor                    = OCUDU_DAPP_ABI_MINOR_V1;
  input.slot_metadata.metadata_schema_id           = OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1;
  input.slot_metadata.metadata_schema_version      = 1;
  input.slot_metadata.metadata                     = {&metadata, sizeof(metadata), 0};
  input.source_sequence                            = 19;

  EXPECT_EQ(api->publish(worker, &input), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(captured.calls, 1U);
  EXPECT_EQ(captured.confidence, (std::array<uint8_t, 2>{{UINT8_MAX, 0}}));
  EXPECT_EQ(captured.source_sequence, 19U);

  api->worker.destroy_worker_context(instance, worker);
  EXPECT_EQ(module.stop(instance), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(module.quiesce(instance, 0), OCUDU_DAPP_OK_V1);
  module.destroy(instance);
  EXPECT_EQ(cudaStreamDestroy(stream), cudaSuccess);
  EXPECT_EQ(cudaFree(device_grid), cudaSuccess);
  EXPECT_EQ(::dlclose(library), 0);
}

} // namespace
