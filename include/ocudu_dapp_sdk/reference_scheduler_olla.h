// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include <cstdint>

namespace ocudu::dapp::reference {

/// Package-local outer-loop link adaptation (baseline replica of host olla_algorithm).
struct olla_config {
  float target_bler   = 0.1f;
  float step_db       = 0.01f; // delta_down
  float max_offset_db = 5.0f;
};

/// Per-UE OLLA tracking entry stored in \ref olla_state_map.
struct olla_ue_entry {
  uint32_t ue_index = 0;
  /// Current outer-loop offset in dB, clamped to +/- olla_config::max_offset_db.
  float offset_db = 0.0f;
  /// MCS this pipeline decided for the UE last time (valid when
  /// `has_last_mcs`); feedback is applied only when the reported used MCS
  /// matches it.
  uint8_t last_decided_mcs = 0;
  bool    has_last_mcs     = false;
  /// Slot-occupancy flag of the open-address map (internal bookkeeping).
  bool occupied = false;
};

/// Fixed-capacity open-address map (no heap). Capacity must be power of two.
/// Linear probing keyed by `ue_index`; when the whole table is probed without
/// a hit, `find_or_insert` evicts the entry at the home slot (bounded, not
/// LRU-perfect). Not internally synchronized — confine each instance to one
/// lane's \ref scheduler_reference_state.
template <uint32_t Capacity>
struct olla_state_map {
  static_assert((Capacity & (Capacity - 1u)) == 0u && Capacity >= 8u, "Capacity must be power of two >= 8");
  olla_ue_entry entries[Capacity]{};

  olla_ue_entry* find_or_insert(uint32_t ue_index) noexcept
  {
    uint32_t slot = ue_index & (Capacity - 1u);
    for (uint32_t i = 0; i != Capacity; ++i) {
      auto& e = entries[(slot + i) & (Capacity - 1u)];
      if (!e.occupied) {
        e          = {};
        e.occupied = true;
        e.ue_index = ue_index;
        return &e;
      }
      if (e.ue_index == ue_index) {
        return &e;
      }
    }
    // Evict probed slot (bounded; not LRU-perfect).
    auto& e    = entries[slot];
    e          = {};
    e.occupied = true;
    e.ue_index = ue_index;
    return &e;
  }

  const olla_ue_entry* find(uint32_t ue_index) const noexcept
  {
    uint32_t slot = ue_index & (Capacity - 1u);
    for (uint32_t i = 0; i != Capacity; ++i) {
      const auto& e = entries[(slot + i) & (Capacity - 1u)];
      if (!e.occupied) {
        return nullptr;
      }
      if (e.ue_index == ue_index) {
        return &e;
      }
    }
    return nullptr;
  }
};

/// Update offset from ACK/CRC outcome. Returns new offset.
float olla_update(float current_offset_db, bool ack_or_crc_ok, uint8_t used_mcs, uint8_t expert_mcs_min,
                  uint8_t expert_mcs_max, const olla_config& cfg) noexcept;

} // namespace ocudu::dapp::reference
