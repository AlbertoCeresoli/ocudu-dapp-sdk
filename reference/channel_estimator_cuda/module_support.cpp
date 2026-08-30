// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu_dapp_sdk/channel_estimator_cuda.h"

using namespace ocudu::dapp::reference;

namespace {

struct channel_estimator_state {
  channel_estimator_cuda_state cuda;
};

struct channel_estimator_lane_state {
  channel_estimator_cuda_worker_state cuda;
};

using channel_estimator_context = stateful_module_context<channel_estimator_state, channel_estimator_lane_state>;

ocudu_dapp_status_v1 invoke(void* context, const ocudu_dapp_channel_estimator_input_v1* input,
                            ocudu_dapp_channel_estimator_output_v1* output) noexcept
{
  if (input == nullptr || output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto status = channel_estimator_context::begin_invocation(context);
  auto*      state  = channel_estimator_context::shared_state(context);
  auto*      lane   = channel_estimator_context::worker_state(context);
  if (status != OCUDU_DAPP_OK_V1 || state == nullptr || lane == nullptr) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto invoked =
      guard_algorithm_call([&] { return estimate_channel_cuda(*input, *output, state->cuda, lane->cuda); });
  if (invoked == OCUDU_DAPP_OK_V1) {
    channel_estimator_context::observe_input_layers(
        context, static_cast<const ocudu_dapp_pusch_metadata_v1*>(input->pusch.metadata.data)->nof_layers);
  }
  return invoked;
}

const ocudu_dapp_channel_estimator_interface_v1 api = {channel_estimator_context::worker_api(), invoke, {}};
#ifndef OCUDU_REFERENCE_CLASS_A_MAX_LAYERS
#define OCUDU_REFERENCE_CLASS_A_MAX_LAYERS 2
#endif
#ifndef OCUDU_REFERENCE_CLASS_A_PROFILE_FLAGS
#define OCUDU_REFERENCE_CLASS_A_PROFILE_FLAGS OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1
#endif
const auto profile    = make_pusch_profile(OCUDU_REFERENCE_CLASS_A_MAX_LAYERS, OCUDU_REFERENCE_CLASS_A_PROFILE_FLAGS);
const auto descriptor = make_interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1,
                                       OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &api,
                                       OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1, &profile);

} // namespace

ocudu_dapp_status_v1 populate_channel_estimator_cuda_module(const ocudu_dapp_host_info_v1* host,
                                                            ocudu_dapp_module_v1*          output) noexcept
{
  return channel_estimator_context::populate(host, output, "org.ocudu.reference.channel-estimator.cuda", &descriptor);
}
