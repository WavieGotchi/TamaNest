/* High-level emulation (TamaLauncher) of the Plus Color's per-pixel layer compositor.
 *
 * The firmware copies a small graphics routine into RAM (A0RAM 0x380a-0x3945). For every screen pixel it
 * redraws, the routine at 0x389e ("get pixel") walks the layer list at 0x36c (16-byte entries, count at
 * 0x66c) from the top layer down, finds the first layer whose rectangle covers the pixel and whose pixel is
 * not transparent (0), and returns that colour index plus the layer's palette offset. 0x380a fetches one
 * pixel from a layer image (4 or 8 bits, optional horizontal/vertical flip).
 *
 * Layer entry:  +0 u32 image pointer (0 = unused)   +4 u16 x   +6 u16 y   +8 u8 width   +9 u8 height
 *               +0xb u8 palette offset   +0xc u8 flags (0x80 8-bit, 0x40 special, 0x20 v-flip, 0x10 h-flip)
 *               +0xe u16 frame offset into the image data
 * Image:        +0 u8 stride (pixels per row), pixel data starts at +6 (+2 then +4 in the original code).
 *
 * The routine only reads memory and returns a value in r4, so it can be computed natively. It is used only
 * when the code in RAM is exactly the analysed code (signature below). Layers with flag 0x40 call a ROM
 * routine we did not analyse: for those we return "not handled" and the emulated code runs as usual.
 * The cycles charged equal the guest instructions the original path would have executed, so game timing
 * stays the same. tc_hle_mode: 0 off, 1 replace the routine, 2 verify (compute natively AND run the real
 * routine, then compare the result and the cycle count at its return). */
#include "tc.h"

/* ---- Engine version 1: Plus Color (Paradise ROM). Get-pixel 0x389e (ret 0x3944), fetch 0x380a. ---- */
#define HLE_PIX_ADDR 0x389eu   /* "get pixel" entry */
#define HLE_RET_ADDR 0x3944u   /* its final ret */
#define HLE_SIG_ADDR 0x380au

/* ---- Engine version 2: iD L and P's (identical code at different addresses) ----
 * fetch: one pixel of a layer; 16-bit flags at +0xc: 0x8000 8-bit pixels, 0x4000 "special" layer group,
 *        0x2000 calls a ROM routine (not analysed: we fall back), 0x1000 character-map layer,
 *        0x40 x/y swapped, 0x20 v-flip, 0x10 h-flip.
 * gpA:   get-pixel over layers WITH 0x4000 (x, y kept on the stack).
 * gpB:   get-pixel over layers WITHOUT 0x4000; 0x1000 layers return a byte from the character map. */
typedef struct HleV2 {
    const char *name;
    uint32_t fetch, gpA, gpA_ret, gpB, gpB_ret, list, count, cmap;
    const uint16_t *sig; uint32_t sig_lo, sig_n;
    /* layer record layout (engine 3 = 4U+, 2026-11-22: 20-byte records, fields moved by +1/+2, the record
     * address is computed with mlt.h r1,0x14 - one more cycle per layer, and ALR = index * 20) */
    uint32_t stride, o_flags, o_pal, o_frame, per_layer;
    int mlt;
    /* 0x2000 layers in gpB (2026-11-22, 4U+): pixel = (~pix & 0xff) % header byte +2, through a ROM routine
     * (modfn) that calls the compiler's unsigned divide (udiv). 0 = not analysed: let the guest run it. */
    uint32_t modfn, udiv;
} HleV2;

int tc_hle_mode = 1;
TcHleStats tc_hle;

static const uint16_t hle_sig_idl[283] = {  /* 0x392-0x5c8 */
    0xa577, 0x3065, 0xc00c, 0x2c64, 0xc001, 0x7004, 0x6804, 0x1905, 0xa588, 0x2e74, 0x2e87, 0x2e48, 0xc00c, 0x2c64, 0xc000, 0x7204,
    0x6804, 0x1806, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00c, 0x2c64, 0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414,
    0xa547, 0xc00c, 0x2864, 0x6804, 0x0a0d, 0xc00e, 0x2c64, 0x2456, 0xaa68, 0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444,
    0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264,
    0xc004, 0x2444, 0x1f10, 0x8844, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004,
    0x2444, 0x70f4, 0x0640, 0x0203, 0x8402, 0xa966, 0x5c06, 0xa977, 0x5c17, 0xc047, 0x6dc4, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d4f,
    0x6c04, 0x2e14, 0x8c44, 0x2e40, 0xc03f, 0x6dc5, 0x2250, 0x3002, 0x6802, 0x193f, 0x2e14, 0xc00c, 0x2c03, 0x2e34, 0xc100, 0x7004,
    0x6804, 0x1937, 0x2e14, 0xc006, 0x2c04, 0x5015, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a, 0x2e14,
    0xc004, 0x2c04, 0x5005, 0x2645, 0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588, 0xdfff,
    0x1d70, 0x2e06, 0xa546, 0x2e34, 0xc080, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc048, 0xc013, 0x1d8b, 0xa546, 0xa546,
    0x6806, 0x1806, 0xc00b, 0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bb7, 0x2e14, 0x6c04, 0x8002, 0x0243,
    0x0640, 0x0203, 0xa963, 0xc047, 0x6dc4, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d5f, 0xa972, 0x2e14, 0x8c44, 0x2e40, 0xc03f, 0x6dc5,
    0x2250, 0x3004, 0x6804, 0x1950, 0x2e14, 0xc00c, 0x2c05, 0x2e54, 0xc100, 0x7004, 0x6804, 0x1b48, 0x2e14, 0xc006, 0x2c04, 0x2e26,
    0x2646, 0xa968, 0x6808, 0x0d40, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f3b, 0x2e14, 0xc004, 0x2c04, 0x2e36, 0x2646, 0xa967, 0x6807,
    0x0d33, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f2e, 0x2e14, 0x2e54, 0xc040, 0x7004, 0x6804, 0x190b, 0x2e24, 0x65c4, 0x8c74, 0x2243,
    0xad34, 0xc047, 0x6e85, 0x2254, 0x2444, 0x1e24, 0xa577, 0xa588, 0xdfff, 0x1d07, 0x2e06, 0xa546, 0xc00c, 0x2c04, 0xc080, 0x7004,
    0x6804, 0x190a, 0x3e64, 0xa546, 0x3004, 0xc002, 0x2447, 0xc048, 0xc013, 0x1c1f, 0xa546, 0x6806, 0x1806, 0xc00b, 0x2404, 0x2246,
    0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0ba6, 0x2e14, 0x6c04, 0x0243, 0x0640,
};
static const uint16_t hle_sig_ps[283] = {  /* 0x52c-0x762 */
    0xa577, 0x3065, 0xc00c, 0x2c64, 0xc001, 0x7004, 0x6804, 0x1905, 0xa588, 0x2e74, 0x2e87, 0x2e48, 0xc00c, 0x2c64, 0xc000, 0x7204,
    0x6804, 0x1806, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00c, 0x2c64, 0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414,
    0xa547, 0xc00c, 0x2864, 0x6804, 0x0a0d, 0xc00e, 0x2c64, 0x2456, 0xaa68, 0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444,
    0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264,
    0xc004, 0x2444, 0x1f10, 0x8844, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004,
    0x2444, 0x70f4, 0x0640, 0x0203, 0x8402, 0xa966, 0x5c06, 0xa977, 0x5c17, 0xc059, 0x6f04, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d4f,
    0x6c04, 0x2e14, 0x8c44, 0x2e40, 0xc051, 0x6f05, 0x2250, 0x3002, 0x6802, 0x193f, 0x2e14, 0xc00c, 0x2c03, 0x2e34, 0xc100, 0x7004,
    0x6804, 0x1937, 0x2e14, 0xc006, 0x2c04, 0x5015, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a, 0x2e14,
    0xc004, 0x2c04, 0x5005, 0x2645, 0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588, 0xdfff,
    0x1d70, 0x2e06, 0xa546, 0x2e34, 0xc080, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc048, 0xc003, 0x1d00, 0xa546, 0xa546,
    0x6806, 0x1806, 0xc00b, 0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bb7, 0x2e14, 0x6c04, 0x8002, 0x0243,
    0x0640, 0x0203, 0xa963, 0xc059, 0x6f04, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d5f, 0xa972, 0x2e14, 0x8c44, 0x2e40, 0xc051, 0x6f05,
    0x2250, 0x3004, 0x6804, 0x1950, 0x2e14, 0xc00c, 0x2c05, 0x2e54, 0xc100, 0x7004, 0x6804, 0x1b48, 0x2e14, 0xc006, 0x2c04, 0x2e26,
    0x2646, 0xa968, 0x6808, 0x0d40, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f3b, 0x2e14, 0xc004, 0x2c04, 0x2e36, 0x2646, 0xa967, 0x6807,
    0x0d33, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f2e, 0x2e14, 0x2e54, 0xc040, 0x7004, 0x6804, 0x190b, 0x2e24, 0x65c4, 0x8c74, 0x2243,
    0xad34, 0xc059, 0x6fc5, 0x2254, 0x2444, 0x1e24, 0xa577, 0xa588, 0xdfff, 0x1d07, 0x2e06, 0xa546, 0xc00c, 0x2c04, 0xc080, 0x7004,
    0x6804, 0x190a, 0x3e64, 0xa546, 0x3004, 0xc002, 0x2447, 0xc048, 0xc002, 0x1c94, 0xa546, 0x6806, 0x1806, 0xc00b, 0x2404, 0x2246,
    0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0ba6, 0x2e14, 0x6c04, 0x0243, 0x0640,
};

static const uint16_t hle_sig_4up[285] = {  /* 0x422-0x65a (4U+ English, 2026-11-22) */
    0xa577, 0x3065, 0xc00e, 0x2c64, 0xc001, 0x7004, 0x6804, 0x1905, 0xa588, 0x2e74, 0x2e87, 0x2e48, 0xc00e, 0x2c64, 0xc000, 0x7204,
    0x6804, 0x1806, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00e, 0x2c64, 0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414,
    0xa547, 0xc00e, 0x2864, 0x6804, 0x0a0d, 0xc010, 0x2c64, 0x2456, 0xaa68, 0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444,
    0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc010, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264,
    0xc004, 0x2444, 0x1f10, 0x8844, 0xc010, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004,
    0x2444, 0x70f4, 0x0640, 0x0203, 0x8402, 0xa966, 0x5c06, 0xa977, 0x5c17, 0xc060, 0x6c44, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d50,
    0x6c04, 0x6d44, 0xa241, 0xa424, 0x2e40, 0xc051, 0x6c45, 0x2250, 0x3002, 0x6802, 0x193f, 0x2e14, 0xc00e, 0x2c03, 0x2e34, 0xc100,
    0x7004, 0x6804, 0x1937, 0x2e14, 0xc006, 0x2c04, 0x5015, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a,
    0x2e14, 0xc004, 0x2c04, 0x5005, 0x2645, 0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588,
    0xdfff, 0x1d6f, 0x2e06, 0xa546, 0x2e34, 0xc080, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc048, 0xc002, 0x1de1, 0xa546,
    0xa546, 0x6806, 0x1806, 0xc00c, 0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bb6, 0x6d44, 0x6c04, 0x8002,
    0x0243, 0x0640, 0x0203, 0xa963, 0xc060, 0x6c44, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d60, 0xa972, 0x6d44, 0xa241, 0xa424, 0x2e40,
    0xc051, 0x6c45, 0x2250, 0x3004, 0x6804, 0x1950, 0x2e14, 0xc00e, 0x2c05, 0x2e54, 0xc100, 0x7004, 0x6804, 0x1b48, 0x2e14, 0xc006,
    0x2c04, 0x2e26, 0x2646, 0xa968, 0x6808, 0x0d40, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f3b, 0x2e14, 0xc004, 0x2c04, 0x2e36, 0x2646,
    0xa967, 0x6807, 0x0d33, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f2e, 0x2e14, 0x2e54, 0xc040, 0x7004, 0x6804, 0x190b, 0x2e24, 0x65c4,
    0x8c74, 0x2243, 0xad34, 0xc060, 0x6d05, 0x2254, 0x2444, 0x1e24, 0xa577, 0xa588, 0xdfff, 0x1d05, 0x2e06, 0xa546, 0xc00e, 0x2c04,
    0xc080, 0x7004, 0x6804, 0x190a, 0x3e64, 0xa546, 0x3004, 0xc002, 0x2447, 0xc048, 0xc002, 0x1c74, 0xa546, 0x6806, 0x1806, 0xc00c,
    0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0ba5, 0x6d44, 0x6c04, 0x0243, 0x0640,
};

/* 2026-11-23: more versions of the same engine, found in A0RAM by the signature scan (scratchpad sigscan.c): the code
 * is identical apart from the data addresses inside it (decoded from ext+ld pairs) and pc-relative calls. */
