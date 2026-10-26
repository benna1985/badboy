/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* RAND (P_RND, ROLL page KNOB 3; seq.c seq_rand): the pattern stays, each pass is varied as it plays
 *   OFF       nothing changes: notes, ROLL, STRCH, PTCH as written
 *   STEPS     steps drop out (fewer note-ons), NOTES move (rnd_off), FX: ROLL repeats (more note-ons)
 *   SLICE     F wanders STRCH (when on) and PTCH; all put back at the next step, and at STOP
 *   locks     a step's own ROLL lock wins over FX
 *   the page  SEQ > RAND: KNOB 1 WHAT (OFF .. ALL), KNOB 2 AMOUNT (0..15), packed in P_RND */
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
    t->p[P_RND] = 14 << 3 | 1;                       /* STEPS at 93 % */
    r = run(t, 12);
    bad += check("STEPS 93 %: steps drop out (fewer note-ons), none moves, no ROLL", r.on < base.on && r.mv == 0 && r.roll == 1u);
    t->p[P_RND] = 14 << 3 | 2;                       /* NOTES */
    r = run(t, 12);
    bad += check("NOTES: every note-on kept, notes move (up to a few semitones; the steps' chord moves together)",
                 r.on == base.on && r.mv >= 2u && r.mv <= 12u && r.roll == 1u);
    t->p[P_RND] = 14 << 3 | 4;                       /* FX */
    r = run(t, 12);
    bad += check("FX: ROLL gets rates during the run (more note-ons), put back at STOP",
                 r.on > base.on && (r.roll & ~1u) && t->p[P_ROLL] == 0 && !t->rnd_on);
    t->p[P_RND] = 14 << 3 | 7;                       /* ALL */
    r = run(t, 12);
    bad += check("ALL: all three at once, ROLL back at STOP", r.mv >= 2u && (r.roll & ~1u) && t->p[P_ROLL] == 0);
    t->p[P_RND] = 4 << 3 | 1;                        /* STEPS at 26 % */
    r = run(t, 12);
    bad += check("STEPS 26 % drops fewer steps than 93 %", r.on < base.on && r.on > run(t, 12).on / 2u);
    t->p[P_RND] = 0;
    r = run(t, 12);
    bad += check("back to OFF: as written again", r.on == base.on && r.mv == 0 && r.roll == 1u);

    /* a step's own ROLL lock wins */
    t = pattern(0);
    t->p[P_RND] = 14 << 3 | 4;
    {
        uint32_t i;
        for (i = 0; i < 16u; i++)
            (void)motion_set_lock(t, i, P_ROLL, 2);
        r = run(t, 6);
        bad += check("a ROLL lock on every step: FX leaves ROLL to the locks (1/16 on all)", r.roll == (1u << 2) && t->p[P_ROLL] == 0);
    }
    /* SLICE: STRCH (on) and PTCH wander, put back */
    t = pattern(0);
    t->eng_req = ENGI_SLICE;
    t->p[P_E2] = 64;
    t->p[P_E3] = 0;
    t->p[P_RND] = 14 << 3 | 4;
    r = run(t, 12);
    ok = r.smin != r.smax && r.pmin != r.pmax && r.smin >= 1u && r.smax <= 127u && t->p[P_E2] == 64 && t->p[P_E3] == 0;
    bad += check("FX on SLICE: STRCH and PTCH wander, both put back at STOP", ok);
    t->p[P_E2] = 0;                                     /* STRCH off stays off */
    r = run(t, 12);
    bad += check("  STRCH off stays off (its timeline is not touched); PTCH still moves", r.smax == 0u && r.pmin != r.pmax);
    /* the FX last the note, within the limits; the note lengthens at a high AMOUNT; the pitch has a slice */
    {
        uint32_t b, blocks, rolls = 0, ext = 0, tie_keep = 0, tie_n = 0, rp_ok = 1, pt_ok = 1, pitch_ok = 1, moved = 0;
        t = pattern(0);
        t->eng_req = ENGI_SLICE;
        usr_nz[0] = 1;                                  /* a user sample in USR1: its 16 slices (DIV 1/16) */
        slc_usr[0].len = 4000;
        t->p[P_E0] = 1;
        t->p[P_E1] = 2;                                 /* DIV 1/16: 16 slices */
        t->p[P_E2] = 64;
        t->p[P_E3] = 0;
        t->p[P_RND] = 15 << 3 | 7;                      /* ALL at 100 % */
        blocks = (div_samples(2) / CTL * 16u - 1u) * 30u;
        seq_start();
        for (b = 0; b < blocks; b++) {
            uint32_t idx;
            seq_tick(t, CTL);
            idx = t->seq_idx;
            if (t->p[P_ROLL] && !((t->p[P_ROLL] >= 2 && t->p[P_ROLL] <= 4))) rolls = 99;
            else if (t->p[P_ROLL]) rolls |= 1u << t->p[P_ROLL];
            if (t->p[P_RPIT] < -5 || t->p[P_RPIT] > 5) rp_ok = 0;
            if (t->p[P_E3] < -5 || t->p[P_E3] > 5) pt_ok = 0;
            if (t->rnd_ext) {
                ext = t->rnd_ext > ext ? t->rnd_ext : ext;
                if (seq_time_at(t, idx) == ST_TIE) {        /* inside the lengthened note: the FX are still on */
                    tie_n++;
                    tie_keep += t->rnd_on && t->p[P_ROLL] >= 2;
                }
            }
            if (t->rnd_off && seq_time_at(t, idx) != ST_TIE && trk[0].step[idx].time == ST_NOTE && trk[0].step[idx].n) {
                uint32_t j;
                moved++;
                for (j = 0; j < trk[0].step[idx].n; j++) {
                    int32_t rel = (int32_t)trk[0].step[idx].note[j] + t->rnd_off - 60 - t->p[P_ROOT];
                    if (rel < 0 || rel >= 16) pitch_ok = 0;
                }
            }
        }
        seq_stop();
        ok = rolls != 99u && (rolls & 0x1Cu) == 0x1Cu && !(rolls & 0x2u);
        bad += check("RAND FX: ROLL is 1/16, 1/32 or 1/64 (all three seen, never 1/8 or the triplets)", ok);
        bad += check("  the roll's pitch change and PTCH stay within 5 semitones", rp_ok && pt_ok);
        bad += check("  at 100 % a note is lengthened (up to 3 steps more), the FX still on through them",
                     ext >= 1u && ext <= 3u && tie_n > 0 && tie_keep == tie_n);
        bad += check("  NOTES on SLICE: every moved pitch is one of the 16 slices", moved > 0 && pitch_ok);
        usr_nz[0] = 0;
        slc_usr[0].len = 0;
        ok = t->p[P_ROLL] == 0 && t->p[P_RPIT] == 0 && t->p[P_E3] == 0 && t->p[P_E2] == 64;
        bad += check("  put back at STOP", ok);
    }
    {   /* a low AMOUNT never lengthens; the real TIE of step 3 keeps the FX of its note */
        uint32_t b, blocks, ext = 0, keep = 0, n = 0;
        t = pattern(0);
        t->p[P_RND] = 4 << 3 | 4;                       /* FX at 27 % */
        blocks = (div_samples(2) / CTL * 16u - 1u) * 60u;
        seq_start();
        for (b = 0; b < blocks; b++) {
            seq_tick(t, CTL);
            ext |= t->rnd_ext;
            if (t->seq_idx == 3u && t->rnd_on) { n++; keep += t->p[P_ROLL] >= 2; }
        }
        seq_stop();
        bad += check("RAND FX at 27 %: no lengthening; a real TIE keeps its note's ROLL", ext == 0u && n > 0 && keep == n);
    }
    /* the page: SEQ > RAND, KNOB 1 WHAT, KNOB 2 AMOUNT */
    t = pattern(0);
    {
        uint32_t i;
        for (i = 0; i < NPAGES && !str_eq(PAGES[i].title, "RAND"); i++)
            ;
        ui.home = 0; ui.page = (uint8_t)i; page_entered(); frame();
        ok = rand_on() && t->p[P_RND] == 0;
        kt(EN_K1, 1);                                   /* OFF -> STEPS, a middle AMOUNT */
        ok &= (t->p[P_RND] & 7) == 1 && (t->p[P_RND] >> 3) == 8;
        kt(EN_K2, 3);
        ok &= (t->p[P_RND] >> 3) == 11 && (t->p[P_RND] & 7) == 1;
        kt(EN_K1, 20);
        ok &= (t->p[P_RND] & 7) == 7 && (t->p[P_RND] >> 3) == 11;
        kt(EN_K2, 50);
        ok &= (t->p[P_RND] >> 3) == 15;
        kt(EN_K2, -50);
        ok &= (t->p[P_RND] >> 3) == 0 && (t->p[P_RND] & 7) == 7;
        kt(EN_K1, -20);
        ok &= (t->p[P_RND] & 7) == 0;
        bad += check("RAND page: KNOB 1 WHAT (OFF .. ALL), KNOB 2 AMOUNT (0..15), each its own half of P_RND", ok);
        t->p[P_RND] = 0;
        kt(EN_K2, 6);                                   /* AMOUNT with WHAT OFF: nothing varies */
        r = run(t, 12);
        bad += check("  WHAT OFF with an AMOUNT: the pattern as written", r.on == base.on && r.mv == 0 && r.roll == 1u);
    }
    printf("%s\n", bad ? "RAND TEST FAILED" : "rand tests passed");
    return bad != 0;
}
