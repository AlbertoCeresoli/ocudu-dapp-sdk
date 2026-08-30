// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Central Class-A L1 validation suite (dlopen any EQ/RX package).
// Tiers: golden · chain · sensitivity · tb
//
// Usage:
//   l1_validation_suite --eq-so PATH [--rx-so PATH] [--mode all|golden|chain|sensitivity|tb]
//                       [--quick] [--frames N] [--seed N]

#include "ocudu/dapp/abi/v1/module.h"
#include "ocudu/dapp/use_cases/v1/equalizer.h"
#include "ocudu/dapp/use_cases/v1/pusch_metadata.h"
#include "ocudu/dapp/use_cases/v1/receiver.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr float inv_sqrt2 = 0.7071067811865476F;

[[noreturn]] void die(const char* msg)
{
  std::fprintf(stderr, "FATAL: %s\n", msg);
  std::exit(2);
}

void require(bool ok, const char* msg)
{
  if (!ok) {
    die(msg);
  }
}

void require_cuda(cudaError_t st, const char* what)
{
  if (st != cudaSuccess) {
    std::fprintf(stderr, "CUDA %s: %s\n", what, cudaGetErrorString(st));
    std::exit(2);
  }
}

uint32_t pack_cbf16(float re, float im)
{
  __nv_bfloat16 bre = __float2bfloat16(re);
  __nv_bfloat16 bim = __float2bfloat16(im);
  uint16_t      re_bits{}, im_bits{};
  std::memcpy(&re_bits, &bre, sizeof(re_bits));
  std::memcpy(&im_bits, &bim, sizeof(im_bits));
  return static_cast<uint32_t>(re_bits) | (static_cast<uint32_t>(im_bits) << 16);
}

float2 qpsk_from_bits(int b0, int b1, float scale)
{
  return {scale * (b0 ? -inv_sqrt2 : inv_sqrt2), scale * (b1 ? -inv_sqrt2 : inv_sqrt2)};
}

float2 qam16_from_label(unsigned label, float scale)
{
  const int       bi0       = (label >> 0) & 1;
  const int       bi1       = (label >> 1) & 1;
  const int       bq0       = (label >> 2) & 1;
  const int       bq1       = (label >> 3) & 1;
  const float     levels[4] = {-3.F, -1.F, 1.F, 3.F};
  const int       ii        = (bi1 << 1) | bi0;
  const int       qq        = (bq1 << 1) | bq0;
  constexpr float norm      = 0.316227766F;
  return {scale * levels[ii] * norm, scale * levels[qq] * norm};
}

int hard_qpsk(float2 s)
{
  return ((s.y < 0.F) << 1) | (s.x < 0.F);
}

int hard_qam16(float2 s)
{
  auto axis = [](float v) {
    constexpr float inv_norm = 3.16227766F;
    const float     x        = v * inv_norm;
    if (x < -2.F)
      return 0;
    if (x < 0.F)
      return 1;
    if (x < 2.F)
      return 2;
    return 3;
  };
  const int ii = axis(s.x), qq = axis(s.y);
  return (ii & 1) | (((ii >> 1) & 1) << 1) | ((qq & 1) << 2) | (((qq >> 1) & 1) << 3);
}

uint32_t crc24a(const uint8_t* bits, size_t n_bits)
{
  uint32_t crc = 0;
  for (size_t i = 0; i < n_bits; ++i) {
    const uint32_t bit = (bits[i] & 1U) ^ ((crc >> 23) & 1U);
    crc                = ((crc << 1) & 0xFFFFFFU) ^ (bit ? 0x1864CFBU : 0U);
  }
  return crc & 0xFFFFFFU;
}

