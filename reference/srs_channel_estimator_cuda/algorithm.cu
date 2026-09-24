// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/reference_srs_channel_estimator.h"
#include "ocudu_dapp_sdk/srs_channel_estimator_cuda.h"
#include <cfloat>
#include <cuda_runtime.h>
#include <math_constants.h>

namespace ocudu::dapp::reference {
namespace {

constexpr uint32_t max_rx     = OCUDU_DAPP_SRS_MAX_RX_PORTS_V1;
constexpr uint32_t max_tx     = OCUDU_DAPP_SRS_MAX_TX_PORTS_V1;
constexpr uint32_t max_length = OCUDU_DAPP_SRS_MAX_SEQUENCE_LENGTH_V1;
/// Delay bins per port: the two search windows (at most dft_size / 8 taps each) and the curve-fit guards.
constexpr uint32_t max_bins        = 1024;
constexpr uint32_t cexp_table_size = 1024;
constexpr float    two_pi          = 2.0F * 3.14159265358979323846F;
constexpr uint32_t threads         = 256;

/// Scalars of one occasion, passed by value to every kernel.
struct occasion {
  uint32_t nof_rx;
  uint32_t nof_tx;
  uint32_t nof_symbols;
  uint32_t length;
  uint32_t nof_sets;
  uint32_t comb;
  uint32_t pilot_set[max_tx];
  uint32_t first_subcarrier[OCUDU_DAPP_SRS_MAX_PILOT_SETS_V1];
  uint32_t scs_khz;
  bool     interleaved;
  /// Time-alignment search.
  uint32_t dft_size;
  uint32_t window;
  uint32_t nof_bins;
  double   sampling_rate_hz;
  bool     fractional;
};

struct buffers {
  const float2* rx;
  const float2* reference;
  float2*       lse;
  float*        profile;
  float2*       channel;
  float*        measurements;
};

__device__ float2 operator+(float2 a, float2 b)
{
  return {a.x + b.x, a.y + b.y};
}

__device__ float2& operator+=(float2& a, float2 b)
{
  a = a + b;
  return a;
}

__device__ float2 cmul(float2 a, float2 b)
{
  return {a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x};
}

__device__ float2 cmul_conj(float2 a, float2 b)
{
  return {a.x * b.x + a.y * b.y, a.y * b.x - a.x * b.y};
}

__device__ const float2* received(const buffers& data, const occasion& srs, uint32_t set, uint32_t rx, uint32_t symbol)
{
  return &data.rx[((static_cast<size_t>(set) * srs.nof_rx + rx) * srs.nof_symbols + symbol) * srs.length];
}

__device__ float2* lse_of(const buffers& data, const occasion& srs, uint32_t rx, uint32_t tx)
{
  return &data.lse[(static_cast<size_t>(rx) * max_tx + tx) * srs.length];
}

/// Sum over the block; every thread gets the result.
template <typename T>
__device__ T block_sum(T value, T* shared)
{
  shared[threadIdx.x] = value;
  __syncthreads();
  for (uint32_t stride = blockDim.x / 2; stride != 0; stride >>= 1) {
    if (threadIdx.x < stride) {
      shared[threadIdx.x] += shared[threadIdx.x + stride];
    }
    __syncthreads();
  }
  const T result = shared[0];
  __syncthreads();
  return result;
}

/// Least-squares estimate of every port pair, averaged over the SRS symbols. Grid: (tx, rx).
__global__ void least_squares_kernel(buffers data, occasion srs)
{
  const uint32_t tx        = blockIdx.x;
  const uint32_t rx        = blockIdx.y;
  const float2*  reference = &data.reference[static_cast<size_t>(tx) * srs.length];
  float2*        lse       = lse_of(data, srs, rx, tx);
  const float    scale     = srs.nof_symbols > 1 ? 1.0F / static_cast<float>(srs.nof_symbols) : 1.0F;
  for (uint32_t n = threadIdx.x; n < srs.length; n += blockDim.x) {
    float2 sum = received(data, srs, srs.pilot_set[tx], rx, 0)[n];
    for (uint32_t symbol = 1; symbol < srs.nof_symbols; ++symbol) {
      sum += received(data, srs, srs.pilot_set[tx], rx, symbol)[n];
    }
    const float2 value = cmul_conj(sum, reference[n]);
    lse[n]             = {value.x * scale, value.y * scale};
  }
}

/// Delay profile of every port at the searched bins: sum over rx ports of |IDFT(lse)|^2. Grid: (tx, bin).
__global__ void delay_profile_kernel(buffers data, occasion srs)
{
  __shared__ float2 shared[threads];
  const uint32_t    tx  = blockIdx.x;
  const uint32_t    bin = blockIdx.y;
  // Bins run from -(window + 2) to window + 1 around the origin, modulo the IDFT size.
  const int64_t  offset = static_cast<int64_t>(bin) - static_cast<int64_t>(srs.window) - 2;
  const uint64_t tap    = static_cast<uint64_t>((offset + srs.dft_size) % srs.dft_size);
  float          power  = 0.0F;
  for (uint32_t rx = 0; rx != srs.nof_rx; ++rx) {
    const float2* lse     = lse_of(data, srs, rx, tx);
    float2        partial = {0.0F, 0.0F};
    for (uint32_t n = threadIdx.x; n < srs.length; n += blockDim.x) {
      // Positive exponent: the host's inverse DFT. The phase is reduced exactly before the float evaluation.
      const uint64_t phase = (tap * n) % srs.dft_size;
      float          s     = 0.0F;
      float          c     = 0.0F;
      sincospif(2.0F * static_cast<float>(phase) / static_cast<float>(srs.dft_size), &s, &c);
      partial += cmul(lse[n], {c, s});
    }
    const float2 sum = block_sum(partial, shared);
    power += sum.x * sum.x + sum.y * sum.y;
  }
  if (threadIdx.x == 0) {
    data.profile[static_cast<size_t>(tx) * max_bins + bin] = power;
  }
}

__device__ float fractional_peak(const float* samples, uint32_t taps)
{
  constexpr float numerator_5[5]   = {-0.400000F, -0.200000F, 0.000000F, 0.200000F, 0.400000F};
  constexpr float denominator_5[5] = {0.571429F, -0.285714F, -0.571429F, -0.285714F, 0.571429F};
  constexpr float numerator_3[3]   = {-0.5F, 0.0F, 0.5F};
  constexpr float denominator_3[3] = {0.5F, -1.0F, 0.5F};
  const float*    numerator        = taps == 5 ? numerator_5 : numerator_3;
  const float*    denominator      = taps == 5 ? denominator_5 : denominator_3;
  float           num              = 0.0F;
  float           den              = 0.0F;
  for (uint32_t tap = 0; tap != taps; ++tap) {
    num += numerator[tap] * samples[tap];
    den += denominator[tap] * samples[tap];
  }
  const float result = -(taps == 5 ? 1.0F : 0.5F) * num / den;
  return isnan(result) || isinf(result) || fabsf(result) > 1.0F ? 0.0F : result;
}

/// The host's complex-exponential table entry for the phase at comb subcarrier \p n.
__device__ float2 phase_ramp(uint32_t n, float step, float offset)
{
  const float phase = static_cast<float>(n) * step + offset;
  const auto  index =
      static_cast<unsigned>(static_cast<int>(roundf(static_cast<float>(cexp_table_size) * phase / two_pi)));
  float s = 0.0F;
  float c = 0.0F;
  sincosf(two_pi * static_cast<float>(index % cexp_table_size) / static_cast<float>(cexp_table_size), &s, &c);
  return {c, s};
}

/// Time alignment, wideband coefficients, noise, EPRE and RSRP. One block.
__global__ void finish_kernel(buffers data, occasion srs)
{
  __shared__ float2 shared_complex[threads];
  __shared__ float  shared_real[threads];
  __shared__ float  phase_step;
  __shared__ double alignment[4];
  __shared__ float2 coefficient[max_rx * max_tx];

  if (threadIdx.x == 0) {
    // Per-port peak search, then the average over ports (min/max: the tightest bounds).
    double value = 0.0, resolution = 0.0;
    // The host starts the bounds at the smallest positive and the largest double.
    double lower = DBL_MIN, upper = DBL_MAX;
    for (uint32_t tx = 0; tx != srs.nof_tx; ++tx) {
      const float*   profile = &data.profile[static_cast<size_t>(tx) * max_bins];
      const uint32_t origin  = srs.window + 2;
      uint32_t       delay = 0, early = 0;
      for (uint32_t tap = 1; tap < srs.window; ++tap) {
        delay = profile[origin + tap] > profile[origin + delay] ? tap : delay;
        early = profile[2 + tap] > profile[2 + early] ? tap : early;
      }
      // A delay wins a tie with an advance of equal correlation.
      const int index    = profile[origin + delay] >= profile[2 + early] ? static_cast<int>(delay)
                                                                         : -static_cast<int>(srs.window - early);
      double    fraction = 0.0;
      if (srs.fractional) {
        const uint32_t taps = srs.window > 2 ? 5 : 3;
        fraction            = fractional_peak(&profile[origin + index - static_cast<int>(taps / 2)], taps);
      }
      value += (static_cast<double>(index) + fraction) / srs.sampling_rate_hz;
      resolution = fmax(resolution, 1.0 / srs.sampling_rate_hz);
      lower      = fmax(lower, -static_cast<double>(srs.window) / srs.sampling_rate_hz);
      upper      = fmin(upper, static_cast<double>(srs.window) / srs.sampling_rate_hz);
    }
    value /= srs.nof_tx;
    alignment[0] = value;
    alignment[1] = resolution;
    alignment[2] = lower;
    alignment[3] = upper;
    // Host evaluation order: kHz, then * 1000.
    phase_step = static_cast<float>(static_cast<double>(two_pi) * value * srs.scs_khz * 1000 * srs.comb);
  }
  __syncthreads();

  // Wideband coefficient: mean of the phase-compensated least-squares estimates.
  float rsrp = 0.0F;
  for (uint32_t rx = 0; rx != srs.nof_rx; ++rx) {
    for (uint32_t tx = 0; tx != srs.nof_tx; ++tx) {
      const float offset =
          phase_step * static_cast<float>(srs.first_subcarrier[srs.pilot_set[tx]]) / static_cast<float>(srs.comb);
      const float2* lse     = lse_of(data, srs, rx, tx);
      float2        partial = {0.0F, 0.0F};
      for (uint32_t n = threadIdx.x; n < srs.length; n += blockDim.x) {
        partial += cmul(lse[n], phase_ramp(n, phase_step, offset));
      }
      const float2 sum  = block_sum(partial, shared_complex);
      const float2 mean = {sum.x / static_cast<float>(srs.length), sum.y / static_cast<float>(srs.length)};
      if (threadIdx.x == 0) {
        coefficient[rx * srs.nof_tx + tx] = mean;
      }
      rsrp += mean.x * mean.x + mean.y * mean.y;
    }
  }
  __syncthreads();

  // Noise: the received pilots of each set, summed over symbols and phase compensated, less the reconstruction of
  // every port of the set counted once per symbol. EPRE: the received pilot power.
  float noise_sum = 0.0F;
  float epre      = 0.0F;
  for (uint32_t rx = 0; rx != srs.nof_rx; ++rx) {
    for (uint32_t set = 0; set != srs.nof_sets; ++set) {
      const float offset = phase_step * static_cast<float>(srs.first_subcarrier[set]) / static_cast<float>(srs.comb);
      float       residual_power = 0.0F;
      float       pilot_power    = 0.0F;
      for (uint32_t n = threadIdx.x; n < srs.length; n += blockDim.x) {
        float2 sum = {0.0F, 0.0F};
        for (uint32_t symbol = 0; symbol != srs.nof_symbols; ++symbol) {
          const float2 pilot = received(data, srs, set, rx, symbol)[n];
          sum += pilot;
          pilot_power += pilot.x * pilot.x + pilot.y * pilot.y;
        }
        float2 residual = cmul(sum, phase_ramp(n, phase_step, offset));
        for (uint32_t tx = 0; tx != srs.nof_tx; ++tx) {
          if (srs.pilot_set[tx] == set) {
            const float2 c         = coefficient[rx * srs.nof_tx + tx];
            const float2 scaled    = {c.x * srs.nof_symbols, c.y * srs.nof_symbols};
            const float2 recovered = cmul(data.reference[static_cast<size_t>(tx) * srs.length + n], scaled);
            residual               = {residual.x - recovered.x, residual.y - recovered.y};
          }
        }
        residual_power += residual.x * residual.x + residual.y * residual.y;
      }
      noise_sum += block_sum(residual_power, shared_real);
      epre += block_sum(pilot_power, shared_real) / static_cast<float>(srs.length);
    }
  }

  if (threadIdx.x == 0) {
    const uint32_t nof_estimates = srs.interleaved ? 2 : srs.nof_tx;
    const uint32_t correction    = srs.interleaved ? 2 : 1;
    const float    noise_variance =
        noise_sum / static_cast<float>((srs.nof_symbols * srs.length - nof_estimates) * correction * srs.nof_rx);
    const float noise_std = fmaxf(sqrtf(noise_variance), sqrtf(rsrp) * 0.01F);
    for (uint32_t index = 0; index != srs.nof_rx * srs.nof_tx; ++index) {
      data.channel[index] = {coefficient[index].x / noise_std, coefficient[index].y / noise_std};
    }
    epre /= static_cast<float>(srs.nof_symbols * correction * srs.nof_rx);
    rsrp /= static_cast<float>(srs.nof_tx * srs.nof_rx);
    for (uint32_t index = 0; index != OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1; ++index) {
      data.measurements[index] = CUDART_NAN_F;
    }
    data.measurements[OCUDU_DAPP_SRS_MEASUREMENT_NOISE_VARIANCE_V1]              = noise_variance;
    data.measurements[OCUDU_DAPP_SRS_MEASUREMENT_EPRE_DB_V1]                     = 10.0F * log10f(epre);
    data.measurements[OCUDU_DAPP_SRS_MEASUREMENT_RSRP_DB_V1]                     = 10.0F * log10f(rsrp);
    data.measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_S_V1]            = static_cast<float>(alignment[0]);
    data.measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_RESOLUTION_S_V1] = static_cast<float>(alignment[1]);
    data.measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MIN_S_V1]        = static_cast<float>(alignment[2]);
    data.measurements[OCUDU_DAPP_SRS_MEASUREMENT_TIME_ALIGNMENT_MAX_S_V1]        = static_cast<float>(alignment[3]);
  }
}

