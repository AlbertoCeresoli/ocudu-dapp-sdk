#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Minimal ISAC processing of the SRS channel response, out of process over E3AP.

CAVEAT — STARTING POINT, NOT A SENSING CAPABILITY
-------------------------------------------------
This script exists to show how an external process obtains the SRS channel
response over E3AP at full rate and what the numbers mean. The processing is a
textbook range-Doppler sketch with no calibration, no tracking, no
association across links, no validation against ground truth and known
weaknesses: the direct-path phase reference and clutter mean are heuristics;
the map floor is a median; delay bins near the IFFT edge alias (a persistent
"reflector" at ~4 us on the reference cell is such an artefact); the Doppler
sign convention is not pinned down; multi-UE Class-C snapshots are keyed by
SRS config id, not by UE. On a quiet bench it still reports occasional
near-threshold cells. Treat every report as "something changed in this cell
of the map", tune or replace the maths, and validate against a real target
before drawing conclusions.

    python3 srs_isac_monitor.py                          # srs.channel stream, all UEs
    python3 srs_isac_monitor.py --rnti 0x4604 --cpi 64 --threshold-db 12 --record isac.h5
    python3 srs_isac_monitor.py --source classc.srs_isac --json

This is the Class-C use case (SRS-ISAC) done as an ordinary process: one SCTP
association to the gNB's E3AP endpoint, an E3 setup, a subscription to the
per-subcarrier SRS channel response (``srs.channel``: one record per UE and
receive/transmit port pair every SRS occasion, TA phase ramp already removed;
or ``classc.srs_isac``: the same estimate as the Class-C snapshot), and a small
range-Doppler pipeline per (UE, Rx port, Tx port):

1. **Normalise** each response: divide by its mean magnitude (AGC/RSRP
   changes must not look like Doppler) and remove the direct path's phase
   (the strongest delay bin is the phase reference), so TA dither, residual
   CFO and oscillator phase noise common to all paths cancel instead of
   smearing the direct path across Doppler.
2. **Static clutter removal**: subtract a per-link coherent running mean of
   the response (``--clutter-alpha``); what remains is the time-varying part
   of the channel (moving reflectors and residual noise).
3. **Range**: inverse FFT across the comb tones (tone spacing
   ``comb x SCS``; with comb 4 at 30 kHz the delay resolution is 1 / (120 x
   120 kHz) = 69 ns, i.e. ~10 m of bistatic range per bin, unambiguous to
   8.3 us).
4. **Doppler**: FFT over the last ``--cpi`` responses of the same link (100 Hz
   SRS -> 64 responses = 0.64 s, 1.6 Hz = 0.07 m/s resolution at 3.4 GHz).
5. **Detection**: a cell must exceed both the map's median and its own delay
   column's Doppler median by ``--threshold-db`` (so a jittery strong path
   cannot light up its whole column), lie outside the zero-Doppler guard and
   beyond the direct-path bins (``--min-delay-bins``), and persist for
   ``--persist`` consecutive maps before it is reported as a moving reflector
   with bistatic range delta, radial speed and SNR. A reported cell is not
   repeated until it disappears for ``--persist`` maps.

Every step is a few lines of numpy so it can be replaced. The response is
only known where the UE actually sounded, so the Doppler axis assumes the
configured SRS period (``--srs-period-ms``, checked against the slot stamps;
gaps reset the coherent window).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from collections import defaultdict, deque
from dataclasses import dataclass, field
from typing import Deque, Dict, List, Optional, Tuple

import numpy as np

from _common import connect, ocudu_e3
from ocudu_e3.classc import decode_srs_isac

C = 299_792_458.0


@dataclass
class Link:
    """Coherent processing state for one (rnti, rx, tx) link."""

    tone_spacing_hz: float
    clutter: Optional[np.ndarray] = None
    window: Deque[np.ndarray] = field(default_factory=deque)
    last_slot_index: Optional[int] = None
    responses: int = 0
    hits: Dict[Tuple[int, int], int] = field(default_factory=dict)   # (doppler bin, delay bin) -> consecutive maps
    reported: Dict[Tuple[int, int], int] = field(default_factory=dict)  # cell -> maps since last seen


@dataclass
class Report:
    rnti: int
    rx: int
    tx: int
    sfn: int
    slot: int
    range_m: float
    speed_mps: float
    snr_db: float
    doppler_hz: float
    delay_ns: float


