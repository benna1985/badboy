#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fit a factory synth preset to a bass sample (host render through build/host/hostsim, as the firmware's DSP).

  tools/fit_bass.py HOSTSIM SAMPLE.wav ROOT_MIDI ENGINE [--iters N] [--seed S] [--secs T]

The target is the sample at its root note; the candidate is the engine playing the same note, held, with no sends.
Search: random starts over the preset's parameters (WAVE etc.), then a shrinking-step local search on the best. The loss is a
log-spectrogram distance (30 Hz .. 5 kHz) plus the loudness contour; levels are normalised (a preset has no level).
Prints the best preset as JSON.
"""
import json, os, subprocess, sys, tempfile
import numpy as np
import scipy.io.wavfile as wf

SR = 44100
P = dict(LEVEL=0, ATK=1, DEC=2, SUS=3, REL=4, FLT=5, DIST=33, CHOR=34, DLY=35, REV=36, VOICE=37, E0=106)
# engine -> parameter ranges of P_E0..P_E7
RANGES = {
    0: [(0, 6), (0, 127), (0, 127), (0, 24), (0, 127), (0, 127), (0, 127), (0, 127)],   # ANALOG: WAVE DTN MIX NOIS (kept low: no random wobble) CUT RES DRV KTR
    6: [(0, 15), (-24, 24), (-24, 24), (0, 50), (0, 0), (0, 127), (0, 127), (0, 127)],   # TRIO: WAVE INT2 INT3 DTN MODE(LP) CUT RES PW
    3: [(0, 4), (0, 4), (0, 127), (0, 127), (0, 127), (0, 40), (0, 0), (0, 127)],        # LOFI: CHIP WAVE DUTY CRSH SWP VIB ARP TONE
}
KEY_OCT = -3



def load(path, secs, speed=1.0):
    sr, x = wf.read(path)
    if x.dtype.kind == 'i':
        x = x.astype(np.float64) / np.iinfo(x.dtype).max
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(1)
    assert sr == SR, sr
    if speed != 1.0:                                      # the sampler playing it another key: resampled
        x = np.interp(np.arange(0, len(x) - 1, speed), np.arange(len(x)), x)
    x = x[:int(secs * SR)]
    n = len(x)
    out = np.zeros(int(secs * SR))
    out[:n] = x
    f = min(int(0.04 * SR), n // 4)                      # the firmware's fade-out of a cut sample
    if n >= int(secs * SR) - 1 and f:
        out[n - f:n] *= np.linspace(1, 0, f)
    return out


def render(hostsim, eng, preset, root, secs):
    """preset: dict(e=[8], env=[4], fenv, mono) -> mono float array from the note-on"""
    oct_, key = divmod(root - 53, 12)                   # keyboard key 0..11 and octave shift
    ps = [(P['E0'] + i, v) for i, v in enumerate(preset['e'])]
    ps += [(P['ATK'], preset['env'][0]), (P['DEC'], preset['env'][1]), (P['SUS'], preset['env'][2]),
           (P['REL'], preset['env'][3]), (P['FLT'], preset['fenv']), (P['VOICE'], preset['mono']),
           (P['DIST'], 0), (P['LEVEL'], 100)]
    env = dict(os.environ, SECS=str(int(secs + 1.0)), NOTE=str(key), OCT=str(oct_), SENDS='0,0,0',
               PSET=','.join(f'{a}:{b}' for a, b in ps))
    fd, tmp = tempfile.mkstemp(suffix='.wav')
    os.close(fd)
    try:
        subprocess.run([hostsim, str(eng), '0', '1', tmp], env=env, check=True, stderr=subprocess.DEVNULL)
        sr, y = wf.read(tmp)
    finally:
        os.remove(tmp)
    y = y.astype(np.float64).mean(1) / 32768.0
    s = int(0.05 * SR) + 2 * 32
    return y[s:s + int(secs * SR)]


def spec(x):
    n, hop = 2048, 512
    w = np.hanning(n)
    fr = np.array([x[i:i + n] * w for i in range(0, len(x) - n, hop)])
    m = np.abs(np.fft.rfft(fr, axis=1))
    return m, np.sqrt((fr ** 2).mean(1) + 1e-12)


LO, HI = int(30 * 2048 / SR), int(5000 * 2048 / SR)


def prep(x):
    r = np.sqrt((x ** 2).mean() + 1e-12)
    return x / r


def loss(t_spec, t_rms, y):
    if not np.isfinite(y).all() or np.abs(y).max() < 1e-4:
        return 1e3
    y = prep(y)
    ys, yr = spec(y)
    td = 20 * np.log10(t_spec[:, LO:HI] + 1e-4)
    yd = 20 * np.log10(ys[:, LO:HI] + 1e-4)
    top = td.max()
    td = np.maximum(td, top - 70)
    yd = np.maximum(yd, top - 70)
    spec_l = np.abs(td - yd).mean()
    tdb = 20 * np.log10(t_rms + 1e-5)
    ydb = 20 * np.log10(yr + 1e-5)
    fl = tdb.max() - 40                                   # the contour below -40 dB does not count (a swell's start, a tail)
    env_l = np.abs(np.maximum(tdb, fl) - np.maximum(ydb, fl)).mean()
    return spec_l + 1.5 * env_l


def loss_static(t_spec, y):
    """another key: the sampler plays the sample faster, a synth keeps its envelope time: only the time-averaged spectrum"""
    if not np.isfinite(y).all() or np.abs(y).max() < 1e-4:
        return 1e3
    ys, _ = spec(prep(y))
    td = 20 * np.log10((t_spec[:, LO:HI] ** 2).mean(0) ** 0.5 + 1e-4)
    yd = 20 * np.log10((ys[:, LO:HI] ** 2).mean(0) ** 0.5 + 1e-4)
    top = td.max()
    return np.abs(np.maximum(td, top - 60) - np.maximum(yd, top - 60)).mean()


def clampp(p, rng):
    p['e'] = [int(min(max(v, lo), hi)) for v, (lo, hi) in zip(p['e'], rng)]
    p['env'] = [int(min(max(v, 0), 127)) for v in p['env']]
    p['env'][3] = min(p['env'][3], 100)                  # a longer release is not silent 12 s after the note-off (regress.c health)
    p['fenv'] = int(min(max(p['fenv'], -64), 63))
    return p


def main():
    a = sys.argv[1:]
    hostsim, wav, root, eng = a[0], a[1], int(a[2]), int(a[3])
    iters = int(a[a.index('--iters') + 1]) if '--iters' in a else 1200
    seed = int(a[a.index('--seed') + 1]) if '--seed' in a else 1
    secs = float(a[a.index('--secs') + 1]) if '--secs' in a else 1.2
    mono = 1
    rng = RANGES[eng]
    r = np.random.default_rng(seed)
    shifts = [0, 12]                                      # the root and an octave up: the keyboard must track
    tg = []
    for sh in shifts:
        tx = prep(load(wav, secs, 2.0 ** (sh / 12)))
        tg.append(spec(tx))

    def ev(p):
        tot = 0.0
        for sh, (ts, tr) in zip(shifts, tg):
            y = render(hostsim, eng, p, root + sh, secs)
            tot += loss(ts, tr, y) if sh == 0 else 0.7 * 3 * loss_static(ts, y)
        return tot / 1.7

    def rand():
        return clampp(dict(e=[int(r.integers(lo, hi + 1)) for lo, hi in rng],
                           env=[int(r.integers(0, 40)), int(r.integers(30, 127)), int(r.integers(0, 127)), int(r.integers(10, 90))],
                           fenv=int(r.integers(-64, 64)), mono=mono), rng)

    pool = []
    for _ in range(max(60, iters // 4)):
        p = rand()
        pool.append((ev(p), p))
    pool.sort(key=lambda z: z[0])
    best = pool[0]
    starts = []
    seen = set()
    for l, p in pool:                                     # the best start of each WAVE, three in all
        if p['e'][0] not in seen:
            seen.add(p['e'][0])
            starts.append((l, p))
        if len(starts) == 3:
            break
    for l, p in starts:
        cur = (l, p)
        step = 24.0
        for it in range(iters // 3):
            q = json.loads(json.dumps(cur[1]))
            k = int(r.integers(1, 4))
            for _ in range(k):
                i = int(r.integers(0, 13))
                d = int(round(r.normal(0, step)))
                if i < 8:
                    if i == 0:
                        continue                          # keep WAVE
                    q['e'][i] += d
                elif i < 12:
                    q['env'][i - 8] += d
                else:
                    q['fenv'] += d
            q = clampp(q, rng)
            l2 = ev(q)
            if l2 < cur[0]:
                cur = (l2, q)
            if it % 100 == 99:
                step = max(2.0, step * 0.7)
        if cur[0] < best[0]:
            best = cur
    print(json.dumps(dict(loss=round(best[0], 3), eng=eng, preset=best[1])))


if __name__ == '__main__':
    main()
