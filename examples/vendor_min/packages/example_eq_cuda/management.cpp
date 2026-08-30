// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Package knobs via the shared A/B/C package_config core.
//
// This is the multi-vendor pattern: declare a static schema; do not hand-roll
// describe_config / get_config / set_config. See docs/package_config.md.
//
// Keys here:
//   enabled     — built-in; host can force conventional PHY (BYPASS)
//   demo_scale  — example lab double (does not alter certifier goldens)

#include "ocudu_dapp_sdk/package_config.h"
#include <mutex>

using namespace ocudu::dapp::package_config;

namespace ocudu::dapp::example {
namespace {

// Process-wide lab knob. Production packages often hang knobs on a type that
// derives from reference::instance instead; either way, keep set_config under
// management_mutex (the core already locks for generation / dry-run).
std::mutex g_mu;
double     g_demo_scale = 1.0;

struct update_scratch {
  double scale{1.0};
  bool   dirty{false};
};

void begin_update(void*, void* scratch) noexcept
{
  auto*                       u = static_cast<update_scratch*>(scratch);
  std::lock_guard<std::mutex> lock(g_mu);
  u->scale = g_demo_scale;
  u->dirty = false;
}

ocudu_dapp_status_v1 validate(void*, void* scratch) noexcept
{
  const double s = static_cast<update_scratch*>(scratch)->scale;
  return (s > 0.0 && s <= 10.0) ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INVALID_INPUT_V1;
}

ocudu_dapp_status_v1 commit(void*, void* scratch) noexcept
{
  auto* u = static_cast<update_scratch*>(scratch);
  if (!u->dirty) {
    return OCUDU_DAPP_OK_V1;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  g_demo_scale = u->scale;
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 get_scale(void*, ocudu_dapp_value_v1* out) noexcept
{
  std::lock_guard<std::mutex> lock(g_mu);
  *out = value_double(g_demo_scale);
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 stage_scale(void*, const ocudu_dapp_value_v1* in, void* scratch) noexcept
{
  auto* u = static_cast<update_scratch*>(scratch);
  if (!parse_double(*in, u->scale)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  u->dirty = true;
  return OCUDU_DAPP_OK_V1;
}

// Static storage: make_config_management keeps a process-lifetime reference.
const config_key kKeys[] = {
    instance_enabled_key(),
    double_key("demo_scale", "Example lab scale (1.0 default; does not alter goldens)", 1.0, 0.0, 10.0, get_scale,
               stage_scale),
};

const config_schema kSchema{
    kKeys,  static_cast<uint32_t>(sizeof(kKeys) / sizeof(kKeys[0])), sizeof(update_scratch), begin_update, validate,
    commit,
};

} // namespace

const ocudu_dapp_management_v1& example_eq_management_api() noexcept
{
  static const ocudu_dapp_management_v1& api = make_config_management(kSchema);
  return api;
}

double example_demo_scale() noexcept
{
  std::lock_guard<std::mutex> lock(g_mu);
  return g_demo_scale;
}

} // namespace ocudu::dapp::example
