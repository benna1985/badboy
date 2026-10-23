#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Load the six 140 bpm breaks of assets/breaks-140bpm/ into the FM-1's user sample slots USR1..USR6 (SLICE SRC USR1..USR6),
one slot per break in the order of BREAKS below. Needs the FM-1 connected over USB and `pip3 install mido python-rtmidi`.

  load_breaks.py           load all six (replaces what the slots hold)
  load_breaks.py --dry     only list what would go where
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DIR = HERE.parent / "assets" / "breaks-140bpm"
BREAKS = [("AMEN", "Amen_2bar.wav"), ("FNKDRUM", "FunkyDrummer_1bar.wav"), ("FNKMULE", "FunkyMule_2bar.wav"),
          ("HOTPANTS", "HotPants_2bar.wav"), ("SESAME", "Sesame_1bar.wav"), ("THINK", "Think_1bar.wav")]


def main():
    dry = "--dry" in sys.argv[1:]
    for slot, (name, fn) in enumerate(BREAKS, 1):
        path = DIR / fn
        print(f"USR{slot}: {name:8} <- {fn}")
        if dry:
            continue
        r = subprocess.run([sys.executable, str(HERE / "fm1_sample_upload.py"), "load", str(slot), name, f"{path}:60"])
        if r.returncode:
            sys.exit(f"USR{slot} failed (see above); nothing after it was loaded")


if __name__ == "__main__":
    main()
