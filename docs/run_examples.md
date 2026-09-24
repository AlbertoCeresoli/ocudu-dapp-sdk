<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Enable and run every example dApp

This page is the **operator cookbook** for the packages this SDK builds: how to
stage them, declare them in DU YAML, and enable/disable them over E3. Authoring
contracts live in the class-specific guides; the end-to-end mental model is
[ocudu_dapp_quickstart.md](ocudu_dapp_quickstart.md).

**Package identity:** stable UUIDs below are assigned in `CMakeLists.txt`. Always
confirm with the staged `.manifest.json` before load (rebuilds that mint new
IDs are a product decision — these references keep the same IDs).

---

## 0. Shared lab setup

### 0.1 Build the SDK artifacts

```bash
export HOST_SDK=/path/to/installed-host-dapp-sdk   # OCUDUDAppSDK
export SDK_SRC=/path/to/ocudu-dapp-sdk
export SDK_BUILD=$SDK_SRC/build-cuda               # or build-cpu

# Always use Release (-O3). Empty CMAKE_BUILD_TYPE builds unoptimized packages
# that miss PUSCH deadlines OTA (circuit breaker on Class-A CPU RX under load).
# CMakeLists defaults to Release when the type is unset, but pass it explicitly.

# CPU references
cmake -S "$SDK_SRC" -B "$SDK_SRC/build-cpu" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOST_SDK" \
  -DOCUDU_DAPP_REF_EXTRAS=ON
cmake --build "$SDK_SRC/build-cpu" -j

# CUDA references + vendor_min + (optional) Python Class-C
cmake -S "$SDK_SRC" -B "$SDK_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOST_SDK" \
  -DOCUDU_DAPP_BUILD_CUDA_EXAMPLES=ON \
  -DOCUDU_DAPP_REF_EXTRAS=ON \
  -DOCUDU_DAPP_BUILD_PYTHON_CLASS_C=ON \
  -DCMAKE_CUDA_ARCHITECTURES=native \
  -DPython3_EXECUTABLE=/path/to/venv/bin/python
cmake --build "$SDK_BUILD" -j
```

Each product package is three files next to each other:

```text
$SDK_BUILD/<stem>.so | <stem>          # MODULE .so or process executable
$SDK_BUILD/<stem>.manifest.json
$SDK_BUILD/<stem>.spdx.json
```

### 0.2 Stage into a catalog

```bash
export CATALOG=/opt/ocudu/dapps          # lab: any writable catalog root
sudo mkdir -p "$CATALOG"

# Example: stage one package (repeat per stem)
stem=ref_equalizer_cuda
sudo cp "$SDK_BUILD/${stem}.so" \
        "$SDK_BUILD/${stem}.manifest.json" \
        "$SDK_BUILD/${stem}.spdx.json" \
        "$CATALOG/"

# Process packages use an executable stem (no .so):
#   ref_spectrumd_cpu, class_c_python_spectrumd, …
```

Lab YAML usually uses `package_signature_mode: allow_unsigned`. Production must
sign with `ocudu-dapp-package` and a trust store (quickstart §5.3).

### 0.3 DU YAML skeleton

Host gNB/DU must be built with `-DENABLE_DAPP=ON` (and usually `-DENABLE_E3=ON`).

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - ref_equalizer_cuda.manifest.json   # catalog-relative names
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 2000
```

Restart the DU after changing `package_manifests` / `catalog_root`.

### 0.4 E3 client helper

```bash
export OCUDU_DAPP_E3_SOCKET=/run/ocudu/dapp-management.sock
export OCUDU_SRC=/path/to/ocudu

DAPPCTL=(python3 "$OCUDU_SRC/apps/tools/dappctl/ocudu_dappctl.py"
  --socket "$OCUDU_DAPP_E3_SOCKET"
  --schema "$OCUDU_SRC/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1"
  --timeout-ms 3000)

"${DAPPCTL[@]}" inventory
```

### 0.5 Standard lifecycle (all catalog packages)

```bash
PACKAGE_ID=<uuid-from-table-below>
PLACEMENT=in-process          # or out-of-process
BACKEND=cpu                   # or cuda

# 1) Load → note instanceId + instanceGeneration from the job result
"${DAPPCTL[@]}" load \
  --package "$PACKAGE_ID" \
  --placement "$PLACEMENT" \
  --backend "$BACKEND"

INSTANCE=<instance-id>
GEN=<generation-from-load>

