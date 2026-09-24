#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Route each UE's PUSCH to one of two receivers by its uplink SINR.

    # neural receiver A below 12 dB, neural receiver B above
    python3 sinr_router.py --below <instance-A> --above <instance-B>
    # neural receiver below 12 dB, the stock receiver above
    python3 sinr_router.py --below <instance-A> --above conventional --threshold-db 12
    # validate the decisions against the gNB without changing anything
    python3 sinr_router.py --below <instance-A> --above conventional --dry-run --duration 30

The controller subscribes to `pusch.symbols` (post-equalization SINR per grant),
keeps a sliding mean per RNTI and, once per period, moves a UE across the
threshold only when its mean clears the hysteresis band and it has held its
current route for the hold time. Only changed UEs are sent, as one `puschRouting`
`update`. UEs that stop transmitting are removed from the table after
`--stale-s`, so a recycled RNTI starts from the default. On exit the
controller removes every entry it set (unless `--keep`), leaving the table as
it found it.

A route target is an instance ID (from `inventory`), `conventional` (the stock
receiver) or `primary` (the gNB default). Both instances must be active; with
two instances of the same hook the gNB needs `dapp.pusch_route_slots: 2`. The
gNB makes the per-grant choice; this process only sets the table, so its pace
(1 s by default) never touches the PUSCH real-time path. The SINR it reads is
the one the serving receiver reports: when two receivers report differently,
widen the hysteresis so the difference alone cannot move a UE.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import signal
import sys
import time
from typing import Deque, Dict, Iterable, List, Optional, Tuple

from _common import connect, ocudu_e3
from ocudu_e3 import management

PuschSymbolsRecord = ocudu_e3.PuschSymbolsRecord


@dataclasses.dataclass
class UeState:
    samples: Deque[Tuple[float, float]] = dataclasses.field(default_factory=collections.deque)
    side: Optional[str] = None  # "below" or "above" once routed
    changed_at: float = float("-inf")
    last_seen: float = float("-inf")

    def mean(self) -> Optional[float]:
        return sum(value for _, value in self.samples) / len(self.samples) if self.samples else None


class SinrRoutePolicy:
    """Threshold policy with hysteresis, a minimum hold time and stale-UE removal. No I/O."""

    def __init__(self, threshold_db: float = 12.0, hysteresis_db: float = 2.0, window_s: float = 2.0,
                 hold_s: float = 3.0, stale_s: float = 10.0, min_samples: int = 5) -> None:
        if hysteresis_db < 0 or window_s <= 0 or hold_s < 0 or stale_s <= 0 or min_samples < 1:
            raise ValueError("invalid policy parameters")
        self.threshold_db = threshold_db
        self.hysteresis_db = hysteresis_db
        self.window_s = window_s
        self.hold_s = hold_s
        self.stale_s = stale_s
        self.min_samples = min_samples
        self.ues: Dict[int, UeState] = {}

    def observe(self, rnti: int, sinr_db: float, now: float) -> None:
        state = self.ues.setdefault(rnti, UeState())
        state.samples.append((now, sinr_db))
        state.last_seen = now
        while state.samples and state.samples[0][0] < now - self.window_s:
            state.samples.popleft()

    def decide(self, now: float) -> Tuple[Dict[int, str], List[int]]:
        """Returns the UEs whose side changed ({rnti: "below"|"above"}) and the routed UEs gone stale."""
        changes: Dict[int, str] = {}
        stale: List[int] = []
        for rnti, state in list(self.ues.items()):
            if now - state.last_seen > self.stale_s:
                if state.side is not None:
                    stale.append(rnti)
                del self.ues[rnti]
                continue
            while state.samples and state.samples[0][0] < now - self.window_s:
                state.samples.popleft()
            mean = state.mean()
            if mean is None or len(state.samples) < self.min_samples:
                continue
            half = self.hysteresis_db / 2
            if state.side is None:
                wanted = "below" if mean < self.threshold_db else "above"
            elif state.side == "above" and mean < self.threshold_db - half:
                wanted = "below"
            elif state.side == "below" and mean > self.threshold_db + half:
                wanted = "above"
            else:
                continue
            if state.side is not None and now - state.changed_at < self.hold_s:
                continue
            state.side = wanted
            state.changed_at = now
            changes[rnti] = wanted
        return changes, stale


def label(target: str) -> str:
    return target if target in ("conventional", "primary") else f"instance {target}"


def check_targets(client, targets: Iterable[str]) -> None:
    """Fail early with a clear message when a target instance is not active on a routable hook."""
    table = client.pusch_routing()["payload"]["puschRouting"]
    bound = {binding["instanceId"] for binding in table.get("bindings", [])}
    for target in targets:
        if target in ("conventional", "primary"):
            continue
        normalized = management.display_identifier(management.parse_identifier(target))
        if normalized not in bound:
            raise SystemExit(f"instance {target} is not active on a channel-estimator, equalizer or receiver hook "
                             f"(active: {', '.join(sorted(bound)) or 'none'})")


