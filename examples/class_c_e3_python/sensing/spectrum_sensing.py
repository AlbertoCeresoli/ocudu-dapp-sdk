# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""PyTorch spectrum radiometer (Class-C spectrum interface demo).

This module is **pure sensing** — no DU transport. Live grids arrive as torch
tensors from ``torch_bridge.publication_to_torch`` (supervised process) or from
the portable-E3 extension.

Contract
--------
* Input: complex grid shaped ``[rx_ports, symbols, subcarriers]`` (complex64).
* Output: ``SensingResult`` (JSON-serializable PRB occupancy summary).

The algorithm is intentionally simple (PRB-average |z|²). Replace ``radiometer``
with your product model; keep the tensor contract.

Research / education only — not a production radiometer. Prefer native
``ref_spectrumd_*`` for product deployments.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any, Mapping

import torch


@dataclass(frozen=True)
class SensingResult:
    """Compact spectrum sensing report (safe to JSON-log)."""

    backend: str
    samples: int
    mean_power: float
    peak_power: float
    occupied_fraction: float
    nof_prbs: int
    nof_rx: int
    nof_symbols: int
    nof_subcarriers: int
    amplitude_scale: float
    device: str

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


def select_device(requested: str | None = None) -> torch.device:
    """Resolve compute device: explicit request, else CUDA if available, else CPU."""
    if requested is not None:
        dev = torch.device(requested)
        if dev.type == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA was requested but torch.cuda.is_available() is False")
        return dev
    if torch.cuda.is_available():
        return torch.device("cuda")
    return torch.device("cpu")


def _as_complex_tensor(grid: Any, device: torch.device) -> torch.Tensor:
    """Coerce input to complex64 on ``device`` (torch path preferred)."""
    if isinstance(grid, torch.Tensor):
        t = grid
    else:
        # Offline / notebook convenience only — live process path always passes torch.
        import numpy as np

        arr = np.asarray(grid)
        if np.iscomplexobj(arr):
            t = torch.from_numpy(np.ascontiguousarray(arr.astype(np.complex64, copy=False)))
        elif arr.dtype == np.float32 and arr.shape[-1] == 2:
            c = arr.view(np.complex64)[..., 0]
            t = torch.from_numpy(np.ascontiguousarray(c))
        else:
            raise TypeError("grid must be complex or last-dim-2 float32 interleaved")
    if not torch.is_complex(t):
        raise TypeError("grid tensor must be complex")
    return t.to(device=device, dtype=torch.complex64)


def radiometer(
    grid: Any,
    *,
    amplitude_scale: float = 1.0,
    subcarriers_per_prb: int = 12,
    power_threshold: float = 1.0,
    device: str | None = None,
) -> SensingResult:
    """PRB-average radiometer on a complex UL resource grid (PyTorch).

    Parameters
    ----------
    grid:
        Complex array-like or tensor ``[rx_ports, symbols, subcarriers]``.
    amplitude_scale:
        Host publication scale (Class-C spectrum metadata).
    subcarriers_per_prb:
        NR default 12.
    power_threshold:
        Linear power threshold on PRB-averaged |z|² for occupancy fraction.
    device:
        Optional torch device (``\"cuda\"``, ``\"cpu\"``, …). Default: CUDA if
        available else CPU. Supervised process workers often force CPU for
        predictable heartbeat latency under dense UL load.
    """
    dev = select_device(device)
    t = _as_complex_tensor(grid, dev)
    if t.ndim != 3:
        raise ValueError("expected grid shape [rx, symbol, subcarrier]")
    rx, symbols, sc = t.shape
    if int(sc) % subcarriers_per_prb != 0:
        raise ValueError(f"subcarriers ({int(sc)}) must be divisible by {subcarriers_per_prb}")

    scale = float(amplitude_scale)
    z = t * scale
    power = z.real.float().square() + z.imag.float().square()
    samples = int(power.numel())
    mean_power = float(power.mean().item()) if samples else 0.0
    peak_power = float(power.max().item()) if samples else 0.0
    nof_prbs = int(sc) // subcarriers_per_prb
    prb = power.reshape(rx, symbols, nof_prbs, subcarriers_per_prb).mean(dim=(0, 1, 3))
    occupied = float((prb >= power_threshold).float().mean().item()) if nof_prbs else 0.0

    return SensingResult(
        backend="torch",
        samples=samples,
        mean_power=mean_power,
        peak_power=peak_power,
        occupied_fraction=occupied,
        nof_prbs=nof_prbs,
        nof_rx=int(rx),
        nof_symbols=int(symbols),
        nof_subcarriers=int(sc),
        amplitude_scale=scale,
        device=str(dev),
    )


def synthetic_spectrum_grid(
    *,
    rx: int = 2,
    symbols: int = 14,
    prbs: int = 25,
    subcarriers_per_prb: int = 12,
    occupied_prbs: tuple[int, ...] = (3, 4, 5, 10),
    noise_sigma: float = 0.1,
    signal_amp: float = 2.0,
    seed: int = 0,
) -> torch.Tensor:
    """Build a complex64 grid for offline demos and unit tests."""
    gen = torch.Generator(device="cpu")
    gen.manual_seed(seed)
    sc = prbs * subcarriers_per_prb
    real = torch.randn(rx, symbols, sc, generator=gen, dtype=torch.float32)
    imag = torch.randn(rx, symbols, sc, generator=gen, dtype=torch.float32)
    grid = (real + 1j * imag) * (noise_sigma / (2.0**0.5))
    grid = grid.to(torch.complex64)
    for p in occupied_prbs:
        if 0 <= p < prbs:
            lo = p * subcarriers_per_prb
            hi = lo + subcarriers_per_prb
            phase = torch.rand(rx, symbols, subcarriers_per_prb, generator=gen) * (2.0 * torch.pi)
            tone = signal_amp * torch.exp(1j * phase.to(torch.complex64))
            grid[:, :, lo:hi] = grid[:, :, lo:hi] + tone
    return grid


def result_from_mapping(m: Mapping[str, Any]) -> SensingResult:
    """Rebuild a ``SensingResult`` from a JSON/dict payload (tests / tools)."""
    return SensingResult(**{k: m[k] for k in SensingResult.__dataclass_fields__})
