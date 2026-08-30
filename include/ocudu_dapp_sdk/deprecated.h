// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

/// Deprecation marker for public SDK declarations.
///
/// API_STABILITY.md requires deprecations to appear in the changelog for at
/// least one release before removal; this macro is the compiler-visible half
/// of that contract. Apply it at the declaration and name the replacement:
///
///   OCUDU_DAPP_DEPRECATED("use run_scheduler_pipeline")
///   ocudu_dapp_status_v1 old_entry(...);
///
/// C++14/C11-compatible: uses [[deprecated]] where available and the GNU
/// attribute otherwise, so it is safe in headers shared across the C ABI.
#if defined(__cplusplus) && __cplusplus >= 201402L
#define OCUDU_DAPP_DEPRECATED(message) [[deprecated(message)]]
#elif defined(__GNUC__) || defined(__clang__)
#define OCUDU_DAPP_DEPRECATED(message) __attribute__((deprecated(message)))
#else
#define OCUDU_DAPP_DEPRECATED(message)
#endif
