#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""OCUDU MCP server: a Model Context Protocol companion for an OCUDU gNB, over E3 only.

    python3 ocudu_mcp_server.py --host 127.0.0.1 --port 36423
    python3 ocudu_mcp_server.py --read-only            # observation tools only
    claude mcp add ocudu -- python3 /path/to/ocudu_mcp_server.py --host 10.0.0.5

Everything the server knows or does goes through one SCTP association to the
gNB's E3AP endpoint: it performs the E3 setup, subscribes to the telemetry
service model (PUSCH symbols/CRC, SRS channel response, gNB metrics JSON) and
keeps a live model of the sector — connected UEs, per-UE link KPIs, BLER,
throughput and PRB-usage history, SRS channel state — while management
requests (dApp inventory/health/metrics/config/lifecycle, incidents, jobs,
quiet reservations, RAN control such as releasing a UE) ride the same
association as control actions. No local socket, no root, no gNB-side plugin.

The server speaks MCP over stdio (JSON-RPC 2.0, newline-delimited) with no
SDK dependency, so any MCP client (Claude Desktop, Claude Code, Cursor, ...)
can attach it. Tools are split into *observe* (always available) and *act*
(disabled with ``--read-only``); mutating tools default to ``dry_run=true``
where the management protocol supports it.

What "settings, gains and scheduler parameters" means here: the E3 management
model exposes configuration per dApp instance (typed key/value entries with a
schema), so cell-level knobs are whatever the loaded dApps publish — the
reference scheduler dApp (target BLER, MCS caps, ...), the equalizer, the
channel emulator (gains, fading), sensing dApps. ``get_dapp_config`` with
``schema=true`` lists the keys, kinds, ranges and defaults; ``set_dapp_config``
writes them with a config-generation check. Anything the gNB does not expose
through E3 is deliberately not faked here.
"""

from __future__ import annotations

import argparse
import json
import logging
import math
import os
import queue
import secrets
import sys
import threading
import time
from collections import defaultdict, deque
from concurrent.futures import Future
from dataclasses import dataclass, field
from typing import Any, Callable, Deque, Dict, List, Optional, Tuple

import numpy as np

from _common import connect, ocudu_e3

mgmt = ocudu_e3.management

SERVER_NAME = "ocudu-e3"
SERVER_VERSION = "0.1.0"
PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")
DEFAULT_STREAMS = ("pusch.symbols", "pusch.crc", "srs.channel", "gnb.metrics_json")
HISTORY_SECONDS = 600
LOG = logging.getLogger("ocudu_mcp")


# ---------------------------------------------------------------------------
# Live sector model fed by E3 telemetry
# ---------------------------------------------------------------------------


def _finite(value) -> Optional[float]:
    try:
        v = float(value)
    except (TypeError, ValueError):
        return None
    return v if math.isfinite(v) else None


@dataclass
class UeState:
    rnti: int
    first_seen: float
    last_seen: float
    # From pusch.symbols (PHY, per grant).
    sinr_db: Optional[float] = None
    evm: Optional[float] = None
    rsrp_db: Optional[float] = None
    epre_db: Optional[float] = None
    ta_us: Optional[float] = None
    cfo_hz: Optional[float] = None
    modulation_bits: Optional[int] = None
    nof_prb: Optional[int] = None
    nof_layers: Optional[int] = None
    target_code_rate: Optional[float] = None
    mcs_index: Optional[int] = None
    mcs_table: Optional[int] = None
    tbs_bytes: Optional[int] = None
    grants: int = 0
    last_symbols: Optional[np.ndarray] = None
    # From pusch.crc.
    crc_window: Deque[Tuple[float, bool]] = field(default_factory=lambda: deque(maxlen=4000))
    # From srs.channel (per port pair).
    srs_rsrp_db: Optional[float] = None
    srs_ta_ns: Optional[float] = None
    srs_noise_var: Optional[float] = None
    srs_ports: Optional[Tuple[int, int]] = None
    srs_last_seen: Optional[float] = None
    srs_h_mag_db: Dict[str, List[float]] = field(default_factory=dict)
    srs_h_phase: Dict[str, List[float]] = field(default_factory=dict)
    srs_occasions: int = 0
    # From gnb.metrics_json (MAC/scheduler view).
    mac: Dict[str, Any] = field(default_factory=dict)
    mac_last_seen: Optional[float] = None

    def bler(self, window_s: float, now: float) -> Optional[Tuple[float, int]]:
        recent = [ok for t, ok in self.crc_window if now - t <= window_s]
        if not recent:
            return None
        return (1.0 - sum(recent) / len(recent), len(recent))

    def summary(self, now: float, window_s: float = 10.0) -> Dict[str, Any]:
        bler = self.bler(window_s, now)
        out: Dict[str, Any] = {
            "rnti": f"0x{self.rnti:04x}",
            "age_s": round(now - self.first_seen, 1),
            "last_seen_s_ago": round(now - self.last_seen, 2),
            "phy": {
                "sinr_db": self.sinr_db,
                "evm": self.evm,
                "rsrp_db": self.rsrp_db,
                "time_alignment_us": self.ta_us,
                "cfo_hz": self.cfo_hz,
                "modulation_bits": self.modulation_bits,
                "mcs_index": self.mcs_index,
                "mcs_table": {0: "qam64", 1: "qam256", 2: "qam64LowSe"}.get(self.mcs_table) if self.mcs_table is not None else None,
                "last_grant_prb": self.nof_prb,
                "last_tbs_bytes": self.tbs_bytes,
                "layers": self.nof_layers,
                "grants_observed": self.grants,
            },
            "ul_bler": None if bler is None else {"bler": round(bler[0], 4), "transport_blocks": bler[1], "window_s": window_s},
            "srs": None,
            "mac": self.mac or None,
        }
        if self.srs_last_seen is not None:
            out["srs"] = {
                "rsrp_db": self.srs_rsrp_db,
                "time_alignment_ns": self.srs_ta_ns,
                "noise_variance": self.srs_noise_var,
                "rx_tx_ports": self.srs_ports,
                "occasions": self.srs_occasions,
                "last_seen_s_ago": round(now - self.srs_last_seen, 2),
                "port_pairs": sorted(self.srs_h_mag_db),
            }
        return out


class SectorState:
    """Thread-safe live model built from E3 indications."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.ues: Dict[int, UeState] = {}
        self.cell: Dict[str, Any] = {}
        self.du_latency: Dict[str, Any] = {}
        self.metrics_reports = 0
        self.last_metrics_time: Optional[float] = None
        # Time series (t, value) for traffic patterns.
        self.cell_history: Dict[str, Deque[Tuple[float, float]]] = defaultdict(lambda: deque(maxlen=20000))
        self.ue_history: Dict[int, Dict[str, Deque[Tuple[float, float]]]] = defaultdict(
            lambda: defaultdict(lambda: deque(maxlen=20000))
        )
        self.stream_counts: Dict[str, int] = defaultdict(int)
        self.dropped_indications = 0
        self.raw_last_report: Optional[dict] = None
        # Other gNB report kinds, kept whole: cu-cp (AMF/NGAP, PDU sessions), app_resource_usage, du_low.
        self.extra: Dict[str, Any] = {}

    def _ue(self, rnti: int, now: float) -> UeState:
        ue = self.ues.get(rnti)
        if ue is None:
            ue = self.ues[rnti] = UeState(rnti, now, now)
        ue.last_seen = now
        return ue

    def ingest(self, indication) -> None:
        now = time.time()
        r = indication.record
        tel = ocudu_e3.telemetry
        with self.lock:
            self.stream_counts[indication.stream] += 1
            self.dropped_indications = max(self.dropped_indications, indication.dropped_objects)
            if isinstance(r, tel.PuschSymbolsRecord):
                ue = self._ue(r.rnti, now)
                ue.grants += 1
                ue.sinr_db, ue.evm, ue.rsrp_db, ue.epre_db = r.sinr_db, r.evm, r.rsrp_db, r.epre_db
                ue.ta_us, ue.cfo_hz = r.time_alignment_us, r.cfo_hz
                ue.modulation_bits, ue.nof_prb, ue.nof_layers = r.modulation_bits, r.nof_prb, r.nof_layers
                ue.target_code_rate = r.target_code_rate
                if r.grant is not None:
                    ue.mcs_index, ue.mcs_table, ue.tbs_bytes = r.grant.mcs_index, r.grant.mcs_table, r.grant.tbs_bytes
                try:
                    ue.last_symbols = np.asarray(r.samples(), dtype=np.complex64)[:4096]
                except Exception:
                    ue.last_symbols = None
                if r.sinr_db is not None:
                    self.ue_history[r.rnti]["sinr_db"].append((now, r.sinr_db))
                if r.crc_ok is not None:
                    ue.crc_window.append((now, bool(r.crc_ok)))
            elif isinstance(r, tel.PuschCrcRecord):
                ue = self._ue(r.rnti, now)
                ue.crc_window.append((now, bool(r.crc_ok)))
            elif isinstance(r, tel.SrsChannelRecord):
                ue = self._ue(r.rnti, now)
                ue.srs_last_seen = now
                ue.srs_occasions += 1
                ue.srs_rsrp_db, ue.srs_ta_ns, ue.srs_noise_var = r.rsrp_db, r.time_alignment_ns, r.noise_variance
                ue.srs_ports = (r.nof_rx_ports, r.nof_tx_ports)
                try:
                    h = np.asarray(r.samples(), dtype=np.complex64)
                    key = f"rx{r.rx_port_index}_tx{r.tx_port_index}"
                    ue.srs_h_mag_db[key] = (20 * np.log10(np.abs(h) + 1e-12)).round(2).tolist()
                    ue.srs_h_phase[key] = np.angle(h).round(3).tolist()
                except Exception:
                    pass
                if r.rsrp_db is not None:
                    self.ue_history[r.rnti]["srs_rsrp_db"].append((now, r.rsrp_db))
            elif isinstance(r, tel.GnbMetricsJsonRecord):
                try:
                    report = r.json()
                except Exception:
                    return
                if isinstance(report, dict):
                    self._ingest_metrics(report, now)

    def _ingest_metrics(self, report: dict, now: float) -> None:
        self.metrics_reports += 1
        self.last_metrics_time = now
        self.raw_last_report = report
        for key in ("cu-cp", "app_resource_usage", "du_low", "buffer_pool"):
            if key in report:
                self.extra[key] = report[key]
        ue_items: List[dict] = list(report.get("ue_list", []) or [])
        for cell in report.get("cells", []) or []:
            metrics = cell.get("cell_metrics") or {}
            ue_items.extend(cell.get("ue_list", []) or [])
            self.cell.update({k: v for k, v in metrics.items()})
            self.cell["last_update_s_ago"] = 0.0
            for key in ("pdsch_prbs_used_per_tdd_slot_idx", "pusch_prbs_used_per_tdd_slot_idx"):
                if isinstance(metrics.get(key), list) and metrics[key]:
                    self.cell_history[key].append((now, float(sum(metrics[key]) / len(metrics[key]))))
            for key in ("error_indication_count", "late_dl_harqs", "late_ul_harqs", "max_latency"):
                if metrics.get(key) is not None:
                    self.cell_history[key].append((now, float(metrics[key])))
        du = (report.get("du") or {}).get("du_high") or {}
        for direction in ("dl", "ul"):
            for entry in ((du.get("mac") or {}).get(direction) or []):
                self.du_latency[direction] = entry
        dl_total = ul_total = 0.0
        for item in ue_items:
            ue_container = item.get("ue_container") or item
            rnti = ue_container.get("rnti")
            if rnti is None:
                continue
            ue = self._ue(int(rnti), now)
            ue.mac_last_seen = now
            ue.mac = {k: v for k, v in ue_container.items() if k != "rnti"}
            for key in ("dl_brate", "ul_brate", "cqi", "dl_mcs", "ul_mcs", "pusch_snr_db", "pucch_snr_db", "bsr", "dl_bs"):
                value = _finite(ue_container.get(key))
                if value is not None:
                    self.ue_history[int(rnti)][key].append((now, value))
            dl_total += _finite(ue_container.get("dl_brate")) or 0.0
            ul_total += _finite(ue_container.get("ul_brate")) or 0.0
        if report.get("ue_list") is not None or any("ue_list" in (c or {}) for c in report.get("cells", []) or []):
            self.cell_history["dl_bitrate_bps"].append((now, dl_total))
            self.cell_history["ul_bitrate_bps"].append((now, ul_total))
            self.cell_history["nof_ues"].append((now, float(len(ue_items))))

    # ---- queries -----------------------------------------------------------
    def prune(self, max_age_s: float = 60.0) -> None:
        now = time.time()
        with self.lock:
            for rnti in [r for r, u in self.ues.items() if now - u.last_seen > max_age_s]:
                del self.ues[rnti]
                self.ue_history.pop(rnti, None)

    def overview(self) -> Dict[str, Any]:
        now = time.time()
        with self.lock:
            active = [u for u in self.ues.values() if now - u.last_seen <= 10]
            blers = [b[0] for b in (u.bler(10.0, now) for u in active) if b]
            sinrs = [u.sinr_db for u in active if u.sinr_db is not None]
            hist = self.cell_history
            latest = lambda key: hist[key][-1][1] if hist.get(key) else None  # noqa: E731
            return {
                "time": now,
                "ues_active_10s": len(active),
                "ues_tracked": len(self.ues),
                "rntis": [f"0x{u.rnti:04x}" for u in active],
                "ul_sinr_db": {"min": min(sinrs), "median": float(np.median(sinrs)), "max": max(sinrs)} if sinrs else None,
                "ul_bler_10s": {"mean": float(np.mean(blers)), "worst": max(blers)} if blers else None,
                "throughput_bps": {"dl": latest("dl_bitrate_bps"), "ul": latest("ul_bitrate_bps")},
                "prb_usage_avg_per_tdd_slot": {
                    "pdsch": latest("pdsch_prbs_used_per_tdd_slot_idx"),
                    "pusch": latest("pusch_prbs_used_per_tdd_slot_idx"),
                },
                "cell_metrics": {k: self.cell.get(k) for k in (
                    "pci", "error_indication_count", "late_dl_harqs", "late_ul_harqs", "max_latency", "average_latency",
                    "avg_prach_delay", "msg3_nof_ok", "msg3_nof_nok", "failed_dl_pdcch", "failed_ul_pdcch",
                    "pucch_tot_rb_usage_avg") if k in self.cell},
                "du_mac_latency_us": {d: {k: e.get(k) for k in ("average_latency_us", "max_latency_us", "cpu_usage_percent")}
                                       for d, e in self.du_latency.items()},
                "cu_cp": self._cu_cp_summary(),
                "app_resource_usage": self.extra.get("app_resource_usage"),
                "du_low_latency_us": {d: {k: (self.extra.get("du_low", {}).get(d) or {}).get(k) for k in ("average_latency_us", "max_latency_us", "cpu_usage_percent")}
                                      for d in ("dl", "ul") if isinstance(self.extra.get("du_low", {}).get(d), dict)},
                "metrics_reports": self.metrics_reports,
                "metrics_last_s_ago": None if self.last_metrics_time is None else round(now - self.last_metrics_time, 1),
                "telemetry_counts": dict(self.stream_counts),
                "agent_side_dropped": self.dropped_indications,
            }

    def _cu_cp_summary(self) -> Optional[Dict[str, Any]]:
        cu = self.extra.get("cu-cp")
        if not isinstance(cu, dict):
            return None
        amfs = []
        for ngap in ((cu.get("ngaps") or {}).get("ngap") or []):
            amfs.append({"amf_name": ngap.get("amf_name"), "connected": ngap.get("connected"),
                         "pdu_sessions": ngap.get("pdu_session_management"),
                         "paging": ngap.get("paging_measurement")})
        return {"id": cu.get("id"), "amfs": amfs, "raw_keys": sorted(cu)}

    def ue_list(self, window_s: float = 10.0) -> List[Dict[str, Any]]:
        now = time.time()
        with self.lock:
            return [u.summary(now, window_s) for u in sorted(self.ues.values(), key=lambda u: u.rnti)]

    def ue(self, rnti: int) -> Optional[Dict[str, Any]]:
        now = time.time()
        with self.lock:
            u = self.ues.get(rnti)
            return None if u is None else u.summary(now)

    def history(self, key: str, window_s: float, rnti: Optional[int] = None, buckets: int = 60) -> Dict[str, Any]:
        now = time.time()
        with self.lock:
            series = (self.ue_history.get(rnti, {}) if rnti is not None else self.cell_history).get(key)
            points = [(t, v) for t, v in (series or []) if now - t <= window_s]
        if not points:
            return {"key": key, "window_s": window_s, "samples": 0, "buckets": []}
        edges = np.linspace(now - window_s, now, buckets + 1)
        values = np.array([v for _, v in points])
        times = np.array([t for t, _ in points])
        out = []
        for i in range(buckets):
            sel = (times >= edges[i]) & (times < edges[i + 1])
            if sel.any():
                out.append({"t_offset_s": round(edges[i] - now, 1), "mean": float(values[sel].mean()),
                            "max": float(values[sel].max()), "n": int(sel.sum())})
        return {"key": key, "window_s": window_s, "samples": len(points), "latest": float(values[-1]),
                "mean": float(values.mean()), "buckets": out}

    def srs_channel(self, rnti: int) -> Optional[Dict[str, Any]]:
        with self.lock:
            u = self.ues.get(rnti)
            if u is None or not u.srs_h_mag_db:
                return None
            return {"rnti": f"0x{rnti:04x}", "rsrp_db": u.srs_rsrp_db, "time_alignment_ns": u.srs_ta_ns,
                    "noise_variance": u.srs_noise_var, "h_mag_db": u.srs_h_mag_db, "h_phase_rad": u.srs_h_phase}

    def constellation(self, rnti: int, max_points: int) -> Optional[Dict[str, Any]]:
        with self.lock:
            u = self.ues.get(rnti)
            if u is None or u.last_symbols is None:
                return None
            s = u.last_symbols[:max_points]
            return {"rnti": f"0x{rnti:04x}", "modulation_bits": u.modulation_bits, "sinr_db": u.sinr_db, "evm": u.evm,
                    "points": [[round(float(x.real), 4), round(float(x.imag), 4)] for x in s],
                    "total_symbols_in_last_grant": int(u.last_symbols.size)}


