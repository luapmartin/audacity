#!/usr/bin/env python3
"""
Analyze a playback_jitter_*.log file produced by PlaybackJitterLogger and
render plots for the 16 ms QTimer inter-arrival distribution, the
correlation with transport events, and the wall-vs-DAC clock drift.

Setup (one-time):
    python3 -m venv tools/perflog/.venv
    tools/perflog/.venv/bin/pip install -r tools/perflog/requirements.txt

Run:
    tools/perflog/.venv/bin/python tools/perflog/analyze_jitter.py \
        [--log PATH] [--out-dir DIR] [--bins 120]

If --log is omitted, the most recent playback_jitter_*.log under the
platform's user-app-data perf_logs/ directory is picked automatically.
"""

import argparse
import csv
import glob
import os
import sys
from dataclasses import dataclass, field
from typing import List, Optional

import matplotlib.pyplot as plt
import numpy as np


TARGET_MS = 16.0
POSITION_UPDATE = "POSITION_UPDATE"
EVENT_COLORS = {
    "STATE_RUNNING":        "#2ca02c",
    "STATE_PAUSED":         "#ff7f0e",
    "STATE_STOPPED":        "#d62728",
    "TIMER_STOP_INACTIVE":  "#9467bd",
    "TIMER_STOP_RECORDING": "#8c564b",
}


@dataclass
class Header:
    steady_start_ns: int = 0
    system_start_iso: str = ""
    sample_rate: float = 0.0
    capacity: int = 0
    overflowed: bool = False
    total_entries: int = 0


@dataclass
class LogData:
    header: Header = field(default_factory=Header)
    wall_ns: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.int64))
    dac_ns: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.int64))
    events: np.ndarray = field(default_factory=lambda: np.array([], dtype=object))
    consumed: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.uint64))


def _default_log_path() -> Optional[str]:
    """Guess the platform-specific perf_logs dir and return the newest log."""
    home = os.path.expanduser("~")
    candidates = [
        os.path.join(home, "Library/Application Support/Audacity4/perf_logs"),
        os.path.join(home, ".config/Audacity4/perf_logs"),
        os.path.join(home, "AppData/Roaming/Audacity/Audacity4/perf_logs"),
    ]
    newest = None
    for d in candidates:
        for f in glob.glob(os.path.join(d, "playback_jitter_*.log")):
            if newest is None or os.path.getmtime(f) > os.path.getmtime(newest):
                newest = f
    return newest


def parse_log(path: str) -> LogData:
    header = Header()
    wall: List[int] = []
    dac: List[int] = []
    evt: List[str] = []
    cons: List[int] = []

    with open(path, "r") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                if "steady_start_ns=" in line:
                    for kv in line.lstrip("#").strip().split():
                        if "=" not in kv:
                            continue
                        k, v = kv.split("=", 1)
                        if k == "steady_start_ns":
                            header.steady_start_ns = int(v)
                        elif k == "system_start_iso":
                            header.system_start_iso = v
                        elif k == "sample_rate":
                            header.sample_rate = float(v)
                        elif k == "capacity":
                            header.capacity = int(v)
                        elif k == "overflowed":
                            header.overflowed = v.strip() == "1"
                        elif k == "total_entries":
                            header.total_entries = int(v)
                continue
            fields_ = line.split(",")
            if len(fields_) != 4:
                continue
            w, d, e, c = fields_
            wall.append(int(w))
            dac.append(int(d) if d else np.iinfo(np.int64).min)
            evt.append(e)
            cons.append(int(c))

    return LogData(
        header=header,
        wall_ns=np.asarray(wall, dtype=np.int64),
        dac_ns=np.asarray(dac, dtype=np.int64),
        events=np.asarray(evt, dtype=object),
        consumed=np.asarray(cons, dtype=np.uint64),
    )


def _print_summary(deltas_ms: np.ndarray, header: Header) -> None:
    if len(deltas_ms) == 0:
        print("no POSITION_UPDATE deltas found")
        return
    p50 = np.percentile(deltas_ms, 50)
    p95 = np.percentile(deltas_ms, 95)
    p99 = np.percentile(deltas_ms, 99)
    outliers = int(np.sum(deltas_ms > TARGET_MS * 1.5))
    print(f"session start: {header.system_start_iso}  sample_rate={header.sample_rate:.1f} Hz")
    print(f"POSITION_UPDATE count: {len(deltas_ms) + 1}"
          f"   overflowed={header.overflowed}   total_entries={header.total_entries}")
    print(f"inter-arrival ms:")
    print(f"  mean={np.mean(deltas_ms):6.3f}  min={np.min(deltas_ms):6.3f}"
          f"  p50={p50:6.3f}  p95={p95:6.3f}  p99={p99:6.3f}  max={np.max(deltas_ms):6.3f}")
    print(f"  outliers > {TARGET_MS * 1.5:.1f} ms: {outliers} "
          f"({100.0 * outliers / len(deltas_ms):.2f}%)")