/// Runs every kernel once on a one-pilot-set, twelve-pilot occasion so the module's kernels are loaded before the
/// first real invocation (CUDA loads kernels lazily, and the first launch otherwise costs milliseconds on the slot).
void warm_up(float2* lse, float* profile) noexcept
{
  constexpr uint32_t length = 12;
  cudaStream_t       stream = nullptr;
  float2*            memory = nullptr;
  if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess ||
      cudaMalloc(&memory, sizeof(float2) * (2 * length + 1 + OCUDU_DAPP_SRS_MEASUREMENT_COUNT_V1)) != cudaSuccess ||
      cudaMemsetAsync(memory, 0, sizeof(float2) * 2 * length, stream) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
  } else {
    occasion srs{};
    srs.nof_rx = srs.nof_tx = srs.nof_symbols = srs.nof_sets = 1;
    srs.length                                               = length;
    srs.comb                                                 = 4;
    srs.scs_khz                                              = 30;
    srs.dft_size                                             = 128;
    srs.window                                               = 2;
    srs.nof_bins                                             = 2 * srs.window + 4;
    srs.sampling_rate_hz                                     = 128.0 * 30e3 * 4;
    srs.fractional                                           = true;
    const buffers data{memory,  memory + length,     lse,
                       profile, memory + 2 * length, reinterpret_cast<float*>(memory + 2 * length + 1)};
    least_squares_kernel<<<dim3(1, 1), threads, 0, stream>>>(data, srs);
    delay_profile_kernel<<<dim3(1, srs.nof_bins), threads, 0, stream>>>(data, srs);
    finish_kernel<<<1, threads, 0, stream>>>(data, srs);
    static_cast<void>(cudaStreamSynchronize(stream));
    static_cast<void>(cudaGetLastError());
  }
  static_cast<void>(cudaFree(memory));
  if (stream != nullptr) {
    static_cast<void>(cudaStreamDestroy(stream));
  }
}

} // namespace

