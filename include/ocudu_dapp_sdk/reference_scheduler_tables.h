// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include <cstdint>
#include <optional>

namespace ocudu::dapp::reference {

/// Portable MCS table ids — match `OCUDU_DAPP_SCHEDULER_MCS_TABLE_*_V1` in scheduler.h.
///   0 = qam64       (TS 38.214 Table 5.1.3.1-1 / UL 6.1.4.1-1)
///   1 = qam256      (TS 38.214 Table 5.1.3.1-2 / UL 6.1.4.1-2)
///   2 = qam64LowSe  (TS 38.214 Table 5.1.3.1-3)
inline constexpr uint8_t MCS_TABLE_QAM64       = 0;
inline constexpr uint8_t MCS_TABLE_QAM256      = 1;
inline constexpr uint8_t MCS_TABLE_QAM64_LOWSE = 2;

/// Human-readable label for logs/docs (never null).
const char* mcs_table_name(uint8_t mcs_table_id) noexcept;

/// True if id is one of {0,1,2}.
bool valid_mcs_table_id(uint8_t mcs_table_id) noexcept;

/// Portable CQI→MCS map (TS 38.214 spectral-efficiency style tables).
/// mcs_table_id selects which CQI/MCS pair of tables (assumes CQI_table_idx == MCS_table_idx).
std::optional<uint8_t> map_cqi_to_mcs_portable(unsigned cqi, uint8_t mcs_table_id) noexcept;

/// Portable UL SNR→MCS ladder (host-calibrated thresholds, table-aware).
/// Transform precoder reduces max MCS to 27 for non-256QAM tables (reserved entries).
uint8_t map_snr_to_mcs_ul_portable(double snr_db, uint8_t mcs_table_id, bool transform_precoder) noexcept;

/// Approximate spectral efficiency (bits/RE) for crude PRB sizing — table-aware.
float mcs_spectral_efficiency(uint8_t mcs, uint8_t mcs_table_id) noexcept;

/// Max legal MCS index for table / transform-precoder combination (host get_max_mcs_ul spirit).
uint8_t max_mcs_for_table(uint8_t mcs_table_id, bool transform_precoder) noexcept;

/// Clamp MCS into [lo, hi] ∩ [0, max_mcs_for_table].
uint8_t clamp_mcs_for_table(int mcs, uint8_t expert_lo, uint8_t expert_hi, uint8_t mcs_table_id, bool tp) noexcept;

} // namespace ocudu::dapp::reference
