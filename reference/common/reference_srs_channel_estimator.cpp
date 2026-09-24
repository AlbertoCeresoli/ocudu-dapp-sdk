// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_srs_channel_estimator.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <vector>

namespace ocudu::dapp::reference {
namespace {

using cf32 = std::complex<float>;

constexpr uint32_t max_rx       = OCUDU_DAPP_SRS_MAX_RX_PORTS_V1;
constexpr uint32_t max_tx       = OCUDU_DAPP_SRS_MAX_TX_PORTS_V1;
constexpr uint32_t max_sets     = OCUDU_DAPP_SRS_MAX_PILOT_SETS_V1;
constexpr uint32_t max_length   = OCUDU_DAPP_SRS_MAX_SEQUENCE_LENGTH_V1;
constexpr uint32_t max_dft_size = 4096;
/// Size of the host estimator's complex-exponential table used to remove the time-alignment phase ramp.
constexpr uint32_t cexp_table_size = 1024;
/// Resource elements of the widest carrier (275 PRB); the host scales the IDFT size against it.
constexpr uint32_t max_carrier_subcarriers = 3300;
/// The host's 2 pi, in single precision.
constexpr float two_pi = 2.0F * static_cast<float>(M_PI);

bool dense_tensor(const ocudu_dapp_tensor_view_v1& tensor, uint32_t domain, uint32_t element_type,
                  uint32_t element_size, uint32_t access, uint32_t layout_id,
                  std::initializer_list<uint64_t> dimensions) noexcept
{
  constexpr uint32_t minimum = offsetof(ocudu_dapp_tensor_view_v1, reserved);
  const bool         host    = domain == OCUDU_DAPP_MEM_HOST_V1;
  if (tensor.struct_size < minimum || tensor.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || tensor.domain != domain ||
      (host ? (tensor.access & OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1) == 0
            : tensor.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1) ||
      (tensor.access & access) != access || tensor.element_type != element_type || tensor.layout_id != layout_id ||
      tensor.layout_version != 1 || tensor.rank != dimensions.size() || tensor.native_handle == 0) {
    return false;
  }
  uint64_t stride = element_size;
  uint32_t index  = static_cast<uint32_t>(dimensions.size());
  for (auto dimension = dimensions.end(); dimension != dimensions.begin();) {
    --dimension;
    --index;
    if (tensor.dimensions[index] != *dimension || tensor.byte_strides[index] != stride) {
      return false;
    }
    stride *= *dimension;
  }
  return tensor.byte_length >= stride;
}

bool valid_metadata(const ocudu_dapp_srs_channel_estimator_metadata_v1& srs) noexcept
{
  const bool interleaved = (srs.flags & OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1) != 0;
  const bool valid_comb  = (srs.comb_size == 2 && srs.n_cs_max == 8) || (srs.comb_size == 4 && srs.n_cs_max == 12);
  if (srs.struct_size < offsetof(ocudu_dapp_srs_channel_estimator_metadata_v1, reserved) ||
      srs.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || (srs.flags & ~OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1) != 0 ||
      srs.nof_rx_ports == 0 || srs.nof_rx_ports > max_rx ||
      (srs.nof_tx_ports != 1 && srs.nof_tx_ports != 2 && srs.nof_tx_ports != 4) || srs.nof_symbols == 0 ||
      srs.start_symbol >= OCUDU_DAPP_SRS_MAX_SYMBOLS_V1 ||
      srs.nof_symbols > OCUDU_DAPP_SRS_MAX_SYMBOLS_V1 - srs.start_symbol || srs.sequence_length == 0 ||
      srs.sequence_length > max_length || !valid_comb || srs.nof_pilot_sets != (interleaved ? 2U : 1U) ||
      (interleaved && srs.nof_tx_ports != 4) || srs.numerology > 4 ||
      srs.subcarrier_spacing_hz != (15000U << srs.numerology) || !std::isfinite(srs.max_time_alignment_s) ||
      !(srs.max_time_alignment_s > 0.0)) {
    return false;
  }
  for (uint32_t port = 0; port != srs.nof_tx_ports; ++port) {
    if (srs.pilot_set[port] >= srs.nof_pilot_sets || srs.cyclic_shift[port] >= srs.n_cs_max) {
      return false;
    }
  }
  return true;
}

/// In-place radix-2 inverse DFT (positive exponent, unnormalised), the direction of the host's TA IDFT.
class inverse_dft
{
public:
  inverse_dft()
  {
    twiddles.resize(max_dft_size / 2);
    for (uint32_t k = 0; k != twiddles.size(); ++k) {
      const double angle = 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(max_dft_size);
      twiddles[k]        = cf32(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
    }
  }

  void run(cf32* data, uint32_t size) const noexcept
  {
    for (uint32_t i = 1, j = 0; i < size; ++i) {
      uint32_t bit = size >> 1;
      for (; (j & bit) != 0; bit >>= 1) {
        j ^= bit;
      }
      j ^= bit;
      if (i < j) {
        std::swap(data[i], data[j]);
      }
    }
    for (uint32_t length = 2; length <= size; length <<= 1) {
      const uint32_t step = max_dft_size / length;
      for (uint32_t start = 0; start < size; start += length) {
        for (uint32_t k = 0; k != length / 2; ++k) {
          const cf32 odd               = twiddles[k * step] * data[start + k + length / 2];
          data[start + k + length / 2] = data[start + k] - odd;
          data[start + k] += odd;
        }
      }
    }
  }

private:
  std::vector<cf32> twiddles;
};

/// Per-thread scratch, sized once for the largest occasion.
struct workspace {
  std::vector<cf32>  lse       = std::vector<cf32>(static_cast<size_t>(max_rx) * max_tx * max_length);
  std::vector<cf32>  noise     = std::vector<cf32>(static_cast<size_t>(max_sets) * max_rx * max_length);
  std::vector<cf32>  spectrum  = std::vector<cf32>(max_dft_size);
  std::vector<float> power     = std::vector<float>(max_dft_size);
  std::vector<cf32>  cexp      = make_cexp();
  inverse_dft        transform = {};

