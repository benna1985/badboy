/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The pattern bank in flash (pbank_flash.c): written when asked and stopped (SAVE > PATTERNS), read back whole (the steps,
 * sound, automation of each slot, the ORDER), a torn write or a changed byte leaves the RAM bank as it was, junk ignored.
 * Built and run by tests/run_tests.sh. */
#include <stdarg.h>
#include <stdint.h>
static uint8_t host_flash[0xA000u] __attribute__((aligned(4)));          /* 0xD2000..0xDBFFF */
#define PBF_XIP() ((const uint8_t *)host_flash)
#define main hostsim_main
#include "hostsim.c"
#undef main

static uint8_t flash_ok = 1, host_busy, host_torn;
static char host_msg[32];
static struct { int force; } ui;
static int transport_busy(void) { return host_busy; }
static void ui_message(const char *s) { snprintf(host_msg, sizeof host_msg, "%s", s); }
static uint32_t host_erases;
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static int st_read(uint32_t off, void *dst, uint32_t n) { (void)off; (void)dst; (void)n; return -1; }
static int st_erase(uint32_t off)
{
    if (off < 0xD2000u || off + 0x1000u > 0xD2000u + sizeof host_flash || (off & 0xFFFu))
        return -8;
    memset(host_flash + (off - 0xD2000u), 0xFF, 0x1000u);
    host_erases++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    uint32_t i;
    if (off < 0xD2000u || off + n > 0xD2000u + sizeof host_flash)
        return -8;
    if (host_torn && off == 0xD2000u)
        return -1;                                       /* power gone before the header (the commit) */
    for (i = 0; i < n; i++)                              /* NOR: bits only go 1 -> 0 */
        host_flash[off - 0xD2000u + i] &= ((const uint8_t *)src)[i];
    return 0;
}
#include "../firmware/src/storage.c"
#include "../firmware/src/pbank_flash.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("pbank_flash: %-70s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}
/* two patterns the same (a blank step's leftover note bytes do not count: the file keeps no blank step) */
static int pat_same(const pbank_pat_t *a, const pbank_pat_t *b)
{
    uint32_t j;
    pbank_pat_t x = *a, y = *b;
    for (j = 0; j < NSTEP; j++) {
        if (pbf_blank(&x.step[j]))
            memset(&x.step[j], 0, sizeof x.step[j]);
        if (pbf_blank(&y.step[j]))
            memset(&y.step[j], 0, sizeof y.step[j]);
    }
    return !memcmp(&x, &y, sizeof x);
}
static void ram_clear(void)
{
    memset(pbank_idx, 0, sizeof pbank_idx);
    memset(&pseq, 0, sizeof pseq);
}

