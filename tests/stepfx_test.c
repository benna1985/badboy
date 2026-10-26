/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SEQ > STEP FX (SLICE per-step ROLL / STRCH / PTCH), on the real sequencer and UI sources (stubs of tests/ui_test.c):
 *   KNOB 1 walks the cursor; KNOB 2 / 3 / 4 lock ROLL / STRCH / PTCH on the cursor step alone
 *   STRCH and PTCH only on a SLICE track; EDIT takes the cursor step's three locks away, others stay
 *   playback: a ROLL lock on one step repeats that step only
 * Built and run by tests/run_tests.sh. */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

/* track 1 alone (ANALOG, POLY), 16 steps of 1/16, GATE 64, no swing: C4 on 0, a REST on 1, E4 on 2 with a TIE on 3,
 * a chord C E G on 4, step 5 empty (a REST), A4 on 6; SEQ > STEP on step `cur` */
static track_t *pattern(uint32_t cur)
{
    track_t *t;
    ui_power_on();
    song.sel = 0;
    t = &trk[0];
    track_defaults_steps(t);
    t->p[P_SLEN] = 16;
    t->p[P_SDIV] = 2;
    t->p[P_SGATE] = 64;
    t->p[P_SSWING] = 0;
    t->p[P_VOICE] = V_POLY;
    song.g[G_SWING] = 0;
    song.g[G_BPM] = 120;
    t->step[0] = (step_t){{60}, 1, ST_NOTE, 0, 0};
    t->step[1] = (step_t){{0}, 0, ST_REST, 0, 0};
    t->step[2] = (step_t){{64}, 1, ST_NOTE, 0, 0};
    t->step[3] = (step_t){{0}, 0, ST_TIE, 0, 0};
    t->step[4] = (step_t){{60, 64, 67}, 3, ST_NOTE, 0, 0};
    t->step[6] = (step_t){{69}, 1, ST_NOTE, 0, 0};
    go_page(GR_ROLL);
    ui.cursor = (uint8_t)cur;
    frame();
    return t;
}
static void go_detail(void)
{
    uint32_t i;
    for (i = 0; i < NPAGES && !str_eq(PAGES[i].title, "DETAIL"); i++)
        ;
    ui.home = 0;
    ui.page = (uint8_t)i;
    page_entered();
    frame();
}

static void kt(uint32_t role, int32_t s)
{
    memset(ui.enc_t, 0, sizeof ui.enc_t);
    turn(role, s);
}
static void go_fx(void)
{
    uint32_t i;
    for (i = 0; i < NPAGES && !str_eq(PAGES[i].title, "STEP FX"); i++)
        ;
    ui.home = 0;
    ui.page = (uint8_t)i;
    page_entered();
    frame();
}
static uint32_t notes(track_t *t, uint32_t blocks)
{
    uint32_t b, n = 0;
    seq_start();
    for (b = 0; b < blocks; b++) {
        uint32_t v0 = vage;
        seq_tick(t, CTL);
        n += vage - v0;
    }
    seq_stop();
    return n;
}
static int locked(track_t *t, uint32_t s, uint32_t id) { int16_t v; return motion_lock_get(t, s, id, &v); }

int main(void)
{
    int bad = 0, ok;
    track_t *t = pattern(2);
    uint32_t base, with;
    t->eng_req = ENGI_SLICE;
    go_fx();
    ok = fx_on() && ui.cursor == 2u;
    kt(EN_K2, 2);
    kt(EN_K3, 5);
    kt(EN_K4, -3);
    ok &= locked(t, 2, P_ROLL) && locked(t, 2, P_E2) && locked(t, 2, P_E3);
    ok &= !locked(t, 0, P_ROLL) && !locked(t, 1, P_E2) && !locked(t, 3, P_E3) && !locked(t, 4, P_ROLL);
    bad += check("STEP FX on SLICE: KNOB 2..4 lock ROLL / STRCH / PTCH on the cursor step only", ok);
    kt(EN_K1, 2);
    ok = ui.cursor == 4u && !locked(t, 4, P_ROLL) && locked(t, 2, P_ROLL);
    kt(EN_K3, 3);
    ok &= locked(t, 4, P_E2) && !locked(t, 4, P_E3);
    bad += check("  KNOB 1 walks the cursor; the next step gets its own lock", ok);
    {
        int16_t v;
        ok = motion_lock_get(t, 2, P_E3, &v) && v == -3 && motion_lock_get(t, 2, P_E2, &v) && v > 0;
    }
    bad += check("  the values locked: PTCH -3 semitones, STRCH raised", ok);
    ui.cursor = 2;
    press(B_EDIT); frame();
    ok = !locked(t, 2, P_ROLL) && !locked(t, 2, P_E2) && !locked(t, 2, P_E3) && locked(t, 4, P_E2);
    bad += check("  EDIT takes the cursor step's three away, step 5's stays", ok);
    t->eng_req = 0;                                    /* ANALOG: ROLL only */
    ui.cursor = 6;
    kt(EN_K3, 4);
    kt(EN_K4, 4);
    ok = !locked(t, 6, P_E2) && !locked(t, 6, P_E3);
    kt(EN_K2, 1);
    ok &= locked(t, 6, P_ROLL);
    bad += check("  not a SLICE track: STRCH / PTCH take no lock, ROLL does", ok);
    /* playback: step 7 (A4) gets a ROLL lock, nothing else changes */
    t = pattern(6);
    t->eng_req = 0;
    base = notes(t, div_samples(2) / CTL * 16u - 1u);
    go_fx();
    kt(EN_K2, 4);
    ok = locked(t, 6, P_ROLL);
    with = notes(t, div_samples(2) / CTL * 16u - 1u);
    bad += check("  a ROLL lock on one step: it repeats, the rest of the bar plays as before", ok && with > base);
    /* a lock lasts the note: step 3 is a TIE of step 3 (index 2): ROLL stays through it, goes back at the next step */
    t = pattern(2);
    t->eng_req = 0;
    t->p[P_ROLL] = 0;
    motion_set_lock(t, 2, P_ROLL, 3);
    motion_set_lock(t, 2, P_RPIT, 5);
    motion.on |= 1u;
    motion_step(t, 1, &motion, 1, 0);
    motion_step(t, 2, &motion, 1, 0);
    ok = t->p[P_ROLL] == 3 && t->p[P_RPIT] == 5;
    motion_step(t, 3, &motion, 1, 1);                  /* the TIE */
    ok &= t->p[P_ROLL] == 3 && t->p[P_RPIT] == 5;
    motion_step(t, 4, &motion, 1, 0);                  /* the next note */
    ok &= t->p[P_ROLL] == 0 && t->p[P_RPIT] == 0;
    bad += check("a ROLL / RPIT lock lasts the whole note: held through its TIE steps, back at the next step", ok);
    go_fx();
    t->eng_req = 0;
    ui.cursor = 2;
    kt(EN_K3, 3);
    ok = locked(t, 2, P_RPIT) && !locked(t, 2, P_E2);
    bad += check("STEP FX on a non-SLICE track: KNOB 3 locks the roll's pitch change (RPIT)", ok);
    printf("%s\n", bad ? "STEP FX TEST FAILED" : "step fx tests passed");
    return bad != 0;
}
