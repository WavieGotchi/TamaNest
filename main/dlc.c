/* DLC for TamaNest (2026-11-23). The store tables, header layout, payload extraction and routing are ported from
 * tamaemu's tools/dlc.c by maragotchi (https://github.com/maragotchi/tamaemu, GPL-3.0); Paramix Studio by mameise
 * uses the same. A record ("TAMAGO" header, ends with a 16-bit sum) is copied verbatim into the first erased slot of
 * its store in the game's flash; the game finds it the next time it opens that shop or menu. Here the flash is the
 * emulator's (ROM + the game's own writes, kept in the save), so ROM files are never changed. */
#include "dlc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

struct DlcDevice {
    uint32_t rv;
    const DlcKind *kinds; int nkinds;
    uint8_t cat_off;                                 /* category byte: 0x50, iD 0x4F */
    void (*route)(int typ, int cat, const char *id, int flag, const uint8_t *rec, size_t len, int *kind, const char **label);
};

/* ---- P's ---- */
static const DlcKind PS_KINDS[] = {
    { 0,  0x5A0000, 0x0800, 20, "MEALS" },      { 1,  0x5B0000, 0x0800, 15, "SNACKS" },
    { 2,  0x610000, 0x4000,  5, "CLOTHING" },   { 3,  0x660000, 0x2000, 10, "ACCESSORIES" },
    { 4,  0x6B0000, 0x2000, 10, "ITEMS" },      { 6,  0x6F0000, 0x2000, 10, "WALLPAPERS" },
    { 7,  0x710000, 0x8000,  2, "GAMES" },      { 8,  0x720000, 0x8000,  2, "VDP PLACES" },
    { 10, 0x750000, 0x8000,  4, "VDP RECIPES" },
};
static const struct { int typ, cat, kind; const char *label; } PS_ROUTE[] = {
    { 0x81, 0x02, 0, "MEAL" }, { 0x81, 0x0D, 1, "SNACK" }, { 0x81, 0x33, 2, "CLOTHING" },
    { 0x81, 0x34, 3, "ACCESSORY" }, { 0x81, 0x35, 3, "ACCESSORY" }, { 0x81, 0x2A, 4, "ITEM" },
    { 0x81, 0x1F, 6, "WALLPAPER" }, { 0x81, 0x29, 10, "VDP RECIPE" }, { 0x94, 0x47, 8, "DESTINATION" },
};
static void route_ps(int typ, int cat, const char *id, int flag, const uint8_t *rec, size_t len, int *kind, const char **label)
{
    (void)rec; (void)len;
    if (typ == 0x94 && cat == 0x5B) { *kind = flag == 0x01 ? 7 : 8; *label = flag == 0x01 ? "GAME" : "VDP PLACE"; return; }
    if (typ == 0x81 && cat == 0x29 && strncmp(id, "idn_rec", 7) == 0) { *kind = -1; *label = "RECIPE (NO STORE)"; return; }
    for (size_t i = 0; i < sizeof PS_ROUTE / sizeof PS_ROUTE[0]; i++)
        if (PS_ROUTE[i].typ == typ && PS_ROUTE[i].cat == cat) { *kind = PS_ROUTE[i].kind; *label = PS_ROUTE[i].label; return; }
    *kind = -1; *label = typ == 0x81 && cat == 0x52 ? "STAMP CARD (NO STORE)" : NULL;
}

