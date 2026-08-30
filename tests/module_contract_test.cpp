// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/abi/v1/metric_catalog.h"
#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu/dapp/use_cases/v1/coordination.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <iostream>

namespace {

bool require(bool condition, const char* message)
{
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

uint64_t monotonic(void*)
{
  return 42;
}

struct complex32 {
  float real;
  float imag;
};

struct prepared_capture {
  uint32_t                                             calls{};
  ocudu_dapp_context_header_v1                         context{};
  std::array<uint8_t, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1> confidence{};
  std::array<uint8_t, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1> scheduler_avoidance{};
  uint32_t                                             nof_prbs{};
  uint32_t                                             scheduler_avoided{};
  ocudu_dapp_srs_isac_cir_preview_v1                   srs_preview{};
  ocudu_dapp_quiet_period_intent_v1                    quiet_intent{};
};

ocudu_dapp_status_v1 capture_prepared(void* context, uint32_t output_port_id, uint32_t schema_id,
                                      const void* typed_object, uint32_t object_size)
{
  auto* capture = static_cast<prepared_capture*>(context);
  if (capture == nullptr || typed_object == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (output_port_id == OCUDU_DAPP_SRS_ISAC_OUTPUT_CIR_PREVIEW_V1 &&
      schema_id == OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 && object_size == sizeof(ocudu_dapp_srs_isac_cir_preview_v1)) {
    capture->srs_preview = *static_cast<const ocudu_dapp_srs_isac_cir_preview_v1*>(typed_object);
    ++capture->calls;
    return OCUDU_DAPP_OK_V1;
  }
  if (output_port_id == OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1 &&
      schema_id == OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1 &&
      object_size == sizeof(ocudu_dapp_quiet_period_intent_v1)) {
    capture->quiet_intent = *static_cast<const ocudu_dapp_quiet_period_intent_v1*>(typed_object);
    ++capture->calls;
    return OCUDU_DAPP_OK_V1;
  }
  if (output_port_id != OCUDU_DAPP_SPECTRUM_OUTPUT_INTERFERENCE_MAP_V1 ||
      schema_id != OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1 ||
      object_size < offsetof(ocudu_dapp_spectrum_interference_map_v1, reserved)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto& output = *static_cast<const ocudu_dapp_spectrum_interference_map_v1*>(typed_object);
  if (output.nof_prbs == 0 || output.nof_prbs > capture->confidence.size() ||
      output.prb_confidence.native_handle == 0) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  capture->context  = output.context;
  capture->nof_prbs = output.nof_prbs;
  std::memcpy(capture->confidence.data(),
              reinterpret_cast<const void*>(output.prb_confidence.native_handle + output.prb_confidence.byte_offset),
              output.nof_prbs);
  ++capture->calls;
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_tensor_view_v1 make_tensor(void* data, uint32_t element_type, uint32_t element_size, uint32_t access,
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
  uint64_t stride = element_size;
  for (uint32_t dimension = view.rank; dimension != 0; --dimension) {
    view.byte_strides[dimension - 1] = stride;
    stride *= view.dimensions[dimension - 1];
  }
  view.byte_length = elements * element_size;
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

ocudu_dapp_status_v1 invoke(const ocudu_dapp_interface_descriptor_v1& descriptor, void* worker,
                            prepared_capture* prepared = nullptr)
{
  std::array<complex32, 8> samples{
      {{1, 0}, {0.7F, 0.7F}, {0.7F, 0.7F}, {0.7F, 0.7F}, {1, 0}, {0.7F, 0.7F}, {0.7F, 0.7F}, {0.7F, 0.7F}}};
  std::array<complex32, 2>     references{{{1, 0}, {1, 0}}};
  std::array<uint16_t, 4>      coordinates{{0, 0, 1, 0}};
  std::array<uint8_t, 8>       data_mask{{0, 1, 1, 1, 0, 1, 1, 1}};
  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = 1;
  pusch.nof_layers           = 1;
  pusch.modulation           = OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.nof_symbols          = 2;
  pusch.nof_subcarriers      = 4;
  pusch.noise_variance_floor = 1.0e-3F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.data_re_mask =
      make_tensor(data_mask.data(), OCUDU_DAPP_ELEMENT_U8_V1, sizeof(uint8_t), OCUDU_DAPP_ACCESS_READ_V1, {2, 4});
  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = 2;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reference_symbols =
      make_tensor(references.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(complex32), OCUDU_DAPP_ACCESS_READ_V1, {1, 2});
  dmrs.coordinates =
      make_tensor(coordinates.data(), OCUDU_DAPP_ELEMENT_U16_V1, sizeof(uint16_t), OCUDU_DAPP_ACCESS_READ_V1, {2, 2});
  const auto rx_grid =
      make_tensor(samples.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(complex32), OCUDU_DAPP_ACCESS_READ_V1, {1, 2, 4});
  switch (descriptor.interface_id) {
    case OCUDU_DAPP_INTERFACE_SPECTRUM_V1: {
      std::array<complex32, 24> spectrum_samples{};
      for (uint32_t index = 0; index != 12; ++index) {
        spectrum_samples[index] = {2.0F, 0.0F};
      }
      ocudu_dapp_spectrum_input_v1 input{};
      input.invocation.struct_size      = sizeof(input.invocation);
      input.invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
      input.invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
      input.invocation.cell_id          = 2;
      input.invocation.radio_time       = {5, 3, 0, 0};
      input.invocation.input_generation = 11;
      input.uplink_resource_grid = make_tensor(spectrum_samples.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(complex32),
                                               OCUDU_DAPP_ACCESS_READ_V1, {1, 1, 24});
      ocudu_dapp_spectrum_slot_metadata_v1 metadata{};
      metadata.struct_size     = sizeof(metadata);
      metadata.abi_major       = OCUDU_DAPP_ABI_MAJOR_V1;
      metadata.bandwidth_hz    = 360'000;
      metadata.rx_ports        = 1;
      metadata.symbols         = 1;
      metadata.subcarriers     = 24;
      metadata.amplitude_scale = 1.0F;
      input.slot_metadata      = typed(OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1, metadata);
      input.source_sequence    = 11;
      return static_cast<const ocudu_dapp_spectrum_interface_v1*>(descriptor.interface_vtable)->publish(worker, &input);
    }
    case OCUDU_DAPP_INTERFACE_SRS_ISAC_V1: {
      ocudu_dapp_srs_isac_input_v1 input{};
      input.invocation.struct_size           = sizeof(input.invocation);
      input.invocation.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
      input.invocation.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
      input.invocation.cell_id               = 2;
      input.invocation.radio_time            = {5, 3, 0, 0};
      input.invocation.input_generation      = 11;
      input.channel_estimates                = rx_grid;
      input.channel_estimates.layout_id      = OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1;
      input.channel_estimates.layout_version = 1;
      ocudu_dapp_srs_isac_metadata_v1 metadata{};
      metadata.struct_size           = sizeof(metadata);
      metadata.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
      metadata.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
      metadata.srs_resource_id       = 17;
      metadata.srs_config_id         = 19;
      metadata.rx_ports              = 1;
      metadata.tx_ports              = 2;
      metadata.comb_size             = 2;
      metadata.subcarrier_spacing_hz = 30'000;
      metadata.frequency_points      = 4;
      metadata.noise_variance        = 0.25F;
      input.srs_metadata             = typed(OCUDU_DAPP_METADATA_SRS_ISAC_V1, metadata);
      input.source_sequence          = 11;
      return static_cast<const ocudu_dapp_srs_isac_interface_v1*>(descriptor.interface_vtable)->publish(worker, &input);
    }
    case OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1: {
      ocudu_dapp_channel_estimator_input_v1  input{};
      ocudu_dapp_channel_estimator_output_v1 output{};
      std::array<complex32, 8>               estimates{};
      std::array<float, 1>                   noise{};
      input.rx_grid            = rx_grid;
      input.pusch              = typed(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
      input.dmrs               = typed(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);
      output.channel_estimates = make_tensor(estimates.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(complex32),
                                             OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, 2, 4});
      output.noise_variances =
          make_tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, sizeof(float), OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1});
      return static_cast<const ocudu_dapp_channel_estimator_interface_v1*>(descriptor.interface_vtable)
          ->invoke(worker, &input, &output);
    }
    case OCUDU_DAPP_INTERFACE_EQUALIZER_V1: {
      ocudu_dapp_equalizer_input_v1  input{};
      ocudu_dapp_equalizer_output_v1 output{};
      std::array<complex32, 8>       estimates{};
      std::array<complex32, 8>       equalized{};
      std::array<float, 8>           noise{};
      input.rx_grid            = rx_grid;
      input.pusch              = typed(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
      input.dmrs               = typed(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);
      output.channel_estimates = make_tensor(estimates.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(complex32),
                                             OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, 2, 4});
      output.equalized_symbols = make_tensor(equalized.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(complex32),
                                             OCUDU_DAPP_ACCESS_WRITE_V1, {1, 2, 4});
      output.post_equalization_noise =
          make_tensor(noise.data(), OCUDU_DAPP_ELEMENT_F32_V1, sizeof(float), OCUDU_DAPP_ACCESS_WRITE_V1, {1, 2, 4});
      return static_cast<const ocudu_dapp_equalizer_interface_v1*>(descriptor.interface_vtable)
          ->invoke(worker, &input, &output);
    }
    case OCUDU_DAPP_INTERFACE_RECEIVER_V1: {
      ocudu_dapp_receiver_input_v1  input{};
      ocudu_dapp_receiver_output_v1 output{};
      std::array<int8_t, 12>        llrs{};
      input.rx_grid = rx_grid;
      input.pusch   = typed(OCUDU_DAPP_METADATA_PUSCH_V1, pusch);
      input.dmrs    = typed(OCUDU_DAPP_METADATA_DMRS_V1, dmrs);
      output.llrs =
          make_tensor(llrs.data(), OCUDU_DAPP_ELEMENT_I8_V1, sizeof(int8_t), OCUDU_DAPP_ACCESS_WRITE_V1, {llrs.size()});
      return static_cast<const ocudu_dapp_receiver_interface_v1*>(descriptor.interface_vtable)
          ->invoke(worker, &input, &output);
    }
    case OCUDU_DAPP_INTERFACE_SCHEDULER_V1: {
      ocudu_dapp_scheduler_input_v1                    input{};
      ocudu_dapp_scheduler_output_v1                   output{};
      std::array<ocudu_dapp_scheduler_candidate_v1, 2> candidates{};
      std::array<ocudu_dapp_scheduler_intent_v1, 2>    intents{};
      for (uint32_t index = 0; index != candidates.size(); ++index) {
        candidates[index].struct_size   = sizeof(candidates[index]);
        candidates[index].candidate_id  = index;
        candidates[index].ue_index      = index + 1;
        candidates[index].pending_bytes = (index + 1) * 100;
      }
      input.direction              = OCUDU_DAPP_SCHEDULER_UL_V1;
      input.invocation.struct_size = sizeof(input.invocation);
      input.invocation.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
      input.invocation.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
      input.invocation.cell_id     = 2;
      input.invocation.radio_time  = {5, 3, 0, 0};
      input.transmission_time      = {5, 4, 0, 0};
      input.nof_candidates         = candidates.size();
      input.candidate_generation   = 1;
      input.candidates             = candidates.data();
      output.struct_size           = sizeof(output);
      output.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
      output.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
      output.intents               = intents.data();
      output.intent_capacity       = intents.size();
      std::array<uint8_t, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1> avoidance{};
      if (prepared != nullptr && prepared->nof_prbs != 0) {
        input.cell_nof_prbs        = prepared->nof_prbs;
        input.interference_context = prepared->context;
        input.optional_interference_mask =
            make_tensor(prepared->confidence.data(), OCUDU_DAPP_ELEMENT_U8_V1, sizeof(uint8_t),
                        OCUDU_DAPP_ACCESS_READ_V1, {prepared->nof_prbs});
        input.optional_interference_mask.layout_id      = OCUDU_DAPP_LAYOUT_PRB_INTERFERENCE_U8_V1;
        input.optional_interference_mask.layout_version = 1;
        input.interference_context_present              = 1;
        input.allowed_output_flags                      = OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1;
        output.avoid_prb_mask                = make_tensor(avoidance.data(), OCUDU_DAPP_ELEMENT_U8_V1, sizeof(uint8_t),
                                                           OCUDU_DAPP_ACCESS_WRITE_V1, {prepared->nof_prbs});
        output.avoid_prb_mask.layout_id      = OCUDU_DAPP_LAYOUT_PRB_INTERFERENCE_U8_V1;
        output.avoid_prb_mask.layout_version = 1;
      }
      const auto status = static_cast<const ocudu_dapp_scheduler_interface_v1*>(descriptor.interface_vtable)
                              ->invoke(worker, &input, &output);
      if (status == OCUDU_DAPP_OK_V1 && prepared != nullptr && prepared->nof_prbs != 0) {
        std::copy_n(avoidance.begin(), prepared->nof_prbs, prepared->scheduler_avoidance.begin());
        prepared->scheduler_avoided =
            static_cast<uint32_t>(std::count(avoidance.begin(), avoidance.begin() + prepared->nof_prbs, uint8_t{1}));
      }
      return status;
    }
    default:
      return OCUDU_DAPP_UNSUPPORTED_V1;
  }
}

} // namespace

int main(int argc, char** argv)
{
  const bool joint  = argc == 3 && std::strcmp(argv[2], "joint") == 0;
  const bool fusion = argc == 3 && std::strcmp(argv[2], "fusion") == 0;
  if (!joint && !fusion && argc != 5 && argc != 8)
    return 2;
  void* library = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!require(library != nullptr, ::dlerror()))
    return 1;
  auto query = reinterpret_cast<ocudu_dapp_query_v1_fn>(::dlsym(library, "ocudu_dapp_query_v1"));
  if (!require(query != nullptr, "query symbol is missing"))
    return 1;

  ocudu_dapp_host_info_v1 host_info{};
  host_info.struct_size = sizeof(host_info);
  host_info.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  host_info.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  ocudu_dapp_module_v1 module{};
  bool                 ok = require(query(&host_info, &module) == OCUDU_DAPP_OK_V1, "query failed") &&
            require(module.nof_interfaces == (joint || fusion ? 2U : 1U), "unexpected interface count") &&
            require(module.management != nullptr, "management extension is missing");
  if (joint) {
    ok = require(module.interfaces[0].interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
                     module.interfaces[0].dapp_class == OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 &&
                     module.interfaces[0].placement_mask == OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                 "invalid joint spectrum interface") &&
         require(module.interfaces[1].interface_id == OCUDU_DAPP_INTERFACE_SCHEDULER_V1 &&
                     module.interfaces[1].dapp_class == OCUDU_DAPP_CLASS_B_DEADLINE_V1 &&
                     module.interfaces[1].placement_mask == OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                 "invalid joint scheduler interface") &&
         require(module.global_name != nullptr &&
                     std::strcmp(module.global_name, "org.ocudu.test.joint-spectrum-scheduler.cpu") == 0,
                 "wrong joint module name") &&
         require(module.version != nullptr && std::strcmp(module.version, "v1") == 0, "wrong joint module version") &&
         ok;
  } else if (fusion) {
    ok = require(module.interfaces[0].interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 &&
                     module.interfaces[0].dapp_class == OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 &&
                     module.interfaces[0].placement_mask == OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                 "invalid fusion SRS interface") &&
         require(module.interfaces[1].interface_id == 0x7f010002U &&
                     module.interfaces[1].dapp_class == OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1 &&
                     module.interfaces[1].placement_mask == OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                 "invalid fusion future-DMRS interface") &&
         require(module.global_name != nullptr &&
                     std::strcmp(module.global_name, "org.ocudu.test.srs-dmrs-fusion.cpu") == 0,
                 "wrong fusion module name") &&
         require(module.version != nullptr && std::strcmp(module.version, "v1") == 0, "wrong fusion module version") &&
         ok;
  } else {
    ok = require(module.interfaces[0].interface_id == std::strtoul(argv[2], nullptr, 0), "wrong interface id") &&
         require(module.interfaces[0].dapp_class == std::strtoul(argv[3], nullptr, 0), "wrong class") &&
         require(module.interfaces[0].placement_mask == std::strtoul(argv[4], nullptr, 0), "wrong placement") && ok;
  }
  if (argc == 8) {
    ok = require(module.global_name != nullptr && std::strcmp(module.global_name, argv[5]) == 0, "wrong global name") &&
         ok;
    ok = require(module.vendor != nullptr && std::strcmp(module.vendor, argv[6]) == 0, "wrong vendor") && ok;
    ok = require(module.version != nullptr && std::strcmp(module.version, argv[7]) == 0, "wrong version") && ok;
  }

  ocudu_dapp_host_api_v1 host{};
  prepared_capture       prepared;
  host.struct_size      = sizeof(host);
  host.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  host.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  host.host_context     = &prepared;
  host.monotonic_ns     = monotonic;
  host.publish_prepared = capture_prepared;
  void* instance        = nullptr;
  ok                    = require(module.create(&host, nullptr, &instance) == OCUDU_DAPP_OK_V1, "create failed") && ok;
  ok                    = require(module.prepare(instance, nullptr) == OCUDU_DAPP_OK_V1, "prepare failed") && ok;
  ok                    = require(module.start(instance) == OCUDU_DAPP_OK_V1, "start failed") && ok;

  auto* worker_api = static_cast<const ocudu_dapp_worker_api_v1*>(module.interfaces[0].interface_vtable);
  void* worker     = nullptr;
  ok = require(worker_api->create_worker_context(instance, 7, &worker) == OCUDU_DAPP_OK_V1, "worker creation failed") &&
       ok;
  void* scheduler_worker = nullptr;
  if (joint) {
    auto* scheduler_workers = static_cast<const ocudu_dapp_worker_api_v1*>(module.interfaces[1].interface_vtable);
    ok = require(scheduler_workers->create_worker_context(instance, 8, &scheduler_worker) == OCUDU_DAPP_OK_V1,
                 "joint scheduler worker creation failed") &&
         require(invoke(module.interfaces[1], scheduler_worker, &prepared) == OCUDU_DAPP_BYPASS_V1,
                 "joint scheduler did not bypass before spectrum state") &&
         ok;
  }
  const auto invocation_status = invoke(module.interfaces[0], worker, &prepared);
  const bool resident_accelerator =
      module.interfaces[0].dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1 &&
      (module.interfaces[0].capability_bits & OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1) != 0;
  if (module.interfaces[0].dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1) {
    const auto* profile = static_cast<const ocudu_dapp_pusch_profile_v1*>(module.interfaces[0].use_case_profile);
    ok = require(profile != nullptr && profile->struct_size >= offsetof(ocudu_dapp_pusch_profile_v1, reserved) &&
                     profile->profile_id == OCUDU_DAPP_PUSCH_PROFILE_V1 && profile->max_nof_prb == 275 &&
                     profile->max_nof_rx_ports == 64 && profile->max_nof_layers == (resident_accelerator ? 2U : 1U) &&
                     profile->modulation_mask == 15 && profile->dmrs_type_mask == 3 &&
                     profile->flags == (resident_accelerator ? OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1 : 0U),
                 "invalid Class-A PUSCH admission profile") &&
         ok;
  } else {
    ok = require(module.interfaces[0].use_case_profile == nullptr, "unexpected non-Class-A PUSCH profile") && ok;
  }
  ok = require(resident_accelerator
                   ? invocation_status == OCUDU_DAPP_INVALID_INPUT_V1 || invocation_status == OCUDU_DAPP_UNSUPPORTED_V1
                   : invocation_status == OCUDU_DAPP_OK_V1,
               "unexpected invocation status") &&
       ok;
  if (joint) {
    ok = require(invoke(module.interfaces[1], scheduler_worker, &prepared) == OCUDU_DAPP_OK_V1,
                 "joint scheduler did not consume fused spectrum state") &&
         require(prepared.scheduler_avoided == 1 && prepared.scheduler_avoidance[0] == 1 &&
                     prepared.scheduler_avoidance[1] == 0,
                 "joint scheduler avoidance output is incorrect") &&
         ok;
  }
  const bool bounded_quiet_authority =
      module.interfaces[0].interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
      (module.interfaces[0].capability_bits & OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1) != 0;
  if (bounded_quiet_authority) {
    ok = require(invoke(module.interfaces[0], worker, &prepared) == OCUDU_DAPP_OK_V1,
                 "repeated spectrum invocation failed") &&
         require(prepared.calls == 3, "spectrum quiet authority was not rate limited") && ok;
  }
  if (module.interfaces[0].interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
    const uint32_t expected_calls = bounded_quiet_authority ? 3U : 1U;
    ok = require(prepared.calls == expected_calls && prepared.nof_prbs == 2 && prepared.confidence[0] == 255 &&
                     prepared.confidence[1] == 0 && prepared.context.cell_id == 2 &&
                     prepared.context.valid_from.slot == 4 &&
                     (!bounded_quiet_authority ||
                      (prepared.quiet_intent.request_id[0] == 11 && prepared.quiet_intent.earliest_start.slot == 7 &&
                       prepared.quiet_intent.first_prb == 0)),
                 "spectrum interference publication failed") &&
         ok;
  } else if (module.interfaces[0].interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 && !fusion) {
    ok = require(prepared.calls == 1 && prepared.srs_preview.source_srs_sequence == 11 &&
                     prepared.srs_preview.srs_resource_id == 17 && prepared.srs_preview.srs_config_id == 19 &&
                     prepared.srs_preview.tap_count == 4 && prepared.srs_preview.context.cell_id == 2 &&
                     prepared.srs_preview.context.created.sfn == 5,
                 "SRS CIR preview publication failed") &&
         ok;
  }

  uint32_t config_count = 0;
  // Packages may expose product knobs beyond the canonical "enabled" key.
  ok = require(module.management->describe_config(instance, nullptr, 0, &config_count) == OCUDU_DAPP_OK_V1 &&
                   config_count >= 1,
               "config discovery failed") &&
       ok;
  uint32_t metric_count = 0;
  ok = require(module.management->describe_metrics(instance, nullptr, 0, &metric_count) == OCUDU_DAPP_OK_V1 &&
                   metric_count == 2,
               "metric discovery failed") &&
       ok;
  std::array<ocudu_dapp_metric_descriptor_v1, 2> metrics{};
  ok = require(module.management->describe_metrics(instance, metrics.data(), metrics.size(), &metric_count) ==
                       OCUDU_DAPP_OK_V1 &&
                   std::strcmp(metrics[0].name, OCUDU_DAPP_METRIC_INVOKE_COUNT_V1) == 0 &&
                   metrics[0].flags == OCUDU_DAPP_METRIC_COUNTER_V1 &&
                   std::strcmp(metrics[1].name, OCUDU_DAPP_METRIC_INPUT_MAX_LAYERS_V1) == 0 &&
                   metrics[1].flags == OCUDU_DAPP_METRIC_GAUGE_V1,
               "canonical invocation metric descriptor failed") &&
       ok;
  std::array<ocudu_dapp_metric_sample_v1, 2> samples{};
  const bool observed_layers = module.interfaces[0].dapp_class == OCUDU_DAPP_CLASS_A_INLINE_V1 && !resident_accelerator;
  const uint64_t expected_invocations = joint ? 3U : bounded_quiet_authority ? 2U : 1U;
  ok = require(module.management->read_metrics(instance, samples.data(), samples.size(), &metric_count) ==
                       OCUDU_DAPP_OK_V1 &&
                   samples[0].value.value.uint64_value == expected_invocations &&
                   samples[1].value.value.uint64_value == (observed_layers ? 1 : 0),
               "invocation metric failed") &&
       ok;

  uint32_t                   config_entries = 0;
  uint64_t                   generation     = 0;
  ocudu_dapp_config_entry_v1 entry{};
  ok = require(module.management->get_config(instance, &entry, 1, &config_entries, &generation) == OCUDU_DAPP_OK_V1 &&
                   entry.value.value.boolean == 1,
               "config read failed") &&
       ok;
  entry.value.value.boolean = 0;
  ocudu_dapp_config_update_v1 update{};
  update.struct_size         = sizeof(update);
  update.nof_entries         = 1;
  update.entries             = &entry;
  update.expected_generation = generation;
  update.boundary            = OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1;
  ok = require(module.management->set_config(instance, &update, &generation) == OCUDU_DAPP_OK_V1,
               "config update failed") &&
       ok;
  ok = require(invoke(module.interfaces[0], worker, &prepared) == OCUDU_DAPP_BYPASS_V1,
               "disabled module did not bypass") &&
       ok;

  worker_api->destroy_worker_context(instance, worker);
  if (joint) {
    auto* scheduler_workers = static_cast<const ocudu_dapp_worker_api_v1*>(module.interfaces[1].interface_vtable);
    scheduler_workers->destroy_worker_context(instance, scheduler_worker);
  }
  ok = require(module.stop(instance) == OCUDU_DAPP_OK_V1, "stop failed") && ok;
  ok = require(module.quiesce(instance, 0) == OCUDU_DAPP_OK_V1, "quiesce failed") && ok;
  module.destroy(instance);
  ::dlclose(library);
  return ok ? 0 : 1;
}