# 2) Walk to authority (update GEN after every successful job)
for action in prepare arm shadow activate; do
  "${DAPPCTL[@]}" lifecycle "$action" \
    --instance "$INSTANCE" \
    --expected-generation "$GEN" \
    --timeout-us 5000000
  # GEN := completed job's instanceGeneration
done

# 3) Confirm
"${DAPPCTL[@]}" inventory

# 4) Disable later
"${DAPPCTL[@]}" lifecycle deactivate \
  --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 5000000
# GEN := new generation
"${DAPPCTL[@]}" lifecycle unload \
  --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 5000000
```

**Process packages after `deactivate`:** some hosts require `unload` then a
fresh `load` before the next `prepare` (inactive process instances cannot be
re-prepared). Prefer unload when in doubt.

**Boot-time enable** (no manual load): use `initial_instances` with
`target: shadow` or `target: active` (quickstart §6.3).

---

## 1. Class A — resident L1 (in-process)

Class A replaces a slice of the **UL PUSCH** path inside the DU process.
**Placement is always `in-process`.** Only **one** Class-A depth should own a
given UL path at a time (CE-only vs CE+EQ vs full receiver). Prefer CUDA on GPU
DUs; CPU packages for integration without a GPU.

Authoring depth: [class_a_cuda_authoring.md](class_a_cuda_authoring.md).

| Artifact | Package UUID | Interface | Placement / backend | Offline check |
|----------|--------------|-----------|---------------------|---------------|
| `ref_channel_estimator_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000003` | `0x00010003` | in-process / cpu | module contract |
| `ref_equalizer_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000004` | `0x00010004` | in-process / cpu | module contract |
| `ref_receiver_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000005` | `0x00010005` | in-process / cpu | module contract |
| `ref_channel_estimator_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000203` | `0x00010003` | in-process / cuda | `certify … 0x00010003` |
| `ref_equalizer_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000204` | `0x00010004` | in-process / cuda | `certify … 0x00010004` |
| `ref_receiver_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000205` | `0x00010005` | in-process / cuda | `certify … 0x00010005` |
| `ref_srs_channel_estimator_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000007` | `0x00010007` | in-process / cpu | module contract, `ocudu-dapp-srs-replay --check` |
| `ref_srs_channel_estimator_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000207` | `0x00010007` | in-process / cuda | `reference_srs_channel_estimator_cuda_test` |

### 1.1 Offline (no DU) — certify before catalog

```bash
# Equalizer CUDA (typical lab package)
$SDK_BUILD/certify_class_a_cuda $SDK_BUILD/ref_equalizer_cuda.so 0x00010004

# CE-only / full receiver
$SDK_BUILD/certify_class_a_cuda $SDK_BUILD/ref_channel_estimator_cuda.so 0x00010003
$SDK_BUILD/certify_class_a_cuda $SDK_BUILD/ref_receiver_cuda.so 0x00010005

# L1 validation suite (EQ + RX chain, noisy synthetic PUSCH)
$SDK_BUILD/l1_validation_suite --mode all --quick \
  --eq-so $SDK_BUILD/ref_equalizer_cuda.so \
  --rx-so $SDK_BUILD/ref_receiver_cuda.so

# CPU packages: module contract / unit tests
ctest --test-dir "$SDK_BUILD" -R 'ref_equalizer_cpu|ref_receiver_cpu|ref_channel' \
  --output-on-failure
```

### 1.2 Stage + DU YAML — equalizer CUDA (typical)

```bash
stem=ref_equalizer_cuda
sudo cp "$SDK_BUILD/${stem}.so" \
        "$SDK_BUILD/${stem}.manifest.json" \
        "$SDK_BUILD/${stem}.spdx.json" \
        "$CATALOG/"
```

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - ref_equalizer_cuda.manifest.json
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 2000
  # Boot-time enable (optional — skip manual load):
  # initial_instances:
  #   - package_manifest: ref_equalizer_cuda.manifest.json
  #     placement: in_process
  #     backend: cuda
  #     target: shadow          # inspect first, then activate over E3
  #     # target: active        # bind on DU start
```

Restart the DU after YAML changes. Confirm inventory lists the package UUID.

### 1.3 Enable over E3 — full copy-paste (equalizer CUDA)

