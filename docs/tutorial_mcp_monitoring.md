<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Tutorial: asking your gNB questions and running KPI-tuning loops with an LLM agent

The [MCP server](../examples/e3_mcp_server/) turns the entire E3 surface of a
running gNB into tools an LLM agent (Claude or any MCP client) can call. This
tutorial is task-oriented: (1) connect and *ask questions* about the live
cell, and (2) set up a **standing monitoring loop** that watches KPIs and
tunes cell / dApp parameters from LLM reasoning — the "operations copilot"
pattern, from a single prompt.

For the exhaustive tool/resource/safety reference see
[`mcp_companion.md`](mcp_companion.md); this tutorial builds on it. To expose
your *own* dApp KPIs so an agent can query them, see
[exposing dApp KPIs over E3](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_dapp_kpis.md).

## 1. Connect

The server speaks MCP over stdio and holds **one E3AP/SCTP association** to
the gNB; every question and action flows through it.

```bash
# Claude Code
claude mcp add ocudu -- python3 \
  /path/to/ocudu-dapp-sdk/examples/e3_mcp_server/ocudu_mcp_server.py \
  --host 127.0.0.1 --port 36423

# Start safe: observation tools only (no config/lifecycle/RAN control exposed)
claude mcp add ocudu-ro -- python3 .../ocudu_mcp_server.py --read-only
```

> **Before you point an agent at anything but a lab cell**, read the safety
> notes below. The E3AP endpoint has no transport authentication in this
> release; `--read-only` and the per-tool `dry_run` defaults are your
> guardrails. Treat write access to a production cell as you would root.

## 2. Ask questions about the running gNB

With the server connected, just ask in natural language. The agent picks from
28 tools; the observation set includes:

| Ask about… | Tool the agent uses |
|------------|--------------------|
| Sector at a glance | `get_cell_overview`, `server_status`, `get_cell_info` |
| Connected UEs | `list_ues`, `get_ue` |
| Link/traffic KPIs | `get_kpis`, `get_traffic_history` |
| PHY detail | `get_srs_channel`, `get_constellation` |
| What dApps are doing | `get_dapp_inventory`, `get_dapp_health`, `get_dapp_metrics`, `get_dapp_config` |
| Problems | `list_incidents`, `list_jobs`, `get_job` |
| Telemetry available | `get_telemetry_catalog`, `list_data_subscriptions` |

Example prompts:

> *"What's the state of the cell right now — how many UEs, and how do the
> uplink SINR and throughput look over the last minute?"*

> *"Are any dApps unhealthy or bypassing? Check breakers and recent
> incidents."*

> *"Show me the worst-performing UE by SINR and what its constellation looks
> like."*

The server also ships two ready-made **prompts** (`prompts/list`):
`triage_cell` (a guided health sweep) and `tune_dapp` (a dry-run-then-confirm
tuning workflow) — invoke them from your MCP client's prompt menu.

## 3. Understand the action tools before you enable them

Eight tools *change* the gNB. All are hidden under `--read-only`. Every
mutating tool defaults to `dry_run=True` — the agent's first call **validates
only**; a real change requires an explicit `dry_run=false`:

| Tool | Effect | `dry_run` default |
|------|--------|-------------------|
| `set_dapp_config` | Typed config (scheduler BLER/MCS/TPC, EQ, emulator, gains) | **True** (validate only) |
| `set_rf_gain` | RU TX/RX gain, applied immediately | **True** (validate only) |
| `release_ue` | RRC release-to-idle of a UE | **True** (validate only) |
| `handover_ue` | Handover to a target PCI | **True** (validate only) |
| `dapp_lifecycle` | prepare/arm/shadow/activate/deactivate/unload/breakerProbe | **True** (validate only) |
| `load_dapp` | Load a catalog package | **True** (validate only) |
| `clear_incidents` | Ack incident history | n/a |
| `set_telemetry_streams` | Re-subscribe this session | n/a |

