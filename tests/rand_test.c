/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* RAND (P_RND, ROLL page KNOB 3; seq.c seq_rand): the pattern stays, each pass is varied as it plays
 *   OFF       nothing changes: notes, ROLL, STRCH, PTCH as written
 *   S         steps drop out (fewer note-ons), N notes move (rnd_off), F ROLL repeats (more note-ons)
 *   SLICE     F wanders STRCH (when on) and PTCH; all put back at the next step, and at STOP
 *   locks     a step's own ROLL lock wins over F
 *   the page  KNOB 3 on ROLL walks OFF S1 .. SNF3 */
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
/* one run of `passes` passes of the 16 steps from PLAY: the note-ons, the largest move seen, the ROLL values seen (bit
 * per value), the STRCH / PTCH seen (min and max) */
typedef struct { uint32_t on, mv, roll, smin, smax, pmin, pmax; } run_t;
static run_t run(track_t *t, uint32_t passes)
{
    run_t r = {0, 0, 0, 999, 0, 999, 0};
    uint32_t b, blocks = (div_samples(2) / CTL * 16u - 1u) * passes;
    seq_start();
    for (b = 0; b < blocks; b++) {
        uint32_t v0 = vage;
        seq_tick(t, CTL);
        r.on += vage - v0;
        if (t->rnd_off) { uint32_t m = (uint32_t)(t->rnd_off < 0 ? -t->rnd_off : t->rnd_off); r.mv = m > r.mv ? m : r.mv; }
        r.roll |= 1u << (t->p[P_ROLL] & 7);
        if ((uint32_t)t->p[P_E2] < r.smin) r.smin = (uint32_t)t->p[P_E2];
        if ((uint32_t)t->p[P_E2] > r.smax) r.smax = (uint32_t)t->p[P_E2];
        if ((uint32_t)(t->p[P_E3] + 100) < r.pmin) r.pmin = (uint32_t)(t->p[P_E3] + 100);
        if ((uint32_t)(t->p[P_E3] + 100) > r.pmax) r.pmax = (uint32_t)(t->p[P_E3] + 100);
    }
    seq_stop();
    return r;
}
static int locked(track_t *t, uint32_t s, uint32_t id) { int16_t v; return motion_lock_get(t, s, id, &v); }

int main(void)
{
    int bad = 0, ok;
    track_t *t = pattern(0);
    run_t base, r;
    base = run(t, 12);
    ok = base.mv == 0 && base.roll == 1u && base.on > 0;
    bad += check("RAND OFF: no move, no ROLL: the pattern as written", ok);
    t->p[P_RND] = 3;                                    /* S3 */
    r = run(t, 12);
    bad += check("S3: steps drop out (fewer note-ons), none moves, no ROLL", r.on < base.on && r.mv == 0 && r.roll == 1u);
    t->p[P_RND] = 6;                                    /* N3 */
    r = run(t, 12);
    bad += check("N3: every note-on kept, notes move (up to a few semitones; the steps' chord moves together)",
                 r.on == base.on && r.mv >= 2u && r.mv <= 12u && r.roll == 1u);
    t->p[P_RND] = 12;                                   /* F3 */
    r = run(t, 12);
    bad += check("F3: ROLL gets rates during the run (more note-ons), put back at STOP",
                 r.on > base.on && (r.roll & ~1u) && t->p[P_ROLL] == 0 && !t->rnd_on);
    t->p[P_RND] = 21;                                   /* SNF3 */
    r = run(t, 12);
    bad += check("SNF3: all three at once, ROLL back at STOP", r.mv >= 2u && (r.roll & ~1u) && t->p[P_ROLL] == 0);
    t->p[P_RND] = 1;                                    /* S1 at 25 %, 15 % drop */
    r = run(t, 12);
    bad += check("S1 drops fewer steps than S3", r.on < base.on && r.on > run(t, 12).on / 2u);
    t->p[P_RND] = 0;
    r = run(t, 12);
    bad += check("back to OFF: as written again", r.on == base.on && r.mv == 0 && r.roll == 1u);

    /* a step's own ROLL lock wins */
    t = pattern(0);
    t->p[P_RND] = 12;
    {
        uint32_t i;
        for (i = 0; i < 16u; i++)
            (void)motion_set_lock(t, i, P_ROLL, 2);
        r = run(t, 6);
        bad += check("a ROLL lock on every step: F leaves ROLL to the locks (1/16 on all)", r.roll == (1u << 2) && t->p[P_ROLL] == 0);
    }
    /* SLICE: STRCH (on) and PTCH wander, put back */
    t = pattern(0);
    t->eng_req = ENGI_SLICE;
    t->p[P_E2] = 64;
    t->p[P_E3] = 0;
    t->p[P_RND] = 12;
    r = run(t, 12);
    ok = r.smin != r.smax && r.pmin != r.pmax && r.smin >= 1u && r.smax <= 127u && t->p[P_E2] == 64 && t->p[P_E3] == 0;
    bad += check("F3 on SLICE: STRCH and PTCH wander, both put back at STOP", ok);
    t->p[P_E2] = 0;                                     /* STRCH off stays off */
    r = run(t, 12);
    bad += check("  STRCH off stays off (its timeline is not touched); PTCH still moves", r.smax == 0u && r.pmin != r.pmax);
    /* the page */
    t = pattern(0);
    {
        uint32_t i;
        char v[16];
        for (i = 0; i < NPAGES && !str_eq(PAGES[i].title, "ROLL"); i++)
            ;
        ui.home = 0; ui.page = (uint8_t)i; page_entered(); frame();
        kt(EN_K3, 1);
        ok = t->p[P_RND] == 1;
        kt(EN_K3, 20);
        ok &= t->p[P_RND] == 21;
        param_format(&TP[P_RND], t->p[P_RND], v, &(const char *){0});
        ok &= str_eq(v, "SNF3");
        kt(EN_K3, -30);
        ok &= t->p[P_RND] == 0;
        bad += check("ROLL page KNOB 3 walks OFF, S1 .. SNF3 (22 values)", ok);
    }
    printf("%s\n", bad ? "RAND TEST FAILED" : "rand tests passed");
    return bad != 0;
}
