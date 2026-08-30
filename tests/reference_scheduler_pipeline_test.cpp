// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_scheduler_pipeline.h"
#include "ocudu_dapp_sdk/reference_scheduler_tables.h"
#include <array>
#include <gtest/gtest.h>
#include <vector>

using namespace ocudu::dapp::reference;

namespace {

ocudu_dapp_tensor_view_v1 make_u8_mask(uint8_t* data, uint32_t n, uint32_t access, uint32_t layout)
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
  mask.byte_length           = n;
  mask.dimensions[0]         = n;
  mask.byte_strides[0]       = sizeof(uint8_t);
  mask.layout_id             = layout;
  mask.layout_version        = 1;
  mask.lease_token           = 1;
  return mask;
}

struct rich_fixture {
  static constexpr uint32_t N    = 3;
  static constexpr uint32_t PRBS = 16;

  std::array<ocudu_dapp_scheduler_candidate_v1, N> candidates{};
  std::array<ocudu_dapp_scheduler_intent_v1, N>    intents{};
  ocudu_dapp_scheduler_input_v1                    input{};
  ocudu_dapp_scheduler_output_v1                   output{};
  std::array<uint8_t, PRBS>                        free_mask{};
  std::array<uint8_t, PRBS>                        interference{};
  std::array<uint8_t, PRBS>                        avoidance{};

  rich_fixture()
  {
    free_mask.fill(1);
    for (uint32_t i = 0; i != N; ++i) {
      candidates[i].struct_size   = sizeof(candidates[i]);
      candidates[i].candidate_id  = i;
      candidates[i].ue_index      = 100 + i;
      candidates[i].pending_bytes = 1000 * (i + 1);
      candidates[i].cqi           = 10.0f + static_cast<float>(i);
      candidates[i].sinr_db       = 8.0f + static_cast<float>(i);
      candidates[i].flags = OCUDU_DAPP_SCHEDULER_CANDIDATE_CQI_VALID_V1 | OCUDU_DAPP_SCHEDULER_CANDIDATE_SINR_VALID_V1 |
                            OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1;
      candidates[i].expert_mcs_min  = 0;
      candidates[i].expert_mcs_max  = 28;
      candidates[i].mcs_table_id    = OCUDU_DAPP_SCHEDULER_MCS_TABLE_QAM64_V1;
      candidates[i].nof_layers      = 1;
      candidates[i].recommended_mcs = 10;
      candidates[i].flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_MCS_VALID_V1;
    }
    input.invocation.cell_id   = 1;
    input.direction            = OCUDU_DAPP_SCHEDULER_UL_V1;
    input.nof_candidates       = N;
    input.cell_nof_prbs        = PRBS;
    input.remaining_slice_prbs = PRBS;
    input.candidate_generation = 42;
    input.candidates           = candidates.data();
    output.struct_size         = sizeof(output);
    output.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
    output.abi_minor           = OCUDU_DAPP_ABI_MINOR_V1;
    output.intents             = intents.data();
    output.intent_capacity     = intents.size();
  }

  void grant_all_authority()
  {
    input.allowed_output_flags = OCUDU_DAPP_SCHEDULER_ALLOW_MCS_V1 | OCUDU_DAPP_SCHEDULER_ALLOW_PRB_LIMITS_V1 |
                                 OCUDU_DAPP_SCHEDULER_ALLOW_PRB_PREFERENCE_V1 |
                                 OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1 | OCUDU_DAPP_SCHEDULER_ALLOW_TPC_V1 |
                                 OCUDU_DAPP_SCHEDULER_ALLOW_LAYERS_V1;
  }

  void add_free_mask()
  {
    input.free_vrb_context_present        = 1;
    input.free_vrb_context.struct_size    = sizeof(input.free_vrb_context);
    input.free_vrb_context.schema_id      = OCUDU_DAPP_SCHEDULER_CONTEXT_FREE_VRB_MASK_V1;
    input.free_vrb_context.schema_version = 1;
    input.free_vrb_context.cell_id        = input.invocation.cell_id;
    input.free_vrb_mask =
        make_u8_mask(free_mask.data(), PRBS, OCUDU_DAPP_ACCESS_READ_V1, OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_FREE_U8_V1);
  }
};

} // namespace

