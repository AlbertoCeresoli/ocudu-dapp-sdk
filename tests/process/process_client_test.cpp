// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "class-c-publication-v1_generated.h"
#include "ocudu/dapp/process/v1/shared_ring.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include "ocudu_dapp_sdk/class_c_input.h"
#include "ocudu_dapp_sdk/module_support.h"
#include "ocudu_dapp_sdk/process_client.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <flatbuffers/flatbuffers.h>
#include <iostream>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace ocudu::dapp::sdk;
using namespace ocudu::dapp::reference;
using namespace std::chrono_literals;

namespace {

namespace schema = Ocudu::Dapp::ClassC::V1;

struct complex32 {
  float real;
  float imag;
};

bool check(bool condition, const char* message)
{
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

uint64_t monotonic_time_ns()
{
  const auto value =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  return value > 0 ? static_cast<uint64_t>(value) : 1;
}

struct host_route {
  int                               ring{-1};
  int                               data_export{-1};
  int                               event{-1};
  int                               completion{-1};
  void*                             address{MAP_FAILED};
  std::size_t                       bytes{};
  ocudu_dapp_shared_ring_header_v1* header{};
  host_route() = default;
  host_route(host_route&& other) noexcept :
    ring(other.ring), data_export(other.data_export), event(other.event), completion(other.completion),
    address(other.address), bytes(other.bytes), header(other.header)
  {
    other.ring        = -1;
    other.data_export = -1;
    other.event       = -1;
    other.completion  = -1;
    other.address     = MAP_FAILED;
    other.bytes       = 0;
    other.header      = nullptr;
  }
  host_route(const host_route&)            = delete;
  host_route& operator=(const host_route&) = delete;
  ~host_route()
  {
    if (address != MAP_FAILED)
      ::munmap(address, bytes);
    if (ring >= 0)
      ::close(ring);
    if (data_export >= 0)
      ::close(data_export);
    if (event >= 0)
      ::close(event);
    if (completion >= 0)
      ::close(completion);
  }
};

host_route make_route()
{
  host_route         result;
  constexpr uint32_t slots   = 2;
  constexpr uint32_t payload = 4096;
  constexpr uint64_t stride  = 4160;
  result.bytes               = OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1 + slots * stride;
  result.ring                = ::memfd_create("sdk-process-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (result.ring < 0 || ::ftruncate(result.ring, result.bytes) != 0 ||
      ::fcntl(result.ring, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
    return result;
  result.address = ::mmap(nullptr, result.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, result.ring, 0);
  if (result.address == MAP_FAILED)
    return result;
  std::memset(result.address, 0, result.bytes);
  result.header                   = static_cast<ocudu_dapp_shared_ring_header_v1*>(result.address);
  result.header->magic            = OCUDU_DAPP_SHARED_RING_MAGIC_V1;
  result.header->version          = OCUDU_DAPP_SHARED_RING_VERSION_V1;
  result.header->header_size      = OCUDU_DAPP_SHARED_RING_HEADER_BYTES_V1;
  result.header->mapped_bytes     = result.bytes;
  result.header->slots            = slots;
  result.header->payload_capacity = payload;
  result.header->slot_stride      = stride;
  result.header->epoch            = 1;
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/fd/%d", result.ring);
  result.data_export = ::open(path, O_RDONLY | O_CLOEXEC);
  result.event       = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  result.completion  = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  return result;
}

bool send_offer(int socket, const host_route& route, uint32_t interface_id = OCUDU_DAPP_INTERFACE_SPECTRUM_V1,
                uint16_t shard_id = 0, uint16_t shard_count = 1, uint64_t generation = 17, uint32_t flags = 0)
{
  ocudu_dapp_route_offer_v1 offer{};
  offer.struct_size             = sizeof(offer);
  offer.interface_id            = interface_id;
  offer.metadata_schema_id      = OCUDU_DAPP_CLASS_C_PUBLICATION_SCHEMA_V1;
  offer.metadata_schema_version = 1;
  offer.route_id                = 9 + shard_id;
  offer.producer_shard_id       = shard_id;
  offer.producer_shard_count    = shard_count;
  offer.flags                   = flags;
  ocudu_dapp_control_header_v1 header{};
  header.magic            = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version          = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type             = OCUDU_DAPP_CONTROL_ROUTE_OFFER_V1;
  header.payload_size     = sizeof(offer);
  header.descriptor_count = OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1;
  header.sequence         = generation;
  std::array<iovec, 2>                                  vectors{{{&header, sizeof(header)}, {&offer, sizeof(offer)}}};
  std::array<int, OCUDU_DAPP_ROUTE_DESCRIPTOR_COUNT_V1> descriptors{route.data_export, route.event, route.completion};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(descriptors))> control{};
  msghdr                                                                message{};
  message.msg_iov        = vectors.data();
  message.msg_iovlen     = vectors.size();
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  auto* rights           = CMSG_FIRSTHDR(&message);
  rights->cmsg_level     = SOL_SOCKET;
  rights->cmsg_type      = SCM_RIGHTS;
  rights->cmsg_len       = CMSG_LEN(sizeof(descriptors));
  std::memcpy(CMSG_DATA(rights), descriptors.data(), sizeof(descriptors));
  return ::sendmsg(socket, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(offer));
}

bool send_bundle(int socket, const ocudu_dapp_route_bundle_v1& bundle)
{
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_V1;
  header.payload_size = sizeof(bundle);
  header.sequence     = bundle.route_generation;
  std::array<iovec, 2> vectors{
      {{&header, sizeof(header)}, {const_cast<ocudu_dapp_route_bundle_v1*>(&bundle), sizeof(bundle)}}};
  msghdr message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  return ::sendmsg(socket, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header) + sizeof(bundle));
}

bool receive_bundle_ready(int socket, pid_t expected_pid, const ocudu_dapp_route_bundle_v1& bundle)
{
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + sizeof(ocudu_dapp_route_bundle_ready_v1)> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(ucred))>                                      control{};
  iovec  vector{data.data(), data.size()};
  msghdr message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(socket, &message, 0);
  if (received != static_cast<ssize_t>(data.size()) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
    return false;
  ucred credentials{};
  bool  have_credentials = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
        item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    }
  }
  ocudu_dapp_control_header_v1     header{};
  ocudu_dapp_route_bundle_ready_v1 ready{};
  std::memcpy(&header, data.data(), sizeof(header));
  std::memcpy(&ready, data.data() + sizeof(header), sizeof(ready));
  return have_credentials && credentials.pid == expected_pid && credentials.uid == ::geteuid() &&
         header.magic == OCUDU_DAPP_CONTROL_MAGIC_V1 && header.version == OCUDU_DAPP_CONTROL_VERSION_V1 &&
         header.type == OCUDU_DAPP_CONTROL_ROUTE_BUNDLE_READY_V1 && header.payload_size == sizeof(ready) &&
         header.descriptor_count == 0 && header.reserved == 0 && header.sequence == bundle.route_generation &&
         ready.struct_size == sizeof(ready) && ready.interface_count == bundle.interface_count &&
         ready.route_count == bundle.route_count && ready.route_generation == bundle.route_generation &&
         ready.bundle_id == bundle.bundle_id &&
         ready.flags == (OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1 | OCUDU_DAPP_PROCESS_CAP_MULTI_INTERFACE_V1) &&
         ready.reserved0 == 0 && ready.reserved[0] == 0 && ready.reserved[1] == 0;
}

