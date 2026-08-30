// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/process_client.h"
#include "ocudu/dapp/process/v1/shared_ring.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace ocudu::dapp::sdk {

namespace {

constexpr uint32_t native_audit_arch() noexcept
{
#if defined(__aarch64__)
  return AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
  return AUDIT_ARCH_X86_64;
#else
#error "The reference process seccomp profile needs an audit architecture mapping"
#endif
}

void append_instruction(std::vector<sock_filter>& filter, uint16_t code, uint8_t jump_true, uint8_t jump_false,
                        uint32_t value)
{
  filter.push_back({code, jump_true, jump_false, value});
}

void deny_syscall(std::vector<sock_filter>& filter, int syscall_number)
{
  append_instruction(filter, BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<uint32_t>(syscall_number));
  append_instruction(filter, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ERRNO | static_cast<uint32_t>(EPERM));
}

bool install_restricted_seccomp_filter() noexcept
{
  try {
    std::vector<sock_filter> filter;
    filter.reserve(64);
    append_instruction(filter, BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(seccomp_data, arch));
    append_instruction(filter, BPF_JMP | BPF_JEQ | BPF_K, 1, 0, native_audit_arch());
    append_instruction(filter, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS);
    append_instruction(filter, BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(seccomp_data, nr));
#ifdef __NR_execve
    deny_syscall(filter, __NR_execve);
#endif
#ifdef __NR_execveat
    deny_syscall(filter, __NR_execveat);
#endif
#ifdef __NR_ptrace
    deny_syscall(filter, __NR_ptrace);
#endif
#ifdef __NR_process_vm_readv
    deny_syscall(filter, __NR_process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
    deny_syscall(filter, __NR_process_vm_writev);
#endif
#ifdef __NR_mount
    deny_syscall(filter, __NR_mount);
#endif
#ifdef __NR_umount2
    deny_syscall(filter, __NR_umount2);
#endif
#ifdef __NR_pivot_root
    deny_syscall(filter, __NR_pivot_root);
#endif
#ifdef __NR_setns
    deny_syscall(filter, __NR_setns);
#endif
#ifdef __NR_unshare
    deny_syscall(filter, __NR_unshare);
#endif
#ifdef __NR_bpf
    deny_syscall(filter, __NR_bpf);
#endif
#ifdef __NR_perf_event_open
    deny_syscall(filter, __NR_perf_event_open);
#endif
#ifdef __NR_userfaultfd
    deny_syscall(filter, __NR_userfaultfd);
#endif
#ifdef __NR_keyctl
    deny_syscall(filter, __NR_keyctl);
#endif
#ifdef __NR_add_key
    deny_syscall(filter, __NR_add_key);
#endif
#ifdef __NR_request_key
    deny_syscall(filter, __NR_request_key);
#endif
#ifdef __NR_open_by_handle_at
    deny_syscall(filter, __NR_open_by_handle_at);
#endif
#ifdef __NR_name_to_handle_at
    deny_syscall(filter, __NR_name_to_handle_at);
#endif
#ifdef __NR_init_module
    deny_syscall(filter, __NR_init_module);
#endif
#ifdef __NR_finit_module
    deny_syscall(filter, __NR_finit_module);
#endif
#ifdef __NR_delete_module
    deny_syscall(filter, __NR_delete_module);
#endif
#ifdef __NR_kexec_load
    deny_syscall(filter, __NR_kexec_load);
#endif
#ifdef __NR_reboot
    deny_syscall(filter, __NR_reboot);
#endif
    append_instruction(filter, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW);
    sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
    return ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program) == 0;
  } catch (...) {
    return false;
  }
}

bool valid_route_flags(uint32_t interface_id, uint32_t flags) noexcept
{
  return flags == 0 ||
         (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 && flags == OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1);
}

bool valid_route_transport(const ocudu_dapp_route_offer_v1& offer) noexcept
{
  if (offer.reserved0 != 0 || offer.reserved != 0)
    return false;
  if (offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CPU_SHARED_RING_V1)
    return offer.backend_id == 0 && offer.export_slot_count == 0 && offer.export_slot_capacity == 0;
  return offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1 &&
         (offer.interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 ||
          offer.interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) &&
         offer.flags == 0 && offer.backend_id == OCUDU_DAPP_BACKEND_CUDA_V1 && offer.export_slot_count >= 2 &&
         offer.export_slot_count <= OCUDU_DAPP_CUDA_EXPORT_MAX_SLOTS_V1 && offer.export_slot_capacity != 0;
}

class unique_fd
{
public:
  explicit unique_fd(int value_ = -1) noexcept : value_(value_) {}
  unique_fd(unique_fd&& other) noexcept : value_(other.release()) {}
  unique_fd& operator=(unique_fd&& other) noexcept
  {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  ~unique_fd() { reset(); }
  unique_fd(const unique_fd&)            = delete;
  unique_fd& operator=(const unique_fd&) = delete;
  int        get() const noexcept { return value_; }
  int        release() noexcept
  {
    const int result = value_;
    value_           = -1;
    return result;
  }
  void reset(int value = -1) noexcept
  {
    if (value_ >= 0)
      ::close(value_);
    value_ = value;
  }
  explicit operator bool() const noexcept { return value_ >= 0; }

private:
  int value_;
};

std::string error_text(const char* operation)
{
  return std::string(operation) + ": " + std::strerror(errno);
}

uint64_t load_acquire(const uint64_t& value) noexcept
{
  return __atomic_load_n(&value, __ATOMIC_ACQUIRE);
}

constexpr uint64_t align64(uint64_t value) noexcept
{
  return (value + OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 - 1U) & ~uint64_t{OCUDU_DAPP_SHARED_RING_ALIGNMENT_V1 - 1U};
}

bool valid_radio_time(const ocudu_dapp_radio_time_v1& time) noexcept
{
  return time.numerology <= 4 && time.sfn < 1024 && time.slot < (10U << time.numerology) && time.reserved == 0;
}

bool all_zero(const uint64_t* values, std::size_t count) noexcept
{
  return std::all_of(values, values + count, [](uint64_t value) { return value == 0; });
}

template <std::size_t N>
bool terminated(const char (&value)[N]) noexcept
{
  return std::find(std::begin(value), std::end(value), '\0') != std::end(value);
}

template <std::size_t N>
bool copy_fixed(char (&destination)[N], const char* source) noexcept
{
  if (source == nullptr)
    return false;
  const std::size_t size = ::strnlen(source, N);
  if (size == N)
    return false;
  std::memcpy(destination, source, size);
  destination[size] = '\0';
  return true;
}

bool decode_value(ocudu_dapp_value_v1& destination, const ocudu_dapp_process_value_v1& source) noexcept
{
  destination      = {};
  destination.kind = source.kind;
  if (source.size > OCUDU_DAPP_PROCESS_MANAGEMENT_VALUE_BYTES_V1 || !all_zero(source.reserved, 2))
    return false;
  switch (source.kind) {
    case OCUDU_DAPP_VALUE_BOOL_V1:
      destination.value.boolean = source.value.boolean;
      return source.size == 0 && source.value.boolean <= 1;
    case OCUDU_DAPP_VALUE_INT64_V1:
      destination.value.int64_value = source.value.int64_value;
      return source.size == 0;
    case OCUDU_DAPP_VALUE_UINT64_V1:
      destination.value.uint64_value = source.value.uint64_value;
      return source.size == 0;
    case OCUDU_DAPP_VALUE_DOUBLE_V1:
      destination.value.double_value = source.value.double_value;
      return source.size == 0;
    case OCUDU_DAPP_VALUE_STRING_V1:
    case OCUDU_DAPP_VALUE_BYTES_V1:
      destination.value.blob = {source.value.bytes, source.size, 0};
      return true;
    default:
      return false;
  }
}

bool encode_value(ocudu_dapp_process_value_v1& destination, const ocudu_dapp_value_v1& source) noexcept
{
  destination      = {};
  destination.kind = source.kind;
  switch (source.kind) {
    case 0:
      return true;
    case OCUDU_DAPP_VALUE_BOOL_V1:
      destination.value.boolean = source.value.boolean;
      return source.value.boolean <= 1;
    case OCUDU_DAPP_VALUE_INT64_V1:
      destination.value.int64_value = source.value.int64_value;
      return true;
    case OCUDU_DAPP_VALUE_UINT64_V1:
      destination.value.uint64_value = source.value.uint64_value;
      return true;
    case OCUDU_DAPP_VALUE_DOUBLE_V1:
      destination.value.double_value = source.value.double_value;
      return true;
    case OCUDU_DAPP_VALUE_STRING_V1:
    case OCUDU_DAPP_VALUE_BYTES_V1:
      if (source.value.blob.size > OCUDU_DAPP_PROCESS_MANAGEMENT_VALUE_BYTES_V1 ||
          (source.value.blob.size != 0 && source.value.blob.data == nullptr))
        return false;
      destination.size = source.value.blob.size;
      if (destination.size != 0)
        std::memcpy(destination.value.bytes, source.value.blob.data, destination.size);
      return true;
    default:
      return false;
  }
}

bool same_radio_time(const ocudu_dapp_radio_time_v1& lhs, const ocudu_dapp_radio_time_v1& rhs) noexcept
{
  return lhs.sfn == rhs.sfn && lhs.slot == rhs.slot && lhs.numerology == rhs.numerology && lhs.reserved == rhs.reserved;
}

bool valid_srs_preview(const ocudu_dapp_srs_isac_cir_preview_v1& preview) noexcept
{
  const auto& context = preview.context;
  if (preview.source_srs_sequence == 0 || context.source_sequence != preview.source_srs_sequence ||
      context.schema_id != OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 || context.schema_version != 1 || context.flags != 0 ||
      context.producer_instance_id[0] != 0 || context.producer_instance_id[1] != 0 ||
      context.producer_generation != 0 || context.confidence_q16 > (1U << 16U) || !valid_radio_time(context.created) ||
      !same_radio_time(context.created, context.valid_from) || !same_radio_time(context.created, context.valid_until) ||
      context.provenance_hash != 0 || !all_zero(context.reserved, std::size(context.reserved)) ||
      preview.tap_count == 0 || preview.tap_count > OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1 ||
      preview.peak_delay_bin >= preview.tap_count || !std::isfinite(preview.mean_channel_power) ||
      preview.mean_channel_power < 0.0F || !std::isfinite(preview.noise_corrected_power) ||
      preview.noise_corrected_power < 0.0F || !std::isfinite(preview.peak_cir_power) || preview.peak_cir_power < 0.0F ||
      !std::isfinite(preview.noise_variance) || preview.noise_variance < 0.0F ||
      !all_zero(preview.reserved, std::size(preview.reserved))) {
    return false;
  }
  for (uint32_t tap = 0; tap != preview.tap_count; ++tap) {
    if (!std::isfinite(preview.tap_power[tap]) || preview.tap_power[tap] < 0.0F ||
        preview.tap_power[tap] > preview.peak_cir_power) {
      return false;
    }
  }
  return preview.tap_power[preview.peak_delay_bin] == preview.peak_cir_power;
}

} // namespace

struct route_mapping {
  unique_fd                               control;
  unique_fd                               ring;
  unique_fd                               notification;
  unique_fd                               completion;
  unique_fd                               accelerator_export;
  void*                                   address{MAP_FAILED};
  std::size_t                             mapped_size{};
  ocudu_dapp_shared_ring_header_v1*       header{};
  void*                                   export_address{MAP_FAILED};
  std::size_t                             export_size{};
  const ocudu_dapp_cuda_export_header_v1* export_header{};
  const ocudu_dapp_cuda_export_slot_v1*   export_slots{};
  ocudu_dapp_route_offer_v1               route_offer{};
  uint64_t                                route_generation{};
  uint64_t                                consumer_read_index{};
  pid_t                                   expected_pid{-1};
  uid_t                                   expected_uid{};
  std::atomic<bool>                       lease_active{false};

