<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Authoring a Class-B scheduler dApp

End-to-end packaging and DU operations:
[`ocudu_dapp_quickstart.md`](ocudu_dapp_quickstart.md). **Copy-paste enable/run
for `ref_scheduler_cpu` (evaluation + full authority):**
[`run_examples.md`](run_examples.md) §3.

This page is the Class-B host-record contract, copy path from the reference,
and accelerator rules.

## Role and deadline

Class B is a **deadline-bound decision** hook on the scheduler path. The v1
production budget is **100 µs** for the complete host boundary (feature and
context preparation, callback, validation, and authoritative commit or
rollback). Conventional policy time is measured separately; it is not hidden
inside the dApp boundary.

The shipped reference is `reference/scheduler` → `ref_scheduler_cpu`.

## ABI boundary (v1)

- **Placement:** in-process only for current-slot authority. Out-of-process
  advisors may publish future Class-C context; they cannot become Class-B
  authority for the current slot.
- **Memory domain:** candidates, intent arrays, optional interference map, and
  PRB avoid mask are **host-visible records**.
- **No inherited PUSCH stream**, resident scheduler tensors, CUDA events, or
  asynchronous completion tokens from OCUDU.
- **Threading:** per-worker; keep lane-local history. No global mutable state
  shared across workers without explicit module synchronization.
- **Errors:** no exception may cross the C ABI. Invalid output, deadline miss,
  or open breaker preserves the conventional scheduler result.

## Copy-and-rename checklist

1. Copy `reference/scheduler/` into a new repository or target.
2. Mint a new package UUID, global name, vendor, and version.
3. Keep the query trampoline pure (`ocudu_dapp_query_v1`).
4. Implement ranking / MCS / pack / TPC logic via `run_scheduler_pipeline` (or
   your own stages). Prefer forking `scheduler_reference_config` knobs over
   rewriting the module trampoline.
5. Emit only intent bits granted by `allowed_output_flags` (`emit_only_allowed`).
6. Build against installed `OCUDUDAppSDK`; emit manifest-v1 + SPDX.
7. Stage into the DU catalog; load with `placement=in-process`, `backend=cpu`
   (or an admitted accelerator backend name if you use one internally).

Scaffold a self-contained Class-B project with
`ocudu-dapp-scaffold-class-bc-cpu --kind scheduler ...` (see `docs/tools.md`),
or use the reference in-tree as the template.

**Baseline pipeline (start here for algorithms):** rank, OLLA, MCS tables,
multi-UE free-VRB packing, TPC, allow-flag gating, and host integrator notes are
in [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md). This page
covers packaging, deadlines, accelerators, and evaluation mode only.

## Using an accelerator inside Class B

Class B defines **deadline and placement**, not algorithm technology. A package
may declare `cuda` (or another host-supported backend) and use it **inside** the
callback:

| Allowed | Forbidden |
|---------|-----------|
| Module-owned streams, device scratch, pinned staging | Expecting OCUDU-owned CUDA streams |
| Transfer host candidates → device → host intent | Resident scheduler tensor ABI (not in v1) |
| Prepare weights in create/prepare/worker hooks | Blocking beyond the Class-B deadline |
| `selected_backend=cuda` for admission/reporting | Out-of-process current-slot authority |

The complete round trip (copy in, compute, copy out, return) counts against the
deadline. Synchronize on the module's own stream before returning host outputs.

The repository intentionally ships **only** `ref_scheduler_cpu`. Introduce a
CUDA scheduler artifact only with deterministic host-record conformance tests
and target-hardware P50/P90/P99/P99.9 evidence under concurrent L1 load.

## Evaluation mode (host)

Before granting authority, configure the DU with
`scheduler_evaluation_only: true`. The instance still follows prepare → arm →
shadow → activate. Once active, the host:

1. Computes the conventional policy.
2. Invokes the dApp with the real bounded input.
3. Validates the complete dApp output.
4. **Discards** a valid dApp decision and keeps the conventional result.

Invalid output still records incidents and may open breakers. Evaluation mode is
**not** lifecycle SHADOW: SHADOW does not bind radio hooks and is not
representative of callback timing.

Host-measured E3 metrics include (non-exhaustive):

- `class_b.policy.invocations`
- `class_b.policy.conventional_time_ns`
- `class_b.policy.dapp_time_ns`

---

## Enable and run `ref_scheduler_cpu` on a DU

