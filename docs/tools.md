<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# SDK and host tools

**Enable / run every example dApp on a live DU:**
[run_examples.md](run_examples.md) (package UUIDs, YAML, E3 lifecycle).

## In this repository

### `examples/vendor_min` (copy-me commercial MODULE)

Canonical multi-vendor Class-A layout: `find_package(OCUDUDapp)`,
`package_config`, thin algorithm. See
[`../examples/vendor_min/README.md`](../examples/vendor_min/README.md) and
[`vendor_toolkit.md`](vendor_toolkit.md).

### `examples/e3_telemetry_python` (Python E3AP observers)

Out-of-process observer dApps that use the OCUDU `ocudu_e3` client over the
E3AP SCTP endpoint: stream catalog, PUSCH quality/BLER with gNB-side RNTI
filters, SRS channel coefficients, and dApp inventory/health/metrics carried as
E3AP control actions. No host build is needed; see
[`../examples/e3_telemetry_python/README.md`](../examples/e3_telemetry_python/README.md).

### `examples/e3_spectrum_sensing` (out-of-process spectrum sensing)

`emitter_detector.py` subscribes to the `classc.spectrum` telemetry over the
E3AP SCTP endpoint and runs emitter detection (noise-floor tracking, wideband
guard, per-PRB run tracking) on the complete uplink resource grid — the same
data the in-process Spectrum dApp sees, consumed by a plain process with no
manifest or lifecycle. See
[`../examples/e3_spectrum_sensing/README.md`](../examples/e3_spectrum_sensing/README.md).

### `examples/e3_isac_sensing` (SRS-ISAC starting point)

`srs_isac_monitor.py` subscribes to the SRS channel response (`srs.channel` or
the `classc.srs_isac` snapshot) over E3AP and runs a minimal range-Doppler
pipeline. Explicitly **not** a functional ISAC capability — a data-flow
starting point to replace the maths of. See
[`../examples/e3_isac_sensing/README.md`](../examples/e3_isac_sensing/README.md).

### `examples/class_c_e3_python` (Python Class-C sensing)

**Research / education example** of Class-C sensing in Python + PyTorch:

| Path | Role |
|------|------|
| Supervised process | Catalog packages `class_c_python_spectrumd` / `class_c_python_srs_isacd` (SHMEM / CUDA-IPC + DLPack) |
| Portable E3 | Independent low-rate consumer (`ocudu_dapp_class_c_e3` + `app.py --mode live`) |

**Hard constraints:**

* Python is supported only for **Class-C out-of-process** (supervised process or
  portable E3). Class-A/B and in-process Class-C MODULEs remain C/C++/CUDA only.
* Not a production RAN performance path — prefer `ref_spectrumd_*` /
  `ref_srs_isacd_*` (or your own native packages) for product deployments.

See
[`../examples/class_c_e3_python/README.md`](../examples/class_c_e3_python/README.md).

### `tools/scaffold_class_a_cuda.py`

Generate an external Class-A CUDA project that depends only on installed
`OCUDUDAppSDK`.

```bash
python3 tools/scaffold_class_a_cuda.py \
  --kind {ce,equalizer,receiver} \
  --target <cmake_target> \
  --global-name com.example.ran.my-module.cuda \
  --package-id <uuid> \
  --output /path/to/new-repo \
  [--vendor "Vendor"] [--version 1]
```

Refuses to overwrite an existing directory. Emits algorithm + support shell,
manifest/SBOM CMake rules, and Clear license headers.

### `tools/scaffold_class_bc_cpu.py`

Generate an external Class-B (scheduler) or Class-C (spectrum, SRS-ISAC) CPU
project that depends only on installed `OCUDUDAppSDK` — no CUDA toolchain
required.

```bash
python3 tools/scaffold_class_bc_cpu.py \
  --kind {scheduler,spectrum,srs-isac} \
  --target <cmake_target> \
  --global-name com.example.ran.my-module.cpu \
  --package-id <uuid> \
  --output /path/to/new-repo \
  [--vendor "Vendor"] [--version 1]
```

