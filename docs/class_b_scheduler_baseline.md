<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Class-B baseline-replica scheduler

**Package:** `org.ocudu.reference.scheduler.cpu` → artifact `ref_scheduler_cpu`  
**UUID:** `8f0d0001-4f9c-4e31-8c00-000000000006` · interface `0x00010006` ·
in-process / cpu  
**Entry:** `ocudu::dapp::reference::run_scheduler_pipeline`  
**ABI:** `ocudu/dapp/use_cases/v1/scheduler.h`

**Enable / run on a live DU** (YAML + E3 evaluation → full authority):
[`run_examples.md`](run_examples.md) §3 ·
[`class_b_accelerator_authoring.md`](class_b_accelerator_authoring.md)
(“Enable and run”).

This dApp is a **fiddleable baseline replica** of the decisions OCUDU usually takes
inside conventional UE scheduling: **who** (rank), **how robust** (OLLA + MCS table),
**how many / where PRBs** (pack), and **UL power step** (TPC).

It never builds grants. It emits **intents**. The host validates, clamps, packs
legally, and still owns HARQ retx, slice RRM, PDCCH/UCI, and DCI.

---

## 1. Big picture

```
                         ┌──────────────────────────────────────────┐
                         │              OCUDU host                   │
                         │                                          │
  retx first ──────────► │  free_vrb_mask (1=free)                  │
  conventional QoS ────► │  conventional_priority per UE            │
  CSI / SNR / PHR ─────► │  candidate features + mcs_table_id       │
  ACK/CRC outcomes ────► │  last_tx_outcome / last_used_mcs         │
  guardrails ──────────► │  allowed_output_flags                    │
                         └──────────────────┬───────────────────────┘
                                            │ Class-B invoke (≤100 µs)
                                            ▼
┌───────────────────────────────────────────────────────────────────────────┐
│  ref_scheduler_cpu  —  run_scheduler_pipeline                             │
│                                                                           │
│   candidates[]                                                            │
│        │                                                                  │
│        ▼                                                                  │
│   ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐   │
│   │  OLLA   │──►│  RANK   │──►│   MCS   │──►│  PACK   │──►│   TPC   │   │
│   │ offset  │   │ who?    │   │ table!  │   │ multi-UE│   │ UL only │   │
│   └─────────┘   └─────────┘   └─────────┘   └─────────┘   └─────────┘   │
│        │              │             │             │             │         │
│        └──────────────┴─────────────┴─────────────┴─────────────┘         │
│                                    │                                      │
│                                    ▼                                      │
│                         intents[]  (+ optional avoid mask)                │
└────────────────────────────────────┬──────────────────────────────────────┘
                                     │
                                     ▼
                         host validate → commit → allocate
```

**Authority model:** host sets `allowed_output_flags` per invocation from YAML
(`scheduler_allow_mcs`, `scheduler_allow_prb_*`, `scheduler_allow_tpc`, …).  
`emit_only_allowed=true` (default) ⇒ the package may compute everything but
**only sets intent bits the host allowed**. With OTA lab flags all true and
`scheduler_evaluation_only: false`, the shipped default still preserves host
newTx MCS, PRB packing, UL TPC, layers, and avoidance. Those stages become
authoritative only when explicitly selected in `scheduler_reference_config`
(the host still clamps legality / retx / PDCCH).

---

## 2. What you can fiddle with

All knobs live in `scheduler_reference_config` (module shared state).

```
                    ┌────────────────────────────────────┐
   rank  ──────────►│ backlog | conventional_passthrough │  "who gets served"
                    │           | pf_lite                │
                    └────────────────────────────────────┘
   olla  ──────────►  off | host_offset_echo | contained_baseline
   mcs   ──────────►  off | host_echo | table_plus_olla     ◄── uses mcs_table_id
   pack  ──────────►  off | limits_only | soft_prefer_clean
                      | hard_pack_first_fit_low|high|prefer_clean
   tpc   ──────────►  off | hold0db | target_sinr
```

**Shipped defaults** (`scheduler_reference_config` in
`reference_scheduler_pipeline.h` — used by `ref_scheduler_cpu`):

