/* SPDX-License-Identifier: GPL-3.0-only */
/* ROLL (P_ROLL, P_RPIT; seq.c seq_hits / seq_step, voice.c roll16), on the real sequencer sources (the stubs of
 * tests/ui_test.c): a NOTE step plays repeats at the track's rate (1/8 .. 1/64, triplets), at least its RATCH, at most 16;
 * a rate no faster than the step is the step as before; RPIT ramps the pitch from the first repeat to the last (1/16
 * semitones in roll16 of the voice); every other note starts with no offset; both are lockable (motion.c). */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static track_t *one_step(uint32_t sdiv, uint32_t roll, int rpit, uint32_t ratch)
{
    track_t *t;
    ui_power_on();
    t = &trk[0];
    track_defaults_steps(t);
    t->p[P_SLEN] = 1;
    t->p[P_SDIV] = (int16_t)sdiv;
    t->p[P_SGATE] = 64;
    t->p[P_SSWING] = 0;
    song.g[G_SWING] = 0;
    t->p[P_ROLL] = (int16_t)roll;
    t->p[P_RPIT] = (int16_t)rpit;
    t->step[0] = (step_t){{60}, 1, ST_NOTE, 0, 100};
    step_set_ratchet(&t->step[0], ratch);
    seq_start();
    return t;
}
/* note-ons in one pass of the step (the last block before the next pass left out); off[k]: roll16 of the k-th */
static uint32_t pass(track_t *t, int16_t *off, uint32_t nmax)
{
    uint32_t blocks = step_samples(t, div_samples((uint32_t)t->p[P_SDIV]), 0) / CTL - 1u, b, n = 0, v0;
    for (b = 0; b < blocks; b++) {
        v0 = vage;
        seq_tick(t, CTL);
        for (; v0 < vage; v0++, n++)
            if (off && n < nmax)
                off[n] = t->roll16[t->m_vi];
    }
    return n;
}

int main(void)
{
    int bad = 0, ok;
    uint32_t n;
    int16_t off[20];
    track_t *t;

    /* 1/16 step (SDIV 2): rate -> repeats */
    {
        static const uint32_t want[8] = {1, 1, 1, 2, 4, 1, 2, 3};   /* OFF 1/8 1/16 1/32 1/64 1/8T 1/16T 1/32T */
        uint32_t r;
        ok = 1;
        for (r = 0; r < 8u; r++) {
            t = one_step(2, r, 0, 1);
            n = pass(t, 0, 0);
            if (n != want[r])
                printf("roll: rate %u: %u note-ons, wanted %u\n", r, n, want[r]);
            ok &= n == want[r];
        }
        bad += check("ROLL rates on a 1/16 step: OFF 1/8 1/16 1, 1/32 2, 1/64 4, 1/8T 1, 1/16T 2, 1/32T 3", ok);
    }
    t = one_step(1, 4, 0, 1);
    bad += check("  a 1/8 step at 1/64: 8", pass(t, 0, 0) == 8u);
    t = one_step(0, 4, 0, 1);
    bad += check("  a 1/4 step at 1/64: 16, no more", pass(t, 0, 0) == 16u);
    t = one_step(2, 3, 0, 4);
    bad += check("  RATCH x4 with 1/32 (2): 4, the RATCH when it is more", pass(t, 0, 0) == 4u);
    t = one_step(2, 4, 0, 2);
    bad += check("  RATCH x2 with 1/64 (4): 4, the roll when it is more", pass(t, 0, 0) == 4u);

    /* pitch ramp */
    t = one_step(2, 4, 12, 1);
    n = pass(t, off, 20);
    bad += check("RPIT +12 over 4 repeats: roll16 0, 64, 128, 192", n == 4u && off[0] == 0 && off[1] == 64 && off[2] == 128 && off[3] == 192);
    t = one_step(2, 4, -24, 1);
    n = pass(t, off, 20);
    bad += check("  RPIT -24: 0, -128, -256, -384", n == 4u && off[0] == 0 && off[1] == -128 && off[2] == -256 && off[3] == -384);
    t = one_step(2, 4, 0, 1);
    n = pass(t, off, 20);
    ok = n == 4u;
    for (n = 0; n < 4u; n++) ok &= off[n] == 0;
    bad += check("  RPIT 0: no offset", ok);
    t = one_step(2, 3, 7, 1);
    pass(t, 0, 0);
    trk_note_on(t, 64, 100);
    bad += check("  a note from the keys starts with no offset", t->roll16[t->m_vi] == 0 && t->roll_next == 0);

    /* locks: the two are automatable */
    bad += check("P_ROLL and P_RPIT lock per step (motion_param)", motion_param(P_ROLL) && motion_param(P_RPIT));
    bad += check("  they sit before P_E0 (ids of the engine values move, the common ones do not)", P_RPIT + 1 == P_E0 && P_ROLL == P_ESYNC + 1);
    bad += check("  defaults: OFF, 0", TP[P_ROLL].def == 0 && TP[P_RPIT].def == 0 && TP[P_ROLL].max == 7 && TP[P_RPIT].min == -24 && TP[P_RPIT].max == 24);
    printf("%s\n", bad ? "roll: FAILED" : "roll: ok");
    return bad != 0;
}
