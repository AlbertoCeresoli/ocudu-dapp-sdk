// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_scheduler_pipeline.h"
#include <array>
#include <gtest/gtest.h>

using namespace ocudu::dapp::reference;

namespace {

struct fixture {
  std::array<ocudu_dapp_scheduler_candidate_v1, 3> candidates{};
  std::array<ocudu_dapp_scheduler_intent_v1, 3>    intents{};
  ocudu_dapp_scheduler_input_v1                    input{};
  ocudu_dapp_scheduler_output_v1                   output{};
  std::array<uint8_t, 8>                           interference{};
  std::array<uint8_t, 8>                           avoidance{};

  fixture()
  {
    for (uint32_t index = 0; index != candidates.size(); ++index) {
      candidates[index].struct_size  = sizeof(candidates[index]);
      candidates[index].candidate_id = index;
      candidates[index].ue_index     = 10 + index;
    }
    candidates[0].pending_bytes = 100;
    candidates[1].pending_bytes = 1'000;
    candidates[2].pending_bytes = 1;
    candidates[2].flags         = OCUDU_DAPP_SCHEDULER_CANDIDATE_PENDING_SR_V1;

    input.direction            = OCUDU_DAPP_SCHEDULER_UL_V1;
    input.nof_candidates       = candidates.size();
    input.candidate_generation = 7;
    input.candidates           = candidates.data();
    output.struct_size         = sizeof(output);
    output.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
    output.abi_minor           = OCUDU_DAPP_ABI_MINOR_V1;
    output.intents             = intents.data();
    output.intent_capacity     = intents.size();
  }

  void add_interference_context()
  {
    input.invocation.cell_id                  = 4;
    input.cell_nof_prbs                       = interference.size();
    input.interference_context_present        = 1;
    input.allowed_output_flags                = OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1;
    input.interference_context.struct_size    = sizeof(input.interference_context);
    input.interference_context.schema_id      = OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1;
    input.interference_context.schema_version = 1;
    input.interference_context.cell_id        = input.invocation.cell_id;
    input.optional_interference_mask          = make_mask(interference.data(), OCUDU_DAPP_ACCESS_READ_V1);
    output.avoid_prb_mask                     = make_mask(avoidance.data(), OCUDU_DAPP_ACCESS_WRITE_V1);
  }

private:
  ocudu_dapp_tensor_view_v1 make_mask(uint8_t* data, uint32_t access)
  {
    ocudu_dapp_tensor_view_v1 mask{};
    mask.struct_size           = sizeof(mask);
    mask.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
    mask.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
    mask.domain                = OCUDU_DAPP_MEM_HOST_V1;
    mask.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
    mask.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
    mask.rank                  = 1;
    mask.access                = access | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
    mask.allocation_id         = access;
    mask.allocation_generation = 1;
    mask.native_handle         = reinterpret_cast<uintptr_t>(data);
    mask.byte_length           = interference.size();
    mask.dimensions[0]         = interference.size();
    mask.byte_strides[0]       = sizeof(uint8_t);
    mask.layout_id             = OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1;
    mask.layout_version        = 1;
    mask.lease_token           = 1;
    return mask;
  }
};

} // namespace

TEST(reference_scheduler_algorithm, ranks_bounded_candidates_and_publishes_complete_intent)
{
  fixture                    data;
  scheduler_reference_config config;
  scheduler_reference_state  state;
  ASSERT_EQ(rank_scheduler_candidates(data.input, data.output, config, state), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(data.output.nof_intents, data.candidates.size());
  EXPECT_EQ(data.output.decision_generation, data.input.candidate_generation);
  EXPECT_GT(data.intents[2].priority, data.intents[1].priority);
  EXPECT_GT(data.intents[1].priority, data.intents[0].priority);
  EXPECT_EQ(state.previous_winner, data.candidates[2].ue_index);
  EXPECT_EQ(state.invocations, 1U);
  for (uint32_t index = 0; index != data.intents.size(); ++index) {
    EXPECT_EQ(data.intents[index].candidate_id, index);
    EXPECT_EQ(data.intents[index].flags, OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1);
  }
}

TEST(reference_scheduler_algorithm, keeps_history_per_worker_without_unbounded_state)
{
  fixture                    data;
  scheduler_reference_config config;
  config.scheduling_request_bonus  = 0.0;
  data.candidates[0].pending_bytes = 100;
  data.candidates[1].pending_bytes = 100;
  data.candidates[2].pending_bytes = 1;
  scheduler_reference_state state;

  ASSERT_EQ(rank_scheduler_candidates(data.input, data.output, config, state), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(state.previous_winner, data.candidates[0].ue_index);
  ASSERT_EQ(rank_scheduler_candidates(data.input, data.output, config, state), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(state.previous_winner, data.candidates[1].ue_index);
  EXPECT_EQ(state.invocations, 2U);
}

TEST(reference_scheduler_algorithm, rejects_malformed_or_unbounded_storage)
{
  fixture                   data;
  scheduler_reference_state state;
  data.output.intent_capacity = 1;
  EXPECT_EQ(rank_scheduler_candidates(data.input, data.output, {}, state), OCUDU_DAPP_INVALID_INPUT_V1);
  data.output.intent_capacity     = data.intents.size();
  data.candidates[1].candidate_id = 99;
  EXPECT_EQ(rank_scheduler_candidates(data.input, data.output, {}, state), OCUDU_DAPP_INVALID_INPUT_V1);
  EXPECT_EQ(state.invocations, 0U);

  data.candidates[1].candidate_id = 1;
  for (auto& candidate : data.candidates) {
    candidate.struct_size = OCUDU_DAPP_SCHEDULER_CANDIDATE_BASE_SIZE_V1;
  }
  EXPECT_EQ(rank_scheduler_candidates(data.input, data.output, {}, state), OCUDU_DAPP_OK_V1);
}

TEST(reference_scheduler_algorithm, derives_binary_prb_avoidance_from_validated_interference_context)
{
  fixture                    data;
  scheduler_reference_config config;
  scheduler_reference_state  state;
  data.add_interference_context();
  config.enable_prb_avoidance = true;
  data.interference           = {0, 191, 192, 255, 0, 200, 1, 0};

  ASSERT_EQ(rank_scheduler_candidates(data.input, data.output, config, state), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(data.output.flags, OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1);
  EXPECT_EQ(data.avoidance, (std::array<uint8_t, 8>{0, 0, 1, 1, 0, 1, 0, 0}));

  data.input.optional_interference_mask.layout_id = 0;
  EXPECT_EQ(rank_scheduler_candidates(data.input, data.output, config, state), OCUDU_DAPP_INVALID_INPUT_V1);
}

TEST(reference_scheduler_algorithm, treats_interference_as_observation_when_host_withholds_mask_authority)
{
  fixture                    data;
  scheduler_reference_config config;
  scheduler_reference_state  state;
  data.add_interference_context();
  data.input.allowed_output_flags = 0;
  data.interference[0]            = 255;

  ASSERT_EQ(rank_scheduler_candidates(data.input, data.output, config, state), OCUDU_DAPP_OK_V1);
  EXPECT_EQ(data.output.flags, 0U);
  EXPECT_EQ(data.avoidance[0], 0U);
}
