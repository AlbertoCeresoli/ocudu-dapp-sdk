// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/portable_e3_consumer.h"
#include "ocudu/dapp/use_cases/v1/spectrum.h"
#include "ocudu/dapp/use_cases/v1/srs_isac.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <new>
#include <unistd.h>

using namespace ocudu::dapp;
using namespace ocudu::dapp::reference;

namespace {

management::identifier transaction_id() noexcept
{
  static std::atomic<uint64_t> sequence{};
  management::identifier       result;
  const uint64_t               now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
  const uint64_t nonce = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  for (unsigned index = 0; index != 8; ++index) {
    result.bytes[index]     = static_cast<uint8_t>(now >> (index * 8U));
    result.bytes[index + 8] = static_cast<uint8_t>((nonce ^ static_cast<uint64_t>(::getpid())) >> (index * 8U));
  }
  return result;
}

bool successful(const e3::codec_result<e3::management_response>& response, std::string& detail) noexcept
{
  if (!response || !response.value) {
    detail = response.detail.empty() ? "portable E3 request failed" : response.detail;
    return false;
  }
  if (response.value->code != management::result_code::ok) {
    detail =
        response.value->error_text.empty() ? management::to_string(response.value->code) : response.value->error_text;
    return false;
  }
  return true;
}

} // namespace

struct portable_e3_consumer::implementation {
  explicit implementation(portable_e3_consumer_config config_) : config(std::move(config_)), client(config.socket_path)
  {
  }

  void reset_assembly() noexcept
  {
    partial.reset();
    next_chunk  = 0;
    chunk_count = 0;
    tensor_size = 0;
  }

  void reset() noexcept
  {
    reset_assembly();
    pending.clear();
  }

  portable_e3_consumer_config                config;
  e3::local_management_client                client;
  std::optional<portable_e3_object>          partial;
  std::deque<e3::portable_publication_chunk> pending;
  uint32_t                                   next_chunk{};
  uint32_t                                   chunk_count{};
  uint64_t                                   tensor_size{};
};

std::pair<std::unique_ptr<portable_e3_consumer>, std::string>
portable_e3_consumer::create(portable_e3_consumer_config config) noexcept
{
  if (config.socket_path.empty() || config.subscription_id.is_zero() ||
      (config.interface_id != OCUDU_DAPP_INTERFACE_SPECTRUM_V1 &&
       config.interface_id != OCUDU_DAPP_INTERFACE_SRS_ISAC_V1) ||
      (config.cell_id && *config.cell_id > 65535) || config.sample_every_n == 0 || config.sample_every_n > 65535 ||
      config.queue_depth_chunks == 0 || config.queue_depth_chunks > 1024 || config.maximum_chunk_bytes < 1024 ||
      config.maximum_chunk_bytes > 65536 || config.lease_us == 0 || config.lease_us > 60'000'000 ||
      config.maximum_tensor_bytes == 0 || config.maximum_tensor_bytes > 64ULL * 1024 * 1024 ||
      config.timeout.count() <= 0 || config.timeout > std::chrono::minutes(1)) {
    return std::make_pair(std::unique_ptr<portable_e3_consumer>{},
                          std::string{"invalid portable E3 consumer configuration"});
  }
  try {
    return std::make_pair(std::unique_ptr<portable_e3_consumer>(
                              new portable_e3_consumer(std::make_unique<implementation>(std::move(config)))),
                          std::string{});
  } catch (const std::bad_alloc&) {
    return std::make_pair(std::unique_ptr<portable_e3_consumer>{},
                          std::string{"unable to allocate portable E3 consumer"});
  } catch (...) {
    return std::make_pair(std::unique_ptr<portable_e3_consumer>{},
                          std::string{"unknown portable E3 consumer construction failure"});
  }
}

portable_e3_consumer::portable_e3_consumer(std::unique_ptr<implementation> state_) : state(std::move(state_)) {}
portable_e3_consumer::~portable_e3_consumer() = default;

bool portable_e3_consumer::subscribe(std::string& detail) noexcept
{
  e3::data_subscription_control request;
  request.header.transaction_id = transaction_id();
  request.action                = e3::data_subscription_action::create;
  request.subscription_id       = state->config.subscription_id;
  request.interface_id          = state->config.interface_id;
  request.cell_id               = state->config.cell_id;
  request.sample_every_n        = state->config.sample_every_n;
  request.queue_depth_chunks    = state->config.queue_depth_chunks;
  request.maximum_chunk_bytes   = state->config.maximum_chunk_bytes;
  request.lease_us              = state->config.lease_us;
  const auto response           = state->client.call(request, state->config.timeout);
  if (!successful(response, detail)) {
    return false;
  }
  if (!std::holds_alternative<e3::data_subscription_result>(response.value->payload)) {
    detail = "portable E3 subscribe response has no subscription payload";
    return false;
  }
  state->reset();
  detail.clear();
  return true;
}

