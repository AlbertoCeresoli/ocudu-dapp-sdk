// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/channel_estimator.h"
#include "ocudu_dapp_sdk/module_support.h"
#include <stdexcept>

#ifndef OCUDU_FAILURE_MODE
#error "OCUDU_FAILURE_MODE is required"
#endif

#ifndef OCUDU_FAILURE_GLOBAL_NAME
#error "OCUDU_FAILURE_GLOBAL_NAME is required"
#endif

namespace {

using namespace ocudu::dapp::reference;

ocudu_dapp_status_v1 invoke(void* context, const ocudu_dapp_channel_estimator_input_v1* input,
                            ocudu_dapp_channel_estimator_output_v1* output)
{
  if (context == nullptr || input == nullptr || output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto admitted = begin_invocation(context);
  if (admitted != OCUDU_DAPP_OK_V1) {
    return admitted;
  }
#if OCUDU_FAILURE_MODE == 1
  throw std::runtime_error("intentional failure-gallery Class-A exception");
#elif OCUDU_FAILURE_MODE == 2
  output->channel_estimates = {};
  return OCUDU_DAPP_OK_V1;
#elif OCUDU_FAILURE_MODE == 3
  return OCUDU_DAPP_DEADLINE_V1;
#else
#error "Unsupported OCUDU_FAILURE_MODE"
#endif
}

const ocudu_dapp_pusch_profile_v1               profile           = make_pusch_profile(1);
const ocudu_dapp_channel_estimator_interface_v1 channel_estimator = [] {
  ocudu_dapp_channel_estimator_interface_v1 result{};
  result.worker = worker_api();
  result.invoke = invoke;
  return result;
}();
const ocudu_dapp_interface_descriptor_v1 interface =
    make_interface(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1, OCUDU_DAPP_CLASS_A_INLINE_V1,
                   1U << (OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1 - 1), OCUDU_DAPP_THREADING_PER_WORKER_V1,
                   &channel_estimator, 0, &profile);

} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  return ocudu::dapp::reference::populate_module(host, output, OCUDU_FAILURE_GLOBAL_NAME, &interface);
}
