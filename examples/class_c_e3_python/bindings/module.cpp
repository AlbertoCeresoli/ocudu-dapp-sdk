// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Thin pybind11 surface over the *real* C++ portable E3 stack:
//   ocudu_portable_e3_consumer  →  OCUDU::DAppManagementClient  →  APER codec
//   decode_class_c_input        →  FlatBuffers Class-C metadata
//
// No protocol reimplementation in Python. Requires host E3 static libs built
// with POSITION_INDEPENDENT_CODE (see ocudu/lib/dapp/CMakeLists.txt).
//
// Portable E3 is an independent low-rate Class-C consumer (not a catalog
// MODULE, not supervised SHMEM). Research / education only for Python sensing.

#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu_dapp_sdk/class_c_input.h"
#include "ocudu_dapp_sdk/portable_e3_consumer.h"
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
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
using namespace ocudu::dapp::reference;

namespace {

bool parse_subscription_hex(const std::string& text, management::identifier& out)
{
  out = {};
  unsigned    byte{};
  std::size_t index{};
  for (char value : text) {
    if (value == '-' || value == ' ') {
      continue;
    }
    const unsigned digit = value >= '0' && value <= '9'   ? static_cast<unsigned>(value - '0')
                           : value >= 'a' && value <= 'f' ? static_cast<unsigned>(value - 'a' + 10)
                           : value >= 'A' && value <= 'F' ? static_cast<unsigned>(value - 'A' + 10)
                                                          : 16U;
    if (digit == 16 || index >= 32) {
      return false;
    }
    if ((index & 1U) == 0) {
      byte = digit << 4U;
    } else {
      out.bytes[index / 2] = static_cast<uint8_t>(byte | digit);
    }
    ++index;
  }
  return index == 32 && !out.is_zero();
}

py::array_t<std::complex<float>> copy_complex_tensor(const strided_complex_view& view)
{
  if (view.data == nullptr || view.element_type != OCUDU_DAPP_ELEMENT_CF32_V1 || view.rank == 0 || view.rank > 3) {
    throw std::runtime_error("unsupported Class-C tensor (need host CF32 rank 1–3)");
  }
  std::vector<py::ssize_t> shape(view.rank);
  std::size_t              count = 1;
  for (uint32_t i = 0; i != view.rank; ++i) {
    shape[i] = static_cast<py::ssize_t>(view.dimensions[i]);
    count *= static_cast<std::size_t>(view.dimensions[i]);
  }
  py::array_t<std::complex<float>> array(shape);
  auto*                            dst   = array.mutable_data();
  const auto*                      src   = view.data;
  bool                             dense = view.byte_strides[view.rank - 1] == sizeof(std::complex<float>);
  if (dense && view.rank >= 2) {
    for (uint32_t i = 0; i + 1 < view.rank; ++i) {
      if (view.byte_strides[i] != view.byte_strides[i + 1] * view.dimensions[i + 1]) {
        dense = false;
        break;
      }
    }
  }
  if (dense && view.byte_length >= count * sizeof(std::complex<float>)) {
    std::memcpy(dst, src, count * sizeof(std::complex<float>));
    return array;
  }
  const auto  dim0 = view.rank > 0 ? view.dimensions[0] : 1;
  const auto  dim1 = view.rank > 1 ? view.dimensions[1] : 1;
  const auto  dim2 = view.rank > 2 ? view.dimensions[2] : 1;
  const auto  s0   = view.rank > 0 ? view.byte_strides[0] : 0;
  const auto  s1   = view.rank > 1 ? view.byte_strides[1] : 0;
  const auto  s2   = view.rank > 2 ? view.byte_strides[2] : sizeof(std::complex<float>);
  std::size_t o    = 0;
  for (uint64_t i0 = 0; i0 != dim0; ++i0) {
    for (uint64_t i1 = 0; i1 != dim1; ++i1) {
      for (uint64_t i2 = 0; i2 != dim2; ++i2) {
        std::memcpy(&dst[o++], src + i0 * s0 + i1 * s1 + i2 * s2, sizeof(std::complex<float>));
      }
    }
  }
  return array;
}

struct SpectrumObservation {
  uint32_t                         cell_id{};
  uint64_t                         publication_sequence{};
  uint64_t                         dropped_objects{};
  double                           amplitude_scale{1.0};
  py::array_t<std::complex<float>> grid;
  std::vector<uint64_t>            shape;
};

struct Publication {
  uint32_t                           interface_id{};
  uint32_t                           cell_id{};
  uint64_t                           publication_sequence{};
  uint64_t                           dropped_objects{};
  py::bytes                          metadata;
  py::bytes                          tensor_raw;
  std::optional<SpectrumObservation> spectrum;
};

class PortableE3Consumer
{
public:
  PortableE3Consumer(std::string socket_path, std::string subscription_hex, uint32_t interface_id,
                     uint32_t sample_every_n, uint32_t queue_depth_chunks, uint32_t maximum_chunk_bytes,
                     uint64_t lease_us, uint64_t maximum_tensor_bytes, int timeout_ms, std::optional<uint32_t> cell_id)
  {
    portable_e3_consumer_config config;
    config.socket_path          = std::move(socket_path);
    config.interface_id         = interface_id;
    config.sample_every_n       = sample_every_n;
    config.queue_depth_chunks   = queue_depth_chunks;
    config.maximum_chunk_bytes  = maximum_chunk_bytes;
    config.lease_us             = lease_us;
    config.maximum_tensor_bytes = maximum_tensor_bytes;
    config.timeout              = std::chrono::milliseconds(timeout_ms);
    config.cell_id              = cell_id;
    if (!parse_subscription_hex(subscription_hex, config.subscription_id)) {
      throw std::invalid_argument("subscription must be 16-byte hex (32 hex digits, optional dashes)");
    }
    auto [consumer, detail] = portable_e3_consumer::create(std::move(config));
    if (!consumer) {
      throw std::runtime_error(detail.empty() ? "failed to create portable E3 consumer" : detail);
    }
    consumer_     = std::move(consumer);
    interface_id_ = interface_id;
  }

