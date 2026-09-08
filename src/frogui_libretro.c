/*
 * frogui_libretro.c - FrogUI as a libretro core for SF3000
 *
 * Picoarch loads this core and handles ALL display/input/SDL init.
 * FrogUI renders its menu into a RGB565 buffer and passes it to video_cb.
 * When user picks a game, write /tmp/frogui_launch.txt and signal SHUTDOWN.
 * icube reads that file and launches picoarch with the real game core.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <stdbool.h>

#include "libretro.h"
#include "font.h"
#include "render.h"
#include "theme.h"
#include "recent_games.h"
#include "favorites.h"
#include "settings.h"

#define SDCARD_BASE  "/mnt/sdcard"
#define CORES_PATH   SDCARD_BASE "/cubegm/cores"
/* roms root resolved at init: prefer roms/, accept ROMS/ (exfat is case-sensitive
 * on this kernel), create roms/ if neither exists — menu always has a valid root. */
static char g_roms_path[64] = SDCARD_BASE "/roms";
#define ROMS_PATH    g_roms_path
#define LAUNCH_FILE  "/tmp/frogui_launch.txt"
#define DESKTOP_BIN  SDCARD_BASE "/cubegm/desktop"   /* standalone desktop launcher (MY OWN) */

/* Console → core mapping (folder name → libretro .so)
 * Folder names match /mnt/sdcard/roms/ subdirectories (gb300_multicore convention). */
