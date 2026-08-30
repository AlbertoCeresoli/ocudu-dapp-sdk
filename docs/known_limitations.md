<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Known limitations before 0.1.0

This file is the public-facing release boundary. The detailed local readiness ledger is intentionally kept outside the repositories.

- The previous-release binary compatibility fixture cannot exist until the first release artifact is frozen.
- GH200 and RTX 4090 native qualification reports require access to those platforms.
- The GB10 Class A performance qualification currently requires a clean rerun and acceptance against explicit deadline thresholds.
- The Class B report must record explicit policy-stage approval in addition to zero timing misses.
- Final OTA evidence requires a stable RF/UE baseline; RF link failures must not be attributed to a dApp without correlated dApp health evidence.
- Reference algorithms are validated, extensible starting points. Their shipped signal-processing choices are not claims of production-optimal performance; the surrounding contract and safety shell are specifically designed to host stronger algorithms.
- The Class C process boundary provides fault containment, not a hostile multi-tenant sandbox.
- Python Class C examples are research conveniences and are not on the real-time production path.
- E3AP over SCTP has no TLS or token authentication in this release; the gNB binds loopback by default and admits peers by literal address. E3AP observers reaching a gNB across hosts need an external boundary (IPsec, VPN, network policy).
- E3AP-OCUDU-V1 mirrors the E3AP procedure set and field names used by libe3 but is not byte-compatible with it; interworking requires an envelope adapter.
- **Schema drift in the reference client tarball**: the E3 data-profile client tarball ships a trimmed `class-c-publication-v1.fbs` with different enumerator numbering than the normative OCUDU schema. Regenerate the client from the normative schema before interop, or enumerator values will silently disagree. (`tests/portable_e3_consumer_test.py` compiles the schema with `flatc` at test time and is the working example.)
- Telemetry attribution is single-cell: every publisher stream reports the publisher's `default_cell_id` (0 today) because upper-PHY diagnostics carry no cell identity.
- Real-time cost of subscribed telemetry: `pusch.grid` captures and the per-subcarrier `srs.channel` recompute read the resource grid on PHY threads (on CUDA-visible grids each read stages the grid to the host). Both are gated by `should_collect` — one atomic load when no subscriber exists — and rate-limited by per-RNTI floors plus a stream-wide hard floor. Ask the gNB operator before subscribing at high rates on production cells.
- Class-C observation is a stream, not guaranteed single-shot delivery: a record published while an isolated worker is still initializing (CUDA context setup takes seconds) expires at its lease and is dropped by design. Publishers must keep publishing fresh observations — as the live PHY does naturally — rather than expect a single publication to survive worker start-up. Worker-side consumers must drain past lease-expired records to reach fresher ones.


The `CHANGELOG.md` release entry and the release record must either close
these items or accept them explicitly.

- Third-party module deadlines are **advisory**: the boundary is measured and
  recorded, but an invocation is not preempted and the breaker does not open on
  deadline misses alone.
- Host inline Class-C copies the whole resource grid on the UL-PHY thread
  (~734 KB, 50-100 us/slot/cell); host inline Class-C is not a 5-cell
  configuration.
- On the resident Class-A inline path the conventional CSI measurement
  kernels are skipped: `pusch.symbols` telemetry and the E3 diagnostics
  carry post-EQ SINR only, with EPRE / RSRP / CFO / TA absent for those
  grants. Planned fix: the Class-A CE/EQ output contract gains
  reserved-slot fields so the dApp reports its own DMRS-derived
  measurements.
- The numeric push-metric (`host_api.metric_add`) and incident-cause id
  namespaces are vendor-private and unregistered: no catalog maps ids
  to names and independent vendors may collide. The string-named
  `describe_metrics`/`read_metrics` catalog is the interoperable surface.

The host repository's `docs/dapp/known_limitations.md` is the normative copy of the E3/telemetry entries above; sync both files when either changes.
