<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Vendor toolkit (minimal commercial dApp repos)

Goal: keep **vendor repositories thin** — proprietary packages + thin tests —
while **ocudu-dapp-sdk** owns shared validation tools, lifecycle helpers, package
config, and optional open Class-A CUDA algorithm libraries.

## Layers

| Layer | Source | Vendor links? |
|-------|--------|----------------|
| **Host runtime** | `ocudu` gNB / `odu` | **No** — only `dlopen` at run time |
| **Open dApp package** | This repo install: **`find_package(OCUDUDapp)`** | **Yes — one package** |
| **ABI** | Host `OCUDUDAppSDK` (dependency of `OCUDUDapp`) | Via `OCUDUDapp::DAppSDK` |
| **Helpers** | `OCUDUDapp::reference_support` (lifecycle + **package config** for A/B/C) | **Yes** |
| **Class-A CUDA** | `OCUDUDapp::ref_*_cuda_algorithm` (`ref_class_a::*`) | Optional |
| **Tools** | `certify_class_a_cuda`, `l1_validation_suite` | Invoke as binaries |
| **Vendor algorithms** | Vendor repo only | Own MODULEs |

Do **not** vendor-copy `include/ocudu_dapp_sdk/`. One `find_package(OCUDUDapp)`;
host ABI is pulled as a dependency.

## Canonical mini example (copy this)

Checked-in template with the full commercial layout (query trampoline /
lifecycle / `package_config` management / thin algorithm into open conventional
EQ):

**[`examples/vendor_min/`](../examples/vendor_min/README.md)**

```bash
# In-tree (SDK CUDA build): example_eq_cuda.so + contract/certify tests
# Standalone (vendor machine):
cmake -S examples/vendor_min -B /tmp/vbuild \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/ocudu-host;/opt/ocudu-dapp"
cmake --build /tmp/vbuild -j
certify_class_a_cuda /tmp/vbuild/example_eq_cuda.so 0x00010004 --shape-set compact
```

File map and design notes: the example README. Management details:
[`package_config.md`](package_config.md).

## Minimal vendor CMake sketch

```cmake
# Single package from ocudu-dapp-sdk install (pulls host ABI as dependency).
find_package(OCUDUDapp 0.1 CONFIG REQUIRED)
find_package(CUDAToolkit REQUIRED)

add_library(my_eq_cuda MODULE
  packages/eq/module.cpp
  packages/eq/module_support.cpp
  packages/eq/management.cpp
  packages/eq/algorithm.cu)
set_target_properties(my_eq_cuda PROPERTIES PREFIX "")
target_link_libraries(my_eq_cuda PRIVATE
  OCUDUDapp::DAppSDK
  OCUDUDapp::reference_support
  OCUDUDapp::ref_equalizer_cuda_algorithm   # optional conventional fallback
  CUDA::cudart)
```

Install (host ABI + open SDK):

```bash
cmake --install ocudu/build --prefix /opt/ocudu-dapp --component dapp_sdk
cmake --install ocudu-dapp-sdk/build-cuda --prefix /opt/ocudu-dapp
# One prefix is fine; or two prefixes on CMAKE_PREFIX_PATH
```

Tests (no duplicated L1 sources):

```bash
certify_class_a_cuda build/my_eq_cuda.so 0x00010004
l1_validation_suite --mode all --quick \
  --eq-so build/my_eq_cuda.so --rx-so build/my_rx_cuda.so
```

## Module layout (recommended)

Keep the same TU split as open refs and `vendor_min` so reviews stay mechanical:

| File | Responsibility |
|------|----------------|
| `module.cpp` | **Only** `ocudu_dapp_query_v1` → populate trampoline (pure, no CUDA init) |
| `module_support.cpp` | Lifecycle, workers, invoke, attach `management` |
| `management.cpp` | `package_config` schema + product stores |
| `algorithm.{cpp,cu}` | Product IP (and optional `ref_class_a::*` fallback) |

## Unified validation