/* ---- iD L ---- */
static const DlcKind IDL_KINDS[] = {
    { 0, 0x590000, 0x0800, 20, "MEALS" },       { 1, 0x5A0000, 0x0800, 20, "SNACKS" },
    { 2, 0x600000, 0x4000,  5, "CLOTHING" },    { 3, 0x650000, 0x2000, 10, "ACCESSORIES" },
    { 4, 0x6A0000, 0x2000, 10, "ITEMS" },       { 5, 0x740000, 0x8000,  5, "DAILY ITEMS" },
    { 6, 0x780000, 0x2000, 10, "WALLPAPERS" },  { 7, 0x7A0000, 0x8000,  2, "GAMES" },
    { 8, 0x7B0000, 0x8000,  2, "DESTINATIONS" },
};
static const struct { int typ, cat, flag, kind; const char *label; } IDL_ROUTE[] = {
    { 0x81, 0x02, 0x01, 0, "MEAL" }, { 0x81, 0x0C, 0x02, 1, "SNACK" }, { 0x81, 0x0D, 0x02, 1, "SNACK" },
    { 0x81, 0x1F, 0x00, 6, "WALLPAPER" }, { 0x81, 0x29, 0x00, 5, "SEED" }, { 0x81, 0x29, 0x02, 5, "DAILY ITEM" },
    { 0x81, 0x2A, 0x01, 4, "ITEM" }, { 0x81, 0x33, 0x00, 2, "CLOTHING" }, { 0x81, 0x34, 0x00, 3, "ACCESSORY" },
    { 0x81, 0x35, 0x00, 3, "ACCESSORY" }, { 0x81, 0x51, 0x01, -1, "LETTER (NOT YET)" },
    { 0x81, 0x52, 0x02, -1, "HAPPY MAIL (NOT YET)" }, { 0x94, 0x47, 0x02, 8, "DESTINATION" }, { 0x94, 0x5B, 0x01, 7, "GAME" },
};
static void route_idl(int typ, int cat, const char *id, int flag, const uint8_t *rec, size_t len, int *kind, const char **label)
{
    (void)id; (void)rec; (void)len;
    for (size_t i = 0; i < sizeof IDL_ROUTE / sizeof IDL_ROUTE[0]; i++)
        if (IDL_ROUTE[i].typ == typ && IDL_ROUTE[i].cat == cat && IDL_ROUTE[i].flag == flag) { *kind = IDL_ROUTE[i].kind; *label = IDL_ROUTE[i].label; return; }
    *kind = -1; *label = NULL;
}

/* ---- 4U / 4U+ (stores overlap; firmware geometry) ---- */
static const DlcKind U4_KINDS[] = {
    { 15, 0x5A0000, 0x2000,  4, "DECORATIONS" }, {  0, 0x5B0000, 0x0800, 20, "MEALS" },
    {  1, 0x5C0000, 0x0800, 20, "SNACKS" },      {  2, 0x610000, 0x4000,  5, "CLOTHES" },
    {  3, 0x660000, 0x2000, 10, "ACCESSORIES" }, {  4, 0x6C0000, 0x2000, 15, "TAMADEPA" },
    {  6, 0x6F0000, 0x2000, 10, "INTERIOR" },    {  7, 0x710000, 0x8000,  2, "GAMES" },
    {  8, 0x720000, 0x8000,  4, "DESTINATIONS" },{ 16, 0x740000, 0x2000,  1, "COUPON" },
    { 17, 0x750000, 0x8000,  1, "BINGO" },       { 18, 0x760000, 0x8000,  1, "GASHAPON" },
    { 19, 0x780000, 0x8000,  8, "CHARACTERS" },
};
static const struct { int typ, cat, flag, kind; const char *label; } U4_ROUTE[] = {
    { 0x81, 0x01, 0x01, 0, "MEAL" }, { 0x81, 0x02, 0x01, 0, "MEAL" }, { 0x81, 0x01, 0x02, 1, "SNACK" },
    { 0x81, 0x0D, 0x02, 1, "SNACK" }, { 0x81, 0x1F, 0x00, 6, "INTERIOR" }, { 0x81, 0x33, 0x00, 2, "OUTFIT" },
    { 0x81, 0x34, 0x00, 3, "ACCESSORY" }, { 0x81, 0x35, 0x00, 3, "ACCESSORY" }, { 0x94, 0x5B, 0x01, 7, "GAME" },
    { 0x94, 0x5B, 0x02, 8, "DESTINATION" }, { 0x94, 0x48, 0x03, 19, "CHARACTER" }, { 0x94, 0x47, 0x02, 8, "DESTINATION" },
};
static int u4_item_id(const char *s)                 /* first run of 2+ digits in the id */
{
    for (; s && *s; s++)
        if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') {
            long n = 0;
            for (; *s >= '0' && *s <= '9'; s++) { n = n * 10 + (*s - '0'); if (n > 0xFFFF) return -1; }
            return (int)n;
        }
    return -1;
}
static void route_4u(int typ, int cat, const char *id, int flag, const uint8_t *rec, size_t len, int *kind, const char **label)
{
    (void)rec; (void)len;
    if (typ == 0x81 && cat == 0x2A && flag == 0x01) {
        int n = u4_item_id(id), m = n % 1000;
        if (n < 0) { *kind = -1; *label = "ITEM WITHOUT A NUMBER"; }
        else if (m >= 601 && m <= 749) { *kind = 16; *label = "COUPON"; }
        else if (m >= 801 && m <= 949) { *kind = 15; *label = "DECORATION"; }
        else { *kind = 4; *label = "TOY"; }
        return;
    }
    if (typ == 0x81 && cat == 0x29 && flag == 0x00) {
        int n = u4_item_id(id), m = n % 1000;
        if (n >= 0 && m >= 301 && m <= 449) { *kind = 17; *label = "BINGO"; }
        else if (n >= 0 && m >= 601 && m <= 749) { *kind = 18; *label = "GASHAPON"; }
        else { *kind = -1; *label = "BINGO/GASHAPON (UNKNOWN)"; }
        return;
    }
    for (size_t i = 0; i < sizeof U4_ROUTE / sizeof U4_ROUTE[0]; i++)
        if (U4_ROUTE[i].typ == typ && U4_ROUTE[i].cat == cat && U4_ROUTE[i].flag == flag) { *kind = U4_ROUTE[i].kind; *label = U4_ROUTE[i].label; return; }
    *kind = -1; *label = NULL;
}

