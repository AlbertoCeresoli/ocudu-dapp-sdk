<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU dApp Quickstart

**Audience:** engineers who want a new out-of-tree dApp repository, a packaged
artifact, and a live OCUDU DU (gNB DU / O-DU) that can load, enable, disable,
monitor, and verify that package.

**Classification:** working version v1 · pre-release contracts remain at v1

This guide is the end-to-end walkthrough. Deeper contracts live in the linked
authoring and operator pages; this page is the path that stitches them into one
repeatable workflow.

| Companion | Role |
|-----------|------|
| [docs/README.md](README.md) | Documentation index (Markdown + Doxygen) |
| [SDK README](../README.md) | Package inventory, build flags, ABI notes |
| [Vendor toolkit](vendor_toolkit.md) | Thin commercial repos, one `find_package(OCUDUDapp)` |
| [vendor_min example](../examples/vendor_min/README.md) | **Copy-me** Class-A MODULE with `package_config` |
| [Package config](package_config.md) | Shared A/B/C E3 knobs (`make_config_management`) |
| [Class-A CUDA authoring](class_a_cuda_authoring.md) | Resident CE / EQ / receiver rules |
| [Class-B accelerator authoring](class_b_accelerator_authoring.md) | Scheduler host-record boundary |
| [Class-B baseline pipeline](class_b_scheduler_baseline.md) | Rank / OLLA / MCS / pack / TPC reference |
| [Class-C CUDA process authoring](class_c_cuda_process_authoring.md) | CUDA-IPC process workers |
| [Observer and authority](observer_authority.md) | Spectrum single-writer rules |
| [Reference packages](reference_packages.md) | Full artifact inventory |
| [**Run examples**](run_examples.md) | **Enable & run every SDK example dApp** (YAML + E3 + UUIDs) |
| [Tools](tools.md) | Scaffold, certifier, host tools |
| SDK Doxygen | `docs/doxygen/` · `cmake --target doxygen` |
| OCUDU `docs/dapp/README.md` | Host Git index (E3 + YAML + quickstart) |
| OCUDU `docs/dapp/e3_management.md` | E3 management guide |
| OCUDU `docs/dapp/e3_wire.md` | E3 APER wire guide |
| OCUDU `docs/dapp/e3_system_design.md` | Runtime + E3 architecture |
| OCUDU Doxygen `dapp_*` / `e3_*` | HTML pages matching OCUDU style |

---

## 0. Mental model (read once)

OCUDU embeds one **dApp Runtime** in the DU process. A dApp is not a side
daemon you point the modem at; it is a **hash-bound catalog package** that the
DU admits, preflights, and drives through a fixed lifecycle.

```text
Your out-of-tree repo  ──build──►  artifact + manifest-v1 + SPDX SBOM
                                         │
                                         ▼
                              operator catalog root
                                         │
                    (optional) ocudu-dapp-package sign / trust
                                         │
                                         ▼
                         DU YAML: package_manifests
                         (+ optional initial_instances)
                                         │
                                         ▼
              E3 management (local socket): load → prepare → arm
                              → shadow → activate
                                         │
                                         ▼
                    metrics / health / incidents  (verify working)
```

**Three timing classes (not one bus):**

| Class | Deadline role | Placement | Typical examples |
|-------|---------------|-----------|------------------|
| **A** | Resident inline L1 | In-process only | Channel estimate, equalizer, full receiver |
| **B** | Bounded RT control (~100 µs) | In-process authority | Scheduler rank / MCS / PRB pack / TPC intents |
| **C** | Async observation | Native **or** supervised process | Spectrum, SRS–ISAC |

**Hard rules that keep the modem safe:**

1. dApps never call each other; the host owns composition.
2. E3 never supplies filesystem paths, executable argv, or route descriptors.
3. Conventional PHY/scheduler remains the fallback when a dApp is absent or fails.
4. Production defaults **require** detached package signatures and an offline trust policy.
5. Query (`ocudu_dapp_query_v1`) stays pure; CUDA init, weights, and threads belong to lifecycle/model hooks.

**Repo split:**

| Tree | Owns |
|------|------|
| **`ocudu`** | ABI headers (via install), runtime, E3, PHY/scheduler adapters, preflight, catalog, `ocudu-dappctl`, `ocudu-dapp-package` |
| **`ocudu-dapp-sdk`** | Open helpers (`OCUDUDapp`), references, `package_config`, scaffolds, certifiers, this guide |
| **Your dApp repo** | Algorithm + thin MODULE shell against **installed** `OCUDUDapp` (ABI via dependency) |