bool receive_ready(int socket, pid_t expected_pid, uint32_t interface_id, uint16_t shard_count,
                   uint64_t generation = 17)
{
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + sizeof(ocudu_dapp_route_ready_v1)> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(ucred))>                               control{};
  iovec  vector{data.data(), data.size()};
  msghdr message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(socket, &message, 0);
  if (received != static_cast<ssize_t>(data.size()) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
    return false;
  ucred credentials{};
  bool  have_credentials = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
        item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    }
  }
  ocudu_dapp_control_header_v1 header{};
  ocudu_dapp_route_ready_v1    ready{};
  std::memcpy(&header, data.data(), sizeof(header));
  std::memcpy(&ready, data.data() + sizeof(header), sizeof(ready));
  return have_credentials && credentials.pid == expected_pid && credentials.uid == ::geteuid() &&
         header.magic == OCUDU_DAPP_CONTROL_MAGIC_V1 && header.version == OCUDU_DAPP_CONTROL_VERSION_V1 &&
         header.type == OCUDU_DAPP_CONTROL_ROUTE_READY_V1 && header.payload_size == sizeof(ready) &&
         header.descriptor_count == 0 && header.reserved == 0 && header.sequence == generation &&
         ready.struct_size == sizeof(ready) && ready.interface_id == interface_id &&
         ready.route_generation == generation && ready.producer_shard_count == shard_count && ready.reserved0 == 0 &&
         ready.flags == OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1 && ready.reserved == 0;
}

bool send_heartbeat_and_receive_response(int socket, uint64_t generation)
{
  ocudu_dapp_process_heartbeat_v1 heartbeat{};
  heartbeat.struct_size       = sizeof(heartbeat);
  heartbeat.route_generation  = generation;
  heartbeat.challenge         = 0x12345678;
  heartbeat.host_monotonic_ns = 99;
  ocudu_dapp_control_header_v1 header{};
  header.magic        = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version      = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type         = OCUDU_DAPP_CONTROL_HEARTBEAT_REQUEST_V1;
  header.payload_size = sizeof(heartbeat);
  header.sequence     = generation;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&heartbeat, sizeof(heartbeat)}}};
  msghdr               message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  if (::sendmsg(socket, &message, MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(header) + sizeof(heartbeat)))
    return false;
  std::array<uint8_t, sizeof(header) + sizeof(heartbeat)> response{};
  if (::recv(socket, response.data(), response.size(), 0) != static_cast<ssize_t>(response.size()))
    return false;
  ocudu_dapp_control_header_v1    returned_header{};
  ocudu_dapp_process_heartbeat_v1 returned_heartbeat{};
  std::memcpy(&returned_header, response.data(), sizeof(returned_header));
  std::memcpy(&returned_heartbeat, response.data() + sizeof(returned_header), sizeof(returned_heartbeat));
  return returned_header.magic == header.magic && returned_header.version == header.version &&
         returned_header.type == OCUDU_DAPP_CONTROL_HEARTBEAT_RESPONSE_V1 &&
         returned_header.payload_size == sizeof(returned_heartbeat) && returned_header.sequence == generation &&
         std::memcmp(&returned_heartbeat, &heartbeat, sizeof(heartbeat)) == 0;
}

bool management_exchange(int socket, ocudu_dapp_process_management_request_v1 request,
                         ocudu_dapp_process_management_response_v1& response, int descriptor = -1)
{
  request.struct_size        = sizeof(request);
  request.route_generation   = 17;
  static uint64_t request_id = 0;
  request.request_id         = ++request_id;
  ocudu_dapp_control_header_v1 header{};
  header.magic            = OCUDU_DAPP_CONTROL_MAGIC_V1;
  header.version          = OCUDU_DAPP_CONTROL_VERSION_V1;
  header.type             = OCUDU_DAPP_CONTROL_MANAGEMENT_REQUEST_V1;
  header.payload_size     = sizeof(request);
  header.descriptor_count = descriptor >= 0 ? 1 : 0;
  header.sequence         = 17;
  std::array<iovec, 2> vectors{{{&header, sizeof(header)}, {&request, sizeof(request)}}};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> control{};
  msghdr                                                        message{};
  message.msg_iov    = vectors.data();
  message.msg_iovlen = vectors.size();
  if (descriptor >= 0) {
    message.msg_control    = control.data();
    message.msg_controllen = control.size();
    auto* rights           = CMSG_FIRSTHDR(&message);
    rights->cmsg_level     = SOL_SOCKET;
    rights->cmsg_type      = SCM_RIGHTS;
    rights->cmsg_len       = CMSG_LEN(sizeof(descriptor));
    std::memcpy(CMSG_DATA(rights), &descriptor, sizeof(descriptor));
  }
  if (::sendmsg(socket, &message, MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(header) + sizeof(request)))
    return false;
  std::array<uint8_t, sizeof(header) + sizeof(response)> bytes{};
  if (::recv(socket, bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size()))
    return false;
  ocudu_dapp_control_header_v1 returned{};
  std::memcpy(&returned, bytes.data(), sizeof(returned));
  std::memcpy(&response, bytes.data() + sizeof(returned), sizeof(response));
  return returned.magic == header.magic && returned.version == header.version &&
         returned.type == OCUDU_DAPP_CONTROL_MANAGEMENT_RESPONSE_V1 && returned.payload_size == sizeof(response) &&
         returned.descriptor_count == 0 && returned.sequence == 17 && response.struct_size == sizeof(response) &&
         response.operation == request.operation && response.route_generation == 17 &&
         response.request_id == request.request_id;
}

