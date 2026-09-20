// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu_dapp_sdk/reference_scheduler_olla.h"
#include <array>
#include <cstdint>

namespace ocudu::dapp::reference {

/// Optional, reference-module-specific create arguments.
///
/// The production reference scheduler is deliberately host-policy passthrough.
/// A benchmark or lab host that explicitly owns policy may pass this structure
/// as native_instance::create()'s opaque create_args. The module copies the
/// requested settings during creation; the pointer is not retained.
struct scheduler_reference_create_args_v1 {
  uint32_t struct_size = sizeof(scheduler_reference_create_args_v1);
  uint32_t version     = 1;
  uint8_t  enable_prb_avoidance{};
  uint8_t  reserved[7]{};
};

enum class scheduler_rank_mode : uint8_t { backlog = 0, conventional_passthrough = 1, pf_lite = 2 };

enum class scheduler_olla_mode : uint8_t { off = 0, host_offset_echo = 1, contained_baseline = 2 };

enum class scheduler_mcs_mode : uint8_t { off = 0, host_echo = 1, table_plus_olla = 2 };

/// PRB packing stage policy. `off` preserves host allocation; `limits_only`
/// emits only min/max PRB limits; `soft_prefer_clean` emits soft preferences
/// biased away from interference; the `hard_pack_*` modes paint exclusive
/// assignments into the free-VRB mask, searching from PRB 0 (`first_fit_low`),
/// from the top of the band (`first_fit_high`), or maximising overlap with
/// interference-free PRBs (`prefer_clean`).
enum class scheduler_pack_mode : uint8_t {
  off                      = 0,
  limits_only              = 1,
  soft_prefer_clean        = 2,
  hard_pack_first_fit_low  = 3,
  hard_pack_first_fit_high = 4,
  hard_pack_prefer_clean   = 5
};

enum class scheduler_tpc_mode : uint8_t { off = 0, hold0db = 1, target_sinr = 2 };

/// Policy knobs of the reference Class-B pipeline. The member initializers are
/// the shipped defaults (host-policy passthrough); the module copies the live
/// configuration once per invocation, so E3 `configSet` writes take effect on
/// the next slot.
struct scheduler_reference_config {
  // Ranking (legacy knobs)
  double backlog_weight             = 1.0;
  double scheduling_request_bonus   = 16.0;
  double consecutive_winner_penalty = 0.25;
  double pf_fairness_coeff          = 2.0;

  // Avoid mask (cell-level soft avoid for host first-fit when not packing hard)
  /// Interference-map confidence (0-255) above which a PRB is avoided.
  uint8_t prb_avoidance_threshold = 192;
  bool    enable_prb_avoidance    = false;

  // Safe shipped default: preserve the host scheduler's policy and exercise only
  // the Class-B ranking boundary. Experimental MCS / OLLA / packing / TPC stages
  // are opt-in; host allow-flags are authority bounds, not an instruction to
  // enable every available policy stage.
  scheduler_rank_mode rank = scheduler_rank_mode::conventional_passthrough;
  scheduler_olla_mode olla = scheduler_olla_mode::off;
  scheduler_mcs_mode  mcs  = scheduler_mcs_mode::off;
  scheduler_pack_mode pack = scheduler_pack_mode::off;
  scheduler_tpc_mode  tpc  = scheduler_tpc_mode::off;

  olla_config olla_cfg{};

  /// TPC thresholds (host-like).
  float tpc_deadband_db = 0.5f;
  float tpc_step_hi_db  = 2.5f;

  /// Crude PRB sizing: assume ~12 data symbols, overhead fraction.
  float   prb_bytes_per_re_scale = 1.0f;
  uint8_t assumed_data_symbols   = 12;

  /// Never set intent bits without host allow-flag (production default).
  bool emit_only_allowed = true;

  /// Emit INTENT_LAYERS when host allows (uses candidate RI / nof_layers).
  bool emit_layers = false;

  /// When hard pack cannot place a UE, mark FORBID (true) or leave PRIORITY with nof=0 (false).
  bool forbid_on_pack_fail = false;
};

/// Per-lane mutable pipeline state carried across invocations. The host
/// serializes invocations on a lane, so no internal locking is needed; do not
/// share one instance between lanes.
struct scheduler_reference_state {
  /// Fixed worker-owned workspace: evaluate never allocates, including sorting.
  /// These are reference implementation limits, not a new public C ABI.
  static constexpr uint32_t maximum_candidates = OCUDU_DAPP_SCHEDULER_MAX_CANDIDATES_V1;
  static constexpr uint32_t maximum_prbs       = 275;
  struct workspace {
    std::array<uint8_t, maximum_prbs>        free{};
    std::array<uint8_t, maximum_candidates>  mcs{}, layers{}, forbid{};
    std::array<double, maximum_candidates>   priorities{};
    std::array<uint32_t, maximum_candidates> order{};
    std::array<uint16_t, maximum_candidates> assign_start{}, assign_nof{}, min_prbs{}, max_prbs{};
  } scratch;
  /// Pipeline invocations observed so far.
  uint64_t invocations{};
  /// UE that won the previous slot (consecutive-winner penalty in backlog rank).
  uint32_t previous_winner{};
  bool     has_previous_winner{};
  /// Package-local OLLA offsets per UE (`contained_baseline` mode).
  olla_state_map<256> olla_map{};
};

/// Multi-stage Class-B reference pipeline (rank → olla → mcs → pack → tpc → avoid).
/// The default configuration is host-policy passthrough; policy-owning stages are opt-in.
ocudu_dapp_status_v1 run_scheduler_pipeline(const ocudu_dapp_scheduler_input_v1& input,
                                            ocudu_dapp_scheduler_output_v1&      output,
                                            const scheduler_reference_config&    config,
                                            scheduler_reference_state&           state) noexcept;

/// Minimal entry point: backlog rank + optional avoid only (ignores richer strategy enums).
/// Use run_scheduler_pipeline for the full multi-stage pipeline.
ocudu_dapp_status_v1 rank_scheduler_candidates(const ocudu_dapp_scheduler_input_v1& input,
                                               ocudu_dapp_scheduler_output_v1&      output,
                                               const scheduler_reference_config&    config,
                                               scheduler_reference_state&           state) noexcept;

} // namespace ocudu::dapp::reference