  static std::vector<cf32> make_cexp()
  {
    std::vector<cf32> table(cexp_table_size);
    for (uint32_t n = 0; n != cexp_table_size; ++n) {
      table[n] = std::polar(1.0F, two_pi * static_cast<float>(n) / static_cast<float>(cexp_table_size));
    }
    return table;
  }
};

struct time_alignment {
  double value;
  double resolution;
  double min;
  double max;
};

/// Host time_alignment_estimator_dft_impl over the least-squares estimates of one transmit port at every rx port.
time_alignment estimate_time_alignment(const ocudu_dapp_srs_channel_estimator_metadata_v1& srs, const cf32* lse,
                                       workspace& scratch) noexcept
{
  const auto     search = make_srs_time_alignment_search(srs);
  const uint32_t size   = search.dft_size;
  const uint32_t length = srs.sequence_length;
  float*         power  = scratch.power.data();
  for (uint32_t rx = 0; rx != srs.nof_rx_ports; ++rx) {
    cf32* spectrum = scratch.spectrum.data();
    std::copy_n(&lse[static_cast<size_t>(rx) * max_tx * length], length, spectrum);
    std::fill(spectrum + length, spectrum + size, cf32());
    scratch.transform.run(spectrum, size);
    for (uint32_t n = 0; n != size; ++n) {
      power[n] = (rx == 0 ? 0.0F : power[n]) + std::norm(spectrum[n]);
    }
  }

  // A delay wins a tie with an advance of equal correlation.
  const uint32_t window = search.max_samples;
  const auto     delay  = std::max_element(power, power + window);
  const auto     early  = std::max_element(power + size - window, power + size);
  int            index  = -static_cast<int>(window - static_cast<uint32_t>(early - (power + size - window)));
  if (*delay >= *early) {
    index = static_cast<int>(delay - power);
  }
  double fraction = 0.0;
  if (search.fractional) {
    const uint32_t       taps = window > 2 ? 5 : 3;
    std::array<float, 5> peak{};
    for (uint32_t tap = 0; tap != taps; ++tap) {
      peak[tap] = power[(static_cast<int64_t>(index) + tap + size - taps / 2) % size];
    }
    fraction = srs_fractional_peak(peak.data(), taps);
  }
  const double rate = search.sampling_rate_hz;
  return {(static_cast<double>(index) + fraction) / rate, 1.0 / rate, -static_cast<double>(window) / rate,
          static_cast<double>(window) / rate};
}

/// Removes the linear phase of \p phase_step per comb subcarrier (from \p phase_offset), the host's table lookup.
void remove_phase_ramp(cf32* values, uint32_t length, float phase_step, float phase_offset, workspace& scratch) noexcept
{
  for (uint32_t n = 0; n != length; ++n) {
    const float phase = static_cast<float>(n) * phase_step + phase_offset;
    const auto  index =
        static_cast<unsigned>(static_cast<int>(std::round(static_cast<float>(cexp_table_size) * phase / two_pi)));
    values[n] *= scratch.cexp[index % cexp_table_size];
  }
}

float average_power(const cf32* values, uint32_t length) noexcept
{
  float sum = 0.0F;
  for (uint32_t n = 0; n != length; ++n) {
    sum += std::norm(values[n]);
  }
  return sum / static_cast<float>(length);
}

void estimate(const srs_invocation& invocation, workspace& scratch) noexcept
{
  const auto&    srs         = *invocation.srs;
  const uint32_t nof_rx      = srs.nof_rx_ports;
  const uint32_t nof_tx      = srs.nof_tx_ports;
  const uint32_t nof_symbols = srs.nof_symbols;
  const uint32_t length      = srs.sequence_length;
  const bool     interleaved = (srs.flags & OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1) != 0;
  const auto*    rx_symbols  = static_cast<const cf32*>(invocation.rx_symbols);
  const auto*    references  = static_cast<const cf32*>(invocation.references);
  auto*          channel     = static_cast<cf32*>(invocation.channel);
  auto*          values      = static_cast<float*>(invocation.measurements);

  const auto received = [&](uint32_t set, uint32_t rx, uint32_t symbol) {
    return &rx_symbols[((static_cast<size_t>(set) * nof_rx + rx) * nof_symbols + symbol) * length];
  };
  const auto lse_of = [&](uint32_t rx, uint32_t tx) {
    return &scratch.lse[(static_cast<size_t>(rx) * max_tx + tx) * length];
  };
  const auto noise_of = [&](uint32_t set, uint32_t rx) {
    return &scratch.noise[(static_cast<size_t>(set) * max_rx + rx) * length];
  };
  // The noise of a pilot set is measured on the pilots of its first port.
  const auto measures_noise = [&](uint32_t tx) { return tx == 0 || (interleaved && tx == 1); };

  std::fill_n(scratch.noise.begin(), static_cast<size_t>(max_sets) * max_rx * length, cf32());
  float          epre = 0.0F;
  time_alignment ta   = {0.0, 0.0, std::numeric_limits<double>::min(), std::numeric_limits<double>::max()};
  for (uint32_t tx = 0; tx != nof_tx; ++tx) {
    const uint32_t set       = srs.pilot_set[tx];
    const cf32*    reference = &references[static_cast<size_t>(tx) * length];
    for (uint32_t rx = 0; rx != nof_rx; ++rx) {
      cf32* lse   = lse_of(rx, tx);
      cf32* noise = noise_of(set, rx);
      for (uint32_t symbol = 0; symbol != nof_symbols; ++symbol) {
        const cf32* pilots = received(set, rx, symbol);
        if (measures_noise(tx)) {
          for (uint32_t n = 0; n != length; ++n) {
            noise[n] += pilots[n];
          }
          epre += average_power(pilots, length);
        }
        for (uint32_t n = 0; n != length; ++n) {
          lse[n] = symbol == 0 ? pilots[n] : lse[n] + pilots[n];
        }
      }
      for (uint32_t n = 0; n != length; ++n) {
        lse[n] *= std::conj(reference[n]);
      }
      if (nof_symbols > 1) {
        const float scale = 1.0F / static_cast<float>(nof_symbols);
        for (uint32_t n = 0; n != length; ++n) {
          lse[n] *= scale;
        }
      }
    }
    // The other ports' contributions, still present per subcarrier, only add later replicas to the delay profile.
    const time_alignment port = estimate_time_alignment(srs, lse_of(0, tx), scratch);
    ta.value += port.value;
    ta.min        = std::max(ta.min, port.min);
    ta.max        = std::min(ta.max, port.max);
    ta.resolution = std::max(ta.resolution, port.resolution);
  }
  ta.value /= nof_tx;

  // Evaluated in the host's order (kHz, then * 1000) so the phase indices round identically.
  const auto phase_step     = static_cast<float>(static_cast<double>(two_pi) * ta.value *
                                                 (srs.subcarrier_spacing_hz / 1000) * 1000 * srs.comb_size);
  float      noise_variance = 0.0F;
  float      rsrp           = 0.0F;
  for (uint32_t rx = 0; rx != nof_rx; ++rx) {
    for (uint32_t tx = 0; tx != nof_tx; ++tx) {
      const uint32_t set = srs.pilot_set[tx];
      const float    phase_offset =
          phase_step * static_cast<float>(srs.first_subcarrier[set]) / static_cast<float>(srs.comb_size);
      cf32* lse = lse_of(rx, tx);
      remove_phase_ramp(lse, length, phase_step, phase_offset, scratch);
      cf32 coefficient = 0.0F;
      for (uint32_t n = 0; n != length; ++n) {
        coefficient += lse[n];
      }
      coefficient /= static_cast<float>(length);
      channel[static_cast<size_t>(rx) * nof_tx + tx] = coefficient;
      rsrp += std::norm(coefficient);

      // Subtract the reconstructed pilots, counted once per SRS symbol, from the accumulated received pilots.
      cf32* noise = noise_of(set, rx);
      if (measures_noise(tx)) {
        remove_phase_ramp(noise, length, phase_step, phase_offset, scratch);
      }
      const cf32  scaled    = static_cast<float>(nof_symbols) * coefficient;
      const cf32* reference = &references[static_cast<size_t>(tx) * length];
      for (uint32_t n = 0; n != length; ++n) {
        noise[n] -= reference[n] * scaled;
      }
    }
    for (uint32_t set = 0; set != srs.nof_pilot_sets; ++set) {
      noise_variance += average_power(noise_of(set, rx), length) * static_cast<float>(length);
    }
  }
  // Degrees of freedom: nof_symbols * length samples per pilot set and rx port, less one per estimated coefficient
  // (two with interleaved pilots, whose two sets hold two coefficients each).
  const uint32_t nof_estimates = interleaved ? 2 : nof_tx;
  const uint32_t correction    = interleaved ? 2 : 1;
  noise_variance /= static_cast<float>((nof_symbols * length - nof_estimates) * correction * nof_rx);

  // Normalise by the noise standard deviation so the squared Frobenius norm reads as SNR, capped at 40 dB.
  const float noise_std = std::max(std::sqrt(noise_variance), std::sqrt(rsrp) * 0.01F);
  const float scale     = 1.0F / noise_std;
  for (uint32_t index = 0; index != nof_rx * nof_tx; ++index) {
    channel[index] *= scale;
  }
  epre /= static_cast<float>(nof_symbols * correction * nof_rx);
  rsrp /= static_cast<float>(nof_tx * nof_rx);

  std::fill_n(values, OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1, std::numeric_limits<float>::quiet_NaN());
  values[OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1]              = noise_variance;
  values[OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1]                     = 10.0F * std::log10(epre);
  values[OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1]                     = 10.0F * std::log10(rsrp);
  values[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1]            = static_cast<float>(ta.value);
  values[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_RESOLUTION_S_V1] = static_cast<float>(ta.resolution);
  values[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MIN_S_V1]        = static_cast<float>(ta.min);
  values[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MAX_S_V1]        = static_cast<float>(ta.max);
}

} // namespace

bool read_srs_invocation(const ocudu_dapp_srs_channel_estimator_input_v1&  input,
                         const ocudu_dapp_srs_channel_estimator_output_v1& output, uint32_t domain,
                         srs_invocation& invocation) noexcept
{
  const auto& wrapper = input.srs;
  if (wrapper.struct_size < sizeof(wrapper) || wrapper.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 ||
      wrapper.metadata_schema_id != OCUDU_DAPP_METADATA_SRS_CHANNEL_ESTIMATOR_V1 ||
      wrapper.metadata_schema_version != 1 || wrapper.metadata.data == nullptr ||
      wrapper.metadata.size < offsetof(ocudu_dapp_srs_channel_estimator_metadata_v1, reserved)) {
    return false;
  }
  const auto& srs = *static_cast<const ocudu_dapp_srs_channel_estimator_metadata_v1*>(wrapper.metadata.data);
  if (!valid_metadata(srs) ||
      !dense_tensor(input.rx_symbols, domain, OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(cf32), OCUDU_DAPP_ACCESS_READ_V1,
                    OCUDU_DAPP_LAYOUT_SRS_RX_SET_PORT_SYMBOL_PILOT_CF32_V1,
                    {srs.nof_pilot_sets, srs.nof_rx_ports, srs.nof_symbols, srs.sequence_length}) ||
      !dense_tensor(input.reference_symbols, domain, OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(cf32),
                    OCUDU_DAPP_ACCESS_READ_V1, OCUDU_DAPP_LAYOUT_SRS_REFERENCE_TX_PILOT_CF32_V1,
                    {srs.nof_tx_ports, srs.sequence_length}) ||
      !dense_tensor(output.channel_matrix, domain, OCUDU_DAPP_ELEMENT_CF32_V1, sizeof(cf32), OCUDU_DAPP_ACCESS_WRITE_V1,
                    OCUDU_DAPP_LAYOUT_SRS_CHANNEL_MATRIX_RX_TX_CF32_V1, {srs.nof_rx_ports, srs.nof_tx_ports}) ||
      !dense_tensor(output.measurements, domain, OCUDU_DAPP_ELEMENT_F32_V1, sizeof(float), OCUDU_DAPP_ACCESS_WRITE_V1,
                    OCUDU_DAPP_LAYOUT_SRS_MEASUREMENTS_F32_V1, {OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1})) {
    return false;
  }
  const auto address = [](const ocudu_dapp_tensor_view_v1& tensor) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(tensor.native_handle + tensor.byte_offset));
  };
  invocation.srs          = &srs;
  invocation.rx_symbols   = address(input.rx_symbols);
  invocation.references   = address(input.reference_symbols);
  invocation.channel      = address(output.channel_matrix);
  invocation.measurements = address(output.measurements);
  return true;
}

