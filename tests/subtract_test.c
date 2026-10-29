/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ben Mathers, on Hügelton Instruments' Felucca */
/* Host test of the SUBTRACT engine (src/eng_subtract.c), same sources as the firmware (hostsim.c):
 *   build/host/subtract_test        (run_tests.sh builds and runs it)
 * 1. the voice: every factory preset sounds, bounded, no DC, notes C1..C6; the engine is the 15th (ENGI_SUBTRACT 14), listed
 *    after FM6, its presets BASS / LEAD / PAD, and the defaults of its eight parameters.
 * 2. the oscillators, by sines (WAV 1 and 2 SIN, the filter open): one at the key's pitch; CT 1 / CT 2 tune each oscillator
 *    by cents (+-50 ct: +-2.93 %), each alone; SEMI moves OSC 2 only (+7: a fifth, +12: an octave, -12 below); SUB (WAV 1
 *    "+SUB") adds the octave below OSC 1, not there without it; NOIS adds noise (high band energy).
 * 3. the track's own parts work on it: the pitch envelope (ENV > PITCH: the start is higher than the end), GLIDE with
 *    the LEGATO voice mode (the second note starts at the first one's pitch), the filter (CUT down lowers the centroid).
 */
#define main hostsim_main
#include "hostsim.c"
#undef main
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#endif


