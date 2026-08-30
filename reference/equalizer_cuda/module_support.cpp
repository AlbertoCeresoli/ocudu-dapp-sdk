// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu_dapp_sdk/equalizer_cuda.h"
#include "ocudu_dapp_sdk/package_config.h"
#include <new>

using namespace ocudu::dapp::package_config;
using namespace ocudu::dapp::reference;

namespace {

struct equalizer_state {
  equalizer_cuda_state cuda;
};

struct equalizer_lane_state {
  explicit equalizer_lane_state(uint32_t)
  {
    if (initialize_equalizer_cuda_worker_state(cuda) != OCUDU_DAPP_OK_V1) {
      throw std::bad_alloc();
    }
  }

  ~equalizer_lane_state() noexcept { release_equalizer_cuda_worker_state(cuda); }

  equalizer_cuda_worker_state cuda;
};

using equalizer_context = stateful_module_context<equalizer_state, equalizer_lane_state>;

ocudu_dapp_status_v1 invoke(void* context, const ocudu_dapp_equalizer_input_v1* input,
                            ocudu_dapp_equalizer_output_v1* output) noexcept
{
  if (input == nullptr || output == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto status = equalizer_context::begin_invocation(context);
  auto*      state  = equalizer_context::shared_state(context);
  auto*      lane   = equalizer_context::worker_state(context);
  if (status != OCUDU_DAPP_OK_V1 || state == nullptr || lane == nullptr) {
    return status != OCUDU_DAPP_OK_V1 ? status : OCUDU_DAPP_INVALID_INPUT_V1;
  }
  const auto invoked = guard_algorithm_call([&] { return equalize_cuda(*input, *output, state->cuda, lane->cuda); });
  if (invoked == OCUDU_DAPP_OK_V1) {
    equalizer_context::observe_input_layers(
        context, static_cast<const ocudu_dapp_pusch_metadata_v1*>(input->pusch.metadata.data)->nof_layers);
  }
  return invoked;
}

const ocudu_dapp_equalizer_interface_v1 api     = {equalizer_context::worker_api(), invoke, {}};
const auto                              profile = make_pusch_profile(2, OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1);
const auto                              descriptor =
    make_interface(OCUDU_DAPP_INTERFACE_EQUALIZER_V1, OCUDU_DAPP_CLASS_A_INLINE_V1, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                   OCUDU_DAPP_THREADING_PER_WORKER_V1, &api, OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1, &profile);

// Open ref demonstrates the same package_config core as commercial packages.
// Product packages add keys beside instance_enabled_key() — see examples/vendor_min
// and docs/package_config.md. Schema must outlive make_config_management.
const config_key    kKeys[] = {instance_enabled_key()};
const config_schema kSchema = {kKeys, 1, 0, nullptr, nullptr, nullptr};

const ocudu_dapp_management_v1& equalizer_ref_management_api() noexcept
{
  static const ocudu_dapp_management_v1& api = make_config_management(kSchema);
  return api;
}

} // namespace

ocudu_dapp_status_v1 populate_equalizer_cuda_module(const ocudu_dapp_host_info_v1* host,
                                                    ocudu_dapp_module_v1*          output) noexcept
{
  const auto status = equalizer_context::populate(host, output, "org.ocudu.reference.equalizer.cuda", &descriptor);
  if (status == OCUDU_DAPP_OK_V1 && output != nullptr) {
    output->management = &equalizer_ref_management_api();
  }
  return status;
}