srs_time_alignment_search
make_srs_time_alignment_search(const ocudu_dapp_srs_channel_estimator_metadata_v1& srs) noexcept
{
  // IDFT size: the sequence scaled against the widest carrier, a power of two, at least one timing-advance step
  // (1/128 of a 15 kHz symbol) per tap.
  const uint32_t scaled   = srs.sequence_length * max_dft_size / max_carrier_subcarriers;
  uint32_t       dft_size = 128;
  while (dft_size < scaled) {
    dft_size <<= 1;
  }
  const double rate = static_cast<double>(static_cast<uint64_t>(dft_size) * srs.subcarrier_spacing_hz * srs.comb_size);
  // Half the normal cyclic prefix, 144 kappa Tc / 2^(mu + 1) with kappa = 64, evaluated as the host evaluates it: an
  // integer count of Tc = 1 / (480 kHz * 4096) times Tc. Products such as 36 taps must not round down.
  constexpr double t_c                = 1.0 / static_cast<double>(480000LL * 4096LL);
  const double     half_cyclic_prefix = static_cast<double>((144LL * 64LL) >> (srs.numerology + 1)) * t_c;
  auto             max_samples        = static_cast<uint32_t>(std::floor(half_cyclic_prefix * rate));
  if (std::isnormal(srs.max_time_alignment_s)) {
    max_samples = std::min(max_samples, static_cast<uint32_t>(std::floor(srs.max_time_alignment_s * rate)));
  }
  return {dft_size, max_samples, rate, dft_size != max_dft_size};
}