static const uint16_t hle_sig_idl15[283] = {  /* iD L 15th Anniversary + Princess Spacy: 0x422-0x658 */
    0xa577, 0x3065, 0xc00c, 0x2c64, 0xc001, 0x7004, 0x6804, 0x1905, 0xa588, 0x2e74, 0x2e87, 0x2e48, 0xc00c, 0x2c64, 0xc000, 0x7204,
    0x6804, 0x1806, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00c, 0x2c64, 0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414,
    0xa547, 0xc00c, 0x2864, 0x6804, 0x0a0d, 0xc00e, 0x2c64, 0x2456, 0xaa68, 0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444,
    0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264,
    0xc004, 0x2444, 0x1f10, 0x8844, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004,
    0x2444, 0x70f4, 0x0640, 0x0203, 0x8402, 0xa966, 0x5c06, 0xa977, 0x5c17, 0xc04b, 0x6f44, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d4f,
    0x6c04, 0x2e14, 0x8c44, 0x2e40, 0xc043, 0x6f45, 0x2250, 0x3002, 0x6802, 0x193f, 0x2e14, 0xc00c, 0x2c03, 0x2e34, 0xc100, 0x7004,
    0x6804, 0x1937, 0x2e14, 0xc006, 0x2c04, 0x5015, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a, 0x2e14,
    0xc004, 0x2c04, 0x5005, 0x2645, 0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588, 0xdfff,
    0x1d70, 0x2e06, 0xa546, 0x2e34, 0xc080, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc048, 0xc013, 0x1d43, 0xa546, 0xa546,
    0x6806, 0x1806, 0xc00b, 0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bb7, 0x2e14, 0x6c04, 0x8002, 0x0243,
    0x0640, 0x0203, 0xa963, 0xc04b, 0x6f44, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d5f, 0xa972, 0x2e14, 0x8c44, 0x2e40, 0xc043, 0x6f45,
    0x2250, 0x3004, 0x6804, 0x1950, 0x2e14, 0xc00c, 0x2c05, 0x2e54, 0xc100, 0x7004, 0x6804, 0x1b48, 0x2e14, 0xc006, 0x2c04, 0x2e26,
    0x2646, 0xa968, 0x6808, 0x0d40, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f3b, 0x2e14, 0xc004, 0x2c04, 0x2e36, 0x2646, 0xa967, 0x6807,
    0x0d33, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f2e, 0x2e14, 0x2e54, 0xc040, 0x7004, 0x6804, 0x190b, 0x2e24, 0x65c4, 0x8c74, 0x2243,
    0xad34, 0xc04c, 0x6c05, 0x2254, 0x2444, 0x1e24, 0xa577, 0xa588, 0xdfff, 0x1d07, 0x2e06, 0xa546, 0xc00c, 0x2c04, 0xc080, 0x7004,
    0x6804, 0x190a, 0x3e64, 0xa546, 0x3004, 0xc002, 0x2447, 0xc048, 0xc012, 0x1cd7, 0xa546, 0x6806, 0x1806, 0xc00b, 0x2404, 0x2246,
    0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0ba6, 0x2e14, 0x6c04, 0x0243, 0x0640,
};
static const uint16_t hle_sig_4u[285] = {  /* 4U: 0x52c-0x766 */
    0xa577, 0x3065, 0xc00e, 0x2c64, 0xc001, 0x7004, 0x6804, 0x1905, 0xa588, 0x2e74, 0x2e87, 0x2e48, 0xc00e, 0x2c64, 0xc000, 0x7204,
    0x6804, 0x1806, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00e, 0x2c64, 0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414,
    0xa547, 0xc00e, 0x2864, 0x6804, 0x0a0d, 0xc010, 0x2c64, 0x2456, 0xaa68, 0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444,
    0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc010, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264,
    0xc004, 0x2444, 0x1f10, 0x8844, 0xc010, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004,
    0x2444, 0x70f4, 0x0640, 0x0203, 0x8402, 0xa966, 0x5c06, 0xa977, 0x5c17, 0xc061, 0x6d84, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d50,
    0x6c04, 0x6d44, 0xa241, 0xa424, 0x2e40, 0xc054, 0x6f85, 0x2250, 0x3002, 0x6802, 0x193f, 0x2e14, 0xc00e, 0x2c03, 0x2e34, 0xc100,
    0x7004, 0x6804, 0x1937, 0x2e14, 0xc006, 0x2c04, 0x5015, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a,
    0x2e14, 0xc004, 0x2c04, 0x5005, 0x2645, 0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588,
    0xdfff, 0x1d6f, 0x2e06, 0xa546, 0x2e34, 0xc080, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc048, 0xc002, 0x1d5c, 0xa546,
    0xa546, 0x6806, 0x1806, 0xc00c, 0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bb6, 0x6d44, 0x6c04, 0x8002,
    0x0243, 0x0640, 0x0203, 0xa963, 0xc061, 0x6d84, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d60, 0xa972, 0x6d44, 0xa241, 0xa424, 0x2e40,
    0xc054, 0x6f85, 0x2250, 0x3004, 0x6804, 0x1950, 0x2e14, 0xc00e, 0x2c05, 0x2e54, 0xc100, 0x7004, 0x6804, 0x1b48, 0x2e14, 0xc006,
    0x2c04, 0x2e26, 0x2646, 0xa968, 0x6808, 0x0d40, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f3b, 0x2e14, 0xc004, 0x2c04, 0x2e36, 0x2646,
    0xa967, 0x6807, 0x0d33, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f2e, 0x2e14, 0x2e54, 0xc040, 0x7004, 0x6804, 0x190b, 0x2e24, 0x65c4,
    0x8c74, 0x2243, 0xad34, 0xc061, 0x6e45, 0x2254, 0x2444, 0x1e24, 0xa577, 0xa588, 0xdfff, 0x1d05, 0x2e06, 0xa546, 0xc00e, 0x2c04,
    0xc080, 0x7004, 0x6804, 0x190a, 0x3e64, 0xa546, 0x3004, 0xc002, 0x2447, 0xc048, 0xc001, 0x1cef, 0xa546, 0x6806, 0x1806, 0xc00c,
    0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0ba5, 0x6d44, 0x6c04, 0x0243, 0x0640,
};
static const uint16_t hle_sig_id[283] = {  /* iD: 0x3c8-0x5fe */
    0xa577, 0x3065, 0xc00c, 0x2c64, 0xc001, 0x7004, 0x6804, 0x1905, 0xa588, 0x2e74, 0x2e87, 0x2e48, 0xc00c, 0x2c64, 0xc000, 0x7204,
    0x6804, 0x1806, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00c, 0x2c64, 0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414,
    0xa547, 0xc00c, 0x2864, 0x6804, 0x0a0d, 0xc00e, 0x2c64, 0x2456, 0xaa68, 0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444,
    0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264,
    0xc004, 0x2444, 0x1f10, 0x8844, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004,
    0x2444, 0x70f4, 0x0640, 0x0203, 0x8402, 0xa966, 0x5c06, 0xa977, 0x5c17, 0xc044, 0x6c04, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d4f,
    0x6c04, 0x2e14, 0x8c44, 0x2e40, 0xc03c, 0x6c05, 0x2250, 0x3002, 0x6802, 0x193f, 0x2e14, 0xc00c, 0x2c03, 0x2e34, 0xc100, 0x7004,
    0x6804, 0x1937, 0x2e14, 0xc006, 0x2c04, 0x5015, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a, 0x2e14,
    0xc004, 0x2c04, 0x5005, 0x2645, 0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588, 0xdfff,
    0x1d70, 0x2e06, 0xa546, 0x2e34, 0xc080, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc018, 0xc00e, 0x1d73, 0xa546, 0xa546,
    0x6806, 0x1806, 0xc00b, 0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bb7, 0x2e14, 0x6c04, 0x8002, 0x0243,
    0x0640, 0x0203, 0xa963, 0xc044, 0x6c04, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d5f, 0xa972, 0x2e14, 0x8c44, 0x2e40, 0xc03c, 0x6c05,
    0x2250, 0x3004, 0x6804, 0x1950, 0x2e14, 0xc00c, 0x2c05, 0x2e54, 0xc100, 0x7004, 0x6804, 0x1b48, 0x2e14, 0xc006, 0x2c04, 0x2e26,
    0x2646, 0xa968, 0x6808, 0x0d40, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f3b, 0x2e14, 0xc004, 0x2c04, 0x2e36, 0x2646, 0xa967, 0x6807,
    0x0d33, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f2e, 0x2e14, 0x2e54, 0xc040, 0x7004, 0x6804, 0x190b, 0x2e24, 0x65c4, 0x8c74, 0x2243,
    0xad34, 0xc044, 0x6cc5, 0x2254, 0x2444, 0x1e24, 0xa577, 0xa588, 0xdfff, 0x1d07, 0x2e06, 0xa546, 0xc00c, 0x2c04, 0xc080, 0x7004,
    0x6804, 0x190a, 0x3e64, 0xa546, 0x3004, 0xc002, 0x2447, 0xc018, 0xc00e, 0x1c07, 0xa546, 0x6806, 0x1806, 0xc00b, 0x2404, 0x2246,
    0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0ba6, 0x2e14, 0x6c04, 0x0243, 0x0640,
};
static const uint16_t hle_sig_idm[283] = {  /* iD Lovely Melody: 0x3c8-0x5fe */
    0xa577, 0x3065, 0xc00c, 0x2c64, 0xc001, 0x7004, 0x6804, 0x1905, 0xa588, 0x2e74, 0x2e87, 0x2e48, 0xc00c, 0x2c64, 0xc000, 0x7204,
    0x6804, 0x1806, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00c, 0x2c64, 0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414,
    0xa547, 0xc00c, 0x2864, 0x6804, 0x0a0d, 0xc00e, 0x2c64, 0x2456, 0xaa68, 0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444,
    0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264,
    0xc004, 0x2444, 0x1f10, 0x8844, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004,
    0x2444, 0x70f4, 0x0640, 0x0203, 0x8402, 0xa966, 0x5c06, 0xa977, 0x5c17, 0xc04f, 0x6e44, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d4f,
    0x6c04, 0x2e14, 0x8c44, 0x2e40, 0xc047, 0x6e45, 0x2250, 0x3002, 0x6802, 0x193f, 0x2e14, 0xc00c, 0x2c03, 0x2e34, 0xc100, 0x7004,
    0x6804, 0x1937, 0x2e14, 0xc006, 0x2c04, 0x5015, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a, 0x2e14,
    0xc004, 0x2c04, 0x5005, 0x2645, 0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588, 0xdfff,
    0x1d70, 0x2e06, 0xa546, 0x2e34, 0xc080, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc018, 0xc00e, 0x1d73, 0xa546, 0xa546,
    0x6806, 0x1806, 0xc00b, 0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bb7, 0x2e14, 0x6c04, 0x8002, 0x0243,
    0x0640, 0x0203, 0xa963, 0xc04f, 0x6e44, 0x2444, 0x6414, 0xa941, 0x6801, 0x0d5f, 0xa972, 0x2e14, 0x8c44, 0x2e40, 0xc047, 0x6e45,
    0x2250, 0x3004, 0x6804, 0x1950, 0x2e14, 0xc00c, 0x2c05, 0x2e54, 0xc100, 0x7004, 0x6804, 0x1b48, 0x2e14, 0xc006, 0x2c04, 0x2e26,
    0x2646, 0xa968, 0x6808, 0x0d40, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f3b, 0x2e14, 0xc004, 0x2c04, 0x2e36, 0x2646, 0xa967, 0x6807,
    0x0d33, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f2e, 0x2e14, 0x2e54, 0xc040, 0x7004, 0x6804, 0x190b, 0x2e24, 0x65c4, 0x8c74, 0x2243,
    0xad34, 0xc04f, 0x6f05, 0x2254, 0x2444, 0x1e24, 0xa577, 0xa588, 0xdfff, 0x1d07, 0x2e06, 0xa546, 0xc00c, 0x2c04, 0xc080, 0x7004,
    0x6804, 0x190a, 0x3e64, 0xa546, 0x3004, 0xc002, 0x2447, 0xc018, 0xc00e, 0x1c07, 0xa546, 0x6806, 0x1806, 0xc00b, 0x2404, 0x2246,
    0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0ba6, 0x2e14, 0x6c04, 0x0243, 0x0640,
};
/* kinds: index 0..2 = engine kinds 2..4, index 3.. = kinds 6.. (5 is Hexagontchi, engine 1) */
static const HleV2 hle_v2[7] = {
    { "iD L", 0x392, 0x438, 0x4f2, 0x4f4, 0x5c6, 0xfdc, 0x11dc, 0x11e8, hle_sig_idl, 0x392, 283, 16, 0xc, 0xb, 0xe, 9, 0, 0x2402be2, 0x2402b92 },
    { "P's",  0x52c, 0x5d2, 0x68c, 0x68e, 0x760, 0x1470, 0x1670, 0x167c, hle_sig_ps, 0x52c, 283, 16, 0xc, 0xb, 0xe, 9, 0, 0x2400c66, 0x2400c16 },
    { "4U+",  0x422, 0x4c8, 0x584, 0x586, 0x65a, 0x1444, 0x1804, 0x1810, hle_sig_4up, 0x422, 285, 20, 0xe, 0xc, 0x10, 10, 1, 0x2400b20, 0x2400ad0 },
    { "iD L 15th/Spacy", 0x422, 0x4c8, 0x582, 0x584, 0x656, 0x10f4, 0x12f4, 0x1300, hle_sig_idl15, 0x422, 283, 16, 0xc, 0xb, 0xe, 9, 0, 0x2402be2, 0x2402b92 },
    { "4U",   0x52c, 0x5d2, 0x68e, 0x690, 0x764, 0x1538, 0x1858, 0x1864, hle_sig_4u, 0x52c, 285, 20, 0xe, 0xc, 0x10, 10, 1, 0x2400b20, 0x2400ad0 },
    { "iD",   0x3c8, 0x46e, 0x528, 0x52a, 0x5fc, 0x0f00, 0x1100, 0x110c, hle_sig_id, 0x3c8, 283, 16, 0xc, 0xb, 0xe, 9, 0, 0xc021e8, 0xc02198 },
    { "iD Melody", 0x3c8, 0x46e, 0x528, 0x52a, 0x5fc, 0x11e4, 0x13e4, 0x13f0, hle_sig_idm, 0x3c8, 283, 16, 0xc, 0xb, 0xe, 9, 0, 0xc021e8, 0xc02198 },
};
#define HV2(kind) (&hle_v2[(kind) <= 4 ? (kind) - 2 : (kind) - 3])

static const uint16_t hle_sig[158] = {
    0xa577, 0x3065, 0xc00c, 0x2464, 0xc000, 0x7204, 0x6804, 0x1907, 0xa588, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00c, 0x2464,
    0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414, 0xa547, 0xc00c, 0x2064, 0x6804, 0x0a0d, 0xc00e, 0x2c64, 0x2456, 0xaa68,
    0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444, 0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc00e, 0x2c64, 0x2456, 0x8816,
    0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444, 0x1f10, 0x8844, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68,
    0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444, 0x70f4, 0x0640, 0x0203, 0x8401, 0xa966, 0x5c06, 0xc019, 0x6ec4,
    0x2444, 0x6414, 0xa941, 0x6801, 0x0d46, 0xa973, 0x2e14, 0x8c44, 0x2e40, 0xc00d, 0x6ec5, 0x2250, 0x3002, 0x6802, 0x1937, 0x2e14,
    0xc006, 0x2c04, 0x2e35, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a, 0x2e14, 0xc004, 0x2c04, 0x5005,
    0x2645, 0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588, 0x1d84, 0x2e06, 0xa546, 0xc00c,
    0x2404, 0xc001, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc018, 0xc1f0, 0x1ded, 0xa546, 0xa546, 0x6806, 0x1806, 0xc00b,
    0x2404, 0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bbf, 0x2e14, 0x6c04, 0x8001, 0x0243, 0x0640,
};

/* Hexagontchi (2026-11-22): the Plus Color engine rebuilt with other addresses: code 0x174 bytes lower
 * (fetch 0x3696, get-pixel 0x372a, ret 0x37ce), layer list at 0x4 (loaded with a one-word ld.w, so each
 * layer costs 8 cycles instead of 9), count 0x204; the draw-window routine 0x176 lower, buffer 0x88c,
 * colour tables 0x78c/0x68c, gradient tables 0x44c/0x20c, gradient range bytes 0x841c9/0x841ca. */
static const uint16_t hle_sig_hexa[157] = {  /* 0x3696-0x37ce */
    0xa577, 0x3065, 0xc00c, 0x2464, 0xc000, 0x7204, 0x6804, 0x1907, 0xa588, 0xc009, 0x2464, 0x2684, 0x6414, 0xa548, 0xc00c, 0x2464,
    0x7104, 0x6804, 0x1806, 0xc008, 0x2464, 0x2674, 0x6414, 0xa547, 0xc00c, 0x2064, 0x6804, 0x0a0d, 0xc00e, 0x2c64, 0x2456, 0xaa68,
    0xa426, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444, 0x1e22, 0x2e74, 0x7014, 0x6804, 0x1810, 0xc00e, 0x2c64, 0x2456, 0x8816,
    0xaa68, 0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444, 0x1f10, 0x8844, 0xc00e, 0x2c64, 0x2456, 0x8816, 0xaa68,
    0xa426, 0x8817, 0x6025, 0x2254, 0x2274, 0x2264, 0xc004, 0x2444, 0x70f4, 0x0640, 0x0203, 0x8401, 0xa966, 0x5c06, 0xc008, 0x6c44,
    0x2444, 0x6414, 0xa941, 0x6801, 0x0d45, 0xa973, 0x2e14, 0x8c44, 0x2e40, 0x6c45, 0x2250, 0x3002, 0x6802, 0x1937, 0x2e14, 0xc006,
    0x2c04, 0x2e35, 0x2645, 0xa958, 0x6808, 0x0d2f, 0x2e14, 0xc009, 0x2404, 0x2a84, 0x0f2a, 0x2e14, 0xc004, 0x2c04, 0x5005, 0x2645,
    0xa957, 0x6807, 0x0d22, 0x2e14, 0xc008, 0x2404, 0x2a74, 0x0f1d, 0x2e14, 0xa577, 0xa588, 0x1d85, 0x2e06, 0xa546, 0xc00c, 0x2404,
    0xc001, 0x7004, 0x6804, 0x1909, 0x3e64, 0xc002, 0x2427, 0xc018, 0xc28d, 0x1df8, 0xa546, 0xa546, 0x6806, 0x1806, 0xc00b, 0x2404,
    0x2246, 0x1f09, 0xa564, 0x2e14, 0x6414, 0xa941, 0x6801, 0x0bc0, 0x2e14, 0x6c04, 0x8001, 0x0243, 0x0640,
};
typedef struct HleV1 {
    const char *name;
    uint32_t pix, ret, list, count, layer_cost;           /* get-pixel: entry, its ret, layer list/count, cycles per layer */
    uint32_t D;                                           /* draw-window routine: Plus Color address - this game's */
    uint32_t buf, tab_a, tab_b, gtab_a, gtab_b, ghi;      /* draw-window data */
    const uint16_t *sig; uint32_t sig_lo, sig_n;
} HleV1;
static const uint16_t hle_sig[158];
static const HleV1 hle_v1[2] = {
    { "Plus Color",  0x389e, 0x3944, 0x36c, 0x66c, 9, 0,     0xcf4, 0xbf4, 0xaf4, 0x8b4, 0x674, 0x841d2, hle_sig,      0x380a, 158 },
    { "Hexagontchi", 0x372a, 0x37ce, 0x4,   0x204, 8, 0x176, 0x88c, 0x78c, 0x68c, 0x44c, 0x20c, 0x841ca, hle_sig_hexa, 0x3696, 157 },
};
#define HV1(kind) (&hle_v1[(kind) == 5])

static int hle_sig_match(Emu *e, uint32_t lo, const uint16_t *sig, uint32_t n)
{
    const uint8_t *p = e->a0ram + lo;
    for (uint32_t i = 0; i < n; i++)
        if ((uint16_t)(p[2 * i] | (p[2 * i + 1] << 8)) != sig[i]) return 0;
    return 1;
}

/* Debug: first mismatching halfword of version-2 entry i, or -1 if it matches. */
int tc_hle_sig_debug(Emu *e, int i, uint32_t *addr, uint32_t *have, uint32_t *want)
{
    const HleV2 *v = &hle_v2[i];
    const uint8_t *p = e->a0ram + v->sig_lo;
    for (uint32_t k = 0; k < v->sig_n; k++) {
        uint32_t h = (uint32_t)(p[2 * k] | (p[2 * k + 1] << 8));
        if (h != v->sig[k]) { *addr = v->sig_lo + 2 * k; *have = h; *want = v->sig[k]; return (int)k; }
    }
    return -1;
}