int main(void)
{
    uint32_t k, i;
    int ok;
    pbank_pat_t ref;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    for (k = 0; k < NTRK; k++) {                         /* four tracks as felucca_init leaves them */
        for (i = 0; i < P_E0; i++)
            trk[k].p[i] = TP[i].def;
        trk[k].eng_req = trk[k].engine = (uint8_t)ENGI_DRUM;
        for (i = 0; i < NSTEP; i++)
            pbank_blank(&trk[k].step[i]);
        trk[k].p[P_SLEN] = 16;
    }
    memset(host_flash, 0xFF, sizeof host_flash);
    ok = pbf_read(0) == 0;
    check("an erased region: nothing to read, the RAM bank stays as it is", ok);

    /* a bank: track 0 pattern 2 (note 61) and 5 (note 64), track 2 pattern 0, the order of track 0 */
    trk[0].step[0].n = 1; trk[0].step[0].note[0] = 60; trk[0].step[0].time = ST_NOTE;
    pbank_switch(&trk[0], 1);
    trk[0].step[0].n = 1; trk[0].step[0].note[0] = 61; trk[0].step[0].time = ST_NOTE;
    trk[0].p[P_PAN] = 17;
    pbank_switch(&trk[0], 4);
    trk[0].step[3].n = 1; trk[0].step[3].note[0] = 64; trk[0].step[3].time = ST_NOTE;
    trk[2].step[1].n = 1; trk[2].step[1].note[0] = 50; trk[2].step[1].time = ST_NOTE;
    pseq.n[0] = 3;
    pseq.row[0][0].pat = 1; pseq.row[0][0].bars = 2;
    pseq.row[0][1].pat = 4; pseq.row[0][1].bars = 1;
    pseq.row[0][2].pat = 0; pseq.row[0][2].bars = 8;
    ok = pbf_write(0) == 0 && host_erases >= 1u;
    check("write: the bank and the order go to flash", ok);
    ref = *pbank_get(0, 1);
    {
        pbank_pat_t keep4 = *pbank_get(0, 4);
        pseq_blob_t pq = pseq;
        ram_clear();
        ok = pbank_get(0, 1) == 0 && pseq.n[0] == 0;
        ok &= pbf_read(0) == 1;
        ok &= pbank_get(0, 1) && pat_same(pbank_get(0, 1), &ref) && pat_same(pbank_get(0, 4), &keep4);
        ok &= !memcmp(&pseq, &pq, sizeof pq);
        check("read: every slot (steps, sound, automation) and the order come back as written", ok);
    }
    ok = pbank_get(0, 1)->step[0].note[0] == 61 && pbank_get(0, 1)->snd.p[P_PAN] == 81 && pbank_get(0, 4)->step[3].note[0] == 64 &&
         pbank_get(2, 0) && pbank_get(2, 0)->step[1].note[0] == 50 && !pbank_get(1, 5) && !pbank_get(0, 7);
    check("read: the stored notes and sound, the slots that were empty are empty", ok);

    /* what the file keeps: a header over the packed payload, the sound once per track while it does not change */
    {
        const pbf_hdr_t *h = (const pbf_hdr_t *)host_flash;
        ok = h->magic == PBF_MAGIC && h->len > 0 && h->len < PBF_CAP && h->len < 3u * sizeof(pbank_pat_t);
        check("the header: magic and the packed payload length (3 patterns: far under their RAM size)", ok);
    }

    /* a changed byte: refused whole, the RAM bank left alone */
    host_flash[PBF_HDR + 40] ^= 0x10;
    trk[1].step[0].n = 1;
    pbank_idx[1][3] = 0;
    ok = pbf_read(0) == 0 && !pbank_get(1, 3) && pbank_get(0, 1);
    host_flash[PBF_HDR + 40] ^= 0x10;
    check("a changed byte in the payload: refused, the RAM bank is left alone", ok);
    ok = pbf_read(0) == 1;
    check("  put back: valid again", ok);

    /* a power cut before the header: nothing valid, no half bank */
    host_torn = 1;
    ok = pbf_write(0) == 1;
    host_torn = 0;
    ok &= pbf_read(0) == 0;
    check("a write torn before its header: SAVE ERROR, flash holds no bank", ok);

    /* junk */
    memset(host_flash, 0xA5, sizeof host_flash);
    ok = pbf_read(0) == 0;
    check("junk in the region: ignored", ok);

    /* pbf_poll: not while the transport runs, WRITE then READ */
    ram_clear();
    pbank_launch(&trk[0], 0);
    trk[0].step[5].n = 1; trk[0].step[5].note[0] = 72; trk[0].step[5].time = ST_NOTE;
    pbank_launch(&trk[0], 0);
    pbf_req = 1; host_busy = 0; pbf_slot = 0xFFu;
    pbf_poll();
    ok = pbf_req == 0 && !strcmp(host_msg, "SAVE A SONG FIRST");
    check("SAVE > PATTERNS with no song saved or loaded yet: SAVE A SONG FIRST", ok);
    pbf_req = 1; host_busy = 1; pbf_slot = 0;
    pbf_poll();
    ok = pbf_req == 1u;
    check("SAVE > PATTERNS WRITE waits while the transport runs", ok);
    host_busy = 0;
    pbf_poll();
    ok = pbf_req == 0 && !strcmp(host_msg, "PATTERNS SAVED") && pbank_get(0, 0) && pbank_get(0, 0)->step[5].note[0] == 72;
    check("  stopped: written (the pattern being played goes back to its slot first)", ok);
    trk[0].step[5].n = 0;
    pbf_req = 2;
    pbf_poll();
    ok = pbf_req == 0 && !strcmp(host_msg, "PATTERNS LOADED") && trk[0].step[5].n == 1 && trk[0].step[5].note[0] == 72;
    check("READ: the bank is back and the tracks play it", ok);
    memset(host_flash, 0xFF, sizeof host_flash);
    pbf_req = 2;
    pbf_poll();
    ok = !strcmp(host_msg, "NO SAVED PATTERNS") && trk[0].step[5].n == 1;
    check("READ with nothing saved: a message, the tracks keep what they play", ok);

    /* songs: each project slot has its own bank */
    memset(host_flash, 0xFF, sizeof host_flash);
    ram_clear();
    memset(pbf_pat, 0, sizeof pbf_pat);
    pbank_launch(&trk[0], 0);
    trk[0].step[1].n = 1; trk[0].step[1].note[0] = 55; trk[0].step[1].time = ST_NOTE;
    pbank_launch(&trk[0], 3);
    trk[0].step[2].n = 1; trk[0].step[2].note[0] = 56; trk[0].step[2].time = ST_NOTE;
    pseq.n[1] = 1; pseq.row[1][0].pat = 3; pseq.row[1][0].bars = 4;
    pbf_song_save(0);
    ok = pbf_slot == 0u;
    ram_clear();
    trk[0].step[1].n = 0;
    pbank_launch(&trk[0], 0);
    trk[0].pat = 9;
    pbank_idx[2][2] = 0;
    pbf_song_save(1);                                    /* another song: an empty-ish bank of its own */
    ok &= pbf_slot == 1u;
    pbf_song_load(0);
    ok &= trk[0].pat == 3u && pbank_get(0, 0) && pbank_get(0, 0)->step[1].note[0] == 55 && pbank_get(0, 3) &&
          pbank_get(0, 3)->step[2].note[0] == 56 && pseq.n[1] == 1 && pseq.row[1][0].bars == 4 && pbf_slot == 0u;
    check("a song's save and load: every pattern, the ORDER and the pattern each track was on", ok);
    pbf_song_load(1);
    ok = !(pbank_get(0, 0) && pbank_get(0, 0)->step[1].note[0] == 55) && pseq.n[1] == 0;
    check("  another song's slot: its own bank, not the first one's", ok);
    pbf_song_load(2);
    ok = !pbank_get(0, 0) && !pbank_get(0, 3) && pseq.n[1] == 0 && trk[0].pat == 0;
    check("  a song with no bank (an old project): the bank is empty, no pattern of another song stays", ok);
    pbf_slot = 0;
    pbf_song_erase(0);
    ok = pbf_read(0) == 0 && pbf_slot == 0xFFu;
    check("  erasing a project takes its bank", ok);

    /* the sound is kept once per track while it does not change: a full bank of 34 patterns fits */
    memset(host_flash, 0xFF, sizeof host_flash);
    ram_clear();
    for (k = 0; k < NTRK; k++)
        for (i = 0; i < NPAT && k * NPAT + i < PSLOTS - 1u; i++) {
            uint32_t j;
            pbank_pat_t *p = pbank_alloc(k, i);
            if (!p)
                break;
            memset(p, 0, sizeof *p);
            p->used = 1;
            for (j = 0; j < NSTEP; j++)
                pbank_blank(&p->step[j]);
            for (j = 0; j < 16; j++) {                   /* a busy 16-step pattern */
                p->step[j].n = 1; p->step[j].note[0] = (uint8_t)(40 + i + j); p->step[j].time = ST_NOTE; p->step[j].vel = 100;
            }
            p->timing[0] = 16;
        }
    ok = pbf_write(1) == 0;
    check("33 busy 16-step patterns (the live ones come with) (one sound per track) fit the 8 KiB of a song", ok);
    {
        uint32_t used = 0;
        pbank_pat_t before[PSLOTS];
        memcpy(before, pbank_slab, sizeof before);
        for (k = 0; k < NTRK * NPAT; k++)
            used += ((uint8_t *)pbank_idx)[k] != 0;
        ram_clear();
        ok = pbf_read(1) == 1;
        for (k = 0; k < used; k++)
            ok &= pat_same(&before[k], &pbank_slab[k]);
        check("  and read back exactly", ok);
    }
    /* patterns that all differ in sound: more than it can hold */
    ram_clear();
    for (i = 0; i < PSLOTS; i++) {
        pbank_pat_t *p = pbank_alloc(i / NPAT, i % NPAT);
        memset(p, 0, sizeof *p);
        p->used = 1;
        p->snd.p[0] = (uint8_t)i;                        /* each its own sound */
        p->mcount = PAT_MOT;
        p->step[0].n = 1;
    }
    ok = pbf_write(2) == 2;
    check("too big for 8 KiB: SONG FULL (2), nothing half written", ok);
    host_busy = 0;

    printf("%s\n", bad ? "PBANK FLASH TEST FAILED" : "pbank flash tests passed");
    return bad != 0;
}
