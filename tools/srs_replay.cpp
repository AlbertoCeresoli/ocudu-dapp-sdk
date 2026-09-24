// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

/// \file
/// \brief Replays SRS channel-estimator vectors through the reference estimator.
///
///     ocudu-dapp-srs-replay VECTORS [--check] [--quiet]
///
/// Runs the CPU reference SRS channel estimator on every occasion of a vector file (the SDK's golden set, or occasions
/// exported from a live capture by examples/e3_srs_dataset/export_srs_vectors.py) and compares its channel matrix and
/// measurements with the answer recorded in the file. Recorded values that are NaN are not compared. With --check the
/// tool exits 1 when an occasion leaves the golden-set agreement bounds.

#include "ocudu_dapp_sdk/reference_srs_channel_estimator.h"
#include "ocudu_dapp_sdk/srs_channel_estimator_vectors.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <vector>

using namespace ocudu::dapp;

namespace {

ocudu_dapp_tensor_view_v1 host_tensor(void* data, uint32_t element_type, uint32_t element_size, uint32_t access,
                                      uint32_t layout_id, std::initializer_list<uint64_t> dimensions)
{
  ocudu_dapp_tensor_view_v1 view{};
  view.struct_size           = sizeof(view);
  view.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  view.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  view.domain                = OCUDU_DAPP_MEM_HOST_V1;
  view.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
  view.element_type          = element_type;
  view.rank                  = dimensions.size();
  view.access                = access | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
  view.allocation_id         = 1;
  view.allocation_generation = 1;
  view.lease_token           = 1;
  view.native_handle         = reinterpret_cast<uintptr_t>(data);
  view.layout_id             = layout_id;
  view.layout_version        = 1;
  uint64_t elements          = 1;
  uint32_t index             = 0;
  for (uint64_t dimension : dimensions) {
    view.dimensions[index++] = dimension;
    elements *= dimension;
  }
  uint64_t stride = element_size;
  for (uint32_t dimension = view.rank; dimension != 0; --dimension) {
    view.byte_strides[dimension - 1] = stride;
    stride *= view.dimensions[dimension - 1];
  }
  view.byte_length = elements * element_size;
  return view;
}

/// Differences of one occasion; NaN where the file records no value.
struct comparison {
  double channel;
  double epre_db;
  double rsrp_db;
  double noise;
  double time_alignment_ns;
  double time_alignment_taps;
};

comparison compare(sdk::srs_vector& vector, ocudu_dapp_status_v1& status)
{
  auto&                                                  srs = vector.metadata;
  std::vector<std::complex<float>>                       channel(vector.channel.size());
  std::array<float, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1> values{};
  ocudu_dapp_srs_channel_estimator_input_v1              input{};
  ocudu_dapp_srs_channel_estimator_output_v1             output{};
  input.invocation.struct_size = sizeof(input.invocation);
  input.invocation.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  input.rx_symbols = host_tensor(vector.rx_symbols.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(std::complex<float>),
                                 OCUDU_DAPP_ACCESS_READ_V1, OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1,
                                 {srs.nof_pilot_sets, srs.nof_rx_ports, srs.nof_symbols, srs.sequence_length});
  input.reference_symbols = host_tensor(
      vector.references.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(std::complex<float>), OCUDU_DAPP_ACCESS_READ_V1,
      OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1, {srs.nof_tx_ports, srs.sequence_length});
  input.srs.struct_size             = sizeof(input.srs);
  input.srs.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
  input.srs.metadata_schema_id      = OCUDU_DAPP_METADATA_SRS_CHANNEL_ESTIMATOR_V1;
  input.srs.metadata_schema_version = 1;
  input.srs.metadata                = {&srs, sizeof(srs), 0};
  output.channel_matrix =
      host_tensor(channel.data(), OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(std::complex<float>), OCUDU_DAPP_ACCESS_WRITE_V1,
                  OCUDU_DAPP_LAYOUT_SRS_CHANNEL_MATRIX_RX_TX_CF32_V1, {srs.nof_rx_ports, srs.nof_tx_ports});
  output.measurements = host_tensor(values.data(), OCUDU_DAPP_ELEMENT_F32_V1, sizeof(float), OCUDU_DAPP_ACCESS_WRITE_V1,
                                    OCUDU_DAPP_LAYOUT_SRS_MEASUREMENTS_F32_V1, {OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1});
  status              = reference::estimate_srs_channel(input, output);

  double error = 0.0;
  double norm  = 0.0;
  for (size_t index = 0; index != channel.size(); ++index) {
    error += std::norm(std::complex<double>(channel[index]) - std::complex<double>(vector.channel[index]));
    norm += std::norm(std::complex<double>(vector.channel[index]));
  }
  const auto&  recorded = vector.measurements;
  const auto   delta    = [&](uint32_t index) { return static_cast<double>(values[index]) - recorded[index]; };
  const double ta       = vector.time_alignment[0];
  const double taps     = std::isnan(vector.time_alignment[1])
                              ? std::nan("")
                              : (values[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1] - ta) / vector.time_alignment[1];
  return {norm > 0 ? std::sqrt(error / norm) : std::sqrt(error),
          delta(OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1),
          delta(OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1),
          delta(OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1) / recorded[OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1],
          (values[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1] - ta) * 1e9,
          taps};
}

/// Median and maximum magnitude of the values that were compared.
void summarize(const char* name, std::vector<double> values, const char* unit)
{
  values.erase(std::remove_if(values.begin(), values.end(), [](double value) { return std::isnan(value); }),
               values.end());
  if (values.empty()) {
    std::printf("  %-22s not recorded\n", name);
    return;
  }
  for (double& value : values) {
    value = std::abs(value);
  }
  std::sort(values.begin(), values.end());
  std::printf("  %-22s median %.3g, max %.3g %s\n", name, values[values.size() / 2], values.back(), unit);
}

} // namespace