```bash
# DAPPCTL from §0.4
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000204

"${DAPPCTL[@]}" inventory

# Load (placement/backend must match the manifest)
JOB=$("${DAPPCTL[@]}" load \
  --package "$PACKAGE_ID" \
  --placement in-process \
  --backend cuda)
# Parse INSTANCE and GEN from the completed job JSON (field names vary by client
# version: instanceId / instance_id, instanceGeneration / instance_generation).
INSTANCE=<instance-id-from-job>
GEN=<generation-from-job>

for action in prepare arm shadow; do
  "${DAPPCTL[@]}" lifecycle "$action" \
    --instance "$INSTANCE" \
    --expected-generation "$GEN" \
    --timeout-us 5000000
  # GEN := completed job's instanceGeneration after each step
done

# Optional: inspect metrics / config while in shadow, then:
"${DAPPCTL[@]}" lifecycle activate \
  --instance "$INSTANCE" \
  --expected-generation "$GEN" \
  --timeout-us 5000000

"${DAPPCTL[@]}" inventory
```

### 1.4 Other Class-A packages (same lifecycle)

| Package | UUID | load flags |
|---------|------|------------|
| `ref_channel_estimator_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000203` | `--placement in-process --backend cuda` |
| `ref_receiver_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000205` | `--placement in-process --backend cuda` |
| `ref_equalizer_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000004` | `--placement in-process --backend cpu` |
| `ref_channel_estimator_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000003` | `--placement in-process --backend cpu` |
| `ref_receiver_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000005` | `--placement in-process --backend cpu` |
| `ref_srs_channel_estimator_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000207` | `--placement in-process --backend cuda` |
| `ref_srs_channel_estimator_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000007` | `--placement in-process --backend cpu` |

The SRS channel estimator occupies its own hook, next to any PUSCH Class-A
package: on an accelerated gNB load the CUDA package, on a host-L1 gNB the CPU
one. Its `dapp.invoke.count` counts SRS occasions.

Stage the matching `.so` + manifest + SBOM; put that manifest in
`package_manifests`; use the row’s UUID and load flags.

### 1.5 Disable Class A

```bash
"${DAPPCTL[@]}" lifecycle deactivate \
  --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 5000000
# GEN := new generation
"${DAPPCTL[@]}" lifecycle unload \
  --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 5000000
```

If native quiescence is uncertain, prefer `lifecycle retry` and a maintenance
DU restart over force-unloading code that may still be on a device stream.

### 1.6 Receiver model ops (optional)

While **shadow** or **active** (package must declare model capability):

```bash
$SDK_BUILD/make_receiver_cuda_model /tmp/receiver-model.bin 0.75

"${DAPPCTL[@]}" model stage \
  --instance "$INSTANCE" --expected-generation "$GEN" \
  --model receiver-levels --version 1 --model-generation 1 \
  --artifact /tmp/receiver-model.bin
# then validate → warm → activate with returned model generations
```

### 1.7 What “working” looks like

| Signal | Expectation |
|--------|-------------|
| Inventory | `state=active`, package UUID present |
| Offline | `certify_class_a_cuda` JSON success before first load |
| Live UL | Class-A metrics advance; host `class_a.fallback.*` near zero when admitted |
| Failures | Wrong backend/placement → load reject; second Class-A depth may conflict — deactivate the other first |

---

## 2. Class A example — `vendor_min` / `example_eq_cuda`

Copy-me commercial MODULE (thin algorithm + `package_config`). Same **in-process
/ cuda** lifecycle as `ref_equalizer_cuda`.

| Artifact | Package UUID | Interface | Placement / backend |
|----------|--------------|-----------|---------------------|
| `example_eq_cuda` | `8f0d0001-4f9c-4e31-8c00-00000000e001` | `0x00010004` | in-process / cuda |

```bash
# Offline
ctest --test-dir "$SDK_BUILD" -R 'example_eq_cuda' --output-on-failure
$SDK_BUILD/certify_class_a_cuda $SDK_BUILD/example_eq_cuda.so 0x00010004 --shape-set compact

# Stage
sudo cp $SDK_BUILD/example_eq_cuda.{so,manifest.json,spdx.json} "$CATALOG/"

# Enable (YAML package_manifests: example_eq_cuda.manifest.json)
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-00000000e001
"${DAPPCTL[@]}" load --package "$PACKAGE_ID" --placement in-process --backend cuda
# prepare → arm → shadow → activate  (same loop as §1.3)
```

Config knobs via E3 (`demo_scale`, instance `enabled`) — see
[package_config.md](package_config.md) and
[examples/vendor_min/README.md](../examples/vendor_min/README.md).