Full detail: **[run_examples.md](run_examples.md) §3**. Algorithms and allow-flag
cheat sheet: [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md).

| Field | Value |
|-------|-------|
| Artifact | `ref_scheduler_cpu.so` |
| Package UUID | `8f0d0001-4f9c-4e31-8c00-000000000006` |
| Interface | `0x00010006` |
| Placement / backend | **in-process / cpu** only (v1 current-slot authority) |

### Stage

```bash
export CATALOG=/opt/ocudu/dapps
cp build-cuda/ref_scheduler_cpu.{so,manifest.json,spdx.json} "$CATALOG/"
# or build-cpu/...
```

### Step A — evaluation (measure without authority)

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_signature_mode: allow_unsigned
  package_manifests:
    - ref_scheduler_cpu.manifest.json
  scheduler_evaluation_only: true
  scheduler_allow_mcs: true
  scheduler_allow_prb_limits: true
  scheduler_allow_prb_preference: true
  scheduler_allow_prb_assignment: true
  scheduler_allow_tpc: true
  scheduler_allow_layers: true
  local_e3:
    socket_path: /run/ocudu/dapp-management.sock
```

```bash
export OCUDU_DAPP_E3_SOCKET=/run/ocudu/dapp-management.sock
DAPPCTL=(python3 /path/to/ocudu/apps/tools/dappctl/ocudu_dappctl.py
  --socket "$OCUDU_DAPP_E3_SOCKET"
  --schema /path/to/ocudu/schemas/e3/e3sm-ocudu-dapp-mgmt-v1.asn1)

PACKAGE_ID=8f0d0001-4f9c-4e31-8c00-000000000006
"${DAPPCTL[@]}" load --package "$PACKAGE_ID" \
  --placement in-process --backend cpu
# INSTANCE / GEN from job

for action in prepare arm shadow activate; do
  "${DAPPCTL[@]}" lifecycle "$action" \
    --instance "$INSTANCE" --expected-generation "$GEN" --timeout-us 5000000
done
```

Expect `class_b.policy.invocations` and `dapp_time_ns` to advance while
conventional grants remain authoritative.

### Step B — full ownership

```yaml
dapp:
  # … same catalog / package_manifests …
  scheduler_evaluation_only: false
  scheduler_allow_mcs: true
  scheduler_allow_prb_limits: true
  scheduler_allow_prb_preference: true
  scheduler_allow_prb_assignment: true
  scheduler_allow_tpc: true
  scheduler_allow_layers: true
```

Restart the DU; re-load/activate if needed. Expect
`class_b.policy.accepted ≈ invocations` and near-zero fallbacks / deadline
misses.

### Disable

```bash
"${DAPPCTL[@]}" lifecycle deactivate --instance "$INSTANCE" \
  --expected-generation "$GEN" --timeout-us 5000000
"${DAPPCTL[@]}" lifecycle unload --instance "$INSTANCE" \
  --expected-generation "$GEN" --timeout-us 5000000
```

Without a Class-B package (or after unload), the conventional scheduler is the
sole authority.
- `class_b.policy.validation_time_ns`
- `class_b.policy.accepted` / `evaluation_accepted`
- `class_b.policy.fallbacks` / `validation_rejections` / `deadline_misses`

## Composition with spectrum (Class C)

dApps never call each other. Spectrum publishes into a **host-owned** cache;
the scheduler **borrows** it when provenance matches. Writers never wait;
readers make bounded snapshot attempts; mismatch returns bypass. The test-only
joint package proves multi-interface packaging — it is not a production product
template.

## Qualification notes

- Development timing campaigns use fixed matrices and unapproved-control
  policies by design when collecting engineering evidence.
- Production admission requires operator-approved affinity/scheduling/frequency
  controls and platform-scoped reports (OCUDU Class-B timing runner).
- Prefer zero validation rejections and investigated deadline misses before
  clearing evaluation mode.

## Related

- [class_b_scheduler_baseline.md](class_b_scheduler_baseline.md) — MCS / pack / TPC algorithms and defaults
- [reference_packages.md](reference_packages.md)
- [observer_authority.md](observer_authority.md)
- OCUDU `docs/dapp/yaml_configuration.md` (`scheduler_allow_*`, evaluation)
- OCUDU `docs/doxygen/dapp_operations.dox` (Class-B evaluation)
- OCUDU `docs/dapp/e3_management.md`