def apply(client, entries: List[dict], dry_run: bool) -> dict:
    response = client.pusch_routing("update", entries, dry_run=dry_run)
    if response["code"] not in ("success", "dryRunSuccess"):
        raise RuntimeError(f"route update refused: {response['code']} {response.get('errorText', '')}")
    return response


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    parser.add_argument("--below", required=True, help="route for UEs whose SINR is under the threshold")
    parser.add_argument("--above", required=True, help="route for UEs whose SINR is over the threshold")
    parser.add_argument("--threshold-db", type=float, default=12.0)
    parser.add_argument("--hysteresis-db", type=float, default=2.0, help="width of the no-switch band around the threshold")
    parser.add_argument("--window-s", type=float, default=2.0, help="SINR averaging window")
    parser.add_argument("--hold-s", type=float, default=3.0, help="minimum time on a route before switching back")
    parser.add_argument("--stale-s", type=float, default=10.0, help="remove a UE's entry after this long without PUSCH")
    parser.add_argument("--min-samples", type=int, default=5, help="grants needed in the window before routing a UE")
    parser.add_argument("--period-s", type=float, default=1.0, help="how often the table is updated")
    parser.add_argument("--interval-ms", type=int, default=20, help="per-UE telemetry capture interval")
    parser.add_argument("--rnti", action="append", help="only these RNTIs (repeatable, e.g. 0x4601)")
    parser.add_argument("--duration", type=float, default=0.0, help="stop after this many seconds (0: until Ctrl-C)")
    parser.add_argument("--dry-run", action="store_true", help="validate every update on the gNB, apply nothing")
    parser.add_argument("--keep", action="store_true", help="leave this controller's entries in place on exit")
    args = parser.parse_args()
    for target in (args.below, args.above):
        management.pusch_route_target(target)  # rejects malformed IDs before connecting

    policy = SinrRoutePolicy(args.threshold_db, args.hysteresis_db, args.window_s, args.hold_s, args.stale_s,
                             args.min_samples)
    rntis = [int(value, 0) for value in args.rnti] if args.rnti else []
    targets = {"below": args.below, "above": args.above}
    stop = {"requested": False}
    signal.signal(signal.SIGINT, lambda *_: stop.update(requested=True))
    signal.signal(signal.SIGTERM, lambda *_: stop.update(requested=True))
    started = time.monotonic()

    def finished() -> bool:
        return stop["requested"] or (args.duration > 0 and time.monotonic() - started >= args.duration)

    with connect(args.host, args.port, "sdk-sinr-router") as client:
        client.setup()
        check_targets(client, targets.values())
        config = ocudu_e3.SubscriptionConfig("pusch.symbols", rnti_filter=rntis, minimum_interval_ms=args.interval_ms)
        client.subscribe(["pusch.symbols"], configs=[config])
        print(f"routing below {args.threshold_db:g} dB to {label(args.below)}, above to {label(args.above)}"
              f"{' (dry run)' if args.dry_run else ''}", flush=True)
        routed: set = set()
        next_update = time.monotonic() + args.period_s
        try:
            # Poll with a short timeout so the table is also updated (and idle UEs age out) while
            # no uplink is scheduled.
            while client.connected and not finished():
                indication = client.next_indication(0.2)
                record = indication.record if indication is not None else None
                if isinstance(record, PuschSymbolsRecord) and record.sinr_db is not None:
                    policy.observe(record.rnti, record.sinr_db, time.monotonic())
                now = time.monotonic()
                if now < next_update:
                    continue
                next_update = now + args.period_s
                changes, stale = policy.decide(now)
                entries = [management.pusch_route_entry(rnti, targets[side]) for rnti, side in changes.items()]
                entries += [management.pusch_route_entry(rnti, "primary") for rnti in stale]
                if not entries:
                    continue
                apply(client, entries, args.dry_run)
                for rnti, side in sorted(changes.items()):
                    mean = policy.ues[rnti].mean()
                    print(f"rnti 0x{rnti:04x} {mean:5.1f} dB -> {side} ({label(targets[side])})", flush=True)
                    routed.add(rnti)
                for rnti in stale:
                    print(f"rnti 0x{rnti:04x} idle -> removed", flush=True)
                    routed.discard(rnti)
        finally:
            if routed and not args.keep and not args.dry_run:
                apply(client, [management.pusch_route_entry(rnti, "primary") for rnti in sorted(routed)], False)
                print(f"removed {len(routed)} entries", flush=True)
            client.release()
    return 0


if __name__ == "__main__":
    sys.exit(main())
