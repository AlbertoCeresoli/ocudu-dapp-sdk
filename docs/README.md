<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU dApp SDK documentation

**Step-by-step tutorials:** [stand up a vendor repo](tutorial_new_vendor_repo.md) ·
[MCP monitoring](tutorial_mcp_monitoring.md) · and the full 12-tutorial
[getting-started ladder](../README.md#getting-started--step-by-step-tutorials).

Toolkit release **0.1.0**, targeting public dApp ABI **v1**. Markdown in this
directory is Git-native (renders on GitLab). Matching Doxygen pages under
`doxygen/` build HTML in the same style as the main OCUDU repository.

## Start here

| Guide | Format | Audience |
|-------|--------|----------|
| [extension_model.md](extension_model.md) | Markdown | **Start here for algorithm work:** stable shell versus replaceable core |
| [ocudu_dapp_quickstart.md](ocudu_dapp_quickstart.md) | Markdown | Authors & operators — full walkthrough |
| [run_examples.md](run_examples.md) | Markdown | **Enable & run every example dApp** (YAML + E3 + UUIDs) |
| [vendor_toolkit.md](vendor_toolkit.md) | Markdown | **Multi-vendor** thin repos, `find_package(OCUDUDapp)` |
| [../examples/vendor_min/](../examples/vendor_min/README.md) | Example | **Copy this** Class-A MODULE template |
| [../examples/class_c_e3_python/](../examples/class_c_e3_python/README.md) | Example | **Python** Class-C out-of-process sensing (PyTorch; research only) |
| [../examples/e3_telemetry_python/](../examples/e3_telemetry_python/README.md) | Example | **Python E3AP observers**: telemetry subscriptions and management over SCTP, no host build |
| [../examples/e3_spectrum_sensing/](../examples/e3_spectrum_sensing/README.md) | Example | **Out-of-process sensing**: emitter detector on the Class-C spectrum grid |
| [../examples/e3_isac_sensing/](../examples/e3_isac_sensing/README.md) | Example | **SRS-ISAC starting point** (explicitly not a sensing capability) |
| [mcp_companion.md](mcp_companion.md) | Guide | **MCP companion**: architecture, setup, full tool reference, safety model, worked sessions, how to grow it into agentic tuning |
| [../examples/e3_mcp_server/](../examples/e3_mcp_server/README.md) | Example | **MCP server**: AI-assistant companion for the gNB over E3 (observe + act tools) |
| [package_config.md](package_config.md) | Markdown | Shared A/B/C config / E3 knobs |
| [doxygen/](doxygen/) | Doxygen | Same topics for `cmake --target doxygen` HTML |
| [../README.md](../README.md) | Markdown | Package inventory, build flags, ABI notes |

## Authoring by class

| Topic | Markdown | Doxygen page |
|-------|----------|--------------|
| Class-A CUDA (CE / EQ / RX) | [class_a_cuda_authoring.md](class_a_cuda_authoring.md) (+ **host CSI / reserved[0]** + enable/run) | `sdk_class_a_cuda` |
| Optional receiver constellation output | [receiver_symbol_output.md](receiver_symbol_output.md) | — |
| Class-B scheduler / accelerators | [class_b_accelerator_authoring.md](class_b_accelerator_authoring.md) (+ **enable/run** section) | `sdk_class_b` |
| Class-B baseline pipeline (MCS / pack / TPC) | [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md) | `sdk_class_b` (baseline section) |
| Class-C CUDA process workers | [class_c_cuda_process_authoring.md](class_c_cuda_process_authoring.md) | `sdk_class_c_cuda` |
| Package config (all classes) | [package_config.md](package_config.md) | — |
| Multi-vendor / commercial CMake | [vendor_toolkit.md](vendor_toolkit.md) | — |
| Spectrum observer vs authority | [observer_authority.md](observer_authority.md) | `sdk_observer_authority` |
| Reference package inventory | [reference_packages.md](reference_packages.md) | `sdk_reference_packages` |
| **Run / enable examples** | [run_examples.md](run_examples.md) | — |
| Tools (scaffold, certify, model) | [tools.md](tools.md) | `sdk_tools` |
| Validation ladder | [validation_ladder.md](validation_ladder.md) | — |
| Release procedure | [releasing.md](releasing.md) | — |
| Security model | [security_model.md](security_model.md) | — |
| Compatibility matrix | [compatibility.md](compatibility.md) | — |
| Known limitations | [known_limitations.md](known_limitations.md) | — |

For opt-in asynchronous full receivers, see
[receiver completion](receiver_completion.md). This extension requires an
integrated host consumer and is not enabled merely by installing the headers.

## Build Doxygen HTML (OCUDU-style)

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/installed/ocudu-dapp-sdk
cmake --build build --target doxygen
# Open build/docs/doxygen/html/index.html
```

Requires Doxygen on `PATH`. The target is optional; Markdown remains the
primary Git web view.

## OCUDU runtime & E3 (host tree)

| Topic | Location |
|-------|----------|
| dApp index (Git Markdown) | `ocudu/docs/dapp/README.md` |
| YAML / CLI (incl. `scheduler_allow_*`) | `ocudu/docs/dapp/yaml_configuration.md` |
| E3 management guide | `ocudu/docs/dapp/e3_management.md` |
| E3 wire guide | `ocudu/docs/dapp/e3_wire.md` |
| E3 system design | `ocudu/docs/dapp/e3_system_design.md` |
| Operator Doxygen | `ocudu/docs/doxygen/dapp_*.dox`, `e3_*.dox` |

All OCUDU/DeepSig-owned package, ABI, E3, FlatBuffers, and evidence contracts
remain **v1** until a formal release decision.

### Host feedback contract (Class-A equalizer, v1)

Documented in [class_a_cuda_authoring.md](class_a_cuda_authoring.md) and host
`ocudu/docs/dapp/e3_system_design.md`: after equalizer-dApp inline success the
host derives scheduler-facing SINR from **post-EQ noise**; optional
`dmrs.reserved[0]` is a host-filled DMRS symbol bitmask packages may ignore.
Open reference equalizers remain compatible without rebuild.
