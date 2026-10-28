#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fit every bass sample (assets/samples-bass) with tools/fit_bass.py, two at a time.
  tools/fit_bass_all.py HOSTSIM OUTDIR [ITERS] [NAME ...]      -> OUTDIR/NAME.json"""
import os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

T = Path(__file__).resolve().parent
JOBS = [("01_808", 31, 1.2), ("02_808D", 31, 1.2), ("03_BASS1", 28, 1.2), ("04_BASS2", 36, 1.2), ("05_REESD", 31, 1.2),
        ("06_SUBA", 33, 1.2), ("07_BASS3", 33, 0.6), ("08_DONK", 29, 0.6), ("09_DRED", 36, 0.8), ("10_DREDR", 36, 0.8),
        ("11_REESE", 36, 1.2), ("12_SDROP", 31, 1.2), ("13_SFALL", 35, 1.2)]


def run(j, hostsim, out, iters):
    name, root, secs = j
    r = subprocess.run([sys.executable, str(T / "fit_bass.py"), hostsim, str(T.parent / "assets/samples-bass" / f"{name}.wav"),
                        str(root), os.environ.get("ENG", "0"), "--iters", str(iters), "--secs", str(secs)], capture_output=True, text=True)
    (out / f"{name}.json").write_text(r.stdout)
    print(name, r.stdout.strip()[:120], flush=True)


if __name__ == "__main__":
    hostsim, out, iters = sys.argv[1], Path(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 1500
    only = sys.argv[4:]
    out.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(2) as ex:
        list(ex.map(lambda j: run(j, hostsim, out, iters), [j for j in JOBS if not only or j[0] in only]))