| Knob | Default | Why |
|------|---------|-----|
| `rank` | `conventional_passthrough` | Same UE order as host time-QoS/RR |
| `olla` | `off` | Preserve host OLLA |
| `mcs` | `off` | Preserve host link adaptation |
| `pack` | `off` | Preserve host PRB allocation |
| `tpc` | `off` | Preserve host UL power-control decision |
| `emit_only_allowed` | `true` | Safe when host only grants priority / partial flags |
| `emit_layers` | `false` | Preserve host layer selection |
| `enable_prb_avoidance` | `false` | Preserve the host PRB mask |

`emit_only_allowed=true` is an authority bound: an enabled stage only sets
intent bits that the host put in `allowed_output_flags`. The allow flags do not
turn a disabled stage on. With the shipped defaults, OTA lab YAML may grant all
authority without changing host MCS, packing, TPC, layers, or PRB avoidance.

### Candidate telemetry contract

The host publishes these policy inputs independently of the selected fallback
scheduler, with a validity bit for every optional value:

| Feature | v1 host semantics |
|---------|-------------------|
| `pending_bytes` | Directional newTx backlog for this UE and slice |
| `hol_delay_us` | Age of the oldest pending DL SDU at `transmission_time`; deliberately invalid for UL because BSR has no packet timestamp |
| `average_rate_bps` | Directional, per-slice served-rate EMA (`alpha=0.01`), converted from bytes per effective scheduling opportunity using the invocation numerology; valid from UE activation, including zero for a new UE |
| `bler` | Exact error ratio over the most recent 64 first-transmission outcomes in this direction; DL DTX and all retransmission outcomes are excluded |
| `bler_nof_observations` | Number of outcomes represented by `bler` (1 through 64 when `BLER_VALID`), so policy code can gate cold-start estimates |

The candidate record remains 104 bytes on LP64. `bler_nof_observations` uses
previously reserved tail storage, and the ABI layout test pins both its offset
and all promoted scheduler-feature offsets. Validators use the immutable
`OCUDU_DAPP_SCHEDULER_CANDIDATE_BASE_SIZE_V1` (88 bytes), never the movable
reserved-tail offset; consumers check the 92-byte extension size before reading
the BLER observation count.

**Conservative / host-echo knobs** (useful for A/B vs conventional LA):

```cpp
config.olla = scheduler_olla_mode::host_offset_echo; // host OLLA offset only
config.mcs  = scheduler_mcs_mode::host_echo;         // copy host LA MCS
config.tpc  = scheduler_tpc_mode::hold0db;           // DCI TPC field = 1 (0 dB)
```

To **experiment** (e.g. aggressive packing + custom OLLA target):

```cpp
config.rank = scheduler_rank_mode::pf_lite;
config.olla = scheduler_olla_mode::contained_baseline;
config.olla_cfg.target_bler = 0.05f;   // more aggressive link adaptation
config.mcs  = scheduler_mcs_mode::table_plus_olla;
config.pack = scheduler_pack_mode::hard_pack_prefer_clean;
config.tpc  = scheduler_tpc_mode::target_sinr;
```

---

## 3. MCS tables (do not ignore)

Every candidate carries **`mcs_table_id`** filled by the host from the UE’s
configured PDSCH/PUSCH MCS table. The package **must** use that id — MCS index
alone is meaningless without the table.

### 3.1 Table ids (ABI)

```
  mcs_table_id
  ┌────┬─────────────────────────────────────────────────────────┐
  │ 0  │ qam64        TS 38.214 5.1.3.1-1 / UL 6.1.4.1-1         │
  │ 1  │ qam256       TS 38.214 5.1.3.1-2 / UL 6.1.4.1-2         │
  │ 2  │ qam64LowSe   TS 38.214 5.1.3.1-3                         │
  └────┴─────────────────────────────────────────────────────────┘
```

Also: **`transform_precoder`** (UL) reduces max MCS to **27** for non-256 tables
(reserved indices), same as host `get_max_mcs_ul`.

### 3.2 Where the table is used