int make_sealed_model()
{
  const int                    writable = ::memfd_create("sdk-model-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  const std::array<uint8_t, 4> bytes{1, 2, 3, 4};
  if (writable < 0 || ::write(writable, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()) ||
      ::fcntl(writable, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
    if (writable >= 0)
      ::close(writable);
    return -1;
  }
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/fd/%d", writable);
  const int readonly = ::open(path, O_RDONLY | O_CLOEXEC);
  ::close(writable);
  return readonly;
}

std::vector<uint8_t> make_metadata(uint32_t interface_id, std::size_t tensor_size,
                                   schema::ElementType  type         = schema::ElementType::ComplexFp32,
                                   uint64_t             element_size = sizeof(complex32),
                                   schema::MemoryDomain domain       = schema::MemoryDomain::PosixShared,
                                   uint64_t observation_time_ns = 100, uint64_t lease_expiry_ns = 1000)
{
  flatbuffers::FlatBufferBuilder builder;
  const schema::Id128            identity(1, 2);
  const schema::Id128            lease(3, 4);
  const schema::RadioTime        time(10, 2, 1, 0x3fff);
  const auto                     context =
      schema::CreateObservationContextDirect(builder, &time, 7, &identity, "monotonic", observation_time_ns);
  const uint64_t        elements = tensor_size / element_size;
  std::vector<uint64_t> dimensions{1, 1, elements};
  std::vector<uint64_t> strides{elements * element_size, elements * element_size, element_size};
  const auto tensor = schema::CreateTensorDescriptorDirect(builder, &identity, 1, domain, 0, type, "reference-complex",
                                                           1, 0, 0, tensor_size, &dimensions, &strides, 65535, nullptr,
                                                           &lease, lease_expiry_ns, true);
  schema::PublicationKind     kind;
  schema::PublicationMetadata metadata_type;
  flatbuffers::Offset<void>   metadata;
  if (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
    const bool           plan_present = elements >= 12 && elements % 12 == 0;
    std::vector<uint8_t> plan(plan_present ? elements / 12 : 0, OCUDU_DAPP_UL_USE_PLANNED_EMPTY_V1);
    if (!plan.empty()) {
      plan.front() = OCUDU_DAPP_UL_USE_PUSCH_V1;
    }
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> annotations;
    if (plan_present) {
      annotations = builder.CreateVector(plan);
    }
    const auto item = schema::CreateSpectrumMetadata(
        builder, 100'000'000, 1, 1, elements, 1.0F, 0, 0, 0, annotations, plan_present ? 42 : 0,
        plan_present ? schema::GridMaskGranularity::SymbolPrb : schema::GridMaskGranularity::None, plan_present);
    kind          = schema::PublicationKind::Spectrum;
    metadata_type = schema::PublicationMetadata::SpectrumMetadata;
    metadata      = item.Union();
  } else {
    const auto item = schema::CreateSrsIsacMetadata(builder, 1, 1, 1, 1, 2, 0, 0, 30'000, elements, 0.0F);
    kind            = schema::PublicationKind::SrsIsac;
    metadata_type   = schema::PublicationMetadata::SrsIsacMetadata;
    metadata        = item.Union();
  }
  const auto envelope =
      schema::CreatePublicationEnvelopeDirect(builder, "org.ocudu.dapp.class-c.publication", 1, 0, kind, 1, &identity,
                                              context, tensor, metadata_type, metadata);
  schema::FinishPublicationEnvelopeBuffer(builder, envelope);
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

int cbf16_decode_test()
{
  const std::array<uint16_t, 4>         payload{{0x3f80, 0, 0, 0x4000}};
  const auto                            metadata = make_metadata(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, sizeof(payload),
                                                                 schema::ElementType::ComplexBf16, sizeof(uint16_t) * 2);
  ocudu::dapp::reference::class_c_input input{};
  if (!check(ocudu::dapp::reference::decode_class_c_input(metadata.data(), metadata.size(),
                                                          reinterpret_cast<const uint8_t*>(payload.data()),
                                                          sizeof(payload), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, input),
             "complex-BF16 metadata decode failed")) {
    return 1;
  }
  ocudu::dapp::reference::spectrum_radiometer_result result{};
  return check(input.tensor.element_type == OCUDU_DAPP_ELEMENT_CBF16_V1, "complex-BF16 element type was lost") &&
                 check(input.observation_time_ns == 100 && input.lease_expiry_ns == 1000 &&
                           !class_c_input_expired(input, 999) && class_c_input_expired(input, 1000),
                       "Class-C lease freshness was not preserved") &&
                 check(ocudu::dapp::reference::run_spectrum_radiometer(input.tensor, input.amplitude_scale, 1.5,
                                                                       result) &&
                           result.samples == 2 && result.mean_power == 2.5,
                       "complex-BF16 decoded radiometer result mismatch")
             ? 0
             : 1;
}

int portable_host_decode_test()
{
  const std::array<complex32, 4>        payload{{{1, 0}, {2, 0}, {3, 0}, {4, 0}}};
  const auto                            metadata = make_metadata(OCUDU_DAPP_INTERFACE_SPECTRUM_V1, sizeof(payload),
                                                                 schema::ElementType::ComplexFp32, sizeof(complex32), schema::MemoryDomain::Host);
  ocudu::dapp::reference::class_c_input input{};
  if (!check(ocudu::dapp::reference::decode_class_c_input(metadata.data(), metadata.size(),
                                                          reinterpret_cast<const uint8_t*>(payload.data()),
                                                          sizeof(payload), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, input),
             "portable host metadata decode failed")) {
    return 1;
  }
  ocudu::dapp::reference::spectrum_radiometer_result result{};
  return check(input.tensor.memory_domain == OCUDU_DAPP_MEM_HOST_V1, "portable host domain was lost") &&
                 check(ocudu::dapp::reference::run_spectrum_radiometer(input.tensor, 1.0, 1.0, result) &&
                           result.samples == payload.size() && result.mean_power == 7.5,
                       "portable host radiometer result mismatch")
             ? 0
             : 1;
}

int ul_plan_decode_test()
{
  flatbuffers::FlatBufferBuilder builder;
  const schema::Id128            identity(1, 2);
  const schema::RadioTime        time(10, 2, 1, 0);
  const auto context = schema::CreateObservationContextDirect(builder, &time, 7, &identity, "monotonic", 100);
  std::array<flatbuffers::Offset<schema::UlAllocationSpan>, 2> spans{
      schema::CreateUlAllocationSpan(builder, 2, 4, 10, 8, schema::UlUseCategory::Pusch, 1U << 3U, 7),
      schema::CreateUlAllocationSpan(builder, 10, 2, 30, 1, schema::UlUseCategory::Pucch, 0, 8)};
  const auto span_vector = builder.CreateVector(spans.data(), spans.size());
  const auto metadata = schema::CreateUlResourcePlanMetadata(builder, 19, 100, 0, 52, schema::GridMaskGranularity::None,
                                                             span_vector, true)
                            .Union();
  const auto envelope =
      schema::CreatePublicationEnvelope(builder, builder.CreateString("org.ocudu.dapp.class-c.publication"), 1, 0,
                                        schema::PublicationKind::UlResourcePlan, 19, &identity, context, 0,
                                        schema::PublicationMetadata::UlResourcePlanMetadata, metadata);
  schema::FinishPublicationEnvelopeBuffer(builder, envelope);
  ocudu::dapp::reference::class_c_input input{};
  if (!check(ocudu::dapp::reference::decode_class_c_input(builder.GetBufferPointer(), builder.GetSize(), nullptr, 0,
                                                          OCUDU_DAPP_INTERFACE_SPECTRUM_V1, input),
             "advance UL plan metadata decode failed")) {
    return 1;
  }
  return check(input.kind == ocudu::dapp::reference::class_c_input_kind::ul_resource_plan &&
                   input.plan_generation == 19 && input.cell_id == 7 && input.cell_nof_prbs == 52 &&
                   input.plan_complete && input.nof_plan_spans == 2 && input.plan_spans[0].first_prb == 10 &&
                   input.plan_spans[1].use_category == OCUDU_DAPP_UL_USE_PUCCH_V1,
               "advance UL plan fields mismatch")
             ? 0
             : 1;
}

void write_publication(host_route& route, uint64_t ring_sequence, const uint8_t* data, std::size_t size,
                       uint32_t interface_id, uint64_t observation_time_ns)
{
  auto* slot = reinterpret_cast<ocudu_dapp_shared_ring_slot_v1*>(
      static_cast<uint8_t*>(route.address) + route.header->header_size +
      (ring_sequence % route.header->slots) * route.header->slot_stride);
  const auto metadata =
      make_metadata(interface_id, size, schema::ElementType::ComplexFp32, sizeof(complex32),
                    schema::MemoryDomain::PosixShared, observation_time_ns, observation_time_ns + 100'000'000);
  const std::size_t tensor_offset = (sizeof(ocudu_dapp_publication_record_header_v1) + metadata.size() + 63U) & ~63U;
  slot->size                      = tensor_offset + size;
  slot->flags                     = 7;
  slot->sequence                  = ring_sequence;
  auto* record                    = reinterpret_cast<uint8_t*>(slot + 1);
  std::memset(record, 0, slot->size);
  ocudu_dapp_publication_record_header_v1 record_header{};
  record_header.magic                = OCUDU_DAPP_PUBLICATION_RECORD_MAGIC_V1;
  record_header.version              = OCUDU_DAPP_PUBLICATION_RECORD_VERSION_V1;
  record_header.header_size          = sizeof(record_header);
  record_header.flags                = OCUDU_DAPP_PUBLICATION_RECORD_INLINE_TENSOR_V1;
  record_header.metadata_size        = metadata.size();
  record_header.tensor_offset        = tensor_offset;
  record_header.tensor_size          = size;
  record_header.publication_sequence = 1;
  record_header.route_epoch          = 1;
  std::memcpy(record, &record_header, sizeof(record_header));
  std::memcpy(record + sizeof(record_header), metadata.data(), metadata.size());
  std::memcpy(record + tensor_offset, data, size);
}

void notify_publications(host_route& route, uint64_t write_index)
{
  __atomic_store_n(&route.header->write_index, write_index, __ATOMIC_RELEASE);
  const uint64_t wake = write_index;
  if (::write(route.event, &wake, sizeof(wake)) != static_cast<ssize_t>(sizeof(wake))) {
    std::abort();
  }
}

void publish(host_route& route, const uint8_t* data, std::size_t size,
             uint32_t interface_id = OCUDU_DAPP_INTERFACE_SPECTRUM_V1)
{
  write_publication(route, 0, data, size, interface_id, monotonic_time_ns());
  notify_publications(route, 1);
}

void publish_latest_batch(host_route& route, const uint8_t* latest, std::size_t size, uint32_t interface_id)
{
  std::vector<uint8_t> superseded(size, 0);
  const uint64_t       now = monotonic_time_ns();
  write_publication(route, 0, superseded.data(), superseded.size(), interface_id, now);
  write_publication(route, 1, latest, size, interface_id, now + 1);
  notify_publications(route, 2);
}

} // namespace

int direct_client_test()
{
  int sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "credential setup failed"))
    return 1;
  auto host = make_route();
  if (!check(host.ring >= 0 && host.data_export >= 0 && host.event >= 0 && host.completion >= 0, "host setup failed"))
    return 1;
  const pid_t sender = ::fork();
  if (!check(sender >= 0, "fork failed"))
    return 1;
  if (sender == 0) {
    ::close(sockets[1]);
    _exit(send_offer(sockets[0], host) ? 0 : 2);
  }
  ::close(sockets[0]);
  auto received     = receive_route(sockets[1], sender, ::geteuid());
  int  child_status = 0;
  if (!check(::waitpid(sender, &child_status, 0) == sender && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
             "offer sender failed"))
    return 1;
  if (!check(static_cast<bool>(received), received.detail.c_str()))
    return 1;
  if (!check(received.route->offer().route_id == 9, "route identity mismatch"))
    return 1;

  const std::array<complex32, 2> payload{{{1, 0}, {0, 1}}};
  publish(host, reinterpret_cast<const uint8_t*>(payload.data()), sizeof(payload));
  if (!check(received.route->wait(100ms), "notification wait failed") ||
      !check(received.route->drain_notifications() == 1, "notification drain failed") ||
      !check(received.route->pending_publications() == 1, "pending publication count mismatch"))
    return 1;
  auto       lease       = received.route->try_acquire();
  const auto publication = lease ? lease->publication() : std::nullopt;
  if (!check(lease.has_value(), "publication acquire failed") ||
      !check(publication.has_value(), "publication record rejected") ||
      !check(publication->header.tensor_size == sizeof(payload) &&
                 std::memcmp(publication->tensor, payload.data(), sizeof(payload)) == 0,
             "publication payload mismatch") ||
      !check(lease->flags() == 7, "publication flags mismatch"))
    return 1;
  lease.reset();
  if (!check(received.route->pending_publications() == 0, "completed publication remained pending"))
    return 1;
  uint64_t completions = 0;
  if (!check(::read(host.completion, &completions, sizeof(completions)) == sizeof(completions) && completions == 1,
             "lease did not signal completion"))
    return 1;
  ::close(sockets[1]);
  return 0;
}

int route_set_test()
{
  int sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "route-set socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0 &&
                 ::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "route-set credential setup failed"))
    return 1;
  auto        shard_one  = make_route();
  auto        shard_zero = make_route();
  const pid_t sender     = ::fork();
  if (!check(sender >= 0, "route-set fork failed"))
    return 1;
  if (sender == 0) {
    ::close(sockets[1]);
    const bool sent = send_offer(sockets[0], shard_one, OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 1, 2, 17,
                                 OCUDU_DAPP_ROUTE_FLAG_UL_RESOURCE_PLAN_V1) &&
                      send_offer(sockets[0], shard_zero, OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 0, 2);
    if (!sent || !receive_ready(sockets[0], ::getppid(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 2))
      _exit(2);
    uint8_t ignored = 0;
    static_cast<void>(::recv(sockets[0], &ignored, sizeof(ignored), 0));
    _exit(0);
  }
  ::close(sockets[0]);
  auto received = receive_route_set(sockets[1], sender, ::geteuid(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 100ms);
  if (!check(static_cast<bool>(received), received.detail.c_str()) ||
      !check(::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1, "worker did not apply no_new_privs") ||
      !check(received.route_set->size() == 2, "route-set size mismatch") ||
      !check(received.route_set->route(0).offer().producer_shard_id == 0 &&
                 received.route_set->route(1).offer().producer_shard_id == 1,
             "route-set ordering mismatch"))
    return 1;

  const std::array<complex32, 2> payload{{{1, 1}, {2, 0}}};
  publish(shard_zero, reinterpret_cast<const uint8_t*>(payload.data()), sizeof(payload));
  if (!check(received.route_set->wait(100ms), "route-set wait failed"))
    return 1;
  auto& route = received.route_set->route(0);
  route.drain_notifications();
  auto       lease       = route.try_acquire();
  const auto publication = lease ? lease->publication() : std::nullopt;
  if (!check(publication.has_value(), "sharded publication rejected") ||
      !check(publication->header.tensor_size == sizeof(payload) &&
                 std::memcmp(publication->tensor, payload.data(), sizeof(payload)) == 0,
             "sharded publication mismatch"))
    return 1;
  lease.reset();
  uint64_t   completions = 0;
  const bool completed =
      ::read(shard_zero.completion, &completions, sizeof(completions)) == sizeof(completions) && completions == 1;
  received.route_set.reset();
  ::close(sockets[1]);
  int child_status = 0;
  return check(completed, "sharded publication completion missing") &&
                 check(::waitpid(sender, &child_status, 0) == sender && WIFEXITED(child_status) &&
                           WEXITSTATUS(child_status) == 0,
                       "route-set sender failed")
             ? 0
             : 1;
}

int route_bundle_test()
{
  constexpr uint32_t future_dmrs_interface_id = 0x7f010002U;
  int                sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "route-bundle socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0 &&
                 ::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "route-bundle credential setup failed"))
    return 1;
  auto                       route = make_route();
  ocudu_dapp_route_bundle_v1 bundle{};
  bundle.struct_size      = sizeof(bundle);
  bundle.interface_count  = 2;
  bundle.route_count      = 1;
  bundle.route_generation = 29;
  bundle.bundle_id        = 0x1020304050607080ULL;
  bundle.interfaces[0]    = {OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, 1, OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1,
                             OCUDU_DAPP_PROCESS_BINDING_BOUND_V1};
  bundle.interfaces[1]    = {future_dmrs_interface_id, 0, OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1,
                             OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1};
  const pid_t sender      = ::fork();
  if (!check(sender >= 0, "route-bundle fork failed"))
    return 1;
  if (sender == 0) {
    ::close(sockets[1]);
    if (!send_bundle(sockets[0], bundle) ||
        !send_offer(sockets[0], route, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, 0, 1, bundle.route_generation) ||
        !receive_bundle_ready(sockets[0], ::getppid(), bundle))
      _exit(2);
    uint8_t ignored = 0;
    static_cast<void>(::recv(sockets[0], &ignored, sizeof(ignored), 0));
    _exit(0);
  }
  ::close(sockets[0]);
  const std::array<process_interface_expectation, 2> expected{
      {{OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, true}, {future_dmrs_interface_id, false}}};
  auto received = receive_route_bundle(sockets[1], sender, ::geteuid(), expected.data(), expected.size(), 100ms);
  if (!check(static_cast<bool>(received), received.detail.c_str()) ||
      !check(received.route_set->size() == 1 && received.route_set->interface_count() == 2,
             "route-bundle dimensions mismatch") ||
      !check(received.route_set->interface_binding(0).state == OCUDU_DAPP_PROCESS_BINDING_BOUND_V1 &&
                 received.route_set->interface_binding(1).state == OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1,
             "route-bundle degraded binding state mismatch") ||
      !check(received.route_set->route(0).offer().interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1,
             "route-bundle route ordering mismatch"))
    return 1;
  received.route_set.reset();
  ::close(sockets[1]);
  int child_status = 0;
  return check(::waitpid(sender, &child_status, 0) == sender && WIFEXITED(child_status) &&
                   WEXITSTATUS(child_status) == 0,
               "route-bundle sender failed")
             ? 0
             : 1;
}

