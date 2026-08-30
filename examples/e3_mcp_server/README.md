<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# OCUDU MCP server (E3 companion for your gNB)

> Full guide — architecture, every tool, safety model, worked sessions and how to
> extend it toward agentic tuning: [`docs/mcp_companion.md`](../../docs/mcp_companion.md).

`ocudu_mcp_server.py` is a [Model Context Protocol](https://modelcontextprotocol.io)
server that turns an OCUDU gNB into a set of tools an AI assistant can use —
what is happening in the sector, who is connected, how the links and traffic
look, what the dApps are doing, and a controlled set of actions. **Everything
goes through the gNB's E3AP SCTP endpoint**: one association carries the
telemetry subscription and every management request. No local socket, no
root, no gNB-side plugin, no extra Python SDK (MCP over stdio is implemented in
the script; it needs only `ocudu_e3`, `asn1tools` and `numpy`).

```bash
# Claude Code
claude mcp add ocudu -- python3 /path/to/ocudu-dapp-sdk/examples/e3_mcp_server/ocudu_mcp_server.py --host 127.0.0.1 --port 36423

# Claude Desktop / any stdio MCP client (claude_desktop_config.json)
{ "mcpServers": { "ocudu": { "command": "python3",
    "args": ["/path/to/ocudu_mcp_server.py", "--host", "10.0.0.5", "--port", "36423"],
    "env": { "OCUDU_E3_SCHEMA_DIR": "/path/to/ocudu/schemas/e3" } } } }

# Observation only
python3 ocudu_mcp_server.py --read-only
```

Set `OCUDU_E3_PYTHON_DIR` / `OCUDU_E3_SCHEMA_DIR` if the `ocudu_e3` package
and ASN.1 schemas are not next to this checkout or installed under
`share/ocudu/dapp`. `--log-file` keeps diagnostics off stdout (the MCP channel).

## What the assistant gets

The server registers **28 tools** (20 observe, 8 act), 8 resources and 2
prompts. The tables below group them; the per-tool argument reference is
[`docs/mcp_companion.md` §4](../../docs/mcp_companion.md#4-tool-reference).

**Observe** (always on) — served from a live sector model the server builds
from `pusch.symbols`, `pusch.crc`, `srs.channel` and `gnb.metrics_json`:

| Tool | Answers |
|------|---------|
| `server_status` | E3 association state, dApp id / RAN identifier, telemetry catalog, current subscription, counters — the first thing to call when something looks off |
| `get_cell_overview` | active UEs, SINR/BLER spread, DL/UL throughput, PRB usage, error indications, late HARQs, DU MAC latency |
| `list_ues`, `get_ue` | per-UE PHY (SINR, EVM, TA, CFO, modulation, MCS index/table, TBS), UL BLER over a window, SRS state, scheduler/MAC view (CQI, MCS, bitrates, BSR, PUSCH/PUCCH SNR) |
| `get_kpis` | cell and per-UE KPIs over a window |
| `get_traffic_history` | bucketed time series: bitrates, UE count, PRB usage, SINR, MCS, CQI — for the cell or one UE |
| `get_srs_channel` | latest per-subcarrier channel response per Rx/Tx port pair |
| `get_constellation` | equalized I/Q from the UE's last PUSCH grant |
| `get_telemetry_catalog`, `get_e3_connections` | what the agent offers; who else is attached over E3 and what they subscribe to |
| `get_cell_info` | served cells as configured: PCI, PLMN/TAC, NCI, band, DL/UL ARFCN and centre frequency, bandwidth, PRBs, SCS, antennas, TDD pattern, PRACH config, RU gains, barred flag — plus which RAN controls the host can honour |
| `get_dapp_inventory`, `get_dapp_health`, `get_dapp_metrics`, `get_dapp_config`, `list_incidents`, `list_jobs`, `get_job`, `list_quiet_reservations`, `list_data_subscriptions` | the dApp lifecycle manager's view (mutations follow their job and report the real outcome) |

**Act** (hidden with `--read-only`; mutating tools default to `dry_run=true`
where the protocol supports it):

| Tool | Does |
|------|------|
| `set_dapp_config` | change typed configuration entries of a dApp instance — this is where scheduler parameters, gains and other cell knobs live (the reference scheduler, equalizer, channel emulator and sensing dApps each publish a schema). Kinds are taken from the instance schema; a config-generation check prevents lost updates |
| `dapp_lifecycle`, `load_dapp` | prepare / arm / shadow / activate / deactivate / retry / unload / breaker-probe; load a package |
| `release_ue` | RRC release-to-idle through the CU-CP control executor (serving PCI from gNB metrics when omitted) |
| `handover_ue` | handover to a target PCI through the CU-CP mobility handler |
| `set_rf_gain` | radio-unit TX/RX gain per port (applied immediately when `dry_run=false`) |
| `clear_incidents` | acknowledge incidents |
| `set_telemetry_streams` | re-subscribe this server (streams, requested interval) |

Resources (`ocudu://cell/overview`, `ocudu://cell/ues`, `ocudu://cell/kpis`,
`ocudu://cell/info`, `ocudu://dapps/inventory`, `ocudu://dapps/health`,
`ocudu://e3/connections`, `ocudu://e3/catalog`) expose the same data for
clients that prefer resources, and two prompts (`triage_cell`, `tune_dapp`)
give the assistant a safe procedure for the two common jobs.

Running it against a live gNB, prerequisites and troubleshooting for all the
Python E3 examples: [`docs/run_examples.md` §6](../../docs/run_examples.md#6-python-e3-examples-out-of-process-no-catalog-package).

## Honest scope

The server exposes exactly what the gNB exposes over E3 — nothing is faked.
The reference scheduler dApp publishes its policy as a typed schema (rank /
OLLA / MCS / packing / TPC modes, OLLA target BLER and step, TPC dead band,
ranking weights and per-UE back-off, PRB avoidance) so `set_dapp_config` tunes
the scheduler directly; other knobs appear the moment a dApp adds them to its
schema. RAN-level mutations are those in the management model's `ranControl`:
UE release, UE handover, RU TX/RX gain. Cell parameters that are not
runtime-mutable (band, bandwidth, TDD pattern, PRACH) are read-only in
`get_cell_info`.

## Tests

`tests/test_examples.py` drives the JSON-RPC handler against a fake E3
session: framing, tool listing (and hiding in read-only mode), sector-model
queries, the exact management requests built for `release_ue` and
`set_dapp_config`, resources and prompts. Live behaviour is verified against
the reference cell in `CHANGELOG.md` and the release record.