# ---------------------------------------------------------------------------
# E3 session: one association for telemetry and management
# ---------------------------------------------------------------------------


class E3Session(threading.Thread):
    """Owns the SCTP association; telemetry is ingested here and management calls are marshalled in."""

    def __init__(self, host: str, port: int, name: str, streams: Tuple[str, ...], state: SectorState) -> None:
        super().__init__(name="e3-session", daemon=True)
        self.host, self.port, self.name_, self.streams, self.state = host, port, name, tuple(streams), state
        self.controls: "queue.Queue[Tuple[Future, Callable]]" = queue.Queue(maxsize=32)
        self.status: Dict[str, Any] = {"connected": False, "reconnects": 0, "last_error": "", "dapp_id": None,
                                       "ran_identifier": None, "catalog": [], "subscribed": [], "indications": 0}
        self.stop_event = threading.Event()
        self.client = None
        self.requested_streams: Tuple[str, ...] = self.streams
        self.requested_interval_ms: Optional[int] = None
        self._subscription = None

    def run(self) -> None:
        backoff = 1.0
        while not self.stop_event.is_set():
            try:
                self._session()
                backoff = 1.0
            except Exception as exc:  # noqa: BLE001
                self.status.update(connected=False, last_error=str(exc))
                LOG.warning("E3 session ended: %s", exc)
            if self.stop_event.is_set():
                break
            self.status["reconnects"] += 1
            self.stop_event.wait(backoff)
            backoff = min(10.0, backoff * 2)

    def _session(self) -> None:
        with connect(self.host, self.port, self.name_) as client:
            self.client = client
            self._subscription = None
            setup = client.setup()
            available = [s.name for s in (setup.telemetry.streams if setup.telemetry else [])]
            self.status.update(connected=True, dapp_id=setup.dapp_id, ran_identifier=setup.ran_identifier,
                               catalog=[{"id": s.telemetry_id, "name": s.name, "description": s.description,
                                         "default_floor_ms": (s.minimum_interval_us or 0) / 1000,
                                         "hard_floor_ms": (s.hard_floor_interval_us or 0) / 1000,
                                         "class_c": s.class_c_portable}
                                        for s in (setup.telemetry.streams if setup.telemetry else [])],
                               last_error="")
            self._resubscribe(client, available)
            last_prune = time.monotonic()
            while not self.stop_event.is_set():
                self._drain_controls(client)
                ind = client.next_indication(timeout=0.1)
                if ind is None:
                    if not client.connected:
                        raise RuntimeError("association closed")
                else:
                    self.status["indications"] += 1
                    try:
                        self.state.ingest(ind)
                    except Exception as exc:  # noqa: BLE001
                        LOG.debug("ingest error: %s", exc)
                if time.monotonic() - last_prune > 5:
                    self.state.prune()
                    last_prune = time.monotonic()
            try:
                client.release()
            except Exception:  # noqa: BLE001
                pass

    def _resubscribe(self, client, available: List[str]) -> None:
        wanted = [s for s in self.requested_streams if s in available]
        if self._subscription is not None:
            try:
                client.unsubscribe(self._subscription)
            except Exception:  # noqa: BLE001
                pass
            self._subscription = None
        if wanted:
            configs = None
            if self.requested_interval_ms is not None:
                configs = [ocudu_e3.SubscriptionConfig(s, minimum_interval_ms=self.requested_interval_ms) for s in wanted]
            self._subscription = client.subscribe(wanted, configs=configs)
        self.status["subscribed"] = wanted

    def _drain_controls(self, client) -> None:
        for _ in range(4):
            try:
                future, action = self.controls.get_nowait()
            except queue.Empty:
                return
            try:
                future.set_result(action(client))
            except Exception as exc:  # noqa: BLE001
                future.set_exception(exc)

    def call(self, action: Callable, timeout: float = 10.0):
        """Run ``action(client)`` on the session thread and return its result."""
        if not self.status.get("connected"):
            raise RuntimeError(f"not connected to E3AP endpoint {self.host}:{self.port}: {self.status.get('last_error') or 'connecting'}")
        future: Future = Future()
        self.controls.put((future, action), timeout=1.0)
        return future.result(timeout=timeout)

    def management(self, request: dict) -> dict:
        return mgmt.normalize(self.call(lambda client: client.management(request)))

    def set_streams(self, streams: Tuple[str, ...], interval_ms: Optional[int]) -> List[str]:
        self.requested_streams, self.requested_interval_ms = streams, interval_ms

        def action(client):
            self._resubscribe(client, [c["name"] for c in self.status["catalog"]])
            return self.status["subscribed"]

        return self.call(action)


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------


