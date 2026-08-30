// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/package_config.h"
#include "ocudu_dapp_sdk/reference_scheduler_pipeline.h"
#include <array>
#include <cstring>
#include <mutex>

using namespace ocudu::dapp::reference;

namespace {

// Class-B policy-authoring map
// ----------------------------
// The host runs its conventional newTx policy first, then invokes this module.
// Every call provides direction/cell/slice/radio-time information, the cell PRB
// width and remaining slice budget, plus one candidate record per UE.  Candidate
// records currently contain:
//   * identity, pending bytes/SR, conventional priority/forbid, directional
//     served-rate EMA, and the oldest pending DL SDU's HOL delay;
//   * DL CQI or UL SINR, first-newTx BLER plus observation count, the host MCS
//     and OLLA offset, MCS table/limits, layers;
//   * a fresh DL ACK or UL CRC outcome and its MCS; and
//   * UL transform-precoder, normalized PHR (when available), and target SINR.
// Hard packing additionally receives a free-VRB snapshot.  Interference-aware
// packing/avoidance receives the optional per-PRB interference mask.
//
// The module returns constrained intents; it does not construct a grant.  It may
// always replace candidate priority/forbid.  MCS, PRB limits/preferences/exact
// assignments, TPC, layers, and UL avoidance are emitted only when both the
// selected stage below and input.allowed_output_flags permit them.  The host
// validates every field, preserves HARQ retx/PDCCH/UCI/slice/grant legality, and
// rolls back to the complete conventional decision on error or deadline miss.
//
// Known v1 boundary gaps: UL BSRs do not carry packet timestamps, so HOL_DELAY
// is deliberately invalid in UL (PENDING_SR remains available). Per-flow QoS,
// HARQ-process/retx control, CCE/UCI/DCI choice, precoder/beam candidates, and
// multi-cell/BWP policy are also not exposed. INTENT_BEAM is an ABI placeholder
// with no v1 host authority/commit path and must not be emitted.
//
// This is the intended place to select the reference algorithms.  For example,
// an explicitly policy-owning experiment could uncomment the following lines;
// leave the default-initialized settings in place for baseline-equivalent OTA use.
// Host YAML must separately grant the corresponding scheduler_allow_* flags.
scheduler_reference_config make_reference_config() noexcept
{
  scheduler_reference_config config{};

  // config.rank = scheduler_rank_mode::backlog;
  // config.olla = scheduler_olla_mode::contained_baseline;
  // config.olla_cfg.target_bler = 0.10f;
  // config.mcs = scheduler_mcs_mode::table_plus_olla;
  // config.pack = scheduler_pack_mode::hard_pack_prefer_clean;
  // config.tpc = scheduler_tpc_mode::target_sinr;
  // config.emit_layers = true;
  // config.enable_prb_avoidance = true;

  return config;
}

/// Enumerated policy modes are configured by name so an operator (or an MCP
/// companion) can read and write them without knowing the numeric encoding.
template <typename Enum, std::size_t N>
struct enum_names {
  std::array<std::pair<const char*, Enum>, N> table;
  const char*                                 name(Enum value) const noexcept
  {
    for (const auto& [text, item] : table) {
      if (item == value) {
        return text;
      }
    }
    return "unknown";
  }
  bool parse(const char* text, Enum& out) const noexcept
  {
    for (const auto& [name_text, item] : table) {
      if (std::strcmp(name_text, text) == 0) {
        out = item;
        return true;
      }
    }
    return false;
  }
};

constexpr enum_names<scheduler_rank_mode, 3> kRankModes{
    {{{"conventional_passthrough", scheduler_rank_mode::conventional_passthrough},
      {"backlog", scheduler_rank_mode::backlog},
      {"pf_lite", scheduler_rank_mode::pf_lite}}}};
constexpr enum_names<scheduler_olla_mode, 3> kOllaModes{
    {{{"off", scheduler_olla_mode::off},
      {"host_offset_echo", scheduler_olla_mode::host_offset_echo},
      {"contained_baseline", scheduler_olla_mode::contained_baseline}}}};
constexpr enum_names<scheduler_mcs_mode, 3>  kMcsModes{{{{"off", scheduler_mcs_mode::off},
                                                         {"host_echo", scheduler_mcs_mode::host_echo},
                                                         {"table_plus_olla", scheduler_mcs_mode::table_plus_olla}}}};
constexpr enum_names<scheduler_pack_mode, 6> kPackModes{
    {{{"off", scheduler_pack_mode::off},
      {"limits_only", scheduler_pack_mode::limits_only},
      {"soft_prefer_clean", scheduler_pack_mode::soft_prefer_clean},
      {"hard_pack_first_fit_low", scheduler_pack_mode::hard_pack_first_fit_low},
      {"hard_pack_first_fit_high", scheduler_pack_mode::hard_pack_first_fit_high},
      {"hard_pack_prefer_clean", scheduler_pack_mode::hard_pack_prefer_clean}}}};
constexpr enum_names<scheduler_tpc_mode, 3> kTpcModes{{{{"off", scheduler_tpc_mode::off},
                                                        {"hold0db", scheduler_tpc_mode::hold0db},
                                                        {"target_sinr", scheduler_tpc_mode::target_sinr}}}};

struct scheduler_model_state {
  /// Host-policy passthrough by default. A host authority grant only bounds what
  /// a configured policy may emit; it must not activate experimental policy.
  /// The live configuration is replaced atomically by set_config; invocations
  /// copy it under the lock once per slot (a few dozen bytes).
  scheduler_reference_config config = make_reference_config();
  mutable std::mutex         config_mutex;

