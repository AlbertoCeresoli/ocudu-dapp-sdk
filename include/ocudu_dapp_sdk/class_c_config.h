// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Shared package-config management skeleton (Class A / B / C).
//
// Canonical include for authors: package_config.h (namespace package_config).
// This header remains the implementation surface; both names are supported.
//
// Vendors declare a static key table + stage/validate/commit hooks. This
// library owns describe/get/set dispatch, generation conflicts, dry-run,
// value codecs, and the instance "enabled" flag. Use with module_support's
// reference::instance (management_mutex + config_generation).
//
// Multi-vendor rule: do not re-implement value codecs or describe/get/set
// loops in package TUs — extend the schema only.
//
// Docs: docs/package_config.md · examples/vendor_min · tests/package_config_test.cpp

#pragma once

#include "ocudu/dapp/abi/v1/management.h"
#include "ocudu_dapp_sdk/module_support.h"
#include <cstddef>
#include <cstdint>

namespace ocudu::dapp::class_c {

// ---------------------------------------------------------------------------
// Value helpers (single implementation for all vendors)
// ---------------------------------------------------------------------------

/// Build typed ABI values for key tables and get() handlers.
ocudu_dapp_value_v1 value_bool(bool value) noexcept;
ocudu_dapp_value_v1 value_double(double value) noexcept;
/// Non-owning string view; caller must keep storage live for the host sample.
ocudu_dapp_value_v1 value_string(const char* data, std::size_t size) noexcept;
ocudu_dapp_value_v1 value_string(const char* c_str) noexcept;

/// Extract a typed value; false when the ABI value has a different kind.
bool parse_bool(const ocudu_dapp_value_v1& value, bool& out) noexcept;
bool parse_double(const ocudu_dapp_value_v1& value, double& out) noexcept;
/// Copies bytes into out; requires STRING kind.
bool parse_string(const ocudu_dapp_value_v1& value, char* out, std::size_t capacity,
                  std::size_t* out_size = nullptr) noexcept;

// ---------------------------------------------------------------------------
// Key schema
// ---------------------------------------------------------------------------

/// Built-in: key is instance-level "enabled" (atomic on reference::instance).
/// When false, begin_invocation returns BYPASS so the host retains conventional path.
constexpr uint32_t config_flag_instance_enabled = 1u << 16;
// Host ABI flags (READ_ONLY, SECRET, …) use the low bits of OCUDU_DAPP_CONFIG_*.

/// One configuration entry of a package schema. All pointers (names,
/// descriptions, aliases, handlers) must have static storage duration; see the
/// \ref bool_key / \ref double_key / \ref string_key builders.
struct config_key {
  const char* key{};
  const char* description{};
  uint32_t    value_kind{}; ///< OCUDU_DAPP_VALUE_*_V1
  uint32_t    flags{};      ///< OCUDU_DAPP_CONFIG_* | config_flag_instance_enabled
  /// Optional alternate names accepted on set_config (null-terminated list).
  const char* const* aliases{};

  ocudu_dapp_value_v1 default_value{};
  ocudu_dapp_value_v1 minimum_value{};
  ocudu_dapp_value_v1 maximum_value{};

  /// Read live value into out (strings must remain valid until next get_config).
  /// Unused when config_flag_instance_enabled is set.
  ocudu_dapp_status_v1 (*get)(void* instance, ocudu_dapp_value_v1* out) noexcept;

  /// Stage a write into vendor scratch (allocated for one set_config call).
  /// Unused for read-only or instance_enabled keys.
  ocudu_dapp_status_v1 (*stage)(void* instance, const ocudu_dapp_value_v1* in, void* scratch) noexcept;
};

/// A package's whole configuration surface: the static key table plus the
/// vendor hooks driving one set_config transaction
/// (begin_update → stage per key → validate → commit).
struct config_schema {
  const config_key* keys{};
  uint32_t          nof_keys{};
  /// Bytes of vendor scratch for one set_config (begin → stage* → validate → commit).
  std::size_t scratch_bytes{};