def _plot_histogram(deltas_ms: np.ndarray, bins: int, out_path: str) -> None:
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.hist(deltas_ms, bins=bins, color="#1f77b4", edgecolor="black", alpha=0.8)
    ax.set_yscale("log")
    ax.axvline(TARGET_MS, color="red", linestyle="--", linewidth=1.5,
               label=f"target {TARGET_MS:.0f} ms")
    p50 = np.percentile(deltas_ms, 50)
    p95 = np.percentile(deltas_ms, 95)
    p99 = np.percentile(deltas_ms, 99)
    ax.axvline(p50, color="green",  linestyle=":", alpha=0.7, label=f"p50 {p50:.2f} ms")
    ax.axvline(p95, color="orange", linestyle=":", alpha=0.7, label=f"p95 {p95:.2f} ms")
    ax.axvline(p99, color="purple", linestyle=":", alpha=0.7, label=f"p99 {p99:.2f} ms")
    ax.set_xlabel("inter-arrival (ms)")
    ax.set_ylabel("count (log)")
    ax.set_title("updatePlaybackPosition() inter-arrival distribution")
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def _plot_timeline(data: LogData, out_path: str) -> None:
    pos_mask = data.events == POSITION_UPDATE
    pos_wall = data.wall_ns[pos_mask]
    if len(pos_wall) < 2:
        return
    t0 = pos_wall[0]
    t_sec = (pos_wall - t0) / 1e9
    deltas_ms = np.diff(pos_wall) / 1e6

    fig, ax = plt.subplots(figsize=(14, 5))
    ax.scatter(t_sec[1:], deltas_ms, s=4, alpha=0.6, color="#1f77b4",
               label="POSITION_UPDATE delta")
    ax.axhline(TARGET_MS, color="red", linestyle="--", linewidth=1, label="16 ms target")

    seen = set()
    for w, e in zip(data.wall_ns, data.events):
        if e == POSITION_UPDATE:
            continue
        color = EVENT_COLORS.get(e, "#7f7f7f")
        label = e if e not in seen else None
        seen.add(e)
        ax.axvline((w - t0) / 1e9, color=color, alpha=0.6, linewidth=1.2, label=label)

    ax.set_xlabel("session time (s)")
    ax.set_ylabel("tick delta (ms)")
    ax.set_title("Jitter over time with transport events")
    ax.set_ylim(bottom=0, top=max(TARGET_MS * 4, float(np.percentile(deltas_ms, 99.5)) * 1.1))
    ax.legend(loc="upper right", ncol=2, fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def _plot_dac_drift(data: LogData, out_path: str) -> None:
    sentinel = np.iinfo(np.int64).min
    valid = (data.events == POSITION_UPDATE) & (data.dac_ns != sentinel)
    if np.sum(valid) < 2:
        return
    wall = data.wall_ns[valid]
    dac = data.dac_ns[valid]
    t0 = wall[0]
    t_sec = (wall - t0) / 1e9
    drift_ms = (wall - dac) / 1e6

    fig, ax = plt.subplots(figsize=(14, 4))
    ax.plot(t_sec, drift_ms, color="#ff7f0e", linewidth=0.9, label="wall - DAC (ms)")
    ax.set_xlabel("session time (s)")
    ax.set_ylabel("drift (ms)")
    ax.set_title("Wall-clock minus DAC-time drift")
    ax.axhline(0, color="black", alpha=0.3, linewidth=0.8)
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--log", help="path to playback_jitter_*.log")
    ap.add_argument("--out-dir", help="directory for plots (default: alongside log)")
    ap.add_argument("--bins", type=int, default=120, help="histogram bin count")
    args = ap.parse_args(argv)

    log_path = args.log or _default_log_path()
    if not log_path or not os.path.isfile(log_path):
        print("no log file found. pass --log PATH or run Audacity with AU_PLAYBACK_JITTER_LOG=ON first.",
              file=sys.stderr)
        return 2

    data = parse_log(log_path)

    pos_mask = data.events == POSITION_UPDATE
    pos_wall = data.wall_ns[pos_mask]
    if len(pos_wall) < 2:
        print("fewer than 2 POSITION_UPDATE entries — nothing to analyze.", file=sys.stderr)
        return 1

    deltas_ms = np.diff(pos_wall) / 1e6

    out_dir = args.out_dir or os.path.join(
        os.path.dirname(log_path),
        "out_" + os.path.basename(log_path).removesuffix(".log").removeprefix("playback_jitter_"),
    )
    os.makedirs(out_dir, exist_ok=True)

    _print_summary(deltas_ms, data.header)
    _plot_histogram(deltas_ms, args.bins, os.path.join(out_dir, "histogram.png"))
    _plot_timeline(data, os.path.join(out_dir, "timeline.png"))
    _plot_dac_drift(data, os.path.join(out_dir, "dac_drift.png"))

    print(f"\nplots written to: {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