def _rnti(value) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def _identifier(value: str) -> bytes:
    return mgmt.parse_identifier(value)


def _typed_value(kind: str, value) -> Tuple[str, Any]:
    kind = kind.lower()
    if kind in ("boolean", "bool"):
        return ("booleanValue", bool(value))
    if kind in ("signed", "int", "integer"):
        return ("signedValue", int(value).to_bytes(8, "big", signed=True))
    if kind in ("unsigned", "uint", "counter"):
        return ("unsignedValue", int(value).to_bytes(8, "big"))
    if kind in ("real", "float", "double"):
        return ("realValue", float(value))
    if kind in ("string", "text"):
        return ("stringValue", str(value).encode("utf-8"))
    if kind in ("bytes", "hex"):
        return ("bytesValue", bytes.fromhex(str(value)))
    raise ValueError(f"unknown value kind {kind!r}")


class Tools:
    def __init__(self, session: E3Session, state: SectorState, read_only: bool) -> None:
        self.session, self.state, self.read_only = session, state, read_only

    # ---- observe -----------------------------------------------------------
    def server_status(self) -> dict:
        return {"server": SERVER_NAME, "version": SERVER_VERSION, "read_only": self.read_only,
                "endpoint": f"e3ap://{self.session.host}:{self.session.port}", **self.session.status,
                "catalog": [c["name"] for c in self.session.status.get("catalog", [])]}

    def get_cell_overview(self) -> dict:
        out = self.state.overview()
        out["e3"] = {"connected": self.session.status["connected"], "ran_identifier": self.session.status["ran_identifier"],
                     "dapp_id": self.session.status["dapp_id"]}
        return out

    def list_ues(self, window_s: float = 10.0) -> dict:
        return {"ues": self.state.ue_list(window_s)}

    def get_ue(self, rnti) -> dict:
        ue = self.state.ue(_rnti(rnti))
        if ue is None:
            raise ValueError(f"RNTI {rnti} not observed (see list_ues)")
        return ue

    def get_kpis(self, window_s: float = 60.0) -> dict:
        keys = ("dl_bitrate_bps", "ul_bitrate_bps", "nof_ues", "pusch_prbs_used_per_tdd_slot_idx",
                "pdsch_prbs_used_per_tdd_slot_idx", "error_indication_count", "late_ul_harqs", "late_dl_harqs")
        now = time.time()
        with self.state.lock:
            ues = list(self.state.ues.values())
        per_ue = []
        for u in ues:
            if now - u.last_seen > window_s:
                continue
            b = u.bler(window_s, now)
            per_ue.append({"rnti": f"0x{u.rnti:04x}", "ul_bler": None if b is None else round(b[0], 4),
                           "transport_blocks": None if b is None else b[1], "sinr_db": u.sinr_db,
                           "dl_brate_bps": _finite(u.mac.get("dl_brate")), "ul_brate_bps": _finite(u.mac.get("ul_brate")),
                           "cqi": u.mac.get("cqi"), "dl_mcs": u.mac.get("dl_mcs"), "ul_mcs": u.mac.get("ul_mcs"),
                           "ta_ns": u.mac.get("ta_ns"), "pusch_snr_db": u.mac.get("pusch_snr_db")})
        return {"window_s": window_s, "cell": {k: self.state.history(k, window_s, buckets=1) for k in keys}, "ues": per_ue}

    def get_traffic_history(self, window_s: float = 300.0, rnti: Optional[str] = None, keys: Optional[List[str]] = None,
                            buckets: int = 60) -> dict:
        if rnti is not None:
            keys = keys or ["dl_brate", "ul_brate", "sinr_db", "ul_mcs", "cqi"]
            return {k: self.state.history(k, window_s, _rnti(rnti), buckets) for k in keys}
        keys = keys or ["dl_bitrate_bps", "ul_bitrate_bps", "nof_ues", "pusch_prbs_used_per_tdd_slot_idx"]
        return {k: self.state.history(k, window_s, None, buckets) for k in keys}

    def get_srs_channel(self, rnti) -> dict:
        out = self.state.srs_channel(_rnti(rnti))
        if out is None:
            raise ValueError(f"no SRS channel response observed for {rnti}")
        return out

    def get_constellation(self, rnti, max_points: int = 512) -> dict:
        out = self.state.constellation(_rnti(rnti), int(max_points))
        if out is None:
            raise ValueError(f"no PUSCH symbols observed for {rnti}")
        return out

    def get_telemetry_catalog(self) -> dict:
        return {"streams": self.session.status.get("catalog", []), "subscribed": self.session.status.get("subscribed", [])}

    def get_e3_connections(self) -> dict:
        inv = self.session.management(mgmt.inventory_request(include_packages=False, include_instances=False))
        return {"connections": (inv.get("payload", {}).get("inventory", {}) or {}).get("connections", [])}

    def get_dapp_inventory(self) -> dict:
        return self.session.management(mgmt.inventory_request()).get("payload", {}).get("inventory", {})

    def get_dapp_health(self) -> dict:
        return self.session.management(mgmt.health_request()).get("payload", {}).get("health", {})

    def get_dapp_metrics(self, instance_id: str, schema: bool = False) -> dict:
        r = self.session.management(mgmt.metrics_request(_identifier(instance_id), schema=schema))
        return r.get("payload", {}).get("metrics", r)

    def get_dapp_config(self, instance_id: str, schema: bool = False) -> dict:
        r = self.session.management(mgmt.config_request(_identifier(instance_id), schema=schema))
        return r.get("payload", {}).get("config", r)

    def list_incidents(self, instance_id: Optional[str] = None, limit: int = 50) -> dict:
        r = self.session.management(mgmt.incidents_request(_identifier(instance_id) if instance_id else None, limit=limit))
        return r.get("payload", {}).get("incidents", r)

    def list_jobs(self) -> dict:
        return self.session.management(mgmt.jobs_request()).get("payload", {}).get("jobs", {})

    def get_job(self, job_id: str) -> dict:
        r = self.session.management(mgmt.jobs_request(_identifier(job_id)))
        jobs = (r.get("payload", {}).get("jobs", {}) or {}).get("jobs", [])
        return jobs[0] if jobs else r

    def _follow_job(self, response: dict, timeout_s: float = 3.0) -> dict:
        """Lifecycle and config mutations are asynchronous: the immediate reply is
        'accepted' with a jobId and the real outcome (success, dryRunSuccess or a
        validation failure) lands in the job record. Wait for it and attach it."""
        job_id = response.get("jobId")
        if not job_id or response.get("code") not in ("accepted",):
            return response
        deadline = time.monotonic() + timeout_s
        job = None
        while time.monotonic() < deadline:
            try:
                job = self.get_job(job_id)
            except Exception:  # noqa: BLE001
                break
            if job.get("state") in ("succeeded", "failed", "cancelled"):
                break
            time.sleep(0.05)
        if job:
            response = dict(response)
            response["job"] = job
            result = job.get("result") or {}
            response["outcome"] = result.get("code") or job.get("state")
            if job.get("state") == "failed" or (result.get("code") not in (None, "success", "dryRunSuccess", "accepted")):
                response["ok"] = False
            else:
                response["ok"] = job.get("state") == "succeeded"
        return response

    def list_quiet_reservations(self, cell_id: Optional[int] = None, limit: int = 50) -> dict:
        body: Dict[str, Any] = {"maximumRecords": limit}
        if cell_id is not None:
            body["cellId"] = cell_id
        r = self.session.management({"header": mgmt.header(), "operation": ("quietReservations", body)})
        return r.get("payload", {}).get("quietReservations", r)

    def get_cell_info(self) -> dict:
        r = self.session.management(mgmt.cell_info_request())
        return r.get("payload", {}).get("cellInfo", r)

    def list_data_subscriptions(self) -> dict:
        r = self.session.management({"header": mgmt.header(), "operation": ("dataSubscriptions", {"action": "list"})})
        return r.get("payload", {}).get("dataSubscriptions", r)

    # ---- act ---------------------------------------------------------------
    def _guard(self, what: str) -> None:
        if self.read_only:
            raise PermissionError(f"{what} refused: server started with --read-only")

    def set_dapp_config(self, instance_id: str, entries: Dict[str, Any], boundary: str = "invocation",
                        dry_run: bool = True, expected_config_generation: Optional[int] = None,
                        kinds: Optional[Dict[str, str]] = None) -> dict:
        self._guard("set_dapp_config")
        ident = _identifier(instance_id)
        schema = self.session.management(mgmt.config_request(ident, schema=True)).get("payload", {}).get("config", {})
        descriptors = {d.get("key"): d for d in schema.get("descriptors", []) or []}
        if expected_config_generation is None:
            effective = self.session.management(mgmt.config_request(ident)).get("payload", {}).get("config", {})
            expected_config_generation = int(effective.get("generation") or 0)
        wire_entries = []
        unknown = []
        for key, value in entries.items():
            kind = (kinds or {}).get(key) or (descriptors.get(key) or {}).get("kind")
            if kind is None:
                unknown.append(key)
                continue
            wire_entries.append({"key": key.encode("utf-8"), "value": _typed_value(str(kind), value)})
        if unknown:
            raise ValueError(f"keys not in the instance config schema (pass kinds= to force): {unknown}; "
                             f"known keys: {sorted(descriptors)}")
        request = {
            "header": mgmt.header(None, ident, dry_run=dry_run),
            "operation": ("configSet", {"expectedConfigGeneration": int(expected_config_generation).to_bytes(8, "big"),
                                        "boundary": boundary, "entries": wire_entries}),
        }
        response = self._follow_job(self.session.management(request))
        if not dry_run and response.get("ok"):
            effective = self.session.management(mgmt.config_request(ident)).get("payload", {}).get("config", {})
            response["effective"] = {e.get("key"): e.get("value") for e in effective.get("entries", []) or [] if e.get("key") in entries}
            response["config_generation"] = effective.get("generation")
        return response

    def dapp_lifecycle(self, instance_id: str, action: str, dry_run: bool = True, timeout_us: int = 1_000_000) -> dict:
        self._guard("dapp_lifecycle")
        allowed = {"prepare", "arm", "enterShadow", "activate", "deactivate", "retryQuiescence", "unload", "breakerProbe"}
        if action not in allowed:
            raise ValueError(f"action must be one of {sorted(allowed)}")
        request = mgmt.lifecycle_request(action, _identifier(instance_id), timeout_us=timeout_us)
        if dry_run:
            request["header"]["dryRun"] = True
        return self._follow_job(self.session.management(request))

    def load_dapp(self, package_id: str, placement: str = "inProcess", backend: str = "cpu", dry_run: bool = True) -> dict:
        self._guard("load_dapp")
        request = mgmt.load_request(_identifier(package_id), placement, backend)
        if dry_run:
            request["header"]["dryRun"] = True
        return self._follow_job(self.session.management(request))

    def clear_incidents(self, through_incident_id: int) -> dict:
        self._guard("clear_incidents")
        return self.session.management({"header": mgmt.header(), "operation": (
            "incidents", {"action": "clearThrough", "afterIncidentId": int(through_incident_id).to_bytes(8, "big"),
                          "maximumRecords": 100})})

    def release_ue(self, rnti, serving_pci: Optional[int] = None, dry_run: bool = True) -> dict:
        self._guard("release_ue")
        r = _rnti(rnti)
        if serving_pci is None:
            with self.state.lock:
                ue = self.state.ues.get(r)
                serving_pci = (ue.mac.get("pci") if ue else None) or self.state.cell.get("pci")
        if serving_pci is None:
            raise ValueError("serving_pci unknown: pass it explicitly (no gNB metrics seen for this UE yet)")
        request = {"header": mgmt.header(None, None, dry_run=dry_run),
                   "operation": ("ranControl", {"action": "releaseUeToIdle", "servingPci": int(serving_pci), "rnti": r})}
        return self.session.management(request)

    def handover_ue(self, rnti, target_pci: int, serving_pci: Optional[int] = None, dry_run: bool = True) -> dict:
        self._guard("handover_ue")
        r = _rnti(rnti)
        if serving_pci is None:
            with self.state.lock:
                ue = self.state.ues.get(r)
                serving_pci = (ue.mac.get("pci") if ue else None) or self.state.cell.get("pci")
        if serving_pci is None:
            raise ValueError("serving_pci unknown: pass it explicitly")
        return self.session.management(mgmt.ran_control_request("handoverUe", int(serving_pci), r,
                                                                target_pci=int(target_pci), dry_run=dry_run))

    def set_rf_gain(self, direction: str, gain_db: float, port_id: int = 0, dry_run: bool = True) -> dict:
        self._guard("set_rf_gain")
        if direction not in ("tx", "rx"):
            raise ValueError("direction must be 'tx' or 'rx'")
        action = "setTxGain" if direction == "tx" else "setRxGain"
        return self.session.management(mgmt.ran_control_request(action, 0, 0, port_id=int(port_id),
                                                                gain_db=float(gain_db), dry_run=dry_run))

    def set_telemetry_streams(self, streams: List[str], interval_ms: Optional[int] = None) -> dict:
        self._guard("set_telemetry_streams")
        return {"subscribed": self.session.set_streams(tuple(streams), interval_ms)}


