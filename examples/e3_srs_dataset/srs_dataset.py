# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""SRS occasions assembled from ``srs.channel`` telemetry, and the HDF5 dataset that holds them.

The gNB publishes one ``srs.channel`` record per (receive port, transmit port) pair of an SRS
occasion. Each record carries the pair's least-squares estimate per comb subcarrier, the gNB's
wideband measurements (RSRP, EPRE, noise variance, time alignment) and, from OCUDU hosts with the
SRS channel-estimator interface, the received pilots and the transmitted sequence the estimate was
computed from. :class:`OccasionAssembler` groups the records of one occasion; :class:`DatasetWriter`
stores complete occasions as fixed-shape tensors, one HDF5 group per occasion shape; and
:class:`SrsDataset` reads them back.

A dataset occasion with pilots holds exactly the input of an
``OCUDU_DAPP_INTERFACE_SRS_CHANNEL_ESTIMATOR_V1`` module: ``rx_symbols`` is CF32
``[pilot_set, rx_port, symbol, pilot]``, ``reference_symbols`` CF32 ``[tx_port, pilot]``, and
:func:`module_metadata` packs the matching ``ocudu_dapp_srs_channel_estimator_metadata_v1``. The
gNB's answer is the label: ``channel_matrix`` (the wideband coefficients scaled as the gNB scales
them) and the measurements.
"""

from __future__ import annotations

import math
import struct
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

import numpy as np

SCHEMA = "ocudu-srs-dataset-v1"

#: ``ocudu_dapp_srs_channel_estimator_metadata_v1`` (152 bytes, little endian).
METADATA = struct.Struct("<IHH" + "8I" + "4I" + "4I" + "2I" + "8I" + "d" + "4Q")
assert METADATA.size == 152
INTERLEAVED_PILOTS = 1

#: Per-UE link indicators copied from the latest gNB metrics report at each occasion.
KPI_FIELDS = ("pusch_snr_db", "ul_mcs", "cqi")


@dataclass
class Occasion:
    """One SRS occasion: every port pair's estimate plus, when captured, the dApp input."""

    rnti: int
    sfn: int
    slot: int
    numerology: int
    monotonic_ns: int
    nof_rx_ports: int
    nof_tx_ports: int
    start_symbol: int
    nof_symbols: int
    comb_size: int
    sequence_length: int
    #: Per transmit port: first comb subcarrier and cyclic shift n_cs.
    first_subcarrier: List[int]
    cyclic_shift: List[int]
    cyclic_shift_maximum: int
    sequence_group: int
    sequence_number: int
    sequence_id: int
    comb_offset: int
    bandwidth_index: int
    frequency_position: int
    frequency_shift: int
    frequency_hopping: int
    rsrp_db: float
    epre_db: float
    noise_variance: float
    time_alignment_ns: float
    #: complex64 [rx, tx, pilot]: least-squares estimate, averaged over symbols, time-alignment ramp removed.
    lse: np.ndarray = field(repr=False)
    #: complex64 [rx, tx, symbol, pilot]: received pilots at each transmit port's comb subcarriers, or None.
    pilots: Optional[np.ndarray] = field(default=None, repr=False)
    #: complex64 [tx, pilot]: transmitted sequences, cyclic shift included, or None.
    reference: Optional[np.ndarray] = field(default=None, repr=False)
    kpi: Dict[str, float] = field(default_factory=dict)

    @property
    def interleaved(self) -> bool:
        """Four ports whose cyclic shift is in the upper half of the range use two pilot sets (TS 38.211 6.4.1.4.3)."""
        return self.nof_tx_ports == 4 and self.cyclic_shift[0] >= self.cyclic_shift_maximum // 2

    @property
    def nof_pilot_sets(self) -> int:
        return 2 if self.interleaved else 1

    def pilot_set(self, tx: int) -> int:
        return tx % 2 if self.interleaved else 0

    @property
    def shape(self) -> str:
        """HDF5 group name: occasions of one shape stack into one tensor."""
        return (f"rx{self.nof_rx_ports}_tx{self.nof_tx_ports}_sym{self.nof_symbols}_comb{self.comb_size}"
                f"_len{self.sequence_length}")

    def rx_symbols(self) -> Optional[np.ndarray]:
        """complex64 [pilot_set, rx, symbol, pilot]: the ``rx_symbols`` tensor of the dApp input."""
        if self.pilots is None:
            return None
        # Port s is the first port of pilot set s (set 1 exists only with interleaved pilots, from port 1).
        return np.stack([self.pilots[:, first_port] for first_port in range(self.nof_pilot_sets)])

    def wideband(self) -> np.ndarray:
        """complex64 [rx, tx]: the gNB's wideband coefficient of each port pair before noise scaling."""
        return self.lse.mean(axis=-1).astype(np.complex64)

    def channel_matrix(self) -> np.ndarray:
        """complex64 [rx, tx]: the wideband coefficients scaled by the inverse noise standard deviation, never above
        40 dB SNR, as the gNB reports its SRS channel matrix."""
        wideband = self.wideband()
        rsrp = float(np.mean(np.abs(wideband) ** 2))
        noise = self.noise_variance if math.isfinite(self.noise_variance) else 0.0
        scale = max(math.sqrt(max(noise, 0.0)), math.sqrt(rsrp) * 0.01)
        return (wideband / scale).astype(np.complex64) if scale > 0 else wideband


