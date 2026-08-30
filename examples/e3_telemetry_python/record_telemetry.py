#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Record telemetry streams at full rate to an HDF5 file.

    python3 record_telemetry.py srs.channel --duration 10 --output srs.h5
    python3 record_telemetry.py pusch.symbols pusch.crc --rnti 0x4601 --output ue.h5
    python3 record_telemetry.py pusch.symbols --format npz --output bursts.npz

A subscription carries the capture interval it wants: ``--interval-ms 0`` (the
default here) asks the producer for every event, and the agent honours the
fastest active request down to the hard floor it advertises in the setup
response. Other subscribers on the same stream (a dashboard at 20-50 ms) keep
their own rate because the carrier thins per subscription.

HDF5 layout (one group per stream, all datasets resizable and appended as
records arrive, so captures are bounded by disk rather than memory):

    /<stream>/<field>          one column per scalar record field plus the
                               indication fields cell_id, sequence,
                               monotonic_ns, dropped_objects; radio_slot is
                               expanded to radio_slot_sfn/slot/numerology
    /<stream>/samples          complex64, all records' samples concatenated
    /<stream>/sample_offset    first sample index of each record
    /<stream>/sample_count     samples in each record (PUSCH bursts differ per
                               grant: PRBs x symbols x layers)
    /<stream>/json             gnb.metrics_json reports as variable-length text
    /pusch.symbols/grant_*     grant parameters (TBS, RV, NDI, base graph, n_ID,
                               MCS index/table, DM-RS config, PRB mask as hex,
                               UCI sizes/offsets) plus crc_ok (-1 unknown) and
                               symbols_truncated, so a burst can be decoded and
                               CRC-checked offline (see pusch_replay in ocudu)
    /pusch.grid/samples        raw pre-equalization REs, port-major x symbol x
                               subcarrier (DM-RS included), with the same grant

Group attributes carry the telemetry id, interface id, and the advertised
floors; file attributes carry the RAN identifier, E3AP version, and capture
parameters. ``--format npz`` keeps everything in memory and writes a NumPy
archive instead (``<stream>__<field>`` arrays) for short captures without h5py.
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import defaultdict

import numpy as np

from _common import connect, ocudu_e3

SAMPLE_STREAMS = {"pusch.symbols", "srs.channel", "pusch.dmrs_channel", "pusch.grid"}
NON_SCALAR = {"equalized_symbols", "coefficients", "estimates", "json_bytes", "resource_elements"}
FLUSH_RECORDS = 256


def flatten(indication) -> dict:
    """Scalar fields of one indication: record fields (radio_slot expanded) plus indication metadata."""
    out = {
        "cell_id": indication.cell_id,
        "sequence": indication.sequence,
        "monotonic_ns": indication.monotonic_ns,
        "dropped_objects": indication.dropped_objects,
    }
    for name, value in vars(indication.record).items():
        if name in NON_SCALAR:
            continue
        if hasattr(value, "__dict__"):
            # radio_slot -> radio_slot_sfn/...; grant -> grant_tbs_bytes/... (prb_mask as hex text)
            for inner, inner_value in vars(value).items():
                out[f"{name}_{inner}"] = inner_value.hex() if isinstance(inner_value, bytes) else inner_value
        elif isinstance(value, bytes):
            out[name] = value.hex()
        else:
            out[name] = value
    return out


def column_dtype(values) -> np.dtype:
    kinds = {type(v) for v in values if v is not None}
    has_none = any(v is None for v in values)
    if kinds <= {str}:
        return np.dtype("O")  # variable-length text (hex masks, JSON)
    if kinds <= {bool}:
        return np.dtype(np.int8) if has_none else np.dtype(bool)  # optional bool: -1 = unknown
    if kinds <= {int, bool}:
        return np.dtype(np.int64) if not has_none else np.dtype(np.float64)
    return np.dtype(np.float64)  # optional floats: None -> NaN