uint64_t fnv1a64(const void* data, size_t nbytes)
{
  const auto* p    = static_cast<const uint8_t*>(data);
  uint64_t    hash = 14695981039346656037ULL;
  for (size_t i = 0; i < nbytes; ++i) {
    hash ^= p[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

ocudu_dapp_tensor_view_v1 device_tensor(void* ptr, uint32_t element, uint32_t access,
                                        std::initializer_list<uint64_t> dims, uint32_t layout = 0)
{
  ocudu_dapp_tensor_view_v1 view{};
  view.struct_size           = sizeof(view);
  view.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
  view.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
  view.domain                = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
  view.backend_id            = OCUDU_DAPP_BACKEND_CUDA_V1;
  view.element_type          = element;
  view.access                = access;
  view.native_handle         = reinterpret_cast<uintptr_t>(ptr);
  view.allocation_id         = 1;
  view.allocation_generation = 1;
  view.rank                  = static_cast<uint32_t>(dims.size());
  view.layout_id             = layout;
  view.layout_version        = layout != 0 ? 1U : 0U;
  view.lease_token           = 1;
  uint32_t elem_bytes        = 4;
  if (element == OCUDU_DAPP_ELEMENT_CF32_V1)
    elem_bytes = 8;
  else if (element == OCUDU_DAPP_ELEMENT_CBF16_V1)
    elem_bytes = 4;
  else if (element == OCUDU_DAPP_ELEMENT_F16_V1 || element == OCUDU_DAPP_ELEMENT_U16_V1)
    elem_bytes = 2;
  uint64_t stride = elem_bytes, count = 1;
  int      i = static_cast<int>(dims.size()) - 1;
  for (auto it = dims.end(); it != dims.begin();) {
    --it;
    view.dimensions[i]   = *it;
    view.byte_strides[i] = stride;
    count *= *it;
    stride *= *it;
    --i;
  }
  view.byte_length = count * elem_bytes;
  return view;
}

// ---- host stubs (match ocudu_dapp_host_api_v1) ----
uint64_t mono_ns(void*) noexcept
{
  return 1'000'000'000ULL;
}
void                 metric_add(void*, uint32_t, uint64_t) noexcept {}
void                 incident(void*, uint32_t, uint64_t) noexcept {}
ocudu_dapp_status_v1 publish(void*, uint32_t, uint32_t, const void*, uint32_t) noexcept
{
  return OCUDU_DAPP_OK_V1;
}
ocudu_dapp_status_v1 extension(void*, uint32_t, uint16_t, const void** out) noexcept
{
  if (out) {
    *out = nullptr;
  }
  return OCUDU_DAPP_UNSUPPORTED_V1;
}

// ---- package session (dlopen) ----
struct package {
  void*                                     handle{};
  ocudu_dapp_module_v1                      module{};
  void*                                     instance{};
  void*                                     worker{};
  const ocudu_dapp_interface_descriptor_v1* descriptor{};
  uint32_t                                  interface_id{};
  bool                                      started{};

  void load(const char* path, uint32_t want_iface)
  {
    handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
    require(handle != nullptr, ::dlerror() ? ::dlerror() : "dlopen failed");
    void* sym = ::dlsym(handle, "ocudu_dapp_query_v1");
    require(sym != nullptr, "ocudu_dapp_query_v1 missing");
    ocudu_dapp_query_v1_fn query = nullptr;
    std::memcpy(&query, &sym, sizeof(query));

    ocudu_dapp_host_info_v1 hi{};
    hi.struct_size  = sizeof(hi);
    hi.abi_major    = OCUDU_DAPP_ABI_MAJOR_V1;
    hi.abi_minor    = OCUDU_DAPP_ABI_MINOR_V1;
    hi.backend_mask = uint64_t{1} << OCUDU_DAPP_BACKEND_CUDA_V1;

    module.struct_size = sizeof(module);
    module.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
    module.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
    require(query(&hi, &module) == OCUDU_DAPP_OK_V1, "query failed");
    require(module.nof_interfaces >= 1 && module.interfaces != nullptr, "no interfaces");
    descriptor   = &module.interfaces[0];
    interface_id = descriptor->interface_id;
    require(interface_id == want_iface, "unexpected interface id");

    ocudu_dapp_host_api_v1 host{};
    host.struct_size      = sizeof(host);
    host.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
    host.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
    host.monotonic_ns     = mono_ns;
    host.metric_add       = metric_add;
    host.incident_token   = incident;
    host.publish_prepared = publish;
    host.query_extension  = extension;

    require(module.create(&host, nullptr, &instance) == OCUDU_DAPP_OK_V1 && instance, "create");
    require(module.prepare(instance, nullptr) == OCUDU_DAPP_OK_V1, "prepare");
    require(module.start(instance) == OCUDU_DAPP_OK_V1, "start");
    started = true;

    const auto* wapi = static_cast<const ocudu_dapp_worker_api_v1*>(descriptor->interface_vtable);
    require(wapi && wapi->create_worker_context, "worker api");
    require(wapi->create_worker_context(instance, 0, &worker) == OCUDU_DAPP_OK_V1 && worker, "create worker");
  }

  void unload()
  {
    if (worker && descriptor) {
      const auto* wapi = static_cast<const ocudu_dapp_worker_api_v1*>(descriptor->interface_vtable);
      if (wapi && wapi->destroy_worker_context) {
        wapi->destroy_worker_context(instance, worker);
      }
      worker = nullptr;
    }
    if (started && module.stop) {
      module.stop(instance);
      started = false;
    }
    if (instance && module.destroy) {
      module.destroy(instance);
      instance = nullptr;
    }
    if (handle) {
      ::dlclose(handle);
      handle = nullptr;
    }
  }
};

// ---- slot simulation ----
struct slot_shape {
  unsigned nof_prb{6};
  unsigned start_prb{0};
  unsigned nof_symbols{14};
  int      dmrs_mask{(1 << 2) | (1 << 11)};
  unsigned mod_order{2};
  float    dmrs_scale{std::sqrt(2.0F)};
  float    snr_db{30.F};
  unsigned alloc_sc() const { return nof_prb * 12U; }
  unsigned start_sc() const { return start_prb * 12U; }
  unsigned grid_sc() const { return (start_prb + nof_prb) * 12U; }
};

struct built_slot {
  unsigned              n_sym{}, full_sc{}, start_sc{}, alloc_sc{}, plane{}, mod_order{};
  std::vector<uint32_t> h_grid;
  std::vector<float2>   h_true, h_refs;
  std::vector<int>      h_labels;
  std::vector<uint8_t>  h_tx_bits;
  std::vector<uint32_t> h_indices;
  std::vector<uint16_t> h_coords;
};

built_slot build_slot(const slot_shape& shape, std::mt19937& rng, const uint8_t* payload = nullptr,
                      size_t payload_len = 0)
{
  built_slot o{};
  o.n_sym     = shape.nof_symbols;
  o.full_sc   = shape.grid_sc();
  o.start_sc  = shape.start_sc();
  o.alloc_sc  = shape.alloc_sc();
  o.plane     = o.full_sc * o.n_sym;
  o.mod_order = shape.mod_order;
  require(shape.mod_order == 2 || shape.mod_order == 4, "QPSK/16QAM only");

  const float                        snr_lin     = std::pow(10.0F, shape.snr_db / 10.0F);
  const float                        noise_sigma = 1.0F / std::sqrt(snr_lin);
  std::normal_distribution<float>    gauss(0.F, 1.F);
  std::uniform_int_distribution<int> bit(0, 1);
  size_t                             bit_cursor = 0;
  auto                               next_bit   = [&]() -> int {
    if (payload) {
      require(bit_cursor < payload_len, "payload exhausted");
      return payload[bit_cursor++] & 1;
    }
    return bit(rng);
  };

  std::vector<float2> h_rx(o.plane, float2{0.F, 0.F});
  const float2        h_gain{0.8F, 0.6F};
  auto                apply = [&](float2 tx) -> float2 {
    float2 y{h_gain.x * tx.x - h_gain.y * tx.y, h_gain.x * tx.y + h_gain.y * tx.x};
    y.x += noise_sigma * gauss(rng);
    y.y += noise_sigma * gauss(rng);
    return y;
  };

  for (unsigned sym = 0; sym < o.n_sym; ++sym) {
    const bool is_dmrs = ((shape.dmrs_mask >> static_cast<int>(sym)) & 1) != 0;
    if (is_dmrs) {
      for (unsigned sc = o.start_sc; sc < o.start_sc + o.alloc_sc; sc += 2) {
        const float2 p = qpsk_from_bits(bit(rng), bit(rng), shape.dmrs_scale);
        o.h_refs.push_back(p);
        o.h_coords.push_back(static_cast<uint16_t>(sym));
        o.h_coords.push_back(static_cast<uint16_t>(sc));
        h_rx[sym * o.full_sc + sc] = apply(p);
      }
    } else {
      for (unsigned sc = o.start_sc; sc < o.start_sc + o.alloc_sc; ++sc) {
        float2 tx{};
        int    label = 0;
        if (shape.mod_order == 2) {
          const int b0 = next_bit(), b1 = next_bit();
          label = (b1 << 1) | b0;
          tx    = qpsk_from_bits(b0, b1, 1.0F);
          o.h_tx_bits.push_back(static_cast<uint8_t>(b0));
          o.h_tx_bits.push_back(static_cast<uint8_t>(b1));
        } else {
          int b[4];
          for (int k = 0; k < 4; ++k) {
            b[k] = next_bit();
            o.h_tx_bits.push_back(static_cast<uint8_t>(b[k]));
          }
          label = b[0] | (b[1] << 1) | (b[2] << 2) | (b[3] << 3);
          tx    = qam16_from_label(static_cast<unsigned>(label), 1.0F);
        }
        o.h_true.push_back(tx);
        o.h_labels.push_back(label);
        o.h_indices.push_back(sym * o.full_sc + sc);
        h_rx[sym * o.full_sc + sc] = apply(tx);
      }
    }
  }
  o.h_grid.resize(o.plane);
  for (unsigned i = 0; i < o.plane; ++i) {
    o.h_grid[i] = pack_cbf16(h_rx[i].x, h_rx[i].y);
  }
  require(!o.h_indices.empty() && !o.h_refs.empty(), "empty pilots/data");
  return o;
}

struct device_bufs {
  cudaStream_t stream{};
  uint32_t*    d_grid{};
  float2*      d_refs{};
  uint16_t*    d_coords{};
  uint32_t*    d_indices{};
  float2*      d_channel{};
  float2*      d_eq{};
  float*       d_noise{};
  __half*      d_llrs{};
  size_t       cap_grid{}, cap_refs{}, cap_coords{}, cap_idx{}, cap_ch{}, cap_eq{}, cap_noise{}, cap_llrs{};

  void init() { require_cuda(cudaStreamCreate(&stream), "stream"); }
  void ensure(size_t* cap, void** p, size_t n, size_t el)
  {
    if (*cap >= n)
      return;
    if (*p)
      cudaFree(*p);
    require_cuda(cudaMalloc(p, n * el), "malloc");
    *cap = n;
  }
  void destroy()
  {
    cudaFree(d_grid);
    cudaFree(d_refs);
    cudaFree(d_coords);
    cudaFree(d_indices);
    cudaFree(d_channel);
    cudaFree(d_eq);
    cudaFree(d_noise);
    cudaFree(d_llrs);
    if (stream)
      cudaStreamDestroy(stream);
  }
};

void upload(device_bufs& d, const built_slot& s)
{
  const uint32_t nd = static_cast<uint32_t>(s.h_indices.size());
  const uint32_t np = static_cast<uint32_t>(s.h_refs.size());
  d.ensure(&d.cap_grid, reinterpret_cast<void**>(&d.d_grid), s.plane, 4);
  d.ensure(&d.cap_refs, reinterpret_cast<void**>(&d.d_refs), np, 8);
  d.ensure(&d.cap_coords, reinterpret_cast<void**>(&d.d_coords), np * 2, 2);
  d.ensure(&d.cap_idx, reinterpret_cast<void**>(&d.d_indices), nd, 4);
  d.ensure(&d.cap_ch, reinterpret_cast<void**>(&d.d_channel), s.n_sym * s.full_sc, 8);
  d.ensure(&d.cap_eq, reinterpret_cast<void**>(&d.d_eq), nd, 8);
  d.ensure(&d.cap_noise, reinterpret_cast<void**>(&d.d_noise), nd, 4);
  d.ensure(&d.cap_llrs, reinterpret_cast<void**>(&d.d_llrs), nd * s.mod_order, 2);
  require_cuda(cudaMemcpyAsync(d.d_grid, s.h_grid.data(), s.plane * 4, cudaMemcpyHostToDevice, d.stream), "H2D");
  require_cuda(cudaMemcpyAsync(d.d_refs, s.h_refs.data(), np * 8, cudaMemcpyHostToDevice, d.stream), "H2D");
  require_cuda(cudaMemcpyAsync(d.d_coords, s.h_coords.data(), np * 4, cudaMemcpyHostToDevice, d.stream), "H2D");
  require_cuda(cudaMemcpyAsync(d.d_indices, s.h_indices.data(), nd * 4, cudaMemcpyHostToDevice, d.stream), "H2D");
}

struct eq_metrics {
  ocudu_dapp_status_v1 status{OCUDU_DAPP_DEVICE_ERROR_V1};
  double               mse{}, ser{};
  uint64_t             symbols{}, errors{}, checksum{};
  std::vector<float2>  eq;
};

eq_metrics run_eq(package& pkg, device_bufs& d, const built_slot& s)
{
  eq_metrics m{};
  upload(d, s);
  const uint32_t nd = static_cast<uint32_t>(s.h_indices.size());
  const uint32_t np = static_cast<uint32_t>(s.h_refs.size());

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = 1;
  pusch.nof_layers           = 1;
  pusch.modulation           = s.mod_order == 4 ? OCUDU_DAPP_MODULATION_QAM16_V1 : OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.start_symbol         = 0;
  pusch.nof_symbols          = s.n_sym;
  pusch.start_subcarrier     = s.start_sc;
  pusch.nof_subcarriers      = s.alloc_sc;
  pusch.noise_variance_floor = 1e-4F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1] = 30'000U;
  pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1]  = s.full_sc;
  pusch.nof_data_re                                                  = nd;
  pusch.data_re_indices = device_tensor(d.d_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1, {nd},
                                        OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);

  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = np;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reference_symbols = device_tensor(d.d_refs, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, np});
  dmrs.coordinates       = device_tensor(d.d_coords, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {np, 2});

  ocudu_dapp_invocation_v1 inv{};
  inv.struct_size      = sizeof(inv);
  inv.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  inv.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  inv.execution_stream = reinterpret_cast<uintptr_t>(d.stream);

  ocudu_dapp_typed_metadata_v1 pusch_v{sizeof(ocudu_dapp_typed_metadata_v1),
                                       OCUDU_DAPP_ABI_MAJOR_V1,
                                       OCUDU_DAPP_ABI_MINOR_V1,
                                       OCUDU_DAPP_METADATA_PUSCH_V1,
                                       1,
                                       {&pusch, sizeof(pusch), 0}};
  ocudu_dapp_typed_metadata_v1 dmrs_v{sizeof(ocudu_dapp_typed_metadata_v1),
                                      OCUDU_DAPP_ABI_MAJOR_V1,
                                      OCUDU_DAPP_ABI_MINOR_V1,
                                      OCUDU_DAPP_METADATA_DMRS_V1,
                                      1,
                                      {&dmrs, sizeof(dmrs), 0}};
  auto grid = device_tensor(d.d_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, s.n_sym, s.full_sc},
                            OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);

  ocudu_dapp_equalizer_input_v1  in{inv, grid, pusch_v, dmrs_v};
  ocudu_dapp_equalizer_output_v1 out{};
  out.channel_estimates =
      device_tensor(d.d_channel, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {1, 1, s.n_sym, s.full_sc});
  out.equalized_symbols       = device_tensor(d.d_eq, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {nd, 1},
                                              OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
  out.post_equalization_noise = device_tensor(d.d_noise, OCUDU_DAPP_ELEMENT_F32_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {nd, 1},
                                              OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);

  const auto* api = static_cast<const ocudu_dapp_equalizer_interface_v1*>(pkg.descriptor->interface_vtable);
  m.status        = api->invoke(pkg.worker, &in, &out);
  if (m.status != OCUDU_DAPP_OK_V1)
    return m;
  require_cuda(cudaStreamSynchronize(d.stream), "sync");
  m.eq.resize(nd);
  require_cuda(cudaMemcpy(m.eq.data(), d.d_eq, nd * 8, cudaMemcpyDeviceToHost), "D2H");
  m.checksum   = fnv1a64(m.eq.data(), m.eq.size() * 8);
  double   mse = 0;
  uint64_t err = 0;
  for (uint32_t i = 0; i < nd; ++i) {
    const float dx = m.eq[i].x - s.h_true[i].x;
    const float dy = m.eq[i].y - s.h_true[i].y;
    mse += dx * dx + dy * dy;
    const int hard = s.mod_order == 2 ? hard_qpsk(m.eq[i]) : hard_qam16(m.eq[i]);
    if (hard != s.h_labels[i])
      ++err;
  }
  m.symbols = nd;
  m.errors  = err;
  m.mse     = mse / nd;
  m.ser     = static_cast<double>(err) / nd;
  return m;
}