int parent_lifetime_test()
{
  int signal = 0;
  return check(!bind_parent_lifetime(1), "init was accepted as a supervising parent") &&
                 check(bind_parent_lifetime(::getppid()), "parent lifetime binding failed") &&
                 check(::prctl(PR_GET_PDEATHSIG, &signal, 0, 0, 0) == 0 && signal == SIGKILL,
                       "parent death signal is not SIGKILL") &&
                 check(::prctl(PR_SET_PDEATHSIG, 0, 0, 0, 0) == 0, "parent death signal reset failed")
             ? 0
             : 1;
}

int parent_death_enforced_test()
{
  if (!check(::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0, "subreaper setup failed"))
    return 1;
  int ready[2];
  int release[2];
  if (!check(::pipe2(ready, O_CLOEXEC) == 0 && ::pipe2(release, O_CLOEXEC) == 0, "lifetime pipes failed"))
    return 1;
  const pid_t supervisor = ::fork();
  if (!check(supervisor >= 0, "lifetime supervisor fork failed"))
    return 1;
  if (supervisor == 0) {
    ::close(ready[0]);
    ::close(release[1]);
    const pid_t worker = ::fork();
    if (worker < 0)
      _exit(2);
    if (worker == 0) {
      ::close(release[0]);
      const pid_t expected_parent = ::getppid();
      const pid_t own_pid         = ::getpid();
      if (!bind_parent_lifetime(expected_parent) ||
          ::write(ready[1], &own_pid, sizeof(own_pid)) != static_cast<ssize_t>(sizeof(own_pid)))
        _exit(3);
      for (;;)
        ::pause();
    }
    ::close(ready[1]);
    char command = 0;
    _exit(::read(release[0], &command, sizeof(command)) == static_cast<ssize_t>(sizeof(command)) ? 0 : 4);
  }
  ::close(ready[1]);
  ::close(release[0]);
  pid_t      worker = -1;
  const bool announced =
      ::read(ready[0], &worker, sizeof(worker)) == static_cast<ssize_t>(sizeof(worker)) && worker > 0;
  ::close(ready[0]);
  char       command  = 1;
  const bool released = ::write(release[1], &command, sizeof(command)) == static_cast<ssize_t>(sizeof(command));
  ::close(release[1]);
  int        supervisor_status = 0;
  int        worker_status     = 0;
  const bool stopped           = announced && released && ::waitpid(supervisor, &supervisor_status, 0) == supervisor &&
                       WIFEXITED(supervisor_status) && WEXITSTATUS(supervisor_status) == 0 &&
                       ::waitpid(worker, &worker_status, 0) == worker && WIFSIGNALED(worker_status) &&
                       WTERMSIG(worker_status) == SIGKILL;
  static_cast<void>(::prctl(PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0));
  return check(stopped, "worker survived its supervising parent") ? 0 : 1;
}

