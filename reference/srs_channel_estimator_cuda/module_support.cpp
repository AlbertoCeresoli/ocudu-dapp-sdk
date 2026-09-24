// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include "ocudu_dapp_sdk/srs_channel_estimator_cuda.h"

using namespace ocudu::dapp::reference;

namespace {

using srs_channel_estimator_context =
    stateful_module_context<srs_channel_estimator_cuda_state, srs_channel_estimator_cuda_worker_state>;

ocudu_dapp_status_v1 invoke(void* context, const ocudu_dapp_srs_channel_estimator_input_v1* input,
                            ocudu_dapp_srs_channel_estimator_output_v1* output) noexcept
{
  if (input == nullptr || output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto status = srs_channel_estimator_context::begin_invocation(context);
  auto*      state  = srs_channel_estimator_context::shared_state(context);
  auto*      lane   = srs_channel_estimator_context::worker_state(context);
  if (status != OCUDU_DAPP_OK_V1 || state == nullptr || lane == nullptr) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_INVALID_INPUT_V1;
  }
  return guard_algorithm_call([&] { return estimate_srs_channel_cuda(*input, *output, *state, *lane); });
}

const ocudu_dapp_srs_channel_estimator_interface_v1 api     = {srs_channel_estimator_context::worker_api(), invoke, {}};
const auto                                          profile = make_srs_profile();
const auto descriptor = make_interface(OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1,
                                       OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &api,
                                       OCUDU_DAPP_SRS_CAP_RESIDENT_ACCELERATOR_V1, &profile);

} // namespace

ocudu_dapp_status_v1 populate_srs_channel_estimator_cuda_module(const ocudu_dapp_host_info_v1* host,
                                                                ocudu_dapp_module_v1*          output) noexcept
{
  return srs_channel_estimator_context::populate(host, output, "org.ocudu.reference.srs-channel-estimator.cuda",
                                                 &descriptor);
}