struct rx_metrics {
  ocudu_dapp_status_v1 status{OCUDU_DAPP_DEVICE_ERROR_V1};
  double               ber{};
  uint64_t             bits{}, errors{}, checksum{};
};

rx_metrics run_rx(package& pkg, device_bufs& d, const built_slot& s)
{
  rx_metrics m{};
  upload(d, s);
  const uint32_t nd  = static_cast<uint32_t>(s.h_indices.size());
  const uint32_t np  = static_cast<uint32_t>(s.h_refs.size());
  const uint32_t mod = s.mod_order;

  ocudu_dapp_pusch_metadata_v1 pusch{};
  pusch.struct_size          = sizeof(pusch);
  pusch.abi_major            = OCUDU_DAPP_ABI_MAJOR_V1;
  pusch.abi_minor            = OCUDU_DAPP_ABI_MINOR_V1;
  pusch.nof_rx_ports         = 1;
  pusch.nof_layers           = 1;
  pusch.modulation           = mod == 4 ? OCUDU_DAPP_MODULATION_QAM16_V1 : OCUDU_DAPP_MODULATION_QPSK_V1;
  pusch.start_symbol         = 0;
  pusch.nof_symbols          = s.n_sym;
  pusch.start_subcarrier     = s.start_sc;
  pusch.nof_subcarriers      = s.alloc_sc;
  pusch.noise_variance_floor = 1e-4F;
  pusch.flags                = OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1;
  pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_SUBCARRIER_SPACING_HZ_V1] = 30'000U;
  pusch.reserved[OCUDU_DAPP_PUSCH_RESERVED_GRID_NOF_SUBCARRIERS_V1]  = s.full_sc;
  pusch.nof_data_re                                                  = nd;
  pusch.data_re_indices = device_tensor(d.d_indices, OCUDU_DAPP_ELEMENT_U32_V1, OCUDU_DAPP_ACCESS_READ_V1, {nd},
                                        OCUDU_DAPP_LAYOUT_PUSCH_DATA_RE_INDICES_U32_V1);

  ocudu_dapp_dmrs_metadata_v1 dmrs{};
  dmrs.struct_size = sizeof(dmrs);
  dmrs.abi_major   = OCUDU_DAPP_ABI_MAJOR_V1;
  dmrs.abi_minor   = OCUDU_DAPP_ABI_MINOR_V1;
  dmrs.nof_pilots  = np;
  dmrs.dmrs_type   = 1;
  dmrs.flags =
      OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1 | OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1;
  dmrs.reference_symbols = device_tensor(d.d_refs, OCUDU_DAPP_ELEMENT_CF32_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, np});
  dmrs.coordinates       = device_tensor(d.d_coords, OCUDU_DAPP_ELEMENT_U16_V1, OCUDU_DAPP_ACCESS_READ_V1, {np, 2});

  ocudu_dapp_invocation_v1 inv{};
  inv.struct_size      = sizeof(inv);
  inv.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
  inv.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
  inv.execution_stream = reinterpret_cast<uintptr_t>(d.stream);

  ocudu_dapp_typed_metadata_v1 pusch_v{sizeof(ocudu_dapp_typed_metadata_v1),
                                       OCUDU_DAPP_ABI_MAJOR_V1,
                                       OCUDU_DAPP_ABI_MINOR_V1,
                                       OCUDU_DAPP_METADATA_PUSCH_V1,
                                       1,
                                       {&pusch, sizeof(pusch), 0}};
  ocudu_dapp_typed_metadata_v1 dmrs_v{sizeof(ocudu_dapp_typed_metadata_v1),
                                      OCUDU_DAPP_ABI_MAJOR_V1,
                                      OCUDU_DAPP_ABI_MINOR_V1,
                                      OCUDU_DAPP_METADATA_DMRS_V1,
                                      1,
                                      {&dmrs, sizeof(dmrs), 0}};
  auto grid = device_tensor(d.d_grid, OCUDU_DAPP_ELEMENT_CBF16_V1, OCUDU_DAPP_ACCESS_READ_V1, {1, s.n_sym, s.full_sc},
                            OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1);

  ocudu_dapp_receiver_input_v1  in{inv, grid, pusch_v, dmrs_v};
  ocudu_dapp_receiver_output_v1 out{};
  out.llrs = device_tensor(d.d_llrs, OCUDU_DAPP_ELEMENT_F16_V1, OCUDU_DAPP_ACCESS_WRITE_V1, {nd, 1, mod},
                           OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);

  const auto* api = static_cast<const ocudu_dapp_receiver_interface_v1*>(pkg.descriptor->interface_vtable);
  m.status        = api->invoke(pkg.worker, &in, &out);
  if (m.status != OCUDU_DAPP_OK_V1)
    return m;
  require_cuda(cudaStreamSynchronize(d.stream), "sync");
  std::vector<__half> llrs(nd * mod);
  require_cuda(cudaMemcpy(llrs.data(), d.d_llrs, llrs.size() * 2, cudaMemcpyDeviceToHost), "D2H");
  m.checksum = fnv1a64(llrs.data(), llrs.size() * 2);

  uint64_t err = 0, tot = 0;
  for (uint32_t i = 0; i < nd; ++i) {
    int hard_label = 0;
    if (mod == 2) {
      const int i0 = (__half2float(llrs[i * 2]) > 0.F) ? 0 : 1;
      const int q0 = (__half2float(llrs[i * 2 + 1]) > 0.F) ? 0 : 1;
      hard_label   = (q0 << 1) | i0;
    } else {
      // 16QAM LLR Gray table may differ from sim labels — chain gates QPSK BER only.
      hard_label = -1;
    }
    if (hard_label >= 0) {
      unsigned x = static_cast<unsigned>(hard_label ^ s.h_labels[i]);
      while (x) {
        err += x & 1U;
        x >>= 1;
      }
      tot += mod;
    }
  }
  m.bits   = tot;
  m.errors = err;
  m.ber    = tot ? static_cast<double>(err) / tot : 0.0;
  return m;
}

