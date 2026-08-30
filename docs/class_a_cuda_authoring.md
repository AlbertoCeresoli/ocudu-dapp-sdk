<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Copying a resident Class-A CUDA dApp

For the end-to-end path from a new repository through DU catalog install,
enable/disable, monitoring, and verification, see
[`ocudu_dapp_quickstart.md`](ocudu_dapp_quickstart.md). **Copy-paste enable/run
for every Class-A reference package (CPU + CUDA):**
[`run_examples.md`](run_examples.md) §1–§2.

This page is the Class-A CUDA contract depth for authors who already have a
scaffolded project.

**Multi-vendor commercial start:** prefer
[`examples/vendor_min`](../examples/vendor_min/README.md) +
[`vendor_toolkit.md`](vendor_toolkit.md) +
[`package_config.md`](package_config.md) (`find_package(OCUDUDapp)`, shared
management). Use the scaffold below when you need a self-contained tree that
depends only on host `OCUDUDAppSDK`.

Choose the narrowest replacement boundary that contains the new algorithm:

| Starting artifact | Replace | Required output |
|---|---|---|
| `ref_channel_estimator_cuda` | channel estimation | `[port, layer, symbol, subcarrier]` CF32 channel and `[port, layer]` F32 noise |
| `ref_equalizer_cuda` | channel estimation and equalization | `[data_re, layer]` CF32 symbols and **F32 post-equalization noise** (required; see host CSI below) |
| `ref_receiver_cuda` | channel estimation, equalization, and demapping | `[data_re, layer, bit]` FP16 pre-descrambling LLRs |

For a new repository, generate a self-contained starting project with a fresh
target, module identity, and package UUID:

```text
python3 tools/scaffold_class_a_cuda.py \
  --kind receiver \
  --target my_receiver_cuda \
  --global-name com.example.ran.my-receiver.cuda \
  --package-id 01234567-89ab-4cde-8123-456789abcdef \
  --vendor "Example Vendor" \
  --version v1 \
  --output ../my-receiver-dapp
```

The generated tree carries the Clear license, includes only the selected hook
depth's algorithm and required support, consumes the installed `OCUDUDAppSDK`,
and builds a hash-bound manifest-v1 record and digest-bound SPDX 2.3 JSON SBOM
beside the shared object. The generator refuses
to overwrite an existing path. CE, CE+EQ, and receiver generation are exercised
by separate generation, build-only, and resident-certification tests. The
build-only gate configures and compiles three external projects, checks the ABI,
identity, hash, capability, and file modes, and verifies source SPDX markers
without invoking the resident algorithms. The certification gate then runs the
numeric CUDA and model-lifecycle matrix on an exclusive GPU.
Before releasing an independently built artifact, also configure this reference
repository with `OCUDU_DAPP_REQUIRE_ABI_COMPILER_MATRIX=ON` and run
`abi.public_v1_compiler_matrix`. That gate compiles the installed public C ABI
under GCC and Clang in both C11 and C++17 modes and compares its LP64 layout to
the frozen v1 fingerprint. It complements the artifact contract and certifier
tests; it does not replace a target-specific historical-binary compatibility
fixture.

The receiver reference includes a small C++ artifact generator so model-state
management can be exercised without reproducing its private binary layout:

```text
make_receiver_cuda_model receiver-model.bin 0.75
ocudu-dappctl model stage --instance INSTANCE --model receiver-levels \
  --version 1 --model-generation 1 --artifact receiver-model.bin
```

Continue with `validate`, `warm`, and `activate`, using the returned model
generation for each operation. The scale argument only makes the reference QAM
table's effect observable; a commercial receiver should replace the artifact
definition, validation, and prepared device state with its own bounded weights.

Within this reference repository, copy only that module directory and give the
artifact, global name, package UUID, and manifest a new identity. Keep
`module.cpp` as the query trampoline. Preserve
the lifecycle and ABI dispatch in `module_support.cpp` when present, then replace
the implementation in `algorithm.cu`. The reference algorithms are deliberately
standalone and do not call OCUDU's generic PHY implementations.

Keep `ocudu_dapp_query_v1` pure, deterministic, and side-effect free. OCUDU first
loads the verified artifact in a disposable preflight helper to query and
validate its immutable descriptor, then performs the same query after loading
the admitted artifact in the DU. The query must not initialize CUDA, allocate
model state, start threads, depend on call count, or retain host pointers. Put
all instance/device/model setup in `create`, `prepare`, or model-management
hooks; the generated `module.cpp` already follows this rule and normally needs
only identity updates.