def as_array(values, dtype) -> np.ndarray:
    if dtype.kind == "O":
        return np.array(["" if v is None else str(v) for v in values], dtype=object)
    if dtype.kind == "f":
        return np.array([np.nan if v is None else v for v in values], dtype=dtype)
    if dtype == np.dtype(np.int8):
        return np.array([-1 if v is None else int(v) for v in values], dtype=dtype)
    return np.array(values, dtype=dtype)


class Hdf5Writer:
    """Streams records into resizable per-stream datasets, flushing in batches."""

    def __init__(self, path: str, setup, args) -> None:
        import h5py  # local import so the npz path works without it

        self.h5py = h5py
        self.file = h5py.File(path, "w")
        self.file.attrs.update({
            "ran_identifier": setup.ran_identifier,
            "e3ap_version": setup.protocol_version,
            "dapp_id": setup.dapp_id,
            "requested_interval_ms": args.interval_ms,
            "drop_policy": args.drop_policy,
            "rnti_filter": np.array([int(r, 0) for r in args.rnti], dtype=np.uint16),
            "capture_start_unix": time.time(),
            "writer": "ocudu-dapp-sdk record_telemetry.py",
        })
        self.groups = {}
        for function in setup.ran_functions.values():
            for stream in function.streams:
                if stream.name in args.streams:
                    group = self.file.create_group(stream.name)
                    group.attrs["telemetry_id"] = stream.telemetry_id
                    group.attrs["interface_id"] = stream.interface_id
                    group.attrs["description"] = stream.description
                    group.attrs["default_floor_us"] = stream.minimum_interval_us or 0
                    group.attrs["hard_floor_us"] = stream.hard_floor_interval_us or 0
                    self.groups[stream.name] = group
        self.pending = defaultdict(lambda: defaultdict(list))
        self.pending_samples = defaultdict(list)
        self.pending_json = defaultdict(list)
        self.sample_total = defaultdict(int)
        self.pending_count = defaultdict(int)

    def add(self, indication) -> None:
        name = indication.stream
        if name not in self.groups:
            self.groups[name] = self.file.create_group(name)
        for key, value in flatten(indication).items():
            self.pending[name][key].append(value)
        if name in SAMPLE_STREAMS:
            block = np.asarray(list(indication.record.samples()), dtype=np.complex64)
            self.pending[name]["sample_offset"].append(self.sample_total[name])
            self.pending[name]["sample_count"].append(block.size)
            self.sample_total[name] += block.size
            self.pending_samples[name].append(block)
        elif name == "gnb.metrics_json":
            self.pending_json[name].append(indication.record.json_bytes.decode("utf-8", "replace"))
        self.pending_count[name] += 1
        if self.pending_count[name] >= FLUSH_RECORDS:
            self.flush(name)

    def _append(self, group, key, data, dtype=None, vlen=False) -> None:
        if not vlen and getattr(data, "dtype", None) is not None and data.dtype.kind == "O":
            vlen = True
            data = [str(v) for v in data.tolist()]
        if key not in group:
            if vlen:
                group.create_dataset(key, shape=(0,), maxshape=(None,), dtype=self.h5py.string_dtype(), chunks=(64,))
            else:
                chunk = (4096,) if key != "samples" else (65536,)
                group.create_dataset(key, shape=(0,), maxshape=(None,), dtype=dtype or data.dtype, chunks=chunk,
                                     compression=None if key == "samples" else "gzip")
        dataset = group[key]
        start = dataset.shape[0]
        dataset.resize(start + len(data), axis=0)
        dataset[start:] = data

    def flush(self, name: str) -> None:
        if self.pending_count[name] == 0:
            return
        group = self.groups[name]
        for key, values in self.pending[name].items():
            dtype = group[key].dtype if key in group else column_dtype(values)
            self._append(group, key, as_array(values, dtype), dtype)
        if self.pending_samples[name]:
            self._append(group, "samples", np.concatenate(self.pending_samples[name]))
        if self.pending_json[name]:
            self._append(group, "json", self.pending_json[name], vlen=True)
        self.pending[name].clear()
        self.pending_samples[name].clear()
        self.pending_json[name].clear()
        self.pending_count[name] = 0
        self.file.flush()

    def close(self) -> None:
        for name in list(self.groups):
            self.flush(name)
        self.file.attrs["capture_end_unix"] = time.time()
        self.file.close()