/* Which known engine is in A0RAM? 0 none, 1 Plus Color, 2.. version 2 entries. (Checked once per tc_turbo call.) */
static int hle_sig_ok(Emu *e)
{
    if (e->dev.a0ram_base != 0) return 0;
    if (hle_sig_match(e, HLE_SIG_ADDR, hle_sig, 158)) return 1;
    for (int i = 0; i < 7; i++)
        if (hle_sig_match(e, hle_v2[i].sig_lo, hle_v2[i].sig, hle_v2[i].sig_n)) return i < 3 ? 2 + i : 3 + i;
    if (hle_sig_match(e, hle_v1[1].sig_lo, hle_v1[1].sig, hle_v1[1].sig_n)) return 5;   /* Hexagontchi */
    return 0;
}

/* Native 0x380a: one pixel of layer L (image P) at (dx, dy). *c gets the guest cycles of that path. */
static inline uint32_t hle_layer_pixel(Emu *e, uint32_t L, uint32_t P, uint32_t dx, uint32_t dy, uint32_t f, uint32_t *c)
{
    uint32_t n = 9 + 5 + 4;                                   /* entry, h-flip test, 8-bit test */
    if (f & 0x20) { dy = (f_rd8(e, L + 9) - dy - 1) & 0xFF; n += 5; }
    if (f & 0x10) { dx = (f_rd8(e, L + 8) - dx - 1) & 0xFF; n += 5; }
    uint32_t off = f_rd16(e, L + 0xE), stride = f_rd8(e, P), v;
    if (f & 0x80) {                                           /* 8 bits per pixel */
        v = f_rd8(e, off + P + 2 + dx + dy * stride + 4);
        n += 13;
    } else {                                                  /* 4 bits per pixel, two per byte */
        uint32_t b = f_rd8(e, off + P + 2 + (dx >> 1) + dy * (stride >> 1) + 4);
        v = (dx & 1) ? (b >> 4) : (b & 0xF);
        n += 4 + 15 + (dx & 1);                               /* odd pixels: one more instruction (srl in the delay slot) */
    }
    *c += n;
    return v;
}

/* Native 0x389e for pixel (r6, r7). Returns 1 and the result/cycles, or 0 if the guest must run it.
 * 2026-11-18: per-row cache. The game asks for every pixel of a row in turn; the layer list (up to 48
 * layers of 16 bytes at 0x36c) was re-read for every pixel, which made arcade games (many sprites) run
 * at ~65%. Now the layers are decoded once per row: layers that miss the row become a plain cycle
 * count, only the layers crossing the row are tested per pixel. Same results and the same guest cycle
 * counts as the original routine (checked by the 1-in-64 spot checks). Rebuilt for every new row, when
 * the layer count changes, and at least every 160 calls. */
typedef struct HleRowEnt {
    uint32_t cost;                     /* cycles of the rejected layers before this entry */
    uint8_t  kind;                     /* 0 = only cost (end), 1 = layer crossing the row, 2 = bail (I/O pointer) */
    int32_t  x0, dy;
    uint32_t w, h, f, pal, off, stride, P;
} HleRowEnt;
static HleRowEnt hle_row[50];
static int hle_row_n, hle_row_cnt = -1, hle_row_calls;
static const HleV1 *hle_row_v;
static int32_t hle_row_y = 0x7fffffff;
static Emu *hle_row_emu;
static int32_t hle_row_x;                   /* x of the last call */

static void hle_row_build(Emu *e, const HleV1 *v1, int32_t y, int32_t cnt)
{
    int n = 0;
    uint32_t cost = 0;
    for (int32_t i = cnt - 1; i >= 0; i--) {
        uint32_t L = v1->list + (uint32_t)i * 16;
        cost += v1->layer_cost;
        uint32_t P = f_rd32(e, L);
        if (P == 0) { cost += 5; continue; }
        if (P - e->dev.io_base < e->dev.io_size) {         /* never touch I/O registers: bail when reached */
            hle_row[n].cost = cost; hle_row[n].kind = 2; n++; cost = 0; continue;
        }
        int32_t dy = (int16_t)(y - (int32_t)f_rd16(e, L + 6));
        cost += 8; if (dy < 0) { cost += 5; continue; }
        uint32_t h = f_rd8(e, L + 9);
        cost += 5; if ((int32_t)h <= dy) { cost += 5; continue; }
        HleRowEnt *r = &hle_row[n++];
        r->cost = cost; r->kind = 1; cost = 0;
        r->x0 = (int32_t)f_rd16(e, L + 4); r->dy = dy;
        r->w = f_rd8(e, L + 8); r->h = h; r->f = f_rd8(e, L + 0xC); r->pal = f_rd8(e, L + 0xB);
        r->off = f_rd16(e, L + 0xE); r->stride = f_rd8(e, P); r->P = P;
    }
    hle_row[n].cost = cost; hle_row[n].kind = 0; n++;
    hle_row_n = n; hle_row_y = y; hle_row_cnt = cnt; hle_row_calls = 0; hle_row_emu = e; hle_row_v = v1;
}

static int hle_pixel(Emu *e, const HleV1 *v1, uint32_t *res, uint32_t *cyc)
{
    int32_t x = (int16_t)e->r[6], y = (int16_t)e->r[7];
    int32_t cnt = (int16_t)f_rd8(e, v1->count);
    uint32_t c = 12;
    if (cnt - 1 < 0) { *res = 0; *cyc = c + 4; return 1; }
    c += 1;                                                   /* first pass enters at 0x38b6 */
    /* Reuse the row only for the next pixel of the same row (x one further, same y, same layer count);
     * anything else - a new row, a new drawing pass, sprites moved in between - rebuilds it. */
    if (!(y == hle_row_y && x == hle_row_x + 1 && cnt == hle_row_cnt && e == hle_row_emu && v1 == hle_row_v && ++hle_row_calls <= 160))
        hle_row_build(e, v1, y, cnt);
    hle_row_x = x;
    for (int k = 0; k < hle_row_n; k++) {
        const HleRowEnt *r = &hle_row[k];
        c += r->cost;
        if (r->kind == 0) break;
        if (r->kind == 2) return 0;
        int32_t dx = (int16_t)(x - r->x0);
        c += 8; if (dx < 0) { c += 5; continue; }
        c += 5; if ((int32_t)r->w <= dx) { c += 5; continue; }
        uint32_t f = r->f;
        if (f & 0x40) return 0;                               /* special layer: let the guest run it */
        c += 4;
        /* = hle_layer_pixel with the row's cached header values */
        uint32_t n = 9 + 5 + 4, ddx = (uint32_t)dx & 0xFF, ddy = (uint32_t)r->dy & 0xFF, v;
        if (f & 0x20) { ddy = (r->h - ddy - 1) & 0xFF; n += 5; }
        if (f & 0x10) { ddx = (r->w - ddx - 1) & 0xFF; n += 5; }
        if (f & 0x80) { v = f_rd8(e, r->off + r->P + 2 + ddx + ddy * r->stride + 4); n += 13; }
        else {
            uint32_t b = f_rd8(e, r->off + r->P + 2 + (ddx >> 1) + ddy * (r->stride >> 1) + 4);
            v = (ddx & 1) ? (b >> 4) : (b & 0xF);
            n += 4 + 15 + (ddx & 1);
        }
        c += n;
        v &= 0xFF;
        c += 8 + 2;
        if (v == 0) { c += 1 + 5; continue; }                 /* transparent: look at the layer below */
        *res = (v + r->pal) & 0xFF;
        *cyc = c + 5 + 3;
        return 1;
    }
    *res = 0; *cyc = c + 4;
    return 1;
}

/* Version 2 fetch (iD L / P's). Cycles as executed by the original, including its ret. */
static inline uint32_t hle2_fetch(Emu *e, const HleV2 *hv, uint32_t L, uint32_t P, uint32_t dx, uint32_t dy, uint32_t f, uint32_t *c, uint32_t *alr)
{
    uint32_t n = 9 + 6 + 5 + 4;                                /* entry, v-flip test, h-flip test, 8-bit test */
    if (f & 0x40) { uint32_t t = dx; dx = dy; dy = t; n += 3; }
    if (f & 0x20) { dy = (f_rd8(e, L + 9) - dy - 1) & 0xFF; n += 5; }
    if (f & 0x10) { dx = (f_rd8(e, L + 8) - dx - 1) & 0xFF; n += 5; }
    uint32_t off = f_rd16(e, L + hv->o_frame), stride = f_rd8(e, P), v;
    if (f & 0x8000) {
        v = f_rd8(e, off + P + 2 + dx + dy * stride + 4);
        *alr = dy * stride;                                   /* mlt.w r8,r6: ALR (AHR = 0, small product) */
        n += 13;
    } else {
        uint32_t b = f_rd8(e, off + P + 2 + (dx >> 1) + dy * (stride >> 1) + 4);
        *alr = dy * (stride >> 1);
        v = (dx & 1) ? (b >> 4) : (b & 0xF);
        n += 4 + 15 + (dx & 1);
    }
    *c += n;
    return v;
}

/* The compiler's unsigned divide (4U+ ROM 0x2400ad0: r6 / r7 -> quotient r4, remainder r5), shift-and-subtract,
 * every instruction one 16-bit word: returns its cycles from entry to its ret (included). */
static uint32_t udiv_cycles(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t c = 2, r5 = 1, r6 = n, r7 = d;                  /* ld.w r5,1 ; ld.w r4,0 */
    for (;;) {                                              /* 0x...ad4: scale the divisor up */
        c += 2; if (r7 >= r6) break;                         /* cmp r7,r6 ; jruge */
        c += 2; if (r5 == 0) break;                          /* cmp r5,0 ; jreq */
        c += 2; if ((int32_t)r7 < 0) break;                  /* cmp r7,0 ; jrlt */
        r7 <<= 1; r5 <<= 1; c += 3;                          /* sll ; sll ; jp */
    }
    for (;;) {                                              /* 0x...af2: subtract down */
        c += 2; if (r5 == 0) break;                          /* cmp r5,0 ; jrne */
        c += 2; if (r6 >= r7) { r6 -= r7; c += 2; }          /* cmp r6,r7 ; jrult ; (sub ; or) */
        r5 >>= 1; r7 >>= 1; c += 2;                          /* srl ; srl */
    }
    *rem = r6;
    return c + 2;                                           /* ld.w r5,r6 ; ret */
}
static const uint16_t hle_modfn_4up[5] = { 0xdff8, 0xdfff, 0x1cd6, 0x2e54, 0x0640 };
static const uint16_t hle_udiv_4up[21] = { 0x6c15, 0x6c04, 0x2a67, 0x120e, 0x6805, 0x180c, 0x6807, 0x0c0a, 0x8c17, 0x8c15, 0x1ef8,
                                           0x2a76, 0x1403, 0x2676, 0x3654, 0x8815, 0x8817, 0x6805, 0x1af9, 0x2e65, 0x0640 };
static int hle_mod_ok(Emu *e, const HleV2 *v)               /* the ROM routines are exactly the analysed ones */
{
    static Emu *ok_e; static uint32_t ok_fn;
    if (ok_e == e && ok_fn == v->modfn) return 1;
    for (int i = 0; i < 5; i++) if (f_rd16(e, v->modfn + 2u * (uint32_t)i) != hle_modfn_4up[i]) return 0;
    for (int i = 0; i < 21; i++) if (f_rd16(e, v->udiv + 2u * (uint32_t)i) != hle_udiv_4up[i]) return 0;
    ok_e = e; ok_fn = v->modfn;
    return 1;
}

/* Version 2 get-pixel. which = 0: gpA (layers with 0x4000), 1: gpB (layers without 0x4000, char maps).
 * Pixel (r6, r7). Returns 1 with result/cycles, or 0 if the guest must run it. */
static int hle2_pixel(Emu *e, const HleV2 *v, int which, uint32_t *res, uint32_t *cyc)
{
    int32_t x = (int16_t)e->r[6], y = (int16_t)e->r[7];
    int32_t i = (int16_t)(f_rd8(e, v->count) - 1);
    uint32_t c = which ? 10 : 14;                             /* entry */
    uint32_t alr_set = 0xFFFFFFFFu;                           /* ALR after the last multiply (mlt.h in engine 3, mlt.w in fetch) */
    int ahr_zero = 0;                                         /* fetch's mlt.w also cleared AHR */
    if (i < 0) { *res = 0; *cyc = c + 3; return 1; }
    c += 1;                                                   /* first pass enters one instruction early */
    for (; i >= 0; i--) {
        uint32_t L = v->list + (uint32_t)i * v->stride;
        c += v->per_layer;
        if (v->mlt) alr_set = (uint32_t)((int32_t)(int16_t)i * 20);   /* mlt.h r1,r4 (r4 = 0x14) */
        uint32_t P = f_rd32(e, L);
        if (P == 0) goto next;
        if (P - e->dev.io_base < e->dev.io_size) return 0;
        {
            uint32_t f = f_rd16(e, L + v->o_flags);
            c += 8;
            if (which ? (f & 0x4000) : !(f & 0x4000)) goto next;
            int32_t dy = (int16_t)(y - (int32_t)f_rd16(e, L + 6));
            c += 8; if (dy < 0) goto next;
            c += 5; if ((int32_t)f_rd8(e, L + 9) <= dy) goto next;
            int32_t dx = (int16_t)(x - (int32_t)f_rd16(e, L + 4));
            c += 8; if (dx < 0) goto next;
            c += 5; if ((int32_t)f_rd8(e, L + 8) <= dx) goto next;
            if (which) {
                c += 6;
                if (f & 0x1000) {                             /* character-map layer: byte straight from the map */
                    uint32_t a = (((uint32_t)x + (((uint32_t)y - 0x1c) << 7)) & 0xFFFF) + v->cmap;
                    *res = f_rd8(e, a);
                    *cyc = c + 9 + 2;
                    { if (alr_set != 0xFFFFFFFFu) e->alr = alr_set; if (ahr_zero) e->ahr = 0; }
                    return 1;
                }
            }
            if ((f & 0x2000) && !(which && v->modfn && hle_mod_ok(e, v))) return 0;   /* ROM routine not analysed */
            c += 5;
            uint32_t pix = hle2_fetch(e, v, L, P, (uint32_t)dx & 0xFF, (uint32_t)dy & 0xFF, f, &c, &alr_set) & 0xFF;
            ahr_zero = 1;
            if (f & 0x2000) {                                 /* gpB 0x62c-0x63a + ROM modfn + udiv: 13 cycles + the divide */
                uint32_t rem;
                c += 13 + udiv_cycles((~pix) & 0xFFu, f_rd8(e, P + 2), &rem);
                pix = rem & 0xFF;
            }
            c += (which ? 8 : 7) + 2;
            if (pix == 0) { c += 1; goto next; }
            *res = (pix + f_rd8(e, L + v->o_pal)) & 0xFF;
            *cyc = c + 5 + (which ? 2 : 3);
            { if (alr_set != 0xFFFFFFFFu) e->alr = alr_set; if (ahr_zero) e->ahr = 0; }
            return 1;
        }
    next:
        c += 5;
    }
    *res = 0; *cyc = c + (which ? 3 : 4);
    { if (alr_set != 0xFFFFFFFFu) e->alr = alr_set; if (ahr_zero) e->ahr = 0; }
    return 1;
}

/* Entry from the interpreter: is target T a known get-pixel routine of engine `kind`? If so try it.
 * Returns 1 if computed (res, cyc, and *ret = address of that routine's final ret for verify mode). */
static int hle_try(Emu *e, int kind, uint32_t t, uint32_t *res, uint32_t *cyc, uint32_t *ret, int *known)
{
    *known = 0;
    if (kind == 1 || kind == 5) {                              /* Plus Color / Hexagontchi */
        const HleV1 *v1 = HV1(kind);
        if (t != v1->pix) return 0;
        *known = 1; *ret = v1->ret;
        return hle_pixel(e, v1, res, cyc);
    }
    const HleV2 *v = HV2(kind);
    if (t != v->gpB && t != v->gpA) return 0;
    uint32_t h0; __asm__ volatile("rsr.ccount %0" : "=a"(h0));
    int ok;
    if (t == v->gpB) { *known = 1; *ret = v->gpB_ret; ok = hle2_pixel(e, v, 1, res, cyc); }
    else { *known = 1; *ret = v->gpA_ret; ok = hle2_pixel(e, v, 0, res, cyc); }
    uint32_t h1; __asm__ volatile("rsr.ccount %0" : "=a"(h1));
    tc_hle.host2 += h1 - h0;                                 /* measuring (2026-11-21): ESP cost of the v2 get-pixel */
    if (ok) tc_hle.guest2 += *cyc;
    return ok;
}

/* Verify mode: called at the routine's ret with the real result and the cycles it took. */
static void hle_check(uint32_t real_res, uint32_t real_cyc)
{
    tc_hle.pending = 0;
    if (real_res == tc_hle.expect) tc_hle.match++; else tc_hle.mismatch++;
    if (real_cyc == tc_hle.expect_cyc) tc_hle.cyc_match++;
    else {
        int32_t d = (int32_t)(real_cyc - tc_hle.expect_cyc);
        tc_hle.cyc_mismatch++; tc_hle.cyc_diff_sum += d;
        if (d >= -64 && d <= 64) {
            tc_hle.small_off++; tc_hle.small_sum += d;
            if (tc_hle.sample_cnt < 8) tc_hle.sample_diff[tc_hle.sample_cnt++] = d;
        } else tc_hle.big_off++;
    }
}

