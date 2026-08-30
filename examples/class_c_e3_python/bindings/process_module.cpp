// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// pybind11: supervised Class-C process transport (CPU SHMEM + optional CUDA-IPC)
// with DLPack export for zero-copy (or device-view) PyTorch tensors.
//
//   torch.utils.dlpack.from_dlpack(handle.to_dlpack())
//
// Host launches the catalog worker with --control-fd=N (same as ref_spectrumd_*).
//
// This module exists so *research* Python workers can participate in Class-C
// out_of_process packages. It is not a production performance path and does
// not enable Python for Class-A/B or in-process MODULEs.
//
// Responsibilities:
//   * ProcessWorker: no_new_privs, route admission, wait/heartbeat, try_acquire
//   * PublicationHandle: meta, densify CBF16→CF32 numpy_grid / to_dlpack, release
//   * send_spectrum_result / send_srs_cir_result on the publication's route_index
//

#include "dlpack.h"
#include "ocudu/dapp/process/v1/control.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu_dapp_sdk/class_c_input.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/process_client.h"

#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
#include "ocudu_dapp_sdk/process_cuda_client.h"
#include <cuda_runtime_api.h>
#endif

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;
using namespace ocudu::dapp;
using namespace ocudu::dapp::sdk;
using namespace ocudu::dapp::reference;