// ---- options / modes ----
struct options {
  std::string mode{"all"};
  const char* eq_so{nullptr};
  const char* rx_so{nullptr};
  bool        quick{false};
  unsigned    frames{8};
  uint32_t    seed{0xC0FFEEu};
};

options parse(int argc, char** argv)
{
  options o;
  if (const char* e = std::getenv("OCUDU_L1_EQ_SO"))
    o.eq_so = e;
  if (const char* e = std::getenv("OCUDU_L1_RX_SO"))
    o.rx_so = e;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--mode") && i + 1 < argc)
      o.mode = argv[++i];
    else if (!std::strcmp(argv[i], "--eq-so") && i + 1 < argc)
      o.eq_so = argv[++i];
    else if (!std::strcmp(argv[i], "--rx-so") && i + 1 < argc)
      o.rx_so = argv[++i];
    else if (!std::strcmp(argv[i], "--quick"))
      o.quick = true;
    else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc)
      o.frames = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
      o.seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    else if (!std::strcmp(argv[i], "--help")) {
      std::printf("l1_validation_suite --eq-so PATH [--rx-so PATH] --mode all|golden|chain|sensitivity|tb "
                  "[--quick] [--frames N] [--seed N]\n");
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown arg %s\n", argv[i]);
      std::exit(2);
    }
  }
  if (o.quick)
    o.frames = std::min(o.frames, 4U);
  require(o.eq_so != nullptr, "--eq-so or OCUDU_L1_EQ_SO required");
  return o;
}