/* ---- iD / iD Melody (4 MB; travel at 0x0E0000) ---- */
static const DlcKind ID_KINDS[] = {
    { 0, 0x3C0000, 0x0800,  6, "MEALS" },     { 1, 0x3C3800, 0x0800,  3, "SNACKS" },
    { 2, 0x370000, 0x1800,  4, "TOYS" },      { 3, 0x3A0000, 0x0800, 10, "LETTERS" },
    { 4, 0x390000, 0x2000,  4, "INTERIOR" },  { 5, 0x360000, 0x8000,  2, "GAMES" },
    { 6, 0x380000, 0x2000,  5, "OUTFITS" },   { 7, 0x38E000, 0x2000,  1, "BACKDROP" },
    { 8, 0x3B0000, 0x2000,  3, "ACCESSORIES" },{ 9, 0x0E0000, 0x8000,  2, "DESTINATIONS" },
};
static const struct { int typ, cat, flag, kind; const char *label; } ID_ROUTE[] = {
    { 0x01, 0x01, 0x01, 0, "MEAL" }, { 0x01, 0x01, 0x03, 1, "SNACK" }, { 0x01, 0x04, 0x01, 2, "TOY" },
    { 0x01, 0x06, 0x00, 3, "LETTER" }, { 0x01, 0x07, 0x00, 4, "WALLPAPER" }, { 0x01, 0x02, 0x00, 8, "ACCESSORY" },
    { 0x02, 0x03, 0x01, 6, "OUTFIT" }, { 0x02, 0x03, 0x02, 7, "BACKDROP" }, { 0x01, 0x01, 0x02, -1, "DOUGHNUT (NO STORE)" },
};
static void route_id(int typ, int cat, const char *id, int flag, const uint8_t *rec, size_t len, int *kind, const char **label)
{
    (void)id;
    if (typ == 0x14 && cat == 0x02 && flag == 0x00) {   /* games and outings share a header: byte 0x64 tells */
        if (!rec || len <= 0x64) { *kind = -1; *label = "GAME/OUTING (TOO SHORT)"; }
        else if (rec[0x64] == 0x37) { *kind = 5; *label = "GAME"; }
        else { *kind = 9; *label = "DESTINATION"; }
        return;
    }
    for (size_t i = 0; i < sizeof ID_ROUTE / sizeof ID_ROUTE[0]; i++)
        if (ID_ROUTE[i].typ == typ && ID_ROUTE[i].cat == cat && ID_ROUTE[i].flag == flag) { *kind = ID_ROUTE[i].kind; *label = ID_ROUTE[i].label; return; }
    *kind = -1; *label = NULL;
}

#define NK(a) (int)(sizeof a / sizeof a[0])
static const DlcDevice DEVICES[] = {
    /* P's */             { 0x024FDFE6u, PS_KINDS,  NK(PS_KINDS),  0x50, route_ps },
    /* iD L (all known versions share the stores; tamaemu lists v050) */
    { 0x024CA8FEu, IDL_KINDS, NK(IDL_KINDS), 0x50, route_idl }, { 0x024CA942u, IDL_KINDS, NK(IDL_KINDS), 0x50, route_idl },
    { 0x024CAA72u, IDL_KINDS, NK(IDL_KINDS), 0x50, route_idl },
    /* iD, iD Melody */   { 0x00C907FAu, ID_KINDS,  NK(ID_KINDS),  0x4F, route_id }, { 0x00C939EAu, ID_KINDS, NK(ID_KINDS), 0x4F, route_id },
    /* 4U+, 4U */         { 0x024F6256u, U4_KINDS,  NK(U4_KINDS),  0x50, route_4u }, { 0x024C680Au, U4_KINDS, NK(U4_KINDS), 0x50, route_4u },
};