---

## 3. Class B — scheduler (`ref_scheduler_cpu`)

Class B is the **deadline-bound scheduler** hook (~100 µs). The only shipped
reference is **CPU in-process**. It emits **intents** (MCS / PRB / TPC / …);
the host still validates and owns retx / PDCCH / legality.

Authoring: [class_b_accelerator_authoring.md](class_b_accelerator_authoring.md) ·
algorithms: [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md).

| Artifact | Package UUID | Interface | Placement / backend |
|----------|--------------|-----------|---------------------|
| `ref_scheduler_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000006` | `0x00010006` | in-process / cpu |

### 3.1 Stage + offline smoke

```bash
stem=ref_scheduler_cpu
sudo cp "$SDK_BUILD/${stem}.so" \
        "$SDK_BUILD/${stem}.manifest.json" \
        "$SDK_BUILD/${stem}.spdx.json" \
        "$CATALOG/"

ctest --test-dir "$SDK_BUILD" -R 'scheduler|ref_scheduler' --output-on-failure
```

### 3.2 Evaluation first (recommended before granting authority)

Host **invokes** the dApp and **discards** valid decisions — conventional
scheduler remains authoritative. This is **not** lifecycle SHADOW.

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - ref_scheduler_cpu.manifest.json
  scheduler_evaluation_only: true
  # Still advertise allow bits so the package can exercise full intents:
  scheduler_allow_mcs: true
  scheduler_allow_prb_limits: true
  scheduler_allow_prb_preference: true
  scheduler_allow_prb_assignment: true
  scheduler_allow_tpc: true
  scheduler_allow_layers: true
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 2000
  # optional boot path:
  # initial_instances:
  #   - package_manifest: ref_scheduler_cpu.manifest.json
  #     placement: in_process
  #     backend: cpu
  #     target: active
```

Restart the DU, then enable:

```bash
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000006

"${DAPPCTL[@]}" load \
  --package "$PACKAGE_ID" \
  --placement in-process \
  --backend cpu
# INSTANCE / GEN from job

for action in prepare arm shadow activate; do
  "${DAPPCTL[@]}" lifecycle "$action" \
    --instance "$INSTANCE" \
    --expected-generation "$GEN" \
    --timeout-us 5000000
  # GEN := new generation each step
done

"${DAPPCTL[@]}" inventory
```

**What “working” looks like in evaluation:**

| Metric (host) | Expectation |
|---------------|-------------|
| `class_b.policy.invocations` | Advances under scheduler load |
| `class_b.policy.dapp_time_ns` | Sampled; budget ~100 µs complete boundary |
| Valid intents | Validated then **discarded** (conventional still grants) |
| `class_b.policy.fallbacks` / validation rejections | Near zero if package healthy |

### 3.3 Full ownership (after evaluation looks clean)

Turn off evaluation and keep allow flags so the package **owns** newTx MCS,
multi-UE PRB packing, and UL TPC (host still clamps legality / retx / PDCCH):

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - ref_scheduler_cpu.manifest.json
  scheduler_evaluation_only: false
  scheduler_allow_mcs: true
  scheduler_allow_prb_limits: true
  scheduler_allow_prb_preference: true
  scheduler_allow_prb_assignment: true
  scheduler_allow_tpc: true
  scheduler_allow_layers: true
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 2000
```

Restart the DU (YAML change). If the instance was unloaded, load + lifecycle
again (§3.2 commands). If it stayed loaded across restart via
`initial_instances`, inventory should show `active`.

**What “working” looks like with authority:**

| Metric | Expectation |
|--------|-------------|
| `class_b.policy.accepted` | ≈ `invocations` |
| `class_b.policy.fallbacks` | Near zero |
| `class_b.policy.validation_rejections` | Near zero |
| `class_b.policy.deadline_misses` | Near zero |
| Live grants | MCS / PRB / TPC reflect dApp intents under host clamps |

Default production without allow flags: package may only influence **priority**
until `scheduler_allow_*` / guardrails open more bits (baseline guide §9).

### 3.4 Disable Class B

```bash
"${DAPPCTL[@]}" lifecycle deactivate \
  --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 5000000
"${DAPPCTL[@]}" lifecycle unload \
  --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 5000000
```

Or remove from `package_manifests` / `initial_instances` and restart the DU.
Conventional scheduler remains the fallback when the dApp is absent.

### 3.5 Class-B notes operators hit

