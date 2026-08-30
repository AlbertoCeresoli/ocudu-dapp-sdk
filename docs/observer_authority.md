<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Spectrum observer and result authority

End-to-end packaging and DU operations:
[`ocudu_dapp_quickstart.md`](ocudu_dapp_quickstart.md). This page is the
observer versus single-writer authority contract for Class-C spectrum.

Class C separates input subscription from permission to affect the DU. A
spectrum package with interface capability bits equal to zero is an observer:
it may consume admitted grids and maintain private state, but it cannot publish
an interference map or quiet-period intent. `ref_spectrum_observerd_cpu` is the
reference isolated observer. It runs the same bounded radiometer input path as
the authoritative example and emits no result on the supervisor control
channel.

The output capabilities are:

| Capability | Meaning |
|---|---|
| `OCUDU_DAPP_SPECTRUM_CAP_INTERFERENCE_MAP_V1` | May propose the typed interference context consumed by the host cache. |
| `OCUDU_DAPP_SPECTRUM_CAP_QUIET_PERIOD_INTENT_V1` | May submit a bounded quiet-period intent for host validation. |
| `OCUDU_DAPP_SPECTRUM_CAP_FUTURE_UL_PLAN_V1` | Requests the committed future-UL-plan input route; it does not independently grant result authority. |

The DU permits exactly one active instance carrying either output capability.
That authority may be native or supervised-process. Any number of admitted
process observers may coexist within the configured route and memory bounds.
ABI v1 still has one native binding per interface, so multiple simultaneous
native spectrum observers are not supported; use isolated observers when
fanout is required.

The CPU reference authority publishes every admitted interference map, but it
rate-limits quiet-period intent attempts per cell to the lifetime of the prior
intent (500 ms). Multiple producer shards therefore cannot multiply scheduler
pressure. This is a reference safety policy in addition to the DU's validation,
budget, and duty-cycle guardrails; product authorities should retain an
equivalent explicit bound.

An observer is not a shadow authority. It must not send a result and cannot
become authoritative merely by constructing the correct wire record. A result
from a capability-zero process is a control-protocol violation: the host
withdraws its routes, increments `process.result_protocol_failures`, records a
security incident, and applies normal bounded restart policy. Peer-to-peer
dApp calls are also prohibited; composition uses host-owned typed routes.

## Package and deployment

Place the read-only executable and manifest together in the configured catalog
and admit only the manifest-relative path:

```yaml
dapp:
  catalog_root: /opt/ocudu/dapps
  package_manifests:
    - ref_spectrum_cpu.manifest.json
    - ref_spectrum_observerd_cpu.manifest.json
```

The observer manifest has a distinct package identity, out-of-process-only
placement, CPU backend, the spectrum interface, and `capabilities: 0`. Do not
edit a generated manifest: changing it invalidates the artifact/manifest
contract and bypasses the source-level observer test.

Use the ordinary E3 lifecycle for both roles:

```sh
# <ocudu>/apps/tools/dappctl/ocudu_dappctl.py; add --sctp HOST:PORT to use the
# gNB's E3AP SCTP endpoint instead of the local management socket.
DAPPCTL="python3 <ocudu>/apps/tools/dappctl/ocudu_dappctl.py --sctp 127.0.0.1:36423"
$DAPPCTL load --package PACKAGE_UUID --placement process --backend cpu
$DAPPCTL lifecycle prepare --instance INSTANCE_UUID --expected-generation N
$DAPPCTL lifecycle arm --instance INSTANCE_UUID --expected-generation N
$DAPPCTL lifecycle shadow --instance INSTANCE_UUID --expected-generation N
$DAPPCTL lifecycle activate --instance INSTANCE_UUID --expected-generation N
$DAPPCTL metrics get --instance INSTANCE_UUID
```

Pass the local socket, schema, and timeout options required by the deployment.
Every mutation is asynchronous: retain the returned job ID, wait for its
terminal result, and use the returned instance generation for the next
mutation.

## Observers over E3AP

Pure observers do not need a package at all. Out-of-process dApps can attach
to the gNB's E3AP SCTP endpoint, subscribe to `classc.spectrum` /
`classc.srs_isac` snapshots (or the PUSCH/SRS telemetry streams), and receive
bounded, drop-accounted indications without any authority over the radio.
[`../examples/e3_telemetry_python/`](../examples/e3_telemetry_python/README.md)
shows the pattern; the wire contract is OCUDU `docs/dapp/e3_wire.md`.

## Acceptance and monitoring

After activation, an observer is healthy when all of the following hold:

- `class_c.route.publications` and `class_c.copy.snapshot.count` advance as
  grids arrive;
- `process.results_accepted` stays zero;
- `process.result_protocol_failures` stays zero;
- heartbeat responses advance and `process.successful_restarts` does not
  advance in steady state;
- inventory reports the intended package identity, placement, and active
  lifecycle state.

Activating a second authority must fail with `lifecycleFailed` and a diagnostic
that another authority is active. The rejected instance enters the ordinary
error state; deactivate it to reach inactive, then unload it. Removing the
current authority invalidates its cached context and reservations before a new
authority is activated. Observers may remain active across that handover.

## Failure gallery

| Symptom | Expected host behavior | Operator action |
|---|---|---|
| Second result authority | Activation fails before publication is enabled. | Keep the existing authority or deactivate/unload it before retrying. |
| Observer emits a result | Protocol failure, route withdrawal, incident, bounded restart. | Disable/unload the package; treat it as a manifest or implementation defect. |
| Observer stops heartbeats | Heartbeat-loss incident, route withdrawal, kill/restart within configured limits. | Inspect incidents and restart counters; unload if recurrence continues. |
| Ring pressure | Backpressure/drop counters advance; the DU producer never waits. | Confirm the worker is retiring superseded work; then reduce subscription rate, increase bounded resources after measurement, or disable the observer. |
| Stale generation in a mutation | E3 conflict without changing the instance. | Refresh inventory and retry with the current generation and a new transaction ID. |
| Deactivation cannot prove quiescence | Resident quarantine; no forced native unload. | Retry quiescence or restart the DU during maintenance. |

The reference tests prove that the worker completes and releases every input
without emitting a control result, that its generated manifest declares zero
capabilities, and that it coexists with a real authority in a running
DU/dummy-RU scenario.
