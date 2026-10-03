// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/coordination.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu_dapp_sdk/class_c_algorithms.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/reference_spectrum_policy.h"
#include <algorithm>
#include <array>

using namespace ocudu::dapp::reference;

namespace {

struct spectrum_model_state {
  spectrum_quiet_authority quiet_authority;
};

struct spectrum_lane_state {
  uint64_t                                                                             latest_plan_generation{};
  uint32_t                                                                             nof_plan_spans{};
  uint16_t                                                                             cell_nof_prbs{};
  bool                                                                                 plan_complete{};
  std::array<ocudu_dapp_ul_resource_span_v1, OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1> plan_spans{};
};

using spectrum_context = stateful_module_context<spectrum_model_state, spectrum_lane_state>;

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

ocudu_dapp_status_v1 publish(void* context, const ocudu_dapp_spectrum_input_v1* input) noexcept
{
  if (input == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  strided_complex_view view{};
  const auto*          slot = metadata(*input);
  if (!make_host_complex_view(input->uplink_resource_grid, view) || slot == nullptr ||
      (slot->flags & ~OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1) != 0) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  expected_ul_use_view expected_use{};
  const bool           plan_present = (slot->flags & OCUDU_DAPP_SPECTRUM_FLAG_EXPECTED_UL_USE_COMPLETE_V1) != 0;
  if ((plan_present &&
       !make_host_expected_ul_use_view(input->scheduled_re_mask, slot->source_plan_generation, expected_use)) ||
      (!plan_present && slot->source_plan_generation != 0)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  // Vendor-private numeric metric ids (the push-path uint32 namespace is
  // unregistered — see docs/known_limitations.md). Prefer the string-named
  // describe_metrics/read_metrics catalog for anything a consumer must interpret.
  constexpr uint32_t metric_id_samples           = 0x10001U;
  constexpr uint32_t metric_id_occupied_ppm      = 0x10002U;
  constexpr uint32_t metric_id_intents_published = 0x10003U;

  const auto status = spectrum_context::begin_invocation(context);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  if (input->source_sequence == 0 || input->source_sequence != input->invocation.input_generation) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  std::array<uint8_t, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1> confidence{};
  uint32_t                                             nof_prbs = 0;
  spectrum_radiometer_result                           result{};
  const auto                                           detected = guard_algorithm_call([&] {
    const bool ok = plan_present ? run_spectrum_prb_radiometer(view, expected_use, slot->amplitude_scale, 1.0, 12,
                                                                                                         confidence.data(), confidence.size(), nof_prbs, &result)
                                                                           : run_spectrum_prb_radiometer(view, slot->amplitude_scale, 1.0, 12, confidence.data(),
                                                                                                         confidence.size(), nof_prbs, &result);
    return ok ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
  });
  if (detected != OCUDU_DAPP_OK_V1) {
    return detected;
  }
  spectrum_context::add_metric(context, metric_id_samples, result.samples);
  spectrum_context::add_metric(context, metric_id_occupied_ppm,
                               static_cast<uint64_t>(result.occupied_fraction * 1'000'000.0));
  const auto* host = spectrum_context::host_api(context);
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
  if (published != OCUDU_DAPP_OK_V1) {
    return published;
  }
  spectrum_context::add_metric(context, metric_id_intents_published, 1);
  const auto occupied =
      std::find_if(confidence.begin(), confidence.begin() + nof_prbs, [](uint8_t value) { return value != 0; });
  if (occupied == confidence.begin() + nof_prbs || host->monotonic_ns == nullptr) {
    return OCUDU_DAPP_OK_V1;
  }
  auto*          shared = spectrum_context::shared_state(context);
  const uint64_t now    = host->monotonic_ns(host->host_context);
  if (shared == nullptr || !shared->quiet_authority.try_admit(input->invocation.cell_id, now)) {
    return OCUDU_DAPP_OK_V1;
  }
  ocudu_dapp_quiet_period_intent_v1 intent{};
  intent.struct_size   = sizeof(intent);
  intent.abi_major     = OCUDU_DAPP_ABI_MAJOR_V1;
  intent.abi_minor     = OCUDU_DAPP_ABI_MINOR_V1;
  intent.request_id[0] = input->source_sequence;
  intent.request_id[1] = UINT64_C(0x5155494554);
  if (!add_slots(input->invocation.radio_time, 4, intent.earliest_start)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  intent.latest_start        = intent.earliest_start;
  intent.duration_slots      = 1;
  intent.repeat_count        = 1;
  intent.first_prb           = static_cast<uint16_t>(occupied - confidence.begin());
  intent.nof_prbs            = 1;
  intent.requested_level     = OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
  intent.budget_class        = 1;
  intent.confidence_q16      = 1U << 15U;
  intent.expiry_monotonic_ns = spectrum_quiet_authority::expiry(now);
  return host->publish_prepared(host->host_context, OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1,
                                OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1, &intent, sizeof(intent));
}

ocudu_dapp_status_v1 publish_plan(void* context, const ocudu_dapp_ul_resource_plan_v1* plan) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_ul_resource_plan_v1, reserved);
  if (plan == nullptr || plan->struct_size < minimum || plan->abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      plan->plan_generation == 0 || plan->nof_spans > OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1 ||
      (plan->nof_spans != 0 && plan->spans == nullptr) || plan->cell_nof_prbs == 0 ||
      plan->cell_nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 ||
      (plan->flags & ~OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1) != 0) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto status = spectrum_context::begin_invocation(context);
  auto*      lane   = spectrum_context::worker_state(context);
  if (status != OCUDU_DAPP_OK_V1 || lane == nullptr) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_INTERNAL_ERROR_V1;
  }
  for (uint32_t index = 0; index != plan->nof_spans; ++index) {
    const auto& span = plan->spans[index];
    if (span.struct_size < offsetof(ocudu_dapp_ul_resource_span_v1, reserved) || span.symbol_count == 0 ||
        span.symbol_start + span.symbol_count > 14 || span.nof_prbs == 0 ||
        static_cast<uint32_t>(span.first_prb) + span.nof_prbs > plan->cell_nof_prbs ||
        span.use_category > OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
  }
  lane->latest_plan_generation = plan->plan_generation;
  lane->nof_plan_spans         = plan->nof_spans;
  lane->cell_nof_prbs          = plan->cell_nof_prbs;
  lane->plan_complete          = (plan->flags & OCUDU_DAPP_UL_RESOURCE_PLAN_COMPLETE_V1) != 0;
  if (plan->nof_spans != 0) {
    std::copy_n(plan->spans, plan->nof_spans, lane->plan_spans.begin());
  }
  return OCUDU_DAPP_OK_V1;
}

const ocudu_dapp_spectrum_interface_v1 api = {spectrum_context::worker_api(), publish, publish_plan, {}};
const auto                             descriptor =
    make_interface(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                   OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 | OCUDU_DAPP_PLACEMENT_OUT_OF_PROCESS_V1,
                   OCUDU_DAPP_THREADING_PER_WORKER_V1, &api,
                   OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1 | OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1 |
                       OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1);
} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  return spectrum_context::populate(host, output, "org.ocudu.reference.spectrum.cpu", &descriptor);
}
