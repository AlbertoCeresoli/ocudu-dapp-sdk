#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Out-of-process spectrum sensing over E3AP: detect external emitters in the uplink grid.

    python3 emitter_detector.py                       # print detections as they happen
    python3 emitter_detector.py --threshold-db 8 --min-slots 3 --record sensing.h5
    python3 emitter_detector.py --center-hz 3410100000 --json

This is not a dApp: it is an ordinary process that opens one SCTP association
to the gNB's E3AP endpoint, performs the E3 setup, subscribes to the
``classc.spectrum`` telemetry (the Class-C Spectrum publication: the complete
uplink resource grid every slot the spectrum route fires, plus the scheduler's
expected uplink use per symbol x PRB) and receives it as E3 indications. The
grid travels as a raw byte blob inside the APER indication — the only encoded
part is the small OCC3 header — so a Python subscriber keeps up with the full
rate (~120 grids/s, ~4 MB/s on a 51-PRB cell, ~120 us to decode each).

Detection is deliberately simple and transparent:

* per snapshot, mean power per (symbol, PRB) summed over receive ports;
* a per-PRB noise floor tracked as an exponential moving average of the power
  observed where the scheduler says the PRB is *planned empty* (nothing of
  ours transmits there), so it follows slow gain changes but not emitters;
* a PRB in an unscheduled cell exceeding floor + ``--threshold-db`` is a hit;
  hits are grouped into contiguous PRB runs and tracked across slots; a run
  that persists ``--min-slots`` slots is reported as an emitter with its
  centre frequency, bandwidth, excess power and duration, and again when it
  ends.

Scheduled cells (PUSCH/PUCCH/SRS/PRACH) are ignored, so our own UEs never
appear as emitters. Replace ``detect()`` with your own algorithm to build a
different sensing application on the same feed.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import numpy as np

from _common import connect, ocudu_e3
from ocudu_e3.classc import SpectrumSnapshot, decode_spectrum

PLANNED_EMPTY = 1
UNKNOWN = 0
WIDEBAND = "wideband"


@dataclass
class Track:
    first_prb: int
    last_prb: int
    first_seen_slot: int
    first_seen_ns: int
    last_seen_ns: int
    slots: int = 1
    peak_excess_db: float = 0.0
    reported: bool = False
    missed: int = 0


