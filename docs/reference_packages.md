<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Reference package inventory

Each primary package emits an artifact (`.so` or process executable), a
hash-bound **manifest-v1** JSON, and a digest-bound **SPDX 2.3** SBOM JSON.
Stable package UUIDs are assigned at build time in `CMakeLists.txt`; copy a
package only after minting a **new** UUID and global name for a product.

**How to enable and run each package on a DU** (YAML, E3 lifecycle, placement /
backend, offline checks): **[run_examples.md](run_examples.md)**.

## Class A — resident L1

| Artifact | Interface | Backend | Placement | Role |
|----------|-----------|---------|-----------|------|
| `ref_channel_estimator_cpu` | `0x00010003` | CPU | in-process | CE only |
| `ref_equalizer_cpu` | `0x00010004` | CPU | in-process | CE + EQ |
| `ref_receiver_cpu` | `0x00010005` | CPU | in-process | CE + EQ + demap |
| `ref_channel_estimator_cuda` | `0x00010003` | CUDA | in-process | CE only (device tensors) |
| `ref_equalizer_cuda` | `0x00010004` | CUDA | in-process | CE + EQ |
| `ref_receiver_cuda` | `0x00010005` | CUDA | in-process | Full RX + model-management demo |

**v1 admitted envelope (references):** ≤2 layers, all QAM, DM-RS type 1/2,
DC-capable, no transform precoding. Deterministic integration algorithms, not
production BLER claims.

**Host contract notes (equalizer depth):**

- **Post-EQ noise** is required for correct host CSI after equalizer-dApp
  inline success (host derives CRC/scheduler SINR from mean noise). Open
  `ref_equalizer_*` packages already write this tensor.
- **`dmrs.reserved[0]`** may carry an optional host-filled DMRS OFDM-symbol
  bitmask. References may ignore it and use coordinates (conforming).
- Soft host wall budgets default near **2000 µs** (demod) / **300 µs** (CE);
  recoverable deadline observations latch a worker after **8 consecutive**
  misses. Certificates and benches may inject tighter limits.

See [class_a_cuda_authoring.md](class_a_cuda_authoring.md) § Host CSI.

Scaffold: `tools/scaffold_class_a_cuda.py --kind ce|equalizer|receiver`.

## Class B — scheduler

| Artifact | Interface | Backend | Placement | Role |
|----------|-----------|---------|-----------|------|
| `ref_scheduler_cpu` | `0x00010006` | CPU | in-process | Host-policy-passthrough rank pipeline; experimental OLLA / MCS / multi-UE pack / TPC stages are opt-in |

**Sources:** `reference/scheduler/module.cpp` (trampoline) +
`reference/common/reference_scheduler_pipeline.cpp` (algorithms).  
**Defaults:** conventional rank passthrough with OLLA, MCS, packing, TPC,
layers, and PRB avoidance disabled. Host `scheduler_allow_*` flags bound the
authority of explicitly selected stages; they do not activate those stages.

No CUDA Class-B reference ships. Internal accelerator backends are allowed by
contract; see [class_b_accelerator_authoring.md](class_b_accelerator_authoring.md)
and [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md).

## Class C — spectrum / SRS–ISAC

| Artifact | Interface | Backend | Placement | Role |
|----------|-----------|---------|-----------|------|
| `ref_spectrum_cpu` | `0x00010001` | CPU | in-process | Radiometer / map authority |
| `ref_srs_isac_cpu` | `0x00010002` | CPU | in-process | Bounded CIR preview |
| `ref_spectrum_cuda` | `0x00010001` | CUDA | in-process | Device-grid radiometer |
| `ref_srs_isac_cuda` | `0x00010002` | CUDA | in-process | Device CIR preview |
| `ref_spectrumd_cpu` | `0x00010001` | CPU | out-of-process | Supervised SHM worker |
| `ref_srs_isacd_cpu` | `0x00010002` | CPU | out-of-process | Supervised SHM worker |
| `ref_spectrum_observerd_cpu` | `0x00010001` | CPU | out-of-process | Observer (no map/quiet caps) |
| `ref_spectrumd_cuda` | `0x00010001` | CUDA | out-of-process | CUDA-IPC spectrum worker |
| `ref_srs_isacd_cuda` | `0x00010002` | CUDA | out-of-process | CUDA-IPC SRS worker |

## Portable E3 consumers (not catalog packages)

Independent binaries when the installed SDK includes E3 management clients:

| Binary | Role |
|--------|------|
| `ref_spectrum_e3_cpu` | Low-rate spectrum host-snapshot consumer |
| `ref_srs_isac_e3_cpu` | Low-rate SRS host-snapshot consumer |

## Python Class-C examples (research / education)

See [`examples/class_c_e3_python`](../examples/class_c_e3_python/README.md).

| Artifact | Placement | Role |
|----------|-----------|------|
| `class_c_python_spectrumd` | out_of_process catalog | PyTorch radiometer over SHMEM/CUDA-IPC |
| `class_c_python_srs_isacd` | out_of_process catalog | PyTorch CIR preview over SHMEM/CUDA-IPC |
| `ocudu_dapp_class_c_e3` + `app.py` | portable E3 client | Low-rate independent spectrum consumer |

**Constraints:** Python is only for **Class-C out-of-process** (not Class-A/B or
in-process MODULEs). These packages are **not** a production performance path —
prefer the native `ref_spectrumd_*` / `ref_srs_isacd_*` rows above for product
deployments. Portable E3 consumers are not catalog packages; the
`class_c_python_*d` binaries **are** catalog packages (manifest next to the
artifact).

## Python E3AP examples (not catalog packages)

Plain Python processes that attach to the gNB's E3AP SCTP endpoint as
independent clients — no manifest, no `load` / lifecycle. How to run them:
[run_examples.md](run_examples.md) §6.

| Directory | Role |
|-----------|------|
| [`examples/e3_telemetry_python`](../examples/e3_telemetry_python/README.md) | Observer scripts: catalog, PUSCH quality, SRS watch, dApp health, full-rate recording, burst export |
| [`examples/e3_spectrum_sensing`](../examples/e3_spectrum_sensing/README.md) | Emitter detection on the `classc.spectrum` uplink grid |
| [`examples/e3_isac_sensing`](../examples/e3_isac_sensing/README.md) | SRS range-Doppler starting point (not a sensing capability) |
| [`examples/e3_mcp_server`](../examples/e3_mcp_server/README.md) | MCP server exposing the gNB to an AI assistant |

## Test-only fixtures (`BUILD_TESTING`)

| Artifact | Purpose |
|----------|---------|
| `test_joint_spectrum_scheduler_cpu` | Multi-interface spectrum C + scheduler B |
| `test_srs_dmrs_fusion_cpu` / `test_srs_dmrs_fusiond_cpu` | Required/optional multi-input binding |
| `failure_*` modules/workers | Operator failure gallery |
| `certifier_bad_*_cuda` | Negative certifier vectors |

Do not deploy test-only fixtures to production catalogs.

## Staging into a DU catalog

```bash
cp build/ref_*.so build/ref_*d_* \
   build/*.manifest.json build/*.spdx.json \
   /opt/ocudu/dapps/
# list catalog-relative manifests in DU YAML package_manifests
```

Then follow **[run_examples.md](run_examples.md)** for per-package `load` /
lifecycle commands and `initial_instances` sketches. Production: sign with
`ocudu-dapp-package` and configure a trust policy.
