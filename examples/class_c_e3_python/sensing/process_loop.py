# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Shared supervised process loop for spectrum + SRS Class-C Python dApps.

This is the heart of the **catalog** (``out_of_process``) path:

1. Construct ``ProcessWorker`` first (``PR_SET_NO_NEW_PRIVS`` + route admission).
2. ``wait()`` on the control/notification fds (heartbeats are serviced here).
3. Drain publications; run PyTorch sensing; optionally send results.

Why this exists
---------------
Python cannot implement Class-A/B (or Class-C) **in-process** MODULEs. Only a
**supervised Class-C process** (or an independent portable-E3 client) can host
a Python runtime. The DU ``posix_spawn``s a small catalog ELF which ``execve``s
this loop with a control socket.

Host / product caveats
----------------------
* **Research / education only.** Python + PyTorch process dApps are excellent
  for prototyping Class-C data paths, but they are **not** appropriate for
  production RAN performance (GIL, interpreter overhead, densify copies,
  heartbeat coupling). Production Class-C sensing should be native C++/CUDA
  packages (see ``ref_spectrumd_*`` / ``ref_srs_isacd_*``).
* Heartbeats are answered only while ``ProcessWorker.wait()`` runs. Long work
  between ``wait()`` calls risks host SIGKILL on heartbeat timeout.
* Never flood stdout with high-rate JSON — the DU may attach a non-drained pipe.
  Lifecycle lines go to stdout; publications go to the log file by default.
* Spectrum results are **observer-only** unless ``OCUDU_DAPP_SPECTRUM_SEND_RESULTS=1``
  (host result admission is strict; consecutive rejects can kill the worker).
