// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::array<ocudu_dapp_modulation_v1, 4> certification_modulations{
    OCUDU_DAPP_MODULATION_QPSK_V1, OCUDU_DAPP_MODULATION_QAM16_V1, OCUDU_DAPP_MODULATION_QAM64_V1,
    OCUDU_DAPP_MODULATION_QAM256_V1};
constexpr std::array<uint32_t, 2> certification_dmrs_types{1, 2};

struct model_options {
  std::string artifact_path;
  std::string baseline_id;
  std::string candidate_id;
  std::string version;
  std::string shape_set{"boundaries"};
  uint64_t    generation{};

  bool enabled() const noexcept { return !artifact_path.empty(); }
};

[[noreturn]] void fail(const std::string& message)
{
  throw std::runtime_error(message);
}

void require(bool condition, const std::string& message)
{
  if (!condition) {
    fail(message);
  }
}

void require_cuda(cudaError_t status, const char* operation)
{
  if (status != cudaSuccess) {
    fail(std::string(operation) + ": " + cudaGetErrorString(status));
  }
}

model_options parse_model_options(int argc, char** argv)
{
  model_options result;
  for (int index = 3; index < argc; index += 2) {
    require(index + 1 < argc, "model option requires a value");
    const std::string option = argv[index];
    const std::string value  = argv[index + 1];
    if (option == "--model-artifact") {
      result.artifact_path = value;
    } else if (option == "--model-id") {
      result.baseline_id = value;
    } else if (option == "--candidate-model-id") {
      result.candidate_id = value;
    } else if (option == "--model-version") {
      result.version = value;
    } else if (option == "--model-generation") {
      errno                           = 0;
      char*                    end    = nullptr;
      const unsigned long long parsed = std::strtoull(value.c_str(), &end, 0);
      require(!value.empty() && value.front() != '-' && errno == 0 && end != value.c_str() && *end == '\0' &&
                  parsed != 0,
              "invalid model generation");
      result.generation = parsed;
    } else if (option == "--shape-set") {
      require(value == "boundaries" || value == "compact", "shape set must be boundaries or compact");
      result.shape_set = value;
    } else {
      fail("unknown option: " + option);
    }
  }
  const bool any = result.enabled() || !result.baseline_id.empty() || !result.candidate_id.empty() ||
                   !result.version.empty() || result.generation != 0;
  require(!any || (result.enabled() && !result.baseline_id.empty() && !result.candidate_id.empty() &&
                   result.baseline_id != result.candidate_id && !result.version.empty() && result.generation != 0),
          "model certification requires artifact, two distinct ids, version, and initial generation");
  return result;
}

std::vector<uint8_t> read_artifact(const std::string& path)
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  require(input.good(), "cannot open model artifact");
  const auto size = input.tellg();
  require(size > 0 && static_cast<uint64_t>(size) <= UINT32_MAX, "model artifact size is invalid");
  std::vector<uint8_t> result(static_cast<size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size));
  require(input.good(), "cannot read model artifact");
  return result;
}

ocudu_dapp_model_request_v1 model_request(uint32_t action, const std::string& id, const model_options& options,
                                          uint64_t generation, const std::vector<uint8_t>* artifact = nullptr)
{
  ocudu_dapp_model_request_v1 result{};
  result.struct_size         = sizeof(result);
  result.action              = action;
  result.model_id            = id.c_str();
  result.model_version       = options.version.c_str();
  result.expected_generation = generation;
  if (artifact != nullptr) {
    result.artifact = {artifact->data(), static_cast<uint32_t>(artifact->size()), 0};
  }
  return result;
}

uint64_t certify_model_lifecycle(const ocudu_dapp_module_v1& module, void* instance, const model_options& options)
{
  require(module.management != nullptr && (module.management->capability_bits & OCUDU_DAPP_MANAGEMENT_MODELS_V1) != 0 &&
              module.management->manage_model != nullptr,
          "module does not expose model management");
  const auto artifact   = read_artifact(options.artifact_path);
  uint64_t   generation = options.generation;
  const auto apply      = [&](uint32_t action, const std::string& id, const std::vector<uint8_t>* bytes = nullptr) {
    auto     request = model_request(action, id, options, generation, bytes);
    uint64_t next_generation{};
    require(module.management->manage_model(instance, &request, &next_generation) == OCUDU_DAPP_OK_V1,
                 "model lifecycle action failed");
    require(next_generation == generation + 1, "model generation did not advance exactly once");
    generation = next_generation;
  };
  const auto activate = [&](const std::string& id) {
    apply(OCUDU_DAPP_MODEL_STAGE_V1, id, &artifact);
    apply(OCUDU_DAPP_MODEL_VALIDATE_V1, id);
    apply(OCUDU_DAPP_MODEL_WARM_V1, id);
    apply(OCUDU_DAPP_MODEL_ACTIVATE_V1, id);
  };
  activate(options.baseline_id);
  activate(options.candidate_id);
  apply(OCUDU_DAPP_MODEL_ROLLBACK_V1, options.baseline_id);
  apply(OCUDU_DAPP_MODEL_RETIRE_V1, options.candidate_id);

  auto     retire_active = model_request(OCUDU_DAPP_MODEL_RETIRE_V1, options.baseline_id, options, generation);
  uint64_t unchanged{};
  require(module.management->manage_model(instance, &retire_active, &unchanged) == OCUDU_DAPP_CONFLICT_V1,
          "retiring the active model did not fail closed");
  require(unchanged == generation, "rejected model action changed the reported generation");
  return generation;
}

std::string json_string(const char* value)
{
  require(value != nullptr, "missing JSON string value");
  std::string result{"\""};
  for (const unsigned char character : std::string(value)) {
    if (character == '\\' || character == '"') {
      result.push_back('\\');
      result.push_back(static_cast<char>(character));
    } else if (character >= 0x20) {
      result.push_back(static_cast<char>(character));
    } else {
      fail("control character in module identity");
    }
  }
  result.push_back('"');
  return result;
}

uint32_t pack_cbf16(float real, float imaginary)
{
  const __nv_bfloat162 packed = __floats2bfloat162_rn(real, imaginary);
  uint32_t             result;
  static_assert(sizeof(result) == sizeof(packed));
  std::memcpy(&result, &packed, sizeof(result));
  return result;
}

float axis_level(uint32_t label, uint32_t bits)
{
  float amplitude = 1.0F;
  float step      = 2.0F;
  for (uint32_t bit = bits; bit > 1; --bit) {
    amplitude = step - (((label >> (bit - 1U)) & 1U) == 0 ? 1.0F : -1.0F) * amplitude;
    step *= 2.0F;
  }
  const float sign = (label & 1U) == 0 ? 1.0F : -1.0F;
  return sign * amplitude / std::sqrt(2.0F * static_cast<float>((1U << (2U * bits)) - 1U) / 3.0F);
}