static int bad;
static void check(const char *what, int ok)
{
    printf("subtract: %-86s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static int32_t out_buf[2 * CTL];
static uint64_t hash;
static void blocks(uint32_t n)
{
    uint32_t i, k;
    while (n--) {
        mix_block(out_buf, CTL);
        for (i = 0; i < 2u * CTL; i++)
            for (k = 0; k < 4u; k++) {
                hash ^= ((uint32_t)out_buf[i] >> (8u * k)) & 0xFFu;
                hash *= 0x100000001B3ull;
            }
    }
}

static uint32_t eng_by_name(const char *n)
{
    uint32_t e;
    for (e = 0; e < NENGINES; e++)
        if (str_eq(ENGINES[e]->name, n))
            return e;
    return 0;
}
static uint32_t preset_by_name(uint32_t e, const char *n)
{
    uint32_t k;
    for (k = 0; k < ENGINES[e]->npresets; k++)
        if (str_eq(ENGINES[e]->presets[k].name, n))
            return k;
    return 0;
}

static void fresh(uint32_t e, uint32_t pi)        /* the boot state (no FX tails), track 1 = engine e preset pi */
{
    uint32_t k;
    memset(dly_buf, 0, sizeof dly_buf);
    memset(cho_buf, 0, sizeof cho_buf);
    memset(rev_comb, 0, sizeof rev_comb);
    memset(rev_ap, 0, sizeof rev_ap);
    memset(&fx, 0, sizeof fx);
    memset(&pf, 0, sizeof pf);
    for (k = 0; k < NTRK; k++)
        pf.mg[k] = 32768;
    perf_held = perf_latched = 0;
    lim_env = LIM_T;
    dc_l = dc_r = dce_l = dce_r = 0;
    vage = 0;
    rng_state = 0x1234567u;
    mod_seed = 0x2545F491u;
    memset(trk, 0, sizeof trk);
    MOD_W_CLEAR();
    memset(sl, 0, sizeof sl);
    memset(T_ESYNC(&trk[0]), 0, 3u * sizeof(uint32_t));   /* (one core: the shared one; two: in trk, cleared above) */
    host_tracks_init();
    for (k = 0; k < NPART; k++)
        host_preset(&trk[k], k ? 0u : e, k ? 0u : pi);
    song.sel = 0;
    song.g[G_CLOCK] = 0;
    song.g[G_BPM] = 120;
    midi_beat_samples = 0;
    hash = 0xCBF29CE484222325ull;
}
static void dry(track_t *t)
{
    t->p[P_DIST] = t->p[P_CHOR] = t->p[P_DLY] = t->p[P_REV] = 0;
}


/* ------------------------------------------------------------ analysis --- */
#define NF 16384
static double sig[NF], wre[NF], wim[NF];
static void fft(void)
{
    uint32_t i, j, len;
    for (i = 0; i < NF; i++) {
        wre[i] = sig[i] * (0.5 - 0.5 * cos(2 * M_PI * i / NF));
        wim[i] = 0;
    }
    for (i = 1, j = 0; i < NF; i++) {
        uint32_t bit = NF >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = wre[i]; wre[i] = wre[j]; wre[j] = t;
            t = wim[i]; wim[i] = wim[j]; wim[j] = t;
        }
    }
    for (len = 2; len <= NF; len <<= 1) {
        double ang = -2 * M_PI / len;
        for (i = 0; i < NF; i += len)
            for (j = 0; j < len / 2; j++) {
                double wr = cos(ang * j), wi = sin(ang * j);
                double xr = wre[i + j + len / 2] * wr - wim[i + j + len / 2] * wi;
                double xi = wre[i + j + len / 2] * wi + wim[i + j + len / 2] * wr;
                wre[i + j + len / 2] = wre[i + j] - xr, wim[i + j + len / 2] = wim[i + j] - xi;
                wre[i + j] += xr, wim[i + j] += xi;
            }
    }
}
static double mag(uint32_t k) { return sqrt(wre[k] * wre[k] + wim[k] * wim[k]); }
static double bin_hz(uint32_t k) { return (double)k * FS / NF; }
/* the strongest bin in [lo, hi] Hz: its frequency (interpolated), *m its magnitude */
static double peak_hz(double lo, double hi, double *m)
{
    uint32_t k, kb = 0;
    double best = -1, a, b, c, d;
    for (k = (uint32_t)(lo * NF / FS); k <= (uint32_t)(hi * NF / FS); k++)
        if (mag(k) > best)
            best = mag(k), kb = k;
    a = mag(kb - 1), b = mag(kb), c = mag(kb + 1);
    d = a - 2 * b + c;
    if (m)
        *m = b;
    return bin_hz(kb) + (d ? 0.5 * (a - c) / d * FS / NF : 0);
}
static double energy(double lo, double hi)
{
    uint32_t k;
    double e = 0;
    for (k = (uint32_t)(lo * NF / FS); k <= (uint32_t)(hi * NF / FS); k++)
        e += mag(k) * mag(k);
    return e;
}

/* track 1: SUBTRACT, the preset's sends off, parameters set by id/value pairs, note held: the left channel from 0.3 s */
static double note_hz(uint32_t n) { return 440.0 * pow(2.0, ((double)n - 69.0) / 12.0); }
static void setp(track_t *t, uint32_t wav1, uint32_t wav2, int semi, int ct1, int ct2, int nois, int cut, int res)
{
    t->p[P_E0] = (int16_t)wav1;
    t->p[P_E1] = (int16_t)wav2;
    t->p[P_E2] = (int16_t)semi;
    t->p[P_E3] = (int16_t)ct1;
    t->p[P_E4] = (int16_t)ct2;
    t->p[P_E5] = (int16_t)nois;
    t->p[P_E6] = (int16_t)cut;
    t->p[P_E7] = (int16_t)res;
    t->p[P_ATK] = 0;
    t->p[P_SUS] = 127;
    t->p[P_ED_FLT] = 0;
    t->p[P_ED_PIT] = 0;
    t->p[P_VOICE] = V_POLY;
    t->p[P_GLIDE] = 0;
    dry(t);
}
static void capture(uint32_t from_block, uint32_t nblocks)   /* into sig[]: the left channel */
{
    uint32_t b, i, k = 0;
    for (b = 0; b < from_block + nblocks; b++) {
        mix_block(out_buf, CTL);
        if (b >= from_block)
            for (i = 0; i < CTL && k < NF; i++)
                sig[k++] = out_buf[2u * i];
    }
}
static void render_note(uint32_t note)
{
    trk_note_on(&trk[0], note, 100);
    capture(FS * 3u / 10u / CTL, NF / CTL);
}

int main(void)
{
    uint32_t e = ENGI_SUBTRACT, k, n, p;
    double m1, m2, f1, f2, f0, base;
    int ok;
    static const int16_t DEF[8] = {0, 0, 0, 0, 8, 0, 90, 30};

    /* ---- 1 ---- */
    check("the 15th engine (14), named SUBTR, after FM6 in the list, 8 presets",
          NENGINES >= 15 && ENGINES[e]->name[0] == 'S' && str_eq(ENGINES[e]->name, "SUBTR") && ENGINES[e]->npresets == 8u &&
              eng_vis(eng_rank(e)) == e && eng_step(ENGI_FM6, 1) == e);
    for (ok = 1, k = 0; k < 8u; k++)
        ok &= ENGINES[e]->edit[k].def == DEF[k];
    check("the defaults: WAV1 SAW, WAV2 SAW, SEMI 0, CT1 0, CT2 8, NOIS 0, CUT 90, RES 30", ok);
    for (ok = 1, p = 0; p < ENGINES[e]->npresets; p++)
        for (n = 24; n <= 84; n += 12) {
            double peak = 0, mean = 0;
            uint32_t i;
            fresh(e, p);
            dry(&trk[0]);
            trk_note_on(&trk[0], n, 110);
            for (i = 0; i < FS / 2u / CTL; i++) {
                uint32_t j;
                mix_block(out_buf, CTL);
                for (j = 0; j < CTL; j++) {
                    peak = fmax(peak, fabs((double)out_buf[2u * j]));
                    mean += out_buf[2u * j];
                }
            }
            mean /= (double)(FS / 2u / CTL * CTL);
            ok &= peak > 400.0 && peak < 30000.0 && fabs(mean) < 0.02 * peak + 50.0;
        }
    check("every preset, C1 .. C6: it sounds, is bounded (< 30000), has no DC", ok);

    /* ---- 2 ---- */
    fresh(e, 0);
    setp(&trk[0], 6u, 3u, 0, 0, 0, 0, 127, 0);                /* WAV1 SIN (6 = SIN, no SUB), WAV2 SIN */
    render_note(48);                                          /* C3 */
    fft();
    f0 = peak_hz(100, 160, &m1);
    base = note_hz(48);
    check("two sines at the key (OSC 1 and 2 at C3, CT 0): one peak at 130.8 Hz", fabs(f0 - base) < 0.4);
    base = note_hz(72);                                       /* C6: the two tunings 30 Hz apart, the bins 1 Hz */
    fresh(e, 0);
    setp(&trk[0], 6u, 3u, 0, 50, 0, 0, 127, 0);
    render_note(72);
    fft();
    f1 = peak_hz(base * 0.99, base * 1.01, 0);
    f2 = peak_hz(base * 1.02, base * 1.04, 0);
    check("CT 1 +50 ct moves OSC 1 only (+2.93 %): peaks at C6 and at C6 x 1.0293", fabs(f2 - base * 1.0293) < 1.0 && fabs(f1 - base) < 1.0);
    fresh(e, 0);
    setp(&trk[0], 6u, 3u, 0, 0, -50, 0, 127, 0);
    render_note(72);
    fft();
    f1 = peak_hz(base * 0.99, base * 1.01, 0);
    f2 = peak_hz(base * 0.96, base * 0.98, 0);
    check("CT 2 -50 ct moves OSC 2 only (-2.84 %): peaks at C6 and at C6 x 0.9716", fabs(f2 - base * 0.97165) < 1.0 && fabs(f1 - base) < 1.0);
    base = note_hz(48);
    for (ok = 1, k = 0; k < 3u; k++) {
        static const int SEMI[3] = {7, 12, -12};
        static const double R[3] = {1.4983, 2.0, 0.5};
        fresh(e, 0);
        setp(&trk[0], 6u, 3u, SEMI[k], 0, 0, 0, 127, 0);
        render_note(48);
        fft();
        f2 = peak_hz(base * (R[k] - 0.05), base * (R[k] + 0.05), &m2);
        peak_hz(100, 160, &m1);
        ok &= fabs(f2 - base * R[k]) < 0.8 && m2 > 0.3 * m1;
    }
    check("SEMI +7 / +12 / -12: OSC 2 a fifth, an octave up, an octave down; OSC 1 stays", ok);
    fresh(e, 0);
    setp(&trk[0], 7u, 3u, 0, 0, 0, 0, 127, 0);                /* WAV1 SIN+SUB */
    render_note(60);
    fft();
    peak_hz(250, 280, &m1);
    f2 = peak_hz(125, 137, &m2);
    check("SUB (WAV1 SIN+SUB): a sine an octave below OSC 1 (130.8 Hz for C4)", fabs(f2 - note_hz(60) / 2) < 0.6 && m2 > 0.3 * m1);
    fresh(e, 0);
    setp(&trk[0], 6u, 3u, 0, 0, 0, 0, 127, 0);
    render_note(60);
    fft();
    peak_hz(250, 280, &m1);
    peak_hz(125, 137, &m2);
    check("..and without the SUB nothing is there", m2 < 0.01 * m1);
    fresh(e, 0);
    setp(&trk[0], 6u, 3u, 0, 0, 0, 0, 127, 0);
    render_note(60);
    fft();
    m1 = energy(4000, 10000);
    fresh(e, 0);
    setp(&trk[0], 6u, 3u, 0, 0, 0, 100, 127, 0);
    render_note(60);
    fft();
    m2 = energy(4000, 10000);
    check("NOIS adds noise: the energy above 4 kHz rises by more than 40 dB over the sines'", m2 > 10000.0 * m1);

    /* ---- 3 ---- */
    {   /* the pitch envelope: ENV > PITCH, the early part of the note above its end */
        double early, late;
        uint32_t i;
        fresh(e, 0);
        setp(&trk[0], 6u, 3u, 0, 0, 0, 0, 127, 0);
        trk[0].p[P_ED_PIT] = 40;
        trk[0].p[P_ATK] = 0;
        trk[0].p[P_DEC] = 70;
        trk[0].p[P_SUS] = 0;
        trk_note_on(&trk[0], 48, 100);
        for (i = 0; i < NF; i++) {
            if (!(i % CTL))
                mix_block(out_buf, CTL);
            sig[i] = out_buf[2u * (i % CTL)];
        }
        fft();
        early = peak_hz(100, 400, 0);
        fresh(e, 0);
        setp(&trk[0], 6u, 3u, 0, 0, 0, 0, 127, 0);
        check("ENV > PITCH (the track's pitch envelope) moves the note: the sound's pitch is not at the key's", fabs(early - base) > 1.0);
        (void)late;
    }
    {   /* LEGATO + GLIDE: the second note starts from the first one's pitch and glides (TIME mode: 1 st per 11.7 ms) */
        uint32_t i, z = 0;
        double hz;
        fresh(e, 0);
        setp(&trk[0], 6u, 3u, 0, 0, 0, 0, 127, 0);
        trk[0].p[P_VOICE] = V_LEGATO;
        trk[0].p[P_GLIDE] = 90;
        trk[0].p[P_GLMODE] = 1;
        trk_note_on(&trk[0], 48, 100);
        for (i = 0; i < FS / 2u / CTL; i++)
            mix_block(out_buf, CTL);
        trk_note_on(&trk[0], 60, 100);
        for (i = 0; i < 2048u / CTL; i++) {            /* the first 46 ms of the second note: its zero crossings */
            uint32_t j;
            mix_block(out_buf, CTL);
            for (j = 0; j < CTL; j++)
                sig[i * CTL + j] = out_buf[2u * j];
        }
        for (i = 1; i < 2048u; i++)
            z += (sig[i - 1u] < 0) != (sig[i] < 0);
        hz = z / 2.0 / (2048.0 / FS);
        check("LEGATO + GLIDE (portamento, TIME): the second note C4 starts near C3, still low in its first 46 ms (< 200 Hz)",
              hz > 110.0 && hz < 200.0);
    }
    {   /* the filter: CUT down lowers the centroid of a saw pair */
        double c1 = 0, c2 = 0, t1 = 0, t2 = 0;
        uint32_t j;
        fresh(e, 0);
        setp(&trk[0], 0u, 0u, 0, 0, 8, 0, 120, 0);
        render_note(48);
        fft();
        for (j = 1; j < NF / 2u; j++)
            c1 += mag(j) * bin_hz(j), t1 += mag(j);
        fresh(e, 0);
        setp(&trk[0], 0u, 0u, 0, 0, 8, 0, 40, 0);
        render_note(48);
        fft();
        for (j = 1; j < NF / 2u; j++)
            c2 += mag(j) * bin_hz(j), t2 += mag(j);
        check("CUT 120 -> 40: the spectral centroid falls by more than half", c2 / t2 < 0.5 * (c1 / t1));
    }
    printf("subtract: %s\n", bad ? "FAILED" : "all ok");
    return bad != 0;
}
