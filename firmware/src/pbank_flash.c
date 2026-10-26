/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The pattern bank and the pattern ORDER in flash (SAVE > PATTERNS, pbank.c).
 *
 * Where: 0xD2000..0xDBFFF, the 40 KiB of what was the sixth user sample slot. One copy, written sector by sector from
 * the RAM bank (no staging buffer): a header sector's first 256 bytes (magic, the entries, their length and CRC, the
 * slot -> entry table) are programmed last, so a power cut during a save leaves no header and the bank is empty at the
 * next start (the previous save is gone with its erase; SAVE says "DO NOT POWER OFF" and writes only while stopped).
 * Payload from +0x100: the ORDER (pseq_blob_t), then the used entries one after the other, numbered in slot order.
 * Read at boot into the RAM bank (the tracks keep what they play: a launch loads from it) and by SAVE > PATTERNS READ. */
#define PBF_BASE (SMP_USER_BASE + SMP_USER_SLOTS * SMP_USER_SIZE)
#define PBF_SIZE SMP_USER_SIZE
#define PBF_HDR 0x100u
#define PBF_MAGIC 0x31424250u                    /* "PBB1" */
#ifndef PBF_XIP                                  /* host tests: a RAM image of the region */
#define PBF_XIP() fm1_xip_ptr(PBF_BASE)
#endif
typedef struct {
    uint32_t magic, n, len, crc;                 /* entries, payload length, st_crc32 of the payload */
    uint8_t idx[NTRK][NPAT];                     /* slot -> entry (1..n), 0 empty */
    uint8_t pad[PBF_HDR - 16u - NTRK * NPAT - 4u];
    uint32_t hcrc;                               /* st_crc32 of everything above */
} pbf_hdr_t;
_Static_assert(sizeof(pbf_hdr_t) == PBF_HDR, "pattern bank header layout");
_Static_assert(sizeof(pbank_pat_t) % 4u == 0u && PBF_HDR + sizeof(pseq_blob_t) + PSLOTS * sizeof(pbank_pat_t) <= PBF_SIZE,
               "the bank fits the region");

/* write the bank: 0 done, 1 flash error, 2 nothing could be stored (full) */
static int pbf_write(void)
{
    pbf_hdr_t h;
    uint32_t k, i, n = 0, len, crc, off, sec;
    memset(&h, 0, sizeof h);
    for (k = 0; k < NTRK; k++)                   /* the patterns being played go back to their slots first */
        if (!pbank_store(&trk[k]))
            return 2;
    for (k = 0; k < NTRK; k++)
        for (i = 0; i < NPAT; i++)
            if (pbank_idx[k][i])
                h.idx[k][i] = (uint8_t)++n;
    len = (uint32_t)sizeof pseq + n * (uint32_t)sizeof(pbank_pat_t);
    for (sec = 0; sec < (PBF_HDR + len + 0xFFFu) / 0x1000u; sec++)
        if (st_erase(PBF_BASE + sec * 0x1000u))
            return 1;
    off = PBF_BASE + PBF_HDR;
    crc = st_crc32_add(0xFFFFFFFFu, &pseq, sizeof pseq);
    if (st_prog(off, &pseq, sizeof pseq))
        return 1;
    off += (uint32_t)sizeof pseq;
    for (k = 0; k < NTRK; k++)                   /* (slot order = entry order) */
        for (i = 0; i < NPAT; i++)
            if (pbank_idx[k][i]) {
                const pbank_pat_t *p = &pbank_slab[pbank_idx[k][i] - 1u];
                crc = st_crc32_add(crc, p, sizeof *p);
                if (st_prog(off, p, sizeof *p))
                    return 1;
                off += (uint32_t)sizeof *p;
            }
    h.magic = PBF_MAGIC;
    h.n = n;
    h.len = len;
    h.crc = ~crc;
    h.hcrc = st_crc32(&h, sizeof h - 4u);
    if (st_prog(PBF_BASE, &h, sizeof h))
        return 1;
    pbank_dirty = 0;
    pseq_dirty = 0;
    return 0;
}

/* read the bank from flash into RAM: 1 done, 0 nothing valid there (the RAM bank is left as it was) */
static int pbf_read(void)
{
    const uint8_t *x = PBF_XIP();
    pbf_hdr_t h;
    uint32_t k, i, crc;
    const pbank_pat_t *src;
    memcpy(&h, x, sizeof h);
    if (h.magic != PBF_MAGIC || h.hcrc != st_crc32(&h, sizeof h - 4u) || h.n > PSLOTS ||
        h.len != (uint32_t)sizeof pseq + h.n * (uint32_t)sizeof(pbank_pat_t))
        return 0;
    crc = ~st_crc32_add(0xFFFFFFFFu, x + PBF_HDR, h.len);
    if (crc != h.crc)
        return 0;
    for (k = 0; k < NTRK; k++)
        for (i = 0; i < NPAT; i++)
            if (h.idx[k][i] > h.n)
                return 0;
    memcpy(&pseq, x + PBF_HDR, sizeof pseq);
    for (k = 0; k < NTRK; k++) {                 /* an order the page could not have made is cut to what it could */
        if (pseq.n[k] > PSEQ_MAX)
            pseq.n[k] = PSEQ_MAX;
        for (i = 0; i < pseq.n[k]; i++) {
            pseq.row[k][i].pat %= NPAT;
            if (!pseq.row[k][i].bars || pseq.row[k][i].bars > 99u)
                pseq.row[k][i].bars = 1;
        }
    }
    src = (const pbank_pat_t *)(x + PBF_HDR + sizeof pseq);
    memset(pbank_idx, 0, sizeof pbank_idx);
    for (k = 0; k < NTRK; k++)
        for (i = 0; i < NPAT; i++)
            if (h.idx[k][i]) {
                pbank_pat_t *d = pbank_alloc(k, i);
                memcpy(d, &src[h.idx[k][i] - 1u], sizeof *d);
                if (d->mcount > PAT_MOT)
                    d->mcount = PAT_MOT;
                d->used = 1;
            }
    pbank_dirty = 0;
    pseq_dirty = 0;
    return 1;
}

/* main loop, with settings_poll: what SAVE > PATTERNS asked for, once the transport stops */
static void pbf_poll(void)
{
    uint32_t k;
    int rc;
    if (!pbf_req || transport_busy() || !flash_ok)
        return;
    if (pbf_req == 1u) {
        rc = pbf_write();
        ui_message(rc == 0 ? "PATTERNS SAVED" : rc == 2 ? "BANK FULL" : "SAVE ERROR");
    } else if (pbf_read()) {
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
static void pbf_boot(void)                       /* persist_boot: the saved bank into RAM (no tracks touched) */
{
    (void)pbf_read();
}
