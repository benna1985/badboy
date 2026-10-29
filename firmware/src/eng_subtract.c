/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Ben Mathers, on Hügelton Instruments' Felucca */
/* SUBTRACT: a two-oscillator subtractive voice in the way of Reason's Subtractor. Each oscillator has its own wave
 * (SAW SQR TRI SIN PWM) and its own fine tune in cents; OSC 2 also a semitone interval (SEMI, up to two octaves either
 * way). SUB (the WAV 1 values "..+SUB") adds a sine an octave below OSC 1. NOIS mixes white noise in. Then the
 * trapezoidal low-pass (CUT, RES) as ANALOG's, with the keyboard tracking at half. The track's own parts do the rest:
 * the amp and filter envelopes (ATK DEC SUS REL, ENV > FILTER), the pitch envelope (ENV > PITCH), LFO, the mod matrix,
 * the VOICE mode LEGATO and GLIDE (portamento).
 *
 * E0 WAV 1 (10 values: a wave, with or without the SUB), E1 WAV 2, E2 SEMI (OSC 2), E3 CT 1 and E4 CT 2 (cents, +-50),
 * E5 NOIS, E6 CUT, E7 RES.
 * Voice state: ph[0] OSC 1, ph[1] OSC 2, ph[2] SUB; s[0], s[1] the filter, s[2] the noise. The engine memory: none. */
static const char *const N_SUBT_WAV1[] = {"SAW", "SAW+SUB", "SQR", "SQR+SUB", "TRI", "TRI+SUB", "SIN", "SIN+SUB", "PWM", "PWM+SUB"};
static const char *const N_SUBT_WAV2[] = {"SAW", "SQR", "TRI", "SIN", "PWM"};

static inline int32_t subt_osc(uint32_t w, uint32_t ph, uint32_t inc, uint32_t pw)
{
    switch (w) {
    case 1:
        return osc_pulse(ph, inc, 0x80000000u);
    case 2:
        return osc_tri(ph);
    case 3:
        return osc_sine(ph);
    case 4:
        return osc_pulse(ph, inc, pw);
    default:
        return osc_saw(ph, inc);
    }
}

static void subt_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->ph[1] = v->ph[0] + 0x40000000u;                /* (the phases keep running over a retrigger: voice.c) */
    v->ph[2] = v->ph[0];
    v->s[0] = v->s[1] = 0;                            /* filter */
    if (!v->s[2])
        v->s[2] = 0x2468ACE + (int32_t)v->age;        /* noise state */
}

static void subt_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t w1 = (uint32_t)p[P_E0] >> 1, sub = (uint32_t)p[P_E0] & 1u, w2 = (uint32_t)p[P_E1], i;
    int32_t ct1 = p[P_E3], ct2 = p[P_E4], nz = p[P_E5] * 200;
    int32_t cut = (p[P_E6] << 8) + m->cutoff + 4 * (v->pitch16 - 60 * 16);
    uint32_t pw = 0x80000000u + (uint32_t)((m->shape - (64 << 8)) << 15);
    uint32_t inc1 = ct1 ? cents_inc(m->pitch16, ct1, m->fine) : m->inc;
    uint32_t inc2 = cents_inc(m->pitch16 + p[P_E2] * 16, ct2, m->fine);
    uint32_t inc3 = m->inc >> 1;                      /* the sub: an octave below OSC 1's pitch, not its detune */
    int32_t lv = sub ? 10922 : 16384;                 /* Q15: each of two (three with the SUB) the same level */
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];   /* state in locals: out[] may alias v->s[] */
    int32_t ic1 = v->s[0], ic2 = v->s[1], nst = v->s[2];
    tsvf_t flt;
    tsvf_coef(&flt, cut, p[P_E7]);
    for (i = 0; i < n; i++) {
        int32_t s = mulq15(subt_osc(w1, ph0, inc1, pw), lv) + mulq15(subt_osc(w2, ph1, inc2, pw), lv), y, a;
        if (sub) {
            s += mulq15(osc_sine(ph2), lv);
            ph2 += inc3;
        }
        ph0 += inc1;
        ph1 += inc2;
        if (nz)
            s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nz);
        y = tsvf_lp(&flt, s >> 1, &ic1, &ic2);        /* linear up to half scale, then a soft knee (the peaks of RES) */
        a = y < 0 ? -y : y;
        if (a > 16000) {
            a = 16000 + (softclip((a - 16000) * 2) >> 1);
            y = y < 0 ? -a : a;
        }
        out[i] += voice_amp(y << 1, m, i) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = nst;
}

/* {WAV 1, WAV 2, SEMI, CT 1, CT 2, NOIS, CUT, RES} */
static const preset_t SUBTRACT_PRESETS[] = {
    {"SUB SAW", {1, 0, 0, -6, 6, 0, 70, 30}, {0, 60, 100, 25}, 30, 1, FX(0, 0, 0, 10), PAT(2)},
    {"FAT SQR", {2, 1, 0, -8, 8, 0, 55, 40}, {0, 55, 90, 25}, 35, 1, FX(0, 0, 10, 10), PAT(2)},
    {"SINE SUB", {7, 3, 0, 0, 0, 0, 40, 0}, {0, 60, 100, 20}, 0, 1, FX(0, 0, 0, 10), PAT(8)},
    {"OCT LEAD", {0, 0, 12, -5, 5, 0, 95, 25}, {2, 70, 100, 40}, 20, 1, FX(0, 10, 40, 25), PAT(4)},
    {"FIFTH", {0, 2, 7, 0, 6, 0, 85, 25}, {0, 65, 95, 35}, 25, 1, FX(0, 10, 40, 25), PAT(4)},
    {"PWM LEAD", {8, 4, 0, -9, 9, 0, 90, 20}, {4, 70, 100, 45}, 15, 1, FX(0, 20, 40, 30), PAT(3)},
    {"DETUNE PD", {0, 0, 0, -12, 12, 0, 70, 15}, {70, 90, 110, 85}, 10, 0, FX(0, 60, 20, 70), PAT(5)},
    {"SYN BRASS", {0, 0, 0, 0, 10, 0, 50, 20}, {12, 70, 90, 30}, 40, 0, FX(0, 20, 20, 40), PAT(6)},
};

static const engine_t ENG_SUBTRACT = {
    .name = "SUBTR",
    .page_title = {"OSC", "FLT"},
    .edit = {
        {"WAV1", F_ENUM, 0, 9, 0, N_SUBT_WAV1, 0},
        {"WAV2", F_ENUM, 0, 4, 0, N_SUBT_WAV2, 0},
        {"SEMI", F_SEMI, -24, 24, 0, 0, 0},
        {"CT1", F_INT, -50, 50, 0, 0, "ct"},
        {"CT2", F_INT, -50, 50, 8, 0, "ct"},
        {"NOIS", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .presets = SUBTRACT_PRESETS,
    .npresets = NELEM(SUBTRACT_PRESETS),
    .note_on = subt_note_on,
    .render = subt_render,
    .knob = {P_E6, P_E7, P_E2, P_E4},
    .keep = 0x03,                /* the filter */
};
