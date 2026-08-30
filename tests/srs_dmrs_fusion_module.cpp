// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu_dapp_sdk/module_support.h"
#include <array>
#include <atomic>

using namespace ocudu::dapp::reference;

namespace {

// Test-only stand-in for a future public DMRS observation interface. It is
// intentionally outside the public v1 interface-ID range.
constexpr uint32_t future_dmrs_interface_id = 0x7f010002U;

struct fusion_shared_state {
  std::atomic<uint64_t> latest_srs_sequence{};
  std::atomic<uint32_t> latest_srs_cell{};
};

struct fusion_lane_state {
  uint64_t nof_srs_observations{};
};

using fusion_context = stateful_module_context<fusion_shared_state, fusion_lane_state>;

ocudu_dapp_status_v1 publish_srs(void* context, const ocudu_dapp_srs_isac_input_v1* input) noexcept
{
  if (input == nullptr || input->source_sequence == 0 || input->source_sequence != input->invocation.input_generation) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto status = fusion_context::begin_invocation(context);
  if (status != OCUDU_DAPP_OK_V1) {
    return status;
  }
  auto* shared = fusion_context::shared_state(context);
  auto* lane   = fusion_context::worker_state(context);
  if (shared == nullptr || lane == nullptr) {
    return OCUDU_DAPP_INTERNAL_ERROR_V1;
  }
  ++lane->nof_srs_observations;
  shared->latest_srs_cell.store(input->invocation.cell_id, std::memory_order_relaxed);
  shared->latest_srs_sequence.store(input->source_sequence, std::memory_order_release);
  return OCUDU_DAPP_OK_V1;
}

const ocudu_dapp_srs_isac_interface_v1 srs_api = {fusion_context::worker_api(), publish_srs, {}};

// The host never calls this unknown interface in the current implementation;
// a non-null structurally stable vtable lets the fixture model a future API.
const ocudu_dapp_worker_api_v1 future_dmrs_api = fusion_context::worker_api();

const std::array<ocudu_dapp_interface_descriptor_v1, 2> descriptors = {
    make_interface(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1,
                   OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &srs_api),
    make_interface(future_dmrs_interface_id, OCUDU_DAPP_CLASS_C_OPEN_LOOP_V1, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                   OCUDU_DAPP_THREADING_PER_WORKER_V1, &future_dmrs_api)};

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  const auto status = fusion_context::populate(host, output, "org.ocudu.test.srs-dmrs-fusion.cpu", descriptors.data());
  if (status == OCUDU_DAPP_OK_V1) {
    output->nof_interfaces = descriptors.size();
    output->interfaces     = descriptors.data();
  }
  return status;
}