  ~route_mapping()
  {
    if (address != MAP_FAILED)
      ::munmap(address, mapped_size);
    if (export_address != MAP_FAILED)
      ::munmap(export_address, export_size);
  }

  ocudu_dapp_shared_ring_slot_v1* slot(uint64_t index) const noexcept
  {
    auto* base = static_cast<uint8_t*>(address) + header->header_size;
    return reinterpret_cast<ocudu_dapp_shared_ring_slot_v1*>(base + (index % header->slots) * header->slot_stride);
  }
};

struct route_set_mapping {
  route_set_mapping(std::vector<std::unique_ptr<process_route>>          routes_,
                    std::vector<ocudu_dapp_process_interface_binding_v1> interfaces_,
                    process_management_binding                           management_) :
    routes(std::move(routes_)), interfaces(std::move(interfaces_)), management(management_)
  {
    descriptors.reserve(routes.size() + 1);
    descriptors.push_back({routes.front()->mapping->control.get(), POLLIN, 0});
    for (const auto& route : routes)
      descriptors.push_back({route->mapping->notification.get(), POLLIN, 0});
  }

  std::vector<std::unique_ptr<process_route>>          routes;
  std::vector<ocudu_dapp_process_interface_binding_v1> interfaces;
  std::vector<pollfd>                                  descriptors;
  bool                                                 control_ok{true};
  process_management_binding                           management;
  bool                                                 service_control() noexcept;
};

namespace {

route_receive_result reject(std::string detail)
{
  return {nullptr, std::move(detail)};
}

route_set_receive_result reject_set(std::string detail)
{
  return {nullptr, std::move(detail)};
}

bool validate_ring(route_mapping& mapping, std::string& detail)
{
  static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
                "Class-C process client requires lock-free 64-bit atomics");
  struct stat status {
  };
  if (::fstat(mapping.ring.get(), &status) != 0 ||
      status.st_size < static_cast<off_t>(OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1) ||
      static_cast<uint64_t>(status.st_size) > OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1) {
    detail = "invalid Class-C ring file size";
    return false;
  }
  if ((::fcntl(mapping.ring.get(), F_GETFL) & O_ACCMODE) != O_RDONLY) {
    detail = "Class-C ring data capability is not read-only";
    return false;
  }
  mapping.mapped_size = status.st_size;
  mapping.address     = ::mmap(nullptr, mapping.mapped_size, PROT_READ, MAP_SHARED, mapping.ring.get(), 0);
  if (mapping.address == MAP_FAILED) {
    detail = error_text("map Class-C ring");
    return false;
  }
  mapping.header     = static_cast<ocudu_dapp_shared_ring_header_v1*>(mapping.address);
  const auto& header = *mapping.header;
  if (header.magic != OCUDU_DAPP_SHARED_RING_MAGIC_V1 || header.version != OCUDU_DAPP_SHARED_RING_VERSION_V1 ||
      header.header_size != OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1 || header.slots < 2 ||
      header.payload_capacity == 0 ||
      header.slot_stride != align64(sizeof(ocudu_dapp_shared_ring_slot_v1) + header.payload_capacity) ||
      header.slot_stride > OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 ||
      header.slots > (OCUDU_DAPP_SHARED_RING_MAX_BYTES_V1 - header.header_size) / header.slot_stride) {
    detail = "invalid Class-C ring header";
    return false;
  }
  const uint64_t expected = header.header_size + static_cast<uint64_t>(header.slots) * header.slot_stride;
  if (expected != header.mapped_bytes || expected != mapping.mapped_size) {
    detail = "inconsistent Class-C ring extent";
    return false;
  }
  mapping.consumer_read_index = load_acquire(header.read_index);
  return true;
}

bool validate_cuda_export(route_mapping& mapping, std::string& detail)
{
  if (!mapping.accelerator_export) {
    detail = "missing CUDA export descriptor";
    return false;
  }
  struct stat status {
  };
  if (::fstat(mapping.accelerator_export.get(), &status) != 0 ||
      status.st_size < static_cast<off_t>(sizeof(ocudu_dapp_cuda_export_header_v1))) {
    detail = "invalid CUDA export descriptor size";
    return false;
  }
  const auto&    offer = mapping.route_offer;
  const uint64_t bytes = sizeof(ocudu_dapp_cuda_export_header_v1) +
                         static_cast<uint64_t>(offer.export_slot_count) * sizeof(ocudu_dapp_cuda_export_slot_v1);
  const int seals = ::fcntl(mapping.accelerator_export.get(), F_GET_SEALS);
  if (bytes != static_cast<uint64_t>(status.st_size) ||
      (::fcntl(mapping.accelerator_export.get(), F_GETFL) & O_ACCMODE) != O_RDONLY || seals < 0 ||
      (seals & (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL)) !=
          (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL)) {
    detail = "CUDA export descriptor is not exact, sealed, and read-only";
    return false;
  }
  mapping.export_size = status.st_size;
  mapping.export_address =
      ::mmap(nullptr, mapping.export_size, PROT_READ, MAP_SHARED, mapping.accelerator_export.get(), 0);
  if (mapping.export_address == MAP_FAILED) {
    detail = error_text("map CUDA export descriptor");
    return false;
  }
  mapping.export_header = static_cast<const ocudu_dapp_cuda_export_header_v1*>(mapping.export_address);
  const auto& header    = *mapping.export_header;
  if (header.magic != OCUDU_DAPP_CUDA_EXPORT_MAGIC_V1 || header.version != OCUDU_DAPP_CUDA_EXPORT_VERSION_V1 ||
      header.header_size != sizeof(header) || header.backend_id != OCUDU_DAPP_BACKEND_CUDA_V1 ||
      header.device_ordinal > UINT16_MAX || header.slot_count != offer.export_slot_count ||
      header.slot_stride != sizeof(ocudu_dapp_cuda_export_slot_v1) ||
      header.slot_capacity != offer.export_slot_capacity || !all_zero(header.reserved, std::size(header.reserved))) {
    detail = "CUDA export descriptor header rejected";
    return false;
  }
  mapping.export_slots = reinterpret_cast<const ocudu_dapp_cuda_export_slot_v1*>(
      static_cast<const uint8_t*>(mapping.export_address) + header.header_size);
  for (uint32_t index = 0; index != header.slot_count; ++index) {
    const auto& slot                  = mapping.export_slots[index];
    const bool  memory_handle_present = std::any_of(std::begin(slot.memory_handle), std::end(slot.memory_handle),
                                                    [](uint8_t value) { return value != 0; });
    const bool  event_handle_present =
        std::any_of(std::begin(slot.ready_event_handle), std::end(slot.ready_event_handle),
                    [](uint8_t value) { return value != 0; });
    if (slot.struct_size != sizeof(slot) || slot.slot_id != index || slot.allocation_id == 0 ||
        slot.capacity != header.slot_capacity || !memory_handle_present || !event_handle_present ||
        !all_zero(slot.reserved, std::size(slot.reserved))) {
      detail = "CUDA export slot descriptor rejected";
      return false;
    }
  }
  return true;
}

bool wait_until_readable(int descriptor, std::chrono::steady_clock::time_point deadline) noexcept
{
  pollfd item{descriptor, POLLIN, 0};
  for (;;) {
    const auto now       = std::chrono::steady_clock::now();
    const auto remaining = now < deadline
                               ? std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
                                          std::chrono::milliseconds(1))
                               : std::chrono::milliseconds::zero();
    const int  result    = ::poll(&item, 1, static_cast<int>(remaining.count()));
    if (result >= 0)
      return result == 1 && (item.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    if (errno != EINTR)
      return false;
  }
}

bool send_route_ready(int control_fd, const ocudu_dapp_route_offer_v1& offer, uint64_t generation,
                      uint32_t capabilities) noexcept
{
  ocudu_dapp_route_ready_v1 ready{};
  ready.struct_size          = sizeof(ready);
  ready.interface_id         = offer.interface_id;
  ready.route_generation     = generation;
  ready.producer_shard_count = offer.producer_shard_count;
  ready.flags                = capabilities;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_ROUTE_READY_V1;
  header.payload_size = sizeof(ready);
  header.sequence     = generation;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&ready, sizeof(ready)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(ready));
}