1. **`scheduler_allow_*` alone is not enough** — the host must also advertise
   those bits in `allowed_output_flags` and accept known allow bits in the
   native invoke gate (see baseline guide §9.1).
2. **`ALLOW_PRB_ASSIGNMENT` needs free-VRB context** after retx; without it the
   host strips assignment for that invoke.
3. **Evaluation ≠ shadow** — evaluation still binds the callback; shadow does not.
4. **No process Class-B authority** in v1 for the current slot — always
   `in-process`.

---

## 4. Class C — spectrum / SRS (native + process)

Exactly **one** spectrum **result authority** may be active (map/quiet
capabilities). Observers may coexist. See
[observer_authority.md](observer_authority.md).

| Artifact | UUID suffix | Placement / backend | Role |
|----------|-------------|---------------------|------|
| `ref_spectrum_cpu` | `…000001` | in-process / cpu | Map authority (native) |
| `ref_srs_isac_cpu` | `…000002` | in-process / cpu | CIR preview |
| `ref_spectrum_cuda` | `…000201` | in-process / cuda | Device radiometer |
| `ref_srs_isac_cuda` | `…000202` | in-process / cuda | Device CIR |
| `ref_spectrumd_cpu` | `…000101` | **out-of-process** / cpu | SHM process authority |
| `ref_srs_isacd_cpu` | `…000102` | **out-of-process** / cpu | SHM process CIR |
| `ref_spectrum_observerd_cpu` | `…000103` | **out-of-process** / cpu | Observer (no map/quiet) |
| `ref_spectrumd_cuda` | `…000301` | **out-of-process** / cuda | CUDA-IPC spectrum |
| `ref_srs_isacd_cuda` | `…000302` | **out-of-process** / cuda | CUDA-IPC SRS |

Prefix for all: `8f0d0001-4f9c-4e31-8c00-`.

### 4.1 Process spectrum authority (CPU) — recommended lab path

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - ref_spectrumd_cpu.manifest.json
    - ref_srs_isacd_cpu.manifest.json
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
```

```bash
# Spectrum authority
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000101
# load --placement out-of-process --backend cpu → prepare → arm → shadow → activate

# SRS CIR (can run alongside spectrum)
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000102
# load --placement out-of-process --backend cpu → … → activate
```

**Offline process smoke** (no radio):

```bash
# Host-side process client test (build with BUILD_TESTING)
ctest --test-dir "$SDK_BUILD" -R 'process_client' --output-on-failure
```

**What “working” looks like:** process starts once; heartbeats advance; spectrum
publications / accepted results advance under UL; SRS CIR previews advance when
UEs transmit SRS.

### 4.2 Observer-only spectrum process

```bash
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000103
# load --placement out-of-process --backend cpu → activate
# Inventory: zero map results; no authority conflict with another observer
```

Do **not** activate a second map-capable spectrum package while one authority
is already active.

### 4.3 Native in-process Class-C

```bash
# CPU spectrum MODULE
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000001
# load --placement in-process --backend cpu

# CUDA spectrum MODULE
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000201
# load --placement in-process --backend cuda
```

### 4.4 Portable E3 consumers (not catalog packages)

These binaries do **not** go through `load` / lifecycle. They attach to the DU
portable-E3 socket as independent clients.

```bash
# After DU is up with portable E3 enabled for Class-C snapshots:
$SDK_BUILD/ref_spectrum_e3_cpu --help     # if built/installed
$SDK_BUILD/ref_srs_isac_e3_cpu --help
```

Python portable E3: [§5.3](#53-portable-e3-independent-client) below.

---

## 5. Python Class-C examples (research / education)

See also [examples/class_c_e3_python/README.md](../examples/class_c_e3_python/README.md).

| Artifact | Package UUID | Placement / backend | Role |
|----------|--------------|---------------------|------|
| `class_c_python_spectrumd` | `8f0d0001-4f9c-4e31-8c00-00000000e101` | out-of-process / cpu | PyTorch radiometer |
| `class_c_python_srs_isacd` | `8f0d0001-4f9c-4e31-8c00-00000000e102` | out-of-process / cpu | PyTorch CIR preview |

**Constraints:** Class-C **out-of-process** only; not production performance
(prefer `ref_spectrumd_*` / `ref_srs_isacd_*`).

### 5.1 Offline algorithms (no DU)

```bash
cd $SDK_SRC/examples/class_c_e3_python
python3 sensing/app.py --mode offline --device cpu
python3 -m unittest discover -s tests -v
```

### 5.2 Supervised process packages on the DU

```bash
# Bake lab log paths if DU spawn strips env (recommended for OTA trees):
cmake -S "$SDK_SRC" -B "$SDK_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DOCUDU_DAPP_PYTHON_SPECTRUM_LOG=/var/log/ocudu/python_spectrum.jsonl \
  -DOCUDU_DAPP_PYTHON_SRS_LOG=/var/log/ocudu/python_srs_isac.jsonl