int run_golden(package& eq, device_bufs& d, const options& opt)
{
  std::printf("\n=== A · GOLDEN (fixed seed, high SNR, via dlopen EQ) ===\n");
  int fails = 0;
  struct C {
    const char* n;
    unsigned    prb, mod;
  };
  const C cases[] = {{"QPSK.6prb", 6, 2}, {"16QAM.6prb", 6, 4}, {"QPSK.12prb", 12, 2}};
  for (const auto& c : cases) {
    slot_shape sh{};
    sh.nof_prb   = c.prb;
    sh.mod_order = c.mod;
    sh.snr_db    = 30.F;
    uint64_t err = 0, sym = 0;
    double   mse_sum = 0;
    uint64_t cs      = 0;
    bool     ok      = true;
    for (unsigned f = 0; f < opt.frames; ++f) {
      std::mt19937 rng(opt.seed + f * 7919U + c.mod);
      auto         slot = build_slot(sh, rng);
      auto         m    = run_eq(eq, d, slot);
      if (m.status != OCUDU_DAPP_OK_V1) {
        ok = false;
        std::printf("  FAIL %s status=%d\n", c.n, static_cast<int>(m.status));
        break;
      }
      err += m.errors;
      sym += m.symbols;
      mse_sum += m.mse * m.symbols;
      cs = m.checksum;
    }
    if (!ok) {
      ++fails;
      continue;
    }
    const double ser  = static_cast<double>(err) / sym;
    const double mse  = mse_sum / sym;
    const bool   pass = err == 0 && mse < (c.mod == 2 ? 5e-3 : 2e-2);
    std::printf("  %s %-12s SER=%.3e MSE=%.3e cs=%016llx\n", pass ? "PASS" : "FAIL", c.n, ser, mse,
                static_cast<unsigned long long>(cs));
    if (!pass)
      ++fails;
  }
  std::printf("A golden: %s\n", fails ? "FAIL" : "PASS");
  return fails;
}

