// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu_dapp_sdk/reference_phy_algorithms_cuda.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <initializer_list>
#include <string>

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
ocudu_dapp_typed_metadata_v1 metadata(uint32_t schema, T& value)
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

receiver_cuda_model_artifact_v1 make_model(float scale)
{
  receiver_cuda_model_artifact_v1 result;
  for (uint32_t bits = 1; bits <= 4; ++bits) {
    for (uint32_t label = 0; label != (1U << bits); ++label) {
      result.constellation_levels[(bits - 1U) * 16U + label] = scale * axis_level(label, bits);
    }
  }
  return result;
}

ocudu_dapp_model_request_v1 request(uint32_t action, const char* id, uint64_t generation,
                                    const receiver_cuda_model_artifact_v1* artifact = nullptr)
{
  ocudu_dapp_model_request_v1 result{};
  result.struct_size         = sizeof(result);
  result.action              = action;
  result.model_id            = id;
  result.model_version       = "1";
  result.expected_generation = generation;
  if (artifact != nullptr) {
    result.artifact = {artifact, sizeof(*artifact), 0};
  }
  return result;
}

class receiver_module_fixture : public ::testing::Test
{
protected:
  void SetUp() override
  {
    const std::string path =
        std::getenv("OCUDU_RECEIVER_CUDA_MODULE") != nullptr ? std::getenv("OCUDU_RECEIVER_CUDA_MODULE") : "";
    ASSERT_FALSE(path.empty());
    library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(library, nullptr) << ::dlerror();
    auto query = reinterpret_cast<ocudu_dapp_query_v1_fn>(::dlsym(library, "ocudu_dapp_query_v1"));
    ASSERT_NE(query, nullptr);
    ocudu_dapp_host_info_v1 info{};
    info.struct_size  = sizeof(info);
    info.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
    info.abi_minor    = OCUDU_DAPP_ABI_MINOR_V1;
    info.backend_mask = uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;
    ASSERT_EQ(query(&info, &module), OCUDU_DAPP_OK_V1);

    host.struct_size = sizeof(host);
    host.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    host.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    ASSERT_EQ(module.create(&host, nullptr, &instance), OCUDU_DAPP_OK_V1);
    ASSERT_EQ(module.prepare(instance, nullptr), OCUDU_DAPP_OK_V1);
    ASSERT_EQ(module.start(instance), OCUDU_DAPP_OK_V1);
    const auto* api = static_cast<const ocudu_dapp_receiver_interface_v1*>(module.interfaces[0].interface_vtable);
    ASSERT_EQ(api->worker.create_worker_context(instance, 3, &worker3), OCUDU_DAPP_OK_V1);
    ASSERT_EQ(api->worker.create_worker_context(instance, 9, &worker9), OCUDU_DAPP_OK_V1);
  }

  void TearDown() override
  {
    if (instance != nullptr) {
      const auto* api = static_cast<const ocudu_dapp_receiver_interface_v1*>(module.interfaces[0].interface_vtable);
      if (worker3 != nullptr) {
        api->worker.destroy_worker_context(instance, worker3);
      }
      if (worker9 != nullptr) {
        api->worker.destroy_worker_context(instance, worker9);
      }
      EXPECT_EQ(module.stop(instance), OCUDU_DAPP_OK_V1);
      EXPECT_EQ(module.quiesce(instance, 0), OCUDU_DAPP_OK_V1);
      module.destroy(instance);
    }
    if (library != nullptr) {
      ::dlclose(library);
    }
  }

  ocudu_dapp_status_v1 apply(ocudu_dapp_model_request_v1& operation)
  {
    return module.management->manage_model(instance, &operation, &generation);
  }

