/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* A song's pattern bank in flash (project.c: SAVE > PROJECT writes it, loading the project reads it; SAVE > PATTERNS
 * READ / WRITE do the same for the current slot).
 *
 * Where: 0xD2000..0xD9FFF, 8 KiB (two sectors) for each of the four project slots, in the 40 KiB of what was the sixth
 * user sample slot (0xDA000..0xDBFFF stays free). A 16-byte header (magic, payload length, CRC, header CRC) is programmed
 * last: a power cut during a save leaves no header and that song has no bank (SAVE says "DO NOT POWER OFF" and writes only
 * while stopped). The payload follows at +16, packed (no RAM staging beyond one 256-byte page):
 *   the pattern each track is on (the live one: pbank_store puts it in its slot first),
 *   the ORDER (per track its row count, then the rows),
 *   the used slots in track / slot order: track and slot, the flags (QUANTIZE, motion on, the sound as the track's
 *   previous record, the motion count), LEN DIV SWING GATE, the sound unless the same (engine, preset, every value),
 *   the automation / locks, a bitmap of the steps that are not blank and those steps.
 * Most of a pattern's size is its sound and most patterns of a track share it, so 34 patterns fit in the 8 KiB; a bank
 * that does not says "SONG FULL" (the project itself is saved). Read into the RAM bank (the tracks keep what they play). */
#define PBF_BASE (SMP_USER_BASE + SMP_USER_SLOTS * SMP_USER_SIZE)
#define PBF_SLOT 0x2000u                         /* the flash a song's bank has */
#define PBF_HDR 16u
#define PBF_CAP (PBF_SLOT - PBF_HDR)
#define PBF_MAGIC 0x32424250u                    /* "PBB2" */
#ifndef PBF_XIP                                  /* host tests: a RAM image of the region */
#define PBF_XIP() fm1_xip_ptr(PBF_BASE)
#endif
typedef struct {
    uint32_t magic, len, crc, hcrc;              /* payload length, st_crc32 of the payload, st_crc32 of the three above */
} pbf_hdr_t;
#define PBF_F_QNT 1u
#define PBF_F_MON 2u
#define PBF_F_SAME 4u                            /* bits 3..7: the motion records */
static uint8_t pbf_pat[NTRK];                    /* what the last pbf_read found: the pattern each track was on */

/* ---- the payload as a byte stream: counted (SONG FULL?), or programmed through one page ---- */
static struct { uint32_t base, n, crc; uint16_t fill; uint8_t page[256], count_only, err; } pw;
static void pw_flush(void)
{
    uint32_t n = pw.fill;
    if (!n)
        return;
    while (n & 3u)
        pw.page[n++] = 0;
    if (!pw.err && st_prog(pw.base + PBF_HDR + (pw.n - pw.fill), pw.page, n))
        pw.err = 1;
    pw.fill = 0;
}
static void pw_put(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    if (pw.count_only) {
        pw.n += n;
        return;
    }
    pw.crc = st_crc32_add(pw.crc, p, n);
    while (n--) {
        pw.page[pw.fill++] = *b++;
        pw.n++;
        if (pw.fill == sizeof pw.page)
            pw_flush();
    }
}
static void pw_u8(uint32_t v)
{
    uint8_t b = (uint8_t)v;
    pw_put(&b, 1);
}
static int pbf_blank(const step_t *s)
{
    return s->n == 0 && s->time == ST_REST && !s->flags && !s->vel && !s->hit && !s->acc && !s->probability;
}
/* the whole payload (the live patterns are in the bank already) */
static void pbf_emit(void)
{
    uint32_t k, i, j, last;
    const pbank_pat_t *prev[NTRK] = {0, 0, 0, 0};
    for (k = 0; k < NTRK; k++)
        pw_u8(trk[k].pat);
    for (k = 0; k < NTRK; k++) {
        pw_u8(pseq.n[k]);
        pw_put(pseq.row[k], pseq.n[k] * (uint32_t)sizeof pseq.row[k][0]);
    }
    for (k = 0, last = 0; k < NTRK; k++)
        for (i = 0; i < NPAT; i++)
            last += pbank_used(k, i);
    pw_u8(last);
    for (k = 0; k < NTRK; k++)
        for (i = 0; i < NPAT; i++) {
            const pbank_pat_t *p = pbank_get(k, i);
            uint8_t map[NSTEP / 8u];
            uint32_t same;
            if (!p)
                continue;
            same = prev[k] && !memcmp(&prev[k]->snd, &p->snd, sizeof p->snd);
            pw_u8(k << 4 | i);
            pw_u8((p->qnt ? PBF_F_QNT : 0u) | (p->mon ? PBF_F_MON : 0u) | (same ? PBF_F_SAME : 0u) | (uint32_t)p->mcount << 3);
            pw_put(p->timing, sizeof p->timing);
            if (!same)
                pw_put(&p->snd, sizeof p->snd);
            pw_put(p->mot, p->mcount * (uint32_t)sizeof p->mot[0]);
            memset(map, 0, sizeof map);
            for (j = 0; j < NSTEP; j++)
                if (!pbf_blank(&p->step[j]))
                    map[j >> 3] |= (uint8_t)(1u << (j & 7u));
            pw_put(map, sizeof map);
            for (j = 0; j < NSTEP; j++)
                if (map[j >> 3] >> (j & 7u) & 1u)
                    pw_put(&p->step[j], sizeof p->step[j]);
            prev[k] = p;
        }
}

