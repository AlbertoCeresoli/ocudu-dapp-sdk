<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# SRS-ISAC over E3AP (out-of-process)

> **Caveat.** This is a very simple starting point that shows how to get the SRS
> channel response out of the gNB over E3AP and what its dimensions mean. It is
> **not** a functional ISAC capability: no calibration, no tracking, no ground-truth
> validation, heuristic clutter/phase handling, IFFT edge artefacts (a persistent
> "reflector" near 4 µs on the reference cell), an unpinned Doppler sign, and
> near-threshold reports on a quiet bench. Use it to get data flowing and replace
> the maths.

`srs_isac_monitor.py` is the SRS-ISAC (Class-C) use case written as an
ordinary process — no manifest, no lifecycle manager, no Class-C worker. It
opens one SCTP association to the gNB's E3AP endpoint, performs the E3 setup,
subscribes to the SRS channel response and runs a minimal range-Doppler
pipeline on it.

```
python3 srs_isac_monitor.py                                  # srs.channel, every UE, every occasion
python3 srs_isac_monitor.py --rnti 0x4604 --cpi 64 --threshold-db 12 --record isac.h5
python3 srs_isac_monitor.py --source classc.srs_isac --json  # the Class-C snapshot instead
```

## Two ways to get the same response

| Source | Record | Rate on the reference cell | Notes |
|--------|--------|----------------------------|-------|
| `srs.channel` (telemetry 3) | `SrsChannelRecord`: RNTI, slot, port pair, RSRP/EPRE/noise/TA, SRS resource fields, 120 complex coefficients (comb 4, TA phase ramp removed) | ~100/s per port pair per UE | recomputed from the grid by the PHY decorator; carries the RNTI |
| `classc.srs_isac` (telemetry 7) | `OCC3` envelope + cf32 `[rx][tx][frequency_points]` | ~200/s (all port pairs in one object) | the Class-C snapshot the in-process SRS-ISAC dApp receives; `ocudu_e3.classc.decode_srs_isac` |

Both travel as raw bytes inside APER `OCTET STRING`s — nothing per sample is
re-encoded — so the Python subscriber keeps up with the full SRS rate.

## Processing (a few lines each — replace them)

1. Normalise each response by its mean magnitude (AGC/RSRP changes must not
   look like Doppler).
2. Subtract a per-link coherent running mean (`--clutter-alpha`): static
   multipath and the direct path go, moving reflectors and noise stay.
3. IFFT across the comb tones → delay profile. Comb 4 at 30 kHz gives a tone
   spacing of 120 kHz: 69 ns (≈10 m bistatic) per bin, unambiguous to 8.3 µs.
4. FFT over the last `--cpi` responses of the link → range-Doppler map. 64
   occasions at 10 ms = 0.64 s: 1.6 Hz (≈0.07 m/s at 3.4 GHz) per bin.
5. Cells above the map median by `--threshold-db`, outside `--guard-bins`
   around zero Doppler and in the positive-delay half, are reported as moving
   reflectors: bistatic range delta, radial speed (needs the carrier —
   `classc.srs_isac` carries it; for `srs.channel` the script asks the gNB's
   E3 `cellInfo` for the UL carrier when `--carrier-hz` is not given, and
   `--carrier-hz` overrides it), SNR.

The expected stride between SRS occasions is derived in slots from
`--srs-period-ms` and **each record's numerology** (slot duration 1 ms / 2^µ),
so it is exact for any SCS. A missed occasion is flagged (the `gaps` counter in
the status line) and resets the coherent window; `--srs-period-ms` must still
match the UE's SRS periodicity for the Doppler axis.
`--record` stores every normalised response and the reports in HDF5 for
offline work.

## Expectations on a quiet bench (observed, not tuned away)

With nothing moving, the pipeline should report nothing or the occasional
near-threshold cell; the noise floor of the map is set by estimation noise
and TA/CFO residuals. A person walking through the UE–gNB path shows up as a
track of a few tens of metres of bistatic range at ±1–2 m/s.

## Files

| File | Purpose |
|------|---------|
| `srs_isac_monitor.py` | the application |
| `_common.py` | locates `ocudu_e3` (built tree, install, or `OCUDU_E3_PYTHON_DIR`) |
| `tests/test_examples.py` | synthetic moving-reflector recovery, gap handling, `--help` |
