/* TamaLauncher: multi-ROM colour Tamagotchi emulator for the Waveshare ESP32-S3-Touch-AMOLED-2.16.
 *
 * Layout of the program:
 *   ui_task    (core 0) owns the screen and the three hardware keys. It runs the launcher menu, the
 *              "PLEASE WAIT" loading screen, and while a game runs it copies the game picture to the screen.
 *   emu_task   (core 1) runs the emulated Tamagotchi CPU at its real clock speed (paced by wall time).
 *   audio_task (core 0) turns the emulated buzzer tone into sound.
 *
 * The whole 480x480 screen is one image (s_fb) that we draw ourselves; LVGL is only used to send it to the display.
 * Menus are drawn on a 160x160 "virtual LCD" with an 8x8 pixel font and scaled 3x, for an LCD look. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <strings.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include <sys/stat.h>
#include "esp_codec_dev.h"
#include "tc.h"
#include "food_icons.h"                           /* original food icons (replaced the game-cut ones, 2026-11-23) */
#include "loading_eggs.h"
#include <sys/time.h>
#include <time.h>
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "wifi_update.h"
#include "file_server.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "update_key.h"
#include "dlc.h"
#include "about_qr.h"
#include "libs/qrcode/qrcodegen.h"            /* LVGL's copy of Nayuki's QR code generator (MIT) */
#include "tinyusb.h"
#include "tusb.h"                                  /* tud_mounted(): USB DRIVE connected */
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "esp_private/usb_phy.h"
#include "font_small.h"
#include "font_menu.h"                             /* Press Start 2P, 8x8 bitmap (OFL); font8x8.h = the old thin font */
#include "esp_lcd_panel_io.h"
#include "rtc_pcf85063.h"
#include "battery_axp2101.h"
#include "esp_pm.h"
#include "esp_sleep.h"

static bool shared_time(uint32_t *secs);   /* board clock as local seconds since 1970 (false if unset) */
static int clock_minute(void);

static const char *TAG = "TamaNest";

/* ------------------------------------------------------------------------------------------------ */
/* Settings                                                                                          */
/* ------------------------------------------------------------------------------------------------ */

/* Hardware keys on the top edge, seen from the screen side: left = A, middle = B, right = C.
 * (The schematic's board drawing is the back view, so its left/right are mirrored.) */
#define KEY_A_GPIO 0    /* "BOOT" */
#define KEY_B_GPIO 16   /* "PWR": also the power button, so never use a long hold on B */
#define KEY_C_GPIO 18   /* "IO18" */
#define MENU_HOLD_MS 2000   /* in a game: C held this long = quick menu; C tap then C held = back to the ROM menu */
#define DOUBLE_TAP_MS 500   /* the tap before the hold must be shorter than this, and the gap too */

/* Start-up benchmark: run the first BENCH_CYCLES of the Plus Color ROM with idle skipping off and log
 * ESP cycles per guest cycle plus a fingerprint of the guest state (to catch behaviour changes). */
#define TL_BENCH 0
#define TL_EXPORT_TEST 0               /* ONE-OFF: copy the newest Plus Color export into roms/ at start-up (2026-11-22) */
#define TL_ROMRECV 0                   /* ONE-OFF: receive a ROM file from the computer over USB into /sdcard/roms (2026-11-22) */
#define TL_WIFITEST 0                  /* ONE-OFF: Wi-Fi scan at start-up + memory before/after (2026-11-22) */
#define TL_ROMXFER 0                   /* ONE-OFF: send ROMs over USB at start-up (2026-11-21) */
/* Guest code profiler: run in short, varying chunks and record where the guest CPU is (only while it is
 * awake), then print the busiest addresses and the code around them every PROF_REPORT_S seconds. */
#define TL_PROFILE 0
/* Print the game RAM over USB when a save is loaded (finding where a game keeps its date). */
#define TL_DIAG 0      /* 1 = extra 3 s log lines: screen updates, game drawing, game clock */
#define TL_RAMDUMP 0   /* 1 = print RAM when a save loads, 2 = also run the midnight test, 3 = RAM snapshot when the quick menu opens (SNAPSHOT items) */
#ifndef TL_DEVLOG               /* developer builds: idf.py -DTAMANEST_DEV=1 (main/CMakeLists.txt) */
#define TL_DEVLOG 0    /* 1 = developer logs: battery diary + power locks + games' flash saves on the SD card (printed at start-up), press traces */
#endif
#define PROF_REPORT_S 10
#define BENCH_CYCLES 5000000ULL
#define BENCH_HLE_MODE 1      /* compositor HLE during the benchmark: 0 off, 1 replace, 2 verify */
#define BENCH_HLE_MODE_8MB 1  /* same, for the cached 8 MB ROM (iD L / P's) */

/* Volume: buzzer wave amplitude (max 32767) and ES8311 codec output volume (0-100). The SOUND setting
 * scales the amplitude: OFF, 25, 50, 75, 100 % (100 % = these values). */
#define BUZZER_AMPLITUDE 10000   /* 2026-11-18: was 4000, too quiet outdoors */
#define CODEC_VOLUME     78       /* was 60 */
static int s_brightness = 40;                     /* screen brightness in percent (10..100) */
static int s_colour = 80;                         /* game colour strength (saturation) in percent, 50..100 */
static uint16_t *s_col_lut;                       /* RGB565 -> softened RGB565 (PSRAM, 128 KB), NULL = off */
static bool s_lcd_look = true;                    /* LCD SHADER setting */
/* DISPLAY: 0 AUTHENTIC (big areas appear row by row, like the real LCD), 1 BALANCED (each finished area
 * at once), 2 SMOOTH (complete screens: after a big area changed, wait until the game is idle again;
 * small things like sprites are shown at once, so busy arcade games stay smooth) */
static int s_draw_mode = 2;
static const char *const DRAW_MODE_NAME[3] = { "AUTHENTIC", "BALANCED", "SMOOTH" };
static volatile bool s_ready_big;                 /* a big window (2048+ pixels) finished since the last frame */
static int s_size_q = 12;                         /* SIZE: game scale in quarters: 12 = 3x (default), 13, 14, 15 = 3.75x (fullscreen) */
static int s_area, s_ax;                          /* game area width/height on the panel and its left/top edge */
static uint8_t s_srcx[480], s_edge[480];          /* panel pixel -> game pixel; 1 = last panel pixel of a game pixel (LCD grid) */
static uint16_t s_pstart[GRAM_W + 1];             /* first panel pixel of each game pixel */
static void size_tables(void)
{
    s_area = GRAM_W * s_size_q / 4; s_area &= ~1; s_ax = ((480 - s_area) / 2) & ~1;   /* 480 = SCR_W */
    for (int p = 0; p < s_area; p++) s_srcx[p] = (uint8_t)(p * GRAM_W / s_area);
    for (int p = 0; p < s_area; p++) s_edge[p] = (p + 1 == s_area) || s_srcx[p + 1] != s_srcx[p];
    for (int sx = 0, p = 0; sx <= GRAM_W; sx++) { while (p < s_area && s_srcx[p] < sx) p++; s_pstart[sx] = (uint16_t)p; }
}
static bool s_shell_on = true;                    /* SHELL: coloured border around the game */
static bool s_battery_on = true;                  /* BATTERY: indicator during play */
static uint16_t s_shell_rom_col;                  /* this ROM's shell colour */
static int s_loader = 1;                           /* loading animation: 0 ROLL (end over end), 1 SPIN (default, owner 2026-11-23); NVS settings/loader */
static int s_menu_bar = 2;                         /* launcher top bar: 0 plain, 1 light band, 2 dark band (default), 3 light band + line,
                                                    * 4 mid band with light text, 5 a line only */
static int s_rom_order;                            /* ROM list: 0 oldest first (default), 1 newest first, 2 A-Z */
static const bool s_heart_out = false;             /* status bars: heart inside the 4-bowl height (IN, owner's pick 2026-11-22) */
static bool s_bars_on = true;                     /* STATUS BARS: hunger bowls left, happiness gauge right (Plus Color) */
static volatile bool s_bars_redraw;
#define OSC3_4U 30000000u                          /* 4U / 4U+ crystal (Paramix-Studio: "the 4U runs at 30 MHz"); was 18.432 MHz
                                                    * like the iD L until 2026-11-22, so 4U+ ran at ~61% speed */
static bool s_hexa;                                /* Hexagontchi: no date copy / clock put-back (it writes its clock every
                                                    * 1.5 s; the put-back made the time jump), no status bars (own stats) */
static bool s_date_plus;                          /* the running game is Plus Color (date copy address, status bars) */
static uint32_t s_pet_shift;                       /* Hexagontchi: its pet block (save 0x84128) and date copy (0x84173) sit 4 bytes
                                                    * later than Plus Color's (0x84124 / 0x8416f) - from its save and date code */
static volatile int s_bar_hunger = -1, s_bar_happy = -1, s_bar_loc = -1;
static volatile int s_bar_game;                    /* status bars for: 0 none, 1 Plus Color, 2 P's, 3 iD L */
static volatile bool s_bars_wait_frame;            /* home <-> away: switch the bars with the first big screen update */
static volatile int s_bars_shown_loc = 0;          /* the location the bars currently follow (1 = away: hidden) */
static int64_t s_bars_wait_since;
static bool s_game_clock = true;                  /* GAME CLOCK: time in the top-left border during play */
static volatile bool s_clock_redraw;
/* SCREENSHOT countdown (owner 2026-11-23): quick menu -> 3, 2, 1 in the border clock -> picture -> camera + OK */
static volatile int64_t s_shot_at;                 /* when to take it (0 = no countdown) */
static volatile int s_shot_show;                   /* border clock shows: 0 the time, 1..3 countdown, -1 OK, -2 failed */
static volatile bool s_shot_req_grab, s_shot_grabbed;   /* ui -> display task: copy the picture; display -> ui: done */
static void shot_grab(void);
static bool shot_save(char *name, size_t n);
static volatile bool s_usb_drive;
static volatile int64_t s_ui_poke;                 /* last touch on the screen (counts as activity for menu sleep) */
static bool s_sd_ok;                               /* the SD card mounted (FAT32) */
static esp_err_t s_sd_err;                         /* why not: ESP_FAIL = a card that is not FAT32, else no card */
static bool s_game_restored;                       /* the running game came from a save (not a fresh start) */                 /* USB DRIVE mode: the computer owns the SD card - no SD writes, no menu sleep */
static bool s_flip = true;                        /* BUTTONS (default BOTTOM, owner 2026-11-22): false = buttons on top, true = bottom (screen turned 180) */
static volatile bool s_vlcd_direct;               /* menus drawn straight to the panel (quick menu over a running game) */
static uint16_t *s_grid_lut;                      /* same, for the gaps between LCD pixels */
static volatile int s_sound = 50;                  /* percent (OFF/10/25/50/75/100); default 50 (owner 2026-11-22) */
static volatile bool s_chime_req;                  /* play the Tamagotchi "select" chime (menu preview) */

#define SCR_W 480
#define SCR_H 480
#define GAME_SCALE 3                                   /* 128x128 game screen -> 384x384 */
#define GAME_X ((SCR_W - GRAM_W * GAME_SCALE) / 2)
#define GAME_Y ((SCR_H - GRAM_H * GAME_SCALE) / 2)
#define IC_COG      "\x01"   /* cog: ring with teeth, round hole */
#define IC_EGG      "\x02"   /* egg with a screen and a button */
#define IC_SUN      "\x03"   /* brightness: sun */
#define IC_SOUND    "\x04"   /* sound: speaker */
#define IC_SCREEN   "\x05"   /* screen: rounded display (B) */
#define IC_EXTRAS   "\x06"   /* extras: outlined star with curved sides (E7) */
#define IC_CLOCK    "\x07"   /* date/time, game clock */
#define IC_SIZE     "\x08"   /* size: corner arrows */
#define IC_SATUR    "\x09"   /* saturation: half-filled circle (A) */
#define IC_GRID     "\x0b"   /* LCD shader: pixel grid */
#define IC_ROWS     "\x0c"   /* display: drawing rows */
#define IC_ORIENT   "\x0e"   /* orientation: up/down (D) */
#define IC_HEART    "\x0f"   /* status bars: heart */
#define IC_SHELL    "\x10"   /* shell: frame (B) */
#define IC_BATT     "\x11"   /* battery: upright (C) */
#define IC_SPEED    "\x12"   /* game speed: fast forward */
#define IC_RESTART  "\x13"   /* restart ROM */
#define IC_EXPORT   "\x14"   /* export save */
#define IC_MENUBAR  "\x15"   /* menu bar: window with a top bar */
#define IC_ORDER    "\x16"   /* ROM order: sort */
#define IC_QUICK    "\x17"   /* quick menu: sliders */
#define IC_COIN     "\x18"   /* gotchi points: coin */
#define IC_GMARK    "\x19"   /* Gotchi Points mark (after a number) */
#define IC_TOOLS    "\x1a"   /* EZTOOLS: heart flower */
#define IC_HOME     "\x1b"   /* TamaNest (top menu): egg in a nest */
#define IC_QUIT     "\x1c"   /* save & quit: door with a knob */
#define IC_USB      "\x1d"   /* USB drive: USB trident */
#define IC_POWER    "\x1e"   /* power off: power symbol */
#define IC_UPDATE   "\x1f"   /* update: turning arrows */
#define IC_WIFI     "\x7f"   /* Wi-Fi: arcs (character 127; 1..31 are all used) */
#define IC_FAST     "\x81"   /* fast load: lightning bolt */
#define IC_ABOUT    "\x82"   /* about: i in a circle */
#define IC_DLC      "\x83"   /* DLC: gift box */
#define IC_ZONE     "\x84"   /* time zone: globe */
#define IC_CAMERA   "\x85"   /* screenshot: camera */
#define IC_KSHIFT   "\x86"   /* keyboard: shift (capital letters) */
#define IC_KSPACE   "\x87"   /* keyboard: space */
#define IC_KDEL     "\x88"   /* keyboard: delete */
#define IC_KEYE     "\x89"   /* keyboard: show / hide the password */
#define IC_XFER     "\x8a"   /* FILE TRANSFER: up and down arrows (owner 2026-11-24) */
#define IC_LOADING  "\x80"   /* loading animation: egg over a progress bar (characters from 128 on are icons too) */
#define VL 160                                          /* virtual LCD for menus: 160x160, scaled 3x */

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Menu palette (green LCD). Index: 0 background, 1 faded text, 2 dark text/lines, 3 highlight bar. */
static const uint16_t PAL[4] = {
    RGB565(0xA8, 0xD4, 0x7C),
    RGB565(0x80, 0xA8, 0x62),
    RGB565(0x1C, 0x2A, 0x14),
    RGB565(0x8E, 0xBA, 0x68),
};
/* Same colours, slightly darker: the thin gaps between LCD pixels */
static uint16_t PAL_GRID[4];
/* Extra colours for the virtual LCD (indices 4..): full-colour screens such as the low-battery screen.
 * They go through the same SATURATION / LCD SHADER tables as the games. */
enum { XN = 4, XW, XY, XLY, XDY, XPK, XBL, XLB, XRD, XGR, XLG, XOR, XCY, XST, XTN, XCE, XGN, XYEAR, XSHINE, XSPOTSH, XPAL_END };
static const uint16_t XPAL[XPAL_END - 4] = {
    RGB565(0, 0, 98), RGB565(255, 255, 255), RGB565(255, 226, 60), RGB565(255, 248, 170), RGB565(236, 170, 20),
    RGB565(255, 150, 190), RGB565(90, 170, 255), RGB565(200, 230, 255), RGB565(255, 70, 70), RGB565(150, 150, 160),
    RGB565(210, 210, 220), RGB565(255, 150, 30), RGB565(120, 220, 230), RGB565(255, 194, 255), RGB565(255, 230, 140),
    RGB565(0, 200, 210), RGB565(60, 190, 90),
    RGB565(0x46, 0x5e, 0x34),                      /* XYEAR: mid-dark menu green (year on the highlighted row) */
    RGB565(0xD2, 0xEC, 0xB0),                      /* XSHINE: loading egg's shine (lighter than the menu's lightest green) */
    RGB565(0x70, 0x96, 0x56) };                    /* XSPOTSH: loading egg's spots in its shadow */

/* ------------------------------------------------------------------------------------------------ */
/* State                                                                                             */
/* ------------------------------------------------------------------------------------------------ */

static Emu *s_emu = NULL;
static Emu *s_emu_buf = NULL;   /* allocated once at boot, in fast internal RAM */
static uint8_t *s_ovl_pool = NULL;
static volatile bool s_emu_running = false;
static volatile bool s_emu_task_alive = false;
static volatile uint8_t s_btn_mask = 0;          /* bit0 A, bit1 B, bit2 C, as the game sees them */
static esp_partition_mmap_handle_t s_mmap_handle = 0;
static bool s_rom_is_mmap = false;

static uint16_t *s_fb = NULL;                     /* the whole screen, 480x480 RGB565, in PSRAM */
static lv_image_dsc_t s_fb_dsc;
static lv_obj_t *s_fb_img = NULL;
static uint8_t *s_vlcd;                           /* virtual LCD for menus (palette indices), VL x VL, in PSRAM */

static esp_codec_dev_handle_t s_spk_dev = NULL;
static volatile uint32_t s_tone_hz = 0;

/* ------------------------------------------------------------------------------------------------ */
/* ROM names and device types                                                                       */
/* ------------------------------------------------------------------------------------------------ */

static void str_lower(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static bool name_has(const char *filename, const char *what)
{
    char low[128];
    str_lower(low, filename, sizeof low);
    return strstr(low, what) != NULL;
}


/* Friendly menu name for a ROM file, like the tamagotchi.vn launcher */
static void rom_name_guess(const char *file, char *out, size_t n)   /* (fallback for unknown ROMs) */
{
    if (name_has(file, "hexa"))                         snprintf(out, n, "HEXAGONTCHI");
    else if (name_has(file, "color") || name_has(file, "plus")) snprintf(out, n, "PLUS COLOR");
    else if (name_has(file, "4u"))                      snprintf(out, n, "TMGC 4U(+)");
    else if (name_has(file, "idl") || name_has(file, "id l") || name_has(file, "id_l")) snprintf(out, n, "TMGC iDL");
    else if (name_has(file, "ps") || name_has(file, "p's")) snprintf(out, n, "TMGC P's");
    else if (name_has(file, "id"))                      snprintf(out, n, "TMGC iD");
    else {
        size_t i = 0;
        for (; file[i] && file[i] != '.' && i + 1 < n; i++) out[i] = (char)toupper((unsigned char)file[i]);
        out[i] = 0;
    }
    if (strlen(out) > 19) out[19] = 0;      /* 20 characters fit on a line */
}

/* ---- Identifying a ROM by its content (2026-11-21) ----
 * Family from the start-up vector: Plus Color family ROMs start at 0xC0xxxx (the 4 MB ROM, often stored twice
 * in an 8 MB file), iD family ROMs at 0x024xxxxx (8 MB, code 4 MB in). Exact model from an FNV-1a fingerprint
 * of the 64 KB of code at that start (KNOWN_ROMS; add new dumps here). Unknown ROMs fall back to their family
 * and then to the file name. Everything per game (CPU profile, 4U NFC chip, name, year, shell colour, date
 * handling, status bars) follows the model. */
typedef enum { M_UNKNOWN = 0, M_PLUS, M_HEXA, M_ID, M_IDL, M_PS, M_4U, M_4UP, M_IDM, M_IDL15, M_IDLSP } RomModel;
typedef struct { RomModel m; int family; uint32_t fp, rv; bool known; } RomInfo;   /* family: 0 unknown, 1 Plus Color, 2 iD */
static const struct { RomModel m; const char *name, *year; } MODEL_INFO[] = {
    { M_UNKNOWN, "", "" },                { M_PLUS, "PLUS COLOR", "2008" },  { M_HEXA, "HEXAGONTCHI", "2009" },
    { M_ID, "TMGC iD", "2009" },          { M_IDL, "TMGC iDL", "2011" },     { M_PS, "TMGC P's", "2012" },
    { M_4U, "TMGC 4U", "2014" },          { M_4UP, "TMGC 4U+", "2015" },
    /* editions (grouped under their base model by rom_base) */
    { M_IDM, "iD LOVELY MELODY", "2010" }, { M_IDL15, "iDL 15TH ANNIV.", "2011" }, { M_IDLSP, "iDL PRINCESS SPACY", "2012" } };
/* Model by the game's reset vector (the first word of its vector table) - the same in every language and dump, so
 * translations are recognised too (2026-11-22; vectors from tamaemu's device map and Paramix Studio's tables). */
static const struct { uint32_t rv; RomModel m; } RV_ROMS[] = {
    { 0x00C2F0ECu, M_PLUS }, { 0x00C4215Eu, M_HEXA }, { 0x00C907FAu, M_ID }, { 0x00C939EAu, M_IDM },
    { 0x024CA8FEu, M_IDL }, { 0x024CA942u, M_IDL }, { 0x024CAA72u, M_IDL }, { 0x024BD18Au, M_IDL15 }, { 0x024C14F6u, M_IDLSP },
    { 0x024FDFE6u, M_PS }, { 0x024C680Au, M_4U }, { 0x024F6256u, M_4UP } };
static const struct { uint32_t fp; RomModel m; const char *what; } KNOWN_ROMS[] = {
    { 0xf15b2166u, M_PLUS, "Plus Color (English)" },
    { 0xa7355d44u, M_HEXA, "Hexagontchi (English)" },
    { 0x7827443bu, M_IDL,  "iD L (English)" },
    { 0x0add38edu, M_PS,   "P's (English)" },
    { 0xdbdde4d4u, M_4UP,  "4U+ (English)" },
};
static RomModel model_from_name(const char *file, int family)
{
    if (family != 2 && name_has(file, "hexa")) return M_HEXA;
    if (family != 2 && (name_has(file, "color") || name_has(file, "plus"))) return M_PLUS;
    if (family == 1) return M_PLUS;
    if (name_has(file, "4u+") || name_has(file, "4uplus") || name_has(file, "4u_plus")) return M_4UP;
    if (name_has(file, "4u")) return M_4U;
    if (name_has(file, "idl") || name_has(file, "id l") || name_has(file, "id_l")) return M_IDL;
    if (name_has(file, "ps") || name_has(file, "p's")) return M_PS;
    if (name_has(file, "id")) return M_ID;
    return M_UNKNOWN;
}
static void rom_identify(const char *file, RomInfo *ri)
{
    memset(ri, 0, sizeof *ri);
    char path[160]; snprintf(path, sizeof path, "/sdcard/roms/%s", file);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long size = ftell(f);
        uint32_t v0 = 0, v4 = 0;
        fseek(f, 0, SEEK_SET); if (fread(&v0, 4, 1, f) != 1) v0 = 0;
        if (size >= 0x400004) { fseek(f, 0x400000, SEEK_SET); if (fread(&v4, 4, 1, f) != 1) v4 = 0; }
        long code = -1;
        if ((v0 & 0xFFC00000u) == 0x00C00000u) { ri->family = 1; code = 0; }
        else if (size >= 0x800000 && (v4 & 0xFF000000u) == 0x02000000u) { ri->family = 2; code = 0x400000; }
        if (code >= 0) {
            uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
            uint32_t h = 2166136261u;
            fseek(f, code, SEEK_SET);
            for (int k = 0; buf && k < 16; k++) {
                size_t n = fread(buf, 1, 4096, f);
                for (size_t j = 0; j < n; j++) h = (h ^ buf[j]) * 16777619u;
            }
            free(buf);
            ri->fp = h;
            for (size_t i = 0; i < sizeof KNOWN_ROMS / sizeof KNOWN_ROMS[0]; i++)
                if (KNOWN_ROMS[i].fp == h) { ri->m = KNOWN_ROMS[i].m; ri->known = true; }
            ri->rv = ri->family == 1 ? v0 : v4;
            for (size_t i = 0; i < sizeof RV_ROMS / sizeof RV_ROMS[0]; i++)
                if (RV_ROMS[i].rv == ri->rv) { ri->m = RV_ROMS[i].m; ri->known = true; }
        }
        fclose(f);
    }
    if (!ri->known) {
        ri->m = model_from_name(file, ri->family);
        if (ri->family == 0) ri->family = (ri->m == M_PLUS || ri->m == M_HEXA) ? 1 : 2;   /* no file: as before */
    }
    ESP_LOGI(TAG, "ROM %s: %s (%s layout, start %08lx, fingerprint %08lx%s)", file, MODEL_INFO[ri->m].name[0] ? MODEL_INFO[ri->m].name : "unknown model",
             ri->family == 1 ? "4 MB" : "8 MB", (unsigned long)ri->rv, (unsigned long)ri->fp, ri->known ? ", known" : ", not in the table");
}
#define MAX_ROMS_ID 32
static char s_ri_file[MAX_ROMS_ID][64];
static RomInfo s_ri[MAX_ROMS_ID];
static int s_ri_n;
static const RomInfo *rom_info(const char *file)          /* cached per file name */
{
    for (int i = 0; i < s_ri_n; i++) if (!strcmp(s_ri_file[i], file)) return &s_ri[i];
    int k = s_ri_n < MAX_ROMS_ID ? s_ri_n++ : MAX_ROMS_ID - 1;
    snprintf(s_ri_file[k], sizeof s_ri_file[k], "%s", file);
    rom_identify(file, &s_ri[k]);
    return &s_ri[k];
}
/* Plus Color family = Plus Color and its editions. iD and iD Melody share their 4 MB memory layout (family 1)
 * but are iD-family games: their own profile (rom_profile). */
static bool rom_is_idc(const char *file) { RomModel m = rom_info(file)->m; return m == M_ID || m == M_IDM; }
static bool rom_is_plus(const char *file) { return rom_info(file)->family == 1 && !rom_is_idc(file); }
static bool rom_is_4u(const char *file) { RomModel m = rom_info(file)->m; return m == M_4U || m == M_4UP; }
static void rom_profile(const char *file, DeviceProfile *d)   /* the device profile for a ROM */
{
    if (rom_is_idc(file)) tc_profile_id(d, rom_info(file)->m == M_IDM);
    else if (rom_is_plus(file)) {
        tc_profile_plus_color(d);
        /* Hexagontchi reads the debug strap (port P06) the other way round: with Plus Color's setting it started
         * in its debug/test mode (character select instead of the egg, clock settable). tamaemu's profile
         * "plus-color-hexa"; owner's tests 2026-11-22. */
        if (rom_info(file)->m == M_HEXA) d->debug_strap_high = false;
    } else {
        tc_profile_idl(d);
        if (rom_is_4u(file)) { d->nfc_pn512 = true; d->osc3_hz = OSC3_4U; }
    }
}
/* Base model for grouping the ROM list (2026-11-22): editions sit under their base model (Hexagontchi is a Plus
 * Color edition; later iD L 15th / Princess Spacy under iD L, etc.). Unknown ROMs are never grouped. */
static RomModel rom_base(const char *file)
{
    RomModel m = rom_info(file)->m;
    return m == M_HEXA ? M_PLUS : m == M_IDM ? M_ID : (m == M_IDL15 || m == M_IDLSP) ? M_IDL : m;
}
static void rom_display_name(const char *file, char *out, size_t n)
{
    const RomInfo *ri = rom_info(file);
    if (ri->m != M_UNKNOWN && name_has(file, "export")) snprintf(out, n, "%s EXP", MODEL_INFO[ri->m].name);   /* an imported export */
    else if (ri->m != M_UNKNOWN) snprintf(out, n, "%s", MODEL_INFO[ri->m].name);
    else rom_name_guess(file, out, n);
}

/* ------------------------------------------------------------------------------------------------ */
/* Screen drawing                                                                                    */
/* ------------------------------------------------------------------------------------------------ */

static void screen_flush(void)
{
    lv_obj_invalidate(s_fb_img);
}

static void vlcd_clear(uint8_t c) { memset(s_vlcd, c, VL * VL); }

static void vlcd_rect(int x, int y, int w, int h, uint8_t c)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= VL) continue;
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && xx < VL) s_vlcd[yy * VL + xx] = c;
    }
}

static void vlcd_frame(int x, int y, int w, int h, uint8_t c)
{
    vlcd_rect(x, y, w, 1, c); vlcd_rect(x, y + h - 1, w, 1, c);
    vlcd_rect(x, y, 1, h, c); vlcd_rect(x + w - 1, y, 1, h, c);
}

static void vlcd_text(int x, int y, const char *s, uint8_t c)
{
    /* little icons for menu items (8x8), as control characters - see the IC_ names (2026-11-22) */
    static const uint8_t ICONS[41][8] = {
        { 0x18, 0x7E, 0x66, 0xC3, 0xC3, 0x66, 0x7E, 0x18 },   /* \x01 cog: ring with teeth, round hole */
        { 0x3C, 0x42, 0xBD, 0xA5, 0xBD, 0x81, 0x5A, 0x3C },   /* \x02 egg with a screen and a button */
        { 0x18, 0x42, 0x3C, 0xBD, 0xBD, 0x3C, 0x42, 0x18 },   /* \x03 brightness: sun */
        { 0x12, 0x31, 0xF5, 0xF5, 0xF5, 0x31, 0x12, 0x00 },   /* \x04 sound: speaker */
        { 0x7E, 0x81, 0xB1, 0xA1, 0x81, 0x81, 0x7E, 0x00 },   /* \x05 screen: rounded display (B) */
        { 0x10, 0x10, 0x28, 0xC6, 0x28, 0x10, 0x10, 0x00 },   /* \x06 extras: outlined star with curved sides (E7, owner 2026-11-22) */
        { 0x3C, 0x52, 0x91, 0x9D, 0x81, 0x42, 0x3C, 0x00 },   /* \x07 date/time, game clock */
        { 0xE7, 0xC3, 0xA5, 0x00, 0x00, 0xA5, 0xC3, 0xE7 },   /* \x08 size: corner arrows */
        { 0x3C, 0x4E, 0x8F, 0x8F, 0x8F, 0x8F, 0x4E, 0x3C },   /* \x09 saturation: half-filled circle (A) */
        { 0xDB, 0xDB, 0x00, 0xDB, 0xDB, 0x00, 0xDB, 0xDB },   /* \x0b LCD shader: pixel grid */
        { 0xFF, 0x00, 0xFC, 0x00, 0xF0, 0x00, 0xC0, 0x00 },   /* \x0c display: drawing rows */
        { 0x10, 0x38, 0x7C, 0x00, 0x00, 0x7C, 0x38, 0x10 },   /* \x0e orientation: up/down (D) */
        { 0x6C, 0xFE, 0xFE, 0x7C, 0x38, 0x10, 0x00, 0x00 },   /* \x0f status bars: heart */
        { 0xFF, 0xC3, 0x81, 0x81, 0x81, 0x81, 0xC3, 0xFF },   /* \x10 shell: frame (B) */
        { 0x38, 0x7C, 0x44, 0x7C, 0x7C, 0x7C, 0x7C, 0x00 },   /* \x11 battery: upright (C) */
        { 0x00, 0x88, 0xCC, 0xEE, 0xCC, 0x88, 0x00, 0x00 },   /* \x12 game speed: fast forward */
        { 0x3A, 0x46, 0x8E, 0x80, 0x81, 0x42, 0x3C, 0x00 },   /* \x13 restart ROM */
        { 0x10, 0x10, 0x10, 0x7C, 0x38, 0x10, 0x82, 0xFE },   /* \x14 export save */
        { 0xFF, 0xFF, 0x81, 0xBD, 0x81, 0xB9, 0x81, 0xFF },   /* \x15 menu bar: window with a top bar */
        { 0x5E, 0x40, 0x5C, 0x40, 0xEC, 0x40, 0x00, 0x00 },   /* \x16 ROM order: sort */
        { 0x00, 0x60, 0xFE, 0x60, 0x0C, 0xFE, 0x0C, 0x00 },   /* \x17 quick menu: two sliders */
        { 0x3C, 0x42, 0x99, 0x99, 0x99, 0x99, 0x42, 0x3C },   /* \x18 gotchi points: coin */
        { 0x08, 0x3E, 0x60, 0xCE, 0xC6, 0x66, 0x3E, 0x08 },   /* \x19 Gotchi Points mark: G with ticks above and below (our own, M3) */
        { 0x6C, 0xFE, 0x7C, 0x38, 0x10, 0xD6, 0x7C, 0x38 },   /* \x1a EZTOOLS: heart flower (T8, owner 2026-11-22) */
        { 0x18, 0x2C, 0x34, 0xBD, 0x81, 0x7E, 0x3C, 0x00 },   /* \x1b TamaNest (top menu): cracked egg in a nest (N6, for now) */
        { 0x7C, 0x44, 0x44, 0x4C, 0x44, 0x44, 0xFE, 0x00 },   /* \x1c save & quit: door with a knob (D7, owner 2026-11-22) */
        { 0x10, 0x38, 0x93, 0x93, 0x54, 0x38, 0x10, 0x38 },   /* \x1d USB drive: USB trident */
        { 0x10, 0x54, 0x92, 0x82, 0x82, 0x44, 0x38, 0x00 },   /* \x1e power off: power symbol (Q5) */
        { 0x3C, 0x43, 0x87, 0x80, 0x01, 0xE1, 0xC2, 0x3C },   /* \x1f update: two turning arrows */
        { 0x7E, 0x81, 0x3C, 0x42, 0x18, 0x24, 0x00, 0x18 },   /* \x7f Wi-Fi: arcs over a dot (no control characters left) */
        { 0x18, 0x24, 0x42, 0x42, 0x42, 0x3C, 0x00, 0xF5 },   /* \x80 loading: egg over a progress bar */
        { 0x0C, 0x18, 0x30, 0x7E, 0x0C, 0x18, 0x30, 0x20 },   /* \x81 fast load: lightning bolt */
        { 0x3C, 0x5A, 0xFF, 0xE7, 0xE7, 0xE7, 0x7E, 0x3C },   /* \x82 about: i in a circle */
        { 0x66, 0x3C, 0xFF, 0x99, 0xFF, 0x99, 0x99, 0xFF },   /* \x83 DLC: gift box with a bow */
        { 0x3C, 0x5A, 0xA5, 0xFF, 0xA5, 0x5A, 0x3C, 0x00 },   /* \x84 time zone: globe, curved meridians (G1) */
        { 0x00, 0x38, 0xFF, 0xE7, 0xDB, 0xE7, 0xFF, 0x00 },   /* \x85 screenshot: camera with a lens */
        { 0x18, 0x3C, 0x7E, 0xFF, 0x3C, 0x3C, 0x3C, 0x00 },   /* \x86 keyboard: shift arrow */
        { 0x00, 0x00, 0x00, 0x00, 0x81, 0x81, 0xFF, 0x00 },   /* \x87 keyboard: space */
        { 0x10, 0x30, 0x7F, 0xFF, 0x7F, 0x30, 0x10, 0x00 },   /* \x88 keyboard: delete (left arrow) */
        { 0x00, 0x3C, 0x42, 0x99, 0x99, 0x42, 0x3C, 0x00 },   /* \x89 keyboard: eye */
        { 0x20, 0x74, 0x24, 0x24, 0x24, 0x24, 0x2E, 0x04 },   /* \x8a file transfer: up and down arrows */
    };
    for (; *s; s++, x += 8) {
        unsigned ch = (unsigned char)*s;
        const uint8_t *g;
        int ic = ch >= 1 && ch < 10 ? (int)ch - 1 : ch >= 11 && ch < 13 ? (int)ch - 2 : ch >= 14 && ch < 32 ? (int)ch - 3 : ch == 127 ? 29 : ch == 128 ? 30 : ch == 129 ? 31 : ch == 130 ? 32 : ch == 131 ? 33 : ch == 132 ? 34 : ch == 133 ? 35 : ch >= 134 && ch <= 138 ? (int)ch - 98 : -1;
        if (ic >= 0 && ic < (int)(sizeof ICONS / sizeof ICONS[0])) g = ICONS[ic];
        else { if (ch < 32 || ch > 127) ch = '?'; g = font_menu[ch - 32]; }
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b)) {
                    int xx = x + b, yy = y + r;
                    if (xx >= 0 && xx < VL && yy >= 0 && yy < VL) s_vlcd[yy * VL + xx] = c;
                }
    }
}

/* Small 5-pixel capitals (font_small.h, Silkscreen) for subtitles; returns the width. */
static int vlcd_text_small(int x, int y, const char *s, uint8_t c, bool measure)
{
    int w = 0;
    for (; *s; s++) {
        unsigned ch = (unsigned char)toupper((unsigned char)*s);
        if (ch < 32 || ch > 95) ch = '?';
        const uint8_t *r = font_small[ch - 32].r;
        if (!measure)
            for (int y2 = 0; y2 < FONT_SMALL_H; y2++)
                for (int b = 0; b < 8; b++)
                    if (r[y2] & (0x80 >> b)) vlcd_rect(x + w + b, y + y2, 1, 1, c);
        w += font_small[ch - 32].w;
    }
    return w;
}
static void vlcd_text_small_center(int y, const char *s, uint8_t c)
{
    vlcd_text_small((VL - vlcd_text_small(0, 0, s, c, true)) / 2, y, s, c, false);
}

static void vlcd_text_center(int y, const char *s, uint8_t c)
{
    vlcd_text((VL - (int)strlen(s) * 8) / 2, y, s, c);
}

/* Power saving (2026-11-18): the CPU runs at 80 MHz unless someone holds a "max speed" lock: the emulator
 * while it runs guest code, the display task while it sends a frame, and the menus (ui lock). Nothing
 * that matters runs slower; the idle time (most of it) costs less. */
static esp_pm_lock_handle_t s_pm_emu, s_pm_disp, s_pm_ui;
/* Light sleep (2026-11-19): allowed only while the pet sleeps (the game switched its LCD off). Then the
 * LVGL worker and its 1 ms tick are paused, the emulator naps until the game's next timer event (at most
 * the next clock second), the keys and audio check less often, and the chip light-sleeps in between
 * (FreeRTOS tickless idle). Buttons wake it (GPIO wakeup). Not while USB is connected (keeps the log). */
static esp_pm_lock_handle_t s_pm_awake;           /* NO_LIGHT_SLEEP: held except while the pet sleeps */
static volatile bool s_pet_asleep;
static TaskHandle_t s_ui_task_h, s_emu_task_h, s_audio_task_h;
#define WAKE_PULSE_MS 2                            /* length of the press the game gets while its screen is off (wake only) */
static int64_t s_btn_defer_t;
static uint8_t s_btn_hide, s_pulse_keys;           /* emu task: keys hidden after a wake blip; keys in a blip */
static uint64_t s_pulse_end;                      /* emu task: a button change waiting for the catch-up */
static volatile uint32_t s_nap_n, s_nap_ms;       /* emulator naps while asleep (statistics) */
static void low_power(bool on);
#define GAME_SPEED_MAX 64                          /* "MAX": more than the board can keep up with, so it runs flat out (the 2 s catch-up limit) */
static volatile int s_time_warp = 1;               /* GAME SPEED (EXTRAS, in a game): game time runs this many times faster; reset on game start */
static void screen_menu(bool in_game);
static void extras_menu(bool in_game);
static bool s_pm_on[3];                           /* held or not (the locks count, so never take one twice) */
static inline void pm_hold(esp_pm_lock_handle_t l, bool on)
{
    if (!l) return;
    int i = l == s_pm_emu ? 0 : l == s_pm_disp ? 1 : 2;
    if (on == s_pm_on[i]) return;
    s_pm_on[i] = on;
    if (on) esp_pm_lock_acquire(l); else esp_pm_lock_release(l);
}
static void pm_init(void)
{
    esp_pm_config_t c = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = true };
    esp_err_t err = esp_pm_configure(&c);
    if (err != ESP_OK) { c.light_sleep_enable = false; err = esp_pm_configure(&c); }
    else {
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "awake", &s_pm_awake);
        if (s_pm_awake) esp_pm_lock_acquire(s_pm_awake);
    }
    if (err == ESP_OK) {
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "emu", &s_pm_emu);
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "disp", &s_pm_disp);
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui", &s_pm_ui);
        pm_hold(s_pm_ui, true);                   /* menus at full speed */
    }
    ESP_LOGI(TAG, "Power management (240/80 MHz, light sleep %s): %s", s_pm_awake ? "while the pet sleeps" : "off", esp_err_to_name(err));
}

static void vlcd_panel_present(const uint16_t *pal, const uint16_t *grid);
/* A line of menu-font text drawn at full panel resolution, 4 panel pixels per font pixel (menu text is 3, double
 * size 6): the loading screen's title (2026-11-22). 0 = no text here, 1 = ink, 2 = grid line of an ink pixel. */
static const char *s_ov_text;
static int s_ov_y;                                 /* panel y of its top */
static int s_ov_x = -1;                            /* panel x of its left edge, -1 = centred (TamaNest title: left) */
static inline int ov_pixel(int X, int Y)
{
    /* each glyph is 7 font pixels + 1 blank column, so advance 8 (was 7: wide letters touched, owner 2026-11-22);
     * long lines drop to 3 panel pixels per font pixel so they stay inside the screen */
    int n = (int)strlen(s_ov_text);
    const int sc = n * 32 - 4 <= 440 ? 4 : 3, adv = 8 * sc;
    int x0 = s_ov_x >= 0 ? s_ov_x : (SCR_W - (n * adv - sc)) / 2;
    if (Y < s_ov_y || Y >= s_ov_y + 8 * sc || X < x0 || X >= x0 + n * adv) return 0;
    int k = (X - x0) / adv, cx = ((X - x0) % adv) / sc, cy = (Y - s_ov_y) / sc;
    unsigned ch = (unsigned char)s_ov_text[k];
    if (ch < 32 || ch > 127) return 0;
    if (!(font_menu[ch - 32][cy] & (0x80 >> cx))) return 0;
    return ((X - x0) % sc == sc - 1 || (Y - s_ov_y) % sc == sc - 1) ? 2 : 1;
}
/* Scale the virtual LCD 3x onto the screen. Each LCD pixel becomes a 3x3 block whose right column and
 * bottom row are a touch darker, which gives the fine pixel grid of a real LCD. */
static void vlcd_present(void)
{
    /* with LCD LOOK / COLOUR on, the menu goes through the same colour tables as the games */
    uint16_t pal[XPAL_END], grid[XPAL_END];
    const bool fx = (s_colour < 100 || s_lcd_look) && s_col_lut && s_grid_lut;
    for (int i = 0; i < 4; i++) {
        pal[i] = fx ? s_col_lut[PAL[i]] : PAL[i];
        grid[i] = fx && s_lcd_look ? s_grid_lut[PAL[i]] : fx ? s_col_lut[PAL_GRID[i]] : PAL_GRID[i];
    }
    for (int i = 4; i < XPAL_END; i++) {           /* extra colours; grid line 7/8 like PAL_GRID */
        uint16_t c = XPAL[i - 4];
        uint16_t d = (uint16_t)(((((c >> 11) & 31) * 7 / 8) << 11) | ((((c >> 5) & 63) * 7 / 8) << 5) | ((c & 31) * 7 / 8));
        pal[i] = fx ? s_col_lut[c] : c;
        grid[i] = fx && s_lcd_look ? s_grid_lut[c] : fx ? s_col_lut[d] : d;
    }
    if (s_vlcd_direct) { vlcd_panel_present(pal, grid); return; }
    bsp_display_lock(-1);
    for (int vy = 0; vy < VL; vy++) {
        uint16_t *r0 = s_fb + (vy * 3) * SCR_W, *r1 = r0 + SCR_W, *r2 = r1 + SCR_W;
        const uint8_t *src = s_vlcd + vy * VL;
        for (int vx = 0; vx < VL; vx++) {
            uint16_t c = pal[src[vx]], g = grid[src[vx]];
            int x = vx * 3;
            r0[x] = c; r0[x + 1] = c; r0[x + 2] = g;
            r1[x] = c; r1[x + 1] = c; r1[x + 2] = g;
            r2[x] = g; r2[x + 1] = g; r2[x + 2] = g;
        }
    }
    if (s_ov_text)                                 /* full-resolution title line */
        for (int Y = s_ov_y; Y < s_ov_y + 32 && Y < SCR_H; Y++)
            for (int X = 0; X < SCR_W; X++) { int o = ov_pixel(X, Y); if (o) s_fb[Y * SCR_W + X] = o == 1 ? pal[2] : grid[2]; }
    screen_flush();
    bsp_display_unlock();
}

/* ---- Game screen: drawn straight to the panel, bypassing LVGL ("dummy draw" mode of the LVGL adapter).
 * Only the rectangle that changed since the last frame is sent, scaled 3x, in small strips from an internal
 * DMA buffer. Much less data than letting LVGL redraw the whole 384x384 area through PSRAM. */
#define STRIP_BYTES 12288
static lv_display_t *s_disp;
static uint16_t *s_strip;                         /* internal, DMA capable */
static uint16_t (*s_shown)[GRAM_W];               /* last picture sent to the panel (PSRAM) */
/* Finished picture: the game's screen memory copied only when the game finishes writing a window, so a
 * half-written window is never shown (Plus Color writes its windows slowly, pixel by pixel). */
static uint16_t (*s_ready)[GRAM_W];
static SemaphoreHandle_t s_ready_mtx;
static volatile bool s_ready_new;
static volatile int64_t s_ready_t;                /* time of the last finished window */
static uint16_t (*s_gram)[GRAM_W];
static bool s_show_all;

/* The menu's virtual LCD sent straight to the panel (while a game owns the screen), 8 panel rows at a time. */
static void vlcd_panel_present(const uint16_t *pal, const uint16_t *grid)
{
    for (int Y = 0; Y < SCR_H; Y += 12) {
        uint16_t *o = s_strip;
        for (int yy = Y; yy < Y + 12; yy++) {
            const uint8_t *src = s_vlcd + (yy / 3) * VL;
            bool grow = (yy % 3) == 2;
            for (int vx = 0; vx < VL; vx++) {
                uint16_t c = pal[src[vx]], g = grid[src[vx]];
                c = (uint16_t)((c >> 8) | (c << 8)); g = (uint16_t)((g >> 8) | (g << 8));
                *o++ = grow ? g : c; *o++ = grow ? g : c; *o++ = g;
            }
            if (s_ov_text && yy >= s_ov_y && yy < s_ov_y + 32) {   /* full-resolution title line */
                uint16_t *row = o - SCR_W, ci = (uint16_t)((pal[2] >> 8) | (pal[2] << 8)), gi = (uint16_t)((grid[2] >> 8) | (grid[2] << 8));
                for (int X = 0; X < SCR_W; X++) { int ov = ov_pixel(X, yy); if (ov) row[X] = ov == 1 ? ci : gi; }
            }
        }
        esp_lv_adapter_dummy_draw_blit(s_disp, 0, Y, SCR_W, Y + 12, s_strip, true);
    }
}

static void panel_fill_black(int x0, int y0, int x1, int y1)   /* [x0,x1) x [y0,y1), even coordinates */
{
    int w = x1 - x0, rows = STRIP_BYTES / (w * 2);
    rows &= ~1;
    memset(s_strip, 0, (size_t)w * rows * 2);
    for (int y = y0; y < y1; y += rows) {
        int ye = y + rows < y1 ? y + rows : y1;
        esp_lv_adapter_dummy_draw_blit(s_disp, x0, y, x1, ye, s_strip, true);
    }
}

/* The AMOLED panel is far more vivid than the real toys' LCDs.
 * COLOUR: blend each colour towards its own grey (luma) by s_colour percent (100 = untouched).
 * LCD LOOK: like a backlit colour LCD: black glows a little (cool dark grey), white is a touch dimmer and
 * warmer, and a faint darker line between the pixels (the LCD's pixel grid; s_grid_lut).
 * Two lookup tables of all 65536 colours, rebuilt when a setting changes. */
static inline bool colour_fx_on(void) { return s_colour < 100 || s_lcd_look; }

static void colour_lut_build(void)
{
    if (!colour_fx_on()) return;
    if (!s_col_lut) s_col_lut = heap_caps_malloc(65536 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_grid_lut) s_grid_lut = heap_caps_malloc(65536 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_col_lut || !s_grid_lut) return;
    const int k = s_colour;                        /* percent */
    /* levels: black -> lo, white -> hi (per channel, 0..255) */
    const int lo[3] = { 18, 20, 26 }, hi[3] = { 244, 240, 230 };
    for (uint32_t c = 0; c < 65536; c++) {
        int v[3] = { (int)((c >> 11) & 31) * 255 / 31, (int)((c >> 5) & 63) * 255 / 63, (int)(c & 31) * 255 / 31 };
        int y = (v[0] * 77 + v[1] * 150 + v[2] * 29) >> 8;
        int gv[3];
        for (int i = 0; i < 3; i++) {
            v[i] = y + (v[i] - y) * k / 100;
            if (s_lcd_look) v[i] = lo[i] + v[i] * (hi[i] - lo[i]) / 255;
            gv[i] = s_lcd_look ? v[i] * (s_size_q == 12 ? 13 : 15) / 16 : v[i];   /* the gap between LCD pixels: a little darker (softer at the larger sizes, where pixels are 3 or 4 wide) */
        }
        s_col_lut[c] = (uint16_t)(((v[0] * 31 + 127) / 255) << 11 | ((v[1] * 63 + 127) / 255) << 5 | ((v[2] * 31 + 127) / 255));
        s_grid_lut[c] = (uint16_t)(((gv[0] * 31 + 127) / 255) << 11 | ((gv[1] * 63 + 127) / 255) << 5 | ((gv[2] * 31 + 127) / 255));
    }
}

/* ---- Battery indicator. Icon: 17x10 pixels, a battery outline with up to 3 bars.
 * In a game: icon only, chunky 3x3 pixels in the free space above the game, top right.
 * In the launcher: percentage + icon in the LCD colours, top right of the virtual LCD. */
#define BAT_VW 28                                 /* in-game icon size in icon pixels (3x3 on the panel) */
#define BAT_VH 12
#define BAT_X 360                                 /* panel position of the area: bolt, then the icon at 384 = menu LCD x 128 */
#define BAT_Y 6                                   /* same place in menu and game (virtual LCD y 2) */
static volatile int s_bat_pct = -2;               /* -2 = not read yet, -1 = no battery */
static volatile bool s_bat_chg, s_bat_redraw;
static bool s_lcd_off_shown;                      /* the game has switched its LCD off; the whole panel is dark */
static uint16_t s_bezel_col;                      /* shell colour around the game, 0 = none */
static int64_t s_bat_time;

/* Battery diary (2026-11-21): every 5 minutes a line in /sdcard/batlog.csv - date/time from the board clock,
 * %, mV, charging, menu/game, the share of the 5 minutes the game's screen was off (pet asleep or idle), the
 * emulator's naps - so unplugged runs can be measured (the serial log only works while plugged in). */
static char s_game_file[64];                       /* ROM file of the running game */
/* Power-mode times from the ESP-IDF profiler (CONFIG_PM_PROFILING): cumulative microseconds in SLEEP (light
 * sleep), APB_MIN (idle, 80 MHz), APB_MAX, CPU_MAX (240 MHz), plus light-sleep counts. */
typedef struct { long long t[4]; long cnt, rej; } PmStats;
static const char *s_pm_dump;                      /* text of the last profiler dump (locks + modes) */
static bool pm_stats_read(PmStats *o)
{
    static char buf[4096];
    memset(o, 0, sizeof *o);
    FILE *m = fmemopen(buf, sizeof buf - 1, "w");
    if (!m) return false;
    esp_pm_dump_locks(m);
    long len = ftell(m);
    fclose(m);
    buf[len > 0 && len < (long)sizeof buf ? len : 0] = 0;
    s_pm_dump = buf;
    static const char *const NM[4] = { "SLEEP", "APB_MIN", "APB_MAX", "CPU_MAX" };
    for (char *l = buf; l && *l; ) {
        char *nl = strchr(l, '\n'); if (nl) *nl = 0;
        char name[16]; unsigned mhz; long long us;
        if (sscanf(l, "%15s %u M %lld", name, &mhz, &us) == 3)   /* "SLEEP     80 M  123" / "CPU_MAX   240M  123" */
            for (int k = 0; k < 4; k++) if (!strcmp(name, NM[k])) o->t[k] = us;
        sscanf(l, "light_sleep_counts:%ld light_sleep_reject_counts:%ld", &o->cnt, &o->rej);
        if (nl) *nl = '\n';                       /* keep the text whole (s_pm_dump is saved/printed later) */
        l = nl ? nl + 1 : NULL;
    }
    return true;
}

static void batlog_tick(int64_t now)
{
    if (s_usb_drive) return;                       /* the computer owns the SD card */
    static int64_t prev, last_write, off_us, tot_us;
    static uint32_t naps0;
    if (prev) {
        int64_t dt = now - prev;
        tot_us += dt;
        if (s_emu_running && s_lcd_off_shown) off_us += dt;
    }
    prev = now;
    if (!last_write) { last_write = now; return; }
    if (now - last_write < 300000000) return;
    last_write = now;
    int pct; bool chg;
    if (!battery_read(&pct, &chg)) return;
    rtc_time_t t; bool lost = false;
    if (!rtc_read(&t, &lost)) memset(&t, 0, sizeof t);
    FILE *f = fopen("/sdcard/logs/batlog.csv", "a");
    if (!f) return;
    static PmStats pm0; static bool pm0_ok;
    PmStats pm; bool pm_ok = pm_stats_read(&pm);
    int pp[4] = { -1, -1, -1, -1 }; long sl = -1, rj = -1;
    if (pm_ok && pm0_ok) {
        long long tot = 0, d[4];
        for (int k = 0; k < 4; k++) { d[k] = pm.t[k] - pm0.t[k]; tot += d[k]; }
        for (int k = 0; k < 4 && tot > 0; k++) pp[k] = (int)(d[k] * 100 / tot);
        sl = pm.cnt - pm0.cnt; rj = pm.rej - pm0.rej;
    }
    if (pm_ok) { pm0 = pm; pm0_ok = true; }
    if (pm_ok && s_pm_dump && tot_us && off_us * 100 / tot_us >= 90) {   /* pet asleep: which locks keep the chip awake? */
        FILE *g = fopen("/sdcard/logs/pmlocks.txt", "w");
        if (g) { fprintf(g, "%04d-%02d-%02d %02d:%02d\n%s", t.year, t.month, t.day, t.hour, t.minute, s_pm_dump); fclose(g); }
    }
    if (ftell(f) == 0) fprintf(f, "date,time,percent,mV,charging,where,screen_off_percent,naps,rom,asleep_pct,idle80_pct,apbmax_pct,cpu240_pct,sleeps,sleep_refused\n");
    fprintf(f, "%04d-%02d-%02d,%02d:%02d,%d,%d,%s,%s,%d,%lu,%s,%d,%d,%d,%d,%ld,%ld\n", t.year, t.month, t.day, t.hour, t.minute, pct, battery_mv(),
            chg ? "yes" : "no", s_emu_running ? "game" : "menu", tot_us ? (int)(off_us * 100 / tot_us) : 0,
            (unsigned long)(s_nap_n >= naps0 ? s_nap_n - naps0 : s_nap_n), s_emu_running ? s_game_file : "-",
            pp[0], pp[1], pp[2], pp[3], sl, rj);
    fclose(f);
    naps0 = s_nap_n; off_us = tot_us = 0;
}

/* Read the power chip at most every 10 s. Returns true if the value changed. */
static bool battery_update(void)
{
    int64_t now = esp_timer_get_time();
#if TL_DEVLOG
    batlog_tick(now);
#endif
    {   /* once a minute: a line for measuring battery drain (minutes since boot, %, mV, charging, screen) */
        static int64_t last_log;
        if (now - last_log >= 60000000) {
            last_log = now;
            int p2; bool c2;
            if (battery_read(&p2, &c2))
                ESP_LOGI(TAG, "BATLOG %lld min | %d%% | %d mV | %s | %s | naps %lu (%lu ms)", now / 60000000, p2, battery_mv(),
                         c2 ? "charging" : "on battery", s_emu_running ? (s_lcd_off_shown ? "game, screen off" : "game") : "menu",
                         (unsigned long)s_nap_n, (unsigned long)s_nap_ms);
        }
    }
    if (s_bat_time && now - s_bat_time < 10000000) return false;
    s_bat_time = now;
    int pct; bool chg;
    if (!battery_read(&pct, &chg) || (pct == s_bat_pct && chg == s_bat_chg)) return false;
    if (s_bat_pct == -2) ESP_LOGI(TAG, "Battery %d%%%s", pct, chg ? " (charging)" : "");
    s_bat_pct = pct; s_bat_chg = chg;
    return true;
}

/* Draw the icon into a grid of colour indices: outline o, bars by level (green/yellow/red or all `mono`). */
/* Draw the icon into a grid of colour indices (1 outline, 2/3/4 green/yellow/red bars, 5/6/7 their light
 * highlight). Rounded body and terminal, bars with a light top row like the shine on a toy's icons.
 * mono != 0: everything in that one colour (launcher LCD). phase >= 0 animates: charging fills the bars up
 * one by one, nearly empty blinks. */
static void bat_icon(uint8_t *px, int stride, int pct, uint8_t o, uint8_t mono, int phase)
{
#define P(x, y) px[(y) * stride + (x)]
    const bool low = pct >= 0 && pct <= 10;
    if (low && !mono) o = 4;                      /* nearly empty: red outline */
    if (low && !s_bat_chg && phase >= 0 && (phase & 1)) return;   /* blink */
    /* body 17x12 with round corners (radius 3), then a flat 2x4 terminal */
    for (int x = 3; x <= 13; x++) { P(x, 0) = o; P(x, 11) = o; }
    P(1, 1) = o; P(2, 1) = o; P(14, 1) = o; P(15, 1) = o;
    P(1, 10) = o; P(2, 10) = o; P(14, 10) = o; P(15, 10) = o;
    P(1, 2) = o; P(15, 2) = o; P(1, 9) = o; P(15, 9) = o;
    for (int y = 3; y <= 8; y++) { P(0, y) = o; P(16, y) = o; }
    for (int y = 4; y <= 7; y++) { P(17, y) = o; P(18, y) = o; }
    /* 0-30 %: one red bar, 31-70 %: two yellow, 71-100 %: three green. Charging (in game): the last bar
     * blinks slowly (1 s on, 1 s off; phase counts half seconds). */
    int level = pct < 0 ? 0 : pct > 70 ? 3 : pct > 30 ? 2 : 1;
    uint8_t bc = mono ? mono : level >= 3 ? 2 : level == 2 ? 3 : 4;   /* green / yellow / red */
    int bars = level;
    if (s_bat_chg && phase >= 0 && level > 0 && pct < 90 && ((phase >> 1) & 1)) bars = level - 1;   /* 90%+: solid (owner) */
    /* three bars 3 wide with 1-pixel gaps (x 3..13), full height (y 2..9); the outer corners of the first
     * and last bar are rounded like the body. Light top row (shine) and darker bottom row (shade). */
    static const int bx[3] = { 3, 7, 11 };
    for (int i = 0; i < bars; i++)
        for (int y = 2; y <= 9; y++)
            for (int x = bx[i]; x < bx[i] + 3; x++) {
                if ((y == 2 || y == 9) && ((i == 0 && x == 3) || (i == 2 && x == 13))) continue;
                P(x, y) = mono ? bc : y == 2 ? (uint8_t)(bc + 3) : y == 9 ? (uint8_t)(bc + 6) : bc;
            }
#undef P
}

static void battery_draw(void)                    /* in game, from the display task */
{
    if (s_bat_pct == -2 || !s_battery_on || s_size_q != 12) return;
    static uint8_t px[BAT_VH][BAT_VW];
    memset(px, 0, sizeof px);
    bat_icon(&px[0][8], BAT_VW, s_bat_pct, 1, 0, (int)(esp_timer_get_time() / 500000));
    if (s_bat_chg) {                              /* charging: lightning bolt left of the battery */
        static const char *const BOLT[8] = { "...##", "..##.", ".##..", "#####", "..##.", ".##..", "##...", "#...." };
        for (int r = 0; r < 8; r++) for (int c = 0; c < 5; c++) if (BOLT[r][c] == '#') px[2 + r][2 + c] = 1;
    }
    /* Soft toy-like colours (not pure primaries), then the same COLOUR / LCD LOOK treatment as the game,
     * pixel grid included, so the icon matches the game screen. The background stays the border's black. */
    uint16_t col[11] = { 0x0000, RGB565(232, 226, 210) /* outline */,
                                     RGB565(112, 192, 92) /* green */, RGB565(242, 200, 72) /* yellow */, RGB565(226, 88, 72) /* red */,
                                     RGB565(176, 228, 150) /* light green */, RGB565(252, 232, 150) /* light yellow */, RGB565(244, 158, 140) /* light red */,
                                     RGB565(72, 146, 64) /* dark green */, RGB565(204, 152, 44) /* dark yellow */, RGB565(176, 58, 48) /* dark red */ };
    const bool fx = colour_fx_on() && s_col_lut && s_grid_lut, grid = fx && s_lcd_look;
    if (s_bezel_col) {                            /* sits on the shell: shell background, outline printed darker */
        uint16_t b = s_bezel_col;
        col[0] = fx ? s_col_lut[b] : b;
        {
            int r = (b >> 11) & 31, g = (b >> 5) & 63, bl = b & 31;
            col[1] = (uint16_t)(((r * 9 / 16) << 11) | ((g * 9 / 16) << 5) | (bl * 9 / 16));
        }
    }
    const int W = BAT_VW * 3, H = BAT_VH * 3;
    uint16_t *o = s_strip;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t i = px[y / 3][x / 3];
            uint16_t c = col[i];
            if (i && fx) c = (grid && (x % 3 == 2 || y % 3 == 2)) ? s_grid_lut[c] : s_col_lut[c];
            *o++ = (uint16_t)((c >> 8) | (c << 8));
        }
    esp_lv_adapter_dummy_draw_blit(s_disp, BAT_X, BAT_Y, BAT_X + W, BAT_Y + H, s_strip, true);
}

/* Small "AM"/"PM" (5 pixels tall) for the 12 h clock, drawn at the bottom of an 8x8 text cell. */
static int vlcd_ampm(int x, int y, bool pm, uint8_t c)   /* returns the width */
{
    static const char *const A[5] = { ".#.", "#.#", "###", "#.#", "#.#" };
    static const char *const P[5] = { "##.", "#.#", "##.", "#..", "#.." };
    static const char *const M[5] = { "#...#", "##.##", "#.#.#", "#...#", "#...#" };
    const char *const *g[2] = { pm ? P : A, M };
    int w = 0;
    for (int k = 0; k < 2; k++) {
        int gw = (int)strlen(g[k][0]);
        for (int r = 0; r < 5; r++) for (int i = 0; i < gw; i++) if (g[k][r][i] == '#') vlcd_rect(x + w + i, y + 2 + r, 1, 1, c);
        w += gw + 1;
    }
    return w - 1;
}
/* Small digits (4x6, bold strokes) at the bottom of an 8x8 text cell; returns the width. */
static int vlcd_small_digits(int x, int y, const char *s, uint8_t c, bool measure)
{
    static const char *const D[10][6] = {
        { ".##.", "#..#", "#..#", "#..#", "#..#", ".##." }, { ".#..", "##..", ".#..", ".#..", ".#..", "###." },
        { ".##.", "#..#", "..#.", ".#..", "#...", "####" }, { "###.", "...#", ".##.", "...#", "...#", "###." },
        { "#..#", "#..#", "#..#", "####", "...#", "...#" }, { "####", "#...", "###.", "...#", "...#", "###." },
        { ".##.", "#...", "###.", "#..#", "#..#", ".##." }, { "####", "...#", "..#.", ".#..", ".#..", ".#.." },
        { ".##.", "#..#", ".##.", "#..#", "#..#", ".##." }, { ".##.", "#..#", "#..#", ".###", "...#", ".##." } };
    int w = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') { w += 2; continue; }
        if (!measure) for (int r = 0; r < 6; r++) for (int i = 0; i < 4; i++) if (D[*s - '0'][r][i] == '#') vlcd_rect(x + w + i, y + 1 + r, 1, 1, c);
        w += 5;
    }
    return w ? w - 1 : 0;
}
/* Release year of a ROM (by file name, like rom_display_name); "" if unknown. */
static const char *rom_year(const char *file) { return MODEL_INFO[rom_info(file)->m].year; }
static const char *const *s_list_right;           /* ROM list: small text at the right of each row (release year) */
/* "h:mm" in the 8x8 font, then a small AM/PM 2 pixels after the last digit. Returns the total width. */
static int vlcd_time12(int x, int y, int hour, int minute, uint8_t c)
{
    char txt[8];
    snprintf(txt, sizeof txt, "%d:%02d", (hour + 11) % 12 + 1, minute);
    vlcd_text(x, y, txt, c);
    int w = (int)strlen(txt) * 8;
    return w + vlcd_ampm(x + w, y, hour >= 12, c);
}

static void vlcd_clock(void);
/* Top bar of the launcher screens (clock + battery), owner testing 2026-11-22: a band behind it. */
static inline uint8_t topbar_ink(void) { return (s_menu_bar == 2 || s_menu_bar == 4) ? 0 : 2; }
static void vlcd_topbar(void)
{
    if (s_menu_bar == 1 || s_menu_bar == 3) vlcd_rect(0, 0, VL, 16, 3);
    else if (s_menu_bar == 2) vlcd_rect(0, 0, VL, 16, 2);
    else if (s_menu_bar == 4) vlcd_rect(0, 0, VL, 16, 1);
    if (s_menu_bar == 3 || s_menu_bar == 5) vlcd_rect(0, 16, VL, 1, 2);
}
/* Launcher: "NN%" then the icon, right-aligned at the top of the virtual LCD, in the dark LCD ink. */
static void vlcd_battery(void)
{
    const uint8_t ink = topbar_ink();
    battery_update();
    if (s_bat_pct == -2) return;
    const int x1 = BAT_X / 3 + 8 + 19, y = BAT_Y / 3; /* exactly where the game shows it */
    uint8_t px[12][19];
    memset(px, 0xFF, sizeof px);                  /* 0xFF = leave the background */
    bat_icon(&px[0][0], 19, s_bat_pct, ink, ink ? ink : 0xFE, -1);
    int ix = x1 - 19;
    for (int r = 0; r < 12; r++) for (int c = 0; c < 19; c++) if (px[r][c] != 0xFF) vlcd_rect(ix + c, y + r, 1, 1, px[r][c] == 0xFE ? 0 : px[r][c]);
    char txt[8];
    if (s_bat_pct < 0) snprintf(txt, sizeof txt, "--");
    else snprintf(txt, sizeof txt, "%d%%", s_bat_pct);
    int tx = ix - 3 - (int)strlen(txt) * 8;
    vlcd_text(tx, y + 2, txt, ink);
    if (s_bat_chg) {                              /* charging: lightning bolt left of the number */
        static const char *const BOLT[8] = { "...##", "..##.", ".##..", "#####", "..##.", ".##..", "##...", "#...." };
        for (int r = 0; r < 8; r++) for (int c = 0; c < 5; c++)
            if (BOLT[r][c] == '#') vlcd_rect(tx - 7 + c, y + 2 + r, 1, 1, ink);
    }
}

static SemaphoreHandle_t s_present_req;
static volatile bool s_present_on, s_presenting;
extern esp_lcd_panel_io_handle_t bsp_display_get_io(void);
/* (2026-11-18: reading the panel - ID 0x04, scanline 0x45 - returns only zeros on this board, so there is
 * no software way to sync to the panel refresh; LCD_TE goes only to test pad TP3.) */
/* In-game clock: the menu clock's look and place (virtual LCD 13,4 = panel 39,12), drawn on the shell in
 * the battery outline's darker shade, with the LCD grid. Region panel x 36..204, y 12..36. */
#define GCLK_X 36
#define GCLK_Y 12
#define GCLK_VW 56
#define GCLK_VH 8
static void game_clock_draw(void)
{
    if (s_size_q != 12) return;                    /* bigger sizes: no border */
    int m = s_game_clock ? clock_minute() : -1;
    vlcd_rect(0, 0, GCLK_VW, GCLK_VH, 0);         /* s_vlcd is free while a game runs: use its corner */
    if (s_shot_show) {                             /* screenshot countdown / result instead of the time */
        char t[8];
        if (s_shot_show > 0) snprintf(t, sizeof t, IC_CAMERA " %d", s_shot_show);
        else snprintf(t, sizeof t, IC_CAMERA " %s", s_shot_show == -1 ? "OK" : "--");
        vlcd_text(1, 0, t, 2);
    } else if (m >= 0) vlcd_time12(1, 0, m / 60, m % 60, 2);
    const bool fx = colour_fx_on() && s_col_lut && s_grid_lut, grid = fx && s_lcd_look;
    uint16_t bg = s_bezel_col, ink;
    if (!bg) ink = RGB565(232, 226, 210);         /* no shell: off-white on black, like the battery */
    else { int r = (bg >> 11) & 31, g = (bg >> 5) & 63, bl = bg & 31; ink = (uint16_t)(((r * 9 / 16) << 11) | ((g * 9 / 16) << 5) | (bl * 9 / 16)); }
    uint16_t bgc = fx ? s_col_lut[bg] : bg;
    uint16_t *o = s_strip;
    for (int y = 0; y < GCLK_VH * 3; y++)
        for (int x = 0; x < GCLK_VW * 3; x++) {
            uint16_t c = bgc;
            if (s_vlcd[(y / 3) * VL + x / 3]) c = fx ? ((grid && (x % 3 == 2 || y % 3 == 2)) ? s_grid_lut[ink] : s_col_lut[ink]) : ink;
            *o++ = (uint16_t)((c >> 8) | (c << 8));
        }
    esp_lv_adapter_dummy_draw_blit(s_disp, GCLK_X, GCLK_Y, GCLK_X + GCLK_VW * 3, GCLK_Y + GCLK_VH * 3, s_strip, true);
}

/* Status bars (Plus Color, 2026-11-19). Left border: rice bowls cut from the game's status screen (2x), one
 * per hunger point (DSTRAM 0x84125) incl. overfed ones, at least 4 slots (empty ones faded). Right border:
 * the game's happiness gauge (navy box, white inside, pink fill; DSTRAM 0x8412c, 0..10) standing upright. */
#define BARS_Y GAME_Y
#define BARS_H (GRAM_H * GAME_SCALE)
#define BOWLS_X 8
#define BOWLS_W 32
#define GAUGE_X 444
#define GAUGE_W 24
#define BARS_BOTTOM ((BARS_H + 140) / 2)       /* bottom line shared by bowl 1 and the gauge (4 bowls centred) */
static uint16_t *s_bars_buf;                       /* PSRAM: one column, panel byte order */
static inline uint16_t bars_px(uint16_t c, bool fx) { if (fx) c = s_col_lut[c]; return (uint16_t)((c >> 8) | (c << 8)); }
static uint16_t blend565(uint16_t a, uint16_t b, int pa)   /* pa% of a, rest b */
{
    int r = (((a >> 11) & 31) * pa + ((b >> 11) & 31) * (100 - pa)) / 100;
    int g = (((a >> 5) & 63) * pa + ((b >> 5) & 63) * (100 - pa)) / 100;
    int bl = ((a & 31) * pa + (b & 31) * (100 - pa)) / 100;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
static void bars_blit(int X, int W)
{
    int rows = ((STRIP_BYTES / 2) / W) & ~1;
    for (int y = 0; y < BARS_H; y += rows) {
        int n = y + rows < BARS_H ? rows : BARS_H - y;
        memcpy(s_strip, s_bars_buf + y * W, (size_t)n * W * 2);
        esp_lv_adapter_dummy_draw_blit(s_disp, X, BARS_Y + y, X + W, BARS_Y + y + n, s_strip, true);
    }
}
/* Pet data per firmware, from Paramix-Studio's TamaEmu (emu/src/emu/main.c, table CARE, GPL-3.0; "found in the simulation
 * and checked on the game's status pages"). Found by the reset vector (word at the start of the code). Addresses with
 * 0x8000 set are DSTRAM 0x84xxx, others A0RAM. pts = Gotchi Points (32-bit, up to 99999); the happiness maximum grows
 * with the medals: mul * (number of bits of med & mask) + add (Plus Color HAPPY! 10 per Happy Symbol, P's ring,
 * iD L friend level); 4U+ (mask 0): 100 once the stage byte med is above 2, else 30. hunmax = full hunger. */
typedef struct { uint32_t rv; uint16_t hun, hap, pts, med; uint8_t hunmax, mul, add, mask, sk; } CareInfo;   /* sk: sick flag offset from hun (0 = none) */
static const CareInfo CARE[] = {
    { 0x00C2F0ECu, 0x8125, 0x812C, 0x8138, 0x815E, 4, 10, 10, 0x7F, 0x0C },   /* Plus Color v040 */
    { 0x024CA8FEu, 0x8221, 0x8228, 0x8234, 0x826C, 4, 20, 19, 0x0F, 0x0C },   /* iD L v050 JP (2011-01-14: block 4 bytes lower; Paramix's sick-flag table) */
    { 0x024CA942u, 0x8225, 0x822C, 0x8238, 0x8270, 4, 20, 19, 0x0F, 0x0C },   /* iD L v050 (2011-02-24) */
    { 0x024CAA72u, 0x8225, 0x822C, 0x8238, 0x8270, 4, 20, 19, 0x0F, 0x0C },   /* iD L v053 (English) */
    { 0x024BD18Au, 0x8201, 0x8208, 0x8214, 0x8250, 4, 20, 19, 0x0F, 0x0C },   /* iD L v054 (15th Anniversary) */
    { 0x024C14F6u, 0x8201, 0x8208, 0x8214, 0x8250, 4, 20, 19, 0x0F, 0x0C },   /* iD L v055 (Princess Spacy) */
    { 0x024FDFE6u, 0x12AC, 0x12B4, 0x12C0, 0x12FA, 4, 4, 4, 0x0F, 0 },     /* P's */
    { 0x024F6256u, 0x11A1, 0x11A8, 0x11B4, 0x11E2, 6, 0, 0, 0, 0 },        /* 4U+ */
    { 0x024C680Au, 0x12A9, 0x12B0, 0x12BC, 0x12EA, 6, 0, 0, 0, 0 },        /* 4U v001 */
};
static const CareInfo *care_find(Emu *e)
{
    if (!e) return NULL;
    uint32_t v = mem_read32(e, e->dev.ttbr_reset);
    for (size_t i = 0; i < sizeof CARE / sizeof CARE[0]; i++) if (CARE[i].rv == v) return &CARE[i];
    return NULL;
}
static uint8_t *care_p(Emu *e, uint16_t a) { return (a & 0x8000) ? e->dstram + (a & 0x7FFF) : e->a0ram + a; }
static int care_happy_max(Emu *e, const CareInfo *c)
{
    uint8_t m = *care_p(e, c->med);
    if (!c->mask) return m > 2 ? 100 : 30;
    return c->mul * __builtin_popcount(m & c->mask) + c->add;
}
static volatile int s_bar_hmax = -1;
static volatile bool s_care_redraw;                /* EZCARE cured the pet: a short C after the quick menu makes the game redraw */               /* the game's current happiness maximum (-1 = not known) */

static void bars_draw(void)                        /* display task */
{
    if (!s_bars_on || !s_bar_game || s_size_q != 12) return;
    const bool ps = s_bar_game == 2, idl = s_bar_game == 3, u4 = s_bar_game == 4;
    const uint16_t (*icons)[16][16] = (ps || u4) ? FOOD_BALL : FOOD_BOWL;   /* rice balls: P's, 4U; rice bowls: Plus Color, iD L */
    const int hmax = s_bar_hmax > 0 ? s_bar_hmax : ps ? 4 : idl ? 19 : u4 ? 30 : 10;           /* happiness: P's 0..4 (quarters), iD L friend level 0..19 (the game's own
                                                        * maximum: 19 after candies and toys; its box looks full there), Plus Color 0..10 */
    const bool here = s_bars_shown_loc != 1;   /* hidden while away (switched with the screen change, see display_task) */
    if (!s_bars_buf) s_bars_buf = heap_caps_malloc(BOWLS_W * BARS_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_bars_buf) return;
    const bool fx = colour_fx_on() && s_col_lut;
    const uint16_t bg = s_bezel_col, bgp = bars_px(bg, fx);
    int hunger = s_bar_hunger, happy = s_bar_happy;
    /* left: bowls */
    for (int i = 0; i < BOWLS_W * BARS_H; i++) s_bars_buf[i] = bgp;
    if (here && hunger >= 0 && hunger <= 20) {
        /* 4 slots, then overfed bowls (up to 2: the 2 hidden hearts) after a gap with a small "+" mark.
         * Kept neutral: overfeeding is not a bonus (fan wikis: it can count as overfeeding / affect evolution). */
        const int norm = u4 ? 6 : 4;                   /* 4U+: six food slots, no overfed ones shown */
        int slots = hunger < norm ? norm : hunger > 6 ? 6 : hunger;
        int total = slots * 36 - 4 + (slots > norm ? 10 : 0), y0 = (BARS_H - total) / 2;   /* centred (owner prefers this to a fixed bottom line) */
        uint16_t ink = bg ? (uint16_t)(((((bg >> 11) & 31) * 9 / 16) << 11) | ((((bg >> 5) & 63) * 9 / 16) << 5) | ((bg & 31) * 9 / 16))
                          : RGB565(232, 226, 210);
        for (int k = 0; k < slots; k++) {
            bool full = k < hunger, over = k >= norm;
            int by = y0 + total - 32 - k * 36 - (over ? 10 : 0);   /* bottom-up: bowl 1 lowest, overfed ones on top */
            for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
                uint16_t c;
                if (full) c = icons[k & 3][y][x];
                else {                                 /* empty slot: a small plate in shades of the shell */
                    uint8_t m = FOOD_PLATE[y][x];
                    c = !m ? 0 : !bg ? (m == 1 ? RGB565(110, 110, 110) : RGB565(60, 60, 60))
                      : (uint16_t)(((((bg >> 11) & 31) * (m == 1 ? 10 : 14) / 16) << 11) | ((((bg >> 5) & 63) * (m == 1 ? 10 : 14) / 16) << 5) | ((bg & 31) * (m == 1 ? 10 : 14) / 16));
                    if (!c && m) c = 1;
                }
                if (!c) continue;
                uint16_t p = bars_px(c, fx);
                int py = by + y * 2, px = x * 2;
                s_bars_buf[py * BOWLS_W + px] = p; s_bars_buf[py * BOWLS_W + px + 1] = p;
                s_bars_buf[(py + 1) * BOWLS_W + px] = p; s_bars_buf[(py + 1) * BOWLS_W + px + 1] = p;
            }
            if (k == 4 && norm == 4) {                 /* "+" between the normal and the overfed bowls */
                static const char *const SP[3] = { ".#.", "###", ".#." };
                uint16_t p = bars_px(ink, fx);
                for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) if (SP[y][x] == '#') {
                    int py = by + 34 + y * 2, px = 13 + x * 2;
                    if (py >= 0 && px + 1 < BOWLS_W) { s_bars_buf[py * BOWLS_W + px] = p; s_bars_buf[py * BOWLS_W + px + 1] = p;
                                                       s_bars_buf[(py + 1) * BOWLS_W + px] = p; s_bars_buf[(py + 1) * BOWLS_W + px + 1] = p; }
                }
            }
        }
    }
    bars_blit(BOWLS_X, BOWLS_W);
    /* right: happiness gauge 17 px wide (4 px navy edge, rounded corners, white inside, pink fill in 2 px steps)
     * with a heart (11 x 10 blocks of 2 px; middle column 3 px so the tip centres on the gauge). IN: heart + 4 px
     * gap + gauge = the 4-bowl height (140 px). OUT: gauge 140 px, heart above it. Gauge x 448..464, centre 456. */
    for (int i = 0; i < GAUGE_W * BARS_H; i++) s_bars_buf[i] = bgp;
    if (happy > hmax) {                                /* never seen: show full, and say so in the log */
        static int logged;
        if (!logged) { logged = 1; ESP_LOGW(TAG, "happiness above %d: %d", hmax, happy); }
        happy = hmax;
    }
    if (here && happy >= 0) {
        const uint16_t navy = u4 ? RGB565(180, 0, 106) : RGB565(0, 0, 98), white = RGB565(255, 255, 255);   /* 4U+: its fan's magenta frame */
        /* Plus Color: pink like its happiness block; P's: its gauge's blue (dark at the bottom, cyan at the top) */
        /* iD L: the yellow of its FRIEND LEVEL box */
        /* 4U+: its status screen's blue fan behind a pink heart */
        const uint16_t pink = ps ? RGB565(70, 200, 255) : idl ? RGB565(232, 255, 0) : u4 ? RGB565(255, 72, 180) : RGB565(255, 68, 255),
                       lpink = ps ? RGB565(190, 240, 255) : idl ? RGB565(250, 255, 170) : u4 ? RGB565(255, 125, 205) : RGB565(255, 170, 255);
        const int gx = 448 - GAUGE_X, gw = 17;
        int ht = s_heart_out ? BARS_BOTTOM - 164 : BARS_BOTTOM - 140;   /* heart top */
        int gt = s_heart_out ? BARS_BOTTOM - 140 : BARS_BOTTOM - 116;   /* gauge top */
        int gh = BARS_BOTTOM - gt;
        int fill = (happy * (gh - 8) / hmax + 1) & ~1;
        for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) {
            if ((y < 2 || y >= gh - 2) && (x < 2 || x >= gw - 2)) continue;
            uint16_t c = navy;
            if (x >= 4 && x < gw - 4 && y >= 4 && y < gh - 4) {
                int up = gh - 5 - y;                   /* 0 at the bottom of the inside */
                if (up >= fill) c = white;
                else if (u4) c = RGB565(0, 64, 255);  /* 4U+: the blue of its fan */
                else if (!ps && !idl) {                /* Plus Color: one colour per 10 points, the game's own rainbow */
                    /* (ROM 0x21cfe4: fa3f fb75 fc60 fe4a 07ef 05df 041f 001f, BGR panel -> these RGB colours) */
                    static const uint16_t SEG[8] = { RGB565(255, 68, 255), RGB565(172, 109, 255), RGB565(0, 141, 255), RGB565(82, 202, 255),
                                                     RGB565(123, 255, 0), RGB565(255, 186, 0), RGB565(255, 129, 0), RGB565(255, 0, 0) };
                    int seg = up * hmax / (gh - 8) / 10;
                    c = SEG[seg < 0 ? 0 : seg > 7 ? 7 : seg];
                }
                else if (!ps) c = pink;
                else {                                 /* P's: gradient dark blue -> cyan over the full height */
                    int t = up * 256 / (gh - 8);
                    c = RGB565(0, 40 + t * 190 / 256, 200 + t * 55 / 256);
                }
            }
            s_bars_buf[(gt + y) * GAUGE_W + gx + x] = bars_px(c, fx);
        }
        static const char *const HEART[10] = {
            "..NNN.NNN..", ".NPPPNPPPN.", "NPLLPPPPPPN", "NPLPPPPPPPN", "NPPPPPPPPPN",
            ".NPPPPPPPN.", "..NPPPPPN..", "...NPPPN...", "....NPN....", ".....N....." };
        for (int r = 0; r < 10; r++) {
            int px = 456 - 11 - GAUGE_X;              /* 23 px wide: 445..467 */
            for (int col = 0; col < 11; col++) {
                int w = col == 5 ? 3 : 2;
                char ch = HEART[r][col];
                if (ch != '.') {
                    uint16_t p = bars_px(ch == 'N' ? navy : ch == 'L' ? lpink : pink, fx);
                    for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < w; dx++) {
                        int yy = ht + r * 2 + dy, xx = px + dx;
                        if (yy >= 0 && yy < BARS_H && xx >= 0 && xx < GAUGE_W) s_bars_buf[yy * GAUGE_W + xx] = p;
                    }
                }
                px += w;
            }
        }
    }
    bars_blit(GAUGE_X, GAUGE_W);
}

static void game_present(void);
/* screen update timing (tearing investigation): updates, total/max send time, panel rows sent */
static volatile uint32_t s_pr_n, s_pr_us, s_pr_max, s_pr_rows;
static volatile uint32_t s_why[3], s_middraw;
/* PRESS TRACE (hiccup investigation, 2026-11-20): after a key press in a game, note when the game finished
 * drawing (lcd_done_hook), when frames were sent to the panel and how far the emulator fell behind real
 * time. One summary per burst of presses (TRACE lines, ms after the first press). */
#define TR_N 64
static volatile int64_t s_tr_t0, s_tr_until;
static volatile uint16_t s_tr_press[TR_N], s_tr_draw[TR_N], s_tr_sent[TR_N];
static volatile int s_tr_np, s_tr_nd, s_tr_ns;
static volatile int32_t s_tr_behind_max, s_tr_send_max, s_tr_behind_at;
static volatile uint64_t s_tr_host, s_tr_exec, s_tr_g2, s_tr_h2;   /* ESP cycles / executed guest cycles during the trace */
static void tr_add(volatile uint16_t *a, volatile int *n, int64_t t)
{
    if (!s_tr_t0 || *n >= TR_N) return;
    int64_t d = (t - s_tr_t0) / 1000;
    if (d < 0) d = 0;
    a[(*n)++] = d > 65535 ? 65535 : (uint16_t)d;
}   /* frame sent because: slept / 40 ms quiet / 120 ms max; game still drawing */
static void display_task(void *arg)
{
    while (1) {
        xSemaphoreTake(s_present_req, portMAX_DELAY);
        s_presenting = true;
        pm_hold(s_pm_disp, true);
        if (s_present_on && s_emu) {
            if (s_bat_redraw) { s_bat_redraw = false; if (!s_lcd_off_shown) battery_draw(); }
            if (s_clock_redraw) { s_clock_redraw = false; if (!s_lcd_off_shown) game_clock_draw(); }
            if (s_bars_redraw) { s_bars_redraw = false; if (!s_lcd_off_shown) bars_draw(); }
            if (s_shot_req_grab) { s_shot_req_grab = false; shot_grab(); s_shot_grabbed = true; }   /* what is on screen now */
            int64_t t0 = esp_timer_get_time();
            uint32_t r0 = s_pr_rows;
            uint64_t w0 = s_emu->lcd_writes;
            game_present();
            if (s_pr_rows != r0 && s_emu->lcd_writes != w0) s_middraw++;   /* the game drew more while we sent */
            if (s_pr_rows != r0) {
                uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
                if (s_tr_t0) { tr_add(s_tr_sent, &s_tr_ns, esp_timer_get_time()); if ((int32_t)us > s_tr_send_max) s_tr_send_max = (int32_t)us; }
                if (s_bars_wait_frame && s_pr_rows - r0 >= 150) {   /* the new screen (home / destination menu) is up */
                    s_bars_wait_frame = false; s_bars_shown_loc = s_bar_loc; bars_draw();
                }
                s_pr_n++; s_pr_us += us; if (us > s_pr_max) s_pr_max = us;
            }
        }
        pm_hold(s_pm_disp, false);
        s_presenting = false;
    }
}

/* Bezel: a thin rounded frame around the game in a soft shell colour per ROM, drawn in the same 3x3 pixel
 * steps as the game (one pixel line, one pixel gap). Units are game pixels: the game covers 16..143. */
static inline bool in_rrect(int u, int v, int x0, int y0, int x1, int y1, int r)
{
    if (u < x0 || u > x1 || v < y0 || v > y1) return false;
    int dx = u < x0 + r ? x0 + r - u : u > x1 - r ? u - (x1 - r) : 0;
    int dy = v < y0 + r ? y0 + r - v : v > y1 - r ? v - (y1 - r) : 0;
    return dx * dx + dy * dy <= r * r + r;        /* "+ r": rounder pixel circles */
}
static inline bool bezel_at(int u, int v)         /* shell colour up to the screen edge, 1 pixel black gap */
{
    return !in_rrect(u, v, 15, 15, 144, 144, 2);
}

static void bezel_rect(int x0, int y0, int x1, int y1)    /* render [x0,x1)x[y0,y1) of the bezel band */
{
    uint16_t c = s_bezel_col;
    if ((s_colour < 100 || s_lcd_look) && s_col_lut) c = s_col_lut[c];
    c = (uint16_t)((c >> 8) | (c << 8));
    const int rows = (STRIP_BYTES / 2 / (x1 - x0)) & ~1;
    for (int ys = y0; ys < y1; ys += rows) {     /* in strips that fit the DMA buffer */
        int ye = ys + rows < y1 ? ys + rows : y1;
        uint16_t *o = s_strip;
        for (int y = ys; y < ye; y++)
            for (int x = x0; x < x1; x++)
                *o++ = bezel_at(x / GAME_SCALE, y / GAME_SCALE) ? c : 0;   /* GAME_X = 16 game pixels */
        esp_lv_adapter_dummy_draw_blit(s_disp, x0, ys, x1, ye, s_strip, true);
    }
}

static void bezel_draw(void)
{
    if (s_size_q != 12) return;                    /* bigger sizes: black around the game */
    if (!s_bezel_col) return;
    const int ge = GAME_Y + GRAM_H * GAME_SCALE;   /* 432 */
    bezel_rect(0, 0, SCR_W, GAME_Y);               /* top */
    bezel_rect(0, ge, SCR_W, SCR_H);               /* bottom */
    bezel_rect(0, GAME_Y, GAME_X, ge);             /* left */
    bezel_rect(GAME_X + GRAM_W * GAME_SCALE, GAME_Y, SCR_W, ge);   /* right */
}

/* Panel sleep (battery, 2026-11-20): while the game's LCD is off the AMOLED is put to sleep (display off 0x28,
 * sleep in 0x10), which stops its scanning and its built-in power supply; brightness 0 alone kept them running.
 * The panel's supply is shared with the ESP32 (no separate switch on this board). Waking: sleep out 0x11, the
 * picture can be written after 5 ms, display on 0x29 once PANEL_WAKE_MS have passed. */
#define PANEL_WAKE_MS 80
static bool s_panel_asleep;
static int64_t s_panel_t;                          /* when it went to sleep / was told to wake */
static void panel_cmd(uint8_t c) { esp_lcd_panel_io_tx_param(bsp_display_get_io(), (int)((0x02u << 24) | ((uint32_t)c << 8)), NULL, 0); }
static void panel_sleep(void)
{
    if (s_panel_asleep) return;
    panel_cmd(0x28);
    panel_cmd(0x10);
    s_panel_asleep = true;
    s_panel_t = esp_timer_get_time();
}
static bool panel_wake_begin(void)                 /* true if it was asleep: call panel_wake_end after drawing */
{
    if (!s_panel_asleep) return false;
    int64_t since = esp_timer_get_time() - s_panel_t;
    if (since < 120000) vTaskDelay(pdMS_TO_TICKS((120000 - since) / 1000 + 1));   /* sleep in -> out needs 120 ms */
    panel_cmd(0x11);
    s_panel_t = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(6));
    return true;
}
static void panel_wake_end(void)
{
    if (!s_panel_asleep) return;
    int64_t w = PANEL_WAKE_MS * 1000LL - (esp_timer_get_time() - s_panel_t);
    if (w > 0) vTaskDelay(pdMS_TO_TICKS(w / 1000 + 1));
    panel_cmd(0x29);
    s_panel_asleep = false;
}
static void panel_wake_now(void) { if (panel_wake_begin()) panel_wake_end(); }

static void game_display_begin(void)
{
    esp_lv_adapter_set_dummy_draw(s_disp, true);
    panel_fill_black(0, 0, SCR_W, SCR_H);
    bezel_draw();
    s_lcd_off_shown = false;
    s_clock_redraw = true;
    s_bars_redraw = true;
    s_show_all = true;
    s_bat_redraw = true;
    s_present_on = true;
}

static void game_display_end(void)
{
    s_present_on = false;
    while (s_presenting) vTaskDelay(pdMS_TO_TICKS(2));
    if (s_lcd_off_shown) { low_power(false); s_lcd_off_shown = false; panel_wake_now(); bsp_display_brightness_set(s_brightness); }   /* left while asleep */
    esp_lv_adapter_set_dummy_draw(s_disp, false);     /* LVGL takes over again and redraws the menu */
}

/* TEMP (2026-11-21): how does each game save to its flash? First operation of a burst: guest PC, SP and the
 * stack above it (return addresses = the routine that started the save); summary in the 3 s log line. */
static struct { volatile int state; uint32_t pc, sp, stack[48], n_prog, n_erase, lo, hi; uint64_t last_cyc; } s_fs;
static volatile uint32_t s_flash_prog_n, s_flash_erase_n;
static int64_t s_erase_t[8];                       /* times of the last 8 flash erases */
static volatile uint32_t s_erase_i;
static int64_t s_patch_until;                      /* status bars hidden until then: the game is rewriting its flash */   /* all flash program / erase operations (forced save check) */
static void flash_hook(Emu *e, int kind, uint32_t off)            /* emulator task */
{
    if (kind) s_flash_prog_n++;
    else {                                         /* erases: remember when (patch detection for the status bars) */
        s_flash_erase_n++;
        s_erase_t[s_erase_i++ & 7] = esp_timer_get_time();
    }
    if (s_fs.state != 1) {                         /* a new burst */
        if (s_fs.state == 2) return;               /* previous one not printed yet */
        s_fs.state = 1; s_fs.pc = e->pc; s_fs.sp = e->sp; s_fs.n_prog = s_fs.n_erase = 0; s_fs.lo = 0xFFFFFFFFu; s_fs.hi = 0;
        for (int i = 0; i < 48; i++) s_fs.stack[i] = mem_read32(e, e->sp + (uint32_t)i * 4);
    }
    if (kind) s_fs.n_prog++; else s_fs.n_erase++;
    if (off < s_fs.lo) s_fs.lo = off;
    if (off > s_fs.hi) s_fs.hi = off;
    s_fs.last_cyc = e->cycles;
}
static void lcd_done_hook(Emu *e, int x0, int y0, int x1, int y1)   /* emulator task, inside tc_run */
{
    if (s_tr_t0) {                                 /* trace: windows finish in groups, note each group once */
        int64_t t = esp_timer_get_time();
        static int64_t last;
        if (t - last > 4000) tr_add(s_tr_draw, &s_tr_nd, t);
        last = t;
    }
    xSemaphoreTake(s_ready_mtx, portMAX_DELAY);
    for (int y = y0; y <= y1; y++) memcpy(&s_ready[y][x0], &s_gram[y][x0], (size_t)(x1 - x0 + 1) * 2);
    s_ready_new = true;
    if ((x1 - x0 + 1) * (y1 - y0 + 1) >= 2048) s_ready_big = true;
    s_ready_t = esp_timer_get_time();
    xSemaphoreGive(s_ready_mtx);
}

static void lcd_row_hook(Emu *e, int x0, int x1, int y)   /* emulator task */
{
    if (s_draw_mode != 0) return;
    const Lcd *l = &e->lcd;
    if ((l->xe - l->xs + 1) * (l->ye - l->ys + 1) < 2048) return;   /* sprites and icons: whole only */
    xSemaphoreTake(s_ready_mtx, portMAX_DELAY);
    memcpy(&s_ready[y][x0], &s_gram[y][x0], (size_t)(x1 - x0 + 1) * 2);
    s_ready_new = true;
    s_ready_t = esp_timer_get_time();
    xSemaphoreGive(s_ready_mtx);
}

static void ready_take_all(void)                  /* fallback: the whole screen memory counts as finished */
{
    xSemaphoreTake(s_ready_mtx, portMAX_DELAY);
    memcpy(s_ready, s_gram, GRAM_W * GRAM_H * sizeof(uint16_t));
    s_ready_new = true;
    xSemaphoreGive(s_ready_mtx);
}

static void game_present(void)
{
    /* The game switched its LCD off (power saving: display off / sleep): show a blank screen, like the toy.
     * It keeps running; when it switches the LCD back on, the whole picture is sent again. */
    const bool lcd_on = s_emu->lcd.disp_on && s_emu->lcd.sleep_out;
    if (!lcd_on) {
        if (!s_lcd_off_shown) {                   /* whole screen dark: game, bezel and battery */
            ESP_LOGI(TAG, "Game switched its screen off");
            panel_fill_black(0, 0, SCR_W, SCR_H);
            bsp_display_brightness_set(0);
            panel_sleep();
            s_lcd_off_shown = true;
            low_power(true);
        }
        return;
    }
    if (s_lcd_off_shown) {
        low_power(false);
        ESP_LOGI(TAG, "Game switched its screen on");
        s_lcd_off_shown = false;
        panel_wake_begin();
        bezel_draw();
        battery_draw();
        game_clock_draw();
        bars_draw();
        s_show_all = true;
        game_present();                           /* draw the picture first, then light the panel */
        panel_wake_end();
        bsp_display_brightness_set(s_brightness);
        return;
    }
    static uint8_t last_madctl = 0xFF, last_inv = 0xFF;
    const uint8_t madctl = s_emu->lcd.madctl, inv = s_emu->lcd.inverted;
    if (madctl != last_madctl || inv != last_inv) {
        ESP_LOGI(TAG, "LCD settings: MADCTL 0x%02x (colour order %s), COLMOD 0x%02x, inverted %d",
                 madctl, (madctl & 0x08) ? "BGR" : "RGB", s_emu->lcd.colmod, inv);
        last_madctl = madctl; last_inv = inv;
        s_show_all = true;
    }
    const bool bgr = (madctl & 0x08) != 0;

    /* 1. what changed since the last frame (in the finished picture) */
    xSemaphoreTake(s_ready_mtx, portMAX_DELAY);
    s_ready_new = false;
    int x0 = GRAM_W, x1 = -1, y0 = GRAM_H, y1 = -1;
    for (int y = 0; y < GRAM_H; y++) {
        if (!s_show_all && memcmp(s_ready[y], s_shown[y], sizeof s_shown[y]) == 0) continue;
        int a = 0, b = GRAM_W - 1;
        if (!s_show_all) {
            while (s_ready[y][a] == s_shown[y][a]) a++;
            while (s_ready[y][b] == s_shown[y][b]) b--;
        }
        if (a < x0) x0 = a;
        if (b > x1) x1 = b;
        if (y < y0) y0 = y;
        y1 = y;
    }
    s_show_all = false;
    if (x1 < 0) { xSemaphoreGive(s_ready_mtx); return; }

    /* Snapshot the changed rectangle first and send exactly that. */
    for (int y = y0; y <= y1; y++) memcpy(&s_shown[y][x0], &s_ready[y][x0], (size_t)(x1 - x0 + 1) * 2);
    xSemaphoreGive(s_ready_mtx);

    /* 2. panel rectangle (the panel wants even start and end coordinates). The game area is s_area square
     * at (s_ax, s_ax); s_srcx / s_edge map panel pixels to game pixels (3x, or 3.25x..3.75x with mixed widths). */
    int X0 = (s_ax + s_pstart[x0]) & ~1, X1 = (s_ax + s_pstart[x1 + 1] + 1) & ~1;
    int Y0 = (s_ax + s_pstart[y0]) & ~1, Y1 = (s_ax + s_pstart[y1 + 1] + 1) & ~1;
    if (X1 > s_ax + s_area) X1 = s_ax + s_area;
    if (Y1 > s_ax + s_area) Y1 = s_ax + s_area;
    int w = X1 - X0, rows = (STRIP_BYTES / (w * 2)) & ~1;

    s_pr_rows += (uint32_t)(Y1 - Y0);
    /* 3. send it in strips. Each game row is converted once into two ready-made panel lines (colour order,
     * inversion, COLOUR / LCD SHADER tables, scaled, byte-swapped): the normal line (last panel pixel of each
     * game pixel darker with the LCD shader) and the grid line (all darker). Panel rows are copies. */
    static uint16_t lineN[SCR_W], lineG[SCR_W];
    const bool fx = colour_fx_on() && s_col_lut && s_grid_lut;
    const bool grid = fx && s_lcd_look;
    const int lx0 = X0 - s_ax;                         /* slice of the lines that is sent */
    int built = -1;
    for (int Y = Y0; Y < Y1; Y += rows) {
        int Ye = Y + rows < Y1 ? Y + rows : Y1;
        uint16_t *o = s_strip;
        for (int yy = Y; yy < Ye; yy++) {
            int py = yy - s_ax;
            int sy = s_srcx[py];
            if (sy != built) {
                built = sy;
                const uint16_t *src = s_shown[sy];
                int last_sx = -1; uint16_t out = 0, out_g = 0;
                for (int p = lx0; p < lx0 + w; p++) {
                    int sx = s_srcx[p];
                    if (sx != last_sx) {
                        last_sx = sx;
                        uint16_t c = src[sx];
                        if (bgr) c = (uint16_t)((c >> 11) | (c & 0x07E0) | ((c & 0x1F) << 11));
                        if (inv) c = (uint16_t)~c;
                        uint16_t g = c;
                        if (fx) { g = grid ? s_grid_lut[c] : s_col_lut[c]; c = s_col_lut[c]; }
                        out = (uint16_t)((c >> 8) | (c << 8)); out_g = (uint16_t)((g >> 8) | (g << 8));
                    }
                    lineN[p] = (grid && s_edge[p]) ? out_g : out;
                    lineG[p] = out_g;
                }
            }
            memcpy(o, ((grid && s_edge[py]) ? lineG : lineN) + lx0, (size_t)w * 2);
            o += w;
        }
        esp_lv_adapter_dummy_draw_blit(s_disp, X0, Y, X1, Ye, s_strip, true);
    }
}

static void screen_fill(uint16_t c)
{
    bsp_display_lock(-1);
    for (int i = 0; i < SCR_W * SCR_H; i++) s_fb[i] = c;
    screen_flush();
    bsp_display_unlock();
}

static void screen_init(void)
{
    for (int i = 0; i < 4; i++) {
        uint16_t c = PAL[i];
        uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        PAL_GRID[i] = (uint16_t)(((r * 7 / 8) << 11) | ((g * 7 / 8) << 5) | (b * 7 / 8));
    }
    s_fb = heap_caps_calloc(SCR_W * SCR_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_vlcd = heap_caps_calloc(VL * VL, 1, MALLOC_CAP_SPIRAM);
    memset(&s_fb_dsc, 0, sizeof s_fb_dsc);
    s_fb_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_fb_dsc.header.w = SCR_W;
    s_fb_dsc.header.h = SCR_H;
    s_fb_dsc.header.stride = SCR_W * 2;
    s_fb_dsc.data_size = SCR_W * SCR_H * 2;
    s_fb_dsc.data = (const uint8_t *)s_fb;

    bsp_display_lock(-1);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    s_fb_img = lv_image_create(scr);
    lv_image_set_src(s_fb_img, &s_fb_dsc);
    lv_obj_set_pos(s_fb_img, 0, 0);
    lv_obj_remove_flag(s_fb_img, LV_OBJ_FLAG_CLICKABLE);
    bsp_display_unlock();
}

/* ------------------------------------------------------------------------------------------------ */
/* Hardware keys                                                                                     */
/* ------------------------------------------------------------------------------------------------ */

static const int s_key_gpio[3] = { KEY_A_GPIO, KEY_B_GPIO, KEY_C_GPIO };
static void key_isr(void *arg);
static int s_key_idle[3];          /* level of each key when not pressed, measured at start-up */
static uint8_t s_keys, s_keys_prev;

static void keys_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << KEY_A_GPIO) | (1ULL << KEY_B_GPIO) | (1ULL << KEY_C_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(5));
    for (int k = 0; k < 3; k++) {
        int ones = 0;
        for (int i = 0; i < 8; i++) { ones += gpio_get_level(s_key_gpio[k]); esp_rom_delay_us(200); }
        s_key_idle[k] = ones >= 4;
    }
    for (int k = 0; k < 3; k++)                   /* a press wakes the chip from light sleep */
        gpio_wakeup_enable(s_key_gpio[k], s_key_idle[k] ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_log_level_set("gpio", ESP_LOG_NONE);     /* the board support code may have installed it already: */
    gpio_install_isr_service(0);                   /* that is fine, so no red "already installed" line at boot */
    esp_log_level_set("gpio", ESP_LOG_INFO);
    for (int k = 0; k < 3; k++) { gpio_isr_handler_add(s_key_gpio[k], key_isr, NULL); gpio_intr_disable(s_key_gpio[k]); }
    ESP_LOGI(TAG, "Keys: idle levels A(GPIO%d)=%d B(GPIO%d)=%d C(GPIO%d)=%d", KEY_A_GPIO, s_key_idle[0],
             KEY_B_GPIO, s_key_idle[1], KEY_C_GPIO, s_key_idle[2]);
}

/* While the pet sleeps, a key press must wake the ui task at once (it only looks every 100 ms then):
 * the keys' wake-up level interrupt notifies it. The interrupt switches itself off (level type would keep
 * firing while held) and the ui task switches it on again once the keys are released. */
static volatile bool s_key_irq_on;
static void key_isr(void *arg)
{
    for (int k = 0; k < 3; k++) gpio_intr_disable(s_key_gpio[k]);
    s_key_irq_on = false;
    BaseType_t hp = pdFALSE;
    if (s_ui_task_h) vTaskNotifyGiveFromISR(s_ui_task_h, &hp);
    if (s_emu_task_h) vTaskNotifyGiveFromISR(s_emu_task_h, &hp);
    portYIELD_FROM_ISR(hp);
}
static void key_irq(bool on)
{
    if (on == s_key_irq_on) return;
    s_key_irq_on = on;
    for (int k = 0; k < 3; k++) { if (on) gpio_intr_enable(s_key_gpio[k]); else gpio_intr_disable(s_key_gpio[k]); }
}

/* MENU SLEEP (2026-11-22): in the launcher menus (no game running) the screen switches off after
 * MENU_SLEEP_S without a key, and the chip light-sleeps like when a pet sleeps. Any key wakes it; that
 * press only wakes (it is not passed on to the menu). Done inside keys_poll, which every menu calls. */
#define MENU_SLEEP_S 60                         /* owner 2026-11-23: was 180 */
static void audio_power(bool on);
static void menu_sleep(void)
{
    ESP_LOGI(TAG, "Menu idle %d s: screen off, sleeping until a key", MENU_SLEEP_S);
    bsp_display_brightness_set(0);
    low_power(true);                               /* pauses LVGL, allows light sleep */
    bool ui_held = s_pm_on[2];
    pm_hold(s_pm_ui, false);
    panel_sleep();
    audio_power(false);
    int base[3];                                   /* wake on a CHANGE from the levels now (a key that reads as */
    for (int k = 0; k < 3; k++) {                  /* held must not wake it straight away) */
        base[k] = gpio_get_level(s_key_gpio[k]);
        gpio_wakeup_enable(s_key_gpio[k], base[k] ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    }
    while (1) {                                    /* wait for any key (GPIO interrupt wakes the task) */
        key_irq(true);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        bool any = false;
        for (int k = 0; k < 3; k++) if (gpio_get_level(s_key_gpio[k]) != base[k]) any = true;
        if (any) break;
    }
    key_irq(false);
    for (int k = 0; k < 3; k++)                    /* back to waking on a press */
        gpio_wakeup_enable(s_key_gpio[k], s_key_idle[k] ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    if (ui_held) pm_hold(s_pm_ui, true);
    panel_wake_begin();
    low_power(false);
    vlcd_present();                                /* the menu as it was */
    panel_wake_end();
    bsp_display_brightness_set(s_brightness);
    audio_power(true);
    ESP_LOGI(TAG, "Menu woke up");
}

/* Read the keys (call every ~10 ms). A key counts as pressed when its level differs from idle for two
 * reads in a row (debounce). Returns the keys that went down since last time. */
static uint8_t keys_poll(void)
{
    static uint8_t raw_prev;                       /* the previous raw reading (debounce) */
    if (!s_emu_running && !s_usb_drive) {          /* menu sleep */
        static int64_t last_key;
        static uint8_t seen;                       /* only a CHANGE of the keys counts as activity, so a key that */
        int64_t now = esp_timer_get_time();        /* reads as held for any reason cannot keep the menu awake */
        if (s_keys != seen || !last_key || s_ui_poke > last_key) { seen = s_keys; last_key = now; }
        else if (now - last_key > MENU_SLEEP_S * 1000000LL) {
            menu_sleep();
            uint8_t raw = 0;                       /* the waking key: held keys count as already down */
            for (int k = 0; k < 3; k++) if (gpio_get_level(s_key_gpio[k]) != s_key_idle[k]) raw |= (uint8_t)(1 << k);
            if (s_flip) raw = (uint8_t)((raw & 2) | ((raw & 1) << 2) | ((raw & 4) >> 2));
            /* the keys held right now count as already down, so the waking press does nothing in the menu. (Was
             * raw | 7 = all three "held": a key's mark only cleared when that key was pressed again, so after the
             * first wake an unused key stayed "held" and the menu never slept again - owner 2026-11-23.) */
            s_keys = s_keys_prev = raw_prev = raw;
            last_key = esp_timer_get_time();
            return 0;
        }
    }
    uint8_t raw = 0;
    for (int k = 0; k < 3; k++)
        if (gpio_get_level(s_key_gpio[k]) != s_key_idle[k]) raw |= (uint8_t)(1 << k);
    if (s_flip) raw = (uint8_t)((raw & 2) | ((raw & 1) << 2) | ((raw & 4) >> 2));   /* buttons at the bottom: still A B C left to right */
    s_keys_prev = s_keys;
    s_keys = (uint8_t)((s_keys & ~(raw ^ raw_prev)) | (raw & raw_prev));   /* change only when 2 reads agree */
    raw_prev = raw;
    uint8_t down = s_keys & ~s_keys_prev;
    if (down && s_emu_running && !s_vlcd_direct) {
        int64_t t = esp_timer_get_time();
        if (!s_tr_t0) { s_tr_np = s_tr_nd = s_tr_ns = 0; s_tr_behind_max = 0; s_tr_send_max = 0; s_tr_host = s_tr_exec = s_tr_g2 = s_tr_h2 = 0; s_tr_t0 = t; }
        tr_add(s_tr_press, &s_tr_np, t);
        s_tr_until = t + 1500000;
    }
    /* (per-key "keys:" log line removed 2026-11-22: suspected in the A-spam hiccups; the press trace covers it) */
    return down;
}

/* ------------------------------------------------------------------------------------------------ */
/* Audio                                                                                             */
/* ------------------------------------------------------------------------------------------------ */

/* Buzzer changes travel from the emulator (core 1) to the audio task (core 0) as timestamped events:
 * "at guest cycle C the tone became H Hz". The audio task plays them back on the guest's own clock,
 * about AUDIO_LATENCY_MS behind the emulator, so every note keeps its exact length and order even
 * though the emulator runs in bursts. If the emulator runs slower than real time, the sound plays
 * slowed down (but complete and at the right pitch). */
#define AUDIO_RATE        16000
#define AUDIO_CHUNK       256          /* samples per write, 16 ms */
#define AUDIO_LATENCY_MS  50
#define TONE_Q            256          /* events in the queue (power of two) */

typedef struct { uint64_t ns; uint32_t hz; } ToneEvt;   /* at guest time ns the tone became hz */
static ToneEvt s_tone_q[TONE_Q];
static volatile uint32_t s_tone_head, s_tone_tail;   /* head: written by emulator, tail: by audio */
static uint32_t s_tone_last;                         /* emulator side: last tone pushed */
static volatile uint64_t s_pub_cycles;               /* guest time in ns reached by the emulator, published for audio */
static volatile uint32_t s_game_hz;                  /* nonzero while a game runs */
/* Guest time bookkeeping (emulator task only): guest time at the start of the current run chunk,
 * the cycle count there, and the guest clock speed (MCLK) during the chunk. */
static uint64_t s_chunk_ns0, s_chunk_c0;
static uint32_t s_chunk_hz = 1;
static inline uint64_t guest_ns_at(uint64_t cycles)
{
    return s_chunk_ns0 + (cycles - s_chunk_c0) * 1000000000ULL / s_chunk_hz;
}
static portMUX_TYPE s_pub_lock = portMUX_INITIALIZER_UNLOCKED;

static void publish_cycles(uint64_t c)
{
    portENTER_CRITICAL(&s_pub_lock); s_pub_cycles = c; portEXIT_CRITICAL(&s_pub_lock);
}
static uint64_t read_cycles(void)
{
    portENTER_CRITICAL(&s_pub_lock); uint64_t c = s_pub_cycles; portEXIT_CRITICAL(&s_pub_lock);
    return c;
}

/* Called by the core after every guest I/O write: if the buzzer tone changed, queue it. */
static void tone_hook(Emu *e)
{
    uint32_t hz = periph_tone_hz(e);
    if (hz == s_tone_last) return;
    if (s_pet_asleep && s_audio_task_h) xTaskNotifyGive(s_audio_task_h);   /* a sound while asleep: wake the audio task */
    uint32_t h = s_tone_head;
    if (h - s_tone_tail >= TONE_Q) return;           /* queue full (should not happen) */
    s_tone_q[h & (TONE_Q - 1)] = (ToneEvt){ guest_ns_at(e->cycles), hz };
    s_tone_head = h + 1;
    s_tone_last = hz;
    s_tone_hz = hz;
}

static void tone_reset(void)
{
    s_tone_tail = s_tone_head;
    s_tone_last = 0;
    s_tone_hz = 0;
}

static bool s_awake_released;                     /* low_power released the no-light-sleep lock */
static void low_power(bool on)                    /* the pet fell asleep (game LCD off) / woke up */
{
    if (on == s_pet_asleep) return;
    if (on) {
        esp_lv_adapter_pause(200);                /* also stops LVGL's 1 ms tick timer */
        s_nap_n = 0; s_nap_ms = 0;
        s_pet_asleep = true;
        /* on USB power the chip stays out of light sleep: otherwise the USB link (and the log) drops (2026-11-22) */
        s_awake_released = !battery_usb_power();
        if (s_pm_awake && s_awake_released) esp_pm_lock_release(s_pm_awake);
    } else {
        if (s_pm_awake && s_awake_released) esp_pm_lock_acquire(s_pm_awake);
        s_awake_released = false;
        s_pet_asleep = false;
        esp_lv_adapter_resume();
        ESP_LOGI(TAG, "Woke up: the emulator napped %lu times, %lu ms in total", (unsigned long)s_nap_n, (unsigned long)s_nap_ms);
    }
}

/* Power saving (2026-11-18): the speaker amplifier and the ES8311 codec are switched off after 1.5 s of
 * silence (esp_codec_dev_close: PA pin off, codec suspended, I2S stopped) and on again for the next sound
 * (~10 ms, well inside the 50 ms audio latency). */
#define AUDIO_OFF_AFTER_MS 10000                 /* in games; in the menus the speaker stays on */
static bool s_audio_open = true;                  /* opened at boot */
#define AUDIO_WAKE_MS 150                         /* codec + amplifier need this long before sound comes out (60 cut the start) */
static int64_t s_audio_on_at;                     /* when it was last switched on */
static void audio_power(bool on)
{
    if (!s_spk_dev || on == s_audio_open) return;
    if (on) {
        esp_codec_dev_sample_info_t fs = { .sample_rate = AUDIO_RATE, .channel = 1, .bits_per_sample = 16 };
        esp_codec_dev_open(s_spk_dev, &fs);
        esp_codec_dev_set_out_vol(s_spk_dev, CODEC_VOLUME);
        s_audio_on_at = esp_timer_get_time();
    } else {
        esp_codec_dev_close(s_spk_dev);
    }
    s_audio_open = on;
}

static void audio_task(void *arg)
{
    int16_t *pcm = malloc(AUDIO_CHUNK * sizeof(int16_t));
    uint64_t aclk = 0;          /* audio's position on the guest clock (ns) */
    bool started = false;
    uint32_t hz = 0;            /* tone being played */
    uint32_t phase = 0;         /* square wave phase, 16.16 fixed point of half periods */
    int64_t last_sound = esp_timer_get_time();   /* when a sample was last not silent */

    while (1) {
        uint32_t ghz = s_game_hz;
        if (s_chime_req && s_spk_dev && (!s_emu_running || s_vlcd_direct)) {   /* menus, also the quick menu */
            /* The real device's select chime (measured from a Plus Color): C6, E6, G6, ~30 ms each. */
            s_chime_req = false;
            if (!s_audio_open) { audio_power(true); vTaskDelay(pdMS_TO_TICKS(AUDIO_WAKE_MS)); }   /* only from the quick menu now */
            last_sound = esp_timer_get_time();
            static const uint16_t notes[3] = { 1046, 1318, 1568 };
            const int per_note = AUDIO_RATE * 30 / 1000, tail = AUDIO_RATE * 40 / 1000;
            int amp = BUZZER_AMPLITUDE * s_sound / 100, n = 0;
            uint32_t ph = 0;
            for (int k = 0; k < 3; k++) {
                uint32_t inc = (uint32_t)(((uint64_t)notes[k] * 2 << 16) / AUDIO_RATE);
                for (int i = 0; i < per_note; i++) {
                    ph += inc;
                    pcm[n++] = (int16_t)(((ph >> 16) & 1) ? amp : -amp);
                    if (n == AUDIO_CHUNK) { esp_codec_dev_write(s_spk_dev, pcm, n * 2); n = 0; }
                }
            }
            for (int i = 0; i < tail; i++) {                /* a little silence so the last note ends cleanly */
                pcm[n++] = 0;
                if (n == AUDIO_CHUNK) { esp_codec_dev_write(s_spk_dev, pcm, n * 2); n = 0; }
            }
            if (n) esp_codec_dev_write(s_spk_dev, pcm, n * 2);
            continue;
        }
        if (!s_spk_dev || !s_emu_running || !ghz) {
            started = false; hz = 0;
            if (!s_audio_open) audio_power(true);         /* menus: speaker stays on, so menu beeps are never cut */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        uint64_t emu = read_cycles();                                   /* guest ns */
        uint64_t lat = (uint64_t)AUDIO_LATENCY_MS * 1000000ULL;
        uint64_t chunk = (uint64_t)AUDIO_CHUNK * 1000000000ULL / AUDIO_RATE;   /* guest ns per chunk at real speed */
        if (emu < lat) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        uint64_t target = emu - lat;
        if (!started || target > aclk + 62 * chunk) { aclk = target > chunk ? target - chunk : 0; started = true; }

        /* How much guest time this chunk covers: normally one chunk; less if the emulator is behind
         * (sound slows down with it), a little more if we are lagging (catch up gently). */
        uint64_t avail = target > aclk ? target - aclk : 0;
        uint64_t adv = avail < chunk ? avail : (avail > 2 * chunk ? chunk + chunk / 4 : chunk);

        if (!s_audio_open && s_pet_asleep && s_tone_tail == s_tone_head) {   /* asleep and silent: nap */
            aclk = target;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));   /* tone_hook wakes us for a sound */
            continue;
        }
        if (!s_audio_open && s_tone_tail != s_tone_head) {   /* a sound is coming: wake the speaker now */
            audio_power(true);
            last_sound = esp_timer_get_time();
            /* the queued sound plays AUDIO_LATENCY_MS after it happened; hold it back a little more if
             * that is not enough for the speaker to wake */
            uint64_t back = (uint64_t)(AUDIO_WAKE_MS - AUDIO_LATENCY_MS + 20) * 1000000ULL;
            aclk = aclk > back ? aclk - back : 0;
        }
        uint32_t inc = 0;
        bool loud = false;
        for (int i = 0; i < AUDIO_CHUNK; i++) {
            uint64_t c = aclk + adv * (uint64_t)i / AUDIO_CHUNK;
            while (s_tone_tail != s_tone_head) {
                ToneEvt *ev = &s_tone_q[s_tone_tail & (TONE_Q - 1)];
                if (ev->ns > c) break;
                hz = ev->hz;
                s_tone_tail++;
            }
            if (hz > 40 && hz < 8000) {
                inc = (uint32_t)(((uint64_t)hz * 2 << 16) / AUDIO_RATE);   /* half periods per sample, 16.16 */
                phase += inc;
                { int amp = BUZZER_AMPLITUDE * s_sound / 100; pcm[i] = (int16_t)(((phase >> 16) & 1) ? amp : -amp); if (amp) loud = true; }
            } else {
                pcm[i] = 0;
            }
        }
        aclk += adv;
        int64_t tnow = esp_timer_get_time();
        if (loud) { last_sound = tnow; audio_power(true); }
        if (s_audio_open && tnow - last_sound > AUDIO_OFF_AFTER_MS * 1000LL) audio_power(false);
        if (s_audio_open) esp_codec_dev_write(s_spk_dev, pcm, AUDIO_CHUNK * sizeof(int16_t));   /* paces this loop */
        else vTaskDelay(pdMS_TO_TICKS(AUDIO_CHUNK * 1000 / AUDIO_RATE));                       /* silent: same pace */
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Loading screen and ROM loading                                                                    */
/* ------------------------------------------------------------------------------------------------ */

static char s_loading_name[24];

/* An original little critter (not a Bandai character) that runs along the loading bar. Two frames: the
 * feet alternate and the body bobs. '#' = dark pixel. */
/* Rolling egg (owner's idea, 2026-11-22) at the end of a loading bar whose top is at y `by`: the frame follows the
 * distance travelled, so it really rolls (one 1/8 turn per 5 px; the egg's outline is ~40 px round). */
static void egg_draw(int which, int fill, int xc, int by)
{
    const uint8_t (*f)[EGG_N] = which == 0 ? EGG_A[(fill / 5) & 7] : EGG_B[(fill / 5) & 7];
    int bot = which == 0 ? EGG_A_BOTTOM[(fill / 5) & 7] : EGG_B_BOTTOM[(fill / 5) & 7];
    int ex = xc - EGG_N / 2, ey = by - 3 - bot;
    static const uint8_t COL[7] = { 0, 0, 1, 2, 3, XSHINE, XSPOTSH };   /* see loading_eggs.h */
    for (int r = 0; r < EGG_N; r++)
        for (int c = 0; c < EGG_N; c++)
            if (f[r][c]) vlcd_rect(ex + c, ey + r, 1, 1, COL[f[r][c]]);
}
/* Minimum loading time (owner 2026-11-22): a game already in the ROM store loads in a blink, so the egg only
 * flashed past. The bar never runs ahead of LOAD_MIN_MS of animation; loading_finish() rolls it to the end. */
#define LOAD_MIN_MS 1600
static int64_t s_load_t0;                          /* start of this loading screen */
static int s_load_pct;                             /* real progress last reported */
static void draw_loading_raw(int percent);
static const char *s_loading_tip, *s_loading_tip2;  /* small text under the bar (copying a big ROM, 2026-11-23) */
static void draw_loading(int percent)
{
    int64_t now = esp_timer_get_time();
    if (percent == 0 || !s_load_t0 || now - s_load_t0 > 60000000LL) s_load_t0 = now;   /* a new loading screen */
    s_load_pct = percent;
    int by_time = (int)((now - s_load_t0) / (LOAD_MIN_MS * 10LL));   /* percent allowed so far */
    draw_loading_raw(percent < by_time ? percent : by_time);
}
static void loading_finish(void)                   /* keep rolling until the minimum time is over, ending at 100% */
{
    if (!s_load_t0) return;
    while (1) {
        int by_time = (int)((esp_timer_get_time() - s_load_t0) / (LOAD_MIN_MS * 10LL));
        draw_loading_raw(by_time > 100 ? 100 : by_time);
        if (by_time >= 100) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    s_load_t0 = 0;
}
static void draw_loading_raw(int percent)
{
    vlcd_clear(0);
    vlcd_text_center(52, "PLEASE WAIT!..", 2);     /* subtitle in the menu font */
    vlcd_clock();                                  /* (also draws the top bar) */
    const int bx = 20, by = 92, bw = VL - 40, bh = 7;
    vlcd_frame(bx, by, bw, bh, 2);
    if (percent > 100) percent = 100;
    int fill = (bw - 4) * percent / 100;
    if (fill > 0) vlcd_rect(bx + 2, by + 2, fill, bh - 4, 2);
    static int frame;
    frame++;
    (void)frame;
    egg_draw(s_loader, fill, bx + 2 + fill, by);
    if (s_loading_tip) {                           /* long loads (a big ROM copied into the fast store): a tip */
        vlcd_text_small_center(108, s_loading_tip, 1);
        if (s_loading_tip2) vlcd_text_small_center(117, s_loading_tip2, 1);
    }
    vlcd_battery();
    s_ov_text = s_loading_name; s_ov_y = 34 * 3;   /* the title, 1.33x menu text, at full resolution */
    vlcd_present();
    s_ov_text = NULL;
}

static void show_error(const char *line1, const char *line2)
{
    vlcd_clear(0);
    vlcd_text_center(64, line1, 2);
    if (line2) vlcd_text_center(76, line2, 2);
    vlcd_text_center(100, "PRESS ANY KEY", 1);
    vlcd_present();
    while (!keys_poll()) vTaskDelay(pdMS_TO_TICKS(10));
}

/* Load a ROM, updating the progress bar. ROMs up to 4 MB are copied into PSRAM. 8 MB ROMs are copied
 * once into the rom_cache flash partition and then mapped (remembered in NVS by file name). */
/* ---- ROM store for 8 MB ROMs (2026-11-18) ----
 * The rom_cache partition (~14 MB) holds 64 KB blocks shared between ROMs: iD L and P's have 51 of 128
 * blocks in common, so both fit at once and switching needs no copying. NVS namespace "romstore":
 *   "hash"  uint32 per physical block (FNV-1a of its 64 KB, 0 = unknown/free)
 *   "roms"  up to RS_MAX_ROMS entries { id = FNV of the file name, stamp = last use }
 *   "m<id>" 128 bytes: physical block of each logical block, 0xFF = all 0xFF (not stored).
 * A ROM already in the store loads instantly. Otherwise each 64 KB block is read from the SD card and
 * either found in the store (same hash and same bytes) or written into a block no ROM uses; if there is
 * none, the least recently used other ROM is dropped. The game reads its ROM through Emu.rom_src[] (one
 * pointer per 64 KB block), so the blocks do not need to be in order. What the game writes to its flash
 * never goes here (overlay slots and the page store, saved in the .sav file). */
#define RS_BLK 0x10000u
#define RS_MAX_ROMS 8
typedef struct { uint32_t id, stamp; } RsRom;
static const esp_partition_t *s_rs_part;
static const uint8_t *s_rs_base;
static esp_partition_mmap_handle_t s_rs_mh;
static uint32_t s_rs_nblk;
static uint32_t *s_rs_hash;
static RsRom s_rs_roms[RS_MAX_ROMS];
static uint8_t s_rom_map[128];                    /* the running ROM: physical block per logical block */
static bool s_rom_in_store;

static uint32_t rs_fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1;
}

static bool rs_open(void)
{
    if (s_rs_base) return true;
    s_rs_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "rom_cache");
    if (!s_rs_part) { ESP_LOGE(TAG, "ROM store: no rom_cache partition"); return false; }
    s_rs_nblk = s_rs_part->size / RS_BLK;
    if (s_rs_nblk > 254) s_rs_nblk = 254;
    const void *p = NULL;
    if (esp_partition_mmap(s_rs_part, 0, (size_t)s_rs_nblk * RS_BLK, ESP_PARTITION_MMAP_DATA, &p, &s_rs_mh) != ESP_OK) {
        ESP_LOGE(TAG, "ROM store: cannot map %lu blocks", (unsigned long)s_rs_nblk);
        return false;
    }
    s_rs_base = p;
    s_rs_hash = heap_caps_calloc(s_rs_nblk, 4, MALLOC_CAP_SPIRAM);
    nvs_handle_t nvs;
    {   /* 2026-11-22: the index points at block positions inside this partition - if the partition moved or
         * changed size (e.g. the two-app-slot layout for updates), forget the index: ROMs are copied again */
        uint32_t geom[2] = { (uint32_t)s_rs_part->address, (uint32_t)s_rs_part->size }, old[2] = { 0, 0 };
        size_t gl = sizeof old; bool have = false;
        if (nvs_open("romstore", NVS_READONLY, &nvs) == ESP_OK) { have = nvs_get_blob(nvs, "geom", old, &gl) == ESP_OK && gl == sizeof old; nvs_close(nvs); }
        bool legacy = !have && geom[0] == 0x210000u && geom[1] == 0xDF0000u;   /* the layout before the check existed */
        if (!have || old[0] != geom[0] || old[1] != geom[1]) {
            if (nvs_open("romstore", NVS_READWRITE, &nvs) == ESP_OK) {
                if (!legacy) { nvs_erase_all(nvs); ESP_LOGW(TAG, "ROM store: partition changed - index cleared, ROMs will be copied again"); }
                nvs_set_blob(nvs, "geom", geom, sizeof geom);
                nvs_commit(nvs); nvs_close(nvs);
            }
        }
    }
    if (nvs_open("romstore", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = s_rs_nblk * 4;
        if (nvs_get_blob(nvs, "hash", s_rs_hash, &len) != ESP_OK) memset(s_rs_hash, 0, s_rs_nblk * 4);
        len = sizeof s_rs_roms;
        if (nvs_get_blob(nvs, "roms", s_rs_roms, &len) != ESP_OK) memset(s_rs_roms, 0, sizeof s_rs_roms);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "ROM store: %lu blocks of 64 KB (%.1f MB)", (unsigned long)s_rs_nblk, s_rs_nblk / 16.0);
    return true;
}

static bool rs_get_map(uint32_t id, uint8_t *map)
{
    char key[12]; snprintf(key, sizeof key, "m%08lx", (unsigned long)id);
    nvs_handle_t nvs; bool ok = false;
    if (nvs_open("romstore", NVS_READONLY, &nvs) == ESP_OK) { size_t len = 128; ok = nvs_get_blob(nvs, key, map, &len) == ESP_OK && len == 128; nvs_close(nvs); }
    return ok;
}

static void rs_save(uint32_t id, const uint8_t *map)   /* map NULL: just hash + list */
{
    nvs_handle_t nvs;
    if (nvs_open("romstore", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, "hash", s_rs_hash, s_rs_nblk * 4);
    nvs_set_blob(nvs, "roms", s_rs_roms, sizeof s_rs_roms);
    if (map) { char key[12]; snprintf(key, sizeof key, "m%08lx", (unsigned long)id); nvs_set_blob(nvs, key, map, 128); }
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void rs_drop(int k)                        /* forget ROM k (its blocks become free unless shared) */
{
    char key[12]; snprintf(key, sizeof key, "m%08lx", (unsigned long)s_rs_roms[k].id);
    nvs_handle_t nvs;
    if (nvs_open("romstore", NVS_READWRITE, &nvs) == ESP_OK) { nvs_erase_key(nvs, key); nvs_commit(nvs); nvs_close(nvs); }
    s_rs_roms[k].id = 0; s_rs_roms[k].stamp = 0;
}

static void rs_used(uint8_t *used, uint32_t skip_id)   /* blocks referenced by the stored ROMs */
{
    memset(used, 0, s_rs_nblk);
    uint8_t m[128];
    for (int k = 0; k < RS_MAX_ROMS; k++)
        if (s_rs_roms[k].id && s_rs_roms[k].id != skip_id && rs_get_map(s_rs_roms[k].id, m))
            for (int b = 0; b < 128; b++) if (m[b] < s_rs_nblk) used[m[b]] = 1;
}

static bool rs_pinned(uint32_t id);
static bool rom_store_copy(const char *filepath, const char *filename, size_t rom_size)   /* the 2026-11-18 loader: copies, dropping the oldest ROM when full */
{
    if (!rs_open()) return false;
    uint32_t id = rs_fnv((const uint8_t *)filename, strlen(filename));
    uint32_t stamp = 1;
    for (int k = 0; k < RS_MAX_ROMS; k++) if (s_rs_roms[k].stamp >= stamp) stamp = s_rs_roms[k].stamp + 1;
    int me = -1;
    for (int k = 0; k < RS_MAX_ROMS; k++) if (s_rs_roms[k].id == id) me = k;
    if (me >= 0 && rs_get_map(id, s_rom_map)) {   /* already stored: instant */
        s_rs_roms[me].stamp = stamp;
        rs_save(id, NULL);
        ESP_LOGI(TAG, "ROM store: '%s' already stored", filename);
        draw_loading(100);
        s_rom_in_store = true;
        return true;
    }
    FILE *f = fopen(filepath, "rb");
    if (!f) { ESP_LOGE(TAG, "Cannot open file on SD: %s", filepath); return false; }
    ESP_LOGI(TAG, "ROM store: adding '%s'...", filename);
    s_loading_tip = "FIRST LOAD TAKES LONGER."; s_loading_tip2 = "TIP: SETTINGS > FAST LOAD";
    if (me >= 0) rs_drop(me);                     /* a half-built entry never counts as stored */
    rs_save(0, NULL);
    draw_loading(0);
    uint8_t *used = heap_caps_calloc(s_rs_nblk, 1, MALLOC_CAP_SPIRAM), *mine = heap_caps_calloc(s_rs_nblk, 1, MALLOC_CAP_SPIRAM);
    uint8_t *sd = heap_caps_malloc(RS_BLK, MALLOC_CAP_SPIRAM), *fl = heap_caps_malloc(RS_BLK, MALLOC_CAP_SPIRAM);
    bool ok = used && mine && sd && fl;
    if (ok) rs_used(used, id);
    int64_t t0 = esp_timer_get_time(), t_sd = 0, t_wr = 0;
    int n_empty = 0, n_shared = 0, n_written = 0, last_pct = -1;
    for (uint32_t b = 0; ok && b < 128; b++) {
        s_rom_map[b] = 0xFF;
        if ((size_t)b * RS_BLK >= rom_size) continue;
        int64_t t = esp_timer_get_time();
        size_t n = fread(sd, 1, RS_BLK, f);
        t_sd += esp_timer_get_time() - t;
        if (n < RS_BLK) memset(sd + n, 0xFF, RS_BLK - n);
        bool empty = true;
        for (uint32_t i = 0; i < RS_BLK; i += 4) if (*(const uint32_t *)(sd + i) != 0xFFFFFFFFu) { empty = false; break; }
        if (empty) { n_empty++; goto next; }
        {
            uint32_t h = rs_fnv(sd, RS_BLK);
            int found = -1;
            for (uint32_t k = 0; k < s_rs_nblk && found < 0; k++)
                if (s_rs_hash[k] == h && esp_partition_read(s_rs_part, (size_t)k * RS_BLK, fl, RS_BLK) == ESP_OK && memcmp(fl, sd, RS_BLK) == 0)
                    found = (int)k;
            if (found >= 0) { s_rom_map[b] = (uint8_t)found; mine[found] = 1; n_shared++; goto next; }
            /* a free block: no stored ROM and not this ROM uses it (fresh ones first) */
            int pick = -1;
            for (int pass = 0; pass < 3 && pick < 0; pass++) {
                for (uint32_t k = 0; k < s_rs_nblk && pick < 0; k++)
                    if (!used[k] && !mine[k] && (pass > 0 || s_rs_hash[k] == 0)) pick = (int)k;
                if (pick < 0 && pass >= 1) {          /* full: drop the least recently used other ROM */
                    int lru = -1;
                    for (int k = 0; k < RS_MAX_ROMS; k++) if (s_rs_roms[k].id && s_rs_roms[k].id != id && !rs_pinned(s_rs_roms[k].id) && (lru < 0 || s_rs_roms[k].stamp < s_rs_roms[lru].stamp)) lru = k;
                    if (lru < 0) break;
                    ESP_LOGW(TAG, "ROM store: full, dropping the least recently used ROM");
                    rs_drop(lru); rs_save(0, NULL); rs_used(used, id);
                    pass = 0;
                }
            }
            if (pick < 0) { ESP_LOGE(TAG, "ROM store: no room"); ok = false; break; }
            t = esp_timer_get_time();
            s_rs_hash[pick] = 0;
            esp_partition_read(s_rs_part, (size_t)pick * RS_BLK, fl, RS_BLK);
            bool fl_empty = true;
            for (uint32_t i = 0; i < RS_BLK; i += 4) if (*(const uint32_t *)(fl + i) != 0xFFFFFFFFu) { fl_empty = false; break; }
            if (!fl_empty && esp_partition_erase_range(s_rs_part, (size_t)pick * RS_BLK, RS_BLK) != ESP_OK) ok = false;
            for (uint32_t pg = 0; ok && pg < RS_BLK; pg += 4096) {
                bool pe = true;
                for (uint32_t i = 0; i < 4096; i += 4) if (*(const uint32_t *)(sd + pg + i) != 0xFFFFFFFFu) { pe = false; break; }
                if (!pe && esp_partition_write(s_rs_part, (size_t)pick * RS_BLK + pg, sd + pg, 4096) != ESP_OK) ok = false;
            }
            t_wr += esp_timer_get_time() - t;
            if (!ok) break;
            s_rs_hash[pick] = h;
            s_rom_map[b] = (uint8_t)pick; mine[pick] = 1; n_written++;
        }
    next:
        { int pct = (int)((b + 1) * 100 / 128); if (pct / 2 != last_pct / 2) { last_pct = pct; draw_loading(pct); } }
    }
    fclose(f);
    free(used); free(mine); free(sd); free(fl);
    s_loading_tip = s_loading_tip2 = NULL;
    ESP_LOGI(TAG, "ROM store: '%s' %s in %.1f s (SD %.1f s, writing %.1f s) | %d blocks shared, %d written, %d empty",
             filename, ok ? "stored" : "FAILED", (esp_timer_get_time() - t0) / 1e6, t_sd / 1e6, t_wr / 1e6, n_shared, n_written, n_empty);
    if (!ok) { rs_save(0, NULL); return false; }
    int slot = -1;
    for (int k = 0; k < RS_MAX_ROMS; k++) if (!s_rs_roms[k].id) { slot = k; break; }
    if (slot < 0) {                               /* list full: replace the oldest */
        slot = 0;
        for (int k = 1; k < RS_MAX_ROMS; k++) if (s_rs_roms[k].stamp < s_rs_roms[slot].stamp) slot = k;
        rs_drop(slot);
    }
    s_rs_roms[slot].id = id; s_rs_roms[slot].stamp = stamp;
    rs_save(id, s_rom_map);
    s_rom_in_store = true;
    return true;
}


/* OVERFLOW INTO MEMORY (2026-11-22, owner chose option 1): with the two app slots the store holds 190 blocks, so
 * iD L (65) + P's (68) + 4U+ (65) no longer fit together, and every switch dropped one and copied it again (~27 s).
 * Now a new ROM is first read into PSRAM (64 KB blocks; empty blocks and blocks already in the store are not kept).
 * If its new blocks fit in the store's free space they are written there (as before, never dropping anything);
 * if not, the game runs from those PSRAM blocks and nothing is dropped. Such a ROM is read from the SD card each
 * time (a few seconds; the empty blocks are remembered in NVS "x<id>" and skipped). Stored ROMs whose files are
 * no longer on the SD card are forgotten when the ROM list is read (rs_prune). If PSRAM runs short, the old
 * copy-and-drop loader is used. */
static uint8_t *s_rom_ram[128];                   /* PSRAM blocks of the running ROM (overflow), NULL = store/empty */
static bool s_rom_overflow;
#define RS_RAM_MARGIN (900 * 1024)                /* PSRAM left free for the game after an overflow ROM */
static void rom_ram_free(void)
{
    for (int b = 0; b < 128; b++) { free(s_rom_ram[b]); s_rom_ram[b] = NULL; }
    s_rom_overflow = false;
}
static void rs_prune(const uint32_t *ids, int n)  /* forget stored ROMs that are no longer on the SD card */
{
    if (!rs_open() || n <= 0) return;
    bool changed = false;
    for (int k = 0; k < RS_MAX_ROMS; k++) {
        if (!s_rs_roms[k].id) continue;
        bool there = false;
        for (int i = 0; i < n; i++) if (ids[i] == s_rs_roms[k].id) there = true;
        if (!there) { rs_drop(k); changed = true; ESP_LOGI(TAG, "ROM store: a stored ROM is no longer on the SD card - its space is free again"); }
    }
    if (changed) rs_save(0, NULL);
}
/* FAST LOAD choices (owner 2026-11-22): PINNED games are kept in the store for good (the automatic rule never
 * removes them); otherwise a game that loads from memory but is played clearly more often (PLAY_MARGIN more
 * starts) than the least-played unpinned stored game takes its place (a one-off copy). NVS romstore: "pins"
 * (ids), "plays" ({id, starts} for each 8 MB ROM). */
#define RS_MAX_PINS 8
#define RS_MAX_PLAYS 16
#define PLAY_MARGIN 3
typedef struct { uint32_t id; uint16_t n; uint16_t pad; } RsPlay;
static uint32_t s_rs_pins[RS_MAX_PINS];
static RsPlay s_rs_plays[RS_MAX_PLAYS];
static bool s_rs_pp_loaded;
static void rs_pp_load(void)
{
    if (s_rs_pp_loaded) return;
    nvs_handle_t nvs;
    if (nvs_open("romstore", NVS_READONLY, &nvs) == ESP_OK) {
        size_t l = sizeof s_rs_pins; if (nvs_get_blob(nvs, "pins", s_rs_pins, &l) != ESP_OK) memset(s_rs_pins, 0, sizeof s_rs_pins);
        l = sizeof s_rs_plays; if (nvs_get_blob(nvs, "plays", s_rs_plays, &l) != ESP_OK) memset(s_rs_plays, 0, sizeof s_rs_plays);
        nvs_close(nvs);
    }
    s_rs_pp_loaded = true;
}
static void rs_pp_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open("romstore", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, "pins", s_rs_pins, sizeof s_rs_pins);
    nvs_set_blob(nvs, "plays", s_rs_plays, sizeof s_rs_plays);
    nvs_commit(nvs); nvs_close(nvs);
}
static bool rs_pinned(uint32_t id) { rs_pp_load(); for (int i = 0; i < RS_MAX_PINS; i++) if (s_rs_pins[i] == id) return true; return false; }
static void rs_set_pin(uint32_t id, bool on)
{
    rs_pp_load();
    for (int i = 0; i < RS_MAX_PINS; i++) if (s_rs_pins[i] == id) s_rs_pins[i] = 0;
    if (on) for (int i = 0; i < RS_MAX_PINS; i++) if (!s_rs_pins[i]) { s_rs_pins[i] = id; break; }
    rs_pp_save();
}
static int rs_plays(uint32_t id) { rs_pp_load(); for (int i = 0; i < RS_MAX_PLAYS; i++) if (s_rs_plays[i].id == id) return s_rs_plays[i].n; return 0; }
static void rs_count_play(uint32_t id)
{
    rs_pp_load();
    int k = -1;
    for (int i = 0; i < RS_MAX_PLAYS; i++) if (s_rs_plays[i].id == id) k = i;
    if (k < 0) {                                   /* new: take a free entry, else the least played */
        k = 0;
        for (int i = 0; i < RS_MAX_PLAYS; i++) if (!s_rs_plays[i].id) { k = i; break; } else if (s_rs_plays[i].n < s_rs_plays[k].n) k = i;
        s_rs_plays[k].id = id; s_rs_plays[k].n = 0;
    }
    if (s_rs_plays[k].n < 60000) s_rs_plays[k].n++;
    rs_pp_save();
}
static bool rs_is_stored(uint32_t id) { if (!rs_open()) return false; for (int k = 0; k < RS_MAX_ROMS; k++) if (s_rs_roms[k].id == id) return true; return false; }
static bool rom_store_load(const char *filepath, const char *filename, size_t rom_size)
{
    rom_ram_free();
    if (!rs_open()) return false;
    uint32_t id = rs_fnv((const uint8_t *)filename, strlen(filename));
    rs_count_play(id);
    int me = -1;
    for (int k = 0; k < RS_MAX_ROMS; k++) if (s_rs_roms[k].id == id) me = k;
    if (me >= 0) return rom_store_copy(filepath, filename, rom_size);   /* already stored: instant */
    FILE *f = fopen(filepath, "rb");
    if (!f) { ESP_LOGE(TAG, "Cannot open file on SD: %s", filepath); return false; }
    char key[12]; snprintf(key, sizeof key, "x%08lx", (unsigned long)id);
    uint8_t emp[16] = { 0 }; bool have_emp = false;
    { nvs_handle_t nvs; if (nvs_open("romstore", NVS_READONLY, &nvs) == ESP_OK) { size_t l = 16; have_emp = nvs_get_blob(nvs, key, emp, &l) == ESP_OK && l == 16; nvs_close(nvs); } }
    uint8_t *used = heap_caps_calloc(s_rs_nblk, 1, MALLOC_CAP_SPIRAM), *mine = heap_caps_calloc(s_rs_nblk, 1, MALLOC_CAP_SPIRAM);
    uint8_t *fl = heap_caps_malloc(RS_BLK, MALLOC_CAP_SPIRAM);
    uint32_t *hsh = heap_caps_calloc(128, 4, MALLOC_CAP_SPIRAM);
    if (!used || !mine || !fl || !hsh) { fclose(f); free(used); free(mine); free(fl); free(hsh); return false; }
    rs_used(used, id);
    int n_free = 0;
    for (uint32_t k = 0; k < s_rs_nblk; k++) if (!used[k]) n_free++;
    draw_loading(0);
    int64_t t0 = esp_timer_get_time();
    int n_new = 0, n_empty = 0, n_shared = 0, last_pct = -1;
    bool ok = true, nomem = false, lowmem = false;
    uint8_t emp_new[16] = { 0 };
    uint8_t need[128] = { 0 };                    /* new blocks not kept in PSRAM (low memory): read again to store */
    uint8_t *scratch = NULL;
    for (uint32_t b = 0; ok && b < 128; b++) {    /* 1: read the ROM (into PSRAM, minus empty and shared blocks) */
        s_rom_map[b] = 0xFF;
        if ((size_t)b * RS_BLK >= rom_size) { emp_new[b >> 3] |= (uint8_t)(1 << (b & 7)); continue; }
        if (have_emp && (emp[b >> 3] & (1 << (b & 7)))) { emp_new[b >> 3] |= (uint8_t)(1 << (b & 7)); n_empty++; continue; }
        /* low memory (2026-11-23): go on counting with one scratch block instead of giving up (a big iD L
         * edition needs ~5 MB, more than is free); the blocks are read again from the SD card if they fit */
        if (!lowmem && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < RS_RAM_MARGIN + RS_BLK) lowmem = true;
        uint8_t *buf = NULL;
        if (lowmem) { if (!scratch) scratch = heap_caps_malloc(RS_BLK, MALLOC_CAP_SPIRAM); buf = scratch; }
        else buf = heap_caps_malloc(RS_BLK, MALLOC_CAP_SPIRAM);
        if (!buf) { nomem = true; break; }
#define FREE_BUF() do { if (buf != scratch) free(buf); } while (0)
        if (fseek(f, (long)b * RS_BLK, SEEK_SET) != 0) { FREE_BUF(); ok = false; break; }
        size_t n = fread(buf, 1, RS_BLK, f);
        if (n < RS_BLK) memset(buf + n, 0xFF, RS_BLK - n);
        bool empty = true;
        for (uint32_t i = 0; i < RS_BLK; i += 4) if (*(const uint32_t *)(buf + i) != 0xFFFFFFFFu) { empty = false; break; }
        if (empty) { FREE_BUF(); emp_new[b >> 3] |= (uint8_t)(1 << (b & 7)); n_empty++; }
        else {
            uint32_t h = rs_fnv(buf, RS_BLK);
            int found = -1;
            for (uint32_t k = 0; k < s_rs_nblk && found < 0; k++)
                if (used[k] && s_rs_hash[k] == h && esp_partition_read(s_rs_part, (size_t)k * RS_BLK, fl, RS_BLK) == ESP_OK && memcmp(fl, buf, RS_BLK) == 0)
                    found = (int)k;
            if (found >= 0) { FREE_BUF(); s_rom_map[b] = (uint8_t)found; mine[found] = 1; n_shared++; }   /* (kept if its owner is dropped) */
            else if (buf == scratch) { need[b] = 1; hsh[b] = h; n_new++; }
            else { s_rom_ram[b] = buf; hsh[b] = h; n_new++; }
#undef FREE_BUF
        }
        int pct = (int)((b + 1) * 30 / 128);
        if (pct / 2 != last_pct / 2) { last_pct = pct; draw_loading(pct); }
    }
    int64_t t_read = esp_timer_get_time() - t0;
    if (ok && nomem) {                            /* no memory even for one block: the old way (respects pins) */
        ESP_LOGW(TAG, "ROM store: not enough memory to read '%s' first - copying it the old way", filename);
        fclose(f); free(scratch);
        rom_ram_free(); free(used); free(mine); free(fl); free(hsh);
        return rom_store_copy(filepath, filename, rom_size);
    }
    if (lowmem) ESP_LOGI(TAG, "ROM store: memory low while reading '%s' - blocks will be read again from the SD card", filename);
    if (ok && n_new > n_free) {                   /* doesn't fit: pinned, or played clearly more than a stored game? */
        bool pin = rs_pinned(id);
        int mine_plays = rs_plays(id);
        while (n_new > n_free) {
            int victim = -1;                          /* the least played unpinned stored game */
            for (int k = 0; k < RS_MAX_ROMS; k++)
                if (s_rs_roms[k].id && s_rs_roms[k].id != id && !rs_pinned(s_rs_roms[k].id) &&
                    (victim < 0 || rs_plays(s_rs_roms[k].id) < rs_plays(s_rs_roms[victim].id))) victim = k;
            if (victim < 0) break;
            if (!pin && mine_plays < rs_plays(s_rs_roms[victim].id) + PLAY_MARGIN) break;
            ESP_LOGI(TAG, "ROM store: making '%s' fast (%s, %d starts) - another game (%d starts) goes back to loading from memory",
                     filename, pin ? "pinned" : "played more", mine_plays, rs_plays(s_rs_roms[victim].id));
            rs_drop(victim); rs_save(0, NULL);
            rs_used(used, id);
            n_free = 0;
            for (uint32_t k = 0; k < s_rs_nblk; k++) if (!used[k] && !mine[k]) n_free++;
        }
    }
    if (ok && n_new <= n_free) {                  /* 2a: fits - write the new blocks into free store blocks */
        int64_t t1 = esp_timer_get_time();
        s_loading_tip = "FIRST LOAD TAKES LONGER."; s_loading_tip2 = "TIP: SETTINGS > FAST LOAD";
        int done = 0;
        for (uint32_t b = 0; ok && b < 128; b++) {
            if (!s_rom_ram[b] && !need[b]) continue;
            if (need[b]) {                            /* not kept: read it again */
                if (fseek(f, (long)b * RS_BLK, SEEK_SET) != 0) { ok = false; break; }
                size_t nr = fread(scratch, 1, RS_BLK, f);
                if (nr < RS_BLK) memset(scratch + nr, 0xFF, RS_BLK - nr);
                if (rs_fnv(scratch, RS_BLK) != hsh[b]) { ok = false; break; }
            }
            int pick = -1;
            for (int pass = 0; pass < 2 && pick < 0; pass++)
                for (uint32_t k = 0; k < s_rs_nblk && pick < 0; k++)
                    if (!used[k] && !mine[k] && (pass > 0 || s_rs_hash[k] == 0)) pick = (int)k;
            if (pick < 0) { ok = false; break; }
            const uint8_t *src = need[b] ? scratch : s_rom_ram[b];
            s_rs_hash[pick] = 0;
            esp_partition_read(s_rs_part, (size_t)pick * RS_BLK, fl, RS_BLK);
            bool fl_empty = true;
            for (uint32_t i = 0; i < RS_BLK; i += 4) if (*(const uint32_t *)(fl + i) != 0xFFFFFFFFu) { fl_empty = false; break; }
            if (!fl_empty && esp_partition_erase_range(s_rs_part, (size_t)pick * RS_BLK, RS_BLK) != ESP_OK) ok = false;
            for (uint32_t pg = 0; ok && pg < RS_BLK; pg += 4096) {
                bool pe = true;
                for (uint32_t i = 0; i < 4096; i += 4) if (*(const uint32_t *)(src + pg + i) != 0xFFFFFFFFu) { pe = false; break; }
                if (!pe && esp_partition_write(s_rs_part, (size_t)pick * RS_BLK + pg, src + pg, 4096) != ESP_OK) ok = false;
            }
            if (!ok) break;
            s_rs_hash[pick] = hsh[b];
            s_rom_map[b] = (uint8_t)pick; mine[pick] = 1;
            free(s_rom_ram[b]); s_rom_ram[b] = NULL; need[b] = 0;
            done++;
            int pct = 30 + done * 70 / (n_new ? n_new : 1);
            if (pct / 2 != last_pct / 2) { last_pct = pct; draw_loading(pct); }
        }
        ESP_LOGI(TAG, "ROM store: '%s' %s in %.1f s (reading %.1f s, writing %.1f s) | %d blocks shared, %d written, %d empty, %d free left",
                 filename, ok ? "stored" : "FAILED", (esp_timer_get_time() - t0) / 1e6, t_read / 1e6, (esp_timer_get_time() - t1) / 1e6,
                 n_shared, n_new, n_empty, n_free - n_new);
        s_loading_tip = s_loading_tip2 = NULL;
        rom_ram_free(); free(used); free(mine); free(fl); free(hsh); fclose(f); free(scratch);
        if (!ok) { rs_save(0, NULL); return false; }
        uint32_t stamp = 1;
        for (int k = 0; k < RS_MAX_ROMS; k++) if (s_rs_roms[k].stamp >= stamp) stamp = s_rs_roms[k].stamp + 1;
        int slot = -1;
        for (int k = 0; k < RS_MAX_ROMS; k++) if (!s_rs_roms[k].id) { slot = k; break; }
        if (slot < 0) {                           /* list full: replace the oldest */
            slot = 0;
            for (int k = 1; k < RS_MAX_ROMS; k++) if (s_rs_roms[k].stamp < s_rs_roms[slot].stamp) slot = k;
            rs_drop(slot);
        }
        s_rs_roms[slot].id = id; s_rs_roms[slot].stamp = stamp;
        rs_save(id, s_rom_map);
        s_rom_in_store = true;
        return true;
    }
    free(used); free(mine); free(fl); free(hsh); fclose(f); free(scratch);
    if (!ok) { rom_ram_free(); return false; }
    if (lowmem) {                                 /* doesn't fit and can't all be held in memory: the old way */
        ESP_LOGW(TAG, "ROM store: '%s' does not fit and memory is short - copying it the old way", filename);
        rom_ram_free();
        return rom_store_copy(filepath, filename, rom_size);
    }
    /* 2b: does not fit - run from PSRAM, keep the store as it is */
    if (memcmp(emp, emp_new, 16)) { nvs_handle_t nvs; if (nvs_open("romstore", NVS_READWRITE, &nvs) == ESP_OK) { nvs_set_blob(nvs, key, emp_new, 16); nvs_commit(nvs); nvs_close(nvs); } }
    s_rom_overflow = true;
    s_rom_in_store = true;
    ESP_LOGI(TAG, "ROM store full: '%s' runs from memory (%d blocks, %.1f MB; %d shared with the store, %d empty; read in %.1f s; PSRAM left %u KB)",
             filename, n_new, n_new / 16.0, n_shared, n_empty, t_read / 1e6, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    draw_loading(100);
    return true;
}

/* Point the emulator's ROM blocks at the store or the PSRAM copies (after tc_init). */
static void rom_apply_map(Emu *e)
{
    if (!s_rom_in_store) return;
    for (uint32_t b = 0; b < 128; b++) {
        const uint8_t *p = (b << 16) >= e->dev.rom_size ? NULL : s_rom_ram[b] ? s_rom_ram[b]
                         : s_rom_map[b] < s_rs_nblk ? s_rs_base + (size_t)s_rom_map[b] * RS_BLK : NULL;
        e->rom_src[b] = p; e->rom_blk[b] = p;
    }
    e->rom_blk[e->par_base >> 16] = 0;
    for (uint32_t b = 0; b < 128; b++) tc_blk_update(e, b);
    /* tc_init read the reset vector through the unmapped store (wrong block): read it again now. All callers
     * call this right after tc_init (a loaded save sets the PC afterwards anyway). Fixed 2026-11-21: fresh
     * starts, RESTART ROM and the benchmark of 8 MB ROMs jumped to garbage. */
    e->pc = mem_read32(e, e->ttbr);
}

static uint8_t *load_rom_storage(const char *filepath, const char *filename, size_t rom_size, bool *out_is_mmap)
{
    const size_t CHUNK = 64 * 1024;
    int last_pct = -1;
    s_rom_in_store = false;
    *out_is_mmap = false;

    if (rom_size <= 4 * 1024 * 1024) {
        FILE *f = fopen(filepath, "rb");
        if (!f) return NULL;
        uint8_t *buf = heap_caps_calloc(1, rom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) { fclose(f); return NULL; }
        size_t off = 0;
        while (off < rom_size) {
            size_t n = fread(buf + off, 1, (rom_size - off) < CHUNK ? (rom_size - off) : CHUNK, f);
            if (n == 0) break;
            off += n;
            int pct = (int)(off * 100 / rom_size);
            if (pct / 2 != last_pct / 2) { last_pct = pct; draw_loading(pct); }
        }
        fclose(f);
        *out_is_mmap = false;
        return buf;
    }

    return rom_store_load(filepath, filename, rom_size) ? (uint8_t *)s_rs_base : NULL;
}

/* ------------------------------------------------------------------------------------------------ */
/* Emulator task                                                                                     */
/* ------------------------------------------------------------------------------------------------ */

#if TL_PROFILE
static bool s_a0_dumped;
#define PROF_N 1024
static uint32_t *s_prof_pc, *s_prof_cnt, s_prof_total, s_prof_lost;   /* tables in PSRAM */
static void prof_hit(uint32_t pc)
{
    if (!s_prof_pc) {
        s_prof_pc = heap_caps_calloc(PROF_N, 4, MALLOC_CAP_SPIRAM);
        s_prof_cnt = heap_caps_calloc(PROF_N, 4, MALLOC_CAP_SPIRAM);
        if (!s_prof_pc || !s_prof_cnt) return;
    }
    uint32_t h = (pc * 2654435761u) >> 22;          /* 10-bit hash */
    for (int i = 0; i < 8; i++) {
        uint32_t k = (h + i) & (PROF_N - 1);
        if (s_prof_cnt[k] && s_prof_pc[k] == pc) { s_prof_cnt[k]++; s_prof_total++; return; }
        if (!s_prof_cnt[k]) { s_prof_pc[k] = pc; s_prof_cnt[k] = 1; s_prof_total++; return; }
    }
    s_prof_lost++;
}
static void prof_report(void)
{
    if (!s_prof_total || !s_prof_cnt) return;
    printf("PROF %lu samples (%lu lost). Top guest PCs:\n", (unsigned long)s_prof_total, (unsigned long)s_prof_lost);
    uint32_t dumped[3]; int nd = 0;   /* code dumps: 0x100 bytes around each */
    for (int n = 0; n < 24; n++) {
        int best = -1;
        for (int k = 0; k < PROF_N; k++) if (s_prof_cnt[k] && (best < 0 || s_prof_cnt[k] > s_prof_cnt[best])) best = k;
        if (best < 0) break;
        printf("PROF %06lx %5.1f%%\n", (unsigned long)s_prof_pc[best], 100.0 * s_prof_cnt[best] / s_prof_total);
        if (nd < 3 && s_prof_pc[best] >= 0x10000u) {   /* dump code of the hottest flash routines (RAM code is dumped once below) */
            bool near = false;
            for (int i = 0; i < nd; i++) if (s_prof_pc[best] - dumped[i] + 0x80u < 0x100u) near = true;
            if (!near) dumped[nd++] = s_prof_pc[best];
        }
        s_prof_cnt[best] = 0;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    for (int i = 0; i < nd; i++) {                    /* raw code around the hottest spots, for disassembly */
        uint32_t a0 = (dumped[i] - 0x80u) & ~1u;
        for (uint32_t a = a0; a < a0 + 0x100u; a += 32) {
            printf("CODE %06lx:", (unsigned long)a);
            for (uint32_t j = 0; j < 32; j += 2) printf(" %04x", mem_read16(s_emu, a + j));
            printf("\n");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_a0_dumped) {                               /* once per game: the RAM code area, for disassembly */
        s_a0_dumped = true;
        for (uint32_t a = 0x300; a < 0x800; a += 32) {
            printf("CODE %06lx:", (unsigned long)a);
            for (uint32_t j = 0; j < 32; j += 2) printf(" %04x", mem_read16(s_emu, a + j));
            printf("\n");
            if ((a & 0xff) == 0xe0) vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    memset(s_prof_cnt, 0, PROF_N * 4); s_prof_total = s_prof_lost = 0;
}
#endif

/* ------------------------------------------------------------------------------------------------ */
/* Saving progress                                                                                    */
/* A save holds the whole emulator state (CPU, RAM, I/O incl. the game clock, LCD), the flash sectors the */
/* game has written (overlay slots, where the pet's data lives) and the screen. Pointers inside Emu are   */
/* rebuilt on restore. File: /sdcard/roms/<rom>.sav, written via a .tmp file so a power cut keeps the old one. */
/* ------------------------------------------------------------------------------------------------ */
#define SAVE_MAGIC   0x31534C54u                   /* "TLS1" */
#define SAVE_EVERY_S 300                           /* periodic save while playing */
typedef struct { uint32_t magic, emu_size, ovl_slots, ovl_size, gram_bytes; } SaveHdr;
#define SAVE_POOL_BYTES ((size_t)OVL_SLOTS * OVL_SIZE)
#define SAVE_GRAM_BYTES ((size_t)GRAM_W * GRAM_H * 2)

static uint8_t *s_snap;                            /* PSRAM: Emu + overlay pool + screen */
static void (*volatile s_emu_job)(void *);          /* DLC: work the emulator task does between run chunks */
static void *volatile s_emu_job_arg;
static volatile bool s_emu_job_done;
static volatile bool s_snap_req, s_snap_done;      /* UI asks, emulator task copies between run chunks */
static volatile bool s_export_req;
static volatile bool s_quit_req;                   /* quick menu SAVE & QUIT: leave the game (saved as usual) */                 /* quick menu: leave the game and export its flash as a ROM file */

/* ---- Flash page store (2026-11-18). The games keep their save data in their flash chip. Writes into the
 * small 8 KB sectors live in the 8 overlay slots; erasing or rewriting a big 64 KB sector needs more room.
 * Before, those writes were thrown away (flash_lost_writes) and P's hung after buying an item, waiting
 * for data that never arrived. Now an erased 64 KB block is marked "paged" (pg_blk / pg_map in Emu: each
 * 8 KB part 0xFF = erased or a page of this pool), and when the overlay slots run out the oldest written
 * part is moved into a page. The used pages are written into the save file after the normal data. */
#define PG_PAGES 128                              /* 128 x 8 KB = 1 MB (PSRAM): room for imported items, VDPs */
#define PG_MAGIC 0x31504750u                      /* "PGP1" */
static uint8_t *s_pg_pool, *s_pg_snap;
static uint32_t s_pg_snap_n;

static inline bool pg_block(const Emu *e, uint32_t b) { return (e->pg_blk[b >> 5] >> (b & 31u)) & 1u; }

static uint32_t pg_used_max(const Emu *e)         /* highest page in use + 1 */
{
    uint32_t n = 0;
    for (uint32_t b = 0; b < 128; b++)
        if (pg_block(e, b)) for (int p = 0; p < 8; p++) { uint8_t k = e->pg_map[b * 8 + p]; if (k < PG_PAGES && k + 1u > n) n = k + 1u; }
    return n;
}

static int pg_alloc(const Emu *e)
{
    uint8_t used[PG_PAGES] = {0};
    for (uint32_t b = 0; b < 128; b++)
        if (pg_block(e, b)) for (int p = 0; p < 8; p++) { uint8_t k = e->pg_map[b * 8 + p]; if (k < PG_PAGES) used[k] = 1; }
    for (int k = 0; k < PG_PAGES; k++) if (!used[k]) return k;
    return -1;
}

static int pg_big_erase(Emu *e, uint32_t base)    /* tc_big_erase: a 64 KB sector was erased */
{
    if (!s_pg_pool) return 0;
    uint32_t b = (base >> 16) & 127u;
    e->pg_blk[b >> 5] |= 1u << (b & 31u);
    memset(&e->pg_map[b * 8], 0xFF, 8);           /* all 8 parts erased; their pages become free */
    ESP_LOGI(TAG, "Flash: game erased 64 KB block %lu", (unsigned long)b);
    return 1;
}

/* 2026-11-23: a part that still matches the ROM takes no page (pg_map 0xFE = "the original ROM"). Before, the first
 * write into a 64 KB block copied all 8 parts into pages, so the 4U English patch (it rewrites many blocks) used
 * up the 128 pages after ~16 blocks, writes were lost and the game crashed. */
static bool pg_same_as_rom(const Emu *e, uint32_t b, uint32_t part, const uint8_t *d)   /* 1 = equal, for 0xFE */
{
    const uint8_t *r = e->rom_src[b];
    if (!r) return false;
    return memcmp(d, r + part * OVL_SIZE, OVL_SIZE) == 0;
}
static bool pg_all_ff(const uint8_t *d) { for (uint32_t i = 0; i < OVL_SIZE; i += 4) if (*(const uint32_t *)(d + i) != 0xFFFFFFFFu) return false; return true; }
static int pg_evict(Emu *e)                       /* tc_ovl_evict: overlay slots full, move one into a page */
{
    if (!s_pg_pool) return 0;
    for (int i = 0; i < e->ovl.n; i++) {
        uint32_t base = e->ovl.base[i];
        if (base == 0xFFFFFFFFu || base - e->par_base < 0x10000u) continue;   /* keep parameter sectors */
        uint32_t b = (base >> 16) & 127u, part = (base >> 13) & 7u;
        if (!pg_block(e, b)) {                    /* block becomes paged: its other parts stay the ROM's (no copy) */
            e->pg_blk[b >> 5] |= 1u << (b & 31u);
            memset(&e->pg_map[b * 8], e->rom_src[b] ? 0xFE : 0xFF, 8);
        }
        const uint8_t *d = e->ovl.mem[i];
        uint8_t k = e->pg_map[b * 8 + part];
        if (pg_same_as_rom(e, b, part, d)) k = 0xFE;          /* written back unchanged: no page needed */
        else if (pg_all_ff(d)) k = 0xFF;                     /* erased */
        else {
            if (k >= PG_PAGES) { int nk = pg_alloc(e); if (nk < 0) { ESP_LOGW(TAG, "Flash: page store full (%d pages)", PG_PAGES); return 0; } k = (uint8_t)nk; }
            memcpy(s_pg_pool + (uint32_t)k * OVL_SIZE, d, OVL_SIZE);
        }
        e->pg_map[b * 8 + part] = k;
        tc_ovl_free(e, i);
        return 1;
    }
    return 0;
}

static void snap_capture(void)                     /* emulator must be between chunks or stopped */
{
    s_pg_snap_n = s_pg_pool ? pg_used_max(s_emu) : 0;
    if (s_pg_snap_n) memcpy(s_pg_snap, s_pg_pool, s_pg_snap_n * OVL_SIZE);
    memcpy(s_snap, s_emu, sizeof(Emu));
    memcpy(s_snap + sizeof(Emu), s_ovl_pool, SAVE_POOL_BYTES);
    memcpy(s_snap + sizeof(Emu) + SAVE_POOL_BYTES, s_gram, SAVE_GRAM_BYTES);
}

/* Put a captured state back into e, which tc_init has just set up for the same ROM. */
static void snap_restore(Emu *e, const uint8_t *snap)
{
    const uint8_t *rom = e->rom, *rom_fast = e->rom_fast, *pg_base = e->pg_base;
    uint32_t rom_fast_len = e->rom_fast_len;
    const uint8_t *rom_src[128]; memcpy(rom_src, e->rom_src, sizeof rom_src);
    uint8_t *ovl_mem[OVL_SLOTS]; memcpy(ovl_mem, e->ovl.mem, sizeof ovl_mem);
    uint16_t (*gram)[GRAM_W] = e->lcd.gram;

    memcpy(e, snap, sizeof(Emu));

    e->rom = rom; e->rom_fast = rom_fast; e->rom_fast_len = rom_fast_len; e->pg_base = pg_base;
    memcpy(e->rom_src, rom_src, sizeof rom_src);
    memcpy(e->ovl.mem, ovl_mem, sizeof ovl_mem);
    e->lcd.gram = gram;
    e->fw_ptr = NULL; e->fw_len = 0; e->fw_lo = 0xFFFFFFFFu;
    /* sanity (2026-11-23, release safety): a damaged save must not point the emulator outside its memory */
    if (e->ovl.n > OVL_SLOTS) e->ovl.n = OVL_SLOTS;
    for (int i = 0; i < OVL_SLOTS; i++)
        if (e->ovl.base[i] != 0xFFFFFFFFu && (e->ovl.base[i] >= e->dev.rom_size || (e->ovl.base[i] & (OVL_SIZE - 1)))) e->ovl.base[i] = 0xFFFFFFFFu;
    for (int k = 0; k < 1024; k++) if (e->pg_map[k] >= PG_PAGES && e->pg_map[k] < 0xFE) e->pg_map[k] = 0xFF;
    for (int b = (int)(e->dev.rom_size >> 16); b < 128; b++) e->pg_blk[b >> 5] &= ~(1u << (b & 31));
    for (int b = 0; b < 128; b++) e->rom_blk[b] = e->rom_src[b];
    e->rom_blk[e->par_base >> 16] = 0;
    for (int b = 0; b < 128; b++) tc_blk_update(e, (uint32_t)b);   /* blocks with written copies read via overlays */
    memcpy(s_ovl_pool, snap + sizeof(Emu), SAVE_POOL_BYTES);
    memcpy(gram, snap + sizeof(Emu) + SAVE_POOL_BYTES, SAVE_GRAM_BYTES);
}

static void save_path(char *out, size_t n, const char *romfile, const char *ext)
{
    snprintf(out, n, "/sdcard/saves/%s", romfile);   /* saves live in their own folder (2026-11-22) */
    char *dot = strrchr(out, '.');
    if (dot) snprintf(dot, n - (size_t)(dot - out), "%s", ext);
}

/* SD card folders (owner 2026-11-22: "the roms folder is pretty messy"): roms/ only the games, saves/ our
 * .sav files (+ .prev / .bak safety copies), exports/, screenshots/, update/, dlc/<game>/; developer builds also
 * snaps/ and logs/ (battery diary, flash-save records, sleep locks). Files from the old layout are moved once. */
static void sd_organise(void)
{
    /* every folder a player uses exists from the first start (owner 2026-11-23); logs/ only in developer builds */
    mkdir("/sdcard/roms", 0777); mkdir("/sdcard/saves", 0777); mkdir("/sdcard/update", 0777); mkdir("/sdcard/dlc", 0777);
    mkdir("/sdcard/exports", 0777); mkdir("/sdcard/screenshots", 0777);
#if TL_DEVLOG
    mkdir("/sdcard/logs", 0777);
#endif
    { static const char *const G[] = { "ID", "IDL", "PS", "4U", "4UPLUS" };   /* DLC folder per game */
      char d[32]; for (int i = 0; i < 5; i++) { snprintf(d, sizeof d, "/sdcard/dlc/%s", G[i]); mkdir(d, 0777); } }
    int moved = 0;
    DIR *d = opendir("/sdcard/roms");
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !strstr(e->d_name, ".sav")) continue;   /* .sav, .sav.prev, .sav.bak */
        char from[300], to[300];
        snprintf(from, sizeof from, "/sdcard/roms/%s", e->d_name);
        snprintf(to, sizeof to, "/sdcard/saves/%s", e->d_name);
        FILE *t = fopen(to, "rb");
        if (t) { fclose(t); continue; }            /* never overwrite */
        if (rename(from, to) == 0) moved++;
    }
    if (d) closedir(d);
    static const char *const LOGS[3] = { "batlog.csv", "flashsaves.txt", "pmlocks.txt" };
    for (int i = 0; i < 3; i++) {
        char from[64], to[64];
        snprintf(from, sizeof from, "/sdcard/%s", LOGS[i]); snprintf(to, sizeof to, "/sdcard/logs/%s", LOGS[i]);
        FILE *t = fopen(to, "rb");
        if (t) { fclose(t); continue; }
        if (rename(from, to) == 0) moved++;
    }
    if (moved) ESP_LOGI(TAG, "SD card tidied: %d file(s) moved to saves/ and logs/", moved);
}

static uint32_t s_lost0;                           /* flash writes lost when the game started (from its save) */
static bool s_game_broken;                         /* the game crashed or lost flash writes: saving stops */
static bool save_write_snap(const char *romfile)
{
    if (s_emu && (s_emu->stopped || s_emu->flash_lost_writes != s_lost0)) {   /* (2026-11-23: the 4U patch crash got saved) */
        if (!s_game_broken) ESP_LOGE(TAG, "NOT SAVED: the game %s - the last good save is kept",
                                     s_emu->stopped ? "stopped (crashed)" : "lost flash writes (no room)");
        s_game_broken = true;
        return false;
    }
    char path[160], tmp[160];
    save_path(path, sizeof path, romfile, ".sav");
    save_path(tmp, sizeof tmp, romfile, ".tmp");
    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGE(TAG, "Save: cannot create %s", tmp); return false; }
    SaveHdr h = { SAVE_MAGIC, sizeof(Emu), OVL_SLOTS, OVL_SIZE, SAVE_GRAM_BYTES };
    size_t total = sizeof(Emu) + SAVE_POOL_BYTES + SAVE_GRAM_BYTES;
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(s_snap, 1, total, f) == total;
    if (ok && s_pg_snap_n) {                       /* flash page store, after the normal data */
        uint32_t pg_h[2] = { PG_MAGIC, s_pg_snap_n };
        ok = fwrite(pg_h, sizeof pg_h, 1, f) == 1 && fwrite(s_pg_snap, OVL_SIZE, s_pg_snap_n, f) == s_pg_snap_n;
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok) { ESP_LOGE(TAG, "Save: write failed"); unlink(tmp); return false; }
    unlink(path);
    if (rename(tmp, path) != 0) { ESP_LOGE(TAG, "Save: rename failed"); return false; }
    ESP_LOGI(TAG, "Saved progress to %s", path);
    return true;
}

/* Load a save for romfile into e (fresh from tc_init). Returns true if restored. */
static bool save_load(const char *romfile, Emu *e)
{
    char path[160];
    save_path(path, sizeof path, romfile, ".sav");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    SaveHdr h;
    size_t total = sizeof(Emu) + SAVE_POOL_BYTES + SAVE_GRAM_BYTES;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == SAVE_MAGIC && h.emu_size == sizeof(Emu) &&
              h.ovl_slots == OVL_SLOTS && h.ovl_size == OVL_SIZE && h.gram_bytes == SAVE_GRAM_BYTES &&
              fread(s_snap, 1, total, f) == total;
    uint32_t pg_h[2] = {0, 0}, pg_n = 0;
    if (ok && fread(pg_h, sizeof pg_h, 1, f) == 1 && pg_h[0] == PG_MAGIC && pg_h[1] <= PG_PAGES && s_pg_snap)
        pg_n = fread(s_pg_snap, OVL_SIZE, pg_h[1], f) == pg_h[1] ? pg_h[1] : 0;
    fclose(f);
    if (!ok) { ESP_LOGW(TAG, "Save %s is not usable (other version or damaged), starting fresh", path); return false; }
    const Emu *se = (const Emu *)s_snap;
    if (se->dev.osc3_hz == 18432000u && e->dev.osc3_hz == OSC3_4U && e->dev.nfc_pn512) {   /* 4U save from before the 30 MHz fix */
        DeviceProfile d = se->dev; d.osc3_hz = OSC3_4U;
        if (memcmp(&d, &e->dev, sizeof d) == 0) { tc_set_osc3((Emu *)se, OSC3_4U); ESP_LOGI(TAG, "Save %s: 4U clock 18.432 -> 30 MHz", path); }
    }
    if (memcmp(&se->dev, &e->dev, sizeof e->dev) != 0) { ESP_LOGW(TAG, "Save %s is for another device type", path); return false; }
    if (se->stopped) {                             /* a crashed game got saved (before 2026-11-23): use the copy from before */
        char prev[168], bad[168];
        snprintf(prev, sizeof prev, "%s.prev", path);
        snprintf(bad, sizeof bad, "%s.crashed", path);
        ESP_LOGW(TAG, "Save %s holds a crashed game - kept as %s, loading the copy from before this session", path, bad);
        remove(bad); rename(path, bad);
        struct stat st;
        if (stat(prev, &st) == 0 && rename(prev, path) == 0) return save_load(romfile, e);
        return false;
    }
    {   /* safety copy of the save as it was before this session: <rom>.sav.prev (replaced on every load) */
        char prev[168];
        snprintf(prev, sizeof prev, "%s.prev", path);
        FILE *fp = fopen(prev, "wb");
        if (fp) {
            bool w = fwrite(&h, sizeof h, 1, fp) == 1 && fwrite(s_snap, 1, total, fp) == total;
            if (w && pg_n) { fwrite(pg_h, sizeof pg_h, 1, fp); fwrite(s_pg_snap, OVL_SIZE, pg_n, fp); }
            fclose(fp);
        }
    }
    {   /* one-time safety copy (<rom>.sav.bak) before the date fix starts changing loaded saves */
        char bak[168];
        snprintf(bak, sizeof bak, "%s.bak", path);
        FILE *fb = fopen(bak, "rb");
        if (fb) fclose(fb);
        else if ((fb = fopen(bak, "wb")) != NULL) {
            bool w = fwrite(&h, sizeof h, 1, fb) == 1 && fwrite(s_snap, 1, total, fb) == total;
            fclose(fb);
            ESP_LOGI(TAG, "Safety copy %s %s", bak, w ? "written" : "FAILED");
        }
    }
    snap_restore(e, s_snap);
    if (pg_n && s_pg_pool) memcpy(s_pg_pool, s_pg_snap, pg_n * OVL_SIZE);
    ESP_LOGI(TAG, "Restored progress from %s%s", path, pg_n ? " (with flash pages)" : "");
    return true;
}

/* The games show the date from their own copy in RAM, not from the clock chip, so a save brings back an
 * old date. The copy is 4 plain numbers: month, day (1-based), hour, minute. Found with RAM dumps 2026-11-18:
 *   Plus Color: DSTRAM 0x8416f.  iD L: DSTRAM 0x84283.  P's: A0RAM 0x130d.  4U+: A0RAM 0x121f.
 * In iD L and P's it follows the marker bytes eb e4 ec 00, so for those games we search for the marker. */
static volatile uint32_t s_rtc_wr_seen;          /* g_tc_rtc_wr already handled (game clock writes) */
static uint8_t *volatile s_date;    /* the game's date copy, NULL = not known */

static bool date_plausible(const uint8_t *d)
{
    return d[0] >= 1 && d[0] <= 12 && d[1] >= 1 && d[1] <= 31 && d[2] <= 23 && d[3] <= 59;
}

static uint8_t *find_game_date(Emu *e)
{
    if (s_hexa) return NULL;                       /* not understood yet (see s_hexa) */
    if (s_date_plus) { uint8_t *d = e->dstram + 0x16f + s_pet_shift; return date_plausible(d) ? d : NULL; }
    {   /* 4U+ (2026-11-22, owner's snapshots: minute counted up 24..31 over 6 min): A0RAM 0x121f, no marker */
        const CareInfo *c = care_find(e);
        if (c && c->rv == 0x024F6256u) { uint8_t *d = e->a0ram + 0x121f; return date_plausible(d) ? d : NULL; }
    }
    uint8_t *areas[3] = { e->a0ram, e->ivram, e->dstram };
    const uint32_t lens[3] = { A0RAM_MAX, IVRAM_MAX, DSTRAM_MAX };
    uint8_t *found = NULL;
    for (int k = 0; k < 3; k++)
        for (uint32_t o = 0; o + 8 <= lens[k]; o++) {
            uint8_t *p = areas[k] + o;
            if (p[0] == 0xeb && p[1] == 0xe4 && p[2] == 0xec && p[3] == 0x00 && date_plausible(p + 4)) {
                if (found) return NULL;            /* more than one: do not guess */
                found = p + 4;
            }
        }
    return found;
}

static inline uint32_t guest_mclk(const Emu *e)
{
    return e->cmu.mclk_hz ? e->cmu.mclk_hz : e->dev.osc3_hz;
}

static void emu_task(void *arg)
{
    /* Pace the guest by its own time. The game switches its CPU clock (MCLK) between 20 MHz and slow
     * modes such as the 32.768 kHz watch crystal; its timers and clock count MCLK cycles. So for each
     * stretch of real time we run (real time x current MCLK) cycles. If the ESP32 cannot keep up,
     * the guest simply runs slower and the log says by how much. */
    int64_t  last = esp_timer_get_time();
    int64_t  owed = 0;                 /* real time (ns) the guest still has to catch up */
    uint64_t gns = 0;                  /* guest time (ns) */
    uint64_t next_ms = 1000000;        /* guest time of the next clock millisecond */
    int64_t  st_t0 = last, st_busy = 0;
    uint64_t st_g0 = 0, st_c0 = s_emu->cycles;

    while (s_emu_running) {
        int64_t now = esp_timer_get_time();
        owed += (now - last) * 1000 * s_time_warp;
        if (s_tr_t0 && owed / 1000 > s_tr_behind_max) { s_tr_behind_max = (int32_t)(owed / 1000); s_tr_behind_at = (int32_t)((now - s_tr_t0) / 1000); }
        last = now;
        /* The game works in bursts (drawing a screen) with rests between. Lag from a burst is caught up in
         * the following rest, so the game clock keeps real time. Only beyond 2 s of lag do we give up. */
        if (owed > 2000000000LL) owed = 2000000000LL;

        if (owed > 0) {
            pm_hold(s_pm_emu, true);
            while (owed > 0 && s_emu_running) {
                /* Buttons at the right guest time: after a nap the guest first catches up the time it slept
                 * (quickly, it is mostly halted), then sees the change. Applied at the start of the catch-up, a
                 * tap looked up to 0.5 s longer to the game, long enough to count as a game press after waking
                 * it (the real toy only wakes). While awake (or when the game is really behind) at once. */
                s_btn_hide &= s_btn_mask;                   /* a hidden wake key is forgotten once released */
                const uint8_t want = s_btn_mask & (uint8_t)~s_btn_hide;
                if (want != s_emu->btn_mask && (!s_pet_asleep || owed < 3000000LL || now - s_btn_defer_t > 300000)) {
                    uint8_t newly = want & (uint8_t)~s_emu->btn_mask;
                    periph_buttons(s_emu, want);
                    s_btn_defer_t = 0;
                    /* Wake press: on the real toy the press that wakes the screen is not a game press (the real
                     * chip's slower wake-up outlasts a tap). With the game's screen off, a new press is only a
                     * short blip (WAKE_PULSE_MS of guest time) that wakes it, then looks released until let go.
                     * Owner confirmed 2026-11-19. */
                    if (newly && WAKE_PULSE_MS && !(s_emu->lcd.disp_on && s_emu->lcd.sleep_out)) {
                        s_pulse_keys |= newly; s_pulse_end = gns + WAKE_PULSE_MS * 1000000ULL;
                        ESP_LOGI(TAG, "Wake press: wakes the game only");
                    }
                } else if (want != s_emu->btn_mask && !s_btn_defer_t) s_btn_defer_t = now;
                uint32_t hz = guest_mclk(s_emu);
                uint64_t cyc = (uint64_t)owed * hz / 1000000000ULL;
                if (cyc == 0) break;                        /* less than one guest cycle owed */
                if (cyc > 20000) cyc = 20000;
                if (cyc > hz / 200 + 1) cyc = hz / 200 + 1;  /* at most 5 ms of guest time per chunk (slow clocks) */
                if (s_pulse_keys) {                         /* stop exactly at the end of the blip */
                    uint64_t left = s_pulse_end > gns ? s_pulse_end - gns : 0;
                    uint64_t c = left * hz / 1000000000ULL + 1;
                    if (cyc > c) cyc = c;
                }
#if TL_PROFILE
                {   /* profiling: chunks of random length, sample the exact guest PC at each chunk end (no aliasing) */
                    static uint32_t r = 12345;
                    r = r * 1103515245u + 12345u;
                    uint32_t lim = 200 + (r >> 16) % 2500;
                    if (cyc > lim) cyc = lim;
                }
#endif

                s_chunk_ns0 = gns; s_chunk_c0 = s_emu->cycles; s_chunk_hz = hz;
                uint32_t tr_h0 = esp_cpu_get_cycle_count();
                uint64_t tr_skip0 = tc_prof.halt_cyc + tc_prof.idle_cyc, tr_g20 = tc_hle.guest2, tr_h20 = tc_hle.host2;
                tc_run(s_emu, s_emu->cycles + cyc);
                if (s_tr_t0) {                            /* press trace: cost of the executed guest cycles */
                    s_tr_host += esp_cpu_get_cycle_count() - tr_h0;
                    uint64_t ran_c = s_emu->cycles - s_chunk_c0, skip = tc_prof.halt_cyc + tc_prof.idle_cyc - tr_skip0;
                    s_tr_exec += ran_c > skip ? ran_c - skip : 0;
                    s_tr_g2 += tc_hle.guest2 - tr_g20; s_tr_h2 += tc_hle.host2 - tr_h20;
                }
#if TL_PROFILE
                if (!s_emu->halted) prof_hit(s_emu->pc);
#endif
                uint64_t ran = s_emu->cycles - s_chunk_c0;
                uint64_t ns = ran * 1000000000ULL / hz;
                gns += ns;
                owed -= (int64_t)ns;
                /* The watch crystal: seconds and clock interrupts come from guest milliseconds, independent of
                 * the CPU clock the game happens to run at (it switches between 20 MHz, 10 MHz and 32 kHz). */
                while (gns >= next_ms) { periph_rtc_ms(s_emu); next_ms += 1000000; }
                if (s_pulse_keys && gns >= s_pulse_end) {   /* end of the wake blip */
                    s_btn_hide |= s_pulse_keys; s_pulse_keys = 0;
                    periph_buttons(s_emu, s_btn_mask & (uint8_t)~s_btn_hide);
                }
                publish_cycles(gns);

                if (esp_timer_get_time() - now > 20000) break;   /* yield at least every 20 ms */
            }
            st_busy += esp_timer_get_time() - now;
            {   /* The game set its own clock (its time-setting screen, or loading its own save after a reset puts
                 * back the old saved time): once it has finished (0.5 s without further clock writes, clock
                 * running), put the board's date and time back. The owner's choice: the game always follows
                 * the real time. */
                static uint32_t pend_val; static int64_t pend_t;
                uint32_t wr = g_tc_rtc_wr;
                if (wr != s_rtc_wr_seen && s_hexa) s_rtc_wr_seen = wr;   /* Hexagontchi sets its clock every 1.5 s: leave it */
                if (wr != s_rtc_wr_seen) {
                    int64_t t = esp_timer_get_time();
                    if (wr != pend_val || !pend_t) { pend_val = wr; pend_t = t; }
                    else if (t - pend_t > 500000 && !(s_emu->ioram[0x1908] & 2) && !(s_emu->ioram[0x190C] & 1)) {
                        uint32_t now_s;
                        if (shared_time(&now_s)) {
                            periph_rtc_set(s_emu, now_s);
                            if (s_date) {
                                rtc_time_t rt; rtc_from_secs(now_s, &rt);
                                s_date[0] = (uint8_t)rt.month; s_date[1] = (uint8_t)rt.day; s_date[2] = (uint8_t)rt.hour; s_date[3] = (uint8_t)rt.minute;
                            }
                            ESP_LOGI(TAG, "The game set its own clock; put back to the board's date and time");
                        }
                        s_rtc_wr_seen = wr; pend_t = 0;
                    }
                }
            }
        }

        if (s_snap_req) { snap_capture(); s_snap_req = false; s_snap_done = true; }
        if (s_emu_job) { s_emu_job(s_emu_job_arg); s_emu_job = NULL; s_emu_job_done = true; }   /* DLC: flash work between chunks */

        if (s_emu->stopped) {                        /* the guest CPU hit something it cannot execute: report once */
            static uint64_t reported_at = UINT64_MAX;
            if (reported_at != s_emu->cycles) {
                reported_at = s_emu->cycles;
                ESP_LOGE(TAG, "GUEST STOPPED at cycle %llu: stop_pc %08lx stop_code %04lx pc %08lx sp %08lx | hle %d calls %lu",
                         (unsigned long long)s_emu->cycles, (unsigned long)s_emu->stop_pc, (unsigned long)s_emu->stop_code,
                         (unsigned long)s_emu->pc, (unsigned long)s_emu->sp, tc_hle_mode, (unsigned long)tc_hle.calls);
                ESP_LOGE(TAG, "  r0-r7: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                         (unsigned long)s_emu->r[0], (unsigned long)s_emu->r[1], (unsigned long)s_emu->r[2], (unsigned long)s_emu->r[3],
                         (unsigned long)s_emu->r[4], (unsigned long)s_emu->r[5], (unsigned long)s_emu->r[6], (unsigned long)s_emu->r[7]);
                ESP_LOGE(TAG, "  r8-r15: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                         (unsigned long)s_emu->r[8], (unsigned long)s_emu->r[9], (unsigned long)s_emu->r[10], (unsigned long)s_emu->r[11],
                         (unsigned long)s_emu->r[12], (unsigned long)s_emu->r[13], (unsigned long)s_emu->r[14], (unsigned long)s_emu->r[15]);
                for (uint32_t a = (s_emu->stop_pc & ~0x1Fu) - 0x40; a < (s_emu->stop_pc & ~0x1Fu) + 0x40; a += 32) {
                    printf("CODE %06lx:", (unsigned long)a);
                    for (uint32_t j = 0; j < 32; j += 2) printf(" %04x", mem_read16(s_emu, a + j));
                    printf("\n");
                }
                for (uint32_t a = s_emu->sp; a < s_emu->sp + 64; a += 32) {
                    printf("STACK %06lx:", (unsigned long)a);
                    for (uint32_t j = 0; j < 32; j += 4) printf(" %08lx", (unsigned long)mem_read32(s_emu, a + j));
                    printf("\n");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        now = esp_timer_get_time();
        if (now - st_t0 >= 3000000) {
            double wall_s = (double)(now - st_t0) / 1e6;
            ESP_LOGI(TAG, "realtime x%.2f | ESP busy %.0f%% | guest clock %.3f MHz | %.2f M cycles/s | idle skip %.0f%% (busy-loop skips %lu)",
                     (double)(gns - st_g0) / 1e9 / wall_s, 100.0 * (double)st_busy / (double)(now - st_t0),
                     guest_mclk(s_emu) / 1e6, (double)(s_emu->cycles - st_c0) / 1e6 / wall_s,
                     100.0 * (double)tc_prof.idle_cyc / (double)((s_emu->cycles - st_c0) ? (s_emu->cycles - st_c0) : 1),
                     (unsigned long)tc_prof.nb_skips);
            if (tc_hle.host2) {                           /* measuring (2026-11-21): engine v2 get-pixel cost */
                double busy_cyc = (double)st_busy * 240.0;
                double gex = (double)(s_emu->cycles - st_c0) - (double)tc_prof.idle_cyc;
                if (gex < 1) gex = 1;
                double g2 = (double)tc_hle.guest2, h2 = (double)tc_hle.host2;
                ESP_LOGI(TAG, "  get-pixel (engine v2): %.0f%% of ESP busy time, %.0f%% of guest cycles, %.1f ESP cycles per guest cycle | rest %.1f",
                         100.0 * h2 / (busy_cyc > 1 ? busy_cyc : 1), 100.0 * g2 / gex, g2 > 0 ? h2 / g2 : 0,
                         gex - g2 > 0 ? (busy_cyc - h2) / (gex - g2) : 0);
                tc_hle.host2 = tc_hle.guest2 = 0;
            }
            if (!s_date) s_date = find_game_date(s_emu);  /* e.g. a fresh game that has just set itself up */
            if (s_date) {
                /* keep the game's date copy equal to its clock chip, so the date also turns over at midnight
                 * while playing (Plus Color did not do this itself in tests) */
                uint8_t *d = s_date;
                const uint8_t *io = s_emu->ioram;
                uint8_t m = (uint8_t)((io[0x1920] >> 4) * 10 + (io[0x1920] & 15)), dd = (uint8_t)((io[0x191C] >> 4) * 10 + (io[0x191C] & 15));
                if (!(io[0x1908] & 2) && !(io[0x190C] & 1) && m >= 1 && m <= 12 && dd >= 1 && dd <= 31 &&
                    d[0] >= 1 && d[0] <= 12 && d[1] >= 1 && d[1] <= 31 && (d[0] != m || d[1] != dd)) {
                    ESP_LOGI(TAG, "Game's own date %d/%d -> %d/%d (follows its clock)", d[0], d[1], m, dd);
                    d[0] = m; d[1] = dd;
                }
            }
#if TL_DIAG
            {
                const uint8_t *io = s_emu->ioram;
                ESP_LOGI(TAG, "  game clock 20%02x-%02x-%02x %02x:%02x:%02x | game wrote its clock %lu times | ctl %02x %02x %02x",
                         io[0x1924], io[0x1920], io[0x191C], io[0x1918], io[0x1914], io[0x1910], (unsigned long)g_tc_rtc_wr,
                         io[0x1904], io[0x1908], io[0x190C]);
            }
#endif
            if (s_fs.state == 1 && s_emu->cycles - s_fs.last_cyc > (uint64_t)guest_mclk(s_emu)) s_fs.state = 2;   /* 1 s quiet: burst over */
            if (s_fs.state == 2) {
                /* also appended to /sdcard/flashsaves.txt (2026-11-22): flashing the board cut the serial recorder twice
                 * during purchase tests; the file is printed over USB at start-up (FLASHLOG lines) */
                static char fl[700];
                int n = snprintf(fl, sizeof fl, "FLASHSAVE %s at %llu s: %lu writes, %lu erases, flash offsets %06lx..%06lx | first at pc %06lx sp %06lx\nFLASHSTACK",
                       s_game_file, (unsigned long long)(esp_timer_get_time() / 1000000),
                       (unsigned long)s_fs.n_prog, (unsigned long)s_fs.n_erase, (unsigned long)s_fs.lo, (unsigned long)s_fs.hi,
                       (unsigned long)s_fs.pc, (unsigned long)s_fs.sp);
                for (int i = 0; i < 48 && n < (int)sizeof fl - 12; i++) n += snprintf(fl + n, sizeof fl - n, " %08lx", (unsigned long)s_fs.stack[i]);
                printf("%s\n", fl);
#if TL_DEVLOG
                FILE *ff = fopen("/sdcard/logs/flashsaves.txt", "a");
                if (ff) { fprintf(ff, "%s\n", fl); fclose(ff); }
#endif
                s_fs.state = 0;
            }
            if (s_emu->flash_dirty) {                     /* the game wrote its own save memory (flash) */
                s_emu->flash_dirty = false;
                ESP_LOGI(TAG, "The game saved to its flash (pages in use %lu)", (unsigned long)(s_pg_pool ? pg_used_max(s_emu) : 0));
            }
            {   /* freeze diagnosis: guest busy-waiting (idle skip ~100%) with no screen writes */
                static int stuck_dumps;
                double idle = (double)tc_prof.idle_cyc / (double)((s_emu->cycles - st_c0) ? (s_emu->cycles - st_c0) : 1);
                if (idle > 0.99 && !s_pr_n && stuck_dumps < 3) {
                    stuck_dumps++;
                    ESP_LOGW(TAG, "STUCK? pc %08lx sp %08lx psr %08lx | flash state %d busy %d lost %lu | halted %d",
                             (unsigned long)s_emu->pc, (unsigned long)s_emu->sp, (unsigned long)s_emu->psr_rest,
                             s_emu->flash_state, s_emu->flash_busy, (unsigned long)s_emu->flash_lost_writes, (int)s_emu->stopped);
                    ESP_LOGW(TAG, "  r0-r7: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                             (unsigned long)s_emu->r[0], (unsigned long)s_emu->r[1], (unsigned long)s_emu->r[2], (unsigned long)s_emu->r[3],
                             (unsigned long)s_emu->r[4], (unsigned long)s_emu->r[5], (unsigned long)s_emu->r[6], (unsigned long)s_emu->r[7]);
                    ESP_LOGW(TAG, "  r8-r15: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                             (unsigned long)s_emu->r[8], (unsigned long)s_emu->r[9], (unsigned long)s_emu->r[10], (unsigned long)s_emu->r[11],
                             (unsigned long)s_emu->r[12], (unsigned long)s_emu->r[13], (unsigned long)s_emu->r[14], (unsigned long)s_emu->r[15]);
                    for (uint32_t a = (s_emu->pc & ~0x1Fu) - 0x60; a < (s_emu->pc & ~0x1Fu) + 0x40; a += 32) {
                        printf("CODE %06lx:", (unsigned long)a);
                        for (uint32_t j = 0; j < 32; j += 2) printf(" %04x", mem_read16(s_emu, a + j));
                        printf("\n");
                    }
                    for (uint32_t a = s_emu->sp; a < s_emu->sp + 96; a += 32) {
                        printf("STACK %06lx:", (unsigned long)a);
                        for (uint32_t j = 0; j < 32; j += 4) printf(" %08lx", (unsigned long)mem_read32(s_emu, a + j));
                        printf("\n");
                    }
                    printf("IO 300260:"); for (int j = 0; j < 0x40; j++) printf(" %02x", s_emu->ioram[0x260 + j]); printf("\n");
                }
            }
#if TL_DIAG
            if (s_pr_n) {
                ESP_LOGI(TAG, "  screen: %lu updates, avg %.1f ms (max %.1f ms), avg %lu panel rows | sent after: sleep %lu, quiet %lu, 120ms-limit %lu | game drew during send %lu",
                         (unsigned long)s_pr_n, s_pr_us / 1000.0 / s_pr_n, s_pr_max / 1000.0, (unsigned long)(s_pr_rows / s_pr_n),
                         (unsigned long)s_why[0], (unsigned long)s_why[1], (unsigned long)s_why[2], (unsigned long)s_middraw);
                s_pr_n = 0; s_pr_us = 0; s_pr_max = 0; s_pr_rows = 0; s_why[0] = s_why[1] = s_why[2] = 0; s_middraw = 0;
                extern uint32_t g_lcd_2c, g_lcd_px, g_lcd_wdone, g_lcd_full, g_lcd_lastw_n; extern uint16_t g_lcd_lastw[8][2];
                ESP_LOGI(TAG, "  game drawing: %lu writes (0x2C), %lu pixels, %lu windows completed, %lu full-screen | last windows %ux%u %ux%u %ux%u %ux%u",
                         (unsigned long)g_lcd_2c, (unsigned long)g_lcd_px, (unsigned long)g_lcd_wdone, (unsigned long)g_lcd_full,
                         g_lcd_lastw[(g_lcd_lastw_n - 1) & 7][0], g_lcd_lastw[(g_lcd_lastw_n - 1) & 7][1], g_lcd_lastw[(g_lcd_lastw_n - 2) & 7][0], g_lcd_lastw[(g_lcd_lastw_n - 2) & 7][1],
                         g_lcd_lastw[(g_lcd_lastw_n - 3) & 7][0], g_lcd_lastw[(g_lcd_lastw_n - 3) & 7][1], g_lcd_lastw[(g_lcd_lastw_n - 4) & 7][0], g_lcd_lastw[(g_lcd_lastw_n - 4) & 7][1]);
                g_lcd_2c = g_lcd_px = g_lcd_wdone = g_lcd_full = 0;
            }
            if (tc_hle.calls)
                ESP_LOGI(TAG, "  native graphics: %lu calls, %lu native, %lu fell back | spot checks %lu ok, %lu WRONG, cycles %lu exact, %lu off",
                         (unsigned long)tc_hle.calls, (unsigned long)tc_hle.done, (unsigned long)tc_hle.bail,
                         (unsigned long)tc_hle.match, (unsigned long)tc_hle.mismatch,
                         (unsigned long)tc_hle.cyc_match, (unsigned long)tc_hle.cyc_mismatch);
#endif
            if (tc_hle.mismatch || tc_hle.cyc_mismatch)   /* native graphics disagreed with the real routine: always report */
                ESP_LOGW(TAG, "  native graphics spot checks: %lu ok, %lu WRONG result, %lu wrong cycles",
                         (unsigned long)tc_hle.match, (unsigned long)tc_hle.mismatch, (unsigned long)tc_hle.cyc_mismatch);
            s_pr_n = 0; s_pr_us = 0; s_pr_max = 0; s_pr_rows = 0; s_why[0] = s_why[1] = s_why[2] = 0; s_middraw = 0;
#if TL_PROFILE
            ESP_LOGI(TAG, "  slow paths/3s: mem rd %lu wr %lu | io rd %lu wr %lu | fetch window %lu | irqs %lu | hle %lu | io hot %06lx:%lu %06lx:%lu %06lx:%lu %06lx:%lu",
                     (unsigned long)tc_prof.mem_rd, (unsigned long)tc_prof.mem_wr, (unsigned long)tc_prof.io_rd, (unsigned long)tc_prof.io_wr,
                     (unsigned long)tc_prof.fetchwin, (unsigned long)tc_prof.irq_taken, (unsigned long)tc_hle.calls,
                     (unsigned long)tc_prof.io_addr[0], (unsigned long)tc_prof.io_cnt[0], (unsigned long)tc_prof.io_addr[1], (unsigned long)tc_prof.io_cnt[1],
                     (unsigned long)tc_prof.io_addr[2], (unsigned long)tc_prof.io_cnt[2], (unsigned long)tc_prof.io_addr[3], (unsigned long)tc_prof.io_cnt[3]);
#endif
            tc_hle.calls = tc_hle.done = tc_hle.bail = tc_hle.match = tc_hle.mismatch = 0;
            tc_hle.cyc_match = tc_hle.cyc_mismatch = 0;
            memset(&tc_prof, 0, sizeof tc_prof);
#if TL_PROFILE
            static int rep_n;
            if (++rep_n * 3 >= PROF_REPORT_S) { rep_n = 0; prof_report(); }
#endif
            st_t0 = now; st_g0 = gns; st_c0 = s_emu->cycles; st_busy = 0;
        }

        /* Caught up: sleep a tick. Still behind: keep going, only yielding briefly now and then so the
         * idle task (watchdog) gets to run. */
        static int64_t last_nap;
        if (s_pet_asleep && s_emu->halted && owed < 2000000LL && (s_btn_mask & (uint8_t)~s_btn_hide) == s_emu->btn_mask && !s_pulse_keys) {
            /* the pet sleeps: nap until the game's next timer event or clock second (light sleep in between);
             * a key press wakes us early (ui task notifies) */
            uint32_t hz = guest_mclk(s_emu);
            uint64_t nx = periph_next(s_emu);
            if (!s_emu->halt_hard && s_emu->wake_at > s_emu->cycles && s_emu->wake_at - s_emu->cycles < nx) nx = s_emu->wake_at - s_emu->cycles;
            uint32_t ms = hz ? (uint32_t)(nx * 1000ULL / hz) : 1;
            if (!(s_emu->ioram[0x1908] & 2) && !(s_emu->ioram[0x190C] & 1)) {
                uint32_t to_sec = s_emu->rtc_cyc < 1000u ? 1000u - s_emu->rtc_cyc : 1u;   /* guest ms to the next clock second */
                if (to_sec < ms) ms = to_sec;
            }
            if (ms > 1000) ms = 1000;                        /* at most one wake per guest second */
            pm_hold(s_pm_emu, false);
            last_nap = now;
            if (ms >= 3) { s_nap_n++; s_nap_ms += ms; ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms - 1)); }
            else vTaskDelay(1);
        } else if (owed < 2000000LL || now - last_nap > 50000) { last_nap = now; pm_hold(s_pm_emu, false); vTaskDelay(1); }
    }

    s_tone_hz = 0;
    pm_hold(s_pm_emu, false);
    s_emu_task_alive = false;
    vTaskDelete(NULL);
}

/* Load the ROM and start the emulator. Returns false (after showing why) if it could not. */
static bool game_start(const char *filename)
{
    char path[160];
    snprintf(path, sizeof path, "/sdcard/roms/%s", filename);
    rom_display_name(filename, s_loading_name, sizeof s_loading_name);
    draw_loading(0);

    s_emu = s_emu_buf;
    memset(s_emu, 0, sizeof(Emu));
    rom_profile(filename, &s_emu->dev);
    /* osc3_hz stays at the real device clock set by the profile (do NOT override); 4U/4U+: 30 MHz */

    uint8_t *rom_ptr = load_rom_storage(path, filename, s_emu->dev.rom_size, &s_rom_is_mmap);
    if (!rom_ptr) {
        ESP_LOGE(TAG, "Failed to load ROM: %s", path);
        s_emu = NULL;
        show_error("CANNOT LOAD ROM", s_loading_name);
        return false;
    }
    memset(s_gram, 0, GRAM_W * GRAM_H * sizeof(uint16_t));
    tc_init(s_emu, rom_ptr, s_gram, s_ovl_pool, OVL_SLOTS);
    rom_apply_map(s_emu);
    s_emu->pg_base = s_pg_pool;
    tc_big_erase = s_pg_pool ? pg_big_erase : NULL;
    tc_ovl_evict = s_pg_pool ? pg_evict : NULL;
    snprintf(s_game_file, sizeof s_game_file, "%s", filename);
    bool restored = save_load(filename, s_emu);
    s_game_restored = restored;
    (void)restored;
#if TL_RAMDUMP == 1 || TL_RAMDUMP == 2
    if (restored) {                                  /* investigation: print the game's RAM as loaded from the save */
        static const struct { const char *n; uint32_t base; const uint8_t *p; uint32_t len; } areas[] = {
            { "A0RAM", 0x00000, NULL, A0RAM_MAX }, { "IVRAM", 0x80000, NULL, IVRAM_MAX }, { "DSTRAM", 0x84000, NULL, DSTRAM_MAX } };
        const uint8_t *ptr[3] = { s_emu->a0ram, s_emu->ivram, s_emu->dstram };
        for (int k = 0; k < 3; k++) {
            for (uint32_t o = 0; o < areas[k].len; o += 32) {
                printf("RAM %06lx:", (unsigned long)(areas[k].base + o));
                for (int j = 0; j < 32; j++) printf(" %02x", ptr[k][o + j]);
                printf("\n");
                if ((o & 0x3FF) == 0x3E0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        printf("RAMDUMP END\n");
        /* Midnight experiment: clock to 23:59:57, run 8 s of guest time, report RAM bytes that changed by +1
         * (the game's own day counter rolls over), then put the save back exactly as it was. */
#if TL_RAMDUMP == 2
        static uint8_t *before;                     /* scratch copy: PSRAM, never internal RAM */
        if (!before) before = heap_caps_malloc(A0RAM_MAX + IVRAM_MAX + DSTRAM_MAX + IORAM_MAX, MALLOC_CAP_SPIRAM);
        memcpy(before, s_emu->a0ram, A0RAM_MAX);
        memcpy(before + A0RAM_MAX, s_emu->ivram, IVRAM_MAX);
        memcpy(before + A0RAM_MAX + IVRAM_MAX, s_emu->dstram, DSTRAM_MAX);
        memcpy(before + A0RAM_MAX + IVRAM_MAX + DSTRAM_MAX, s_emu->ioram, IORAM_MAX);
        uint32_t now_s;
        if (shared_time(&now_s)) {
            uint32_t t = now_s - now_s % 86400 + 86400 - 10;         /* today 23:59:50 */
            const uint8_t *io = s_emu->ioram;
            printf("MIDNIGHT before: rtc y%02x m%02x d%02x %02x:%02x:%02x wd%d sel%d ctl%02x\n", io[0x1924], io[0x1920], io[0x191C],
                   io[0x1918], io[0x1914], io[0x1910], io[0x1928], (io[0x1904] >> 2) & 3, io[0x1904]);
            periph_rtc_set(s_emu, t);
            for (int ms = 0; ms < 70000 && !s_emu->stopped; ms++) {
                uint32_t hz = guest_mclk(s_emu);
                tc_run(s_emu, s_emu->cycles + hz / 1000 + 1);
                periph_rtc_ms(s_emu);
                if ((ms & 255) == 0) vTaskDelay(1);
            }
            const uint8_t *now_p[4] = { s_emu->a0ram, s_emu->ivram, s_emu->dstram, s_emu->ioram };
            const uint32_t base[4] = { 0x00000, 0x80000, 0x84000, 0x300000 }, len[4] = { A0RAM_MAX, IVRAM_MAX, DSTRAM_MAX, IORAM_MAX };
            uint32_t off = 0;
            for (int k = 0; k < 4; k++) {
                for (uint32_t o = 0; o < len[k]; o++) {
                    uint8_t b0 = before[off + o], b1 = now_p[k][o];
                    /* +1 changes anywhere, and any change of a byte that held 6 (the month/day) or in I/O */
                    if (b1 != b0 && ((uint8_t)(b1 - b0) == 1 || b0 == 6 || k == 3) && !(base[k] + o >= 0xc00 && base[k] + o < 0x1100))
                        printf("MIDNIGHT +1 at %06lx: %02x -> %02x (next bytes %02x %02x %02x, previous %02x %02x)\n",
                               (unsigned long)(base[k] + o), b0, b1, now_p[k][o + 1], now_p[k][o + 2], now_p[k][o + 3],
                               o ? now_p[k][o - 1] : 0, o > 1 ? now_p[k][o - 2] : 0);
                }
                off += len[k];
            }
            printf("MIDNIGHT after: rtc y%02x m%02x d%02x %02x:%02x:%02x sel%d\n", io[0x1924], io[0x1920], io[0x191C],
                   io[0x1918], io[0x1914], io[0x1910], (io[0x1904] >> 2) & 3);
            printf("MIDNIGHT END (guest stopped %d)\n", s_emu->stopped);
            /* put the loaded save back */
            memset(s_emu, 0, sizeof(Emu));
            rom_profile(filename, &s_emu->dev);
            tc_init(s_emu, rom_ptr, s_gram, s_ovl_pool, OVL_SLOTS);
            save_load(filename, s_emu);
        }
#endif
    }
#endif
    loading_finish();                              /* before the game's clock is set (the wait does not cost game time) */
    s_rtc_wr_seen = g_tc_rtc_wr;
    s_time_warp = 1;
    g_tc_bat_adc = 0x350;                          /* healthy battery for the game (BATTERY TEST resets) */
    s_bar_hunger = s_bar_happy = s_bar_loc = s_bar_hmax = -1; s_bars_wait_frame = false; s_bars_shown_loc = 0;
    s_date_plus = rom_is_plus(filename);
    s_pet_shift = rom_info(filename)->m == M_HEXA ? 4 : 0;
    s_hexa = rom_info(filename)->m == M_HEXA;
    {   RomModel m = rom_info(filename)->m;
        (void)m; RomModel b = rom_base(filename);   /* editions use their base model's bar art */
        s_bar_game = b == M_PLUS && m != M_HEXA ? 1 : b == M_PS ? 2 : b == M_IDL ? 3 : (b == M_4UP || b == M_4U) ? 4 : 0;   }   /* Hexagontchi: own stats ("work"), later */
    /* bezel: a soft shell colour per toy */
    if (rom_is_plus(filename))                    s_bezel_col = RGB565(218, 170, 182);   /* pink */
    else if (rom_base(filename) == M_IDL) s_bezel_col = RGB565(150, 180, 210);   /* sky blue */
    else if (rom_base(filename) == M_ID)  s_bezel_col = RGB565(226, 208, 150);   /* soft butter yellow (iD, 2026-11-22, provisional) */
    else if (rom_info(filename)->m == M_PS) s_bezel_col = RGB565(190, 176, 212);    /* lavender */
    else if (rom_is_4u(filename))                 s_bezel_col = RGB565(236, 184, 204);   /* soft pink (owner's pick A, 2026-11-22) */
    else                                                 s_bezel_col = RGB565(166, 196, 176);   /* mint */
    s_shell_rom_col = s_bezel_col;
    if (!s_shell_on) s_bezel_col = 0;
    s_date = find_game_date(s_emu);
    {
        uint32_t now_s;
        if (shared_time(&now_s)) {
            periph_rtc_set(s_emu, now_s);              /* the game always starts at the real date and time */
            ESP_LOGI(TAG, "Game clock set from the board clock");
            if (s_date) {                              /* put today's date into the game's own copy too */
                uint8_t *d = s_date;
                rtc_time_t t;
                rtc_from_secs(now_s, &t);
                ESP_LOGI(TAG, "Game's own date was %d/%d %02d:%02d", d[0], d[1], d[2], d[3]);
                d[0] = (uint8_t)t.month; d[1] = (uint8_t)t.day; d[2] = (uint8_t)t.hour; d[3] = (uint8_t)t.minute;
                ESP_LOGI(TAG, "Game's own date set to %d/%d %02d:%02d", d[0], d[1], d[2], d[3]);
            } else ESP_LOGI(TAG, "Game's own date copy not found (yet)");
        }
    }
    tone_reset();
    publish_cycles(0);
    memset(&tc_hle, 0, sizeof tc_hle);
#if TL_PROFILE
    tc_pc_hook = NULL;                             /* sampled at chunk ends instead (exact, no aliasing) */
#endif
#if TL_PROFILE
    s_a0_dumped = false;
#endif
    s_chunk_ns0 = 0; s_chunk_c0 = 0; s_chunk_hz = s_emu->dev.osc3_hz;
    s_game_hz = s_emu->dev.osc3_hz;
    tc_io_write_hook = tone_hook;
    s_lost0 = s_emu->flash_lost_writes; s_game_broken = false;   /* (a save never holds a crashed game) */
    memcpy(s_ready, s_gram, GRAM_W * GRAM_H * sizeof(uint16_t));   /* the picture as loaded */
    s_ready_new = false; s_ready_t = 0;
    tc_lcd_done_hook = lcd_done_hook;
    tc_flash_hook = flash_hook; s_fs.state = 0;
    tc_lcd_row_hook = lcd_row_hook;

    game_display_begin();
    ESP_LOGI(TAG, "Internal RAM free %u bytes, largest DMA-capable block %u bytes (menu screen updates need ~15 KB); PSRAM free %u KB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    s_btn_mask = 0;
    s_emu_running = true;
    s_emu_task_alive = true;
    /* Priority 7: above the LVGL task (6), so nothing else steals core 1 from the emulator. */
    xTaskCreatePinnedToCore(emu_task, "paramix_task", 16384, NULL, 7, &s_emu_task_h, 1);
    return true;
}

static void game_stop(void)
{
    s_present_on = false;
    while (s_presenting) vTaskDelay(pdMS_TO_TICKS(2));
    s_emu_running = false;
    while (s_emu_task_alive) vTaskDelay(pdMS_TO_TICKS(5));
    s_game_hz = 0;
    tc_io_write_hook = NULL;
    tc_lcd_done_hook = NULL;
    tc_lcd_row_hook = NULL;
    tone_reset();
    if (s_emu) {
        if (s_rom_in_store) { s_rom_in_store = false; rom_ram_free(); }   /* the ROM store stays mapped; overflow blocks freed */
        else if (s_rom_is_mmap && s_emu->rom) { esp_partition_munmap(s_mmap_handle); s_rom_is_mmap = false; }
        else if (s_emu->rom) free((void *)s_emu->rom);
        s_emu = NULL;                             /* s_emu_buf stays allocated for the next ROM */
    }
    s_btn_mask = 0;
}

/* Run the game until C is tapped then held (MENU_HOLD_MS). Keys go to the game as they are. */
/* Low battery: at LOW_BAT_PCT or less (not charging, for 15 s) the game is saved and left, and a
 * "PLEASE CHARGE" screen appears, so a flat battery never loses progress. Asked once until it recovers. */
#define LOW_BAT_PCT 5
static bool s_low_exit, s_low_ack;

/* Low-battery screen (2026-11-20, owner's design choice): in the style of the colour games' own warning -
 * patterned bands, a framed white box, a bold title and a tired battery character. The character is our own
 * ("puffed out", lying down and sweating; happy with a bolt while charging), not the games' art. The bands are
 * a placeholder for all colour games (owner will send other devices' borders). Greyscale games (later) get
 * the "sleepy" design (reference_files/mockups). */
static const char *const PUFFED[2][15] = {
    { ".....bb..........................", "....bBBb.........................", "....bBBb.........................",
      ".....bb.NNNNNNNNNNNNNNNNNNNNNN...", ".......NLLLYYYYYYYYYYYYYYYYYYYN..", "......NLYYYYYYYYYYYYYYYYYYYYyyNNN",
      "......NYYYYNNNNYYYYYNNNNYYYYyyNgN", "......NYYYYYKKYYYYYYYKKYYYYYyyNgN", "......NYYPPYYYYYNNNYYYYYPPYYyyNgN",
      "......NYYYYYYYYNYYYNYYYYYYYYyyNNN", "......NRRRRRRRYYYYYYYYYYYYYYyyN..", ".......NRRRRRRRRyyyyyyyyyyyyyN...",
      "........NNNNNNNNNNNNNNNNNNNNN....", "..........NN.............NN......", "..........NN.............NN......" },
    { ".................................", ".................................", ".................................",
      "........NNNNNNNNNNNNNNNNNNNNNN...", ".......NLLLYYYYYYYYYYYYYYYYYYYN..", "......NLYYYYYYYYYYYYYYYYYYYYyyNNN",
      "......NYYYYYNNYYYYYYYNNYYYYYyyNgN", "......NYYYYNYYNYYYYYNYYNYYYYyyNgN", "......NYYPPYYYYNYYYNYYYYPPYYyyNgN",
      "......NYYYYYYYYYNNNYYYYYYYYYyyNNN", "......NOOOOOOOYYYYYYYYYYYYYYyyN..", ".......NOOOOOOOOyyyyyyyyyyyyyN...",
      "........NNNNNNNNNNNNNNNNNNNNN....", "..........NN.............NN......", "..........NN.............NN......" },
};
static const char *const LB_BOLT[7] = { "...##", "..##.", ".##..", "#####", "..##.", ".##..", "##..." };
static uint8_t lb_col(char ch)
{
    switch (ch) {
    case 'N': case 'K': return XN;  case 'Y': return XY;  case 'L': return XLY; case 'y': return XDY;
    case 'P': return XPK; case 'B': return XBL; case 'b': return XLB; case 'R': return XRD;
    case 'g': return XLG; case 'O': return XOR; default: return 0xFF;
    }
}
static inline void lb_px(int x, int y, uint8_t c) { if (x >= 0 && x < VL && y >= 0 && y < VL) s_vlcd[y * VL + x] = c; }
static void lb_band(int y0, int y1, int phase)
{
    for (int y = y0; y < y1; y++) for (int x = 0; x < VL; x++) lb_px(x, y, ((x / 3) & 1) ? XW : XST);
    for (int t = 0; t < 6; t++) {
        const int tw = 22, x0 = 2 + t * 26;
        const bool alt = ((t + phase) & 1) != 0;
        const uint8_t fill = alt ? XCY : XTN, edge = alt ? XCE : XOR;
        for (int y = y0 + 2; y < y1 - 2; y++)
            for (int x = x0; x < x0 + tw; x++) {
                int cx = x - x0 < x0 + tw - 1 - x ? x - x0 : x0 + tw - 1 - x;
                int cy = y - y0 - 2 < y1 - 3 - y ? y - y0 - 2 : y1 - 3 - y;
                if (cx + cy < 3) continue;
                lb_px(x, y, (cx == 0 || cy == 0 || cx + cy == 3) ? edge : fill);
            }
        int bx = x0 + tw / 2 - 2, by = (y0 + y1) / 2 - 3;
        for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (LB_BOLT[r][c] == '#') lb_px(bx + c, by + r, alt ? XN : XOR);
    }
}
static void lb_draw(bool charging)
{
    vlcd_clear(XW);
    lb_band(0, 22, 0);
    lb_band(138, 160, 1);
    const int x0 = 1, x1 = VL - 2, y0 = 24, y1 = 135;  /* framed box: red, orange, grey shadow, white */
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int cx = x - x0 < x1 - x ? x - x0 : x1 - x, cy = y - y0 < y1 - y ? y - y0 : y1 - y;
            if (cx + cy < 2) continue;
            uint8_t c = XW;
            if (cx == 0 || cy == 0 || cx + cy == 2) c = XRD;
            else if (cx == 1 || cy == 1) c = XOR;
            else if ((cx == 2 || cy == 2) && (y - y0 == 2 || x - x0 == 2)) c = XGR;
            lb_px(x, y, c);
        }
    vlcd_text_center(31, "LOW BATTERY!!", XN);
    const char *const *spr = PUFFED[charging ? 1 : 0];
    const int sx = (VL - 33 * 2) / 2 - (charging ? 6 : 0), sy = 52;
    for (int r = 0; r < 15; r++)
        for (int c = 0; spr[r][c]; c++) {
            uint8_t k = lb_col(spr[r][c]);
            if (k != 0xFF) { lb_px(sx + c * 2, sy + r * 2, k); lb_px(sx + c * 2 + 1, sy + r * 2, k);
                             lb_px(sx + c * 2, sy + r * 2 + 1, k); lb_px(sx + c * 2 + 1, sy + r * 2 + 1, k); }
        }
    if (charging)                                  /* a big bolt beside it */
        for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) if (LB_BOLT[r][c] == '#')
            for (int d = 0; d < 4; d++) lb_px(sx + 70 + c * 2 + (d & 1), sy + 6 + r * 2 + (d >> 1), XOR);
    vlcd_text_center(96, "GAME SAVED", XN);
    vlcd_text_center(108, charging ? "CHARGING..." : "PLEASE CHARGE", charging ? XGN : XRD);
    vlcd_text_center(122, "C: MENU", XGR);
}

static void low_battery_screen_run(bool test)
{
    s_low_ack = true;
    int64_t t0 = esp_timer_get_time();
    bool was_chg = false;
    while (1) {
        battery_update();
        lb_draw(s_bat_chg);
        vlcd_present();
        if (s_bat_chg && !was_chg) { was_chg = true; t0 = esp_timer_get_time(); }
        if (!test && was_chg && esp_timer_get_time() - t0 > 2000000) return;   /* plugged in: back to the menu */
        for (int i = 0; i < 50; i++) {
            if (keys_poll() & 4) return;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_bat_time = 0;                           /* read the battery again (watch for the charger) */
    }
}
static void low_battery_screen(void) { low_battery_screen_run(false); }

static void quick_menu(void);
static void game_loop_run(void);
#if TL_RAMDUMP == 3
static void ram_snapshot(void);
static int64_t s_snap_at;                          /* timed snapshot (quick menu) */
#endif
/* EXPORT ROM (2026-11-20): the game's flash as it is now (the ROM with everything the game saved into it)
 * as a plain ROM image, /sdcard/exports/<rom>.bin, for other emulators (Paramix, tamaemu). The emulator is
 * stopped; written via a .tmp file. */
static void draw_loading(int percent);
static void show_error(const char *line1, const char *line2);
/* FORCED SAVE before export (2026-11-22). Every colour firmware we know (Plus Color, iD L, P's; same engine
 * in 4U+) saves its pet with one routine that takes no arguments: interrupts off, erase the save sector,
 * copy two RAM blocks halfword by halfword into it, interrupts on (Plus Color 0xc3d6b8: DSTRAM 0x84124 x46 +
 * A0RAM 0x168 x250 -> flash 0xffe000; iD L 0x24dd06e: 0x84224 x84 + 0xd5c x318 -> 0x200e000; P's 0x251fb0c:
 * 0x12a8 x156 + 0xefc x754 -> 0x200e000). It is found by its copy loop: "ld %r8,%r7; ld %r4,%r0; sll %r4,1"
 * followed by "add %r4,%r5; ld.uh %r7,[%r4]", the entry being the "pushn %r1" just before (unique in each ROM).
 * The export runs it in the stopped emulator, so the exported flash holds a save made by the game itself. */
static uint16_t rom16(Emu *e, uint32_t off)
{
    const uint8_t *p = e->rom_src[(off >> 16) & 127];
    return p ? (uint16_t)(p[off & 0xFFFF] | p[(off & 0xFFFF) + 1] << 8) : 0xFFFF;
}
static uint32_t find_save_routine(Emu *e)
{
    uint32_t found = 0; int hits = 0;
    for (uint32_t off = 40; off + 24 < e->dev.rom_size; off += 2) {
        if (rom16(e, off) != 0x2e78 || rom16(e, off + 2) != 0x2e04 || rom16(e, off + 4) != 0x8c14) continue;
        bool loop = false;
        for (uint32_t j = off + 6; j < off + 20; j += 2) if (rom16(e, j) == 0x2254 && rom16(e, j + 2) == 0x2c47) loop = true;
        if (!loop) continue;
        uint32_t entry = 0;
        for (uint32_t j = off - 2; j >= off - 40; j -= 2) if (rom16(e, j) == 0x0201) { entry = j; break; }
        if (!entry) continue;
        hits++; found = e->dev.rom_base + entry;
    }
    if (hits != 1) { ESP_LOGW(TAG, "Forced save: save routine %s", hits ? "found more than once - not used" : "not found"); return 0; }
    return found;
}
/* Run a guest routine to its end: its return address points outside every memory area, so the CPU core stops
 * there by itself (e->stopped, stop_pc). CPU registers are put back afterwards. Returns true if it returned. */
#define GCALL_RETURN 0x0FFFFFF0u
static bool guest_call(Emu *e, uint32_t func, uint64_t max_cycles)
{
    uint32_t r[16]; memcpy(r, e->r, sizeof r);
    const uint32_t sp = e->sp, pc = e->pc, alr = e->alr, ahr = e->ahr, psr_rest = e->psr_rest;
    const uint8_t fn = e->f_n, fz = e->f_z, fv = e->f_v, fc = e->f_c, ie = e->f_ie, il = e->il;
    const bool halted = e->halted, halt_hard = e->halt_hard;
    e->sp -= 4; mem_write32(e, e->sp, GCALL_RETURN);
    e->pc = func; e->halted = false; e->stopped = false;
    const uint64_t end = e->cycles + max_cycles;
    while (!e->stopped && e->cycles < end) tc_run(e, e->cycles + 65536 < end ? e->cycles + 65536 : end);
    const bool ok = e->stopped && (e->stop_pc & 0x0FFFFFFFu) == GCALL_RETURN && e->sp == sp;
    if (!ok) ESP_LOGW(TAG, "Forced save: routine did not return (stopped %d at %08lx, sp %08lx/%08lx)",
                      e->stopped, (unsigned long)e->stop_pc, (unsigned long)e->sp, (unsigned long)sp);
    memcpy(e->r, r, sizeof r);
    e->sp = sp; e->pc = pc; e->alr = alr; e->ahr = ahr; e->psr_rest = psr_rest;
    e->f_n = fn; e->f_z = fz; e->f_v = fv; e->f_c = fc; e->f_ie = ie; e->il = il;
    e->halted = halted; e->halt_hard = halt_hard; e->stopped = false;
    return ok;
}
static void force_game_save(Emu *e)                  /* emulator stopped (export) */
{
    int64_t t0 = esp_timer_get_time();
    uint32_t f = find_save_routine(e);
    if (!f) return;
    uint32_t p0 = s_flash_prog_n, e0 = s_flash_erase_n;
    bool ok = guest_call(e, f, 40000000ULL);
    ESP_LOGI(TAG, "Forced save: game's save routine at %08lx %s, %lu flash writes, %lu erases (%lld ms)",
             (unsigned long)f, ok ? "done" : "FAILED", (unsigned long)(s_flash_prog_n - p0), (unsigned long)(s_flash_erase_n - e0),
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void rom_export(void)
{
    const uint32_t size = s_emu->dev.rom_size;
    char path[128], tmp[132];
    mkdir("/sdcard/exports", 0777);
    {   /* <rom>_YYYY-MM-DD_HHMM.bin, so exports never overwrite each other */
        char base[64];
        snprintf(base, sizeof base, "%s", s_game_file);
        char *dot = strrchr(base, '.');
        if (dot) *dot = 0;
        rtc_time_t t; bool lost = false;
        if (rtc_read(&t, &lost))
            snprintf(path, sizeof path, "/sdcard/exports/%s_%04d-%02d-%02d_%02d%02d.sav", base, t.year, t.month, t.day, t.hour, t.minute);
        else snprintf(path, sizeof path, "/sdcard/exports/%s.sav", base);
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    uint8_t *buf = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
    FILE *f = buf ? fopen(tmp, "wb") : NULL;
    bool ok = f != NULL;
    char name_was[sizeof s_loading_name];
    memcpy(name_was, s_loading_name, sizeof name_was);
    snprintf(s_loading_name, sizeof s_loading_name, "EXPORTING SAVE");
    s_vlcd_direct = true;                          /* menus straight to the panel (game display mode) */
    if (s_lcd_off_shown) { low_power(false); s_lcd_off_shown = false; panel_wake_now(); bsp_display_brightness_set(s_brightness); }
    pm_hold(s_pm_ui, true);
    int64_t t0 = esp_timer_get_time();
    int last_pct = -1;
    for (uint32_t off = 0; ok && off < size; off += 32768) {
        uint32_t n = size - off < 32768 ? size - off : 32768;
        tc_rom_export(s_emu, off, buf, n);
        ok = fwrite(buf, 1, n, f) == n;
        int pct = (int)((uint64_t)(off + n) * 100 / size);
        if (pct / 2 != last_pct / 2) { last_pct = pct; draw_loading(pct); }
    }
    if (f) ok = (fclose(f) == 0) && ok;
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }
    else remove(tmp);
    free(buf);
    ESP_LOGI(TAG, "ROM export %s: %s (%lu bytes, %lld ms)", ok ? "done" : "FAILED", path, (unsigned long)size,
             (esp_timer_get_time() - t0) / 1000);
    show_error(ok ? "SAVE EXPORTED" : "EXPORT FAILED", ok ? "SD:/EXPORTS/" : "CHECK SD CARD");
    memcpy(s_loading_name, name_was, sizeof name_was);
    s_vlcd_direct = false;
}

static void vlcd_title(const char *title);
static void save_flag(const char *key, int v);
/* Versions the speed-ups know (2026-11-23). Others run, but drawing is interpreted and may be slow: say so once per
 * ROM file (owner: point people to GitHub to report it). NVS settings "w<fnv>" = shown. */
static bool rom_optimised(const char *file)
{
    static const uint32_t OK[] = { 0x00C2F0ECu, 0x00C4215Eu, 0x024CAA72u, 0x024CA8FEu, 0x024FDFE6u, 0x024F6256u,
                                   0x024BD18Au, 0x024C14F6u, 0x00C907FAu, 0x00C939EAu, 0x024C680Au };   /* + iD L 15th/Spacy, iD, iD Melody, 4U (2026-11-23) */
    uint32_t rv = rom_info(file)->rv;
    for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++) if (OK[i] == rv) return true;
    return false;
}
static void rom_speed_notice(const char *file)
{
    if (rom_optimised(file)) return;
    char key[12]; snprintf(key, sizeof key, "w%08lx", (unsigned long)rs_fnv((const uint8_t *)file, strlen(file)));
    nvs_handle_t nvs; int32_t seen = 0;
    if (nvs_open("settings", NVS_READONLY, &nvs) == ESP_OK) { nvs_get_i32(nvs, key, &seen); nvs_close(nvs); }
    if (seen) return;
    vlcd_clear(0);
    vlcd_title(IC_ABOUT " PLEASE NOTE");
    static const char *const L[] = { "THIS GAME VERSION HAS NOT", "BEEN TESTED OR OPTIMISED", "YET, SO IT MAY RUN SLOWLY.", "",
                                     "PLEASE REPORT IT (OR ASK", "FOR IT) ON GITHUB:", "GITHUB.COM/WAVIEGOTCHI/",
                                     "TAMANEST" };
    const int n = sizeof L / sizeof L[0];
    vlcd_rect(6, 40, VL - 12, 10 + n * 9, 1);
    for (int i = 0; i < n; i++) vlcd_text_small_center(45 + i * 9, L[i], 2);
    vlcd_text_center(134, "B:OK", 1);
    vlcd_clock(); vlcd_battery(); vlcd_present();
    while (!(keys_poll() & 6)) vTaskDelay(pdMS_TO_TICKS(10));
    save_flag(key, 1);
}

static void game_broken_screen(void)               /* after a game that crashed: say why nothing was saved */
{
    if (!s_game_broken) return;
    s_game_broken = false;
    vlcd_clear(0);
    vlcd_title(IC_ABOUT " NOT SAVED");
    static const char *const L[] = { "THE GAME STOPPED WORKING", "PROPERLY, SO THIS SESSION", "WAS NOT SAVED.", "", "YOUR LAST GOOD SAVE IS KEPT." };
    const int n = sizeof L / sizeof L[0];
    vlcd_rect(6, 44, VL - 12, 10 + n * 9, 1);
    for (int i = 0; i < n; i++) vlcd_text_small_center(49 + i * 9, L[i], 2);
    vlcd_text_center(134, "B:OK", 1);
    vlcd_clock(); vlcd_battery(); vlcd_present();
    while (!(keys_poll() & 6)) vTaskDelay(pdMS_TO_TICKS(10));
}

static void game_loop(void)                       /* menus run at full speed; in a game only what is busy */
{
    pm_hold(s_pm_ui, false);
    game_loop_run();
    pm_hold(s_pm_ui, true);
}
static void game_loop_run(void)
{
    int64_t last_frame = 0, last_change = 0, last_save = esp_timer_get_time();
    uint64_t last_lcd = UINT64_MAX;
    bool dirty = false;
    uint32_t sleeps_mark = 0, idle_mark = 0;
    int64_t dirty_since = 0;
    int64_t c_down_since = 0, c_last_up = 0, c_last_down = 0;
    bool c_double = false, c_block = true;          /* C may still be held from the menu */
    if (s_bat_pct != -2) xSemaphoreGive(s_present_req);   /* draw the known level at once */
    int64_t low_since = 0;
    s_low_exit = false;
    const int64_t loop_t0 = esp_timer_get_time();
    int64_t wake_until = 0, care_c_from = 0;       /* care_c_from: a 150 ms C press for the game after EZCARE cured it */
    /* loaded while asleep: wake it, so the screen is not blank. ONLY for a restored save: on a fresh start the
     * press landed in the middle of the game's boot - Hexagontchi then entered a test mode (character select
     * instead of the egg, clock changeable with C; 2026-11-22) */
    bool wake_check = s_game_restored;
    uint8_t wake_keys = 0;                          /* keys pressed while the game's screen was off: they only wake it */
    uint8_t qm_hold = 0;                            /* keys held when the quick menu closed */
    int64_t wake_press_t = 0;
    while (1) {
        keys_poll();
        int64_t now = esp_timer_get_time();
        if (wake_check && now - loop_t0 > 400000) {
            wake_check = false;
            if (s_emu && !(s_emu->lcd.disp_on && s_emu->lcd.sleep_out)) { wake_until = now + 150000; ESP_LOGI(TAG, "Loaded asleep: pressing A to wake the screen"); }
        }
        /* Wake press timing log (diagnostic). */
        const bool scr_off = s_emu && !(s_emu->lcd.disp_on && s_emu->lcd.sleep_out);
        if (scr_off) {
            if (s_keys & ~wake_keys) { if (!wake_keys) wake_press_t = now; wake_keys |= s_keys; }
        } else if (wake_keys) {
            if (wake_press_t) { ESP_LOGI(TAG, "Wake press: screen on %d ms after the press (key %s)", (int)((now - wake_press_t) / 1000), (s_keys & wake_keys) ? "still held" : "released"); wake_press_t = 0; }
            wake_keys = 0;
        }
        uint8_t keys_game = s_keys;   /* the game decides itself (real device: the waking press is not a game press) */
        qm_hold &= s_keys;                                /* keys still held from the quick menu: not for the game */
        keys_game &= (uint8_t)~qm_hold;
        s_btn_mask = (uint8_t)((c_block ? (keys_game & ~4) : keys_game) | (now < wake_until ? 1 : 0)
                             | (care_c_from && now >= care_c_from && now < care_c_from + 150000 ? 4 : 0));   /* the C that closed a menu is not for the game */
        /* C button: held 2 s = quick menu (game keeps running); tap, then hold 2 s = save and back to the
         * ROM menu. The game also sees the C presses, as before with A. */
        if (c_block) { if (!(s_keys & 4)) c_block = false; }
        else if (s_keys & 4) {
            if (!c_down_since) {
                c_double = now - c_last_up < DOUBLE_TAP_MS * 1000LL && c_last_up - c_last_down < DOUBLE_TAP_MS * 1000LL;
                c_last_down = c_down_since = now;
            } else if (now - c_down_since >= MENU_HOLD_MS * 1000LL) {
                if (c_double) break;                      /* back to the ROM menu (saves) */
                quick_menu();
                if (s_care_redraw) { s_care_redraw = false; care_c_from = esp_timer_get_time() + 300000; }
                keys_poll();
                qm_hold = s_keys;                         /* e.g. the B that chose SNAPSHOT / RESTART ROM */
                if (s_export_req || s_quit_req) break;                  /* EXPORT ROM + EXIT */
                c_down_since = 0; c_block = true;          /* ignore C until it is released */
                last_frame = 0;
                continue;
            }
        } else if (c_down_since) { c_last_up = now; c_down_since = 0; }

        /* Show a frame once the game has finished drawing it (no screen writes for 15 ms), so new screens
         * appear in one go instead of top to bottom; during long animations still at least every 120 ms. */
        /* Only finished windows are shown (lcd_done_hook). Send once the game has finished a group of windows:
         * it slept, or no window finished for 8 ms; during long animations at least every 120 ms (still
         * finished windows only). Fallback for drawing that never finishes a window: after 60 ms without
         * screen writes, or 300 ms of drawing, the whole screen memory is taken as it is. */
        if (s_emu && s_emu->lcd_writes != last_lcd) {
            if (!dirty) dirty_since = now;
            last_lcd = s_emu->lcd_writes; last_change = now; dirty = true; sleeps_mark = tc_sleeps; idle_mark = tc_idle_events;
        }
        bool slept = tc_sleeps != sleeps_mark;
        if (dirty && !s_ready_new && now - s_ready_t >= 300000 && (now - last_change >= 60000 || now - dirty_since >= 300000)) {
            ready_take_all();                         /* no finished window for a while: take what is there */
            dirty = false;
            s_why[1]++;
        }
        /* 2026-11-18: up to ~60 fps, and at least every 40 ms while the game draws without a pause (was 120 ms
         * and 30 fps: quick animations like the apples' wobble lost frames, busy scenes looked slow) */
        /* LIVE DRAW off: only complete screens. A menu change in iD L / P's is several windows drawn one after
         * another; wait until the game is idle again (slept, or an idle/busy loop was skipped), or 50 ms
         * without screen writes, or at most 300 ms. */
        bool idled = tc_idle_events != idle_mark;
        bool quick = ((slept || now - s_ready_t >= 6000) && now - last_frame >= 16000) || now - last_frame >= 40000;
        bool show = s_draw_mode != 2 ? quick
            : s_ready_big ? (((slept || idled || now - last_change >= 50000) && now - last_frame >= 16000) || now - last_frame >= 300000)
                          : quick;               /* SMOOTH: only sprites/small parts changed - show them like BALANCED */
        if (s_ready_new && show) {
            s_why[slept ? 0 : now - last_frame >= 40000 ? 2 : 1]++;
            s_ready_big = false;
            last_frame = now;
            if (now - last_change >= 8000) dirty = false;
            xSemaphoreGive(s_present_req);                /* the display task sends it */
        }
        if (battery_update()) { s_bat_redraw = true; xSemaphoreGive(s_present_req); }   /* checks every 10 s */
        if (s_bars_wait_frame && now - s_bars_wait_since > 3000000) { s_bars_wait_frame = false; s_bars_shown_loc = s_bar_loc; s_bars_redraw = true; xSemaphoreGive(s_present_req); }
        if (s_bar_game && s_emu) {                    /* status bars: watch hunger / happiness */
            static int64_t last_bar;
            if (now - last_bar >= 250000) {
                last_bar = now;
                int h, p;
                /* Hunger and happiness from the pet-data table (Paramix Studio's CARE, per reset vector): every known
                 * version and edition of a model gets its bars (2026-11-22; before, fixed addresses for the EN dumps). */
                const CareInfo *c = care_find(s_emu);
                if (s_bar_game >= 2 || (s_bar_game == 1 && c)) {
                    h = c ? *care_p(s_emu, c->hun) : -1; p = c ? *care_p(s_emu, c->hap) : -1;
                } else { h = s_emu->dstram[0x125 + s_pet_shift]; p = s_emu->dstram[0x12c + s_pet_shift]; }   /* Hexagontchi +4 (unconfirmed for the bars) */
                /* A patch (e.g. the 4U English patch, delivered as a destination) rewrites big parts of the flash for
                 * minutes; meanwhile the game's version can't be read and the bars blinked. 4+ erases within 10 s
                 * (a normal save erases 1-2) hide the bars until 10 s after the last erase (owner 2026-11-23). */
                {   uint32_t i = s_erase_i;
                    if (i >= 4 && now - s_erase_t[(i - 4) & 7] < 10000000) s_patch_until = now + 10000000;
                    if (now < s_patch_until) { h = -1; p = -1; }   }
                /* Away from home (2026-11-21): no location variable exists, but the game runs every outing (game
                 * centre, market, park, remodelers, lottery, ...) inside one routine. Its return address sits at a
                 * fixed place on the stack (A0RAM 0x4fcc = 0xc37d86 while out; home 0xc383ca, menus 0xc37d06 /
                 * 0xc37cda, events e.g. 0xc3824e). Only trusted when the bottom frame is the known main
                 * (0x4ffc = 0xc2f1a0), so another ROM version just keeps the bars on. */
                const uint8_t *st = s_emu->a0ram;
                uint32_t ret = st[0x4fcc] | st[0x4fcd] << 8 | st[0x4fce] << 16 | (uint32_t)st[0x4fcf] << 24;
                uint32_t top = st[0x4ffc] | st[0x4ffd] << 8 | st[0x4ffe] << 16 | (uint32_t)st[0x4fff] << 24;
                int loc = 0;                              /* 1 = away */
                if (s_bar_game == 1) loc = top == 0x00c2f1a0 && ret == 0x00c37d86;
                else {   /* P's / iD L (2026-11-21, same method): one "going out" routine for every destination */
                    const int o = s_bar_game == 2 || s_bar_game == 4 ? 0x4f3c : 0x4fd4;
                    uint32_t r2 = st[o] | st[o + 1] << 8 | st[o + 2] << 16 | (uint32_t)st[o + 3] << 24;
                    if (s_bar_game == 2) loc = top == 0x024fe0a0 && r2 == 0x0240c08a;   /* P's: A0RAM 0x4f3c */
                    else if (s_bar_game == 4) loc = top == 0x024f6310 && r2 == 0x0240b23e;   /* 4U+: A0RAM 0x4f3c (TamaDepa, park) */
                    else loc = top == 0x024cab2c && r2 == 0x02408334;                   /* iD L: A0RAM 0x4fd4 */
                }
                {   /* happiness maximum: grows with medals / Happy Symbols / puzzle pieces (CARE table) */
                    const CareInfo *c = care_find(s_emu);
                    int hm = c ? care_happy_max(s_emu, c) : -1;
                    if (hm != s_bar_hmax) {
                        ESP_LOGI(TAG, "Status bars: happiness maximum %d (medal byte %02x)", hm, c ? *care_p(s_emu, c->med) : 0);
                        s_bar_hmax = hm; s_bars_redraw = true; xSemaphoreGive(s_present_req);
                    }
                }
                if (h != s_bar_hunger || p != s_bar_happy || loc != s_bar_loc) {
                    if (loc != s_bar_loc) ESP_LOGI(TAG, "Status bars: pet %s", loc ? "away from home (bars hidden)" : "at home");
                    if (h != s_bar_hunger || p != s_bar_happy) ESP_LOGI(TAG, "Status bars: hunger %d, happiness %d", h, p);
                    if (s_bar_loc >= 0 && loc != s_bar_loc) { s_bars_wait_frame = true; s_bars_wait_since = now; }   /* home <-> away */
                    else if (s_bar_loc < 0) s_bars_shown_loc = loc;   /* first reading after the game started */
                    s_bar_hunger = h; s_bar_happy = p; s_bar_loc = loc; s_bars_redraw = true; xSemaphoreGive(s_present_req); }
            }
        }
        if (s_tr_t0 && now > s_tr_until && !TL_DEVLOG) s_tr_t0 = 0;   /* (release: no press traces) */
        if (s_tr_t0 && now > s_tr_until) {            /* press trace finished: print it */
            static const char *const NM[3] = { "presses", "drawn", "sent" };
            volatile uint16_t *const A[3] = { s_tr_press, s_tr_draw, s_tr_sent };
            const int N[3] = { s_tr_np, s_tr_nd, s_tr_ns };
            for (int k = 0; k < 3; k++) {
                printf("TRACE %s (%d):", NM[k], N[k]);
                for (int i = 0; i < N[k]; i++) printf(" %u", A[k][i]);
                printf("\n");
            }
            printf("TRACE emulator behind max %ld us (at %ld ms) | frame send max %ld us | display %s\n",
                   (long)s_tr_behind_max, (long)s_tr_behind_at, (long)s_tr_send_max, DRAW_MODE_NAME[s_draw_mode]);
            {   double ex = (double)s_tr_exec, h = (double)s_tr_host, g2 = (double)s_tr_g2, h2 = (double)s_tr_h2;
                printf("TRACE cost: %.1f ESP cycles per executed guest cycle (%.1f M executed) | get-pixel %.0f%% of executed, %.1f ESP/cycle | rest %.1f ESP/cycle\n",
                       ex > 0 ? h / ex : 0, ex / 1e6, ex > 0 ? 100 * g2 / ex : 0, g2 > 0 ? h2 / g2 : 0, ex - g2 > 0 ? (h - h2) / (ex - g2) : 0); }
            s_tr_t0 = 0;
        }
        {   /* how often the game measures its battery (finding its low-battery warning) */
            static uint32_t seen_reads; static int64_t first_t;
            uint32_t r = g_tc_bat_reads;
            if (r != seen_reads) {
                if (!first_t) first_t = now;
                ESP_LOGI(TAG, "Game measured its battery (%lu times, %lld s since the first; reading 0x%03x)",
                         (unsigned long)r, (now - first_t) / 1000000, g_tc_bat_adc);
                seen_reads = r;
            }
        }
        {   /* SCREENSHOT countdown: 3, 2, 1 in the border clock, then the display task copies the picture */
            static int64_t shot_msg_until;
            if (s_shot_at) {
                int left = (int)((s_shot_at - now + 999999) / 1000000);
                if (left > 0) {
                    if (left != s_shot_show) { s_shot_show = left; s_clock_redraw = true; xSemaphoreGive(s_present_req); }
                } else if (!s_shot_req_grab && !s_shot_grabbed) { s_shot_req_grab = true; xSemaphoreGive(s_present_req); }
            }
            if (s_shot_grabbed) {
                s_shot_grabbed = false; s_shot_at = 0;
                bsp_display_brightness_set(s_brightness / 4);   /* shutter blink (also where there is no border) */
                vTaskDelay(pdMS_TO_TICKS(70));
                bsp_display_brightness_set(s_brightness);
                char name[64];
                s_shot_show = shot_save(name, sizeof name) ? -1 : -2;
                s_clock_redraw = true; xSemaphoreGive(s_present_req);
                shot_msg_until = esp_timer_get_time() + 1500000;
            }
            if (shot_msg_until && now > shot_msg_until) {
                shot_msg_until = 0; s_shot_show = 0; s_clock_redraw = true; xSemaphoreGive(s_present_req);
            }
        }
        {   static int64_t last_clk; static int last_min = -2;   /* in-game clock: once a second, redraw on a new minute */
            if (now - last_clk >= 1000000) { last_clk = now; int mm = clock_minute(); if (mm != last_min) { last_min = mm; s_clock_redraw = true; xSemaphoreGive(s_present_req); } } }
#if TL_RAMDUMP == 3
        if (s_snap_at && now >= s_snap_at) { s_snap_at = 0; ram_snapshot(); }
        else
#endif
        if (s_bat_pct >= 0 && (s_bat_chg || s_bat_pct <= 10)) {
            static int64_t last_anim;                 /* charging / nearly empty: animate (checked twice a second) */
            if (now - last_anim >= 500000) { last_anim = now; s_bat_redraw = true; xSemaphoreGive(s_present_req); }
        }
        if (s_bat_pct > 8 || s_bat_chg) s_low_ack = false;
        if (s_bat_pct >= 0 && s_bat_pct <= LOW_BAT_PCT && !s_bat_chg && !s_low_ack) {
            if (!low_since) low_since = now;
            else if (now - low_since >= 15000000) { s_low_exit = true; break; }   /* save and ask to charge */
        } else low_since = 0;
        if (s_pet_asleep && s_keys && s_emu_task_h) xTaskNotifyGive(s_emu_task_h);   /* a key: wake the napping emulator */
        if (now - last_save >= SAVE_EVERY_S * 1000000LL) {   /* periodic save: snapshot between run chunks */
            last_save = now;
            s_snap_done = false; s_snap_req = true;
            for (int i = 0; i < 100 && !s_snap_done; i++) vTaskDelay(pdMS_TO_TICKS(2));
            s_snap_req = false;
            if (s_snap_done) save_write_snap(s_game_file);
        }
        {   /* asleep: look at the keys once a second, but a key interrupt wakes us at once; after a key, 10 ms
             * polling for a second so the press is seen (debounce needs two reads) */
            static int64_t last_key_t;
            if (s_keys) last_key_t = now;
            bool slow = s_pet_asleep && !s_keys && now - last_key_t > 1000000;
            key_irq(slow);
            if (slow) { if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000))) last_key_t = esp_timer_get_time(); }   /* the key interrupt wakes us */
            else vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    key_irq(false);
    /* leaving the game: stop the emulator, then save */
    s_present_on = false;
    while (s_presenting) vTaskDelay(pdMS_TO_TICKS(2));
    s_emu_running = false;
    while (s_emu_task_alive) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_emu) { snap_capture(); save_write_snap(s_game_file); }
    if (s_emu && s_export_req && !s_game_broken) { force_game_save(s_emu); rom_export(); }   /* .sav above keeps the state from before */
    s_export_req = false; s_quit_req = false;
    s_shot_at = 0; s_shot_show = 0; s_shot_req_grab = false; s_shot_grabbed = false;   /* no screenshot countdown left over */
    game_stop();
    game_display_end();
    /* The menu appears at once; A is still held, but the menu only reacts to new presses. */
}

/* ------------------------------------------------------------------------------------------------ */
/* Launcher menu                                                                                     */
/* ------------------------------------------------------------------------------------------------ */

#define MAX_ROMS 32
static char s_rom_file[MAX_ROMS][64];
static char s_rom_name[MAX_ROMS][24];
static int s_rom_count;

static void scan_roms(void)
{
    s_rom_count = 0;
    DIR *dir = opendir("/sdcard/roms");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_rom_count < MAX_ROMS) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 5 || len >= sizeof s_rom_file[0]) continue;
        if (strcasecmp(ent->d_name + len - 4, ".bin") != 0) continue;
        snprintf(s_rom_file[s_rom_count], sizeof s_rom_file[0], "%s", ent->d_name);
        rom_display_name(ent->d_name, s_rom_name[s_rom_count], sizeof s_rom_name[0]);
        s_rom_count++;
    }
    closedir(dir);
    {   /* stored ROMs whose files are gone free their space (2026-11-22) */
        static uint32_t ids[MAX_ROMS];
        for (int i = 0; i < s_rom_count; i++) ids[i] = rs_fnv((const uint8_t *)s_rom_file[i], strlen(s_rom_file[i]));
        if (s_rom_count < MAX_ROMS) rs_prune(ids, s_rom_count);   /* (a full list may have missed some files) */
    }
    /* Order (2026-11-21): by release year, oldest first, then by name; unknown years last. Alternatives the
     * owner may choose later: newest first, by popularity (a fixed ranking), alphabetical. */
    for (int i = 1; i < s_rom_count; i++)
        for (int j = i; j > 0; j--) {
            const char *ya = rom_year(s_rom_file[j - 1]), *yb = rom_year(s_rom_file[j]);
            int ka = ya[0] ? atoi(ya) : 9999, kb = yb[0] ? atoi(yb) : 9999;
            if (s_rom_order == 1) { ka = ya[0] ? -ka : 1; kb = yb[0] ? -kb : 1; }   /* newest first, unknown last */
            if (s_rom_order == 2) ka = kb = 0;                                      /* A-Z: by name only */
            if (ka < kb || (ka == kb && strcmp(s_rom_name[j - 1], s_rom_name[j]) <= 0)) break;
            char tf[sizeof s_rom_file[0]], tn[sizeof s_rom_name[0]];
            memcpy(tf, s_rom_file[j], sizeof tf); memcpy(s_rom_file[j], s_rom_file[j - 1], sizeof tf); memcpy(s_rom_file[j - 1], tf, sizeof tf);
            memcpy(tn, s_rom_name[j], sizeof tn); memcpy(s_rom_name[j], s_rom_name[j - 1], sizeof tn); memcpy(s_rom_name[j - 1], tn, sizeof tn);
        }
    /* Same model more than once (e.g. an English and a Japanese dump - their code fingerprints are identical,
     * only text and pictures differ): add the language from the file name, so the rows can be told apart
     * (owner 2026-11-22). Names with en/eng/english -> EN, jp/jpn/ja/japan -> JP. */
    for (int i = 0; i < s_rom_count; i++) {
        RomModel mi = rom_info(s_rom_file[i])->m;
        bool dup = false;
        for (int j = 0; j < s_rom_count; j++) if (j != i && mi != M_UNKNOWN && rom_info(s_rom_file[j])->m == mi && !name_has(s_rom_file[j], "export")) dup = true;
        if (!dup || name_has(s_rom_file[i], "export")) continue;
        char base[64]; snprintf(base, sizeof base, "%s", s_rom_file[i]);
        for (char *c = base; *c; c++) *c = (char)tolower((unsigned char)*c);
        char *dot = strrchr(base, '.'); if (dot) *dot = 0;
        const char *lang = NULL;
        for (char *t = strtok(base, "_- ."); t; t = strtok(NULL, "_- ."))
            if (!strcmp(t, "en") || !strcmp(t, "eng") || !strcmp(t, "english")) lang = "EN";
            else if (!strcmp(t, "jp") || !strcmp(t, "jpn") || !strcmp(t, "ja") || !strcmp(t, "japan") || !strcmp(t, "japanese")) lang = "JP";
        if (lang && strlen(s_rom_name[i]) + 3 < sizeof s_rom_name[0]) { strcat(s_rom_name[i], " "); strcat(s_rom_name[i], lang); }
    }
    ESP_LOGI(TAG, "Found %d ROM(s) in /sdcard/roms", s_rom_count);
}

#define TITLE_Y    22    /* menu title, just below the clock/battery row (was 40, owner 2026-11-22) */
#define LIST_Y     36    /* first list row (virtual LCD pixels) */
#define LIST_PITCH 11
#define LIST_ROWS  11

/* Clock in the top left of the launcher screens (same row as the battery on the right). */
static bool shared_time(uint32_t *secs);
static int s_clock_shown = -1;
static int clock_minute(void)                     /* minutes since midnight, -1 if the clock is not set */
{
    uint32_t t;
    return shared_time(&t) ? (int)(t % 86400 / 60) : -1;
}
static void vlcd_clock(void)
{
    int m = clock_minute();
    s_clock_shown = m;
    vlcd_topbar();                                /* band first (clock and battery are drawn on it) */
    if (m < 0) vlcd_text(13, BAT_Y / 3 + 2, "--:--", topbar_ink());
    else vlcd_time12(13, BAT_Y / 3 + 2, m / 60, m % 60, topbar_ink());   /* 12 h, small AM/PM */
}

/* Menu title (owner 2026-11-22, option T2): mid-shade band from under the top bar down to the line, dark text. */
static const char *s_title_right;                 /* small text at the right end of the title band (UPDATE: version) */
static void vlcd_title(const char *title)
{
    vlcd_rect(0, 16, VL, TITLE_Y + 10 - 16, 1);
    if ((unsigned char)title[0] == (unsigned char)IC_HOME[0]) {
        /* TamaNest's own icon, bigger than the 8x8 menu icons (owner's pick H7, 2026-11-22): egg with a shine
         * in a woven nest, 12x11, bottom in line with the title text; the text starts where it always does */
        static const uint16_t NEST_ICON[11] = { 0x0F00, 0x1080, 0x2840, 0x3040, 0x5020, 0x4020, 0x4020, 0xFFF0, 0xA950, 0x56A0, 0x3FC0 };
        for (int r = 0; r < 11; r++)
            for (int b = 0; b < 12; b++)
                if (NEST_ICON[r] & (0x8000 >> b)) vlcd_rect(4 + b, TITLE_Y - 3 + r, 1, 1, 2);
        /* the name at 1.33x (4 panel pixels per font pixel, like the loading screen title), drawn at full
         * resolution by the overlay: as tall as the icon, centred on it (owner's pick B) */
        s_ov_text = title[1] == ' ' ? title + 2 : title + 1;
        s_ov_x = 20 * 3;
        s_ov_y = (TITLE_Y - 3) * 3 + (11 * 3 - 7 * 4 + 1) / 2;   /* letters (28 px) centred on the icon (33 px), not bottom-aligned */
    } else vlcd_text(4, TITLE_Y, title, 2);
    vlcd_rect(0, TITLE_Y + 10, VL, 1, 2);
    if (s_title_right) {                           /* bottom in line with the title letters */
        int w = vlcd_text_small(0, 0, s_title_right, 2, true);
        vlcd_text_small(VL - 6 - w, TITLE_Y + 1, s_title_right, 2, false);
    }
}

/* "LABEL" with its value right-aligned to the edge of the list row. */
static void list_item(char *out, size_t n, const char *label, const char *value)
{
    const int width = 19;                         /* characters from x 4 to the right edge */
    char lb[32];
    snprintf(lb, sizeof lb, "%s", label);
    if ((unsigned char)lb[0] < 32 && lb[1] == ' ' && (int)(strlen(lb) + strlen(value)) + 1 > width)
        memmove(lb + 1, lb + 2, strlen(lb + 2) + 1);   /* too long: the icon goes right against the text */
    label = lb;
    int pad = width - (int)strlen(label) - (int)strlen(value);
    if (pad < 1) pad = 1;
    snprintf(out, n, "%s%*s%s", label, pad, "", value);
}

/* Draw a list screen: title with a rule under it, items, the selected one on a highlight bar. */
static int s_list_sep = -1;
static int s_list_rows = 0;                         /* items on screen; 0 = LIST_ROWS (two-line lists set fewer) */   /* item index above which draw_list puts a faded line (quick menu: before RESTART ROM) */
static void draw_list(const char *title, const char *const *items, int n, int sel, int top)
{
    vlcd_clear(0);
    vlcd_title(title);
    if (n == 0) { vlcd_text(4, LIST_Y + 2, "NO ROMS FOUND", 1); vlcd_text_small(4, LIST_Y + 16, "PUT .BIN FILES IN SD:/ROMS/", 1, false); }
    /* rows from the separator on sit SEP_GAP lower, the line in the middle of that gap (owner 2026-11-22: cramped) */
    #define SEP_GAP 6
    const int rows = s_list_rows ? s_list_rows : LIST_ROWS;
    const bool sep_vis = s_list_sep > top && s_list_sep < top + rows && s_list_sep < n;
    int extra = 0;                                /* rows taken by two-line items above this one */
    for (int i = 0; i < rows && top + i < n; i++) {
        int y = LIST_Y + (i + extra) * LIST_PITCH + (sep_vis && top + i >= s_list_sep ? SEP_GAP : 0);
        bool on = (top + i == sel);
        const char *nl = strchr(items[top + i], '\n');   /* two-line item (UPDATE > WI-FI: the network below) */
        if (on) vlcd_rect(0, y, VL, nl ? 2 * LIST_PITCH : LIST_PITCH, 3);
        if (nl) {
            char first[32];
            snprintf(first, sizeof first, "%.*s", (int)(nl - items[top + i]), items[top + i]);
            vlcd_text(4, y + 2, first, on ? 2 : 1);
            {   unsigned char c0 = (unsigned char)items[top + i][0];   /* under the label text after an icon, else a small indent */
                vlcd_text(c0 < 32 || c0 > 126 ? 20 : 12, y + 2 + LIST_PITCH, nl + 1, on ? 2 : 1); }
            extra++;
            continue;
        }
        vlcd_text(4, y + 2, items[top + i], on ? 2 : 1);
        if (on && s_list_right && s_list_right[top + i] && s_list_right[top + i][0] &&   /* highlighted row only, */
            4 + (int)strlen(items[top + i]) * 8 + 4 <= VL - 4 - vlcd_small_digits(0, 0, s_list_right[top + i], 0, true)) {   /* if it fits */
            int w = vlcd_small_digits(0, 0, s_list_right[top + i], 0, true);
            vlcd_small_digits(VL - 4 - w, y + 2, s_list_right[top + i], on ? XYEAR : 1, false);
        }
    }
    if (sep_vis) {
        int y = LIST_Y + (s_list_sep - top) * LIST_PITCH + SEP_GAP / 2;   /* faded line (light shade) in the gap */
        vlcd_rect(4, y, VL - 8, 1, 3);
    }
    if (n > rows) {                                /* scroll bar at the right edge, past the end of the text (the old ^ and v */
        const int bx = VL - 3, by = LIST_Y + 1;   /* arrows drew over values such as time-zone offsets, owner 2026-11-23) */
        const int bh = LIST_ROWS * LIST_PITCH - 6;
        int th = bh * rows / n; if (th < 6) th = 6;
        int ty = by + (bh - th) * top / (n - rows);
        vlcd_rect(bx, by, 2, bh, 3);               /* track: light shade */
        vlcd_rect(bx, ty, 2, th, 2);               /* thumb: dark ink (mid shade was too close to the track) */
    }
    vlcd_clock();
    vlcd_battery();
    vlcd_present();
    s_ov_text = NULL; s_ov_x = -1;                 /* a big title (vlcd_title) is only for this screen */
}

/* One list screen driven by the keys: A = next, B = choose, C = back.
 * Returns the chosen index, -1 for back, or -2 after refresh_ms without a key (0 = wait for ever) so the
 * caller can update the items (for example the clock) and call again. *sel_io keeps the selection. */
static int run_list_refresh(const char *title, const char *const *items, int n, int *sel_io, int refresh_ms)
{
    int sel = *sel_io, top = 0;
    bool redraw = true;
    int64_t until = refresh_ms ? esp_timer_get_time() + (int64_t)refresh_ms * 1000 : 0;
    int64_t last_check = esp_timer_get_time();
    while (1) {
        if (until && esp_timer_get_time() >= until) { *sel_io = sel; return -2; }
        if (esp_timer_get_time() - last_check >= 1000000) {   /* new minute: redraw for the clock (and battery) */
            last_check = esp_timer_get_time();
            if (clock_minute() != s_clock_shown) redraw = true;
        }
        {   /* look-ahead (owner 2026-11-23: five rows): the list scrolls before the highlight gets near the
             * edge, so the next items (or the previous ones) can be seen; short screens keep it near the middle */
            const int rows = s_list_rows ? s_list_rows : LIST_ROWS;
            const int ctx = 5 < (rows - 1) / 2 ? 5 : (rows - 1) / 2;
            if (sel < top + ctx) top = sel - ctx;
            if (sel > top + rows - 1 - ctx) top = sel - rows + 1 + ctx;
            if (top > n - rows) top = n - rows;
            if (top < 0) top = 0;
        }
        if (redraw) { draw_list(title, items, n, sel, top); redraw = false; }
        uint8_t down = keys_poll();
        static int64_t a_t, a_rep;                    /* hold A to scroll (long lists: ROMs, DLC) */
        int64_t tnow = esp_timer_get_time();
        if (down & 1) { a_t = tnow; a_rep = tnow + 450000; }
        if ((down & 1) && n > 0) { sel = (sel + 1) % n; redraw = true; }
        else if ((s_keys & 1) && a_t && tnow >= a_rep && n > 1) { a_rep = tnow + 90000; if (sel + 1 < n) { sel++; redraw = true; } }
        if (!(s_keys & 1)) a_t = 0;
        *sel_io = sel;
        if ((down & 2) && n > 0) return sel;
        if (down & 4) return -1;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static int run_list(const char *title, const char *const *items, int n, int sel)
{
    return run_list_refresh(title, items, n, &sel, 0);
}

/* Screen brightness in percent (10..100), remembered in NVS: s_brightness (declared at the top). */

static void brightness_load_apply(void)
{
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READONLY, &nvs) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(nvs, "bright", &v) == ESP_OK && v >= 10 && v <= 100) s_brightness = (int)v;
        if (nvs_get_i32(nvs, "sound", &v) == ESP_OK && v >= 0 && v <= 100) s_sound = (int)v;
        if (nvs_get_i32(nvs, "colour", &v) == ESP_OK && v >= 50 && v <= 100) s_colour = (int)v;
        if (nvs_get_i32(nvs, "lcdlook", &v) == ESP_OK) s_lcd_look = v != 0;
        if (nvs_get_i32(nvs, "flip", &v) == ESP_OK) s_flip = v != 0;
        if (nvs_get_i32(nvs, "drawmode", &v) == ESP_OK && v >= 0 && v <= 2) s_draw_mode = (int)v;
        if (nvs_get_i32(nvs, "gclock", &v) == ESP_OK) s_game_clock = v != 0;
        if (nvs_get_i32(nvs, "shell", &v) == ESP_OK) s_shell_on = v != 0;
        if (nvs_get_i32(nvs, "batshow", &v) == ESP_OK) s_battery_on = v != 0;
        if (nvs_get_i32(nvs, "bars", &v) == ESP_OK) s_bars_on = v != 0;
        if (nvs_get_i32(nvs, "romorder", &v) == ESP_OK && v >= 0 && v <= 2) s_rom_order = (int)v;
        if (nvs_get_i32(nvs, "menubar", &v) == ESP_OK && v >= 0 && v <= 5) s_menu_bar = (int)v;
        if (nvs_get_i32(nvs, "loader", &v) == ESP_OK && v >= 0 && v <= 1) s_loader = (int)v;
        if (nvs_get_i32(nvs, "size", &v) == ESP_OK && (v == 12 || v == 14 || v == 15)) s_size_q = (int)v;   /* (3.25x was dropped) */
        nvs_close(nvs);
    }
    bsp_display_brightness_set(s_brightness);
    colour_lut_build();
    if (s_flip) bsp_display_rotation_set(BSP_DISPLAY_ROTATE_90);   /* MADCTL 0x60: the default 0xA0 turned 180 */
    size_tables();
}

static void game_clock_toggle(void)
{
    s_game_clock = !s_game_clock;
    s_clock_redraw = true;
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READWRITE, &nvs) == ESP_OK) { nvs_set_i32(nvs, "gclock", s_game_clock); nvs_commit(nvs); nvs_close(nvs); }
}

static void save_flag(const char *key, int v)
{
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READWRITE, &nvs) == ESP_OK) { nvs_set_i32(nvs, key, v); nvs_commit(nvs); nvs_close(nvs); }
}
static void shell_toggle(void) { s_shell_on = !s_shell_on; s_bezel_col = s_shell_on ? s_shell_rom_col : 0; save_flag("shell", s_shell_on); }
static void battery_toggle(void) { s_battery_on = !s_battery_on; save_flag("batshow", s_battery_on); }
static void bars_toggle(void) { s_bars_on = !s_bars_on; s_bars_redraw = true; save_flag("bars", s_bars_on); }
static void rom_order_step(void) { s_rom_order = (s_rom_order + 1) % 3; save_flag("romorder", s_rom_order); }

/* Option picker with an explanation (2026-11-22, owner: SIZE shows the size, DISPLAY explains each mode):
 * A = next option, B = use it, C = back unchanged. info() draws the explanation for the highlighted option
 * below the list. Returns the chosen index or -1. */
static int option_pick(const char *title, const char *const *names, int n, int cur, void (*info)(int))
{
    int k = cur;
    bool redraw = true;
    while (1) {
        if (redraw) {
            redraw = false;
            vlcd_clear(0);
            vlcd_title(title);
            for (int i = 0; i < n; i++) {
                int y = LIST_Y + i * LIST_PITCH;
                if (i == k) vlcd_rect(0, y, VL, LIST_PITCH, 3);
                vlcd_text(4, y + 2, names[i], i == k ? 2 : 1);
                if (i == cur) vlcd_text(VL - 12, y + 2, "*", i == k ? 2 : 1);   /* the one in use */
            }
            vlcd_rect(6, 72, VL - 12, 58, 1);           /* info box: mid shade like the title band, dark text */
            info(k);
            vlcd_text_center(134, "A:NEXT B:OK C:BACK", 1);   /* menu font, kept off the rounded bottom corners */
            vlcd_clock();
            vlcd_battery();
            vlcd_present();
        }
        uint8_t down = keys_poll();
        if (down & 4) return -1;
        if (down & 2) return k;
        if (down & 1) { k = (k + 1) % n; redraw = true; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static void size_info(int k)                       /* a little picture of the screen and the game on it */
{
    /* Inside the info box (x 6..154, y 72..129); texts end before x ~150 (the panel edge hides the outer pixels). */
    static const int GW[3] = { 40, 44, 48 };      /* PERFECT / LARGE / FULL: borders 4 / 2 / 0 px, whole 4 px grid cells */
    static const char *const T[3][4] = { { "80% OF SCREEN", "SHARPEST", "EVEN PIXELS", "SHOWS EXTRAS" },
                                         { "93% OF SCREEN", "BIGGER", "UNEVEN PIXELS", "EXTRAS HIDDEN" },
                                         { "ALL OF SCREEN", "BIGGEST", "UNEVEN PIXELS", "EXTRAS HIDDEN" } };
    const int X = 13, Y = 77, S = 48;
    vlcd_rect(X, Y, S, S, k == 0 ? 3 : 1);         /* shell (PERFECT) or the black border (box shade, so the outline stays thin) */
    int g = GW[k], gx = X + (S - g) / 2, gy = Y + (S - g) / 2;
    vlcd_rect(gx, gy, g, g, 0);                     /* the game picture with a faint pixel grid */
    for (int i = 4; i < g; i += 4) { vlcd_rect(gx + i, gy, 1, g, 3); vlcd_rect(gx, gy + i, g, 1, 3); }
    /* outline drawn last (the game picture fills the whole box at FULL); rounded by leaving out the corner pixel */
    vlcd_rect(X, Y - 1, S, 1, 2); vlcd_rect(X, Y + S, S, 1, 2);
    vlcd_rect(X - 1, Y, 1, S, 2); vlcd_rect(X + S, Y, 1, S, 2);
    for (int i = 0; i < 4; i++) {                   /* one statement per line, each with a bullet */
        vlcd_rect(70, Y + 5 + i * 11, 2, 2, 2);
        vlcd_text_small(75, Y + 3 + i * 11, T[k][i], 2, false);
    }
}
static void display_info(int k)
{
    static const char *const T[3][4] = {
        { "DRAWS PIXELS ROW BY ROW,", "LIKE THE REAL TOY.", "NEW SCREENS APPEAR", "FROM TOP TO BOTTOM." },
        { "SHOWS EACH PART OF THE", "PICTURE WHEN IT IS READY.", "QUICK, BUT SCREENS CAN", "LOOK HALF-DRAWN." },
        { "WAITS FOR THE WHOLE", "PICTURE, THEN SHOWS IT.", "NO HALF-DRAWN SCREENS.", "(RECOMMENDED)" } };
    for (int i = 0; i < 4; i++) vlcd_text_small_center(79 + i * 11, T[k][i], 2);
}
/* LOADING (owner 2026-11-22): the two eggs, each shown rolling along a bar in the info box while highlighted. */
static void loader_pick(void)
{
    static const char *const N[2] = { "ROLL", "SPIN" };   /* (were TUMBLE / ROLL; owner 2026-11-22) */
    int k = s_loader, pct = 0;
    while (1) {
        vlcd_clear(0);
        vlcd_title(IC_LOADING " LOADING");
        for (int i = 0; i < 2; i++) {
            int y = LIST_Y + i * LIST_PITCH;
            if (i == k) vlcd_rect(0, y, VL, LIST_PITCH, 3);
            vlcd_text(4, y + 2, N[i], i == k ? 2 : 1);
            if (i == s_loader) vlcd_text(VL - 12, y + 2, "*", i == k ? 2 : 1);
        }
        vlcd_rect(6, 72, VL - 12, 58, 1);
        const int bx = 16, by = 114, bw = VL - 32, bh = 7;    /* the demo: bar + egg, no text */
        vlcd_rect(bx, by, bw, bh, 0);
        vlcd_frame(bx, by, bw, bh, 2);
        int fill = (bw - 4) * (pct > 100 ? 100 : pct) / 100;
        if (fill > 0) vlcd_rect(bx + 2, by + 2, fill, bh - 4, 2);
        egg_draw(k, fill, bx + 2 + fill, by);
        vlcd_text_center(134, "A:NEXT B:OK C:BACK", 1);
        vlcd_clock();
        vlcd_battery();
        vlcd_present();
        pct += 2;
        if (pct > 120) pct = 0;                        /* a short rest at 100%, then again */
        for (int t = 0; t < 6; t++) {                  /* ~60 ms per step, keys read every 10 ms */
            uint8_t down = keys_poll();
            if (down & 4) return;
            if (down & 2) { if (k != s_loader) { s_loader = k; save_flag("loader", s_loader); } return; }
            if (down & 1) { k = (k + 1) % 2; pct = 0; break; }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
static void size_pick(void)
{
    static const char *const N[3] = { "PERFECT", "LARGE", "FULL" };
    static const int Q[3] = { 12, 14, 15 };
    int cur = s_size_q == 15 ? 2 : s_size_q == 14 ? 1 : 0;
    int r = option_pick(IC_SIZE " SIZE", N, 3, cur, size_info);
    if (r >= 0 && r != cur) { s_size_q = Q[r]; size_tables(); colour_lut_build(); save_flag("size", s_size_q); }
}
static void display_pick(void)
{
    int r = option_pick(IC_ROWS " DISPLAY", DRAW_MODE_NAME, 3, s_draw_mode, display_info);
    if (r >= 0 && r != s_draw_mode) { s_draw_mode = r; save_flag("drawmode", s_draw_mode); }
}


static void flip_toggle(void)                     /* ORIENTATION: buttons on top / at the bottom */
{
    s_flip = !s_flip;
    bsp_display_rotation_set(s_flip ? BSP_DISPLAY_ROTATE_90 : BSP_DISPLAY_ROTATE_270);
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "flip", s_flip);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void colour_step(void)                     /* 100, 90, ... 50, then back to 100 */
{
    s_colour = s_colour <= 50 ? 100 : s_colour - 10;
    colour_lut_build();
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "colour", s_colour);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void lcd_look_toggle(void)
{
    s_lcd_look = !s_lcd_look;
    colour_lut_build();
    nvs_handle_t nvs;
    if (nvs_open("settings", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, "lcdlook", s_lcd_look);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* Level editor (2026-11-21, owner's choice for SOUND and BRIGHTNESS): A = lower, B = higher (hold to repeat),
 * C = back. No wrap-around, so turning the sound down at night never passes 100%. The new level applies at
 * once (sound: the select chime plays at it); saved to NVS once when leaving. */
static int level_edit(const char *title, const int *steps, int n, int cur, void (*apply)(int))
{
    int k = 0;
    while (k < n - 1 && steps[k] < cur) k++;
    int start = k;
    int64_t held_t = 0, last_rep = 0;
    uint8_t held = 0;
    bool redraw = true;
    while (1) {
        if (redraw) {
            redraw = false;
            vlcd_clear(0);
            vlcd_title(title);
            char v[12];
            if (steps[k] == 0) snprintf(v, sizeof v, "OFF"); else snprintf(v, sizeof v, "%d%%", steps[k]);
            vlcd_text_center(64, v, 2);
            const int gap = 2, bw = (VL - 16 - gap * (n - 1)) / n, x0 = (VL - (bw * n + gap * (n - 1))) / 2;
            for (int i = 0; i < n; i++) {                   /* level bar: filled up to the current step */
                int h = 6 + (i * 10) / (n - 1), x = x0 + i * (bw + gap), y = 98 - h;
                if (i <= k) vlcd_rect(x, y, bw, h, 2);
                else { vlcd_rect(x, y, bw, 1, 1); vlcd_rect(x, y + h - 1, bw, 1, 1); vlcd_rect(x, y, 1, h, 1); vlcd_rect(x + bw - 1, y, 1, h, 1); }
            }
            vlcd_text_center(112, "A:- B:+ C:BACK", 1);
            vlcd_clock();
            vlcd_battery();
            vlcd_present();
        }
        uint8_t down = keys_poll();
        int64_t now = esp_timer_get_time();
        if (down & 4) break;
        int step = 0;
        if (down & 1) { step = -1; held = 1; held_t = now; last_rep = now; }
        else if (down & 2) { step = 1; held = 2; held_t = now; last_rep = now; }
        else if (held && (s_keys & held) && now - held_t > 400000 && now - last_rep > 120000) { step = held == 1 ? -1 : 1; last_rep = now; }
        else if (held && !(s_keys & held)) held = 0;
        if (step && k + step >= 0 && k + step < n) { k += step; apply(steps[k]); redraw = true; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return k != start ? steps[k] : -1;               /* -1: unchanged */
}
static void save_flag(const char *key, int v);
static void bright_apply(int v) { s_brightness = v; bsp_display_brightness_set(v); }
static void sound_apply(int v) { s_sound = v; if (v) s_chime_req = true; }
static void brightness_edit(void)
{
    static const int ST[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
    if (level_edit(IC_SUN " BRIGHTNESS", ST, 10, s_brightness, bright_apply) >= 0) save_flag("bright", s_brightness);
}
/* SATURATION as a slider (owner 2026-11-22). In a game at PERFECT size the game stays visible and keeps running:
 * the slider sits in the shell under the game and every step recolours the game at once. Elsewhere (menus, other
 * sizes, or while the pet sleeps with its screen off) the normal level screen; the menu itself shows the colours. */
static void sat_apply(int v) { s_colour = v; colour_lut_build(); }
static void sat_slider_draw(int k, int n, int val)
{
    enum { SW = 120, SH = 14, PX = 60, PY = 436 };   /* 120x14 cells of 3x3 panel pixels at panel (60, 436): 360x42, under the game */
    vlcd_rect(0, 0, SW, SH, 0);                      /* drawn in the corner of the menu LCD buffer (redrawn afterwards) */
    vlcd_rect(1, 0, SW - 2, 1, 2); vlcd_rect(1, SH - 1, SW - 2, 1, 2);
    vlcd_rect(0, 1, 1, SH - 2, 2); vlcd_rect(SW - 1, 1, 1, SH - 2, 2);
    char t[24];
    snprintf(t, sizeof t, "SATURATION %d%%", val);
    vlcd_text_small(5, 4, t, 2, false);
    const int segw = 3, gap = 1, x0 = SW - 5 - (n * (segw + gap) - gap);
    for (int i = 0; i < n; i++) {                    /* rising level bars, like the level screens */
        int h = 3 + i * 6 / (n - 1), x = x0 + i * (segw + gap), y = SH - 3 - h;
        if (i <= k) vlcd_rect(x, y, segw, h, 2);
        else { vlcd_rect(x, y, segw, 1, 1); vlcd_rect(x, y + h - 1, segw, 1, 1); vlcd_rect(x, y, 1, h, 1); vlcd_rect(x + segw - 1, y, 1, h, 1); }
    }
    uint16_t pal[4], grid[4];
    const bool fx = colour_fx_on() && s_col_lut && s_grid_lut;
    for (int i = 0; i < 4; i++) {
        pal[i] = fx ? s_col_lut[PAL[i]] : PAL[i];
        grid[i] = fx && s_lcd_look ? s_grid_lut[PAL[i]] : fx ? s_col_lut[PAL_GRID[i]] : PAL_GRID[i];
        pal[i] = (uint16_t)((pal[i] >> 8) | (pal[i] << 8)); grid[i] = (uint16_t)((grid[i] >> 8) | (grid[i] << 8));
    }
    for (int Y = 0; Y < SH * 3; Y += 14) {           /* 288 x 14 panel rows per strip */
        uint16_t *o = s_strip;
        for (int yy = Y; yy < Y + 14; yy++) {
            const uint8_t *src = s_vlcd + (yy / 3) * VL;
            for (int X = 0; X < SW * 3; X++) {
                uint8_t c = src[X / 3] & 3;
                *o++ = (X % 3 == 2 || yy % 3 == 2) ? grid[c] : pal[c];
            }
        }
        esp_lv_adapter_dummy_draw_blit(s_disp, PX, PY + Y, PX + SW * 3, PY + Y + 14, s_strip, true);
    }
}
static void saturation_edit(bool in_game)
{
    static const int ST[] = { 50, 60, 70, 80, 90, 100 };
    const int n = 6;
    bool live = in_game && s_size_q == 12 && s_emu && s_emu->lcd.disp_on && s_emu->lcd.sleep_out;
    if (!live) {
        if (level_edit(IC_SATUR " SATURATION", ST, n, s_colour, sat_apply) >= 0) save_flag("colour", s_colour);
        return;
    }
    int k = 0;
    while (k < n - 1 && ST[k] < s_colour) k++;
    const int start = k;
    bool redraw = true, first = true;
    int64_t held_t = 0, last_rep = 0;
    uint8_t held = 0;
    while (1) {
        if (redraw) {                                /* stop the game's picture, recolour the frame, show it all again */
            redraw = false;
            s_present_on = false;
            while (s_presenting) vTaskDelay(pdMS_TO_TICKS(2));
            if (first) { panel_fill_black(0, 0, SCR_W, SCR_H); first = false; }   /* the menu covered everything */
            bezel_draw();
            sat_slider_draw(k, n, ST[k]);
            s_clock_redraw = true; s_bars_redraw = true; s_bat_redraw = true; s_show_all = true;
            s_present_on = true;
            xSemaphoreGive(s_present_req);
        }
        uint8_t down = keys_poll();
        int64_t now = esp_timer_get_time();
        if (down & 4) break;
        int step = 0;
        if (down & 1) { step = -1; held = 1; held_t = now; last_rep = now; }
        else if (down & 2) { step = 1; held = 2; held_t = now; last_rep = now; }
        else if (held && (s_keys & held) && now - held_t > 400000 && now - last_rep > 150000) { step = held == 1 ? -1 : 1; last_rep = now; }
        else if (held && !(s_keys & held)) held = 0;
        if (step && k + step >= 0 && k + step < n) { k += step; sat_apply(ST[k]); redraw = true; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_present_on = false;                            /* back to the menu (it covers the whole panel again) */
    while (s_presenting) vTaskDelay(pdMS_TO_TICKS(2));
    if (k != start) save_flag("colour", s_colour);
}
static void sound_edit(void)
{
    static const int ST[] = { 0, 10, 25, 50, 75, 100 };
    if (level_edit(IC_SOUND " SOUND", ST, 6, s_sound, sound_apply) >= 0) save_flag("sound", s_sound);
}


/* ---- Shared date and time (board clock chip). Every game gets it when it starts (fresh or from a save). ---- */
static bool shared_time(uint32_t *secs);
static bool board_local_secs(uint32_t *secs) { return shared_time(secs); }
static bool shared_time(uint32_t *secs)
{
    rtc_time_t t; bool lost = false;
    if (!rtc_read(&t, &lost) || lost || t.year < 2024) return false;
    *secs = rtc_to_secs(&t);
    return true;
}


/* ---- Time zone (DATE/TIME > TIME ZONE, 2026-11-23). The board clock keeps LOCAL time. The zone is only used by
 * SET FROM INTERNET to turn UTC into local time (with summer time as it is at that moment). The clock is NEVER
 * changed automatically - not from the internet and not for daylight saving (owner: game clocks must not jump).
 * Rules checked on the Mac against November and July offsets (scratchpad tz/t.c). */
/* Time zones (name <= 12 characters, POSIX TZ rule with daylight saving). West to east. */
static const struct { const char *name, *tz; } TZONES[] = {
    { "HONOLULU",     "HST10" },
    { "ALASKA",       "AKST9AKDT,M3.2.0,M11.1.0" },
    { "US PACIFIC",   "PST8PDT,M3.2.0,M11.1.0" },
    { "ARIZONA",      "MST7" },
    { "US MOUNTAIN",  "MST7MDT,M3.2.0,M11.1.0" },
    { "MEXICO CITY",  "CST6" },
    { "US CENTRAL",   "CST6CDT,M3.2.0,M11.1.0" },
    { "US EASTERN",   "EST5EDT,M3.2.0,M11.1.0" },
    { "ATLANTIC",     "AST4ADT,M3.2.0,M11.1.0" },
    { "CHILE",        "CLT4CLST,M9.1.0,M4.1.0" },
    { "NEWFOUNDLAND", "NST3:30NDT,M3.2.0,M11.1.0" },
    { "BRAZIL/ARG.",  "BRT3" },
    { "UTC",          "UTC0" },
    { "UK/IRELAND",   "GMT0BST,M3.5.0/1,M10.5.0" },
    { "PORTUGAL",     "WET0WEST,M3.5.0/1,M10.5.0" },
    { "W. AFRICA",    "WAT-1" },
    { "C. EUROPE",    "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "S. AFRICA",    "SAST-2" },
    { "E. EUROPE",    "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "EGYPT",        "EET-2EEST,M4.5.5/0,M10.5.4/23" },
    { "PALESTINE",    "EET-2EEST,M3.4.4/50,M10.4.4/50" },   /* tz database Asia/Gaza, Asia/Hebron (device-checked 2026-11-23) */
    { "TURKEY",       "TRT-3" },
    { "MOSCOW",       "MSK-3" },
    { "E. AFRICA",    "EAT-3" },
    { "IRAN",         "IRST-3:30" },
    { "DUBAI/GULF",   "GST-4" },
    { "PAKISTAN",     "PKT-5" },
    { "INDIA",        "IST-5:30" },
    { "NEPAL",        "NPT-5:45" },
    { "BANGLADESH",   "BDT-6" },
    { "THAI/VIETNAM", "ICT-7" },
    { "JAKARTA",      "WIB-7" },
    { "CHINA/HK",     "CST-8" },
    { "SINGAPORE",    "SGT-8" },
    { "PHILIPPINES",  "PHT-8" },
    { "PERTH",        "AWST-8" },
    { "JAPAN",        "JST-9" },
    { "KOREA",        "KST-9" },
    { "DARWIN",       "ACST-9:30" },
    { "ADELAIDE",     "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "BRISBANE",     "AEST-10" },
    { "SYDNEY/MELB.", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "HOBART",       "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "NEW ZEALAND",  "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define TZ_COUNT ((int)(sizeof TZONES / sizeof TZONES[0]))
static int s_tz = -1;                              /* index in TZONES; -1 = not set */
static int tz_offset_at(int z, time_t utc)         /* the zone's offset from UTC in minutes at that moment */
{
    setenv("TZ", TZONES[z].tz, 1); tzset();
    struct tm l; localtime_r(&utc, &l);
    setenv("TZ", "UTC0", 1); tzset();              /* back: the ESP32 clock holds local time "as UTC" (file dates) */
    rtc_time_t r = { l.tm_year + 1900, l.tm_mon + 1, l.tm_mday, l.tm_hour, l.tm_min, l.tm_sec };
    return (int)(((int64_t)rtc_to_secs(&r) - (int64_t)utc) / 60);
}
static void tz_offset_text(int off, char *out, size_t n)   /* "+11", "+5:30", "-3:30", "+0" */
{
    int a = off < 0 ? -off : off;
    if (a % 60) snprintf(out, n, "%c%d:%02d", off < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(out, n, "%c%d", off < 0 ? '-' : '+', a / 60);
}
static void tz_load(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READONLY, &h) != ESP_OK) return;
    char name[16]; size_t n = sizeof name;
    if (nvs_get_str(h, "tzname", name, &n) == ESP_OK)
        for (int i = 0; i < TZ_COUNT; i++) if (!strcmp(name, TZONES[i].name)) s_tz = i;
    nvs_close(h);
}
static void tz_save(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) != ESP_OK) return;
    if (s_tz >= 0) nvs_set_str(h, "tzname", TZONES[s_tz].name); else nvs_erase_key(h, "tzname");
    nvs_commit(h); nvs_close(h);
}
static bool board_local_secs(uint32_t *secs);
static void sys_time_from_rtc(void);
static int tz_offset_now(int z)                    /* offset now, judged from the board clock (local time) */
{
    uint32_t local;
    if (!board_local_secs(&local)) return tz_offset_at(z, time(NULL));
    int off = tz_offset_at(z, (time_t)local);      /* first guess, then once more from the UTC it implies */
    return tz_offset_at(z, (time_t)local - (time_t)off * 60);
}

/* The SD card's file dates come from the ESP32's own clock (FatFs get_fattime -> time()), which nobody set,
 * so every file was dated 1980 (owner noticed in USB DRIVE mode, 2026-11-22). Set it from the board clock at
 * start-up and whenever the date/time is changed. Local time is kept as if it were UTC (no time zone set),
 * so the files show the board's local time. */
static void sys_time_from_rtc(void)
{
    uint32_t secs;
    if (!shared_time(&secs)) return;
    struct timeval tv = { .tv_sec = (time_t)secs, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

/* Date/time editor. A: change the value (hold to repeat), B: next field (confirm on the last), C: back/cancel.
 * Returns true if the time was set. */
static bool edit_datetime(void)
{
    rtc_time_t t; bool lost = false;
    /* Years offered: from last year to 24 years ahead, counted from the clock's year (or, if the clock is
     * not set, the year this firmware was built), so the list moves on with time and never gets long. */
    const int build_year = atoi(&__DATE__[7]);
    bool clock_ok = rtc_read(&t, &lost) && !lost && t.year >= build_year - 1 && t.year <= 2099;
    const int year_lo = (clock_ok ? t.year : build_year) - 1;
    if (!clock_ok) t = (rtc_time_t){ build_year, 1, 1, 12, 0, 0 };
    int field = 0;                                     /* 0 year, 1 month, 2 day, 3 hour, 4 minute */
    int64_t a_held = 0, last_rep = 0;
    bool redraw = true;
    while (1) {
        if (redraw) {
            redraw = false;
            char d[16], h[12];
            snprintf(d, sizeof d, "%04d-%02d-%02d", t.year, t.month, t.day);
            snprintf(h, sizeof h, "%2d:%02d", (t.hour + 11) % 12 + 1, t.minute);   /* 12 h; AM/PM drawn small after it */
            vlcd_clear(0);
            vlcd_title(IC_CLOCK " SET DATE/TIME");
            int dx = (VL - 10 * 8) / 2, hx = (VL - (5 * 8 + 11)) / 2;
            static const int fx[5] = { 0, 5, 8, 0, 3 }, fw[5] = { 4, 2, 2, 2, 2 };
            if (field < 3) vlcd_rect(dx + fx[field] * 8 - 1, 64, fw[field] * 8 + 2, 11, 3);
            else vlcd_rect(hx + fx[field] * 8 - 1, 84, fw[field] * 8 + 2, 11, 3);
            vlcd_text(dx, 66, d, 2);
            vlcd_text(hx, 86, h, 2);
            vlcd_ampm(hx + 5 * 8, 86, t.hour >= 12, 2);
            vlcd_text_center(116, "A:CHANGE B:NEXT", 1);
            vlcd_text_center(128, "C:BACK", 1);
            vlcd_present();
        }
        uint8_t down = keys_poll();
        int64_t now = esp_timer_get_time();
        bool step = false;
        if (down & 1) { step = true; a_held = now; last_rep = now; }
        else if ((s_keys & 1) && now - a_held > 400000 && now - last_rep > 80000) { step = true; last_rep = now; }
        if (step) {
            switch (field) {
            case 0: t.year = t.year >= year_lo + 25 ? year_lo : t.year + 1; break;   /* 26 years from last year */
            case 1: t.month = t.month % 12 + 1; break;
            case 2: t.day = t.day % rtc_days_in_month(t.year, t.month) + 1; break;
            case 3: t.hour = (t.hour + 1) % 24; break;
            case 4: t.minute = (t.minute + 1) % 60; break;
            }
            redraw = true;
        }
        if (t.day > rtc_days_in_month(t.year, t.month)) { t.day = rtc_days_in_month(t.year, t.month); redraw = true; }
        if (down & 2) {
            if (field == 4) {
                t.second = 0;
                bool ok = rtc_write(&t);
                if (ok) sys_time_from_rtc();

                ESP_LOGI(TAG, "Date/time set to %04d-%02d-%02d %02d:%02d (%s)", t.year, t.month, t.day, t.hour, t.minute, ok ? "ok" : "FAILED");
                return ok;
            }
            field++; redraw = true;
        }
        if (down & 4) {
            if (field == 0) return false;
            field--; redraw = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* At start-up: if the board clock lost its time (battery ran out), offer to set it. */
/* At start-up: offer to set the clock if it lost its time (battery ran out), or after a FRESH install (owner
 * 2026-11-23: not after updates). A fresh install with erase wipes the saved settings; an update keeps them, so
 * "no settings saved yet" means a new install. Asked once: the answer is remembered (settings/welcomed). */
static void time_from_internet(void);
static void startup_time_check(void)
{
    rtc_time_t t; bool lost = false;
    if (!rtc_read(&t, &lost)) return;                  /* no clock chip answering: nothing to do */
    const bool ok = !lost && t.year >= 2024;
    bool fresh = false;
    {
        nvs_handle_t h;
        if (nvs_open("settings", NVS_READONLY, &h) != ESP_OK) fresh = true;   /* nothing ever saved */
        else nvs_close(h);
    }
    if (ok) {
        ESP_LOGI(TAG, "Board clock: %04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
        sys_time_from_rtc();                           /* file dates on the SD card */
        if (!fresh) return;
    }
    ESP_LOGI(TAG, "Asking to set the clock (%s)", ok ? "new install" : "clock lost its time");
    char keep[24];
    if (ok) snprintf(keep, sizeof keep, "  KEEP %d:%02d %s", (t.hour + 11) % 12 + 1, t.minute, t.hour >= 12 ? "PM" : "AM");
    else snprintf(keep, sizeof keep, "  LATER");
    const char *const items[3] = { IC_WIFI " FROM INTERNET", IC_CLOCK " BY HAND", keep };
    int r = run_list(IC_CLOCK " SET THE TIME?", items, 3, 0);
    save_flag("welcomed", 1);                          /* also creates the settings: not asked again */
    if (r == 0) time_from_internet();
    else if (r == 1) edit_datetime();
}


/* RESTART ROM: like the reset button on the back of the toy. The CPU, RAM and I/O start fresh; the game's
 * flash (where it keeps the pet) stays as it is. The game then starts up as after a battery change. */
static void game_restart(void)
{
    s_emu_running = false;
    while (s_emu_task_alive) vTaskDelay(pdMS_TO_TICKS(5));
    Emu *e = s_emu;
    Overlay ovl = e->ovl;
    uint32_t ovl_blk[4], pg_blk[4];
    static uint8_t pg_map[1024];
    memcpy(ovl_blk, e->ovl_blk, sizeof ovl_blk); memcpy(pg_blk, e->pg_blk, sizeof pg_blk); memcpy(pg_map, e->pg_map, sizeof pg_map);
    tc_init(e, e->rom, s_gram, s_ovl_pool, OVL_SLOTS);
    rom_apply_map(e);
    e->ovl = ovl;
    memcpy(e->ovl_blk, ovl_blk, sizeof ovl_blk); memcpy(e->pg_blk, pg_blk, sizeof pg_blk); memcpy(e->pg_map, pg_map, sizeof pg_map);
    e->pg_base = s_pg_pool;
    for (uint32_t b = 0; b < 128; b++) tc_blk_update(e, b);
    uint32_t now_s;
    if (shared_time(&now_s)) periph_rtc_set(e, now_s);
    s_date = NULL;                                 /* found again once the game has set itself up */
    s_rtc_wr_seen = g_tc_rtc_wr;                   /* its own clock writes after the restart get put back */
    tone_reset();
    publish_cycles(0);
    memset(&tc_hle, 0, sizeof tc_hle);
    s_chunk_ns0 = 0; s_chunk_c0 = 0; s_chunk_hz = e->dev.osc3_hz;
    s_game_hz = e->dev.osc3_hz;
    xSemaphoreTake(s_ready_mtx, portMAX_DELAY);
    memcpy(s_ready, s_gram, GRAM_W * GRAM_H * sizeof(uint16_t));
    s_ready_new = false;
    xSemaphoreGive(s_ready_mtx);
    ESP_LOGW(TAG, "RESTART ROM (flash kept)");
    s_emu_running = true;
    s_emu_task_alive = true;
    xTaskCreatePinnedToCore(emu_task, "paramix_task", 16384, NULL, 7, &s_emu_task_h, 1);
}

/* Quick menu over a running game (C held 2 s): screen and sound settings. The game keeps running behind it
 * (its keys are released, its picture is not shown); C closes the menu and the game screen comes back. */
#if TL_RAMDUMP == 3
/* Investigation (hunger/happiness): opening the quick menu prints the game's RAM (A0RAM, IVRAM, DSTRAM). */
static void ram_snapshot(void)
{
    static int n;
    static uint8_t *ram; static uint16_t *scr;
    n++;
    if (!ram) ram = heap_caps_malloc(A0RAM_MAX + IVRAM_MAX + DSTRAM_MAX, MALLOC_CAP_SPIRAM);
    if (!scr) scr = heap_caps_malloc(GRAM_W * GRAM_H * 2, MALLOC_CAP_SPIRAM);
    if (!ram || !scr) return;
    memcpy(scr, s_ready, GRAM_W * GRAM_H * 2);    /* grab first (the game moves on while we print) */
    memcpy(ram, s_emu->a0ram, A0RAM_MAX); memcpy(ram + A0RAM_MAX, s_emu->ivram, IVRAM_MAX);
    memcpy(ram + A0RAM_MAX + IVRAM_MAX, s_emu->dstram, DSTRAM_MAX);
    {   /* also kept on the SD card (2026-11-22: a session's snapshots were lost when the log recorder was off):
         * /sdcard/snaps/<rom>_NN.bin = "SNAPBIN1" + A0RAM + IVRAM + DSTRAM + screen (RGB565 as drawn) */
        mkdir("/sdcard/snaps", 0777);
        char base[48]; snprintf(base, sizeof base, "%s", s_game_file);
        char *dot = strrchr(base, '.'); if (dot) *dot = 0;
        char path[96];
        for (int k = 1; k < 1000; k++) {
            snprintf(path, sizeof path, "/sdcard/snaps/%s_%02d.bin", base, k);
            FILE *t = fopen(path, "rb");
            if (!t) break;
            fclose(t);
        }
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite("SNAPBIN1", 1, 8, f);
            fwrite(ram, 1, A0RAM_MAX + IVRAM_MAX + DSTRAM_MAX, f);
            fwrite(scr, 2, GRAM_W * GRAM_H, f);
            fclose(f);
            ESP_LOGI(TAG, "Snapshot %d saved as %s", n, path);
        }
    }
    printf("SNAP %d BEGIN\n", n);
    const uint8_t *ptr[3] = { ram, ram + A0RAM_MAX, ram + A0RAM_MAX + IVRAM_MAX };
    const uint32_t base[3] = { 0x00000, 0x80000, 0x84000 }, len[3] = { A0RAM_MAX, IVRAM_MAX, DSTRAM_MAX };
    for (int k = 0; k < 3; k++)
        for (uint32_t o = 0; o < len[k]; o += 32) {
            printf("S%d %06lx:", n, (unsigned long)(base[k] + o));
            for (int j = 0; j < 32; j++) printf(" %02x", ptr[k][o + j]);
            printf("\n");
            if ((o & 0x3FF) == 0x3E0) vTaskDelay(pdMS_TO_TICKS(5));
        }
    for (int y = 0; y < GRAM_H; y++) {             /* and the screen as the game drew it (RGB565) */
        printf("G%d %03d:", n, y);
        for (int x = 0; x < GRAM_W; x++) printf(" %04x", scr[y * GRAM_W + x]);
        printf("\n");
        if ((y & 7) == 7) vTaskDelay(pdMS_TO_TICKS(5));
    }
    printf("SNAP %d END\n", n);
}
#endif

/* GOTCHI POINTS editor (2026-11-22, owner: an EZGOTCHI-like option): 5 digits, A = next digit, B = digit + 1 (hold to
 * repeat), C = done. Written into the game's RAM (CARE table) when leaving, if changed; the game keeps it in its
 * own save like any points it earns. */
static void points_edit(const CareInfo *c)
{
    uint8_t *p = care_p(s_emu, c->pts);
    uint32_t v0 = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    if (v0 > 99999) v0 = 99999;
    int d[5];
    for (int i = 4, t = (int)v0; i >= 0; i--, t /= 10) d[i] = t % 10;
    int k = 0;
    int64_t held_t = 0, last_rep = 0;
    bool held = false, redraw = true;
    while (1) {
        if (redraw) {
            redraw = false;
            vlcd_clear(0);
            vlcd_title(IC_COIN " GOTCHI POINTS");
            const int x0 = (VL - 5 * 12 - 12) / 2, y = 60;
            for (int i = 0; i < 5; i++) {
                char ch[2] = { (char)('0' + d[i]), 0 };
                int x = x0 + i * 12;
                if (i == k) vlcd_rect(x - 2, y - 2, 12, 12, 3);
                vlcd_text(x, y, ch, 2);
                if (i == k) vlcd_rect(x - 1, y + 11, 10, 1, 2);
            }
            vlcd_text(x0 + 5 * 12 + 2, y, IC_GMARK, 2);   /* the Gotchi Points mark after the number */
            vlcd_text_center(112, "A:NEXT B:+1 C:OK", 1);
            vlcd_clock();
            vlcd_battery();
            vlcd_present();
        }
        uint8_t down = keys_poll();
        int64_t now = esp_timer_get_time();
        if (down & 4) break;
        if (down & 1) { k = (k + 1) % 5; redraw = true; }
        bool inc = false;
        if (down & 2) { inc = true; held = true; held_t = now; last_rep = now; }
        else if (held && (s_keys & 2) && now - held_t > 400000 && now - last_rep > 150000) { inc = true; last_rep = now; }
        else if (held && !(s_keys & 2)) held = false;
        if (inc) { d[k] = (d[k] + 1) % 10; redraw = true; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) v = v * 10 + (uint32_t)d[i];
    if (v != v0) {
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = 0;
        ESP_LOGI(TAG, "Gotchi Points %lu -> %lu", (unsigned long)v0, (unsigned long)v);
    }
}

/* EZCARE (2026-11-22, as Paramix-Studio's TamaEmu does it): hunger up to full, happiness up to the game's current
 * maximum (medals / Happy Symbols / puzzle pieces), sickness cured where the flag is known (Plus Color, iD L);
 * never lowers anything. A pet that is not sick is left as it is (the flag bytes are already 0). */
static void full_care(const CareInfo *c)
{
    uint8_t *h = care_p(s_emu, c->hun), *p = care_p(s_emu, c->hap);
    int cap = care_happy_max(s_emu, c);
    ESP_LOGI(TAG, "Full care: hunger %d -> %d, happiness %d -> %d", *h, *h < c->hunmax ? c->hunmax : *h, *p, *p < cap ? cap : *p);
    if (*h < c->hunmax) *h = c->hunmax;
    if (*p < cap) *p = (uint8_t)cap;
    if (c->sk) {   /* sickness (Paramix: hun+0x0C = the sick flag (skull), +0x0D its counter; the game tidies the rest) */
        uint8_t *k = care_p(s_emu, (uint16_t)(c->hun + c->sk));
        if (k[0]) { ESP_LOGI(TAG, "Full care: pet was sick (%02x %02x) -> cured", k[0], k[1]); s_care_redraw = true; }
        k[0] = 0; k[1] = 0;
    }
}

/* EZTOOLS sub-menu of the quick menu (owner's names 2026-11-22): EZGOTCHI = Gotchi Points editor, EZCARE = full care. */
/* ---- DLC (2026-11-23, owner's design): quick menu > DLC. Files go in SD:/DLC/<GAME>/ (ID, IDL, PS, 4U, 4UPLUS;
 * folders inside are fine) - the same record can suit several games (4U+ collections carry P's and iD L items), so
 * the folder says which game a file is for. Raw TAMAGO records or the JPEG cards with a record inside. Installing
 * writes the record into the game's flash (main/dlc.c, tamaemu's tables) between emulator run chunks; a save
 * follows. Patches come as destinations, so they install the same way. */
static bool emu_call(void (*fn)(void *), void *arg)     /* run fn in the emulator task between run chunks */
{
    if (!s_emu_task_alive) { fn(arg); return true; }
    s_emu_job_done = false; s_emu_job_arg = arg; s_emu_job = fn;
    for (int i = 0; i < 400 && !s_emu_job_done; i++) { if (s_emu_task_h) xTaskNotifyGive(s_emu_task_h); vTaskDelay(pdMS_TO_TICKS(10)); }
    bool ok = s_emu_job_done;
    s_emu_job = NULL;
    if (!ok) ESP_LOGW(TAG, "DLC: the emulator did not answer");
    return ok;
}
static void game_save_now(void)                       /* a save between run chunks, like the periodic one */
{
    s_snap_done = false; s_snap_req = true;
    for (int i = 0; i < 200 && !s_snap_done; i++) { if (s_emu_task_h) xTaskNotifyGive(s_emu_task_h); vTaskDelay(pdMS_TO_TICKS(5)); }
    s_snap_req = false;
    if (s_snap_done) save_write_snap(s_game_file);
}
static const char *dlc_game_dir(void)                 /* SD:/DLC/<this> for the running game */
{
    RomModel b = rom_base(s_game_file);
    return b == M_ID ? "ID" : b == M_IDL ? "IDL" : b == M_PS ? "PS" : b == M_4U ? "4U" : b == M_4UP ? "4UPLUS" : NULL;
}
static uint32_t fnv32(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; while (n--) h = (h ^ *p++) * 16777619u; return h; }

/* what is installed: per store the used slots, and a hash of every installed record (to mark files ADDED) */
#define DLC_MAX_INST 160
typedef struct { const DlcDevice *d; int used[24]; uint32_t hash[DLC_MAX_INST]; int nh; } DlcState;
static DlcState *s_dst;
static void job_state(void *a)
{
    DlcState *st = a;
    st->nh = 0;
    uint8_t *buf = heap_caps_malloc(0x8000, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < dlc_kind_count(st->d) && i < 24; i++) {
        const DlcKind *k = dlc_kind_at(st->d, i);
        st->used[i] = 0;
        for (int s = 0; s < k->nslots; s++) {
            uint8_t h[0x4C];
            if (!dlc_slot(s_emu, k, s, h, sizeof h)) continue;
            st->used[i]++;
            size_t len = ((size_t)h[0x48] << 24) | ((size_t)h[0x49] << 16) | ((size_t)h[0x4A] << 8) | h[0x4B];
            if (buf && len >= 0x60 && len <= k->slotsz && len <= 0x8000 && st->nh < DLC_MAX_INST) {
                tc_rom_export(s_emu, k->base + (uint32_t)s * k->slotsz, buf, (uint32_t)len);
                st->hash[st->nh++] = fnv32(buf, len);
            }
        }
    }
    free(buf);
}
typedef struct { const DlcKind *k; const uint8_t *p; size_t len; int r, slot; } DlcJob;
static void job_install(void *a) { DlcJob *j = a; j->r = dlc_install(s_emu, j->k, j->p, j->len, &j->slot); }
static void job_remove(void *a) { DlcJob *j = a; j->r = dlc_remove(s_emu, j->k, j->slot); }

static bool name_ascii(const char *s) { for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) return false; return true; }
static void dlc_message(const char *l1, const char *l2)
{
    vlcd_clear(0);
    vlcd_title(IC_DLC " DLC");
    vlcd_rect(6, 50, VL - 12, 36, 1);
    vlcd_text_small_center(58, l1, 2);
    if (l2) vlcd_text_small_center(70, l2, 2);
    vlcd_text_center(134, "C:BACK", 1);
    vlcd_clock(); vlcd_battery(); vlcd_present();
    while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
}
static void dlc_wait_screen(const char *l1)
{
    vlcd_clear(0); vlcd_title(IC_DLC " DLC"); vlcd_rect(6, 50, VL - 12, 24, 1); vlcd_text_small_center(58, l1, 2);
    vlcd_clock(); vlcd_battery(); vlcd_present();
}

/* one folder: subfolders, then files (name, store, state) */
#define DLC_MAX_ENT 200
typedef struct { char fname[64]; char name[20]; bool dir; int kind; uint32_t hash; uint8_t state; } DlcEnt;
enum { DS_NEW, DS_ADDED, DS_FULL, DS_NOSTORE, DS_BAD };
static DlcEnt *s_dent;
static int s_dent_n;
static void dlc_read_dir(const DlcDevice *d, const char *dir)
{
    s_dent_n = 0;
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *ent;
    int nfile = 0;
    while ((ent = readdir(dp)) && s_dent_n < DLC_MAX_ENT) {
        if (ent->d_name[0] == '.' || ent->d_name[0] == '_') continue;   /* hidden, and _READ-ME files */
        if (strlen(ent->d_name) >= sizeof s_dent[0].fname) continue;
        DlcEnt *e = &s_dent[s_dent_n];
        memset(e, 0, sizeof *e);
        snprintf(e->fname, sizeof e->fname, "%s", ent->d_name);
        char nm[64]; snprintf(nm, sizeof nm, "%s", ent->d_name);
        if (ent->d_type == DT_DIR) {
            e->dir = true;
            snprintf(e->name, sizeof e->name, "%s", name_ascii(nm) ? nm : "FOLDER");
            s_dent_n++;
            continue;
        }
        char path[192]; snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        uint8_t *p; size_t len; const char *why;
        if (dlc_extract(path, &p, &len, &why)) continue;   /* not a DLC file */
        char *dot = strrchr(nm, '.'); if (dot) *dot = 0;
        if (!name_ascii(nm) || !nm[0]) { dlc_name(p, len, nm, sizeof nm); if (!nm[0]) snprintf(nm, sizeof nm, "DLC %d", nfile + 1); }
        snprintf(e->name, sizeof e->name, "%s", nm);
        const char *label;
        dlc_route(d, p, len, &e->kind, &label);
        e->hash = fnv32(p, len);
        const DlcKind *k = dlc_kind(d, e->kind);
        e->state = !k ? DS_NOSTORE : len > k->slotsz ? DS_BAD : DS_NEW;
        free(p);
        nfile++; s_dent_n++;
    }
    closedir(dp);
    for (int i = 1; i < s_dent_n; i++)                /* folders first, then by name */
        for (int j = i; j > 0; j--) {
            DlcEnt *a = &s_dent[j - 1], *b = &s_dent[j];
            if (a->dir > b->dir || (a->dir == b->dir && strcasecmp(a->name, b->name) <= 0)) break;
            DlcEnt t = *a; *a = *b; *b = t;
        }
}
static void dlc_states(const DlcDevice *d)            /* ADDED / FULL from what is installed now */
{
    for (int i = 0; i < s_dent_n; i++) {
        DlcEnt *e = &s_dent[i];
        if (e->dir || e->state == DS_NOSTORE || e->state == DS_BAD) continue;
        e->state = DS_NEW;
        for (int h = 0; h < s_dst->nh; h++) if (s_dst->hash[h] == e->hash) e->state = DS_ADDED;
        if (e->state == DS_NEW)
            for (int k = 0; k < dlc_kind_count(d); k++)
                if (dlc_kind_at(d, k)->kind == e->kind && s_dst->used[k] >= dlc_kind_at(d, k)->nslots) e->state = DS_FULL;
    }
}
static int dlc_install_ent(const DlcDevice *d, const char *dir, DlcEnt *e)
{
    char path[192]; snprintf(path, sizeof path, "%s/%s", dir, e->fname);
    uint8_t *p; size_t len; const char *why;
    if (dlc_extract(path, &p, &len, &why)) { e->state = DS_BAD; return -2; }
    DlcJob j = { .k = dlc_kind(d, e->kind), .p = p, .len = len, .r = -2 };
    if (j.k) emu_call(job_install, &j);
    free(p);
    ESP_LOGI(TAG, "DLC: %s -> %s slot %d: %d", path, j.k ? j.k->label : "?", j.slot, j.r);
    return j.r;
}
static bool dlc_browse(const DlcDevice *d, const char *dir, const char *title)   /* true if something was added */
{
    dlc_wait_screen("READING THE FOLDER...");
    dlc_read_dir(d, dir);
    s_dst->d = d; emu_call(job_state, s_dst); dlc_states(d);
    /* this folder's entries are copied, so going into a subfolder can reuse s_dent */
    DlcEnt *ents = heap_caps_malloc(sizeof(DlcEnt) * (s_dent_n ? s_dent_n : 1), MALLOC_CAP_SPIRAM);
    if (!ents) return false;
    int n = s_dent_n; memcpy(ents, s_dent, sizeof(DlcEnt) * n);
    bool changed = false;
    int sel = 0;
    char (*rows)[24] = heap_caps_malloc(24 * (n + 1), MALLOC_CAP_SPIRAM);
    const char **items = heap_caps_malloc(sizeof(char *) * (n + 1), MALLOC_CAP_SPIRAM);
    if (!rows || !items) { free(ents); free(rows); free(items); return false; }
    if (n == 0) { char l2[48]; snprintf(l2, sizeof l2, "IN SD:%s", dir + 7); dlc_message("NO DLC FILES", l2); free(ents); free(rows); free(items); return false; }
    while (1) {
        int nnew = 0, nfiles = 0;
        for (int i = 0; i < n; i++) if (!ents[i].dir) { nfiles++; if (ents[i].state == DS_NEW) nnew++; }
        int m = 0, first = 0;
        if (nfiles) { char v[8]; snprintf(v, sizeof v, "%d", nnew); list_item(rows[m], 24, "ADD ALL NEW", v); items[m] = rows[m]; m++; first = 1; }
        static const char *const ST[] = { "NEW", "ADDED", "FULL", "-", "BAD" };
        for (int i = 0; i < n; i++) {
            char nm[18]; snprintf(nm, sizeof nm, "%.*s", ents[i].dir ? 16 : 13, ents[i].name);
            list_item(rows[m], 24, nm, ents[i].dir ? ">" : ST[ents[i].state]);
            items[m] = rows[m]; m++;
        }
        int r = run_list_refresh(title, items, m, &sel, 0);
        if (r < 0) break;
        if (first && r == 0) {                        /* ADD ALL NEW in this folder */
            int ok = 0, full = 0;
            for (int i = 0; i < n; i++) if (!ents[i].dir && ents[i].state == DS_NEW) {
                int x = dlc_install_ent(d, dir, &ents[i]);
                if (x == 0) ok++; else if (x == -1) full++;
            }
            if (ok) changed = true;
            emu_call(job_state, s_dst);
            memcpy(s_dent, ents, sizeof(DlcEnt) * n); s_dent_n = n; dlc_states(d); memcpy(ents, s_dent, sizeof(DlcEnt) * n);
            char l1[32], l2[40]; snprintf(l1, sizeof l1, "%d ADDED.", ok); snprintf(l2, sizeof l2, "%d DID NOT FIT (CATEGORY FULL).", full);
            dlc_message(l1, full ? l2 : NULL);
            continue;
        }
        DlcEnt *e = &ents[r - first];
        if (e->dir) {
            char sub[192]; snprintf(sub, sizeof sub, "%s/%s", dir, e->fname);
            char t[24]; snprintf(t, sizeof t, IC_DLC " %.14s", e->name);
            if (dlc_browse(d, sub, t)) changed = true;
            emu_call(job_state, s_dst);                /* the subfolder may have added things */
            memcpy(s_dent, ents, sizeof(DlcEnt) * n); s_dent_n = n; dlc_states(d); memcpy(ents, s_dent, sizeof(DlcEnt) * n);
            continue;
        }
        const DlcKind *k = dlc_kind(d, e->kind);
        if (e->state == DS_NEW) {
            int x = dlc_install_ent(d, dir, e);
            if (x == 0) changed = true;
            emu_call(job_state, s_dst);
            memcpy(s_dent, ents, sizeof(DlcEnt) * n); s_dent_n = n; dlc_states(d); memcpy(ents, s_dent, sizeof(DlcEnt) * n);
            dlc_message(x == 0 ? "ADDED TO" : x == -1 ? "NO ROOM LEFT IN" : x == -3 ? "THIS GAME VERSION KEEPS" : "COULD NOT ADD TO",
                        x == -3 ? "ITS ITEMS ELSEWHERE (NOT ADDED)" : k ? k->label : "");
        } else if (e->state == DS_ADDED) dlc_message("ALREADY ADDED.", k ? k->label : NULL);
        else if (e->state == DS_FULL) dlc_message("NO ROOM LEFT: REMOVE AN ITEM", "FROM THAT CATEGORY FIRST.");
        else if (e->state == DS_NOSTORE) dlc_message("THIS GAME HAS NO PLACE", "FOR THIS KIND OF DLC (YET).");
        else dlc_message("THE FILE IS DAMAGED", "OR TOO BIG.");
    }
    free(ents); free(rows); free(items);
    return changed;
}

typedef struct { const DlcKind *k; int n; int slots[24]; char names[24][20]; } DlcCat;
static void job_list(void *x)                         /* the occupied slots of a store, with names */
{
    DlcCat *c = x;
    uint8_t h[0x60];
    c->n = 0;
    for (int s = 0; s < c->k->nslots && s < 24; s++) {
        if (!dlc_slot(s_emu, c->k, s, h, sizeof h)) continue;
        char nm[32]; dlc_name(h, sizeof h, nm, sizeof nm);
        if (!nm[0]) snprintf(nm, sizeof nm, "ITEM %d", s + 1);
        snprintf(c->names[c->n], sizeof c->names[0], "%s", nm);
        c->slots[c->n++] = s;
    }
}
static bool dlc_category(const DlcKind *k)            /* the items in one store; B twice removes one */
{
    int sel = 0, armed = -1;
    bool changed = false;
    static DlcCat c;
    static char rows[24][24];
    while (1) {
        c.k = k;
        emu_call(job_list, &c);
        if (c.n == 0) { dlc_message("NOTHING ADDED HERE YET.", NULL); break; }
        const char *items[24];
        for (int i = 0; i < c.n; i++) {
            char nm[16]; snprintf(nm, sizeof nm, "%.13s", c.names[i]);
            if (i == armed) list_item(rows[i], sizeof rows[i], "REMOVE?", "B AGAIN");
            else list_item(rows[i], sizeof rows[i], nm, "");
            items[i] = rows[i];
        }
        if (sel >= c.n) sel = c.n - 1;
        char title[24]; snprintf(title, sizeof title, IC_DLC " %s", k->label);
        int r = run_list_refresh(title, items, c.n, &sel, 0);
        if (r < 0) break;
        if (armed != r) { armed = r; continue; }
        armed = -1;
        DlcJob j = { .k = k, .slot = c.slots[r], .r = -1 };
        emu_call(job_remove, &j);
        if (j.r == 0) changed = true;
        ESP_LOGI(TAG, "DLC: removed %s slot %d: %d", k->label, c.slots[r], j.r);
    }
    return changed;
}
static void dlc_menu(void)
{
    const DlcDevice *d = dlc_device(s_emu);
    const char *gd = dlc_game_dir();
    if (!d || !gd) { dlc_message("THIS GAME HAS NO DLC", "STORES WE KNOW OF (YET)."); return; }
    if (!s_dent) s_dent = heap_caps_calloc(DLC_MAX_ENT, sizeof(DlcEnt), MALLOC_CAP_SPIRAM);
    if (!s_dst) s_dst = heap_caps_calloc(1, sizeof(DlcState), MALLOC_CAP_SPIRAM);
    if (!s_dent || !s_dst) return;
    char root[48]; snprintf(root, sizeof root, "/sdcard/dlc/%s", gd);
    mkdir("/sdcard/dlc", 0777); mkdir(root, 0777);
    int sel = 0;
    bool changed = false;
    while (1) {
        s_dst->d = d; emu_call(job_state, s_dst);
        int nk = dlc_kind_count(d);
        static char rows[25][24];
        const char *items[25];
        char v[24]; snprintf(v, sizeof v, "%s >", gd);
        list_item(rows[0], sizeof rows[0], "ADD FROM SD", v); items[0] = rows[0];
        for (int i = 0; i < nk && i < 24; i++) {
            const DlcKind *k = dlc_kind_at(d, i);
            snprintf(v, sizeof v, "%d/%d", s_dst->used[i], k->nslots);
            list_item(rows[i + 1], sizeof rows[i + 1], k->label, v);
            items[i + 1] = rows[i + 1];
        }
        s_list_sep = 1;
        int r = run_list(IC_DLC " DLC", items, nk + 1, sel);
        s_list_sep = -1;
        if (r < 0) break;
        sel = r;
        char t[24]; snprintf(t, sizeof t, IC_DLC " DLC/%s", gd);
        if (r == 0) { if (dlc_browse(d, root, t)) changed = true; }
        else if (dlc_category(dlc_kind_at(d, r - 1))) changed = true;
    }
    if (changed) { dlc_wait_screen("SAVING..."); game_save_now(); }
}

static void pet_tools_menu(const CareInfo *c)
{
    int sel = 0;
    bool done = false;
    while (1) {
        char gp[24], v[12];
        const uint8_t *pp = care_p(s_emu, c->pts);
        snprintf(v, sizeof v, "%lu", (unsigned long)(pp[0] | pp[1] << 8 | pp[2] << 16 | (uint32_t)pp[3] << 24));
        list_item(gp, sizeof gp, IC_COIN " EZGOTCHI", v);
        const char *fc = done ? IC_HEART " EZCARE: DONE" : IC_HEART " EZCARE";
        const char *const items[] = { gp, fc };
        int r = run_list(IC_TOOLS " EZTOOLS", items, 2, sel);
        if (r < 0) return;
        sel = r;
        if (r == 0) { points_edit(c); done = false; }
        else { full_care(c); done = true; }
    }
}

/* SCREENSHOT (quick menu, 2026-11-23): the game picture 3 s after the quick menu closes (countdown in the border
 * clock; the display task copies what is on screen), in the game's own
 * colours (no SATURATION / LCD SHADER), 4x bigger (512x512, every game pixel 4x4), as a 24-bit BMP in
 * SD:/screenshots/<rom>_YYYY-MM-DD_HHMMSS.bmp. */
static uint16_t (*s_shot)[GRAM_W];                 /* copy of s_shown (PSRAM) */
static bool s_shot_ok, s_shot_bgr, s_shot_inv;
static void shot_grab(void)                        /* call while nothing is presenting */
{
    if (!s_shot) s_shot = heap_caps_malloc(GRAM_W * GRAM_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_shot_ok = s_shot && s_shown && !s_lcd_off_shown;
    if (!s_shot_ok) return;
    memcpy(s_shot, s_shown, GRAM_W * GRAM_H * sizeof(uint16_t));
    s_shot_bgr = (s_emu->lcd.madctl & 0x08) != 0;
    s_shot_inv = s_emu->lcd.inverted;
}
static bool shot_save(char *name, size_t n)
{
    if (!s_shot_ok) return false;
    enum { SC = 4, W = GRAM_W * SC, H = GRAM_H * SC, ROW = W * 3 };
    mkdir("/sdcard/screenshots", 0777);
    char base[64];
    snprintf(base, sizeof base, "%s", s_game_file);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    rtc_time_t t; bool lost = false;
    if (rtc_read(&t, &lost) && !lost)
        snprintf(name, n, "%.40s_%04d-%02d-%02d_%02d%02d%02d.bmp", base, t.year, t.month, t.day, t.hour, t.minute, t.second);
    else snprintf(name, n, "%.40s_%lld.bmp", base, (long long)(esp_timer_get_time() / 1000000));
    char path[128];
    snprintf(path, sizeof path, "/sdcard/screenshots/%s", name);
    FILE *f = fopen(path, "wb");
    uint8_t *row = malloc(ROW);
    bool ok = f && row;
    if (ok) {
        uint8_t h[54] = { 'B', 'M' };
        uint32_t size = 54 + (uint32_t)ROW * H;
        #define PUT32(o, v) do { h[o] = (uint8_t)(v); h[o + 1] = (uint8_t)((v) >> 8); h[o + 2] = (uint8_t)((v) >> 16); h[o + 3] = (uint8_t)((v) >> 24); } while (0)
        PUT32(2, size); PUT32(10, 54); PUT32(14, 40); PUT32(18, W); PUT32(22, H);
        h[26] = 1; h[28] = 24; PUT32(34, (uint32_t)ROW * H); PUT32(38, 2835); PUT32(42, 2835);   /* 72 dpi */
        #undef PUT32
        ok = fwrite(h, 1, 54, f) == 54;
        for (int y = GRAM_H - 1; ok && y >= 0; y--) {   /* BMP rows go bottom-up */
            for (int x = 0; x < GRAM_W; x++) {
                uint16_t c = s_shot[y][x];
                if (s_shot_bgr) c = (uint16_t)((c >> 11) | (c & 0x07E0) | ((c & 0x1F) << 11));
                if (s_shot_inv) c = (uint16_t)~c;
                uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31), g = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
                        b = (uint8_t)((c & 0x1F) * 255 / 31);
                for (int k = 0; k < SC; k++) { uint8_t *o = row + (x * SC + k) * 3; o[0] = b; o[1] = g; o[2] = r; }
            }
            for (int k = 0; k < SC && ok; k++) ok = fwrite(row, 1, ROW, f) == ROW;
        }
    }
    free(row);
    if (f && fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    ESP_LOGI(TAG, "Screenshot %s: %s", path, ok ? "saved" : "FAILED");
    return ok;
}

static void quick_menu(void)
{
    pm_hold(s_pm_ui, true);
    s_btn_mask = 0;
    s_present_on = false;
    while (s_presenting) vTaskDelay(pdMS_TO_TICKS(2));
    if (s_lcd_off_shown) { low_power(false); s_lcd_off_shown = false; panel_wake_now(); bsp_display_brightness_set(s_brightness); }
    s_vlcd_direct = true;
    int sel = 0;
    bool restart_armed = false, export_armed = false;
    while (1) {
        char b[24], so[24], sc[24], ex[24], gp[24], v[12];
        (void)v;
        snprintf(v, sizeof v, "%d%%", s_brightness); list_item(b, sizeof b, IC_SUN " BRIGHTNESS", v);
        if (s_sound) snprintf(v, sizeof v, "%d%%", s_sound); else snprintf(v, sizeof v, "OFF");
        list_item(so, sizeof so, IC_SOUND " SOUND", v);
        list_item(sc, sizeof sc, IC_SCREEN " SCREEN", ">");
        list_item(ex, sizeof ex, IC_EXTRAS " EXTRAS", ">");
        const CareInfo *care = care_find(s_emu);
        if (care) list_item(gp, sizeof gp, IC_TOOLS " EZTOOLS", ">");
        char dl[24]; list_item(dl, sizeof dl, IC_DLC " DLC", ">");
        const char *rs = restart_armed ? IC_RESTART " RESTART: B AGAIN" : IC_RESTART " RESTART ROM";
        const char *xp = export_armed ? IC_EXPORT " EXPORT: B AGAIN" : IC_EXPORT " EXPORT SAVE";
        /* item list built per pass: ids, so optional items (points, test items) can come and go */
        enum { Q_BRIGHT, Q_SOUND, Q_SCREEN, Q_EXTRAS, Q_POINTS, Q_DLC, Q_SHOT, Q_HEART, Q_SNAP5, Q_SNAP10, Q_QUIT, Q_RESTART, Q_EXPORT };
        const char *items[13]; int ids[13], n = 0;
        items[n] = b;  ids[n++] = Q_BRIGHT;
        items[n] = so; ids[n++] = Q_SOUND;
        items[n] = sc; ids[n++] = Q_SCREEN;
        items[n] = ex; ids[n++] = Q_EXTRAS;
        if (care) { items[n] = gp; ids[n++] = Q_POINTS; }
        if (dlc_device(s_emu)) { items[n] = dl; ids[n++] = Q_DLC; }
        items[n] = IC_CAMERA " SCREENSHOT IN 3S"; ids[n++] = Q_SHOT;
#if TL_RAMDUMP == 3
        items[n] = "SNAPSHOT IN 5S";  ids[n++] = Q_SNAP5;
        items[n] = "SNAPSHOT IN 10S"; ids[n++] = Q_SNAP10;
#endif
        s_list_sep = n;                               /* line above SAVE & QUIT / RESTART ROM / EXPORT SAVE, which stay last */
        items[n] = IC_QUIT " SAVE & QUIT"; ids[n++] = Q_QUIT;
        items[n] = rs; ids[n++] = Q_RESTART;
        items[n] = xp; ids[n++] = Q_EXPORT;
        if (sel >= n) sel = 0;
        int r = run_list(IC_QUICK " QUICK MENU", items, n, sel);
        s_list_sep = -1;
        if (r < 0) break;                             /* C: back to the game */
        sel = r;
        int id = ids[r];
#if TL_RAMDUMP == 3
        if (id == Q_SNAP5 || id == Q_SNAP10) { s_snap_at = esp_timer_get_time() + (id == Q_SNAP5 ? 5000000 : 10000000); break; }   /* close; snapshot later */
#endif
        if (id == Q_BRIGHT) brightness_edit();
        else if (id == Q_SOUND) sound_edit();
        else if (id == Q_SCREEN) screen_menu(true);
        else if (id == Q_EXTRAS) extras_menu(true);
        else if (id == Q_POINTS) pet_tools_menu(care);
        else if (id == Q_DLC) dlc_menu();
        else if (id == Q_SHOT) { s_shot_show = 0; s_shot_at = esp_timer_get_time() + 3000000; break; }   /* back to the game, count down */
        else if (id == Q_QUIT) { s_quit_req = true; break; }   /* game_loop leaves the game: saves, back to the ROM list */
        else if (id == Q_RESTART) {
            export_armed = false;
            if (!restart_armed) { restart_armed = true; continue; }   /* ask once more */
            game_restart(); break;
        } else if (id == Q_EXPORT) {
            restart_armed = false;
            if (!export_armed) { export_armed = true; continue; }
            s_export_req = true; break;                  /* game_loop leaves the game: save, then export */
        }
        restart_armed = false; export_armed = false;
    }
    s_vlcd_direct = false;
    panel_fill_black(0, 0, SCR_W, SCR_H);             /* the game screen again, all of it */
    bezel_draw();
    s_clock_redraw = true;
    s_bars_redraw = true;
    s_show_all = true;
    s_bat_redraw = true;
    s_present_on = true;
    xSemaphoreGive(s_present_req);
    pm_hold(s_pm_ui, false);
}

/* SCREEN sub-menu: saturation, LCD shader, display mode, orientation. */
static void screen_menu(bool in_game)
{
    /* SIZE and DISPLAY only matter in a game: quick menu only (owner 2026-11-22) */
    int sel = 0;
    while (1) {
        char sa[24], lk[24], ld[24], fl[24], sz[24], mb[24], lo[24], v[12];
        static const char *const SZ[4] = { "PERFECT", "", "LARGE", "FULL" };   /* 3x, -, 3.5x, 3.75x */
        list_item(sz, sizeof sz, IC_SIZE " SIZE", SZ[s_size_q - 12]);
        snprintf(v, sizeof v, "%d%%", s_colour); list_item(sa, sizeof sa, IC_SATUR " SATURATION", v);
        list_item(lk, sizeof lk, IC_GRID " LCD SHADER", s_lcd_look ? "ON" : "OFF");
        list_item(ld, sizeof ld, IC_ROWS " DISPLAY", DRAW_MODE_NAME[s_draw_mode]);
        list_item(fl, sizeof fl, IC_ORIENT " BUTTONS", s_flip ? "BOTTOM" : "TOP");
        static const char *const MB[6] = { "PLAIN", "LIGHT", "DARK", "LINED", "MID", "LINE" };
        list_item(mb, sizeof mb, IC_MENUBAR " MENU BAR", MB[s_menu_bar]);
        static const char *const LO[2] = { "ROLL", "SPIN" };
        list_item(lo, sizeof lo, IC_LOADING " LOADING", LO[s_loader]);
        enum { S_SIZE, S_SAT, S_LCD, S_DISP, S_BTN, S_BAR, S_LOAD };
        const char *items[7]; int ids[7], n = 0;
        if (in_game) { items[n] = sz; ids[n++] = S_SIZE; }
        items[n] = sa; ids[n++] = S_SAT;
        items[n] = lk; ids[n++] = S_LCD;
        if (in_game) { items[n] = ld; ids[n++] = S_DISP; }
        items[n] = fl; ids[n++] = S_BTN;
        items[n] = mb; ids[n++] = S_BAR;
        items[n] = lo; ids[n++] = S_LOAD;
        int r = run_list(IC_SCREEN " SCREEN", items, n, sel);
        if (r < 0) return;
        sel = r;
        switch (ids[r]) {
        case S_SIZE: size_pick(); break;
        case S_SAT:  saturation_edit(in_game); break;
        case S_LCD:  lcd_look_toggle(); break;
        case S_DISP: display_pick(); break;
        case S_BTN:  flip_toggle(); break;
        case S_LOAD: loader_pick(); break;
        default: {   /* DARK -> LIGHT -> LINED -> MID -> LINE -> PLAIN -> DARK */
            static const int NEXT[6] = { 2, 3, 1, 4, 5, 0 };
            s_menu_bar = NEXT[s_menu_bar]; save_flag("menubar", s_menu_bar);
        } }
    }
}

/* EXTRAS sub-menu: game clock, shell, battery; in a game also GAME SPEED (the pet ages faster). */
static void extras_menu(bool in_game)
{
    int sel = 0;
    while (1) {
        char gc[24], sh[24], bt[24], gs[24], sb[24], v[12];
        list_item(sb, sizeof sb, IC_HEART " STATUS BARS", s_bars_on ? "ON" : "OFF");
        list_item(gc, sizeof gc, IC_CLOCK " BORDER CLOCK", s_game_clock ? "ON" : "OFF");
        list_item(sh, sizeof sh, IC_SHELL " SHELL", s_shell_on ? "ON" : "OFF");
        list_item(bt, sizeof bt, IC_BATT " BATTERY", s_battery_on ? "ON" : "OFF");
        if (s_time_warp >= GAME_SPEED_MAX) snprintf(v, sizeof v, "MAX"); else snprintf(v, sizeof v, "X%d", s_time_warp);
        list_item(gs, sizeof gs, IC_SPEED " GAME SPEED", v);
        /* BORDER CLOCK and GAME SPEED only in the quick menu (they only matter in a game; owner 2026-11-22) */
        const char *const items_game[] = { sb, gc, sh, bt, gs };
        const char *const items_menu[] = { sb, sh, bt };
        int r = run_list(IC_EXTRAS " EXTRAS", in_game ? items_game : items_menu, in_game ? 5 : 3, sel);
        if (r < 0) return;
        sel = r;
        if (!in_game && r >= 1) r++;                  /* same numbering as the game list */
        if (r == 0) bars_toggle();
        else if (r == 1) game_clock_toggle();
        else if (r == 2) shell_toggle();
        else if (r == 3) battery_toggle();
        else s_time_warp = s_time_warp >= GAME_SPEED_MAX ? 1 : s_time_warp >= 8 ? GAME_SPEED_MAX : s_time_warp * 2;   /* x1 x2 x4 x8 MAX: also ages the pet */
    }
}

/* USB DRIVE (2026-11-22, owner): the SD card appears on the computer as a USB drive (TinyUSB mass storage on the
 * ESP32-S3's USB port), so ROMs and saves can be copied without a card reader. The USB port leaves its normal
 * serial/JTAG role for this, so leaving restarts the board. While the computer has the card, TamaNest does not
 * touch it (s_usb_drive: no battery diary, no menu sleep). */
static void usb_drive_screen(const char *const *lines, int n, const char *hint)
{
    vlcd_clear(0);
    vlcd_title(IC_USB " USB DRIVE");
    vlcd_rect(6, 44, VL - 12, 14 + n * 11, 1);
    for (int i = 0; i < n; i++) vlcd_text_small_center(51 + i * 11, lines[i], 2);
    vlcd_text_center(134, hint, 1);
    vlcd_clock();
    vlcd_present();
}
static void usb_drive_mode(void)
{
    static const char *const ASK[5] = { "SHOWS THE SD CARD AS A", "DRIVE ON YOUR COMPUTER,", "TO COPY ROMS AND SAVES.", "TAMANEST RESTARTS", "WHEN YOU LEAVE." };
    static const char *const ASKFMT[5] = { "THIS CARD IS NOT FAT32.", "B SHOWS IT ON YOUR", "COMPUTER: FORMAT IT THERE", "AS FAT32 (MS-DOS FAT).", "IT ERASES THE CARD." };
    static const char *const NOCARD[3] = { "NO SD CARD FOUND.", "PUT A CARD IN THE SLOT", "AND TRY AGAIN." };
    /* a card TamaNest cannot read (e.g. exFAT): start it without its file system, so the computer can format it */
    if (!bsp_sdcard) { esp_err_t r = bsp_sdcard_init_raw(); ESP_LOGI(TAG, "SD card started for USB DRIVE only: %s", esp_err_to_name(r)); }
    if (!bsp_sdcard) {
        usb_drive_screen(NOCARD, 3, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    usb_drive_screen(s_sd_ok ? ASK : ASKFMT, 5, "B:START C:BACK");
    while (1) {
        uint8_t down = keys_poll();
        if (down & 4) return;
        if (down & 2) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_usb_drive = true;
    esp_err_t err = ESP_FAIL;
    if (bsp_sdcard) {
        const tinyusb_msc_driver_config_t dcfg = { .user_flags.auto_mount_off = 1 };   /* never mount it for the app */
        err = tinyusb_msc_install_driver(&dcfg);
        tinyusb_msc_storage_handle_t h;
        const tinyusb_msc_storage_config_t cfg = { .medium.card = bsp_sdcard, .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB };
        if (err == ESP_OK) err = tinyusb_msc_new_storage_sdmmc(&cfg, &h);
        const tinyusb_config_t tcfg = TINYUSB_DEFAULT_CONFIG();
        if (err == ESP_OK) err = tinyusb_driver_install(&tcfg);
    }
    ESP_LOGI(TAG, "USB DRIVE mode: %s", esp_err_to_name(err));   /* (the log stops here: the USB port changes role) */
    static const char *const ON[5] = { "CONNECTED.", "THE DRIVE APPEARS ON YOUR", "COMPUTER IN A MOMENT.", "COPY YOUR FILES, EJECT", "THE DRIVE, THEN PRESS C." };
    static const char *const WAIT[5] = { "CONNECTING...", "", "PLUG TAMANEST INTO", "YOUR COMPUTER WITH", "A DATA CABLE." };
    static const char *const BAD[3] = { "COULD NOT START.", "CHECK THE SD CARD.", "C RESTARTS TAMANEST." };
    /* feedback (owner 2026-11-23): CONNECTING... until the computer has set the drive up (tud_mounted), then
     * CONNECTED; back to CONNECTING... if the cable comes out */
    int shown = -1;
    while (1) {
        int st = err != ESP_OK ? 2 : tud_mounted() ? 1 : 0;
        if (st != shown) {
            shown = st;
            if (st == 2) usb_drive_screen(BAD, 3, "C:RESTART");
            else if (st == 1) usb_drive_screen(ON, 5, "C:DONE (RESTART)");
            else usb_drive_screen(WAIT, 5, "C:CANCEL (RESTART)");
            if (st < 2) ESP_LOGI(TAG, "USB DRIVE: %s", st ? "connected" : "waiting for the computer");
        }
        uint8_t down = keys_poll();
        if ((down & 4) && shown == 1) {               /* connected: make sure the drive was ejected (owner 2026-11-23) */
            static const char *const EJ[4] = { "HAVE YOU EJECTED THE", "DRIVE ON YOUR COMPUTER?", "(OTHERWISE NEW FILES", "MAY BE LOST.)" };
            usb_drive_screen(EJ, 4, "B:YES  C:NO");
            uint8_t k = 0;
            while (!(k = keys_poll() & 6)) vTaskDelay(pdMS_TO_TICKS(20));
            if (k & 2) break;
            shown = -1;                                /* not yet: back to the CONNECTED screen */
            continue;
        }
        if (down & 4) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    static const char *const BYE[1] = { "RESTARTING..." };
    usb_drive_screen(BYE, 1, "");
    /* Give the USB port back to the serial/JTAG side first: the PHY routing survives a software restart, so
     * without this the computer saw no USB device at all until the board was switched off (2026-11-22). */
    if (err == ESP_OK) tinyusb_driver_uninstall();
    usb_phy_handle_t ph;
    const usb_phy_config_t pc = { .controller = USB_PHY_CTRL_SERIAL_JTAG, .target = USB_PHY_TARGET_INT };
    usb_new_phy(&pc, &ph);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* UPDATE (2026-11-22, step 1 of the Wi-Fi updates): install SD:/UPDATE/TAMANEST.BIN into the other app slot. The
 * new version starts "on trial" (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): it is confirmed once the main menu is
 * reached; if it never gets there, the bootloader goes back to the previous version. Wi-Fi will later download
 * into the same file. */
#define UPDATE_FILE "/sdcard/update/tamanest.bin"
static bool update_file_version(char *ver, size_t n)   /* version of the update file (its esp_app_desc_t) */
{
    FILE *f = fopen(UPDATE_FILE, "rb");
    if (!f) return false;
    uint8_t hdr[32 + sizeof(esp_app_desc_t)];
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && hdr[0] == 0xE9;   /* image magic */
    fclose(f);
    const esp_app_desc_t *d = (const esp_app_desc_t *)(hdr + 32);           /* after the image + first segment header */
    if (!ok || d->magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    snprintf(ver, n, "%.31s", d->version);
    return true;
}
static void update_screen(const char *const *lines, int nl, const char *hint);
/* A long name (e.g. a Wi-Fi network, up to 32 characters) on at most two small-font lines (owner 2026-11-24: names
 * ran off the "CONNECTING TO" screen). Breaks after a space, '-' or '_' when it can; ".." if two lines are not
 * enough. Returns the number of lines (1 or 2). */
#define WRAP_PX 144
static int wrap_name(const char *name, char *l1, char *l2, size_t n)
{
    snprintf(l1, n, "%s", name); l2[0] = 0;
    if (vlcd_text_small(0, 0, l1, 2, true) <= WRAP_PX) return 1;
    size_t len = strlen(name), cut = 0;
    char t[48];
    for (size_t i = 1; i < len && i < sizeof t; i++) {   /* longest start that fits */
        memcpy(t, name, i); t[i] = 0;
        if (vlcd_text_small(0, 0, t, 2, true) > WRAP_PX) break;
        cut = i;
    }
    for (size_t i = cut; i > cut / 2; i--)        /* prefer a break after a separator */
        if (name[i - 1] == ' ' || name[i - 1] == '-' || name[i - 1] == '_') { cut = i; break; }
    snprintf(l1, n, "%.*s", (int)cut, name);
    snprintf(l2, n, "%s", name + cut);
    while (strlen(l2) > 3 && vlcd_text_small(0, 0, l2, 2, true) > WRAP_PX) {
        size_t l = strlen(l2); l2[l - 3] = '.'; l2[l - 2] = '.'; l2[l - 1] = 0;
    }
    return 2;
}
static void connecting_screen(const char *ssid, void (*screen)(const char *const *, int, const char *))
{
    char a[48], b[48];
    int n = wrap_name(ssid, a, b, sizeof a);
    const char *const C[3] = { "CONNECTING TO", a, b };
    screen(C, 1 + n, "");
}
/* Title of the message screens below (Wi-Fi setup, connecting, ...), which UPDATE, DATE/TIME and WI-FI TRANSFER
 * share: each sets its own while it runs (owner 2026-11-24: WI-FI TRANSFER showed "UPDATE"). */
static const char *s_msg_title = IC_UPDATE " UPDATE";
static void update_screen(const char *const *lines, int nl, const char *hint)
{
    vlcd_clear(0);
    vlcd_title(s_msg_title);
    vlcd_rect(6, 44, VL - 12, 14 + nl * 11, 1);
    for (int i = 0; i < nl; i++) vlcd_text_small_center(51 + i * 11, lines[i], 2);
    vlcd_text_center(134, hint, 1);
    vlcd_clock();
    vlcd_battery();
    vlcd_present();
}
/* Updates need some battery (owner 2026-11-22): at least UPDATE_MIN_BAT %, or USB power. (A power cut while
 * installing would not break TamaNest - the old version keeps running - but better not to start.) */
#define UPDATE_MIN_BAT 30                          /* on battery */
#define UPDATE_MIN_BAT_PLUGGED 15                  /* plugged in: enough to finish if the cable comes out */
static bool update_battery_ok(void)
{
    int pct; bool chg;
    if (!battery_read(&pct, &chg)) return true;      /* no fuel gauge answer: nothing to judge by */
    const bool ext = chg || battery_usb_power();
    if (pct >= (ext ? UPDATE_MIN_BAT_PLUGGED : UPDATE_MIN_BAT)) return true;
    char l1[32];
    snprintf(l1, sizeof l1, "THE BATTERY IS AT %d%%.", pct);
    const char *const L[4] = { "NOT ENOUGH BATTERY", l1, ext ? "PLEASE LET IT CHARGE" : "PLEASE CHARGE TO 30%",
                               ext ? "TO 15%, THEN RETRY." : "OR PLUG IN, THEN RETRY." };
    update_screen(L, 4, "C:BACK");
    ESP_LOGI(TAG, "Update refused: battery %d%%", pct);
    while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
    return false;
}
/* Install UPDATE_FILE (version `file`) into the other app slot; restarts on success. Returns only on failure. */
/* SIGNED UPDATES (2026-11-23): an update file must end with the 128-byte trailer written by web/sign_update.py -
 * "TNSIG001", key id, image length, ECDSA P-256 signature (r||s) of SHA-256(image) - made with the private key
 * that belongs to UPDATE_PUBKEY (main/update_key.h). Anything else is refused, so neither a doctored SD card file
 * nor a changed download can replace TamaNest. (The web installer over USB is not affected: it is the recovery.) */
#define SIG_TRAILER 128
static bool update_trailer(FILE *f, long size, long *image_len, uint8_t sig[64])
{
    uint8_t t[SIG_TRAILER];
    if (size <= SIG_TRAILER + 1024 || fseek(f, size - SIG_TRAILER, SEEK_SET) != 0 ||
        fread(t, 1, SIG_TRAILER, f) != SIG_TRAILER) { fseek(f, 0, SEEK_SET); return false; }
    fseek(f, 0, SEEK_SET);
    if (memcmp(t, "TNSIG001", 8) != 0 || memcmp(t + 8, UPDATE_KEY_ID, 4) != 0) return false;
    uint32_t n = (uint32_t)t[12] | (uint32_t)t[13] << 8 | (uint32_t)t[14] << 16 | (uint32_t)t[15] << 24;
    if ((long)n != size - SIG_TRAILER) return false;
    *image_len = (long)n;
    memcpy(sig, t + 16, 64);
    return true;
}
static bool update_sig_ok(const uint8_t hash[32], const uint8_t sig[64])
{
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t k;
    psa_status_t st = psa_import_key(&a, UPDATE_PUBKEY, sizeof UPDATE_PUBKEY, &k);
    if (st != PSA_SUCCESS) { ESP_LOGW(TAG, "Update key import failed (%d)", (int)st); return false; }
    st = psa_verify_hash(k, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32, sig, 64);
    psa_destroy_key(k);
    return st == PSA_SUCCESS;
}
static void update_refused(void)
{
    const char *const B[3] = { "THIS UPDATE IS NOT AN", "OFFICIAL TAMANEST BUILD.", "TAMANEST WAS NOT CHANGED." };
    update_screen(B, 3, "C:BACK");
    while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
}
static void update_install(const char *now, const char *file)
{
    if (!update_battery_ok()) return;
    FILE *f = fopen(UPDATE_FILE, "rb");
    long size = 0, img = 0;
    uint8_t sig[64];
    if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET); }
    if (!f || !update_trailer(f, size, &img, sig)) {   /* not signed with our key: refuse before touching flash */
        if (f) fclose(f);
        ESP_LOGW(TAG, "Update refused: %s is not signed with the TamaNest key", UPDATE_FILE);
        update_refused();
        return;
    }
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    esp_err_t err = dst ? esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h) : ESP_FAIL;
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    if (!buf) err = ESP_ERR_NO_MEM;
    psa_hash_operation_t hop = PSA_HASH_OPERATION_INIT;
    if (err == ESP_OK && (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hop, PSA_ALG_SHA_256) != PSA_SUCCESS)) err = ESP_FAIL;
    snprintf(s_loading_name, sizeof s_loading_name, "UPDATING");
    long done = 0; int last = -1;
    pm_hold(s_pm_ui, true);
    while (err == ESP_OK && done < img) {         /* the app image only (the trailer is not written) */
        size_t want = img - done < 4096 ? (size_t)(img - done) : 4096;
        size_t n = fread(buf, 1, want, f);
        if (n == 0) { err = ESP_FAIL; break; }
        if (psa_hash_update(&hop, buf, n) != PSA_SUCCESS) { err = ESP_FAIL; break; }
        err = esp_ota_write(h, buf, n);
        done += (long)n;
        int pct = (int)(done * 100 / (img ? img : 1));
        if (pct / 2 != last / 2) { last = pct; draw_loading(pct); }
    }
    fclose(f);
    free(buf);
    bool bad_sig = false;
    if (err == ESP_OK) {
        uint8_t hash[32]; size_t hl = 0;
        if (psa_hash_finish(&hop, hash, sizeof hash, &hl) != PSA_SUCCESS || hl != 32) err = ESP_FAIL;
        else if (!update_sig_ok(hash, sig)) { bad_sig = true; err = ESP_ERR_INVALID_CRC; }
    } else psa_hash_abort(&hop);
    if (h) {
        if (err == ESP_OK) err = esp_ota_end(h);   /* also checks the image itself */
        else esp_ota_abort(h);                     /* throw the written slot away; the running version stays */
    }
    if (err == ESP_OK) err = esp_ota_set_boot_partition(dst);
    ESP_LOGI(TAG, "Update %s -> %s from %s (%ld bytes, signature %s): %s", now, file, UPDATE_FILE, img,
             bad_sig ? "WRONG" : "ok", esp_err_to_name(err));
    if (bad_sig) { update_refused(); return; }
    if (err != ESP_OK) {
        const char *const B[3] = { "THE UPDATE FAILED.", "TAMANEST WAS NOT CHANGED.", "(CHECK THE FILE.)" };
        update_screen(B, 3, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    remove(UPDATE_FILE);                           /* installed: the file is not needed any more (owner 2026-11-23) */
    const char *const OK[2] = { "UPDATE INSTALLED.", "RESTARTING..." };
    update_screen(OK, 2, "");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

/* ---- Wi-Fi updates (2026-11-22, step 2) ----
 * The network is remembered in NVS "wifi" (ssid, pass, url). It can be chosen on the board (scan list, password on
 * an on-screen keyboard: touch, or A/B/C) or given in SD:/wifi.txt (lines ssid=..., password=..., update_url=...),
 * which is read each time the UPDATE screen opens. The update site is a folder with manifest.json (its "version")
 * and firmware/tamanest.bin - the GitHub Pages copy of web/ made by web/make_release.sh. */
#define UPDATE_URL_DEFAULT "https://waviegotchi.github.io/TamaNest"   /* GitHub Pages of github.com/WavieGotchi/TamaNest (web/) */
static char s_wifi_ssid[33], s_wifi_pass[65], s_upd_url[128];
static void wifi_creds_load(void)
{
    nvs_handle_t nvs;
    snprintf(s_upd_url, sizeof s_upd_url, "%s", UPDATE_URL_DEFAULT);
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t n = sizeof s_wifi_ssid; if (nvs_get_str(nvs, "ssid", s_wifi_ssid, &n) != ESP_OK) s_wifi_ssid[0] = 0;
    n = sizeof s_wifi_pass; if (nvs_get_str(nvs, "pass", s_wifi_pass, &n) != ESP_OK) s_wifi_pass[0] = 0;
#if TL_DEVLOG   /* developer builds only: another update site (local testing). Release builds always use the official
               * site, so an SD card file can never point updates at a different website (security, 2026-11-23). */
    char u[sizeof s_upd_url]; n = sizeof u;
    if (nvs_get_str(nvs, "url", u, &n) == ESP_OK && u[0]) snprintf(s_upd_url, sizeof s_upd_url, "%s", u);
#endif
    nvs_close(nvs);
}
static void wifi_creds_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, "ssid", s_wifi_ssid);
    nvs_set_str(nvs, "pass", s_wifi_pass);
    if (strcmp(s_upd_url, UPDATE_URL_DEFAULT)) nvs_set_str(nvs, "url", s_upd_url); else nvs_erase_key(nvs, "url");
    nvs_commit(nvs);
    nvs_close(nvs);
}
static void wifi_txt_import(void)                  /* SD:/wifi.txt overrides what is remembered */
{
    FILE *f = fopen("/sdcard/wifi.txt", "r");
    if (!f) return;
    char line[160], keep[1024];
    size_t kept = 0;
    bool any = false, had_pass = false;
    keep[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char orig[160];
        snprintf(orig, sizeof orig, "%s", line);
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        bool drop = false;
        if (eq && line[0] != '#') {
            *eq = 0;
            char *k = line, *v = eq + 1;
            while (*k == ' ') k++;
            for (char *e = k + strlen(k); e > k && e[-1] == ' '; ) *--e = 0;   /* "ssid = x" works too */
            while (*v == ' ') v++;
            for (int i = 0; k[i]; i++) k[i] = (char)tolower((unsigned char)k[i]);
            if (!strcmp(k, "ssid")) { snprintf(s_wifi_ssid, sizeof s_wifi_ssid, "%s", v); any = true; }
            else if (!strcmp(k, "password") || !strcmp(k, "pass")) {
                snprintf(s_wifi_pass, sizeof s_wifi_pass, "%s", v); any = true; had_pass = drop = true;
            }
#if TL_DEVLOG
            else if (!strcmp(k, "update_url") || !strcmp(k, "url")) {
                size_t l = strlen(v); while (l && v[l - 1] == '/') v[--l] = 0;
                snprintf(s_upd_url, sizeof s_upd_url, "%s", v); any = true;
            }
#endif
        }
        if (!drop) {                               /* every other line stays in the file as it was */
            size_t l = strlen(orig);
            if (kept + l < sizeof keep) { memcpy(keep + kept, orig, l + 1); kept += l; }
        }
    }
    fclose(f);
    if (!any) return;
    wifi_creds_save();
    ESP_LOGI(TAG, "wifi.txt read (network%s)", had_pass ? " and password" : "");
    if (had_pass) {   /* security (owner 2026-11-23): the password must not stay readable on the SD card */
        FILE *o = fopen("/sdcard/wifi.tmp", "w");
        bool ok = o != NULL;
        if (ok) {
            if (kept && keep[kept - 1] != '\n') { keep[kept++] = '\n'; keep[kept] = 0; }
            ok = fputs(keep, o) >= 0 && fputs("# The password was saved on the device and removed from this file.\n", o) >= 0;
            ok = fclose(o) == 0 && ok;
        }
        if (ok) { remove("/sdcard/wifi.txt"); ok = rename("/sdcard/wifi.tmp", "/sdcard/wifi.txt") == 0; }
        if (!ok) { remove("/sdcard/wifi.tmp"); remove("/sdcard/wifi.txt"); }   /* never leave the password behind */
        ESP_LOGI(TAG, "wifi.txt: password removed from the SD card");
    }
}

/* Menus while Wi-Fi is on are drawn straight to the panel in 12 KB strips (like the quick menu), so they need no
 * big internal memory block; Wi-Fi gets its memory back when it is switched off. If the internal memory is too
 * broken up afterwards for the normal menu screen, TamaNest restarts (2-3 s). */
static void net_ui_begin(void)
{
    esp_lv_adapter_set_dummy_draw(s_disp, true);
    s_vlcd_direct = true;
}
static bool s_emu_lent;                          /* emulator memory lent to Wi-Fi (phone setup) */
/* Wi-Fi as an access point (PHONE SETUP) or with a file server (WI-FI TRANSFER) needs more of the chip's internal
 * memory than is free in the menus, so the emulator's 65 KB (unused while no game runs) is lent to it;
 * net_ui_end() takes it back. portal_prepare() first makes the things the system creates once and keeps (see
 * wifi_update.c), so they do not land inside the lent space and the 65 KB comes back in one piece. */
#if CONFIG_HEAP_TRACING_STANDALONE
/* Diagnostic builds only (heap tracing on): which allocations are still inside the lent 65 KB when it should come
 * back, and who made them (resolve the addresses with addr2line on the ELF). Used to find the one-time allocations
 * that portal_prepare() now makes in advance. */
#include "esp_heap_trace.h"
static uintptr_t s_lent_at;
static heap_trace_record_t *s_trace_buf;
#define TRACE_N 1500
static void lent_trace_start(void)
{
    if (!s_trace_buf) s_trace_buf = heap_caps_calloc(TRACE_N, sizeof(heap_trace_record_t), MALLOC_CAP_SPIRAM);
    if (!s_trace_buf || heap_trace_init_standalone(s_trace_buf, TRACE_N) != ESP_OK) return;
    heap_trace_start(HEAP_TRACE_LEAKS);
}
static void lent_trace_dump(void)
{
    heap_trace_stop();
    size_t n = heap_trace_get_count();
    for (size_t i = 0; i < n; i++) {
        heap_trace_record_t r;
        if (heap_trace_get(i, &r) != ESP_OK) continue;
        uintptr_t a = (uintptr_t)r.address;
        if (!(a + r.size > s_lent_at && a < s_lent_at + sizeof(Emu))) continue;
        char t[160]; int k = 0;
        for (int d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH && k < (int)sizeof t - 12; d++)
            k += snprintf(t + k, sizeof t - k, " %p", r.alloced_by[d]);
        ESP_LOGI(TAG, "TRACE in lent space: +%u size %u by%s", (unsigned)(a - s_lent_at), (unsigned)r.size, t);
    }
}
#else
static void lent_trace_start(void) {}
static void lent_trace_dump(void) {}
#endif
static void emu_lend(void)
{
    portal_prepare();
    if (!s_emu && s_emu_buf && esp_ptr_internal(s_emu_buf) && !s_emu_lent) {
#if CONFIG_HEAP_TRACING_STANDALONE
        s_lent_at = (uintptr_t)s_emu_buf;
#endif
        heap_caps_free(s_emu_buf);
        s_emu_buf = NULL;
        s_emu_lent = true;
        lent_trace_start();
    }
}
static void net_ui_end(void)
{
    wifi_off();
    if (s_emu_lent) lent_trace_dump();
    if (s_emu_lent) {                              /* take the emulator's fast memory back in one piece */
        s_emu_buf = heap_caps_calloc(1, sizeof(Emu), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "Emulator memory back from Wi-Fi: %s", s_emu_buf ? "yes" : "no (restarting)");
        if (!s_emu_buf) {
            const char *const R[1] = { "RESTARTING..." };
            update_screen(R, 1, "");
            vTaskDelay(pdMS_TO_TICKS(600));
            esp_restart();
        }
        s_emu_lent = false;
    }
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "After Wi-Fi: internal RAM free %u, largest DMA block %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)big);
    if (big < 16 * 1024) {                         /* the menu screen sends 480x16-pixel bands (15360 bytes) */
        ESP_LOGW(TAG, "Internal memory too fragmented for the menu screen: restarting");
        const char *const R[1] = { "RESTARTING..." };
        update_screen(R, 1, "");
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();
    }
    s_vlcd_direct = false;
    esp_lv_adapter_set_dummy_draw(s_disp, false);
}

/* Touch (only on the keyboard screen; the touch chip is otherwise held in reset to save power). Panel
 * coordinates; BUTTONS BOTTOM turns the picture 180 degrees in the panel itself, so the touch point is turned too. */
static lv_indev_t *s_touch;
static void touch_enable(bool on)
{
    if (on) { gpio_set_level(BSP_LCD_TOUCH_RST, 1); vTaskDelay(pdMS_TO_TICKS(120)); }   /* chip starts up */
    bsp_display_lock(-1);
    s_touch = bsp_display_get_input_dev();
    if (s_touch) lv_indev_enable(s_touch, on);
    bsp_display_unlock();
    if (!on) gpio_set_level(BSP_LCD_TOUCH_RST, 0);
    ESP_LOGI(TAG, "Touch %s%s", on ? "on" : "off", s_touch ? "" : " (no input device)");
}
static bool touch_point(int *vx, int *vy)          /* pressed? and where, in virtual LCD pixels */
{
    if (!s_touch) return false;
    lv_point_t p;
    bsp_display_lock(-1);
    bool down = lv_indev_get_state(s_touch) == LV_INDEV_STATE_PRESSED;
    lv_indev_get_point(s_touch, &p);
    bsp_display_unlock();
    if (!down) return false;
    int x = p.x, y = p.y;
    if (s_flip) { x = SCR_W - 1 - x; y = SCR_H - 1 - y; }
    *vx = x / 3; *vy = y / 3;
    s_ui_poke = esp_timer_get_time();
    return true;
}

/* On-screen keyboard: 4 rows of 7 keys (20x20 virtual pixels = 60 panel pixels) and a bottom row of 7:
 * SHIFT (capital letters, stays on), 123 (numbers), #+= (symbols), SPACE, DELETE, EYE (show/hide a password) and
 * OK (owner 2026-11-24: cycling through pages was slow and a hidden password hid typos). Tap a key, or: A moves the
 * cursor (hold to repeat), B types the key under it, C deletes (C with nothing typed = back). Returns false for back. */
#define KB_X 10
#define KB_Y 56                                    /* (was 50: more room for the prompt and the text field, owner 2026-11-22) */
static const char *const KB_PAGES[4] = {
    "abcdefghijklmnopqrstuvwxyz.-",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-",
    "1234567890!@#$%^&*()_+=?/:;'",
    "\",<>[]{}|\\~`                ",
};
enum { KB_SHIFT = 28, KB_NUM, KB_SYM, KB_SPACE, KB_DEL, KB_EYE, KB_OK, KB_N };
static void kb_key_rect(int k, int *x, int *y, int *w, int *h)
{
    if (k < 28) { *x = KB_X + (k % 7) * 20; *y = KB_Y + (k / 7) * 20; *w = 20; *h = 20; return; }
    *x = KB_X + (k - 28) * 20; *y = KB_Y + 80; *w = 20; *h = 16;
}
static int kb_hit(int vx, int vy)
{
    for (int k = 0; k < KB_N; k++) {
        int x, y, w, h; kb_key_rect(k, &x, &y, &w, &h);
        if (vx >= x && vx < x + w && vy >= y && vy < y + h) return k;
    }
    return -1;
}
static bool s_kb_secret;                           /* typing a password (the EYE key shows it) */
static bool s_kb_show;                             /* password shown in full */
static void kb_draw(const char *title, const char *prompt, const char *text, int mode, bool shift, int cur, int pressed)
{
    vlcd_clear(0);
    vlcd_title(title);
    char pr[48];                                   /* the prompt, shortened to fit (long network names) */
    snprintf(pr, sizeof pr, "%s", prompt);
    while (strlen(pr) > 4 && vlcd_text_small(0, 0, pr, 2, true) > VL - 12) { size_t l = strlen(pr); pr[l - 3] = '.'; pr[l - 2] = '.'; pr[l - 1] = 0; }
    vlcd_text_small(6, 35, pr, 2, false);
    vlcd_frame(6, 43, VL - 12, 10, 2);
    int len = (int)strlen(text), first = len > 16 ? len - 16 : 0;
    char shown[20];
    snprintf(shown, sizeof shown, "%s_", text + first);
    if (s_kb_secret && !s_kb_show)                 /* hidden: only the last character shows */
        for (int i = 0; i < len - first - 1; i++) shown[i] = '*';
    if (!len) vlcd_text_small(10, 45, "TAP, OR A MOVE  B TYPE", 1, false);
    else vlcd_text(8, 44, shown, 2);
    const int page = mode == 0 ? (shift ? 1 : 0) : mode;
    for (int k = 0; k < KB_N; k++) {
        int x, y, w, h; kb_key_rect(k, &x, &y, &w, &h);
        if (k == KB_EYE && !s_kb_secret) continue;   /* only for passwords */
        char lab[6] = "";
        const char *small = NULL;                    /* label in the small font */
        bool lit = (k == KB_SHIFT && shift && mode == 0) || (k == KB_EYE && s_kb_show);
        if (k < 28) { lab[0] = KB_PAGES[page][k]; lab[1] = 0; if (lab[0] == ' ') continue; }
        else if (k == KB_SHIFT) snprintf(lab, sizeof lab, IC_KSHIFT);
        else if (k == KB_NUM) small = mode == 2 ? "ABC" : "123";
        else if (k == KB_SYM) small = mode == 3 ? "ABC" : "#+=";
        else if (k == KB_SPACE) snprintf(lab, sizeof lab, IC_KSPACE);
        else if (k == KB_DEL) snprintf(lab, sizeof lab, IC_KDEL);
        else if (k == KB_EYE) snprintf(lab, sizeof lab, IC_KEYE);
        else snprintf(lab, sizeof lab, "OK");
        bool on = k == cur, dn = k == pressed || lit;
        if (dn) vlcd_rect(x + 1, y + 1, w - 2, h - 2, 2);
        else if (on) vlcd_rect(x + 1, y + 1, w - 2, h - 2, 3);
        vlcd_frame(x + 1, y + 1, w - 2, h - 2, on || dn ? 2 : 1);
        if (small) { int tw = vlcd_text_small(0, 0, small, 2, true); vlcd_text_small(x + (w - tw) / 2, y + (h - 5) / 2, small, dn ? 0 : 2, false); }
        else vlcd_text(x + (w - (int)strlen(lab) * 8) / 2, y + (h - 8) / 2, lab, dn ? 0 : 2);
    }
    vlcd_clock();
    vlcd_battery();
    vlcd_present();
}
static bool text_entry(const char *title, const char *prompt, char *buf, size_t max)
{
    int mode = 0, cur = 0, pressed = -1;           /* mode 0 letters, 2 numbers, 3 symbols */
    bool shift = false, redraw = true;
    int64_t a_down = 0, a_rep = 0;
    s_kb_show = false;
    touch_enable(true);
    while (1) {
        if (redraw) { kb_draw(title, prompt, buf, mode, shift, cur, pressed); redraw = false; }
        int k = -1;                                  /* key to type this pass */
        int vx, vy;
        bool t = touch_point(&vx, &vy);
        if (t) { int h = kb_hit(vx, vy); if (h == KB_EYE && !s_kb_secret) h = -1;
                 if (h != pressed) { pressed = h; if (h >= 0) cur = h; redraw = true; } }
        else if (pressed >= 0) { k = pressed; pressed = -1; redraw = true; }   /* typed when the finger lifts */
        uint8_t down = keys_poll();
        int64_t now = esp_timer_get_time();
        bool step = false;
        if (down & 1) { a_down = now; a_rep = now + 400000; step = true; }
        else if ((s_keys & 1) && a_down && now >= a_rep) { a_rep = now + 90000; step = true; }
        if (step) {                                  /* next key (blank symbol slots and a hidden EYE are skipped) */
            const int page = mode == 0 ? (shift ? 1 : 0) : mode;
            do cur = (cur + 1) % KB_N;
            while ((cur < 28 && KB_PAGES[page][cur] == ' ') || (cur == KB_EYE && !s_kb_secret));
            redraw = true;
        }
        if (!(s_keys & 1)) a_down = 0;
        if (down & 2) k = cur;
        if (down & 4) {
            size_t l = strlen(buf);
            if (!l) { touch_enable(false); return false; }
            buf[l - 1] = 0; redraw = true;
        }
        if (k >= 0) {
            size_t l = strlen(buf);
            const int page = mode == 0 ? (shift ? 1 : 0) : mode;
            if (k < 28) { char c = KB_PAGES[page][k]; if (c != ' ' && l + 1 < max) { buf[l] = c; buf[l + 1] = 0; } }
            else if (k == KB_SHIFT) { if (mode == 0) shift = !shift; else { mode = 0; shift = true; } }
            else if (k == KB_NUM) mode = mode == 2 ? 0 : 2;
            else if (k == KB_SYM) mode = mode == 3 ? 0 : 3;
            else if (k == KB_SPACE) { if (l + 1 < max) { buf[l] = ' '; buf[l + 1] = 0; } }
            else if (k == KB_DEL) { if (l) buf[l - 1] = 0; }
            else if (k == KB_EYE) s_kb_show = !s_kb_show;
            else { touch_enable(false); return true; }
            redraw = true;
        }
        vTaskDelay(pdMS_TO_TICKS(t ? 15 : 20));
    }
}

/* Test a network and keep it (NVS) if it works. Shows CONNECTING TO; the reason in *why if not. */
static esp_err_t wifi_test_keep(const char *ssid, const char *pass, const char **why)
{
    connecting_screen(ssid, update_screen);
    esp_err_t e = wifi_connect(ssid, pass, 15000, why);
    wifi_off();
    if (e != ESP_OK) return e;
    snprintf(s_wifi_ssid, sizeof s_wifi_ssid, "%s", ssid);
    snprintf(s_wifi_pass, sizeof s_wifi_pass, "%s", pass);
    wifi_creds_save();
    return ESP_OK;
}
static void wifi_kept_screen(void)
{
    const char *const OK[2] = { "CONNECTED. TAMANEST WILL", "USE THIS WI-FI." };
    update_screen(OK, 2, "C:BACK");
    while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
}

/* PHONE SETUP screen (owner 2026-11-24): a QR code that joins TamaNest's own setup network, the name and password
 * to type instead, and what is happening. phone: 0 waiting, 1 a phone has joined. */
#define PHONE_SETUP_MIN 10                          /* the setup network closes after this many minutes */
static void phone_screen(const char *ap, const char *pw, const uint8_t *qr, int phone)
{
    vlcd_clear(0);
    vlcd_title(IC_WIFI " PHONE SETUP");
    int y = 38;
    if (qr) {                                        /* 2 px per module; the light screen is the quiet zone */
        const int n = qrcodegen_getSize(qr), sc = 2, x0 = 8, y0 = 37;
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                if (qrcodegen_getModule(qr, c, r)) vlcd_rect(x0 + c * sc, y0 + r * sc, sc, sc, 2);
    }
    const int x = 78;
    vlcd_text_small(x, y, "SCAN WITH", 2, false); y += 8;
    vlcd_text_small(x, y, "YOUR PHONE'S", 2, false); y += 8;
    vlcd_text_small(x, y, "CAMERA, OR", 2, false); y += 8;
    vlcd_text_small(x, y, "JOIN THE WI-FI", 2, false); y += 12;
    vlcd_text_small(x, y, ap, 2, false); y += 12;
    vlcd_text_small(x, y, "PASSWORD", 1, false); y += 8;
    vlcd_text_small(x, y, pw, 2, false);
    vlcd_rect(6, 106, VL - 12, 24, 1);
    vlcd_text_small_center(110, phone ? "PHONE CONNECTED." : "WAITING FOR YOUR PHONE...", 2);
    vlcd_text_small_center(120, phone ? "NO PAGE? OPEN 192.168.4.1" : "A SETUP PAGE OPENS ON IT.", 2);
    vlcd_text_center(134, "C:CANCEL", 1);
    vlcd_clock();
    vlcd_battery();
    vlcd_present();
}

/* Set up Wi-Fi from a phone: TamaNest becomes its own Wi-Fi network for a few minutes and the phone sends the
 * home network's name and password from a web page (wifi_update.c portal_*). `aps` = the networks just scanned. */
static void phone_setup(const wifi_ap_t *aps, int n)
{
    char ap[24], pw[12], q[96];
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(4)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
    portal_names(ap, sizeof ap, pw, sizeof pw);       /* the device's own setup password (kept) */
    snprintf(q, sizeof q, "WIFI:T:WPA;S:%s;P:%s;;", ap, pw);
    const bool qok = qrcodegen_encodeText(q, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 4, qrcodegen_Mask_AUTO, true);
    memset(q, 0, sizeof q);
    /* Wi-Fi as an access point needs more of the chip's internal memory than is free in the menus, so the
     * emulator's 65 KB (unused while no game runs) is lent to it; net_ui_end() takes it back (or restarts). */
    emu_lend();
    for (;;) {
        const char *const S[1] = { "STARTING PHONE SETUP..." };
        update_screen(S, 1, "");
        if (portal_start(ap, pw, aps, n) != ESP_OK) {
            portal_stop();
            const char *const E[2] = { "PHONE SETUP DID NOT START.", "PLEASE TRY AGAIN." };
            update_screen(E, 2, "C:BACK");
            while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
            return;
        }
        const int64_t t0 = esp_timer_get_time();
        int shown = -1;
        char ssid[33], pass[65];
        bool got = false, timeout = false;
        for (;;) {
            s_ui_poke = esp_timer_get_time();         /* the menus do not fall asleep while the phone is used */
            const int ph = portal_phones() > 0;
            if (ph != shown) { phone_screen(ap, pw, qok ? qr : NULL, ph); shown = ph; }
            if (portal_take(ssid, sizeof ssid, pass, sizeof pass)) { got = true; break; }
            if (keys_poll() & 4) break;
            if (esp_timer_get_time() - t0 > PHONE_SETUP_MIN * 60LL * 1000000) { timeout = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!got) {
            portal_stop();
            if (timeout) {
                const char *const T[2] = { "PHONE SETUP CLOSED", "(NOTHING WAS SENT)." };
                update_screen(T, 2, "C:BACK");
                while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
            }
            return;
        }
        connecting_screen(ssid, update_screen);
        vTaskDelay(pdMS_TO_TICKS(1500));              /* let the phone receive its "thank you" page */
        portal_stop();
        const char *why = NULL;
        esp_err_t e = wifi_test_keep(ssid, pass, &why);
        memset(pass, 0, sizeof pass);
        if (e == ESP_OK) { wifi_kept_screen(); return; }
        const char *const F[3] = { "COULD NOT CONNECT:", why, "TRY AGAIN FROM THE PHONE?" };
        update_screen(F, 3, "B:YES C:BACK");
        uint8_t d;
        while (!((d = keys_poll()) & 6)) vTaskDelay(pdMS_TO_TICKS(10));
        if (!(d & 2)) return;
    }
}

/* Choose a network: scan, list (strongest first), password. Tests the connection before keeping it. */
static void wifi_setup(void)
{
    const char *const S[1] = { "LOOKING FOR WI-FI..." };
    update_screen(S, 1, "");
    static wifi_ap_t aps[12];
    int n = wifi_scan(aps, 12);
    wifi_off();                                      /* not needed while choosing and typing */
    ESP_LOGI(TAG, "Wi-Fi scan: %d networks", n);
    if (n < 0) {
        const char *const E[2] = { "WI-FI DID NOT START.", "PLEASE TRY AGAIN." };
        update_screen(E, 2, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    static char rows[14][24];
    const char *items[15];
    snprintf(rows[13], sizeof rows[13], "SET UP WITH A PHONE");   /* first: no typing on the device (owner 2026-11-24) */
    items[0] = rows[13];
    for (int i = 0; i < n; i++) {
        char nm[18];
        if (strlen(aps[i].ssid) > 15) snprintf(nm, sizeof nm, "%.13s..", aps[i].ssid);   /* cut off: ".." */
        else snprintf(nm, sizeof nm, "%s", aps[i].ssid);
        int bars = aps[i].rssi > -60 ? 3 : aps[i].rssi > -72 ? 2 : 1;
        list_item(rows[i], sizeof rows[i], nm, bars == 3 ? "***" : bars == 2 ? "**" : "*");
        items[1 + i] = rows[i];
    }
    snprintf(rows[n], sizeof rows[n], "OTHER (TYPE NAME)");
    items[1 + n] = rows[n];
    int m = n + 2;
    static char forget[24];
    if (s_wifi_ssid[0]) { snprintf(forget, sizeof forget, "FORGET SAVED WI-FI"); items[m++] = forget; }
    int r = run_list(IC_WIFI " CHOOSE WI-FI", items, m, 0);
    if (r < 0) return;
    if (r == 0) { phone_setup(aps, n); return; }
    r--;                                             /* from here: 0..n-1 networks, n other, n+1 forget */
    if (r == n + 1) {                                /* forget: e.g. before giving the device to someone */
        char nm[48];
        char n2[48];
        int nl = wrap_name(s_wifi_ssid, nm, n2, sizeof nm);
        const char *const Q[4] = { "FORGET THIS WI-FI AND", "ITS PASSWORD?", nm, n2 };
        update_screen(Q, 2 + nl, "B:FORGET C:BACK");
        uint8_t d;
        while (!((d = keys_poll()) & 6)) vTaskDelay(pdMS_TO_TICKS(10));
        if (d & 2) {
            memset(s_wifi_pass, 0, sizeof s_wifi_pass);
            s_wifi_ssid[0] = 0;
            nvs_handle_t h;
            if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_key(h, "ssid"); nvs_erase_key(h, "pass"); nvs_commit(h); nvs_close(h); }
            ESP_LOGI(TAG, "Saved Wi-Fi forgotten");
            const char *const D[1] = { "WI-FI FORGOTTEN." };
            update_screen(D, 1, "C:BACK");
            while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        }
        return;
    }
    char ssid[33] = "", pass[65] = "";
    bool secure = true;
    if (r < n) { snprintf(ssid, sizeof ssid, "%s", aps[r].ssid); secure = aps[r].secure; }
    else if (!text_entry(IC_WIFI " NETWORK NAME", "TYPE THE NAME OF THE NETWORK", ssid, sizeof ssid) || !ssid[0]) return;
    if (secure) {
        char prompt[48];
        snprintf(prompt, sizeof prompt, "PASSWORD FOR %.18s", ssid);
        if (!strcmp(ssid, s_wifi_ssid)) snprintf(pass, sizeof pass, "%s", s_wifi_pass);   /* known: start from it */
        s_kb_secret = true;
        bool typed = text_entry(IC_WIFI " PASSWORD", prompt, pass, sizeof pass);
        s_kb_secret = false;
        if (!typed) return;
    }
    const char *why = NULL;
    if (wifi_test_keep(ssid, pass, &why) != ESP_OK) {
        memset(pass, 0, sizeof pass);
        const char *const F[2] = { "COULD NOT CONNECT:", why };
        update_screen(F, 2, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    memset(pass, 0, sizeof pass);
    wifi_kept_screen();
}

static int ver_cmp(const char *a, const char *b)   /* "1.2.10" style; >0 if a is newer */
{
    for (int i = 0; i < 3; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x > y ? 1 : -1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}
static void dl_progress(long done, long total)
{
    static int last = -1;
    int pct = total > 0 ? (int)(done * 100 / total) : (int)((done / 20000) % 100);
    if (done < 8192) last = -1;
    if (pct / 2 != last / 2) { last = pct; draw_loading(pct); }
}
static void update_online(const char *now)
{
    if (!s_upd_url[0]) {
        const char *const N[4] = { "NO UPDATE SITE IS SET IN", "THIS VERSION. PUT THE NEW", "TAMANEST.BIN IN THE SD", "CARD'S UPDATE FOLDER." };
        update_screen(N, 4, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    if (!s_wifi_ssid[0]) { wifi_setup(); if (!s_wifi_ssid[0]) return; }
    connecting_screen(s_wifi_ssid, update_screen);
    const char *why = NULL;
    char msg[48], ver[24] = "";
    if (wifi_connect(s_wifi_ssid, s_wifi_pass, 15000, &why) != ESP_OK) {
        wifi_off();
        const char *const F[3] = { "COULD NOT CONNECT:", why, "B:CHOOSE WI-FI" };
        update_screen(F, 3, "B:WI-FI C:BACK");
        while (1) {
            uint8_t d = keys_poll();
            if (d & 4) return;
            if (d & 2) { wifi_setup(); return; }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    const char *const K[1] = { "CHECKING FOR UPDATES..." };
    update_screen(K, 1, "");
    char url[200], *txt = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    snprintf(url, sizeof url, "%s/manifest.json", s_upd_url);
    esp_err_t e = txt ? net_get_text(url, txt, 2048) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) {
        const char *p = strstr(txt, "\"version\"");
        p = p ? strchr(p + 9, '"') : NULL;
        if (p) { p++; size_t l = strcspn(p, "\""); if (l < sizeof ver) { memcpy(ver, p, l); ver[l] = 0; } }
    }
    free(txt);
    ESP_LOGI(TAG, "Update check: %s -> %s (site has %s)", url, esp_err_to_name(e), ver[0] ? ver : "?");
    if (e != ESP_OK || !ver[0]) {
        wifi_off();
        char sig[32];
        snprintf(sig, sizeof sig, "(SIGNAL %d DBM). MOVE", wifi_rssi());
        const char *const F[3] = { "THE UPDATE SITE DID", "NOT ANSWER.", "PLEASE TRY AGAIN LATER." };
        const char *const WK[3] = { "THE WI-FI SIGNAL IS WEAK", sig, "CLOSER TO THE ROUTER." };
        const bool weak = wifi_rssi() != 0 && wifi_rssi() < -72;
        update_screen(weak ? WK : F, 3, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    if (ver_cmp(ver, now) <= 0) {
        wifi_off();
        snprintf(msg, sizeof msg, "TAMANEST %s", now);
        const char *const U[2] = { msg, "IS UP TO DATE." };
        update_screen(U, 2, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    snprintf(msg, sizeof msg, "TAMANEST %s IS OUT.", ver);
    const char *const A[3] = { msg, "DOWNLOAD AND INSTALL?", "(YOUR SAVES STAY.)" };
    update_screen(A, 3, "B:YES C:BACK");
    while (1) {
        uint8_t d = keys_poll();
        if (d & 4) { wifi_off(); return; }
        if (d & 2) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!update_battery_ok()) { wifi_off(); return; }   /* before downloading, too */
    mkdir("/sdcard/update", 0777);
    snprintf(url, sizeof url, "%s/firmware/tamanest.bin", s_upd_url);
    snprintf(s_loading_name, sizeof s_loading_name, "DOWNLOADING");
    draw_loading(0);
    e = net_download(url, UPDATE_FILE, dl_progress);
    wifi_off();
    char got[48] = "";
    if (e != ESP_OK || !update_file_version(got, sizeof got) || strcmp(got, ver)) {
        ESP_LOGW(TAG, "Update download: %s, file version %s", esp_err_to_name(e), got[0] ? got : "?");
        const bool weak = wifi_rssi() != 0 && wifi_rssi() < -72;
        const char *const F[3] = { "THE DOWNLOAD FAILED.", "TAMANEST WAS NOT CHANGED.", weak ? "MOVE CLOSER TO THE ROUTER." : "PLEASE TRY AGAIN." };
        update_screen(F, 3, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    update_install(now, got);                        /* restarts when it worked */
}

/* SETTINGS > UPDATE: CHECK ONLINE, WI-FI (network), FROM SD CARD. */
static void update_menu(void)
{
    char now[48];
    snprintf(now, sizeof now, "%.31s", esp_app_get_description()->version);
    wifi_creds_load();
    wifi_txt_import();
    net_ui_begin();
    int sel = 0;
    while (1) {
        char file[48], it0[24], it1[48], it2[24], v[20];
        bool have = update_file_version(file, sizeof file);
        snprintf(v, sizeof v, "%s", now);
        list_item(it0, sizeof it0, IC_UPDATE " CHECK ONLINE", "");
        {   /* two lines (owner 2026-11-22): "WI-FI  >", then the network on a line of its own (16 characters) */
            char w1[24], nm[20];
            list_item(w1, sizeof w1, IC_WIFI " WI-FI", s_wifi_ssid[0] ? ">" : "NOT SET >");
            if (!s_wifi_ssid[0]) nm[0] = 0;          /* no network yet: one line */
            else if (strlen(s_wifi_ssid) > 16) snprintf(nm, sizeof nm, "%.14s..", s_wifi_ssid);
            else snprintf(nm, sizeof nm, "%s", s_wifi_ssid);
            if (nm[0]) snprintf(it1, sizeof it1, "%s\n%s", w1, nm);
            else snprintf(it1, sizeof it1, "%s", w1);
        }
        snprintf(v, sizeof v, "%.9s", have ? file : "NONE");
        list_item(it2, sizeof it2, IC_EXPORT " SD CARD", v);
        const char *const items[3] = { it0, it1, it2 };
        char vv[24];
        snprintf(vv, sizeof vv, "VERSION %.12s", now);
        s_title_right = vv;
        int r = run_list(IC_UPDATE " UPDATE", items, 3, sel);
        s_title_right = NULL;
        if (r < 0) break;
        sel = r;
        if (r == 0) update_online(now);
        else if (r == 1) wifi_setup();
        else {
            char l0[48], l1[48];
            snprintf(l0, sizeof l0, "INSTALLED: TAMANEST %s", now);
            if (!have) {
                mkdir("/sdcard/update", 0777);
                const char *const L[4] = { l0, "TO UPDATE, PUT THE NEW", "TAMANEST.BIN IN THE SD", "CARD'S UPDATE FOLDER." };
                update_screen(L, 4, "C:BACK");
                while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            snprintf(l1, sizeof l1, "ON THE SD CARD: %s", file);
            const char *const L[4] = { l0, l1, "INSTALL IT? TAMANEST", "RESTARTS AFTERWARDS." };
            update_screen(L, 4, "B:INSTALL C:BACK");
            bool go = false;
            while (1) {
                uint8_t d = keys_poll();
                if (d & 4) break;
                if (d & 2) { go = true; break; }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (go) update_install(now, file);
        }
    }
    net_ui_end();
}


/* ---- DATE/TIME menu (2026-11-23): SET BY HAND, SET FROM INTERNET, TIME ZONE ---- */
static void time_screen(const char *const *lines, int nl, const char *hint)
{
    vlcd_clear(0);
    vlcd_title(IC_CLOCK " DATE/TIME");
    vlcd_rect(6, 44, VL - 12, 14 + nl * 11, 1);
    for (int i = 0; i < nl; i++) vlcd_text_small_center(51 + i * 11, lines[i], 2);
    vlcd_text_center(134, hint, 1);
    vlcd_clock();
    vlcd_battery();
    vlcd_present();
}
static uint8_t time_wait_keys(uint8_t mask)
{
    uint8_t d;
    while (!((d = keys_poll()) & mask)) vTaskDelay(pdMS_TO_TICKS(10));
    return d;
}
/* Zone list: name + its offset now. Returns the index, or -1 (C). */
static int zone_pick(const char *title, int sel, time_t utc)
{
    static char rows[TZ_COUNT][24];
    static const char *items[TZ_COUNT];
    for (int i = 0; i < TZ_COUNT; i++) {
        char o[12];
        tz_offset_text(tz_offset_at(i, utc), o, sizeof o);
        list_item(rows[i], sizeof rows[i], TZONES[i].name, o);
        items[i] = rows[i];
    }
    return run_list(title, items, TZ_COUNT, sel < 0 ? 0 : sel);
}
static int tz_guess(time_t utc)                    /* the zone that matches the board clock now (first time) */
{
    uint32_t local;
    int utc_i = 0;
    for (int i = 0; i < TZ_COUNT; i++) if (!strcmp(TZONES[i].name, "UTC")) utc_i = i;
    if (!board_local_secs(&local)) return utc_i;
    int64_t d = ((int64_t)local - (int64_t)utc) / 60;
    d = (d >= 0 ? d + 7 : d - 7) / 15 * 15;        /* to the nearest quarter hour */
    for (int i = 0; i < TZ_COUNT; i++) if (tz_offset_at(i, utc) == d) return i;
    return utc_i;
}
static void time_run(void);
static void time_from_internet(void)
{
    s_msg_title = IC_CLOCK " DATE/TIME";
    time_run();
    s_msg_title = IC_UPDATE " UPDATE";
}
static void time_run(void)
{
    wifi_creds_load();
    wifi_txt_import();
    net_ui_begin();
    if (!s_wifi_ssid[0]) wifi_setup();
    if (!s_wifi_ssid[0]) { net_ui_end(); return; }
    connecting_screen(s_wifi_ssid, time_screen);
    const char *why = NULL;
    if (wifi_connect(s_wifi_ssid, s_wifi_pass, 15000, &why) != ESP_OK) {
        wifi_off();
        const char *const F[2] = { "COULD NOT CONNECT:", why };
        time_screen(F, 2, "B:WI-FI C:BACK");
        if (time_wait_keys(6) & 2) wifi_setup();
        net_ui_end();
        return;
    }
    const char *const G[1] = { "GETTING THE TIME..." };
    time_screen(G, 1, "");
    time_t utc = 0;
    esp_err_t e = net_get_utc(&utc, 12000);
    wifi_off();
    sys_time_from_rtc();                           /* the ESP32 clock back to local time (file dates) */
    if (e != ESP_OK) {
        const char *const N[3] = { "THE TIME SERVERS DID", "NOT ANSWER.", "PLEASE TRY AGAIN LATER." };
        time_screen(N, 3, "C:BACK");
        time_wait_keys(6);
        net_ui_end();
        return;
    }
    int64_t got = esp_timer_get_time();
    int z = s_tz;
    if (z < 0) {                                   /* first time: confirm the zone (guessed from the clock) */
        z = zone_pick(IC_ZONE " YOUR TIME ZONE?", tz_guess(utc), utc);
        if (z < 0) { net_ui_end(); return; }
    }
    utc += (time_t)((esp_timer_get_time() - got) / 1000000);   /* time spent choosing */
    int off = tz_offset_at(z, utc);
    rtc_time_t t;
    rtc_from_secs((uint32_t)(utc + (time_t)off * 60), &t);
    bool ok = rtc_write(&t);
    if (ok) { s_tz = z; tz_save(); sys_time_from_rtc(); }
    ESP_LOGI(TAG, "Time from the internet: %04d-%02d-%02d %02d:%02d:%02d, zone %s (%s)", t.year, t.month, t.day,
             t.hour, t.minute, t.second, TZONES[z].name, ok ? "set" : "FAILED");
    char l1[32], l2[32];
    snprintf(l1, sizeof l1, "%04d-%02d-%02d  %d:%02d %s", t.year, t.month, t.day, (t.hour + 11) % 12 + 1, t.minute,
             t.hour >= 12 ? "PM" : "AM");
    snprintf(l2, sizeof l2, "ZONE: %s", TZONES[z].name);
    const char *const R[3] = { ok ? "TIME SET:" : "COULD NOT SET THE CLOCK.", l1, l2 };
    time_screen(R, 3, "B:OK");
    time_wait_keys(6);
    net_ui_end();
}
/* WI-FI TRANSFER (owner 2026-11-24): the SD card's files in a web browser on the same Wi-Fi (main/file_server.c).
 * The screen shows a QR code of the link (with a new access code), the address and code to type instead, and what
 * the browser is doing. C stops. */
static void fit_small(char *t, int px)               /* shorten with ".." to fit px in the small font */
{
    while (strlen(t) > 3 && vlcd_text_small(0, 0, t, 2, true) > px) {
        size_t l = strlen(t); t[l - 3] = '.'; t[l - 2] = '.'; t[l - 1] = 0;
    }
}
static void xfer_screen(const char *ip, const char *code, const uint8_t *qr, const char *l1, const char *l2, const char *hint)
{
    vlcd_clear(0);
    vlcd_title(IC_WIFI " WI-FI TRANSFER");
    if (qr) {
        const int n = qrcodegen_getSize(qr), sc = 2, x0 = 8, y0 = 37;
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                if (qrcodegen_getModule(qr, c, r)) vlcd_rect(x0 + c * sc, y0 + r * sc, sc, sc, 2);
    }
    const int x = 78;
    int y = 38;
    vlcd_text_small(x, y, "SCAN WITH", 2, false); y += 8;
    vlcd_text_small(x, y, "YOUR PHONE,", 2, false); y += 8;
    vlcd_text_small(x, y, "OR OPEN IN A", 2, false); y += 8;
    vlcd_text_small(x, y, "BROWSER:", 2, false); y += 12;
    vlcd_text_small(x, y, ip, 2, false); y += 12;
    vlcd_text_small(x, y, "CODE", 1, false); y += 8;
    vlcd_text_small(x, y, code, 2, false);
    vlcd_rect(6, 106, VL - 12, 24, 1);
    vlcd_text_small_center(110, l1, 2);
    vlcd_text_small_center(120, l2, 2);
    vlcd_text_center(134, hint, 1);
    vlcd_clock();
    vlcd_battery();
    vlcd_present();
}
static void xfer_run(void);
static void wifi_transfer_mode(void)
{
    if (!s_sd_ok) {
        const char *const N[2] = { "NO SD CARD TO SHOW.", "PUT IN A FAT32 CARD FIRST." };
        update_screen(N, 2, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }
    wifi_creds_load();
    wifi_txt_import();
    s_msg_title = IC_WIFI " WI-FI TRANSFER";
    xfer_run();
    s_msg_title = IC_UPDATE " UPDATE";
}
static void xfer_run(void)
{
    net_ui_begin();
    if (!s_wifi_ssid[0]) wifi_setup();
    if (!s_wifi_ssid[0]) { net_ui_end(); return; }
    connecting_screen(s_wifi_ssid, update_screen);
    const char *why = NULL;
    char ip[20];
    if (wifi_connect(s_wifi_ssid, s_wifi_pass, 15000, &why) != ESP_OK || !wifi_ip(ip, sizeof ip)) {
        wifi_off();
        const char *const F[3] = { "COULD NOT CONNECT:", why ? why : "NO ADDRESS", "B: CHOOSE ANOTHER WI-FI" };
        update_screen(F, 3, "B:WI-FI C:BACK");
        uint8_t d;
        while (!((d = keys_poll()) & 6)) vTaskDelay(pdMS_TO_TICKS(10));
        if (d & 2) wifi_setup();
        net_ui_end();
        return;
    }
    /* Lend the emulator's memory only now: the first Wi-Fi connection after start-up makes things Wi-Fi keeps for
     * good (radio and security locks, a semaphore for this task - found with heap tracing), and they must not land
     * inside the lent space or it cannot come back in one piece. */
    emu_lend();
    char code[8], url[64];
    snprintf(code, sizeof code, "%06lu", (unsigned long)(esp_random() % 1000000UL));
    snprintf(url, sizeof url, "http://%s/?k=%s", ip, code);
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(4)], tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(4)];
    const bool qok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 4, qrcodegen_Mask_AUTO, true);
    if (file_server_start(code) != ESP_OK) {
        wifi_off();
        const char *const E[2] = { "THE FILE SERVER DID NOT", "START. PLEASE TRY AGAIN." };
        update_screen(E, 2, "C:BACK");
        while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
        net_ui_end();
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi transfer on");
    char last[96] = "", l1[40], l2[56];
    int64_t check_t = 0;
    bool lost = false;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        s_ui_poke = now;                              /* no menu sleep while the transfer screen is open */
        if (now - check_t > 2000000) {                /* still on the Wi-Fi? */
            check_t = now;
            char ip2[20];
            lost = !wifi_ip(ip2, sizeof ip2);
        }
        fs_status_t st;
        file_server_status(&st);
        snprintf(l2, sizeof l2, "%s", st.name);
        fit_small(l2, 144);
        if (lost) { snprintf(l1, sizeof l1, "WI-FI CONNECTION LOST."); snprintf(l2, sizeof l2, "C, THEN OPEN THIS AGAIN."); }
        else if (st.bad_codes >= 20) { snprintf(l1, sizeof l1, "LOCKED: TOO MANY WRONG"); snprintf(l2, sizeof l2, "CODES. C, THEN TRY AGAIN."); }
        else if (st.busy && st.total > 0)
            snprintf(l1, sizeof l1, "%s %ld%%", st.what, (long)((long long)st.done * 100 / st.total));
        else if (st.what[0]) snprintf(l1, sizeof l1, "%s:", st.what);
        else if (st.last_us) { snprintf(l1, sizeof l1, "BROWSER CONNECTED."); snprintf(l2, sizeof l2, "STAY ON THIS SCREEN."); }
        else { snprintf(l1, sizeof l1, "READY. USE A DEVICE ON"); snprintf(l2, sizeof l2, "THE SAME WI-FI."); }
        char sig[96];
        snprintf(sig, sizeof sig, "%s|%s", l1, l2);
        if (strcmp(sig, last)) { snprintf(last, sizeof last, "%s", sig); xfer_screen(ip, code, qok ? qr : NULL, l1, l2, "C:STOP"); }
        if (keys_poll() & 4) {
            if (!st.busy) break;
            const char *const Q[2] = { "A FILE IS STILL BEING", "SENT. STOP ANYWAY?" };
            update_screen(Q, 2, "B:STOP C:NO");
            uint8_t d;
            while (!((d = keys_poll()) & 6)) vTaskDelay(pdMS_TO_TICKS(10));
            if (d & 2) break;
            last[0] = 0;                              /* redraw */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    file_server_stop();
    memset(code, 0, sizeof code);
    ESP_LOGI(TAG, "Wi-Fi transfer off");
    net_ui_end();
}

/* FILE TRANSFER (owner 2026-11-24): the two ways to reach the SD card from another device. */
static void file_transfer_menu(void)
{
    int sel = 0;
    for (;;) {
        const char *const items[2] = { IC_USB " USB DRIVE", IC_WIFI " WI-FI TRANSFER" };
        int r = run_list(IC_XFER " FILE TRANSFER", items, 2, sel);
        if (r < 0) return;
        sel = r;
        if (r == 0) usb_drive_mode();
        else wifi_transfer_mode();
    }
}

static void zone_menu(void)                        /* only remembers the zone; the clock is not touched */
{
    uint32_t local;
    time_t utc = board_local_secs(&local) ? (time_t)local - (time_t)(s_tz >= 0 ? tz_offset_now(s_tz) : 0) * 60 : time(NULL);
    int z = zone_pick(IC_ZONE " TIME ZONE", s_tz >= 0 ? s_tz : tz_guess(utc), utc);
    if (z < 0 || z == s_tz) return;
    ESP_LOGI(TAG, "Time zone: %s (was %s)", TZONES[z].name, s_tz >= 0 ? TZONES[s_tz].name : "not set");
    s_tz = z;
    tz_save();
}
static void datetime_menu(void)
{
    int sel = 0;
    while (1) {
        char hand[24], net[24], z1[24], zone[48];
        list_item(hand, sizeof hand, IC_CLOCK " SET BY HAND", ">");
        list_item(net, sizeof net, IC_WIFI " SET FROM INTERNET", "");
        list_item(z1, sizeof z1, IC_ZONE " TIME ZONE", s_tz < 0 ? "NOT SET >" : ">");
        if (s_tz >= 0) {
            char o[12];
            tz_offset_text(tz_offset_now(s_tz), o, sizeof o);
            if (strlen(TZONES[s_tz].name) + 1 + strlen(o) <= 16)   /* the second line holds 16 characters */
                snprintf(zone, sizeof zone, "%s\n%s %s", z1, TZONES[s_tz].name, o);
            else snprintf(zone, sizeof zone, "%s\n%s", z1, TZONES[s_tz].name);
        } else snprintf(zone, sizeof zone, "%s", z1);
        const char *const items[3] = { hand, net, zone };
        int r = run_list(IC_CLOCK " DATE/TIME", items, 3, sel);
        if (r < 0) return;
        sel = r;
        if (r == 0) edit_datetime();
        else if (r == 1) time_from_internet();
        else zone_menu();
    }
}

/* SETTINGS > ABOUT (2026-11-23). Required by the additional terms in NOTICE.md: the credits and the source link
 * must stay in every version, and a modified version must say so here. */
static void about_screen(void)
{
    /* three pages (owner 2026-11-23): 1 free + link, 2 QR code of the link, 3 credits. A or C go to the next page;
     * C on the last page leaves, so nothing is skipped; B goes back a page. */
    char title[32];
    snprintf(title, sizeof title, IC_ABOUT " TAMANEST %.10s", esp_app_get_description()->version);
    static const char *const P1[] = { "TAMANEST BY WAVIEGOTCHI", "FREE SOFTWARE  -  GPL-3.0", "", "GET IT AND ITS SOURCE AT",
                                      "GITHUB.COM/WAVIEGOTCHI/", "TAMANEST", "",
                                      "NEVER PAY FOR TAMANEST,", "YOU CAN GET IT FREE THERE." };
    static const char *const P3[] = { "EMULATOR BASED ON", "TAMAEMU BY MARAGOTCHI", "AND PARAMIX STUDIO", "BY MAMEISE",
                                      "FONTS: PRESS START 2P,", "SILKSCREEN (OFL)", "", "NOT AFFILIATED WITH BANDAI" };
    int pg = 0;
    while (1) {
        vlcd_clear(0);
        vlcd_title(title);
        if (pg == 1) {                                 /* QR code, 2 px per module, light quiet zone */
            const int sc = 2, n = ABOUT_QR_N, w = n * sc, x0 = (VL - w) / 2, y0 = 40;
            vlcd_rect(x0 - 6, y0 - 4, w + 12, w + 8, 0);
            vlcd_frame(x0 - 7, y0 - 5, w + 14, w + 10, 1);
            for (int r = 0; r < n; r++)
                for (int c = 0; c < n; c++)
                    if (ABOUT_QR[r] & (0x80000000u >> c)) vlcd_rect(x0 + c * sc, y0 + r * sc, sc, sc, 2);
            vlcd_text_small_center(y0 + w + 9, "SCAN FOR THE DOWNLOAD", 2);
            vlcd_text_small_center(y0 + w + 18, "AND THE SOURCE CODE", 2);
        } else {
            const char *const *L = pg == 0 ? P1 : P3;
            int n = pg == 0 ? (int)(sizeof P1 / sizeof P1[0]) : (int)(sizeof P3 / sizeof P3[0]);
            int sp = n > 8 ? 8 : 9;                    /* 9 lines (page 1 with the author) sit 8 px apart */
            int h = 10 + n * sp, y0 = 38 + (90 - h) / 2;
            vlcd_rect(6, y0, VL - 12, h, 1);
            for (int i = 0; i < n; i++) vlcd_text_small_center(y0 + 5 + i * sp, L[i], 2);
        }
        for (int d = 0; d < 3; d++) {                  /* page dots: which page, how many */
            int x = VL / 2 - 13 + d * 10;
            if (d == pg) vlcd_rect(x, 127, 6, 6, 2);
            else { vlcd_frame(x, 127, 6, 6, 1); }
        }
        char pn[16]; snprintf(pn, sizeof pn, "PAGE %d/3", pg + 1);
        vlcd_text_small(VL - 6 - vlcd_text_small(0, 0, pn, 1, true), 127, pn, 1, false);
        if (pg < 2) vlcd_text_center(138, "A,C:NEXT PAGE", 1);
        else {                                         /* "A:PAGE 1  C:DONE": the font's 1 has a wide blank left, so it
                                                        * is drawn 3 px closer to look like a single space (owner) */
            const int x = (VL - 16 * 8) / 2;
            vlcd_text(x, 138, "A:PAGE", 1); vlcd_text(x + 7 * 8 - 3, 138, "1", 1); vlcd_text(x + 10 * 8, 138, "C:DONE", 1);
        }
        vlcd_clock();
        vlcd_battery();
        vlcd_present();
        uint8_t k = 0;
        while (!(k = keys_poll())) vTaskDelay(pdMS_TO_TICKS(10));
        if (k & 4) { if (pg == 2) return; pg++; }
        else if (k & 1) pg = (pg + 1) % 3;
        else if (k & 2) { if (pg > 0) pg--; }
    }
}

/* SETTINGS > FAST LOAD (2026-11-22): the 8 MB games with where they load from; B pins / unpins. */
/* How many 64 KB store blocks a ROM needs (its non-empty blocks): from the store, the remembered empty-block map, or
 * by reading the file once (remembered in NVS romstore "x<id>"). -1 = unknown (no file). */
static int rs_rom_need(const char *file)
{
    uint32_t id = rs_fnv((const uint8_t *)file, strlen(file));
    uint8_t m[128];
    if (rs_is_stored(id) && rs_get_map(id, m)) { int n = 0; for (int b = 0; b < 128; b++) n += m[b] != 0xFF; return n; }
    char key[12]; snprintf(key, sizeof key, "x%08lx", (unsigned long)id);
    uint8_t emp[16]; size_t l = 16; nvs_handle_t nvs; bool have = false;
    if (nvs_open("romstore", NVS_READONLY, &nvs) == ESP_OK) { have = nvs_get_blob(nvs, key, emp, &l) == ESP_OK && l == 16; nvs_close(nvs); }
    if (!have) {                                   /* read the file once: which 64 KB blocks are empty */
        char path[160]; snprintf(path, sizeof path, "/sdcard/roms/%s", file);
        FILE *f = fopen(path, "rb");
        uint8_t *buf = heap_caps_malloc(RS_BLK, MALLOC_CAP_SPIRAM);
        if (!f || !buf) { if (f) fclose(f); free(buf); return -1; }
        memset(emp, 0, sizeof emp);
        for (int b = 0; b < 128; b++) {
            size_t n = fread(buf, 1, RS_BLK, f);
            bool e = true;
            for (size_t i = 0; i + 4 <= n; i += 4) if (*(const uint32_t *)(buf + i) != 0xFFFFFFFFu) { e = false; break; }
            if (e) emp[b >> 3] |= (uint8_t)(1 << (b & 7));
        }
        fclose(f); free(buf);
        if (nvs_open("romstore", NVS_READWRITE, &nvs) == ESP_OK) { nvs_set_blob(nvs, key, emp, 16); nvs_commit(nvs); nvs_close(nvs); }
    }
    int n = 0;
    for (int b = 0; b < 128; b++) n += !(emp[b >> 3] & (1 << (b & 7)));
    return n;
}
static void fast_msg(const char *l1, const char *l2, const char *l3, bool wait)
{
    vlcd_clear(0);
    vlcd_title(IC_FAST " FAST LOAD");
    vlcd_rect(6, 50, VL - 12, 40, 1);
    vlcd_text_small_center(56, l1, 2);
    if (l2) vlcd_text_small_center(67, l2, 2);
    if (l3) vlcd_text_small_center(76, l3, 2);
    if (wait) vlcd_text_center(134, "C:BACK", 1);
    vlcd_clock(); vlcd_battery(); vlcd_present();
    if (wait) while (!(keys_poll() & 6)) vTaskDelay(pdMS_TO_TICKS(10));
}
/* SETTINGS > FAST LOAD (2026-11-22; two-line rows and the room check 2026-11-23): the 8 MB games with where they load
 * from; B pins / unpins. Pinning is refused when all pinned games together would not fit in the fast store. */
static void fast_load_menu(void)
{
    int sel = 0;
    while (1) {
        static char rows[MAX_ROMS][48];
        static int idx[MAX_ROMS];
        const char *items[MAX_ROMS];
        int n = 0;
        for (int i = 0; i < s_rom_count && n < MAX_ROMS; i++) {
            if (rom_info(s_rom_file[i])->family != 2) continue;   /* 4 MB games always load quickly */
            uint32_t id = rs_fnv((const uint8_t *)s_rom_file[i], strlen(s_rom_file[i]));
            bool pin = rs_pinned(id), fast = rs_is_stored(id);
            const char *st = pin ? (fast ? "PINNED, FAST" : "PINNED, NEXT LOAD") : fast ? "FAST" : "FROM SD CARD";
            snprintf(rows[n], sizeof rows[n], "%.18s\n%s", s_rom_name[i], st);
            idx[n] = i; items[n] = rows[n]; n++;
        }
        if (n == 0) { show_error("NO 8 MB GAMES", "ON THE SD CARD"); return; }
        s_list_rows = 4;
        int r = run_list(IC_FAST " FAST LOAD", items, n, sel);
        s_list_rows = 0;
        if (r < 0) return;
        sel = r;
        const char *file = s_rom_file[idx[r]];
        uint32_t id = rs_fnv((const uint8_t *)file, strlen(file));
        if (rs_pinned(id)) { rs_set_pin(id, false); continue; }
        /* room check: all pinned games + this one must fit */
        fast_msg("CHECKING THE ROOM...", NULL, NULL, false);
        int need = rs_rom_need(file), total = need, pins = 1;
        for (int i = 0; i < s_rom_count && need >= 0; i++) {
            uint32_t pid = rs_fnv((const uint8_t *)s_rom_file[i], strlen(s_rom_file[i]));
            if (pid == id || !rs_pinned(pid)) continue;
            int k = rs_rom_need(s_rom_file[i]);
            if (k > 0) { total += k; pins++; }
        }
        bool any_pin = false; for (int k = 0; k < RS_MAX_PINS; k++) if (s_rs_pins[k]) any_pin = true;
        int free_pins = 0; for (int k = 0; k < RS_MAX_PINS; k++) if (!s_rs_pins[k]) free_pins++;
        (void)any_pin;
        ESP_LOGI(TAG, "Pin %s: needs %d blocks, all pinned %d of %lu", file, need, total, (unsigned long)s_rs_nblk);
        if (need < 0 || total > (int)s_rs_nblk || free_pins == 0) {
            fast_msg("NOT ENOUGH ROOM.", "TO PIN THIS GAME, UNPIN", "ANOTHER GAME FIRST.", true);
            continue;
        }
        rs_set_pin(id, true);
    }
}

/* SETTINGS: brightness, sound, SCREEN >, DATE/TIME >, ROM ORDER. A next, B change/open, C back. */
static void settings_menu(void)
{
    int sel = 0;
    while (1) {
        char b[24], so[24], sc[24], ex[24], dt[24], ro[24], v[12];
        snprintf(v, sizeof v, "%d%%", s_brightness); list_item(b, sizeof b, IC_SUN " BRIGHTNESS", v);
        if (s_sound) snprintf(v, sizeof v, "%d%%", s_sound); else snprintf(v, sizeof v, "OFF");
        list_item(so, sizeof so, IC_SOUND " SOUND", v);
        list_item(sc, sizeof sc, IC_SCREEN " SCREEN", ">");
        (void)ex;   /* EXTRAS only in the quick menu: none of it matters outside a game (owner 2026-11-22) */
        list_item(dt, sizeof dt, IC_CLOCK " DATE/TIME", ">");
        static const char *const RO[3] = { "OLDEST", "NEWEST", "A-Z" };
        list_item(ro, sizeof ro, IC_ORDER " ROM ORDER", RO[s_rom_order]);
        char fl[24];
        list_item(fl, sizeof fl, IC_FAST " FAST LOAD", ">");
        const char *const items[] = { b, so, sc, dt, ro, fl, IC_UPDATE " UPDATE" };
        int r = run_list("\x01 SETTINGS", items, 7, sel);
        if (r < 0) return;
        sel = r;
        if (r == 0) brightness_edit();               /* level editor: A lower, B higher */
        else if (r == 1) sound_edit();
        else if (r == 2) screen_menu(false);
        else if (r == 3) datetime_menu();
        else if (r == 4) rom_order_step();
        else if (r == 5) { if (!s_rom_count) scan_roms(); fast_load_menu(); }
        else update_menu();
    }
}

static void ui_task(void *arg)
{
    keys_init();
    brightness_load_apply();
    tz_load();
    startup_time_check();
    sys_time_from_rtc();                           /* again now that everything is up (the boot-time try may be too early) */
    {   /* this version reached the menu: confirm it (a fresh update starts on trial, see update_menu) */
        esp_ota_img_states_t st;
        const esp_partition_t *run = esp_ota_get_running_partition();
        if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Update confirmed: running %s", esp_app_get_description()->version);
        }
        ESP_LOGI(TAG, "TamaNest %s (app slot %s)", esp_app_get_description()->version, run ? run->label : "?");
    }
#if TL_WIFITEST
    {   /* does Wi-Fi fit in memory, and is the menu screen still possible afterwards? */
        ESP_LOGI(TAG, "WIFITEST before: internal free %u, largest DMA %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        static wifi_ap_t aps[12];
        int64_t t0 = esp_timer_get_time();
        int n = wifi_scan(aps, 12);
        ESP_LOGI(TAG, "WIFITEST scan: %d networks in %lld ms; on: internal free %u, largest DMA %u", n, (esp_timer_get_time() - t0) / 1000,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        for (int i = 0; i < n; i++) ESP_LOGI(TAG, "WIFITEST  %d dBm %s", aps[i].rssi, aps[i].secure ? "(secured)" : "(open)");   /* names not logged */
        wifi_off();
        ESP_LOGI(TAG, "WIFITEST after: internal free %u, largest DMA %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
#endif
    int cat_sel = 0, rom_sel = 0;
    while (1) {
        /* Top level: categories. Only colour Tamagotchis for now; more (like the tamagotchi.vn
         * launcher's VINTAGE TMGC, DIGIMON...) can be added here later. */
        static bool off_armed;
        /* system actions under a line (owner 2026-11-22): USB DRIVE (moved from SETTINGS) and POWER OFF */
        static char sub[3][24];                        /* items that open a list: ">" at the right edge */
        list_item(sub[0], sizeof sub[0], "\x02 COLOUR TMGC", ">");
        list_item(sub[1], sizeof sub[1], "\x01 SETTINGS", ">");
        list_item(sub[2], sizeof sub[2], IC_XFER " FILE TRANSFER", ">");
        const char *const cats[] = { sub[0], sub[1], sub[2],
                                     off_armed ? IC_POWER " OFF: B AGAIN" : IC_POWER " POWER OFF", IC_ABOUT " ABOUT & CREDITS" };
        s_list_sep = 2;
        int r = run_list_refresh(IC_HOME " TAMANEST", cats, 5, &cat_sel, 1000);
        s_list_sep = -1;
        if (r == -2) { if (cat_sel != 3) off_armed = false; continue; }
        if (r < 0) { cat_sel = 0; off_armed = false; continue; }
        cat_sel = r;
        if (r == 2) { off_armed = false; file_transfer_menu(); continue; }   /* USB DRIVE + WI-FI TRANSFER (owner 2026-11-24) */
        if (r == 4) { off_armed = false; about_screen(); continue; }   /* ABOUT: home menu, last (owner 2026-11-23) */
        if (r == 3) {                                  /* POWER OFF (owner 2026-11-22): like holding the PWR key */
            if (!off_armed) { off_armed = true; continue; }
            ESP_LOGI(TAG, "Power off from the menu");
            bsp_display_brightness_set(0);
            vTaskDelay(pdMS_TO_TICKS(200));            /* SD writes are all closed in the menu; the screen goes dark first */
            battery_power_off();
            vTaskDelay(pdMS_TO_TICKS(2000));
            off_armed = false;                         /* (only reached if the power chip did not answer) */
            bsp_display_brightness_set(s_brightness);
            continue;
        }
        off_armed = false;
        if (cat_sel == 1) { settings_menu(); continue; }

        if (!s_sd_ok && !bsp_sdcard) {                /* a card put in while TamaNest runs: try again (2026-11-23) */
            s_sd_err = bsp_sdcard_mount();
            if (s_sd_err == ESP_OK) { s_sd_ok = true; sd_organise(); ESP_LOGI(TAG, "SD card found and mounted"); }
        }
        if (!s_sd_ok) {                               /* e.g. a new 64 GB+ card: those come formatted exFAT */
            static const char *const NO[5] = { "NO SD CARD FOUND.", "PUT A CARD (8-32 GB,", "FAT32) IN THE SLOT,", "THEN OPEN THIS AGAIN.", "" };
            static const char *const BADFMT[5] = { "THE CARD CAN NOT BE READ.", "FORMAT IT AS FAT32", "(MS-DOS FAT): USB DRIVE", "SHOWS IT ON A COMPUTER.", "8-32 GB CARDS WORK BEST." };
            const char *const *L = s_sd_err == ESP_FAIL ? BADFMT : NO;
            vlcd_clear(0);
            vlcd_title("\x02 SD CARD");
            vlcd_rect(6, 44, VL - 12, 69, 1);
            for (int i = 0; i < 5; i++) vlcd_text_small_center(51 + i * 11, L[i], 2);
            vlcd_text_center(134, "C:BACK", 1);
            vlcd_clock(); vlcd_present();
            while (!(keys_poll() & 4)) vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        scan_roms();                                  /* read the SD card's ROM list when the list opens */
        while (1) {
            /* Top list: a ROM, or a family row when 2+ ROMs share a base model (2026-11-22, owner: e.g. Plus Color
             * + Hexagontchi). The family row sits where its first member would be in the chosen order. */
            const char *items[MAX_ROMS], *years[MAX_ROMS];
            int kind[MAX_ROMS], n = 0;                 /* kind >= 0: ROM index; < 0: family -(base model + 1) */
            static char fam[MAX_ROMS][24];
            bool used[MAX_ROMS] = { 0 };
            for (int i = 0; i < s_rom_count; i++) {
                if (used[i]) continue;
                RomModel b = rom_base(s_rom_file[i]);
                int cnt = 0;
                for (int j = i; j < s_rom_count && b != M_UNKNOWN; j++) if (!used[j] && rom_base(s_rom_file[j]) == b) cnt++;
                if (cnt >= 2) {
                    for (int j = i; j < s_rom_count; j++) if (rom_base(s_rom_file[j]) == b) used[j] = true;
                    char cn[8]; snprintf(cn, sizeof cn, "%d >", cnt);   /* "PLUS COLOR      2 >" (owner's pick) */
                    list_item(fam[n], sizeof fam[n], MODEL_INFO[b].name, cn);
                    items[n] = fam[n]; years[n] = ""; kind[n] = -(int)b - 1; n++;
                } else { items[n] = s_rom_name[i]; years[n] = rom_year(s_rom_file[i]); kind[n] = i; n++; }
            }
            if (rom_sel >= n) rom_sel = 0;
            s_list_right = years;
            int r = run_list("\x02 COLOUR TMGC", items, n, rom_sel);
            s_list_right = NULL;
            if (r < 0) break;                       /* C: back to categories */
            rom_sel = r;
            if (kind[r] >= 0) {
                rom_speed_notice(s_rom_file[kind[r]]);
                if (game_start(s_rom_file[kind[r]])) { game_loop(); if (s_low_exit) low_battery_screen(); game_broken_screen(); }
                continue;
            }
            RomModel b = (RomModel)(-kind[r] - 1);   /* family sub-list: its ROMs in the same order */
            char title[24]; snprintf(title, sizeof title, "\x02 %s", MODEL_INFO[b].name);
            int sub_sel = 0;
            while (1) {
                const char *si[MAX_ROMS], *sy[MAX_ROMS]; int sk[MAX_ROMS], sn = 0;
                for (int j = 0; j < s_rom_count; j++)
                    if (rom_base(s_rom_file[j]) == b) { si[sn] = s_rom_name[j]; sy[sn] = rom_year(s_rom_file[j]); sk[sn++] = j; }
                if (sub_sel >= sn) sub_sel = 0;
                s_list_right = sy;
                int q = run_list(title, si, sn, sub_sel);
                s_list_right = NULL;
                if (q < 0) break;                   /* C: back to the top list */
                sub_sel = q;
                rom_speed_notice(s_rom_file[sk[q]]);
                if (game_start(s_rom_file[sk[q]])) { game_loop(); if (s_low_exit) low_battery_screen(); game_broken_screen(); }
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */

#if TL_BENCH
#include "esp_cpu.h"
#include "xtensa_perfmon_access.h"
#include "xtensa/xt_perf_consts.h"
static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
static uint64_t s_bench_cycles = BENCH_CYCLES;
static uint32_t s_blk_hits[128];                   /* TEMP 2026-11-21: guest PC samples per 64 KB ROM block */
static void blk_pc_hook(uint32_t pc)
{
    uint32_t o = pc - s_emu->dev.rom_base;
    if (o < s_emu->dev.rom_size) s_blk_hits[o >> 16]++;
}
static int s_bench_psram, s_bench_dump;
static void bench_rom(int idx, int hle_mode)
{
    char path[160];
    snprintf(path, sizeof path, "/sdcard/roms/%s", s_rom_file[idx]);
    rom_display_name(s_rom_file[idx], s_loading_name, sizeof s_loading_name);
    s_emu = s_emu_buf;
    memset(s_emu, 0, sizeof(Emu));
    rom_profile(s_rom_file[idx], &s_emu->dev);
    bool mm;
    uint8_t *rom = load_rom_storage(path, s_rom_file[idx], s_emu->dev.rom_size, &mm);
    if (!rom) { ESP_LOGW(TAG, "BENCH: ROM load failed"); s_emu = NULL; return; }
    memset(s_gram, 0, GRAM_W * GRAM_H * sizeof(uint16_t));
    tc_init(s_emu, rom, s_gram, s_ovl_pool, OVL_SLOTS);
    rom_apply_map(s_emu);
    static uint8_t *copies[128];
    if (s_bench_psram) {                           /* copy the 8 most used code blocks to PSRAM */
        bool used[128] = { 0 };
        for (int n = 0; n < 8; n++) {
            int best = -1;
            for (int b = 0; b < 128; b++) if (!used[b] && s_blk_hits[b] && (best < 0 || s_blk_hits[b] > s_blk_hits[best])) best = b;
            if (best < 0 || !s_emu->rom_src[best]) break;
            used[best] = true;
            if (!copies[best]) copies[best] = heap_caps_malloc(65536, MALLOC_CAP_SPIRAM);
            if (!copies[best]) break;
            memcpy(copies[best], s_emu->rom_src[best], 65536);
            ESP_LOGI(TAG, "BENCHBLK block %d (%lu samples) -> PSRAM", best, (unsigned long)s_blk_hits[best]);
            s_emu->rom_src[best] = copies[best]; s_emu->rom_blk[best] = copies[best];
        }
        for (uint32_t b = 0; b < 128; b++) tc_blk_update(s_emu, b);
        s_emu->pc = mem_read32(s_emu, s_emu->ttbr);
    }
    memset(&tc_prof, 0, sizeof tc_prof);
    int saved_hle = tc_hle_mode;
    tc_hle_mode = hle_mode;
    memset(&tc_hle, 0, sizeof tc_hle);
    { extern uint32_t tc_rom_reads; tc_rom_reads = 0; }
    uint64_t host = 0;
    while (s_emu->cycles < s_bench_cycles && !s_emu->stopped) {
        uint32_t h0 = esp_cpu_get_cycle_count();
        tc_run(s_emu, s_emu->cycles + 20000);
        host += esp_cpu_get_cycle_count() - h0;
        if ((s_emu->cycles / 20000) % 50 == 0) vTaskDelay(1);   /* let the idle task run (watchdog) */
    }
    uint64_t awake = s_emu->cycles - tc_prof.halt_cyc - tc_prof.idle_cyc;
    if (s_bench_dump) {                            /* the engine's RAM code, for disassembly (2026-11-21) */
        printf("DUMPROM %s\n", s_rom_file[idx]);
        for (uint32_t a = 0x300; a < 0x2000; a += 32) {
            printf("CODE %06lx:", (unsigned long)a);
            for (uint32_t j = 0; j < 32; j += 2) printf(" %04x", mem_read16(s_emu, a + j));
            printf("\n");
            if ((a & 0xff) == 0xe0) vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (tc_hle.host2)
        ESP_LOGI(TAG, "BENCHGP %s: get-pixel %.0f%% of ESP time, %.0f%% of executed guest cycles, %.1f ESP/guest cycle | rest %.1f | overall %.1f",
                 s_rom_file[idx], 100.0 * (double)tc_hle.host2 / (double)host, 100.0 * (double)tc_hle.guest2 / (double)awake,
                 (double)tc_hle.host2 / (double)(tc_hle.guest2 ? tc_hle.guest2 : 1),
                 (double)(host - tc_hle.host2) / (double)(awake > tc_hle.guest2 ? awake - tc_hle.guest2 : 1), (double)host / (double)awake);
    { extern uint32_t tc_rom_reads; ESP_LOGI(TAG, "BENCHROM %s: %.1f ROM reads per 1000 executed guest cycles", s_rom_file[idx], 1000.0 * tc_rom_reads / awake); tc_rom_reads = 0; }
    ESP_LOGI(TAG, "BENCHSLOW %s per 1000 executed guest cycles: mem_rd %.1f mem_wr %.1f io_rd %.1f io_wr %.1f fetchwin %.2f tick %.2f exec_calls %.2f lcd_writes %.1f | tick host %llu cyc",
             s_rom_file[idx], 1000.0 * tc_prof.mem_rd / awake, 1000.0 * tc_prof.mem_wr / awake, 1000.0 * tc_prof.io_rd / awake,
             1000.0 * tc_prof.io_wr / awake, 1000.0 * tc_prof.fetchwin / awake, 1000.0 * tc_prof.tick / awake,
             1000.0 * tc_prof.exec_calls / awake, 1000.0 * (double)s_emu->lcd_writes / awake, (unsigned long long)tc_prof.tick_host_cyc);
    /* Hardware counters over a further 1M cycles: what the ESP spends its instructions on */
    {
        static const struct { const char *n; int sel, mask; } ev[] = {
            { "instrs", XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL }, { "cycles", XTPERF_CNT_CYCLES, 1 },
            { "jx", XTPERF_CNT_INSN, XTPERF_MASK_INSN_JX }, { "j", XTPERF_CNT_INSN, XTPERF_MASK_INSN_J },
            { "br taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_TAKEN }, { "br not", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_NOT_TAKEN },
            { "loads", XTPERF_CNT_D_LOAD_U1, 0xF }, { "stores", XTPERF_CNT_D_STORE_U1, 0xF },
            { "bubbles", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_ALL }, { "calls", XTPERF_CNT_INSN, XTPERF_MASK_INSN_CALLX | XTPERF_MASK_INSN_CALL },
        };
        char line[300]; int n = 0;
        for (int i = 0; i < (int)(sizeof ev / sizeof ev[0]); i += 2) {
            xtensa_perfmon_stop();
            xtensa_perfmon_init(0, ev[i].sel, ev[i].mask, 0, -1);
            xtensa_perfmon_init(1, ev[i + 1].sel, ev[i + 1].mask, 0, -1);
            xtensa_perfmon_reset(0); xtensa_perfmon_reset(1);
            uint64_t c0 = s_emu->cycles, hz0 = tc_prof.halt_cyc + tc_prof.idle_cyc;
            portDISABLE_INTERRUPTS();
            xtensa_perfmon_start();
            tc_run(s_emu, s_emu->cycles + 200000);
            xtensa_perfmon_stop();
            portENABLE_INTERRUPTS();
            double aw = (double)((s_emu->cycles - c0) - (tc_prof.halt_cyc + tc_prof.idle_cyc - hz0));
            if (aw < 1) aw = 1;
            n += snprintf(line + n, sizeof line - n, "%s %.1f, %s %.1f; ", ev[i].n, xtensa_perfmon_value(0) / aw,
                          ev[i + 1].n, xtensa_perfmon_value(1) / aw);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        printf("BENCHMIX %s\n", line);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    uint32_t h = 2166136261u;
    h = fnv(h, s_emu->r, sizeof s_emu->r); h = fnv(h, &s_emu->pc, 4); h = fnv(h, &s_emu->sp, 4);
    h = fnv(h, s_emu->a0ram, sizeof s_emu->a0ram); h = fnv(h, s_emu->ivram, sizeof s_emu->ivram);
    h = fnv(h, s_emu->dstram, sizeof s_emu->dstram); h = fnv(h, s_gram, GRAM_W * GRAM_H * 2);
    ESP_LOGI(TAG, "BENCH %s: %.1f ESP cycles per executed guest cycle, %.1f per guest cycle overall (%.2f s for %llu cycles, %llu executed) | state %08lx pc %06lx%s",
             s_rom_file[idx], awake ? (double)host / awake : 0, (double)host / (double)s_emu->cycles, host / 240e6, (unsigned long long)s_emu->cycles,
             (unsigned long long)awake, (unsigned long)h, (unsigned long)s_emu->pc, s_emu->stopped ? " STOPPED" : "");
    for (int v = 0; v < 2; v++) {
        uint32_t a, have, want;
        int k = tc_hle_sig_debug(s_emu, v, &a, &have, &want);
        printf("BENCHSIG engine2[%d]: %s\n", v, k < 0 ? "match" : "");
        if (k >= 0) printf("BENCHSIG   first difference at %06lx: RAM %04lx, expected %04lx (index %d)\n",
                           (unsigned long)a, (unsigned long)have, (unsigned long)want, k);
    }
    printf("BENCHHLE %s mode %d: calls %lu, native %lu, bailed %lu | verify: results %lu ok / %lu wrong, cycles %lu ok / %lu off (sum diff %ld)\n",
           s_rom_file[idx], hle_mode, (unsigned long)tc_hle.calls, (unsigned long)tc_hle.done, (unsigned long)tc_hle.bail,
           (unsigned long)tc_hle.match, (unsigned long)tc_hle.mismatch, (unsigned long)tc_hle.cyc_match,
           (unsigned long)tc_hle.cyc_mismatch, (long)tc_hle.cyc_diff_sum);
    printf("BENCHHLE off by <=64: %lu (sum %ld, samples %ld %ld %ld %ld %ld %ld %ld %ld), off by more: %lu\n",
           (unsigned long)tc_hle.small_off, (long)tc_hle.small_sum, (long)tc_hle.sample_diff[0], (long)tc_hle.sample_diff[1],
           (long)tc_hle.sample_diff[2], (long)tc_hle.sample_diff[3], (long)tc_hle.sample_diff[4], (long)tc_hle.sample_diff[5],
           (long)tc_hle.sample_diff[6], (long)tc_hle.sample_diff[7], (unsigned long)tc_hle.big_off);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    tc_hle_mode = saved_hle;
    if (s_rom_in_store) { s_rom_in_store = false; rom_ram_free(); } else if (mm) { esp_partition_munmap(s_mmap_handle); } else free(rom);
    s_emu = NULL;
}
/* Self-test of saving: run, save to SD, run on (fingerprint A); fresh emulator, load the file, run the same
 * cycles (fingerprint B). A == B means the save captures everything the game needs. */
static uint32_t state_fp(void)
{
    uint32_t h = 2166136261u;
    h = fnv(h, s_emu->r, sizeof s_emu->r); h = fnv(h, &s_emu->pc, 4); h = fnv(h, &s_emu->sp, 4);
    h = fnv(h, &s_emu->cycles, 8);
    h = fnv(h, s_emu->a0ram, sizeof s_emu->a0ram); h = fnv(h, s_emu->ivram, sizeof s_emu->ivram);
    h = fnv(h, s_emu->dstram, sizeof s_emu->dstram); h = fnv(h, s_emu->ioram, sizeof s_emu->ioram);
    h = fnv(h, s_gram, GRAM_W * GRAM_H * 2); h = fnv(h, s_ovl_pool, SAVE_POOL_BYTES);
    return h;
}
static void run_to(uint64_t c) { while (s_emu->cycles < c && !s_emu->stopped) tc_run(s_emu, s_emu->cycles + 20000); }
static void save_selftest(int idx)
{
    char path[160];
    snprintf(path, sizeof path, "/sdcard/roms/%s", s_rom_file[idx]);
    bool mm;
    s_emu = s_emu_buf; memset(s_emu, 0, sizeof(Emu));
    tc_profile_plus_color(&s_emu->dev);
    uint8_t *rom = load_rom_storage(path, s_rom_file[idx], s_emu->dev.rom_size, &mm);
    if (!rom) { s_emu = NULL; return; }
    int saved_hle = tc_hle_mode; tc_hle_mode = 0;            /* fully deterministic for the comparison */
    memset(s_gram, 0, SAVE_GRAM_BYTES); memset(s_ovl_pool, 0, SAVE_POOL_BYTES);
    tc_init(s_emu, rom, s_gram, s_ovl_pool, OVL_SLOTS);
    run_to(8000000);
    snap_capture();
    bool wrote = save_write_snap("selftest.bin");
    uint64_t c0 = s_emu->cycles;
    run_to(c0 + 2000000);
    uint32_t a = state_fp();

    memset(s_emu, 0, sizeof(Emu)); tc_profile_plus_color(&s_emu->dev);
    memset(s_gram, 0, SAVE_GRAM_BYTES); memset(s_ovl_pool, 0, SAVE_POOL_BYTES);
    tc_init(s_emu, rom, s_gram, s_ovl_pool, OVL_SLOTS);
    bool loaded = save_load("selftest.bin", s_emu);
    run_to(c0 + 2000000);
    uint32_t b = state_fp();
    printf("SAVETEST wrote %d loaded %d | continued %08lx | restored %08lx | %s\n", wrote, loaded,
           (unsigned long)a, (unsigned long)b, (wrote && loaded && a == b) ? "PASS" : "FAIL");
    fflush(stdout);
    unlink("/sdcard/roms/selftest.sav");
    tc_hle_mode = saved_hle;
    free(rom);
    s_emu = NULL;
}

/* Benchmark Plus Color, and whichever 8 MB ROM is already in the flash cache (no slow re-copy). */
static void run_bench(void)
{
    scan_roms();
    /* 2026-11-21: engine v2 (iD L, P's) from power-on, spot checks off (deterministic fingerprint) */
    s_bench_cycles = 60000000ULL;
    tc_hle_spot_on = 0;
    for (int i = 0; i < s_rom_count; i++) {
        if (rom_info(s_rom_file[i])->m == M_4UP) {      /* 2026-11-22: 4U+ engine 3 get-pixel: verify, then A/B */
            ESP_LOGI(TAG, "BENCHCASE 4U+ verify"); bench_rom(i, 2);
            ESP_LOGI(TAG, "BENCHCASE 4U+ native"); bench_rom(i, 1);
            ESP_LOGI(TAG, "BENCHCASE 4U+ interpreted"); bench_rom(i, 0);
        }
        if (rom_info(s_rom_file[i])->m == M_IDL || rom_info(s_rom_file[i])->m == M_PS) {
            extern int tc_idle_skip;
            tc_hle_draw_on = 1; tc_hle_draw2_runs = 0; bench_rom(i, 1);
            ESP_LOGI(TAG, "BENCHDRAW2 on: native loop entries %lu", (unsigned long)tc_hle_draw2_runs);
            tc_hle_draw_on = 0; tc_hle_draw2_runs = 0; bench_rom(i, 1);
            ESP_LOGI(TAG, "BENCHDRAW2 off: native loop entries %lu", (unsigned long)tc_hle_draw2_runs);
            tc_hle_draw_on = 1;
        }
    }
    tc_hle_spot_on = 1;
    s_bench_cycles = BENCH_CYCLES;
    return;
    for (int i = 0; i < s_rom_count; i++) if (rom_is_plus(s_rom_file[i])) {
        /* native draw-window check: the same run with it on and off must give the same state fingerprint */
        tc_hle_spot_on = 0;
        tc_hle_draw_on = 1; tc_hle_draw_runs = 0; bench_rom(i, BENCH_HLE_MODE);
        ESP_LOGI(TAG, "BENCHDRAW on: native draw entries %lu", (unsigned long)tc_hle_draw_runs);
        tc_hle_draw_on = 0; tc_hle_draw_runs = 0; bench_rom(i, BENCH_HLE_MODE);
        ESP_LOGI(TAG, "BENCHDRAW off: native draw entries %lu", (unsigned long)tc_hle_draw_runs);
        tc_hle_draw_on = 1; tc_hle_spot_on = 1;
        break; }
    return;
    char cached[128] = "";
    nvs_handle_t nvs;
    if (nvs_open("tama_cache", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof cached;
        if (nvs_get_str(nvs, "rom_name", cached, &len) != ESP_OK) cached[0] = 0;
        nvs_close(nvs);
    }
    for (int i = 0; i < s_rom_count; i++)
        if (cached[0] && strcmp(s_rom_file[i], cached) == 0) { bench_rom(i, BENCH_HLE_MODE_8MB); break; }
}
#endif

void app_main(void)
{
    /* the audio library logs every speaker wake (info) and a harmless "channel not enabled yet" error when it
     * stops an already stopped I2S channel before starting it */
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    esp_log_level_set("I2S_IF", ESP_LOG_WARN);
    esp_log_level_set("Adev_Codec", ESP_LOG_WARN);
    nvs_flash_init();

    /* Emulator state first, in internal RAM (much faster than PSRAM). Done before the display
     * starts so it gets a clean contiguous block; the display/DMA buffers take what is left. */
    s_emu_buf = heap_caps_calloc(1, sizeof(Emu), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_emu_buf) {
        ESP_LOGW(TAG, "No internal RAM for emulator state, falling back to PSRAM (slow)");
        s_emu_buf = heap_caps_calloc(1, sizeof(Emu), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    ESP_LOGI(TAG, "Emulator state: %u bytes, internal RAM: %s", (unsigned)sizeof(Emu),
             (s_emu_buf && esp_ptr_internal(s_emu_buf)) ? "yes" : "NO");

    /* Same as bsp_display_start(), but the LVGL task is pinned to core 0 so core 1 is free for the emulator. */
    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = { .swap_xy = 1, .mirror_x = 0, .mirror_y = 1 },
    };
    disp_cfg.lv_adapter_cfg.task_core_id = 0;
    s_disp = bsp_display_start_with_config(&disp_cfg);

    pm_init();
    {   /* power saving: the touch screen is not used. Stop LVGL reading it and hold the touch chip in reset. */
        bsp_display_lock(-1);
        lv_indev_t *indev = bsp_display_get_input_dev();
        if (indev) lv_indev_enable(indev, false);
        bsp_display_unlock();
        gpio_set_direction(BSP_LCD_TOUCH_RST, GPIO_MODE_OUTPUT);
        gpio_set_level(BSP_LCD_TOUCH_RST, 0);
        ESP_LOGI(TAG, "Touch controller off (held in reset)");
    }
    {   /* power saving: chips we never use. ES7210 microphone ADC (I2C 0x40): the codec driver's own stop
         * sequence (mics, bias, ADC, clocks powered down). QMI8658 motion sensor (0x6B): sensors stay off and
         * its internal 2 MHz oscillator is stopped (CTRL1 bit 0). Missing chips just do not answer. */
        i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
        i2c_master_dev_handle_t d;
        i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x40, .scl_speed_hz = 100000 };
        int ok_adc = 0, ok_imu = 0;
        if (bus && i2c_master_bus_add_device(bus, &cfg, &d) == ESP_OK) {
            static const uint8_t seq[][2] = { {0x47, 0xff}, {0x48, 0xff}, {0x49, 0xff}, {0x4A, 0xff}, {0x4B, 0xff}, {0x4C, 0xff},
                                              {0x40, 0xc0}, {0x01, 0x7f}, {0x06, 0x07} };
            ok_adc = 1;
            for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) if (i2c_master_transmit(d, seq[i], 2, 50) != ESP_OK) ok_adc = 0;
            i2c_master_bus_rm_device(d);
        }
        cfg.device_address = 0x6B;
        if (bus && i2c_master_bus_add_device(bus, &cfg, &d) == ESP_OK) {
            uint8_t reg = 0x02, v = 0;
            if (i2c_master_transmit_receive(d, &reg, 1, &v, 1, 50) == ESP_OK) {
                uint8_t w1[2] = { 0x08, 0x00 };                  /* CTRL7: accelerometer and gyro off */
                uint8_t w2[2] = { 0x02, (uint8_t)(v | 1) };      /* CTRL1: sensorDisable */
                ok_imu = i2c_master_transmit(d, w1, 2, 50) == ESP_OK && i2c_master_transmit(d, w2, 2, 50) == ESP_OK;
            }
            i2c_master_bus_rm_device(d);
        }
        ESP_LOGI(TAG, "Unused chips powered down: microphone ADC %s, motion sensor %s", ok_adc ? "yes" : "no answer", ok_imu ? "yes" : "no answer");
    }
    s_strip = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_shown = heap_caps_calloc(GRAM_W * GRAM_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_ready = heap_caps_calloc(GRAM_W * GRAM_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_ready_mtx = xSemaphoreCreateMutex();
    screen_init();

    sys_time_from_rtc();                           /* before the first SD writes: real file dates */
    esp_err_t sd_err = bsp_sdcard_mount();
    if (sd_err == ESP_OK) sd_organise();
    s_sd_ok = sd_err == ESP_OK;
    s_sd_err = sd_err;
    if (sd_err != ESP_OK) ESP_LOGW(TAG, "SD card mount returned %s", esp_err_to_name(sd_err));
    else if (bsp_sdcard)
        ESP_LOGI(TAG, "SD card: %s, %.1f GB", bsp_sdcard->cid.name,
                 (double)bsp_sdcard->csd.capacity * bsp_sdcard->csd.sector_size / 1e9);

#if TL_EXPORT_TEST
    {   /* ONE-OFF (2026-11-22): copy the newest Plus Color export to /sdcard/roms/color_en_export.bin for the owner's test */
        DIR *d = opendir("/sdcard/exports");
        char best[96] = "";
        struct dirent *de;
        while (d && (de = readdir(d)) != NULL) {
            ESP_LOGI(TAG, "exports: %s", de->d_name);
            if (!strncmp(de->d_name, "color_en_", 9) && strstr(de->d_name, ".sav") && strcmp(de->d_name, best) > 0)
                snprintf(best, sizeof best, "%s", de->d_name);
        }
        if (d) closedir(d);
        FILE *chk = fopen("/sdcard/roms/color_en_export.bin", "rb");
        if (chk) { fclose(chk); ESP_LOGI(TAG, "Export test: color_en_export.bin already there"); }
        else if (best[0]) {
            char src[128]; snprintf(src, sizeof src, "/sdcard/exports/%s", best);
            FILE *fi = fopen(src, "rb"), *fo = fopen("/sdcard/roms/color_en_export.bin", "wb");
            uint8_t *b = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
            size_t tot = 0, n2;
            while (fi && fo && b && (n2 = fread(b, 1, 32768, fi)) > 0) tot += fwrite(b, 1, n2, fo);
            if (fi) fclose(fi);
            if (fo) fclose(fo);
            free(b);
            ESP_LOGI(TAG, "Export test: %s -> /sdcard/roms/color_en_export.bin (%u bytes)", src, (unsigned)tot);
        }
    }
#endif
    s_spk_dev = bsp_audio_codec_speaker_init();
    if (s_spk_dev) {
        esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 1, .bits_per_sample = 16 };
        esp_codec_dev_open(s_spk_dev, &fs);
        esp_codec_dev_set_out_vol(s_spk_dev, CODEC_VOLUME);
        xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 7, &s_audio_task_h, 0);   /* 7 (was 3): above the display (4) and ui (5) tasks - big screen changes starved it and the sound stuttered (2026-11-23) */
    }

    s_gram = heap_caps_calloc(GRAM_W * GRAM_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_ovl_pool = heap_caps_calloc(OVL_SLOTS, OVL_SIZE, MALLOC_CAP_SPIRAM);
    rs_open();                                     /* map the ROM store now (logs its size, or why not) */
#if TL_ROMRECV
    {   /* ONE-OFF (2026-11-22): receive files from the computer: "ROMFILE!" + name[64] + size u32, then chunks
         * "CK" off u32 n u32 data[n] fnv u32; each chunk is answered "OK" or "ER" (the computer resends);
         * "ALLDONE!" ends. Written to /sdcard/roms/<name>.tmp, renamed when complete. Waits 20 s for a start. */
        printf("ROMRECV READY\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_log_level_set("*", ESP_LOG_NONE);
        usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ucfg.rx_buffer_size = 16384; ucfg.tx_buffer_size = 1024;
        if (usb_serial_jtag_driver_install(&ucfg) == ESP_OK) {
            uint8_t *buf = heap_caps_malloc(4096 + 16, MALLOC_CAP_INTERNAL);
            #define RX(p, n, ms) ((int)usb_serial_jtag_read_bytes((p), (n), pdMS_TO_TICKS(ms)))
            #define RXALL(p, n, ms) ({ int _g = 0, _n = (int)(n); while (_g < _n) { int _r = RX((uint8_t *)(p) + _g, _n - _g, (ms)); if (_r <= 0) break; _g += _r; } _g; })
            uint8_t magic[8];
            int64_t until = esp_timer_get_time() + 20000000;
            while (buf && esp_timer_get_time() < until) {
                if (RXALL(magic, 8, 1000) != 8) continue;
                if (!memcmp(magic, "ALLDONE!", 8)) break;
                if (memcmp(magic, "ROMFILE!", 8)) continue;
                char name[65] = {0}; uint32_t size = 0;
                if (RXALL(name, 64, 2000) != 64 || RXALL(&size, 4, 2000) != 4) break;
                char path[160], tmp[168];
                snprintf(path, sizeof path, "/sdcard/roms/%s", name); snprintf(tmp, sizeof tmp, "%s.tmp", path);
                FILE *f = fopen(tmp, "wb");
                uint32_t got = 0;
                while (f && got < size) {
                    uint8_t hdr[10];
                    #define NAK(code) do { uint8_t _d[256]; while (RX(_d, sizeof _d, 200) > 0) {} usb_serial_jtag_write_bytes(code, 2, portMAX_DELAY); } while (0)
                    if (RXALL(hdr, 10, 3000) != 10 || memcmp(hdr, "CK", 2)) { NAK("E1"); continue; }
                    uint32_t off, n; memcpy(&off, hdr + 2, 4); memcpy(&n, hdr + 6, 4);
                    uint32_t h0;
                    if (n > 4096 || off != got) { NAK("E2"); continue; }
                    if (RXALL(buf, n, 3000) != (int)n || RXALL(&h0, 4, 3000) != 4) { NAK("E3"); continue; }
                    uint32_t h = 2166136261u; for (uint32_t j = 0; j < n; j++) h = (h ^ buf[j]) * 16777619u;
                    if (h != h0) { NAK("E4"); continue; }
                    if (fwrite(buf, 1, n, f) != n) { NAK("E5"); continue; }
                    got += n;
                    usb_serial_jtag_write_bytes("OK", 2, portMAX_DELAY);
                }
                if (f) fclose(f);
                if (got == size) { remove(path); rename(tmp, path); usb_serial_jtag_write_bytes("SAVED!!!", 8, portMAX_DELAY); }
                else { remove(tmp); usb_serial_jtag_write_bytes("FAILED!!", 8, portMAX_DELAY); }
                until = esp_timer_get_time() + 20000000;
            }
            #undef RX
            #undef RXALL
            free(buf);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        esp_log_level_set("*", ESP_LOG_INFO);
    }
#endif
#if TL_ROMXFER
    {   /* ONE-OFF (2026-11-21): send ROM files to the computer over USB, raw, in checked 4 KB chunks */
        /* 2026-11-22 (second run): every ROM with "hexa" in its name except the English one already copied */
        static char names[8][64]; static const char *FILES[8]; int nf = 0;
        {   DIR *dd = opendir("/sdcard/roms"); struct dirent *de;
            while (dd && (de = readdir(dd)) != NULL && nf < 8)
                if (name_has(de->d_name, "hexa") && strcasecmp(de->d_name, "color_hexa_en.bin") && strlen(de->d_name) < 64) {
                    snprintf(names[nf], 64, "%s", de->d_name); FILES[nf] = names[nf]; nf++; }
            if (dd) closedir(dd); }
        printf("ROMXFER START\n");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_log_level_set("*", ESP_LOG_NONE);
        usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ucfg.tx_buffer_size = 16384;
        if (usb_serial_jtag_driver_install(&ucfg) == ESP_OK) {
            uint8_t *buf = heap_caps_malloc(4096 + 16, MALLOC_CAP_INTERNAL);
            for (int fi = 0; buf && fi < nf; fi++) {
                char path[96]; snprintf(path, sizeof path, "/sdcard/roms/%s", FILES[fi]);
                FILE *f = fopen(path, "rb"); if (!f) continue;
                fseek(f, 0, SEEK_END); uint32_t size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
                uint8_t hdr[8 + 64 + 4] = "ROMFILE!";
                memset(hdr + 8, 0, 64); snprintf((char *)hdr + 8, 64, "%s", FILES[fi]); memcpy(hdr + 72, &size, 4);
                usb_serial_jtag_write_bytes(hdr, sizeof hdr, portMAX_DELAY);
                for (uint32_t off = 0; off < size; off += 4096) {
                    uint32_t n = size - off < 4096 ? size - off : 4096;
                    memcpy(buf, "CK", 2); memcpy(buf + 2, &off, 4); memcpy(buf + 6, &n, 4);
                    if (fread(buf + 10, 1, n, f) != n) break;
                    uint32_t h = 2166136261u; for (uint32_t j = 0; j < n; j++) h = (h ^ buf[10 + j]) * 16777619u;
                    memcpy(buf + 10 + n, &h, 4);
                    usb_serial_jtag_write_bytes(buf, 14 + n, portMAX_DELAY);
                }
                fclose(f);
            }
            usb_serial_jtag_write_bytes("ALLDONE!", 8, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        esp_log_level_set("*", ESP_LOG_INFO);
    }
#endif
#if TL_DEVLOG
    {   /* battery diary over USB (2026-11-21): print /sdcard/batlog.csv at start-up, so it can be read without
         * taking the SD card out (the launcher's serial log is recorded on the computer) */
        FILE *f = fopen("/sdcard/logs/batlog.csv", "r");
        if (f) {
            char line[160];
            printf("BATDIARY BEGIN\n");
            int n = 0;
            while (fgets(line, sizeof line, f)) { printf("BATDIARY %s", line); if ((++n & 31) == 0) vTaskDelay(pdMS_TO_TICKS(20)); }
            printf("BATDIARY END (%d lines)\n", n);
            fclose(f);
        }
    }
    {   FILE *g = fopen("/sdcard/logs/flashsaves.txt", "r");   /* the games' own flash saves (purchase tests) */
        if (g) { char line[720]; printf("FLASHLOG BEGIN\n"); while (fgets(line, sizeof line, g)) printf("FLASHLOG %s", line); printf("FLASHLOG END\n"); fclose(g); }
    }
    {   FILE *g = fopen("/sdcard/logs/pmlocks.txt", "r");   /* the lock list from the last time the pet slept */
        if (g) { char line[160]; printf("PMLOCKS BEGIN\n"); while (fgets(line, sizeof line, g)) printf("PMLOCKS %s", line); printf("PMLOCKS END\n"); fclose(g); }
    }
#endif
    {   PmStats st;
        if (pm_stats_read(&st))
            ESP_LOGI(TAG, "Power profiler: sleep %lld us, idle80 %lld, apbmax %lld, cpu240 %lld | sleeps %ld refused %ld",
                     st.t[0], st.t[1], st.t[2], st.t[3], st.cnt, st.rej);
    }
    s_snap = heap_caps_malloc(sizeof(Emu) + SAVE_POOL_BYTES + SAVE_GRAM_BYTES, MALLOC_CAP_SPIRAM);
    s_pg_pool = heap_caps_calloc(PG_PAGES, OVL_SIZE, MALLOC_CAP_SPIRAM);
    s_pg_snap = heap_caps_malloc((size_t)PG_PAGES * OVL_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_pg_pool || !s_pg_snap) { ESP_LOGE(TAG, "No PSRAM for the flash page store"); free(s_pg_pool); free(s_pg_snap); s_pg_pool = s_pg_snap = NULL; }
    ESP_LOGI(TAG, "PSRAM free after setup: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "Internal RAM free %u bytes, largest DMA block %u bytes (the menu screen needs ~15 KB)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

#if TL_BENCH
    run_bench();
#endif
    s_present_req = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(display_task, "display_task", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(ui_task, "ui_task", 8192, NULL, 5, &s_ui_task_h, 0);
}