typedef struct { const char *console_name; const char *core_path; } ConsoleMapping;
static const ConsoleMapping console_mappings[] = {
    /* NES */
    {"nes",    CORES_PATH "/fceumm_libretro.so"},
    {"nesq",   CORES_PATH "/quicknes_libretro.so"},
    {"nest",   CORES_PATH "/nestopia_libretro.so"},
    {"FC",     CORES_PATH "/fceumm_libretro.so"},
    {"NES",    CORES_PATH "/quicknes_libretro.so"},
    /* SNES */
    {"snes",   CORES_PATH "/snes9x2005_plus_libretro.so"},
    {"snes02", CORES_PATH "/snes9x2002_libretro.so"},
    {"SFC",    CORES_PATH "/snes9x2005_plus_libretro.so"},
    /* Game Boy */
    {"gb",     CORES_PATH "/gambatte_libretro.so"},
    {"gbgb",   CORES_PATH "/gearboy_libretro.so"},
    {"gbb",    CORES_PATH "/tgbdual_libretro.so"},
    {"dblcherrygb", CORES_PATH "/gambatte_libretro.so"},
    /* GBA */
    {"gba",    CORES_PATH "/gpsp_libretro.so"},
    {"gbav",   CORES_PATH "/vba_next_libretro.so"},
    {"mgba",   CORES_PATH "/mgba_libretro.so"},
    {"gbaf",   CORES_PATH "/mgba_libretro.so"},
    {"GBA",    CORES_PATH "/gpsp_libretro.so"},
    /* Sega */
    {"sega",   CORES_PATH "/picodrive_libretro.so"},
    {"gg",     CORES_PATH "/gearsystem_libretro.so"},
    {"gpgx",   CORES_PATH "/genesis_plus_gx_libretro.so"},
    {"MD",     CORES_PATH "/picodrive_libretro.so"},
    {"SMS",    CORES_PATH "/picodrive_libretro.so"},
    {"GG",     CORES_PATH "/gearsystem_libretro.so"},
    /* Atari */
    {"a26",    CORES_PATH "/stella2014_libretro.so"},
    {"a5200",  CORES_PATH "/a5200_libretro.so"},
    {"a78",    CORES_PATH "/prosystem_libretro.so"},
    {"a800",   CORES_PATH "/atari800_libretro.so"},
    /* Lynx */
    {"lnx",    CORES_PATH "/handy_libretro.so"},
    /* PC Engine */
    {"pce",    CORES_PATH "/mednafen_pce_fast_libretro.so"},
    {"pcesgx", CORES_PATH "/mednafen_supergrafx_libretro.so"},
    /* Neo Geo Pocket */
    {"ngpc",   CORES_PATH "/race_libretro.so"},
    /* WonderSwan */
    {"wswan",  CORES_PATH "/mednafen_wswan_libretro.so"},
    {"wsv",    CORES_PATH "/potator_libretro.so"},
    /* Virtual Boy */
    {"vb",     CORES_PATH "/mednafen_vb_libretro.so"},
    /* PC-FX */
    {"pcfx",   CORES_PATH "/mednafen_pcfx_libretro.so"},
    /* PC-8800 */
    {"pc8800", CORES_PATH "/quasi88_libretro.so"},
    /* MSX */
    {"msx",    CORES_PATH "/bluemsx_libretro.so"},
    /* C64 */
    {"c64",    CORES_PATH "/vice_x64_libretro.so"},
    {"c64sc",  CORES_PATH "/vice_x64sc_libretro.so"},
    {"c64f",   CORES_PATH "/frodo_libretro.so"},
    {"c64fc",  CORES_PATH "/frodo_libretro.so"},
    {"vic20",  CORES_PATH "/vice_xvic_libretro.so"},
    /* Amstrad */
    {"amstrad",  CORES_PATH "/crocods_libretro.so"},
    {"amstradb", CORES_PATH "/cap32_libretro.so"},
    /* ZX Spectrum */
    {"spec",   CORES_PATH "/fuse_libretro.so"},
    {"zx81",   CORES_PATH "/81_libretro.so"},
    /* Coleco */
    {"col",    CORES_PATH "/gearcoleco_libretro.so"},
    /* Ports / games */
    {"Quake",  CORES_PATH "/tyrquake_libretro.so"},
    {"outrun", CORES_PATH "/cannonball_libretro.so"},
    {"wolf3d", CORES_PATH "/ecwolf_libretro.so"},
    {"prboom", CORES_PATH "/prboom_libretro.so"},
    {"cavestory", CORES_PATH "/nxengine_libretro.so"},
    {"flashback", CORES_PATH "/reminiscence_libretro.so"},
    {"xrick",  CORES_PATH "/xrick_libretro.so"},
    {"gw",     CORES_PATH "/gw_libretro.so"},
    {"jnb",    CORES_PATH "/jumpnbump_libretro.so"},
    /* Misc */
    {"fake08", CORES_PATH "/fake08_libretro.so"},
    {"lowres-nx", CORES_PATH "/lowresnx_libretro.so"},
    {"gme",    CORES_PATH "/gme_libretro.so"},
    {"m2k",    CORES_PATH "/mame2000_libretro.so"},
    {"pokem",  CORES_PATH "/pokemini_libretro.so"},
    {"int",    CORES_PATH "/freeintv_libretro.so"},
    {"fcf",    CORES_PATH "/freechaf_libretro.so"},
    {"cdg",    CORES_PATH "/pocketcdg_libretro.so"},
    {"chip8",  CORES_PATH "/jaxe_libretro.so"},
    {"retro8", CORES_PATH "/retro8_libretro.so"},
    {"arduboy",CORES_PATH "/arduous_libretro.so"},
    {"vec",    CORES_PATH "/vecx_libretro.so"},
    {"thom",   CORES_PATH "/theodore_libretro.so"},
    {"o2em",   CORES_PATH "/o2em_libretro.so"},
    {"xmil",   CORES_PATH "/x68k_libretro.so"},
    {"geolith",CORES_PATH "/geolith_libretro.so"},
    {"gong",   CORES_PATH "/gong_libretro.so"},
    {"vapor",  CORES_PATH "/vaporspec_libretro.so"},
    {"amiga",  CORES_PATH "/uae_libretro.so"},
    {"atari-st", CORES_PATH "/castaway_libretro.so"},
    /* Standalone apps (MY OWN): value is a binary, not a .so — launched directly */
    {"desktop", DESKTOP_BIN},
    {NULL, NULL}
};

static const char* get_core_for_folder(const char *folder) {
    for (int i = 0; console_mappings[i].console_name; i++)
        if (strcasecmp(console_mappings[i].console_name, folder) == 0)
            return console_mappings[i].core_path;
    return NULL;
}

/* Libretro callbacks */
static retro_video_refresh_t     video_cb     = NULL;
static retro_environment_t       environ_cb   = NULL;
static retro_input_poll_t        input_poll_cb = NULL;
static retro_input_state_t       input_state_cb = NULL;