Commercial weights and proprietary kernels stay in **your** tree. Do not copy
generic OCUDU PHY sources into a dApp. Prefer
[`examples/vendor_min`](../examples/vendor_min/README.md) over forking open
reference sources when starting a commercial Class-A package.

---

## 1. Prerequisites

### 1.1 Toolchain

- CMake ≥ 3.18, C++17 toolchain matching the OCUDU ABI build (GCC or Clang).
- For Class-A CUDA packages: CUDA toolkit whose architectures match the target
  (for example GB10 SM `121`).
- Python 3 for `scaffold_class_a_cuda.py`, the Python E3 client, and optional tests.
- Optional: Compute Sanitizer for memcheck/racecheck before field trials.

### 1.2 Install OCUDU with dApp + E3

From an OCUDU source tree built for your platform:

```bash
export OCUDU_SRC=/path/to/ocudu
export OCUDU_BUILD=$OCUDU_SRC/build-dapp
export OCUDU_PREFIX=$HOME/ocudu-install

cmake -S "$OCUDU_SRC" -B "$OCUDU_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$OCUDU_PREFIX" \
  -DENABLE_DAPP=ON \
  -DENABLE_E3=ON \
  -DENABLE_CUDA=ON   # omit when CPU-only

cmake --build "$OCUDU_BUILD" -j"$(nproc)"
cmake --install "$OCUDU_BUILD"
```

Confirm the dApp component is present:

```bash
ls "$OCUDU_PREFIX/lib/cmake/OCUDUDAppSDK/"
ls "$OCUDU_PREFIX/bin/ocudu-dapp-package" \
   "$OCUDU_PREFIX/bin/ocudu-dapp-preflight" 2>/dev/null || true
# Management clients may live under the apps tree of the build or install layout.
```

The host CMake package name is **`OCUDUDAppSDK`**. The open helpers package
from this repository is **`OCUDUDapp`** (installs lifecycle support, package
config, optional Class-A CUDA algorithms, certifier). Commercial modules should
`find_package(OCUDUDapp)` so they do not include OCUDU or SDK sources by path.

### 1.3 Build and install the open SDK once

```bash
export SDK_SRC=/path/to/ocudu-dapp-sdk
export SDK_BUILD=$SDK_SRC/build-cuda   # or build-cpu
export DAPP_PREFIX=$HOME/ocudu-dapp    # or same as OCUDU_PREFIX

cmake -S "$SDK_SRC" -B "$SDK_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$OCUDU_PREFIX" \
  -DCMAKE_CUDA_ARCHITECTURES=121       # when CUDA

cmake --build "$SDK_BUILD" -j"$(nproc)"
ctest --test-dir "$SDK_BUILD" --output-on-failure
cmake --install "$SDK_BUILD" --prefix "$DAPP_PREFIX"
```

A green SDK suite proves the install is usable before you author a new package.
CUDA trees also build `examples/vendor_min` (`example_eq_cuda`) and
`package_config` unit tests when testing is enabled.

---

## 2. Create a new dApp repository

**Commercial Class-A (install-only multi-vendor):** copy
[`examples/vendor_min`](../examples/vendor_min/README.md) and swap algorithm +
`package_config` keys — see [vendor_toolkit.md](vendor_toolkit.md).

**Python Class-C sensing (research / education only):** copy
[`examples/class_c_e3_python`](../examples/class_c_e3_python/README.md). Covers
supervised process catalog packages (`class_c_python_spectrumd` /
`class_c_python_srs_isacd`, SHMEM/CUDA-IPC + DLPack) and an optional portable-E3
client (pybind over C++ `portable_e3_consumer`; no Python APER). Sensing is
PyTorch. **Python is only for Class-C out-of-process** — not Class-A/B or
in-process MODULEs — and is **not** a production performance path (prefer
`ref_spectrumd_*` / `ref_srs_isacd_*`). Portable E3 requires host E3 libs built
with `POSITION_INDEPENDENT_CODE`.

**Self-contained scaffold** (needs only host `OCUDUDAppSDK`, vendors some
helpers into the new tree): §2.1 below.

**Class B/C C++ packages:** start from a reference directory and rename identity fields.

### 2.0 Copy the mini vendor template (recommended for fleets)

```bash
cp -a "$SDK_SRC/examples/vendor_min" "$HOME/src/my-eq-dapp"
# Edit global name / vendor / package UUID in CMakeLists + populate identity.
# Replace algorithm.cpp; extend management.cpp keys.
cmake -S "$HOME/src/my-eq-dapp" -B "$HOME/src/my-eq-dapp/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$OCUDU_PREFIX;$DAPP_PREFIX"
cmake --build "$HOME/src/my-eq-dapp/build" -j
```