TEST(reference_scheduler_tables, cqi_and_snr_maps_are_monotonic)
{
  auto m4 = map_cqi_to_mcs_portable(4, MCS_TABLE_QAM64);
  auto m8 = map_cqi_to_mcs_portable(8, MCS_TABLE_QAM64);
  ASSERT_TRUE(m4 && m8);
  EXPECT_LE(*m4, *m8);
  EXPECT_LE(map_snr_to_mcs_ul_portable(0.0, MCS_TABLE_QAM64, false),
            map_snr_to_mcs_ul_portable(10.0, MCS_TABLE_QAM64, false));
}

TEST(reference_scheduler_tables, mcs_table_selection_differs_qam64_vs_256)
{
  // Same CQI maps differently across spectral-efficiency tables (host maps).
  auto m64  = map_cqi_to_mcs_portable(10, MCS_TABLE_QAM64);       // → 18
  auto m256 = map_cqi_to_mcs_portable(10, MCS_TABLE_QAM256);      // → 17
  auto mlow = map_cqi_to_mcs_portable(10, MCS_TABLE_QAM64_LOWSE); // → 18
  ASSERT_TRUE(m64 && m256 && mlow);
  EXPECT_EQ(*m64, 18);
  EXPECT_EQ(*m256, 17);
  EXPECT_EQ(*mlow, 18);
  EXPECT_EQ(max_mcs_for_table(MCS_TABLE_QAM64, false), 28);
  EXPECT_EQ(max_mcs_for_table(MCS_TABLE_QAM256, false), 27);
  EXPECT_EQ(max_mcs_for_table(MCS_TABLE_QAM64, true), 27); // transform precoder
  EXPECT_STREQ(mcs_table_name(MCS_TABLE_QAM64), "qam64");
  EXPECT_STREQ(mcs_table_name(MCS_TABLE_QAM256), "qam256");
  EXPECT_STREQ(mcs_table_name(MCS_TABLE_QAM64_LOWSE), "qam64LowSe");
  // At the same moderate SNR, 256QAM ladder is stricter → lower MCS index.
  EXPECT_GT(map_snr_to_mcs_ul_portable(12.0, MCS_TABLE_QAM64, false),
            map_snr_to_mcs_ul_portable(12.0, MCS_TABLE_QAM256, false));
  EXPECT_GE(mcs_spectral_efficiency(20, MCS_TABLE_QAM256), mcs_spectral_efficiency(20, MCS_TABLE_QAM64_LOWSE));
}

TEST(reference_scheduler_pipeline, mcs_respects_candidate_table_id)
{
  rich_fixture f;
  f.grant_all_authority();
  f.add_free_mask();
  f.input.direction = OCUDU_DAPP_SCHEDULER_DL_V1;
  for (auto& c : f.candidates) {
    c.cqi            = 10.0f;
    c.flags          = OCUDU_DAPP_SCHEDULER_CANDIDATE_CQI_VALID_V1 | OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1;
    c.mcs_table_id   = MCS_TABLE_QAM256;
    c.expert_mcs_min = 0;
    c.expert_mcs_max = 27;
  }
  scheduler_reference_config cfg;
  cfg.rank = scheduler_rank_mode::conventional_passthrough;
  cfg.olla = scheduler_olla_mode::off;
  cfg.mcs  = scheduler_mcs_mode::table_plus_olla;
  cfg.pack = scheduler_pack_mode::off;
  cfg.tpc  = scheduler_tpc_mode::off;
  scheduler_reference_state state;
  ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);
  // CQI 10 on qam256 table → MCS 17 per portable map; must not exceed 27.
  EXPECT_EQ(f.intents[0].recommended_mcs, 17);
  EXPECT_LE(f.intents[0].maximum_mcs, 27);
}

TEST(reference_scheduler_olla, steps_on_ack_and_nack)
{
  olla_config cfg;
  cfg.target_bler   = 0.1f;
  cfg.step_db       = 0.1f;
  cfg.max_offset_db = 5.0f;
  float o           = 0.0f;
  o                 = olla_update(o, true, 10, 0, 28, cfg);
  EXPECT_GT(o, 0.0f);
  const float after_ack = o;
  o                     = olla_update(o, false, 10, 0, 28, cfg);
  EXPECT_LT(o, after_ack);
}

TEST(reference_scheduler_pipeline, without_allow_flags_emits_only_priority)
{
  rich_fixture               f;
  scheduler_reference_config cfg;
  cfg.emit_only_allowed = true;
  cfg.mcs               = scheduler_mcs_mode::table_plus_olla;
  cfg.pack              = scheduler_pack_mode::hard_pack_first_fit_low;
  cfg.tpc               = scheduler_tpc_mode::target_sinr;
  scheduler_reference_state state;
  ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(f.output.nof_intents, f.N);
  for (uint32_t i = 0; i != f.N; ++i) {
    EXPECT_EQ(f.intents[i].flags, OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1);
  }
}

