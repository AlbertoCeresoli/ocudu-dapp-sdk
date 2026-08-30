// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_scheduler_tables.h"
#include "ocudu/dapp/use_cases/v1/scheduler.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace ocudu::dapp::reference {
namespace {

constexpr unsigned CQI_TABLE_SIZE = 16;

// Spectral-efficiency CQI→MCS maps (portable copy of host mcs_calculator).
// Index 0: qam64, 1: qam256, 2: qam64LowSe. Entry 0 is invalid CQI.
constexpr int cqi_to_mcs_table[3][CQI_TABLE_SIZE] = {{-1, 0, 0, 2, 4, 6, 8, 11, 13, 15, 18, 20, 22, 24, 26, 28},
                                                     {-1, 0, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27},
                                                     {-1, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28}};

// Host-calibrated min SNR (dB) for each MCS — 64QAM / 64QAM-LowSE ladders (29 entries).
constexpr std::array<double, 29> ul_snr_qam64 = {-5.7998, -3.5500, -2.925,  -2.5625, -1.0500, 0.98266, 1.6250,  2.5425,
                                                 3.4175,  4.3548,  5.3695,  5.8250,  6.6375,  7.6375,  9.5875,  10.4000,
                                                 11.1540, 12.1070, 12.5250, 13.0625, 13.5250, 13.9375, 14.1160, 14.5525,
                                                 14.9725, 15.3450, 15.9175, 16.0425, 16.591};

// 256QAM ladder (28 entries; MCS 0..27).
constexpr std::array<double, 28> ul_snr_qam256 = {1.7998,  3.5500,  4.925,   5.5625,  6.0500,  7.98266, 8.6250,
                                                  9.5425,  10.4175, 11.3548, 12.3695, 12.8250, 13.1375, 13.8475,
                                                  14.6875, 15.554,  15.9540, 16.1070, 16.8250, 17.0625, 17.4250,
                                                  17.9375, 18.1160, 18.5525, 18.8725, 19.0150, 20.591,  21.691};

// Rough SE (bits/RE) per MCS for PRB sizing — approximate, not full TBS tables.
// qam64 / qam64LowSe share envelope; 256QAM stretches higher SE at high MCS.
constexpr float se_qam64[29] = {0.23f, 0.31f, 0.38f, 0.49f, 0.60f, 0.74f, 0.88f, 1.03f, 1.18f, 1.33f,
                                1.48f, 1.70f, 1.91f, 2.16f, 2.41f, 2.57f, 2.73f, 3.03f, 3.32f, 3.61f,
                                3.90f, 4.21f, 4.52f, 4.82f, 5.12f, 5.33f, 5.55f, 5.78f, 6.00f};

// 256QAM: slightly higher SE at upper MCS (crude).
constexpr float se_qam256[28] = {0.23f, 0.38f, 0.49f, 0.60f, 0.74f, 0.88f, 1.03f, 1.18f, 1.48f, 1.70f,
                                 1.91f, 2.16f, 2.41f, 2.73f, 3.03f, 3.32f, 3.61f, 3.90f, 4.21f, 4.52f,
                                 4.82f, 5.12f, 5.55f, 5.78f, 6.20f, 6.60f, 7.00f, 7.40f};

// LowSE: slightly lower SE for same MCS index (more robust coding).
constexpr float se_qam64_lowse[29] = {0.15f, 0.23f, 0.31f, 0.38f, 0.49f, 0.60f, 0.74f, 0.88f, 1.03f, 1.18f,
                                      1.33f, 1.48f, 1.70f, 1.91f, 2.16f, 2.41f, 2.57f, 2.73f, 3.03f, 3.32f,
                                      3.61f, 3.90f, 4.21f, 4.52f, 4.82f, 5.12f, 5.33f, 5.55f, 5.78f};

uint8_t resolve_table(uint8_t mcs_table_id) noexcept
{
  return mcs_table_id <= MCS_TABLE_QAM64_LOWSE ? mcs_table_id : MCS_TABLE_QAM64;
}

} // namespace

const char* mcs_table_name(uint8_t mcs_table_id) noexcept
{
  switch (mcs_table_id) {
    case MCS_TABLE_QAM64:
      return "qam64";
    case MCS_TABLE_QAM256:
      return "qam256";
    case MCS_TABLE_QAM64_LOWSE:
      return "qam64LowSe";
    default:
      return "unknown";
  }
}

bool valid_mcs_table_id(uint8_t mcs_table_id) noexcept
{
  return mcs_table_id <= MCS_TABLE_QAM64_LOWSE;
}

std::optional<uint8_t> map_cqi_to_mcs_portable(unsigned cqi, uint8_t mcs_table_id) noexcept
{
  if (cqi == 0 || cqi >= CQI_TABLE_SIZE || !valid_mcs_table_id(mcs_table_id)) {
    return std::nullopt;
  }
  const int mcs = cqi_to_mcs_table[mcs_table_id][cqi];
  if (mcs < 0) {
    return std::nullopt;
  }
  return static_cast<uint8_t>(mcs);
}

uint8_t map_snr_to_mcs_ul_portable(double snr_db, uint8_t mcs_table_id, bool transform_precoder) noexcept
{
  const uint8_t table = resolve_table(mcs_table_id);
  const double* begin = ul_snr_qam64.data();
  size_t        n     = ul_snr_qam64.size();
  if (table == MCS_TABLE_QAM256) {
    begin = ul_snr_qam256.data();
    n     = ul_snr_qam256.size();
  }
  // LowSE reuses the 64QAM SNR ladder (same as host mcs_calculator today).
  // Transform precoder: drop reserved last entry on non-256 tables.
  if (transform_precoder && table != MCS_TABLE_QAM256 && n > 28) {
    n = 28;
  }
  if (snr_db <= begin[0]) {
    return 0;
  }
  if (snr_db >= begin[n - 1]) {
    return static_cast<uint8_t>(n - 1);
  }
  size_t i = 1;
  while (i < n && begin[i] <= snr_db) {
    ++i;
  }
  return static_cast<uint8_t>(i - 1);
}

float mcs_spectral_efficiency(uint8_t mcs, uint8_t mcs_table_id) noexcept
{
  const uint8_t table = resolve_table(mcs_table_id);
  if (table == MCS_TABLE_QAM256) {
    return se_qam256[std::min<uint8_t>(mcs, 27)];
  }
  if (table == MCS_TABLE_QAM64_LOWSE) {
    return se_qam64_lowse[std::min<uint8_t>(mcs, 28)];
  }
  return se_qam64[std::min<uint8_t>(mcs, 28)];
}

uint8_t max_mcs_for_table(uint8_t mcs_table_id, bool transform_precoder) noexcept
{
  const uint8_t table = resolve_table(mcs_table_id);
  // Host get_max_mcs_ul: 256QAM or TP → 27; else 28.
  if (table == MCS_TABLE_QAM256 || transform_precoder) {
    return 27;
  }
  return 28;
}

uint8_t clamp_mcs_for_table(int mcs, uint8_t expert_lo, uint8_t expert_hi, uint8_t mcs_table_id, bool tp) noexcept
{
  const uint8_t table_max = max_mcs_for_table(mcs_table_id, tp);
  uint8_t       lo        = expert_lo;
  uint8_t       hi        = std::min(expert_hi, table_max);
  if (lo > hi) {
    lo = 0;
    hi = table_max;
  }
  if (mcs < static_cast<int>(lo)) {
    return lo;
  }
  if (mcs > static_cast<int>(hi)) {
    return hi;
  }
  return static_cast<uint8_t>(mcs);
}

} // namespace ocudu::dapp::reference