TOOL_SPECS: List[Tuple[str, str, dict, bool]] = [
    # (name, description, input schema, mutating)
    ("server_status", "E3 association state, agent identity, telemetry catalog and counters.", {"type": "object", "properties": {}}, False),
    ("get_cell_overview", "One-shot sector summary: active UEs, SINR/BLER spread, throughput, PRB usage, cell counters, DU latency.", {"type": "object", "properties": {}}, False),
    ("list_ues", "All observed UEs with PHY (SINR/EVM/TA/CFO/MCS), UL BLER over a window, SRS state and MAC/scheduler metrics.",
     {"type": "object", "properties": {"window_s": {"type": "number", "default": 10}}}, False),
    ("get_ue", "Everything known about one UE.", {"type": "object", "properties": {"rnti": {"type": "string", "description": "e.g. 0x4601"}}, "required": ["rnti"]}, False),
    ("get_kpis", "Cell and per-UE KPIs aggregated over a window (bitrates, BLER, PRB usage, HARQ lateness, error indications).",
     {"type": "object", "properties": {"window_s": {"type": "number", "default": 60}}}, False),
    ("get_traffic_history", "Bucketed time series (traffic patterns) for the cell or one UE: bitrates, UE count, PRB usage, SINR, MCS, CQI.",
     {"type": "object", "properties": {"window_s": {"type": "number", "default": 300}, "rnti": {"type": "string"},
                                       "keys": {"type": "array", "items": {"type": "string"}}, "buckets": {"type": "integer", "default": 60}}}, False),
    ("get_srs_channel", "Latest per-subcarrier SRS channel response (magnitude dB, phase) per Rx/Tx port pair for a UE.",
     {"type": "object", "properties": {"rnti": {"type": "string"}}, "required": ["rnti"]}, False),
    ("get_constellation", "Equalized PUSCH symbols (I/Q points) from the UE's last captured grant.",
     {"type": "object", "properties": {"rnti": {"type": "string"}, "max_points": {"type": "integer", "default": 512}}, "required": ["rnti"]}, False),
    ("get_telemetry_catalog", "Telemetry streams the agent offers (ids, floors) and what this server subscribes to.", {"type": "object", "properties": {}}, False),
    ("get_e3_connections", "Downstream E3 associations on the gNB (dashboards, observers, this server) and their subscriptions.", {"type": "object", "properties": {}}, False),
    ("get_dapp_inventory", "dApp packages and instances loaded in the gNB's lifecycle manager (ids, states, backends, placement).", {"type": "object", "properties": {}}, False),
    ("get_dapp_health", "Aggregate dApp health counters (deadline misses, fallbacks, breaker trips, device errors).", {"type": "object", "properties": {}}, False),
    ("get_dapp_metrics", "Metrics of one dApp instance (or their schema).",
     {"type": "object", "properties": {"instance_id": {"type": "string"}, "schema": {"type": "boolean", "default": False}}, "required": ["instance_id"]}, False),
    ("get_dapp_config", "Effective configuration of one dApp instance, or its schema (keys, kinds, ranges, defaults) with schema=true. Scheduler parameters, gains and other cell knobs live here.",
     {"type": "object", "properties": {"instance_id": {"type": "string"}, "schema": {"type": "boolean", "default": False}}, "required": ["instance_id"]}, False),
    ("list_incidents", "Recent dApp incidents (errors, exceptions, invalid input, deadline misses).",
     {"type": "object", "properties": {"instance_id": {"type": "string"}, "limit": {"type": "integer", "default": 50}}}, False),
    ("list_jobs", "Asynchronous management jobs (loads, lifecycle transitions, config sets).", {"type": "object", "properties": {}}, False),
    ("get_job", "One management job by id, with its final result code and detail.",
     {"type": "object", "properties": {"job_id": {"type": "string"}}, "required": ["job_id"]}, False),
    ("list_quiet_reservations", "Quiet-period reservations granted to sensing dApps.",
     {"type": "object", "properties": {"cell_id": {"type": "integer"}, "limit": {"type": "integer", "default": 50}}}, False),
    ("get_cell_info", "Served cells as the gNB knows them: PCI, PLMN/TAC, NCI, band, DL/UL ARFCN and centre frequency, bandwidth, PRBs, SCS, antennas, TDD pattern, PRACH config, RU gains, barred flag; plus which RAN controls this host can honour.",
     {"type": "object", "properties": {}}, False),
    ("list_data_subscriptions", "Portable-store subscriptions (who consumes which telemetry interface, queue depth, drops).", {"type": "object", "properties": {}}, False),
    ("set_dapp_config", "Change configuration entries of a dApp instance (e.g. scheduler target BLER, policy modes, back-off weights; equalizer or channel-emulator knobs). Kinds are taken from the instance schema; dry_run=true (default) validates only. The call follows the asynchronous job and returns its outcome ('ok', 'outcome', 'job'), and re-reads the effective values after a real write.",
     {"type": "object", "properties": {"instance_id": {"type": "string"}, "entries": {"type": "object", "additionalProperties": True},
                                       "boundary": {"type": "string", "enum": ["immediate", "invocation", "slot", "restart"], "default": "invocation"},
                                       "dry_run": {"type": "boolean", "default": True}, "expected_config_generation": {"type": "integer"},
                                       "kinds": {"type": "object", "additionalProperties": {"type": "string"}}},
      "required": ["instance_id", "entries"]}, True),
    ("dapp_lifecycle", "Drive a dApp instance: prepare, arm, enterShadow, activate, deactivate, retryQuiescence, unload, breakerProbe. dry_run=true (default) validates only; set dry_run=false to apply.",
     {"type": "object", "properties": {"instance_id": {"type": "string"}, "action": {"type": "string"}, "dry_run": {"type": "boolean", "default": True},
                                       "timeout_us": {"type": "integer", "default": 1000000}}, "required": ["instance_id", "action"]}, True),
    ("load_dapp", "Load a catalog package as a new instance. dry_run=true (default) validates only; set dry_run=false to apply.",
     {"type": "object", "properties": {"package_id": {"type": "string"}, "placement": {"type": "string", "enum": ["inProcess", "supervisedProcess"], "default": "inProcess"},
                                       "backend": {"type": "string", "enum": ["cpu", "cuda", "otherAccelerator"], "default": "cpu"}, "dry_run": {"type": "boolean", "default": True}},
      "required": ["package_id"]}, True),
    ("clear_incidents", "Acknowledge incidents up to and including an incident id.",
     {"type": "object", "properties": {"through_incident_id": {"type": "integer"}}, "required": ["through_incident_id"]}, True),
    ("release_ue", "Release a UE to RRC idle (RAN control through the CU-CP). dry_run=true (default) only validates the path; the serving PCI is taken from gNB metrics when omitted.",
     {"type": "object", "properties": {"rnti": {"type": "string"}, "serving_pci": {"type": "integer"}, "dry_run": {"type": "boolean", "default": True}}, "required": ["rnti"]}, True),
    ("handover_ue", "Hand a UE over to another cell (target PCI) through the CU-CP mobility handler. dry_run=true (default) validates only.",
     {"type": "object", "properties": {"rnti": {"type": "string"}, "target_pci": {"type": "integer"}, "serving_pci": {"type": "integer"},
                                       "dry_run": {"type": "boolean", "default": True}}, "required": ["rnti", "target_pci"]}, True),
    ("set_rf_gain", "Set the radio unit's TX or RX gain (dB) on a port; applied immediately when dry_run=false. Check get_cell_info capabilities first.",
     {"type": "object", "properties": {"direction": {"type": "string", "enum": ["tx", "rx"]}, "gain_db": {"type": "number"},
                                       "port_id": {"type": "integer", "default": 0}, "dry_run": {"type": "boolean", "default": True}},
      "required": ["direction", "gain_db"]}, True),
    ("set_telemetry_streams", "Change which telemetry streams this server subscribes to and the requested capture interval (0 = full rate).",
     {"type": "object", "properties": {"streams": {"type": "array", "items": {"type": "string"}}, "interval_ms": {"type": "integer"}}, "required": ["streams"]}, True),
]

