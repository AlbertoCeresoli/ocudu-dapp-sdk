<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Package config (shared A / B / C management)

One management core for **every** Class A, B, and C package. Vendors declare a
static key table; the SDK owns describe/get/set, generation conflicts, dry-run,
value codecs, and the instance `enabled` flag.

## Why this exists

E3 / `dappctl` / host ops all speak the same config surface. Re-implementing
loops and codecs in each commercial MODULE diverges multi-vendor behaviour and
bugs. Link `OCUDUDapp::reference_support` and extend the schema only.

| Include | Role |
|---------|------|
| `ocudu_dapp_sdk/package_config.h` | **Canonical** name (`namespace package_config = class_c`) |
| `ocudu_dapp_sdk/class_c_config.h` | Implementation header (historical name; same API) |

## Quick recipe

```cpp
#include "ocudu_dapp_sdk/package_config.h"

using namespace ocudu::dapp::package_config;

// 1) Optional update scratch (bytes reserved once per set_config).
struct update_scratch {
  double alpha{0.2};
  bool   dirty{false};
};

// 2) Hooks: begin copies live → scratch; stage writes keys; validate; commit.
void begin_update(void* /*instance*/, void* scratch) noexcept { /* … */ }
ocudu_dapp_status_v1 validate(void*, void* scratch) noexcept { /* bounds */ }
ocudu_dapp_status_v1 commit(void*, void* scratch) noexcept { /* publish */ }
ocudu_dapp_status_v1 get_alpha(void*, ocudu_dapp_value_v1* out) noexcept;
ocudu_dapp_status_v1 stage_alpha(void*, const ocudu_dapp_value_v1* in, void* scratch) noexcept;

// 3) Static schema (process-lifetime).
static const config_key kKeys[] = {
  instance_enabled_key(),  // always first in open refs; required for BYPASS when off
  double_key("alpha_fast", "EWMA alpha", 0.2, 0.001, 1.0, get_alpha, stage_alpha),
};
static const config_schema kSchema{
  kKeys,
  static_cast<uint32_t>(sizeof(kKeys) / sizeof(kKeys[0])),
  sizeof(update_scratch),
  begin_update,
  validate,
  commit,
};

// 4) Attach on populate (after module_support fills the module).
const ocudu_dapp_management_v1& my_management_api() noexcept
{
  static const ocudu_dapp_management_v1& api = make_config_management(kSchema);
  return api;
}
```

In `module_support` / populate:

```cpp
output->management = &my_management_api();
```

The instance pointer passed to management APIs is a `reference::instance*`
(from `stateful_module_context` / `module_support`). Do not cast it to a
product type unless you own a custom base; put product knobs in file-scope
stores or in fields of a type that **derives** from `instance`.

## What the core owns

- `describe_config` / `get_config` / `set_config` dispatch over the key table
- `expected_generation` conflicts → `OCUDU_DAPP_CONFLICT_V1`
- `dry_run` (validate only; no commit, no generation bump)
- `instance_enabled_key()` → `instance.enabled` atomic (data-path BYPASS when false)
- `OCUDU_DAPP_CONFIG_READ_ONLY_V1` rejection on set
- Key **aliases** (null-terminated `const char* const*`)
- Bool / double / string value helpers (`value_*` / `parse_*`)

## What vendors own

- Key names, descriptions, min/max/default metadata
- Live stores (process-wide or instance-derived)
- `begin_update` / `stage` / `validate` / `commit` for product knobs
- Optional READ_ONLY telemetry strings

## Working examples

| Location | What it shows |
|----------|----------------|
| [`examples/vendor_min/.../management.cpp`](../examples/vendor_min/packages/example_eq_cuda/management.cpp) | Commercial-style `demo_scale` + `enabled` |
| `reference/equalizer_cuda/module_support.cpp` | Open ref: `enabled` only via `package_config` |
| Commercial DeepSig packages | Full product key tables (same core) |
| `tests/package_config_test.cpp` | Unit tests: describe, dry-run, generation, aliases, bounds |

## CMake

```cmake
find_package(OCUDUDapp 0.1 CONFIG REQUIRED)
target_link_libraries(my_pkg PRIVATE
  OCUDUDapp::DAppSDK
  OCUDUDapp::reference_support)  # package_config + module_support
```

In-tree SDK builds use `ocudu_reference_support` / `OCUDU::DAppSDK`.

## Contract tests

`module_contract_test` requires **at least one** config key (typically
`enabled`). Packages may expose additional product knobs; counts `>= 1` pass.

## Do not

- Hand-roll describe/get/set loops in package TUs
- Fork `class_c_config.cpp` into vendor repos
- Hold non-static schema / key table storage (management API is process-lifetime)
- Treat host PHY BLER tools as dApp config (different surface; see commercial tests)