@dataclass
class Detector:
    threshold_db: float
    min_slots: int
    floor_alpha: float
    max_missed: int = 2
    #: PRBs never considered (default: the centre PRB, which carries the receiver's DC/LO spur).
    exclude_prbs: Tuple[int, ...] = ()
    #: A snapshot with more than this fraction of PRBs hit is reported once as a "wideband" event
    #: (receiver saturation, PRACH from a very close UE, duplexer leakage) instead of as emitters.
    wideband_fraction: float = 0.5
    floor_db: Optional[np.ndarray] = None
    tracks: List[Track] = field(default_factory=list)
    stats: Dict[str, int] = field(default_factory=lambda: {"snapshots": 0, "hits": 0, "emitters": 0})

    def detect(self, snap: SpectrumSnapshot) -> List[dict]:
        """Update the floor and tracks with one snapshot; return events (start/end) to report."""
        self.stats["snapshots"] += 1
        power = snap.prb_power()  # (symbols, nof_prb)
        power_db = 10.0 * np.log10(power + 1e-20)
        if snap.occupancy is not None and snap.occupancy.shape == power.shape:
            unscheduled = (snap.occupancy == PLANNED_EMPTY) | (snap.occupancy == UNKNOWN)
        else:
            unscheduled = np.ones_like(power, dtype=bool)  # no plan: treat everything as candidate

        # Noise floor per PRB from unscheduled cells: the median over symbols is robust to a
        # single emitter burst, and the EMA keeps slow drift while resisting fast changes.
        floor_sample = np.full(snap.nof_prb, np.nan)
        for prb in range(snap.nof_prb):
            column = power_db[unscheduled[:, prb], prb]
            if column.size:
                floor_sample[prb] = np.median(column)
        if self.floor_db is None:
            self.floor_db = np.where(np.isnan(floor_sample), np.nanmedian(floor_sample), floor_sample)
            return []
        valid = ~np.isnan(floor_sample)
        # Only let the floor learn from cells that are not currently above threshold.
        quiet = valid & (floor_sample < self.floor_db + self.threshold_db)
        self.floor_db[quiet] = (1 - self.floor_alpha) * self.floor_db[quiet] + self.floor_alpha * floor_sample[quiet]

        # Hits: unscheduled cells above floor + threshold; a PRB is hit if any of its symbols is.
        excess = power_db - self.floor_db[None, :]
        hit_cells = unscheduled & (excess > self.threshold_db)
        hit_prb = hit_cells.any(axis=0)
        prb_excess = np.where(hit_cells, excess, -np.inf).max(axis=0)
        for prb in self.exclude_prbs:
            if 0 <= prb < snap.nof_prb:
                hit_prb[prb] = False
        self.stats["hits"] += int(hit_prb.sum())

        events = []
        now_ns = snap.observation_time_ns or time.time_ns()
        slot_index = snap.sfn * (10 << snap.numerology) + snap.slot
        if hit_prb.sum() > self.wideband_fraction * snap.nof_prb:
            # Everything lit up at once: not a narrowband emitter. Report it as its own event kind
            # (rate limited to one per slot) and do not let it start or extend emitter tracks.
            self.stats["wideband"] = self.stats.get("wideband", 0) + 1
            events.append({"event": WIDEBAND, "sfn": snap.sfn, "slot": snap.slot,
                           "prb_hit": int(hit_prb.sum()), "nof_prb": snap.nof_prb,
                           "peak_excess_db": round(float(prb_excess[hit_prb].max()), 1)})
            return events
        runs = contiguous_runs(hit_prb)
        matched = set()
        for start, stop in runs:
            run_excess = float(prb_excess[start:stop].max())
            track = self.match(start, stop)
            if track is None:
                track = Track(start, stop - 1, slot_index, now_ns, now_ns, peak_excess_db=run_excess)
                self.tracks.append(track)
            else:
                track.first_prb = min(track.first_prb, start)
                track.last_prb = max(track.last_prb, stop - 1)
                track.slots += 1
                track.last_seen_ns = now_ns
                track.missed = 0
                track.peak_excess_db = max(track.peak_excess_db, run_excess)
            matched.add(id(track))
            if not track.reported and track.slots >= self.min_slots:
                track.reported = True
                self.stats["emitters"] += 1
                events.append(self.event("emitter", track, snap))
        for track in list(self.tracks):
            if id(track) in matched:
                continue
            track.missed += 1
            if track.missed > self.max_missed:
                self.tracks.remove(track)
                if track.reported:
                    events.append(self.event("emitter_end", track, snap))
        return events

    def match(self, start: int, stop: int) -> Optional[Track]:
        for track in self.tracks:
            if start <= track.last_prb + 1 and stop - 1 >= track.first_prb - 1:
                return track
        return None

    def event(self, kind: str, track: Track, snap: SpectrumSnapshot) -> dict:
        centres = snap.prb_center_frequencies_hz()
        prb_bw = 12 * snap.subcarrier_spacing_hz
        f_lo = centres[track.first_prb] - prb_bw / 2
        f_hi = centres[track.last_prb] + prb_bw / 2
        return {
            "event": kind,
            "sfn": snap.sfn,
            "slot": snap.slot,
            "prb_first": track.first_prb,
            "prb_last": track.last_prb,
            "bandwidth_hz": float(f_hi - f_lo),
            "center_hz": float((f_lo + f_hi) / 2),
            "peak_excess_db": round(track.peak_excess_db, 1),
            "duration_ms": round((track.last_seen_ns - track.first_seen_ns) / 1e6, 1),
            "slots_seen": track.slots,
        }