```
                    candidate.mcs_table_id
                              │
          ┌───────────────────┼───────────────────┐
          ▼                   ▼                   ▼
   CQI → MCS map      SNR → MCS ladder     max_mcs / SE
   (DL tables 0/1/2)  (UL qam64 vs 256)   (clamp + PRB size)
```

| Path | Table effect |
|------|----------------|
| DL `table_plus_olla` | Different CQI→MCS spectral-efficiency maps per table |
| UL `table_plus_olla` | Different SNR ladders (64 vs 256); LowSE reuses 64 ladder (host-like) |
| Clamp | `max_mcs_for_table(id, tp)` → 28 or 27 |
| PRB sizing | `mcs_spectral_efficiency(mcs, id)` — LowSE lower SE, 256 higher at high MCS |
| Host validate | Rejects recommended_mcs > table max |

Helpers: `reference_scheduler_tables.h`  
(`map_cqi_to_mcs_portable`, `map_snr_to_mcs_ul_portable`, `max_mcs_for_table`,
`clamp_mcs_for_table`, `mcs_table_name`).

### 3.3 Algorithm: MCS selection

```
function select_mcs(candidate, direction, olla_offset, config):
    table ← candidate.mcs_table_id   # 0/1/2
    tp    ← candidate.transform_precoder
    lo,hi ← expert_mcs_min/max ∩ [0, max_mcs_for_table(table, tp)]

    if config.mcs == host_echo and MCS_VALID:
        return clamp(candidate.recommended_mcs, lo, hi, table, tp)

    if config.mcs == table_plus_olla:
        if DL and CQI_VALID:
            eff ← cqi + olla_offset          # fractional CQI ok
            mcs ← interpolate(map_cqi(floor(eff), table),
                              map_cqi(ceil(eff),  table))
            return clamp(mcs, lo, hi, table, tp)
        if UL and SINR_VALID:
            mcs ← map_snr(sinr_db + olla_offset, table, tp)
            return clamp(mcs, lo, hi, table, tp)
        if MCS_VALID:
            return clamp(candidate.recommended_mcs, lo, hi, table, tp)

    return lo   # safe fallback
```

**Fiddle ideas**

- Bias toward robustness: force `mcs_table_id` interpretation LowSE SE, or cap `maximum_mcs`.
- Bias toward rate: use 256 maps only when host table is 256 (never invent 256 MCS on a 64 UE).
- Compare `host_echo` vs `table_plus_olla` under evaluation mode to measure table drift.

---

## 4. Algorithm: OLLA (outer-loop link adaptation)

```
                    ACK / CRC OK                NACK / CRC KO
                         │                            │
                         ▼                            ▼
                   offset += Δ_down            offset -= Δ_up
                                                   Δ_up = Δ_down · (1−BLER)/BLER

                   clamp offset ∈ [−max, +max]
                   (no +step at max MCS, no −step at min MCS)
```

```
function olla_update(offset, ack, used_mcs, mcs_min, mcs_max, cfg):
    Δ_down ← cfg.step_db
    Δ_up   ← (1 − cfg.target_bler) · Δ_down / cfg.target_bler
    if used_mcs ≥ mcs_max: Δ_down ← 0     # saturation guard
    if used_mcs ≤ mcs_min: Δ_up   ← 0
    offset ← offset + (ack ? Δ_down : −Δ_up)
    return clamp(offset, −cfg.max_offset_db, +cfg.max_offset_db)
```

| Mode | Behavior |
|------|----------|
| `off` | offset = 0 |
| `host_offset_echo` | use `host_olla_offset_db` (host still owns feedback) |
| `contained_baseline` | package-local map UE→offset; update from `last_tx_outcome` when `last_used_mcs` matches previous decision |

**Shipped OLLA defaults** (`olla_config` in
`include/ocudu_dapp_sdk/reference_scheduler_olla.h`):

| Field | Default | Meaning |
|-------|---------|---------|
| `target_bler` | `0.1` | target first-transmission BLER |
| `step_db` | `0.01` | Δ_down per ACK (Δ_up derived from `target_bler`) |
| `max_offset_db` | `5.0` | symmetric offset clamp (±5 dB) |

**Fiddle:** `olla_cfg.target_bler` (e.g. 0.1 → 0.01), `step_db`, `max_offset_db`.