cmake --build "$SDK_BUILD" -j --target \
  ocudu_dapp_class_c_process \
  class_c_python_spectrumd class_c_python_srs_isacd \
  class_c_python_spectrumd_package class_c_python_srs_isacd_package

sudo cp $SDK_BUILD/class_c_python_spectrumd \
        $SDK_BUILD/class_c_python_spectrumd.manifest.json \
        $SDK_BUILD/class_c_python_spectrumd.spdx.json \
        $SDK_BUILD/class_c_python_srs_isacd \
        $SDK_BUILD/class_c_python_srs_isacd.manifest.json \
        $SDK_BUILD/class_c_python_srs_isacd.spdx.json \
        "$CATALOG/"
# Also ensure the baked Python/venv can import torch + ocudu_dapp_class_c_process
# (PYTHONPATH / LD_LIBRARY_PATH are compile-time baked into the launcher).
```

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - class_c_python_spectrumd.manifest.json
    - class_c_python_srs_isacd.manifest.json
  # Process heartbeats need enough headroom under dense UL:
  # (host-specific knobs — see your gNB dapp process timeouts)
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
```

```bash
# Spectrum (default observer-only sensing + JSONL unless you opt into results)
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-00000000e101
# load --placement out-of-process --backend cpu → prepare → arm → shadow → activate

# SRS CIR
PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-00000000e102
# load --placement out-of-process --backend cpu → … → activate
```

**JSONL logs:** path baked at build time (or `OCUDU_DAPP_LOG_FILE` if env is not
stripped). Spectrum logs periodically; SRS more often. Expect
`event=started` with `research_only: true`, then `publication` lines with
torch radiometer / CIR summaries.

**Spectrum host results (optional):** only if you intentionally want map
results and understand authority — set env inside the worker packaging or use a
native package instead. This example defaults **observer-only** for spectrum
unless `OCUDU_DAPP_SPECTRUM_SEND_RESULTS=1` is available to the worker.

### 5.3 Portable E3 independent client

Not a catalog package — no `load` lifecycle.

```bash
export PYTHONPATH=$SDK_BUILD:$PYTHONPATH
python3 $SDK_SRC/examples/class_c_e3_python/sensing/app.py --mode live \
  --socket /run/ocudu/dapp-e3.sock \
  --sample-every 10 \
  --device cpu \
  --max-objects 20
```

Requires host E3 libs built with `POSITION_INDEPENDENT_CODE` and a DU that
emits portable Class-C snapshots.

---

## 6. Python E3 examples (out of process, no catalog package)

Four example directories connect to a running gNB over the **E3AP SCTP
endpoint** as independent client processes. They are **not catalog packages**:
no manifest, no `load` / lifecycle, no DU restart — start and stop them like
any other program.