template <typename T>
class device_buffer
{
public:
  device_buffer() = default;
  explicit device_buffer(size_t count_) : count(count_)
  {
    require_cuda(cudaMalloc(&pointer, count * sizeof(T)), "cudaMalloc");
  }
  ~device_buffer()
  {
    if (pointer != nullptr) {
      static_cast<void>(cudaFree(pointer));
    }
  }
  device_buffer(const device_buffer&)            = delete;
  device_buffer& operator=(const device_buffer&) = delete;

  T*     get() const noexcept { return pointer; }
  size_t size() const noexcept { return count; }

private:
  T*     pointer{};
  size_t count{};
};

template <typename T>
class guarded_device_buffer
{
  static constexpr size_t guard_bytes = 256;
  static constexpr int    guard_value = 0xa5;

public:
  explicit guarded_device_buffer(size_t capacity_) : capacity(capacity_)
  {
    require(capacity != 0, "guarded output capacity is zero");
    require_cuda(cudaMalloc(&allocation, 2U * guard_bytes + capacity * sizeof(T)), "cudaMalloc guarded output");
  }
  ~guarded_device_buffer()
  {
    if (allocation != nullptr) {
      static_cast<void>(cudaFree(allocation));
    }
  }
  guarded_device_buffer(const guarded_device_buffer&)            = delete;
  guarded_device_buffer& operator=(const guarded_device_buffer&) = delete;

  void prepare(size_t count, cudaStream_t stream)
  {
    require(count != 0 && count <= capacity, "guarded output size is invalid");
    active = count;
    require_cuda(cudaMemsetAsync(allocation, guard_value, allocation_bytes(), stream), "poison guarded output");
  }

  T*     data() const noexcept { return reinterpret_cast<T*>(static_cast<uint8_t*>(allocation) + guard_bytes); }
  void*  base() const noexcept { return allocation; }
  size_t size() const noexcept { return capacity; }
  size_t active_size() const noexcept { return active; }
  static constexpr size_t offset() noexcept { return guard_bytes; }

  void verify_bounds(const char* name) const
  {
    require(active != 0, std::string(name) + " was not prepared");
    const size_t         suffix_bytes = guard_bytes + (capacity - active) * sizeof(T);
    std::vector<uint8_t> prefix(guard_bytes);
    std::vector<uint8_t> suffix(suffix_bytes);
    require_cuda(cudaMemcpy(prefix.data(), allocation, prefix.size(), cudaMemcpyDeviceToHost), "copy output prefix");
    require_cuda(cudaMemcpy(suffix.data(), data() + active, suffix.size(), cudaMemcpyDeviceToHost),
                 "copy output suffix");
    const auto intact = [](const std::vector<uint8_t>& bytes) {
      return std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == guard_value; });
    };
    require(intact(prefix) && intact(suffix), std::string(name) + " wrote outside its advertised byte range");
  }

private:
  size_t allocation_bytes() const noexcept { return 2U * guard_bytes + capacity * sizeof(T); }

  void*  allocation{};
  size_t capacity{};
  size_t active{};
};

class cuda_stream
{
public:
  cuda_stream() { require_cuda(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking), "cudaStreamCreateWithFlags"); }
  ~cuda_stream()
  {
    if (value != nullptr) {
      static_cast<void>(cudaStreamSynchronize(value));
      static_cast<void>(cudaStreamDestroy(value));
    }
  }
  cuda_stream(const cuda_stream&)            = delete;
  cuda_stream& operator=(const cuda_stream&) = delete;

  operator cudaStream_t() const noexcept { return value; }
  cudaStream_t get() const noexcept { return value; }

private:
  cudaStream_t value{};
};