---

## 5. Algorithm: rank (who)

```
function rank(candidate, state, config):
    if conventional_passthrough:
        if CONVENTIONAL_FORBID: FORBID
        else priority ← conventional_priority     # host time-QoS / RR

    else if backlog:
        priority ← w·log1p(pending_bytes)
                 + (SR ? sr_bonus : 0)
                 − (same UE won last slot ? winner_penalty : 0)
                 − candidate_id·1e-9

    else if pf_lite:
        se     ← spectral_efficiency(mcs, mcs_table_id)
        estim  ← se · layers · (pending>0)
        avg    ← average_rate_bps (or 1)
        priority ← estim / avg^α  + SR bonus
```

---

## 6. Algorithm: multi-UE pack (grid shape)

Host supplies **free_vrb_mask** after retx (`1` = free for newTx).

```
working_free ← copy(free_vrb_mask)
order ← candidates sorted by priority (desc)

for ue in order (skip FORBID):
    need ← estimate_prbs(pending, mcs, layers, mcs_table_id, remaining_slice)
    start ← find_run(working_free, need, policy)
         # first_fit_low  : search from PRB 0
         # first_fit_high : search from top of band
         # prefer_clean   : maximize overlap with interference==0
    if found:
        paint working_free[start : start+need] ← 0
        intent.preferred_start_prb / preferred_nof_prbs ← hard assignment
        remaining_slice −= need
    else if forbid_on_pack_fail:
        FORBID
```

```
  Band (example)          first_fit_low              first_fit_high
  [0 ................ N)  [UE0][UE1][  free  ]       [  free  ][UE1][UE0]

  prefer_clean (interference map)
  dirty ####....####....  → place UEs in clean "...." islands first
```

**Fiddle use cases**

| Goal | Pack mode |
|------|-----------|
| Match host | `hard_pack_first_fit_low` |
| Spectrum edge free for sharing | `hard_pack_first_fit_high` or custom edge pack |
| Avoid sensed interferers | `hard_pack_prefer_clean` + spectrum interference context |
| Soft only | `soft_prefer_clean` / `limits_only` |

Under `ALLOW_PRB_ASSIGNMENT`, a preference that is a subset of the free mask,
non-overlapping with other hard assigns, and inside the slice budget is
committed as a hard assign. An in-band preference that fails those occupancy
checks is kept as a soft preference (host first-fit fallback) and does **not**
reject the invoke (MCS / TPC / priority stay). Off-cell / zero-length ranges
are still invalid.

---

## 7. Algorithm: TPC (UL)

```
gap = tpc_target_sinr_db − sinr_db

  gap > +2.5 dB  →  TPC=3  (+3 dB)
  gap > +0.5 dB  →  TPC=2  (+1 dB)
  gap > −0.5 dB  →  TPC=1  ( 0 dB)
  else           →  TPC=0  (−1 dB)
```

`hold0db` always emits TPC=1 (common when host closed-loop PC is disabled).

---

## 8. Intent → host (what actually moves air)

```
  INTENT_PRIORITY / FORBID     → sort / drop
  INTENT_MCS                   → recommended_mcs (table-clamped by host)
  INTENT_PRB_LIMITS            → min/max PRB size
  INTENT_PRB_PREFERENCE        → soft start/nof  OR hard assign if ALLOW_ASSIGNMENT
  INTENT_TPC                   → DCI TPC field (UL)
  INTENT_LAYERS                → RI clamp
  OUTPUT_AVOID_PRB_MASK        → block newTx PRBs (UL, with interference evidence)
```

Host still: retx, PDCCH, code-rate/TBS legality, quiet/SSB holes, slice RRM.

---

## 9. Allow-flags cheat sheet

| Host flag | Package may emit |
|-----------|------------------|
| (always with PRIORITY path) | `PRIORITY` / `FORBID` |
| `ALLOW_MCS` | `INTENT_MCS` |
| `ALLOW_PRB_LIMITS` | `INTENT_PRB_LIMITS` |
| `ALLOW_PRB_PREFERENCE` | soft `preferred_*` |
| `ALLOW_PRB_ASSIGNMENT` | hard pack via `preferred_*` |
| `ALLOW_TPC` | `INTENT_TPC` |
| `ALLOW_LAYERS` | `INTENT_LAYERS` |
| `ALLOW_AVOID_PRB_MASK` | avoid mask (UL + interference) |

