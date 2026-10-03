<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU MCP companion — an AI assistant for your gNB, over E3

`examples/e3_mcp_server/ocudu_mcp_server.py` turns an OCUDU gNB into a set of
[Model Context Protocol](https://modelcontextprotocol.io) tools that any MCP
client — Claude Code, Claude Desktop, Cursor, a custom agent — can use to ask
what is happening in the sector and to act on it. This guide covers how it
works, how to run it, every tool it exposes, the safety model, worked
sessions against the reference cell, and how to grow it into agentic tuning.

## 1. Why this exists

Operating a cell means reading a dozen dashboards, remembering which knob lives
where, and correlating PHY, MAC and lifecycle evidence by hand. The gNB already
publishes all of that over one interface — **E3** — as telemetry (PUSCH
symbols and CRC, SRS channel response, raw grid, gNB metrics JSON, Class-C
snapshots) and as a management service model (dApp inventory, health, metrics,
typed configuration, lifecycle, incidents, jobs, RAN control). The companion
gives an assistant that interface as tools: it keeps a live model of the sector
from the telemetry and turns management requests into tool calls, so a question
like *"why is UL BLER high for this UE?"* or *"tighten the OLLA loop"* becomes a
short, checkable conversation instead of a shell session.

Everything is E3 by design: **one SCTP association**, no local socket, no root,
no gNB-side plugin, no MCP SDK dependency. Whatever the gNB does not expose over
E3 is deliberately *not* faked — adding a capability means adding it to the gNB
(section 8), after which the companion picks it up.

## 2. Architecture

```
MCP client (Claude Code / Desktop / agent)
        │  JSON-RPC 2.0 over stdio (MCP: initialize, tools/*, resources/*, prompts/*)
        ▼
ocudu_mcp_server.py
  ├── McpServer            protocol framing, tool registry, read-only gating
  ├── Tools                observe tools read SectorState; act tools issue management requests
  ├── SectorState          live model: UEs (PHY + MAC views), BLER windows, throughput/PRB/
  │                        SINR/MCS history rings, SRS response, constellation, cell counters,
  │                        CU-CP/AMF, DU latency, resource usage
  └── E3Session (thread)   owns the SCTP association: E3 setup → subscribe → ingest indications;
                           management calls are marshalled onto this thread (one association
                           carries both directions)
        │  E3AP-OCUDU-V2 (libe3 E3AP + OCUDU additions, ASN.1 APER over SCTP, port 36423)
        ▼
gNB E3AP endpoint: telemetry service model (RAN function 10) + management (RAN function 11)
```

Telemetry the server subscribes to by default: `pusch.symbols` (per-grant
equalized I/Q + link KPIs + grant parameters + CRC verdict), `pusch.crc`,
`srs.channel` (per-subcarrier response), `gnb.metrics_json` (the scheduler /
MAC / CU-CP / DU-low reports). When `--interval-ms` is omitted (the default)
it subscribes **without a rate request**, so it receives the agent's default
floors and never changes what other subscribers (the dashboard, recorders)
get. Passing `--interval-ms` — or calling `set_telemetry_streams` with
`interval_ms` — puts an explicit `minimum_interval_ms` in the subscription,
which the agent honours down to the advertised hard floor.

Management requests ride the same association as E3 control actions. Asynchronous
operations (`configSet`, lifecycle, load) answer `accepted` with a job id; the
companion **follows the job** and returns its real outcome — `success`,
`dryRunSuccess`, `invalidRequest` (validation), `conflict` (stale generation) —
and re-reads the effective values after a write, so the assistant never mistakes
"queued" for "done".

## 3. Running it

```bash
# Claude Code
claude mcp add ocudu -- python3 /path/to/ocudu-dapp-sdk/examples/e3_mcp_server/ocudu_mcp_server.py \
    --host 127.0.0.1 --port 36423

# Claude Desktop (claude_desktop_config.json) or any stdio MCP client
{ "mcpServers": { "ocudu": {
    "command": "python3",
    "args": ["/path/to/ocudu_mcp_server.py", "--host", "10.0.0.5", "--port", "36423", "--read-only"],
    "env": { "OCUDU_E3_SCHEMA_DIR": "/path/to/ocudu/schemas/e3",
             "OCUDU_E3_PYTHON_DIR": "/path/to/ocudu/python" } } } }
```

| Option | Meaning |
|--------|---------|
| `--host`, `--port` | gNB E3AP endpoint (`dapp.e3ap_sctp` in the gNB YAML; default `127.0.0.1:36423`) |
| `--read-only` | expose observation tools only; act tools are hidden from `tools/list` and refused |
| `--streams`, `--interval-ms` | telemetry streams to subscribe to and an explicit capture interval (0 = full rate) |
| `--dapp-name` | name shown in the gNB's E3 connection inventory (default `ocudu-mcp`) |
| `--log-file` | diagnostics go here; stdout is the MCP channel and must stay clean |

Requirements: Python ≥ 3.10, `asn1tools`, `numpy`, the `ocudu_e3` package
(found next to this checkout, in an installed `share/ocudu/dapp/python`, or via
`OCUDU_E3_PYTHON_DIR`) and the ASN.1 schemas (`OCUDU_E3_SCHEMA_DIR`). The gNB
must have `dapp.e3ap_sctp` enabled; the reference cell configuration does.

Check it is alive with any client, or drive it by hand:

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{}}}' \
               '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"server_status"}}' \
  | python3 ocudu_mcp_server.py | tail -1