const DlcDevice *dlc_device(Emu *e)
{
    if (!e) return NULL;
    uint32_t rv = mem_read32(e, e->dev.ttbr_reset);
    for (int i = 0; i < NK(DEVICES); i++) if (DEVICES[i].rv == rv) return &DEVICES[i];
    return NULL;
}
int dlc_kind_count(const DlcDevice *d) { return d ? d->nkinds : 0; }
const DlcKind *dlc_kind_at(const DlcDevice *d, int i) { return d && i >= 0 && i < d->nkinds ? &d->kinds[i] : NULL; }
const DlcKind *dlc_kind(const DlcDevice *d, int kind)
{
    for (int i = 0; d && i < d->nkinds; i++) if (d->kinds[i].kind == kind) return &d->kinds[i];
    return NULL;
}

/* ---- records ---- */
static bool sum_ok(const uint8_t *b, size_t len)    /* records end with a big-endian 16-bit sum of the bytes before */
{
    if (len < 3) return false;
    uint32_t s = 0;
    for (size_t i = 0; i < len - 2; i++) s += b[i];
    return (s & 0xFFFFu) == (((uint32_t)b[len - 2] << 8) | b[len - 1]);
}
static const uint8_t *find(const uint8_t *h, size_t hn, const uint8_t *n, size_t nn)
{
    for (size_t i = 0; hn >= nn && i + nn <= hn; i++) if (!memcmp(h + i, n, nn)) return h + i;
    return NULL;
}
int dlc_extract(const char *path, uint8_t **out, size_t *len, const char **why)
{
    *out = NULL; *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "CANNOT OPEN"; return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); *why = sz <= 0 ? "EMPTY FILE" : "FILE TOO BIG"; return -1; }
    uint8_t *d = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!d) { fclose(f); *why = "OUT OF MEMORY"; return -1; }
    bool rd = fread(d, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    if (!rd) { free(d); *why = "READ ERROR"; return -1; }
    const uint8_t *blob = d; size_t bn = (size_t)sz;
    if (!(bn >= 6 && !memcmp(d, "TAMAGO", 6))) {    /* a JPEG with the record after its end marker */
        static const uint8_t EOI[2] = { 0xFF, 0xD9 };
        const uint8_t *e = find(d, bn, EOI, 2), *m;
        if (e) { size_t after = (size_t)(e - d) + 2; m = find(d + after, bn - after, (const uint8_t *)"TAMAGO", 6); }
        else m = find(d, bn, (const uint8_t *)"TAMAGO", 6);
        if (!m) { free(d); *why = "NOT A DLC FILE"; return -1; }
        blob = m; bn = (size_t)sz - (size_t)(m - d);
    }
    size_t declared = 0, n = 0;
    if (bn >= 0x4C) {
        size_t ln = ((size_t)blob[0x48] << 24) | ((size_t)blob[0x49] << 16) | ((size_t)blob[0x4A] << 8) | blob[0x4B];
        if (ln >= 0x60 && ln <= bn) declared = ln;
    }
    if (declared && sum_ok(blob, declared)) n = declared;
    else for (size_t slack = 0; slack <= 16 && slack < bn; slack++) {
        size_t c = bn - slack;
        if (c < 0x60 || c < declared) continue;
        if (sum_ok(blob, c)) { n = c; break; }
    }
    if (!n) { free(d); *why = "DAMAGED FILE"; return -1; }
    uint8_t *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) { free(d); *why = "OUT OF MEMORY"; return -1; }
    memcpy(p, blob, n);
    free(d);
    *out = p; *len = n;
    return 0;
}
void dlc_route(const DlcDevice *d, const uint8_t *p, size_t len, int *kind, const char **label)
{
    *kind = -1; *label = NULL;
    if (!d || len < 0x52) return;
    char id[0x15]; size_t k = 0;
    for (size_t i = 0x34; i < 0x48 && p[i] && k + 1 < sizeof id; i++) id[k++] = (char)p[i];
    id[k] = 0;
    d->route(p[0x4E], p[d->cat_off], id, p[0x51], p, len, kind, label);
}
void dlc_name(const uint8_t *p, size_t len, char *out, size_t n)
{
    out[0] = 0;
    if (len < 0x48 || !n) return;
    size_t k = 0; bool ascii = true;                  /* the name (UTF-16BE at 0x06) if it is plain ASCII */
    for (size_t i = 6; i + 1 < 0x34; i += 2) {
        uint16_t u = (uint16_t)((p[i] << 8) | p[i + 1]);
        if (!u) break;
        if (u < 32 || u > 126) { ascii = false; break; }
        if (k + 1 < n) out[k++] = (char)u;
    }
    out[k] = 0;
    while (k && out[k - 1] == ' ') out[--k] = 0;
    if (ascii && k) return;
    k = 0;                                            /* else its ASCII id */
    for (size_t i = 0x34; i < 0x48 && p[i] >= 32 && p[i] <= 126 && k + 1 < n; i++) out[k++] = (char)p[i];
    out[k] = 0;
}