### 2.1 Scaffold a Class-A CUDA project

Generate a **new** git root outside the SDK tree. The tool refuses to overwrite
an existing directory.

```bash
# Use a fresh UUID for every package you will ship.
PACKAGE_ID="$(uuidgen)"   # or: python3 -c 'import uuid; print(uuid.uuid4())'

python3 "$SDK_SRC/tools/scaffold_class_a_cuda.py" \
  --kind receiver \
  --target my_neural_rx_cuda \
  --global-name com.example.ran.neural-rx.cuda \
  --package-id "$PACKAGE_ID" \
  --vendor "Example Vendor" \
  --version 1 \
  --output "$HOME/src/my-neural-rx-dapp"
```

| `--kind` | Hook depth | Interface id | Replace |
|----------|------------|--------------|---------|
| `ce` | Channel estimate only | `0x00010003` | H + noise |
| `equalizer` | CE + EQ | `0x00010004` | compact symbols + post-EQ noise |
| `receiver` | CE + EQ + demap | `0x00010005` | compact FP16 LLRs (pre-descramble) |

What you get:

- Self-contained CMake project depending only on `OCUDUDAppSDK`.
- Stable query trampoline (`module.cpp`) and lifecycle shell (`module_support.cpp`).
- Algorithm file `algorithm.cu` (the only file you normally rewrite).
- Hash-bound **manifest-v1** and digest-bound **SPDX 2.3** SBOM emission on build.
- Clear BSD-3-Clause-Clear license headers matching the SDK style.

Initialize version control:

```bash
cd "$HOME/src/my-neural-rx-dapp"
git init
git add .
git commit -m "Initial Class-A CUDA receiver scaffold from OCUDU dApp SDK"
```

### 2.2 Alternative: copy a reference module inside a thin repo

When the scaffold does not cover your class (scheduler, spectrum, process worker):

```bash
mkdir -p "$HOME/src/my-spectrum-dapp"
# Copy one reference surface + the common helpers you need, then rewrite:
#   package UUID, global name, vendor, version, target names
# Keep the C ABI query pure; keep algorithm state in instance/worker contexts.
```

| Want… | Start from (SDK tree) |
|-------|------------------------|
| CPU Class A CE/EQ/RX | `reference/channel_estimator`, `equalizer`, `receiver` |
| CUDA Class A | scaffold **or** `reference/*_cuda` |
| Scheduler (Class B) | `reference/scheduler` + `docs/class_b_accelerator_authoring.md` + `docs/class_b_scheduler_baseline.md` |
| Spectrum native | `reference/spectrum` or `spectrum_cuda` |
| Spectrum process | `reference/process` + `ref_spectrumd_*` patterns |
| Observer-only spectrum | `ref_spectrum_observerd_cpu` (zero map/quiet caps) |
| SRS–ISAC | `reference/srs_isac` / `srs_isac_cuda` / process variants |

### 2.3 What not to put in the new repo

- OCUDU PHY implementation sources or private headers.
- Signing private keys (never in git, CI artifacts, or DU images).
- Absolute host paths inside manifests (the DU resolves catalog-relative names).
- Global mutable state shared across PUSCH lanes (use per-worker state).

---

## 3. Implement the dApp

### 3.1 Class-A CUDA hot path (receiver example)

Keep:

| File | Responsibility |
|------|----------------|
| `module.cpp` | Pure `ocudu_dapp_query_v1`; identity strings only |
| `module_support.cpp` | Lifecycle, workers, metrics, model slots, ABI dispatch |

Replace:

| File | Responsibility |
|------|----------------|
| `algorithm.cu` | Kernels + exported algorithm entry |

Invocation contract:

```text
on invoke:
  1. Validate every tensor (domain, type, rank, shape, stride, stream, counts, flags)
  2. Reject out-of-profile shapes and unknown completeness bits → UNSUPPORTED
  3. Enqueue only on invocation.execution_stream
  4. Write only caller-owned outputs
  5. Return without host-side stream synchronize, E3, alloc, or blocking I/O
```

Put weight upload, graph capture, and workspace allocation in **prepare** or
**model warm**, not in invoke. The reference receiver shows a two-slot
stage / validate / warm / activate / rollback model path; keep that shell if you
ship weights.