/* App state */
#define MAX_PATH_LEN 512
#define INITIAL_ENTRIES_CAPACITY 64
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif

typedef struct { char name[256]; int is_dir; } DirEntry;

static DirEntry *entries    = NULL;
static int entry_count      = 0;
static int entry_capacity   = 0;
static char current_path[MAX_PATH_LEN] = SDCARD_BASE "/roms";
static int selected_index   = 0;
static int scroll_offset    = 0;
static uint16_t *framebuffer = NULL;
static bool shutdown_requested = false;

static void dbg(const char *msg) {
    FILE *f = fopen("/tmp/frogui_crash.log", "a");
    if (f) { fputs(msg, f); fputs("\n", f); fclose(f); }
    fprintf(stderr, "FROGUI_DBG: %s\n", msg);
}

/* --- Cubevol direct input (picoarch input_state_cb returns nothing on SF3000) --- */
#define CV_UP     4
#define CV_DOWN   6
#define CV_LEFT   7
#define CV_RIGHT  5
#define CV_A     13
#define CV_B     14
#define CV_X     12
#define CV_Y     15
#define CV_L     10
#define CV_R     11
#define CV_SEL    0
#define CV_START  3

static volatile uint32_t *cv_keys = NULL;
static int cv_shmid = -1;

static void cv_init(void) {
    key_t key = ftok("/tmp/joy_key", 'a');
    if (key == (key_t)-1) { dbg("cv_init: ftok failed"); return; }
    cv_shmid = shmget(key, 4, 0666);
    if (cv_shmid < 0) { dbg("cv_init: shmget failed"); return; }
    cv_keys = (volatile uint32_t *)shmat(cv_shmid, NULL, 0);
    if (cv_keys == (void *)-1) { cv_keys = NULL; dbg("cv_init: shmat failed"); return; }
    dbg("cv_init: OK");
}

static uint32_t cv_read(void) {
    return cv_keys ? (*cv_keys & 0xFFFF) : 0;
}

static bool cv_btn(uint32_t state, int bit) { return (state >> bit) & 1; }

/* --- Settings ---
 * Stored at /mnt/sdcard/frogui/settings.txt, key=value format.
 * Two options: theme + font. */
#define SETTINGS_DIR  "/mnt/sdcard/frogui"
#define SETTINGS_FILE SETTINGS_DIR "/settings.txt"

static const char *font_names[] = {"GamePocket", "Monogram"};
#define FONT_COUNT 2

static bool settings_menu_active = false;
static int settings_menu_idx = 0;       /* which row: 0=theme, 1=font */
static int settings_theme_idx = 0;
static int settings_font_idx = 0;
#define SETTINGS_ROW_COUNT 2

static void mkdir_p(const char *path) {
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    system(cmd);
}

static void settings_apply(void) {
    extern const int theme_count;
    if (settings_theme_idx < 0 || settings_theme_idx >= theme_count) settings_theme_idx = 0;
    if (settings_font_idx < 0 || settings_font_idx >= FONT_COUNT) settings_font_idx = 0;
    theme_apply(settings_theme_idx);
    font_load_from_settings(font_names[settings_font_idx]);
}

static void settings_load_file(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[256];
    extern const int theme_count;
    extern const Theme themes[];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        char *nl = strchr(val, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(val, '\r'); if (cr) *cr = '\0';
        if (strcmp(line, "theme") == 0) {
            for (int i = 0; i < theme_count; i++)
                if (strcmp(themes[i].name, val) == 0) { settings_theme_idx = i; break; }
        } else if (strcmp(line, "font") == 0) {
            for (int i = 0; i < FONT_COUNT; i++)
                if (strcmp(font_names[i], val) == 0) { settings_font_idx = i; break; }
        }
    }
    fclose(f);
}

static void settings_save_file(void) {
    extern const Theme themes[];
    mkdir_p(SETTINGS_DIR);
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { dbg("settings save: fopen failed"); return; }
    fprintf(f, "theme=%s\n", themes[settings_theme_idx].name);
    fprintf(f, "font=%s\n", font_names[settings_font_idx]);
    fclose(f);
}

static const char* get_basename(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b+1 : path;
}

#define SETTINGS_ENTRY_NAME ">> Settings"

