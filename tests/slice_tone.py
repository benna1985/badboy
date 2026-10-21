#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""A pure 220.5 Hz tone (100 samples a cycle at 22,050 Hz), 3.2 s, as a user-slot file for the SLICE STRCH / BITS
tests (tests/slice_test.c): writes OUT.hdr and OUT.bin like tools/fm1_sample_upload.py build.
  tests/slice_tone.py OUT"""
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import sampleio as sio  # noqa: E402

n = int(3.2 * sio.SLOT_RATE)
x = [int(20000 * math.sin(2 * math.pi * i / 100.0)) for i in range(n)]
hdr, data = sio.user_slot("TONE", [(x, 60, None, None)])
Path(sys.argv[1] + ".hdr").write_bytes(hdr)
Path(sys.argv[1] + ".bin").write_bytes(data)
print(f"slice_tone: {sys.argv[1]}.hdr/.bin, {n} samples, {len(data)} B ADPCM")
