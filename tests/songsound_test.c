/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SONG (GLO) plays the sounds of the slots as well as their patterns (song_chain.c chain_sound_set):
 *   a section's track has the engine, preset and sound values its slot's project had for it
 *   LEN DIV SWING GATE QUANTIZE come from the slot's sequence as before; MUTE stays the track's own
 *   a silent lane keeps the sound of the section before
 *   STOP (and the song's end) brings back the sounds the tracks had before PLAY
 *   the saved projects stay untouched
 * Built and run by tests/run_tests.sh. */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

/* slot A and B sounds: the engine and a PAN / DIST each track gets in them */
static const uint8_t ENG_A[NTRK] = {ENGI_DRUM, ENGI_SAMPLE, ENGI_PHYS, ENGI_DRUM};
static const uint8_t ENG_B[NTRK] = {0, 2, 0, 5};
static void make_slot(uint32_t slot, const uint8_t *eng, int16_t pan0, int16_t dist0)
{
    uint32_t k;
    for (k = 0; k < NTRK; k++) {
        set_engine_of(&trk[k], eng[k]);
        trk[k].p[P_PAN] = (int16_t)(pan0 + (int)k);
        trk[k].p[P_DIST] = (int16_t)(dist0 + 3 * (int)k);
    }
    project_save(slot);
}

int main(void)
{
    int bad = 0, ok;
    uint32_t k, n;
    int16_t own[P_COUNT];
    uint8_t own_eng, own_pre;
    ui_power_on();
    for (k = 0; k < NTRK; k++)
        my_steps(&trk[k]);
    make_slot(0, ENG_A, -20, 30);
    make_slot(1, ENG_B, 40, 70);
    /* the working sounds: something else again, MUTE on track 1 */
    set_engine_of(&trk[0], ENGI_SLICE);
    trk[0].p[P_PAN] = 5;
    trk[0].p[P_MUTE] = 1;
    memcpy(own, trk[0].p, sizeof own);
    own_eng = trk[0].eng_req;
    own_pre = trk[0].preset;
    chain_config.count = 3;
    chain_config.row[0] = (chain_row_t){{0, 0, 1, 1}, 1};
    chain_config.row[1] = (chain_row_t){{1, CHAIN_SILENT, 0, 1}, 1};
    chain_config.row[2] = (chain_row_t){{0, 0, 0, 0}, 1};
    ok = chain_prepare() == 0;
    events_block(32);
    ok &= chain.running && chain.row == 0u;
    ok &= trk[0].eng_req == ENG_A[0] && trk[1].eng_req == ENG_A[1] && trk[0].p[P_PAN] == -20 && trk[1].p[P_DIST] == 33;
    ok &= trk[2].eng_req == ENG_B[2] && trk[3].eng_req == ENG_B[3] && trk[2].p[P_PAN] == 42 && trk[3].p[P_DIST] == 79;
    bad += check("SONG section 1: T1 and T2 play slot A's engines and values, T3 and T4 slot B's", ok);
    ok = trk[0].p[P_MUTE] == 1 && trk[0].p[P_SLEN] == chain.source[0].timing[0][0];
    bad += check("  MUTE stays the track's own; LEN comes from the slot's sequence", ok);
    n = 0;
    while (n++ < 20000u && chain.running && chain.row == 0u)
        events_block(32);
    ok = chain.running && chain.row == 1u && trk[0].eng_req == ENG_B[0] && trk[0].p[P_PAN] == 40 && trk[0].p[P_DIST] == 70;
    ok &= trk[1].eng_req == ENG_A[1] && trk[1].p[P_DIST] == 33;             /* the silent lane keeps the sound before */
    ok &= trk[2].eng_req == ENG_A[2] && trk[2].p[P_PAN] == -18 && trk[3].eng_req == ENG_B[3];
    bad += check("SONG section 2: T1 slot B's sound, T2 (silent) keeps A's, T3 slot A's, T4 slot B's", ok);
    n = 0;
    while (n++ < 40000u && chain.running)
        events_block(32);
    ok = !chain.running && !song.playing;
    ok &= trk[0].eng_req == own_eng && trk[0].preset == own_pre && !memcmp(trk[0].p, own, sizeof own);
    bad += check("SONG end: T1 has its own sound back (engine, preset, every value)", ok);
    ok = project_used(0) && project_used(1);
    bad += check("  the saved projects are untouched", ok);
    printf("%s\n", bad ? "SONG SOUND TEST FAILED" : "song sound tests passed");
    return bad != 0;
}
