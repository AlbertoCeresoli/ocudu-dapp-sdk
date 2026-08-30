// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Implementation of the shared package-config management core (Class A/B/C).
// Public entry: make_config_management. Prefer including package_config.h in
// vendor code. Docs: docs/package_config.md · examples/vendor_min.

#include "ocudu_dapp_sdk/class_c_config.h"
#include <cstring>
#include <mutex>
#include <vector>

namespace ocudu::dapp::class_c {
namespace {

using reference::instance;

constexpr std::size_t max_schemas = 16;

bool key_matches(const config_key& key, const char* name) noexcept
{
  if (name == nullptr || key.key == nullptr) {
    return false;
  }
  if (std::strcmp(key.key, name) == 0) {
    return true;
  }
  if (key.aliases == nullptr) {
    return false;
  }
  for (const char* const* alias = key.aliases; *alias != nullptr; ++alias) {
    if (std::strcmp(*alias, name) == 0) {
      return true;
    }
  }
  return false;
}

int find_key(const config_schema& schema, const char* name) noexcept
{
  for (uint32_t i = 0; i != schema.nof_keys; ++i) {
    if (key_matches(schema.keys[i], name)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

struct registered_schema {
  config_schema            schema{};
  ocudu_dapp_management_v1 api{};
};

registered_schema g_schemas[max_schemas]{};
std::size_t       g_schema_count = 0;
std::mutex        g_schema_mutex;

ocudu_dapp_status_v1 describe_impl(const config_schema& schema, ocudu_dapp_config_descriptor_v1* descriptors,
                                   uint32_t capacity, uint32_t* count) noexcept
{
  if (count == nullptr || (capacity != 0 && descriptors == nullptr) || schema.keys == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  *count = schema.nof_keys;
  if (capacity == 0) {
    return OCUDU_DAPP_OK_V1;
  }
  const uint32_t n = capacity < schema.nof_keys ? capacity : schema.nof_keys;
  for (uint32_t i = 0; i != n; ++i) {
    const auto& key              = schema.keys[i];
    descriptors[i]               = {};
    descriptors[i].struct_size   = sizeof(descriptors[i]);
    descriptors[i].key           = key.key;
    descriptors[i].description   = key.description;
    descriptors[i].value_kind    = key.value_kind;
    descriptors[i].flags         = key.flags & ~config_flag_instance_enabled;
    descriptors[i].default_value = key.default_value;
    descriptors[i].minimum_value = key.minimum_value;
    descriptors[i].maximum_value = key.maximum_value;
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 get_impl(const config_schema& schema, void* context, ocudu_dapp_config_entry_v1* entries,
                              uint32_t capacity, uint32_t* count, uint64_t* generation) noexcept
{
  if (context == nullptr || count == nullptr || generation == nullptr || (capacity != 0 && entries == nullptr) ||
      schema.keys == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto&                       state = *static_cast<instance*>(context);
  std::lock_guard<std::mutex> lock(state.management_mutex);
  *count      = schema.nof_keys;
  *generation = state.config_generation;
  if (capacity == 0) {
    return OCUDU_DAPP_OK_V1;
  }
  const uint32_t n = capacity < schema.nof_keys ? capacity : schema.nof_keys;
  for (uint32_t i = 0; i != n; ++i) {
    entries[i]      = {};
    entries[i].key  = schema.keys[i].key;
    const auto& key = schema.keys[i];
    if ((key.flags & config_flag_instance_enabled) != 0) {
      entries[i].value = value_bool(state.enabled.load(std::memory_order_acquire));
      continue;
    }
    if (key.get == nullptr) {
      return OCUDU_DAPP_INTERNAL_ERROR_V1;
    }
    const auto status = key.get(context, &entries[i].value);
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
  }
  return OCUDU_DAPP_OK_V1;
}

ocudu_dapp_status_v1 set_impl(const config_schema& schema, void* context, const ocudu_dapp_config_update_v1* update,
                              uint64_t* generation) noexcept
{
  if (context == nullptr || update == nullptr || generation == nullptr || update->entries == nullptr ||
      update->nof_entries == 0 || schema.keys == nullptr) {
    return OCUDU_DAPP_INVALID_INPUT_V1;
  }
  auto&                       state = *static_cast<instance*>(context);
  std::lock_guard<std::mutex> lock(state.management_mutex);
  if (update->expected_generation != state.config_generation) {
    *generation = state.config_generation;
    return OCUDU_DAPP_CONFLICT_V1;
  }

  std::vector<unsigned char> storage;
  void*                      scratch = nullptr;
  if (schema.scratch_bytes != 0) {
    try {
      storage.resize(schema.scratch_bytes);
    } catch (...) {
      return OCUDU_DAPP_RESOURCE_V1;
    }
    scratch = storage.data();
  }

  if (schema.begin_update != nullptr) {
    schema.begin_update(context, scratch);
  }

  bool have_enabled = false;
  bool enabled_val  = false;

  for (uint32_t i = 0; i != update->nof_entries; ++i) {
    const auto& entry = update->entries[i];
    if (entry.key == nullptr) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    const int index = find_key(schema, entry.key);
    if (index < 0) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    const auto& key = schema.keys[static_cast<uint32_t>(index)];
    if ((key.flags & OCUDU_DAPP_CONFIG_READ_ONLY_V1) != 0) {
      return OCUDU_DAPP_INVALID_INPUT_V1;
    }
    if ((key.flags & config_flag_instance_enabled) != 0) {
      if (!parse_bool(entry.value, enabled_val)) {
        return OCUDU_DAPP_INVALID_INPUT_V1;
      }
      have_enabled = true;
      continue;
    }
    if (key.stage == nullptr) {
      return OCUDU_DAPP_INTERNAL_ERROR_V1;
    }
    const auto status = key.stage(context, &entry.value, scratch);
    if (status != OCUDU_DAPP_OK_V1) {
      return status;
    }
  }

  if (schema.validate != nullptr) {
    const auto status = schema.validate(context, scratch);
    if (status != OCUDU_DAPP_OK_V1) {
      *generation = state.config_generation;
      return status;
    }
  }

  if (!update->dry_run) {
    if (have_enabled) {
      state.enabled.store(enabled_val, std::memory_order_release);
    }
    if (schema.commit != nullptr) {
      const auto status = schema.commit(context, scratch);
      if (status != OCUDU_DAPP_OK_V1) {
        *generation = state.config_generation;
        return status;
      }
    }
    ++state.config_generation;
  }
  *generation = state.config_generation;
  return OCUDU_DAPP_OK_V1;
}

#define OCUDU_CLASS_C_TRAMPOLINES(N)                                                                                   \
  ocudu_dapp_status_v1 describe_##N(void*, ocudu_dapp_config_descriptor_v1* descriptors, uint32_t capacity,            \
                                    uint32_t* count) noexcept                                                          \
  {                                                                                                                    \
    return describe_impl(g_schemas[N].schema, descriptors, capacity, count);                                           \
  }                                                                                                                    \
  ocudu_dapp_status_v1 get_##N(void* instance, ocudu_dapp_config_entry_v1* entries, uint32_t capacity,                 \
                               uint32_t* count, uint64_t* generation) noexcept                                         \
  {                                                                                                                    \
    return get_impl(g_schemas[N].schema, instance, entries, capacity, count, generation);                              \
  }                                                                                                                    \
  ocudu_dapp_status_v1 set_##N(void* instance, const ocudu_dapp_config_update_v1* update,                              \
                               uint64_t* generation) noexcept                                                          \
  {                                                                                                                    \
    return set_impl(g_schemas[N].schema, instance, update, generation);                                                \
  }

OCUDU_CLASS_C_TRAMPOLINES(0)
OCUDU_CLASS_C_TRAMPOLINES(1)
OCUDU_CLASS_C_TRAMPOLINES(2)
OCUDU_CLASS_C_TRAMPOLINES(3)
OCUDU_CLASS_C_TRAMPOLINES(4)
OCUDU_CLASS_C_TRAMPOLINES(5)
OCUDU_CLASS_C_TRAMPOLINES(6)
OCUDU_CLASS_C_TRAMPOLINES(7)
OCUDU_CLASS_C_TRAMPOLINES(8)
OCUDU_CLASS_C_TRAMPOLINES(9)
OCUDU_CLASS_C_TRAMPOLINES(10)
OCUDU_CLASS_C_TRAMPOLINES(11)
OCUDU_CLASS_C_TRAMPOLINES(12)
OCUDU_CLASS_C_TRAMPOLINES(13)
OCUDU_CLASS_C_TRAMPOLINES(14)
OCUDU_CLASS_C_TRAMPOLINES(15)

struct trampoline_set {
  ocudu_dapp_status_v1 (*describe)(void*, ocudu_dapp_config_descriptor_v1*, uint32_t, uint32_t*);
  ocudu_dapp_status_v1 (*get)(void*, ocudu_dapp_config_entry_v1*, uint32_t, uint32_t*, uint64_t*);
  ocudu_dapp_status_v1 (*set)(void*, const ocudu_dapp_config_update_v1*, uint64_t*);
};

const trampoline_set k_trampolines[max_schemas] = {
    {describe_0, get_0, set_0},    {describe_1, get_1, set_1},    {describe_2, get_2, set_2},
    {describe_3, get_3, set_3},    {describe_4, get_4, set_4},    {describe_5, get_5, set_5},
    {describe_6, get_6, set_6},    {describe_7, get_7, set_7},    {describe_8, get_8, set_8},
    {describe_9, get_9, set_9},    {describe_10, get_10, set_10}, {describe_11, get_11, set_11},
    {describe_12, get_12, set_12}, {describe_13, get_13, set_13}, {describe_14, get_14, set_14},
    {describe_15, get_15, set_15},
};

} // namespace

ocudu_dapp_value_v1 value_bool(bool value) noexcept
{
  ocudu_dapp_value_v1 result{};
  result.kind          = OCUDU_DAPP_VALUE_BOOL_V1;
  result.value.boolean = value ? 1 : 0;
  return result;
}

ocudu_dapp_value_v1 value_double(double value) noexcept
{
  ocudu_dapp_value_v1 result{};
  result.kind               = OCUDU_DAPP_VALUE_DOUBLE_V1;
  result.value.double_value = value;
  return result;
}

ocudu_dapp_value_v1 value_string(const char* data, std::size_t size) noexcept
{
  ocudu_dapp_value_v1 result{};
  result.kind            = OCUDU_DAPP_VALUE_STRING_V1;
  result.value.blob.data = data;
  result.value.blob.size = size;
  return result;
}

ocudu_dapp_value_v1 value_string(const char* c_str) noexcept
{
  if (c_str == nullptr) {
    return value_string(static_cast<const char*>(nullptr), 0);
  }
  return value_string(c_str, std::strlen(c_str));
}

bool parse_bool(const ocudu_dapp_value_v1& value, bool& out) noexcept
{
  if (value.kind != OCUDU_DAPP_VALUE_BOOL_V1 || value.value.boolean > 1) {
    return false;
  }
  out = value.value.boolean != 0;
  return true;
}

bool parse_double(const ocudu_dapp_value_v1& value, double& out) noexcept
{
  if (value.kind == OCUDU_DAPP_VALUE_DOUBLE_V1) {
    out = value.value.double_value;
    return true;
  }
  if (value.kind == OCUDU_DAPP_VALUE_UINT64_V1) {
    out = static_cast<double>(value.value.uint64_value);
    return true;
  }
  if (value.kind == OCUDU_DAPP_VALUE_INT64_V1) {
    out = static_cast<double>(value.value.int64_value);
    return true;
  }
  return false;
}

bool parse_string(const ocudu_dapp_value_v1& value, char* out, std::size_t capacity, std::size_t* out_size) noexcept
{
  if (value.kind != OCUDU_DAPP_VALUE_STRING_V1 || out == nullptr || capacity == 0) {
    return false;
  }
  const auto size = static_cast<std::size_t>(value.value.blob.size);
  if (value.value.blob.data == nullptr && size != 0) {
    return false;
  }
  if (size >= capacity) {
    return false;
  }
  if (size != 0) {
    std::memcpy(out, value.value.blob.data, size);
  }
  out[size] = '\0';
  if (out_size != nullptr) {
    *out_size = size;
  }
  return true;
}

const ocudu_dapp_management_v1& make_config_management(const ocudu_dapp_management_v1& base,
                                                       const config_schema&            schema) noexcept
{
  std::lock_guard<std::mutex> lock(g_schema_mutex);
  if (g_schema_count >= max_schemas) {
    return base;
  }
  const std::size_t index              = g_schema_count++;
  g_schemas[index].schema              = schema;
  g_schemas[index].api                 = base;
  g_schemas[index].api.describe_config = k_trampolines[index].describe;
  g_schemas[index].api.get_config      = k_trampolines[index].get;
  g_schemas[index].api.set_config      = k_trampolines[index].set;
  return g_schemas[index].api;
}

const ocudu_dapp_management_v1& make_config_management(const config_schema& schema) noexcept
{
  return make_config_management(reference::management_api(), schema);
}

} // namespace ocudu::dapp::class_c