  scheduler_model_state() = default;

  explicit scheduler_model_state(const void* create_args) noexcept
  {
    if (create_args == nullptr) {
      return;
    }
    const auto& args = *static_cast<const scheduler_reference_create_args_v1*>(create_args);
    if (args.struct_size < sizeof(scheduler_reference_create_args_v1) || args.version != 1) {
      return;
    }
    config.enable_prb_avoidance = args.enable_prb_avoidance != 0;
  }

  scheduler_reference_config snapshot() const noexcept
  {
    std::lock_guard<std::mutex> lock(config_mutex);
    return config;
  }
};

struct scheduler_lane_state {
  scheduler_reference_state history;
};

using scheduler_context = stateful_module_context<scheduler_model_state, scheduler_lane_state>;

// ---------------------------------------------------------------------------
// E3-visible configuration: scheduler targets, back-off weights and policy modes.
// Read through get_config, written through set_config (validated, generation
// checked, applied atomically). Keys are stable API for tooling such as
// ocudu_dappctl and the OCUDU MCP server.
// ---------------------------------------------------------------------------
namespace config {
using namespace ocudu::dapp::package_config;

struct scratch {
  scheduler_reference_config config;
  bool                       dirty{false};
};

scheduler_model_state* model_of(void* instance) noexcept
{
  return scheduler_context::shared_state_from_instance(instance);
}

void begin_update(void* instance, void* raw) noexcept
{
  auto* s   = static_cast<scratch*>(raw);
  auto* m   = model_of(instance);
  s->config = m != nullptr ? m->snapshot() : make_reference_config();
  s->dirty  = false;
}

ocudu_dapp_status_v1 validate(void*, void* raw) noexcept
{
  const auto& c  = static_cast<scratch*>(raw)->config;
  const bool  ok = c.olla_cfg.target_bler > 0.0f && c.olla_cfg.target_bler < 0.5f && c.olla_cfg.step_db > 0.0f &&
                  c.olla_cfg.step_db <= 1.0f && c.olla_cfg.max_offset_db >= 0.0f && c.olla_cfg.max_offset_db <= 20.0f &&
                  c.tpc_deadband_db >= 0.0f && c.tpc_deadband_db <= 10.0f && c.tpc_step_hi_db >= 0.0f &&
                  c.tpc_step_hi_db <= 10.0f && c.backlog_weight >= 0.0 && c.scheduling_request_bonus >= 0.0 &&
                  c.consecutive_winner_penalty >= 0.0 && c.consecutive_winner_penalty <= 1.0 &&
                  c.pf_fairness_coeff >= 0.0 && c.assumed_data_symbols >= 1 && c.assumed_data_symbols <= 14 &&
                  c.prb_bytes_per_re_scale > 0.0f && c.prb_bytes_per_re_scale <= 4.0f;
  return ok ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
}

ocudu_dapp_status_v1 commit(void* instance, void* raw) noexcept
{
  auto* s = static_cast<scratch*>(raw);
  auto* m = model_of(instance);
  if (m == nullptr) {
    return OCUDU_DAPP_INTERNAL_ERROR_V1;
  }
  if (!s->dirty) {
    return OCUDU_DAPP_OK_V1;
  }
  std::lock_guard<std::mutex> lock(m->config_mutex);
  m->config = s->config;
  return OCUDU_DAPP_OK_V1;
}

// Field accessors: one tiny struct per configurable field so the same get/stage
// templates serve top-level and nested members alike.
using cfg = scheduler_reference_config;
#define DEFINE_FIELD(name, type, expr)                                                                                 \
  struct field_##name {                                                                                                \
    using value_type = type;                                                                                           \
    static type& ref(cfg& c) noexcept                                                                                  \
    {                                                                                                                  \
      return c.expr;                                                                                                   \
    }                                                                                                                  \
    static const type& ref(const cfg& c) noexcept                                                                      \
    {                                                                                                                  \
      return c.expr;                                                                                                   \
    }                                                                                                                  \
  };
DEFINE_FIELD(olla_target_bler, float, olla_cfg.target_bler)
DEFINE_FIELD(olla_step_db, float, olla_cfg.step_db)
DEFINE_FIELD(olla_max_offset_db, float, olla_cfg.max_offset_db)
DEFINE_FIELD(tpc_deadband_db, float, tpc_deadband_db)
DEFINE_FIELD(tpc_step_hi_db, float, tpc_step_hi_db)
DEFINE_FIELD(backlog_weight, double, backlog_weight)
DEFINE_FIELD(scheduling_request_bonus, double, scheduling_request_bonus)
DEFINE_FIELD(consecutive_winner_penalty, double, consecutive_winner_penalty)
DEFINE_FIELD(pf_fairness_coeff, double, pf_fairness_coeff)
DEFINE_FIELD(prb_avoidance_threshold, uint8_t, prb_avoidance_threshold)
DEFINE_FIELD(assumed_data_symbols, uint8_t, assumed_data_symbols)
DEFINE_FIELD(prb_bytes_per_re_scale, float, prb_bytes_per_re_scale)
DEFINE_FIELD(enable_prb_avoidance, bool, enable_prb_avoidance)
DEFINE_FIELD(emit_layers, bool, emit_layers)
DEFINE_FIELD(emit_only_allowed, bool, emit_only_allowed)
DEFINE_FIELD(forbid_on_pack_fail, bool, forbid_on_pack_fail)
DEFINE_FIELD(rank, scheduler_rank_mode, rank)
DEFINE_FIELD(olla, scheduler_olla_mode, olla)
DEFINE_FIELD(mcs, scheduler_mcs_mode, mcs)
DEFINE_FIELD(pack, scheduler_pack_mode, pack)
DEFINE_FIELD(tpc, scheduler_tpc_mode, tpc)
#undef DEFINE_FIELD

template <typename Field>
ocudu_dapp_status_v1 get_number(void* instance, ocudu_dapp_value_v1* out) noexcept
{
  auto* m = model_of(instance);
  if (m == nullptr) {
    return OCUDU_DAPP_INTERNAL_ERROR_V1;
  }
  const cfg current = m->snapshot();
  *out              = value_double(static_cast<double>(Field::ref(current)));
  return OCUDU_DAPP_OK_V1;
}

template <typename Field>
ocudu_dapp_status_v1 stage_number(void*, const ocudu_dapp_value_v1* in, void* raw) noexcept
{
  double value = 0.0;
  if (!parse_double(*in, value)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto* s               = static_cast<scratch*>(raw);
  Field::ref(s->config) = static_cast<typename Field::value_type>(value);
  s->dirty              = true;
  return OCUDU_DAPP_OK_V1;
}

template <typename Field>
ocudu_dapp_status_v1 get_flag(void* instance, ocudu_dapp_value_v1* out) noexcept
{
  auto* m = model_of(instance);
  if (m == nullptr) {
    return OCUDU_DAPP_INTERNAL_ERROR_V1;
  }
  const cfg current = m->snapshot();
  *out              = value_bool(Field::ref(current));
  return OCUDU_DAPP_OK_V1;
}

template <typename Field>
ocudu_dapp_status_v1 stage_flag(void*, const ocudu_dapp_value_v1* in, void* raw) noexcept
{
  bool value = false;
  if (!parse_bool(*in, value)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto* s               = static_cast<scratch*>(raw);
  Field::ref(s->config) = value;
  s->dirty              = true;
  return OCUDU_DAPP_OK_V1;
}

template <typename Field, const auto& Names>
ocudu_dapp_status_v1 get_mode(void* instance, ocudu_dapp_value_v1* out) noexcept
{
  auto* m = model_of(instance);
  if (m == nullptr) {
    return OCUDU_DAPP_INTERNAL_ERROR_V1;
  }
  const cfg current = m->snapshot();
  *out              = value_string(Names.name(Field::ref(current)));
  return OCUDU_DAPP_OK_V1;
}

template <typename Field, const auto& Names>
ocudu_dapp_status_v1 stage_mode(void*, const ocudu_dapp_value_v1* in, void* raw) noexcept
{
  char text[48] = {};
  if (!parse_string(*in, text, sizeof(text) - 1)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto* s = static_cast<scratch*>(raw);
  if (!Names.parse(text, Field::ref(s->config))) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  s->dirty = true;
  return OCUDU_DAPP_OK_V1;
}

#define NUMBER_KEY(key, text, name, lo, hi)                                                                            \
  double_key(key, text, static_cast<double>(field_##name::ref(cfg{})), lo, hi, get_number<field_##name>,               \
             stage_number<field_##name>)
#define FLAG_KEY(key, text, name)                                                                                      \
  bool_key(key, text, field_##name::ref(cfg{}), get_flag<field_##name>, stage_flag<field_##name>)
#define MODE_KEY(key, text, name, names)                                                                               \
  string_key(key, text, get_mode<field_##name, names>, stage_mode<field_##name, names>)

const config_key kKeys[] = {
    instance_enabled_key(),
    MODE_KEY("rank_mode", "UE ranking policy: conventional_passthrough | backlog | pf_lite", rank, kRankModes),
    MODE_KEY("olla_mode", "Outer-loop link adaptation: off | host_offset_echo | contained_baseline", olla, kOllaModes),
    MODE_KEY("mcs_mode", "MCS selection: off | host_echo | table_plus_olla", mcs, kMcsModes),
    MODE_KEY(
        "pack_mode",
        "PRB packing: off | limits_only | soft_prefer_clean | hard_pack_first_fit_low | hard_pack_first_fit_high | "
        "hard_pack_prefer_clean",
        pack, kPackModes),
    MODE_KEY("tpc_mode", "Uplink power control: off | hold0db | target_sinr", tpc, kTpcModes),
    NUMBER_KEY("olla_target_bler", "OLLA target BLER for MCS back-off (0..0.5)", olla_target_bler, 0.001, 0.5),
    NUMBER_KEY("olla_step_db", "OLLA offset step per ACK/NACK in dB", olla_step_db, 0.001, 1.0),
    NUMBER_KEY("olla_max_offset_db", "OLLA offset clamp in dB", olla_max_offset_db, 0.0, 20.0),
    NUMBER_KEY("tpc_deadband_db", "TPC dead band around the target SINR in dB", tpc_deadband_db, 0.0, 10.0),
    NUMBER_KEY("tpc_step_hi_db", "TPC error above which the large step is used, dB", tpc_step_hi_db, 0.0, 10.0),
    NUMBER_KEY("backlog_weight", "Ranking weight of buffered bytes", backlog_weight, 0.0, 100.0),
    NUMBER_KEY("scheduling_request_bonus", "Ranking bonus for a pending scheduling request", scheduling_request_bonus,
               0.0, 1000.0),
    NUMBER_KEY("consecutive_winner_penalty", "Back-off applied to a UE scheduled in consecutive slots (0..1)",
               consecutive_winner_penalty, 0.0, 1.0),
    NUMBER_KEY("pf_fairness_coeff", "Proportional-fair fairness exponent", pf_fairness_coeff, 0.0, 10.0),
    NUMBER_KEY("prb_avoidance_threshold", "Interference-map confidence above which a PRB is avoided (0..255)",
               prb_avoidance_threshold, 0.0, 255.0),
    NUMBER_KEY("assumed_data_symbols", "Data symbols per slot assumed when sizing PRB grants", assumed_data_symbols,
               1.0, 14.0),
    NUMBER_KEY("prb_bytes_per_re_scale", "Overhead scale applied to bytes-per-RE when sizing PRB grants",
               prb_bytes_per_re_scale, 0.1, 4.0),
    FLAG_KEY("enable_prb_avoidance", "Honour the interference-map avoid mask", enable_prb_avoidance),
    FLAG_KEY("emit_layers", "Emit layer intents from candidate RI when the host allows", emit_layers),
    FLAG_KEY("emit_only_allowed", "Never emit intents the host has not allowed", emit_only_allowed),
    FLAG_KEY("forbid_on_pack_fail", "Mark FORBID when hard packing cannot place a UE", forbid_on_pack_fail),
};
#undef NUMBER_KEY
#undef FLAG_KEY
#undef MODE_KEY

const config_schema kSchema{
    kKeys, static_cast<uint32_t>(sizeof(kKeys) / sizeof(kKeys[0])), sizeof(scratch), begin_update, validate, commit};

const ocudu_dapp_management_v1& management_api() noexcept
{
  static const ocudu_dapp_management_v1& api = make_config_management(kSchema);
  return api;
}
} // namespace config

ocudu_dapp_status_v1 invoke(void* context, const ocudu_dapp_scheduler_input_v1* input,
                            ocudu_dapp_scheduler_output_v1* output) noexcept
{
  if (input == nullptr || output == nullptr)
    return OCUDU_DAPP_INVALID_INPUT_V1;
  const auto status = scheduler_context::begin_invocation(context);
  if (status != OCUDU_DAPP_OK_V1)
    return status;
  const auto* model = scheduler_context::shared_state(context);
  auto*       lane  = scheduler_context::worker_state(context);
  if (model == nullptr || lane == nullptr)
    return OCUDU_DAPP_INTERNAL_ERROR_V1;
  const scheduler_reference_config config = model->snapshot();
  return guard_algorithm_call([&] { return run_scheduler_pipeline(*input, *output, config, lane->history); });
}
const ocudu_dapp_scheduler_interface_v1 api = {scheduler_context::worker_api(), invoke, {}};
const auto descriptor = make_interface(OCUDU_DAPP_INTERFACE_SCHEDULER_V1, OCUDU_DAPP_CLASS_B_DEADLINE_V1,
                                       OCUDU_DAPP_PLACEMENT_IN_PROCESS_V1, OCUDU_DAPP_THREADING_PER_WORKER_V1, &api);
} // namespace

extern "C" OCUDU_DAPP_EXPORT_V1 ocudu_dapp_status_v1 ocudu_dapp_query_v1(const ocudu_dapp_host_info_v1* host,
                                                                         ocudu_dapp_module_v1*          output)
{
  const auto status = scheduler_context::populate(host, output, "org.ocudu.reference.scheduler.cpu", &descriptor);
  if (status == OCUDU_DAPP_OK_V1 && output != nullptr) {
    output->management = &config::management_api();
  }
  return status;
}