static void scan_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;
    entry_count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (stat(full, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) {
            const char *ext = strrchr(e->d_name, '.');
            if (ext && (strcasecmp(ext,".csv")==0 || strcasecmp(ext,".txt")==0 ||
                        strcasecmp(ext,".xml")==0 || strcasecmp(ext,".jpg")==0 ||
                        strcasecmp(ext,".png")==0)) continue;
        }
        if (entry_count >= entry_capacity) {
            entry_capacity = entry_capacity ? entry_capacity*2 : INITIAL_ENTRIES_CAPACITY;
            entries = realloc(entries, entry_capacity * sizeof(DirEntry));
            if (!entries) { closedir(dir); return; }
        }
        strncpy(entries[entry_count].name, e->d_name, 255);
        entries[entry_count].name[255] = '\0';
        entries[entry_count].is_dir = S_ISDIR(st.st_mode);
        entry_count++;
    }
    closedir(dir);
    /* Sort: dirs first, then alpha */
    for (int i = 0; i < entry_count-1; i++)
        for (int j = i+1; j < entry_count; j++)
            if (entries[i].is_dir < entries[j].is_dir ||
                (entries[i].is_dir == entries[j].is_dir &&
                 strcasecmp(entries[i].name, entries[j].name) > 0)) {
                DirEntry tmp = entries[i]; entries[i] = entries[j]; entries[j] = tmp;
            }
    /* Append Settings shortcut at root level */
    if (strcmp(path, ROMS_PATH) == 0) {
        if (entry_count >= entry_capacity) {
            entry_capacity = entry_capacity ? entry_capacity*2 : INITIAL_ENTRIES_CAPACITY;
            entries = realloc(entries, entry_capacity * sizeof(DirEntry));
        }
        strncpy(entries[entry_count].name, SETTINGS_ENTRY_NAME, 255);
        entries[entry_count].name[255] = '\0';
        entries[entry_count].is_dir = 0;  /* treat as file so it appears last */
        entry_count++;
    }

    selected_index = 0;
    scroll_offset  = 0;
}

static void request_game_launch(const char *core_path, const char *rom_path) {
    dbg("fork picoarch");
    pid_t pid = fork();
    if (pid == 0) {
        size_t cl = strlen(core_path);
        if (cl < 3 || strcmp(core_path + cl - 3, ".so") != 0) {
            /* Standalone app: exec directly, no picoarch wrapper, no rom arg */
            execl(core_path, get_basename(core_path), (char*)NULL);
            _exit(127);
        }
        /* Child: replace with picoarch+game */
        execl("/mnt/sdcard/cubegm/picoarch", "picoarch", core_path, rom_path, (char*)NULL);
        _exit(127);
    } else if (pid > 0) {
        /* Parent: wait for game to finish, then return to frogui menu */
        int status;
        waitpid(pid, &status, 0);
        dbg("game finished, returning to frogui");
    } else {
        dbg("fork failed");
    }
}