bool send_route_bundle_ready(int control_fd, const ocudu_dapp_route_bundle_v1& bundle, uint32_t capabilities) noexcept
{
  ocudu_dapp_route_bundle_ready_v1 ready{};
  ready.struct_size      = sizeof(ready);
  ready.interface_count  = bundle.interface_count;
  ready.route_count      = bundle.route_count;
  ready.flags            = capabilities;
  ready.route_generation = bundle.route_generation;
  ready.bundle_id        = bundle.bundle_id;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_READY_V1;
  header.payload_size = sizeof(ready);
  header.sequence     = bundle.route_generation;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&ready, sizeof(ready)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(control_fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(ready));
}

bool valid_management_request(const ocudu_dapp_process_management_request_v1& request, std::size_t descriptors) noexcept
{
  const bool model = request.operation == OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1;
  if (request.struct_size != sizeof(request) || request.route_generation == 0 || request.request_id == 0 ||
      request.operation < OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIBE_CONFIG_V1 ||
      request.operation > OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1 ||
      request.entry_count > OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1 ||
      (request.flags & ~OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1) != 0 ||
      descriptors != (model && request.artifact_size != 0 ? 1U : 0U) || request.artifact_size > 1024 * 1024 ||
      !all_zero(request.reserved, std::size(request.reserved)))
    return false;
  if (model)
    return request.entry_count == 0 && request.item_offset == 0 && request.expected_generation != 0 &&
           request.boundary_or_action >= OCUDU_DAPP_MODEL_STAGE_V1 &&
           request.boundary_or_action <= OCUDU_DAPP_MODEL_RETIRE_V1 && terminated(request.model_id) &&
           request.model_id[0] != '\0' && terminated(request.model_version);
  if (request.artifact_size != 0 || request.model_id[0] != '\0' || request.model_version[0] != '\0')
    return false;
  if (request.operation != OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1)
    return request.entry_count == 0 && request.expected_generation == 0 && request.boundary_or_action == 0;
  if (request.item_offset != 0 || request.entry_count == 0 || request.expected_generation == 0 ||
      request.boundary_or_action < OCUDU_DAPP_CONFIG_BOUNDARY_IMMEDIATE_V1 ||
      request.boundary_or_action > OCUDU_DAPP_CONFIG_BOUNDARY_RESTART_V1)
    return false;
  for (uint32_t index = 0; index != request.entry_count; ++index) {
    ocudu_dapp_value_v1 ignored{};
    if (!terminated(request.entries[index].key) || request.entries[index].key[0] == '\0' ||
        !decode_value(ignored, request.entries[index].value))
      return false;
  }
  return true;
}

class mapped_model
{
public:
  ~mapped_model()
  {
    if (address != nullptr)
      ::munmap(address, size);
  }
  void*       address{};
  std::size_t size{};
};

