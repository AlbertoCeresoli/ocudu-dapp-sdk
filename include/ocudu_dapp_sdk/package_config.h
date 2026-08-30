// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
// Canonical include for multi-vendor package configuration (Class A / B / C).
//
// Prefer this header in new code. It re-exports the shared management skeleton
// implemented in class_c_config.* under the clearer package_config name.
//
// Vendor recipe (see docs/package_config.md and examples/vendor_min):
//   1. Declare static config_key[] + config_schema (process lifetime).
//   2. Implement get/stage/begin/validate/commit for product knobs.
//   3. make_config_management(schema) → assign module.management on populate.
//   4. Link OCUDUDapp::reference_support (or in-tree ocudu_reference_support).
//
// Do not re-implement describe_config / get_config / set_config loops.

#pragma once

#include "ocudu_dapp_sdk/class_c_config.h"

namespace ocudu::dapp {
/// Alias of class_c — single implementation, vendor-friendly name.
namespace package_config = class_c;
} // namespace ocudu::dapp