def _value(value, default=math.nan) -> float:
    return default if value is None else float(value)


def _int(value) -> int:
    return 0 if value is None else int(value)


class OccasionAssembler:
    """Collects the per-port-pair records of each occasion and returns the occasion once all have arrived.

    Records of one occasion share (RNTI, SFN, slot). The telemetry stream is lossy: an occasion that
    misses a record stays pending until ``max_pending`` newer occasions arrive and is then counted in
    :attr:`incomplete`.
    """

    def __init__(self, max_pending: int = 64) -> None:
        if max_pending <= 0:
            raise ValueError("max_pending must be positive")
        self.max_pending = max_pending
        self.pending: "OrderedDict[Tuple[int, int, int, int], Dict[Tuple[int, int], object]]" = OrderedDict()
        self.first_seen: Dict[Tuple[int, int, int, int], int] = {}
        self.incomplete = 0
        self.inconsistent = 0

    def add(self, record, monotonic_ns: int = 0) -> Optional[Occasion]:
        key = (record.rnti, record.radio_slot.sfn, record.radio_slot.slot, record.radio_slot.numerology)
        pairs = self.pending.get(key)
        if pairs is None:
            pairs = self.pending[key] = {}
            self.first_seen[key] = monotonic_ns
            while len(self.pending) > self.max_pending:
                stale, _ = self.pending.popitem(last=False)
                self.first_seen.pop(stale, None)
                self.incomplete += 1
        pairs[(record.rx_port_index, record.tx_port_index)] = record
        if len(pairs) < record.nof_rx_ports * record.nof_tx_ports:
            return None
        del self.pending[key]
        started = self.first_seen.pop(key, monotonic_ns)
        occasion = self._build(key, pairs, started)
        if occasion is None:
            self.inconsistent += 1
        return occasion

    @staticmethod
    def _build(key, pairs, monotonic_ns: int) -> Optional[Occasion]:
        first = pairs[(0, 0)] if (0, 0) in pairs else next(iter(pairs.values()))
        rx_ports, tx_ports, length = first.nof_rx_ports, first.nof_tx_ports, first.nof_subcarriers
        if set(pairs) != {(rx, tx) for rx in range(rx_ports) for tx in range(tx_ports)} or length == 0:
            return None
        if any(r.nof_rx_ports != rx_ports or r.nof_tx_ports != tx_ports or r.nof_subcarriers != length
               for r in pairs.values()):
            return None
        lse = np.empty((rx_ports, tx_ports, length), dtype=np.complex64)
        for (rx, tx), record in pairs.items():
            values = np.frombuffer(record.coefficients, dtype=np.complex64)
            if values.size != length:
                return None
            lse[rx, tx] = values
        # The dApp input is kept only when every pair carries a consistent set of pilots.
        symbols = first.nof_symbols
        pilots = reference = None
        if all(r.received_pilots and r.reference_symbols for r in pairs.values()):
            pilots = np.empty((rx_ports, tx_ports, symbols, length), dtype=np.complex64)
            reference = np.empty((tx_ports, length), dtype=np.complex64)
            for (rx, tx), record in pairs.items():
                received = np.frombuffer(record.received_pilots, dtype=np.complex64)
                sequence = np.frombuffer(record.reference_symbols, dtype=np.complex64)
                if received.size != symbols * length or sequence.size != length:
                    pilots = reference = None
                    break
                pilots[rx, tx] = received.reshape(symbols, length)
                reference[tx] = sequence
        by_tx = [pairs[(0, tx)] for tx in range(tx_ports)]
        return Occasion(
            rnti=key[0], sfn=key[1], slot=key[2], numerology=key[3], monotonic_ns=monotonic_ns,
            nof_rx_ports=rx_ports, nof_tx_ports=tx_ports, start_symbol=first.start_symbol, nof_symbols=symbols,
            comb_size=first.comb_size, sequence_length=length,
            first_subcarrier=[_int(r.mapping_initial_subcarrier) for r in by_tx],
            cyclic_shift=[_int(r.cyclic_shift_index) for r in by_tx],
            cyclic_shift_maximum=_int(first.cyclic_shift_maximum),
            sequence_group=_int(first.sequence_group), sequence_number=_int(first.sequence_number),
            sequence_id=_int(first.sequence_id), comb_offset=_int(first.comb_offset),
            bandwidth_index=_int(first.bandwidth_index), frequency_position=_int(first.frequency_position),
            frequency_shift=_int(first.frequency_shift), frequency_hopping=_int(first.frequency_hopping),
            rsrp_db=_value(first.rsrp_db), epre_db=_value(first.epre_db),
            noise_variance=_value(first.noise_variance), time_alignment_ns=_value(first.time_alignment_ns),
            lse=lse, pilots=pilots, reference=reference)