bool model_artifact_view(int descriptor, uint64_t expected_size, mapped_model& mapping) noexcept
{
  if (expected_size == 0)
    return descriptor < 0;
  struct stat status {
  };
  const int seals = descriptor >= 0 ? ::fcntl(descriptor, F_GET_SEALS) : -1;
  if (descriptor < 0 || (::fcntl(descriptor, F_GETFL) & O_ACCMODE) != O_RDONLY || ::fstat(descriptor, &status) != 0 ||
      status.st_size != static_cast<off_t>(expected_size) ||
      (seals & (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) !=
          (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
    return false;
  mapping.address = ::mmap(nullptr, expected_size, PROT_READ, MAP_SHARED, descriptor, 0);
  if (mapping.address == MAP_FAILED) {
    mapping.address = nullptr;
    return false;
  }
  mapping.size = expected_size;
  return true;
}

bool execute_management(process_management_binding binding, const ocudu_dapp_process_management_request_v1& request,
                        int artifact_fd, ocudu_dapp_process_management_response_v1& response) noexcept
{
  response                  = {};
  response.struct_size      = sizeof(response);
  response.operation        = request.operation;
  response.route_generation = request.route_generation;
  response.request_id       = request.request_id;
  response.status           = OCUDU_DAPP_UNSUPPORTED_V1;
  if (binding.api == nullptr || binding.instance == nullptr)
    return true;
  try {
    uint32_t count = 0;
    switch (request.operation) {
      case OCUDU_DAPP_PROCESS_MANAGEMENT_DESCRIBE_CONFIG_V1: {
        if ((binding.api->capability_bits & OCUDU_DAPP_MANAGEMENT_CONFIG_V1) == 0 ||
            binding.api->describe_config == nullptr || request.entry_count != 0 || request.expected_generation != 0 ||
            request.boundary_or_action != 0 || artifact_fd >= 0)
          break;
        auto status = binding.api->describe_config(binding.instance, nullptr, 0, &count);
        if (status != OCUDU_DAPP_OK_V1 || count > OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1) {
          response.status = status == OCUDU_DAPP_OK_V1 ? OCUDU_DAPP_RESOURCE_V1 : status;
          response.count  = 0;
          break;
        }
        std::array<ocudu_dapp_config_descriptor_v1, OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1> values{};
        status          = binding.api->describe_config(binding.instance, values.data(), values.size(), &count);
        response.status = status;
        response.count  = count;
        if (count > values.size()) {
          response.status = OCUDU_DAPP_RESOURCE_V1;
          response.count  = 0;
          break;
        }
        if (status != OCUDU_DAPP_OK_V1 || request.item_offset >= count)
          break;
        const auto& source = values[request.item_offset];
        auto&       output = response.payload.descriptor;
        if (source.struct_size < sizeof(source) || !copy_fixed(output.key, source.key) ||
            !copy_fixed(output.description, source.description != nullptr ? source.description : "") ||
            !encode_value(output.default_value, source.default_value) ||
            !encode_value(output.minimum_value, source.minimum_value) ||
            !encode_value(output.maximum_value, source.maximum_value)) {
          response.status = OCUDU_DAPP_INVALID_OUTPUT_V1;
          break;
        }
        output.value_kind = source.value_kind;
        output.flags      = source.flags;
        response.flags    = request.item_offset + 1 < count ? OCUDU_DAPP_PROCESS_MANAGEMENT_MORE_V1 : 0;
        break;
      }
      case OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1: {
        if ((binding.api->capability_bits & OCUDU_DAPP_MANAGEMENT_CONFIG_V1) == 0 ||
            binding.api->get_config == nullptr || request.entry_count != 0 || request.expected_generation != 0 ||
            request.boundary_or_action != 0 || request.item_offset != 0 || artifact_fd >= 0)
          break;
        std::array<ocudu_dapp_config_entry_v1, OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1> values{};
        response.status = binding.api->get_config(binding.instance, values.data(), values.size(), &count,
                                                  &response.resulting_generation);
        response.count  = count;
        if (count > values.size()) {
          response.status = OCUDU_DAPP_RESOURCE_V1;
          response.count  = 0;
          break;
        }
        if (response.status != OCUDU_DAPP_OK_V1)
          break;
        for (uint32_t index = 0; index != count; ++index) {
          if (!copy_fixed(response.payload.entries[index].key, values[index].key) ||
              !encode_value(response.payload.entries[index].value, values[index].value)) {
            response.status = OCUDU_DAPP_INVALID_OUTPUT_V1;
            break;
          }
        }
        break;
      }
      case OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1: {
        if ((binding.api->capability_bits & OCUDU_DAPP_MANAGEMENT_CONFIG_V1) == 0 ||
            binding.api->set_config == nullptr || request.entry_count == 0 || request.item_offset != 0 ||
            request.expected_generation == 0 || artifact_fd >= 0)
          break;
        std::array<ocudu_dapp_config_entry_v1, OCUDU_DAPP_PROCESS_MANAGEMENT_MAX_ENTRIES_V1> entries{};
        for (uint32_t index = 0; index != request.entry_count; ++index) {
          if (!terminated(request.entries[index].key) || request.entries[index].key[0] == '\0' ||
              !decode_value(entries[index].value, request.entries[index].value)) {
            response.status = OCUDU_DAPP_INVALID_INPUT_V1;
            return true;
          }
          entries[index].key = request.entries[index].key;
        }
        ocudu_dapp_config_update_v1 update{};
        update.struct_size         = sizeof(update);
        update.nof_entries         = request.entry_count;
        update.entries             = entries.data();
        update.expected_generation = request.expected_generation;
        update.boundary            = request.boundary_or_action;
        update.dry_run             = (request.flags & OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1) != 0;
        response.status            = binding.api->set_config(binding.instance, &update, &response.resulting_generation);
        break;
      }
      case OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1: {
        if ((binding.api->capability_bits & OCUDU_DAPP_MANAGEMENT_MODELS_V1) == 0 ||
            binding.api->manage_model == nullptr || request.entry_count != 0 || request.item_offset != 0 ||
            request.expected_generation == 0 || !terminated(request.model_id) || request.model_id[0] == '\0' ||
            !terminated(request.model_version))
          break;
        mapped_model artifact;
        if (!model_artifact_view(artifact_fd, request.artifact_size, artifact)) {
          response.status = OCUDU_DAPP_INVALID_INPUT_V1;
          break;
        }
        ocudu_dapp_model_request_v1 model{};
        model.struct_size         = sizeof(model);
        model.action              = request.boundary_or_action;
        model.model_id            = request.model_id;
        model.model_version       = request.model_version;
        model.artifact            = {artifact.address, static_cast<uint32_t>(artifact.size), 0};
        model.expected_generation = request.expected_generation;
        model.dry_run             = (request.flags & OCUDU_DAPP_PROCESS_MANAGEMENT_DRY_RUN_V1) != 0;
        response.status           = binding.api->manage_model(binding.instance, &model, &response.resulting_generation);
        break;
      }
      default:
        response.status = OCUDU_DAPP_INVALID_INPUT_V1;
        break;
    }
  } catch (const std::bad_alloc&) {
    response.status = OCUDU_DAPP_RESOURCE_V1;
  } catch (...) {
    response.status = OCUDU_DAPP_EXCEPTION_V1;
  }
  return true;
}

} // namespace

bool bind_parent_lifetime(pid_t expected_parent_pid) noexcept
{
  if (expected_parent_pid <= 1 || ::getppid() != expected_parent_pid ||
      ::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0) {
    return false;
  }
  return ::getppid() == expected_parent_pid;
}

bool apply_restricted_process_policy() noexcept
{
  return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && install_restricted_seccomp_filter();
}

bool route_set_mapping::service_control() noexcept
{
  auto& reference = *routes.front()->mapping;
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + OCUDU_DAPP_CONTROL_MAX_PAYLOAD_V1> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(ucred)) + CMSG_SPACE(sizeof(int))>     control{};
  iovec  vector{data.data(), data.size()};
  msghdr message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(reference.control.get(), &message, MSG_CMSG_CLOEXEC);
  if (received < static_cast<ssize_t>(sizeof(ocudu_dapp_control_header_v1)) ||
      (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
    return false;

  ucred                  credentials{};
  bool                   have_credentials = false;
  std::vector<unique_fd> descriptors;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level != SOL_SOCKET)
      continue;
    if (item->cmsg_type == SCM_CREDENTIALS && item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    } else if (item->cmsg_type == SCM_RIGHTS && item->cmsg_len == CMSG_LEN(sizeof(int))) {
      int descriptor;
      std::memcpy(&descriptor, CMSG_DATA(item), sizeof(descriptor));
      descriptors.emplace_back(descriptor);
    }
  }
  ocudu_dapp_control_header_v1 header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (!have_credentials || credentials.pid != reference.expected_pid || credentials.uid != reference.expected_uid ||
      header.magic != OCUDU_DAPP_CONTROL_MAGIC_V1 || header.version != OCUDU_DAPP_CONTROL_VERSION_V1 ||
      header.reserved != 0 || header.payload_size > OCUDU_DAPP_CONTROL_MAX_PAYLOAD_V1 ||
      header.descriptor_count != descriptors.size() ||
      received != static_cast<ssize_t>(sizeof(header) + header.payload_size) ||
      header.sequence != reference.route_generation)
    return false;
  if (header.type == OCUDU_DAPP_CONTROL_HEARTBEAT_REQUEST_V1) {
    ocudu_dapp_process_heartbeat_v1 heartbeat{};
    if (!descriptors.empty() || header.payload_size != sizeof(heartbeat))
      return false;
    std::memcpy(&heartbeat, data.data() + sizeof(header), sizeof(heartbeat));
    if (heartbeat.struct_size != sizeof(heartbeat) || heartbeat.flags != 0 ||
        heartbeat.route_generation != reference.route_generation || heartbeat.challenge == 0 ||
        heartbeat.host_monotonic_ns == 0 || !all_zero(heartbeat.reserved, std::size(heartbeat.reserved)))
      return false;
    header.type = OCUDU_DAPP_CONTROL_HEARTBEAT_RESPONSE_V1;
    std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&heartbeat, sizeof(heartbeat)}}};
    message            = {};
    message.msg_iov    = vectors.data();
    message.msg_iovlen = vectors.size();
    return ::sendmsg(reference.control.get(), &message, MSG_NOSIGNAL) ==
           static_cast<ssize_t>(sizeof(header) + sizeof(heartbeat));
  }
  if (header.type != OCUDU_DAPP_CONTROL_MANAGEMENT_REQUEST_V1 ||
      header.payload_size != sizeof(ocudu_dapp_process_management_request_v1))
    return false;
  ocudu_dapp_process_management_request_v1 request{};
  std::memcpy(&request, data.data() + sizeof(header), sizeof(request));
  if (request.route_generation != reference.route_generation || !valid_management_request(request, descriptors.size()))
    return false;
  ocudu_dapp_process_management_response_v1 response{};
  if (!execute_management(management, request, descriptors.empty() ? -1 : descriptors.front().get(), response))
    return false;
  header.type             = OCUDU_DAPP_CONTROL_MANAGEMENT_RESPONSE_V1;
  header.payload_size     = sizeof(response);
  header.descriptor_count = 0;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&response, sizeof(response)}}};
  message            = {};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(reference.control.get(), &message, MSG_NOSIGNAL) ==
         static_cast<ssize_t>(sizeof(header) + sizeof(response));
}