int run_chain(package& eq, package* rx, device_bufs& d, const options& opt)
{
  std::printf("\n=== B · CHAIN (EQ + optional RX package) ===\n");
  int fails = 0;
  // Always EQ SER gate; RX BER only for QPSK when rx package present.
  struct C {
    const char* n;
    unsigned    prb, mod;
    bool        ber;
  };
  const C cases[] = {{"QPSK.6prb", 6, 2, true}, {"16QAM.6prb", 6, 4, false}, {"QPSK.12prb", 12, 2, true}};
  for (const auto& c : cases) {
    slot_shape sh{};
    sh.nof_prb      = c.prb;
    sh.mod_order    = c.mod;
    sh.snr_db       = 28.F;
    uint64_t eq_err = 0, eq_sym = 0, bit_err = 0, bits = 0;
    uint64_t eq_cs = 0, llr_cs = 0;
    bool     ok = true;
    for (unsigned f = 0; f < opt.frames; ++f) {
      std::mt19937 rng(opt.seed + 1000 + f * 4243U + c.mod);
      auto         slot = build_slot(sh, rng);
      auto         em   = run_eq(eq, d, slot);
      if (em.status != OCUDU_DAPP_OK_V1) {
        ok = false;
        std::printf("  FAIL %s eq status=%d\n", c.n, static_cast<int>(em.status));
        break;
      }
      eq_err += em.errors;
      eq_sym += em.symbols;
      eq_cs = em.checksum;
      if (rx && c.ber) {
        auto rm = run_rx(*rx, d, slot);
        if (rm.status != OCUDU_DAPP_OK_V1) {
          ok = false;
          std::printf("  FAIL %s rx status=%d\n", c.n, static_cast<int>(rm.status));
          break;
        }
        bit_err += rm.errors;
        bits += rm.bits;
        llr_cs = rm.checksum;
      } else if (rx && !c.ber) {
        auto rm = run_rx(*rx, d, slot);
        if (rm.status != OCUDU_DAPP_OK_V1) {
          ok = false;
          std::printf("  FAIL %s rx status=%d\n", c.n, static_cast<int>(rm.status));
          break;
        }
        llr_cs = rm.checksum;
      }
    }
    if (!ok) {
      ++fails;
      continue;
    }
    const double ser  = static_cast<double>(eq_err) / eq_sym;
    const double ber  = bits ? static_cast<double>(bit_err) / bits : 0.0;
    const bool   pass = eq_err == 0 && (!c.ber || bit_err == 0) && (!rx || llr_cs != 0);
    std::printf("  %s %-12s EQ-SER=%.3e RX-BER=%.3e eq_cs=%016llx llr_cs=%016llx%s\n", pass ? "PASS" : "FAIL", c.n, ser,
                ber, static_cast<unsigned long long>(eq_cs), static_cast<unsigned long long>(llr_cs),
                c.ber ? "" : " [EQ+RX health]");
    if (!pass)
      ++fails;
  }
  if (!rx)
    std::printf("  (no --rx-so: EQ-only chain)\n");
  std::printf("B chain: %s\n", fails ? "FAIL" : "PASS");
  return fails;
}