int duplicate_route_set_test()
{
  int sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "duplicate-set socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "duplicate-set credential setup failed"))
    return 1;
  auto        first     = make_route();
  auto        duplicate = make_route();
  const pid_t sender    = ::fork();
  if (!check(sender >= 0, "duplicate-set fork failed"))
    return 1;
  if (sender == 0) {
    ::close(sockets[1]);
    const bool sent = send_offer(sockets[0], first, OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 0, 2) &&
                      send_offer(sockets[0], duplicate, OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 0, 2);
    _exit(sent ? 0 : 2);
  }
  ::close(sockets[0]);
  const auto received     = receive_route_set(sockets[1], sender, ::geteuid(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 100ms);
  int        child_status = 0;
  return check(::waitpid(sender, &child_status, 0) == sender && WIFEXITED(child_status) &&
                   WEXITSTATUS(child_status) == 0,
               "duplicate-set sender failed") &&
                 check(!received && received.detail == "duplicate route shard offer", "duplicate shard was accepted")
             ? 0
             : 1;
}

int heartbeat_route_set_test()
{
  int sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "heartbeat socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0 &&
                 ::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "heartbeat credential setup failed"))
    return 1;
  auto        route  = make_route();
  const pid_t sender = ::fork();
  if (!check(sender >= 0, "heartbeat fork failed"))
    return 1;
  if (sender == 0) {
    ::close(sockets[1]);
    const bool ready =
        send_offer(sockets[0], route) && receive_ready(sockets[0], ::getppid(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 1);
    const bool heartbeat = ready && send_heartbeat_and_receive_response(sockets[0], 17);
    std::this_thread::sleep_for(30ms);
    _exit(heartbeat ? 0 : 2);
  }
  ::close(sockets[0]);
  auto received = receive_route_set(sockets[1], sender, ::geteuid(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 100ms);
  if (!check(static_cast<bool>(received), received.detail.c_str()))
    return 1;
  const bool publication_ready = received.route_set->wait(10ms);
  int        status            = 0;
  return check(!publication_ready, "heartbeat was surfaced as a data publication") &&
                 check(received.route_set->control_healthy(), "valid heartbeat made control unhealthy") &&
                 check(::waitpid(sender, &status, 0) == sender && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                       "heartbeat exchange failed")
             ? 0
             : 1;
}

int management_route_set_test()
{
  int sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "management socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0 &&
                 ::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "management credential setup failed"))
    return 1;
  auto        route  = make_route();
  const pid_t sender = ::fork();
  if (!check(sender >= 0, "management fork failed"))
    return 1;
  if (sender == 0) {
    ::close(sockets[1]);
    bool ok =
        send_offer(sockets[0], route) && receive_ready(sockets[0], ::getppid(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 1);
    ocudu_dapp_process_management_response_v1 response{};
    ocudu_dapp_process_management_request_v1  request{};
    request.operation = OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1;
    ok = ok && management_exchange(sockets[0], request, response) && response.status == OCUDU_DAPP_OK_V1 &&
         response.resulting_generation == 1 && response.count == 1 &&
         response.payload.entries[0].value.value.boolean == 1;

    request                     = {};
    request.operation           = OCUDU_DAPP_PROCESS_MANAGEMENT_SET_CONFIG_V1;
    request.expected_generation = 1;
    request.boundary_or_action  = OCUDU_DAPP_CONFIG_BOUNDARY_INVOCATION_V1;
    request.entry_count         = 1;
    std::strcpy(request.entries[0].key, "enabled");
    request.entries[0].value.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
    request.entries[0].value.value.boolean = false;
    ok = ok && management_exchange(sockets[0], request, response) && response.status == OCUDU_DAPP_OK_V1 &&
         response.resulting_generation == 2;

    request           = {};
    request.operation = OCUDU_DAPP_PROCESS_MANAGEMENT_GET_CONFIG_V1;
    ok = ok && management_exchange(sockets[0], request, response) && response.status == OCUDU_DAPP_OK_V1 &&
         response.resulting_generation == 2 && response.payload.entries[0].value.value.boolean == 0;

    const int model             = make_sealed_model();
    request                     = {};
    request.operation           = OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1;
    request.expected_generation = 1;
    request.boundary_or_action  = OCUDU_DAPP_MODEL_STAGE_V1;
    request.artifact_size       = 4;
    std::strcpy(request.model_id, "reference-model");
    std::strcpy(request.model_version, "1");
    ok = ok && model >= 0 && management_exchange(sockets[0], request, response, model) &&
         response.status == OCUDU_DAPP_OK_V1 && response.resulting_generation == 2;
    if (model >= 0)
      ::close(model);
    for (uint32_t action : {OCUDU_DAPP_MODEL_VALIDATE_V1, OCUDU_DAPP_MODEL_WARM_V1, OCUDU_DAPP_MODEL_ACTIVATE_V1}) {
      request                     = {};
      request.operation           = OCUDU_DAPP_PROCESS_MANAGEMENT_MODEL_V1;
      request.expected_generation = response.resulting_generation;
      request.boundary_or_action  = action;
      std::strcpy(request.model_id, "reference-model");
      std::strcpy(request.model_version, "1");
      ok = ok && management_exchange(sockets[0], request, response) && response.status == OCUDU_DAPP_OK_V1;
    }
    const bool success = ok && response.resulting_generation == 5;
    std::this_thread::sleep_for(150ms);
    _exit(success ? 0 : 2);
  }
  ::close(sockets[0]);
  instance management;
  auto     received = receive_route_set(sockets[1], sender, ::geteuid(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 100ms,
                                        {&management_api(), &management});
  if (!check(static_cast<bool>(received), received.detail.c_str()))
    return 1;
  if (!check(!received.route_set->wait(100ms), "management control appeared as a data publication") ||
      !check(received.route_set->control_healthy(), "valid management request made control unhealthy"))
    return 1;
  int status = 0;
  return check(::waitpid(sender, &status, 0) == sender && WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "management exchange failed")
             ? 0
             : 1;
}

int incomplete_route_set_test()
{
  int sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "incomplete-set socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "incomplete-set credential setup failed"))
    return 1;
  auto        first  = make_route();
  const pid_t sender = ::fork();
  if (!check(sender >= 0, "incomplete-set fork failed"))
    return 1;
  if (sender == 0) {
    ::close(sockets[1]);
    if (!send_offer(sockets[0], first, OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 0, 2))
      _exit(2);
    uint8_t ignored = 0;
    ::recv(sockets[0], &ignored, sizeof(ignored), 0);
    _exit(0);
  }
  ::close(sockets[0]);
  const auto received = receive_route_set(sockets[1], sender, ::geteuid(), OCUDU_DAPP_INTERFACE_SPECTRUM_V1, 20ms);
  ::close(sockets[1]);
  int child_status = 0;
  return check(::waitpid(sender, &child_status, 0) == sender && WIFEXITED(child_status) &&
                   WEXITSTATUS(child_status) == 0,
               "incomplete-set sender failed") &&
                 check(!received && received.detail == "route-set admission timed out",
                       "incomplete shard set was accepted")
             ? 0
             : 1;
}

bool receive_spectrum_result(int socket, pid_t worker, uint64_t route_id)
{
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + sizeof(ocudu_dapp_process_spectrum_result_v1)> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(ucred))>                                           control{};
  iovec  vector{data.data(), data.size()};
  msghdr message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(socket, &message, 0);
  ucred         credentials{};
  bool          have_credentials = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
        item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    }
  }
  ocudu_dapp_control_header_v1          header{};
  ocudu_dapp_process_spectrum_result_v1 result{};
  if (received == static_cast<ssize_t>(data.size())) {
    std::memcpy(&header, data.data(), sizeof(header));
    std::memcpy(&result, data.data() + sizeof(header), sizeof(result));
  }
  return received == static_cast<ssize_t>(data.size()) && (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0 &&
         have_credentials && credentials.pid == worker && credentials.uid == ::geteuid() &&
         header.magic == OCUDU_DAPP_CONTROL_MAGIC_V1 && header.version == OCUDU_DAPP_CONTROL_VERSION_V1 &&
         header.type == OCUDU_DAPP_CONTROL_SPECTRUM_RESULT_V1 && header.payload_size == sizeof(result) &&
         header.descriptor_count == 0 && header.reserved == 0 && header.sequence == 17 &&
         result.struct_size == sizeof(result) && result.schema_id == OCUDU_DAPP_CONTEXT_INTERFERENCE_MAP_V1 &&
         result.route_id == route_id && result.source_grid_sequence == 1 && result.source_plan_generation == 42 &&
         result.cell_id == 7 && result.confidence_q16 == (1U << 16U) && result.created.sfn == 10 &&
         result.created.slot == 2 && result.valid_from.slot == 3 && result.valid_until.slot == 6 &&
         result.first_prb == 0 && result.nof_prbs == 2 && result.prb_confidence[0] == 0 &&
         result.prb_confidence[1] == UINT8_MAX;
}

