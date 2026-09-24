// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ocudu/dapp/use_cases/v1/srs_channel_estimator.h"
#include <array>
#include <complex>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ocudu::dapp::sdk {

/// \file
/// \brief SRS channel-estimator vector files ("OSRSCEV1").
///
/// A vector pairs the exact input an OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1 module receives with the gNB's
/// answer for the same occasion. OCUDU writes the golden set (srs_dapp_channel_estimator_vectors, host estimator);
/// examples/e3_srs_dataset/export_srs_vectors.py writes occasions captured from a live cell (the live estimator).
/// Layout, little endian: "OSRSCEV1", u32 version (1), u32 case count, then per case a u32 metadata size, the
/// metadata bytes, rx_symbols (CF32), reference_symbols (CF32), the channel matrix (CF32 [rx, tx]), the
/// measurements (F32, NaN when absent) and the time alignment in double precision (value, resolution, min, max).

/// One SRS occasion: the module input and the gNB's answer.
struct srs_vector {
  ocudu_dapp_srs_channel_estimator_metadata_v1           metadata{};
  std::vector<std::complex<float>>                       rx_symbols;
  std::vector<std::complex<float>>                       references;
  std::vector<std::complex<float>>                       channel;
  std::array<float, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1> measurements{};
  /// Time alignment in double precision: value, resolution, min, max.
  std::array<double, 4> time_alignment{};
};

/// Reads a vector file; throws std::runtime_error when it is missing, truncated or not a vector file.
inline std::vector<srs_vector> read_srs_vectors(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  const auto read = [&file](void* data, size_t size) {
    if (!file.read(static_cast<char*>(data), static_cast<std::streamsize>(size))) {
      throw std::runtime_error("truncated SRS vector file");
    }
  };
  char     magic[8];
  uint32_t version = 0;
  uint32_t count   = 0;
  read(magic, sizeof(magic));
  read(&version, sizeof(version));
  read(&count, sizeof(count));
  if (std::memcmp(magic, "OSRSCEV1", sizeof(magic)) != 0 || version != 1) {
    throw std::runtime_error("not an SRS channel-estimator vector file");
  }
  std::vector<srs_vector> vectors(count);
  for (auto& vector : vectors) {
    uint32_t metadata_size = 0;
    read(&metadata_size, sizeof(metadata_size));
    if (metadata_size != sizeof(vector.metadata)) {
      throw std::runtime_error("unexpected SRS metadata size");
    }
    read(&vector.metadata, sizeof(vector.metadata));
    const auto& srs = vector.metadata;
    vector.rx_symbols.resize(static_cast<size_t>(srs.nof_pilot_sets) * srs.nof_rx_ports * srs.nof_symbols *
                             srs.sequence_length);
    vector.references.resize(static_cast<size_t>(srs.nof_tx_ports) * srs.sequence_length);
    vector.channel.resize(static_cast<size_t>(srs.nof_rx_ports) * srs.nof_tx_ports);
    read(vector.rx_symbols.data(), vector.rx_symbols.size() * sizeof(std::complex<float>));
    read(vector.references.data(), vector.references.size() * sizeof(std::complex<float>));
    read(vector.channel.data(), vector.channel.size() * sizeof(std::complex<float>));
    read(vector.measurements.data(), sizeof(vector.measurements));
    read(vector.time_alignment.data(), sizeof(vector.time_alignment));
  }
  return vectors;
}

/// Tolerances of a reference implementation against the host estimator. Both run in single precision with different
/// summation orders and transforms, so they agree to rounding rather than to the bit: over the golden set the CPU and
/// CUDA references stay within 4e-7 (channel, noise), 1.1e-5 dB (EPRE, RSRP) and 1e-6 of a correlation tap (time
/// alignment). The bounds leave an order of magnitude of margin; an algorithmic difference exceeds them by far.
struct srs_tolerance {
  /// Relative error of the channel matrix (Frobenius norm).
  double channel = 1e-5;
  /// EPRE and RSRP, dB.
  double power_db = 1e-4;
  /// Relative error of the noise variance.
  double noise = 1e-5;
  /// Time alignment, as a fraction of the correlation resolution.
  double time_alignment = 1e-4;
};

} // namespace ocudu::dapp::sdk
