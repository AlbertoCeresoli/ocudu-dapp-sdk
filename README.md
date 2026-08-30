<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU dApp SDK and reference modules

This standalone project is the authoring kit for extendable OCUDU reference
dApps. Each reference is a working end-to-end implementation whose algorithm
core is intentionally replaceable: authors keep the validated ABI, lifecycle,
memory/stream, metrics, packaging, and host-authority boundaries while adding
better CE, EQ, receiver, scheduler, spectrum, or SRS/ISAC algorithms. The
project consumes the installed `OCUDUDAppSDK` CMake package; it does not copy
OCUDU ABI headers or require the OCUDU source tree.

**Documentation index:** [`docs/README.md`](docs/README.md)

**Canonical home:** <https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk>

| Start here | Purpose |
|------------|---------|
| [`docs/extension_model.md`](docs/extension_model.md) | What to keep stable and what algorithm authors replace |
| [`docs/ocudu_dapp_quickstart.md`](docs/ocudu_dapp_quickstart.md) | Full walkthrough: repo → package → DU → E3 |
| [`docs/run_examples.md`](docs/run_examples.md) | **Enable & run every example dApp** (YAML + E3 + UUIDs) |
| [`examples/vendor_min/`](examples/vendor_min/README.md) | **Copy-me** commercial Class-A MODULE |
| [`examples/class_c_e3_python/`](examples/class_c_e3_python/README.md) | **Python** Class-C out-of-process sensing (PyTorch; research only) |
| [`examples/e3_telemetry_python/`](examples/e3_telemetry_python/README.md) | **Python E3AP observers**: subscribe to PUSCH/SRS/metrics telemetry and run management over SCTP |
| [`examples/e3_spectrum_sensing/`](examples/e3_spectrum_sensing/README.md) | **Out-of-process sensing**: emitter detection on the Class-C spectrum grid over E3AP |
| [`examples/e3_isac_sensing/`](examples/e3_isac_sensing/README.md) | **SRS-ISAC starting point**: range-Doppler sketch on the SRS channel response (not a sensing capability) |
| [`examples/e3_mcp_server/`](examples/e3_mcp_server/README.md) | **MCP companion**: Model Context Protocol server exposing sector state, UEs, KPIs, dApp management and RAN control over E3 |
| [`docs/mcp_companion.md`](docs/mcp_companion.md) | **MCP companion guide**: tools, safety model, worked sessions, extension recipes |
| [`docs/vendor_toolkit.md`](docs/vendor_toolkit.md) | Thin multi-vendor repos, `find_package(OCUDUDapp)` |
| [`docs/package_config.md`](docs/package_config.md) | Shared A/B/C config / management core |

Matching **OCUDU-style Doxygen** pages live under [`docs/doxygen/`](docs/doxygen/)
(`cmake --build <build> --target doxygen`). The host runtime publishes the same
workflow as page `dapp_quickstart` plus dedicated E3 guides.

## Getting started — step-by-step tutorials

New here? These tutorials walk the whole journey, from an empty vendor repo to
an LLM agent tuning a live cell. Tutorials 1–4 need **no keys**; signing and
trust (5) are a deferrable production step, and (6) covers exposing your dApp's
own KPIs. Follow in order, or jump to the one that matches your role. No radio
needed: the sibling `ocudu-docker/` repo builds both public repos into a
container and ships a loopback E3 agent the tutorial commands can target.

```bash
# clone the set side by side; the platform repo must be cloned as ocudu/
mkdir ocudu-workspace && cd ocudu-workspace
git clone https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform.git ocudu
git clone https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-sdk.git
git clone https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-quickstart.git ocudu-docker
```