float srs_fractional_peak(const float* samples, uint32_t nof_samples) noexcept
{
  // Least-squares parabola through the samples: offset = -b / (2 a), the host curve_fitting_fractional_max weights.
  static constexpr std::array<float, 5> numerator_5   = {-0.400000F, -0.200000F, 0.000000F, 0.200000F, 0.400000F};
  static constexpr std::array<float, 5> denominator_5 = {0.571429F, -0.285714F, -0.571429F, -0.285714F, 0.571429F};
  static constexpr std::array<float, 3> numerator_3   = {-0.5F, 0.0F, 0.5F};
  static constexpr std::array<float, 3> denominator_3 = {0.5F, -1.0F, 0.5F};
  const float*                          numerator     = nullptr;
  const float*                          denominator   = nullptr;
  float                                 correction    = 1.0F;
  if (nof_samples == 5) {
    numerator   = numerator_5.data();
    denominator = denominator_5.data();
  } else if (nof_samples == 3) {
    numerator   = numerator_3.data();
    denominator = denominator_3.data();
    correction  = 0.5F;
  } else {
    return 0.0F;
  }
  float num = 0.0F;
  float den = 0.0F;
  for (uint32_t n = 0; n != nof_samples; ++n) {
    num += numerator[n] * samples[n];
    den += denominator[n] * samples[n];
  }
  const float result = -correction * num / den;
  return std::isnan(result) || std::isinf(result) || std::abs(result) > 1.0F ? 0.0F : result;
}

ocudu_dapp_status_v1 estimate_srs_channel(const ocudu_dapp_srs_channel_estimator_input_v1& input,
                                          ocudu_dapp_srs_channel_estimator_output_v1&      output)
{
  srs_invocation invocation;
  if (!read_srs_invocation(input, output, OCUDU_DAPP_MEM_HOST_V1, invocation)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  thread_local workspace scratch;
  estimate(invocation, scratch);
  return OCUDU_DAPP_OK_V1;
}

} // namespace ocudu::dapp::reference