| Directory | What it is |
|-----------|------------|
| [`examples/e3_telemetry_python`](../examples/e3_telemetry_python/README.md) | Six observer scripts: catalog, PUSCH quality, SRS watch, dApp health, full-rate HDF5/npz recording, PUSCH burst export |
| [`examples/e3_spectrum_sensing`](../examples/e3_spectrum_sensing/README.md) | `emitter_detector.py` — uplink-grid emitter detection from `classc.spectrum` |
| [`examples/e3_isac_sensing`](../examples/e3_isac_sensing/README.md) | `srs_isac_monitor.py` — range-Doppler pipeline on the SRS channel response |
| [`examples/e3_mcp_server`](../examples/e3_mcp_server/README.md) | `ocudu_mcp_server.py` — MCP server exposing the gNB to an AI assistant |
| [`examples/e3_receiver_routing`](../examples/e3_receiver_routing/README.md) | `sinr_router.py` — routes each UE's PUSCH to one of two receivers (or the stock receiver) by uplink SINR through the gNB's per-UE route table |
| [`examples/e3_srs_dataset`](../examples/e3_srs_dataset/README.md) | `capture_srs_dataset.py` — SRS channel-estimation datasets (the SRS estimator dApp input plus the gNB's answer) from a live cell; `export_srs_vectors.py` + `ocudu-dapp-srs-replay` replay them through the reference estimator |

### 6.1 Prerequisites

- **gNB side:** DU YAML enables the E3AP SCTP endpoint (`dapp.e3ap_sctp`,
  default port 36423 — see the OCUDU `docs/dapp/yaml_configuration.md`). For a
  radio-free dry run, the OCUDU build's loopback agent `e3ap_test_server`
  serves the same protocol.
- **Python side:** Python ≥ 3.10 with `asn1tools` (all examples), `numpy`
  (spectrum, ISAC, MCP server, and the telemetry recorder/exporter), `h5py`
  (HDF5 recording / burst export), and `flatbuffers` (Class-C payload decoding
  in the spectrum and ISAC examples).
- **Kernel SCTP:** the client opens a plain
  `socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP)` (`ocudu_e3/sctp.py`, standard
  library only), so the kernel must provide SCTP (Linux: `sctp` module,
  `modprobe sctp` if `/proc/sys/net/sctp` is absent).

### 6.2 Locating the `ocudu_e3` client

Every example ships a `_common.py` (see
`examples/e3_telemetry_python/_common.py`) that searches, in order: the
`OCUDU_E3_PYTHON_DIR` environment variable, a sibling `ocudu/python` checkout,
then installed prefixes (`<prefix>/share/ocudu/dapp/python`). The ASN.1
schemas are found through `OCUDU_E3_SCHEMA_DIR` or the same install prefix.

```bash
export OCUDU_E3_PYTHON_DIR=/path/to/ocudu/python
export OCUDU_E3_SCHEMA_DIR=/path/to/ocudu/schemas/e3
```

### 6.3 One command per example

```bash
cd $SDK_SRC/examples

# Telemetry catalog (then watch_pusch_quality.py, record_telemetry.py, …)
python3 e3_telemetry_python/e3_catalog.py --host 127.0.0.1 --port 36423

# Uplink emitter detection on the Class-C spectrum feed
python3 e3_spectrum_sensing/emitter_detector.py --threshold-db 8 --min-slots 3

# SRS range-Doppler monitor
python3 e3_isac_sensing/srs_isac_monitor.py --cpi 64 --threshold-db 12

# MCP server for an AI assistant (stdio; e.g. `claude mcp add ocudu -- …`)
python3 e3_mcp_server/ocudu_mcp_server.py --host 127.0.0.1 --port 36423

# Per-UE receiver routing by SINR (both instances active; see its README)
python3 e3_receiver_routing/sinr_router.py --below <instance-A> --above conventional --threshold-db 12
```

### 6.4 Tests

Each directory has a `tests/` package runnable offline:

```bash
cd $SDK_SRC/examples/e3_telemetry_python   # or e3_spectrum_sensing, e3_isac_sensing, e3_mcp_server, e3_receiver_routing
python3 -m unittest discover -s tests -v
```

The same tests are registered with CTest (`ctest -R example.e3_` in a
configured build). Setting `OCUDU_E3AP_TEST_SERVER` to the OCUDU loopback
agent additionally runs a live catalog/subscription round trip.

### 6.5 Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `Connection refused` on connect | gNB not running, or `dapp.e3ap_sctp` not enabled in the DU YAML; confirm host/port and that the endpoint is configured |
| `Protocol not supported` / SCTP socket error | Kernel SCTP unavailable — load the `sctp` module (or install kernel SCTP support); containers may need it on the host |
| `RuntimeError: ocudu_e3.classc requires the 'flatbuffers' package` | Spectrum / ISAC examples decode Class-C FlatBuffers payloads — `pip install flatbuffers` |
| `SystemExit: ocudu_e3 not found` | Set `OCUDU_E3_PYTHON_DIR` (§6.2) and install `asn1tools` |
| `note: <stream> hard floor is N ms; requests below it are clamped` (stderr) | Informational from `record_telemetry.py`: the agent advertises a hard floor for the stream and clamps faster requests — the capture still runs at the floor rate |
| Scripts connect but print nothing | Telemetry is UE-driven: with no UE attached (or none matching an `--rnti` filter) `pusch.*` / `srs.*` / Class-C streams publish nothing — attach a UE and generate uplink traffic |

---

## 7. Multi-package lab sketches

### 7.1 Class-A EQ CUDA + process spectrum observer

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - ref_equalizer_cuda.manifest.json
    - ref_spectrum_observerd_cpu.manifest.json
  initial_instances:
    - package_manifest: ref_equalizer_cuda.manifest.json
      placement: in_process
      backend: cuda
      target: active
    - package_manifest: ref_spectrum_observerd_cpu.manifest.json
      placement: out_of_process
      backend: cpu
      target: active
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
```

### 7.2 Scheduler evaluation + SRS process

```yaml
dapp:
  scheduler_evaluation_only: true
  package_manifests:
    - ref_scheduler_cpu.manifest.json
    - ref_srs_isacd_cpu.manifest.json
  # load/activate both manually, or use initial_instances
```

### 7.3 Python Class-C pair (spectrum + SRS)

```yaml
dapp:
  package_manifests:
    - class_c_python_spectrumd.manifest.json
    - class_c_python_srs_isacd.manifest.json
  # Prefer manual activate after inventory confirms both packages, or:
  # initial_instances with target: active for each
```

---

## 8. Verify and troubleshoot

| Check | How |
|-------|-----|
| Package admitted | `"${DAPPCTL[@]}" inventory` shows package UUID |
| Instance active | inventory `state=active`, generation advances on lifecycle |
| Class A live | UL traffic + certifier already green offline; host Class-A metrics |
| Class B live | evaluation first; then `class_b.policy.accepted` under traffic |
| Class C process | worker PID stable; heartbeats; publications/results advance |
| Python process | JSONL `started` + `publication`; no heartbeat SIGKILL |
| Authority conflict | deactivate other spectrum map package first |

Common failures (quickstart § troubleshooting): wrong placement/backend vs
manifest, stale generation, second spectrum authority, process heartbeat
timeout, catalog path not matching staged files.

---

## 9. Quick identity cheat sheet

| Stem | UUID | load placement | load backend |
|------|------|----------------|--------------|
| `ref_channel_estimator_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000003` | in-process | cpu |
| `ref_equalizer_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000004` | in-process | cpu |
| `ref_receiver_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000005` | in-process | cpu |
| `ref_srs_channel_estimator_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000007` | in-process | cpu |
| `ref_scheduler_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000006` | in-process | cpu |
| `ref_spectrum_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000001` | in-process | cpu |
| `ref_srs_isac_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000002` | in-process | cpu |
| `ref_spectrumd_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000101` | out-of-process | cpu |
| `ref_srs_isacd_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000102` | out-of-process | cpu |
| `ref_spectrum_observerd_cpu` | `8f0d0001-4f9c-4e31-8c00-000000000103` | out-of-process | cpu |
| `ref_channel_estimator_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000203` | in-process | cuda |
| `ref_equalizer_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000204` | in-process | cuda |
| `ref_receiver_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000205` | in-process | cuda |
| `ref_srs_channel_estimator_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000207` | in-process | cuda |
| `ref_spectrum_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000201` | in-process | cuda |
| `ref_srs_isac_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000202` | in-process | cuda |
| `ref_spectrumd_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000301` | out-of-process | cuda |
| `ref_srs_isacd_cuda` | `8f0d0001-4f9c-4e31-8c00-000000000302` | out-of-process | cuda |
| `example_eq_cuda` | `8f0d0001-4f9c-4e31-8c00-00000000e001` | in-process | cuda |
| `class_c_python_spectrumd` | `8f0d0001-4f9c-4e31-8c00-00000000e101` | out-of-process | cpu |
| `class_c_python_srs_isacd` | `8f0d0001-4f9c-4e31-8c00-00000000e102` | out-of-process | cpu |

---

## Related

| Doc | Role |
|-----|------|
| [ocudu_dapp_quickstart.md](ocudu_dapp_quickstart.md) | Full walkthrough + signing + disable |
| [reference_packages.md](reference_packages.md) | Inventory tables |
| [observer_authority.md](observer_authority.md) | Spectrum single-writer |
| [class_a_cuda_authoring.md](class_a_cuda_authoring.md) | Class-A authoring |
| [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md) | Class-B knobs |
| [class_c_cuda_process_authoring.md](class_c_cuda_process_authoring.md) | Class-C process / CUDA-IPC |
| [tools.md](tools.md) | Certifier, scaffold, host tools |
| [examples/class_c_e3_python/README.md](../examples/class_c_e3_python/README.md) | Python authoring detail |
| [examples/vendor_min/README.md](../examples/vendor_min/README.md) | Vendor MODULE template |