Every generated CUDA algorithm entry receives two already-resolved C++ state
objects: one instance-owned object shared by all workers and one mutable object
owned by the current worker/lane. Extend the selected hook-specific types for
prepared read-mostly weights/tables and lane-local scratch/history. The CE
reference types are deliberately empty, CE+EQ demonstrates bounded worker
scratch, and receiver demonstrates a generation-published model slot plus
worker completion fences. Do not replace these references with a process-global
singleton; multiple module instances and concurrent PUSCH lanes are valid.

The invocation contract is strict:

- validate every tensor domain, backend, element type, rank, dimensions, layout,
  access mode, stride, byte extent, stream, and bounded count before launching;
- enqueue only on `invocation.execution_stream`; never synchronize that stream;
- allocate and upload model state in lifecycle or model-management operations,
  not in invocation;
- keep mutable scratch per admitted worker/lane, since concurrent PUSCH lanes may
  invoke the same module instance;
- keep the ABI callback `noexcept` and call replaceable implementation code
  through `guard_algorithm_call`; the generated shell maps `std::bad_alloc` to
  `RESOURCE` and every other exception to `EXCEPTION`, allowing OCUDU's incident
  and breaker policy to contain the failure instead of terminating the process;
- if persistent device/model state can be referenced by queued work, record its
  lane-local completion fence even when the implementation returns failure or
  throws, since it may have enqueued work before failing;
- write only caller-owned outputs and return an explicit unsupported/error status
  before launching when the requested profile cannot be handled;
- do not retain borrowed tensor pointers beyond the completion event associated
  with the invocation.

`ref_equalizer_cuda/module_support.cpp` is the minimal executable example of
that ownership rule. Its lane constructor creates one bounded
`equalizer_cuda_worker_state`; the lane destructor retires it; and
`equalize_cuda` uses its per-port noise array without allocation or
synchronization. It never aliases caller-owned channel or symbol outputs as
scratch. The generated CE+EQ project includes this support file, so a new
algorithm normally keeps the worker lifecycle and changes only the exported
`equalize_cuda` implementation and kernels in `algorithm.cu`. Increase a bound
only through an explicit profile and state-size change, never by allocating on
an invocation miss.

Input completeness is also part of the contract, not an inference from tensor
shape. `OCUDU_DAPP_DMRS_REFERENCE_SYMBOLS_LAYER_COMPLETE_V1` says the reference
plane covers every declared layer, and
`OCUDU_DAPP_DMRS_COORDINATES_SHARED_ACROSS_LAYERS_V1` says its one coordinate
table applies to every layer. CE+EQ and receiver additionally require
`OCUDU_DAPP_PUSCH_DATA_RE_SELECTION_COMPLETE_V1`; the compact indices then name
every output data RE in canonical order. Reject unknown flag bits. The host
performs the same checks before callback dispatch, while the reference modules
repeat them defensively at their trust boundary.

OCUDU publishes host-owned `class_a.fallback.*` counters through the normal E3
metric query for every Class-A instance. Reasons distinguish lifecycle
admission, open breaker, overlapping hook ownership, profile fields (metadata,
layers, ports, allocation, modulation, DM-RS, transform precoding, and DC), and
module outcomes (bypass, unsupported, invalid input/output, deadline,
exception, device, resource, and other returned errors). These counters require
no module code and cover pre-dispatch fallbacks that a module cannot observe.
Returning `UNSUPPORTED` remains a legal defensive fallback and does not advance
the circuit breaker; keep the declarative profile narrow enough that this is
exceptional rather than routine.

Resident CUDA completion is asynchronous. The callback deadline check covers
host validation and enqueue only; OCUDU brackets the module plus the required
CE/EQ/receiver continuation with timing-enabled events on the inherited stream
and evaluates that boundary after its normal downstream synchronization. A
late boundary increments `class_a.completion.deadline_misses` and records an E3
completion incident containing `observedLatencyNs` and `deadlineNs`. It does
not increment `class_a.fallback.deadline`, because the already-consumed current
result was not replayed through the conventional receiver. Repeated misses can
open that lane's breaker so later calls use the conventional path. A true
same-invocation deadline fallback would require certified parallel conventional
execution and is outside the current contract.

