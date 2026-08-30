// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/class_c_algorithms.h"
#include "ocudu_dapp_sdk/class_c_input.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/process_client.h"
#include "ocudu_dapp_sdk/reference_spectrum_policy.h"
#ifdef OCUDU_REFERENCE_CUDA_PROCESS
#include "ocudu_dapp_sdk/process_cuda_client.h"
#include "ocudu_dapp_sdk/spectrum_cuda.h"
#include "ocudu_dapp_sdk/srs_isac_cuda.h"
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

using namespace ocudu::dapp::sdk;
using namespace ocudu::dapp::reference;
using namespace std::chrono_literals;

#ifndef OCUDU_REFERENCE_INTERFACE_ID
#error "OCUDU_REFERENCE_INTERFACE_ID is required"
#endif

#ifndef OCUDU_REFERENCE_OBSERVER_ONLY
#define OCUDU_REFERENCE_OBSERVER_ONLY 0
#endif

#if OCUDU_REFERENCE_OBSERVER_ONLY && OCUDU_REFERENCE_INTERFACE_ID != 0x00010001
#error "The observer-only reference profile is defined only for spectrum sensing"
#endif

namespace {

volatile std::sig_atomic_t stopping = 0;

#ifdef OCUDU_REFERENCE_CUDA_PROCESS
struct cuda_route_set {
  std::vector<std::unique_ptr<cuda_process_route>> routes;
#if OCUDU_REFERENCE_INTERFACE_ID == 0x00010001
  std::vector<spectrum_cuda_worker_state> workers;
#elif OCUDU_REFERENCE_INTERFACE_ID == 0x00010002
  std::vector<srs_isac_cuda_worker_state> workers;
#else
#error "Unsupported CUDA Class-C reference interface"
#endif