namespace {

struct route_bundle_receive_result {
  std::optional<ocudu_dapp_route_bundle_v1> bundle;
  std::string                               detail;
  explicit                                  operator bool() const noexcept { return bundle.has_value(); }
};

route_bundle_receive_result receive_route_bundle_frame(int control_fd, pid_t expected_pid, uid_t expected_uid)
{
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + sizeof(ocudu_dapp_route_bundle_v1)> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(ucred))>                                control{};
  iovec  vector{data.data(), data.size()};
  msghdr message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(control_fd, &message, MSG_CMSG_CLOEXEC);
  if (received <= 0)
    return {{}, received == 0 ? "control peer closed" : error_text("receive route bundle")};

  ucred credentials{};
  bool  have_credentials = false;
  bool  have_descriptors = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level != SOL_SOCKET)
      continue;
    if (item->cmsg_type == SCM_CREDENTIALS && item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    } else if (item->cmsg_type == SCM_RIGHTS) {
      have_descriptors = true;
    }
  }
  if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || received != static_cast<ssize_t>(data.size()) ||
      !have_credentials || have_descriptors || credentials.pid != expected_pid || credentials.uid != expected_uid) {
    return {{}, "truncated, unauthenticated, or descriptor-bearing route bundle"};
  }
  ocudu_dapp_control_header_v1 header{};
  ocudu_dapp_route_bundle_v1   bundle{};
  std::memcpy(&header, data.data(), sizeof(header));
  std::memcpy(&bundle, data.data() + sizeof(header), sizeof(bundle));
  if (header.magic != OCUDU_DAPP_CONTROL_MAGIC_V1 || header.version != OCUDU_DAPP_CONTROL_VERSION_V1 ||
      header.type != OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_V1 || header.payload_size != sizeof(bundle) ||
      header.descriptor_count != 0 || header.reserved != 0 || header.sequence == 0 ||
      bundle.struct_size != sizeof(bundle) || bundle.interface_count < 2 ||
      bundle.interface_count > OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1 || bundle.route_count == 0 ||
      bundle.route_count > OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1 || bundle.flags != 0 || bundle.reserved0 != 0 ||
      bundle.route_generation != header.sequence || bundle.bundle_id == 0 ||
      !all_zero(bundle.reserved, std::size(bundle.reserved))) {
    return {{}, "invalid route bundle frame"};
  }
  return {bundle, {}};
}

} // namespace

