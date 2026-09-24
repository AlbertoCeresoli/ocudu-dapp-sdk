// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include <cstdint>

namespace ocudu::dapp::reference {

/// \brief Reference SRS channel estimator: the OCUDU host estimator (srs_estimator_generic_impl), standalone.
///
/// For every transmit port the least-squares estimate is averaged over the SRS symbols; the time alignment of each
/// port is the peak of the zero-padded IDFT of those estimates (DFT time-alignment estimator, fractional peak by
/// curve fitting) and the ports' alignments are averaged. After removing the resulting phase ramp, the wideband
/// coefficient of each port pair is the mean over subcarriers; the noise is what remains of the received pilots once
/// the reconstructed signal is subtracted. The channel matrix is scaled by the inverse noise standard deviation
/// (never above 40 dB SNR), and EPRE, RSRP, noise variance and time alignment follow the host estimator exactly.
///
/// Inputs and outputs are host CF32/F32 tensors laid out as srs_channel_estimator.h describes.
ocudu_dapp_status_v1 estimate_srs_channel(const ocudu_dapp_srs_channel_estimator_input_v1& input,
                                          ocudu_dapp_srs_channel_estimator_output_v1&      output);

/// Validated SRS channel-estimator invocation: pointers into the caller's tensors.
struct srs_invocation {
  const ocudu_dapp_srs_channel_estimator_metadata_v1* srs          = nullptr;
  const void*                                         rx_symbols   = nullptr;
  const void*                                         references   = nullptr;
  void*                                               channel      = nullptr;
  void*                                               measurements = nullptr;
};

/// \brief Checks an invocation against the interface contract.
///
/// Every tensor must be dense, in \p domain (and, for a device domain, on the CUDA backend), with the canonical layout
/// and the shape the metadata implies. Returns false for anything else.
bool read_srs_invocation(const ocudu_dapp_srs_channel_estimator_input_v1&  input,
                         const ocudu_dapp_srs_channel_estimator_output_v1& output, uint32_t domain,
                         srs_invocation& invocation) noexcept;

/// Time-alignment search parameters of the host DFT time-alignment estimator.
struct srs_time_alignment_search {
  /// Zero-padded IDFT size.
  uint32_t dft_size;
  /// Taps searched on each side of the origin.
  uint32_t max_samples;
  /// Correlation sampling rate, dft_size * subcarrier spacing * comb size, in hertz.
  double sampling_rate_hz;
  /// Whether the peak is refined by curve fitting (every IDFT size but the largest).
  bool fractional;
};

/// The search the host estimator runs for \p srs (same DFT size, window and refinement).
srs_time_alignment_search
make_srs_time_alignment_search(const ocudu_dapp_srs_channel_estimator_metadata_v1& srs) noexcept;

/// Fractional peak offset of three or five correlation samples centred on the peak, as the host curve fit computes it.
float srs_fractional_peak(const float* samples, uint32_t nof_samples) noexcept;

} // namespace ocudu::dapp::reference