```

## 4. Tool reference

`TOOL_SPECS` in `ocudu_mcp_server.py` registers **28 tools**: the 20
observation tools in §4.1 and the 8 action tools in §4.2. The example README
carries a shorter grouped table; this is the complete list.

### 4.1 Observe (always available)

| Tool | Arguments | Returns |
|------|-----------|---------|
| `server_status` | — | association state, dApp id, RAN identifier, catalog, subscription, counters |
| `get_cell_overview` | — | active UEs, UL SINR min/median/max, UL BLER mean/worst (10 s), DL/UL throughput, PRB usage per TDD slot, cell counters (error indications, late HARQs, latency, PRACH, Msg3), DU MAC and DU-low latency, CU-CP/AMF state and PDU-session counters, gNB CPU/memory, telemetry counters |
| `list_ues` | `window_s` | every observed UE: PHY view (SINR, EVM, RSRP, TA, CFO, modulation, **MCS index/table**, TBS, PRBs, layers), UL BLER over the window, SRS state, MAC view (CQI, RI, DL/UL MCS and bit rates, OK/NOK counts, BSR, HARQ/CE/SR delays, PUSCH/PUCCH SNR, TA) |
| `get_ue` | `rnti` | the same for one UE |
| `get_kpis` | `window_s` | cell series (bit rates, UE count, PRB usage, error indications, late HARQs) and per-UE KPIs over the window |
| `get_traffic_history` | `window_s`, `rnti?`, `keys?`, `buckets` | bucketed time series (mean/max/n per bucket) for the cell (`dl_bitrate_bps`, `ul_bitrate_bps`, `nof_ues`, PRB usage, …) or one UE (`dl_brate`, `ul_brate`, `sinr_db`, `ul_mcs`, `cqi`, …) |
| `get_srs_channel` | `rnti` | latest per-subcarrier SRS response (magnitude dB, phase) per Rx/Tx port pair |
| `get_constellation` | `rnti`, `max_points` | equalized I/Q points of the UE's last PUSCH grant |
| `get_telemetry_catalog` | — | streams the agent offers (ids, default and hard floors) and this server's subscription |
| `get_e3_connections` | — | downstream E3 associations (dashboards, observers, this server) with their subscriptions |
| `get_cell_info` | — | served cells: PCI, PLMN/TAC, NCI, band, DL/UL ARFCN and centre Hz, bandwidth/PRBs, SCS, antennas, TDD pattern, PRACH configuration, RU gains, barred flag; plus RAN-control capabilities |
| `get_dapp_inventory` | — | packages and instances (ids, states, backends, placement) |
| `get_dapp_health` | — | aggregate dApp health counters |
| `get_dapp_metrics` | `instance_id`, `schema?` | one instance's metrics or their schema |
| `get_dapp_config` | `instance_id`, `schema?` | effective configuration, or the schema (keys, kinds, ranges, defaults, descriptions) |
| `list_incidents` | `instance_id?`, `limit` | recent dApp incidents |
| `list_jobs`, `get_job` | —, `job_id` | asynchronous management jobs and their results |
| `list_quiet_reservations` | `cell_id?`, `limit` | quiet-period reservations |
| `list_data_subscriptions` | — | portable-store subscriptions (consumers, depth, drops) |

### 4.2 Act (hidden with `--read-only`; default to `dry_run=true`)

| Tool | Arguments | Effect |
|------|-----------|--------|
| `set_dapp_config` | `instance_id`, `entries{key: value}`, `boundary`, `dry_run`, `expected_config_generation?`, `kinds?` | typed configuration write: kinds come from the instance schema, the generation check prevents lost updates, the job is followed and effective values re-read. **This is where scheduler policy, OLLA/TPC targets, back-off weights and other dApp knobs live** |
| `dapp_lifecycle` | `instance_id`, `action`, `dry_run`, `timeout_us` | `prepare`, `arm`, `enterShadow`, `activate`, `deactivate`, `retryQuiescence`, `unload`, `breakerProbe` |
| `load_dapp` | `package_id`, `placement`, `backend`, `dry_run` | load a catalog package |
| `clear_incidents` | `through_incident_id` | acknowledge incidents |
| `release_ue` | `rnti`, `serving_pci?`, `dry_run` | RRC release-to-idle via the CU-CP control executor |
| `handover_ue` | `rnti`, `target_pci`, `serving_pci?`, `dry_run` | handover via the CU-CP mobility handler |
| `set_rf_gain` | `direction` (`tx`/`rx`), `gain_db`, `port_id`, `dry_run` | RU gain through the RU gain controller — applied immediately, not deferred |
| `set_telemetry_streams` | `streams[]`, `interval_ms?` | re-subscribe this server |

Tool annotations follow the MCP convention: observe tools carry
`readOnlyHint`, mutations that affect a UE or the radio carry
`destructiveHint`, so clients that gate on annotations behave sensibly.

### 4.3 Resources and prompts

Resources mirror the observation tools for clients that prefer them:
`ocudu://cell/overview`, `ocudu://cell/ues`, `ocudu://cell/kpis`,
`ocudu://cell/info`, `ocudu://dapps/inventory`, `ocudu://dapps/health`,
`ocudu://e3/connections`, `ocudu://e3/catalog`.