route_receive_result receive_route(int control_fd, pid_t expected_pid, uid_t expected_uid)
{
  if (control_fd < 0 || expected_pid <= 0)
    return reject("invalid control endpoint or expected peer");
  std::vector<unique_fd> descriptors;
  descriptors.reserve(OCUDU_DAPP_CONTROL_MAX_DESCRIPTORS_V1);
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + OCUDU_DAPP_CONTROL_MAX_PAYLOAD_V1> data{};
  alignas(cmsghdr)
      std::array<uint8_t, CMSG_SPACE(OCUDU_DAPP_CONTROL_MAX_DESCRIPTORS_V1 * sizeof(int)) + CMSG_SPACE(sizeof(ucred))>
          control{};
  iovec   vector{data.data(), data.size()};
  msghdr  message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(control_fd, &message, MSG_CMSG_CLOEXEC);
  if (received <= 0)
    return reject(received == 0 ? "control peer closed" : error_text("receive route offer"));

  ucred credentials{};
  bool  have_credentials = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level != SOL_SOCKET)
      continue;
    if (item->cmsg_type == SCM_CREDENTIALS && item->cmsg_len == CMSG_LEN(sizeof(ucred))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    } else if (item->cmsg_type == SCM_RIGHTS && item->cmsg_len >= CMSG_LEN(sizeof(int))) {
      const std::size_t count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      if (count > OCUDU_DAPP_CONTROL_MAX_DESCRIPTORS_V1)
        return reject("too many route descriptors");
      const auto* values = reinterpret_cast<const int*>(CMSG_DATA(item));
      for (std::size_t i = 0; i != count; ++i)
        descriptors.emplace_back(values[i]);
    }
  }
  if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
      received < static_cast<ssize_t>(sizeof(ocudu_dapp_control_header_v1)) || !have_credentials) {
    return reject("truncated or unauthenticated route offer");
  }
  ocudu_dapp_control_header_v1 header{};
  std::memcpy(&header, data.data(), sizeof(header));
  if (header.magic != OCUDU_DAPP_CONTROL_MAGIC_V1 || header.version != OCUDU_DAPP_CONTROL_VERSION_V1 ||
      header.type != OCUDU_DAPP_CONTROL_ROUTE_OFFER_V1 || header.reserved != 0 || header.sequence == 0 ||
      header.payload_size != sizeof(ocudu_dapp_route_offer_v1) ||
      (header.descriptor_count != OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1 &&
       header.descriptor_count != OCUDU_DAPP_ROUTE_DESCRIPTOR_CUDA_COUNT_V1) ||
      descriptors.size() != header.descriptor_count ||
      received != static_cast<ssize_t>(sizeof(header) + header.payload_size)) {
    return reject("invalid route offer framing");
  }
  if (credentials.pid != expected_pid || credentials.uid != expected_uid)
    return reject("route peer credentials rejected");

  auto mapping = std::make_shared<route_mapping>();
  std::memcpy(&mapping->route_offer, data.data() + sizeof(header), sizeof(mapping->route_offer));
  if (mapping->route_offer.struct_size != sizeof(mapping->route_offer) || mapping->route_offer.interface_id == 0 ||
      mapping->route_offer.metadata_schema_id == 0 || mapping->route_offer.metadata_schema_version == 0 ||
      mapping->route_offer.route_id == 0 || mapping->route_offer.producer_shard_count == 0 ||
      mapping->route_offer.producer_shard_count > OCUDU_DAPP_MAX_PRODUCER_SHARDS_V1 ||
      mapping->route_offer.producer_shard_id >= mapping->route_offer.producer_shard_count ||
      !valid_route_transport(mapping->route_offer)) {
    return reject("invalid route offer payload");
  }
  const uint16_t expected_descriptors =
      mapping->route_offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1
          ? OCUDU_DAPP_ROUTE_DESCRIPTOR_CUDA_COUNT_V1
          : OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1;
  if (header.descriptor_count != expected_descriptors)
    return reject("route transport descriptor count mismatch");
  mapping->control          = unique_fd(::fcntl(control_fd, F_DUPFD_CLOEXEC, 0));
  mapping->route_generation = header.sequence;
  mapping->expected_pid     = expected_pid;
  mapping->expected_uid     = expected_uid;
  mapping->ring             = std::move(descriptors[OCUDU_DAPP_ROUTE_DESCRIPTOR_DATA_V1]);
  mapping->notification     = std::move(descriptors[OCUDU_DAPP_ROUTE_DESCRIPTOR_NOTIFICATION_V1]);
  mapping->completion       = std::move(descriptors[OCUDU_DAPP_ROUTE_DESCRIPTOR_COMPLETION_V1]);
  if (expected_descriptors == OCUDU_DAPP_ROUTE_DESCRIPTOR_CUDA_COUNT_V1)
    mapping->accelerator_export = std::move(descriptors[OCUDU_DAPP_ROUTE_DESCRIPTOR_ACCELERATOR_EXPORT_V1]);
  std::string detail;
  if (!mapping->control)
    return reject(error_text("duplicate route control endpoint"));
  if (!validate_ring(*mapping, detail))
    return reject(std::move(detail));
  if (mapping->route_offer.transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1 &&
      !validate_cuda_export(*mapping, detail))
    return reject(std::move(detail));
  return {std::make_unique<process_route>(std::move(mapping)), {}};
}

route_set_receive_result receive_route_set(int control_fd, pid_t expected_pid, uid_t expected_uid,
                                           uint32_t expected_interface_id, std::chrono::milliseconds timeout,
                                           process_management_binding management)
{
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return reject_set("unable to apply no_new_privs");
  const auto bounded = std::chrono::milliseconds(
      std::min<int64_t>(std::max<int64_t>(timeout.count(), 0), std::numeric_limits<int>::max()));
  const auto deadline = std::chrono::steady_clock::now() + bounded;
  if (!wait_until_readable(control_fd, deadline))
    return reject_set("route-set admission timed out");
  auto first = receive_route(control_fd, expected_pid, expected_uid);
  if (!first)
    return reject_set(std::move(first.detail));
  const auto reference  = first.route->offer();
  const auto generation = first.route->route_generation();
  if (reference.interface_id != expected_interface_id || !valid_route_flags(reference.interface_id, reference.flags))
    return reject_set("route interface rejected");

  std::vector<std::unique_ptr<process_route>> by_shard(reference.producer_shard_count);
  by_shard[reference.producer_shard_id] = std::move(first.route);
  for (uint16_t received = 1; received != reference.producer_shard_count; ++received) {
    if (!wait_until_readable(control_fd, deadline))
      return reject_set("route-set admission timed out");
    auto next = receive_route(control_fd, expected_pid, expected_uid);
    if (!next)
      return reject_set(std::move(next.detail));
    const auto& offer = next.route->offer();
    if (offer.interface_id != reference.interface_id || offer.metadata_schema_id != reference.metadata_schema_id ||
        offer.metadata_schema_version != reference.metadata_schema_version ||
        !valid_route_flags(offer.interface_id, offer.flags) ||
        offer.producer_shard_count != reference.producer_shard_count || next.route->route_generation() != generation)
      return reject_set("inconsistent route shard offer");
    if (by_shard[offer.producer_shard_id] != nullptr)
      return reject_set("duplicate route shard offer");
    for (const auto& route : by_shard) {
      if (route != nullptr && route->offer().route_id == offer.route_id)
        return reject_set("duplicate route identity");
    }
    by_shard[offer.producer_shard_id] = std::move(next.route);
  }
  const bool     cuda_routes = std::any_of(by_shard.begin(), by_shard.end(), [](const auto& route) {
    return route->offer().transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1;
  });
  const uint32_t capabilities =
      OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1 | (cuda_routes ? OCUDU_DAPP_PROCESS_CAP_CUDA_IPC_V1 : 0U);
  if (!send_route_ready(control_fd, reference, generation, capabilities))
    return reject_set("route-ready acknowledgement failed");
  std::vector<ocudu_dapp_process_interface_binding_v1> interfaces{
      {reference.interface_id, reference.producer_shard_count, OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1,
       OCUDU_DAPP_PROCESS_BINDING_BOUND_V1}};
  return {
      std::unique_ptr<process_route_set>(new process_route_set(std::move(by_shard), std::move(interfaces), management)),
      {}};
}