class NpzWriter:
    """In-memory fallback; same column names, arrays named <stream>__<field>."""

    def __init__(self, path: str, setup, args) -> None:
        self.path = path
        self.columns = defaultdict(lambda: defaultdict(list))
        self.samples = defaultdict(list)
        self.total = defaultdict(int)

    def add(self, indication) -> None:
        name = indication.stream
        for key, value in flatten(indication).items():
            self.columns[name][key].append(value)
        if name in SAMPLE_STREAMS:
            block = np.asarray(list(indication.record.samples()), dtype=np.complex64)
            self.columns[name]["sample_offset"].append(self.total[name])
            self.columns[name]["sample_count"].append(block.size)
            self.total[name] += block.size
            self.samples[name].append(block)
        elif name == "gnb.metrics_json":
            self.columns[name]["json"].append(indication.record.json_bytes.decode("utf-8", "replace"))

    def close(self) -> None:
        archive = {}
        for name, fields in self.columns.items():
            prefix = name.replace(".", "_")
            for key, values in fields.items():
                archive[f"{prefix}__{key}"] = np.array(values) if key == "json" else as_array(values, column_dtype(values))
            if name in self.samples:
                archive[f"{prefix}__samples"] = np.concatenate(self.samples[name]) if self.samples[name] else np.zeros(0, np.complex64)
        np.savez_compressed(self.path, **archive)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("streams", nargs="+", help="telemetry stream names, e.g. srs.channel pusch.symbols")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36423)
    parser.add_argument("--rnti", action="append", default=[], help="restrict per-UE streams to this RNTI (repeatable)")
    parser.add_argument("--interval-ms", type=int, default=0,
                        help="requested per-RNTI capture interval; 0 = every event (default)")
    parser.add_argument("--drop-policy", default="dropOldest", choices=("dropNewest", "dropOldest", "coalesceLatest"),
                        help="what the agent does when this subscriber falls behind (default keeps the newest)")
    parser.add_argument("--duration", type=float, default=10.0, help="capture length in seconds")
    parser.add_argument("--format", choices=("h5", "npz"), default="h5")
    parser.add_argument("--output", default=None, help="default telemetry_capture.<format>")
    args = parser.parse_args()
    output = args.output or f"telemetry_capture.{args.format}"

    rntis = [int(r, 0) for r in args.rnti]
    configs = [ocudu_e3.SubscriptionConfig(stream, rnti_filter=rntis, minimum_interval_ms=args.interval_ms,
                                           drop_policy=args.drop_policy) for stream in args.streams]
    counts = defaultdict(int)
    dropped = defaultdict(int)
    with connect(args.host, args.port, "sdk-recorder") as client:
        setup = client.setup()
        for function in setup.ran_functions.values():
            for stream in function.streams:
                if stream.name in args.streams and stream.hard_floor_interval_us:
                    print(f"note: {stream.name} hard floor is {stream.hard_floor_interval_us / 1000:.0f} ms; "
                          f"requests below it are clamped", file=sys.stderr)
        writer = (Hdf5Writer if args.format == "h5" else NpzWriter)(output, setup, args)
        client.subscribe(args.streams, configs=configs)
        deadline = time.monotonic() + args.duration
        started = time.monotonic()
        try:
            for indication in client.indications(timeout=1.0, stop=lambda: time.monotonic() >= deadline):
                counts[indication.stream] += 1
                dropped[indication.stream] = indication.dropped_objects
                writer.add(indication)
        finally:
            writer.close()
        client.release()
    elapsed = time.monotonic() - started

    for name in args.streams:
        print(f"{name:20s} records={counts[name]:6d} rate={counts[name] / max(elapsed, 1e-9):7.1f}/s "
              f"agent-side dropped={dropped[name]}")
    print(f"wrote {output} ({elapsed:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
