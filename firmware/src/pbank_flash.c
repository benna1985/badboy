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
    uint32_t magic, len, crc;                    /* payload length, st_crc32 of the payload */
    uint8_t pad[PBF_HDR - 12u - 4u];
    uint32_t hcrc;                               /* st_crc32 of everything above */
} pbf_hdr_t;
_Static_assert(sizeof(pbf_hdr_t) == PBF_HDR, "pattern bank header layout");
/* the payload: the slot -> entry table, the ORDER, then the entries up to the last one in use (the RAM bank as it is) */
typedef struct {
    uint8_t idx[NTRK][NPAT];
    pseq_blob_t seq;
} pbf_head_t;
_Static_assert(PBF_HDR + sizeof(pbf_head_t) + PSLOTS * sizeof(pbank_pat_t) <= PBF_SIZE, "the bank fits the region");
_Static_assert(sizeof(pbf_head_t) % 4u == 0u && sizeof(pbank_pat_t) % 4u == 0u, "written in words");

/* write the bank: 0 done, 1 flash error, 2 nothing could be stored (full) */
static int pbf_write(void)
{
    pbf_hdr_t h;
    pbf_head_t d;
    uint32_t k, n = 0, len, crc, off, sec;
    for (k = 0; k < NTRK; k++)                   /* the patterns being played go back to their slots first */
        if (!pbank_store(&trk[k]))
            return 2;
    memcpy(d.idx, pbank_idx, sizeof d.idx);
    d.seq = pseq;
    for (k = 0; k < NTRK * NPAT; k++)
        if (((const uint8_t *)pbank_idx)[k] > n)
            n = ((const uint8_t *)pbank_idx)[k];
    len = (uint32_t)sizeof d + n * (uint32_t)sizeof(pbank_pat_t);
    for (sec = 0; sec < (PBF_HDR + len + 0xFFFu) / 0x1000u; sec++)
        if (st_erase(PBF_BASE + sec * 0x1000u))
            return 1;
    off = PBF_BASE + PBF_HDR;
    crc = st_crc32_add(st_crc32_add(0xFFFFFFFFu, &d, sizeof d), pbank_slab, n * (uint32_t)sizeof(pbank_pat_t));
    if (st_prog(off, &d, sizeof d) || st_prog(off + (uint32_t)sizeof d, pbank_slab, n * (uint32_t)sizeof(pbank_pat_t)))
        return 1;
    memset(&h, 0, sizeof h);
    h.magic = PBF_MAGIC;
    h.len = len;
    h.crc = ~crc;
    h.hcrc = st_crc32(&h, sizeof h - 4u);
    if (st_prog(PBF_BASE, &h, sizeof h))         /* (the commit: last) */
        return 1;
    pbank_dirty = 0;
    pseq_dirty = 0;
    return 0;
}

/* read the bank from flash into RAM: 1 done, 0 nothing valid there (the RAM bank is left as it was) */
static int pbf_read(void)
{
    const uint8_t *x = PBF_XIP();
    const pbf_head_t *d = (const pbf_head_t *)(x + PBF_HDR);
    pbf_hdr_t h;
    uint32_t k, n;
    memcpy(&h, x, sizeof h);
    if (h.magic != PBF_MAGIC || h.hcrc != st_crc32(&h, sizeof h - 4u) || h.len < sizeof *d ||
        (h.len - sizeof *d) % sizeof(pbank_pat_t) || (h.len - sizeof *d) / sizeof(pbank_pat_t) > PSLOTS ||
        ~st_crc32_add(0xFFFFFFFFu, d, h.len) != h.crc)
        return 0;
    n = (h.len - (uint32_t)sizeof *d) / (uint32_t)sizeof(pbank_pat_t);
    for (k = 0; k < NTRK * NPAT; k++)
        if (((const uint8_t *)d->idx)[k] > n)
            return 0;
    memcpy(pbank_idx, d->idx, sizeof pbank_idx);
    memcpy(&pseq, &d->seq, sizeof pseq);
    memcpy(pbank_slab, d + 1, n * sizeof(pbank_pat_t));
    for (k = 0; k < NTRK; k++)                   /* what the page could not have made is cut to what it could */
        pseq.n[k] = pseq.n[k] > PSEQ_MAX ? PSEQ_MAX : pseq.n[k];
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
