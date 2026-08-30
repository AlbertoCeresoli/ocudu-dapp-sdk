// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Lifecycle + invoke — commercial packages keep product state here.
//
// Pattern:
//   - stateful_module_context for instance + per-worker CUDA state
//   - guard_algorithm_call so exceptions never cross the C ABI
//   - attach package_config management after populate (E3 / dappctl knobs)

#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu_dapp_sdk/equalizer_cuda.h"
#include <new>

namespace ocudu::dapp::example {
ocudu_dapp_status_v1 equalize(const ocudu_dapp_equalizer_input_v1& input, ocudu_dapp_equalizer_output_v1& output,
                              const reference::equalizer_cuda_state&  state,
                              reference::equalizer_cuda_worker_state& worker);
const ocudu_dapp_management_v1& example_eq_management_api() noexcept;
} // namespace ocudu::dapp::example

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
  const auto invoked =
      guard_algorithm_call([&] { return ocudu::dapp::example::equalize(*input, *output, state->cuda, lane->cuda); });
  if (invoked == OCUDU_DAPP_OK_V1 && input->pusch.metadata.data != nullptr) {
    const auto* pusch = static_cast<const ocudu_dapp_pusch_metadata_v1*>(input->pusch.metadata.data);
    if (pusch != nullptr) {
      equalizer_context::observe_input_layers(context, pusch->nof_layers);
    }
  }
  return invoked;
}

const ocudu_dapp_equalizer_interface_v1 api     = {equalizer_context::worker_api(), invoke, {}};
const auto                              profile = make_pusch_profile(2, OCUDU_DAPP_PUSCH_PROFILE_DC_OVERLAP_V1);
const auto                              descriptor =
    make_interface(OCUDU_DAPP_INTERFACE_EQUALIZER_V1, OCUDU_DAPP_CLASS_A_INLINE_V1, OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1,
                   OCUDU_DAPP_THREADING_PER_WORKER_V1, &api, OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1, &profile);

} // namespace

ocudu_dapp_status_v1 populate_example_eq_cuda_module(const ocudu_dapp_host_info_v1* host,
                                                     ocudu_dapp_module_v1*          output) noexcept
{
  // Identity strings must match the package manifest (hash-bound admission).
  const auto status = equalizer_context::populate(host, output, "org.ocudu.example.vendor-min.eq.cuda", "OCUDU example",
                                                  "1", &descriptor);
  if (status == OCUDU_DAPP_OK_V1 && output != nullptr) {
    output->management = &ocudu::dapp::example::example_eq_management_api();
  }
  return status;
}