template <typename T>
ocudu_dapp_tensor_view_v1 device_tensor(T* pointer, uint32_t element_type, uint32_t access,
                                        std::initializer_list<uint64_t> dimensions, uint32_t layout, uint64_t identity)
{
  ocudu_dapp_tensor_view_v1 result{};
  result.struct_size           = sizeof(result);
  result.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  result.domain                = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  result.backend_id            = OCUDU_DAPP_BACKEND_CUDA_V1;
  result.element_type          = element_type;
  result.rank                  = dimensions.size();
  result.access                = access;
  result.allocation_id         = identity;
  result.allocation_generation = 1;
  result.native_handle         = reinterpret_cast<uintptr_t>(pointer);
  result.layout_id             = layout;
  result.layout_version        = 1;
  result.lease_token           = identity;
  uint64_t elements            = 1;
  uint32_t index               = 0;
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
ocudu_dapp_tensor_view_v1 guarded_output_tensor(const guarded_device_buffer<T>& buffer, uint32_t element_type,
                                                std::initializer_list<uint64_t> dimensions, uint32_t layout,
                                                uint64_t identity)
{
  auto result = device_tensor(buffer.data(), element_type, OCUDU_DAPP_ACCESS_WRITE_V1, dimensions, layout, identity);
  require(result.byte_length == buffer.active_size() * sizeof(T), "guarded tensor dimensions do not match output");
  result.native_handle = reinterpret_cast<uintptr_t>(buffer.base());
  result.byte_offset   = buffer.offset();
  return result;
}

bool same_tensor_storage(const ocudu_dapp_tensor_view_v1& expected, const ocudu_dapp_tensor_view_v1& actual) noexcept
{
  if (expected.struct_size != actual.struct_size || expected.abi_major != actual.abi_major ||
      expected.abi_minor != actual.abi_minor || expected.domain != actual.domain ||
      expected.backend_id != actual.backend_id || expected.element_type != actual.element_type ||
      expected.rank != actual.rank || expected.access != actual.access ||
      expected.device_ordinal != actual.device_ordinal || expected.allocation_id != actual.allocation_id ||
      expected.allocation_generation != actual.allocation_generation ||
      expected.native_handle != actual.native_handle || expected.byte_offset != actual.byte_offset ||
      expected.byte_length != actual.byte_length || expected.layout_id != actual.layout_id ||
      expected.layout_version != actual.layout_version || expected.ready_token != actual.ready_token ||
      expected.lease_token != actual.lease_token) {
    return false;
  }
  for (uint32_t dimension = 0; dimension != OCUDU_DAPP_MAX_TENSOR_RANK_V1; ++dimension) {
    if (expected.dimensions[dimension] != actual.dimensions[dimension] ||
        expected.byte_strides[dimension] != actual.byte_strides[dimension]) {
      return false;
    }
  }
  return true;
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

bool empty_metadata(const ocudu_dapp_typed_metadata_v1& metadata) noexcept
{
  return metadata.struct_size == 0 && metadata.abi_major == 0 && metadata.abi_minor == 0 &&
         metadata.metadata_schema_id == 0 && metadata.metadata_schema_version == 0 &&
         metadata.metadata.data == nullptr && metadata.metadata.size == 0 && metadata.metadata.reserved == 0;
}

uint64_t monotonic_ns(void*) noexcept
{
  return 1;
}
void                 metric_add(void*, uint32_t, uint64_t) noexcept {}
void                 incident(void*, uint32_t, uint64_t) noexcept {}
ocudu_dapp_status_v1 publish(void*, uint32_t, uint32_t, const void*, uint32_t) noexcept
{
  return OCUDU_DAPP_INVALID_OUTPUT_V1;
}
ocudu_dapp_status_v1 extension(void*, uint32_t, uint16_t, const void** output) noexcept
{
  if (output != nullptr) {
    *output = nullptr;
  }
  return OCUDU_DAPP_UNSUPPORTED_V1;
}

struct library_guard {
  void* handle{};
  ~library_guard()
  {
    if (handle != nullptr) {
      ::dlclose(handle);
    }
  }
};

class module_session
{
public:
  module_session(const ocudu_dapp_module_v1& module_, const ocudu_dapp_host_api_v1& host) : module(module_)
  {
    try {
      require(module.create(&host, nullptr, &instance) == OCUDU_DAPP_OK_V1 && instance != nullptr, "create failed");
      require(module.configure(instance, nullptr) == OCUDU_DAPP_OK_V1, "configure failed");
      require(module.prepare(instance, nullptr) == OCUDU_DAPP_OK_V1, "prepare failed");
      if (module.warmup != nullptr) {
        require(module.warmup(instance, nullptr) == OCUDU_DAPP_OK_V1, "warmup failed");
      }
      require(module.start(instance) == OCUDU_DAPP_OK_V1, "start failed");
      started = true;
    } catch (...) {
      retire();
      throw;
    }
  }

  ~module_session() { retire(); }

  void create_workers(const ocudu_dapp_worker_api_v1& api)
  {
    worker_api = &api;
    for (uint32_t id = 0; id != workers.size(); ++id) {
      require(api.create_worker_context(instance, id, &workers[id]) == OCUDU_DAPP_OK_V1 && workers[id] != nullptr,
              "worker creation failed");
    }
  }

  void* context(uint32_t id) const noexcept { return workers[id]; }
  void* module_instance() const noexcept { return instance; }

  void finish()
  {
    for (void*& worker : workers) {
      if (worker != nullptr) {
        void* current = worker;
        worker        = nullptr;
        worker_api->destroy_worker_context(instance, current);
      }
    }
    if (started) {
      require(module.stop(instance) == OCUDU_DAPP_OK_V1, "stop failed");
      started = false;
    }
    require(module.quiesce(instance, std::numeric_limits<uint64_t>::max()) == OCUDU_DAPP_OK_V1, "quiesce failed");
    void* current = instance;
    instance      = nullptr;
    module.destroy(current);
  }

private:
  void retire() noexcept
  {
    for (void*& worker : workers) {
      if (worker != nullptr) {
        void* current = worker;
        worker        = nullptr;
        try {
          worker_api->destroy_worker_context(instance, current);
        } catch (...) {
        }
      }
    }
    if (started) {
      try {
        static_cast<void>(module.stop(instance));
      } catch (...) {
      }
    }
    if (instance != nullptr) {
      try {
        static_cast<void>(module.quiesce(instance, std::numeric_limits<uint64_t>::max()));
      } catch (...) {
      }
      try {
        module.destroy(instance);
      } catch (...) {
      }
      instance = nullptr;
    }
  }
  const ocudu_dapp_module_v1&     module;
  const ocudu_dapp_worker_api_v1* worker_api{};
  void*                           instance{};
  std::array<void*, 2>            workers{};
  bool                            started{};
};

struct certification_shape {
  uint32_t nof_prb;
  uint32_t nof_ports;
  uint32_t nof_layers;
  uint32_t start_symbol{};
  uint32_t start_prb{};
  uint32_t grid_nof_symbols{1};
  uint32_t grid_nof_prb{};
  uint32_t nof_symbols{1};
  uint32_t nof_dmrs_symbols{1};

  uint32_t allocation_subcarriers() const noexcept { return nof_prb * 12U; }
  uint32_t grid_subcarriers() const noexcept { return (grid_nof_prb == 0 ? start_prb + nof_prb : grid_nof_prb) * 12U; }
  uint32_t nof_pilots() const noexcept { return 2U * nof_dmrs_symbols; }
  uint32_t nof_data_re() const noexcept { return nof_symbols * allocation_subcarriers() - 3U * nof_dmrs_symbols; }
  uint32_t dmrs_symbol_offset(uint32_t index) const noexcept
  {
    return nof_dmrs_symbols == 1 ? 0U : index * (nof_symbols - 1U) / (nof_dmrs_symbols - 1U);
  }
};

class certification_lane
{
  static constexpr uint32_t pilots_per_dmrs_symbol = 2;

public:
  certification_lane(uint32_t id_, certification_shape shape_) :
    id(id_), shape(shape_), grid_symbols(shape.grid_nof_symbols), grid_subcarriers(shape.grid_subcarriers()),
    plane(grid_symbols * grid_subcarriers), grid(shape.nof_ports * plane),
    references(shape.nof_layers * shape.nof_pilots()), coordinates(2U * shape.nof_pilots()),
    indices(shape.nof_data_re()), labels(indices.size() * shape.nof_layers), expected(labels.size()),
    d_grid(grid.size()), d_references(references.size()), d_coordinates(coordinates.size()), d_indices(indices.size()),
    d_channel(shape.nof_ports * shape.nof_layers * plane), d_ce_noise(shape.nof_ports * shape.nof_layers),
    d_equalized(labels.size()), d_eq_noise(labels.size()), d_llrs(labels.size() * 8U)
  {
    require(shape.nof_prb != 0 && shape.nof_ports != 0 && shape.nof_layers != 0 && shape.nof_layers <= 2 &&
                shape.nof_symbols != 0 && shape.nof_dmrs_symbols != 0 && shape.nof_dmrs_symbols <= 2 &&
                shape.nof_dmrs_symbols <= shape.nof_symbols && shape.grid_nof_symbols != 0 &&
                shape.start_symbol + shape.nof_symbols <= shape.grid_nof_symbols &&
                shape.start_prb + shape.nof_prb <= grid_subcarriers / 12U,
            "invalid certification shape");
    size_t data_index = 0;
    for (uint32_t symbol_offset = 0; symbol_offset != shape.nof_symbols; ++symbol_offset) {
      bool is_dmrs_symbol = false;
      for (uint32_t dmrs_symbol = 0; dmrs_symbol != shape.nof_dmrs_symbols; ++dmrs_symbol) {
        is_dmrs_symbol |= symbol_offset == shape.dmrs_symbol_offset(dmrs_symbol);
      }
      for (uint32_t subcarrier_offset = 0; subcarrier_offset != shape.allocation_subcarriers(); ++subcarrier_offset) {
        if (is_dmrs_symbol && subcarrier_offset < 3U) {
          continue;
        }
        indices[data_index++] =
            (shape.start_symbol + symbol_offset) * grid_subcarriers + shape.start_prb * 12U + subcarrier_offset;
      }
    }
    require(data_index == indices.size(), "certification data-RE construction failed");
    for (uint32_t layer = 0; layer != shape.nof_layers; ++layer) {
      for (uint32_t dmrs_symbol = 0; dmrs_symbol != shape.nof_dmrs_symbols; ++dmrs_symbol) {
        for (uint32_t pilot_offset = 0; pilot_offset != pilots_per_dmrs_symbol; ++pilot_offset) {
          const uint32_t pilot                           = dmrs_symbol * pilots_per_dmrs_symbol + pilot_offset;
          const float    layer_sign                      = layer == 0 ? 1.0F
                                                                      : (shape.nof_dmrs_symbols == 1 ? (pilot_offset == 0 ? 1.0F : -1.0F)
                                                                                                     : (dmrs_symbol == 0 ? 1.0F : -1.0F));
          references[layer * shape.nof_pilots() + pilot] = {layer_sign, 0.0F};
        }
      }
    }
    write_dmrs_vector(1);
    require_cuda(
        cudaMemcpyAsync(d_grid.get(), grid.data(), grid.size() * sizeof(grid[0]), cudaMemcpyHostToDevice, stream),
        "copy grid");
    require_cuda(cudaMemcpyAsync(d_references.get(), references.data(), references.size() * sizeof(references[0]),
                                 cudaMemcpyHostToDevice, stream),
                 "copy references");
    require_cuda(cudaMemcpyAsync(d_coordinates.get(), coordinates.data(), coordinates.size() * sizeof(coordinates[0]),
                                 cudaMemcpyHostToDevice, stream),
                 "copy coordinates");
    require_cuda(cudaMemcpyAsync(d_indices.get(), indices.data(), indices.size() * sizeof(indices[0]),
                                 cudaMemcpyHostToDevice, stream),
                 "copy indices");

    const uint64_t identity                                            = 100 + id * 16;
    pusch.struct_size                                                  = sizeof(pusch);
    pusch.abi_major                                                    = OCUDU_DAPP_ABI_MAJOR_V1;
    pusch.abi_minor                                                    = OCUDU_DAPP_ABI_MINOR_V1;
    pusch.nof_rx_ports                                                 = shape.nof_ports;
    pusch.nof_layers                                                   = shape.nof_layers;
    pusch.modulation                                                   = OCUDU_DAPP_MODULATION_QPSK_V1;
    pusch.start_symbol                                                 = shape.start_symbol;
    pusch.nof_symbols                                                  = shape.nof_symbols;
    pusch.start_subcarrier                                             = shape.start_prb * 12U;
    pusch.nof_subcarriers                                              = shape.allocation_subcarriers();
    pusch.nof_data_re                                                  = indices.size();
    pusch.noise_variance_floor                                         = 1.0e-6F;
    pusch.flags                                                        = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
    pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1] = 30'000U;
    pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1]  = grid_subcarriers;
    pusch.data_re_indices =
        device_tensor(d_indices.get(), OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1, {indices.size()},
                      OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1, identity + 1);
    dmrs.struct_size = sizeof(dmrs);
    dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    dmrs.nof_pilots  = shape.nof_pilots();
    dmrs.dmrs_type   = 1;
    dmrs.flags =
        OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
    dmrs.reference_symbols = device_tensor(d_references.get(), OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                           {shape.nof_layers, shape.nof_pilots()}, 0, identity + 2);
    dmrs.coordinates       = device_tensor(d_coordinates.get(), OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                           {shape.nof_pilots(), 2}, 0, identity + 3);
    invocation.struct_size = sizeof(invocation);
    invocation.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    invocation.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    invocation.worker_id   = id;
    invocation.execution_stream = reinterpret_cast<uintptr_t>(stream.get());
    grid_view                   = device_tensor(d_grid.get(), OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1,
                                                {shape.nof_ports, grid_symbols, grid_subcarriers},
                                                OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1, identity + 4);
    pusch_view                  = typed(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
    dmrs_view                   = typed(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);
  }

  void select_modulation(ocudu_dapp_modulation_v1 modulation)
  {
    const uint32_t modulation_order = static_cast<uint32_t>(modulation);
    const uint32_t axis_bits        = modulation_order / 2U;
    require(axis_bits >= 1 && axis_bits <= 4, "unsupported certification modulation");
    const uint32_t mask = (1U << axis_bits) - 1U;
    for (size_t data = 0; data != indices.size(); ++data) {
      const uint32_t data_label = static_cast<uint32_t>(data);
      for (uint32_t layer = 0; layer != shape.nof_layers; ++layer) {
        const size_t index = data * shape.nof_layers + layer;
        labels[index]      = {(data_label + 3U * layer) & mask, (2U * data_label + 1U + 5U * layer) & mask};
        expected[index]    = {axis_level(labels[index][0], axis_bits), axis_level(labels[index][1], axis_bits)};
      }
    }
    for (uint32_t port = 0; port != shape.nof_ports; ++port) {
      for (size_t data = 0; data != indices.size(); ++data) {
        float2 received{};
        for (uint32_t layer = 0; layer != shape.nof_layers; ++layer) {
          const float2 value = expected[data * shape.nof_layers + layer];
          const float  gain  = channel_gain(port, layer);
          received.x += gain * value.x;
          received.y += gain * value.y;
        }
        grid[port * plane + indices[data]] = pack_cbf16(received.x, received.y);
      }
    }
    pusch.modulation = modulation;
    require_cuda(
        cudaMemcpyAsync(d_grid.get(), grid.data(), grid.size() * sizeof(grid[0]), cudaMemcpyHostToDevice, stream),
        "copy modulation vector");
  }

  void select_dmrs_type(uint32_t type)
  {
    require(type == 1 || type == 2, "unsupported certification DM-RS type");
    write_dmrs_vector(type);
    require_cuda(cudaMemcpyAsync(d_coordinates.get(), coordinates.data(), coordinates.size() * sizeof(coordinates[0]),
                                 cudaMemcpyHostToDevice, stream),
                 "copy DM-RS coordinates");
    require_cuda(
        cudaMemcpyAsync(d_grid.get(), grid.data(), grid.size() * sizeof(grid[0]), cudaMemcpyHostToDevice, stream),
        "copy DM-RS vector");
    dmrs.dmrs_type = type;
    pusch.flags    = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1 | (type == 2 ? OCUDU_DAPP_PUSCH_DC_OVERLAP_V1 : 0U);
  }

  ocudu_dapp_status_v1 invoke(const ocudu_dapp_interface_descriptor_v1& descriptor, void* worker)
  {
    switch (descriptor.interface_id) {
      case OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1: {
        d_channel.prepare(d_channel.size(), stream);
        d_ce_noise.prepare(d_ce_noise.size(), stream);
        ocudu_dapp_channel_estimator_input_v1  input{invocation, grid_view, pusch_view, dmrs_view};
        ocudu_dapp_channel_estimator_output_v1 output{};
        output.channel_estimates =
            guarded_output_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1,
                                  {shape.nof_ports, shape.nof_layers, grid_symbols, grid_subcarriers}, 0, 200 + id);
        output.noise_variances      = guarded_output_tensor(d_ce_noise, OCUDU_DAPP_ELEMENT_F32_V1,
                                                            {shape.nof_ports, shape.nof_layers}, 0, 210 + id);
        const auto expected_channel = output.channel_estimates;
        const auto expected_noise   = output.noise_variances;
        const auto status = static_cast<const ocudu_dapp_channel_estimator_interface_v1*>(descriptor.interface_vtable)
                                ->invoke(worker, &input, &output);
        require(same_tensor_storage(expected_channel, output.channel_estimates) &&
                    same_tensor_storage(expected_noise, output.noise_variances),
                "channel estimator substituted caller-owned output storage");
        return status;
      }
      case OCUDU_DAPP_INTERFACE_EQUALIZER_V1: {
        d_channel.prepare(d_channel.size(), stream);
        d_equalized.prepare(d_equalized.size(), stream);
        d_eq_noise.prepare(d_eq_noise.size(), stream);
        ocudu_dapp_equalizer_input_v1  input{invocation, grid_view, pusch_view, dmrs_view};
        ocudu_dapp_equalizer_output_v1 output{};
        output.channel_estimates =
            guarded_output_tensor(d_channel, OCUDU_DAPP_ELEMENT_CF32_V1,
                                  {shape.nof_ports, shape.nof_layers, grid_symbols, grid_subcarriers}, 0, 200 + id);
        output.equalized_symbols =
            guarded_output_tensor(d_equalized, OCUDU_DAPP_ELEMENT_CF32_V1, {indices.size(), shape.nof_layers},
                                  OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1, 220 + id);
        output.post_equalization_noise =
            guarded_output_tensor(d_eq_noise, OCUDU_DAPP_ELEMENT_F32_V1, {indices.size(), shape.nof_layers},
                                  OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1, 230 + id);
        const auto expected_channel = output.channel_estimates;
        const auto expected_symbols = output.equalized_symbols;
        const auto expected_noise   = output.post_equalization_noise;
        const auto status           = static_cast<const ocudu_dapp_equalizer_interface_v1*>(descriptor.interface_vtable)
                                ->invoke(worker, &input, &output);
        require(same_tensor_storage(expected_channel, output.channel_estimates) &&
                    same_tensor_storage(expected_symbols, output.equalized_symbols) &&
                    same_tensor_storage(expected_noise, output.post_equalization_noise),
                "equalizer substituted caller-owned output storage");
        return status;
      }
      case OCUDU_DAPP_INTERFACE_RECEIVER_V1: {
        const size_t llr_count = labels.size() * static_cast<uint32_t>(pusch.modulation);
        d_llrs.prepare(llr_count, stream);
        ocudu_dapp_receiver_input_v1  input{invocation, grid_view, pusch_view, dmrs_view};
        ocudu_dapp_receiver_output_v1 output{};
        output.llrs              = guarded_output_tensor(d_llrs, OCUDU_DAPP_ELEMENT_F16_V1,
                                                         {indices.size(), shape.nof_layers, static_cast<uint32_t>(pusch.modulation)},
                                                         OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1, 240 + id);
        const auto expected_llrs = output.llrs;
        const auto status        = static_cast<const ocudu_dapp_receiver_interface_v1*>(descriptor.interface_vtable)
                                ->invoke(worker, &input, &output);
        require(same_tensor_storage(expected_llrs, output.llrs), "receiver substituted caller-owned output storage");
        require(empty_metadata(output.uci) && empty_metadata(output.metrics) && output.confidence_q16 <= (1U << 16U) &&
                    output.reserved0 == 0,
                "receiver returned unsupported v1 side output");
        return status;
      }
      default:
        return OCUDU_DAPP_UNSUPPORTED_V1;
    }
  }

  void validate(uint32_t interface_id)
  {
    require_cuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize");
    if (interface_id == OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1) {
      d_channel.verify_bounds("channel estimates");
      d_ce_noise.verify_bounds("channel-estimator noise");
      std::vector<float2> channel(d_channel.size());
      require_cuda(
          cudaMemcpy(channel.data(), d_channel.data(), channel.size() * sizeof(channel[0]), cudaMemcpyDeviceToHost),
          "copy channel output");
      for (uint32_t port = 0; port != shape.nof_ports; ++port) {
        for (uint32_t layer = 0; layer != shape.nof_layers; ++layer) {
          const float  expected_gain = channel_gain(port, layer);
          const size_t first         = (port * shape.nof_layers + layer) * plane;
          for (size_t index = first; index != first + plane; ++index) {
            require(std::abs(channel[index].x - expected_gain) < 1.0e-5F && std::abs(channel[index].y) < 1.0e-5F,
                    "channel output failed declared-shape vector");
          }
        }
      }
      std::vector<float> noise(d_ce_noise.size());
      require_cuda(cudaMemcpy(noise.data(), d_ce_noise.data(), noise.size() * sizeof(noise[0]), cudaMemcpyDeviceToHost),
                   "copy channel-estimator noise");
      require(
          std::all_of(noise.begin(), noise.end(), [](float value) { return std::isfinite(value) && value >= 0.0F; }),
          "channel-estimator noise was not completely written");
    } else if (interface_id == OCUDU_DAPP_INTERFACE_EQUALIZER_V1) {
      d_channel.verify_bounds("equalizer channel estimates");
      d_equalized.verify_bounds("equalized symbols");
      d_eq_noise.verify_bounds("post-equalization noise");
      std::vector<float2> channel(d_channel.size());
      std::vector<float2> equalized(expected.size());
      std::vector<float>  noise(expected.size());
      require_cuda(
          cudaMemcpy(channel.data(), d_channel.data(), channel.size() * sizeof(channel[0]), cudaMemcpyDeviceToHost),
          "copy equalizer channel output");
      require_cuda(cudaMemcpy(equalized.data(), d_equalized.data(), equalized.size() * sizeof(equalized[0]),
                              cudaMemcpyDeviceToHost),
                   "copy equalized output");
      require_cuda(cudaMemcpy(noise.data(), d_eq_noise.data(), noise.size() * sizeof(noise[0]), cudaMemcpyDeviceToHost),
                   "copy equalizer noise");
      for (uint32_t port = 0; port != shape.nof_ports; ++port) {
        for (uint32_t layer = 0; layer != shape.nof_layers; ++layer) {
          const float  expected_gain = channel_gain(port, layer);
          const size_t first         = (port * shape.nof_layers + layer) * plane;
          for (size_t index = first; index != first + plane; ++index) {
            require(std::abs(channel[index].x - expected_gain) < 1.0e-5F && std::abs(channel[index].y) < 1.0e-5F,
                    "equalizer channel output was not completely written");
          }
        }
      }
      for (size_t index = 0; index != expected.size(); ++index) {
        require(std::abs(equalized[index].x - expected[index].x) < 5.0e-3F &&
                    std::abs(equalized[index].y - expected[index].y) < 5.0e-3F && std::isfinite(noise[index]) &&
                    noise[index] >= 0.0F,
                "equalizer output failed dense data-RE vector");
      }
    } else {
      d_llrs.verify_bounds("receiver LLRs");
      std::vector<__half> llrs(labels.size() * 8U);
      const uint32_t      modulation = static_cast<uint32_t>(pusch.modulation);
      const size_t        count      = labels.size() * modulation;
      require_cuda(cudaMemcpy(llrs.data(), d_llrs.data(), count * sizeof(__half), cudaMemcpyDeviceToHost),
                   "copy LLR output");
      const uint32_t axis_bits = modulation / 2U;
      for (size_t data_layer = 0; data_layer != labels.size(); ++data_layer) {
        for (uint32_t bit = 0; bit != axis_bits; ++bit) {
          const size_t base     = data_layer * modulation + 2U * bit;
          const float  real_llr = __half2float(llrs[base]);
          const float  imag_llr = __half2float(llrs[base + 1]);
          require(std::isfinite(real_llr) && std::isfinite(imag_llr) && real_llr != 0.0F && imag_llr != 0.0F &&
                      (real_llr > 0.0F) == (((labels[data_layer][0] >> bit) & 1U) == 0) &&
                      (imag_llr > 0.0F) == (((labels[data_layer][1] >> bit) & 1U) == 0),
                  "receiver output failed dense QAM hard-decision vector");
        }
      }
    }
  }

private:
  void write_dmrs_vector(uint32_t type)
  {
    const uint16_t start_subcarrier = static_cast<uint16_t>(shape.start_prb * 12U);
    for (uint32_t port = 0; port != shape.nof_ports; ++port) {
      for (uint32_t dmrs_symbol = 0; dmrs_symbol != shape.nof_dmrs_symbols; ++dmrs_symbol) {
        const uint16_t symbol       = static_cast<uint16_t>(shape.start_symbol + shape.dmrs_symbol_offset(dmrs_symbol));
        const size_t   pilot_origin = static_cast<size_t>(symbol) * grid_subcarriers + start_subcarrier;
        std::fill_n(grid.begin() + port * plane + pilot_origin, 3, uint32_t{});
        for (uint32_t pilot_offset = 0; pilot_offset != pilots_per_dmrs_symbol; ++pilot_offset) {
          const uint32_t pilot = dmrs_symbol * pilots_per_dmrs_symbol + pilot_offset;
          const uint16_t subcarrier =
              static_cast<uint16_t>(start_subcarrier + (pilot_offset == 0 ? 0U : (type == 1 ? 2U : 1U)));
          coordinates[2U * pilot]     = symbol;
          coordinates[2U * pilot + 1] = subcarrier;
          float2 received{};
          for (uint32_t layer = 0; layer != shape.nof_layers; ++layer) {
            const float  gain      = channel_gain(port, layer);
            const float2 reference = references[layer * shape.nof_pilots() + pilot];
            received.x += gain * reference.x;
            received.y += gain * reference.y;
          }
          grid[port * plane + static_cast<size_t>(symbol) * grid_subcarriers + subcarrier] =
              pack_cbf16(received.x, received.y);
        }
      }
    }
  }

  float channel_gain(uint32_t port, uint32_t layer) const noexcept
  {
    return shape.nof_layers == 1 || port % shape.nof_layers == layer ? 1.0F : 0.0F;
  }

  uint32_t                             id;
  certification_shape                  shape;
  uint32_t                             grid_symbols;
  uint32_t                             grid_subcarriers;
  uint32_t                             plane;
  std::vector<uint32_t>                grid;
  std::vector<float2>                  references;
  std::vector<uint16_t>                coordinates;
  std::vector<uint32_t>                indices;
  std::vector<std::array<uint32_t, 2>> labels;
  std::vector<float2>                  expected;
  device_buffer<uint32_t>              d_grid;
  device_buffer<float2>                d_references;
  device_buffer<uint16_t>              d_coordinates;
  device_buffer<uint32_t>              d_indices;
  guarded_device_buffer<float2>        d_channel;
  guarded_device_buffer<float>         d_ce_noise;
  guarded_device_buffer<float2>        d_equalized;
  guarded_device_buffer<float>         d_eq_noise;
  guarded_device_buffer<__half>        d_llrs;
  cuda_stream                          stream;
  ocudu_dapp_pusch_metadata_v1         pusch{};
  ocudu_dapp_dmrs_metadata_v1          dmrs{};
  ocudu_dapp_invocation_v1             invocation{};
  ocudu_dapp_tensor_view_v1            grid_view{};
  ocudu_dapp_typed_metadata_v1         pusch_view{};
  ocudu_dapp_typed_metadata_v1         dmrs_view{};
};