namespace {

/// Keeps SHM/CUDA lease alive until PyTorch (or any DLPack consumer) frees the capsule.
struct DlpackLeaseContext {
  publication_lease    lease;
  std::vector<int64_t> shape;
  std::vector<int64_t> strides; // element strides; empty => compact row-major
  DLManagedTensor      managed{};
  // Optional private host staging (only if we must densify / D2H).
  std::unique_ptr<std::complex<float>[]> owned_host;
};

void dlpack_deleter(DLManagedTensor* self)
{
  if (self == nullptr) {
    return;
  }
  auto* ctx = static_cast<DlpackLeaseContext*>(self->manager_ctx);
  delete ctx;
}

py::capsule make_dlpack_capsule(std::unique_ptr<DlpackLeaseContext> ctx)
{
  ctx->managed.manager_ctx = ctx.get();
  ctx->managed.deleter     = &dlpack_deleter;
  // PyCapsule steals nothing from unique_ptr until we release.
  DLManagedTensor* raw = &ctx->managed;
  // Transfer ownership of ctx to the capsule via managed.manager_ctx; release unique_ptr.
  ctx.release();
  return py::capsule(raw, "dltensor", [](PyObject* obj) {
    auto* managed = static_cast<DLManagedTensor*>(PyCapsule_GetPointer(obj, "dltensor"));
    if (managed == nullptr) {
      // Name may already have been renamed to "used_dltensor" by consumer.
      PyErr_Clear();
      managed = static_cast<DLManagedTensor*>(PyCapsule_GetPointer(obj, "used_dltensor"));
      if (managed == nullptr) {
        PyErr_Clear();
        return;
      }
    }
    if (managed->deleter) {
      managed->deleter(managed);
    }
  });
}

void fill_compact_strides(const std::vector<int64_t>& shape, std::vector<int64_t>& strides)
{
  strides.resize(shape.size());
  int64_t acc = 1;
  for (int i = static_cast<int>(shape.size()) - 1; i >= 0; --i) {
    strides[static_cast<std::size_t>(i)] = acc;
    acc *= shape[static_cast<std::size_t>(i)];
  }
}

// Host UL grids from CUDA low-PHY are typically CBF16 (complex bfloat16).
// Convert to complex64 for PyTorch (kDLComplex/64).
float bf16_to_f32(uint16_t bits) noexcept
{
  const uint32_t bits32 = static_cast<uint32_t>(bits) << 16U;
  float          value  = 0.0F;
  std::memcpy(&value, &bits32, sizeof(value));
  return value;
}

std::complex<float> load_complex_sample(const strided_complex_view& view, uint64_t byte_offset)
{
  if (view.element_type == OCUDU_DAPP_ELEMENT_CF32_V1) {
    std::complex<float> sample{};
    std::memcpy(&sample, view.data + byte_offset, sizeof(sample));
    return sample;
  }
  if (view.element_type == OCUDU_DAPP_ELEMENT_CBF16_V1) {
    uint16_t re = 0;
    uint16_t im = 0;
    std::memcpy(&re, view.data + byte_offset, sizeof(re));
    std::memcpy(&im, view.data + byte_offset + sizeof(re), sizeof(im));
    return {bf16_to_f32(re), bf16_to_f32(im)};
  }
  throw std::runtime_error("unsupported element type for complex load: " + std::to_string(view.element_type));
}

std::size_t complex_element_bytes(uint32_t element_type) noexcept
{
  switch (element_type) {
    case OCUDU_DAPP_ELEMENT_CF32_V1:
      return sizeof(std::complex<float>);
    case OCUDU_DAPP_ELEMENT_CBF16_V1:
      return 4; // 2 x bf16
    default:
      return 0;
  }
}

bool is_dense_complex(const strided_complex_view& view)
{
  const auto elem = complex_element_bytes(view.element_type);
  if (view.data == nullptr || elem == 0 || view.rank == 0) {
    return false;
  }
  if (view.byte_strides[view.rank - 1] != elem) {
    return false;
  }
  for (uint32_t i = 0; i + 1 < view.rank; ++i) {
    if (view.byte_strides[i] != view.byte_strides[i + 1] * view.dimensions[i + 1]) {
      return false;
    }
  }
  return true;
}

py::array_t<std::complex<float>> copy_complex_tensor(const strided_complex_view& view)
{
  const auto elem = complex_element_bytes(view.element_type);
  if (view.data == nullptr || elem == 0 || view.rank == 0 || view.rank > 3) {
    throw std::runtime_error(
        "unsupported Class-C tensor (need CF32/CBF16 rank 1–3): data=" + std::string(view.data ? "nonnull" : "null") +
        " element_type=" + std::to_string(view.element_type) + " rank=" + std::to_string(view.rank) +
        " domain=" + std::to_string(view.memory_domain) + " backend=" + std::to_string(view.backend_id));
  }
  std::vector<py::ssize_t> shape(view.rank);
  std::size_t              count = 1;
  for (uint32_t i = 0; i != view.rank; ++i) {
    shape[i] = static_cast<py::ssize_t>(view.dimensions[i]);
    count *= static_cast<std::size_t>(view.dimensions[i]);
  }
  py::array_t<std::complex<float>> array(shape);
  auto*                            dst = array.mutable_data();
  if (view.element_type == OCUDU_DAPP_ELEMENT_CF32_V1 && is_dense_complex(view) &&
      view.byte_length >= count * sizeof(std::complex<float>)) {
    std::memcpy(dst, view.data, count * sizeof(std::complex<float>));
  } else if (view.element_type == OCUDU_DAPP_ELEMENT_CBF16_V1 && is_dense_complex(view) &&
             view.byte_length >= count * 4) {
    const auto* src = reinterpret_cast<const uint16_t*>(view.data);
    for (std::size_t i = 0; i != count; ++i) {
      dst[i] = {bf16_to_f32(src[2 * i]), bf16_to_f32(src[2 * i + 1])};
    }
  } else {
    const auto  dim0 = view.rank > 0 ? view.dimensions[0] : 1;
    const auto  dim1 = view.rank > 1 ? view.dimensions[1] : 1;
    const auto  dim2 = view.rank > 2 ? view.dimensions[2] : 1;
    const auto  s0   = view.rank > 0 ? view.byte_strides[0] : 0;
    const auto  s1   = view.rank > 1 ? view.byte_strides[1] : 0;
    const auto  s2   = view.rank > 2 ? view.byte_strides[2] : elem;
    std::size_t o    = 0;
    for (uint64_t i0 = 0; i0 != dim0; ++i0) {
      for (uint64_t i1 = 0; i1 != dim1; ++i1) {
        for (uint64_t i2 = 0; i2 != dim2; ++i2) {
          dst[o++] = load_complex_sample(view, i0 * s0 + i1 * s1 + i2 * s2);
        }
      }
    }
  }
  return array;
}

struct PublicationMeta {
  std::string           kind; // "spectrum" | "srs_isac"
  uint32_t              cell_id{};
  uint64_t              publication_sequence{};
  double                amplitude_scale{1.0}; // spectrum
  double                noise_variance{};     // srs_isac
  uint32_t              srs_resource_id{};
  uint32_t              srs_config_id{};
  std::string           memory;
  std::vector<uint64_t> shape;
  uint32_t              device_ordinal{UINT32_MAX};
  uint16_t              sfn{};
  uint16_t              slot{};
  uint8_t               numerology{};
};

bool add_radio_slots(const ocudu_dapp_radio_time_v1& input, uint32_t delta, ocudu_dapp_radio_time_v1& output) noexcept
{
  if (input.numerology > 4 || input.sfn >= 1024 || input.reserved != 0) {
    return false;
  }
  const uint32_t slots_per_frame = 10U << input.numerology;
  if (input.slot >= slots_per_frame) {
    return false;
  }
  const uint32_t cycle = 1024U * slots_per_frame;
  const uint32_t value = (static_cast<uint32_t>(input.sfn) * slots_per_frame + input.slot + delta) % cycle;
  output = {static_cast<uint16_t>(value / slots_per_frame), static_cast<uint16_t>(value % slots_per_frame),
            input.numerology, 0};
  return true;
}

ocudu_dapp_radio_time_v1 make_radio_time(uint16_t sfn, uint16_t slot, uint8_t numerology) noexcept
{
  ocudu_dapp_radio_time_v1 time{};
  time.sfn        = sfn;
  time.slot       = slot;
  time.numerology = numerology;
  time.reserved   = 0;
  return time;
}

class PublicationHandle
{
public:
  PublicationHandle(publication_lease lease, uint32_t interface_id, std::size_t route_index
#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
                    ,
                    cuda_process_route* cuda_route
#endif
                    ) :
    lease_(std::move(lease)), interface_id_(interface_id), route_index_(route_index)
#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
    ,
    cuda_route_(cuda_route)
#endif
  {
  }

