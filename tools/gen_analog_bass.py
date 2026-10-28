#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""ANALOG's synth copies of the thirteen bass samples (assets/samples-bass) -> build/gen/eng_analog_x.c.

  tools/gen_analog_bass.py OUT.c

ANALOG's preset table lives in the Felucca-Engines submodule (firmware/engines, MIT), which this tree does not change:
this copies its eng_analog.c with the rows below added at the end of ANALOG_PRESETS (the twelve factory presets keep
their numbers; engines.c includes the copy). Each row is a preset fitted to its sample by tools/fit_bass.py (a host render
of the engine against the sample at its root note, and the time-averaged spectrum an octave up; LOSS is the fit's distance,
0 = the sample itself): {NAME, SAMPLE, {WAVE DTN MIX NOIS CUT RES DRV KTR}, {ATK DEC SUS REL}, ENV>FILTER, LOSS}.
They are BASS presets (category.c: eight-character names "SY ..", the sample's own name after the SY), MONO and dry.
"""
import sys
from pathlib import Path

ROWS = [
    ('SY 808', '808', [2, 7, 90, 24, 17, 45, 45, 51], [33, 108, 0, 88], 2, 0.8),
    ('SY 808D', '808D', [4, 94, 19, 24, 67, 44, 36, 78], [45, 8, 29, 43], 20, 6.32),
    ('SY BASS1', 'BASS1', [3, 15, 0, 21, 88, 108, 119, 127], [47, 127, 91, 116], -23, 2.62),
    ('SY BASS2', 'BASS2', [2, 58, 114, 15, 17, 95, 6, 24], [39, 99, 0, 99], 25, 2.0),
    ('SY REESD', 'REESD', [0, 51, 39, 8, 122, 27, 105, 26], [42, 0, 31, 49], -32, 7.2),
    ('SY SUBA', 'SUBA', [3, 2, 0, 0, 0, 18, 127, 22], [37, 38, 62, 67], 42, 0.93),
    ('SY BASS3', 'BASS3', [1, 96, 69, 15, 6, 117, 21, 59], [37, 91, 0, 12], 63, 7.47),
    ('SY DONK', 'DONK', [3, 0, 127, 10, 36, 127, 117, 102], [0, 80, 0, 41], 49, 5.53),
    ('SY DRED', 'DRED', [4, 28, 34, 13, 127, 118, 127, 127], [71, 126, 126, 100], -64, 14.8),
    ('SY DREDR', 'DREDR', [1, 3, 117, 0, 51, 102, 34, 127], [33, 106, 21, 88], 30, 4.88),
    ('SY REESE', 'REESE', [0, 57, 40, 24, 24, 97, 17, 125], [22, 62, 47, 92], 63, 3.47),
    ('SY SDROP', 'SDROP', [2, 12, 123, 24, 12, 47, 27, 100], [0, 108, 0, 71], -12, 1.04),
    ('SY SFALL', 'SFALL', [2, 34, 7, 24, 15, 80, 31, 83], [50, 119, 31, 65], -1, 1.0),
]

SRC = Path(__file__).resolve().parent.parent / "firmware" / "engines" / "src" / "eng_analog.c"


def main():
    out = Path(sys.argv[1])
    lines = SRC.read_text().split("\n")
    k = next(i for i, s in enumerate(lines) if s.startswith("static const engine_t ENG_ANALOG"))
    j = k - 1
    while not lines[j].strip():
        j -= 1
    if lines[j] != "};" or "ANALOG_PRESETS" not in "\n".join(lines[max(0, j - 40):j]):
        raise SystemExit("gen_analog_bass.py: ANALOG_PRESETS is not where it was (Felucca-Engines changed): " + str(SRC))
    add = ["    /* the synth copies of the bass samples (tools/gen_analog_bass.py): fitted by tools/fit_bass.py */"]
    for name, sample, e, env, fenv, loss in ROWS:
        assert len(name) <= 8 and len(e) == 8 and len(env) == 4
        add.append('    {"%s", {%s}, {%s}, %d, 1, FX(0, 0, 0, 10), PAT(2)},   /* %s, fit %.1f */'
                   % (name, ", ".join(map(str, e)), ", ".join(map(str, env)), fenv, sample, loss))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines[:j] + add + lines[j:]))


if __name__ == "__main__":
    main()