* Results **must** use the publication's ``route_index`` (not a hard-coded 0).
"""

from __future__ import annotations

import json
import os
import sys
from collections.abc import Callable
from typing import Any, TextIO

import numpy as np
import torch

from sensing.spectrum_sensing import SensingResult, radiometer, select_device
from sensing.srs_cir import CirResult, estimate_cir
from sensing.torch_bridge import publication_to_torch

_TRUE = frozenset({"1", "true", "yes", "on"})
LogFn = Callable[..., None]


def _env_flag(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in _TRUE


def _emit(log_file: TextIO | None, payload: dict[str, Any], *, to_stdout: bool) -> None:
    line = json.dumps(payload, separators=(",", ":"))
    if to_stdout:
        try:
            sys.stdout.write(line + "\n")
            sys.stdout.flush()
        except Exception:
            pass
    if log_file is not None:
        try:
            log_file.write(line + "\n")
            log_file.flush()
        except Exception:
            pass


def _resolve_observer_only(role: str, observer_only: bool) -> bool:
    if _env_flag("OCUDU_DAPP_OBSERVER_ONLY"):
        return True
    # Spectrum: default observer-only (sensing + JSONL). Opt-in host results:
    #   OCUDU_DAPP_SPECTRUM_SEND_RESULTS=1
    if role == "spectrum" and not _env_flag("OCUDU_DAPP_SPECTRUM_SEND_RESULTS"):
        return True
    return observer_only


def _should_log_publication(seen: int, log_every: int) -> bool:
    return seen < 5 or (seen % log_every) == 0


def run_process_worker(
    *,
    role: str,
    control_fd: int,
    interface_id: int,
    timeout_ms: int = 5000,
    wait_ms: int = 50,
    device: str | None = None,
    threshold: float = 1.0,
    max_publications: int = 0,
    observer_only: bool = False,
    hold_lease: bool = False,
    json_log: bool = False,
    log_path: str | None = None,
) -> int:
    """Run the supervised Class-C worker loop.

    Parameters
    ----------
    role:
        ``\"spectrum\"`` or ``\"srs_isac\"``.
    control_fd:
        Control socket fd from the DU (``--control-fd=N``).
    interface_id:
        ``0x00010001`` (spectrum) or ``0x00010002`` (SRS).
    timeout_ms:
        Route admission timeout for ``ProcessWorker`` construction.
    wait_ms:
        ``ProcessWorker.wait()`` poll period; heartbeats are serviced here.
    device:
        Torch device string (``\"cpu\"``, ``\"cuda\"``, …). Launcher defaults to CPU.
    threshold:
        Spectrum PRB occupancy power threshold (linear).
    max_publications:
        Stop after N publications; ``0`` means run until control closes.
    observer_only:
        If True, never send results. Spectrum also defaults observer-only via env
        policy unless ``OCUDU_DAPP_SPECTRUM_SEND_RESULTS=1``.
    hold_lease:
        Reserved for zero-copy experiments. The loop always uses ``copy=True``
        so the ring can advance during torch work.
    json_log:
        Mirror lifecycle events to stdout (still do not flood publications).
    log_path:
        Append JSON lines (publications + lifecycle) to this file.
    """
    del hold_lease  # reserved; loop always copies so the ring can advance

    try:
        import ocudu_dapp_class_c_process as proc
    except ImportError as error:
        print(
            "FAIL: ocudu_dapp_class_c_process not found. "
            "Build with OCUDU_DAPP_BUILD_PYTHON_CLASS_C=ON; launcher must set PYTHONPATH.",
            flush=True,
        )
        print(error, flush=True)
        return 2

    if role not in ("spectrum", "srs_isac"):
        print(f"unknown role {role}", flush=True)
        return 2

    observer_only = _resolve_observer_only(role, observer_only)
    compute = select_device(device)

    # ProcessWorker first: no_new_privs + route admission (host will not wait forever).
    try:
        worker = proc.ProcessWorker(control_fd, interface_id, timeout_ms)
    except Exception as error:
        print(json.dumps({"event": "fatal", "error": f"ProcessWorker: {error}"}), flush=True)
        return 3

    log_file: TextIO | None = None
    if log_path:
        try:
            log_file = open(log_path, "a", encoding="utf-8")  # noqa: SIM115
        except OSError as error:
            print(
                json.dumps({"event": "warn", "error": f"log open failed: {error}", "path": log_path}),
                flush=True,
            )
            log_path = None

    def log_event(payload: dict[str, Any], *, important: bool = False) -> None:
        lifecycle = payload.get("event") in ("started", "fatal", "error", "warn")
        _emit(log_file, payload, to_stdout=important or (json_log and lifecycle))

    log_event(
        {
            "event": "started",
            "role": role,
            "routes": worker.route_count(),
            "cuda_process": bool(proc.CUDA_PROCESS_ENABLED),
            "torch": torch.__version__,
            "compute_device": str(compute),
            "log_path": log_path,
            "observer_only": observer_only,
            "research_only": True,
            "placement": "out_of_process",
            "dapp_class": "C",
        },
        important=True,
    )

    seen = 0
    log_every = 50 if role == "spectrum" else 10
    try:
        while max_publications <= 0 or seen < max_publications:
            try:
                if not worker.control_healthy():
                    log_event({"event": "fatal", "error": "control_unhealthy"}, important=True)
                    return 5
                if not worker.wait(wait_ms):
                    continue
            except BaseException as wait_err:
                log_event(
                    {"event": "fatal", "error": f"wait/control: {type(wait_err).__name__}: {wait_err}"},
                    important=True,
                )
                return 5

            drained = 0
            while max_publications <= 0 or seen < max_publications:
                try:
                    handle = worker.try_acquire()
                except BaseException as acq_err:
                    log_event(
                        {"event": "fatal", "error": f"try_acquire: {type(acq_err).__name__}: {acq_err}"},
                        important=True,
                    )
                    return 5
                if handle is None:
                    break

                try:
                    route_index = int(handle.route_index())
                except Exception:
                    route_index = 0

                meta = None
                try:
                    meta = handle.meta()
                    if meta.kind == "ul_resource_plan":
                        continue
                    if role == "spectrum" and meta.kind != "spectrum":
                        continue
                    if role == "srs_isac" and meta.kind != "srs_isac":
                        continue

                    # Owned torch tensor; lease released so the SHMEM ring can advance.
                    grid, meta = publication_to_torch(handle, copy=True)
                    if str(grid.device) != str(compute):
                        grid = grid.to(compute, non_blocking=False)

                    if role == "spectrum":
                        result = radiometer(
                            grid,
                            amplitude_scale=meta.amplitude_scale,
                            power_threshold=threshold,
                            device=str(compute),
                        )
                        if _should_log_publication(seen, log_every):
                            log_event(
                                {
                                    "event": "publication",
                                    "role": "spectrum",
                                    "sequence": meta.publication_sequence,
                                    "cell_id": meta.cell_id,
                                    "route_index": route_index,
                                    "memory": meta.memory,
                                    "shape": list(meta.shape),
                                    "tensor_device": str(grid.device),
                                    "sfn": meta.sfn,
                                    "slot": meta.slot,
                                    "result": result.as_dict(),
                                }
                            )
                        if not observer_only and result.nof_prbs > 0:
                            _send_spectrum(
                                worker,
                                log_event,
                                route_index=route_index,
                                meta=meta,
                                result=result,
                                log_every=log_every,
                                seen=seen,
                            )
                    else:
                        cir = estimate_cir(
                            grid,
                            noise_variance=meta.noise_variance,
                            device=str(compute),
                        )
                        if _should_log_publication(seen, log_every):
                            summary = {k: v for k, v in cir.as_dict().items() if k != "tap_power"}
                            summary["tap_peak"] = float(max(cir.tap_power) if cir.tap_power else 0.0)
                            log_event(
                                {
                                    "event": "publication",
                                    "role": "srs_isac",
                                    "sequence": meta.publication_sequence,
                                    "cell_id": meta.cell_id,
                                    "route_index": route_index,
                                    "srs_resource_id": meta.srs_resource_id,
                                    "srs_config_id": meta.srs_config_id,
                                    "memory": meta.memory,
                                    "shape": list(meta.shape),
                                    "tensor_device": str(grid.device),
                                    "sfn": meta.sfn,
                                    "slot": meta.slot,
                                    "result": summary,
                                }
                            )
                        if not observer_only:
                            _send_srs_cir(
                                worker,
                                log_event,
                                route_index=route_index,
                                meta=meta,
                                cir=cir,
                                log_every=log_every,
                                seen=seen,
                            )
                    seen += 1
                    drained += 1
                except Exception as error:
                    log_event(
                        {
                            "event": "error",
                            "error": str(error),
                            "kind": getattr(meta, "kind", None) if meta is not None else None,
                            "shape": list(getattr(meta, "shape", []) or []) if meta is not None else None,
                            "sequence": getattr(meta, "publication_sequence", None) if meta is not None else None,
                        },
                        important=True,
                    )
                finally:
                    try:
                        handle.release()
                    except Exception:
                        pass

                # Yield to wait()/heartbeat after a small batch.
                if drained >= 8:
                    break
    finally:
        if log_file is not None:
            log_file.close()
    return 0


def _send_spectrum(
    worker: Any,
    log_event: LogFn,
    *,
    route_index: int,
    meta: Any,
    result: SensingResult,
    log_every: int,
    seen: int,
) -> None:
    """Return spectrum map to host. Must use the publication's route_index."""
    prb = np.full(
        (result.nof_prbs,),
        255 if result.occupied_fraction > 0.1 else 0,
        dtype=np.uint8,
    )
    try:
        ok = worker.send_spectrum_result(
            route_index,
            meta.publication_sequence,
            meta.cell_id,
            result.nof_prbs,
            prb,
            sfn=int(meta.sfn),
            slot=int(meta.slot),
            numerology=int(meta.numerology),
        )
        if not ok and _should_log_publication(seen, log_every):
            log_event(
                {
                    "event": "warn",
                    "warn": "send_spectrum_result returned false",
                    "sequence": meta.publication_sequence,
                    "route_index": route_index,
                }
            )
    except Exception as send_err:
        if _should_log_publication(seen, log_every):
            log_event(
                {
                    "event": "warn",
                    "warn": f"send_spectrum_result: {send_err}",
                    "sequence": meta.publication_sequence,
                    "route_index": route_index,
                }
            )


def _send_srs_cir(
    worker: Any,
    log_event: LogFn,
    *,
    route_index: int,
    meta: Any,
    cir: CirResult,
    log_every: int,
    seen: int,
) -> None:
    """Return CIR preview to host. Sensing is torch; ABI needs a float32 buffer."""
    if cir.tap_power_tensor is not None:
        taps = cir.tap_power_tensor.detach().cpu().numpy()
    else:
        taps = np.asarray(cir.tap_power, dtype=np.float32)
    try:
        ok = worker.send_srs_cir_result(
            route_index,
            meta.publication_sequence,
            meta.cell_id,
            meta.srs_resource_id,
            meta.srs_config_id,
            cir.peak_delay_bin,
            cir.mean_channel_power,
            cir.noise_corrected_power,
            cir.peak_cir_power,
            cir.noise_variance,
            taps,
            sfn=int(meta.sfn),
            slot=int(meta.slot),
            numerology=int(meta.numerology),
        )
        if not ok and _should_log_publication(seen, log_every):
            log_event(
                {
                    "event": "warn",
                    "warn": "send_srs_cir_result returned false",
                    "sequence": meta.publication_sequence,
                    "route_index": route_index,
                }
            )
    except Exception as send_err:
        if _should_log_publication(seen, log_every):
            log_event(
                {
                    "event": "warn",
                    "warn": f"send_srs_cir_result: {send_err}",
                    "sequence": meta.publication_sequence,
                    "route_index": route_index,
                }
            )