Every Class-A descriptor also carries an immutable
`ocudu_dapp_pusch_profile_v1`. Its PRB, receive-port, layer, modulation, DM-RS,
and transform-precoding envelope must exactly match the hash-bound package
manifest. OCUDU copies this profile when binding a worker and checks it before
acquiring invocation activity or calling module code. Keep both declarations
narrow and truthful when changing an algorithm; returning `UNSUPPORTED` is a
defensive backstop, not normal shape discovery on the deadline path. The
generated CUDA scaffold declares the current 275-PRB, 64-port, two-layer,
all-QAM, DM-RS-type-1/2, DC-overlap-capable, no-transform profile in both
places. Its compact data-RE indices exclude DC when applicable; do not retain
that capability when replacing the input-selection contract with an algorithm
that cannot preserve this behavior.

This is the reference v1 admission envelope, not the maximum representable by
the extensible ABI. The positive certifier rejects a final DSO that declares
more than two layers or transform precoding, and its JSON records the admitted
profile explicitly. A future profile may extend either feature only with new
algorithm, metadata, conformance, fallback, and performance evidence.

The executable reference profile supports one layer with multi-port MRC and two
layers with a closed-form 2x2 zero-forcing equalizer. Its two-layer DM-RS input
uses common pilot coordinates and layer-major references, matching ports
1000/1001. Supporting layers whose DM-RS ports occupy different RE patterns will
require a future union-coordinate contract/profile; do not silently reinterpret
the current tensor.

Run the CUDA unit suite first. It includes a deterministic two-port/two-layer
fixture that recovers both channel columns, both equalized symbols, and both LLR
streams. Then run the packaged OCUDU resident tests, Compute Sanitizer memcheck
and racecheck, and the Class-A benchmark with both layer counts. For example:

```text
dapp_class_a_cuda_benchmark -P 25,106,273 -L 1,2 -M 2,4,6,8
```

Run the packaged artifact itself through the offline positive certifier before
handing it to OCUDU:

```text
certify_class_a_cuda my_receiver_cuda.so 0x00010005
```

The JSON result proves that the queried DSO completed lifecycle, two concurrent
worker/stream lanes at the minimum 1-PRB/1-port/1-layer shape, representative
25- and 106-PRB two-port/two-layer shapes, and the exact declared maximum
PRB/port/layer shape. It also places a 25-PRB allocation at symbol 2 and PRB 6
inside a 14-symbol, 106-PRB full grid. That vector detects modules which
incorrectly assume that DM-RS coordinates or data-RE indices always begin at a
zero-origin allocation-local tensor. A second vector at that origin spans four
allocation symbols and places DM-RS on symbols 2 and 5. Neither DM-RS symbol is
independently sufficient to resolve both layers: the joint four-pilot set is
required, so accepting a multi-symbol descriptor while reading only the first
pilot symbol cannot pass. Every shape runs QPSK, 16-QAM, 64-QAM,
and 256-QAM under declared DM-RS Types 1 and 2. The fixtures distinguish their first-CDM-group
subcarrier patterns, include a DC-overlap slice, perform hook-specific numeric
checks, and select a dense canonical data-RE range. The six reference shapes
therefore validate 9, 297, 297-at-nonzero-origin, 1194 across four symbols,
1269, and 3297 data REs per
lane with symbols varied by data RE and layer, proving complete compact tensor
lengths and ordering.
Every caller-owned CUDA output is backed by a poisoned allocation with a
nonzero `byte_offset`. After each invocation, the tool verifies that the module
preserved the exact tensor storage descriptor, wrote every required value, and
left the prefix, suffix, and inactive-capacity canaries unchanged. This catches
hard-coded base-pointer assumptions, partial writes, storage substitution, and
the common one-element kernel overrun in the final packaged DSO. Receiver checks
also require finite nonzero LLRs with the v1 side-output slots absent. The tool
checks canonical metrics and retires cleanly on the named CUDA device. A
model-managed receiver should additionally run:

```text
certify_class_a_cuda my_receiver_cuda.so 0x00010005 \
  --model-artifact receiver-model.bin \
  --model-id baseline --candidate-model-id candidate \
  --model-version 1 --model-generation 1
```

That path verifies two generation-explicit stage/validate/warm/activate
sequences, rollback to the baseline, retirement of the candidate, and rejection
of an attempt to retire the active model without changing its generation.
The JSON records every allocation origin and enclosing-grid shape. Preserve it
beside the separately verified hash-bound manifest. Extend
the certification matrix with deployment-specific shape/antenna combinations,
divergent DM-RS port/CDM patterns, and every model artifact the package
actually declares; this portable matrix is a starting gate, not the entire
release certificate.

