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
    /* BANK FULL: PSLOTS entries shared; past them a launch / copy is refused and nothing is lost */
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
        bad += check("BANK FULL: the shared entries used up, the next is refused", ok);
    }

    /* ---- GLO > LAUNCH on the white keys, then the ORDER: record the launches, play them back, edit the rows ---- */
    ui_power_on();
    t = &trk[0];
    my_steps(t);
    go_page(GR_LAUNCH); frame();
    tap_key(key_at(0, 2));                            /* stopped: the third white key = pattern 3, at once */
    ok = t->pat == 2 && trk[0].pat_q == 0;
    bad += check("LAUNCH: a white key launches its pattern (stopped: at once)", ok);
    ok = (launch_leds() & (1u << key_at(0, 2))) && !(launch_leds() & (1u << key_at(0, 0)));
    bad += check("  its LED shows the pattern playing", ok);
    {   /* WHEN = NOW, playing: the key restarts the pattern from step 0, the same pattern too */
        ui.l_scope = 1; ui.l_now = 1;
        song.playing = 1;
        t->seq_idx = 9; t->seq_pos = 1234;
        tap_key(key_at(0, 2));
        ok = t->pat == 2 && t->seq_idx == (uint16_t)(t->p[P_SLEN] - 1) && t->seq_pos == 0x7FFFFFFF;
        song.playing = 0; ui.l_now = 0; ui.l_scope = 0;
        bad += check("LAUNCH NOW: the key retriggers the playing pattern from its first step", ok);
    }
    tap_key(key_at(0, 0));
    t->step[2].n = 1; t->step[2].note[0] = 66; t->step[2].time = ST_NOTE;       /* pattern 1 gets a note */
    memset(pbank_idx, 0, sizeof pbank_idx);
    memset(&pseq, 0, sizeof pseq);
    pbank_launch(t, 0);
    pseq_mode = PSEQ_REC;
    seq_start();
    ok = pseq.n[0] == 1 && pseq.row[0][0].pat == 0 && pseq.row[0][0].bars == 1 && pseq.n[1] == 1;
    bad += check("ORDER REC: PLAY starts a new order with the pattern each track is on", ok);
    pbank_launch(t, 3);                               /* queued: the next bar line */
    to_bar();                                         /* bar 2 starts: pattern 4 */
    to_bar();                                         /* bar 3: still 4 */
    pbank_launch(t, 0);
    to_bar();                                         /* bar 4: back on 1 */
    ok = pseq.n[0] == 3 && pseq.row[0][0].pat == 0 && pseq.row[0][0].bars == 1 && pseq.row[0][1].pat == 3 &&
         pseq.row[0][1].bars == 2 && pseq.row[0][2].pat == 0 && pseq.row[0][2].bars == 1;
    ok &= pseq.n[1] == 1 && pseq.row[1][0].bars == 4;
    bad += check("  each launch a row: pattern 1 x1, pattern 4 x2, pattern 1 x1; an untouched track one long row", ok);
    seq_stop();
    ok = pseq_mode == PSEQ_PLAY;
    bad += check("  STOP ends the recording: MODE is PLAY", ok);
    pbank_launch(t, 5);
    pseq_mode = PSEQ_PLAY;
    seq_start();
    ok = t->pat == 0 && pseq_pos[0] == 0 && pseq_left[0] == 1;
    bad += check("ORDER PLAY: PLAY starts row 1 (its pattern)", ok);
    to_bar();
    ok = t->pat == 3 && pseq_pos[0] == 1 && pseq_left[0] == 2;
    to_bar();
    ok &= t->pat == 3 && pseq_left[0] == 1;
    to_bar();
    ok &= t->pat == 0 && pseq_pos[0] == 2;
    to_bar();
    ok &= t->pat == 0 && pseq_pos[0] == 0 && t->step[2].note[0] == 66;
    bad += check("  the rows follow at each bar line (1, 4, 4, 1) and loop", ok);
    seq_stop();
    pseq_mode = PSEQ_OFF;
    seq_start();
    ok = pseq_pos[0] == 0 && t->pat == 0;
    for (i = 0; i < 4u; i++) to_bar();
    ok &= t->pat == 0;
    seq_stop();
    bad += check("ORDER OFF: the order is left alone while the launches stay manual", ok);

    /* the page: ROW, PAT, BARS, MODE */
    go_page(GR_ORDER); frame();
    pseq_mode = PSEQ_OFF;
    ui.o_row = 0;
    turn(EN_K1, 1);
    ok = ui.o_row == 1u;
    turn(EN_K2, 2);
    ok &= pseq.row[0][1].pat == 5;
    turn(EN_K3, 3);
    ok &= pseq.row[0][1].bars == 5;
    bad += check("ORDER page: KNOB 1 the row, 2 its pattern, 3 its bars", ok);
    turn(EN_K1, 5);
    ok = ui.o_row == 3u && pseq.n[0] == 3;           /* + ADD: one past the last row */
    turn(EN_K3, 1);
    ok &= pseq.n[0] == 4 && pseq.row[0][3].pat == pseq.row[0][2].pat && pseq.row[0][3].bars == 2;
    bad += check("  + ADD: turning PAT or BARS there makes a new row like the last", ok);
    turn(EN_K3, -5);
    ok = pseq.n[0] == 3 && ui.o_row == 3u;
    bad += check("  BARS below 1 deletes the row", ok);
    turn(EN_K4, 1);
    ok = pseq_mode == PSEQ_PLAY;
    turn(EN_K4, 1);
    ok &= pseq_mode == PSEQ_REC;
    turn(EN_K4, 5);
    ok &= pseq_mode == PSEQ_REC;
    turn(EN_K4, -2);
    ok &= pseq_mode == PSEQ_OFF;
    bad += check("  MODE: OFF, PLAY, REC", ok);
    {
        uint32_t n0 = pseq.n[0];
        pseq_mode = PSEQ_PLAY;
        seq_start();
        turn(EN_K4, 1);
        ok = pseq_mode == PSEQ_PLAY;                   /* not while playing */
        turn(EN_K1, -9); turn(EN_K2, 1); turn(EN_K3, 1);
        ok &= pseq.n[0] == n0;                         /* rows still editable while PLAY plays */
        pseq.n[0] = 1;
        to_bar();                                      /* a row index past the end after an edit: back to row 1 */
        ok &= pseq_pos[0] == 0;
        seq_stop();
        bad += check("  while playing: MODE stays; rows can be edited and the position stays valid", ok);
    }

    /* SAVE > PATTERNS: SLOT, SCOPE, LOAD / STORE, READ / WRITE */
    go_page(GR_PBANK); frame();
    memset(pbank_idx, 0, sizeof pbank_idx);
    pbank_launch(t, 0);
    t->step[7].n = 1; t->step[7].note[0] = 70; t->step[7].time = ST_NOTE;
    ui.l_dest = 0;
    ui.l_scope = 0;
    turn(EN_K1, 4);
    ok = ui.l_dest == 4u;
    turn(EN_K3, 1);                                  /* SCOPE ALL: every track goes into slot 5 */
    {
        uint32_t k;
        for (k = 0; k < NTRK; k++)
            ok &= pbank_get(k, 4) != NULL;
        ok &= pbank_get(0, 4)->step[7].note[0] == 70;
    }
    bad += check("PATTERNS: KNOB 1 the slot, KNOB 3 right stores every track's playing pattern in it (SCOPE ALL)", ok);
    turn(EN_K3, -1);
    ok = t->pat == 4 && t->step[7].note[0] == 70;
    {
        uint32_t k;
        for (k = 0; k < NTRK; k++)
            ok &= trk[k].pat == 4u;
    }
    bad += check("  KNOB 3 left loads the slot into every track", ok);
    /* one track only */
    turn(EN_K2, 2);
    ok = ui.l_scope == 2u;
    ui.l_dest = 6;
    trk[1].step[3].n = 1; trk[1].step[3].note[0] = 61; trk[1].step[3].time = ST_NOTE;
    turn(EN_K3, 1);
    ok &= pbank_get(1, 6) && pbank_get(1, 6)->step[3].note[0] == 61 && !pbank_get(0, 6) && !pbank_get(2, 6);
    bad += check("  SCOPE T2: STORE touches only track 2", ok);
    turn(EN_K3, -1);
    ok = trk[1].pat == 6u && trk[0].pat == 4u && trk[2].pat == 4u;
    bad += check("  SCOPE T2: LOAD moves only track 2", ok);
    ui.l_scope = 0;
    turn(EN_K2, 10);
    ok = ui.l_scope == NTRK;
    turn(EN_K2, -10);
    ok &= ui.l_scope == 0u;
    ui.l_scope = 0;
    bad += check("  KNOB 2 scrolls ALL, T1..T4", ok);
    turn(EN_K4, 1);
    ok = pbf_req == 1u;
    pbf_req = 0;
    turn(EN_K4, -1);
    ok &= pbf_req == 2u;
    pbf_req = 0;
    seq_start();
    turn(EN_K3, 1);
    ok &= pbf_req == 0;
    seq_stop();
    bad += check("  KNOB 4 right asks for a WRITE, left a READ (not while playing)", ok);
    printf("%s\n", bad ? "PATTERN TEST FAILED" : "pattern tests passed");
    return bad != 0;
}