srs_channel_estimator_cuda_worker_state::srs_channel_estimator_cuda_worker_state() noexcept
{
  if (cudaMalloc(&lse, sizeof(float2) * max_rx * max_tx * max_length) != cudaSuccess ||
      cudaMalloc(&profile, sizeof(float) * max_tx * max_bins) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return;
  }
  // Worker creation runs off the slot path; the first invocation must not pay for kernel loading.
  warm_up(static_cast<float2*>(lse), profile);
}

srs_channel_estimator_cuda_worker_state::~srs_channel_estimator_cuda_worker_state()
{
  static_cast<void>(cudaFree(lse));
  static_cast<void>(cudaFree(profile));
}

ocudu_dapp_status_v1 estimate_srs_channel_cuda(const ocudu_dapp_srs_channel_estimator_input_v1& input,
                                               ocudu_dapp_srs_channel_estimator_output_v1&      output,
                                               const srs_channel_estimator_cuda_state&,
                                               srs_channel_estimator_cuda_worker_state& worker)
{
  srs_invocation invocation;
  if (input.invocation.execution_stream == 0 ||
      !read_srs_invocation(input, output, OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1, invocation)) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  if (!worker.ready()) {
    return OCUDU_DAPP_RESOURCE_V1;
  }
  const auto& metadata = *invocation.srs;
  const auto  search   = make_srs_time_alignment_search(metadata);
  occasion    srs{};
  srs.nof_rx      = metadata.nof_rx_ports;
  srs.nof_tx      = metadata.nof_tx_ports;
  srs.nof_symbols = metadata.nof_symbols;
  srs.length      = metadata.sequence_length;
  srs.nof_sets    = metadata.nof_pilot_sets;
  srs.comb        = metadata.comb_size;
  srs.scs_khz     = metadata.subcarrier_spacing_hz / 1000;
  srs.interleaved = (metadata.flags & OCUDU_DAPP_SRS_FLAG_INTERLEAVED_PILOTS_V1) != 0;
  for (uint32_t port = 0; port != max_tx; ++port) {
    srs.pilot_set[port] = metadata.pilot_set[port];
  }
  for (uint32_t set = 0; set != OCUDU_DAPP_SRS_MAX_PILOT_SETS_V1; ++set) {
    srs.first_subcarrier[set] = metadata.first_subcarrier[set];
  }
  srs.dft_size         = search.dft_size;
  srs.window           = search.max_samples;
  srs.nof_bins         = 2 * search.max_samples + 4;
  srs.sampling_rate_hz = search.sampling_rate_hz;
  srs.fractional       = search.fractional;
  if (srs.window == 0 || srs.nof_bins > max_bins) {
    return OCUDU_DAPP_UNSUPPORTED_V1;
  }

  const buffers data{static_cast<const float2*>(invocation.rx_symbols),
                     static_cast<const float2*>(invocation.references),
                     static_cast<float2*>(worker.lse),
                     worker.profile,
                     static_cast<float2*>(invocation.channel),
                     static_cast<float*>(invocation.measurements)};
  auto*         stream = reinterpret_cast<cudaStream_t>(static_cast<uintptr_t>(input.invocation.execution_stream));
  least_squares_kernel<<<dim3(srs.nof_tx, srs.nof_rx), threads, 0, stream>>>(data, srs);
  delay_profile_kernel<<<dim3(srs.nof_tx, srs.nof_bins), threads, 0, stream>>>(data, srs);
  finish_kernel<<<1, threads, 0, stream>>>(data, srs);
  return cudaPeekAtLastError() == cudaSuccess ? OCUDU_DAPP_OK_V1 : OCUDU_DAPP_INTERNAL_ERROR_V1;
}

} // namespace ocudu::dapp::reference