Admitted reference envelope (v1): up to **2 layers**, all QAM, DM-RS type 1/2,
DC-capable, **no transform precoding**. Outside the profile → conventional
fallback with host-owned reason metrics.

### 3.2 Class B (scheduler)

- Synchronous in-process host-record hook.
- May use CUDA **internally**, but still returns host-visible intent inside the
  deadline; no inherited PUSCH stream.
- Shipped reference (`ref_scheduler_cpu`) is a baseline-replica pipeline: rank,
  OLLA, table-aware MCS, multi-UE free-VRB pack, UL TPC — gated by host
  `scheduler_allow_*` / `allowed_output_flags`. See
  [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md).
- Prefer `scheduler_evaluation_only` in DU YAML while measuring (ACTIVE but
  conventional authority retained). See OCUDU operator docs.
- Before authoritative MCS/PRB/TPC: confirm host accepts
  `ALLOW_KNOWN_MASK` bits (not only avoid-mask) and publishes free-VRB when
  assignment is allowed; health metric is
  `class_b.policy.accepted ≈ invocations`.

### 3.3 Class C (spectrum / SRS)

- Producer never waits on the consumer.
- Native placement shares the DU address space; process placement uses
  credentialed rings (CPU snapshot or CUDA export pool).
- One active spectrum **result authority**; observers may coexist.
- Process workers should call `apply_restricted_process_policy()`, answer
  heartbeats, and release every lease before completion. Production profiles
  should require seccomp filter mode before route admission.

---

## 4. Build, test, and certify offline

### 4.1 Build your package against the install

```bash
cd "$HOME/src/my-neural-rx-dapp"
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$OCUDU_PREFIX" \
  -DCMAKE_CUDA_ARCHITECTURES=121
cmake --build build -j"$(nproc)"
```

Expected products (names follow `--target`):

```text
build/my_neural_rx_cuda.so
build/my_neural_rx_cuda.manifest.json
build/my_neural_rx_cuda.spdx.json
```

### 4.2 Offline positive certificate (Class A CUDA)

Use the certifier built with the reference SDK (or installed tools if packaged):

```bash
# Interface ids: CE 0x00010003, EQ 0x00010004, RX 0x00010005
"$SDK_BUILD/certify_class_a_cuda" \
  "$HOME/src/my-neural-rx-dapp/build/my_neural_rx_cuda.so" \
  0x00010005 \
  --model-artifact /path/to/weights.bin \   # when model-managed
  --model-id baseline --candidate-model-id candidate \
  --model-version 1 --model-generation 1
```

The JSON result covers lifecycle, concurrent workers/streams, min /
representative / max shapes, QAM × DM-RS matrix, grid-relative placement,
poisoned-buffer canaries, and clean stop/quiesce. Preserve it beside the
manifest as release evidence.

Optional sanitizer:

```bash
# memcheck: default boundary shape set
# racecheck: --shape-set compact (same concurrency, lighter fill)
compute-sanitizer --tool memcheck \
  "$SDK_BUILD/certify_class_a_cuda" build/my_neural_rx_cuda.so 0x00010005
```

### 4.3 Optional ABI fingerprint gate

On the SDK tree, release validation can freeze public LP64 layout:

```bash
cmake -S "$SDK_SRC" -B "$SDK_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$OCUDU_PREFIX" \
  -DOCUDU_DAPP_REQUIRE_ABI_COMPILER_MATRIX=ON
ctest --test-dir "$SDK_BUILD" -R '^abi\.public_v1_compiler_matrix$' --output-on-failure
```

---

## 5. Install the package into the OCUDU catalog

### 5.1 Stage immutable artifacts

Pick an operator-controlled catalog root. Copy **exactly** the three bound
products together:

```bash
export CATALOG=/opt/ocudu/dapps
sudo mkdir -p "$CATALOG"
sudo cp \
  "$HOME/src/my-neural-rx-dapp/build/my_neural_rx_cuda.so" \
  "$HOME/src/my-neural-rx-dapp/build/my_neural_rx_cuda.manifest.json" \
  "$HOME/src/my-neural-rx-dapp/build/my_neural_rx_cuda.spdx.json" \
  "$CATALOG/"
```

You may also stage reference packages from `$SDK_BUILD` for a multi-dApp DU.

### 5.2 Development: allow unsigned packages

Local labs may set `package_signature_mode: allow_unsigned`. A **present**
malformed or wrong-key signature still fails closed. Do not ship production
with this mode.

### 5.3 Production: sign and trust

Private keys stay offline (mode `0600`), never on the DU.