  uint64_t    sequence() const { return lease_ ? lease_.sequence() : 0; }
  std::size_t route_index() const { return route_index_; }

  PublicationMeta meta()
  {
    ensure_decoded();
    return meta_;
  }

  /// Owned host copy (always safe after release()). Prefer to_dlpack() for zero-copy.
  py::array_t<std::complex<float>> numpy_grid()
  {
    ensure_decoded();
#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
    if (cuda_view_) {
      return d2h_numpy();
    }
#endif
    return copy_complex_tensor(decoded_.tensor);
  }

  /// DLPack capsule for torch.utils.dlpack.from_dlpack.
  /// Host SHM dense: zero-copy view of ring memory (lease held by capsule).
  /// Host SHM strided: densified private buffer (lease still held until free).
  /// CUDA-IPC: zero-copy device view (kDLCUDA); lease held until free.
  py::object to_dlpack()
  {
    ensure_decoded();
    if (!lease_) {
      throw std::runtime_error("publication lease already released");
    }
    auto ctx = std::make_unique<DlpackLeaseContext>();
    // Move lease into DLPack lifetime.
    ctx->lease = std::move(lease_);

    ctx->shape.resize(decoded_.tensor.rank);
    for (uint32_t i = 0; i != decoded_.tensor.rank; ++i) {
      ctx->shape[i] = static_cast<int64_t>(decoded_.tensor.dimensions[i]);
    }
    fill_compact_strides(ctx->shape, ctx->strides);

    void*    data_ptr = nullptr;
    DLDevice device{kDLCPU, 0};

#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
    // CUDA-IPC CF32 can zero-copy into torch. CBF16 device tensors are densified
    // to host complex64 (PyTorch has no portable complex-bf16 DLPack path here).
    if (cuda_view_ && decoded_.tensor.element_type == OCUDU_DAPP_ELEMENT_CF32_V1) {
      data_ptr             = const_cast<uint8_t*>(cuda_view_->data);
      device.device_type   = kDLCUDA;
      device.device_id     = static_cast<int32_t>(cuda_view_->device_ordinal);
      meta_.memory         = "cuda_ipc_dlpack";
      meta_.device_ordinal = cuda_view_->device_ordinal;
    } else
#endif
    {
      if (is_dense_complex(decoded_.tensor) && decoded_.tensor.element_type == OCUDU_DAPP_ELEMENT_CF32_V1) {
        data_ptr     = const_cast<uint8_t*>(decoded_.tensor.data);
        meta_.memory = "host_shm_dlpack";
      } else {
        // Densify / convert CBF16→CF32 into owned host storage.
        auto              arr   = copy_complex_tensor(decoded_.tensor);
        const std::size_t count = static_cast<std::size_t>(arr.size());
        ctx->owned_host.reset(new std::complex<float>[count]);
        std::memcpy(ctx->owned_host.get(), arr.data(), count * sizeof(std::complex<float>));
        data_ptr     = ctx->owned_host.get();
        meta_.memory = (decoded_.tensor.element_type == OCUDU_DAPP_ELEMENT_CBF16_V1) ? "host_shm_cbf16_to_cf32_dlpack"
                                                                                     : "host_shm_densified_dlpack";
      }
    }

    ctx->managed.dl_tensor.data        = data_ptr;
    ctx->managed.dl_tensor.device      = device;
    ctx->managed.dl_tensor.ndim        = static_cast<int32_t>(ctx->shape.size());
    ctx->managed.dl_tensor.dtype       = DLDataType{static_cast<uint8_t>(kDLComplex), 64, 1};
    ctx->managed.dl_tensor.shape       = ctx->shape.data();
    ctx->managed.dl_tensor.strides     = ctx->strides.data();
    ctx->managed.dl_tensor.byte_offset = 0;

    return make_dlpack_capsule(std::move(ctx));
  }