class IsacProcessor:
    def __init__(self, cpi: int, clutter_alpha: float, threshold_db: float, guard_bins: int,
                 srs_period_ms: float, carrier_hz: float, max_reports: int = 3, min_delay_bins: int = 2,
                 persist: int = 3) -> None:
        self.min_delay_bins = min_delay_bins
        self.persist = persist
        self.cpi = cpi
        self.clutter_alpha = clutter_alpha
        self.threshold_db = threshold_db
        self.guard_bins = guard_bins
        self.srs_period_s = srs_period_ms / 1e3
        self.carrier_hz = carrier_hz
        self.max_reports = max_reports
        self.links: Dict[Tuple[int, int, int], Link] = {}
        self.stats = {"responses": 0, "maps": 0, "reports": 0, "gaps": 0}
        self.last_map: Optional[np.ndarray] = None

    def push(self, rnti: int, rx: int, tx: int, h: np.ndarray, tone_spacing_hz: float,
             slot_index: int, sfn: int, slot: int, carrier_hz: Optional[float],
             numerology: int = 1) -> List[Report]:
        key = (rnti, rx, tx)
        link = self.links.get(key)
        if link is None:
            link = self.links[key] = Link(tone_spacing_hz)
        self.stats["responses"] += 1
        link.responses += 1
        h = np.asarray(h, dtype=np.complex64)
        scale = np.abs(h).mean()
        if not np.isfinite(scale) or scale <= 0:
            return []
        h = h / scale
        # Direct-path phase reference: common phase noise / TA dither cancels.
        profile = np.fft.ifft(h)
        reference = profile[np.argmax(np.abs(profile))]
        if abs(reference) > 0:
            h = h * (np.conj(reference) / abs(reference))

        # Contiguity: a missed SRS occasion breaks the Doppler phase progression.
        # Slot duration follows the record's numerology (1 ms / 2^mu), so the
        # expected stride between SRS occasions is exact for any SCS.
        slots_per_period = max(1, round(self.srs_period_s * (1000.0 * (1 << numerology))))
        if link.last_slot_index is not None and slot_index - link.last_slot_index != slots_per_period:
            if slot_index - link.last_slot_index > slots_per_period:
                self.stats["gaps"] += 1
                link.window.clear()
        link.last_slot_index = slot_index

        # Static clutter: coherent running mean of the (normalised) response.
        if link.clutter is None:
            link.clutter = h.copy()
            return []
        dynamic = h - link.clutter
        link.clutter = (1 - self.clutter_alpha) * link.clutter + self.clutter_alpha * h

        # Range profile of the dynamic component (Hann window tames tone-truncation sidelobes).
        n = h.size
        cir = np.fft.ifft(dynamic * np.hanning(n))
        link.window.append(cir)
        while len(link.window) > self.cpi:
            link.window.popleft()
        if len(link.window) < self.cpi:
            return []

        # Range-Doppler map: FFT over the coherent window per delay bin.
        stack = np.stack(link.window)  # (cpi, n)
        rd = np.fft.fftshift(np.fft.fft(stack * np.hanning(self.cpi)[:, None], axis=0), axes=0)
        power_db = 20 * np.log10(np.abs(rd) + 1e-12)
        self.last_map = power_db
        self.stats["maps"] += 1

        zero = self.cpi // 2
        global_excess = power_db - np.median(power_db)
        column_excess = power_db - np.median(power_db, axis=0, keepdims=True)
        excess = np.minimum(global_excess, column_excess)
        excess[zero - self.guard_bins : zero + self.guard_bins + 1, :] = -np.inf  # static / residual clutter
        excess[:, : self.min_delay_bins] = -np.inf  # direct path and its leakage
        excess[:, n // 2 :] = -np.inf  # negative delays are wraparound of the IFFT
        candidates = np.argwhere(excess > self.threshold_db)
        # Persistence: count consecutive maps per cell; report once when it reaches the threshold.
        current = {tuple(int(v) for v in cell) for cell in candidates}
        for cell in list(link.hits):
            if cell not in current:
                del link.hits[cell]
        for cell in list(link.reported):
            link.reported[cell] = 0 if cell in current else link.reported[cell] + 1
            if link.reported[cell] > self.persist:
                del link.reported[cell]
        fresh = []
        for cell in current:
            link.hits[cell] = link.hits.get(cell, 0) + 1
            if link.hits[cell] >= self.persist and cell not in link.reported:
                link.reported[cell] = 0
                fresh.append(cell)
        if not fresh:
            return []
        candidates = np.array(fresh)
        order = np.argsort(excess[candidates[:, 0], candidates[:, 1]])[::-1]
        reports = []
        delay_res_s = 1.0 / (n * link.tone_spacing_hz)
        doppler_res_hz = 1.0 / (self.cpi * self.srs_period_s)
        fc = carrier_hz or self.carrier_hz
        for index in order[: self.max_reports]:
            d_bin, r_bin = candidates[index]
            doppler_hz = (d_bin - zero) * doppler_res_hz
            delay_s = r_bin * delay_res_s
            speed = doppler_hz * C / (2 * fc) if fc else float("nan")
            reports.append(Report(rnti, rx, tx, sfn, slot, delay_s * C, speed, float(excess[d_bin, r_bin]),
                                  doppler_hz, delay_s * 1e9))
        self.stats["reports"] += len(reports)
        return reports


def slot_index_of(sfn: int, slot: int, numerology: int) -> int:
    return sfn * (10 << numerology) + slot


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    parser.add_argument("--source", choices=("srs.channel", "classc.srs_isac"), default="srs.channel",
                        help="telemetry stream carrying the SRS response")
    parser.add_argument("--rnti", action="append", default=[], help="restrict to this UE (repeatable, srs.channel only)")
    parser.add_argument("--interval-ms", type=int, default=0, help="requested capture interval; 0 = every SRS occasion")
    parser.add_argument("--cpi", type=int, default=64, help="coherent processing interval in SRS occasions")
    parser.add_argument("--clutter-alpha", type=float, default=0.02, help="running-mean weight for static clutter")
    parser.add_argument("--threshold-db", type=float, default=15.0, help="excess over the range-Doppler median")
    parser.add_argument("--guard-bins", type=int, default=2, help="Doppler bins around zero to ignore")
    parser.add_argument("--min-delay-bins", type=int, default=2, help="delay bins from the direct path to ignore")
    parser.add_argument("--persist", type=int, default=3, help="consecutive maps a cell must exceed before reporting")
    parser.add_argument("--srs-period-ms", type=float, default=10.0, help="SRS periodicity of the sounding UEs")
    parser.add_argument("--carrier-hz", type=float, default=0.0, help="carrier for Doppler->speed when the feed lacks it")
    parser.add_argument("--duration", type=float, default=0.0, help="stop after N seconds (0 = until Ctrl-C)")
    parser.add_argument("--record", default=None, help="HDF5 file with every normalised response and the reports")
    parser.add_argument("--json", action="store_true", help="JSON lines instead of text")
    parser.add_argument("--stats-every", type=float, default=5.0, help="seconds between statistics lines on stderr")
    args = parser.parse_args()

    print("# srs_isac_monitor: starting-point example, not a validated sensing capability — see the module docstring",
          file=sys.stderr)
    processor = IsacProcessor(args.cpi, args.clutter_alpha, args.threshold_db, args.guard_bins,
                              args.srs_period_ms, args.carrier_hz, min_delay_bins=args.min_delay_bins,
                              persist=args.persist)
    recorder = None
    if args.record:
        import h5py

        recorder = h5py.File(args.record, "w")
        rec = {"rnti": [], "rx": [], "tx": [], "sfn": [], "slot": [], "h": [], "reports": []}
    started = time.monotonic()
    last_stats = started
    deadline = started + args.duration if args.duration > 0 else None
    rntis = [int(r, 0) for r in args.rnti]
    carrier_seen: Optional[float] = None

    def emit(report: Report) -> None:
        if recorder is not None:
            rec["reports"].append(json.dumps(report.__dict__))
        if args.json:
            print(json.dumps(report.__dict__), flush=True)
        else:
            print(f"[sfn {report.sfn:4d}.{report.slot:2d}] rnti 0x{report.rnti:04x} rx{report.rx}/tx{report.tx}  "
                  f"moving reflector: +{report.range_m:6.1f} m bistatic ({report.delay_ns:5.0f} ns)  "
                  f"{report.speed_mps:+6.2f} m/s ({report.doppler_hz:+6.1f} Hz)  {report.snr_db:4.1f} dB", flush=True)

    configs = []
    if args.source == "srs.channel":
        configs.append(ocudu_e3.SubscriptionConfig("srs.channel", rnti_filter=rntis, minimum_interval_ms=args.interval_ms,
                                                   drop_policy="dropOldest"))
    else:
        configs.append(ocudu_e3.SubscriptionConfig("classc.srs_isac", minimum_interval_ms=args.interval_ms,
                                                   drop_policy="dropOldest"))
    try:
        with connect(args.host, args.port, "sdk-srs-isac") as client:
            client.setup()
            if not args.carrier_hz and args.source == "srs.channel":
                # srs.channel records carry no carrier frequency; ask the agent's
                # management function for the serving cell instead of printing NaN.
                try:
                    cells = client.cell_info().get("cells") or []
                    ul_hz = float(cells[0].get("ulCenterHz") or cells[0].get("dlCenterHz") or 0.0) if cells else 0.0
                    if ul_hz > 0.0:
                        processor.carrier_hz = ul_hz
                        print(f"# carrier {ul_hz / 1e6:.3f} MHz from E3 cellInfo (override with --carrier-hz)",
                              file=sys.stderr)
                except Exception as exc:  # cellInfo is optional on older agents
                    print(f"# carrier unknown ({exc}); speeds will be NaN — pass --carrier-hz", file=sys.stderr)
            client.subscribe([args.source], configs=configs)
            for ind in client.indications(timeout=1.0, stop=lambda: deadline is not None and time.monotonic() >= deadline):
                r = ind.record
                if args.source == "srs.channel":
                    h = np.asarray(list(r.samples()), dtype=np.complex64)
                    reports = processor.push(r.rnti, r.rx_port_index, r.tx_port_index, h,
                                             r.comb_size * (15_000 << r.radio_slot.numerology),
                                             slot_index_of(r.radio_slot.sfn, r.radio_slot.slot, r.radio_slot.numerology),
                                             r.radio_slot.sfn, r.radio_slot.slot, carrier_seen,
                                             r.radio_slot.numerology)
                    if recorder is not None:
                        rec["rnti"].append(r.rnti); rec["rx"].append(r.rx_port_index); rec["tx"].append(r.tx_port_index)
                        rec["sfn"].append(r.radio_slot.sfn); rec["slot"].append(r.radio_slot.slot); rec["h"].append(h)
                else:
                    snap = decode_srs_isac(r.metadata, r.tensor)
                    carrier_seen = float(snap.reference_frequency_hz) or carrier_seen
                    reports = []
                    link_id = snap.rnti or (0x10000 + snap.srs_config_id)  # Class-C snapshots may not carry the RNTI
                    for rx in range(snap.rx_ports):
                        for tx in range(snap.tx_ports):
                            reports += processor.push(link_id, rx, tx, snap.h[rx, tx], snap.tone_spacing_hz,
                                                      slot_index_of(snap.sfn, snap.slot, snap.numerology),
                                                      snap.sfn, snap.slot, carrier_seen, snap.numerology)
                            if recorder is not None:
                                rec["rnti"].append(snap.rnti); rec["rx"].append(rx); rec["tx"].append(tx)
                                rec["sfn"].append(snap.sfn); rec["slot"].append(snap.slot); rec["h"].append(snap.h[rx, tx])
                for report in reports:
                    emit(report)
                now = time.monotonic()
                if args.stats_every and now - last_stats >= args.stats_every:
                    last_stats = now
                    s = processor.stats
                    print(f"# {s['responses'] / (now - started):6.1f} responses/s  links {len(processor.links)}  "
                          f"maps {s['maps']}  reports {s['reports']}  gaps {s['gaps']}  dropped {ind.dropped_objects}",
                          file=sys.stderr, flush=True)
            client.release()
    except KeyboardInterrupt:
        pass
    finally:
        if recorder is not None:
            for key in ("rnti", "rx", "tx", "sfn", "slot"):
                recorder.create_dataset(key, data=np.array(rec[key]))
            if rec["h"]:
                lengths = {len(h) for h in rec["h"]}
                if len(lengths) == 1:
                    recorder.create_dataset("h", data=np.stack(rec["h"]), compression="gzip")
                else:
                    recorder.create_dataset("h", data=np.concatenate(rec["h"]))
                    recorder.create_dataset("h_len", data=np.array([len(h) for h in rec["h"]]))
            recorder.create_dataset("reports_json", data=np.array(rec["reports"], dtype=object),
                                    dtype=__import__("h5py").string_dtype())
            recorder.close()
    print(f"# done: {processor.stats}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
