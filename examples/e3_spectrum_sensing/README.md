<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Spectrum sensing over E3AP (out-of-process)

`emitter_detector.py` is a sensing application that is **not a dApp**: no
manifest, no lifecycle manager, no Class-C worker. It is a plain process that
opens one SCTP association to the gNB's E3AP endpoint, completes the E3 setup,
subscribes to the `classc.spectrum` telemetry and consumes the uplink resource
grid as it is published — the same data the in-process Spectrum dApp sees.

```
python3 emitter_detector.py                         # live detections on 127.0.0.1:36423
python3 emitter_detector.py --threshold-db 8 --min-slots 3 --record sensing.h5
python3 emitter_detector.py --json | tee events.jsonl
```

## What arrives and how fast

Each `classc.spectrum` indication is one Class-C publication: a FlatBuffers
`PublicationEnvelope` (`OCC3`, ~1.2 KB) with the radio time, cell, carrier
reference frequency, bandwidth, tensor descriptor and the scheduler's
**expected uplink use per symbol × PRB**, followed by the **complete uplink
resource grid** (`[rx_port][symbol][subcarrier]`, complex bfloat16).

Nothing about the grid is re-encoded on the wire. E3AP carries it as an APER
`OCTET STRING`, i.e. a length prefix and the raw bytes — the agent memcpys it
and only the small header is PER-coded. On the reference cell (51 PRB, 1 Rx
port) a Python subscriber receives **~120 grids/s, ~4 MB/s**, and
`ocudu_e3.classc.decode_spectrum` costs ~120 µs per grid (FlatBuffers header
read plus one `numpy.frombuffer`). Larger cells scale linearly (273 PRB × 4
ports × 14 symbols × 4 B ≈ 730 KB per grid); the E3AP fragment size is 64 KiB
and the per-subscription queue is sized for the largest object. If the grids
arrive faster than a process can decode, thin the subscription —
`--interval-ms` requests a minimum publication interval, `--sample-every-n`
keeps every Nth object (both travel in the subscription, so the gNB does the
thinning) — or move the consumer to C++ (`ocudu_dapp_e3ap_codec`); the wire
format is the same.

## Detection logic (replace it with yours)

1. Mean power per (symbol, PRB), summed over receive ports.
2. A per-PRB **noise floor** tracked as an EMA of the power seen where the
   scheduler marks the PRB *planned empty*; it follows gain drift but is never
   updated from cells above threshold.
3. Unscheduled cells above `floor + --threshold-db` are hits; the centre PRB is
   excluded by default (`--exclude-prb center`) because it carries the
   receiver's DC/LO spur (≈ +11 dB on the reference cell).
4. If more than `--wideband-fraction` of the PRBs light up at once the slot is
   reported once as a **wideband** event (receiver saturation, PRACH from a
   very close UE, duplexer leakage) and never as emitters.
5. Contiguous PRB runs are tracked across slots; a run alive for `--min-slots`
   is reported as an **emitter** (centre frequency, bandwidth, excess power)
   and again when it ends.

Scheduled cells (PUSCH/PUCCH/SRS/PRACH) never count, so the cell's own UEs
are not emitters. `--record` writes per-slot PRB power, occupancy, the floor
and the event list to HDF5 for offline tuning.

## Files

| File | Purpose |
|------|---------|
| `emitter_detector.py` | the application |
| `_common.py` | locates `ocudu_e3` (built tree, install, or `OCUDU_E3_PYTHON_DIR`) |
| `tests/test_examples.py` | detector unit test on synthetic grids + `--help` |

Requires `asn1tools`, `flatbuffers`, `numpy` (and `h5py` for `--record`).