```bash
# One-time key material (offline host)
ocudu-dapp-package keygen \
  --private-key /secure/operator-dapp-key.pem \
  --public-key  /secure/operator-dapp-key.pub.pem

# Sign the exact staged manifest bytes
ocudu-dapp-package sign \
  --catalog-root "$CATALOG" \
  --manifest my_neural_rx_cuda.manifest.json \
  --private-key /secure/operator-dapp-key.pem \
  --package-sequence 1

# Trust policy with per-package sequence floors
# PACKAGE_ID must match the UUID inside the manifest.
ocudu-dapp-package trust \
  --output /etc/ocudu/dapp-trust.json \
  --policy-epoch 1 \
  --public-key /secure/operator-dapp-key.pub.pem \
  --package "${PACKAGE_ID}:1"

ocudu-dapp-package inspect \
  --catalog-root "$CATALOG" \
  --manifest my_neural_rx_cuda.manifest.json \
  --trust-store /etc/ocudu/dapp-trust.json \
  --minimum-policy-epoch 1
```

Full custody, two-key rollover, and revocation procedure:
OCUDU `dapp_release_provisioning` Doxygen page.

---

## 6. Enable dApps on the OCUDU gNB / DU

### 6.1 Build flags (reminder)

Without `-DENABLE_DAPP=ON` (and usually `-DENABLE_E3=ON` for management), the
DU has no dApp service. Absence of a `dapp:` YAML block leaves the modem path
unchanged even when the binary was built with the feature.

### 6.2 Minimum DU YAML

Add a `dapp:` section to the DU (or O-DU) configuration used by your gNB stack
(dynamic DU, split-8, or split-7.2 share the same schema):

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  # Lab only — production uses required + trust store (below).
  package_signature_mode: allow_unsigned
  package_manifests:
    - my_neural_rx_cuda.manifest.json
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 1000
```

Production-signed example:

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: required
  package_trust_store: /etc/ocudu/dapp-trust.json
  minimum_trust_policy_epoch: 1
  package_manifests:
    - my_neural_rx_cuda.manifest.json
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 1000
```

### 6.3 Declarative start at every DU boot (optional)

For packages that must be present without racing radio startup, use
`initial_instances`. Each entry names a manifest already listed under
`package_manifests`. Targets are only `shadow` or `active`. Startup is
synchronous, all-or-nothing, and runs **before** E3 ingress; any failure
unwinds every selected instance in reverse.

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - my_neural_rx_cuda.manifest.json
    - ref_spectrumd_cpu.manifest.json
  initial_instances:
    - package_manifest: my_neural_rx_cuda.manifest.json
      placement: in_process
      backend: cuda
      target: shadow          # inspect / model-stage before authority
      lifecycle_timeout_ms: 1000
    - package_manifest: ref_spectrumd_cpu.manifest.json
      placement: out_of_process
      backend: cpu
      target: active
      lifecycle_timeout_ms: 2000
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 1000
```

### 6.4 Class-B evaluation mode (optional)

```yaml
dapp:
  scheduler_evaluation_only: true
  # ... package_manifests including ref_scheduler_cpu.manifest.json ...
```

Evaluation mode is **not** lifecycle SHADOW: the instance is ACTIVE and bound,
but a valid dApp decision is discarded and the conventional scheduler remains
authoritative. Use it to measure before granting authority.

### 6.4.1 Full Class-B ownership (after evaluation looks clean)

```yaml
dapp:
  scheduler_evaluation_only: false
  scheduler_allow_mcs: true
  scheduler_allow_prb_limits: true
  scheduler_allow_prb_preference: true
  scheduler_allow_prb_assignment: true
  scheduler_allow_tpc: true
  scheduler_allow_layers: true
```

With `ref_scheduler_cpu` active, the package owns newTx MCS, multi-UE PRB
packing, and UL TPC (host still validates legality, retx, PDCCH). Expect
`class_b.policy.accepted ≈ invocations` and near-zero fallbacks. Algorithms and
host integrator checklist:
[class_b_scheduler_baseline.md](class_b_scheduler_baseline.md).

### 6.5 Start the DU

Start your usual OCUDU DU/gNB process with the updated YAML. Confirm the local
E3 socket exists and accepts inventory:

```bash
export OCUDU_DAPP_E3_SOCKET=/run/ocudu/dapp-management.sock
# Prefer the installed or in-tree Python client; C++ ocudu-dappctl is equivalent.
python3 /path/to/ocudu/apps/tools/dappctl/ocudu_dappctl.py \
  --socket "$OCUDU_DAPP_E3_SOCKET" \
  --schema /path/to/ocudu/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1 \
  inventory