/* write song `slot`'s bank: 0 done, 1 flash error, 2 does not fit (or no free entry for a live pattern) */
static int pbf_write(uint32_t slot)
{
    pbf_hdr_t h;
    uint32_t k, base = PBF_BASE + (slot & 3u) * PBF_SLOT;
    for (k = 0; k < NTRK; k++)                   /* the patterns being played go back to their slots first */
        if (!pbank_store(&trk[k]))
            return 2;
    memset(&pw, 0, sizeof pw);
    pw.count_only = 1;
    pbf_emit();
    if (pw.n > PBF_CAP)
        return 2;
    if (st_erase(base) || st_erase(base + 0x1000u))
        return 1;
    memset(&pw, 0, sizeof pw);
    pw.base = base;
    pw.crc = 0xFFFFFFFFu;
    pbf_emit();
    pw_flush();
    if (pw.err)
        return 1;
    h.magic = PBF_MAGIC;
    h.len = pw.n;
    h.crc = ~pw.crc;
    h.hcrc = st_crc32(&h, 12u);
    if (st_prog(base, &h, sizeof h))             /* (the commit: last) */
        return 1;
    pbank_dirty = 0;
    pseq_dirty = 0;
    return 0;
}

/* the parser over the stored payload: `apply` 0 only checks it, 1 fills the RAM bank (clearing it first) */
static struct { const uint8_t *p, *end; } pr;
static int pr_get(void *dst, uint32_t n)
{
    if ((uint32_t)(pr.end - pr.p) < n)
        return 0;
    if (dst)
        memcpy(dst, pr.p, n);
    pr.p += n;
    return 1;
}
static int pbf_parse(const uint8_t *x, uint32_t len, int apply)
{
    uint8_t pat[NTRK], pn[NTRK], have[NTRK] = {0, 0, 0, 0}, cnt, ts, fl, map[NSTEP / 8u];
    pseq_row_t rows[NTRK][PSEQ_MAX];
    const pbank_pat_t *prev[NTRK] = {0, 0, 0, 0};
    uint32_t k, i, j, r;
    pr.p = x;
    pr.end = x + len;
    for (k = 0; k < NTRK; k++)
        if (!pr_get(&pat[k], 1) || pat[k] >= NPAT)
            return 0;
    for (k = 0; k < NTRK; k++)
        if (!pr_get(&pn[k], 1) || pn[k] > PSEQ_MAX || !pr_get(rows[k], pn[k] * (uint32_t)sizeof rows[k][0]))
            return 0;
    if (!pr_get(&cnt, 1) || cnt > PSLOTS)
        return 0;
    if (apply) {
        memset(pbank_idx, 0, sizeof pbank_idx);
        memset(&pseq, 0, sizeof pseq);
        for (k = 0; k < NTRK; k++) {
            pseq.n[k] = pn[k];
            memcpy(pseq.row[k], rows[k], pn[k] * sizeof rows[k][0]);
            pbf_pat[k] = pat[k];
        }
    }
    for (r = 0; r < cnt; r++) {
        pbank_pat_t *p = 0;
        uint32_t mc;
        if (!pr_get(&ts, 1) || !pr_get(&fl, 1) || (ts >> 4) >= NTRK || (ts & 15u) >= NPAT)
            return 0;
        k = ts >> 4;
        i = ts & 15u;
        mc = fl >> 3;
        if (mc > PAT_MOT)
            return 0;
        if (apply) {
            if (pbank_idx[k][i] || !(p = pbank_alloc(k, i)))
                return 0;
            p->used = 1;
            p->qnt = (fl & PBF_F_QNT) != 0;
            p->mon = (fl & PBF_F_MON) != 0;
            p->mcount = (uint8_t)mc;
        }
        if (!pr_get(p ? (void *)p->timing : 0, sizeof p->timing))
            return 0;
        if (fl & PBF_F_SAME) {
            if (!have[k])
                return 0;
            if (p && prev[k])
                p->snd = prev[k]->snd;
        } else if (!pr_get(p ? (void *)&p->snd : 0, sizeof p->snd)) {
            return 0;
        }
        have[k] = 1;
        if (!pr_get(p ? (void *)p->mot : 0, mc * (uint32_t)sizeof p->mot[0]) || !pr_get(map, sizeof map))
            return 0;
        for (j = 0; j < NSTEP; j++) {
            if (map[j >> 3] >> (j & 7u) & 1u) {
                if (!pr_get(p ? (void *)&p->step[j] : 0, sizeof p->step[j]))
                    return 0;
            } else if (p) {
                pbank_blank(&p->step[j]);
            }
        }
        if (p)
            prev[k] = p;
    }
    return pr.p == pr.end;
}

