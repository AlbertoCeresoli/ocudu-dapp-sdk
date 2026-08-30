// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu_dapp_sdk/class_c_algorithms.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/reference_scheduler_pipeline.h"
#include <array>
#include <atomic>
#include <cstddef>

using namespace ocudu::dapp::reference;

namespace {

struct joint_snapshot {
  uint64_t source_sequence{};
  uint32_t cell_id{};
  uint32_t nof_prbs{};
};

struct joint_shared_state {
  scheduler_reference_config config;
  std::atomic_flag           writer = ATOMIC_FLAG_INIT;
  std::atomic<uint64_t>      sequence{};
  std::atomic<uint64_t>      source_sequence{};
  std::atomic<uint32_t>      cell_id{};
  std::atomic<uint32_t>      nof_prbs{};

  joint_shared_state() { config.enable_prb_avoidance = true; }

  void publish(joint_snapshot value) noexcept
  {
    if (writer.test_and_set(std::memory_order_acquire)) {
      return;
    }
    const uint64_t current = sequence.load(std::memory_order_relaxed);
    sequence.store(current + 1, std::memory_order_release);
    source_sequence.store(value.source_sequence, std::memory_order_relaxed);
    cell_id.store(value.cell_id, std::memory_order_relaxed);
    nof_prbs.store(value.nof_prbs, std::memory_order_relaxed);
    sequence.store(current + 2, std::memory_order_release);
    writer.clear(std::memory_order_release);
  }

  bool read(joint_snapshot& value) const noexcept
  {
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
      const uint64_t before = sequence.load(std::memory_order_acquire);
      if (before == 0 || (before & 1U) != 0) {
        continue;
      }
      value.source_sequence = source_sequence.load(std::memory_order_relaxed);
      value.cell_id         = cell_id.load(std::memory_order_relaxed);
      value.nof_prbs        = nof_prbs.load(std::memory_order_relaxed);
      if (sequence.load(std::memory_order_acquire) == before) {
        return true;
      }
    }
    return false;
  }
};

struct joint_lane_state {
  scheduler_reference_state scheduler;
};

using joint_context = stateful_module_context<joint_shared_state, joint_lane_state>;

const ocudu_dapp_spectrum_slot_metadata_v1* metadata(const ocudu_dapp_spectrum_input_v1& input) noexcept
{
  constexpr uint32_t wrapper_minimum =
      offsetof(ocudu_dapp_typed_metadata_v1, metadata) + sizeof(ocudu_dapp_blob_view_v1);
  constexpr uint32_t value_minimum = offsetof(ocudu_dapp_spectrum_slot_metadata_v1, reserved);
  const auto&        wrapper       = input.slot_metadata;
  if (wrapper.struct_size < wrapper_minimum || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_SPECTRUM_SLOT_V1 || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < value_minimum) {
    return nullptr;
  }
  const auto* value = static_cast<const ocudu_dapp_spectrum_slot_metadata_v1*>(wrapper.metadata.data);
  return value->struct_size >= value_minimum && value->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 ? value : nullptr;
}

bool add_slots(const ocudu_dapp_radio_time_v1& input, uint32_t delta, ocudu_dapp_radio_time_v1& output) noexcept
{
  if (input.numerology > 4 || input.sfn >= 1024 || input.reserved != 0) {
    return false;
  }
  const uint32_t slots_per_frame = 10U << input.numerology;
  if (input.slot >= slots_per_frame) {
    return false;
  }
  const uint32_t cycle = 1024U * slots_per_frame;
  const uint32_t value = (static_cast<uint32_t>(input.sfn) * slots_per_frame + input.slot + delta) % cycle;
  output = {static_cast<uint16_t>(value / slots_per_frame), static_cast<uint16_t>(value % slots_per_frame),
            input.numerology, 0};
  return true;
}