/* ---- Plus Color: the draw-window routine 0x3946 in native code (2026-11-18) ----
 * The routine fills a buffer at 0xcf4 with get-pixel results (phase 1, loop 0x3962/0x396a), then sends
 * every buffered pixel to the LCD as two bytes from one of four colour tables (phase 2, loop 0x39f8).
 * Interpreted, that loop code cost more than the pixels themselves. Here every instruction is one C
 * statement with the emulator's cycle rule (1 cycle per 16-bit word, ext prefixes included), and it can
 * stop after any instruction exactly where the interpreter would handle a peripheral event, so timers
 * and interrupts happen at the same guest cycle as before. Entered at the three loop heads (from a taken
 * jump in tc_turbo); leaves at 0x3998 / 0x3aa8 (once per call, interpreted), on an event, or before a
 * get-pixel call that has to run for real. Returns 1 if it ran. */
static inline uint32_t hd_rd8(Emu *e, uint32_t a)  { return f_rd8(e, a); }
static inline uint32_t hd_rd32(Emu *e, uint32_t a) { return f_rd32(e, a); }
int tc_hle_draw_on = 1;
int tc_hle_spot_on = 0;                 /* 1 = live self-checks: 1-in-64 get-pixel calls and every 16th draw-loop entry run
                                         * the original code (2026-11-22: off by default - every engine passed the Mac A/B
                                         * and verify tests; they cost ~5% of the interpreted work) */
uint32_t tc_hle_draw_runs;
int tc_hle_draw(Emu *e, int kind, uint32_t *pcp, uint32_t *spp, uint32_t *flp, int32_t *leftp)
{
    /* Written with Plus Color's addresses; Hexagontchi's routine is the same code D bytes lower, so pc is
     * converted on the way in and out, and the data addresses come from the table. */
    const HleV1 *v1 = HV1(kind);
    const uint32_t D = v1->D;
    uint32_t *R = e->r, sp = *spp, fl = *flp, pc = *pcp + D;
    int32_t left = *leftp;
    if (left <= 0) return 0;
    tc_hle_draw_runs++;
/* instruction at A, next at N: charge its words, stop if an event is due */
#define HI(A, N)  do { left -= (int32_t)(((N) - (A)) >> 1); if (left <= 0) { pc = (N); goto out; } } while (0)
/* delayed branch at A (taken -> T, else A+4): branch word, delay slot statement S, then the event check */
#define HD(A, C, T, S) do { int _t = cond_ok((C), fl); left -= 1; S; left -= 1; pc = _t ? (T) : (A) + 4; \
                            if (left <= 0) { goto out; } \
                            if (_t) { goto *hd_lbl(T); } } while (0)
#define ADDF(a, b) op_add((a), (b), &fl)
#define SUBF(a, b) op_sub((a), (b), &fl)
#define CMPF(a, b) ((void)op_sub((a), (b), &fl))
#define NZF(q) do { uint32_t _q = (q); fl = (fl & 3u) | ((_q >> 31) << 3) | ((uint32_t)(_q == 0) << 2); } while (0)
    static void *const lbl[] = { &&L3962, &&L396a, &&L398e, &&L39f8, &&L3a74, &&L3a38, &&L3a9c };
#define hd_lbl(T) ((T) == 0x3962u ? lbl[0] : (T) == 0x396au ? lbl[1] : (T) == 0x398eu ? lbl[2] : (T) == 0x39f8u ? lbl[3] : \
                   (T) == 0x3a74u ? lbl[4] : (T) == 0x3a38u ? lbl[5] : lbl[6])
    if (pc == 0x3962u) goto L3962;
    if (pc == 0x396au) goto L396a;
    if (pc == 0x39f8u) goto L39f8;
    return 0;

    /* ---- phase 1: rows (r3 = y, up to [sp+1]), pixels (r0 = x, up to r2) ---- */
L3962:
    R[0] = hd_rd32(e, sp + 8);                   HI(0x3962, 0x3964);   /* ld.w r0,[sp+2] */
    CMPF(R[0], R[2]);                            HI(0x3964, 0x3966);   /* cmp r0,r2 */
    HD(0x3966, 8, 0x398eu, R[4] = R[3]);                               /* jrugt.d 0x398e ; ld.w r4,r3 */
L396a:
    R[1] = hd_rd32(e, sp + 12);                  HI(0x396a, 0x396c);   /* ld.w r1,[sp+3] */
    R[4] = R[1];                                 HI(0x396c, 0x396e);   /* ld.w r4,r1 */
    R[4] = ADDF(R[4], 1);                        HI(0x396e, 0x3970);   /* add r4,1 */
    R[4] &= 0xFFFFu;                             HI(0x3970, 0x3972);   /* ld.uh r4,r4 */
    f_wr32(e, sp + 12, R[4]);                    HI(0x3972, 0x3974);   /* ld.w [sp+3],r4 */
    R[5] = v1->buf;                              HI(0x3974, 0x3978);   /* xld.w r5,0xcf4 */
    R[1] = ADDF(R[1], R[5]);                     HI(0x3978, 0x397a);   /* add r1,r5 */
    R[6] = R[0];                                 HI(0x397a, 0x397c);   /* ld.w r6,r0 */
    {   /* call.d 0x389e ; ld.w r7,r3  - the get-pixel call, computed natively */
        uint32_t old7 = R[7], res, cyc;
        R[7] = R[3];
        if (!hle_pixel(e, v1, &res, &cyc)) { R[7] = old7; pc = 0x397c; goto out; }   /* must run for real */
        left -= 2;                                /* call.d + delay slot */
        tc_hle.calls++; tc_hle.done++;
        R[4] = res; left -= (int32_t)cyc;
        pc = 0x3980; if (left <= 0) goto out;
    }
    f_wr8(e, R[1], R[4]);                        HI(0x3980, 0x3982);   /* ld.b [r1],r4 */
    R[4] = R[0];                                 HI(0x3982, 0x3984);   /* ld.w r4,r0 */
    R[4] = ADDF(R[4], 1);                        HI(0x3984, 0x3986);   /* add r4,1 */
    R[0] = R[4] & 0xFFu;                         HI(0x3986, 0x3988);   /* ld.ub r0,r4 */
    CMPF(R[0], R[2]);                            HI(0x3988, 0x398a);   /* cmp r0,r2 */
    HD(0x398a, 11, 0x396au, R[4] = R[3]);                              /* jrule.d 0x396a ; ld.w r4,r3 */
L398e:
    R[4] = ADDF(R[4], 1);                        HI(0x398e, 0x3990);   /* add r4,1 */
    R[3] = R[4] & 0xFFu;                         HI(0x3990, 0x3992);   /* ld.ub r3,r4 */
    R[4] = hd_rd32(e, sp + 4);                   HI(0x3992, 0x3994);   /* ld.w r4,[sp+1] */
    CMPF(R[3], R[4]);                            HI(0x3994, 0x3996);   /* cmp r3,r4 */
    {   int t = cond_ok(11, fl);                                       /* jrule 0x3962 */
        left -= 1; pc = t ? 0x3962u : 0x3998u;
        if (left <= 0) goto out;
        if (t) goto L3962;
        goto out; }                              /* 0x3998: phase 2 set-up, interpreted */

    /* ---- phase 2: buffered pixel [sp+3] of r12, LCD data port r8, row r3, column r7 of r13 ---- */
L39f8:
    R[4] = hd_rd32(e, sp + 12);                  HI(0x39f8, 0x39fa);   /* ld.w r4,[sp+3] */
    R[5] = v1->buf;                              HI(0x39fa, 0x39fe);   /* xld.w r5,0xcf4 */
    R[4] = ADDF(R[4], R[5]);                     HI(0x39fe, 0x3a00);   /* add r4,r5 */
    R[6] = hd_rd8(e, R[4]);                      HI(0x3a00, 0x3a02);   /* ld.ub r6,[r4] */
    CMPF(R[11], R[6]);                           HI(0x3a02, 0x3a04);   /* cmp r11,r6 */
    HD(0x3a04, 8, 0x3a74u, R[4] = R[6]);                               /* jrugt.d 0x3a74 ; ld.w r4,r6 */
    R[4] = v1->ghi;                              HI(0x3a08, 0x3a0e);   /* xld.w r4,0x841d2 (3 words) */
    R[4] = hd_rd8(e, R[4]);                      HI(0x3a0e, 0x3a10);   /* ld.ub r4,[r4] */
    CMPF(R[4], R[6]);                            HI(0x3a10, 0x3a12);   /* cmp r4,r6 */
    HD(0x3a12, 11, 0x3a74u, R[4] = R[6]);                              /* jrule.d 0x3a74 ; ld.w r4,r6 */
    R[4] = R[3];                                 HI(0x3a16, 0x3a18);   /* ld.w r4,r3 */
    R[4] = SUBF(R[4], 0x1c);                     HI(0x3a18, 0x3a1a);   /* sub r4,0x1c */
    R[4] &= 0xFFu;                               HI(0x3a1a, 0x3a1c);   /* ld.ub r4,r4 */
    CMPF(R[4], R[0]);                            HI(0x3a1c, 0x3a1e);   /* cmp r4,r0 */
    HD(0x3a1e, 8, 0x3a74u, R[4] = R[6]);                               /* jrugt.d 0x3a74 ; ld.w r4,r6 */
    R[4] = hd_rd8(e, R[14]);                     HI(0x3a22, 0x3a24);   /* ld.ub r4,[r14] */
    R[9] = R[6];                                 HI(0x3a24, 0x3a26);   /* ld.w r9,r6 */
    R[9] = SUBF(R[9], R[4]);                     HI(0x3a26, 0x3a28);   /* sub r9,r4 */
    R[4] = 0x5b;                                 HI(0x3a28, 0x3a2c);   /* xld.w r4,0x5b */
    R[4] = SUBF(R[4], R[3]);                     HI(0x3a2c, 0x3a2e);   /* sub r4,r3 */
    CMPF(R[4], 0);                               HI(0x3a2e, 0x3a30);   /* cmp r4,0x0 */
    HD(0x3a30, 5, 0x3a38u, R[5] = R[4]);                               /* jrge.d 0x3a38 ; ld.w r5,r4 */
    R[4] = ADDF(R[4], 3);                        HI(0x3a34, 0x3a36);   /* add r4,0x3 */
    R[5] = R[4];                                 HI(0x3a36, 0x3a38);   /* ld.w r5,r4 */
L3a38:
    R[5] = (uint32_t)((int32_t)R[5] >> 2); NZF(R[5]); HI(0x3a38, 0x3a3a);   /* sra r5,0x2 */
    R[4] = R[5];                                 HI(0x3a3a, 0x3a3c);   /* ld.w r4,r5 */
    R[4] <<= 3; NZF(R[4]);                       HI(0x3a3c, 0x3a3e);   /* sll r4,0x3 */
    R[4] = ADDF(R[4], R[5]);                     HI(0x3a3e, 0x3a40);   /* add r4,r5 */
    R[4] <<= 2; NZF(R[4]);                       HI(0x3a40, 0x3a42);   /* sll r4,0x2 */
    R[5] = v1->gtab_a;                           HI(0x3a42, 0x3a46);   /* xld.w r5,0x8b4 */
    R[4] = ADDF(R[4], R[5]);                     HI(0x3a46, 0x3a48);   /* add r4,r5 */
    R[4] = ADDF(R[4], R[9]);                     HI(0x3a48, 0x3a4a);   /* add r4,r9 */
    R[4] = hd_rd8(e, R[4]);                      HI(0x3a4a, 0x3a4c);   /* ld.ub r4,[r4] */
    f_wr8(e, R[8], R[4]);                        HI(0x3a4c, 0x3a4e);   /* ld.b [r8],r4  (LCD) */
    R[4] = hd_rd8(e, R[14]);                     HI(0x3a4e, 0x3a50);   /* ld.ub r4,[r14] */
    R[6] = SUBF(R[6], R[4]);                     HI(0x3a50, 0x3a52);   /* sub r6,r4 */
    R[4] = 0x5b;                                 HI(0x3a52, 0x3a56);   /* xld.w r4,0x5b */
    R[4] = SUBF(R[4], R[3]);                     HI(0x3a56, 0x3a58);   /* sub r4,r3 */
    CMPF(R[4], 0);                               HI(0x3a58, 0x3a5a);   /* cmp r4,0x0 */
    {   int t = cond_ok(5, fl);                                        /* jrge 0x3a5e */
        left -= 1; pc = t ? 0x3a5eu : 0x3a5cu;
        if (left <= 0) goto out;
        if (!t) { R[4] = ADDF(R[4], 3);          HI(0x3a5c, 0x3a5e); } /* add r4,0x3 */
    }
    R[4] = (uint32_t)((int32_t)R[4] >> 2); NZF(R[4]); HI(0x3a5e, 0x3a60);   /* sra r4,0x2 */
    R[5] = R[4];                                 HI(0x3a60, 0x3a62);   /* ld.w r5,r4 */
    R[5] <<= 3; NZF(R[5]);                       HI(0x3a62, 0x3a64);   /* sll r5,0x3 */
    R[5] = ADDF(R[5], R[4]);                     HI(0x3a64, 0x3a66);   /* add r5,r4 */
    R[5] <<= 2; NZF(R[5]);                       HI(0x3a66, 0x3a68);   /* sll r5,0x2 */
    R[4] = v1->gtab_b;                           HI(0x3a68, 0x3a6c);   /* xld.w r4,0x674 */
    R[5] = ADDF(R[5], R[4]);                     HI(0x3a6c, 0x3a6e);   /* add r5,r4 */
    R[5] = ADDF(R[5], R[6]);                     HI(0x3a6e, 0x3a70);   /* add r5,r6 */
    R[4] = hd_rd8(e, R[5]);                      HI(0x3a70, 0x3a72);   /* ld.ub r4,[r5] */
    left -= 1; pc = 0x3a88; if (left <= 0) goto out;                   /* jp 0x3a88 */
    goto L3a88;
L3a74:
    R[5] = v1->tab_a;                            HI(0x3a74, 0x3a78);   /* xld.w r5,0xbf4 */
    R[4] = ADDF(R[4], R[5]);                     HI(0x3a78, 0x3a7a);   /* add r4,r5 */
    R[4] = hd_rd8(e, R[4]);                      HI(0x3a7a, 0x3a7c);   /* ld.ub r4,[r4] */
    f_wr8(e, R[8], R[4]);                        HI(0x3a7c, 0x3a7e);   /* ld.b [r8],r4  (LCD) */
    R[4] = R[6];                                 HI(0x3a7e, 0x3a80);   /* ld.w r4,r6 */
    R[5] = v1->tab_b;                            HI(0x3a80, 0x3a84);   /* xld.w r5,0xaf4 */
    R[4] = ADDF(R[4], R[5]);                     HI(0x3a84, 0x3a86);   /* add r4,r5 */
    R[4] = hd_rd8(e, R[4]);                      HI(0x3a86, 0x3a88);   /* ld.ub r4,[r4] */
L3a88:
    f_wr8(e, R[8], R[4]);                        HI(0x3a88, 0x3a8a);   /* ld.b [r8],r4  (LCD) */
    R[4] = R[7];                                 HI(0x3a8a, 0x3a8c);   /* ld.w r4,r7 */
    R[4] = ADDF(R[4], 1);                        HI(0x3a8c, 0x3a8e);   /* add r4,0x1 */
    R[7] = R[4] & 0xFFFFu;                       HI(0x3a8e, 0x3a90);   /* ld.uh r7,r4 */
    CMPF(R[13], R[7]);                           HI(0x3a90, 0x3a92);   /* cmp r13,r7 */
    HD(0x3a92, 8, 0x3a9cu, R[4] = R[3]);                               /* jrugt.d 0x3a9c ; ld.w r4,r3 */
    R[7] = 0;                                    HI(0x3a96, 0x3a98);   /* ld.w r7,0x0 */
    R[4] = ADDF(R[4], 1);                        HI(0x3a98, 0x3a9a);   /* add r4,0x1 */
    R[3] = R[4] & 0xFFu;                         HI(0x3a9a, 0x3a9c);   /* ld.ub r3,r4 */
L3a9c:
    R[4] = hd_rd32(e, sp + 12);                  HI(0x3a9c, 0x3a9e);   /* ld.w r4,[sp+3] */
    R[4] = ADDF(R[4], 1);                        HI(0x3a9e, 0x3aa0);   /* add r4,0x1 */
    R[4] &= 0xFFFFu;                             HI(0x3aa0, 0x3aa2);   /* ld.uh r4,r4 */
    f_wr32(e, sp + 12, R[4]);                    HI(0x3aa2, 0x3aa4);   /* ld.w [sp+3],r4 */
    CMPF(R[4], R[12]);                           HI(0x3aa4, 0x3aa6);   /* cmp r4,r12 */
    {   int t = cond_ok(10, fl);                                       /* jrult 0x39f8 */
        left -= 1; pc = t ? 0x39f8u : 0x3aa8u;
        if (left <= 0) goto out;
        if (t) goto L39f8;
    }