| # | Tutorial | You are… | You will… |
|---|----------|----------|-----------|
| 1 | [**Stand up a new vendor dApp repo**](docs/tutorial_new_vendor_repo.md) | a **vendor** adding new capabilities | scaffold, build, and certify your own dApps against the installed SDK — no OCUDU source, your own CI and cadence |
| 2 | [**Install dApps & load them from the gNB YAML**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_install_and_yaml.md) | an **operator / SI** | stage a catalog, build a trust policy, and bring instances up at startup or over E3 |
| 3 | [**Operate dApps with `dappctl`**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_dappctl_operations.md) | an **operator** | load → prepare → arm → shadow → activate, swap versions (A/B), deactivate/unload, watch and reset circuit breakers |
| 4 | [**Monitor & tune a live gNB with an LLM agent**](docs/tutorial_mcp_monitoring.md) | an **operator / researcher** | ask questions about the running cell over MCP and set KPI-driven tuning loops with LLM reasoning |
| 5 | [**Signing, SBOMs & key management**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_signing_and_trust.md) | **vendors + operators** | the production trust path — sign, build a trust policy, rotate/revoke, custody (skip for a first run) |
| 6 | [**Exposing dApp KPIs & querying over E3**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_dapp_kpis.md) | **vendors + operators** | declare your own metrics in a dApp and read them via `dappctl`, `ocudu_e3`, a dashboard, or the MCP agent |
| 7 | [**Your first dApp: standalone Class-C over E3**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_class_c_standalone.md) | a **developer** | a Python (or C++) out-of-process dApp in ~40 lines — subscribe, decode numpy snapshots, act back |
| 8 | [**Telemetry deep-dive & data capture**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_telemetry_capture.md) | a **researcher** | all 8 streams, the floor/rate model, HDF5 capture, offline PUSCH replay with the gNB's own receiver |
| 9 | [**Neural-model operations**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_model_operations.md) | **vendors + operators** | stage → validate → warm → activate → rollback over E3; generation discipline; live model A/B |
| 10 | [**Class-B scheduler control**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_class_b_scheduler.md) | **vendors + operators** | the ~100 µs contract, operator allow-flags, evaluation-first authority, sensing→PRB-avoidance |
| 11 | [**Troubleshooting runbook**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_troubleshooting.md) | **everyone** | symptom → cause → fix for admission, lifecycle, telemetry, E3, agents, containers |
| 12 | [**Capstone: the whole loop**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_capstone.md) | **everyone** | one story end to end: capture → train → package → shadow → activate → monitor → rollback |

New terms: the [**glossary**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/glossary.md). Extending the
platform itself (new contracts, streams, operations):
[**proposing_extensions.md**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/proposing_extensions.md).

Zero hardware? [**Tutorial 0 — the docker testbed**](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_docker_testbed.md)
runs the platform and a loopback E3 agent so every tutorial above works
radio-free.

### The three contracts you can implement

A dApp implements one (or more) of three **use-case contracts**, each a
different point on the performance-vs-isolation continuum — pick the one that
matches your application:

- **Class A — Resident inline L1** (`docs/class_a_cuda_authoring.md`). Replace
  a PUSCH stage — channel estimation (`0x00010003`), CE+equalization
  (`0x00010004`), or the full receiver-to-LLR chain (`0x00010005`) — with your
  kernels running **in the slot, on the host's CUDA stream**. This is the home
  of **neural receivers** and learned CE/EQ, with live model
  stage/validate/warm/activate/**rollback**. Maximum performance, maximum
  trust; in-process only.
- **Class B — Bounded real-time control** (`docs/class_b_scheduler_baseline.md`).
  Advise the MAC **scheduler** (`0x00010006`) with validated intents (priority,
  PRB avoidance, MCS/TPC hints) inside a ~100 µs boundary, under operator allow
  flags, introduced evaluation-mode-first. The home of **learned scheduling and
  link adaptation**.
- **Class C — Asynchronous observation & advisory**
  (`docs/class_c_cuda_process_authoring.md`). Observe uplink spectrum
  (`0x00010001`) and SRS channel estimates (`0x00010002`) **without ever
  blocking a producer** — in-process, in a supervised worker process, or as a
  portable E3 client on any host. The home of **spectrum sensing, ISAC,
  interference avoidance, and dataset capture**; results feed host-owned caches
  the scheduler consumes.

The architecture and use cases in depth: the companion paper and slide deck,
published separately. Class-by-class authoring detail:
[`docs/README.md`](docs/README.md).

## Contents

- [Public ABI fingerprint gate](#public-abi-fingerprint-gate)
- [Reference modules at a glance](#reference-modules-at-a-glance)
- [Packaging, signing, and SBOMs](#packaging-signing-and-sboms)
- [Class-A CUDA replacement depths](#class-a-cuda-replacement-depths)
- [Class-B scheduler](#class-b-scheduler)
- [Offline certification: `certify_class_a_cuda`](#offline-certification-certify_class_a_cuda)
- [Concurrency and exception-safety patterns](#concurrency-and-exception-safety-patterns)
- [Class-C references: spectrum and SRS-ISAC](#class-c-references-spectrum-and-srs-isac)
- [Isolated process workers](#isolated-process-workers)
- [E3 fallback consumers](#e3-fallback-consumers)
- [Manifests and catalog deployment](#manifests-and-catalog-deployment)
- [Building and testing](#building-and-testing)

## Public ABI fingerprint gate

Release validation can require a frozen public-ABI compiler matrix:

```text
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOCUDU_DAPP_REQUIRE_ABI_COMPILER_MATRIX=ON
ctest --test-dir build -R '^abi\.public_v1_compiler_matrix$' --output-on-failure
```

The test compiles one layout dump against the installed public headers as GCC
C11, Clang C11, GCC C++17, and Clang C++17, then requires every result to match
the checked-in v1 LP64 fingerprint exactly. It covers ABI constants, structure
size/alignment, and offsets at the pointer, tensor, dispatch, management, and
six use-case boundaries. This detects accidental native-layout drift before an
independently built module is packaged. It does not claim a 32-bit ABI, C++
standard-library ABI compatibility (the public boundary is C and contains no
standard-library types), or compatibility with a historical binary fixture;
those require separate target fingerprints and a frozen older-module load test.

## Reference modules at a glance

The project builds six CPU contract modules and three isolated CPU workers. When a CUDA compiler is available,
it also builds three CUDA-resident Class-A modules, native CUDA spectrum and SRS-ISAC modules, and an isolated
CUDA spectrum worker. It exercises their descriptor,
lifecycle, worker-context, configuration, metric, and data-path surfaces. The
Class-A examples independently implement compact conventional algorithms:
all-pilot flat least-squares channel estimation, one- or two-layer multi-port combining
and zero-forcing equalization with residual-noise propagation, and max-log QAM soft
demapping. They use explicit pilot tensors
and caller-owned output buffers, allocate nothing in the hot path, and do not
call or modify OCUDU's generic PHY implementations. They are deterministic,
validated starting algorithms rather than a claim that the supplied signal
processing is optimal. They are explicitly meant to be extended or replaced
by stronger implementations behind the same contracts.

## Packaging, signing, and SBOMs

The SDK emits strict hash-bound manifest-v1 records and digest-bound SPDX 2.3
JSON SBOMs, but does not ship a vendor or
operator signing key. Deployment packaging is intentionally separate from
module compilation: copy the selected immutable manifest, SBOM, and artifact into an
operator-controlled catalog, then use OCUDU's installed
`ocudu-dapp-package keygen`, `sign`, `trust`, and `inspect` commands. Production
DU configuration requires detached signatures and an offline trust policy by
default. The trust policy supplies active/revoked keys, a monotonic policy
epoch, and per-package sequence floors. Artifact and SBOM SHA-256 values remain
independent admission checks covered by the detached manifest signature. E3 inventory exposes verification status and
non-secret signer metadata after admission. The registered OCUDU test
`dapp_dummy_ru_e2e.signed_sdk_catalog_lifecycle` assembles these independently
built examples, signs all six baseline CPU packages, and proves ordinary DU
bootstrap and lifecycle management under the generated trust policy.
`dapp_dummy_ru_e2e.signed_declarative_initial_instances` additionally proves
that the same signed, SBOM-bound packages can be selected at startup, managed
in shadow through ordinary E3 configuration and model operations, then
activated and unloaded without a startup-only loader or version change.

## Class-A CUDA replacement depths

The CUDA examples expose the same three replacement depths as separate artifacts:
`ref_channel_estimator_cuda` (CE), `ref_equalizer_cuda` (CE+EQ), and
`ref_receiver_cuda` (CE+EQ+soft demapping). Each validates CUDA device tensors,
queues only on the inherited `invocation.execution_stream`, writes caller-owned
resident outputs, and returns without allocating or synchronizing. The compact
`data_re_indices` tensor avoids scanning a dense mask in accelerator code. The
baseline is deliberately easy to copy: it assumes a flat channel and supports
one or two layers, and the full receiver uses a concise max-log demapper for QPSK, 16-QAM, 64-QAM,
and 256-QAM. Unsupported shapes return an explicit status so OCUDU retains
conventional authority. Each artifact declares the same bounded PUSCH admission
profile in its module descriptor and hash-bound manifest; OCUDU checks it before
module callback dispatch. Explicit PUSCH/DM-RS completeness flags prevent a
copied algorithm from silently treating a partial data-RE selection or
single-layer pilot plane as complete. A new implementation
normally copies one module directory and replaces the kernel and exported
algorithm function in its local `algorithm.cu`; the small `module.cpp` is a
stable query trampoline, while `module_support.cpp` retains lifecycle, worker
ownership, model management, metrics, and ABI dispatch. Shared contract
validation lives separately in `include/ocudu_dapp_sdk/class_a_cuda_contract.cuh`,
so changing a CE algorithm cannot silently alter the CE+EQ or receiver artifact.
Host-owned per-reason Class-A fallback counters remain queryable through E3 even
when profile admission or an OCUDU adapter bypasses before module dispatch.
For resident CUDA work, OCUDU also measures the complete replacement boundary
with per-lane CUDA events and publishes `class_a.completion.deadline_misses`.
This is intentionally distinct from `class_a.fallback.deadline`: a module may
enqueue successfully but finish after its deadline, in which case the current
result was late and cannot truthfully be described as having fallen back. The
completion incident carries the measured GPU duration and configured deadline;
the per-worker circuit breaker protects subsequent invocations.
All three CUDA algorithm entry points receive an explicit instance-owned state
object and a mutable per-worker state object. The CE reference leaves both
empty, CE+EQ uses lane-local bounded scratch, and the receiver demonstrates
persistent accelerator/model state: its prepare hook
allocates and uploads one read-only constellation table per module instance,
and each admitted worker owns independent lane history plus a bounded
channel/noise workspace. One 128-thread block per receive port consumes all
declared pilots for every admitted layer and derives residual noise before the
next kernel; the receiver does not repeat pilot estimation per data RE.
Invocation only borrows prepared state and
enqueues kernels; retirement occurs after host quiescence. CPU and CUDA
references use the canonical interleaved real/imaginary QAM bit semantics. Its
model-management example extends the same pattern with two bounded device
slots. `stage`, `validate`, and `warm` fully check and upload a pointer-free
`receiver_cuda_model_artifact_v1` off the hot path; `activate` performs one
atomic slot publication; and `rollback` republishes the retained prior slot.
The two-layer profile uses the common DM-RS coordinate set of ports 1000/1001
and a layer-major reference-symbol tensor. Per-lane CUDA completion events and a
short enqueue-reader epoch prevent an
inactive slot from being overwritten or freed while any inherited stream may
still read it. The active model cannot be retired, and a third warm operation
returns `CONFLICT` until the retained rollback model is retired. This is a
small executable template for replacing the table with real weights while
preserving bounded memory, generation checks, rollback, and allocation-free
invocation.
`make_receiver_cuda_model` emits this example's pointer-free artifact for live
E3 stage/validate/warm/activate tests; it is an offline C++ tool, not a runtime
dependency of the module or DU.

The current resident continuation contracts are intentionally direct:

- CE writes port-major full-slot CF32 channel estimates
  (`[rx_port, layer, symbol, subcarrier]`) plus an F32 `[rx_port, layer]`
  noise-variance tensor (the same estimate is written for every layer of a
  port);
- CE+EQ writes compact CF32 symbols and F32 post-EQ noise in
  `[data_re, layer]` order; and
- receiver writes compact FP16 pre-descrambling LLRs in
  `[data_re, layer, bit]` order.

Each CUDA artifact links only its own algorithm object. This lets an author copy
exactly one module directory, keep the frontend and support files unchanged,
and replace one algorithm function without inheriting OCUDU implementation
code or unused kernels from the other replacement depths. See
[`docs/class_a_cuda_authoring.md`](docs/class_a_cuda_authoring.md) for the
copy-and-rename checklist, concurrency rules, supported multilayer profile, and
validation commands. Its scaffold command emits a self-contained external
project for any of the three CUDA hook depths and refuses to overwrite existing
work. OCUDU queries each verified artifact first in a disposable preflight
helper and then in the DU, so the stable query trampoline is deliberately pure;
CUDA contexts, weights, scratch, and threads belong to lifecycle state rather
than descriptor discovery. The generated CE, CE+EQ, and receiver projects all
preserve these explicit state references, so a copied algorithm can add
prepared weights or lane history without global state or a public-ABI change.

## Class-B scheduler

The Class-B scheduler interface remains a synchronous in-process host-record
hook, but this does not require its implementation to execute only on a CPU. A
package may declare `cuda` or another host-supported backend and use it
internally; it still receives host-visible candidates/masks, owns any transfer,
stream, prepared state, and synchronization, and must return the complete host
intent inside the same deadline. No resident Class-B tensor/stream contract or
CUDA scheduler reference is implemented yet. See
[`docs/class_b_accelerator_authoring.md`](docs/class_b_accelerator_authoring.md)
for the packaging / deadline boundary.

The shipped `ref_scheduler_cpu` is a five-stage pipeline — rank, package OLLA,
table-aware MCS, multi-UE PRB packing (on free-VRB), and UL TPC — whose
**shipped defaults are host-equivalent**: every stage is off / conventional
passthrough until enabled over E3, and it emits only intents the host grants
via `allowed_output_flags` (`emit_only_allowed`).
Algorithms, knobs, and host allow-flag wiring are documented in
[`docs/class_b_scheduler_baseline.md`](docs/class_b_scheduler_baseline.md).

## Offline certification: `certify_class_a_cuda`

`certify_class_a_cuda` is the offline positive-vector companion to OCUDU's
bounded production preflight. It loads the final DSO through the public ABI,
runs its complete null-configuration lifecycle, creates two independent
workers and CUDA streams, and submits deterministic vectors at the
1-PRB/1-port/1-layer minimum, representative 25- and 106-PRB two-port/two-layer
shapes, and the package's declared maximum PRB/port/layer shape. A second
25-PRB vector places that allocation at symbol 2 and PRB 6 inside a 14-symbol,
106-PRB full grid. This proves that DM-RS coordinates and data-RE indices are
interpreted relative to the supplied grid rather than accidentally relative to
an allocation-local base. A sixth vector expands the same placement to four
allocation symbols with DM-RS on symbols 2 and 5. Its two-layer references are
rank-deficient within either DM-RS symbol and resolve both channel columns only
when the module consumes the joint multi-symbol pilot set. Every shape runs
QPSK, 16-QAM, 64-QAM, and 256-QAM
on both lanes under declared DM-RS Types
1 and 2. Their fixtures use the distinct first-CDM-group subcarrier patterns
`{0,2}` and `{0,1}` respectively; the Type-2 slice also exercises DC-overlap
acceptance. It synchronizes only at the certification boundary and checks H,
equalized symbols, or finite, nonzero LLR hard decisions at the selected hook
depth. Every allocation excludes the three reserved pilot-pattern positions in
each DM-RS symbol and otherwise selects the dense canonical allocation order,
so CE+EQ and receiver validate 9, 297, 1269,
and 3297 data REs per single-symbol lane plus 1194 data REs in the four-symbol
vector rather than a single token RE. Symbols vary by data
RE and layer, exercising the complete compact output length and ordering. Receiver
UCI/metric extension slots must remain absent in v1. Each caller-owned output
uses a nonzero byte offset inside a poisoned CUDA allocation. The certifier
checks exact descriptor identity, every required output value, and intact
prefix, suffix, and inactive-capacity canaries after every invocation. It also
requires the canonical invocation
and maximum-layer metrics and a clean stop/quiesce/destroy sequence. For a
model-managed receiver, optional arguments exercise two generation-explicit
stage/validate/warm/activate sequences, rollback, retirement, and fail-closed
active-model retirement before the numeric matrix. The single-line JSON result
includes module identity, allocation origins, enclosing-grid shapes, final
model generation when applicable, and CUDA device/driver/runtime facts:

```text
build-cuda/certify_class_a_cuda \
  build-cuda/ref_equalizer_cuda.so 0x00010004

# Same concurrency/layer matrix without maximum-grid repetition, for racecheck:
build-cuda/certify_class_a_cuda \
  build-cuda/ref_equalizer_cuda.so 0x00010004 --shape-set compact

build-cuda/certify_class_a_cuda \
  build-cuda/ref_receiver_cuda.so 0x00010005 \
  --model-artifact build-cuda/certify-receiver-model.bin \
  --model-id baseline --candidate-model-id candidate \
  --model-version 1 --model-generation 1
```

All three packaged references and all three freshly generated external
scaffolds run through this tool in CTest. The package verifier separately
binds the exact artifact hash and SPDX package/file identity. This deterministic smoke certificate is not a
substitute for additional deployment-specific shape/antenna combinations, divergent DM-RS
port/CDM patterns, the full model-artifact matrix, Compute Sanitizer,
contention, BLER/EVM, or controlled tail-latency qualification, and it is
deliberately not executed during ordinary DU startup.
The default `boundaries` shape set is the release smoke gate and remains the
memcheck input. `compact` retains two workers, the maximum admitted layer depth,
all QAM/DM-RS combinations, and DC handling for practical racecheck runs; it is
not a substitute for the default boundary certificate.

Fresh scaffold validation is split deliberately. The CPU-only
`scaffold.class_a_cuda_generation` test checks transactional generation and
source contracts. In a CUDA build, `scaffold.class_a_cuda_build` independently
configures, compiles, queries, and verifies the packages without invoking their
resident algorithms. `scaffold.class_a_cuda` adds the exclusive-GPU numeric and
model-lifecycle certificate. A busy accelerator can therefore block runtime
qualification without blocking proof that a copied project still builds.

## Concurrency and exception-safety patterns

The CE+EQ reference makes the concurrency pattern concrete: every admitted
PUSCH worker owns a fixed, 64-port CUDA noise workspace created with the worker
and released with it. The invocation neither allocates nor borrows an output
tensor as temporary storage. A copied equalizer should preserve this lane-local
state pattern and replace only `equalize_cuda` and its kernels. Separate OCUDU
PUSCH lanes then execute the same module instance concurrently without sharing
mutable scratch.

All six CPU/CUDA Class-A shells keep their ABI callbacks `noexcept` while
calling replaceable algorithms through `guard_algorithm_call`. Allocation
failure becomes `RESOURCE`; any other exception becomes `EXCEPTION`, which the
DU records and applies to its lane-local breaker. The receiver also records its
model-slot completion fence after failed calls, because an implementation may
have enqueued work before failing. Generated CUDA projects inherit both rules.

The C ABI deliberately leaves module and worker contexts opaque. The optional
`stateful_module_context<SharedState, WorkerState>` C++ helper preserves that
contract while letting each dApp define its own instance-owned state (for
example, model weights and read-mostly tables) and persistent per-worker state
(for example, scratch or recurrent history). It adds no invocation lock:
mutable instance state must be synchronized by the module, while one worker
state is owned by one admitted execution lane.

## Class-C references: spectrum and SRS-ISAC

The Class-C spectrum example is a radiometer. It consumes OCUDU's native
complex-BF16 grid or a complex-FP32 view directly, avoiding a full-grid format
conversion. The SRS-ISAC example computes a bounded CIR preview from
frequency-domain channel estimates. Both algorithms run through the native
module and isolated worker examples. The scheduler module
(`reference/scheduler` + `reference/common/reference_scheduler_pipeline.cpp`)
runs the five-stage Class-B pipeline (rank / OLLA / MCS / multi-UE pack /
TPC — every stage shipped off), keeps only lane-local history, and can emit a host-authorized
PRB-avoidance mask from the shared interference context when allowed.

With `BUILD_TESTING`, `test_joint_spectrum_scheduler_cpu` is a conformance
fixture rather than a seventh top-level reference dApp. One v1 DSO and one
instance expose the Class-C spectrum and Class-B scheduler descriptors. Their
worker contexts remain independent; only a bounded instance-owned atomic
snapshot is shared. Spectrum still publishes its complete interference map
through the host-owned prepared-output/cache path, and the scheduler accepts
that cache as the authoritative data source only when its provenance matches
the snapshot. Writers never wait, readers make two bounded snapshot attempts,
and mismatch returns `BYPASS`. Production loading tests prove one lifecycle
gate publishes and withdraws both interfaces atomically, while a forced
spectrum failure opens only the spectrum breaker and leaves scheduler fallback
and invocation operational. The fixture demonstrates multi-interface
packaging and safe state fusion without creating a private path around OCUDU's
validation and policy authority.

`ref_spectrum_cuda` is the first native accelerator Class-C reference. OCUDU
borrows a CUDA-visible full-slot grid on a host-owned per-producer stream,
orders that stream after the grid-ready event, and records grid completion after
the callback. The module runs the same simple plan-aware per-PRB radiometer
directly on complex BF16 device storage; it copies only the bounded expected-use
map to device and the at-most-275-byte confidence map back to pinned host
storage. The v1 prepared-output transaction is synchronous, so this reference
synchronizes the inherited stream before publishing that compact result. It
does not copy or migrate the full resource grid. A future asynchronous Class-C
result API can remove this final callback wait without changing input ownership.

`ref_srs_isac_cuda` consumes a read-only CF32 `[rx][tx][frequency]` response on
the host-owned per-producer stream. The current OCUDU adapter derives that
response directly from the CUDA-visible complex-BF16 SRS grid into lane-owned
device storage with one ordered kernel; it neither materializes a host response
nor changes the generic SRS estimator. The reference module computes a bounded
64-tap inverse-DFT/CIR power preview with per-worker device scratch, then copies
only the taps and mean power to pinned host storage. As with spectrum, the
synchronous v1 prepared-output transaction requires a stream wait before the
compact report is published. Distinct SRS producer lanes own distinct streams,
response buffers, sequence caches, and module worker state.

## Isolated process workers

`ocudu_process_client` is the dependency-light worker-side transport helper. It
receives a credentialed route offer, owns the transferred ring and event
capabilities, validates the fixed C wire layout, and exposes zero-copy RAII
leases. Its route-set wait loop also authenticates and answers the supervisor's
generation-bound heartbeat challenges. Heartbeats remain control-plane events:
they are never returned to the algorithm as data publications. It links no
OCUDU runtime code. Before waiting for any route capability it applies Linux
`no_new_privs`; failure aborts admission. The host verifies that kernel state,
applies its own core/open-file/address-space limits, and only then transfers
shared-memory descriptors.

`ocudu_process_cuda_client` is the optional CUDA extension used by
`ref_spectrumd_cuda` and `ref_srs_isacd_cuda`. During admission it validates the
sealed, read-only export descriptor and imports every fixed allocation and
interprocess-ready event once.
For each publication it checks the external slot identifier and exact valid
extent, waits for the host event on a worker-owned stream, and returns a device
view. The worker must finish all device access before releasing the ordinary
ring lease; that lease is the sole completion signal used to retire the paired
export slot.

The DU never exports an arbitrary L1 allocation. After waiting for the native
grid-ready event, it makes one device-to-device snapshot into a bounded,
route-owned CUDA IPC pool and records that slot's ready event. The pool has the
same fixed cardinality as the shared ring, reclaims slots in publication order,
and is reset only after the supervisor has proved the old child dead. This is
an honest one-copy process boundary, not a zero-copy claim. It provides process
crash and address-space isolation, but a CUDA IPC mapping is not a security
boundary against a malicious GPU client; admit CUDA workers as trusted code and
use CPU shared-memory placement when stronger containment matters. See
[`docs/class_c_cuda_process_authoring.md`](docs/class_c_cuda_process_authoring.md).
Reference workers also install a bounded seccomp deny-list before route
admission. It blocks executable-image replacement, process injection and
privileged kernel administration, but does not sandbox CUDA device activity.

`ref_spectrumd_cpu`, `ref_spectrum_observerd_cpu`, and `ref_srs_isacd_cpu` are
isolated Class-C worker executables using that client. Their supervisor supplies a private control
endpoint; each worker authenticates its parent and rejects a route for the wrong
interface. Each process consumes validated zero-copy shared-memory
publications with the same deterministic algorithms as its native counterpart.
The reference process workers snapshot each bounded route backlog, retire superseded or
expired tensor leases, and compute the newest remaining publication so open-loop
analytics cannot accumulate stale work. The spectrum authority returns its
bounded per-PRB U8 interference map and a per-cell rate-limited quiet intent
over the authenticated sequenced-packet channel; the bulk grid remains on the
one-copy shared-memory ring. The host correlates each result to one recently
published route/source tuple, supplies producer identity and generation, and
admits map publication only when the package declares the spectrum-result
capability. Observer-only spectrum workers may coexist with the single active
map authority. The observer reference declares zero output capabilities,
computes the same bounded radiometer input path, and deliberately emits no
control result. A worker that stops servicing its event loop also stops answering
heartbeats; the host withdraws every route before its bounded kill/restart
sequence and exposes the loss through ordinary E3 metrics and incidents.

## E3 fallback consumers

When the installed SDK includes E3 management, the build also emits
`ref_spectrum_e3_cpu` and `ref_srs_isac_e3_cpu`. These are ordinary,
independently launched binaries—not supervised package workers and not child
processes forked by the DU. They create a leased subscription over the
credentialed local APER endpoint, reassemble bounded host-snapshot chunks, and
validate the same FlatBuffers v1 metadata used by the high-rate Class-C paths.
The spectrum example runs the simple radiometer; the SRS example forms the
same placeholder CIR preview. They demonstrate the more isolated, portable,
and deliberately lower-rate E3 fallback:

```text
./build/ref_spectrum_e3_cpu \
  --socket /run/ocudu/dapp-e3.sock \
  --subscription 404142434445464748494a4b4c4d4e4f \
  --sample-every 10

./build/ref_srs_isac_e3_cpu \
  --socket /run/ocudu/dapp-e3.sock \
  --subscription 505152535455565758595a5b5c5d5e5f
```

Both clients bound tensor size, chunk size, queue depth, lease, and poll
timeout; reject discontinuous or mismatched chunks; retain all chunks when one
poll carries multiple objects; and remove the subscription on clean exit.
Shared-memory and CUDA-IPC workers remain the recommended high-throughput
placement. The APER profile copies a host snapshot and is intended for sampled
sensing/ISAC, diagnostics, and less-trusted Class-C consumers where process
independence matters more than peak throughput.

## Manifests and catalog deployment

The CPU build emits nine reference `*.manifest.json` files and nine `*.spdx.json` files
beside the six native modules and three isolated workers. A manifest gives the artifact a stable package
identity, exact SHA-256 digest, backend, placement, interface contract, and the
SHA-256 identity of its SPDX 2.3 package/file record. The manifest schema remains
v1 while this work is pre-release; provenance is an optional v1 extension so
older v1 packages remain admissible. When provenance is present, OCUDU verifies
it fail-closed before admitting the package. The
native Class-C artifacts are admitted only in-process; the separately packaged
worker executables are admitted only out-of-process. This avoids treating an E3
request as authority to load an arbitrary path.

A CUDA-enabled build adds seven manifests whose backend is exactly `cuda`:
three Class-A replacements that require resident accelerator tensors and the
two in-process Class-C references described above, plus isolated spectrum and
SRS-ISAC workers. The shipped Class-B scheduler artifact remains CPU-only,
while the runtime also admits an
in-process Class-B package that declares a host-supported accelerator backend.
Such a package still uses the existing host-record hook; it does not acquire
resident scheduler tensors or an OCUDU-owned CUDA stream.

Copy each selected artifact, manifest, and named SBOM together beneath the DU's approved
catalog root, then list only normalized catalog-relative manifest paths:

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_manifests:
    - ref_spectrum_cpu.manifest.json
    - ref_srs_isac_cpu.manifest.json
    - ref_channel_estimator_cpu.manifest.json
    - ref_equalizer_cpu.manifest.json
    - ref_receiver_cpu.manifest.json
    - ref_scheduler_cpu.manifest.json
    - ref_spectrumd_cpu.manifest.json
  initial_instances:
    - package_manifest: ref_receiver_cpu.manifest.json
      placement: in_process
      backend: cpu
      target: shadow
      lifecycle_timeout_ms: 1000
    - package_manifest: ref_spectrumd_cpu.manifest.json
      placement: out_of_process
      backend: cpu
      target: active
      lifecycle_timeout_ms: 2000
```

Every `initial_instances` entry must name an already admitted manifest. The DU
uses the ordinary load/prepare/arm/shadow/activate path before E3 ingress and
reverses the complete selected set if any transition fails. A shadow instance
can receive normal E3 configuration or model operations before activation.
This orchestration remains working version v1; it does not create a startup
module ABI or a second manifest schema.

The three process manifests are admitted the same way. For each supported
single-interface Class-C executable, the DU binds the package identity to its
own producer-shard routes and allocates shared rings only when E3 loads an
instance. Loading a process package never accepts caller-supplied executable
arguments or routes.

See [Observer and authority operation](docs/observer_authority.md) for the
capability contract, coexistence workflow, monitoring checks, and expected
failure modes.

## Building and testing

```sh
# Release (-O3) is the default when CMAKE_BUILD_TYPE is unset; pass it explicitly
# so OTA Class-A packages meet PUSCH deadlines.
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/installed/ocudu-dapp-sdk
cmake --build build
ctest --test-dir build --output-on-failure
```

For the clean CPU sanitizer release gate, configure a separate tree so no
uninstrumented objects can be reused:

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DOCUDU_DAPP_BUILD_CUDA_EXAMPLES=OFF \
  -DOCUDU_DAPP_BUILD_PYTHON_CLASS_C=OFF \
  -DOCUDU_DAPP_ENABLE_ASAN=ON \
  -DOCUDU_DAPP_ENABLE_UBSAN=ON
cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
```

For the resident examples, select the target architecture explicitly when the
toolchain does not infer it (GB10 is SM 12.1):

```sh
cmake -S . -B build-cuda \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/installed/ocudu-dapp-sdk \
  -DCMAKE_CUDA_ARCHITECTURES=121
cmake --build build-cuda
ctest --test-dir build-cuda -R 'cuda' --output-on-failure
```

An OCUDU CUDA checkout can additionally run every packaged CUDA artifact through
its production verifier, loader, per-lane hooks, accelerated PUSCH or
CUDA-visible spectrum/SRS path, CUDA process export/restart path, completion
barriers, metrics, and unload path:

```sh
cmake -S /path/to/ocudu -B /path/to/ocudu/build-gpu \
  -DENABLE_CUDA=ON -DOCUDU_DAPP_REFERENCE_CUDA_DIR="$PWD/build-cuda"
cmake --build /path/to/ocudu/build-gpu --target pusch_resident_dapp_adapter_test
```