  std::array<float, 4> invoke_llrs(void* worker)
  {
    std::array<uint32_t, 8> grid{pack_cbf16(2.0F, 0.0F), pack_cbf16(2.0F, 2.0F)};
    std::array<float2, 1>   references{{{1.0F, 0.0F}}};
    std::array<uint16_t, 2> coordinates{{0, 0}};
    std::array<uint32_t, 1> indices{{1}};
    cudaStream_t            stream{};
    EXPECT_EQ(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), cudaSuccess);
    uint32_t* device_grid{};
    float2*   device_references{};
    uint16_t* device_coordinates{};
    uint32_t* device_indices{};
    __half*   device_llrs{};
    EXPECT_EQ(cudaMalloc(&device_grid, sizeof(grid)), cudaSuccess);
    EXPECT_EQ(cudaMalloc(&device_references, sizeof(references)), cudaSuccess);
    EXPECT_EQ(cudaMalloc(&device_coordinates, sizeof(coordinates)), cudaSuccess);
    EXPECT_EQ(cudaMalloc(&device_indices, sizeof(indices)), cudaSuccess);
    EXPECT_EQ(cudaMalloc(&device_llrs, 4 * sizeof(__half)), cudaSuccess);
    EXPECT_EQ(cudaMemcpyAsync(device_grid, grid.data(), sizeof(grid), cudaMemcpyHostToDevice, stream), cudaSuccess);
    EXPECT_EQ(cudaMemcpyAsync(device_references, references.data(), sizeof(references), cudaMemcpyHostToDevice, stream),
              cudaSuccess);
    EXPECT_EQ(
        cudaMemcpyAsync(device_coordinates, coordinates.data(), sizeof(coordinates), cudaMemcpyHostToDevice, stream),
        cudaSuccess);
    EXPECT_EQ(cudaMemcpyAsync(device_indices, indices.data(), sizeof(indices), cudaMemcpyHostToDevice, stream),
              cudaSuccess);

    ocudu_dapp_pusch_metadata_v1 pusch{};
    pusch.struct_size          = sizeof(pusch);
    pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
    pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
    pusch.nof_rx_ports         = 1;
    pusch.nof_layers           = 1;
    pusch.modulation           = OCUDU_DAPP_MODULATION_QAM16_V1;
    pusch.nof_symbols          = 2;
    pusch.nof_subcarriers      = 4;
    pusch.nof_data_re          = 1;
    pusch.noise_variance_floor = 0.01F;
    pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
    pusch.data_re_indices = device_tensor(device_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1},
                                          OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);
    ocudu_dapp_dmrs_metadata_v1 dmrs{};
    dmrs.struct_size = sizeof(dmrs);
    dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    dmrs.nof_pilots  = 1;
    dmrs.dmrs_type   = 1;
    dmrs.flags =
        OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
    dmrs.reference_symbols =
        device_tensor(device_references, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, 1});
    dmrs.coordinates = device_tensor(device_coordinates, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, 2});

    ocudu_dapp_receiver_input_v1 input{};
    input.invocation.struct_size      = sizeof(input.invocation);
    input.invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
    input.invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
    input.invocation.execution_stream = reinterpret_cast<uintptr_t>(stream);
    input.rx_grid = device_tensor(device_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, 2, 4},
                                  OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);
    input.pusch   = metadata(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
    input.dmrs    = metadata(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);
    ocudu_dapp_receiver_output_v1 output{};
    output.llrs     = device_tensor(device_llrs, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, 4},
                                    OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
    const auto* api = static_cast<const ocudu_dapp_receiver_interface_v1*>(module.interfaces[0].interface_vtable);
    EXPECT_EQ(api->invoke(worker, &input, &output), OCUDU_DAPP_OK_V1);
    EXPECT_EQ(cudaStreamSynchronize(stream), cudaSuccess);

    std::array<__half, 4> raw{};
    EXPECT_EQ(cudaMemcpy(raw.data(), device_llrs, sizeof(raw), cudaMemcpyDeviceToHost), cudaSuccess);
    std::array<float, 4> result{};
    for (uint32_t index = 0; index != result.size(); ++index) {
      result[index] = __half2float(raw[index]);
    }
    cudaFree(device_llrs);
    cudaFree(device_indices);
    cudaFree(device_coordinates);
    cudaFree(device_references);
    cudaFree(device_grid);
    cudaStreamDestroy(stream);
    return result;
  }

  void*                  library{};
  ocudu_dapp_module_v1   module{};
  ocudu_dapp_host_api_v1 host{};
  void*                  instance{};
  void*                  worker3{};
  void*                  worker9{};
  uint64_t               generation{1};
};

