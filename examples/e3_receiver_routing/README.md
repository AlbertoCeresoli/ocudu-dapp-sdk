<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# SINR-driven receiver routing

`sinr_router.py` is an independent E3AP client that chooses, for every UE, which
PUSCH receiver serves it: one receiver below an SINR threshold, another above.
It is the controller half of the gNB's per-UE routing of Class-A PUSCH dApps
(OCUDU `docs/dapp/pusch_routing.md`): the gNB holds a route table and makes the
choice for every grant; this process watches link quality and edits the table
about once a second.

```text
 gNB ── pusch.symbols (SINR per grant) ──▶ sinr_router.py
  ▲                                              │  mean SINR per RNTI, threshold,
  └──── puschRouting update (changed UEs) ◀──────┘  hysteresis, hold time
```

## Two receivers, or a receiver and the stock receiver

```bash
# Instance IDs of the active receivers (the Class-A instances in the inventory)
python3 -m ocudu_e3 --port 36423 inventory

# Neural receiver A below 12 dB, neural receiver B above
python3 sinr_router.py --below <instance-A> --above <instance-B>

# Neural receiver below 12 dB, the gNB's stock receiver above
python3 sinr_router.py --below <instance-A> --above conventional --threshold-db 12

# Check what it would do, validated by the gNB, without changing the table
python3 sinr_router.py --below <instance-A> --above conventional --dry-run --duration 30
```

A target is an instance ID, `conventional` (the stock receiver) or `primary`
(the gNB default: the first instance of each hook, as without routing). Both
target instances must be active before the router starts; it checks this and
exits with the list of active instances otherwise. Two instances of the same
hook (two receivers, say) need the gNB setting:

```yaml
dapp:
  pusch_route_slots: 2
```

A receiver instance and the stock receiver, or a receiver and an equalizer, work
with the default of one slot.

## Policy

| Option | Default | Effect |
|--------|---------|--------|
| `--threshold-db` | 12 | SINR that separates the two routes |
| `--hysteresis-db` | 2 | A UE changes route only once its mean leaves the band threshold ± hysteresis/2 |
| `--window-s` | 2 | Averaging window of the per-UE mean (arithmetic mean of the reported dB values) |
| `--min-samples` | 5 | Grants in the window before a UE is routed at all |
| `--hold-s` | 3 | Minimum time on a route before the UE may switch back |
| `--stale-s` | 10 | A UE without PUSCH for this long is removed from the table (RNTIs are recycled) |
| `--period-s` | 1 | How often changed UEs are sent, as one `update` |
| `--interval-ms` | 20 | Per-UE telemetry capture interval requested from the gNB |
| `--rnti` | all | Restrict routing to these UEs |
| `--keep` | off | Leave the entries in place on exit (by default the router removes every entry it set) |

The SINR comes from the receiver that served each grant. When the two receivers
report SINR differently, a UE right at the threshold could otherwise move
because of the difference alone; widen `--hysteresis-db` to more than that
difference. Entries apply to the RNTI in every cell, since the telemetry record
carries no cell index; use `ocudu_dappctl.py pusch-route` for cell-scoped
entries.

The policy is `SinrRoutePolicy`, a small class with no I/O, so it is easy to
replace: route on BLER, on MCS, on a model's own confidence metric, or on
anything else the E3 streams carry. `tests/test_examples.py` shows how to test a
policy offline.

## Checking the result

```bash
ocudu_dappctl.py --sctp 127.0.0.1:36423 pusch-route show     # the table the router maintains
ocudu_dappctl.py --sctp 127.0.0.1:36423 metrics get --instance <instance-A>
```

Each receiver's `dapp.invoke.count` counts exactly the grants of the UEs routed
to it. `unavailableLookups` in `pusch-route show` counts grants whose entry
named an instance that was not active (the stock receiver served them).

## Tests

`tests/test_examples.py` checks the policy (initial routing, hysteresis, hold
time, idle UEs) offline and, with `OCUDU_E3AP_TEST_SERVER` pointing at the
OCUDU loopback agent (`e3ap_test_server`), runs the router against it: both
loopback UEs are routed, and the table is empty again after exit.