  ~cuda_route_set()
  {
    for (auto& worker : workers) {
#if OCUDU_REFERENCE_INTERFACE_ID == 0x00010001
      release_spectrum_cuda_worker_state(worker);
#else
      release_srs_isac_cuda_worker_state(worker);
#endif
    }
  }
};
#endif

void request_stop(int)
{
  stopping = 1;
}

uint64_t monotonic_time_ns() noexcept
{
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<uint64_t>(value) : 0;
}

#if OCUDU_REFERENCE_INTERFACE_ID == 0x00010001 && !OCUDU_REFERENCE_OBSERVER_ONLY
bool add_slots(const ocudu_dapp_radio_time_v1& input, uint32_t delta, ocudu_dapp_radio_time_v1& output) noexcept
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
#endif

int parse_control_fd(int argc, char** argv, bool& once)
{
  int control_fd = -1;
  for (int i = 1; i != argc; ++i) {
    constexpr const char prefix[] = "--control-fd=";
    if (std::strncmp(argv[i], prefix, sizeof(prefix) - 1) == 0) {
      char* end        = nullptr;
      errno            = 0;
      const long value = std::strtol(argv[i] + sizeof(prefix) - 1, &end, 10);
      if (errno != 0 || end == nullptr || *end != '\0' || value < 0 || value > INT32_MAX)
        return -1;
      control_fd = static_cast<int>(value);
    } else if (std::strcmp(argv[i], "--once") == 0) {
      once = true;
    } else {
      return -1;
    }
  }
  return control_fd;
}

int run(int argc, char** argv)
{
  bool      once       = false;
  const int control_fd = parse_control_fd(argc, argv, once);
  if (control_fd < 0)
    return 2;
  const pid_t parent_pid = ::getppid();
  if (!bind_parent_lifetime(parent_pid) || !apply_restricted_process_policy())
    return 2;
  instance management_state;
  management_state.active.store(true, std::memory_order_release);
#ifdef OCUDU_REFERENCE_OPTIONAL_DMRS_INTERFACE_ID
  const std::array<process_interface_expectation, 2> expected{
      {{OCUDU_REFERENCE_INTERFACE_ID, true}, {OCUDU_REFERENCE_OPTIONAL_DMRS_INTERFACE_ID, false}}};
  auto routes = receive_route_bundle(control_fd, parent_pid, ::geteuid(), expected.data(), expected.size(), 5s,
                                     {&management_api(), &management_state});
#else
  auto routes = receive_route_set(control_fd, parent_pid, ::geteuid(), OCUDU_REFERENCE_INTERFACE_ID, 5s,
                                  {&management_api(), &management_state});
#endif
  if (!routes) {
    std::cerr << routes.detail << '\n';
    return 3;
  }

#ifdef OCUDU_REFERENCE_CUDA_PROCESS
  cuda_route_set cuda;
  cuda.routes.resize(routes.route_set->size());
  cuda.workers.resize(routes.route_set->size());
  for (std::size_t index = 0; index != routes.route_set->size(); ++index) {
    auto& route = routes.route_set->route(index);
    if (route.offer().transport_kind != OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1) {
      continue;
    }
    auto imported = cuda_process_route::create(route);
#if OCUDU_REFERENCE_INTERFACE_ID == 0x00010001
    const auto initialized = initialize_spectrum_cuda_worker_state(cuda.workers[index]);
#else
    const auto initialized = initialize_srs_isac_cuda_worker_state(cuda.workers[index]);
#endif
    if (!imported || initialized != OCUDU_DAPP_OK_V1) {
      std::cerr << (imported ? "unable to initialize CUDA Class-C worker" : imported.detail) << '\n';
      return 4;
    }
    cuda.routes[index] = std::move(imported.route);
  }
#endif

#if OCUDU_REFERENCE_INTERFACE_ID == 0x00010001 && !OCUDU_REFERENCE_OBSERVER_ONLY
  spectrum_quiet_authority quiet_authority;
#endif

  while (!stopping) {
    if (!routes.route_set->wait(250ms)) {
      if (!routes.route_set->control_healthy())
        return 5;
      continue;
    }
    for (std::size_t index = 0; index != routes.route_set->size(); ++index) {
      auto& route = routes.route_set->route(index);
      route.drain_notifications();
      uint32_t superseded = route.pending_publications();
      if (superseded != 0) {
        --superseded;
      }
      while (auto lease = route.try_acquire()) {
        if (superseded != 0) {
          --superseded;
          continue;
        }
        const auto publication = lease->publication();
        if (!publication)
          return 5;
        const uint8_t* tensor      = publication->tensor;
        std::size_t    tensor_size = publication->header.tensor_size;
#ifdef OCUDU_REFERENCE_CUDA_PROCESS
        std::optional<cuda_publication_view> cuda_publication;
        if ((publication->header.flags & OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1) != 0) {
          if (cuda.routes[index] == nullptr || !(cuda_publication = cuda.routes[index]->prepare(*publication)))
            return 5;
          tensor      = cuda_publication->data;
          tensor_size = cuda_publication->size;
        }
#endif
        class_c_input decoded{};
        if (!decode_class_c_input(publication->metadata, publication->header.metadata_size, tensor, tensor_size,
                                  OCUDU_REFERENCE_INTERFACE_ID, decoded))
          return 5;
        if (decoded.source_sequence != publication->header.publication_sequence)
          return 5;
        if (class_c_input_expired(decoded, monotonic_time_ns())) {
          // Stale records are normal while this worker was still initializing
          // (CUDA context setup takes seconds); keep draining so the freshest
          // queued record is reached instead of waiting for the next doorbell.
          if (once)
            return 0;
          continue;
        }
        if (begin_invocation(management_state) == OCUDU_DAPP_BYPASS_V1) {
          if (once)
            return 0;
          break;
        }
#if OCUDU_REFERENCE_INTERFACE_ID == 0x00010001
        if (decoded.kind == class_c_input_kind::ul_resource_plan) {
          if ((publication->header.flags & OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1) != 0)
            return 5;
          if (once)
            return 0;
          break;
        }
        if (decoded.kind != class_c_input_kind::spectrum ||
#ifdef OCUDU_REFERENCE_CUDA_PROCESS
            (publication->header.flags & OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1) == 0)
#else
            (publication->header.flags & OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1) == 0)
#endif
          return 5;
        std::array<uint8_t, OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1> confidence{};
        uint32_t                                             nof_prbs = 0;
        bool                                                 detected;
#ifdef OCUDU_REFERENCE_CUDA_PROCESS
        if (!cuda_publication || decoded.tensor.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1 ||
            decoded.tensor.memory_domain != OCUDU_DAPP_MEM_ACCELERATOR_IPC_EXPORT_V1) {
          return 5;
        }
        ocudu_dapp_spectrum_input_v1 cuda_input{};
        cuda_input.invocation.struct_size      = sizeof(cuda_input.invocation);
        cuda_input.invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
        cuda_input.invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
        cuda_input.invocation.execution_stream = cuda_publication->execution_stream;
        auto& grid                             = cuda_input.uplink_resource_grid;
        grid.struct_size                       = sizeof(grid);
        grid.abi_major                         = OCUDU_DAPP_ABI_MAJOR_V1;
        grid.abi_minor                         = OCUDU_DAPP_ABI_MINOR_V1;
        grid.domain                            = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
        grid.backend_id                        = OCUDU_DAPP_BACKEND_CUDA_V1;
        grid.device_ordinal                    = cuda_publication->device_ordinal;
        grid.element_type                      = decoded.tensor.element_type;
        grid.rank                              = decoded.tensor.rank;
        grid.access                            = OCUDU_DAPP_ACCESS_READ_V1;
        grid.allocation_id                     = decoded.source_sequence;
        grid.allocation_generation             = decoded.source_sequence;
        grid.native_handle                     = reinterpret_cast<uintptr_t>(decoded.tensor.data);
        grid.byte_length                       = decoded.tensor.byte_length;
        std::copy_n(decoded.tensor.dimensions, grid.rank, grid.dimensions);
        std::copy_n(decoded.tensor.byte_strides, grid.rank, grid.byte_strides);
        grid.layout_id      = OCUDU_DAPP_LAYOUT_RESOURCE_GRID_PORT_SYMBOL_SUBCARRIER_V1;
        grid.layout_version = 1;
        grid.lease_token    = decoded.source_sequence;
        if (decoded.expected_ul_use.plan_generation != 0) {
          auto& plan                 = cuda_input.scheduled_re_mask;
          plan.struct_size           = sizeof(plan);
          plan.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
          plan.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
          plan.domain                = OCUDU_DAPP_MEM_HOST_V1;
          plan.backend_id            = OCUDU_DAPP_BACKEND_HOST_V1;
          plan.element_type          = OCUDU_DAPP_ELEMENT_U8_V1;
          plan.rank                  = 2;
          plan.access                = OCUDU_DAPP_ACCESS_READ_V1 | OCUDU_DAPP_ACCESS_HOST_VISIBLE_V1;
          plan.allocation_id         = decoded.source_sequence;
          plan.allocation_generation = decoded.expected_ul_use.plan_generation;
          plan.native_handle         = reinterpret_cast<uintptr_t>(decoded.expected_ul_use.data);
          plan.byte_length     = static_cast<uint64_t>(decoded.expected_ul_use.symbols) * decoded.expected_ul_use.prbs;
          plan.dimensions[0]   = decoded.expected_ul_use.symbols;
          plan.dimensions[1]   = decoded.expected_ul_use.prbs;
          plan.byte_strides[0] = decoded.expected_ul_use.prbs;
          plan.byte_strides[1] = sizeof(uint8_t);
          plan.layout_id       = OCUDU_DAPP_LAYOUT_EXPECTED_UL_USE_SYMBOL_PRB_U8_V1;
          plan.layout_version  = 1;
          plan.lease_token     = decoded.source_sequence;
        }
        spectrum_cuda_result cuda_result{};
        const auto           status =
            run_spectrum_radiometer_cuda(cuda_input, decoded.amplitude_scale, cuda.workers[index], cuda_result);
        if (status != OCUDU_DAPP_OK_V1 || cuda_result.nof_prbs > confidence.size())
          return 5;
        std::copy_n(cuda_result.confidence, cuda_result.nof_prbs, confidence.begin());
        nof_prbs = cuda_result.nof_prbs;
        detected = true;
#else
        if (decoded.expected_ul_use.plan_generation != 0) {
          detected = run_spectrum_prb_radiometer(decoded.tensor, decoded.expected_ul_use, decoded.amplitude_scale, 1.0,
                                                 12, confidence.data(), confidence.size(), nof_prbs);
        } else {
          detected = run_spectrum_prb_radiometer(decoded.tensor, decoded.amplitude_scale, 1.0, 12, confidence.data(),
                                                 confidence.size(), nof_prbs);
        }
#endif
        if (!detected)
          return 5;
#if OCUDU_REFERENCE_OBSERVER_ONLY
        if (once)
          return 0;
        continue;
#else
        ocudu_dapp_process_spectrum_result_v1 result{};
        result.source_grid_sequence   = decoded.source_sequence;
        result.source_plan_generation = decoded.expected_ul_use.plan_generation;
        result.cell_id                = decoded.cell_id;
#ifdef OCUDU_REFERENCE_INVALID_PROCESS_RESULT
        // Test-only target: violate source correlation without changing the
        // production reference worker or the process transport protocol.
        result.cell_id = UINT32_MAX;
#endif
        result.confidence_q16 = 1U << 16U;
        result.created        = decoded.radio_time;
        result.nof_prbs       = nof_prbs;
        std::copy_n(confidence.begin(), nof_prbs, result.prb_confidence);
        if (!add_slots(result.created, 1, result.valid_from) || !add_slots(result.created, 4, result.valid_until) ||
            !route.send_spectrum_result(result))
          return 5;
        const auto occupied =
            std::find_if(confidence.begin(), confidence.begin() + nof_prbs, [](uint8_t value) { return value != 0; });
        if (occupied != confidence.begin() + nof_prbs) {
          const uint64_t now = monotonic_time_ns();
          if (!quiet_authority.try_admit(decoded.cell_id, now)) {
            if (once)
              return 0;
            break;
          }
          ocudu_dapp_process_quiet_intent_result_v1 quiet{};
          quiet.source_grid_sequence   = decoded.source_sequence;
          quiet.source_plan_generation = decoded.expected_ul_use.plan_generation;
          quiet.cell_id                = decoded.cell_id;
          quiet.created                = decoded.radio_time;
          auto& intent                 = quiet.intent;
          intent.struct_size           = sizeof(intent);
          intent.abi_major             = OCUDU_DAPP_ABI_MAJOR_V1;
          intent.abi_minor             = OCUDU_DAPP_ABI_MINOR_V1;
          intent.request_id[0]         = decoded.source_sequence;
          intent.request_id[1]         = UINT64_C(0x5155494554);
          if (!add_slots(decoded.radio_time, 4, intent.earliest_start))
            return 5;
          intent.latest_start        = intent.earliest_start;
          intent.duration_slots      = 1;
          intent.repeat_count        = 1;
          intent.first_prb           = static_cast<uint16_t>(occupied - confidence.begin());
          intent.nof_prbs            = 1;
          intent.requested_level     = OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1;
          intent.budget_class        = 1;
          intent.confidence_q16      = 1U << 15U;
          intent.expiry_monotonic_ns = spectrum_quiet_authority::expiry(now);
          if (!route.send_quiet_intent_result(quiet))
            return 5;
        }
#endif
#elif OCUDU_REFERENCE_INTERFACE_ID == 0x00010002
        if (decoded.kind != class_c_input_kind::srs_isac ||
#ifdef OCUDU_REFERENCE_CUDA_PROCESS
            (publication->header.flags & OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1) == 0)
#else
            (publication->header.flags & OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1) == 0)
#endif
          return 5;
        srs_cir_analysis analysis{};
#ifdef OCUDU_REFERENCE_CUDA_PROCESS
        if (!cuda_publication || decoded.tensor.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1 ||
            decoded.tensor.memory_domain != OCUDU_DAPP_MEM_ACCELERATOR_IPC_EXPORT_V1) {
          return 5;
        }
        ocudu_dapp_srs_isac_input_v1 cuda_input{};
        cuda_input.invocation.struct_size      = sizeof(cuda_input.invocation);
        cuda_input.invocation.abi_major        = OCUDU_DAPP_ABI_MAJOR_V1;
        cuda_input.invocation.abi_minor        = OCUDU_DAPP_ABI_MINOR_V1;
        cuda_input.invocation.execution_stream = cuda_publication->execution_stream;
        auto& channel                          = cuda_input.channel_estimates;
        channel.struct_size                    = sizeof(channel);
        channel.abi_major                      = OCUDU_DAPP_ABI_MAJOR_V1;
        channel.abi_minor                      = OCUDU_DAPP_ABI_MINOR_V1;
        channel.domain                         = OCUDU_DAPP_MEM_ACCELERATOR_DEVICE_V1;
        channel.backend_id                     = OCUDU_DAPP_BACKEND_CUDA_V1;
        channel.device_ordinal                 = cuda_publication->device_ordinal;
        channel.element_type                   = decoded.tensor.element_type;
        channel.rank                           = decoded.tensor.rank;
        channel.access                         = OCUDU_DAPP_ACCESS_READ_V1;
        channel.allocation_id                  = decoded.source_sequence;
        channel.allocation_generation          = decoded.source_sequence;
        channel.native_handle                  = reinterpret_cast<uintptr_t>(decoded.tensor.data);
        channel.byte_length                    = decoded.tensor.byte_length;
        std::copy_n(decoded.tensor.dimensions, channel.rank, channel.dimensions);
        std::copy_n(decoded.tensor.byte_strides, channel.rank, channel.byte_strides);
        channel.layout_id      = OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1;
        channel.layout_version = 1;
        channel.lease_token    = decoded.source_sequence;
        if (run_srs_cir_cuda(cuda_input, decoded.noise_variance, cuda.workers[index], analysis) != OCUDU_DAPP_OK_V1) {
          return 5;
        }
#else
        if (!estimate_srs_cir_placeholder(decoded.tensor, decoded.noise_variance, analysis))
          return 5;
#endif
        ocudu_dapp_process_srs_isac_result_v1 result{};
        auto&                                 preview = result.preview;
        if (!make_srs_cir_preview(analysis, decoded.noise_variance, preview))
          return 5;
        preview.context.struct_size     = sizeof(preview.context);
        preview.context.schema_id       = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
        preview.context.schema_version  = 1;
        preview.context.source_sequence = decoded.source_sequence;
        preview.context.cell_id         = decoded.cell_id;
        preview.context.confidence_q16  = 1U << 16U;
        preview.context.created         = decoded.radio_time;
        preview.context.valid_from      = decoded.radio_time;
        preview.context.valid_until     = decoded.radio_time;
        preview.source_srs_sequence     = decoded.source_sequence;
        preview.srs_resource_id         = decoded.srs_resource_id;
        preview.srs_config_id           = decoded.srs_config_id;
        if (!route.send_srs_isac_result(result))
          return 5;
#else
#error "Unsupported Class-C reference interface"
#endif
        if (once)
          return 0;
        break;
      }
    }
  }
  return 0;
}

} // namespace

int main(int argc, char** argv)
{
  std::signal(SIGTERM, request_stop);
  std::signal(SIGINT, request_stop);
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "worker exception: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "worker exception: unknown\n";
  }
  return 6;
}