```

You should see your package UUID in the package inventory before any load.

---

## 7. Enable and disable a dApp at runtime

Per-package cookbooks (every reference + example UUID, placement, offline
check, and multi-package sketches): **[run_examples.md](run_examples.md)**.

Mutations are **asynchronous jobs**. Every successful mutation returns a job id;
wait for terminal success and carry the returned **instance generation** into
the next call. Stale generations fail with conflict rather than applying twice.

### 7.1 Client conventions

```bash
DAPPCTL=(python3 /path/to/ocudu/apps/tools/dappctl/ocudu_dappctl.py
  --socket "$OCUDU_DAPP_E3_SOCKET"
  --schema /path/to/ocudu/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1
  --timeout-ms 2000)

# Inventory (packages + instances)
"${DAPPCTL[@]}" inventory

# Load creates an instance; placement/backend must match the signed manifest.
"${DAPPCTL[@]}" load \
  --package "$PACKAGE_ID" \
  --placement in-process \
  --backend cuda
# → note instanceId and instanceGeneration from the completed job
```

Lifecycle actions (C++ aliases in parentheses):

| Action | Meaning |
|--------|---------|
| `prepare` | Create module state; still unbound |
| `arm` | Worker readiness / binding prep |
| `shadow` (`enter-shadow`) | Warm without hook authority |
| `activate` (`enable`) | Bind hooks; dApp may affect the path |
| `deactivate` (`disable`) | Remove authority; keep instance |
| `unload` | Destroy instance after quiescence |
| `retry` (`retry-quiescence`) | Retry uncertain native quiescence |

### 7.2 Enable path (manual)

```bash
INSTANCE=...          # from load result
GEN=1                 # from each successive result

for action in prepare arm shadow activate; do
  "${DAPPCTL[@]}" lifecycle "$action" \
    --instance "$INSTANCE" \
    --expected-generation "$GEN" \
    --timeout-us 2000000
  # update GEN from the completed job's instanceGeneration
done
```

Recommended first enable target for new packages: stop at **shadow**, run
inventory / config / optional model stage, then `activate`.

### 7.3 Disable path

```bash
"${DAPPCTL[@]}" lifecycle deactivate \
  --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 2000000
# GEN := new generation

"${DAPPCTL[@]}" lifecycle unload \
  --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 2000000
```

If native quiescence is uncertain, **retry** quiescence; do not force unload of
resident code that may still be on a device stream. A maintenance DU restart is
preferred over uncertain `dlclose`.

### 7.4 Disable without deleting the catalog entry

- Runtime: `deactivate` (and optionally `unload`) leaves the package admitted in
  the catalog for a later load.
- Boot-time: remove the package from `initial_instances` and/or
  `package_manifests`, then restart the DU.
- Full removal: stop using the package, unload instances, then delete the three
  catalog files and update the trust policy floors when signed.

### 7.5 Model operations (receivers with weights)

While **shadow** or **active** (subject to package capabilities):

```bash
"${DAPPCTL[@]}" model stage \
  --instance "$INSTANCE" --expected-generation "$GEN" \
  --model neural-rx --version 1 --model-generation 1 \
  --artifact /secure/weights.bin
# then validate → warm → activate with returned model generations
```

---

## 8. Monitor the dApp

### 8.1 Health and jobs

```bash
"${DAPPCTL[@]}" health
"${DAPPCTL[@]}" jobs list
"${DAPPCTL[@]}" jobs get --job "$JOB_ID"
```

### 8.2 Metrics

```bash
# Learn names and units once per package generation
"${DAPPCTL[@]}" metrics schema --instance "$INSTANCE"