Default production path: **only priority** until guardrails enable more
(`scheduler_dapp_guardrails.allow_*` / YAML `scheduler_allow_*`). Evaluation mode
can validate rich intents without applying them.

### 9.1 Host integrators (OCUDU runtime)

YAML `scheduler_allow_*` alone is not enough. The host path must:

1. **Advertise** allow bits in `allowed_output_flags` (policy /
   `scheduler_dapp_policy.cpp` from guardrails).
2. **Accept** any subset of `OCUDU_DAPP_SCHEDULER_ALLOW_KNOWN_MASK_V1` in the
   native invoke gate (`lib/dapp/runtime/native_worker.cpp`). Rejecting unknown
   bits is correct; rejecting **known** MCS/PRB/TPC bits forces 100% Class-B
   fallbacks (`class_b.policy.fallbacks`).
3. **Publish free-VRB** after retx when granting `ALLOW_PRB_ASSIGNMENT`
   (`free_vrb_context_present` + mask). Without it, policy strips assignment
   authority for that invoke.
4. **Validate and commit** intents in policy (or discard under
   `scheduler_evaluation_only`).

Live health: `class_b.policy.accepted ≈ invocations` with near-zero
`fallbacks` / `validation_rejections` / `deadline_misses`.

---

## 10. End-to-end slot timeline

```
  slot N
  ──────
  1. retx scheduled                         (host)
  2. free_vrb = ~used                       (host → dApp input)
  3. conventional priorities                (host time_qos / RR)
  4. feature fill (CQI/SNR/MCS/table/…)     (host)
  5. Class-B pipeline                       (dApp, ≤100 µs budget)
  6. validate intents (+ joint pack)        (host)
  7. commit priorities + MCS/PRB/TPC        (host, or evaluation discard)
  8. stage1 PDCCH reserve by priority       (host)
  9. stage2 VRB: hard assign or first-fit   (host)
 10. DCI + HARQ bookkeeping                 (host)
```

---

## 11. How to try a new tweak (recipe)

1. **Fork config**, not the trampoline  
   The shipped defaults are the member initializers of `scheduler_reference_config`
   in `include/ocudu_dapp_sdk/reference_scheduler_pipeline.h`; override them in
   `make_reference_config()` in `reference/scheduler/module.cpp` or in your package.
2. **Pick one stage** to change (e.g. only pack).
3. **Keep `emit_only_allowed=true`**.
4. **Unit test offline**  
   `ctest -R reference_scheduler --output-on-failure`
5. **Host evaluation mode** before authoritative MCS/pack/TPC.
6. **Compare KPIs** vs conventional (BLER, MCS histogram, PRB edge occupancy).

Example: edge-pack for coexistence with spectrum Class-C

```cpp
config.pack = scheduler_pack_mode::hard_pack_first_fit_high;
config.mcs  = scheduler_mcs_mode::table_plus_olla; // respect mcs_table_id
config.rank = scheduler_rank_mode::pf_lite;
```

Example: low-BLER industrial

```cpp
config.olla = scheduler_olla_mode::contained_baseline;
config.olla_cfg.target_bler = 0.01f;
config.mcs  = scheduler_mcs_mode::table_plus_olla;
// Prefer lower maximum_mcs in a custom stage if you want a hard cap
```

---

## 12. Source map