out:
    *pcp = pc - D; *spp = sp; *flp = fl; *leftp = left;      /* back to this game's addresses */
    return 1;
#undef HI
#undef HD
#undef ADDF
#undef SUBF
#undef CMPF
#undef NZF
#undef hd_lbl
}

/* Engine v2 draw loops (iD L addresses; P's = +0x19a): fill A 0x5ec-0x60e, fill B 0x6ec-0x70e,
 * send 0x686-0x6aa, gradient send 0x7b8-0x864. Expected code words per game (from the 2026-11-21 dumps). */
static const uint16_t hd2_words_idl[142] = {
    0x2e24, 0x6014, 0xad42, 0xc047, 0x6e84, 0x2241, 0x2e06, 0xdfff, 0x1d1e, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40,
    0x17f0, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc047, 0x6e85, 0x2251, 0x2e06, 0xdffe, 0x1dfc, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540,
    0x5024, 0x2a40, 0x17f0, 0x2e21, 0x22d4, 0x2445, 0x2e54, 0x22c4, 0x2444, 0x3474, 0x2e54, 0x22b4, 0x2444, 0x3474, 0x2295, 0x2454,
    0x3474, 0x2e64, 0x6014, 0xad46, 0x2a86, 0x15ef, 0x2e64, 0x2ec4, 0x2ec5, 0x6015, 0xad5c, 0xc047, 0x6e85, 0x2254, 0x2449, 0x2a9a,
    0x113d, 0x2e94, 0xc001, 0xc10c, 0x6e74, 0x2444, 0x2a94, 0x1736, 0x2e94, 0x5034, 0xc000, 0x6bf4, 0x1131, 0x2e94, 0xc001, 0x6db5,
    0x2615, 0xad55, 0xc001, 0x6d87, 0xa675, 0xa427, 0x22e4, 0x2444, 0xc001, 0x6c08, 0x2658, 0xaa84, 0xa424, 0x2247, 0x9067, 0xc001,
    0x6d16, 0xa665, 0xa426, 0x2e94, 0x2234, 0x2444, 0xaa84, 0xa424, 0x2246, 0x9066, 0xc003, 0x6ff4, 0xa645, 0xa425, 0x2e94, 0x2224,
    0x2444, 0xaa84, 0xa424, 0x2245, 0x9065, 0xa577, 0x34d7, 0xa566, 0x34d6, 0xa555, 0x34d5, 0x1f0e, 0x2eb4, 0x22e4, 0x2444, 0x34d4,
    0x2e94, 0x2234, 0x2444, 0x34d4, 0x2e94, 0x2224, 0x2444, 0x34d4, 0x2eb4, 0x6014, 0xa54b, 0x2a0b, 0x17ac, 0x2ec4,
};
static const uint16_t hd2_words_ps[142] = {
    0x2e24, 0x6014, 0xad42, 0xc059, 0x6fc4, 0x2241, 0x2e06, 0xdfff, 0x1d1e, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40,
    0x17f0, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc059, 0x6fc5, 0x2251, 0x2e06, 0xdffe, 0x1dfc, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540,
    0x5024, 0x2a40, 0x17f0, 0x2e21, 0x22d4, 0x2445, 0x2e54, 0x22c4, 0x2444, 0x3474, 0x2e54, 0x22b4, 0x2444, 0x3474, 0x2295, 0x2454,
    0x3474, 0x2e64, 0x6014, 0xad46, 0x2a86, 0x15ef, 0x2e64, 0x2ec4, 0x2ec5, 0x6015, 0xad5c, 0xc059, 0x6fc5, 0x2254, 0x2449, 0x2a9a,
    0x113d, 0x2e94, 0xc001, 0xc110, 0x6cf4, 0x2444, 0x2a94, 0x1736, 0x2e94, 0x5034, 0xc000, 0x6bf4, 0x1131, 0x2e94, 0xc001, 0x6db5,
    0x2615, 0xad55, 0xc001, 0x6d87, 0xa675, 0xa427, 0x22e4, 0x2444, 0xc001, 0x6c08, 0x2658, 0xaa84, 0xa424, 0x2247, 0x9067, 0xc001,
    0x6d16, 0xa665, 0xa426, 0x2e94, 0x2234, 0x2444, 0xaa84, 0xa424, 0x2246, 0x9066, 0xc003, 0x6ff4, 0xa645, 0xa425, 0x2e94, 0x2224,
    0x2444, 0xaa84, 0xa424, 0x2245, 0x9065, 0xa577, 0x34d7, 0xa566, 0x34d6, 0xa555, 0x34d5, 0x1f0e, 0x2eb4, 0x22e4, 0x2444, 0x34d4,
    0x2e94, 0x2234, 0x2444, 0x34d4, 0x2e94, 0x2224, 0x2444, 0x34d4, 0x2eb4, 0x6014, 0xa54b, 0x2a0b, 0x17ac, 0x2ec4,
};

/* 4U+ (engine 3, 2026-11-22): fill A 0x680 and fill B 0x7dc are iD L's fill loops (buffer 0x1810); the send loop
 * 0x750 is new (picks one of two palettes per window: [0xdf6] == 1 -> tables r13/r12/r11, else +0x100); the
 * gradient loop 0x8e4-0x98c is iD L's with other registers (range low r1, y r14, R table r10, row [sp+4], range
 * high [0x8425f]) and a plain (not delayed) jrugt for the row test. Code words from the 2026-11-22 dump. */
static const uint16_t hd2_words_4up[159] = {
    /* fill A 0x680 (18) */
    0x2e24, 0x6014, 0xad42, 0xc060, 0x6d04, 0x2241, 0x2e06, 0xdfff, 0x1d1c, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40, 0x17f0, 0x2e21,
    /* fill B 0x7dc (18) */
    0x2e24, 0x6014, 0xad42, 0xc060, 0x6d05, 0x2251, 0x2e06, 0xdffe, 0x1dcd, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40, 0x17f0, 0x2e21,
    /* send 0x750 (38) */
    0xc060, 0x6d05, 0x2254, 0x2445, 0x6819, 0x1b0d, 0x2e54, 0x22d4, 0x2444, 0x3474, 0x2e54, 0x22c4, 0x2444, 0x3474, 0x2e54, 0x22b4, 0x2444, 0x1e0e, 0x22d4, 0xc100,
    0x2444, 0x3474, 0x2e54, 0x22c4, 0xc100, 0x2444, 0x3474, 0x2e54, 0x22b4, 0xc100, 0x2444, 0x3474, 0x2e64, 0x6014, 0xad46, 0x2a86, 0x15dc, 0x2e64,
    /* gradient 0x8e4 (85) */
    0x2ec5, 0x6015, 0xad5c, 0xc060, 0x6d05, 0x2254, 0x2449, 0x2a91, 0x113c, 0x2e94, 0xc001, 0xc109, 0x6df4, 0x2444, 0x2a94, 0x1735, 0x2e94, 0x5047, 0xc000, 0x6bf7,
    0x1030, 0xc001, 0x6db5, 0x26e5, 0xad55, 0xc001, 0x6d87, 0xa675, 0xa427, 0x22a4, 0x2444, 0xc001, 0x6c08, 0x2658, 0xaa84, 0xa424, 0x2247, 0x9067, 0xc001, 0x6d16,
    0xa665, 0xa426, 0x2e94, 0x2234, 0x2444, 0xaa84, 0xa424, 0x2246, 0x9066, 0xc003, 0x6ff4, 0xa645, 0xa425, 0x2e94, 0x2224, 0x2444, 0xaa84, 0xa424, 0x2245, 0x9065,
    0xa577, 0x34d7, 0xa566, 0x34d6, 0xa555, 0x34d5, 0x1f0e, 0x2eb4, 0x22a4, 0x2444, 0x34d4, 0x2e94, 0x2234, 0x2444, 0x34d4, 0x2e94, 0x2224, 0x2444, 0x34d4, 0x2eb4,
    0x6014, 0xa54b, 0x2a0b, 0x17ad, 0x2ec4,
};

/* 4U+ row loops (2026-11-22): the code around the pixel loops - next row, row bounds - also native, so a
 * window is drawn without returning to the interpreter between rows (it was 28% of the interpreted work). */
static const uint16_t hd2_4up_outerA[30] = {  /* 0x674-0x6ae (row loop + its inner pixel loop) */
    0x5010, 0x5024, 0x2a40, 0x1116, 0x2e34, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc060, 0x6d04, 0x2241, 0x2e06, 0xdfff, 0x1d1c, 0x2e37,
    0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40, 0x17f0, 0x2e21, 0x2e34, 0x6014, 0xa543, 0x5004, 0x2a43, 0x16e3,
};
static const uint16_t hd2_4up_outerB[34] = {  /* 0x7d0-0x812 (row loop + its inner pixel loop) */
    0x5010, 0x5024, 0x2a40, 0x1116, 0x2e24, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc060, 0x6d05, 0x2251, 0x2e06, 0xdffe, 0x1dcd, 0x2e37,
    0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40, 0x17f0, 0x2e21, 0x2e24, 0x5035, 0x2254, 0xad42, 0x2e34, 0x6014, 0xa543, 0x5004,
    0x2a43, 0x16df,
};
static const uint16_t hd2_4up_outerG[112] = {  /* 0x8c0-0x99e (row loop + its inner pixel loop) */
    0x502b, 0x2a0b, 0x1165, 0x2ec4, 0xc001, 0xc109, 0x6de4, 0x2441, 0x2ee4, 0x65c4, 0xa544, 0x5c44, 0xc0f0, 0x6d0a, 0xc00c, 0xc000,
    0x6c1d, 0x2ec4, 0x2ec5, 0x6015, 0xad5c, 0xc060, 0x6d05, 0x2254, 0x2449, 0x2a91, 0x113c, 0x2e94, 0xc001, 0xc109, 0x6df4, 0x2444,
    0x2a94, 0x1735, 0x2e94, 0x5047, 0xc000, 0x6bf7, 0x1030, 0xc001, 0x6db5, 0x26e5, 0xad55, 0xc001, 0x6d87, 0xa675, 0xa427, 0x22a4,
    0x2444, 0xc001, 0x6c08, 0x2658, 0xaa84, 0xa424, 0x2247, 0x9067, 0xc001, 0x6d16, 0xa665, 0xa426, 0x2e94, 0x2234, 0x2444, 0xaa84,
    0xa424, 0x2246, 0x9066, 0xc003, 0x6ff4, 0xa645, 0xa425, 0x2e94, 0x2224, 0x2444, 0xaa84, 0xa424, 0x2245, 0x9065, 0xa577, 0x34d7,
    0xa566, 0x34d6, 0xa555, 0x34d5, 0x1f0e, 0x2eb4, 0x22a4, 0x2444, 0x34d4, 0x2e94, 0x2234, 0x2444, 0x34d4, 0x2e94, 0x2224, 0x2444,
    0x34d4, 0x2eb4, 0x6014, 0xa54b, 0x2a0b, 0x17ad, 0x2ec4, 0x5038, 0x2284, 0xad4c, 0x2ee4, 0x6014, 0xa54e, 0x5014, 0x2a4e, 0x1691,
};

/* ---- iD L / P's: the draw-window loops in native code (2026-11-21) ----
 * Engine v2 redraws an area in two steps: fill loops (A: get-pixel over 0x4000 layers, B: without) write
 * the colour index of every pixel into a buffer, then a send loop writes 3 bytes per pixel to the LCD from
 * three colour tables (simple loop 0x686, or 0x7ba with an optional brightness gradient for one colour range
 * in the upper rows, using mltu.h/mlt.w). Interpreted, every pixel cost ~3 LCD writes through the slow path
 * and ~5 delayed branches/calls: ~110 ESP cycles per guest cycle, so page changes ran at ~75% speed.
 * Same rules as tc_hle_draw: one C statement per instruction, 1 cycle per 16-bit word (ext words included),
 * stop after any instruction when an event is due (pc = next instruction), a delayed branch and its slot
 * as one step. The code words of each loop are checked on every entry (the game rewrites parts of this RAM
 * code), so any other code is simply interpreted. P's runs the same code 0x19a bytes later. */
