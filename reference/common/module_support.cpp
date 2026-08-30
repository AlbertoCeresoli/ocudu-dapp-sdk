// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include <cstring>
#include <new>

using namespace ocudu::dapp::reference;

namespace {

struct worker_context {
  instance* owner;
  uint32_t  worker_id;
};

ocudu_dapp_value_v1 boolean_value(bool value) noexcept
{
  ocudu_dapp_value_v1 result{};
  result.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
  result.value.boolean = value;
  return result;
}

ocudu_dapp_value_v1 counter_value(uint64_t value) noexcept
{
  ocudu_dapp_value_v1 result{};
  result.kind               = OCUDU_DAPP_VALUE_UINT64_V1;
  result.value.uint64_value = value;
  return result;
}

ocudu_dapp_status_v1 create_worker(void* context, uint32_t worker_id, void** output) noexcept
{
  if (context == nullptr || output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto* worker = new (std::nothrow) worker_context{static_cast<instance*>(context), worker_id};
  if (worker == nullptr) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  *output = worker;
  return OCUDU_DAPP_OK_V1;
}

void destroy_worker(void*, void* worker) noexcept
{
  delete static_cast<worker_context*>(worker);
}

ocudu_dapp_status_v1 describe_config(void*, ocudu_dapp_config_descriptor_v1* descriptors, uint32_t capacity,
                                     uint32_t* count) noexcept
{
  if (count == nullptr || (capacity != 0 && descriptors == nullptr)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  *count = 1;
  if (capacity == 0) {
    return OCUDU_DAPP_OK_V1;
  }
  auto& descriptor         = descriptors[0];
  descriptor               = {};
  descriptor.struct_size   = sizeof(descriptor);
  descriptor.key           = "enabled";
  descriptor.description   = "Accept data-path invocations";
  descriptor.value_kind    = OCUDU_DAPP_VALUE_BOOL_V1;
  descriptor.default_value = boolean_value(true);
  descriptor.minimum_value = boolean_value(false);
  descriptor.maximum_value = boolean_value(true);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 get_config(void* context, ocudu_dapp_config_entry_v1* entries, uint32_t capacity, uint32_t* count,
                                uint64_t* generation) noexcept
{
  if (context == nullptr || count == nullptr || generation == nullptr || (capacity != 0 && entries == nullptr)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto&                       state = *static_cast<instance*>(context);
  std::lock_guard<std::mutex> lock(state.management_mutex);
  *count      = 1;
  *generation = state.config_generation;
  if (capacity != 0) {
    entries[0]       = {};
    entries[0].key   = "enabled";
    entries[0].value = boolean_value(state.enabled.load(std::memory_order_acquire));
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 set_config(void* context, const ocudu_dapp_config_update_v1* update, uint64_t* generation) noexcept
{
  if (context == nullptr || update == nullptr || generation == nullptr || update->nof_entries != 1 ||
      update->entries == nullptr || update->entries[0].key == nullptr ||
      std::strcmp(update->entries[0].key, "enabled") != 0 ||
      update->entries[0].value.kind != OCUDU_DAPP_VALUE_BOOL_V1 || update->entries[0].value.value.boolean > 1) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto&                       state = *static_cast<instance*>(context);
  std::lock_guard<std::mutex> lock(state.management_mutex);
  if (update->expected_generation != state.config_generation) {
    *generation = state.config_generation;
    return OCUDU_DAPP_CONFLICT_V1;
  }
  if (!update->dry_run) {
    state.enabled.store(update->entries[0].value.value.boolean != 0, std::memory_order_release);
    ++state.config_generation;
  }
  *generation = state.config_generation;
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 describe_metrics(void*, ocudu_dapp_metric_descriptor_v1* descriptors, uint32_t capacity,
                                      uint32_t* count) noexcept
{
  if (count == nullptr || (capacity != 0 && descriptors == nullptr)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  *count = 2;
  if (capacity != 0) {
    descriptors[0]             = {};
    descriptors[0].struct_size = sizeof(descriptors[0]);
    descriptors[0].name        = OCUDU_DAPP_METRIC_INVOKE_COUNT_V1;
    descriptors[0].description = "Accepted data-path invocations";
    descriptors[0].unit        = "invocations";
    descriptors[0].value_kind  = OCUDU_DAPP_VALUE_UINT64_V1;
    descriptors[0].flags       = OCUDU_DAPP_METRIC_COUNTER_V1;
    if (capacity > 1) {
      descriptors[1]             = {};
      descriptors[1].struct_size = sizeof(descriptors[1]);
      descriptors[1].name        = OCUDU_DAPP_METRIC_INPUT_MAX_LAYERS_V1;
      descriptors[1].description = "Maximum PUSCH layer count observed; zero when not applicable";
      descriptors[1].unit        = "layers";
      descriptors[1].value_kind  = OCUDU_DAPP_VALUE_UINT64_V1;
      descriptors[1].flags       = OCUDU_DAPP_METRIC_GAUGE_V1;
    }
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 read_metrics(void* context, ocudu_dapp_metric_sample_v1* samples, uint32_t capacity,
                                  uint32_t* count) noexcept
{
  if (context == nullptr || count == nullptr || (capacity != 0 && samples == nullptr)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto& state = *static_cast<instance*>(context);
  *count      = 2;
  if (capacity != 0) {
    samples[0]       = {};
    samples[0].name  = OCUDU_DAPP_METRIC_INVOKE_COUNT_V1;
    samples[0].value = counter_value(state.invocation_count.load(std::memory_order_relaxed));
    if (state.host.monotonic_ns != nullptr) {
      samples[0].monotonic_timestamp_ns = state.host.monotonic_ns(state.host.host_context);
    }
    if (capacity > 1) {
      samples[1]                        = {};
      samples[1].name                   = OCUDU_DAPP_METRIC_INPUT_MAX_LAYERS_V1;
      samples[1].value                  = counter_value(state.input_max_layers.load(std::memory_order_relaxed));
      samples[1].monotonic_timestamp_ns = samples[0].monotonic_timestamp_ns;
    }
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 manage_model(void* context, const ocudu_dapp_model_request_v1* request,
                                  uint64_t* generation) noexcept
{
  if (context == nullptr || request == nullptr || generation == nullptr || request->struct_size < sizeof(*request) ||
      request->expected_generation == 0 || request->action < OCUDU_DAPP_MODEL_STAGE_V1 ||
      request->action > OCUDU_DAPP_MODEL_RETIRE_V1 || request->model_id == nullptr || request->model_id[0] == '\0' ||
      request->artifact.size > 1024 * 1024 || (request->artifact.size != 0 && request->artifact.data == nullptr)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto&                       state = *static_cast<instance*>(context);
  std::lock_guard<std::mutex> lock(state.management_mutex);
  *generation = state.model_generation;
  if (request->expected_generation != state.model_generation) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  std::string id;
  std::string version;
  try {
    id      = request->model_id;
    version = request->model_version != nullptr ? request->model_version : "";
  } catch (const std::bad_alloc&) {
    return OCUDU_DAPP_RESOURCE_V1;
  } catch (...) {
    return OCUDU_DAPP_EXCEPTION_V1;
  }
  auto valid = [&]() noexcept {
    switch (request->action) {
      case OCUDU_DAPP_MODEL_STAGE_V1:
        return request->artifact.size != 0;
      case OCUDU_DAPP_MODEL_VALIDATE_V1:
        return state.staged_model.id == id && state.staged_model.phase >= 1;
      case OCUDU_DAPP_MODEL_WARM_V1:
        return state.staged_model.id == id && state.staged_model.phase >= 2;
      case OCUDU_DAPP_MODEL_ACTIVATE_V1:
        return state.staged_model.id == id && state.staged_model.phase >= 3;
      case OCUDU_DAPP_MODEL_ROLLBACK_V1:
        return !state.previous_model.id.empty() && state.previous_model.id == id;
      case OCUDU_DAPP_MODEL_RETIRE_V1:
        return state.staged_model.id == id || state.active_model.id == id || state.previous_model.id == id;
      default:
        return false;
    }
  };
  if (!valid()) {
    return OCUDU_DAPP_CONFLICT_V1;
  }
  if (request->dry_run) {
    return OCUDU_DAPP_OK_V1;
  }
  try {
    switch (request->action) {
      case OCUDU_DAPP_MODEL_STAGE_V1: {
        const auto* begin  = static_cast<const uint8_t*>(request->artifact.data);
        state.staged_model = {id, version, std::vector<uint8_t>(begin, begin + request->artifact.size), 1};
        break;
      }
      case OCUDU_DAPP_MODEL_VALIDATE_V1:
        state.staged_model.phase = 2;
        break;
      case OCUDU_DAPP_MODEL_WARM_V1:
        state.staged_model.phase = 3;
        break;
      case OCUDU_DAPP_MODEL_ACTIVATE_V1:
        state.previous_model = std::move(state.active_model);
        state.active_model   = std::move(state.staged_model);
        state.staged_model   = {};
        break;
      case OCUDU_DAPP_MODEL_ROLLBACK_V1:
        std::swap(state.active_model, state.previous_model);
        break;
      case OCUDU_DAPP_MODEL_RETIRE_V1:
        if (state.staged_model.id == id)
          state.staged_model = {};
        if (state.active_model.id == id)
          state.active_model = {};
        if (state.previous_model.id == id)
          state.previous_model = {};
        break;
      default:
        return OCUDU_DAPP_INTERNAL_ERROR_V1;
    }
  } catch (const std::bad_alloc&) {
    return OCUDU_DAPP_RESOURCE_V1;
  } catch (...) {
    return OCUDU_DAPP_EXCEPTION_V1;
  }
  *generation = ++state.model_generation;
  return OCUDU_DAPP_OK_V1;
}

} // namespace

ocudu_dapp_status_v1 ocudu::dapp::reference::create(const ocudu_dapp_host_api_v1* host, const void*,
                                                    void**                        output) noexcept
{
  if (output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  *output     = nullptr;
  auto* state = new (std::nothrow) instance;
  if (state == nullptr) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  const auto status = initialize_instance(*state, host);
  if (status != OCUDU_DAPP_OK_V1) {
    delete state;
    return status;
  }
  *output = state;
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::initialize_instance(instance&                     state,
                                                                 const ocudu_dapp_host_api_v1* host) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_host_api_v1, reserved);
  if (host == nullptr || host->struct_size < minimum || host->abi_major != OCUDU_DAPP_ABI_MAJOR_V1) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  state.host = *host;
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::configure(void* context, const void*) noexcept
{
  return context != nullptr ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::prepare(void* context, const void*) noexcept
{
  return context != nullptr ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::warmup(void* context, const void*) noexcept
{
  return context != nullptr ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::start(void* context) noexcept
{
  if (context == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  static_cast<instance*>(context)->active.store(true, std::memory_order_release);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::stop(void* context) noexcept
{
  if (context == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  static_cast<instance*>(context)->active.store(false, std::memory_order_release);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::quiesce(void* context, uint64_t) noexcept
{
  return context != nullptr ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
}

void ocudu::dapp::reference::destroy(void* context) noexcept
{
  delete static_cast<instance*>(context);
}

const ocudu_dapp_worker_api_v1& ocudu::dapp::reference::worker_api() noexcept
{
  static const ocudu_dapp_worker_api_v1 api = [] {
    ocudu_dapp_worker_api_v1 result{};
    result.struct_size            = sizeof(result);
    result.abi_major              = OCUDU_DAPP_ABI_MAJOR_V1;
    result.abi_minor              = OCUDU_DAPP_ABI_MINOR_V1;
    result.create_worker_context  = create_worker;
    result.destroy_worker_context = destroy_worker;
    return result;
  }();
  return api;
}

const ocudu_dapp_management_v1& ocudu::dapp::reference::management_api() noexcept
{
  static const ocudu_dapp_management_v1 api = [] {
    ocudu_dapp_management_v1 result{};
    result.struct_size = sizeof(result);
    result.major       = OCUDU_DAPP_MANAGEMENT_MAJOR_V1;
    result.minor       = OCUDU_DAPP_MANAGEMENT_MINOR_V1;
    result.capability_bits =
        OCUDU_DAPP_MANAGEMENT_CONFIG_V1 | OCUDU_DAPP_MANAGEMENT_METRICS_V1 | OCUDU_DAPP_MANAGEMENT_MODELS_V1;
    result.describe_config  = describe_config;
    result.get_config       = get_config;
    result.set_config       = set_config;
    result.describe_metrics = describe_metrics;
    result.read_metrics     = read_metrics;
    result.manage_model     = manage_model;
    return result;
  }();
  return api;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::begin_invocation(void* context) noexcept
{
  if (context == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  return begin_invocation(*static_cast<worker_context*>(context)->owner);
}

ocudu_dapp_status_v1 ocudu::dapp::reference::begin_invocation(instance& state) noexcept
{
  if (!state.active.load(std::memory_order_acquire) || !state.enabled.load(std::memory_order_acquire)) {
    return OCUDU_DAPP_BYPASS_V1;
  }
  state.invocation_count.fetch_add(1, std::memory_order_relaxed);
  return OCUDU_DAPP_OK_V1;
}

void ocudu::dapp::reference::add_metric(void* context, uint32_t metric_id, uint64_t value) noexcept
{
  if (context == nullptr) {
    return;
  }
  add_metric(*static_cast<worker_context*>(context)->owner, metric_id, value);
}

void ocudu::dapp::reference::add_metric(instance& state, uint32_t metric_id, uint64_t value) noexcept
{
  if (state.host.metric_add != nullptr) {
    state.host.metric_add(state.host.host_context, metric_id, value);
  }
}

void ocudu::dapp::reference::observe_input_layers(void* context, uint32_t nof_layers) noexcept
{
  if (context != nullptr) {
    observe_input_layers(*static_cast<worker_context*>(context)->owner, nof_layers);
  }
}

void ocudu::dapp::reference::observe_input_layers(instance& state, uint32_t nof_layers) noexcept
{
  uint64_t maximum = state.input_max_layers.load(std::memory_order_relaxed);
  while (maximum < nof_layers && !state.input_max_layers.compare_exchange_weak(
                                     maximum, nof_layers, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

const ocudu_dapp_host_api_v1* ocudu::dapp::reference::host_api(void* context) noexcept
{
  auto* worker = static_cast<worker_context*>(context);
  return worker != nullptr && worker->owner != nullptr ? &worker->owner->host : nullptr;
}

ocudu_dapp_pusch_profile_v1 ocudu::dapp::reference::make_pusch_profile(uint32_t max_layers, uint32_t flags) noexcept
{
  ocudu_dapp_pusch_profile_v1 result{};
  result.struct_size      = sizeof(result);
  result.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  result.profile_id       = OCUDU_DAPP_PUSCH_PROFILE_V1;
  result.max_nof_prb      = 275;
  result.max_nof_rx_ports = 64;
  result.max_nof_layers   = max_layers;
  result.modulation_mask  = OCUDU_DAPP_PUSCH_MODULATION_QPSK_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM16_V1 |
                           OCUDU_DAPP_PUSCH_MODULATION_QAM64_V1 | OCUDU_DAPP_PUSCH_MODULATION_QAM256_V1;
  result.dmrs_type_mask = OCUDU_DAPP_PUSCH_DMRS_TYPE1_V1 | OCUDU_DAPP_PUSCH_DMRS_TYPE2_V1;
  result.flags          = flags;
  return result;
}

ocudu_dapp_interface_descriptor_v1
ocudu::dapp::reference::make_interface(uint32_t interface_id, uint32_t dapp_class, uint32_t placement_mask,
                                       uint32_t threading, const void* vtable, uint64_t capabilities,
                                       const ocudu_dapp_pusch_profile_v1* profile) noexcept
{
  ocudu_dapp_interface_descriptor_v1 result{};
  result.struct_size      = sizeof(result);
  result.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  result.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  result.interface_id     = interface_id;
  result.interface_major  = 1;
  result.interface_minor  = 0;
  result.dapp_class       = dapp_class;
  result.placement_mask   = placement_mask;
  result.threading        = threading;
  result.interface_vtable = vtable;
  result.capability_bits  = capabilities;
  result.use_case_profile = profile;
  return result;
}

ocudu_dapp_status_v1
ocudu::dapp::reference::populate_module(const ocudu_dapp_host_info_v1* host, ocudu_dapp_module_v1* output,
                                        const char* name, const ocudu_dapp_interface_descriptor_v1* interface) noexcept
{
  return populate_module(host, output, name, "OCUDU reference", "v1", interface);
}

ocudu_dapp_status_v1
ocudu::dapp::reference::populate_module(const ocudu_dapp_host_info_v1* host, ocudu_dapp_module_v1* output,
                                        const char* name, const char* vendor, const char* version,
                                        const ocudu_dapp_interface_descriptor_v1* interface) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_host_info_v1, reserved);
  if (host == nullptr || output == nullptr || name == nullptr || vendor == nullptr || version == nullptr ||
      interface == nullptr || host->struct_size < minimum || host->abi_major != OCUDU_DAPP_ABI_MAJOR_V1) {
    return OCUDU_DAPP_UNSUPPORTED_V1;
  }
  *output                = {};
  output->struct_size    = sizeof(*output);
  output->abi_major      = OCUDU_DAPP_ABI_MAJOR_V1;
  output->abi_minor      = OCUDU_DAPP_ABI_MINOR_V1;
  output->global_name    = name;
  output->vendor         = vendor;
  output->version        = version;
  output->nof_interfaces = 1;
  output->interfaces     = interface;
  output->create         = create;
  output->configure      = configure;
  output->prepare        = prepare;
  output->warmup         = warmup;
  output->start          = start;
  output->stop           = stop;
  output->quiesce        = quiesce;
  output->destroy        = destroy;
  output->management     = &management_api();
  return OCUDU_DAPP_OK_V1;
}