def module_metadata(occasion: Occasion, grid_nof_subcarriers: Optional[int] = None) -> bytes:
    """``ocudu_dapp_srs_channel_estimator_metadata_v1`` for the occasion, as the gNB adapter fills it.

    The telemetry does not carry the carrier width; by default the smallest whole number of resource
    blocks that holds the SRS is used, which the estimator only checks, never reads.
    """
    sets = occasion.nof_pilot_sets
    first = occasion.first_subcarrier[:sets]
    last = max(first) + occasion.comb_size * (occasion.sequence_length - 1)
    if grid_nof_subcarriers is None:
        grid_nof_subcarriers = (last // 12 + 1) * 12
    scs_hz = 15000 << occasion.numerology
    pilot_set = [occasion.pilot_set(tx) for tx in range(occasion.nof_tx_ports)] + [0] * (4 - occasion.nof_tx_ports)
    cyclic_shift = occasion.cyclic_shift + [0] * (4 - occasion.nof_tx_ports)
    max_time_alignment = 1.0 / (occasion.cyclic_shift_maximum * scs_hz * occasion.comb_size)
    return METADATA.pack(
        METADATA.size, 1, 0,
        occasion.nof_rx_ports, occasion.nof_tx_ports, occasion.start_symbol, occasion.nof_symbols,
        occasion.sequence_length, occasion.comb_size, sets, occasion.cyclic_shift_maximum,
        *pilot_set, *cyclic_shift, *(first + [0] * (2 - sets)),
        occasion.sequence_group, occasion.sequence_number, scs_hz, occasion.numerology, grid_nof_subcarriers,
        INTERLEAVED_PILOTS if occasion.interleaved else 0, occasion.rnti, 0,
        max_time_alignment, 0, 0, 0, 0)


class DatasetWriter:
    """Appends occasions to an HDF5 file, one group per occasion shape under ``/occasions``."""

    FLUSH = 64

    def __init__(self, path: str, attributes: Optional[dict] = None) -> None:
        import h5py

        self.h5py = h5py
        self.file = h5py.File(path, "w")
        self.file.attrs.update({"schema": SCHEMA, "capture_start_unix": time.time(),
                                "writer": "ocudu-dapp-sdk capture_srs_dataset.py"})
        self.file.attrs.update(attributes or {})
        self.root = self.file.create_group("occasions")
        self.buffers: Dict[str, List[Occasion]] = {}
        self.counts: Dict[str, int] = {}
        self.with_pilots = 0

    def add(self, occasion: Occasion) -> None:
        self.buffers.setdefault(occasion.shape, []).append(occasion)
        self.with_pilots += occasion.pilots is not None
        if len(self.buffers[occasion.shape]) >= self.FLUSH:
            self._flush(occasion.shape)

    def _group(self, occasion: Occasion):
        if occasion.shape in self.root:
            return self.root[occasion.shape]
        group = self.root.create_group(occasion.shape)
        group.attrs.update({
            "nof_rx_ports": occasion.nof_rx_ports, "nof_tx_ports": occasion.nof_tx_ports,
            "nof_symbols": occasion.nof_symbols, "comb_size": occasion.comb_size,
            "sequence_length": occasion.sequence_length,
        })
        return group

    def _append(self, group, name: str, data: np.ndarray) -> None:
        if 0 in data.shape[1:]:
            return  # e.g. the pilot planes of a shape without SRS symbols: nothing to store
        if name not in group:
            group.create_dataset(name, shape=(0,) + data.shape[1:], maxshape=(None,) + data.shape[1:],
                                 dtype=data.dtype, chunks=(min(64, max(1, data.shape[0])),) + data.shape[1:],
                                 compression="gzip" if data.dtype.kind != "c" else None)
        dataset = group[name]
        start = dataset.shape[0]
        dataset.resize(start + data.shape[0], axis=0)
        dataset[start:] = data

    def _flush(self, shape: str) -> None:
        occasions = self.buffers.get(shape)
        if not occasions:
            return
        group = self._group(occasions[0])
        column = lambda getter, dtype: np.asarray([getter(o) for o in occasions], dtype=dtype)  # noqa: E731
        columns = {
            "rnti": column(lambda o: o.rnti, np.uint16),
            "sfn": column(lambda o: o.sfn, np.uint16),
            "slot": column(lambda o: o.slot, np.uint8),
            "numerology": column(lambda o: o.numerology, np.uint8),
            "monotonic_ns": column(lambda o: o.monotonic_ns, np.uint64),
            "start_symbol": column(lambda o: o.start_symbol, np.uint8),
            "first_subcarrier": column(lambda o: o.first_subcarrier, np.uint16),
            "cyclic_shift": column(lambda o: o.cyclic_shift, np.uint8),
            "cyclic_shift_maximum": column(lambda o: o.cyclic_shift_maximum, np.uint8),
            "sequence_group": column(lambda o: o.sequence_group, np.uint8),
            "sequence_number": column(lambda o: o.sequence_number, np.uint8),
            "sequence_id": column(lambda o: o.sequence_id, np.uint16),
            "comb_offset": column(lambda o: o.comb_offset, np.uint8),
            "bandwidth_index": column(lambda o: o.bandwidth_index, np.uint8),
            "frequency_position": column(lambda o: o.frequency_position, np.uint8),
            "frequency_shift": column(lambda o: o.frequency_shift, np.uint16),
            "frequency_hopping": column(lambda o: o.frequency_hopping, np.uint8),
            "rsrp_db": column(lambda o: o.rsrp_db, np.float32),
            "epre_db": column(lambda o: o.epre_db, np.float32),
            "noise_variance": column(lambda o: o.noise_variance, np.float32),
            "time_alignment_ns": column(lambda o: o.time_alignment_ns, np.float32),
            "has_pilots": column(lambda o: o.pilots is not None, np.bool_),
            "lse": np.stack([o.lse for o in occasions]),
            "wideband": np.stack([o.wideband() for o in occasions]),
            "channel_matrix": np.stack([o.channel_matrix() for o in occasions]),
        }
        for name in KPI_FIELDS:
            columns[name] = column(lambda o: o.kpi.get(name, math.nan), np.float32)
        # Occasions without pilots keep zeros there, flagged by has_pilots, so every column stays aligned.
        first = occasions[0]
        sets = max(o.nof_pilot_sets for o in occasions)
        rx_symbols = np.zeros((len(occasions), sets, first.nof_rx_ports, first.nof_symbols, first.sequence_length),
                              dtype=np.complex64)
        reference = np.zeros((len(occasions), first.nof_tx_ports, first.sequence_length), dtype=np.complex64)
        for index, occasion in enumerate(occasions):
            if occasion.pilots is not None:
                planes = occasion.rx_symbols()
                rx_symbols[index, : planes.shape[0]] = planes
                reference[index] = occasion.reference
        columns["rx_symbols"] = rx_symbols
        columns["reference_symbols"] = reference
        columns["nof_pilot_sets"] = column(lambda o: o.nof_pilot_sets, np.uint8)
        for name, data in columns.items():
            self._append(group, name, data)
        self.counts[shape] = self.counts.get(shape, 0) + len(occasions)
        occasions.clear()
        self.file.flush()

    def close(self, attributes: Optional[dict] = None) -> None:
        for shape in list(self.buffers):
            self._flush(shape)
        self.file.attrs["capture_end_unix"] = time.time()
        self.file.attrs.update(attributes or {})
        self.file.close()


class SrsDataset:
    """Read-only view of a dataset: ``for occasion in SrsDataset(path).occasions(): ...``."""

    def __init__(self, path: str) -> None:
        import h5py

        self.file = h5py.File(path, "r")
        if self.file.attrs.get("schema") != SCHEMA:
            raise ValueError(f"{path} is not an {SCHEMA} file")

    def __enter__(self) -> "SrsDataset":
        return self

    def __exit__(self, *_) -> None:
        self.file.close()

    @property
    def shapes(self) -> List[str]:
        return sorted(self.file["occasions"])

    def arrays(self, shape: str) -> Dict[str, np.ndarray]:
        """Every column of one shape group, loaded into memory (for training loops). The group's fixed shape
        (``nof_rx_ports``, ``nof_tx_ports``, ``nof_symbols``, ``comb_size``, ``sequence_length``) is in
        ``self.file["occasions"][shape].attrs``."""
        group = self.file["occasions"][shape]
        return {name: group[name][()] for name in group}

    def __len__(self) -> int:
        return sum(self.file["occasions"][shape]["rnti"].shape[0] for shape in self.shapes)

    def occasions(self, shapes: Optional[Sequence[str]] = None) -> Iterator[Occasion]:
        for shape in shapes or self.shapes:
            columns = self.arrays(shape)
            attributes = dict(self.file["occasions"][shape].attrs)
            for index in range(columns["rnti"].shape[0]):
                yield self._occasion(columns, index, attributes)

    @staticmethod
    def _occasion(columns: Dict[str, np.ndarray], index: int, attributes: dict) -> Occasion:
        lse = columns["lse"][index]
        rx_ports, tx_ports, length = lse.shape
        sets = int(columns["nof_pilot_sets"][index])
        pilots = reference = None
        if columns["has_pilots"][index]:
            planes = columns["rx_symbols"][index]
            pilots = np.stack([planes[tx % sets] for tx in range(tx_ports)], axis=1)
            reference = columns["reference_symbols"][index]
        return Occasion(
            rnti=int(columns["rnti"][index]), sfn=int(columns["sfn"][index]), slot=int(columns["slot"][index]),
            numerology=int(columns["numerology"][index]), monotonic_ns=int(columns["monotonic_ns"][index]),
            nof_rx_ports=rx_ports, nof_tx_ports=tx_ports, start_symbol=int(columns["start_symbol"][index]),
            nof_symbols=int(attributes["nof_symbols"]), comb_size=int(attributes["comb_size"]),
            sequence_length=length,
            first_subcarrier=[int(v) for v in columns["first_subcarrier"][index]],
            cyclic_shift=[int(v) for v in columns["cyclic_shift"][index]],
            cyclic_shift_maximum=int(columns["cyclic_shift_maximum"][index]),
            sequence_group=int(columns["sequence_group"][index]),
            sequence_number=int(columns["sequence_number"][index]), sequence_id=int(columns["sequence_id"][index]),
            comb_offset=int(columns["comb_offset"][index]), bandwidth_index=int(columns["bandwidth_index"][index]),
            frequency_position=int(columns["frequency_position"][index]),
            frequency_shift=int(columns["frequency_shift"][index]),
            frequency_hopping=int(columns["frequency_hopping"][index]),
            rsrp_db=float(columns["rsrp_db"][index]), epre_db=float(columns["epre_db"][index]),
            noise_variance=float(columns["noise_variance"][index]),
            time_alignment_ns=float(columns["time_alignment_ns"][index]),
            lse=lse, pilots=pilots, reference=reference,
            kpi={name: float(columns[name][index]) for name in KPI_FIELDS if name in columns})