Prefer one profile-driven runner (packaging → algorithm → system):

```bash
python3 tools/dapp_validate.py --profile tools/lab_profile.example.json
```

See [`validation_ladder.md`](validation_ladder.md).

## Shared conventional Class-A CUDA

Open algorithms live in namespace **`ocudu::dapp::ref_class_a`**
(`include/ocudu_dapp_sdk/ref_class_a_cuda.h` + installed static libs).

The installable algorithm archives export **only** `ref_class_a::*` so commercial
`equalize_cuda` / `receive_cuda` entry points cannot ODR-clash. Open reference
packages in this repo use a separate `*_reference_api` wrapper layer.

Commercial Neural entrypoints call `ref_class_a::*` for conventional fallback
via a thin bridge. Also share `ocudu_dapp_sdk/class_a_cuda_contract.cuh`
(single LS/ZF contract) from the toolkit include tree.

Standalone vendor builds that link `ref_*_cuda_algorithm` may need a thin local
`reference_api.cpp` (see `examples/vendor_min`) when the open `equalize_cuda`
host wrapper is not installed as a separate target.

## module_support

Link **`OCUDUDapp::reference_support`** (not a vendored copy of
`module_support.cpp`). Use the identity-aware populate overload so
vendor/version match the package manifest:

```cpp
context::populate(host, output, "com.vendor.pkg", "Vendor Inc.", "1", &descriptor);
// then: output->management = &my_management_api();
```

## Package config management (Class A / B / C — one core)

Do **not** re-implement `describe_config` / `get_config` / `set_config` loops or
value codecs. Full guide: [`package_config.md`](package_config.md).

```cpp
#include "ocudu_dapp_sdk/package_config.h"

using namespace ocudu::dapp::package_config;

static const config_key kKeys[] = {
  instance_enabled_key(),
  double_key("alpha_fast", "…", 0.2, 0.001, 1.0, get_alpha, stage_alpha),
};
static const config_schema kSchema{kKeys, N, sizeof(Scratch), begin, validate, commit};

const ocudu_dapp_management_v1& my_management_api() noexcept {
  static const auto& api = make_config_management(kSchema);
  return api;
}
```

## Scaffold (new Class-A package)

```bash
python3 tools/scaffold_class_a_cuda.py \
  --kind equalizer --target my_eq_cuda \
  --global-name com.example.eq.cuda \
  --package-id 00000000-0000-4000-8000-000000000001 \
  --output /tmp/my_eq_cuda
cmake -S /tmp/my_eq_cuda -B /tmp/my_eq_cuda/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOCUDUDAppSDK_DIR=/path/to/host/lib/cmake/OCUDUDAppSDK
cmake --build /tmp/my_eq_cuda/build
certify_class_a_cuda /tmp/my_eq_cuda/build/my_eq_cuda.so 0x00010004
```

The scaffold is **self-contained**: it vendors `module_support`, the CUDA
contract, and (for EQ/RX) `ref_class_a` + thin wrappers. It needs only
`OCUDUDAppSDK` at configure time — not a sibling SDK source tree. Prefer
migrating scaffolds to **install-only** `OCUDUDapp` + shared `package_config`
when packaging for multi-vendor fleets (see `vendor_min`).

## Install-only commercial build smoke

```bash
cmake --install ocudu/build --prefix /opt/ocudu-dapp --component dapp_sdk
cmake --install ocudu-dapp-sdk/build-cuda --prefix /opt/ocudu-dapp
cmake -S acmerf-dapps -B /tmp/vendor-install-only \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/ocudu-dapp
cmake --build /tmp/vendor-install-only -j
```

## Do not

- Link `odu` / host PHY into vendor packages
- Fork `l1_validation` or `include/ocudu_dapp_sdk/` into each vendor repo
- Depend on host-private `ocudu/dapp/runtime/*` headers from packages
- Point CMake at an ocudu-dapp-sdk **source tree** for algorithms (use the install)
- Re-implement package config loops (use `package_config.h`)