ocudu_dapp_status_v1 publish_spectrum(void* context, const ocudu_dapp_spectrum_input_v1* input) noexcept
{
  if (input == nullptr || input->source_sequence == 0 || input->source_sequence != input->invocation.input_generation) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto status = joint_context::begin_invocation(context);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  if (input->source_sequence == UINT64_MAX) {
    return OCUDU_DAPP_EXCEPTION_V1;
  }
  strided_complex_view view{};
  const auto*          slot = metadata(*input);
  if (slot == nullptr || !make_host_complex_view(input->uplink_resource_grid, view)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  std::array<uint8_t, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1> confidence{};
  uint32_t                                             nof_prbs = 0;
  if (!run_spectrum_prb_radiometer(view, slot->amplitude_scale, 1.0, 12, confidence.data(), confidence.size(),
                                   nof_prbs)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto* host = joint_context::host_api(context);
  if (host == nullptr || host->publish_prepared == nullptr) {
    return OCUDU_DAPP_BYPASS_V1;
  }

  ocudu_dapp_spectrum_interference_map_v1 output{};
  output.struct_size             = sizeof(output);
  output.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  output.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
  output.context.struct_size     = sizeof(output.context);
  output.context.schema_id       = OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1;
  output.context.schema_version  = 1;
  output.context.source_sequence = input->source_sequence;
  output.context.cell_id         = input->invocation.cell_id;
  output.context.confidence_q16  = 1U << 16U;
  output.context.created         = input->invocation.radio_time;
  if (!add_slots(output.context.created, 1, output.context.valid_from) ||
      !add_slots(output.context.created, 4, output.context.valid_until)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  output.prb_confidence.struct_size           = sizeof(output.prb_confidence);
  output.prb_confidence.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  output.prb_confidence.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  output.prb_confidence.domain                = OCUDU_DAPP_MEM_HOST_V1;
  output.prb_confidence.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  output.prb_confidence.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
  output.prb_confidence.rank                  = 1;
  output.prb_confidence.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  output.prb_confidence.allocation_id         = input->source_sequence;
  output.prb_confidence.allocation_generation = input->invocation.input_generation;
  output.prb_confidence.native_handle         = reinterpret_cast<uintptr_t>(confidence.data());
  output.prb_confidence.byte_length           = nof_prbs;
  output.prb_confidence.dimensions[0]         = nof_prbs;
  output.prb_confidence.byte_strides[0]       = sizeof(uint8_t);
  output.prb_confidence.layout_id             = OCUDU_DAPP_LAYOUT_PRB_INTERFERENCE_U8_V1;
  output.prb_confidence.layout_version        = 1;
  output.prb_confidence.lease_token           = input->source_sequence;
  output.source_grid_sequence                 = input->source_sequence;
  output.source_plan_generation               = slot->source_plan_generation;
  output.nof_prbs                             = nof_prbs;
  const auto published = host->publish_prepared(host->host_context, OCUDU_DAPP_SPECTRUM_OUTPUT_INTERFERENCE_MAP_V1,
                                                OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1, &output, sizeof(output));
  if (published == OCUDU_DAPP_OK_V1) {
    auto* shared = joint_context::shared_state(context);
    if (shared != nullptr) {
      shared->publish({input->source_sequence, input->invocation.cell_id, nof_prbs});
    }
  }
  return published;
}

ocudu_dapp_status_v1 invoke_scheduler(void* context, const ocudu_dapp_scheduler_input_v1* input,
                                      ocudu_dapp_scheduler_output_v1* output) noexcept
{
  if (input == nullptr || output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto status = joint_context::begin_invocation(context);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  auto* shared = joint_context::shared_state(context);
  auto* lane   = joint_context::worker_state(context);
  if (shared == nullptr || lane == nullptr) {
    return OCUDU_DAPP_INTERNAL_ERROR_V1;
  }
  joint_snapshot latest{};
  if (!shared->read(latest) || input->interference_context_present != 1 ||
      input->interference_context.source_sequence != latest.source_sequence ||
      input->interference_context.cell_id != latest.cell_id || input->invocation.cell_id != latest.cell_id ||
      input->cell_nof_prbs != latest.nof_prbs) {
    return OCUDU_DAPP_BYPASS_V1;
  }
  return rank_scheduler_candidates(*input, *output, shared->config, lane->scheduler);
}

const ocudu_dapp_spectrum_interface_v1  spectrum_api  = {joint_context::worker_api(), publish_spectrum, nullptr, {}};
const ocudu_dapp_scheduler_interface_v1 scheduler_api = {joint_context::worker_api(), invoke_scheduler, {}};
const std::array<ocudu_dapp_interface_descriptor_v1, 2> descriptors = {
    make_interface(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                   OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &spectrum_api,
                   OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1),
    make_interface(OCUDU_DAPP_INTERFACE_SCHEDULER_V1, OCUDU_DAPP_CLASS_B_DEADLINE_V1,
                   OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &scheduler_api)};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  const auto status =
      joint_context::populate(host, output, "org.ocudu.test.joint-spectrum-scheduler.cpu", descriptors.data());
  if (status == OCUDU_DAPP_OK_V1) {
    output->nof_interfaces = descriptors.size();
    output->interfaces     = descriptors.data();
  }
  return status;
}
