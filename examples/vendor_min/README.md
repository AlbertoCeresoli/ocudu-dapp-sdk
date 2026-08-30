<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Mini vendor package example

This tree is the **canonical multi-vendor template**: how a commercial Class-A
CUDA package should look when it consumes **only** the open `OCUDUDapp` install
(host ABI is a transitive dependency). Copy this layout; replace the algorithm
and key table with product IP.

Guides: [`docs/vendor_toolkit.md`](../../docs/vendor_toolkit.md) ·
[`docs/package_config.md`](../../docs/package_config.md)

## Layout

```text
examples/vendor_min/
  CMakeLists.txt                 # standalone: find_package(OCUDUDapp)
  packages/example_eq_cuda/
    module.cpp                   # query trampoline only (pure)
    module_support.cpp           # lifecycle + invoke + attach management
    management.cpp               # package_config schema (demo_scale + enabled)
    algorithm.cpp                # thin product entry → open equalize_cuda
    reference_api.cpp            # standalone-only host wrappers onto ref_class_a
```

| File | Why it exists |
|------|----------------|
| `module.cpp` | Host may call query many times; must not init CUDA/GPU/threads |
| `module_support.cpp` | `stateful_module_context` + equalizer invoke |
| `management.cpp` | E3/dappctl knobs via shared core (not hand-rolled loops) |
| `algorithm.cpp` | Swap this for proprietary EQ; keep signature stable |
| `reference_api.cpp` | Only when building **outside** the monorepo against installed `ref_*_algorithm` |

## What it demonstrates

| Concern | Implementation |
|---------|----------------|
| One CMake package | `find_package(OCUDUDapp 0.1 CONFIG REQUIRED)` |
| ABI | `OCUDUDapp::DAppSDK` |
| Lifecycle | `OCUDUDapp::reference_support` / `module_support.h` |
| Config / E3 knobs | `package_config.h` → `make_config_management` |
| Conventional EQ | In-tree: `ref_equalizer_cuda_reference_api`; standalone: local `reference_api` + `ref_equalizer_cuda_algorithm` |
| Manifest / SBOM | `write_package_manifest.cmake` next to the MODULE |
| Contract | `config_count >= 1` (enabled + optional product keys) |

## Standalone build (vendor machine)

```bash
# Prefixes: host dapp_sdk + ocudu-dapp-sdk install (or one combined prefix)
cmake -S examples/vendor_min -B /tmp/vendor_min_build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/ocudu-host;/opt/ocudu-dapp"
cmake --build /tmp/vendor_min_build -j
certify_class_a_cuda /tmp/vendor_min_build/example_eq_cuda.so 0x00010004 --shape-set compact
```

## In-tree build

Enabled with the SDK CUDA examples (`OCUDU_DAPP_BUILD_CUDA_EXAMPLES`). Artifact:

`build-cuda/example_eq_cuda.so` (plus `.manifest.json` / `.spdx.json`)

Tests:

| CTest name | Purpose |
|------------|---------|
| `contract.example_eq_cuda` | ABI/lifecycle/config/metrics smoke |
| `certify.example_eq_cuda` | Class-A CUDA numeric certificate (`--shape-set compact`) |

```bash
ctest --test-dir build-cuda -R 'example_eq_cuda' --output-on-failure
```

## Enable on a live DU

Package UUID: `8f0d0001-4f9c-4e31-8c00-00000000e001` · interface `0x00010004` ·
**in-process / cuda**. Full cookbook (YAML + E3 for every SDK example):
[`docs/run_examples.md`](../../docs/run_examples.md) §2.

```bash
# Stage
cp build-cuda/example_eq_cuda.{so,manifest.json,spdx.json} /opt/ocudu/dapps/

# DU YAML package_manifests: example_eq_cuda.manifest.json
# Then:
# load --package 8f0d0001-4f9c-4e31-8c00-00000000e001 \
#   --placement in-process --backend cuda
# prepare → arm → shadow → activate
```

## Extending the example

1. **New knobs** — add keys in `management.cpp` (`double_key` / `bool_key` / …);
   keep `instance_enabled_key()` so the host can BYPASS.
2. **Product algorithm** — replace the body of `example::equalize` in
   `algorithm.cpp`; optionally call `ref_class_a::equalize` as fallback.
3. **Identity** — change global name / vendor / version / package UUID in
   CMake manifest rules and `populate_*` to match shipping policy.

## Do not

- Fork `include/ocudu_dapp_sdk/`
- Re-implement describe/get/set loops (use `package_config`)
- Link `odu` / host PHY into the MODULE
- Put CUDA context or weight load in `ocudu_dapp_query_v1`
