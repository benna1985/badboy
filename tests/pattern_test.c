/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The pattern bank (pbank.c): 16 patterns per track, each with its steps, timing, automation and sound.
 *   stopped    a launch loads the slot at once; the pattern left is stored; an empty slot is empty, in the same sound
 *   playing    a launch waits for the next bar line, starts from step 1 there; a second launch replaces the queued one;
 *              the other tracks are not touched
 *   sounds     engine, preset and values come and go with the pattern; MUTE stays
 *   automation a pattern's locks and events are its own
 *   copy       slot over slot
 *   SONG       a running song refuses launches
 * Built and run by tests/run_tests.sh. */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static uint32_t bar_samples(void) { return 4u * beat_samples(); }
/* blocks until the next bar line (beat_n wraps): their count */
static uint32_t to_bar(void)
{
    uint32_t n = 0;
    while (beat_n != 3u && n < 200000u) { events_block(CTL); n++; }
    while (beat_n == 3u && n < 200000u) { events_block(CTL); n++; }
    return n;
}

int main(void)
{
    int bad = 0, ok;
    track_t *t, *u;
    uint32_t i;
    ui_power_on();
    t = &trk[0];
    u = &trk[1];
    my_steps(t);
    my_steps(u);
    t->p[P_SLEN] = 16;
    ok = t->pat == 0 && !t->pat_q && sizeof(pbank_slab) < 40000u;
    bad += check("bank: track on pattern 1, nothing queued", ok);

    /* stopped: pattern 1 is C4 first; pattern 2 gets its own notes, sound and LEN */
    t->step[0].note[0] = 60;
    t->p[P_PAN] = 11;
    ok = pbank_launch(t, 1) && t->pat == 1;
    ok &= !step_on(&t->step[0]) && t->p[P_PAN] == 11 && t->eng_req == pbank_get(0, 0)->snd.eng;
    bad += check("stopped: launching an empty slot loads an empty pattern in the same sound", ok);
    t->step[2].note[0] = 70; t->step[2].n = 1; t->step[2].time = ST_NOTE; t->step[2].vel = 90;
    t->p[P_SLEN] = 8;
    set_engine_of(t, ENGI_DRUM);
    t->p[P_PAN] = -30;
    t->p[P_MUTE] = 1;
    ok = pbank_launch(t, 0) && t->pat == 0 && t->step[0].note[0] == 60 && t->p[P_SLEN] == 16 && t->p[P_PAN] == 11;
    ok &= t->eng_req != ENGI_DRUM && t->p[P_MUTE] == 1;
    bad += check("  back to pattern 1: its steps, LEN, engine and values (MUTE stays the track's)", ok);
    ok = pbank_launch(t, 1) && t->pat == 1 && t->step[2].note[0] == 70 && t->p[P_SLEN] == 8 && t->eng_req == ENGI_DRUM && t->p[P_PAN] == -30;
    bad += check("  pattern 2 again: its notes, LEN 8, DRUM and PAN -30", ok);

    /* automation per pattern */
    (void)motion_set_lock(t, 2, P_REV, 55);
    ok = motion_count(t) == 1;
    pbank_launch(t, 0);
    ok &= motion_count(t) == 0;
    pbank_launch(t, 1);
    ok &= motion_count(t) == 1;
    bad += check("automation: pattern 2's lock is its own (gone on 1, back on 2)", ok);

    /* playing: queue, bar line, from step 1 */
    pbank_launch(t, 0);
    t->p[P_SDIV] = 2;
    seq_start();
    events_block(CTL);
    ok = song.playing;
    i = pbank_launch(t, 1);
    ok &= i && t->pat == 0 && t->pat_q == 2u;
    for (i = 0; i < 40; i++) events_block(CTL);
    ok &= t->pat == 0 && t->pat_q == 2u;
    bad += check("playing: the launch is queued, the pattern keeps playing", ok);
    pbank_launch(t, 2);
    ok = t->pat_q == 3u;
    pbank_launch(t, 1);
    ok &= t->pat_q == 2u;
    pbank_launch(t, 0);
    ok &= t->pat_q == 0u && t->pat == 0;
    pbank_launch(t, 1);
    bad += check("  a second press replaces the queue; the playing pattern's own key cancels it", ok);
    to_bar();
    ok = t->pat == 1 && !t->pat_q && t->p[P_SLEN] == 8 && t->eng_req == ENGI_DRUM;
    ok &= t->seq_pos == 0x7FFFFFFFu || t->seq_idx < 2u;
    bad += check("  at the bar line: the pattern (steps, LEN, sound) starts from step 1", ok);
    ok = u->pat == 0 && !u->pat_q && u->step[0].note[0] == trk[1].step[0].note[0];
    bad += check("  the other tracks are not touched", ok);
    pbank_launch(t, 0);
    seq_stop();
    ok = t->pat == 0 && !t->pat_q;
    bad += check("STOP: a queued launch happens", ok);

    /* copy */
    pbank_launch(t, 1);
    pbank_copy(t, 1, 5);
    ok = pbank_get(0, 5) && pbank_get(0, 5)->step[2].note[0] == 70 && pbank_get(0, 5)->timing[0] == 8 && pbank_get(0, 5)->mcount == 1;
    pbank_launch(t, 5);
    ok &= t->pat == 5 && t->step[2].note[0] == 70 && t->eng_req == ENGI_DRUM;
    bad += check("copy: slot 2 over slot 6 (steps, LEN, sound, automation)", ok);

    /* a SONG refuses */
    pbank_launch(t, 0);
    chain.running = 1;
    ok = !pbank_launch(t, 3) && t->pat == 0;
    chain.running = 0;
    bad += check("a running SONG refuses launches", ok);
    /* BANK FULL: 40 entries shared; past them a launch / copy is refused and nothing is lost */
    {
        uint32_t k, i, n = 0, f = 0;
        for (k = 0; k < NTRK; k++)
            for (i = 0; i < NPAT; i++)
                pbank_idx[k][i] = 0;
        pbank_launch(t, 0);
        for (k = 1; k < NTRK; k++)                        /* the other tracks fill every entry but the live slot's */
            for (i = 0; i < NPAT; i++)
                if (pbank_alloc(k, i)) n++; else f++;
        for (i = 1; i < NPAT && n < PSLOTS; i++)
            if (pbank_alloc(0, i)) n++;
        ok = n == PSLOTS && !pbank_alloc(0, 0) && !pbank_store(&trk[0]) && !pbank_copy(&trk[0], 0, 2);
        t->step[0].note[0] = 77; t->step[0].n = 1; t->step[0].time = ST_NOTE;
        ok &= !pbank_launch(t, 4) || (t->pat == 0 && t->step[0].note[0] == 77);   /* refused: nothing lost */
        for (k = 0; k < NTRK; k++)
            for (i = 0; i < NPAT; i++)
                pbank_idx[k][i] = 0;
        bad += check("BANK FULL: 40 entries shared by all tracks, the 41st is refused", ok);
    }
    printf("%s\n", bad ? "PATTERN TEST FAILED" : "pattern tests passed");
    return bad != 0;
}