static int hd2_check(Emu *e, const uint16_t *want, uint32_t a, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, a += 2)
        if ((uint32_t)(e->a0ram[a] | (e->a0ram[a + 1] << 8)) != want[i]) return 0;
    return 1;
}
uint32_t tc_hle_draw2_runs;
static const uint16_t hd2_words_idl15[142] = {  /* fill A 0x5ec+0x90, fill B 0x6ec+0x90, send 0x686+0x90, gradient 0x7b8+0x90 */
    0x2e24, 0x6014, 0xad42, 0xc04c, 0x6c04, 0x2241, 0x2e06, 0xdfff, 0x1d1e, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40,
    0x17f0, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc04c, 0x6c05, 0x2251, 0x2e06, 0xdffe, 0x1dfc, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540,
    0x5024, 0x2a40, 0x17f0, 0x2e21, 0x22d4, 0x2445, 0x2e54, 0x22c4, 0x2444, 0x3474, 0x2e54, 0x22b4, 0x2444, 0x3474, 0x2295, 0x2454,
    0x3474, 0x2e64, 0x6014, 0xad46, 0x2a86, 0x15ef, 0x2e64, 0x2ec4, 0x2ec5, 0x6015, 0xad5c, 0xc04c, 0x6c05, 0x2254, 0x2449, 0x2a9a,
    0x113d, 0x2e94, 0xc001, 0xc10b, 0x6eb4, 0x2444, 0x2a94, 0x1736, 0x2e94, 0x5034, 0xc000, 0x6bf4, 0x1131, 0x2e94, 0xc001, 0x6db5,
    0x2615, 0xad55, 0xc001, 0x6d87, 0xa675, 0xa427, 0x22e4, 0x2444, 0xc001, 0x6c08, 0x2658, 0xaa84, 0xa424, 0x2247, 0x9067, 0xc001,
    0x6d16, 0xa665, 0xa426, 0x2e94, 0x2234, 0x2444, 0xaa84, 0xa424, 0x2246, 0x9066, 0xc003, 0x6ff4, 0xa645, 0xa425, 0x2e94, 0x2224,
    0x2444, 0xaa84, 0xa424, 0x2245, 0x9065, 0xa577, 0x34d7, 0xa566, 0x34d6, 0xa555, 0x34d5, 0x1f0e, 0x2eb4, 0x22e4, 0x2444, 0x34d4,
    0x2e94, 0x2234, 0x2444, 0x34d4, 0x2e94, 0x2224, 0x2444, 0x34d4, 0x2eb4, 0x6014, 0xa54b, 0x2a0b, 0x17ac, 0x2ec4,
};
static const uint16_t hd2_words_id[142] = {  /* fill A 0x5ec+0x36, fill B 0x6ec+0x3a, send 0x686+0x3a, gradient 0x7b8+0x3c */
    0x2e24, 0x6014, 0xad42, 0xc044, 0x6cc4, 0x2241, 0x2e06, 0xdfff, 0x1d1e, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40,
    0x17f0, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc044, 0x6cc5, 0x2251, 0x2e06, 0xdffe, 0x1dfa, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540,
    0x5024, 0x2a40, 0x17f0, 0x2e21, 0x22d4, 0x2445, 0x2e54, 0x22c4, 0x2444, 0x3474, 0x2e54, 0x22b4, 0x2444, 0x3474, 0x2295, 0x2454,
    0x3474, 0x2e64, 0x6014, 0xad46, 0x2a86, 0x15ef, 0x2e64, 0x2ec4, 0x2ec5, 0x6015, 0xad5c, 0xc044, 0x6cc5, 0x2254, 0x2449, 0x2a9a,
    0x113d, 0x2e94, 0xc001, 0xc10a, 0x6c74, 0x2444, 0x2a94, 0x1736, 0x2e94, 0x5034, 0xc000, 0x6bf4, 0x1131, 0x2e94, 0xc001, 0x6db5,
    0x2615, 0xad55, 0xc001, 0x6d87, 0xa675, 0xa427, 0x22e4, 0x2444, 0xc001, 0x6c08, 0x2658, 0xaa84, 0xa424, 0x2247, 0x9067, 0xc001,
    0x6d16, 0xa665, 0xa426, 0x2e94, 0x2234, 0x2444, 0xaa84, 0xa424, 0x2246, 0x9066, 0xc003, 0x6ff4, 0xa645, 0xa425, 0x2e94, 0x2224,
    0x2444, 0xaa84, 0xa424, 0x2245, 0x9065, 0xa577, 0x34d7, 0xa566, 0x34d6, 0xa555, 0x34d5, 0x1f0e, 0x2eb4, 0x22e4, 0x2444, 0x34d4,
    0x2e94, 0x2234, 0x2444, 0x34d4, 0x2e94, 0x2224, 0x2444, 0x34d4, 0x2eb4, 0x6014, 0xa54b, 0x2a0b, 0x17ac, 0x2ec4,
};
static const uint16_t hd2_words_idm[142] = {  /* fill A 0x5ec+0x36, fill B 0x6ec+0x36, send 0x686+0x36, gradient 0x7b8+0x36 */
    0x2e24, 0x6014, 0xad42, 0xc04f, 0x6f04, 0x2241, 0x2e06, 0xdfff, 0x1d1e, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40,
    0x17f0, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc04f, 0x6f05, 0x2251, 0x2e06, 0xdffe, 0x1dfc, 0x2e37, 0x3414, 0x2e04, 0x6014, 0xa540,
    0x5024, 0x2a40, 0x17f0, 0x2e21, 0x22d4, 0x2445, 0x2e54, 0x22c4, 0x2444, 0x3474, 0x2e54, 0x22b4, 0x2444, 0x3474, 0x2295, 0x2454,
    0x3474, 0x2e64, 0x6014, 0xad46, 0x2a86, 0x15ef, 0x2e64, 0x2ec4, 0x2ec5, 0x6015, 0xad5c, 0xc04f, 0x6f05, 0x2254, 0x2449, 0x2a9a,
    0x113d, 0x2e94, 0xc001, 0xc10a, 0x6c74, 0x2444, 0x2a94, 0x1736, 0x2e94, 0x5034, 0xc000, 0x6bf4, 0x1131, 0x2e94, 0xc001, 0x6db5,
    0x2615, 0xad55, 0xc001, 0x6d87, 0xa675, 0xa427, 0x22e4, 0x2444, 0xc001, 0x6c08, 0x2658, 0xaa84, 0xa424, 0x2247, 0x9067, 0xc001,
    0x6d16, 0xa665, 0xa426, 0x2e94, 0x2234, 0x2444, 0xaa84, 0xa424, 0x2246, 0x9066, 0xc003, 0x6ff4, 0xa645, 0xa425, 0x2e94, 0x2224,
    0x2444, 0xaa84, 0xa424, 0x2245, 0x9065, 0xa577, 0x34d7, 0xa566, 0x34d6, 0xa555, 0x34d5, 0x1f0e, 0x2eb4, 0x22e4, 0x2444, 0x34d4,
    0x2e94, 0x2234, 0x2444, 0x34d4, 0x2e94, 0x2224, 0x2444, 0x34d4, 0x2eb4, 0x6014, 0xa54b, 0x2a0b, 0x17ac, 0x2ec4,
};
/* 4U (kind 7, 2026-11-23): the 4U+ loops 0x10a bytes later; buffer 0x1864, gradient range 0x84272/0x84273, R table 0x3c64 */
static const uint16_t hd2_4u_outerA[30] = {  /* 0x77e (4U+ 0x674) */
    0x5010, 0x5024, 0x2a40, 0x1116, 0x2e34, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc061, 0x6e44, 0x2241, 0x2e06, 0xdfff, 0x1d1c, 0x2e37,
    0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40, 0x17f0, 0x2e21, 0x2e34, 0x6014, 0xa543, 0x5004, 0x2a43, 0x16e3,
};
static const uint16_t hd2_4u_outerB[34] = {  /* 0x8da (4U+ 0x7d0) */
    0x5010, 0x5024, 0x2a40, 0x1116, 0x2e24, 0x2e21, 0x2e24, 0x6014, 0xad42, 0xc061, 0x6e45, 0x2251, 0x2e06, 0xdffe, 0x1dcd, 0x2e37,
    0x3414, 0x2e04, 0x6014, 0xa540, 0x5024, 0x2a40, 0x17f0, 0x2e21, 0x2e24, 0x5035, 0x2254, 0xad42, 0x2e34, 0x6014, 0xa543, 0x5004,
    0x2a43, 0x16df,
};
static const uint16_t hd2_4u_send[38] = {  /* 0x85a (4U+ 0x750) */
    0xc061, 0x6e45, 0x2254, 0x2445, 0x6819, 0x1b0d, 0x2e54, 0x22d4, 0x2444, 0x3474, 0x2e54, 0x22c4, 0x2444, 0x3474, 0x2e54, 0x22b4,
    0x2444, 0x1e0e, 0x22d4, 0xc100, 0x2444, 0x3474, 0x2e54, 0x22c4, 0xc100, 0x2444, 0x3474, 0x2e54, 0x22b4, 0xc100, 0x2444, 0x3474,
    0x2e64, 0x6014, 0xad46, 0x2a86, 0x15dc, 0x2e64,
};
static const uint16_t hd2_4u_outerG[112] = {  /* 0x9ca (4U+ 0x8c0) */
    0x502b, 0x2a0b, 0x1165, 0x2ec4, 0xc001, 0xc109, 0x6f24, 0x2441, 0x2ee4, 0x65c4, 0xa544, 0x5c44, 0xc0f1, 0x6e4a, 0xc00c, 0xc000,
    0x6c1d, 0x2ec4, 0x2ec5, 0x6015, 0xad5c, 0xc061, 0x6e45, 0x2254, 0x2449, 0x2a91, 0x113c, 0x2e94, 0xc001, 0xc109, 0x6f34, 0x2444,
    0x2a94, 0x1735, 0x2e94, 0x5047, 0xc000, 0x6bf7, 0x1030, 0xc001, 0x6db5, 0x26e5, 0xad55, 0xc001, 0x6d87, 0xa675, 0xa427, 0x22a4,
    0x2444, 0xc001, 0x6c08, 0x2658, 0xaa84, 0xa424, 0x2247, 0x9067, 0xc001, 0x6d16, 0xa665, 0xa426, 0x2e94, 0x2234, 0x2444, 0xaa84,
    0xa424, 0x2246, 0x9066, 0xc003, 0x6ff4, 0xa645, 0xa425, 0x2e94, 0x2224, 0x2444, 0xaa84, 0xa424, 0x2245, 0x9065, 0xa577, 0x34d7,
    0xa566, 0x34d6, 0xa555, 0x34d5, 0x1f0e, 0x2eb4, 0x22a4, 0x2444, 0x34d4, 0x2e94, 0x2234, 0x2444, 0x34d4, 0x2e94, 0x2224, 0x2444,
    0x34d4, 0x2eb4, 0x6014, 0xa54b, 0x2a0b, 0x17ad, 0x2ec4, 0x5038, 0x2284, 0xad4c, 0x2ee4, 0x6014, 0xa54e, 0x5014, 0x2a4e, 0x1691,
};
/* 2026-11-23: iD L 15th/Spacy (kind 6), iD (8) and iD Melody (9) have iD L's draw loops too: same code with their
 * own pixel buffer and gradient range addresses; each loop at its own offset (iD's differ by a few bytes). */
typedef struct { uint16_t bA, bB, bS, bG; uint32_t buf, ghi; const uint16_t *w; } Hd2X;
static const Hd2X hd2_x[10] = {
    [6] = { 0x90, 0x90, 0x90, 0x90, 0x1300, 0x842ebu, hd2_words_idl15 },
    [8] = { 0x36, 0x3a, 0x3a, 0x3c, 0x110c, 0x84287u, hd2_words_id },
    [9] = { 0x36, 0x36, 0x36, 0x36, 0x13f0, 0x84287u, hd2_words_idm },
};
static inline int hd2_head(int kind, uint32_t pc)   /* loop heads where the interpreter enters native code */
{
    const Hd2X *x = &hd2_x[kind];
    return x->w && (pc == 0x5ecu + x->bA || pc == 0x6ecu + x->bB || pc == 0x686u + x->bS || pc == 0x7bau + x->bG);
}
static inline int hd2_resume(int kind, uint32_t pc)  /* the after-get-pixel resume points */
{
    const Hd2X *x = &hd2_x[kind];
    return x->w && (pc == 0x600u + x->bA || pc == 0x700u + x->bB);
}

int tc_hle_draw2(Emu *e, int kind, uint32_t *pcp, uint32_t *spp, uint32_t *flp, int32_t *leftp)
{
    uint32_t *R = e->r, sp = *spp, fl = *flp, pc = *pcp;
    int32_t left = *leftp;
    if (left <= 0 || kind < 2 || kind == 5 || kind > 9) return 0;
    uint32_t B = kind == 3 ? 0x19au : 0;                      /* P's: same code, 0x19a bytes later; 4U+: per loop */
    const uint16_t *W = kind == 3 ? hd2_words_ps : hd2_words_idl;
    uint32_t BUF = kind == 4 ? 0x1810u : kind == 3 ? 0x167cu : 0x11e8u;   /* pixel buffer */
    uint32_t GHI = kind == 3 ? 0x8440fu : 0x84327u;            /* gradient colour range, upper end */
    const HleV2 *v = HV2(kind);
    /* 4U+ (4) and 4U (7): D = code offset of 4U against 4U+, and its data addresses */
    const uint32_t D = kind == 7 ? 0x10au : 0;
    const uint32_t GLO = kind == 7 ? 0x84272u : 0x8425eu, GHI4 = kind == 7 ? 0x84273u : 0x8425fu, RT = kind == 7 ? 0x3c64u : 0x3c10u;
    if (kind == 7) BUF = 0x1864u;
    if (kind >= 6 && kind != 7) {                              /* the other iD L-style versions (2026-11-23) */
        const Hd2X *x = &hd2_x[kind];
        if (!x->w) return 0;
        W = x->w; BUF = x->buf; GHI = x->ghi;
        if (pc == 0x5ecu + x->bA || pc == 0x600u + x->bA) {
            B = x->bA; if (!hd2_check(e, W, 0x5ec + B, 18)) return 0;
            tc_hle_draw2_runs++; if (pc == 0x600u + B) goto L600; goto L5ec;
        }
        if (pc == 0x6ecu + x->bB || pc == 0x700u + x->bB) {
            B = x->bB; if (!hd2_check(e, W + 18, 0x6ec + B, 18)) return 0;
            tc_hle_draw2_runs++; if (pc == 0x700u + B) goto L700; goto L6ec;
        }
        if (pc == 0x686u + x->bS) { B = x->bS; if (!hd2_check(e, W + 36, 0x686 + B, 19)) return 0; tc_hle_draw2_runs++; goto L686; }
        if (pc == 0x7bau + x->bG) { B = x->bG; if (!hd2_check(e, W + 55, 0x7b8 + B, 87)) return 0; tc_hle_draw2_runs++; goto L7ba; }
        return 0;
    }
    const uint32_t lp = pc - B;
#define HI(A, N)  do { left -= (int32_t)(((N) - (A)) >> 1); if (left <= 0) { pc = (N) + B; goto out; } } while (0)
/* delayed branch at A, slot S: taken -> goto LT (pc T), else continue (HDN) or leave at A+4 (HDX) */
#define HDN(A, C, LT, T, S) do { int _t = cond_ok((C), fl); left -= 1; S; left -= 1; pc = (_t ? (T) : (A) + 4) + B; \
                                 if (left <= 0) { goto out; } if (_t) { goto LT; } } while (0)
#define HDX(A, C, LT, T, S) do { HDN(A, C, LT, T, S); goto out; } while (0)
#define ADDF(a, b) op_add((a), (b), &fl)
#define SUBF(a, b) op_sub((a), (b), &fl)
#define CMPF(a, b) ((void)op_sub((a), (b), &fl))
#define NZF(q) do { uint32_t _q = (q); fl = (fl & 3u) | ((_q >> 31) << 3) | ((uint32_t)(_q == 0) << 2); } while (0)
#define MLTUH(a, b) (e->alr = ((a) & 0xFFFFu) * ((b) & 0xFFFFu))
#define MLTW(a, b) do { int64_t _p = (int64_t)(int32_t)(a) * (int32_t)(b); e->alr = (uint32_t)_p; e->ahr = (uint32_t)((uint64_t)_p >> 32); } while (0)
/* scall.d gp ; ld.w r7,r3 - get-pixel computed natively; leaves before the call if it must run for real */
#define GETPIX(A, WHICH) do { uint32_t _o7 = R[7], _res, _cyc; R[7] = R[3]; \
        if (!hle2_pixel(e, v, (WHICH), &_res, &_cyc)) { R[7] = _o7; pc = (A) + B; goto out; } \
        tc_hle.calls++; tc_hle.done++; left -= 3; R[4] = _res; left -= (int32_t)_cyc; \
        if (left <= 0) { pc = (A) + 6 + B; goto out; } } while (0)
    if (kind == 4 || kind == 7) {                                           /* 4U+: fill loops shared with iD L, two loops of its own */
        /* every entry checks the whole row loop (the pixel loops now continue into it) */
        const uint32_t q = pc - D;
        if (q == 0x680u || q == 0x674u || q == 0x694u) {
            if (!hd2_check(e, kind == 7 ? hd2_4u_outerA : hd2_4up_outerA, 0x674 + D, 30)) return 0;
            tc_hle_draw2_runs++; B = 0x680 - 0x5ec + D;
            if (q == 0x674u) goto U674;
            if (q == 0x694u) goto L600;
            goto L5ec;
        }
        if (q == 0x7dcu || q == 0x7d0u || q == 0x7f0u) {
            if (!hd2_check(e, kind == 7 ? hd2_4u_outerB : hd2_4up_outerB, 0x7d0 + D, 34)) return 0;
            tc_hle_draw2_runs++; B = 0x7dc - 0x6ec + D;
            if (q == 0x7d0u) goto U7d0;
            if (q == 0x7f0u) goto L700;
            goto L6ec;
        }
        if (q == 0x750u) { if (!hd2_check(e, kind == 7 ? hd2_4u_send : hd2_words_4up + 36, 0x750 + D, 38)) return 0; tc_hle_draw2_runs++; B = D; goto U750; }
        if (q == 0x8e4u || q == 0x8c0u) {
            if (!hd2_check(e, kind == 7 ? hd2_4u_outerG : hd2_4up_outerG, 0x8c0 + D, 112)) return 0;
            tc_hle_draw2_runs++; B = D;
            if (q == 0x8c0u) goto U8c0;
            goto U8e4;
        }
        return 0;
    }
    if (lp == 0x5ecu || lp == 0x600u) {
        if (!hd2_check(e, W, 0x5ec + B, 18)) return 0;
        tc_hle_draw2_runs++;
        if (lp == 0x600u) goto L600;
        goto L5ec;
    }
    if (lp == 0x6ecu || lp == 0x700u) {
        if (!hd2_check(e, W + 18, 0x6ec + B, 18)) return 0;
        tc_hle_draw2_runs++;
        if (lp == 0x700u) goto L700;
        goto L6ec;
    }
    if (lp == 0x686u) { if (!hd2_check(e, W + 36, 0x686 + B, 19)) return 0; tc_hle_draw2_runs++; goto L686; }
    if (lp == 0x7bau) { if (!hd2_check(e, W + 55, 0x7b8 + B, 87)) return 0; tc_hle_draw2_runs++; goto L7ba; }
    return 0;

    /* ---- fill A: r0 = x (to [sp+2]), r1 = buffer, r2 = index, r3 = y ---- */
L5ec:
    R[4] = R[2];                                 HI(0x5ec, 0x5ee);   /* ld.w r4,r2 */
    R[4] = ADDF(R[4], 1);                        HI(0x5ee, 0x5f0);   /* add r4,0x1 */
    R[2] = R[4] & 0xFFFFu;                       HI(0x5f0, 0x5f2);   /* ld.uh r2,r4 */
    R[4] = BUF;                                  HI(0x5f2, 0x5f6);   /* xld.w r4,BUF */
    R[1] = ADDF(R[1], R[4]);                     HI(0x5f6, 0x5f8);   /* add r1,r4 */
    R[6] = R[0];                                 HI(0x5f8, 0x5fa);   /* ld.w r6,r0 */
    GETPIX(0x5fa, 0);                                                /* scall.d gpA ; ld.w r7,r3 */
L600:                                                                /* resume point after an event (r4 = pixel) */
    f_wr8(e, R[1], R[4]);                        HI(0x600, 0x602);   /* ld.b [r1],r4 */
    R[4] = R[0];                                 HI(0x602, 0x604);   /* ld.w r4,r0 */
    R[4] = ADDF(R[4], 1);                        HI(0x604, 0x606);   /* add r4,0x1 */
    R[0] = R[4] & 0xFFu;                         HI(0x606, 0x608);   /* ld.ub r0,r4 */
    R[4] = f_rd32(e, sp + 8);                    HI(0x608, 0x60a);   /* ld.w r4,[sp+0x2] */
    CMPF(R[0], R[4]);                            HI(0x60a, 0x60c);   /* cmp r0,r4 */
    HDN(0x60c, 11, L5ec, 0x5ec, R[1] = R[2]);                       /* jrule.d 0x5ec ; ld.w r1,r2 */
    if (kind == 4 || kind == 7) goto U6a4;                                          /* 4U+: on to its row loop */
    goto out;

    /* ---- fill B: the same with r5 and get-pixel B ---- */