def contiguous_runs(mask: np.ndarray) -> List[Tuple[int, int]]:
    runs = []
    start = None
    for index, flag in enumerate(mask.tolist() + [False]):
        if flag and start is None:
            start = index
        elif not flag and start is not None:
            runs.append((start, index))
            start = None
    return runs


class Recorder:
    """Optional HDF5 log of every snapshot's per-PRB power, occupancy and floor, plus events."""

    def __init__(self, path: str) -> None:
        import h5py

        self.h5py = h5py
        self.file = h5py.File(path, "w")
        self.events = []
        self.pending = {"power_db": [], "occupancy": [], "floor_db": [], "sfn": [], "slot": [], "time_ns": []}

    def add(self, snap: SpectrumSnapshot, power_db: np.ndarray, floor_db: Optional[np.ndarray]) -> None:
        self.pending["power_db"].append(power_db.astype(np.float32))
        self.pending["occupancy"].append(snap.occupancy if snap.occupancy is not None else np.zeros_like(power_db, np.uint8))
        self.pending["floor_db"].append((floor_db if floor_db is not None else np.full(snap.nof_prb, np.nan)).astype(np.float32))
        self.pending["sfn"].append(snap.sfn)
        self.pending["slot"].append(snap.slot)
        self.pending["time_ns"].append(snap.observation_time_ns)
        if len(self.pending["sfn"]) >= 256:
            self.flush()

    def flush(self) -> None:
        if not self.pending["sfn"]:
            return
        for key, values in self.pending.items():
            data = np.stack(values) if values[0].__class__ is np.ndarray else np.array(values)
            if key not in self.file:
                self.file.create_dataset(key, data=data, maxshape=(None,) + data.shape[1:], chunks=True,
                                         compression="gzip")
            else:
                dataset = self.file[key]
                dataset.resize(dataset.shape[0] + data.shape[0], axis=0)
                dataset[-data.shape[0]:] = data
            values.clear()
        self.file.flush()

    def close(self) -> None:
        self.flush()
        self.file.create_dataset("events_json", data=np.array([json.dumps(e) for e in self.events], dtype=object),
                                 dtype=self.h5py.string_dtype())
        self.file.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    parser.add_argument("--threshold-db", type=float, default=10.0, help="excess over the noise floor that counts as a hit")
    parser.add_argument("--min-slots", type=int, default=2, help="consecutive snapshots before an emitter is reported")
    parser.add_argument("--floor-alpha", type=float, default=0.02, help="noise-floor EMA weight per snapshot")
    parser.add_argument("--interval-ms", type=int, default=0, help="requested capture interval; 0 = every publication")
    parser.add_argument("--sample-every-n", type=int, default=1,
                        help="keep every N-th publication of the subscription (agent-side thinning)")
    parser.add_argument("--center-hz", type=float, default=None,
                        help="carrier centre frequency when the producer does not report one")
    parser.add_argument("--duration", type=float, default=0.0, help="stop after this many seconds (0 = run until Ctrl-C)")
    parser.add_argument("--record", default=None, help="HDF5 file for per-slot PRB power, occupancy, floor and events")
    parser.add_argument("--exclude-prb", default="center",
                        help="comma-separated PRBs to ignore, 'center' for the DC PRB (default), or 'none'")
    parser.add_argument("--wideband-fraction", type=float, default=0.5,
                        help="fraction of PRBs hit at once that is reported as a wideband event, not emitters")
    parser.add_argument("--quiet-wideband", action="store_true", help="do not print wideband events")
    parser.add_argument("--json", action="store_true", help="emit events as JSON lines instead of text")
    parser.add_argument("--stats-every", type=float, default=5.0, help="print feed statistics every N seconds (0 = off)")
    args = parser.parse_args()

    detector = Detector(args.threshold_db, args.min_slots, args.floor_alpha, wideband_fraction=args.wideband_fraction)
    exclude_center = args.exclude_prb == "center"
    if args.exclude_prb not in ("center", "none"):
        detector.exclude_prbs = tuple(int(p) for p in args.exclude_prb.split(","))
    recorder = Recorder(args.record) if args.record else None
    counts = {"objects": 0, "bytes": 0, "decode_s": 0.0}
    started = time.monotonic()
    last_stats = started
    deadline = started + args.duration if args.duration > 0 else None

    def emit(event: dict) -> None:
        if recorder:
            recorder.events.append(event)
        if args.json:
            print(json.dumps(event), flush=True)
        elif event["event"] == WIDEBAND:
            print(f"[sfn {event['sfn']:4d}.{event['slot']:2d}] wideband {event['prb_hit']}/{event['nof_prb']} PRBs "
                  f"+{event['peak_excess_db']:.1f} dB (saturation / PRACH / leakage, not an emitter)", flush=True)
        elif event["event"] == "emitter":
            print(f"[sfn {event['sfn']:4d}.{event['slot']:2d}] EMITTER  PRB {event['prb_first']:3d}-{event['prb_last']:3d} "
                  f"{event['center_hz'] / 1e6:10.3f} MHz  bw {event['bandwidth_hz'] / 1e6:6.2f} MHz  "
                  f"+{event['peak_excess_db']:.1f} dB over floor", flush=True)
        else:
            print(f"[sfn {event['sfn']:4d}.{event['slot']:2d}] ended    PRB {event['prb_first']:3d}-{event['prb_last']:3d} "
                  f"after {event['duration_ms']:.0f} ms ({event['slots_seen']} slots)", flush=True)

    try:
        with connect(args.host, args.port, "sdk-emitter-detector") as client:
            client.setup()
            client.subscribe(["classc.spectrum"],
                             configs=[ocudu_e3.SubscriptionConfig("classc.spectrum", minimum_interval_ms=args.interval_ms,
                                                                        sample_every_n=max(1, args.sample_every_n),
                                                                  drop_policy="dropOldest")])
            for indication in client.indications(timeout=1.0, stop=lambda: deadline is not None and time.monotonic() >= deadline):
                record = indication.record
                t0 = time.perf_counter()
                snap = decode_spectrum(record.metadata, record.tensor)
                if snap.reference_frequency_hz == 0 and args.center_hz:
                    snap.reference_frequency_hz = int(args.center_hz)
                if exclude_center and not detector.exclude_prbs:
                    detector.exclude_prbs = (snap.nof_prb // 2,)
                counts["decode_s"] += time.perf_counter() - t0
                counts["objects"] += 1
                counts["bytes"] += len(record.tensor)
                for event in detector.detect(snap):
                    if event["event"] == WIDEBAND and args.quiet_wideband:
                        continue
                    emit(event)
                if recorder:
                    recorder.add(snap, 10 * np.log10(snap.prb_power() + 1e-20), detector.floor_db)
                now = time.monotonic()
                if args.stats_every and now - last_stats >= args.stats_every:
                    last_stats = now
                    elapsed = now - started
                    floor = detector.floor_db
                    print(f"# {counts['objects'] / elapsed:6.1f} grids/s {counts['bytes'] / elapsed / 1e6:5.2f} MB/s "
                          f"decode {1e6 * counts['decode_s'] / max(counts['objects'], 1):5.0f} us "
                          f"floor {np.nanmedian(floor) if floor is not None else float('nan'):6.1f} dB "
                          f"hits {detector.stats['hits']} emitters {detector.stats['emitters']} "
                          f"wideband {detector.stats.get('wideband', 0)} "
                          f"dropped {indication.dropped_objects}", file=sys.stderr, flush=True)
            client.release()
    except KeyboardInterrupt:
        pass
    finally:
        if recorder:
            recorder.close()
    elapsed = time.monotonic() - started
    print(f"# done: {counts['objects']} grids in {elapsed:.1f} s, {detector.stats['emitters']} emitters reported",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