portable_e3_poll_result portable_e3_consumer::poll() noexcept
{
  try {
    if (state->pending.empty()) {
      e3::data_poll_query request;
      request.header.transaction_id = transaction_id();
      request.subscription_id       = state->config.subscription_id;
      request.maximum_chunks        = 8;
      auto        response          = state->client.call(request, state->config.timeout);
      std::string detail;
      if (!successful(response, detail)) {
        return {portable_e3_poll_status::error, {}, std::move(detail)};
      }
      if (!std::holds_alternative<e3::data_publication_result>(response.value->payload)) {
        return {portable_e3_poll_status::error, {}, "portable E3 poll response has no publication payload"};
      }
      auto& publications = std::get<e3::data_publication_result>(response.value->payload);
      for (auto& chunk : publications.chunks) {
        state->pending.push_back(std::move(chunk));
      }
    }
    while (!state->pending.empty()) {
      auto chunk = std::move(state->pending.front());
      state->pending.pop_front();
      const bool first = chunk.chunk_index == 0;
      if (chunk.subscription_id != state->config.subscription_id || chunk.interface_id != state->config.interface_id ||
          chunk.chunk_count == 0 || chunk.chunk_index >= chunk.chunk_count || chunk.tensor_size == 0 ||
          chunk.tensor_size > state->config.maximum_tensor_bytes || chunk.tensor_chunk.empty() ||
          (first && chunk.metadata.empty()) || (!first && !chunk.metadata.empty())) {
        state->reset();
        return {portable_e3_poll_status::error, {}, "invalid portable E3 publication chunk"};
      }
      if (first) {
        state->partial.emplace();
        state->partial->interface_id         = chunk.interface_id;
        state->partial->cell_id              = chunk.cell_id;
        state->partial->publication_sequence = chunk.publication_sequence;
        state->partial->route_epoch          = chunk.route_epoch;
        state->partial->dropped_objects      = chunk.dropped_objects;
        state->partial->metadata             = chunk.metadata;
        state->partial->tensor.clear();
        state->partial->tensor.reserve(static_cast<std::size_t>(chunk.tensor_size));
        state->next_chunk  = 0;
        state->chunk_count = chunk.chunk_count;
        state->tensor_size = chunk.tensor_size;
      }
      if (!state->partial || chunk.chunk_index != state->next_chunk || chunk.chunk_count != state->chunk_count ||
          chunk.cell_id != state->partial->cell_id ||
          chunk.publication_sequence != state->partial->publication_sequence ||
          chunk.route_epoch != state->partial->route_epoch || chunk.tensor_size != state->tensor_size ||
          chunk.tensor_chunk.size() > state->tensor_size ||
          state->partial->tensor.size() > state->tensor_size - chunk.tensor_chunk.size()) {
        state->reset();
        return {portable_e3_poll_status::error, {}, "noncontiguous portable E3 publication chunks"};
      }
      state->partial->tensor.insert(state->partial->tensor.end(), chunk.tensor_chunk.begin(), chunk.tensor_chunk.end());
      ++state->next_chunk;
      if (state->next_chunk == state->chunk_count) {
        if (state->partial->tensor.size() != state->tensor_size) {
          state->reset();
          return {portable_e3_poll_status::error, {}, "portable E3 tensor length mismatch"};
        }
        portable_e3_object complete = std::move(*state->partial);
        state->reset_assembly();
        return {portable_e3_poll_status::object, std::move(complete), {}};
      }
    }
    return {portable_e3_poll_status::no_data, {}, {}};
  } catch (const std::bad_alloc&) {
    state->reset();
    return {portable_e3_poll_status::error, {}, "portable E3 consumer allocation failed"};
  } catch (...) {
    state->reset();
    return {portable_e3_poll_status::error, {}, "unknown portable E3 consumer failure"};
  }
}

bool portable_e3_consumer::unsubscribe(std::string& detail) noexcept
{
  e3::data_subscription_control request;
  request.header.transaction_id = transaction_id();
  request.action                = e3::data_subscription_action::remove;
  request.subscription_id       = state->config.subscription_id;
  const auto response           = state->client.call(request, state->config.timeout);
  state->reset();
  return successful(response, detail);
}