  void release()
  {
    lease_.reset();
#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
    cuda_view_.reset();
#endif
    decoded_ready_ = false;
  }

  explicit operator bool() const { return static_cast<bool>(lease_); }

private:
  void ensure_decoded()
  {
    if (decoded_ready_) {
      return;
    }
    if (!lease_) {
      throw std::runtime_error("publication lease already released");
    }
    const auto publication = lease_.publication();
    if (!publication) {
      throw std::runtime_error("invalid publication record on lease");
    }
    const uint8_t* tensor      = publication->tensor;
    std::size_t    tensor_size = publication->header.tensor_size;
    meta_.publication_sequence = publication->header.publication_sequence;
    meta_.memory               = "host_shm";

#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
    if ((publication->header.flags & OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1) != 0) {
      if (cuda_route_ == nullptr) {
        throw std::runtime_error("CUDA-IPC publication but no cuda_process_route");
      }
      cuda_view_ = cuda_route_->prepare(*publication);
      if (!cuda_view_) {
        throw std::runtime_error("cuda_process_route::prepare failed");
      }
      tensor               = cuda_view_->data;
      tensor_size          = cuda_view_->size;
      meta_.memory         = "cuda_ipc";
      meta_.device_ordinal = cuda_view_->device_ordinal;
    }
#else
    if ((publication->header.flags & OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1) != 0) {
      throw std::runtime_error("CUDA-IPC publication but extension built without CUDA process support");
    }
#endif

    if (!decode_class_c_input(publication->metadata, publication->header.metadata_size, tensor, tensor_size,
                              interface_id_, decoded_)) {
      throw std::runtime_error("decode_class_c_input failed");
    }
    if (decoded_.kind == class_c_input_kind::spectrum) {
      meta_.kind            = "spectrum";
      meta_.amplitude_scale = decoded_.amplitude_scale;
    } else if (decoded_.kind == class_c_input_kind::srs_isac) {
      meta_.kind            = "srs_isac";
      meta_.noise_variance  = decoded_.noise_variance;
      meta_.srs_resource_id = decoded_.srs_resource_id;
      meta_.srs_config_id   = decoded_.srs_config_id;
    } else if (decoded_.kind == class_c_input_kind::ul_resource_plan) {
      meta_.kind = "ul_resource_plan";
    } else {
      throw std::runtime_error("unsupported Class-C publication kind");
    }
    meta_.cell_id    = decoded_.cell_id;
    meta_.sfn        = decoded_.radio_time.sfn;
    meta_.slot       = decoded_.radio_time.slot;
    meta_.numerology = decoded_.radio_time.numerology;
    meta_.shape.assign(decoded_.tensor.dimensions, decoded_.tensor.dimensions + decoded_.tensor.rank);
    decoded_ready_ = true;
  }

#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
  py::array_t<std::complex<float>> d2h_numpy()
  {
    if (!cuda_view_) {
      throw std::runtime_error("D2H requires a CUDA publication view");
    }
    std::size_t              count = 1;
    std::vector<py::ssize_t> shape(decoded_.tensor.rank);
    for (uint32_t i = 0; i != decoded_.tensor.rank; ++i) {
      shape[i] = static_cast<py::ssize_t>(decoded_.tensor.dimensions[i]);
      count *= static_cast<std::size_t>(decoded_.tensor.dimensions[i]);
    }
    if (decoded_.tensor.element_type == OCUDU_DAPP_ELEMENT_CF32_V1) {
      const std::size_t                nbytes = count * sizeof(std::complex<float>);
      py::array_t<std::complex<float>> host(shape);
      if (cudaMemcpy(host.mutable_data(), cuda_view_->data, nbytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
        throw std::runtime_error(std::string("cudaMemcpy D2H failed: ") + cudaGetErrorString(cudaGetLastError()));
      }
      return host;
    }
    if (decoded_.tensor.element_type == OCUDU_DAPP_ELEMENT_CBF16_V1) {
      // Device CBF16 → host raw → convert to complex64.
      const std::size_t    nbytes = count * 4;
      std::vector<uint8_t> raw(nbytes);
      if (cudaMemcpy(raw.data(), cuda_view_->data, nbytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
        throw std::runtime_error(std::string("cudaMemcpy CBF16 D2H failed: ") + cudaGetErrorString(cudaGetLastError()));
      }
      strided_complex_view host_view = decoded_.tensor;
      host_view.data                 = raw.data();
      host_view.byte_length          = nbytes;
      host_view.memory_domain        = OCUDU_DAPP_MEM_HOST_V1;
      // Compact strides in element bytes.
      host_view.byte_strides[host_view.rank - 1] = 4;
      for (int i = static_cast<int>(host_view.rank) - 2; i >= 0; --i) {
        host_view.byte_strides[static_cast<std::size_t>(i)] = host_view.byte_strides[static_cast<std::size_t>(i + 1)] *
                                                              host_view.dimensions[static_cast<std::size_t>(i + 1)];
      }
      return copy_complex_tensor(host_view);
    }
    throw std::runtime_error("D2H unsupported element type: " + std::to_string(decoded_.tensor.element_type));
  }
#endif

  publication_lease lease_;
  uint32_t          interface_id_{};
  std::size_t       route_index_{0};
  bool              decoded_ready_{false};
  class_c_input     decoded_{};
  PublicationMeta   meta_{};
#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
  cuda_process_route*                  cuda_route_{};
  std::optional<cuda_publication_view> cuda_view_;
#endif
};

class ProcessWorker
{
public:
  ProcessWorker(int control_fd, uint32_t interface_id, int timeout_ms) :
    interface_id_(interface_id), management_state_{}
  {
    if (control_fd < 0) {
      throw std::invalid_argument("control_fd must be >= 0");
    }
    parent_pid_ = ::getppid();
    if (!bind_parent_lifetime(parent_pid_)) {
      throw std::runtime_error("bind_parent_lifetime failed (parent already gone?)");
    }
    management_state_.active.store(true, std::memory_order_release);
    auto result = receive_route_set(control_fd, parent_pid_, ::geteuid(), interface_id,
                                    std::chrono::milliseconds(timeout_ms), {&management_api(), &management_state_});
    if (!result) {
      throw std::runtime_error(result.detail.empty() ? "receive_route_set failed" : result.detail);
    }
    routes_ = std::move(result.route_set);

#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
    cuda_routes_.resize(routes_->size());
    for (std::size_t i = 0; i != routes_->size(); ++i) {
      auto& route = routes_->route(i);
      if (route.offer().transport_kind != OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1) {
        continue;
      }
      auto imported = cuda_process_route::create(route);
      if (!imported) {
        throw std::runtime_error(imported.detail.empty() ? "cuda_process_route::create failed" : imported.detail);
      }
      cuda_routes_[i] = std::move(imported.route);
    }
#endif
  }

  std::size_t route_count() const { return routes_ ? routes_->size() : 0; }
  bool        control_healthy() const { return routes_ && routes_->control_healthy(); }

  bool wait(int timeout_ms)
  {
    // Keep GIL during wait: control/heartbeat servicing must stay on this thread
    // with a consistent pybind environment. Poll is short (tens of ms).
    return routes_ && routes_->wait(std::chrono::milliseconds(timeout_ms));
  }

  /// Returns a PublicationHandle (with ``route_index`` set) or ``None``.
  /// Results MUST be sent on ``handle.route_index()`` or the host rejects them
  /// and eventually SIGKILLs the worker for consecutive protocol failures.
  py::object try_acquire()
  {
    if (!routes_) {
      return py::none();
    }
    for (std::size_t i = 0; i != routes_->size(); ++i) {
      auto& route = routes_->route(i);
      route.drain_notifications();
      if (auto lease = route.try_acquire()) {
#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
        return py::cast(PublicationHandle(std::move(*lease), interface_id_, i, cuda_routes_[i].get()));
#else
        return py::cast(PublicationHandle(std::move(*lease), interface_id_, i));
#endif
      }
    }
    return py::none();
  }

  bool send_spectrum_result(std::size_t route_index, uint64_t source_seq, uint32_t cell_id, uint32_t nof_prbs,
                            py::array_t<uint8_t, py::array::c_style | py::array::forcecast> confidence, uint16_t sfn,
                            uint16_t slot, uint8_t numerology)
  {
    if (!routes_ || route_index >= routes_->size()) {
      throw std::out_of_range("route_index");
    }
    if (source_seq == 0 || nof_prbs == 0 || nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 ||
        static_cast<std::size_t>(confidence.size()) < nof_prbs) {
      throw std::invalid_argument("invalid confidence / nof_prbs / sequence");
    }
    ocudu_dapp_process_spectrum_result_v1 result{};
    result.struct_size          = sizeof(result);
    result.source_grid_sequence = source_seq;
    result.cell_id              = cell_id;
    result.confidence_q16       = 1U << 16U;
    result.nof_prbs             = nof_prbs;
    result.created              = make_radio_time(sfn, slot, numerology);
    if (!add_radio_slots(result.created, 1, result.valid_from) ||
        !add_radio_slots(result.created, 4, result.valid_until)) {
      throw std::invalid_argument("invalid radio time for spectrum result");
    }
    std::memcpy(result.prb_confidence, confidence.data(), nof_prbs);
    return routes_->route(route_index).send_spectrum_result(result);
  }

  /// Send a bounded CIR preview (UE-TRP / SRS-ISAC example — not full ISAC).
  bool send_srs_cir_result(std::size_t route_index, uint64_t source_seq, uint32_t cell_id, uint32_t srs_resource_id,
                           uint32_t srs_config_id, uint32_t peak_delay_bin, float mean_channel_power,
                           float noise_corrected_power, float peak_cir_power, float noise_variance,
                           py::array_t<float, py::array::c_style | py::array::forcecast> tap_power, uint16_t sfn,
                           uint16_t slot, uint8_t numerology)
  {
    if (!routes_ || route_index >= routes_->size()) {
      throw std::out_of_range("route_index");
    }
    const auto n = static_cast<uint32_t>(tap_power.size());
    if (source_seq == 0 || n == 0 || n > OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1 || peak_delay_bin >= n) {
      throw std::invalid_argument("invalid CIR taps / peak_delay_bin / sequence");
    }
    // Host admits only finite non-negative powers with tap_power[i] <= peak_cir_power.
    float peak = peak_cir_power;
    for (uint32_t i = 0; i != n; ++i) {
      const float tap = tap_power.data()[i];
      if (!std::isfinite(tap) || tap < 0.0F) {
        throw std::invalid_argument("non-finite or negative CIR tap");
      }
      if (tap > peak) {
        peak = tap;
      }
    }
    if (!std::isfinite(peak) || peak < 0.0F || !std::isfinite(mean_channel_power) || mean_channel_power < 0.0F ||
        !std::isfinite(noise_corrected_power) || noise_corrected_power < 0.0F || !std::isfinite(noise_variance) ||
        noise_variance < 0.0F) {
      throw std::invalid_argument("non-finite CIR power fields");
    }

    ocudu_dapp_process_srs_isac_result_v1 result{};
    result.struct_size              = sizeof(result);
    auto& preview                   = result.preview;
    preview.struct_size             = sizeof(preview);
    preview.abi_major               = OCUDU_DAPP_ABI_MAJOR_V1;
    preview.abi_minor               = OCUDU_DAPP_ABI_MINOR_V1;
    preview.context.struct_size     = sizeof(preview.context);
    preview.context.schema_id       = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
    preview.context.schema_version  = 1;
    preview.context.source_sequence = source_seq;
    preview.context.cell_id         = cell_id;
    preview.context.confidence_q16  = 1U << 16U;
    preview.context.created         = make_radio_time(sfn, slot, numerology);
    preview.context.valid_from      = preview.context.created;
    preview.context.valid_until     = preview.context.created;
    preview.source_srs_sequence     = source_seq;
    preview.srs_resource_id         = srs_resource_id;
    preview.srs_config_id           = srs_config_id;
    preview.tap_count               = n;
    preview.peak_delay_bin          = peak_delay_bin;
    preview.mean_channel_power      = mean_channel_power;
    preview.noise_corrected_power   = noise_corrected_power;
    preview.peak_cir_power          = peak;
    preview.noise_variance          = noise_variance;
    std::memcpy(preview.tap_power, tap_power.data(), n * sizeof(float));
    return routes_->route(route_index).send_srs_isac_result(result);
  }

private:
  uint32_t                           interface_id_{};
  pid_t                              parent_pid_{};
  instance                           management_state_{};
  std::unique_ptr<process_route_set> routes_;
#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
  std::vector<std::unique_ptr<cuda_process_route>> cuda_routes_;
#endif
};

} // namespace

PYBIND11_MODULE(ocudu_dapp_class_c_process, m)
{
  m.doc() = "Supervised Class-C process transport (CPU SHMEM + CUDA-IPC) with DLPack→PyTorch.";

  m.attr("INTERFACE_SPECTRUM") = py::int_(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  m.attr("INTERFACE_SRS_ISAC") = py::int_(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
#ifdef OCUDU_DAPP_PY_CUDA_PROCESS
  m.attr("CUDA_PROCESS_ENABLED") = true;
#else
  m.attr("CUDA_PROCESS_ENABLED") = false;
#endif

  py::class_<PublicationMeta>(m, "PublicationMeta")
      .def_readonly("kind", &PublicationMeta::kind)
      .def_readonly("cell_id", &PublicationMeta::cell_id)
      .def_readonly("publication_sequence", &PublicationMeta::publication_sequence)
      .def_readonly("amplitude_scale", &PublicationMeta::amplitude_scale)
      .def_readonly("noise_variance", &PublicationMeta::noise_variance)
      .def_readonly("srs_resource_id", &PublicationMeta::srs_resource_id)
      .def_readonly("srs_config_id", &PublicationMeta::srs_config_id)
      .def_readonly("memory", &PublicationMeta::memory)
      .def_readonly("shape", &PublicationMeta::shape)
      .def_readonly("device_ordinal", &PublicationMeta::device_ordinal)
      .def_readonly("sfn", &PublicationMeta::sfn)
      .def_readonly("slot", &PublicationMeta::slot)
      .def_readonly("numerology", &PublicationMeta::numerology);

  py::class_<PublicationHandle>(m, "PublicationHandle")
      .def("sequence", &PublicationHandle::sequence)
      .def("route_index", &PublicationHandle::route_index,
           "Route index this publication was acquired from (use for send_*_result).")
      .def("meta", &PublicationHandle::meta)
      .def("numpy_grid", &PublicationHandle::numpy_grid, "Owned host complex64 copy (safe after release).")
      .def("to_dlpack", &PublicationHandle::to_dlpack,
           "DLPack capsule: zero-copy host SHM or CUDA device view; lease held until free.\n"
           "  import torch\n"
           "  t = torch.utils.dlpack.from_dlpack(handle.to_dlpack())")
      .def("release", &PublicationHandle::release)
      .def("__bool__", &PublicationHandle::operator bool)
      .def("__enter__", [](PublicationHandle& self) -> PublicationHandle& { return self; })
      .def("__exit__", [](PublicationHandle& self, py::object, py::object, py::object) {
        self.release();
        return false;
      });

  py::class_<ProcessWorker>(m, "ProcessWorker")
      .def(py::init<int, uint32_t, int>(), py::arg("control_fd"),
           py::arg("interface_id") = OCUDU_DAPP_INTERFACE_SPECTRUM_V1, py::arg("timeout_ms") = 5000)
      .def("route_count", &ProcessWorker::route_count)
      .def("control_healthy", &ProcessWorker::control_healthy)
      .def("wait", &ProcessWorker::wait, py::arg("timeout_ms") = 250)
      .def("try_acquire", &ProcessWorker::try_acquire)
      .def("send_spectrum_result", &ProcessWorker::send_spectrum_result, py::arg("route_index"),
           py::arg("source_sequence"), py::arg("cell_id"), py::arg("nof_prbs"), py::arg("confidence"),
           py::arg("sfn") = 0, py::arg("slot") = 0, py::arg("numerology") = 0)
      .def("send_srs_cir_result", &ProcessWorker::send_srs_cir_result, py::arg("route_index"),
           py::arg("source_sequence"), py::arg("cell_id"), py::arg("srs_resource_id"), py::arg("srs_config_id"),
           py::arg("peak_delay_bin"), py::arg("mean_channel_power"), py::arg("noise_corrected_power"),
           py::arg("peak_cir_power"), py::arg("noise_variance"), py::arg("tap_power"), py::arg("sfn") = 0,
           py::arg("slot") = 0, py::arg("numerology") = 0);
}