route_set_receive_result receive_route_bundle(int control_fd, pid_t expected_pid, uid_t expected_uid,
                                              const process_interface_expectation* expectations,
                                              std::size_t expectation_count, std::chrono::milliseconds timeout,
                                              process_management_binding management)
{
  if (control_fd < 0 || expected_pid <= 0 || expectations == nullptr || expectation_count < 2 ||
      expectation_count > OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1)
    return reject_set("invalid route bundle expectation");
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return reject_set("unable to apply no_new_privs");
  const auto bounded = std::chrono::milliseconds(
      std::min<int64_t>(std::max<int64_t>(timeout.count(), 0), std::numeric_limits<int>::max()));
  const auto deadline = std::chrono::steady_clock::now() + bounded;
  if (!wait_until_readable(control_fd, deadline))
    return reject_set("route-bundle admission timed out");
  auto received_bundle = receive_route_bundle_frame(control_fd, expected_pid, expected_uid);
  if (!received_bundle)
    return reject_set(std::move(received_bundle.detail));
  const auto& bundle = *received_bundle.bundle;
  if (bundle.interface_count != expectation_count)
    return reject_set("route bundle interface count mismatch");

  std::vector<ocudu_dapp_process_interface_binding_v1> interfaces;
  interfaces.reserve(bundle.interface_count);
  std::size_t expected_routes = 0;
  for (std::size_t index = 0; index != bundle.interface_count; ++index) {
    const auto&   actual      = bundle.interfaces[index];
    const auto&   expectation = expectations[index];
    const uint8_t expected_requirement =
        expectation.required ? OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1 : OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1;
    const bool bound       = actual.state == OCUDU_DAPP_PROCESS_BINDING_BOUND_V1;
    const bool unavailable = actual.state == OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1;
    if (expectation.interface_id == 0 || actual.interface_id != expectation.interface_id ||
        actual.requirement != expected_requirement || (!bound && !unavailable) || (expectation.required && !bound) ||
        (bound && actual.route_count == 0) || (unavailable && (expectation.required || actual.route_count != 0)) ||
        std::any_of(expectations, expectations + index,
                    [&](const auto& previous) { return previous.interface_id == expectation.interface_id; })) {
      return reject_set("route bundle binding rejected");
    }
    expected_routes += actual.route_count;
    interfaces.push_back(actual);
  }
  for (std::size_t index = bundle.interface_count; index != OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1; ++index) {
    const auto& unused = bundle.interfaces[index];
    if (unused.interface_id != 0 || unused.route_count != 0 || unused.requirement != 0 || unused.state != 0)
      return reject_set("route bundle has nonzero unused binding records");
  }
  if (expected_routes != bundle.route_count)
    return reject_set("route bundle route count mismatch");

  std::vector<std::unique_ptr<process_route>> routes;
  routes.reserve(bundle.route_count);
  for (uint16_t index = 0; index != bundle.route_count; ++index) {
    if (!wait_until_readable(control_fd, deadline))
      return reject_set("route-bundle admission timed out");
    auto route = receive_route(control_fd, expected_pid, expected_uid);
    if (!route)
      return reject_set(std::move(route.detail));
    if (route.route->route_generation() != bundle.route_generation)
      return reject_set("route bundle generation mismatch");
    routes.push_back(std::move(route.route));
  }

  std::sort(routes.begin(), routes.end(), [&](const auto& left, const auto& right) {
    const auto ordinal = [&](uint32_t interface_id) {
      for (std::size_t index = 0; index != interfaces.size(); ++index) {
        if (interfaces[index].interface_id == interface_id)
          return index;
      }
      return interfaces.size();
    };
    const auto left_ordinal  = ordinal(left->offer().interface_id);
    const auto right_ordinal = ordinal(right->offer().interface_id);
    return left_ordinal != right_ordinal ? left_ordinal < right_ordinal
                                         : left->offer().producer_shard_id < right->offer().producer_shard_id;
  });
  std::size_t route_offset = 0;
  for (const auto& binding : interfaces) {
    if (binding.state != OCUDU_DAPP_PROCESS_BINDING_BOUND_V1)
      continue;
    uint32_t schema_id      = 0;
    uint32_t schema_version = 0;
    for (uint16_t shard = 0; shard != binding.route_count; ++shard, ++route_offset) {
      if (route_offset >= routes.size())
        return reject_set("incomplete route bundle");
      const auto& offer = routes[route_offset]->offer();
      if (offer.interface_id != binding.interface_id || offer.producer_shard_id != shard ||
          offer.producer_shard_count != binding.route_count || !valid_route_flags(offer.interface_id, offer.flags))
        return reject_set("inconsistent route bundle offer");
      if (shard == 0) {
        schema_id      = offer.metadata_schema_id;
        schema_version = offer.metadata_schema_version;
      } else if (offer.metadata_schema_id != schema_id || offer.metadata_schema_version != schema_version) {
        return reject_set("inconsistent route bundle schema");
      }
      if (std::any_of(routes.begin(), routes.begin() + route_offset,
                      [&](const auto& previous) { return previous->offer().route_id == offer.route_id; }))
        return reject_set("duplicate route bundle identity");
    }
  }
  if (route_offset != routes.size())
    return reject_set("route bundle contains an undeclared interface");

  const bool     cuda_routes  = std::any_of(routes.begin(), routes.end(), [](const auto& route) {
    return route->offer().transport_kind == OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1;
  });
  const uint32_t capabilities = OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1 |
                                OCUDU_DAPP_PROCESS_CAP_MULTI_INTERFACE_V1 |
                                (cuda_routes ? OCUDU_DAPP_PROCESS_CAP_CUDA_IPC_V1 : 0U);
  if (!send_route_bundle_ready(control_fd, bundle, capabilities))
    return reject_set("route-bundle acknowledgement failed");
  return {
      std::unique_ptr<process_route_set>(new process_route_set(std::move(routes), std::move(interfaces), management)),
      {}};
}

const ocudu_dapp_route_offer_v1& process_route::offer() const noexcept
{
  return mapping->route_offer;
}

uint64_t process_route::route_generation() const noexcept
{
  return mapping->route_generation;
}

bool process_route::wait(std::chrono::milliseconds timeout) noexcept
{
  pollfd     descriptor{mapping->notification.get(), POLLIN, 0};
  const auto bounded = std::chrono::milliseconds(
      std::min<int64_t>(std::max<int64_t>(timeout.count(), 0), std::numeric_limits<int>::max()));
  const auto deadline  = std::chrono::steady_clock::now() + bounded;
  auto       remaining = bounded;
  do {
    const int result = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (result >= 0)
      return result == 1 && (descriptor.revents & POLLIN) != 0;
    if (errno != EINTR)
      return false;
    remaining =
        std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                 std::chrono::milliseconds::zero());
  } while (remaining.count() != 0);
  return false;
}

process_route_set::process_route_set(std::vector<std::unique_ptr<process_route>>          routes,
                                     std::vector<ocudu_dapp_process_interface_binding_v1> interfaces,
                                     process_management_binding                           management) :
  mapping(std::make_unique<route_set_mapping>(std::move(routes), std::move(interfaces), management))
{
}

process_route_set::~process_route_set() = default;

std::size_t process_route_set::size() const noexcept
{
  return mapping->routes.size();
}

process_route& process_route_set::route(std::size_t index)
{
  return *mapping->routes.at(index);
}

std::size_t process_route_set::interface_count() const noexcept
{
  return mapping->interfaces.size();
}

const ocudu_dapp_process_interface_binding_v1& process_route_set::interface_binding(std::size_t index) const
{
  return mapping->interfaces.at(index);
}

bool process_route_set::wait(std::chrono::milliseconds timeout) noexcept
{
  for (auto& descriptor : mapping->descriptors)
    descriptor.revents = 0;
  const auto bounded = std::chrono::milliseconds(
      std::min<int64_t>(std::max<int64_t>(timeout.count(), 0), std::numeric_limits<int>::max()));
  const auto deadline  = std::chrono::steady_clock::now() + bounded;
  auto       remaining = bounded;
  do {
    const int result =
        ::poll(mapping->descriptors.data(), mapping->descriptors.size(), static_cast<int>(remaining.count()));
    if (result >= 0) {
      if (result == 0)
        return false;
      if ((mapping->descriptors.front().revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        if ((mapping->descriptors.front().revents & POLLIN) == 0 || !mapping->service_control()) {
          mapping->control_ok = false;
          return false;
        }
        mapping->descriptors.front().revents = 0;
      }
      if (std::any_of(mapping->descriptors.begin() + 1, mapping->descriptors.end(),
                      [](const pollfd& item) { return (item.revents & POLLIN) != 0; }))
        return true;
      remaining =
          std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                   std::chrono::milliseconds::zero());
      continue;
    }
    if (errno != EINTR)
      return false;
    remaining =
        std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                 std::chrono::milliseconds::zero());
  } while (remaining.count() != 0);
  return false;
}