static void handle_input(void) {
    static bool a_last=false, b_last=false, up_last=false, dn_last=false;
    static bool l_last=false, r_last=false;

    uint32_t keys = cv_read();
    /* rkgame owns evdev on some setups → cubevol stops updating /tmp/joy_key.
     * OR in the frontend's joypad state using the same raw bit layout. */
    if (input_state_cb) {
        if (input_poll_cb) input_poll_cb();
        #define RB(id, bit) if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, (id))) keys |= (1u << (bit))
        RB(RETRO_DEVICE_ID_JOYPAD_UP, CV_UP);      RB(RETRO_DEVICE_ID_JOYPAD_DOWN, CV_DOWN);
        RB(RETRO_DEVICE_ID_JOYPAD_LEFT, CV_LEFT);  RB(RETRO_DEVICE_ID_JOYPAD_RIGHT, CV_RIGHT);
        RB(RETRO_DEVICE_ID_JOYPAD_A, CV_A);        RB(RETRO_DEVICE_ID_JOYPAD_B, CV_B);
        RB(RETRO_DEVICE_ID_JOYPAD_X, CV_X);        RB(RETRO_DEVICE_ID_JOYPAD_Y, CV_Y);
        RB(RETRO_DEVICE_ID_JOYPAD_L, CV_L);        RB(RETRO_DEVICE_ID_JOYPAD_R, CV_R);
        RB(RETRO_DEVICE_ID_JOYPAD_START, CV_START); RB(RETRO_DEVICE_ID_JOYPAD_SELECT, CV_SEL);
        #undef RB
    }
    bool a  = cv_btn(keys, CV_A);
    bool b  = cv_btn(keys, CV_B);
    bool up = cv_btn(keys, CV_UP);
    bool dn = cv_btn(keys, CV_DOWN);
    bool lt = cv_btn(keys, CV_LEFT);
    bool rt = cv_btn(keys, CV_RIGHT);

    if (settings_menu_active) {
        extern const int theme_count;
        if (up && !up_last) settings_menu_idx = (settings_menu_idx - 1 + SETTINGS_ROW_COUNT) % SETTINGS_ROW_COUNT;
        if (dn && !dn_last) settings_menu_idx = (settings_menu_idx + 1) % SETTINGS_ROW_COUNT;
        if ((lt && !l_last) || (rt && !r_last)) {
            int delta = (rt && !r_last) ? 1 : -1;
            if (settings_menu_idx == 0) {
                settings_theme_idx = (settings_theme_idx + delta + theme_count) % theme_count;
            } else {
                settings_font_idx = (settings_font_idx + delta + FONT_COUNT) % FONT_COUNT;
            }
            settings_apply();
        }
        if ((a && !a_last) || (b && !b_last)) {
            settings_save_file();
            settings_menu_active = false;
        }
        a_last=a; b_last=b; up_last=up; dn_last=dn; l_last=lt; r_last=rt;
        return;
    }

    if (a && !a_last && selected_index < entry_count) {
        if (entries[selected_index].is_dir) {
            const char *app = get_core_for_folder(entries[selected_index].name);
            if (app && strlen(app) >= 3 && strcmp(app + strlen(app) - 3, ".so") != 0) {
                request_game_launch(app, "");   /* standalone app: launch, don't enter */
            } else {
            char new_path[MAX_PATH_LEN];
            snprintf(new_path, sizeof(new_path), "%s/%s", current_path, entries[selected_index].name);
            strncpy(current_path, new_path, MAX_PATH_LEN-1);
            current_path[MAX_PATH_LEN-1] = '\0';
            scan_directory(current_path);
            }
        } else {
            if (strcmp(entries[selected_index].name, SETTINGS_ENTRY_NAME) == 0) {
                settings_menu_active = true;
                settings_menu_idx = 0;
            } else {
                const char *folder = get_basename(current_path);
                const char *core   = get_core_for_folder(folder);
                if (core) {
                    char rom_path[MAX_PATH_LEN];
                    snprintf(rom_path, sizeof(rom_path), "%s/%s", current_path, entries[selected_index].name);
                    request_game_launch(core, rom_path);
                }
            }
        }
    }

    if (b && !b_last && strcmp(current_path, ROMS_PATH) != 0) {
        char *slash = strrchr(current_path, '/');
        if (slash) { *slash = '\0'; scan_directory(current_path); }
    }

    if (up && !up_last && selected_index > 0) {
        selected_index--;
        if (selected_index < scroll_offset) scroll_offset = selected_index;
    }
    if (dn && !dn_last && selected_index < entry_count-1) {
        selected_index++;
        if (selected_index >= scroll_offset + VISIBLE_ENTRIES)
            scroll_offset = selected_index - VISIBLE_ENTRIES + 1;
    }

    a_last=a; b_last=b; up_last=up; dn_last=dn; l_last=lt; r_last=rt;
}

/* ---- libretro API ---- */

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "TreeFrogUI";
    info->library_version  = "1.0-sf3000";
    info->valid_extensions = "";
    info->need_fullpath    = false;
    info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = SCREEN_WIDTH;
    info->geometry.base_height  = SCREEN_HEIGHT;
    info->geometry.max_width    = SCREEN_WIDTH;
    info->geometry.max_height   = SCREEN_HEIGHT;
    info->geometry.aspect_ratio = (float)SCREEN_WIDTH / SCREEN_HEIGHT;
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = 44100.0;
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    bool no_game = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
}

void retro_set_video_refresh(retro_video_refresh_t cb)          { video_cb      = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)            { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb){ (void)cb; }
void retro_set_input_poll(retro_input_poll_t cb)                { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)              { input_state_cb = cb; }


