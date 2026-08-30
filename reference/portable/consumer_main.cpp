// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/class_c_algorithms.h"
#include "ocudu_dapp_sdk/class_c_input.h"
#include "ocudu_dapp_sdk/portable_e3_consumer.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>

#ifndef OCUDU_REFERENCE_PORTABLE_INTERFACE
#error "OCUDU_REFERENCE_PORTABLE_INTERFACE is required"
#endif

using namespace ocudu::dapp;
using namespace ocudu::dapp::reference;

namespace {

std::atomic<bool> running{true};

void stop(int) noexcept
{
  running.store(false, std::memory_order_release);
}

bool parse_identifier(std::string_view text, management::identifier& output) noexcept
{
  output = {};
  unsigned    byte{};
  std::size_t index{};
  for (char value : text) {
    if (value == '-') {
      continue;
    }
    const unsigned digit = value >= '0' && value <= '9'   ? static_cast<unsigned>(value - '0')
                           : value >= 'a' && value <= 'f' ? static_cast<unsigned>(value - 'a' + 10)
                           : value >= 'A' && value <= 'F' ? static_cast<unsigned>(value - 'A' + 10)
                                                          : 16;
    if (digit == 16 || index >= 32) {
      return false;
    }
    if ((index & 1U) == 0) {
      byte = digit << 4U;
    } else {
      output.bytes[index / 2] = static_cast<uint8_t>(byte | digit);
    }
    ++index;
  }
  return index == 32 && !output.is_zero();
}

template <typename T>
bool parse_unsigned(std::string_view text, T& output) noexcept
{
  uint64_t   value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > std::numeric_limits<T>::max()) {
    return false;
  }
  output = static_cast<T>(value);
  return true;
}

bool process(const portable_e3_object& object) noexcept
{
  class_c_input input;
  if (!decode_class_c_input(object.metadata.data(), object.metadata.size(), object.tensor.data(), object.tensor.size(),
                            OCUDU_REFERENCE_PORTABLE_INTERFACE, input)) {
    std::fprintf(stderr, "invalid Class-C metadata/tensor publication\n");
    return false;
  }
#if OCUDU_REFERENCE_PORTABLE_INTERFACE == OCUDU_DAPP_INTERFACE_SPECTRUM_V1
  spectrum_radiometer_result result;
  if (input.kind != class_c_input_kind::spectrum ||
      !run_spectrum_radiometer(input.tensor, input.amplitude_scale, 1.0, result)) {
    return false;
  }
  std::printf("spectrum sequence=%llu cell=%u samples=%llu mean-power=%.6f occupied=%.6f dropped=%llu\n",
              static_cast<unsigned long long>(object.publication_sequence), object.cell_id,
              static_cast<unsigned long long>(result.samples), result.mean_power, result.occupied_fraction,
              static_cast<unsigned long long>(object.dropped_objects));
#else
  srs_cir_analysis result;
  if (input.kind != class_c_input_kind::srs_isac ||
      !estimate_srs_cir_placeholder(input.tensor, input.noise_variance, result)) {
    return false;
  }
  std::printf("srs-isac sequence=%llu cell=%u resource=%u taps=%u peak-bin=%u peak-power=%.6f dropped=%llu\n",
              static_cast<unsigned long long>(object.publication_sequence), object.cell_id, input.srs_resource_id,
              result.tap_count, result.peak_delay_bin, result.peak_cir_power,
              static_cast<unsigned long long>(object.dropped_objects));
#endif
  return true;
}

} // namespace

int main(int argc, char** argv)
{
  portable_e3_consumer_config config;
  config.interface_id = OCUDU_REFERENCE_PORTABLE_INTERFACE;
  uint32_t poll_ms    = 10;
  uint64_t maximum_objects{};
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (index + 1 >= argc) {
      std::fprintf(stderr, "option %s needs a value\n", argv[index]);
      return 2;
    }
    const std::string_view value(argv[++index]);
    if (option == "--socket") {
      config.socket_path = value;
    } else if (option == "--subscription") {
      if (!parse_identifier(value, config.subscription_id)) {
        std::fprintf(stderr, "invalid subscription ID\n");
        return 2;
      }
    } else if (option == "--cell") {
      uint32_t cell{};
      if (!parse_unsigned(value, cell) || cell > 65535) {
        return 2;
      }
      config.cell_id = cell;
    } else if (option == "--sample-every") {
      if (!parse_unsigned(value, config.sample_every_n)) {
        return 2;
      }
    } else if (option == "--poll-ms") {
      if (!parse_unsigned(value, poll_ms) || poll_ms == 0 || poll_ms > 60000) {
        return 2;
      }
    } else if (option == "--maximum-objects") {
      if (!parse_unsigned(value, maximum_objects)) {
        return 2;
      }
    } else {
      std::fprintf(stderr, "unknown option %s\n", argv[index - 1]);
      return 2;
    }
  }
  auto [consumer, detail] = portable_e3_consumer::create(std::move(config));
  if (!consumer || !consumer->subscribe(detail)) {
    std::fprintf(stderr, "%s\n", detail.c_str());
    return 1;
  }
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  bool     ok = true;
  uint64_t processed{};
  while (running.load(std::memory_order_acquire)) {
    auto result = consumer->poll();
    if (result.status == portable_e3_poll_status::error) {
      std::fprintf(stderr, "%s\n", result.detail.c_str());
      ok = false;
      break;
    }
    if (result.object && !process(*result.object)) {
      ok = false;
      break;
    }
    if (result.object && maximum_objects != 0 && ++processed >= maximum_objects) {
      break;
    }
    if (result.status == portable_e3_poll_status::no_data) {
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
  }
  if (!consumer->unsubscribe(detail)) {
    std::fprintf(stderr, "%s\n", detail.c_str());
    ok = false;
  }
  return ok ? 0 : 1;
}