L6ec:
    R[4] = R[2];                                 HI(0x6ec, 0x6ee);   /* ld.w r4,r2 */
    R[4] = ADDF(R[4], 1);                        HI(0x6ee, 0x6f0);   /* add r4,0x1 */
    R[2] = R[4] & 0xFFFFu;                       HI(0x6f0, 0x6f2);   /* ld.uh r2,r4 */
    R[5] = BUF;                                  HI(0x6f2, 0x6f6);   /* xld.w r5,BUF */
    R[1] = ADDF(R[1], R[5]);                     HI(0x6f6, 0x6f8);   /* add r1,r5 */
    R[6] = R[0];                                 HI(0x6f8, 0x6fa);   /* ld.w r6,r0 */
    GETPIX(0x6fa, 1);                                                /* scall.d gpB ; ld.w r7,r3 */
L700:                                                                /* resume point after an event (r4 = pixel) */
    f_wr8(e, R[1], R[4]);                        HI(0x700, 0x702);   /* ld.b [r1],r4 */
    R[4] = R[0];                                 HI(0x702, 0x704);   /* ld.w r4,r0 */
    R[4] = ADDF(R[4], 1);                        HI(0x704, 0x706);   /* add r4,0x1 */
    R[0] = R[4] & 0xFFu;                         HI(0x706, 0x708);   /* ld.ub r0,r4 */
    R[4] = f_rd32(e, sp + 8);                    HI(0x708, 0x70a);   /* ld.w r4,[sp+0x2] */
    CMPF(R[0], R[4]);                            HI(0x70a, 0x70c);   /* cmp r0,r4 */
    HDN(0x70c, 11, L6ec, 0x6ec, R[1] = R[2]);                       /* jrule.d 0x6ec ; ld.w r1,r2 */
    if (kind == 4 || kind == 7) goto U800;                                          /* 4U+: on to its row loop */
    goto out;

    /* ---- simple send: r6 = index (to r8), tables r13 (buffer), r12/r11/r9 (R, G, B), r7 = LCD data ---- */
L686:
    R[4] = ADDF(R[4], R[13]);                    HI(0x686, 0x688);   /* add r4,r13 */
    R[5] = f_rd8(e, R[4]);                       HI(0x688, 0x68a);   /* ld.ub r5,[r4] */
    R[4] = R[5];                                 HI(0x68a, 0x68c);   /* ld.w r4,r5 */
    R[4] = ADDF(R[4], R[12]);                    HI(0x68c, 0x68e);   /* add r4,r12 */
    R[4] = f_rd8(e, R[4]);                       HI(0x68e, 0x690);   /* ld.ub r4,[r4] */
    f_wr8(e, R[7], R[4]);                        HI(0x690, 0x692);   /* ld.b [r7],r4  (LCD) */
    R[4] = R[5];                                 HI(0x692, 0x694);   /* ld.w r4,r5 */
    R[4] = ADDF(R[4], R[11]);                    HI(0x694, 0x696);   /* add r4,r11 */
    R[4] = f_rd8(e, R[4]);                       HI(0x696, 0x698);   /* ld.ub r4,[r4] */
    f_wr8(e, R[7], R[4]);                        HI(0x698, 0x69a);   /* ld.b [r7],r4  (LCD) */
    R[5] = ADDF(R[5], R[9]);                     HI(0x69a, 0x69c);   /* add r5,r9 */
    R[4] = f_rd8(e, R[5]);                       HI(0x69c, 0x69e);   /* ld.ub r4,[r5] */
    f_wr8(e, R[7], R[4]);                        HI(0x69e, 0x6a0);   /* ld.b [r7],r4  (LCD) */
    R[4] = R[6];                                 HI(0x6a0, 0x6a2);   /* ld.w r4,r6 */
    R[4] = ADDF(R[4], 1);                        HI(0x6a2, 0x6a4);   /* add r4,0x1 */
    R[6] = R[4] & 0xFFFFu;                       HI(0x6a4, 0x6a6);   /* ld.uh r6,r4 */
    CMPF(R[6], R[8]);                            HI(0x6a6, 0x6a8);   /* cmp r6,r8 */
    HDX(0x6a8, 10, L686, 0x686, R[4] = R[6]);                       /* jrult.d 0x686 ; ld.w r4,r6 */

    /* ---- gradient send: r12 = index, r11 = x (to r0), r1 = y, r9 = colour, r10 = range low,
     *      r14/r3/r2 = R/G/B tables, r13 = LCD data, [sp+3] = row inside the gradient ---- */
L7ba:
    R[5] = R[12];                                HI(0x7ba, 0x7bc);   /* ld.w r5,r12 */
    R[5] = ADDF(R[5], 1);                        HI(0x7bc, 0x7be);   /* add r5,0x1 */
    R[12] = R[5] & 0xFFFFu;                      HI(0x7be, 0x7c0);   /* ld.uh r12,r5 */
    R[5] = BUF;                                  HI(0x7c0, 0x7c4);   /* xld.w r5,BUF */
    R[4] = ADDF(R[4], R[5]);                     HI(0x7c4, 0x7c6);   /* add r4,r5 */
    R[9] = f_rd8(e, R[4]);                       HI(0x7c6, 0x7c8);   /* ld.ub r9,[r4] */
    CMPF(R[10], R[9]);                           HI(0x7c8, 0x7ca);   /* cmp r10,r9 */
    HDN(0x7ca, 8, L844, 0x844, R[4] = R[9]);                        /* jrugt.d 0x844 ; ld.w r4,r9 */
    R[4] = GHI;                                  HI(0x7ce, 0x7d4);   /* xld.w r4,GHI */
    R[4] = f_rd8(e, R[4]);                       HI(0x7d4, 0x7d6);   /* ld.ub r4,[r4] */
    CMPF(R[4], R[9]);                            HI(0x7d6, 0x7d8);   /* cmp r4,r9 */
    HDN(0x7d8, 11, L844, 0x844, R[4] = R[9]);                       /* jrule.d 0x844 ; ld.w r4,r9 */
    R[4] = f_rd32(e, sp + 12);                   HI(0x7dc, 0x7de);   /* ld.w r4,[sp+0x3] */
    CMPF(R[4], 0x3f);                            HI(0x7de, 0x7e2);   /* xcmp r4,0x3f */
    HDN(0x7e2, 8, L844, 0x844, R[4] = R[9]);                        /* jrugt.d 0x844 ; ld.w r4,r9 */
    R[5] = 0x5b;                                 HI(0x7e6, 0x7ea);   /* xld.w r5,0x5b */
    R[5] = SUBF(R[5], R[1]);                     HI(0x7ea, 0x7ec);   /* sub r5,r1 */
    R[5] &= 0xFFFFu;                             HI(0x7ec, 0x7ee);   /* ld.uh r5,r5 */
    R[7] = 0x58;                                 HI(0x7ee, 0x7f2);   /* xld.w r7,0x58 */
    MLTUH(R[5], R[7]);                           HI(0x7f2, 0x7f4);   /* mltu.h r5,r7 */
    R[7] = e->alr;                               HI(0x7f4, 0x7f6);   /* ld.w r7,%alr */
    R[4] = ADDF(R[4], R[14]);                    HI(0x7f6, 0x7f8);   /* add r4,r14 */
    R[4] = f_rd8(e, R[4]);                       HI(0x7f8, 0x7fa);   /* ld.ub r4,[r4] */
    R[8] = 0x40;                                 HI(0x7fa, 0x7fe);   /* xld.w r8,0x40 */
    R[8] = SUBF(R[8], R[5]);                     HI(0x7fe, 0x800);   /* sub r8,r5 */
    MLTW(R[4], R[8]);                            HI(0x800, 0x802);   /* mlt.w r4,r8 */
    R[4] = e->alr;                               HI(0x802, 0x804);   /* ld.w r4,%alr */
    R[7] = ADDF(R[7], R[4]);                     HI(0x804, 0x806);   /* add r7,r4 */
    R[7] = (uint32_t)((int32_t)R[7] >> 6); NZF(R[7]); HI(0x806, 0x808);   /* sra r7,0x6 */
    R[6] = 0x51;                                 HI(0x808, 0x80c);   /* xld.w r6,0x51 */
    MLTUH(R[5], R[6]);                           HI(0x80c, 0x80e);   /* mltu.h r5,r6 */
    R[6] = e->alr;                               HI(0x80e, 0x810);   /* ld.w r6,%alr */
    R[4] = R[9];                                 HI(0x810, 0x812);   /* ld.w r4,r9 */
    R[4] = ADDF(R[4], R[3]);                     HI(0x812, 0x814);   /* add r4,r3 */
    R[4] = f_rd8(e, R[4]);                       HI(0x814, 0x816);   /* ld.ub r4,[r4] */
    MLTW(R[4], R[8]);                            HI(0x816, 0x818);   /* mlt.w r4,r8 */
    R[4] = e->alr;                               HI(0x818, 0x81a);   /* ld.w r4,%alr */
    R[6] = ADDF(R[6], R[4]);                     HI(0x81a, 0x81c);   /* add r6,r4 */
    R[6] = (uint32_t)((int32_t)R[6] >> 6); NZF(R[6]); HI(0x81c, 0x81e);   /* sra r6,0x6 */
    R[4] = 0xff;                                 HI(0x81e, 0x822);   /* xld.w r4,0xff */
    MLTUH(R[5], R[4]);                           HI(0x822, 0x824);   /* mltu.h r5,r4 */
    R[5] = e->alr;                               HI(0x824, 0x826);   /* ld.w r5,%alr */
    R[4] = R[9];                                 HI(0x826, 0x828);   /* ld.w r4,r9 */
    R[4] = ADDF(R[4], R[2]);                     HI(0x828, 0x82a);   /* add r4,r2 */
    R[4] = f_rd8(e, R[4]);                       HI(0x82a, 0x82c);   /* ld.ub r4,[r4] */
    MLTW(R[4], R[8]);                            HI(0x82c, 0x82e);   /* mlt.w r4,r8 */
    R[4] = e->alr;                               HI(0x82e, 0x830);   /* ld.w r4,%alr */
    R[5] = ADDF(R[5], R[4]);                     HI(0x830, 0x832);   /* add r5,r4 */
    R[5] = (uint32_t)((int32_t)R[5] >> 6); NZF(R[5]); HI(0x832, 0x834);   /* sra r5,0x6 */
    R[7] &= 0xFFu;                               HI(0x834, 0x836);   /* ld.ub r7,r7 */
    f_wr8(e, R[13], R[7]);                       HI(0x836, 0x838);   /* ld.b [r13],r7  (LCD) */
    R[6] &= 0xFFu;                               HI(0x838, 0x83a);   /* ld.ub r6,r6 */
    f_wr8(e, R[13], R[6]);                       HI(0x83a, 0x83c);   /* ld.b [r13],r6  (LCD) */
    R[5] &= 0xFFu;                               HI(0x83c, 0x83e);   /* ld.ub r5,r5 */
    f_wr8(e, R[13], R[5]);                       HI(0x83e, 0x840);   /* ld.b [r13],r5  (LCD) */
    left -= 1; R[4] = R[11]; left -= 1; pc = 0x85c + B;               /* jp.d 0x85c ; ld.w r4,r11 */
    if (left <= 0) goto out;
    goto L85c;
L844:
    R[4] = ADDF(R[4], R[14]);                    HI(0x844, 0x846);   /* add r4,r14 */
    R[4] = f_rd8(e, R[4]);                       HI(0x846, 0x848);   /* ld.ub r4,[r4] */
    f_wr8(e, R[13], R[4]);                       HI(0x848, 0x84a);   /* ld.b [r13],r4  (LCD) */
    R[4] = R[9];                                 HI(0x84a, 0x84c);   /* ld.w r4,r9 */
    R[4] = ADDF(R[4], R[3]);                     HI(0x84c, 0x84e);   /* add r4,r3 */
    R[4] = f_rd8(e, R[4]);                       HI(0x84e, 0x850);   /* ld.ub r4,[r4] */
    f_wr8(e, R[13], R[4]);                       HI(0x850, 0x852);   /* ld.b [r13],r4  (LCD) */
    R[4] = R[9];                                 HI(0x852, 0x854);   /* ld.w r4,r9 */
    R[4] = ADDF(R[4], R[2]);                     HI(0x854, 0x856);   /* add r4,r2 */
    R[4] = f_rd8(e, R[4]);                       HI(0x856, 0x858);   /* ld.ub r4,[r4] */
    f_wr8(e, R[13], R[4]);                       HI(0x858, 0x85a);   /* ld.b [r13],r4  (LCD) */
    R[4] = R[11];                                HI(0x85a, 0x85c);   /* ld.w r4,r11 */
L85c:
    R[4] = ADDF(R[4], 1);                        HI(0x85c, 0x85e);   /* add r4,0x1 */
    R[11] = R[4] & 0xFFu;                        HI(0x85e, 0x860);   /* ld.ub r11,r4 */
    CMPF(R[11], R[0]);                           HI(0x860, 0x862);   /* cmp r11,r0 */
    HDX(0x862, 11, L7ba, 0x7ba, R[4] = R[12]);                      /* jrule.d 0x7ba ; ld.w r4,r12 */

    /* ---- 4U+ send: r6 = index (to r8), buffer 0x1810, r9 = palette flag, r13/r12/r11 = R/G/B tables
     *      (+0x100 for the second palette), r7 = LCD data ---- */
U750:
    R[5] = BUF;                                  HI(0x750, 0x754);   /* xld.w r5,0x1810 */
    R[4] = ADDF(R[4], R[5]);                     HI(0x754, 0x756);   /* add r4,r5 */
    R[5] = f_rd8(e, R[4]);                       HI(0x756, 0x758);   /* ld.ub r5,[r4] */
    CMPF(R[9], 1);                               HI(0x758, 0x75a);   /* cmp r9,0x1 */
    HDN(0x75a, 13, U774, 0x774, R[4] = R[5]);                       /* jrne.d 0x774 ; ld.w r4,r5 */
    R[4] = ADDF(R[4], R[13]);                    HI(0x75e, 0x760);   /* add r4,r13 */
    R[4] = f_rd8(e, R[4]);                       HI(0x760, 0x762);   /* ld.ub r4,[r4] */
    f_wr8(e, R[7], R[4]);                        HI(0x762, 0x764);   /* ld.b [r7],r4  (LCD) */
    R[4] = R[5];                                 HI(0x764, 0x766);   /* ld.w r4,r5 */
    R[4] = ADDF(R[4], R[12]);                    HI(0x766, 0x768);   /* add r4,r12 */
    R[4] = f_rd8(e, R[4]);                       HI(0x768, 0x76a);   /* ld.ub r4,[r4] */
    f_wr8(e, R[7], R[4]);                        HI(0x76a, 0x76c);   /* ld.b [r7],r4  (LCD) */
    R[4] = R[5];                                 HI(0x76c, 0x76e);   /* ld.w r4,r5 */
    R[4] = ADDF(R[4], R[11]);                    HI(0x76e, 0x770);   /* add r4,r11 */
    R[4] = f_rd8(e, R[4]);                       HI(0x770, 0x772);   /* ld.ub r4,[r4] */
    left -= 1; pc = 0x78e + B;                                       /* jp 0x78e (not delayed) */
    if (left <= 0) goto out;
    goto U78e;
U774:
    R[4] = ADDF(R[4], R[13]);                    HI(0x774, 0x776);   /* add r4,r13 */
    R[4] = f_rd8(e, R[4] + 0x100u);              HI(0x776, 0x77a);   /* xld.ub r4,[r4+0x100] */
    f_wr8(e, R[7], R[4]);                        HI(0x77a, 0x77c);   /* ld.b [r7],r4  (LCD) */
    R[4] = R[5];                                 HI(0x77c, 0x77e);   /* ld.w r4,r5 */
    R[4] = ADDF(R[4], R[12]);                    HI(0x77e, 0x780);   /* add r4,r12 */
    R[4] = f_rd8(e, R[4] + 0x100u);              HI(0x780, 0x784);   /* xld.ub r4,[r4+0x100] */
    f_wr8(e, R[7], R[4]);                        HI(0x784, 0x786);   /* ld.b [r7],r4  (LCD) */
    R[4] = R[5];                                 HI(0x786, 0x788);   /* ld.w r4,r5 */
    R[4] = ADDF(R[4], R[11]);                    HI(0x788, 0x78a);   /* add r4,r11 */
    R[4] = f_rd8(e, R[4] + 0x100u);              HI(0x78a, 0x78e);   /* xld.ub r4,[r4+0x100] */
U78e:
    f_wr8(e, R[7], R[4]);                        HI(0x78e, 0x790);   /* ld.b [r7],r4  (LCD) */
    R[4] = R[6];                                 HI(0x790, 0x792);   /* ld.w r4,r6 */
    R[4] = ADDF(R[4], 1);                        HI(0x792, 0x794);   /* add r4,0x1 */
    R[6] = R[4] & 0xFFFFu;                       HI(0x794, 0x796);   /* ld.uh r6,r4 */
    CMPF(R[6], R[8]);                            HI(0x796, 0x798);   /* cmp r6,r8 */
    HDX(0x798, 10, U750, 0x750, R[4] = R[6]);                       /* jrult.d 0x750 ; ld.w r4,r6 */

    /* ---- 4U+ gradient send: r12 = index, r11 = x (to r0), r14 = y, r9 = colour, r1 = range low,
     *      r10/r3/r2 = R/G/B tables, r13 = LCD data, [sp+4] = row inside the gradient ---- */