Prompts give the assistant a procedure: `triage_cell` (what to look at, what
thresholds matter, always explain with the numbers seen) and `tune_dapp`
(discover the schema, propose, dry-run, show before/after, then apply).

## 5. Safety model

- **Two rings.** Observe tools only read the sector model or issue read-only
  management queries. Act tools mutate; `--read-only` removes them entirely.
- **Dry-run first.** Every act tool that the protocol supports defaults to
  `dry_run=true`; the gNB validates (ranges, enum names, capability, target
  PCI, gain window) without side effects and the companion reports the job's
  verdict. Real writes require an explicit `dry_run=false`.
- **Generation checks.** Configuration writes carry the expected config
  generation; a concurrent change yields `conflict`, never a silent overwrite.
- **Host authority stays with the host.** RAN control is admitted by the
  gNB's authority object and deferred onto the owning component's executor
  (CU-CP control executor for release/handover); scheduler policy stages only
  take effect where the host YAML's `scheduler_allow_*` flags permit.
- **Transport trust.** The E3AP endpoint has no authentication yet
  (RR-015). Keep act tools on loopback or a trusted network, or run
  `--read-only` elsewhere.

## 6. Worked sessions (reference cell, 2026-08-26)

### 6.1 "How is the sector looking?"

`get_cell_overview` → 4 RNTIs active in the last 10 s, UL SINR −10…9.7 dB,
UL BLER mean 0.52 / worst 1.0, DL 0.92 Mb/s, UL 0.86 Mb/s, PRB usage 0.5/0.75
per TDD slot, no error indications or late HARQs, MAC latency 8 µs avg,
DU-low UL 286 µs avg, AMF connected.

`get_kpis(45)` / `list_ues` separated two populations:

- **0x4711** — the real user: PUSCH SNR 22 dB, CQI 15, DL MCS 26–27, UL MCS
  15–18, ~0.9 Mb/s each way, UL BLER 10–12 % on 4 000 TBs (i.e. sitting on the
  host OLLA's 10 % target).
- **0x474a / 0x474f / 0x4753 …** — short-lived RNTIs at PUSCH SNR −10 dB, UL
  MCS 0, BLER 98–100 % on a few hundred TBs, then gone: a device failing
  attach at the cell edge. An RF/coverage problem, not a scheduler one.

`get_dapp_health`: 6 deadline misses, 7 invalid inputs, 11 fallbacks lifetime
against tens of thousands of invocations — noise.

### 6.2 "Can we fine-tune the scheduler?"

`get_dapp_inventory` → scheduler instance; `get_dapp_config(schema=true)` →
22 keys; effective values showed full passthrough (`rank_mode=
conventional_passthrough`, `olla_mode=off`, `mcs_mode=off`, `pack_mode=off`,
`tpc_mode=off`). The host grants all `scheduler_allow_*` flags, so dApp policy
would take effect.

Proposal, dry-run through `set_dapp_config` → `dryRunSuccess`:

```json
{"olla_mode": "contained_baseline", "mcs_mode": "table_plus_olla",
 "olla_target_bler": 0.10, "olla_step_db": 0.05, "olla_max_offset_db": 6.0,
 "consecutive_winner_penalty": 0.35}
```

Reasoning the assistant can give: the good UE oscillates UL MCS 15–18 at the
BLER target, so a faster OLLA step with a 6 dB clamp should tighten it; because
retransmissions on this cell rarely decode (RR-023), a more conservative first
transmission pays more than HARQ; the winner penalty only matters with a second
attached UE. Out-of-range and unknown values are refused by the dApp's
validator (`invalidRequest`), a stale generation by the host (`conflict`) — both
were exercised live.

## 7. Extending the companion (Python side)

Adding a tool is three steps in `ocudu_mcp_server.py`:

1. Implement a method on `Tools` (observe: read `self.state`; act: call
   `self._guard(name)` first, build the request with `ocudu_e3.management`
   helpers, send via `self.session.management`, and wrap asynchronous results
   in `self._follow_job`).
2. Add a `TOOL_SPECS` entry: name, description (write it for the assistant —
   say what the numbers mean and when to use it), JSON input schema, and the
   mutating flag (drives read-only gating and annotations).
3. Optionally expose it as a resource (`RESOURCES`) or reference it in a
   prompt (`PROMPTS`), and add a case to `tests/test_examples.py` (the fake
   session records the exact management request built).

Ideas already on the list: an `events` tool (deltas since the last call —
UE arrivals/losses, BLER/SINR excursions, breaker trips, new incidents),
`get_spectrum_snapshot` / `get_pusch_burst` for evidence from `classc.spectrum`
and `pusch.grid`, `export_pusch_burst` for `ocudu-pusch-replay`, MCP resource
subscriptions for push updates, and a `link_budget(rnti)` tool computing MAC vs
PHY BLER, retransmission ratio and PRB share.

## 8. Extending the gNB side (what the companion can grow into)

The companion only ever reflects E3. To give it more to see or do:

- **A dApp knob** — add a `config_key` to the dApp's schema
  (`ocudu_dapp_sdk/package_config.h`; the reference scheduler in
  `reference/scheduler/module.cpp` shows numeric, boolean and named-enum
  keys with validation and atomic commit). `get_dapp_config(schema=true)`
  shows it and `set_dapp_config` writes it with no companion change.
- **A telemetry stream** — add a record to `OCUDU-TELEMETRY-SM`
  (`schemas/e3/e3sm-ocudu-telemetry-v1.asn1`), a publisher hook, and a
  decoder in `ocudu_e3.telemetry`; then ingest it in `SectorState`.
- **A RAN-control verb** — add a `RanControlAction` alternative and any
  fields to `E3SM-OCUDU-DAPP-MGMT-V1`, extend `ran_control_handler` and the
  gNB authority (`cu_cp_e3_ran_control` in `apps/gnb/gnb.cpp`), validate it in
  `management_service`, then add a tool. `handoverUe` and `setTxGain` are the
  template.
- **Cell facts** — extend `CellDescriptor` and the O-DU factory's fill.

Candidates: admission control / cell barring, per-UE MCS or PRB caps through
the scheduler dApp, PRACH-occasion marking in the spectrum plan (so sensing
needs no heuristics), transport counters in the connection inventory, and
authentication on the E3AP endpoint before act tools leave loopback.

## 9. Toward agentic tuning

The pieces for a closed loop are in place: observe (`get_kpis`,
`get_traffic_history`), act (`set_dapp_config` with dry-run and generation
check), and verify (re-read effective values, watch the KPI window). A tuning
agent should:

1. Establish a baseline window (e.g. 60 s of `get_kpis` and per-UE
   `get_traffic_history`).
2. Propose one change at a time, dry-run it, and record the before-state.
3. Apply with `dry_run=false`, wait a comparable window, and compare BLER,
   MCS distribution and throughput per UE.
4. Keep or revert (`set_dapp_config` again, same generation discipline), and
   write the decision and numbers down.

Guard rails belong in the loop, not the tools: bound the parameter ranges the
agent may touch, never change more than one policy stage at once on a live
cell, and prefer `--read-only` companions for anything that only needs to
watch.

## 10. Tests and verification

`examples/e3_mcp_server/tests/test_examples.py` drives the JSON-RPC handler
against a fake E3 session: framing (`initialize`, notifications, `tools/list`),
read-only gating, sector-model queries, the exact management requests built for
`release_ue`, `handover_ue`, `set_rf_gain` and `set_dapp_config`, job following,
resources and prompts. It is registered in the SDK's CTest as
`example.e3_mcp_server.protocol`. Live behaviour is verified against the
reference cell in `CHANGELOG.md` and the release record (28 tools; every observation populated;
dry-run and real writes, validation failures and generation conflicts all
reported correctly).
