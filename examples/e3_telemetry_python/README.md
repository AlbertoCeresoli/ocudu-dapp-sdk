<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# Python E3AP observer examples

Six small scripts that connect to a running OCUDU gNB **as out-of-process
dApps** over the E3AP SCTP endpoint (`dapp.e3ap_sctp`, default port 36423),
subscribe to telemetry, and use the management service model — no host build,
no root socket, no ZMQ plugin. (`_common.py` is the shared helper that locates
`ocudu_e3`, not a script.)

| Script | What it demonstrates |
|--------|----------------------|
| `e3_catalog.py` | E3 Setup: RAN functions, telemetry streams, payload bounds |
| `watch_pusch_quality.py` | Two streams combined (`pusch.symbols` + `pusch.crc`) into per-UE SINR/BLER with an RNTI filter applied by the gNB; optional ASCII constellation from the equalized I/Q |
| `srs_channel_watch.py` | SRS channel coefficients per port pair plus the gNB JSON metrics UE table |
| `dapp_health_over_e3ap.py` | Inventory, health, and per-instance metrics as E3AP control actions (E3SM-OCUDU-DAPP-MGMT-V1) |
| `record_telemetry.py` | Full-rate capture of any streams into HDF5 (or a NumPy `.npz` archive) — see "Recording at full rate" |
| `export_pusch_burst.py` | Turns a recorded `pusch.symbols` burst into the JSON + cf32 pair `ocudu-pusch-replay` decodes — see "Decoding recorded PUSCH bursts" |

Running these against a live gNB, plus troubleshooting shared by all the
Python E3 examples: [`docs/run_examples.md` §6](../../docs/run_examples.md#6-python-e3-examples-out-of-process-no-catalog-package).

## Requirements

- Python ≥ 3.10 and `asn1tools` (`pip install asn1tools`) for every script.
- `numpy` is required by `record_telemetry.py` (imported unconditionally, both
  output formats) and `export_pusch_burst.py`; `h5py` is required by
  `export_pusch_burst.py` and by `record_telemetry.py` in its default `--format
  h5` mode (the `--format npz` path imports only `numpy`). The four watch /
  catalog / health scripts need neither.
- The `ocudu_e3` client: either a sibling `ocudu/` checkout (auto-detected), an
  installed `dapp_sdk` component (`<prefix>/share/ocudu/dapp/python`), or
  `OCUDU_E3_PYTHON_DIR` pointing at one of them. Schemas are found the same way
  or through `OCUDU_E3_SCHEMA_DIR`.
- A gNB configured with `dapp.e3ap_sctp` (see the OCUDU
  `docs/dapp/yaml_configuration.md`), or the loopback test agent
  `e3ap_test_server` from the OCUDU build for a radio-free dry run.

```bash
export OCUDU_E3_PYTHON_DIR=/path/to/ocudu/python
export OCUDU_E3_SCHEMA_DIR=/path/to/ocudu/schemas/e3
python3 e3_catalog.py
python3 watch_pusch_quality.py --rnti 0x4601 --duration 20 --iq-preview
python3 srs_channel_watch.py --duration 30
python3 dapp_health_over_e3ap.py --watch 2
```

## What the gNB guarantees you

- **Bounded and lossy by design.** Every stream is a bounded queue; a slow
  observer sees the freshest object and `indication.dropped_objects` tells it
  how many it missed. The radio path never waits for you.
- **Filters run on the gNB.** RNTI lists, cell filters, sampling and per-UE
  floors are part of the subscription, so the association carries only what
  you asked for.
- **Everything is APER ASN.1 over SCTP.** The schemas are the contract
  (`schemas/e3/*.asn1` in the OCUDU tree); the client is a thin wrapper you
  can replace.
- **Observation only.** These streams cannot influence scheduling or the PHY.
  Management control actions carry the same authority as the local management
  socket, so treat the E3AP port as a control-plane port.

## Tests

`ctest -R example.e3_telemetry_python` runs the scripts' `--help` paths and,
when `OCUDU_E3AP_TEST_SERVER` points at the OCUDU loopback agent, a live
catalog/subscription round trip.

## Recording at full rate

`record_telemetry.py` subscribes with `minimum_interval_ms=0` (every event)
and streams records into an HDF5 file (`h5py`), one group per stream:

    python3 record_telemetry.py srs.channel --duration 10 --output srs.h5
    python3 record_telemetry.py pusch.symbols pusch.crc --rnti 0x4601 --output ue.h5

HDF5 fits the data: PUSCH grants differ in PRBs, symbols, and layers, so each
record has its own sample count and slot metadata. Scalar fields are resizable
columns (`/pusch.symbols/rnti`, `radio_slot_sfn`, `sinr_db`, ...), samples are
one contiguous complex64 dataset with per-record `sample_offset`/`sample_count`,
and everything is appended in batches so captures are bounded by disk, not
memory. Group attributes record the telemetry id, interface id, description
and the advertised default and hard floors; file attributes record the RAN
identifier, E3AP version and capture parameters. `--format npz` keeps the
capture in memory and writes a NumPy archive instead for short captures
(arrays named `<stream>__<field>`, e.g. `pusch_symbols__rnti`, with the same
`sample_offset` / `sample_count` / `samples` columns). The npz writer stores
**only the arrays**: it does not carry the group or file attributes (telemetry
ids, floors, RAN identifier), so use HDF5 when that provenance matters.

The requested interval travels in the subscription; the agent runs the
producer at the fastest active request down to the hard floor it advertises
(`e3_catalog.py` prints both floors) and thins slower subscriptions
individually, so a dashboard sharing the stream keeps its 20-50 ms cadence.

Reading a capture:

```python
import h5py, numpy as np
with h5py.File("ue.h5") as f:
    g = f["pusch.symbols"]
    for i in range(g["rnti"].shape[0]):
        n, o = g["sample_count"][i], g["sample_offset"][i]
        iq = g["samples"][o:o + n]            # one grant's equalized symbols
        print(hex(g["rnti"][i]), g["radio_slot_sfn"][i], g["nof_prb"][i], g["modulation_bits"][i], iq.shape)
```

## Decoding recorded PUSCH bursts

Each `pusch.symbols` record now carries the grant parameters (`grant_*`
columns: TBS, RV, NDI, LDPC base graph, n_ID, MCS index/table, DM-RS
configuration, PRB mask, UCI sizes and beta offsets) and the gNB's `crc_ok`.
`export_pusch_burst.py` turns one burst (or all of them) into a JSON + cf32
pair, and `ocudu-pusch-replay` (built with ocudu, `apps/tools/pusch_replay`)
runs the gNB's own demapper, descrambler, UL-SCH demultiplexer and LDPC
decoder on it:

    python3 record_telemetry.py pusch.symbols --duration 30 --output ue.h5
    python3 export_pusch_burst.py ue.h5 --all --output bursts/
    for b in bursts/*.json; do ocudu-pusch-replay "$b"; done

Each line reports `replay CRC: OK|FAIL | gNB CRC: OK|FAIL` (and `MISMATCH`
when they differ). `pusch.grid` records the raw pre-equalization resource
elements (`/pusch.grid/samples`, port × symbol × subcarrier via
`PuschGridRecord.grid()`) with the same grant block for re-running channel
estimation and equalization offline.