RESOURCES = [
    ("ocudu://cell/overview", "Cell overview", "get_cell_overview"),
    ("ocudu://cell/ues", "Connected UEs", "list_ues"),
    ("ocudu://cell/kpis", "KPIs (60 s)", "get_kpis"),
    ("ocudu://dapps/inventory", "dApp inventory", "get_dapp_inventory"),
    ("ocudu://dapps/health", "dApp health", "get_dapp_health"),
    ("ocudu://e3/connections", "Downstream E3 connections", "get_e3_connections"),
    ("ocudu://e3/catalog", "Telemetry catalog", "get_telemetry_catalog"),
    ("ocudu://cell/info", "Cell configuration", "get_cell_info"),
]

PROMPTS = {
    "triage_cell": ("Assess the sector's health and explain what to look at next.",
                    "Use get_cell_overview, list_ues, get_kpis and get_dapp_health. Flag UEs with UL BLER above 10% or SINR "
                    "below 5 dB, throughput collapses in get_traffic_history, late HARQs or error indications, and any dApp "
                    "incidents. Explain each finding with the numbers you saw and suggest one concrete next step for each."),
    "tune_dapp": ("Find and adjust a dApp configuration knob safely.",
                  "Use get_dapp_inventory to find the instance, get_dapp_config with schema=true to see the keys, kinds and "
                  "ranges, then propose a change and run set_dapp_config with dry_run=true first. Only apply with "
                  "dry_run=false after showing the user the before/after values and the expected effect."),
}