bool receive_srs_isac_result(int socket, pid_t worker, uint64_t route_id)
{
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + sizeof(ocudu_dapp_process_srs_isac_result_v1)> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(ucred))>                                           control{};
  iovec  vector{data.data(), data.size()};
  msghdr message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(socket, &message, 0);
  ucred         credentials{};
  bool          have_credentials = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
        item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    }
  }
  ocudu_dapp_control_header_v1          header{};
  ocudu_dapp_process_srs_isac_result_v1 result{};
  if (received == static_cast<ssize_t>(data.size())) {
    std::memcpy(&header, data.data(), sizeof(header));
    std::memcpy(&result, data.data() + sizeof(header), sizeof(result));
  }
  const auto& preview = result.preview;
  return received == static_cast<ssize_t>(data.size()) && (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0 &&
         have_credentials && credentials.pid == worker && credentials.uid == ::geteuid() &&
         header.magic == OCUDU_DAPP_CONTROL_MAGIC_V1 && header.version == OCUDU_DAPP_CONTROL_VERSION_V1 &&
         header.type == OCUDU_DAPP_CONTROL_SRS_ISAC_RESULT_V1 && header.payload_size == sizeof(result) &&
         header.descriptor_count == 0 && header.reserved == 0 && header.sequence == 17 &&
         result.struct_size == sizeof(result) && result.schema_id == OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 &&
         result.route_id == route_id && preview.struct_size == sizeof(preview) &&
         preview.abi_major == OCUDU_DAPP_ABI_MAJOR_V1 && preview.context.struct_size == sizeof(preview.context) &&
         preview.context.schema_id == OCUDU_DAPP_REPORT_SRS_CIR_PREVIEW_V1 && preview.context.schema_version == 1 &&
         preview.context.source_sequence == 1 && preview.context.cell_id == 7 && preview.context.created.sfn == 10 &&
         preview.context.created.slot == 2 && preview.source_srs_sequence == 1 && preview.srs_resource_id == 1 &&
         preview.srs_config_id == 1 && preview.tap_count == 24 && preview.peak_delay_bin == 0 &&
         std::isfinite(preview.mean_channel_power) && preview.mean_channel_power == 2.0F &&
         preview.noise_corrected_power == 2.0F && preview.peak_cir_power == 1.0F && preview.tap_power[0] == 1.0F &&
         std::all_of(preview.tap_power + preview.tap_count, preview.tap_power + OCUDU_DAPP_SRS_ISAC_MAX_CIR_TAPS_V1,
                     [](float value) { return value == 0.0F; });
}

