// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/class_c_input.h"
#include "class-c-publication-v1_generated.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <algorithm>
#include <cmath>
#include <flatbuffers/verifier.h>
#include <limits>
#include <string_view>

namespace schema = Ocudu::Dapp::ClassC::V1;
using namespace ocudu::dapp::reference;

namespace {

constexpr std::string_view schema_id = "org.ocudu.dapp.class-c.publication";

bool copy_tensor(const schema::TensorDescriptor& descriptor, const uint8_t* tensor, std::size_t tensor_size,
                 strided_complex_view& output) noexcept
{
  const auto* dimensions = descriptor.dimensions();
  const auto* strides    = descriptor.byte_strides();
  uint32_t    element_type{};
  uint64_t    element_size{};
  if (descriptor.element_type() == schema::ElementType::ComplexFp32) {
    element_type = OCUDU_DAPP_ELEMENT_CF32_V1;
    element_size = 8;
  } else if (descriptor.element_type() == schema::ElementType::ComplexBf16) {
    element_type = OCUDU_DAPP_ELEMENT_CBF16_V1;
    element_size = 4;
  }
  if (tensor == nullptr || tensor_size == 0 || element_size == 0 || dimensions == nullptr || strides == nullptr ||
      dimensions->size() == 0 || dimensions->size() > OCUDU_DAPP_MAX_TENSOR_RANK_V1 ||
      dimensions->size() != strides->size()) {
    return false;
  }
  const bool host =
      (descriptor.domain() == schema::MemoryDomain::PosixShared || descriptor.domain() == schema::MemoryDomain::Host) &&
      descriptor.backend_id() == OCUDU_DAPP_BACKEND_HOST_V1 && descriptor.device_ordinal() == UINT16_MAX;
  const bool cuda = descriptor.domain() == schema::MemoryDomain::AcceleratorIpcExport &&
                    descriptor.backend_id() == OCUDU_DAPP_BACKEND_CUDA_V1 && descriptor.device_ordinal() != UINT16_MAX;
  if (!host && !cuda) {
    return false;
  }
  output                = {};
  output.data           = tensor;
  output.byte_length    = tensor_size;
  output.element_type   = element_type;
  output.memory_domain  = cuda                                                ? OCUDU_DAPP_MEM_ACCELERATOR_IPC_EXPORT_V1
                          : descriptor.domain() == schema::MemoryDomain::Host ? OCUDU_DAPP_MEM_HOST_V1
                                                                              : OCUDU_DAPP_MEM_POSIX_SHARED_V1;
  output.backend_id     = descriptor.backend_id();
  output.device_ordinal = cuda ? descriptor.device_ordinal() : UINT32_MAX;
  output.rank           = dimensions->size();
  uint64_t extent       = element_size;
  for (uint32_t axis = 0; axis != output.rank; ++axis) {
    output.dimensions[axis]   = (*dimensions)[axis];
    output.byte_strides[axis] = (*strides)[axis];
    if (output.dimensions[axis] == 0 || output.byte_strides[axis] == 0 ||
        output.dimensions[axis] - 1 > (std::numeric_limits<uint64_t>::max() - extent) / output.byte_strides[axis]) {
      return false;
    }
    extent += (output.dimensions[axis] - 1) * output.byte_strides[axis];
  }
  return extent <= tensor_size && extent <= descriptor.byte_length();
}

} // namespace