TEST(reference_scheduler_pipeline, shipped_default_preserves_host_policy_even_with_all_authority)
{
  rich_fixture f;
  f.grant_all_authority();
  f.add_free_mask();
  f.input.allowed_output_flags |= OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1;
  f.input.interference_context_present        = 1;
  f.input.interference_context.struct_size    = sizeof(f.input.interference_context);
  f.input.interference_context.schema_id      = OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1;
  f.input.interference_context.schema_version = 1;
  f.input.interference_context.cell_id        = f.input.invocation.cell_id;
  f.input.optional_interference_mask          = make_u8_mask(f.interference.data(), f.PRBS, OCUDU_DAPP_ACCESS_READ_V1,
                                                             OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1);
  f.output.avoid_prb_mask                     = make_u8_mask(f.avoidance.data(), f.PRBS, OCUDU_DAPP_ACCESS_WRITE_V1,
                                                             OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1);
  f.interference.fill(255);

  f.candidates[0].conventional_priority = 1.0;
  f.candidates[1].conventional_priority = 9.0;
  f.candidates[2].conventional_priority = 3.0;

  scheduler_reference_config cfg;
  scheduler_reference_state  state;
  ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);
  ASSERT_EQ(f.output.nof_intents, f.N);
  EXPECT_EQ(f.output.flags, 0U);
  EXPECT_DOUBLE_EQ(f.intents[0].priority, 1.0);
  EXPECT_DOUBLE_EQ(f.intents[1].priority, 9.0);
  EXPECT_DOUBLE_EQ(f.intents[2].priority, 3.0);
  for (uint32_t i = 0; i != f.N; ++i) {
    EXPECT_EQ(f.intents[i].flags, OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1);
  }
}

TEST(reference_scheduler_pipeline, hard_pack_non_overlapping_under_assignment_authority)
{
  rich_fixture f;
  f.grant_all_authority();
  f.add_free_mask();
  // Occupy mid-band so pack must place around it.
  for (uint32_t p = 6; p < 10; ++p) {
    f.free_mask[p] = 0;
  }

  scheduler_reference_config cfg;
  cfg.emit_only_allowed      = true;
  cfg.rank                   = scheduler_rank_mode::backlog;
  cfg.olla                   = scheduler_olla_mode::off;
  cfg.mcs                    = scheduler_mcs_mode::table_plus_olla;
  cfg.pack                   = scheduler_pack_mode::hard_pack_first_fit_low;
  cfg.tpc                    = scheduler_tpc_mode::hold0db;
  cfg.assumed_data_symbols   = 12;
  cfg.prb_bytes_per_re_scale = 0.05f; // force larger PRB grants for test visibility

  scheduler_reference_state state;
  ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);

  std::array<uint8_t, rich_fixture::PRBS> painted{};
  painted.fill(0);
  for (uint32_t i = 0; i != f.N; ++i) {
    const auto& intent = f.intents[i];
    EXPECT_NE((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1), 0u);
    EXPECT_NE((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_MCS_V1), 0u);
    if ((intent.flags & OCUDU_DAPP_SCHEDULER_INTENT_PRB_PREFERENCE_V1) != 0) {
      const uint16_t s = intent.preferred_start_prb;
      const uint16_t n = intent.preferred_nof_prbs;
      ASSERT_GT(n, 0);
      ASSERT_LE(static_cast<uint32_t>(s) + n, rich_fixture::PRBS);
      for (uint16_t k = 0; k != n; ++k) {
        EXPECT_EQ(f.free_mask[s + k], 1) << "assignment must be free in original mask";
        EXPECT_EQ(painted[s + k], 0) << "overlap";
        painted[s + k] = 1;
      }
    }
  }
}

TEST(reference_scheduler_pipeline, first_fit_high_packs_from_top)
{
  rich_fixture f;
  f.grant_all_authority();
  f.add_free_mask();
  for (auto& c : f.candidates) {
    c.pending_bytes = 50; // small
  }
  scheduler_reference_config cfg;
  cfg.mcs                    = scheduler_mcs_mode::host_echo;
  cfg.pack                   = scheduler_pack_mode::hard_pack_first_fit_high;
  cfg.tpc                    = scheduler_tpc_mode::off;
  cfg.olla                   = scheduler_olla_mode::off;
  cfg.prb_bytes_per_re_scale = 1.0f;
  scheduler_reference_state state;
  ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);

  bool any = false;
  for (uint32_t i = 0; i != f.N; ++i) {
    if ((f.intents[i].flags & OCUDU_DAPP_SCHEDULER_INTENT_PRB_PREFERENCE_V1) != 0) {
      any = true;
      // With high-first and small needs, starts should be toward top of band.
      EXPECT_GE(f.intents[i].preferred_start_prb + f.intents[i].preferred_nof_prbs, rich_fixture::PRBS / 2);
    }
  }
  EXPECT_TRUE(any);
}