bool receive_quiet_intent_result(int socket, pid_t worker, uint64_t route_id)
{
  std::array<uint8_t, sizeof(ocudu_dapp_control_header_v1) + sizeof(ocudu_dapp_process_quiet_intent_result_v1)> data{};
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(ucred))> control{};
  iovec                                                           vector{data.data(), data.size()};
  msghdr                                                          message{};
  message.msg_iov        = &vector;
  message.msg_iovlen     = 1;
  message.msg_control    = control.data();
  message.msg_controllen = control.size();
  const ssize_t received = ::recvmsg(socket, &message, 0);
  ucred         credentials{};
  bool          have_credentials = false;
  for (auto* item = CMSG_FIRSTHDR(&message); item != nullptr; item = CMSG_NXTHDR(&message, item)) {
    if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_CREDENTIALS &&
        item->cmsg_len == CMSG_LEN(sizeof(credentials))) {
      std::memcpy(&credentials, CMSG_DATA(item), sizeof(credentials));
      have_credentials = true;
    }
  }
  ocudu_dapp_control_header_v1              header{};
  ocudu_dapp_process_quiet_intent_result_v1 result{};
  if (received == static_cast<ssize_t>(data.size())) {
    std::memcpy(&header, data.data(), sizeof(header));
    std::memcpy(&result, data.data() + sizeof(header), sizeof(result));
  }
  const auto& intent = result.intent;
  return received == static_cast<ssize_t>(data.size()) && (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0 &&
         have_credentials && credentials.pid == worker && credentials.uid == ::geteuid() &&
         header.magic == OCUDU_DAPP_CONTROL_MAGIC_V1 && header.version == OCUDU_DAPP_CONTROL_VERSION_V1 &&
         header.type == OCUDU_DAPP_CONTROL_QUIET_INTENT_RESULT_V1 && header.payload_size == sizeof(result) &&
         header.descriptor_count == 0 && header.reserved == 0 && header.sequence == 17 &&
         result.struct_size == sizeof(result) &&
         result.schema_id == OCUDU_DAPP_SPECTRUM_OUTPUT_QUIET_PERIOD_INTENT_V1 && result.route_id == route_id &&
         result.source_grid_sequence == 1 && result.source_plan_generation == 42 && result.cell_id == 7 &&
         result.created.sfn == 10 && result.created.slot == 2 && intent.struct_size == sizeof(intent) &&
         intent.request_id[0] == 1 && intent.earliest_start.sfn == 10 && intent.earliest_start.slot == 6 &&
         intent.latest_start.slot == 6 && intent.duration_slots == 1 && intent.repeat_count == 1 &&
         intent.first_prb == 1 && intent.nof_prbs == 1 && intent.requested_level == OCUDU_DAPP_QUIET_NO_NEW_PUSCH_V1 &&
         intent.expiry_monotonic_ns != 0;
}

int worker_test(const char* worker_path, uint32_t interface_id, int expected_exit, bool sharded, bool observer)
{
  int sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0 &&
                 ::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "credential setup failed"))
    return 1;
  auto host         = make_route();
  auto second_shard = make_route();
  if (!check(host.ring >= 0 && host.data_export >= 0 && host.event >= 0 && host.completion >= 0, "host setup failed"))
    return 1;

  const pid_t worker = ::fork();
  if (!check(worker >= 0, "fork failed"))
    return 1;
  if (worker == 0) {
    ::close(sockets[0]);
    if (::dup2(sockets[1], 3) < 0 || ::fcntl(3, F_SETFD, 0) != 0)
      _exit(125);
    if (sockets[1] != 3)
      ::close(sockets[1]);
    ::execl(worker_path, worker_path, "--control-fd=3", "--once", nullptr);
    _exit(126);
  }
  ::close(sockets[1]);
  const uint16_t shard_count = sharded ? 2 : 1;
  if (!check(send_offer(sockets[0], host, interface_id, 0, shard_count), "route offer failed") ||
      (sharded &&
       !check(send_offer(sockets[0], second_shard, interface_id, 1, shard_count), "second route offer failed")))
    return 1;
  if (expected_exit == 0 &&
      !check(receive_ready(sockets[0], worker, interface_id, shard_count), "route-ready acknowledgement failed"))
    return 1;
  std::array<complex32, 24> payload{};
  if (interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1) {
    payload.fill({2, 0});
  } else {
    std::fill_n(payload.begin(), 12, complex32{2, 0});
  }
  auto& publication_shard = sharded ? second_shard : host;
  if (expected_exit == 0)
    publish_latest_batch(publication_shard, reinterpret_cast<const uint8_t*>(payload.data()), sizeof(payload),
                         interface_id);
  if (expected_exit == 0 && !observer && interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
      !check(receive_spectrum_result(sockets[0], worker, sharded ? 10 : 9), "spectrum result mismatch"))
    return 1;
  if (expected_exit == 0 && !observer && interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
      !check(receive_quiet_intent_result(sockets[0], worker, sharded ? 10 : 9), "quiet intent result mismatch"))
    return 1;
  if (expected_exit == 0 && interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 &&
      !check(receive_srs_isac_result(sockets[0], worker, sharded ? 10 : 9), "SRS-ISAC result mismatch"))
    return 1;
  int status = 0;
  if (!check(::waitpid(worker, &status, 0) == worker && WIFEXITED(status) && WEXITSTATUS(status) == expected_exit,
             "worker execution failed"))
    return 1;
  if (expected_exit == 0) {
    uint64_t completions = 0;
    if (!check(::read(publication_shard.completion, &completions, sizeof(completions)) == sizeof(completions) &&
                   completions == 2,
               "worker did not retire superseded publications"))
      return 1;
  }
  if (observer) {
    uint8_t unexpected{};
    if (!check(::recv(sockets[0], &unexpected, sizeof(unexpected), MSG_DONTWAIT) == 0,
               "observer worker emitted a control result"))
      return 1;
  }
  ::close(sockets[0]);
  return 0;
}

