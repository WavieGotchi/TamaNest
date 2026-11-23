/* DLC: install downloadable content (items, food, games, destinations, patches delivered as destinations) into the
 * running game's flash. Store tables and routing follow tamaemu's tools/dlc.c by maragotchi (GPL-3.0). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tc.h"

typedef struct { int kind; uint32_t base, slotsz; int nslots; const char *label; } DlcKind;
typedef struct DlcDevice DlcDevice;

const DlcDevice *dlc_device(Emu *e);                 /* by the game's reset vector; NULL = no DLC for this game */
int dlc_kind_count(const DlcDevice *d);
const DlcKind *dlc_kind_at(const DlcDevice *d, int i);
const DlcKind *dlc_kind(const DlcDevice *d, int kind);

/* Read a file (raw TAMAGO record, or a JPEG with the record after it). 0 = ok; *out malloc'd (PSRAM). */
int dlc_extract(const char *path, uint8_t **out, size_t *len, const char **why);
/* Which store a record goes to: kind (-1 = none) and a short description. */
void dlc_route(const DlcDevice *d, const uint8_t *p, size_t len, int *kind, const char **label);
/* A printable name for a record: ASCII from its own name or id, else "" (the caller uses the file name). */
void dlc_name(const uint8_t *p, size_t len, char *out, size_t n);

/* Store access - call from the emulator task (between run chunks) only. */
int  dlc_used(Emu *e, const DlcKind *k);                                     /* occupied slots */
bool dlc_slot(Emu *e, const DlcKind *k, int slot, uint8_t *hdr, size_t n);   /* true = occupied; first n bytes */
/* Install: 0 ok, 1 already installed, -1 store full / too big, -2 flash write failed, -3 the store does not look
 * like one (unknown version: nothing written). *slot_out = slot used. */
int  dlc_install(Emu *e, const DlcKind *k, const uint8_t *p, size_t len, int *slot_out);
int  dlc_remove(Emu *e, const DlcKind *k, int slot);                         /* 0 ok */