For Compute Sanitizer, run memcheck against the default boundary matrix. Use
`--shape-set compact` for racecheck: it preserves two independent workers and
streams, maximum admitted layer depth, all QAM and DM-RS combinations, and DC
handling at one PRB without repeatedly instrumenting the same full-grid fill.
The compact result supplements rather than replaces the boundary certificate.

OCUDU currently admits all three artifacts at the first two-layer profile. The
CE-only module remains scoped to H/noise: after it returns, OCUDU performs the
two-layer zero-forcing continuation into its own compact symbol/noise buffers
before the existing resident demapper and descrambler. A new CE implementation
therefore replaces only `estimate_channel_cuda`; it does not need to absorb
equalization or demapping. Requests outside the admitted shape still take the
complete conventional accelerated receiver.

The shipped Class-B scheduler reference remains CPU-only, but a Class-B package
may use CUDA or another admitted backend internally. Do not copy these resident
PUSCH profiles into scheduler packages: the v1 scheduler boundary still carries
compact host records and supplies no resident tensor or inherited-stream
contract. See `class_b_accelerator_authoring.md` for that distinction.

---

## Host CSI, DMRS metadata, and soft deadlines (author contract)

These host behaviors apply to **all** Class-A equalizer packages (open reference
and third-party). They are **not** a new ABI major version.

### Post-EQ noise → scheduler SINR

After a successful **equalizer** (CE+EQ) **inline** invoke on the GPU-resident
PUSCH path:

1. The host’s resident CE stub **does not** fill CSI.  
2. Conventional full-pipeline E2E SINR D2H is **skipped**.  
3. Soft demap still uses your **post-EQ noise** tensor.  
4. The **host** publishes CRC / scheduler SINR as  
   \(\mathrm{SINR_{dB}} = -10\log_{10}(\mathrm{mean}\,\sigma^2_{\mathrm{post}})\).

**Author requirement:** write finite, non-negative post-EQ noise for every
admitted data RE (same storage the demapper already expects). Do **not** leave
a constant near-zero floor that would invent huge SINR. Reference
`ref_equalizer_cuda` already satisfies this; no rebuild is required for that
host fix alone.

### Optional `dmrs.reserved[0]` symbol mask

`ocudu_dapp_dmrs_metadata_v1.reserved[0]` may carry a host-filled OFDM-symbol
bitmask (bit *s* set when symbol *s* has DMRS). Coordinates remain on-device
for pilot scatter.

| Package behavior | Conforming? |
|------------------|-------------|
| Ignore `reserved[0]` and derive symbols from coordinates | **Yes** (default for open references) |
| Prefer `reserved[0]` when non-zero to skip a coords D2H used only for symbol discovery | **Yes** (optional optimization) |

Do not require `reserved[0] == 0` in package validators if you want to run
against modern hosts that fill the hint.

### Optional `pusch.reserved[3]` DC subcarrier

`ocudu_dapp_pusch_metadata_v1.reserved[OCUDU_DAPP_PUSCH_RESERVED_DC_SUBCARRIER_PLUS_ONE_V1]`
(index 3) carries the UE-signalled transmitter DC location as a grid
subcarrier index **plus one**; zero means unknown or an older host. The
`OCUDU_DAPP_PUSCH_DC_OVERLAP_V1` flag still says whether it falls inside this
grant. The host only publishes it when the UE reports a DC location inside the
carrier (FAPI `txDirectCurrentLocation` ≠ 3300), so the hint is radio-specific
by construction: on a UE without a DC inside the grid nothing changes.

Why it matters: the UE's LO leakage sits on that RE. The stock host receiver
zeroes the channel estimate there; a device-side estimator that reads pilots
from the grid does not know and, worse, spreads the corrupted pilot through
any frequency-domain smoothing. `class_a_cuda_contract.cuh` now exposes it as
`inputs::dc_subcarrier` (`no_dc_subcarrier` when absent); `ls_pilot_sample`,
the noise estimators and `equalize` skip that pilot and return an erasure
(noise `1e6`, symbol `0`) for the data RE. The host keeps erasures out of the
SINR it reports to link adaptation. Measured on the reference cell (51 PRB,
DC at RE 306) wide grants covering PRB 25 went from 20–90% CRC failure at
MCS 17–25 to the same 1–10% as narrow grants.

