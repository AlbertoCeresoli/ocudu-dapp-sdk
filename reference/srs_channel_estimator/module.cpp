// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/reference_srs_channel_estimator.h"

using namespace ocudu::dapp::reference;

namespace {
ocudu_dapp_status_v1 invoke(void* context, const ocudu_dapp_srs_channel_estimator_input_v1* input,
                            ocudu_dapp_srs_channel_estimator_output_v1* output) noexcept
{
  if (input == nullptr || output == nullptr)
    return OCUDU_DAPP_INVALID_INPUT_V1;
  const auto status = begin_invocation(context);
  if (status != OCUDU_DAPP_OK_V1)
    return status;
  const auto invoked = guard_algorithm_call([&] { return estimate_srs_channel(*input, *output); });
  // The SRS transmit ports are this interface's layers in the input-envelope metric.
  if (invoked == OCUDU_DAPP_OK_V1)
    observe_input_layers(
        context,
        static_cast<const ocudu_dapp_srs_channel_estimator_metadata_v1*>(input->srs.metadata.data)->nof_tx_ports);
  return invoked;
}
const ocudu_dapp_srs_channel_estimator_interface_v1 api     = {worker_api(), invoke, {}};
const auto                                          profile = make_srs_profile();
const auto                                          descriptor =
    make_interface(OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1,
                   OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &api, 0, &profile);
} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  return populate_module(host, output, "org.ocudu.reference.srs-channel-estimator.cpu", &descriptor);
}