| Concern | File |
|---------|------|
| Pipeline + config | `include/ocudu_dapp_sdk/reference_scheduler_pipeline.h` |
| Tables / MCS map | `include/ocudu_dapp_sdk/reference_scheduler_tables.h` + `.cpp` |
| OLLA | `include/ocudu_dapp_sdk/reference_scheduler_olla.h` + `.cpp` |
| Orchestration (main routines) | `reference/common/reference_scheduler_pipeline.cpp` |
| DSO trampoline | `reference/scheduler/module.cpp` |
| ABI | `ocudu/include/ocudu/dapp/use_cases/v1/scheduler.h` |
| Host feature / validate / commit | `ocudu/lib/scheduler/dapp/scheduler_dapp_policy.cpp` |
| Host invoke gate (`ALLOW_KNOWN_MASK`) | `ocudu/lib/dapp/runtime/native_worker.cpp` |
| Host free mask + consume | `intra_slice_scheduler.cpp`, `ue_cell_grid_allocator.*` |
| YAML allow / eval knobs | `ocudu/docs/dapp/yaml_configuration.md` |
| Tests | `tests/reference_scheduler_*_test.cpp` |

---

## 13. Offline tests

```bash
# ocudu-dapp-sdk build dir (headers from installed OCUDUDAppSDK)
ctest -R reference_scheduler --output-on-failure
```

Covers: CQI/SNR tables, **table id differences**, OLLA steps, allow-gating,
hard non-overlap pack, high-first pack, TPC map, passthrough rank, legacy entry.

Host:

```bash
# from the OCUDU host build tree (<ocudu>/build), not this repository
./tests/unittests/scheduler/policy/scheduler_dapp_policy_test
```

---

## 14. Related

- [class_b_accelerator_authoring.md](class_b_accelerator_authoring.md) — packaging / deadline / eval mode  
- [reference_packages.md](reference_packages.md) — artifact inventory  
- [ocudu_dapp_quickstart.md](ocudu_dapp_quickstart.md) — end-to-end catalog / E3 path  
- Host YAML: `ocudu/docs/dapp/yaml_configuration.md` (`scheduler_allow_*`, evaluation)  
- Host still owns retx, slice RRM, PDCCH/UCI, free-mask truth, final legality

## Configuration over E3 (management `configQuery` / `configSet`)

The reference scheduler publishes its policy knobs as a typed configuration
schema, so `ocudu_dappctl config-schema`, the dashboard and the MCP companion
(`set_dapp_config`) can read and change them with a config-generation check.
Writes are validated, applied atomically, and picked up by the next
invocation (the pipeline copies the live configuration once per slot).

| Key | Kind | Range / values | Meaning |
|-----|------|----------------|---------|
| `enabled` | bool | | accept data-path invocations (host bypass when false) |
| `rank_mode` | string | `conventional_passthrough`, `backlog`, `pf_lite` | UE ranking policy |
| `olla_mode` | string | `off`, `host_offset_echo`, `contained_baseline` | outer-loop link adaptation |
| `mcs_mode` | string | `off`, `host_echo`, `table_plus_olla` | MCS selection |
| `pack_mode` | string | `off`, `limits_only`, `soft_prefer_clean`, `hard_pack_first_fit_low`, `hard_pack_first_fit_high`, `hard_pack_prefer_clean` | PRB packing |
| `tpc_mode` | string | `off`, `hold0db`, `target_sinr` | uplink power control |
| `olla_target_bler` | real | 0.001–0.5 | OLLA target BLER (MCS back-off target) |
| `olla_step_db` | real | 0.001–1 | OLLA offset step per ACK/NACK |
| `olla_max_offset_db` | real | 0–20 | OLLA offset clamp |
| `tpc_deadband_db` / `tpc_step_hi_db` | real | 0–10 | TPC dead band / large-step threshold. Each is range-checked independently on `configSet` (`0 <= x <= 10`); no ordering between the two values is enforced |
| `backlog_weight`, `scheduling_request_bonus`, `consecutive_winner_penalty`, `pf_fairness_coeff` | real | see schema | ranking weights and per-UE back-off |
| `prb_avoidance_threshold` | integer | 0–255 | interference-map confidence above which a PRB is avoided (stored as `uint8_t`; a staged numeric value is cast to that integer type) |
| `assumed_data_symbols`, `prb_bytes_per_re_scale` | real | 1–14 / 0.1–4 | grant sizing assumptions |
| `enable_prb_avoidance`, `emit_layers`, `emit_only_allowed`, `forbid_on_pack_fail` | bool | | feature switches |

Host allow-flags remain the authority bound: enabling a policy stage here only
matters when the host YAML grants the corresponding `scheduler_allow_*`.