int run_sensitivity(package& eq, device_bufs& d, const options& opt)
{
  std::printf("\n=== C · SENSITIVITY SNR sweep ===\n");
  int                      fails = 0;
  const std::vector<float> snrs  = opt.quick ? std::vector<float>{0.F, 5.F, 10.F, 15.F, 20.F}
                                             : std::vector<float>{0.F, 2.5F, 5.F, 7.5F, 10.F, 12.5F, 15.F, 17.5F, 20.F};
  auto                     sweep = [&](unsigned mod) {
    const float  gate_snr = mod == 2 ? 15.F : 20.F;
    const double limit    = mod == 2 ? 1e-2 : 5e-2;
    double       ser_gate = 1.0;
    std::printf("  mod=%u\n", mod);
    for (float snr : snrs) {
      slot_shape sh{};
      sh.nof_prb         = 6;
      sh.mod_order       = mod;
      sh.snr_db          = snr;
      uint64_t       err = 0, sym = 0;
      double         mse_sum = 0;
      const unsigned frames  = opt.quick ? 3U : opt.frames;
      bool           ok      = true;
      for (unsigned f = 0; f < frames; ++f) {
        std::mt19937 rng(opt.seed + 2000 + static_cast<uint32_t>(snr * 10) + f * 17 + mod);
        auto         slot = build_slot(sh, rng);
        auto         m    = run_eq(eq, d, slot);
        if (m.status != OCUDU_DAPP_OK_V1) {
          ok = false;
          break;
        }
        err += m.errors;
        sym += m.symbols;
        mse_sum += m.mse * m.symbols;
      }
      if (!ok || !sym) {
        std::printf("    SNR=%.1f FAIL\n", snr);
        ++fails;
        continue;
      }
      const double ser = static_cast<double>(err) / sym;
      std::printf("    SNR=%5.1f  SER=%.3e  MSE=%.3e  sym=%llu\n", snr, ser, mse_sum / sym,
                                      static_cast<unsigned long long>(sym));
      if (std::abs(snr - gate_snr) < 0.1F)
        ser_gate = ser;
    }
    const bool pass = ser_gate <= limit;
    std::printf("  %s mod=%u SER@%.0fdB=%.3e (limit %.1e)\n", pass ? "PASS" : "FAIL", mod, gate_snr, ser_gate, limit);
    if (!pass)
      ++fails;
  };
  sweep(2);
  sweep(4);
  std::printf("C sensitivity: %s\n", fails ? "FAIL" : "PASS");
  return fails;
}

