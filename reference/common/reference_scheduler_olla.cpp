// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_scheduler_olla.h"
#include <algorithm>
#include <cmath>

namespace ocudu::dapp::reference {

float olla_update(float current_offset_db, bool ack_or_crc_ok, uint8_t used_mcs, uint8_t expert_mcs_min,
                  uint8_t expert_mcs_max, const olla_config& cfg) noexcept
{
  if (!(cfg.target_bler > 0.0f && cfg.target_bler < 0.5f) || !(cfg.step_db > 0.0f) || !(cfg.max_offset_db > 0.0f)) {
    return current_offset_db;
  }
  const float delta_down = cfg.step_db;
  const float delta_up   = (1.0f - cfg.target_bler) * cfg.step_db / cfg.target_bler;
  // Saturation guard (host olla_algorithm): no up at max MCS, no down at min MCS.
  const float eff_down = used_mcs >= expert_mcs_max ? 0.0f : delta_down;
  const float eff_up   = used_mcs <= expert_mcs_min ? 0.0f : delta_up;
  float       offset   = current_offset_db + (ack_or_crc_ok ? eff_down : -eff_up);
  offset               = std::max(-cfg.max_offset_db, std::min(cfg.max_offset_db, offset));
  return offset;
}

} // namespace ocudu::dapp::reference