uint64_t certify_shape(const ocudu_dapp_interface_descriptor_v1& descriptor, module_session& session,
                       certification_shape shape)
{
  certification_lane lane0(0, shape);
  certification_lane lane1(1, shape);
  uint64_t           invocations = 0;
  for (uint32_t dmrs_type : certification_dmrs_types) {
    lane0.select_dmrs_type(dmrs_type);
    lane1.select_dmrs_type(dmrs_type);
    for (ocudu_dapp_modulation_v1 modulation : certification_modulations) {
      lane0.select_modulation(modulation);
      lane1.select_modulation(modulation);
      require(lane0.invoke(descriptor, session.context(0)) == OCUDU_DAPP_OK_V1, "lane 0 invocation failed");
      require(lane1.invoke(descriptor, session.context(1)) == OCUDU_DAPP_OK_V1, "lane 1 invocation failed");
      lane0.validate(descriptor.interface_id);
      lane1.validate(descriptor.interface_id);
      invocations += 2;
    }
  }
  return invocations;
}

uint64_t metric_value(const ocudu_dapp_module_v1& module, void* instance, const char* name)
{
  require(module.management != nullptr && module.management->read_metrics != nullptr,
          "management metrics are required");
  uint32_t count = 0;
  require(module.management->read_metrics(instance, nullptr, 0, &count) == OCUDU_DAPP_OK_V1 && count <= 64,
          "metric count failed");
  std::array<ocudu_dapp_metric_sample_v1, 64> samples{};
  require(module.management->read_metrics(instance, samples.data(), samples.size(), &count) == OCUDU_DAPP_OK_V1,
          "metric read failed");
  for (uint32_t index = 0; index != count; ++index) {
    if (samples[index].name != nullptr && std::strcmp(samples[index].name, name) == 0) {
      return samples[index].value.value.uint64_value;
    }
  }
  fail(std::string("missing metric: ") + name);
}

} // namespace

