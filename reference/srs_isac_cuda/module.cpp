// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/srs_isac_cuda.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>

using namespace ocudu::dapp::reference;

namespace {

struct srs_isac_cuda_shared_state {
};

struct srs_isac_cuda_lane_state {
  srs_isac_cuda_lane_state() noexcept : status(initialize_srs_isac_cuda_worker_state(worker)) {}
  ~srs_isac_cuda_lane_state() noexcept { release_srs_isac_cuda_worker_state(worker); }

  srs_isac_cuda_worker_state worker{};
  ocudu_dapp_status_v1       status{OCUDU_DAPP_RESOURCE_V1};
};

using srs_isac_cuda_context = stateful_module_context<srs_isac_cuda_shared_state, srs_isac_cuda_lane_state>;

const ocudu_dapp_srs_isac_metadata_v1* metadata(const ocudu_dapp_srs_isac_input_v1& input) noexcept
{
  constexpr uint32_t wrapper_minimum =
      offsetof(ocudu_dapp_typed_metadata_v1, metadata) + sizeof(ocudu_dapp_blob_view_v1);
  constexpr uint32_t value_minimum = offsetof(ocudu_dapp_srs_isac_metadata_v1, reserved);
  const auto&        wrapper       = input.srs_metadata;
  if (wrapper.struct_size < wrapper_minimum || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_SRS_ISAC_V1 || wrapper.metadata_schema_version != 1 ||
      wrapper.metadata.data == nullptr || wrapper.metadata.size < value_minimum || wrapper.metadata.reserved != 0) {
    return nullptr;
  }
  const auto* value = static_cast<const ocudu_dapp_srs_isac_metadata_v1*>(wrapper.metadata.data);
  return value->struct_size >= value_minimum && value->abi_major == OCUDU_DAPP_ABI_MAJOR_V1 && value->rx_ports != 0 &&
                 value->rx_ports <= 4 && value->tx_ports != 0 && value->tx_ports <= 4 && value->frequency_points != 0 &&
                 (value->comb_size == 2 || value->comb_size == 4) && value->comb_offset < value->comb_size &&
                 value->subcarrier_spacing_hz != 0 && std::isfinite(value->noise_variance) &&
                 value->noise_variance >= 0.0F && std::isfinite(value->rsrp_db) && std::isfinite(value->epre_db) &&
                 input.channel_estimates.dimensions[0] == value->rx_ports &&
                 input.channel_estimates.dimensions[1] == value->tx_ports &&
                 input.channel_estimates.dimensions[2] == value->frequency_points && input.source_sequence != 0 &&
                 input.source_sequence == input.invocation.input_generation &&
                 std::all_of(std::begin(value->reserved), std::end(value->reserved),
                             [](uint64_t item) { return item == 0; })
             ? value
             : nullptr;
}

ocudu_dapp_status_v1 publish(void* context, const ocudu_dapp_srs_isac_input_v1* input) noexcept
{
  auto* lane = srs_isac_cuda_context::worker_state(context);
  if (input == nullptr || lane == nullptr || lane->status != OCUDU_DAPP_OK_V1) {
    return input == nullptr || lane == nullptr ? OCUDU_DAPP_INVALID_INPUT_V1 : lane->status;
  }
  const auto* source_metadata = metadata(*input);
  if (source_metadata == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto admitted = srs_isac_cuda_context::begin_invocation(context);
  if (admitted != OCUDU_DAPP_OK_V1) {
    return admitted;
  }
  srs_cir_analysis analysis{};
  const auto       status = guard_algorithm_call(
      [&] { return run_srs_cir_cuda(*input, source_metadata->noise_variance, lane->worker, analysis); });
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  constexpr uint32_t metric_id_samples        = 0x20001U; // vendor-private namespace
  constexpr uint32_t metric_id_peak_delay_bin = 0x20002U;
  srs_isac_cuda_context::add_metric(context, metric_id_samples, analysis.samples);
  srs_isac_cuda_context::add_metric(context, metric_id_peak_delay_bin, analysis.peak_delay_bin);
  ocudu_dapp_srs_isac_cir_preview_v1 preview{};
  if (!make_srs_cir_preview(analysis, source_metadata->noise_variance, preview)) {
    return OCUDU_DAPP_INVALID_OUTPUT_V1;
  }
  preview.context.struct_size     = sizeof(preview.context);
  preview.context.schema_id       = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
  preview.context.schema_version  = 1;
  preview.context.source_sequence = input->source_sequence;
  preview.context.cell_id         = input->invocation.cell_id;
  preview.context.confidence_q16  = 1U << 16U;
  preview.context.created         = input->invocation.radio_time;
  preview.context.valid_from      = input->invocation.radio_time;
  preview.context.valid_until     = input->invocation.radio_time;
  preview.source_srs_sequence     = input->source_sequence;
  preview.srs_resource_id         = source_metadata->srs_resource_id;
  preview.srs_config_id           = source_metadata->srs_config_id;
  const auto* host                = srs_isac_cuda_context::host_api(context);
  if (host == nullptr || host->publish_prepared == nullptr) {
    return OCUDU_DAPP_OK_V1;
  }
  const auto published = host->publish_prepared(host->host_context, OCUDU_DAPP_SRS_ISAC_OUTPUT_CIR_PREVIEW_V1,
                                                OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1, &preview, sizeof(preview));
  if (published == OCUDU_DAPP_OK_V1) {
    constexpr uint32_t metric_id_previews_published = 0x20003U;
    srs_isac_cuda_context::add_metric(context, metric_id_previews_published, 1);
  }
  return published == OCUDU_DAPP_BYPASS_V1 || published == OCUDU_DAPP_UNSUPPORTED_V1 ? OCUDU_DAPP_OK_V1 : published;
}

const ocudu_dapp_srs_isac_interface_v1 api = {srs_isac_cuda_context::worker_api(), publish, {}};
const auto descriptor = make_interface(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                                       OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &api);

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  return srs_isac_cuda_context::populate(host, output, "org.ocudu.reference.srs-isac.cuda", &descriptor);
}