`set_dapp_config` fetches the schema and rejects keys not in it, and reads the
current `expected_config_generation` for a lost-update check — so an agent
cannot invent a knob or clobber a concurrent change. The generation model
(from [`e3_management.md`](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/e3_management.md)) makes
concurrent human + agent control safe.

## 4. Set up a monitoring-and-tuning loop

This is the payoff: because every KPI is a tool and every safe action is a
dry-run-by-default tool, a standing loop is just a recurring prompt. In Claude
Code, `/loop` runs a prompt on an interval; the same idea works from any
scheduler that can re-invoke your MCP client.

A conservative UL-SINR watchdog, every 5 minutes:

> **`/loop 5m`** *"Read `get_kpis` and `list_incidents` for cell 0. If uplink
> SINR has degraded more than 3 dB versus the last hour's typical value, or a
> dApp breaker is open: (a) summarize what changed, (b) propose one bounded
> adjustment — e.g. tighten the scheduler's MCS cap via `set_dapp_config`, or
> reset a breaker via `dapp_lifecycle breaker-probe` — and (c) apply it with
> `dry_run=false` only after showing me the dry-run result. Otherwise report
> 'nominal' and change nothing. Never touch RAN control (release/handover/gain)
> in this loop."*

The shape that makes this trustworthy:
1. **Observe first** — KPIs + incidents, every tick.
2. **Reason** — the agent decides whether anything crossed a threshold.
3. **Dry-run** — the proposed change is validated by the host before commit.
4. **Confirm** — commit only the change you (or a policy) approved.
5. **Bound the blast radius** — the prompt names exactly which tools are in
   scope; keep RAN control out of autonomous loops until you trust the policy.

Other loop patterns worth stealing:

- **Coverage/interference sweep:** watch `get_srs_channel` and the Class-C
  interference view; when an emitter appears, ask the agent to request a
  quiet-period reservation via `set_dapp_config` on the sensing dApp.
- **Model-health guard:** poll `get_dapp_health` and `get_dapp_metrics` for a
  neural-receiver instance; on a completion-deadline trend, have the agent
  `dapp_lifecycle` the shadow instance forward or recommend a rollback.
- **A/B referee:** with an old and a new receiver both loaded (see the
  [dappctl A/B workflow](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_dappctl_operations.md#3-swap-a-package-for-a-new-version-the-ab-workflow)),
  have the agent compare `get_dapp_metrics` across both each interval and flag
  when the new one is convincingly better.

## 5. Safety checklist for autonomous operation

- [ ] Start with `--read-only`; graduate to writes only on a lab/pre-prod cell.
- [ ] Keep `bind_address` on loopback (or an authenticated boundary) — the
      E3AP endpoint has no TLS/token auth today, and the loopback default is
      the boundary.
- [ ] In loop prompts, **enumerate the allowed tools**; explicitly exclude
      RAN control (`release_ue`/`handover_ue`/`set_rf_gain`) from unattended
      loops.
- [ ] Require a dry-run before every commit; the config tool's
      generation-CAS + schema check backs this up host-side.
- [ ] Prefer reversible actions (config, breaker-probe, shadow moves) over
      package unload/load in autonomous loops.
- [ ] Watch the agent: `list_jobs` and `list_incidents` are your audit trail
      of what it did.

## Where this fits

This is the same E3 surface the [`dappctl`
tutorial](https://gitlab.com/ocudu/work_groups/wg2_ai_ran/ocudu-dapp-platform/-/blob/main/docs/dapp/tutorial_dappctl_operations.md) drives by
hand — the MCP server just makes it agent-drivable and adds LLM reasoning on
top. A cell that observes its own KPIs, proposes bounded adjustments, and
applies them under human-set policy is the natural endpoint: the platform's
guardrails (typed validation, dry-run, generations, allow flags, host-arbitrated
control) are what make handing the loop to an agent as safe as handing it to a
script.