# Snapshot counters/gauges
"${DAPPCTL[@]}" metrics get --instance "$INSTANCE"
```

Host-owned Class-A signals (present even when the module never ran):

| Metric / signal | Meaning |
|-----------------|---------|
| `class_a.fallback.*` | Host bypassed before or instead of trusting a result |
| `class_a.completion.deadline_misses` | Soft GPU completion budget late; current result was not conventionally replayed (distinct from breaker-open) |
| `dapp.invoke.count` / `dapp.invoke.latency_ns` | Module-reported when honest |
| `dapp.breaker.open` | Lane breaker open after repeated faults |
| Scheduler UE `pusch_snr_db` (with equalizer active) | Should be **present** under UL: host derives SINR from mean post-EQ noise after equalizer-dApp success |

Equalizer packages must write finite post-EQ noise; optional host-filled
`dmrs.reserved[0]` DMRS symbol bitmask may be ignored. Soft host wall budgets
default near 2000 µs (demod) / 300 µs (CE); a worker latch opens after 8
consecutive recoverable deadline observations. Details:
[class_a_cuda_authoring.md](class_a_cuda_authoring.md) § Host CSI.

Class B (host-measured policy path) includes
`class_b.policy.invocations`, `class_b.policy.dapp_time_ns`,
`class_b.policy.deadline_misses`, `class_b.policy.fallbacks`, and related
counters.

Class C process health includes route publications, copy/export counters,
heartbeat progress, restart counts, and result protocol failures. Observer
workers should keep `process.results_accepted` and protocol-failure counters at
zero for map-shaped results.

### 8.3 Incidents

```bash
"${DAPPCTL[@]}" incidents list --instance "$INSTANCE" --limit 100
# Optional filter: --cause deadlineMiss|heartbeatLoss|policyRejection|...
```

Clear only after retention policy says so:

```bash
"${DAPPCTL[@]}" incidents clear --through "$SEQ"
```

### 8.4 Quiet reservations (spectrum / policy)

```bash
"${DAPPCTL[@]}" quiet-reservations --instance "$INSTANCE"
```

### 8.5 Portable Class-C E3 data (optional)

Independent consumers can subscribe to bounded host-snapshot spectrum/SRS
payloads when the DU enables the portable profile. See SDK README
`ref_spectrum_e3_cpu` / `ref_srs_isac_e3_cpu` and
`"${DAPPCTL[@]}" data subscribe|poll|list|remove`.

---

## 9. Verify it is working

Use this checklist in order. Stop at the first hard failure.

### 9.1 Package admission

- [ ] `inventory` lists the package with expected interface, placement, backend, and capabilities.
- [ ] Production: signature status **verified**; package sequence and policy epoch match trust store.
- [ ] Artifact SHA-256 and SBOM digest still match the staged files (`ocudu-dapp-package inspect`).

### 9.2 Lifecycle

- [ ] `load` → `prepare` → `arm` → `shadow` succeed with advancing generations.
- [ ] `activate` succeeds; inventory state is `active`.
- [ ] `jobs` show terminal `succeeded` for each mutation.

### 9.3 Data path / authority

| Class | Working means… |
|-------|----------------|
| **A** | `dapp.invoke.count` advances under UL traffic; `class_a.fallback.*` not spiking for in-profile traffic; completion deadline misses within your site policy |
| **B** | `class_b.policy.invocations` advances; zero unexpected validation rejections; eval mode vs authority mode understood |
| **C** authority | route publications and accepted results advance; single-writer map present when declared |
| **C** observer | publications consumed; **zero** map/result protocol emissions; heartbeats advance |

### 9.4 Failure containment (spot checks)

- [ ] Force a known bad config or unload path still leaves conventional PHY/scheduler healthy.
- [ ] Process worker kill (Class C) restarts within bounds; Class A continues if co-resident.
- [ ] `deactivate` / `unload` empty live authority without DU crash.

### 9.5 Optional automated gates

```bash
# SDK self-test of references and scaffolds
ctest --test-dir "$SDK_BUILD" --output-on-failure

# OCUDU dummy-RU campaigns (from OCUDU build tree; labels dapp;integration)
ctest --test-dir "$OCUDU_BUILD" -R '^dapp_dummy_ru_e2e\.' --output-on-failure

# Operator failure gallery (containment evidence, not latency)
ctest --test-dir "$OCUDU_BUILD" -R '^dapp_dummy_ru_e2e\.operator_failure_gallery$'
```

Class-A latency **release** admission uses
`<ocudu>/tests/benchmarks/dapp/run_class_a_cuda_qualification.py` (OCUDU host tree) with an
**operator-approved** policy. A development report that fails only because
controls are unapproved is evidence tooling, not a field pass.

### 9.6 Interpreting “it works” vs “it is production-qualified”

| Claim | Sufficient evidence |
|-------|---------------------|
| Functional drop-in | Certifier + inventory + activate + advancing metrics under traffic |
| Contained on failure | Incidents/breakers/fallback + failure gallery |
| Production tail / soak | Platform-scoped qualification reports with approved controls and multi-hour soak |

This quickstart gets you to a **working, operable** dApp. Production RF/latency
admission is a separate, explicit campaign.

---

## 10. End-to-end lab recipe (copy/paste)

Assumes OCUDU installed to `$OCUDU_PREFIX`, SDK at `$SDK_SRC`, CUDA SM 121.

```bash
set -euo pipefail
export OCUDU_PREFIX=${OCUDU_PREFIX:-$HOME/ocudu-install}
export SDK_SRC=${SDK_SRC:-$HOME/src/ocudu-dapp-sdk}
export SDK_BUILD=$SDK_SRC/build-cuda
export DAPP_SRC=$HOME/src/my-neural-rx-dapp
export CATALOG=${CATALOG:-/tmp/ocudu-dapps}
export PACKAGE_ID=$(uuidgen)

