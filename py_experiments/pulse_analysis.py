"""Прямоугольные импульсы: diff → period, pulse, duty, freq.

  d = diff(signal)
  максимум = пик |d| > edge_frac·max(|d|), где d > 0  (фронт вверх)
  минимум  = пик |d| > edge_frac·max(|d|), где d < 0  (фронт вниз)
  period   = расстояние между соседними максимумами
  pulse    = от максимума до первого d < edge_frac·min(d) до следующего максимума
  duty     = pulse / period
  freq     = 1 / period
"""

from __future__ import annotations

import configparser
import zipfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np


def read_sigrok_samplerate(zip_path: Path | str, device: int = 1) -> int:
    with zipfile.ZipFile(zip_path) as zf:
        cfg = configparser.ConfigParser()
        cfg.read_string(zf.read("metadata").decode())
    val, unit = cfg[f"device {device}"]["samplerate"].strip().split()
    scale = {"Hz": 1, "kHz": 1_000, "MHz": 1_000_000, "GHz": 1_000_000_000}
    return int(float(val) * scale[unit])


def read_sigrok_analog(zip_path: Path | str, device: int = 1, channel: int = 1) -> np.ndarray:
    prefix = f"analog-{device}-{channel}-"
    with zipfile.ZipFile(zip_path) as zf:
        names = sorted(
            (n for n in zf.namelist() if n.startswith(prefix)),
            key=lambda n: int(n.rsplit("-", 1)[1]),
        )
        raw = b"".join(zf.read(n) for n in names)
    return np.frombuffer(raw, dtype="<f4")


@dataclass
class PulseSeries:
    maxima: np.ndarray     # индексы максимумов diff (фронт вверх)
    period: np.ndarray     # сек, между соседними максимумами
    period_n: np.ndarray   # сэмплы diff, между соседними максимумами
    pulse: np.ndarray      # сек, max → min
    duty: np.ndarray
    freq: np.ndarray       # Гц
    t_center: np.ndarray   # сек
    steady: np.ndarray


def _edge_peaks(d: np.ndarray, edge_frac: float) -> np.ndarray:
    ad = np.abs(d)
    thr = edge_frac * ad.max()
    idx = np.flatnonzero(ad > thr)
    if idx.size == 0:
        return np.array([], dtype=int)

    split = np.where(np.diff(idx) > 1)[0]
    starts = np.concatenate([[0], split + 1])
    ends = np.concatenate([split + 1, [idx.size]])
    return np.array([idx[a:b][np.argmax(ad[idx[a:b]])] for a, b in zip(starts, ends)])


def _pulse_widths(
    d: np.ndarray, maxima: np.ndarray, thr_fall: float, fs: int
) -> np.ndarray:
    n = len(maxima) - 1
    pulse = np.full(n, np.nan)
    for i, p in enumerate(maxima[:-1]):
        seg = d[p : maxima[i + 1]]
        hit = np.flatnonzero(seg < thr_fall)
        if hit.size:
            pulse[i] = hit[0] / fs
    return pulse


def analyze_rect_pulses(
    signal: np.ndarray,
    fs: int,
    *,
    edge_frac: float = 0.9,
    trim_frac: float = 0.01,
) -> PulseSeries:
    empty = PulseSeries(
        maxima=np.array([], dtype=int),
        period=np.array([]),
        period_n=np.array([], dtype=int),
        pulse=np.array([]),
        duty=np.array([]),
        freq=np.array([]),
        t_center=np.array([]),
        steady=np.array([], dtype=bool),
    )

    n = len(signal)
    trim = int(n * trim_frac)
    if n < 2 * trim + 4:
        return empty

    x = signal[trim : n - trim]
    t0 = trim / fs

    d = np.diff(x)
    thr_fall = edge_frac * d.min()
    peaks = _edge_peaks(d, edge_frac)
    maxima = peaks[d[peaks] > 0]
    if len(maxima) < 2:
        return empty

    period_n = np.diff(maxima)
    period = period_n / fs
    pulse = _pulse_widths(d, maxima, thr_fall, fs)

    duty = pulse / period
    freq = 1.0 / period
    t_center = (maxima[:-1] + maxima[1:]) / 2 / fs + t0

    med = np.median(period)
    mad = np.median(np.abs(period - med))
    if mad > 0:
        steady = np.abs(period - med) <= 3 * 1.4826 * mad
    else:
        steady = np.ones(len(period), dtype=bool)

    return PulseSeries(maxima, period, period_n, pulse, duty, freq, t_center, steady)