  void subscribe()
  {
    std::string detail;
    if (!consumer_->subscribe(detail)) {
      throw std::runtime_error(detail.empty() ? "subscribe failed" : detail);
    }
  }

  void unsubscribe()
  {
    std::string detail;
    if (!consumer_->unsubscribe(detail)) {
      throw std::runtime_error(detail.empty() ? "unsubscribe failed" : detail);
    }
  }

  py::object poll()
  {
    const auto result = consumer_->poll();
    if (result.status == portable_e3_poll_status::no_data) {
      return py::none();
    }
    if (result.status == portable_e3_poll_status::error || !result.object) {
      throw std::runtime_error(result.detail.empty() ? "portable E3 poll failed" : result.detail);
    }
    return py::cast(make_decoded(*result.object));
  }

private:
  Publication make_decoded(const portable_e3_object& object) const
  {
    Publication out;
    out.interface_id         = object.interface_id;
    out.cell_id              = object.cell_id;
    out.publication_sequence = object.publication_sequence;
    out.dropped_objects      = object.dropped_objects;
    out.metadata             = py::bytes(reinterpret_cast<const char*>(object.metadata.data()), object.metadata.size());
    out.tensor_raw           = py::bytes(reinterpret_cast<const char*>(object.tensor.data()), object.tensor.size());

    class_c_input decoded{};
    if (!decode_class_c_input(object.metadata.data(), object.metadata.size(), object.tensor.data(),
                              object.tensor.size(), interface_id_, decoded)) {
      return out;
    }
    if (decoded.kind == class_c_input_kind::spectrum) {
      SpectrumObservation so;
      so.cell_id              = object.cell_id;
      so.publication_sequence = object.publication_sequence;
      so.dropped_objects      = object.dropped_objects;
      so.amplitude_scale      = decoded.amplitude_scale;
      so.grid                 = copy_complex_tensor(decoded.tensor);
      so.shape.assign(decoded.tensor.dimensions, decoded.tensor.dimensions + decoded.tensor.rank);
      out.spectrum = std::move(so);
    }
    return out;
  }

  std::unique_ptr<portable_e3_consumer> consumer_;
  uint32_t                              interface_id_{};
};

} // namespace

PYBIND11_MODULE(ocudu_dapp_class_c_e3, m)
{
  m.doc() = "Native portable E3 Class-C client (C++ portable_e3_consumer + decode_class_c_input). "
            "Out-of-process only; pairs with Python/PyTorch sensing.";

  m.attr("INTERFACE_SPECTRUM") = py::int_(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  m.attr("INTERFACE_SRS_ISAC") = py::int_(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);

  py::class_<SpectrumObservation>(m, "SpectrumObservation")
      .def_readonly("cell_id", &SpectrumObservation::cell_id)
      .def_readonly("publication_sequence", &SpectrumObservation::publication_sequence)
      .def_readonly("dropped_objects", &SpectrumObservation::dropped_objects)
      .def_readonly("amplitude_scale", &SpectrumObservation::amplitude_scale)
      .def_readonly("grid", &SpectrumObservation::grid, "complex64 ndarray [rx, symbol, subcarrier]")
      .def_readonly("shape", &SpectrumObservation::shape);

  py::class_<Publication>(m, "Publication")
      .def_readonly("interface_id", &Publication::interface_id)
      .def_readonly("cell_id", &Publication::cell_id)
      .def_readonly("publication_sequence", &Publication::publication_sequence)
      .def_readonly("dropped_objects", &Publication::dropped_objects)
      .def_readonly("metadata", &Publication::metadata)
      .def_readonly("tensor_raw", &Publication::tensor_raw)
      .def_readonly("spectrum", &Publication::spectrum);

  py::class_<PortableE3Consumer>(m, "PortableE3Consumer")
      .def(py::init<std::string, std::string, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, int,
                    std::optional<uint32_t>>(),
           py::arg("socket_path"), py::arg("subscription_hex"),
           py::arg("interface_id") = OCUDU_DAPP_INTERFACE_SPECTRUM_V1, py::arg("sample_every_n") = 10,
           py::arg("queue_depth_chunks") = 64, py::arg("maximum_chunk_bytes") = 64 * 1024,
           py::arg("lease_us") = 1'000'000, py::arg("maximum_tensor_bytes") = 4ULL * 1024 * 1024,
           py::arg("timeout_ms") = 1000, py::arg("cell_id") = py::none())
      .def("subscribe", &PortableE3Consumer::subscribe)
      .def("unsubscribe", &PortableE3Consumer::unsubscribe)
      .def("poll", &PortableE3Consumer::poll, "Return Publication, or None if no data.");
}