/* read song `slot`'s bank into RAM: 1 done, 0 nothing valid there (the RAM bank is left as it was) */
static int pbf_read(uint32_t slot)
{
    const uint8_t *x = PBF_XIP() + (slot & 3u) * PBF_SLOT;
    pbf_hdr_t h;
    memcpy(&h, x, sizeof h);
    if (h.magic != PBF_MAGIC || h.hcrc != st_crc32(&h, 12u) || h.len > PBF_CAP ||
        ~st_crc32_add(0xFFFFFFFFu, x + PBF_HDR, h.len) != h.crc || !pbf_parse(x + PBF_HDR, h.len, 0) ||
        !pbf_parse(x + PBF_HDR, h.len, 1))
        return 0;
    pbank_dirty = 0;
    pseq_dirty = 0;
    return 1;
}
/* a project is loaded (slot): its bank in, or no bank (another song's patterns must not stay), the tracks on the patterns
 * they were saved on (the project holds each one's steps, so nothing is loaded from the bank) */
static void pbf_song_load(uint32_t slot)
{
    uint32_t k;
    if (!flash_ok || !pbf_read(slot)) {
        memset(pbank_idx, 0, sizeof pbank_idx);
        memset(&pseq, 0, sizeof pseq);
        memset(pbf_pat, 0, sizeof pbf_pat);
        pbank_dirty = 0;
        pseq_dirty = 0;
    }
    for (k = 0; k < NTRK; k++) {
        trk[k].pat = pbf_pat[k] % NPAT;
        trk[k].pat_q = 0;
    }
    pbf_slot = (uint8_t)(slot & 3u);
}
/* a project is saved (slot): its bank with it */
static void pbf_song_save(uint32_t slot)
{
    int rc;
    if (!flash_ok)
        return;
    rc = pbf_write(slot);
    pbf_slot = (uint8_t)(slot & 3u);
    if (rc == 2)
        ui_message("PATTERNS TOO BIG");
    else if (rc)
        ui_message("PATTERN SAVE ERROR");
}
/* a project erased: its bank goes (the header sector) */
static void pbf_song_erase(uint32_t slot)
{
    if (flash_ok && ((const pbf_hdr_t *)(PBF_XIP() + (slot & 3u) * PBF_SLOT))->magic == PBF_MAGIC)
        (void)st_erase(PBF_BASE + (slot & 3u) * PBF_SLOT);
    if (pbf_slot == (slot & 3u))
        pbf_slot = 0xFFu;
}

/* main loop, with settings_poll: what SAVE > PATTERNS asked for (the current song's bank), once the transport stops */
static void pbf_poll(void)
{
    uint32_t k;
    int rc;
    if (!pbf_req || transport_busy() || !flash_ok)
        return;
    if (pbf_slot > 3u) {
        ui_message("SAVE A SONG FIRST");
    } else if (pbf_req == 1u) {
        rc = pbf_write(pbf_slot);
        ui_message(rc == 0 ? "PATTERNS SAVED" : rc == 2 ? "SONG FULL" : "SAVE ERROR");
    } else if (pbf_read(pbf_slot)) {
        for (k = 0; k < NTRK; k++) {             /* the tracks play what they had in the bank again */
            fm1_irq_off();
            pbank_load(&trk[k], trk[k].pat);
            fm1_irq_on();
        }
        ui_message("PATTERNS LOADED");
        ui.force = 1;
    } else {
        ui_message("NO SAVED PATTERNS");
    }
    pbf_req = 0;
}