U8e4:
    R[5] = R[12];                                HI(0x8e4, 0x8e6);   /* ld.w r5,r12 */
    R[5] = ADDF(R[5], 1);                        HI(0x8e6, 0x8e8);   /* add r5,0x1 */
    R[12] = R[5] & 0xFFFFu;                      HI(0x8e8, 0x8ea);   /* ld.uh r12,r5 */
    R[5] = BUF;                                  HI(0x8ea, 0x8ee);   /* xld.w r5,0x1810 */
    R[4] = ADDF(R[4], R[5]);                     HI(0x8ee, 0x8f0);   /* add r4,r5 */
    R[9] = f_rd8(e, R[4]);                       HI(0x8f0, 0x8f2);   /* ld.ub r9,[r4] */
    CMPF(R[1], R[9]);                            HI(0x8f2, 0x8f4);   /* cmp r1,r9 */
    HDN(0x8f4, 8, U96c, 0x96c, R[4] = R[9]);                        /* jrugt.d 0x96c ; ld.w r4,r9 */
    R[4] = GHI4;                                 HI(0x8f8, 0x8fe);   /* xld.w r4,0x8425f */
    R[4] = f_rd8(e, R[4]);                       HI(0x8fe, 0x900);   /* ld.ub r4,[r4] */
    CMPF(R[4], R[9]);                            HI(0x900, 0x902);   /* cmp r4,r9 */
    HDN(0x902, 11, U96c, 0x96c, R[4] = R[9]);                       /* jrule.d 0x96c ; ld.w r4,r9 */
    R[7] = f_rd32(e, sp + 16);                   HI(0x906, 0x908);   /* ld.w r7,[sp+0x4] */
    CMPF(R[7], 0x3f);                            HI(0x908, 0x90c);   /* xcmp r7,0x3f */
    {   int t = cond_ok(8, fl);                                      /* jrugt 0x96c (not delayed) */
        left -= 1; pc = t ? 0x96cu + D : 0x90eu + D;
        if (left <= 0) goto out;
        if (t) goto U96c;
    }
    R[5] = 0x5b;                                 HI(0x90e, 0x912);   /* xld.w r5,0x5b */
    R[5] = SUBF(R[5], R[14]);                    HI(0x912, 0x914);   /* sub r5,r14 */
    R[5] &= 0xFFFFu;                             HI(0x914, 0x916);   /* ld.uh r5,r5 */
    R[7] = 0x58;                                 HI(0x916, 0x91a);   /* xld.w r7,0x58 */
    MLTUH(R[5], R[7]);                           HI(0x91a, 0x91c);   /* mltu.h r5,r7 */
    R[7] = e->alr;                               HI(0x91c, 0x91e);   /* ld.w r7,%alr */
    R[4] = ADDF(R[4], R[10]);                    HI(0x91e, 0x920);   /* add r4,r10 */
    R[4] = f_rd8(e, R[4]);                       HI(0x920, 0x922);   /* ld.ub r4,[r4] */
    R[8] = 0x40;                                 HI(0x922, 0x926);   /* xld.w r8,0x40 */
    R[8] = SUBF(R[8], R[5]);                     HI(0x926, 0x928);   /* sub r8,r5 */
    MLTW(R[4], R[8]);                            HI(0x928, 0x92a);   /* mlt.w r4,r8 */
    R[4] = e->alr;                               HI(0x92a, 0x92c);   /* ld.w r4,%alr */
    R[7] = ADDF(R[7], R[4]);                     HI(0x92c, 0x92e);   /* add r7,r4 */
    R[7] = (uint32_t)((int32_t)R[7] >> 6); NZF(R[7]); HI(0x92e, 0x930);   /* sra r7,0x6 */
    R[6] = 0x51;                                 HI(0x930, 0x934);   /* xld.w r6,0x51 */
    MLTUH(R[5], R[6]);                           HI(0x934, 0x936);   /* mltu.h r5,r6 */
    R[6] = e->alr;                               HI(0x936, 0x938);   /* ld.w r6,%alr */
    R[4] = R[9];                                 HI(0x938, 0x93a);   /* ld.w r4,r9 */
    R[4] = ADDF(R[4], R[3]);                     HI(0x93a, 0x93c);   /* add r4,r3 */
    R[4] = f_rd8(e, R[4]);                       HI(0x93c, 0x93e);   /* ld.ub r4,[r4] */
    MLTW(R[4], R[8]);                            HI(0x93e, 0x940);   /* mlt.w r4,r8 */
    R[4] = e->alr;                               HI(0x940, 0x942);   /* ld.w r4,%alr */
    R[6] = ADDF(R[6], R[4]);                     HI(0x942, 0x944);   /* add r6,r4 */
    R[6] = (uint32_t)((int32_t)R[6] >> 6); NZF(R[6]); HI(0x944, 0x946);   /* sra r6,0x6 */
    R[4] = 0xff;                                 HI(0x946, 0x94a);   /* xld.w r4,0xff */
    MLTUH(R[5], R[4]);                           HI(0x94a, 0x94c);   /* mltu.h r5,r4 */
    R[5] = e->alr;                               HI(0x94c, 0x94e);   /* ld.w r5,%alr */
    R[4] = R[9];                                 HI(0x94e, 0x950);   /* ld.w r4,r9 */
    R[4] = ADDF(R[4], R[2]);                     HI(0x950, 0x952);   /* add r4,r2 */
    R[4] = f_rd8(e, R[4]);                       HI(0x952, 0x954);   /* ld.ub r4,[r4] */
    MLTW(R[4], R[8]);                            HI(0x954, 0x956);   /* mlt.w r4,r8 */
    R[4] = e->alr;                               HI(0x956, 0x958);   /* ld.w r4,%alr */
    R[5] = ADDF(R[5], R[4]);                     HI(0x958, 0x95a);   /* add r5,r4 */
    R[5] = (uint32_t)((int32_t)R[5] >> 6); NZF(R[5]); HI(0x95a, 0x95c);   /* sra r5,0x6 */
    R[7] &= 0xFFu;                               HI(0x95c, 0x95e);   /* ld.ub r7,r7 */
    f_wr8(e, R[13], R[7]);                       HI(0x95e, 0x960);   /* ld.b [r13],r7  (LCD) */
    R[6] &= 0xFFu;                               HI(0x960, 0x962);   /* ld.ub r6,r6 */
    f_wr8(e, R[13], R[6]);                       HI(0x962, 0x964);   /* ld.b [r13],r6  (LCD) */
    R[5] &= 0xFFu;                               HI(0x964, 0x966);   /* ld.ub r5,r5 */
    f_wr8(e, R[13], R[5]);                       HI(0x966, 0x968);   /* ld.b [r13],r5  (LCD) */
    left -= 1; R[4] = R[11]; left -= 1; pc = 0x984 + B;              /* jp.d 0x984 ; ld.w r4,r11 (2026-11-24: + B was missing - on the 4U a cycle-budget exit here resumed 0x10a too early: the night-tint runaway) */
    if (left <= 0) goto out;
    goto U984;
U96c:
    R[4] = ADDF(R[4], R[10]);                    HI(0x96c, 0x96e);   /* add r4,r10 */
    R[4] = f_rd8(e, R[4]);                       HI(0x96e, 0x970);   /* ld.ub r4,[r4] */
    f_wr8(e, R[13], R[4]);                       HI(0x970, 0x972);   /* ld.b [r13],r4  (LCD) */
    R[4] = R[9];                                 HI(0x972, 0x974);   /* ld.w r4,r9 */
    R[4] = ADDF(R[4], R[3]);                     HI(0x974, 0x976);   /* add r4,r3 */
    R[4] = f_rd8(e, R[4]);                       HI(0x976, 0x978);   /* ld.ub r4,[r4] */
    f_wr8(e, R[13], R[4]);                       HI(0x978, 0x97a);   /* ld.b [r13],r4  (LCD) */
    R[4] = R[9];                                 HI(0x97a, 0x97c);   /* ld.w r4,r9 */
    R[4] = ADDF(R[4], R[2]);                     HI(0x97c, 0x97e);   /* add r4,r2 */
    R[4] = f_rd8(e, R[4]);                       HI(0x97e, 0x980);   /* ld.ub r4,[r4] */
    f_wr8(e, R[13], R[4]);                       HI(0x980, 0x982);   /* ld.b [r13],r4  (LCD) */
    R[4] = R[11];                                HI(0x982, 0x984);   /* ld.w r4,r11 */
U984:
    R[4] = ADDF(R[4], 1);                        HI(0x984, 0x986);   /* add r4,0x1 */
    R[11] = R[4] & 0xFFu;                        HI(0x986, 0x988);   /* ld.ub r11,r4 */
    CMPF(R[11], R[0]);                           HI(0x988, 0x98a);   /* cmp r11,r0 */
    HDN(0x98a, 11, U8e4, 0x8e4, R[4] = R[12]);                      /* jrule.d 0x8e4 ; ld.w r4,r12 */
    goto U98e;                                                         /* on to the row loop */

    /* ---- 4U+ row loops (absolute addresses: HIA / HDNA; the pixel loops they enter use B) ---- */
#define HIA(A, N)  do { left -= (int32_t)(((N) - (A)) >> 1); if (left <= 0) { pc = (N) + D; goto out; } } while (0)
#define HDNA(A, C, LT, T, S) do { int _t = cond_ok((C), fl); left -= 1; S; left -= 1; pc = (_t ? (T) : (A) + 4) + D; \
                                  if (left <= 0) { goto out; } if (_t) { goto LT; } } while (0)
    /* fill A: r3 = y (to [sp+0]), r0 = x from [sp+1] to [sp+2], r2 = buffer index */
U674:
    R[0] = f_rd32(e, sp + 4);                    HIA(0x674, 0x676);  /* ld.w r0,[sp+0x1] */
    R[4] = f_rd32(e, sp + 8);                    HIA(0x676, 0x678);  /* ld.w r4,[sp+0x2] */
    CMPF(R[0], R[4]);                            HIA(0x678, 0x67a);  /* cmp r0,r4 */
    HDNA(0x67a, 8, U6a6, 0x6a6, R[4] = R[3]);                        /* jrugt.d 0x6a6 ; ld.w r4,r3 */
    R[1] = R[2];                                 HIA(0x67e, 0x680);  /* ld.w r1,r2 */
    goto L5ec;                                                       /* pixel loop 0x680 (B = 0x94) */
U6a4:
    R[4] = R[3];                                 HIA(0x6a4, 0x6a6);  /* ld.w r4,r3 */
U6a6:
    R[4] = ADDF(R[4], 1);                        HIA(0x6a6, 0x6a8);  /* add r4,0x1 */
    R[3] = R[4] & 0xFFu;                         HIA(0x6a8, 0x6aa);  /* ld.ub r3,r4 */
    R[4] = f_rd32(e, sp);                        HIA(0x6aa, 0x6ac);  /* ld.w r4,[sp+0x0] */
    CMPF(R[3], R[4]);                            HIA(0x6ac, 0x6ae);  /* cmp r3,r4 */
    {   int t = cond_ok(11, fl);                                     /* jrule 0x674 (not delayed) */
        left -= 1; pc = t ? 0x674u + D : 0x6b0u + D;
        if (left <= 0) goto out;
        if (t) goto U674;
        goto out; }
    /* fill B: r3 = y (to [sp+0]), r0 = x from [sp+1] to [sp+2], r2 = index, [sp+3] = row step */
U7d0:
    R[0] = f_rd32(e, sp + 4);                    HIA(0x7d0, 0x7d2);  /* ld.w r0,[sp+0x1] */
    R[4] = f_rd32(e, sp + 8);                    HIA(0x7d2, 0x7d4);  /* ld.w r4,[sp+0x2] */
    CMPF(R[0], R[4]);                            HIA(0x7d4, 0x7d6);  /* cmp r0,r4 */
    HDNA(0x7d6, 8, U802, 0x802, R[4] = R[2]);                        /* jrugt.d 0x802 ; ld.w r4,r2 */
    R[1] = R[2];                                 HIA(0x7da, 0x7dc);  /* ld.w r1,r2 */
    goto L6ec;                                                       /* pixel loop 0x7dc (B = 0xf0) */
U800:
    R[4] = R[2];                                 HIA(0x800, 0x802);  /* ld.w r4,r2 */
U802:
    R[5] = f_rd32(e, sp + 12);                   HIA(0x802, 0x804);  /* ld.w r5,[sp+0x3] */
    R[4] = ADDF(R[4], R[5]);                     HIA(0x804, 0x806);  /* add r4,r5 */
    R[2] = R[4] & 0xFFFFu;                       HIA(0x806, 0x808);  /* ld.uh r2,r4 */
    R[4] = R[3];                                 HIA(0x808, 0x80a);  /* ld.w r4,r3 */
    R[4] = ADDF(R[4], 1);                        HIA(0x80a, 0x80c);  /* add r4,0x1 */
    R[3] = R[4] & 0xFFu;                         HIA(0x80c, 0x80e);  /* ld.ub r3,r4 */
    R[4] = f_rd32(e, sp);                        HIA(0x80e, 0x810);  /* ld.w r4,[sp+0x0] */
    CMPF(R[3], R[4]);                            HIA(0x810, 0x812);  /* cmp r3,r4 */
    {   int t = cond_ok(11, fl);                                     /* jrule 0x7d0 (not delayed) */
        left -= 1; pc = t ? 0x7d0u + D : 0x814u + D;
        if (left <= 0) goto out;
        if (t) goto U7d0;
        goto out; }
    /* gradient send: r14 = y up to [sp+1], r11 = x from [sp+2] to r0, r12 = index, [sp+3] = row step */
U8c0:
    R[11] = f_rd32(e, sp + 8);                   HIA(0x8c0, 0x8c2);  /* ld.w r11,[sp+0x2] */
    CMPF(R[11], R[0]);                           HIA(0x8c2, 0x8c4);  /* cmp r11,r0 */
    HDNA(0x8c4, 8, U98e, 0x98e, R[4] = R[12]);                       /* jrugt.d 0x98e ; ld.w r4,r12 */
    R[4] = GLO;                                  HIA(0x8c8, 0x8ce);  /* xld.w r4,0x8425e */
    R[1] = f_rd8(e, R[4]);                       HIA(0x8ce, 0x8d0);  /* ld.ub r1,[r4] */
    R[4] = R[14];                                HIA(0x8d0, 0x8d2);  /* ld.w r4,r14 */
    R[4] = SUBF(R[4], 0x1c);                     HIA(0x8d2, 0x8d4);  /* sub r4,0x1c */
    R[4] &= 0xFFu;                               HIA(0x8d4, 0x8d6);  /* ld.ub r4,r4 */
    f_wr32(e, sp + 16, R[4]);                    HIA(0x8d6, 0x8d8);  /* ld.w [sp+0x4],r4 */
    R[10] = RT;                                  HIA(0x8d8, 0x8dc);  /* xld.w r10,0x3c10 */
    R[13] = 0x600001u;                           HIA(0x8dc, 0x8e2);  /* xld.w r13,0x600001 */
    R[4] = R[12];                                HIA(0x8e2, 0x8e4);  /* ld.w r4,r12 */
    goto U8e4;
U98e:
    R[8] = f_rd32(e, sp + 12);                   HIA(0x98e, 0x990);  /* ld.w r8,[sp+0x3] */
    R[4] = ADDF(R[4], R[8]);                     HIA(0x990, 0x992);  /* add r4,r8 */
    R[12] = R[4] & 0xFFFFu;                      HIA(0x992, 0x994);  /* ld.uh r12,r4 */
    R[4] = R[14];                                HIA(0x994, 0x996);  /* ld.w r4,r14 */
    R[4] = ADDF(R[4], 1);                        HIA(0x996, 0x998);  /* add r4,0x1 */
    R[14] = R[4] & 0xFFu;                        HIA(0x998, 0x99a);  /* ld.ub r14,r4 */
    R[4] = f_rd32(e, sp + 4);                    HIA(0x99a, 0x99c);  /* ld.w r4,[sp+0x1] */
    CMPF(R[14], R[4]);                           HIA(0x99c, 0x99e);  /* cmp r14,r4 */
    {   int t = cond_ok(11, fl);                                     /* jrule 0x8c0 (not delayed) */
        left -= 1; pc = t ? 0x8c0u + D : 0x9a0u + D;
        if (left <= 0) goto out;
        if (t) goto U8c0;
        goto out; }
#undef HIA
#undef HDNA

out:
    *pcp = pc; *spp = sp; *flp = fl; *leftp = left;
    return 1;
#undef HI
#undef HDN
#undef HDX
#undef ADDF
#undef SUBF
#undef CMPF
#undef NZF
#undef MLTUH
#undef MLTW
#undef GETPIX
}
