# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""PyTorch CIR estimate from UE SRS UL channel estimates.

All sensing math is **torch** (IDFT-style early-delay power profile, peak pick,
noise correction). This is a **placeholder** for Class-C SRS / UE-TRP style
sensing — not tracking, association, or full ISAC.

Contract
--------
* Input: complex tensor ``[..., frequency]`` (typically ``[rx, tx, frequency]``).
* Output: ``CirResult`` with up to 64 early-delay taps (process result ABI bound).

Replace ``estimate_cir`` with your product model; keep the tensor + result shape
compatible with ``send_srs_cir_result`` if you return results to the host.

Research / education only — not a production ISAC pipeline. Prefer native
``ref_srs_isacd_*`` for product deployments.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

import torch

from sensing.spectrum_sensing import select_device

MAX_CIR_TAPS = 64


@dataclass
class CirResult:
    """CIR report. Sensing is torch; ``tap_power`` list is for JSON logging only."""

    backend: str
    device: str
    tap_count: int
    peak_delay_bin: int
    mean_channel_power: float
    noise_corrected_power: float
    peak_cir_power: float
    noise_variance: float
    tap_power: list[float]
    #: CPU float32 tensor for process ABI send (same values as ``tap_power``).
    tap_power_tensor: torch.Tensor | None = None

    def as_dict(self) -> dict[str, Any]:
        return {
            "backend": self.backend,
            "device": self.device,
            "tap_count": self.tap_count,
            "peak_delay_bin": self.peak_delay_bin,
            "mean_channel_power": self.mean_channel_power,
            "noise_corrected_power": self.noise_corrected_power,
            "peak_cir_power": self.peak_cir_power,
            "noise_variance": self.noise_variance,
            "tap_power": self.tap_power,
        }


def estimate_cir(
    channel: Any,
    *,
    noise_variance: float = 0.0,
    max_taps: int = MAX_CIR_TAPS,
    device: str | None = None,
) -> CirResult:
    """IDFT-style CIR power along the last (frequency) axis — pure PyTorch.

    Parameters
    ----------
    channel:
        Complex tensor ``[..., frequency]`` (e.g. ``[rx, tx, frequency]``).
    noise_variance:
        Linear noise power from SRS metadata.
    max_taps:
        Cap early-delay bins (≤ 64 for the process CIR preview ABI).
    device:
        Torch device. Supervised workers often force CPU for latency predictability.
    """
    dev = select_device(device)
    h = channel if isinstance(channel, torch.Tensor) else torch.as_tensor(channel)
    if not torch.is_complex(h):
        raise TypeError("channel must be a complex torch tensor")
    h = h.to(device=dev, dtype=torch.complex64, non_blocking=False)
    if h.ndim < 1:
        raise ValueError("channel rank must be >= 1")

    n_freq = int(h.shape[-1])
    taps = min(n_freq, int(max_taps), MAX_CIR_TAPS)
    rows = int(h.numel() // n_freq)
    h2 = h.reshape(rows, n_freq)

    # cir[b, t] = mean_f  h[b, f] * exp(+j 2π t f / N)
    f = torch.arange(n_freq, device=dev, dtype=torch.float32)
    t = torch.arange(taps, device=dev, dtype=torch.float32)
    angle = 2.0 * torch.pi * t[:, None] * f[None, :] / float(n_freq)
    basis = torch.exp(torch.complex(torch.zeros_like(angle), angle))
    cir = (h2[:, None, :] * basis[None, :, :]).mean(dim=-1)
    tap_power = torch.clamp(cir.real.square() + cir.imag.square(), min=0.0).mean(dim=0)

    sample_power = h2.real.square() + h2.imag.square()
    mean_ch = torch.clamp(sample_power.mean(), min=0.0)
    peak_bin = int(torch.argmax(tap_power).item())
    # Host ABI requires every tap_power[i] <= peak_cir_power.
    peak_p = torch.maximum(tap_power[peak_bin], tap_power.max())
    nv = max(0.0, float(noise_variance))
    noise_corr = torch.clamp(mean_ch - nv, min=0.0)

    taps_cpu = tap_power.detach().to(dtype=torch.float32, device="cpu")
    return CirResult(
        backend="torch",
        device=str(dev),
        tap_count=taps,
        peak_delay_bin=peak_bin,
        mean_channel_power=float(mean_ch.item()),
        noise_corrected_power=float(noise_corr.item()),
        peak_cir_power=float(peak_p.item()),
        noise_variance=nv,
        tap_power=taps_cpu.tolist(),
        tap_power_tensor=taps_cpu,
    )


def synthetic_srs_channel(
    *,
    rx: int = 2,
    tx: int = 1,
    frequency_points: int = 48,
    delay_bin: int = 3,
    snr_db: float = 20.0,
    seed: int = 0,
) -> torch.Tensor:
    """Synthetic ``[rx, tx, frequency]`` complex64 channel with one delay path."""
    gen = torch.Generator(device="cpu")
    gen.manual_seed(seed)
    f = torch.arange(frequency_points, dtype=torch.float32)
    phase = 2.0 * torch.pi * float(delay_bin) * f / float(frequency_points)
    tone = torch.exp(torch.complex(torch.zeros_like(phase), -phase))
    amp = 10.0 ** (snr_db / 20.0)
    noise = (
        torch.randn(rx, tx, frequency_points, generator=gen)
        + 1j * torch.randn(rx, tx, frequency_points, generator=gen)
    ) * (0.1 / (2.0**0.5))
    return amp * tone.view(1, 1, -1) + noise.to(torch.complex64)