/* ---- stores ---- */
bool dlc_slot(Emu *e, const DlcKind *k, int slot, uint8_t *hdr, size_t n)
{
    uint8_t h[6];
    tc_rom_export(e, k->base + (uint32_t)slot * k->slotsz, h, 6);
    if (hdr && n) tc_rom_export(e, k->base + (uint32_t)slot * k->slotsz, hdr, (uint32_t)(n < k->slotsz ? n : k->slotsz));
    return !memcmp(h, "TAMAGO", 6);
}
int dlc_used(Emu *e, const DlcKind *k)
{
    int u = 0;
    for (int s = 0; s < k->nslots; s++) if (dlc_slot(e, k, s, NULL, 0)) u++;
    return u;
}
int dlc_install(Emu *e, const DlcKind *k, const uint8_t *p, size_t len, int *slot_out)
{
    if (len > k->slotsz) return -1;
    uint8_t *cur = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!cur) return -2;
    int free_slot = -1;
    /* safety: every slot must hold a record or be erased - anything else means this game keeps its stores
     * somewhere else (an unknown version), and writing could damage its data */
    for (int s = 0; s < k->nslots; s++) {
        uint8_t h[16];
        tc_rom_export(e, k->base + (uint32_t)s * k->slotsz, h, 16);
        bool ff = true;
        for (int i = 0; i < 16; i++) if (h[i] != 0xFF) ff = false;
        if (!ff && memcmp(h, "TAMAGO", 6)) { free(cur); return -3; }
    }
    for (int s = 0; s < k->nslots; s++) {
        uint32_t off = k->base + (uint32_t)s * k->slotsz;
        tc_rom_export(e, off, cur, (uint32_t)len);
        if (!memcmp(cur, "TAMAGO", 6)) {
            if (!memcmp(cur, p, len)) { free(cur); if (slot_out) *slot_out = s; return 1; }   /* already there */
            continue;
        }
        if (free_slot < 0) {                          /* only a fully erased slot (never overwrite data) */
            bool erased = true;
            for (size_t i = 0; i < len; i++) if (cur[i] != 0xFF) { erased = false; break; }
            if (erased) free_slot = s;
        }
    }
    free(cur);
    if (free_slot < 0) return -1;
    if (tc_flash_program(e, k->base + (uint32_t)free_slot * k->slotsz, p, (uint32_t)len)) return -2;
    if (slot_out) *slot_out = free_slot;
    return 0;
}
int dlc_remove(Emu *e, const DlcKind *k, int slot)
{
    /* erase works on whole sectors: keep the rest of the sector, erase, write it back without this slot */
    uint32_t off = k->base + (uint32_t)slot * k->slotsz, base, size;
    tc_flash_sector(e, off, &base, &size);
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) return -1;
    tc_rom_export(e, base, buf, size);
    uint32_t a = off - base, n = k->slotsz;
    if (a + n > size) n = size - a;                   /* (slots never cross sectors in these layouts) */
    memset(buf + a, 0xFF, n);
    int r = tc_flash_erase(e, base);
    if (!r) r = tc_flash_program(e, base, buf, size);
    free(buf);
    return r;
}
