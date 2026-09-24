<!-- SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc. -->
<!-- SPDX-License-Identifier: BSD-3-Clause-Clear -->

# SRS channel-estimation datasets from a live cell

Three scripts that turn the gNB's SRS telemetry into training and evaluation data
for SRS channel estimators, the SRS counterpart of the PUSCH capture and replay in
[`e3_telemetry_python`](../e3_telemetry_python/README.md):

| Script | What it does |
|--------|--------------|
| `capture_srs_dataset.py` | Subscribes to `srs.channel` and the gNB metrics report over E3AP, assembles the port pairs of every SRS occasion and writes them into an HDF5 dataset |
| `srs_dataset.py` | The dataset library: occasion assembly, writer, reader, and the dApp metadata of an occasion |
| `export_srs_vectors.py` | Writes captured occasions as SRS channel-estimator vectors, which `ocudu-dapp-srs-replay` runs through the reference estimator |

```text
 gNB ── srs.channel (per port pair: LS estimate, pilots, reference, RSRP/EPRE/noise/TA) ──▶ capture_srs_dataset.py ──▶ srs.h5
     ── gnb.metrics_json (per UE: PUSCH SINR, UL MCS, CQI) ─────────────────────────────▶         │
                                                                                                    ▼
                                             ocudu-dapp-srs-replay ◀── live.bin ◀── export_srs_vectors.py
```

## What an occasion holds

The gNB publishes one `srs.channel` record per receive/transmit port pair of an SRS
occasion. From OCUDU hosts with the SRS channel-estimator dApp interface
(`OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1`) each record carries, besides the
least-squares estimate and the gNB's measurements, the received pilots and the
transmitted sequence the estimate came from. An assembled occasion therefore holds
exactly what an SRS channel-estimator dApp receives, plus the gNB's answer:

| Dataset column | Shape | Content |
|----------------|-------|---------|
| `rx_symbols` | CF32 `[N, pilot_set, rx, symbol, pilot]` | The dApp's `rx_symbols` input: received pilots at each pilot set's comb subcarriers |
| `reference_symbols` | CF32 `[N, tx, pilot]` | The dApp's `reference_symbols` input: each port's sequence, cyclic shift included |
| `lse` | CF32 `[N, rx, tx, pilot]` | Least-squares estimate per comb subcarrier, averaged over symbols, time-alignment ramp removed |
| `wideband` | CF32 `[N, rx, tx]` | The gNB's wideband coefficient per port pair (mean of `lse`) |
| `channel_matrix` | CF32 `[N, rx, tx]` | The gNB's SRS channel matrix: `wideband` scaled by the inverse noise standard deviation (at most 40 dB SNR) |
| `rsrp_db`, `epre_db`, `noise_variance`, `time_alignment_ns` | F32 `[N]` | The gNB's measurements |
| `pusch_snr_db`, `ul_mcs`, `cqi` | F32 `[N]` | The UE's link state from the latest metrics report (NaN when unknown) |
| `rnti`, `sfn`, `slot`, `numerology`, `monotonic_ns` | `[N]` | Who and when |
| `first_subcarrier`, `cyclic_shift` | `[N, tx]` | Each transmit port's first comb subcarrier and cyclic shift n_cs |
| `sequence_group`, `sequence_number`, `sequence_id`, `cyclic_shift_maximum`, `comb_offset`, `bandwidth_index`, `frequency_position`, `frequency_shift`, `frequency_hopping`, `start_symbol` | `[N]` | The SRS configuration |
| `has_pilots`, `nof_pilot_sets` | `[N]` | Whether the occasion carries the dApp input (older hosts publish only the estimate), and 1 or 2 pilot sets |

Occasions are grouped by shape (`/occasions/rx2_tx1_sym1_comb4_len156`, ...), so each
column of a group is one fixed-shape tensor ready for a training loop; the group
attributes give the shape. Four ports whose cyclic shift is in the upper half of the
range interleave their pilots over two comb offsets (TS 38.211 6.4.1.4.3): ports 0
and 2 use pilot set 0, ports 1 and 3 pilot set 1.

## Capturing

```bash
# Every SRS occasion of every UE for a minute
python3 capture_srs_dataset.py --duration 60 --output srs.h5

# One UE, stop after 5000 occasions
python3 capture_srs_dataset.py --rnti 0x4601 --occasions 5000 --output ue.h5
```

`--interval-ms 0` (the default) asks for every occasion; the gNB enforces the hard
floor it advertises (`e3_catalog.py` prints it) and thins other subscribers of the
stream independently, so a dashboard keeps its rate. The stream is lossy by design:
an occasion with a missing port pair is counted as incomplete, never written half.
The summary line reports occasions written, how many carry the dApp input, and the
incomplete and dropped counts.

## Reading

```python
from srs_dataset import SrsDataset

with SrsDataset("srs.h5") as dataset:
    for shape in dataset.shapes:
        columns = dataset.arrays(shape)            # every column of the group as NumPy arrays
        x = columns["rx_symbols"][columns["has_pilots"]]
        y = columns["channel_matrix"][columns["has_pilots"]]
    for occasion in dataset.occasions():          # or one occasion at a time
        print(hex(occasion.rnti), occasion.shape, occasion.rsrp_db, occasion.kpi)
```

`srs_dataset.module_metadata(occasion)` packs the occasion's
`ocudu_dapp_srs_channel_estimator_metadata_v1`, so an offline harness can call a
module with exactly the invocation the gNB would make.

## Replaying through the reference estimator

```bash
python3 export_srs_vectors.py srs.h5 --output live.bin
ocudu-dapp-srs-replay live.bin
```

`ocudu-dapp-srs-replay` (built and installed with the SDK) runs the reference SRS
channel estimator on every exported occasion and prints, per occasion and in
summary, how its channel matrix, EPRE, RSRP, noise variance and time alignment
compare with what the live cell reported. On a host-L1 gNB the two are the same
algorithm; on an accelerated gNB the cell ran its CUDA estimator, so the replay is
also a check of that implementation. The same tool with `--check` verifies the
SDK's golden vectors (`tests/data/srs_channel_estimator_vectors.bin`).

## Requirements

Python 3.10 or later, `asn1tools`, `numpy` and `h5py` (`pip install -r
requirements.txt`), and the `ocudu_e3` client from the OCUDU tree or an installed
`dapp_sdk` component (see [`e3_telemetry_python`](../e3_telemetry_python/README.md#requirements)).
The dApp input needs an OCUDU host with the SRS channel-estimator interface; older
hosts still yield estimate-only datasets.

## Tests

`ctest -R example.e3_srs_dataset` publishes the golden vectors as the gNB publishes
`srs.channel` records and checks that the capture, dataset and export path hands
back the module input byte for byte; with `OCUDU_E3AP_TEST_SERVER` pointing at the
OCUDU loopback agent it also captures from it.