| Package behavior | Conforming? |
|------------------|-------------|
| Ignore `reserved[3]` | **Yes** (but expect the DC pilot to corrupt your estimate on radios with LO leakage) |
| Exclude the DC pilot from estimation and erase the DC data RE | **Yes** (what the shared contract helpers do) |

### Soft wall budgets

Host defaults for Class-A soft completion wait are on the order of **2000 µs**
(demodulator / equalizer path) and **300 µs** (channel estimator). Recoverable
deadline observations open that lane after **8 consecutive** misses. Tests and
benchmarks may inject tighter budgets. Soft deadline metrics are distinct from
circuit-breaker open.

### Multi-worker packaging (guidance)

If the DU invokes equalizer workers concurrently, avoid holding a single host
mutex across GPU completion. Prefer multiple independent lanes/handles, staged
scratch, and GPU-side stream waits—the same multi-buffer pattern as the host
PUSCH GPU demodulator. This is packaging guidance, not a second public ABI.

### Compatibility matrix

| Host change | Open `ref_*_cuda` | Third-party EQ writing post-EQ noise | Third-party ignoring `reserved[0]` |
|-------------|-------------------|--------------------------------------|-------------------------------------|
| Post-EQ SINR export | Works | Works | Works |
| Host fills `reserved[0]` | Works | Works | Works |
| Soft 2000 µs default | Works | Works | Works |

## Enable and run Class-A packages on a DU

Full cookbook (all UUIDs, CPU variants, vendor_min, disable path):
**[run_examples.md](run_examples.md) §1–§2**. Short path for the equalizer CUDA
reference:

### 1. Offline certify

```bash
certify_class_a_cuda build-cuda/ref_equalizer_cuda.so 0x00010004
```

### 2. Stage catalog

```bash
export CATALOG=/opt/ocudu/dapps
cp build-cuda/ref_equalizer_cuda.{so,manifest.json,spdx.json} "$CATALOG/"
```

### 3. DU YAML

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned   # lab only
  package_manifests:
    - ref_equalizer_cuda.manifest.json
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
    request_timeout_ms: 2000
  # optional: start bound without manual E3
  # initial_instances:
  #   - package_manifest: ref_equalizer_cuda.manifest.json
  #     placement: in_process
  #     backend: cuda
  #     target: active
```

Restart the DU. Host must be built with `-DENABLE_DAPP=ON`.

### 4. Load and activate (E3)

```bash
export OCUDU_DAPP_E3_SOCKET=/run/ocudu/dapp-management.sock
DAPPCTL=(python3 /path/to/ocudu/apps/tools/dappctl/ocudu_dappctl.py
  --socket "$OCUDU_DAPP_E3_SOCKET"
  --schema /path/to/ocudu/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1)

PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000204   # ref_equalizer_cuda

"${DAPPCTL[@]}" load --package "$PACKAGE_ID" \
  --placement in-process --backend cuda
# INSTANCE / GEN from completed job

for action in prepare arm shadow activate; do
  "${DAPPCTL[@]}" lifecycle "$action" \
    --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 5000000
  # update GEN after each success
done
```

| Other reference | UUID | Interface | load |
|-----------------|------|-----------|------|
| `ref_channel_estimator_cuda` | `…000203` | `0x00010003` | in-process / cuda |
| `ref_receiver_cuda` | `…000205` | `0x00010005` | in-process / cuda |
| `ref_equalizer_cpu` | `…000004` | `0x00010004` | in-process / cpu |
| `example_eq_cuda` (vendor_min) | `…e001` | `0x00010004` | in-process / cuda |

UUID prefix: `8f0d0001-4f9c-4e31-8c00-`. Only one Class-A depth should own the
UL path at a time.

### 5. Disable

```bash
"${DAPPCTL[@]}" lifecycle deactivate --instance "$INSTANCE" \
  --expected-generation "$GEN" --timeout-us 5000000
"${DAPPCTL[@]}" lifecycle unload --instance "$INSTANCE" \
  --expected-generation "$GEN" --timeout-us 5000000
```

### 6. Verify

Inventory `state=active`; under UL traffic, Class-A metrics advance and host
`class_a.fallback.*` stays near zero when the package is admitted. Prefer
stopping at **shadow** first for new packages, then `activate`.

With an **equalizer** package active, scheduler UE metrics should show a
**present** `pusch_snr_db` under UL traffic (host derives SINR from your
post-EQ noise). Soft `class_a.completion.deadline_misses` may rise under
contention without implying breaker-open.
