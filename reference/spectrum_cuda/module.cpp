// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/spectrum_cuda.h"
#include <cmath>
#include <cstddef>

using namespace ocudu::dapp::reference;

namespace {

struct spectrum_cuda_shared_state {
};

struct spectrum_cuda_lane_state {
  spectrum_cuda_lane_state() noexcept : status(initialize_spectrum_cuda_worker_state(worker)) {}
  ~spectrum_cuda_lane_state() noexcept { release_spectrum_cuda_worker_state(worker); }

  spectrum_cuda_worker_state worker{};
  ocudu_dapp_status_v1       status{OCUDU_DAPP_RESOURCE_V1};
};

using spectrum_cuda_context = stateful_module_context<spectrum_cuda_shared_state, spectrum_cuda_lane_state>;

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
  return value->struct_size >= value_minimum && value->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 &&
                 value->rx_ports == input.uplink_resource_grid.dimensions[0] &&
                 value->symbols == input.uplink_resource_grid.dimensions[1] &&
                 value->subcarriers == input.uplink_resource_grid.dimensions[2] &&
                 std::isfinite(value->amplitude_scale) && value->amplitude_scale > 0.0F &&
                 (value->flags & ~OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1) == 0
             ? value
             : nullptr;
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
  const uint32_t value =
      (static_cast<uint32_t>(input.sfn) * slots_per_frame + input.slot + delta) % (1024U * slots_per_frame);
  output = {static_cast<uint16_t>(value / slots_per_frame), static_cast<uint16_t>(value % slots_per_frame),
            input.numerology, 0};
  return true;
}

ocudu_dapp_status_v1 publish(void* context, const ocudu_dapp_spectrum_input_v1* input) noexcept
{
  auto* lane = spectrum_cuda_context::worker_state(context);
  if (input == nullptr || lane == nullptr || lane->status != OCUDU_DAPP_OK_V1) {
    return lane == nullptr || input == nullptr ? OCUDU_DAPP_INVALID_INPUT_V1 : lane->status;
  }
  const auto* slot = metadata(*input);
  const bool  plan_present =
      slot != nullptr && (slot->flags & OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1) != 0;
  if (slot == nullptr || input->source_sequence == 0 || input->source_sequence != input->invocation.input_generation ||
      (plan_present ? slot->source_plan_generation == 0 ||
                          input->scheduled_re_mask.allocation_generation != slot->source_plan_generation
                    : slot->source_plan_generation != 0 || input->scheduled_re_mask.struct_size != 0)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto admitted = spectrum_cuda_context::begin_invocation(context);
  if (admitted != OCUDU_DAPP_OK_V1) {
    return admitted;
  }
  spectrum_cuda_result result{};
  const auto           status = guard_algorithm_call(
      [&] { return run_spectrum_radiometer_cuda(*input, slot->amplitude_scale, lane->worker, result); });
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  constexpr uint32_t metric_id_samples      = 0x10001U; // vendor-private namespace
  constexpr uint32_t metric_id_occupied_ppm = 0x10002U;
  spectrum_cuda_context::add_metric(context, metric_id_samples, result.samples);
  spectrum_cuda_context::add_metric(context, metric_id_occupied_ppm,
                                    result.nof_prbs == 0 ? 0 : result.occupied_prbs * 1'000'000ULL / result.nof_prbs);
  const auto* host = spectrum_cuda_context::host_api(context);
  if (host == nullptr || host->publish_prepared == nullptr) {
    return OCUDU_DAPP_OK_V1;
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
  output.prb_confidence.domain                = OCUDU_DAPP_MEM_HOST_PINNED_V1;
  output.prb_confidence.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  output.prb_confidence.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
  output.prb_confidence.rank                  = 1;
  output.prb_confidence.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  output.prb_confidence.allocation_id         = input->source_sequence;
  output.prb_confidence.allocation_generation = input->invocation.input_generation;
  output.prb_confidence.native_handle         = reinterpret_cast<uintptr_t>(result.confidence);
  output.prb_confidence.byte_length           = result.nof_prbs;
  output.prb_confidence.dimensions[0]         = result.nof_prbs;
  output.prb_confidence.byte_strides[0]       = sizeof(uint8_t);
  output.prb_confidence.layout_id             = OCUDU_DAPP_LAYOUT_PRB_INTERFERENCE_U8_V1;
  output.prb_confidence.layout_version        = 1;
  output.prb_confidence.lease_token           = input->source_sequence;
  output.source_grid_sequence                 = input->source_sequence;
  output.source_plan_generation               = slot->source_plan_generation;
  output.nof_prbs                             = result.nof_prbs;
  const auto published = host->publish_prepared(host->host_context, OCUDU_DAPP_SPECTRUM_OUTPUT_INTERFERENCE_MAP_V1,
                                                OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1, &output, sizeof(output));
  if (published == OCUDU_DAPP_OK_V1) {
    constexpr uint32_t metric_id_intents_published = 0x10003U;
    spectrum_cuda_context::add_metric(context, metric_id_intents_published, 1);
  }
  return published;
}

const ocudu_dapp_spectrum_interface_v1 api = {spectrum_cuda_context::worker_api(), publish, nullptr, {}};
const auto descriptor = make_interface(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                       OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &api,
                                       OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1);

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  return spectrum_cuda_context::populate(host, output, "org.ocudu.reference.spectrum.cuda", &descriptor);
}
