// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_scheduler_pipeline.h"
#include "ocudu_dapp_sdk/reference_scheduler_tables.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

using namespace ocudu::dapp::reference;

namespace {

bool valid_interference_input(const ocudu_dapp_scheduler_input_v1& input) noexcept
{
  constexpr uint32_t context_minimum = offsetof(ocudu_dapp_context_header_v1, reserved);
  const auto&        context         = input.interference_context;
  const auto&        mask            = input.optional_interference_mask;
  return input.interference_context_present == 1 && context.struct_size >= context_minimum &&
         context.schema_id == OCUDU_DAPP_SCHEDULER_CONTEXT_INTERFERENCE_MAP_V1 && context.schema_version == 1 &&
         context.cell_id == input.invocation.cell_id && mask.domain == OCUDU_DAPP_MEM_HOST_V1 &&
         mask.backend_id == OCUDU_DAPP_BACKEND_HOST_V1 && mask.element_type == OCUDU_DAPP_ELEMENT_U8_V1 &&
         mask.rank == 1 && mask.dimensions[0] == input.cell_nof_prbs && mask.byte_strides[0] == sizeof(uint8_t) &&
         mask.byte_length >= input.cell_nof_prbs &&
         mask.layout_id == OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1 &&
         (mask.access & (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1)) ==
             (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) &&
         mask.native_handle != 0;
}

bool valid_avoidance_output(const ocudu_dapp_scheduler_output_v1& output, uint32_t nof_prbs) noexcept
{
  const auto& mask = output.avoid_prb_mask;
  return mask.domain == OCUDU_DAPP_MEM_HOST_V1 && mask.backend_id == OCUDU_DAPP_BACKEND_HOST_V1 &&
         mask.element_type == OCUDU_DAPP_ELEMENT_U8_V1 && mask.rank == 1 && mask.dimensions[0] == nof_prbs &&
         mask.byte_strides[0] == sizeof(uint8_t) && mask.byte_length >= nof_prbs &&
         mask.layout_id == OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_INTERFERENCE_U8_V1 &&
         (mask.access & (OCUDU_DAPP_ACCESS_WRITE_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1)) ==
             (OCUDU_DAPP_ACCESS_WRITE_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) &&
         mask.native_handle != 0;
}

bool valid_free_vrb_input(const ocudu_dapp_scheduler_input_v1& input) noexcept
{
  constexpr uint32_t context_minimum = offsetof(ocudu_dapp_context_header_v1, reserved);
  if (input.free_vrb_context_present != 1 || input.cell_nof_prbs == 0) {
    return false;
  }
  const auto& context = input.free_vrb_context;
  const auto& mask    = input.free_vrb_mask;
  return context.struct_size >= context_minimum && context.schema_id == OCUDU_DAPP_SCHEDULER_CONTEXT_FREE_VRB_MASK_V1 &&
         context.schema_version == 1 && context.cell_id == input.invocation.cell_id &&
         mask.domain == OCUDU_DAPP_MEM_HOST_V1 && mask.backend_id == OCUDU_DAPP_BACKEND_HOST_V1 &&
         mask.element_type == OCUDU_DAPP_ELEMENT_U8_V1 && mask.rank == 1 && mask.dimensions[0] == input.cell_nof_prbs &&
         mask.byte_strides[0] == sizeof(uint8_t) && mask.byte_length >= input.cell_nof_prbs &&
         mask.layout_id == OCUDU_DAPP_SCHEDULER_LAYOUT_PRB_FREE_U8_V1 &&
         (mask.access & (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1)) ==
             (OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) &&
         mask.native_handle != 0;
}

bool allowed(const ocudu_dapp_scheduler_input_v1& input, uint32_t bit, bool emit_only) noexcept
{
  return !emit_only || (input.allowed_output_flags & bit) != 0;
}

uint16_t estimate_nof_prbs(uint64_t pending_bytes, uint8_t mcs, uint8_t layers, uint8_t mcs_table_id, uint8_t symbols,
                           float scale, uint16_t cell_prbs, uint16_t remaining) noexcept
{
  if (pending_bytes == 0 || cell_prbs == 0) {
    return 0;
  }
  const float se        = mcs_spectral_efficiency(mcs, mcs_table_id);
  const float layers_f  = std::max<uint8_t>(1, layers);
  const float re_prb    = static_cast<float>(std::max<uint8_t>(1, symbols)) * 12.0f; // subcarriers
  const float bytes_prb = std::max(1.0f, se * layers_f * re_prb * scale / 8.0f);
  uint32_t need = static_cast<uint32_t>(std::ceil(static_cast<double>(pending_bytes) / static_cast<double>(bytes_prb)));
  need          = std::max(1u, need);
  const uint16_t cap = remaining != 0 ? std::min<uint16_t>(cell_prbs, remaining) : cell_prbs;
  return static_cast<uint16_t>(std::min<uint32_t>(need, cap));
}

/// Find first contiguous free run of length need in free[0..nof). free[i]==1 means free.
bool find_run(const uint8_t* free, uint32_t nof, uint16_t need, bool from_high, uint16_t& start_out) noexcept
{
  if (need == 0 || need > nof) {
    return false;
  }
  if (!from_high) {
    uint32_t run       = 0;
    uint32_t run_start = 0;
    for (uint32_t i = 0; i != nof; ++i) {
      if (free[i] != 0) {
        if (run == 0) {
          run_start = i;
        }
        ++run;
        if (run >= need) {
          start_out = static_cast<uint16_t>(run_start);
          return true;
        }
      } else {
        run = 0;
      }
    }
    return false;
  }
  uint32_t run = 0;
  for (int i = static_cast<int>(nof) - 1; i >= 0; --i) {
    if (free[static_cast<uint32_t>(i)] != 0) {
      ++run;
      if (run >= need) {
        start_out = static_cast<uint16_t>(static_cast<uint32_t>(i));
        return true;
      }
    } else {
      run = 0;
    }
  }
  return false;
}

/// Prefer free run maximizing clean (interference==0) PRBs; fall back to first-fit low.
bool find_run_prefer_clean(const uint8_t* free, const uint8_t* interference, uint32_t nof, uint16_t need,
                           uint16_t& start_out) noexcept
{
  if (need == 0 || need > nof) {
    return false;
  }
  int      best_score = -1;
  uint16_t best_start = 0;
  for (uint32_t s = 0; s + need <= nof; ++s) {
    bool ok = true;
    for (uint16_t k = 0; k != need; ++k) {
      if (free[s + k] == 0) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      continue;
    }
    int score = static_cast<int>(need);
    if (interference != nullptr) {
      score = 0;
      for (uint16_t k = 0; k != need; ++k) {
        score += interference[s + k] == 0 ? 1 : 0;
      }
    }
    if (score > best_score) {
      best_score = score;
      best_start = static_cast<uint16_t>(s);
    }
  }
  if (best_score < 0) {
    return find_run(free, nof, need, false, start_out);
  }
  start_out = best_start;
  return true;
}

void paint(uint8_t* free, uint16_t start, uint16_t nof) noexcept
{
  for (uint16_t i = 0; i != nof; ++i) {
    free[start + i] = 0;
  }
}

int8_t map_tpc(float gap_db, float deadband, float step_hi) noexcept
{
  // Host-like: gap = target - measured. Positive gap ⇒ need more power.
  if (gap_db > step_hi) {
    return 3; // +3 dB
  }
  if (gap_db > deadband) {
    return 2; // +1 dB
  }
  if (gap_db > -deadband) {
    return 1; // 0 dB
  }
  return 0; // -1 dB
}

} // namespace