int bundle_worker_test(const char* worker_path)
{
  constexpr uint32_t future_dmrs_interface_id = 0x7f010002U;
  int                sockets[2];
  if (!check(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) == 0, "bundle-worker socketpair failed"))
    return 1;
  int enabled = 1;
  if (!check(::setsockopt(sockets[0], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0 &&
                 ::setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0,
             "bundle-worker credential setup failed"))
    return 1;
  auto        route  = make_route();
  const pid_t worker = ::fork();
  if (!check(worker >= 0, "bundle-worker fork failed"))
    return 1;
  if (worker == 0) {
    ::close(sockets[0]);
    if (::dup2(sockets[1], 3) < 0 || ::fcntl(3, F_SETFD, 0) != 0)
      _exit(125);
    if (sockets[1] != 3)
      ::close(sockets[1]);
    ::execl(worker_path, worker_path, "--control-fd=3", "--once", nullptr);
    _exit(126);
  }
  ::close(sockets[1]);
  ocudu_dapp_route_bundle_v1 bundle{};
  bundle.struct_size      = sizeof(bundle);
  bundle.interface_count  = 2;
  bundle.route_count      = 1;
  bundle.route_generation = 17;
  bundle.bundle_id        = 0x8877665544332211ULL;
  bundle.interfaces[0]    = {OCUDU_DAPP_INTERFACE_SRS_ISAC_V1, 1, OCUDU_DAPP_PROCESS_BINDING_REQUIRED_V1,
                             OCUDU_DAPP_PROCESS_BINDING_BOUND_V1};
  bundle.interfaces[1]    = {future_dmrs_interface_id, 0, OCUDU_DAPP_PROCESS_BINDING_OPTIONAL_V1,
                             OCUDU_DAPP_PROCESS_BINDING_UNAVAILABLE_V1};
  if (!check(send_bundle(sockets[0], bundle), "bundle-worker preamble failed") ||
      !check(send_offer(sockets[0], route, OCUDU_DAPP_INTERFACE_SRS_ISAC_V1), "bundle-worker route failed") ||
      !check(receive_bundle_ready(sockets[0], worker, bundle), "bundle-worker acknowledgement failed"))
    return 1;
  std::array<complex32, 24> payload{};
  std::fill_n(payload.begin(), 12, complex32{2, 0});
  publish(route, reinterpret_cast<const uint8_t*>(payload.data()), sizeof(payload), OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  if (!check(receive_srs_isac_result(sockets[0], worker, 9), "bundle-worker SRS result mismatch"))
    return 1;
  int status = 0;
  if (!check(::waitpid(worker, &status, 0) == worker && WIFEXITED(status) && WEXITSTATUS(status) == 0,
             "bundle-worker execution failed"))
    return 1;
  uint64_t completions = 0;
  if (!check(::read(route.completion, &completions, sizeof(completions)) == sizeof(completions) && completions == 1,
             "bundle-worker did not complete publication"))
    return 1;
  ::close(sockets[0]);
  return 0;
}

int restricted_policy_test()
{
  const pid_t child = ::fork();
  if (!check(child >= 0, "security-policy fork failed"))
    return 1;
  if (child == 0) {
    if (!apply_restricted_process_policy() || ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1 ||
        ::prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 2)
      _exit(10);
    char  name[] = "forbidden";
    char* arguments[]{name, nullptr};
    ::execv("/definitely-not-an-ocudu-worker", arguments);
    _exit(errno == EPERM ? 0 : 11);
  }
  int status = 0;
  return check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "restricted policy did not enter seccomp or block exec")
             ? 0
             : 1;
}

int main(int argc, char** argv)
{
  if (argc == 1)
    return direct_client_test();
  if (argc == 2 && std::strcmp(argv[1], "--route-set") == 0)
    return route_set_test();
  if (argc == 2 && std::strcmp(argv[1], "--route-bundle") == 0)
    return route_bundle_test();
  if (argc == 2 && std::strcmp(argv[1], "--parent-lifetime") == 0)
    return parent_lifetime_test();
  if (argc == 2 && std::strcmp(argv[1], "--parent-death-enforced") == 0)
    return parent_death_enforced_test();
  if (argc == 2 && std::strcmp(argv[1], "--duplicate-route-set") == 0)
    return duplicate_route_set_test();
  if (argc == 2 && std::strcmp(argv[1], "--heartbeat-route-set") == 0)
    return heartbeat_route_set_test();
  if (argc == 2 && std::strcmp(argv[1], "--management-route-set") == 0)
    return management_route_set_test();
  if (argc == 2 && std::strcmp(argv[1], "--incomplete-route-set") == 0)
    return incomplete_route_set_test();
  if (argc == 2 && std::strcmp(argv[1], "--decode-cbf16") == 0)
    return cbf16_decode_test();
  if (argc == 2 && std::strcmp(argv[1], "--decode-portable-host") == 0)
    return portable_host_decode_test();
  if (argc == 2 && std::strcmp(argv[1], "--decode-ul-plan") == 0)
    return ul_plan_decode_test();
  if (argc == 2 && std::strcmp(argv[1], "--restricted-policy") == 0)
    return restricted_policy_test();
  if (argc == 3 && std::strcmp(argv[1], "--bundle-worker") == 0)
    return bundle_worker_test(argv[2]);
  if (argc != 3 && argc != 4 && argc != 5)
    return 2;
  if (argc == 5 && std::strcmp(argv[4], "--sharded") != 0)
    return 2;
  const bool observer  = argc == 4 && std::strcmp(argv[3], "--observer") == 0;
  const int  exit_code = argc >= 4 && !observer ? static_cast<int>(std::strtol(argv[3], nullptr, 0)) : 0;
  return worker_test(argv[1], static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0)), exit_code, argc == 5, observer);
}
