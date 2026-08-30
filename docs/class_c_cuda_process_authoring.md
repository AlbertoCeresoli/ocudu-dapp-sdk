<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Authoring Class-C dApps (native, process, CUDA)

> Start a self-contained CPU Class-C vendor project with
> `ocudu-dapp-scaffold-class-bc-cpu --kind spectrum|srs-isac ...` (docs/tools.md).

End-to-end packaging and DU operations:
[`ocudu_dapp_quickstart.md`](ocudu_dapp_quickstart.md). This page covers Class-C
placement options with emphasis on the CUDA process export-pool path.

## Package config management (shared basis)

All Class A/B/C packages should use the shared package-config core in
`ocudu_dapp_sdk/package_config.h` (alias of `class_c_config.h`,
`make_config_management`). That is the **single** place for:

- value codecs (bool / double / string)
- describe / get / set dispatch
- generation conflict + dry-run
- instance `enabled`
- READ_ONLY and key aliases

Vendor packages only supply a static key table and stage/validate/commit hooks
into their private config store (see DeepSig spectrum/ISAC `management.cpp` and
[`vendor_toolkit.md`](vendor_toolkit.md)).

## Role

Class C is **open-loop analytics / sensing**. Producers never wait on consumers.
Results (when authorized) are compact, validated, and host-stamped. High-rate
tensors use native or supervised process routes; management and low-rate
portable snapshots use E3 (see OCUDU E3 guides).

Primary interfaces:

| Interface | Id | Reference algorithms |
|-----------|-----|----------------------|
| Spectrum | `0x00010001` | Plan-aware radiometer / per-PRB confidence map |
| SRS–ISAC | `0x00010002` | Bounded CIR / early-delay power preview |

## Placement matrix

| Placement | Artifact examples | Data path |
|-----------|-------------------|-----------|
| Native in-process CPU | `ref_spectrum_cpu`, `ref_srs_isac_cpu` | Direct host views |
| Native in-process CUDA | `ref_spectrum_cuda`, `ref_srs_isac_cuda` | Host stream + device grid/H |
| Supervised process CPU | `ref_spectrumd_cpu`, `ref_srs_isacd_cpu`, observer | SHM ring + seqpacket results |
| Supervised process CUDA | `ref_spectrumd_cuda`, `ref_srs_isacd_cuda` | D2D export pool + CUDA IPC |
| Supervised process (Python, research) | `class_c_python_spectrumd`, `class_c_python_srs_isacd` | Same process transport + pybind/DLPack + PyTorch sensing |
| Independent E3 consumer | `ref_spectrum_e3_cpu`, `ref_srs_isac_e3_cpu` | Portable host-snapshot chunks |
| Independent E3 consumer (Python, research) | [`examples/class_c_e3_python`](../examples/class_c_e3_python/README.md) | **pybind** → C++ `portable_e3_consumer` + **PyTorch** (host E3 must be PIC) |

**Python placement rule:** CPython is supported only for **Class-C out-of-process**
workers (supervised process packages or independent portable E3). Class-A/B and
Class-C **in-process** MODULEs remain C/C++/CUDA only. Python packages are a
research / education path, not production RAN performance — ship native
`ref_*d_*` (or equivalent) for product catalogs.

## Observer vs authority

Declaring spectrum input does **not** grant map or quiet authority. Packages
without interference-map and quiet-period capability bits are **observers**.
Exactly one active spectrum **result authority** is permitted across native and
process placements. See [observer_authority.md](observer_authority.md).

## Native CUDA Class C

- OCUDU borrows a CUDA-visible full-slot grid (spectrum) or derived CF32 H
  (SRS) on a host-owned per-producer stream.
- Module enqueues on the inherited stream, uses per-worker device scratch, and
  copies only a **compact** prepared report to pinned host storage.
- v1 prepared-output is synchronous: the reference waits on the stream before
  publishing the compact result. Full-grid migration is not performed.

## Supervised process (CPU)

Link `ocudu_process_client`:

1. Authenticate parent; call `apply_restricted_process_policy()` before route
   wait. It applies `no_new_privs` plus the reference seccomp deny-list.