ocudu_dapp_status_v1 ocudu::dapp::reference::run_scheduler_pipeline(const ocudu_dapp_scheduler_input_v1& input,
                                                                    ocudu_dapp_scheduler_output_v1&      output,
                                                                    const scheduler_reference_config&    config,
                                                                    scheduler_reference_state&           state) noexcept
{
  constexpr uint32_t candidate_minimum = OCUDU_DAPP_SCHEDULER_CANDIDATE_BASE_SIZE_V1;
  constexpr uint32_t output_minimum    = offsetof(ocudu_dapp_scheduler_output_v1, reserved);
  if ((input.direction != OCUDU_DAPP_SCHEDULER_DL_V1 && input.direction != OCUDU_DAPP_SCHEDULER_UL_V1) ||
      (input.allowed_output_flags & ~OCUDU_DAPP_SCHEDULER_ALLOW_KNOWN_MASK_V1) != 0 ||
      ((input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1) != 0 &&
       (input.direction != OCUDU_DAPP_SCHEDULER_UL_V1 || input.interference_context_present == 0)) ||
      input.nof_candidates == 0 || input.candidates == nullptr || input.candidate_generation == 0 ||
      output.struct_size < output_minimum || output.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || output.intents == nullptr ||
      output.intent_capacity < input.nof_candidates || !std::isfinite(config.backlog_weight) ||
      !std::isfinite(config.scheduling_request_bonus) || !std::isfinite(config.consecutive_winner_penalty) ||
      config.backlog_weight < 0.0 || config.scheduling_request_bonus < 0.0 || config.consecutive_winner_penalty < 0.0) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }

  const bool publish_avoidance =
      config.enable_prb_avoidance &&
      allowed(input, OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1, config.emit_only_allowed) &&
      (input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_AVOID_PRB_MASK_V1) != 0;
  if (publish_avoidance && (!valid_interference_input(input) || !valid_avoidance_output(output, input.cell_nof_prbs))) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }

  const bool want_hard_pack = (config.pack == scheduler_pack_mode::hard_pack_first_fit_low ||
                               config.pack == scheduler_pack_mode::hard_pack_first_fit_high ||
                               config.pack == scheduler_pack_mode::hard_pack_prefer_clean) &&
                              allowed(input, OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1, config.emit_only_allowed) &&
                              (input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1) != 0;
  if (want_hard_pack && !valid_free_vrb_input(input)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }

  const bool emit_mcs = config.mcs != scheduler_mcs_mode::off &&
                        allowed(input, OCUDU_DAPP_SCHEDULER_ALLOW_MCS_V1, config.emit_only_allowed) &&
                        (input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_MCS_V1) != 0;
  const bool emit_prb_limits = (config.pack == scheduler_pack_mode::limits_only || want_hard_pack) &&
                               allowed(input, OCUDU_DAPP_SCHEDULER_ALLOW_PRB_LIMITS_V1, config.emit_only_allowed) &&
                               (input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_PRB_LIMITS_V1) != 0;
  const bool emit_prb_pref =
      (config.pack == scheduler_pack_mode::soft_prefer_clean || want_hard_pack) &&
      allowed(input,
              want_hard_pack ? OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1
                             : OCUDU_DAPP_SCHEDULER_ALLOW_PRB_PREFERENCE_V1,
              config.emit_only_allowed) &&
      (input.allowed_output_flags & (want_hard_pack ? OCUDU_DAPP_SCHEDULER_ALLOW_PRB_ASSIGNMENT_V1
                                                    : OCUDU_DAPP_SCHEDULER_ALLOW_PRB_PREFERENCE_V1)) != 0;
  const bool emit_tpc = config.tpc != scheduler_tpc_mode::off &&
                        allowed(input, OCUDU_DAPP_SCHEDULER_ALLOW_TPC_V1, config.emit_only_allowed) &&
                        (input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_TPC_V1) != 0;
  const bool emit_layers = config.emit_layers &&
                           allowed(input, OCUDU_DAPP_SCHEDULER_ALLOW_LAYERS_V1, config.emit_only_allowed) &&
                           (input.allowed_output_flags & OCUDU_DAPP_SCHEDULER_ALLOW_LAYERS_V1) != 0;

  // Working free mask for packing (stack for small cells; heap fallback).
  std::vector<uint8_t> free_work;
  const uint8_t*       interference_ptr = nullptr;
  if (want_hard_pack) {
    free_work.resize(input.cell_nof_prbs);
    const auto* src = reinterpret_cast<const uint8_t*>(input.free_vrb_mask.native_handle);
    std::copy_n(src, input.cell_nof_prbs, free_work.data());
  }
  if (valid_interference_input(input)) {
    interference_ptr = reinterpret_cast<const uint8_t*>(input.optional_interference_mask.native_handle);
  }

  // Per-candidate decided MCS for pack sizing.
  std::vector<uint8_t> decided_mcs(input.nof_candidates, 0);
  std::vector<uint8_t> decided_layers(input.nof_candidates, 1);
  std::vector<double>  priorities(input.nof_candidates, 0.0);
  std::vector<uint8_t> forbid(input.nof_candidates, 0);

  double   best_priority = -std::numeric_limits<double>::infinity();
  uint32_t best_ue       = 0;

  // ---- Stage: OLLA update + MCS + rank ----
  for (uint32_t index = 0; index != input.nof_candidates; ++index) {
    const auto& candidate = input.candidates[index];
    if (candidate.struct_size < candidate_minimum || candidate.candidate_id != index) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }

    // Expert MCS envelope ∩ table-legal max (qam64/256/LowSe + transform precoder).
    const bool    tp     = candidate.transform_precoder != 0;
    const uint8_t table  = valid_mcs_table_id(candidate.mcs_table_id) ? candidate.mcs_table_id : MCS_TABLE_QAM64;
    uint8_t       mcs_lo = 0;
    uint8_t       mcs_hi = max_mcs_for_table(table, tp);
    if ((candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1) != 0) {
      mcs_lo = candidate.expert_mcs_min;
      mcs_hi = candidate.expert_mcs_max;
    }

    // OLLA state
    float olla_offset = 0.0f;
    if (config.olla == scheduler_olla_mode::host_offset_echo &&
        (candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_OLLA_OFFSET_VALID_V1) != 0) {
      olla_offset = candidate.host_olla_offset_db;
    } else if (config.olla == scheduler_olla_mode::contained_baseline) {
      auto* entry = state.olla_map.find_or_insert(candidate.ue_index);
      if ((candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_OLLA_OFFSET_VALID_V1) != 0 && !entry->has_last_mcs) {
        entry->offset_db = candidate.host_olla_offset_db;
      }
      if ((candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_OUTCOME_VALID_V1) != 0 && entry->has_last_mcs &&
          candidate.last_used_mcs == entry->last_decided_mcs &&
          candidate.last_tx_outcome != OCUDU_DAPP_SCHEDULER_OUTCOME_NONE_V1 &&
          candidate.last_tx_outcome != OCUDU_DAPP_SCHEDULER_OUTCOME_DTX_V1) {
        const bool ok    = candidate.last_tx_outcome == OCUDU_DAPP_SCHEDULER_OUTCOME_ACK_OR_CRC_OK_V1;
        entry->offset_db = olla_update(entry->offset_db, ok, candidate.last_used_mcs, mcs_lo, mcs_hi, config.olla_cfg);
      }
      olla_offset = entry->offset_db;
    }

    // MCS decision — always table-aware (CQI/SNR ladders + max index).
    uint8_t mcs    = 0;
    bool    mcs_ok = false;
    if (config.mcs == scheduler_mcs_mode::host_echo &&
        (candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_MCS_VALID_V1) != 0) {
      mcs    = clamp_mcs_for_table(candidate.recommended_mcs, mcs_lo, mcs_hi, table, tp);
      mcs_ok = true;
    } else if (config.mcs == scheduler_mcs_mode::table_plus_olla) {
      if (input.direction == OCUDU_DAPP_SCHEDULER_DL_V1 &&
          (candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_CQI_VALID_V1) != 0) {
        const float eff   = candidate.cqi + olla_offset;
        const float lb    = std::floor(std::max(1.0f, std::min(15.0f, eff)));
        const float ub    = std::ceil(std::max(1.0f, std::min(15.0f, eff)));
        const float coeff = std::max(1.0f, std::min(15.0f, eff)) - lb;
        const auto  m0    = map_cqi_to_mcs_portable(static_cast<unsigned>(lb), table);
        const auto  m1    = map_cqi_to_mcs_portable(static_cast<unsigned>(ub), table);
        if (m0 && m1) {
          mcs    = clamp_mcs_for_table(static_cast<int>(std::floor(*m0 * (1.0f - coeff) + *m1 * coeff)), mcs_lo, mcs_hi,
                                       table, tp);
          mcs_ok = true;
        }
      } else if (input.direction == OCUDU_DAPP_SCHEDULER_UL_V1 &&
                 (candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_SINR_VALID_V1) != 0) {
        mcs = clamp_mcs_for_table(
            map_snr_to_mcs_ul_portable(candidate.sinr_db + static_cast<double>(olla_offset), table, tp), mcs_lo, mcs_hi,
            table, tp);
        mcs_ok = true;
      } else if ((candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_MCS_VALID_V1) != 0) {
        mcs    = clamp_mcs_for_table(candidate.recommended_mcs, mcs_lo, mcs_hi, table, tp);
        mcs_ok = true;
      }
    }
    if (!mcs_ok && (candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_MCS_VALID_V1) != 0) {
      mcs    = clamp_mcs_for_table(candidate.recommended_mcs, mcs_lo, mcs_hi, table, tp);
      mcs_ok = true;
    }
    if (!mcs_ok) {
      mcs = clamp_mcs_for_table(0, mcs_lo, mcs_hi, table, tp);
    }
    decided_mcs[index] = mcs;
    if (config.olla == scheduler_olla_mode::contained_baseline) {
      auto* entry             = state.olla_map.find_or_insert(candidate.ue_index);
      entry->last_decided_mcs = mcs;
      entry->has_last_mcs     = true;
    }

    uint8_t layers = 1;
    if ((candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_LAYERS_VALID_V1) != 0 && candidate.nof_layers > 0) {
      layers = candidate.nof_layers;
    }
    decided_layers[index] = layers;

    // Rank
    double priority  = 0.0;
    bool   is_forbid = (candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_CONVENTIONAL_FORBID_V1) != 0;
    if (config.rank == scheduler_rank_mode::conventional_passthrough) {
      if (is_forbid) {
        forbid[index] = 1;
      } else {
        priority = candidate.conventional_priority;
      }
    } else if (config.rank == scheduler_rank_mode::pf_lite) {
      double avg = 1.0;
      if ((candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_AVERAGE_RATE_VALID_V1) != 0 &&
          candidate.average_rate_bps > 0) {
        avg = static_cast<double>(candidate.average_rate_bps);
      }
      const double se    = mcs_spectral_efficiency(mcs, candidate.mcs_table_id);
      const double estim = se * layers * std::max<uint64_t>(1, candidate.pending_bytes > 0 ? 1 : 0);
      const double pf    = estim > 0.0 ? estim / std::pow(std::max(avg, 1.0), config.pf_fairness_coeff) : 0.0;
      const double sr =
          (candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_PENDING_SR_V1) != 0 ? config.scheduling_request_bonus : 0.0;
      priority = pf + sr - static_cast<double>(candidate.candidate_id) * 1.0e-9;
    } else {
      // backlog
      const double backlog = std::log1p(static_cast<double>(candidate.pending_bytes));
      const double sr_bonus =
          (candidate.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_PENDING_SR_V1) != 0 ? config.scheduling_request_bonus : 0.0;
      const double repeat_penalty = state.has_previous_winner && candidate.ue_index == state.previous_winner
                                        ? config.consecutive_winner_penalty
                                        : 0.0;
      const double tie_break      = -static_cast<double>(candidate.candidate_id) * 1.0e-9;
      priority                    = config.backlog_weight * backlog + sr_bonus - repeat_penalty + tie_break;
    }
    if (!std::isfinite(priority) && forbid[index] == 0) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    priorities[index] = priority;
    if (forbid[index] == 0 && priority > best_priority) {
      best_priority = priority;
      best_ue       = candidate.ue_index;
    }
  }

  // Sort order for packing: indices by priority desc
  std::vector<uint32_t> order(input.nof_candidates);
  for (uint32_t i = 0; i != input.nof_candidates; ++i) {
    order[i] = i;
  }
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    if (forbid[a] != forbid[b]) {
      return forbid[a] < forbid[b];
    }
    return priorities[a] > priorities[b];
  });

  // ---- Stage: pack ----
  std::vector<uint16_t> assign_start(input.nof_candidates, 0);
  std::vector<uint16_t> assign_nof(input.nof_candidates, 0);
  std::vector<uint16_t> min_prbs(input.nof_candidates, 0);
  std::vector<uint16_t> max_prbs(input.nof_candidates, 0);
  uint16_t              remaining = input.remaining_slice_prbs;
  if (remaining == 0 && input.cell_nof_prbs != 0) {
    remaining = static_cast<uint16_t>(input.cell_nof_prbs);
  }

  if (config.pack != scheduler_pack_mode::off) {
    for (uint32_t ord = 0; ord != order.size(); ++ord) {
      const uint32_t index     = order[ord];
      const auto&    candidate = input.candidates[index];
      if (forbid[index] != 0) {
        continue;
      }
      const uint16_t need =
          estimate_nof_prbs(candidate.pending_bytes, decided_mcs[index], decided_layers[index], candidate.mcs_table_id,
                            config.assumed_data_symbols, config.prb_bytes_per_re_scale,
                            static_cast<uint16_t>(input.cell_nof_prbs), remaining);
      min_prbs[index] = need > 0 ? 1 : 0;
      max_prbs[index] = need;

      if (want_hard_pack && need > 0) {
        uint16_t start = 0;
        bool     found = false;
        if (config.pack == scheduler_pack_mode::hard_pack_first_fit_high) {
          found = find_run(free_work.data(), input.cell_nof_prbs, need, true, start);
        } else if (config.pack == scheduler_pack_mode::hard_pack_prefer_clean) {
          found = find_run_prefer_clean(free_work.data(), interference_ptr, input.cell_nof_prbs, need, start);
        } else {
          found = find_run(free_work.data(), input.cell_nof_prbs, need, false, start);
        }
        if (found) {
          assign_start[index] = start;
          assign_nof[index]   = need;
          paint(free_work.data(), start, need);
          if (remaining >= need) {
            remaining = static_cast<uint16_t>(remaining - need);
          } else {
            remaining = 0;
          }
        } else if (config.forbid_on_pack_fail) {
          forbid[index] = 1;
        }
      } else if (config.pack == scheduler_pack_mode::soft_prefer_clean && need > 0 && free_work.empty() &&
                 input.cell_nof_prbs > 0) {
        // Soft preference without free mask: prefer low PRBs or clean interference band.
        if (interference_ptr != nullptr) {
          uint16_t best       = 0;
          int      best_score = -1;
          for (uint32_t s = 0; s + need <= input.cell_nof_prbs; ++s) {
            int score = 0;
            for (uint16_t k = 0; k != need; ++k) {
              score += interference_ptr[s + k] == 0 ? 1 : 0;
            }
            if (score > best_score) {
              best_score = score;
              best       = static_cast<uint16_t>(s);
            }
          }
          assign_start[index] = best;
          assign_nof[index]   = need;
        } else {
          assign_start[index] = 0;
          assign_nof[index]   = need;
        }
      } else if (config.pack == scheduler_pack_mode::limits_only) {
        // max_prbs already set
      }
    }
  }

  // ---- Emit intents ----
  for (uint32_t index = 0; index != input.nof_candidates; ++index) {
    auto& intent        = output.intents[index];
    intent              = {};
    intent.struct_size  = sizeof(intent);
    intent.candidate_id = index;
    if (forbid[index] != 0) {
      intent.flags = OCUDU_DAPP_SCHEDULER_INTENT_FORBID_V1;
    } else {
      intent.flags    = OCUDU_DAPP_SCHEDULER_INTENT_PRIORITY_V1;
      intent.priority = priorities[index];
    }

    if (emit_mcs && forbid[index] == 0) {
      intent.flags |= OCUDU_DAPP_SCHEDULER_INTENT_MCS_V1;
      intent.recommended_mcs = decided_mcs[index];
      const auto&   cand     = input.candidates[index];
      const uint8_t table    = valid_mcs_table_id(cand.mcs_table_id) ? cand.mcs_table_id : MCS_TABLE_QAM64;
      const bool    tp       = cand.transform_precoder != 0;
      uint8_t       hi       = max_mcs_for_table(table, tp);
      if ((cand.flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_EXPERT_MCS_VALID_V1) != 0) {
        hi = std::min(hi, cand.expert_mcs_max);
      }
      intent.maximum_mcs = hi;
    }
    if (emit_layers && forbid[index] == 0) {
      intent.flags |= OCUDU_DAPP_SCHEDULER_INTENT_LAYERS_V1;
      intent.nof_layers = decided_layers[index];
    }
    if (emit_prb_limits && forbid[index] == 0 && max_prbs[index] > 0) {
      intent.flags |= OCUDU_DAPP_SCHEDULER_INTENT_PRB_LIMITS_V1;
      intent.min_prbs = min_prbs[index];
      intent.max_prbs = max_prbs[index];
    }
    if (emit_prb_pref && forbid[index] == 0 && assign_nof[index] > 0) {
      intent.flags |= OCUDU_DAPP_SCHEDULER_INTENT_PRB_PREFERENCE_V1;
      intent.preferred_start_prb = assign_start[index];
      intent.preferred_nof_prbs  = assign_nof[index];
    }
    if (emit_tpc && forbid[index] == 0 && input.direction == OCUDU_DAPP_SCHEDULER_UL_V1) {
      intent.flags |= OCUDU_DAPP_SCHEDULER_INTENT_TPC_V1;
      if (config.tpc == scheduler_tpc_mode::hold0db) {
        intent.tpc_command = 1;
      } else {
        float target = input.candidates[index].tpc_target_sinr_db;
        float sinr   = 0.0f;
        bool  have   = (input.candidates[index].flags & OCUDU_DAPP_SCHEDULER_CANDIDATE_SINR_VALID_V1) != 0;
        if (have) {
          sinr = input.candidates[index].sinr_db;
        }
        if (!have) {
          intent.tpc_command = 1;
        } else {
          // If target not set, hold 0 dB.
          if (target == 0.0f && sinr == 0.0f) {
            intent.tpc_command = 1;
          } else if (target == 0.0f) {
            intent.tpc_command = 1;
          } else {
            intent.tpc_command = map_tpc(target - sinr, config.tpc_deadband_db, config.tpc_step_hi_db);
          }
        }
      }
    }
  }

  output.nof_intents         = input.nof_candidates;
  output.flags               = 0;
  output.decision_generation = input.candidate_generation;

  if (publish_avoidance) {
    const auto* interference = reinterpret_cast<const uint8_t*>(input.optional_interference_mask.native_handle);
    auto*       avoidance    = reinterpret_cast<uint8_t*>(output.avoid_prb_mask.native_handle);
    uint32_t    avoided      = 0;
    for (uint32_t prb = 0; prb != input.cell_nof_prbs; ++prb) {
      avoidance[prb] = interference[prb] >= config.prb_avoidance_threshold ? 1 : 0;
      avoided += avoidance[prb];
    }
    if (avoided != 0) {
      output.flags |= OCUDU_DAPP_SCHEDULER_OUTPUT_AVOID_PRB_MASK_V1;
    }
  }

  state.previous_winner     = best_ue;
  state.has_previous_winner = true;
  ++state.invocations;
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 ocudu::dapp::reference::rank_scheduler_candidates(const ocudu_dapp_scheduler_input_v1& input,
                                                                       ocudu_dapp_scheduler_output_v1&      output,
                                                                       const scheduler_reference_config&    config,
                                                                       scheduler_reference_state& state) noexcept
{
  // Legacy entry: force backlog rank only, no MCS/pack/TPC/layers (still respects avoid mask).
  scheduler_reference_config legacy = config;
  legacy.rank                       = scheduler_rank_mode::backlog;
  legacy.olla                       = scheduler_olla_mode::off;
  legacy.mcs                        = scheduler_mcs_mode::off;
  legacy.pack                       = scheduler_pack_mode::off;
  legacy.tpc                        = scheduler_tpc_mode::off;
  legacy.emit_layers                = false;
  return run_scheduler_pipeline(input, output, legacy, state);
}