The scheduler kind vendors the five-stage reference pipeline (every stage
shipped off — host-equivalent until enabled over E3); the Class-C kinds vendor
the radiometer / CIR-sketch cores and the never-block-the-producer publication
shell. Validate the artifact with `module_contract_test` from an SDK build
(the emitted README shows the exact invocation). `scaffold.class_bc_cpu` in
this repo's ctest builds and contract-tests all three kinds end to end.
Installed as `ocudu-dapp-scaffold-class-bc-cpu`.

### `l1_validation_suite` (build tree)

Central Class-A **L1 A–D** offline suite (noisy simulated PUSCH → SER/MSE,
chain, SNR sensitivity, uncoded CRC24A). Loads packages via **dlopen** so the
same binary validates open `ref_*_cuda` and vendor commercial DSOs.

```bash
build-cuda/l1_validation_suite --mode all --quick \
  --eq-so build-cuda/ref_equalizer_cuda.so \
  --rx-so build-cuda/ref_receiver_cuda.so
```

See [`tools/l1_validation/README.md`](../tools/l1_validation/README.md) and
[`vendor_toolkit.md`](vendor_toolkit.md).

### `certify_class_a_cuda` (build tree)

Offline positive certificate for a final Class-A CUDA DSO.

```bash
build-cuda/certify_class_a_cuda <module.so> <interface_id> \
  [--shape-set boundaries|compact] \
  [--model-artifact path --model-id id --candidate-model-id id \
   --model-version v --model-generation n]
```

| Interface | Id |
|-----------|-----|
| CE | `0x00010003` |
| Equalizer | `0x00010004` |
| Receiver | `0x00010005` |

Checks lifecycle, concurrent workers/streams, shape matrix, QAM×DM-RS, canaries,
metrics, and optional model generations. JSON on stdout — preserve with the
manifest. Not a substitute for live DU or production tail qualification.

### `ocudu-dapp-srs-replay`

Runs the CPU reference SRS channel estimator on every occasion of an SRS vector
file and compares its channel matrix, EPRE, RSRP, noise variance and time
alignment with the answer recorded in the file (values recorded as NaN are
skipped). The input is the golden set (`tests/data/srs_channel_estimator_vectors.bin`)
or occasions exported from a live capture (`examples/e3_srs_dataset`).

```bash
ocudu-dapp-srs-replay live.bin                  # per-occasion lines and a summary
ocudu-dapp-srs-replay golden.bin --check --quiet   # exit 1 outside the golden bounds
```

### `make_receiver_cuda_model` (build tree)

Emits the **reference** pointer-free receiver model artifact for demos and
certifier model tests. Product packages must replace the binary format and
validation with their own exporter.

```bash
build-cuda/make_receiver_cuda_model receiver-model.bin 0.75
```

## On the OCUDU host (installed or build tree)

| Tool | Role |
|------|------|
| `ocudu-dapp-package` | Offline `keygen`, `sign`, `trust`, `inspect` |
| `ocudu-dapp-preflight` | Disposable native package preflight before DU admission |
| `ocudu-dappctl` | C++ local E3 management client |
| `apps/tools/dappctl/ocudu_dappctl.py` | Python/JSON E3 client (same schema) |

See OCUDU `docs/dapp/e3_management.md` and `docs/doxygen/dapp_operations.dox`.

## Compute Sanitizer (external)

```bash
compute-sanitizer --tool memcheck \
  build-cuda/certify_class_a_cuda module.so 0x00010005
# racecheck: add --shape-set compact on the certifier
```

## CTest registration

CUDA builds register certifier, scaffold generation/build/cert, rejection, and
process-worker tests. Prefer:

```bash
ctest --test-dir build-cuda --output-on-failure
```

### `examples/e3_mcp_server` (MCP companion)

`ocudu_mcp_server.py` speaks the Model Context Protocol over stdio and uses one
E3AP association for everything: a live sector model (UEs, KPIs, traffic
history, SRS/constellation snapshots) for the observation tools and E3
management (dApp config/lifecycle, incidents, RAN control) for the action
tools. `--read-only` hides the mutating tools. See the guide
[`mcp_companion.md`](mcp_companion.md) and the example README
[`../examples/e3_mcp_server/README.md`](../examples/e3_mcp_server/README.md).