bool ocudu::dapp::reference::decode_class_c_input(const uint8_t* metadata, std::size_t metadata_size,
                                                  const uint8_t* tensor, std::size_t tensor_size,
                                                  uint32_t expected_interface_id, class_c_input& output) noexcept
{
  if (metadata == nullptr || metadata_size == 0 || metadata_size > 1024U * 1024U) {
    return false;
  }
  flatbuffers::Verifier verifier(metadata, metadata_size);
  if (!schema::VerifyPublicationEnvelopeBuffer(verifier)) {
    return false;
  }
  const auto* envelope = schema::GetPublicationEnvelope(metadata);
  const auto* context  = envelope->context();
  if (envelope->schema_id() == nullptr || envelope->schema_id()->string_view() != schema_id ||
      envelope->schema_version_major() != 1 || envelope->publication_sequence() == 0 || context == nullptr ||
      context->radio_time() == nullptr) {
    return false;
  }
  const auto* radio_time = context->radio_time();
  if (radio_time->numerology() > 4 || radio_time->sfn() >= 1024 ||
      radio_time->slot() >= (10U << radio_time->numerology())) {
    return false;
  }
  class_c_input decoded{};
  decoded.source_sequence       = envelope->publication_sequence();
  decoded.observation_time_ns   = context->observation_time_ns();
  decoded.cell_id               = context->cell_id();
  decoded.radio_time.sfn        = radio_time->sfn();
  decoded.radio_time.slot       = radio_time->slot();
  decoded.radio_time.numerology = radio_time->numerology();
  if (expected_interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
      envelope->publication_kind() == schema::PublicationKind::UlResourcePlan &&
      envelope->metadata_type() == schema::PublicationMetadata::UlResourcePlanMetadata) {
    const auto* item  = envelope->metadata_as_UlResourcePlanMetadata();
    const auto* spans = item != nullptr ? item->spans() : nullptr;
    if (item == nullptr || envelope->tensor() != nullptr || tensor != nullptr || tensor_size != 0 ||
        item->plan_generation() == 0 || item->plan_generation() != decoded.source_sequence || item->first_prb() != 0 ||
        item->prb_count() == 0 || item->prb_count() > OCUDU_DAPP_SPECTRUM_MAX_PRBS_V1 ||
        item->mask_granularity() != schema::GridMaskGranularity::None || spans == nullptr ||
        spans->size() > OCUDU_DAPP_UL_RESOURCE_PLAN_MAX_SPANS_V1) {
      return false;
    }
    decoded.kind            = class_c_input_kind::ul_resource_plan;
    decoded.plan_generation = item->plan_generation();
    decoded.nof_plan_spans  = spans->size();
    decoded.cell_nof_prbs   = item->prb_count();
    decoded.plan_complete   = item->complete();
    for (uint32_t index = 0; index != decoded.nof_plan_spans; ++index) {
      const auto* source = spans->Get(index);
      if (source == nullptr || source->symbol_count() == 0 || source->symbol_start() + source->symbol_count() > 14 ||
          source->prb_count() == 0 ||
          static_cast<uint32_t>(source->prb_start()) + source->prb_count() > decoded.cell_nof_prbs ||
          source->use_category() > schema::UlUseCategory::ReservedQuiet) {
        return false;
      }
      auto& destination            = decoded.plan_spans[index];
      destination.struct_size      = sizeof(destination);
      destination.symbol_start     = source->symbol_start();
      destination.symbol_count     = source->symbol_count();
      destination.first_prb        = source->prb_start();
      destination.nof_prbs         = source->prb_count();
      destination.use_category     = static_cast<uint8_t>(source->use_category());
      destination.dmrs_symbol_mask = source->dmrs_symbol_mask();
      destination.allocation_id    = source->allocation_id();
    }
  } else if (envelope->tensor() == nullptr || !copy_tensor(*envelope->tensor(), tensor, tensor_size, decoded.tensor)) {
    return false;
  } else if (expected_interface_id == OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
             envelope->publication_kind() == schema::PublicationKind::Spectrum &&
             envelope->metadata_type() == schema::PublicationMetadata::SpectrumMetadata) {
    const auto* item = envelope->metadata_as_SpectrumMetadata();
    if (item == nullptr || !std::isfinite(item->scale()) || item->scale() <= 0.0F || item->rx_ports() == 0 ||
        item->symbols() == 0 || item->symbols() > 14 || item->subcarriers() == 0 || decoded.tensor.rank != 3 ||
        item->rx_ports() != decoded.tensor.dimensions[0] || item->symbols() != decoded.tensor.dimensions[1] ||
        item->subcarriers() != decoded.tensor.dimensions[2]) {
      return false;
    }
    decoded.kind            = class_c_input_kind::spectrum;
    decoded.amplitude_scale = item->scale();
    const auto* annotations = item->occupancy_annotations();
    if (item->plan_generation() != 0) {
      const uint32_t nof_prbs = item->subcarriers() / 12;
      if (!item->occupancy_complete() || item->occupancy_granularity() != schema::GridMaskGranularity::SymbolPrb ||
          annotations == nullptr || item->subcarriers() % 12 != 0 || item->symbols() != decoded.tensor.dimensions[1] ||
          nof_prbs != decoded.tensor.dimensions[2] / 12 ||
          annotations->size() != static_cast<uint64_t>(item->symbols()) * nof_prbs ||
          !std::all_of(annotations->begin(), annotations->end(),
                       [](uint8_t value) { return value <= OCUDU_DAPP_UL_USE_RESERVED_QUIET_V1; })) {
        return false;
      }
      decoded.expected_ul_use = {annotations->data(), item->symbols(), nof_prbs, item->plan_generation()};
    } else if (item->occupancy_complete() || item->occupancy_granularity() != schema::GridMaskGranularity::None ||
               (annotations != nullptr && annotations->size() != 0)) {
      return false;
    }
  } else if (expected_interface_id == OCUDU_DAPP_INTERFACE_SRS_ISAC_V1 &&
             envelope->publication_kind() == schema::PublicationKind::SrsIsac &&
             envelope->metadata_type() == schema::PublicationMetadata::SrsIsacMetadata) {
    const auto* item = envelope->metadata_as_SrsIsacMetadata();
    if (item == nullptr || !std::isfinite(item->noise_variance()) || item->noise_variance() < 0.0F ||
        !std::isfinite(item->rsrp()) || !std::isfinite(item->epre()) || item->rx_ports() == 0 ||
        item->tx_ports() == 0 || item->frequency_points() == 0 ||
        (item->comb_size() != 2 && item->comb_size() != 4 && item->comb_size() != 8) ||
        item->comb_offset() >= item->comb_size() || item->subcarrier_spacing_hz() == 0 || decoded.tensor.rank != 3 ||
        decoded.tensor.dimensions[0] != item->rx_ports() || decoded.tensor.dimensions[1] != item->tx_ports() ||
        decoded.tensor.dimensions[2] != item->frequency_points()) {
      return false;
    }
    decoded.kind            = class_c_input_kind::srs_isac;
    decoded.srs_resource_id = item->srs_resource_id();
    decoded.srs_config_id   = item->srs_config_id();
    decoded.noise_variance  = item->noise_variance();
  } else {
    return false;
  }
  if (envelope->tensor() != nullptr) {
    decoded.lease_expiry_ns = envelope->tensor()->lease_expiry_ns();
  }
  output = decoded;
  return true;
}

bool ocudu::dapp::reference::class_c_input_expired(const class_c_input& input, uint64_t monotonic_time_ns) noexcept
{
  if (input.kind == class_c_input_kind::ul_resource_plan) {
    return false;
  }
  return monotonic_time_ns == 0 || input.observation_time_ns == 0 ||
         input.lease_expiry_ns <= input.observation_time_ns || monotonic_time_ns >= input.lease_expiry_ns;
}