int main(int argc, char** argv)
{
  const char* path  = nullptr;
  bool        check = false;
  bool        quiet = false;
  for (int index = 1; index != argc; ++index) {
    if (std::strcmp(argv[index], "--check") == 0) {
      check = true;
    } else if (std::strcmp(argv[index], "--quiet") == 0) {
      quiet = true;
    } else if (path == nullptr && argv[index][0] != '-') {
      path = argv[index];
    } else {
      path = nullptr;
      break;
    }
  }
  if (path == nullptr) {
    std::fprintf(stderr, "usage: ocudu-dapp-srs-replay VECTORS [--check] [--quiet]\n");
    return 2;
  }

  std::vector<sdk::srs_vector> cases;
  try {
    cases = sdk::read_srs_vectors(path);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 2;
  }

  const sdk::srs_tolerance tolerance;
  std::vector<double>      channel, epre, rsrp, noise, ta_ns;
  unsigned                 rejected = 0;
  unsigned                 outside  = 0;
  for (size_t index = 0; index != cases.size(); ++index) {
    ocudu_dapp_status_v1 status = OCUDU_DAPP_OK_V1;
    const auto           diff   = compare(cases[index], status);
    const auto&          srs    = cases[index].metadata;
    if (status != OCUDU_DAPP_OK_V1) {
      ++rejected;
      std::printf("occasion %zu: rejected by the reference estimator (status %d)\n", index, status);
      continue;
    }
    channel.push_back(diff.channel);
    epre.push_back(diff.epre_db);
    rsrp.push_back(diff.rsrp_db);
    noise.push_back(diff.noise);
    ta_ns.push_back(diff.time_alignment_ns);
    const auto exceeds = [](double value, double bound) { return !std::isnan(value) && std::abs(value) > bound; };
    const bool out     = exceeds(diff.channel, tolerance.channel) || exceeds(diff.epre_db, tolerance.power_db) ||
                     exceeds(diff.rsrp_db, tolerance.power_db) || exceeds(diff.noise, tolerance.noise) ||
                     exceeds(diff.time_alignment_taps, tolerance.time_alignment);
    outside += out;
    if (!quiet) {
      std::printf("occasion %4zu rnti 0x%04x rx %u tx %u sym %u comb %u len %4u: channel %.2e  epre %+.2e dB  "
                  "rsrp %+.2e dB  noise %+.2e  ta %+.3f ns%s\n",
                  index, srs.rnti, srs.nof_rx_ports, srs.nof_tx_ports, srs.nof_symbols, srs.comb_size,
                  srs.sequence_length, diff.channel, diff.epre_db, diff.rsrp_db, diff.noise, diff.time_alignment_ns,
                  out ? "  (outside the golden bounds)" : "");
    }
  }
  std::printf("%zu occasions, %u rejected, %u outside the golden bounds; reference vs recorded:\n", cases.size(),
              rejected, outside);
  summarize("channel (relative)", channel, "");
  summarize("EPRE", epre, "dB");
  summarize("RSRP", rsrp, "dB");
  summarize("noise (relative)", noise, "");
  summarize("time alignment", ta_ns, "ns");
  return check && (rejected != 0 || outside != 0) ? 1 : 0;
}