TEST_F(receiver_module_fixture, model_swap_is_generation_safe_bounded_and_rollback_capable)
{
  auto       first        = make_model(1.0F);
  auto       second       = make_model(0.95F);
  auto       third        = make_model(1.05F);
  const auto default_llrs = invoke_llrs(worker3);

  auto malformed = request(OCUDU_DAPP_MODEL_STAGE_V1, "bad", generation, &first);
  malformed.artifact.size -= 1;
  EXPECT_EQ(apply(malformed), OCUDU_DAPP_INVALID_INPUT_V1);
  EXPECT_EQ(generation, 1U);

  auto stage = request(OCUDU_DAPP_MODEL_STAGE_V1, "first", generation, &first);
  ASSERT_EQ(apply(stage), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(generation, 2U);
  for (uint32_t action : {OCUDU_DAPP_MODEL_VALIDATE_V1, OCUDU_DAPP_MODEL_WARM_V1, OCUDU_DAPP_MODEL_ACTIVATE_V1}) {
    auto operation = request(action, "first", generation);
    ASSERT_EQ(apply(operation), OCUDU_DAPP_OK_V1);
  }
  EXPECT_EQ(generation, 5U);
  const auto first_llrs = invoke_llrs(worker9);
  EXPECT_EQ(first_llrs, default_llrs);

  stage = request(OCUDU_DAPP_MODEL_STAGE_V1, "second", generation, &second);
  ASSERT_EQ(apply(stage), OCUDU_DAPP_OK_V1);
  for (uint32_t action : {OCUDU_DAPP_MODEL_VALIDATE_V1, OCUDU_DAPP_MODEL_WARM_V1, OCUDU_DAPP_MODEL_ACTIVATE_V1}) {
    auto operation = request(action, "second", generation);
    ASSERT_EQ(apply(operation), OCUDU_DAPP_OK_V1);
  }
  EXPECT_EQ(generation, 9U);
  const auto second_llrs = invoke_llrs(worker3);
  EXPECT_NE(second_llrs[2], first_llrs[2]);

  stage = request(OCUDU_DAPP_MODEL_STAGE_V1, "third", generation, &third);
  ASSERT_EQ(apply(stage), OCUDU_DAPP_OK_V1);
  auto validate = request(OCUDU_DAPP_MODEL_VALIDATE_V1, "third", generation);
  ASSERT_EQ(apply(validate), OCUDU_DAPP_OK_V1);
  auto warm = request(OCUDU_DAPP_MODEL_WARM_V1, "third", generation);
  EXPECT_EQ(apply(warm), OCUDU_DAPP_CONFLICT_V1);
  EXPECT_EQ(generation, 11U);

  auto rollback = request(OCUDU_DAPP_MODEL_ROLLBACK_V1, "first", generation);
  ASSERT_EQ(apply(rollback), OCUDU_DAPP_OK_V1);
  const auto rollback_llrs = invoke_llrs(worker9);
  EXPECT_EQ(rollback_llrs, first_llrs);
  auto retire = request(OCUDU_DAPP_MODEL_RETIRE_V1, "second", generation);
  ASSERT_EQ(apply(retire), OCUDU_DAPP_OK_V1);

  stage = request(OCUDU_DAPP_MODEL_STAGE_V1, "third", generation, &third);
  ASSERT_EQ(apply(stage), OCUDU_DAPP_OK_V1);
  for (uint32_t action : {OCUDU_DAPP_MODEL_VALIDATE_V1, OCUDU_DAPP_MODEL_WARM_V1, OCUDU_DAPP_MODEL_ACTIVATE_V1}) {
    auto operation = request(action, "third", generation);
    ASSERT_EQ(apply(operation), OCUDU_DAPP_OK_V1);
  }
  auto retire_active = request(OCUDU_DAPP_MODEL_RETIRE_V1, "third", generation);
  EXPECT_EQ(apply(retire_active), OCUDU_DAPP_CONFLICT_V1);
}

} // namespace