# ---------------------------------------------------------------------------
# Minimal MCP (JSON-RPC 2.0 over stdio)
# ---------------------------------------------------------------------------


def _json_default(value):
    if isinstance(value, (np.integer,)):
        return int(value)
    if isinstance(value, (np.floating,)):
        return None if not math.isfinite(float(value)) else float(value)
    if isinstance(value, np.ndarray):
        return value.tolist()
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return str(value)


class McpServer:
    def __init__(self, tools: Tools, read_only: bool) -> None:
        self.tools = tools
        self.read_only = read_only
        self.specs = [spec for spec in TOOL_SPECS if not (read_only and spec[3])]
        self.protocol_version = PROTOCOL_VERSIONS[0]

    # -- dispatch ---------------------------------------------------------
    def handle(self, message: dict) -> Optional[dict]:
        method = message.get("method")
        msg_id = message.get("id")
        params = message.get("params") or {}
        if method is None:
            return None
        try:
            if method == "initialize":
                requested = params.get("protocolVersion")
                self.protocol_version = requested if requested in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
                result = {"protocolVersion": self.protocol_version,
                          "capabilities": {"tools": {"listChanged": False}, "resources": {"subscribe": False, "listChanged": False},
                                           "prompts": {"listChanged": False}},
                          "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                          "instructions": ("OCUDU gNB companion over E3. Observation tools read a live sector model; "
                                           "act tools mutate the gNB through E3 management and default to dry_run where "
                                           "supported. Read-only mode: %s." % self.read_only)}
            elif method in ("notifications/initialized", "notifications/cancelled", "notifications/roots/list_changed"):
                return None
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": [{"name": n, "description": d, "inputSchema": s,
                                     "annotations": {"readOnlyHint": not m, "destructiveHint": m and n in ("release_ue", "handover_ue", "set_rf_gain", "dapp_lifecycle"),
                                                     "openWorldHint": False}} for n, d, s, m in self.specs]}
            elif method == "tools/call":
                result = self._call_tool(params.get("name"), params.get("arguments") or {})
            elif method == "resources/list":
                result = {"resources": [{"uri": u, "name": n, "mimeType": "application/json"} for u, n, _ in RESOURCES]}
            elif method == "resources/read":
                uri = params.get("uri")
                match = [r for r in RESOURCES if r[0] == uri]
                if not match:
                    raise KeyError(f"unknown resource {uri}")
                payload = getattr(self.tools, match[0][2])()
                result = {"contents": [{"uri": uri, "mimeType": "application/json", "text": json.dumps(payload, default=_json_default)}]}
            elif method == "prompts/list":
                result = {"prompts": [{"name": n, "description": d} for n, (d, _) in PROMPTS.items()]}
            elif method == "prompts/get":
                name = params.get("name")
                if name not in PROMPTS:
                    raise KeyError(f"unknown prompt {name}")
                result = {"description": PROMPTS[name][0],
                          "messages": [{"role": "user", "content": {"type": "text", "text": PROMPTS[name][1]}}]}
            else:
                if msg_id is None:
                    return None
                return {"jsonrpc": "2.0", "id": msg_id, "error": {"code": -32601, "message": f"method not found: {method}"}}
        except Exception as exc:  # noqa: BLE001
            if msg_id is None:
                return None
            return {"jsonrpc": "2.0", "id": msg_id, "error": {"code": -32602, "message": str(exc)}}
        if msg_id is None:
            return None
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}

    def _call_tool(self, name: Optional[str], arguments: dict) -> dict:
        spec = next((s for s in self.specs if s[0] == name), None)
        if spec is None:
            raise KeyError(f"unknown tool {name}")
        method = getattr(self.tools, name)
        try:
            payload = method(**arguments)
            return {"content": [{"type": "text", "text": json.dumps(payload, default=_json_default, indent=1)}],
                    "structuredContent": json.loads(json.dumps(payload, default=_json_default)) if isinstance(payload, dict) else None,
                    "isError": False}
        except Exception as exc:  # noqa: BLE001
            return {"content": [{"type": "text", "text": f"{type(exc).__name__}: {exc}"}], "isError": True}

    # -- stdio loop ---------------------------------------------------------
    def serve_stdio(self) -> None:
        out = sys.stdout
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                out.write(json.dumps({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}}) + "\n")
                out.flush()
                continue
            messages = message if isinstance(message, list) else [message]
            for item in messages:
                response = self.handle(item)
                if response is not None:
                    out.write(json.dumps(response, default=_json_default) + "\n")
                    out.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=os.environ.get("OCUDU_E3_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("OCUDU_E3_PORT", "36423")))
    parser.add_argument("--dapp-name", default="ocudu-mcp")
    parser.add_argument("--streams", default=",".join(DEFAULT_STREAMS), help="telemetry streams to subscribe to")
    parser.add_argument("--interval-ms", type=int, default=None, help="requested capture interval for all streams")
    parser.add_argument("--read-only", action="store_true", help="expose observation tools only")
    parser.add_argument("--log-file", default=os.environ.get("OCUDU_MCP_LOG"), help="log to this file (stdout is the MCP channel)")
    args = parser.parse_args()
    logging.basicConfig(filename=args.log_file, level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s") if args.log_file \
        else logging.basicConfig(stream=sys.stderr, level=logging.WARNING)

    state = SectorState()
    session = E3Session(args.host, args.port, args.dapp_name, tuple(s for s in args.streams.split(",") if s), state)
    session.requested_interval_ms = args.interval_ms
    session.start()
    server = McpServer(Tools(session, state, args.read_only), args.read_only)
    LOG.info("ocudu MCP server on stdio; E3AP %s:%s read_only=%s", args.host, args.port, args.read_only)
    try:
        server.serve_stdio()
    except KeyboardInterrupt:
        pass
    finally:
        session.stop_event.set()
    return 0


if __name__ == "__main__":
    sys.exit(main())