void retro_init(void) {
    dbg("retro_init start");
    cv_init();
    font_init();
    dbg("font_init done");
    theme_init();
    dbg("theme_init done");
    settings_load_file();
    settings_apply();
    dbg("settings loaded");
    recent_games_init();
    dbg("recent_games_init done");
    favorites_init();
    dbg("favorites_init done");

    framebuffer = calloc(SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(uint16_t));
    dbg("calloc done");
    render_init(framebuffer);
    dbg("render_init done");
    /* Resolve roms root before first scan (see g_roms_path). */
    {
        DIR *d = opendir(g_roms_path);
        if (!d) {
            DIR *alt = opendir(SDCARD_BASE "/ROMS");
            if (alt) { closedir(alt); strcpy(g_roms_path, SDCARD_BASE "/ROMS"); }
            else mkdir(g_roms_path, 0777);
        } else closedir(d);
        strncpy(current_path, g_roms_path, MAX_PATH_LEN-1);
        current_path[MAX_PATH_LEN-1] = '\0';
    }
    scan_directory(current_path);
    dbg("scan_directory done");
    shutdown_requested = false;
    dbg("retro_init complete");
}

void retro_deinit(void) {
    free(framebuffer); framebuffer = NULL;
    free(entries);     entries = NULL;
    entry_count = entry_capacity = 0;
}

bool retro_load_game(const struct retro_game_info *info) {
    (void)info;
    return true;  /* supports no-game */
}

void retro_unload_game(void) {}

static void render_settings_menu(void) {
    extern const Theme themes[];
    render_clear_screen(framebuffer);
    render_header(framebuffer, "SETTINGS");

    char line[128];
    int y0 = START_Y;
    /* Theme row */
    snprintf(line, sizeof(line), "Theme: < %s >", themes[settings_theme_idx].name);
    if (settings_menu_idx == 0) {
        render_text_pillbox(framebuffer, PADDING, y0, line, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
    } else {
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y0, line, COLOR_TEXT);
    }
    /* Font row */
    snprintf(line, sizeof(line), "Font: < %s >", font_names[settings_font_idx]);
    int y1 = y0 + ITEM_HEIGHT;
    if (settings_menu_idx == 1) {
        render_text_pillbox(framebuffer, PADDING, y1, line, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
    } else {
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, y1, line, COLOR_TEXT);
    }
    render_legend(framebuffer, LEGEND_X_NONE);
}

void retro_run(void) {
    handle_input();

    if (settings_menu_active) {
        render_settings_menu();
    } else {
        render_clear_screen(framebuffer);
        const char *title = (strcmp(current_path, ROMS_PATH) == 0)
                            ? "TREEFROGUI: SYSTEMS" : get_basename(current_path);
        render_header(framebuffer, title);
        int visible = min(entry_count - scroll_offset, VISIBLE_ENTRIES);
        for (int i = 0; i < visible; i++) {
            int idx = scroll_offset + i;
            render_menu_item(framebuffer, i, entries[idx].name, entries[idx].is_dir,
                             (idx == selected_index), scroll_offset, 0);
        }
        render_legend(framebuffer, LEGEND_X_NONE);
    }

    if (video_cb)
        video_cb(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
}

void retro_reset(void) { scan_directory(ROMS_PATH); strncpy(current_path, ROMS_PATH, MAX_PATH_LEN-1); }

unsigned retro_get_region(void)                                    { return RETRO_REGION_NTSC; }
size_t   retro_serialize_size(void)                                { return 0; }
bool     retro_serialize(void *d, size_t s)                        { (void)d;(void)s; return false; }
bool     retro_unserialize(const void *d, size_t s)                { (void)d;(void)s; return false; }
void     retro_cheat_reset(void)                                   {}
void     retro_cheat_set(unsigned i, bool e, const char *c)        { (void)i;(void)e;(void)c; }
void    *retro_get_memory_data(unsigned id)                        { (void)id; return NULL; }
size_t   retro_get_memory_size(unsigned id)                        { (void)id; return 0; }
bool     retro_load_game_special(unsigned t,
             const struct retro_game_info *i, size_t n)            { (void)t;(void)i;(void)n; return false; }