2. Accept credentialed route offers; own ring/event capabilities.
3. Snapshot each route's bounded backlog, retire superseded entries, and run
   the newest publication from that snapshot. This latest-wins policy prevents
   an open-loop analytic from computing an already obsolete queue.
4. Validate the producer's monotonic tensor lease before running an algorithm;
   release every lease, including superseded and expired work.
5. Answer generation-bound heartbeats on the control channel.
6. Emit only bounded, source-correlated results when authorized; observers emit
   none.

Host verifies `no_new_privs` and, when configured, seccomp filter mode; applies
RLIMIT and bounded core-dump policy; then transfers descriptors. Malformed
routes are rejected without data delivery. CPU/GPU cores contain sensitive
runtime state, so field-debug deployments must use a protected destination,
retention bounds and matching build-id symbol archives.

## Supervised process (CUDA IPC)

The v1 CUDA process transport preserves async publication while keeping the
full spectrum grid or derived SRS H on the device.

### Host (DU) responsibilities

1. Own a fixed **export pool**: one CUDA allocation + interprocess-ready event
   per shared-ring slot.
2. After L1 readiness, perform **one D2D snapshot** of the exact tensor extent
   into a free export slot; record the ready event.
3. Publish FlatBuffer metadata + external slot id + byte extent on the ring
   (no tensor bytes, no process pointers).
4. Reclaim slots in FIFO order after lease completion; reset pools only after
   the old child is reaped.

### Worker responsibilities

Link `ocudu_process_client` and `ocudu_process_cuda_client`:

1. At route admission, import every fixed allocation and event once via
   `cuda_process_route`.
2. For each lease, wait on the selected event on a worker-owned nonblocking
   stream, run kernels, finish all device access, **then** destroy the lease.
3. Do not switch streams without an explicit dependency; do not import handles
   per publication.
4. Treat bad slot ids, extents, domains, or devices as invalid input; release
   the lease without producing a result.

The reference worker applies the same bounded latest-wins snapshot before it
imports or waits on a CUDA publication, so superseded export slots are returned
without launching obsolete device work.

Executable references: `ref_spectrumd_cuda`, `ref_srs_isacd_cuda`.

### Honesty and security

- This is a **one-copy** isolation boundary, not zero-copy and not a security
  boundary against a malicious GPU client.
- The reference seccomp policy reduces CPU syscall attack surface but does not
  constrain CUDA kernels or replace UID, namespace, cgroup or Landlock policy.
- Do not export arbitrary L1 allocations or secrets.
- Prefer CPU SHM placement when stronger containment matters.

### SRS specifics

The SRS route derives bounded `[rx][tx][frequency]` CF32 H on a route-owned
stream, then snapshots into the export pool. The worker runs the same bounded
CIR preview as the native CUDA reference and synchronizes its stream before
lease release.

## Multi-interface process packages

v1 supports manifest-ordered process route bundles with required/optional
atomic binding, legacy single-interface compatibility, restart renegotiation,
and E3 binding-state inventory. Required bindings that fail keep the instance
closed; optional bindings may report `unavailable`. See the multi-input fusion
test fixture for degradation semantics — it is not a seventh product dApp.

## Copy-and-rename checklist (process worker)

1. Copy `reference/process/` patterns and the matching algorithm library.
2. New UUID, global name, vendor, version; set interface compile definitions.
3. Observer-only: define `OCUDU_REFERENCE_OBSERVER_ONLY` (or equivalent caps=0).
4. Keep hot path allocation-free; weights/scratch in process/route state.
5. Build executable + manifest (out-of-process placement only).
6. Stage catalog; load with `placement=process` / `out_of_process` as configured.

## Monitoring

Healthy process authority: publications and accepted results advance; restarts
near zero; heartbeats advance. Observer: zero map results and protocol failures.
Incidents: `heartbeatLoss`, `processExit`, `securityViolation` (observer emit).

## Related

- [run_examples.md](run_examples.md) — enable/run every Class-C package on a DU
- [reference_packages.md](reference_packages.md)
- [observer_authority.md](observer_authority.md)
- OCUDU `docs/dapp/e3_system_design.md` (Class-C in the runtime)
- OCUDU `docs/doxygen/dapp_observability.dox` (copy accounting)