TEST(reference_scheduler_pipeline, contained_olla_reacts_to_outcomes)
{
  rich_fixture f;
  f.grant_all_authority();
  f.add_free_mask();
  f.candidates[0].flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_OUTCOME_VALID_V1;
  f.candidates[0].last_tx_outcome = OCUDU_DAPP_SCHEDULER_OUTCOME_NACK_OR_CRC_KO_V1;
  f.candidates[0].last_used_mcs   = 12;
  f.candidates[0].sinr_db         = 10.0f;

  scheduler_reference_config cfg;
  cfg.olla = scheduler_olla_mode::contained_baseline;
  cfg.mcs  = scheduler_mcs_mode::table_plus_olla;
  cfg.pack = scheduler_pack_mode::off;
  cfg.tpc  = scheduler_tpc_mode::off;
  scheduler_reference_state state;

  // First call: establish last_decided_mcs
  ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);
  const int16_t mcs_first = f.intents[0].recommended_mcs;

  // Second call: outcome matches previous MCS → NACK pulls MCS down over time
  f.candidates[0].last_used_mcs = static_cast<uint8_t>(mcs_first);
  for (int i = 0; i < 50; ++i) {
    ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);
  }
  EXPECT_LE(f.intents[0].recommended_mcs, mcs_first);
}

TEST(reference_scheduler_pipeline, tpc_maps_sinr_gap)
{
  rich_fixture f;
  f.input.allowed_output_flags       = OCUDU_DAPP_SCHEDULER_ALLOW_TPC_V1;
  f.candidates[0].sinr_db            = 0.0f;
  f.candidates[0].tpc_target_sinr_db = 5.0f;
  scheduler_reference_config cfg;
  cfg.mcs  = scheduler_mcs_mode::off;
  cfg.pack = scheduler_pack_mode::off;
  cfg.olla = scheduler_olla_mode::off;
  cfg.tpc  = scheduler_tpc_mode::target_sinr;
  scheduler_reference_state state;
  ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);
  EXPECT_NE((f.intents[0].flags & OCUDU_DAPP_SCHEDULER_INTENT_TPC_V1), 0u);
  EXPECT_EQ(f.intents[0].tpc_command, 3); // large positive gap
}

TEST(reference_scheduler_pipeline, conventional_passthrough_rank)
{
  rich_fixture f;
  f.candidates[0].conventional_priority = 1.0;
  f.candidates[1].conventional_priority = 9.0;
  f.candidates[2].conventional_priority = 3.0;
  f.candidates[2].flags |= OCUDU_DAPP_SCHEDULER_CANDIDATE_CONVENTIONAL_FORBID_V1;
  scheduler_reference_config cfg;
  cfg.rank = scheduler_rank_mode::conventional_passthrough;
  cfg.mcs  = scheduler_mcs_mode::off;
  cfg.pack = scheduler_pack_mode::off;
  cfg.olla = scheduler_olla_mode::off;
  cfg.tpc  = scheduler_tpc_mode::off;
  scheduler_reference_state state;
  ASSERT_EQ(run_scheduler_pipeline(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);
  EXPECT_DOUBLE_EQ(f.intents[1].priority, 9.0);
  EXPECT_EQ(f.intents[2].flags, OCUDU_DAPP_SCHEDULER_INTENT_FORBID_V1);
}

TEST(reference_scheduler_pipeline, legacy_rank_entry_ignores_rich_modes)
{
  rich_fixture f;
  f.grant_all_authority();
  f.add_free_mask();
  scheduler_reference_config cfg;
  cfg.mcs  = scheduler_mcs_mode::table_plus_olla;
  cfg.pack = scheduler_pack_mode::hard_pack_first_fit_low;
  cfg.tpc  = scheduler_tpc_mode::target_sinr;
  scheduler_reference_state state;
  ASSERT_EQ(rank_scheduler_candidates(f.input, f.output, cfg, state), OCUDU_DAPP_OK_V1);
  for (uint32_t i = 0; i != f.N; ++i) {
    EXPECT_EQ(f.intents[i].flags, OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1);
  }
}