int main(int argc, char** argv)
{
  try {
    require(argc >= 3 && (argc - 3) % 2 == 0,
            "usage: certify_class_a_cuda MODULE INTERFACE_ID [certification options]");
    const model_options models       = parse_model_options(argc, argv);
    char*               end          = nullptr;
    const uint64_t      interface_id = std::strtoull(argv[2], &end, 0);
    require(end != argv[2] && *end == '\0' && interface_id <= UINT32_MAX, "invalid interface id");

    library_guard library{::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)};
    const char*   load_error = ::dlerror();
    require(library.handle != nullptr, load_error != nullptr ? load_error : "dlopen failed");
    ::dlerror();
    void* symbol = ::dlsym(library.handle, "ocudu_dapp_query_v1");
    require(::dlerror() == nullptr && symbol != nullptr, "query symbol is missing");
    ocudu_dapp_query_v1_fn query = nullptr;
    static_assert(sizeof(query) == sizeof(symbol));
    std::memcpy(&query, &symbol, sizeof(query));

    ocudu_dapp_host_info_v1 host_info{};
    host_info.struct_size  = sizeof(host_info);
    host_info.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
    host_info.abi_minor    = OCUDU_DAPP_ABI_MINOR_V1;
    host_info.backend_mask = uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;
    ocudu_dapp_module_v1 module{};
    module.struct_size = sizeof(module);
    module.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    module.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    require(query(&host_info, &module) == OCUDU_DAPP_OK_V1, "module query failed");
    require(module.struct_size >= offsetof(ocudu_dapp_module_v1, reserved) &&
                module.abi_major == OCUDU_DAPP_ABI_MAJOR_V1 && module.global_name != nullptr &&
                module.version != nullptr && module.create != nullptr && module.configure != nullptr &&
                module.prepare != nullptr && module.start != nullptr && module.stop != nullptr &&
                module.quiesce != nullptr && module.destroy != nullptr,
            "module descriptor is incomplete");
    require(module.nof_interfaces == 1 && module.interfaces != nullptr, "one interface is required");
    const auto& descriptor = module.interfaces[0];
    require(descriptor.struct_size >= offsetof(ocudu_dapp_interface_descriptor_v1, reserved) &&
                descriptor.abi_major == OCUDU_DAPP_ABI_MAJOR_V1 && descriptor.interface_major == 1 &&
                descriptor.interface_vtable != nullptr,
            "interface descriptor is incomplete");
    require(descriptor.interface_id == interface_id, "interface id does not match request");
    require(interface_id == OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1 ||
                interface_id == OCUDU_DAPP_INTERFACE_EQUALIZER_V1 || interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1,
            "artifact is not a supported Class-A PUSCH depth");
    require(descriptor.dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1 &&
                (descriptor.capability_bits & OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1) != 0,
            "artifact is not resident Class A");
    const auto*        profile         = static_cast<const ocudu_dapp_pusch_profile_v1*>(descriptor.use_case_profile);
    constexpr uint32_t all_modulations = OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM16_V1 |
                                         OCUDU_DAPP_PUSCH_MODULATION_QAM64_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM256_V1;
    require(profile != nullptr && profile->struct_size >= offsetof(ocudu_dapp_pusch_profile_v1, reserved) &&
                profile->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 && profile->profile_id == OCUDU_DAPP_PUSCH_PROFILE_V1 &&
                profile->max_nof_prb != 0 && profile->max_nof_prb <= 275 && profile->max_nof_rx_ports != 0 &&
                profile->max_nof_rx_ports <= 64 && profile->max_nof_layers != 0 && profile->max_nof_layers <= 2 &&
                profile->max_nof_rx_ports >= profile->max_nof_layers &&
                (profile->modulation_mask & all_modulations) == all_modulations &&
                (profile->dmrs_type_mask & (OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1 | OCUDU_DAPP_PUSCH_DMRS_TYPE2_V1)) ==
                    (OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1 | OCUDU_DAPP_PUSCH_DMRS_TYPE2_V1) &&
                profile->flags == OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1,
            "declared profile is outside the reference v1 envelope");

    ocudu_dapp_host_api_v1 host{};
    host.struct_size      = sizeof(host);
    host.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
    host.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
    host.capabilities     = host_info.host_capabilities;
    host.monotonic_ns     = monotonic_ns;
    host.metric_add       = metric_add;
    host.incident_token   = incident;
    host.publish_prepared = publish;
    host.query_extension  = extension;

    module_session session(module, host);
    require(!models.enabled() || interface_id == OCUDU_DAPP_INTERFACE_RECEIVER_V1,
            "model certification is supported only for the receiver hook");
    const uint64_t certified_model_generation =
        models.enabled() ? certify_model_lifecycle(module, session.module_instance(), models) : 0;
    const auto* worker_api = static_cast<const ocudu_dapp_worker_api_v1*>(descriptor.interface_vtable);
    require(worker_api != nullptr && worker_api->struct_size >= offsetof(ocudu_dapp_worker_api_v1, reserved) &&
                worker_api->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 && worker_api->create_worker_context != nullptr &&
                worker_api->destroy_worker_context != nullptr,
            "worker API is incomplete");
    session.create_workers(*worker_api);
    std::vector<certification_shape> shapes;
    if (models.shape_set == "compact") {
      const uint32_t layers = profile->max_nof_layers < 2 ? profile->max_nof_layers : 2;
      const uint32_t ports  = profile->max_nof_rx_ports < 2 ? profile->max_nof_rx_ports : 2;
      shapes.push_back({1, ports, layers < ports ? layers : ports});
    } else {
      const auto append_shape = [&](certification_shape candidate) {
        const auto same_shape = [](const certification_shape& lhs, const certification_shape& rhs) {
          return lhs.nof_prb == rhs.nof_prb && lhs.nof_ports == rhs.nof_ports && lhs.nof_layers == rhs.nof_layers &&
                 lhs.start_symbol == rhs.start_symbol && lhs.start_prb == rhs.start_prb &&
                 lhs.grid_nof_symbols == rhs.grid_nof_symbols && lhs.grid_nof_prb == rhs.grid_nof_prb &&
                 lhs.nof_symbols == rhs.nof_symbols && lhs.nof_dmrs_symbols == rhs.nof_dmrs_symbols;
        };
        if (shapes.empty() || !same_shape(shapes.back(), candidate)) {
          shapes.push_back(candidate);
        }
      };
      append_shape({1, 1, 1});
      const uint32_t intermediate_ports  = std::min(profile->max_nof_rx_ports, 2U);
      const uint32_t intermediate_layers = std::min({profile->max_nof_layers, intermediate_ports, 2U});
      append_shape({std::min(profile->max_nof_prb, 25U), intermediate_ports, intermediate_layers});
      if (profile->max_nof_prb > 25U) {
        append_shape({25U, intermediate_ports, intermediate_layers, 2U, std::min(6U, profile->max_nof_prb - 25U), 14U,
                      std::min(profile->max_nof_prb, 106U)});
        append_shape({25U, intermediate_ports, intermediate_layers, 2U, std::min(6U, profile->max_nof_prb - 25U), 14U,
                      std::min(profile->max_nof_prb, 106U), 4U, 2U});
      }
      append_shape({std::min(profile->max_nof_prb, 106U), intermediate_ports, intermediate_layers});
      const certification_shape maximum{profile->max_nof_prb, profile->max_nof_rx_ports, profile->max_nof_layers};
      append_shape(maximum);
    }
    uint64_t expected_invocations = 0;
    for (certification_shape shape : shapes) {
      expected_invocations += certify_shape(descriptor, session, shape);
    }
    require(metric_value(module, session.module_instance(), OCUDU_DAPP_METRIC_INVOKE_COUNT_V1) == expected_invocations,
            "invocation metric did not record the modulation matrix");
    require(metric_value(module, session.module_instance(), OCUDU_DAPP_METRIC_INPUT_MAX_LAYERS_V1) ==
                profile->max_nof_layers,
            "maximum-layer metric did not record the vector");
    session.finish();

    int            device = 0;
    int            driver_version{};
    int            runtime_version{};
    cudaDeviceProp device_properties{};
    require_cuda(cudaGetDevice(&device), "cudaGetDevice");
    require_cuda(cudaGetDeviceProperties(&device_properties, device), "cudaGetDeviceProperties");
    require_cuda(cudaDriverGetVersion(&driver_version), "cudaDriverGetVersion");
    require_cuda(cudaRuntimeGetVersion(&runtime_version), "cudaRuntimeGetVersion");
    std::cout << "{\"status\":\"passed\",\"artifact\":" << json_string(argv[1])
              << ",\"globalName\":" << json_string(module.global_name) << ",\"version\":" << json_string(module.version)
              << ",\"backend\":\"cuda\",\"interfaceId\":" << interface_id
              << ",\"profile\":{\"maxPrb\":" << profile->max_nof_prb << ",\"maxRxPorts\":" << profile->max_nof_rx_ports
              << ",\"maxLayers\":" << profile->max_nof_layers
              << ",\"transformPrecoding\":false,\"dcOverlap\":true},\"lanes\":2,\"shapes\":[";
    for (size_t index = 0; index != shapes.size(); ++index) {
      if (index != 0) {
        std::cout << ',';
      }
      std::cout << "{\"prb\":" << shapes[index].nof_prb << ",\"ports\":" << shapes[index].nof_ports
                << ",\"layers\":" << shapes[index].nof_layers << ",\"startSymbol\":" << shapes[index].start_symbol
                << ",\"startPrb\":" << shapes[index].start_prb << ",\"gridSymbols\":" << shapes[index].grid_nof_symbols
                << ",\"gridPrb\":"
                << (shapes[index].grid_nof_prb == 0 ? shapes[index].start_prb + shapes[index].nof_prb
                                                    : shapes[index].grid_nof_prb)
                << ",\"allocationSymbols\":" << shapes[index].nof_symbols << ",\"dmrsSymbols\":[";
      for (uint32_t dmrs_symbol = 0; dmrs_symbol != shapes[index].nof_dmrs_symbols; ++dmrs_symbol) {
        if (dmrs_symbol != 0) {
          std::cout << ',';
        }
        std::cout << shapes[index].start_symbol + shapes[index].dmrs_symbol_offset(dmrs_symbol);
      }
      std::cout << "],\"pilots\":" << shapes[index].nof_pilots() << ",\"dataRe\":" << shapes[index].nof_data_re()
                << '}';
    }
    std::cout << "],\"shapeSet\":" << json_string(models.shape_set.c_str())
              << ",\"modulations\":[2,4,6,8],\"dmrsTypes\":[1,2],"
                 "\"coordinateBasis\":\"rxGrid\","
                 "\"dmrsSubcarrierOffsets\":{\"1\":[0,2],\"2\":[0,1]},"
                 "\"dcOverlap\":true,\"outputStorage\":{\"byteOffset\":256,\"guardBytes\":256,"
                 "\"boundsChecked\":true,\"identityChecked\":true,\"completeWritesChecked\":true},"
                 "\"invocations\":"
              << expected_invocations << ",\"modelLifecycle\":" << (models.enabled() ? "true" : "false");
    if (models.enabled()) {
      std::cout << ",\"modelGeneration\":" << certified_model_generation;
    }
    std::cout << ",\"device\":" << device << ",\"deviceName\":" << json_string(device_properties.name)
              << ",\"computeCapability\":\"" << device_properties.major << '.' << device_properties.minor
              << "\",\"driverVersion\":" << driver_version << ",\"runtimeVersion\":" << runtime_version << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Class-A CUDA certification failed: " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "Class-A CUDA certification failed: unknown exception\n";
    return 1;
  }
}