  /// Copy live package state into scratch (called once at start of set_config).
  void (*begin_update)(void* instance, void* scratch) noexcept;
  /// Validate staged scratch after all keys applied (bounds, cross-field).
  ocudu_dapp_status_v1 (*validate)(void* instance, void* scratch) noexcept;
  /// Apply scratch to live stores (not called on dry_run).
  ocudu_dapp_status_v1 (*commit)(void* instance, void* scratch) noexcept;
};

/// Build a management API that reuses base metrics/model handlers and replaces
/// describe/get/set_config with table-driven dispatch for `schema`.
///
/// The returned reference is process-lifetime stable. `schema` and its key
/// tables must have static storage duration.
const ocudu_dapp_management_v1& make_config_management(const ocudu_dapp_management_v1& base,
                                                       const config_schema&            schema) noexcept;

/// Convenience: base = reference::management_api().
const ocudu_dapp_management_v1& make_config_management(const config_schema& schema) noexcept;

// ---------------------------------------------------------------------------
// Describe helpers for building default/min/max values in key tables
// ---------------------------------------------------------------------------

inline config_key bool_key(const char* key, const char* description, bool default_value,
                           ocudu_dapp_status_v1 (*get)(void*, ocudu_dapp_value_v1*) noexcept,
                           ocudu_dapp_status_v1 (*stage)(void*, const ocudu_dapp_value_v1*, void*) noexcept,
                           uint32_t flags = 0, const char* const* aliases = nullptr) noexcept
{
  config_key k{};
  k.key           = key;
  k.description   = description;
  k.value_kind    = OCUDU_DAPP_VALUE_BOOL_V1;
  k.flags         = flags;
  k.aliases       = aliases;
  k.default_value = value_bool(default_value);
  k.minimum_value = value_bool(false);
  k.maximum_value = value_bool(true);
  k.get           = get;
  k.stage         = stage;
  return k;
}

inline config_key double_key(const char* key, const char* description, double default_value, double lo, double hi,
                             ocudu_dapp_status_v1 (*get)(void*, ocudu_dapp_value_v1*) noexcept,
                             ocudu_dapp_status_v1 (*stage)(void*, const ocudu_dapp_value_v1*, void*) noexcept,
                             uint32_t flags = 0, const char* const* aliases = nullptr) noexcept
{
  config_key k{};
  k.key           = key;
  k.description   = description;
  k.value_kind    = OCUDU_DAPP_VALUE_DOUBLE_V1;
  k.flags         = flags;
  k.aliases       = aliases;
  k.default_value = value_double(default_value);
  k.minimum_value = value_double(lo);
  k.maximum_value = value_double(hi);
  k.get           = get;
  k.stage         = stage;
  return k;
}

inline config_key string_key(const char* key, const char* description,
                             ocudu_dapp_status_v1 (*get)(void*, ocudu_dapp_value_v1*) noexcept,
                             ocudu_dapp_status_v1 (*stage)(void*, const ocudu_dapp_value_v1*, void*) noexcept,
                             uint32_t flags = 0, const char* const* aliases = nullptr) noexcept
{
  config_key k{};
  k.key         = key;
  k.description = description;
  k.value_kind  = OCUDU_DAPP_VALUE_STRING_V1;
  k.flags       = flags;
  k.aliases     = aliases;
  k.get         = get;
  k.stage       = stage;
  return k;
}

/// Standard instance "enabled" key (no vendor get/stage required).
/// Host turns this off to force conventional PHY for the instance.
inline config_key instance_enabled_key(const char* description = "Accept data-path invocations") noexcept
{
  config_key k{};
  k.key           = "enabled";
  k.description   = description;
  k.value_kind    = OCUDU_DAPP_VALUE_BOOL_V1;
  k.flags         = config_flag_instance_enabled;
  k.default_value = value_bool(true);
  k.minimum_value = value_bool(false);
  k.maximum_value = value_bool(true);
  return k;
}

} // namespace ocudu::dapp::class_c
