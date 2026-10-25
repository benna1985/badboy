/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The pattern bank (1.6): 16 patterns per track, each with its own sound.
 *
 * A track's steps, timing, automation and sound are the pattern it plays now (t->pat, 0..15: the live one, what the
 * pages edit). The other fifteen sit in pbank. Launching a pattern (pbank_launch) stores the live one back into its
 * slot and loads the new one into the track: its steps, LEN DIV SWING GATE QUANTIZE, automation and locks, and the
 * sound (engine, preset, every value: song_chain.c chain_sound_set). An empty slot loads as an empty pattern in the
 * sound the track has. Playing, a launch waits for the next bar line (beat_n wraps to 0: the bars of PLAY, 4/4) and
 * the pattern starts from its step 1 there; stopped, it happens at once. Not while a SONG plays (its sections choose).
 *
 * A pattern is stored from RAM into flash by pbank_store.c (pbank_dirty tracks the changed banks).
 * pbank_pat_t is not a project: PROJECT slots and SONG are as before. A project load replaces the live pattern. */
#define NPAT 16
#define PAT_MOT 32                       /* automation and locks kept per pattern (the track's 128 are shared by the four) */
typedef struct {
    uint8_t used, qnt, mon, mcount;      /* used: stored; mon: the track's motion was on; mcount: records kept */
    int16_t timing[4];                   /* LEN DIV SWING GATE */
    chain_snd_t snd;
    motion_event_t mot[PAT_MOT];         /* (place's track bits are not kept: bank[k] is track k's) */
    step_t step[NSTEP];
} pbank_pat_t;
static pbank_pat_t pbank[NTRK][NPAT] __attribute__((section(".pool")));
static uint8_t pbank_dirty;                /* bit k: track k's bank differs from flash */
static uint8_t pbank_flash_cur[NTRK];      /* (pbank_store.c) */

static void pbank_blank(step_t *s)         /* ui.c step_clear */
{
    s->n = 0;
    s->time = ST_REST;
    s->flags = 0;
    s->vel = 0;
    s->hit = s->acc = 0;
    s->probability = 0;
}

/* the live pattern of track t -> its slot */
static __attribute__((noinline)) void pbank_store(track_t *t)
{
    uint32_t k = trk_index(t), i, n = 0, f;
    pbank_pat_t *p = &pbank[k][t->pat % NPAT];
    memcpy(p->step, t->step, sizeof p->step);
    memcpy(p->timing, &t->p[P_SLEN], sizeof p->timing);
    p->qnt = (uint8_t)(t->p[P_SQNT] != 0);
    chain_sound_save(t, &p->snd);
    f = motion_guard();
    for (i = 0; i < motion.count && n < PAT_MOT; i++)
        if ((motion.event[i].place >> 6) == k) {
            p->mot[n] = motion.event[i];
            p->mot[n].place &= 63u;
            n++;
        }
    p->mon = (uint8_t)((motion.on >> k) & 1u);
    motion_unguard(f);
    p->mcount = (uint8_t)n;
    p->used = 1;
    pbank_dirty |= (uint8_t)(1u << k);
}

/* slot idx of track t -> the live pattern (t->pat becomes idx). An empty slot: no steps, no automation, the sound as is */
static __attribute__((noinline)) void pbank_load(track_t *t, uint32_t idx)
{
    uint32_t k = trk_index(t), i, n = 0, f;
    const pbank_pat_t *p = &pbank[k][idx % NPAT];
    t->pat = (uint8_t)(idx % NPAT);
    seq_release(t);
    t->rat_left = 0;
    t->nd_late = t->nd_early = ND_NONE;
    t->nd_fired = 0;
    t->rh_n = t->rskip_n = 0;
    if (p->used) {
        memcpy(t->step, p->step, sizeof t->step);
        chain_sound_set(t, &p->snd, 1);
        memcpy(&t->p[P_SLEN], p->timing, sizeof p->timing);
        t->p[P_SQNT] = p->qnt;
    } else {
        for (i = 0; i < NSTEP; i++)
            pbank_blank(&t->step[i]);
    }
    f = motion_guard();
    motion_restore(t);
    for (i = 0; i < motion.count; i++)                   /* the track's records out, the pattern's in */
        if ((motion.event[i].place >> 6) != k)
            motion.event[n++] = motion.event[i];
    if (p->used)
        for (i = 0; i < p->mcount && n < MOTION_MAX; i++) {
            motion.event[n] = p->mot[i];
            motion.event[n].place = (uint8_t)(k << 6 | (p->mot[i].place & 63u));
            n++;
        }
    memset(motion.event + n, 0, (MOTION_MAX - n) * sizeof motion.event[0]);
    motion.count = (uint8_t)n;
    motion.on = (uint8_t)((motion.on & ~(1u << k)) | (p->used && p->mon ? 1u << k : 0u));
    motion_full = 0;
    motion_rebase(t);
    motion_unguard(f);
    if (song.playing) {                                  /* from its step 1 on the next block */
        t->seq_idx = (uint16_t)((t->p[P_SLEN] ? t->p[P_SLEN] : 1) - 1);
        t->seq_pos = 0x7FFFFFFFu;
    }
}

/* track t plays slot idx from now: the live pattern goes back to its slot first */
static void pbank_switch(track_t *t, uint32_t idx)
{
    t->pat_q = 0;
    if (idx % NPAT == t->pat)
        return;
    pbank_store(t);
    pbank_load(t, idx);
}

/* launch: at the next bar while playing (a second press changes what is queued), at once stopped. 0: refused (a SONG) */
static int pbank_launch(track_t *t, uint32_t idx)
{
    if (chain.running || chain.armed)
        return 0;
    idx %= NPAT;
    if (!song.playing) {
        pbank_switch(t, idx);
        return 1;
    }
    t->pat_q = (uint8_t)(idx == t->pat ? 0u : idx + 1u);
    return 1;
}
/* a bar line (events_block: beat_n wrapped to 0): what is queued starts */
static void pbank_bar(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        if (trk[i].pat_q)
            pbank_switch(&trk[i], (uint32_t)trk[i].pat_q - 1u);
}
/* STOP: what is queued happens (the next PLAY starts it) */
static void pbank_stop(void)
{
    pbank_bar();
}
/* slot src of track t copied over slot dst (the live pattern first stored); dst becomes the pattern playing if it was */
static void pbank_copy(track_t *t, uint32_t src, uint32_t dst)
{
    uint32_t k = trk_index(t);
    src %= NPAT;
    dst %= NPAT;
    if (src == dst)
        return;
    pbank_store(t);
    pbank[k][dst] = pbank[k][src];
    pbank_dirty |= (uint8_t)(1u << k);
    if (t->pat == dst)
        pbank_load(t, dst);
}