# 1) Scaffold
python3 "$SDK_SRC/tools/scaffold_class_a_cuda.py" \
  --kind receiver \
  --target my_neural_rx_cuda \
  --global-name com.example.ran.neural-rx.cuda \
  --package-id "$PACKAGE_ID" \
  --vendor "Example Vendor" \
  --version 1 \
  --output "$DAPP_SRC"

# 2) Build package (reference algorithm is already a valid drop-in)
#    Always Release for OTA — unoptimized Class-A misses PUSCH deadlines.
cmake -S "$DAPP_SRC" -B "$DAPP_SRC/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$OCUDU_PREFIX" \
  -DCMAKE_CUDA_ARCHITECTURES=121
cmake --build "$DAPP_SRC/build" -j"$(nproc)"

# 3) Certify
cmake --build "$SDK_BUILD" --target certify_class_a_cuda
"$SDK_BUILD/certify_class_a_cuda" \
  "$DAPP_SRC/build/my_neural_rx_cuda.so" 0x00010005 | tee "$DAPP_SRC/build/certify.json"

# 4) Stage catalog
mkdir -p "$CATALOG"
cp "$DAPP_SRC/build/my_neural_rx_cuda".{so,manifest.json,spdx.json} "$CATALOG/"

# 5) Point the DU YAML at $CATALOG + package_manifests + local_e3, start DU.
# 6) inventory → load → prepare → arm → shadow → activate → metrics get
echo "Package id: $PACKAGE_ID"
echo "Catalog:    $CATALOG"
```

Replace `algorithm.cu` only after the **unmodified scaffold package** has
completed steps 3–6 successfully. That isolates algorithm regressions from
packaging mistakes.

---

## 11. Troubleshooting

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| Package missing from inventory | Manifest not listed or catalog path wrong | Check `catalog_root` and `package_manifests` names (catalog-relative) |
| Load fails signature | Trust policy / sequence / epoch mismatch | `ocudu-dapp-package inspect`; raise sequence or update floors |
| Preflight timeout | CUDA init in query, or profiler attached | Keep query pure; avoid Nsight on preflight without raised diagnostic timeout |
| Activate fails second spectrum authority | Single-writer map rule | Deactivate existing authority first |
| High `class_a.fallback.*` | Profile mismatch, breaker open, unsupported shape | Metrics schema + incidents; narrow or fix profile |
| `class_a.completion.deadline_misses` only | Late GPU completion | Optimize kernels / clocks; breaker may open on repeat |
| Process restart storm | Crash or heartbeat miss | Incidents `processExit` / `heartbeatLoss`; check worker logs and limits |
| Stale generation conflict | Parallel clients or skipped GEN update | Re-inventory; retry with fresh transaction + expected generation |
| Unload hangs / quarantine | Uncertain native quiescence | `lifecycle retry`; maintenance restart if needed |

Operator failure gallery (deterministic containment campaign):

```bash
ctest --test-dir "$OCUDU_BUILD" -R '^dapp_dummy_ru_e2e\.operator_failure_gallery$'
```

---

## 12. Where to go next

| Need | Document |
|------|----------|
| Kernel-level Class-A rules | [class_a_cuda_authoring.md](class_a_cuda_authoring.md) |
| Scheduler backend rules | [class_b_accelerator_authoring.md](class_b_accelerator_authoring.md) |
| Scheduler baseline algorithms | [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md) |
| CUDA process Class C | [class_c_cuda_process_authoring.md](class_c_cuda_process_authoring.md) |
| Observer vs authority | [observer_authority.md](observer_authority.md) |
| Signing custody | OCUDU `dapp_release_provisioning` |
| Metrics semantics | OCUDU `dapp_observability` |
| Normative architecture | Design tree `ocudu_dapp_architecture_design.pdf` |

All OCUDU/DeepSig-owned package, ABI, E3, FlatBuffers, and evidence contracts
remain **v1** until a formal release decision. `SPDX-2.3` is the external SBOM
format identifier, not a local package version bump.