int run_tb(package& eq, device_bufs& d, const options& opt)
{
  std::printf("\n=== D · TB CRC24A (uncoded through EQ) ===\n");
  int  fails  = 0;
  auto one_tb = [&](float snr, unsigned frames) -> std::pair<unsigned, unsigned> {
    unsigned ok = 0, tot = 0;
    for (unsigned f = 0; f < frames; ++f) {
      slot_shape sh{};
      sh.nof_prb   = 6;
      sh.mod_order = 2;
      sh.snr_db    = snr;
      std::mt19937 probe(opt.seed + 3000 + f);
      auto         cap_slot = build_slot(sh, probe);
      const size_t cap      = cap_slot.h_tx_bits.size();
      require(cap > 32, "TB capacity");
      const size_t                       info = cap - 24;
      std::vector<uint8_t>               payload(cap);
      std::mt19937                       brng(opt.seed + 4000 + f * 31);
      std::uniform_int_distribution<int> bit(0, 1);
      for (size_t i = 0; i < info; ++i)
        payload[i] = static_cast<uint8_t>(bit(brng));
      const uint32_t crc = crc24a(payload.data(), info);
      for (int k = 0; k < 24; ++k)
        payload[info + k] = static_cast<uint8_t>((crc >> (23 - k)) & 1U);
      std::mt19937 rng(opt.seed + 5000 + f * 17);
      auto         slot = build_slot(sh, rng, payload.data(), payload.size());
      auto         m    = run_eq(eq, d, slot);
      ++tot;
      if (m.status != OCUDU_DAPP_OK_V1)
        continue;
      std::vector<uint8_t> rx;
      for (const auto& s : m.eq) {
        const int lab = hard_qpsk(s);
        rx.push_back(static_cast<uint8_t>(lab & 1));
        rx.push_back(static_cast<uint8_t>((lab >> 1) & 1));
      }
      if (rx.size() != cap)
        continue;
      const uint32_t rx_crc = crc24a(rx.data(), info);
      uint32_t       got    = 0;
      for (int k = 0; k < 24; ++k)
        got = (got << 1) | (rx[info + k] & 1U);
      if (rx_crc == got)
        ++ok;
    }
    return {ok, tot};
  };
  {
    const unsigned frames = opt.quick ? 4U : std::max(8U, opt.frames);
    auto [ok, tot]        = one_tb(30.F, frames);
    const bool pass       = ok == tot && tot > 0;
    std::printf("  %s highSNR(30dB) TB-CRC %u/%u\n", pass ? "PASS" : "FAIL", ok, tot);
    if (!pass)
      ++fails;
  }
  for (float snr :
       (opt.quick ? std::vector<float>{5.F, 10.F, 15.F, 20.F} : std::vector<float>{0.F, 5.F, 10.F, 15.F, 20.F, 25.F})) {
    auto [ok, tot] = one_tb(snr, opt.quick ? 3U : 6U);
    std::printf("    SNR=%5.1f  TB_OK=%u/%u\n", snr, ok, tot);
  }
  {
    auto [ok, tot]    = one_tb(20.F, opt.quick ? 4U : 8U);
    const double r    = tot ? static_cast<double>(ok) / tot : 0.0;
    const bool   pass = r >= 0.75;
    std::printf("  %s midSNR(20dB) rate=%.2f\n", pass ? "PASS" : "FAIL", r);
    if (!pass)
      ++fails;
  }
  std::printf("D tb-crc: %s\n", fails ? "FAIL" : "PASS");
  return fails;
}

} // namespace

int main(int argc, char** argv)
{
  const options opt = parse(argc, argv);
  std::printf("L1 validation suite (dlopen)  mode=%s  eq=%s  rx=%s  frames=%u\n", opt.mode.c_str(), opt.eq_so,
              opt.rx_so ? opt.rx_so : "(none)", opt.frames);

  package     eq{}, rx{};
  device_bufs dev;
  dev.init();
  eq.load(opt.eq_so, OCUDU_DAPP_INTERFACE_EQUALIZER_V1);
  package* rxp = nullptr;
  if (opt.rx_so) {
    rx.load(opt.rx_so, OCUDU_DAPP_INTERFACE_RECEIVER_V1);
    rxp = &rx;
  }

  int        fails = 0;
  const bool all   = opt.mode == "all";
  if (all || opt.mode == "golden")
    fails += run_golden(eq, dev, opt);
  if (all || opt.mode == "chain")
    fails += run_chain(eq, rxp, dev, opt);
  if (all || opt.mode == "sensitivity")
    fails += run_sensitivity(eq, dev, opt);
  if (all || opt.mode == "tb")
    fails += run_tb(eq, dev, opt);
  if (!all && opt.mode != "golden" && opt.mode != "chain" && opt.mode != "sensitivity" && opt.mode != "tb") {
    std::fprintf(stderr, "unknown mode %s\n", opt.mode.c_str());
    fails = 2;
  }

  if (rxp)
    rx.unload();
  eq.unload();
  dev.destroy();
  std::printf("\nRESULT: %s  (fails=%d)\n", fails ? "FAIL" : "PASS", fails);
  return fails ? 1 : 0;
}