bool process_route_set::control_healthy() const noexcept
{
  return mapping->control_ok;
}

uint64_t process_route::drain_notifications() noexcept
{
  uint64_t total = 0;
  uint64_t value = 0;
  while (::read(mapping->notification.get(), &value, sizeof(value)) == sizeof(value))
    total += value;
  return total;
}

uint32_t process_route::pending_publications() const noexcept
{
  const uint64_t read  = mapping->consumer_read_index;
  const uint64_t write = load_acquire(mapping->header->write_index);
  if (write < read) {
    return 0;
  }
  return static_cast<uint32_t>(std::min<uint64_t>(write - read, mapping->header->slots));
}

bool process_route::send_spectrum_result(ocudu_dapp_process_spectrum_result_v1 result) noexcept
{
  if (mapping->route_offer.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 || result.source_grid_sequence == 0 ||
      result.confidence_q16 > (1U << 16U) || !valid_radio_time(result.created) ||
      !valid_radio_time(result.valid_from) || !valid_radio_time(result.valid_until) || result.first_prb != 0 ||
      result.nof_prbs == 0 || result.nof_prbs > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 ||
      std::any_of(std::begin(result.reserved0), std::end(result.reserved0), [](uint8_t value) { return value != 0; }) ||
      !all_zero(result.reserved, std::size(result.reserved))) {
    return false;
  }
  std::fill(result.prb_confidence + result.nof_prbs, result.prb_confidence + OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1,
            uint8_t{0});
  result.struct_size = sizeof(result);
  result.schema_id   = OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1;
  result.route_id    = mapping->route_offer.route_id;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_SPECTRUM_RESULT_V1;
  header.payload_size = sizeof(result);
  header.sequence     = mapping->route_generation;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&result, sizeof(result)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(mapping->control.get(), &message, MSG_NOSIGNAL) ==
         static_cast<ssize_t>(sizeof(header) + sizeof(result));
}

bool process_route::send_quiet_intent_result(ocudu_dapp_process_quiet_intent_result_v1 result) noexcept
{
  const auto& intent = result.intent;
  if (mapping->route_offer.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 || result.source_grid_sequence == 0 ||
      !valid_radio_time(result.created) || intent.struct_size != sizeof(intent) ||
      intent.abi_major != OCUDU_DAPP_ABI_MAJOR_V1 || (intent.request_id[0] == 0 && intent.request_id[1] == 0) ||
      !valid_radio_time(intent.earliest_start) || !valid_radio_time(intent.latest_start) ||
      intent.duration_slots == 0 || intent.repeat_count == 0 || intent.nof_prbs == 0 || intent.confidence_q16 == 0 ||
      intent.confidence_q16 > (1U << 16U) || intent.expiry_monotonic_ns == 0 || intent.reserved0 != 0 ||
      result.reserved0 != 0 || !all_zero(intent.reserved, std::size(intent.reserved)) ||
      !all_zero(result.reserved, std::size(result.reserved))) {
    return false;
  }
  result.struct_size = sizeof(result);
  result.schema_id   = OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1;
  result.route_id    = mapping->route_offer.route_id;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_QUIET_INTENT_RESULT_V1;
  header.payload_size = sizeof(result);
  header.sequence     = mapping->route_generation;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&result, sizeof(result)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(mapping->control.get(), &message, MSG_NOSIGNAL) ==
         static_cast<ssize_t>(sizeof(header) + sizeof(result));
}

bool process_route::send_srs_isac_result(ocudu_dapp_process_srs_isac_result_v1 result) noexcept
{
  if (mapping->route_offer.interface_id != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 || !valid_srs_preview(result.preview) ||
      !all_zero(result.reserved, std::size(result.reserved))) {
    return false;
  }
  std::fill(result.preview.tap_power + result.preview.tap_count,
            result.preview.tap_power + OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1, 0.0F);
  result.struct_size                 = sizeof(result);
  result.schema_id                   = OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1;
  result.route_id                    = mapping->route_offer.route_id;
  result.preview.struct_size         = sizeof(result.preview);
  result.preview.abi_major           = OCUDU_DAPP_ABI_MAJOR_V1;
  result.preview.abi_minor           = OCUDU_DAPP_ABI_MINOR_V1;
  result.preview.context.struct_size = sizeof(result.preview.context);
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_SRS_ISAC_RESULT_V1;
  header.payload_size = sizeof(result);
  header.sequence     = mapping->route_generation;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&result, sizeof(result)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(mapping->control.get(), &message, MSG_NOSIGNAL) ==
         static_cast<ssize_t>(sizeof(header) + sizeof(result));
}

std::optional<publication_lease> process_route::try_acquire() noexcept
{
  bool expected = false;
  if (!mapping->lease_active.compare_exchange_strong(expected, true, std::memory_order_acquire))
    return std::nullopt;
  const uint64_t read  = mapping->consumer_read_index;
  const uint64_t write = load_acquire(mapping->header->write_index);
  if (read == write) {
    mapping->lease_active.store(false, std::memory_order_release);
    return std::nullopt;
  }
  auto* slot = mapping->slot(read);
  if (slot->size > mapping->header->payload_capacity || slot->sequence != read) {
    mapping->lease_active.store(false, std::memory_order_release);
    return std::nullopt;
  }
  return publication_lease(mapping, reinterpret_cast<const uint8_t*>(slot + 1), slot->size, slot->flags, read,
                           load_acquire(mapping->header->epoch));
}

std::optional<cuda_export_pool_view> process_route::cuda_export_pool() const noexcept
{
  if (mapping->export_header == nullptr || mapping->export_slots == nullptr)
    return std::nullopt;
  return cuda_export_pool_view{mapping->export_header, mapping->export_slots};
}

publication_lease::publication_lease(std::shared_ptr<route_mapping> owner_, const uint8_t* bytes_, std::size_t size_,
                                     uint32_t flags_, uint64_t sequence_, uint64_t epoch_) noexcept :
  owner(std::move(owner_)), bytes(bytes_), byte_count(size_), entry_flags(flags_), entry_sequence(sequence_),
  acquired_epoch(epoch_)
{
}

publication_lease::publication_lease(publication_lease&& other) noexcept
{
  *this = std::move(other);
}

publication_lease& publication_lease::operator=(publication_lease&& other) noexcept
{
  if (this != &other) {
    reset();
    owner            = std::move(other.owner);
    bytes            = other.bytes;
    byte_count       = other.byte_count;
    entry_flags      = other.entry_flags;
    entry_sequence   = other.entry_sequence;
    acquired_epoch   = other.acquired_epoch;
    other.bytes      = nullptr;
    other.byte_count = 0;
  }
  return *this;
}

publication_lease::~publication_lease()
{
  reset();
}

std::optional<ocudu_dapp_publication_record_view_v1> publication_lease::publication() const noexcept
{
  ocudu_dapp_publication_record_view_v1 view{};
  if (ocudu_dapp_parse_publication_record_v1(bytes, byte_count, &view) != OCUDU_DAPP_PUBLICATION_RECORD_OK_V1)
    return std::nullopt;
  return view;
}

void publication_lease::reset() noexcept
{
  if (owner != nullptr && load_acquire(owner->header->epoch) == acquired_epoch) {
    const uint64_t increment = 1;
    const ssize_t  written   = ::write(owner->completion.get(), &increment, sizeof(increment));
    if (written == sizeof(increment))
      owner->consumer_read_index = entry_sequence + 1;
  }
  if (owner != nullptr)
    owner->lease_active.store(false, std::memory_order_release);
  owner.reset();
  bytes      = nullptr;
  byte_count = 0;
}

} // namespace ocudu::dapp::sdk
