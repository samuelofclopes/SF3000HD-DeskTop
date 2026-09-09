/* ============================================================================
 * CLICKER 3000 - a modern clicker for the DataFrog SF3000 (HiChip MIPS32r2)
 * Native standalone app: owns the framebuffer and the gamepad bitmask; exits
 * cleanly so the launcher (stock desktop) gets control back.
 *
 * BUILD (on a PC with the SF3000 toolchain, see Makefile):
 *   make            -> clicker3000 (MIPS32r2 ELF, drop it in your apps folder)
 *   make host       -> host test build (x86, same game logic)
 *   make test       -> host build + logic selftest
 *
 * CONTROLS (default key bits, overridable in /mnt/sdcard/frogui/keymap.txt):
 *   A click (hold = auto-click)   X open/close shop
 *   START pause menu              B back / close
 *   D-pad navigate shop           A buy
 *
 * On exit the overlay framebuffer is cleared and the process returns 0, so
 * whatever launched it (stock desktop) regains the screen and input.
 * ============================================================================ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <linux/fb.h>

#include "font3000_data.h"

#ifdef HOST_TEST
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* forward declarations */
static void save_now(void);
static void load_now(void);
static void push_toast(const char *head, const char *txt, uint32_t col);

/* ------------------------- tunable game design ---------------------------- */
#define GAME_NAME     "CLICKER 3000"
#define SAVE_NAME     "clicker3000.sav"
#define GOAL          1e12        /* win at 1 trillion                    */
#define STAR_CAP      40          /* stars above this stop adding power   */
#define CLICK_LV_CAP  500         /* per-item click power cap (levels)    */
#define AUTOCLICK_DIV 10          /* hold A: one click every N frames     */
#define N_ITEMS       11
#define N_ACH         8

static const double ITEM_BASE[N_ITEMS] = {15, 300, 8000, 50, 550, 6000, 65000,
                                          750000, 9e6, 1.2e8, 1.6e9};
static const double ITEM_G[N_ITEMS]    = {1.50, 1.35, 1.30, 1.15, 1.15, 1.16,
                                          1.17, 1.18, 1.19, 1.20, 1.21};
static const double ITEM_VLR[N_ITEMS]  = {1, 8, 60, 0.5, 5, 45, 400, 4000,
                                          4e4, 4e5, 4e6};
static const int    ITEM_ISCLICK[N_ITEMS] = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
static const char  *ITEM_NAME[N_ITEMS] = {"THUMB", "GLOVE", "HAMMER", "MOUSE",
        "ROBOT", "FACTORY", "REACTOR", "PORTAL", "WORMHOLE", "SINGULARITY",
        "OMEGA"};
static const char  *ITEM_TAG[N_ITEMS]  = {"TH", "GL", "HM", "MS", "RB", "FA",
                                          "RE", "PO", "WH", "SG", "OM"};
static const uint32_t ITEM_COL[N_ITEMS] = {
        0xE8A33D, 0x5FB0E8, 0xB478E8, 0x8FCB6B, 0xE86A6A, 0xE8A33D, 0x6BE0C8,
        0xC86BE0, 0x6B8CE8, 0xE8D26B, 0xF0F0F0};
static const char *ACH_NAME[N_ACH] = {"FIRST CLICK!", "100 CLICKS!",
        "1000 CLICKS!", "1K EARNED!", "1M EARNED!", "1B EARNED!",
        "OMEGA BOUGHT!", "GOAL REACHED!"};

/* palette (ARGB, alpha 255 = opaque) */
#define C_BG      0xFF0F1220u
#define C_BG_TOP  0xFF161B2Cu
#define C_PANEL   0xF01C2130u
#define C_PANEL2  0xF0242B3Du
#define C_TEXT    0xFFF2F5FAu
#define C_DIM     0xFF8A93A8u
#define C_GREEN   0xFF3DDC84u
#define C_ORANGE  0xFFFF9F3Cu
#define C_ORANGE2 0xFFFFC46Bu
#define C_ORANGE3 0xFFB96A1Fu
#define C_RED     0xFFFF5A5Au
#define C_GOLD    0xFFFFD166u
#define C_TEAL    0xFF2BD9C8u
#define C_WHITE   0xFFFFFFFFu

/* ------------------------------- game state ------------------------------- */
typedef struct {
        double money, earned, best;
        unsigned long long clicks;
        int stars, ach, won;
        int lv[N_ITEMS];
        double playtime;
} Save;

static Save st;
static int mode;               /* 0=title 1=game 2=pause */
static bool shop_open;
static float shop_t;           /* 0..1 slide animation */
static int shop_sel;           /* 0..N_ITEMS-1 rows, N_ITEMS = prestige */
static float shop_scroll;      /* smooth scroll offset (px) */
static int shop_row_flash;     /* row index flashing red */
static float shop_flash_t;
static int menu_sel;           /* pause menu */
static bool win_show;          /* goal overlay visible */
static bool prestige_confirm;
static float press_t;
static float time_acc;
static bool g_quit;

#define MAX_TOAST 6
typedef struct { float t; char head[24]; char txt[40]; uint32_t col; } Toast;
static Toast toasts[MAX_TOAST];
static float shop_panel_x(void);

/* --------------------------------- time ----------------------------------- */
static double now_sec(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ============================== ECONOMY =================================== */
static double item_cost(int i) { return ITEM_BASE[i] * pow(ITEM_G[i], st.lv[i]); }

static double star_mult(void) {
        int s = st.stars > STAR_CAP ? STAR_CAP : st.stars;
        return 1.0 + 0.25 * (double)s;
}

static double click_power(void) {
        double p = 1.0;
        for (int i = 0; i < 3; i++)
                p += ITEM_VLR[i] * (double)(st.lv[i] > CLICK_LV_CAP ?
                                            CLICK_LV_CAP : st.lv[i]);
        return p * star_mult();
}

static double cps(void) {
        double c = 0.0;
        for (int i = 3; i < N_ITEMS; i++)
                c += ITEM_VLR[i] * (double)st.lv[i];
        return c * star_mult();
}

static int prestige_gain(void) {
        double lg = st.earned > 1.0 ? log10(st.earned) : 0.0;
        int gain = 1;
        if (lg > 15.0) gain += (int)((lg - 15.0) / 3.0);
        return gain;
}

/* GOAL bar fraction: LINEAR share of earned vs goal (v1.1 - the old log
 * scale made $33 of $1T look like 1/5 of the bar; hardware report). */
static float goal_frac(void) {
        float k = (float)(st.earned / GOAL);
        if (k < 0) k = 0;
        if (k > 1) k = 1;
        return k;
}

static void push_toast(const char *head, const char *txt, uint32_t col) {
        for (int i = MAX_TOAST - 1; i > 0; i--) toasts[i] = toasts[i - 1];
        memset(&toasts[0], 0, sizeof(Toast));
        toasts[0].t = 3.4f;
        snprintf(toasts[0].head, sizeof(toasts[0].head), "%s", head);
        snprintf(toasts[0].txt, sizeof(toasts[0].txt), "%s", txt);
        toasts[0].col = col;
}

static void grant_ach(int b) {
        if (st.ach & (1 << b)) return;
        st.ach |= (1 << b);
        push_toast("TROPHY UNLOCKED", ACH_NAME[b], C_GOLD);
}

static void do_prestige(void) {
        int gain = prestige_gain();
        st.stars += gain;
        st.money = 0; st.earned = 0; st.won = 0;
        for (int i = 0; i < N_ITEMS; i++) st.lv[i] = 0;
        shop_open = false; shop_t = 0; win_show = false;
        char msg[64];
        snprintf(msg, sizeof(msg), "+%d STAR(S), +25%% EACH", gain);
        push_toast("PRESTIGE!", msg, C_GOLD);
        save_now();
}

static void add_money(double x) {
        if (x == 0) return;
        st.money += x; st.earned += x;
        if (st.money > st.best) st.best = st.money;
        if (st.earned >= 1e3) grant_ach(3);
        if (st.earned >= 1e6) grant_ach(4);
        if (st.earned >= 1e9) grant_ach(5);
        if (!st.won && st.earned >= GOAL) {
                st.won = 1; grant_ach(7);
                win_show = true; save_now();
        }
}

static void do_click(void) {
        double p = click_power();
        add_money(p);
        st.clicks++;
        press_t = 0.12f;
        if (st.clicks >= 1) grant_ach(0);
        if (st.clicks >= 100) grant_ach(1);
        if (st.clicks >= 1000) grant_ach(2);
}

static bool try_buy(int i) {
        double c = item_cost(i);
        if (st.money >= c) {
                st.money -= c; st.lv[i]++;
                if (i == N_ITEMS - 1) grant_ach(6);
                save_now();
                return true;
        }
        shop_row_flash = i; shop_flash_t = 0.3f;
        return false;
}

/* ============================== NUMBER FORMAT ============================= */
static const char *SUF[] = {"", "K", "M", "B", "T", "Qa", "Qi", "Sx", "Sp",
                            "Oc", "No", "Dc"};
#define N_SUF 12

static void fmt_num(double v, bool money, char *out, size_t cap) {
        if (v < 0) { snprintf(out, cap, money ? "-$" : "-"); v = -v;
                     size_t l = strlen(out); fmt_num(v, money, out + l, cap - l);
                     return; }
        if (v < 1000.0) {
                if (money) snprintf(out, cap, "$%lld", (long long)v);
                else snprintf(out, cap, "%lld", (long long)v);
                return;
        }
        int g = (int)(log10(v) / 3.0);
        if (g >= N_SUF) {
                int e = (int)log10(v);
                snprintf(out, cap, money ? "$%.2fe%d" : "%.2fe%d",
                         v / pow(10.0, e), e);
                return;
        }
        double m = v / pow(1000.0, g);
        int dec = m < 10.0 ? 2 : (m < 100.0 ? 1 : 0);
        double r = m * pow(10.0, dec);
        if (round(r) >= 1000.0) { g++; m /= 1000.0; dec = m < 10.0 ? 2 : 1; }
        if (g >= N_SUF) {
                int e = (int)log10(v);
                snprintf(out, cap, money ? "$%.2fe%d" : "%.2fe%d",
                         v / pow(10.0, e), e);
                return;
        }
        snprintf(out, cap, money ? "$%.*f%s" : "%.*f%s", dec, m, SUF[g]);
}

/* ================================ SAVE ==================================== */
/* v1.2: the save lives in osdata on the SD card, so the desktop/apps listing
 * (which shows every file next to the binary) stays clean.  A save left by
 * an older version next to the binary is adopted on first load and deleted
 * after the first successful save in the new location - no progress lost. */
#define OS_DATA_DIR   "/mnt/sdcard/osdata"
#define SAVE_SUBDIR   "clicker3000"
static char save_path[512];
static bool save_migrated;   /* legacy save adopted -> unlink after save */

static void legacy_save_path(char *out, size_t cap) {
        out[0] = 0;
        char ex[400];
        ssize_t n = readlink("/proc/self/exe", ex, sizeof(ex) - 1);
        if (n <= 0) return;
        ex[n] = 0;
        char *slash = strrchr(ex, '/');
        if (!slash) return;
        *slash = 0;
        snprintf(out, cap, "%s/" SAVE_NAME, ex);
}

static bool osdata_ready(void) {
        mkdir(OS_DATA_DIR, 0755);                    /* EEXIST is fine */
        char dir[256];
        snprintf(dir, sizeof(dir), OS_DATA_DIR "/" SAVE_SUBDIR);
        mkdir(dir, 0755);
        return access(dir, W_OK) == 0;
}

static void find_save_path(void) {
        const char *env = getenv("CLICKER3000_SAVE");
        if (env && *env) { snprintf(save_path, sizeof(save_path), "%s", env);
                           return; }
        if (osdata_ready()) {
                snprintf(save_path, sizeof(save_path),
                         OS_DATA_DIR "/" SAVE_SUBDIR "/" SAVE_NAME);
                return;
        }
        char lg[512];
        legacy_save_path(lg, sizeof(lg));
        if (lg[0]) { snprintf(save_path, sizeof(save_path), "%s", lg); return; }
        snprintf(save_path, sizeof(save_path), "./" SAVE_NAME);
}

static void default_state(void) {
        memset(&st, 0, sizeof(st));
}

static void save_now(void) {
        char tmp[560];
        snprintf(tmp, sizeof(tmp), "%s.tmp", save_path);
        FILE *f = fopen(tmp, "w");
        if (!f) return;
        fprintf(f, "clicker3000 v1\n");
        fprintf(f, "money %.17g\nearned %.17g\nbest %.17g\nclicks %llu\n",
                st.money, st.earned, st.best, st.clicks);
        fprintf(f, "stars %d\nach %d\nwon %d\nplay %.0f\n",
                st.stars, st.ach, st.won, st.playtime);
        fprintf(f, "lv");
        for (int i = 0; i < N_ITEMS; i++) fprintf(f, " %d", st.lv[i]);
        fprintf(f, "\n");
        fclose(f);
        if (rename(tmp, save_path) != 0) return;
        if (save_migrated) {                 /* progress is now safe in the
                                              * new place - drop the old file
                                              * so the apps list stays clean */
                char lg[512];
                legacy_save_path(lg, sizeof(lg));
                if (lg[0] && strcmp(lg, save_path)) unlink(lg);
                save_migrated = false;
        }
}

static void load_now(void) {
        default_state();
        FILE *f = fopen(save_path, "r");
        if (!f) {
                /* first run after the v1.2 update: adopt a save left by an
                 * older version next to the binary, so nothing is lost */
                char lg[512];
                legacy_save_path(lg, sizeof(lg));
                if (lg[0] && strcmp(lg, save_path)) {
                        f = fopen(lg, "r");
                        if (f) save_migrated = true;
                }
        }
        if (!f) return;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "clicker3000 v%*d") == 0 &&
                    strncmp(line, "clicker3000", 11) == 0) continue;
                double d;
                unsigned long long ull;
                int i;
                if (sscanf(line, "money %lf", &d) == 1) st.money = d;
                else if (sscanf(line, "earned %lf", &d) == 1) st.earned = d;
                else if (sscanf(line, "best %lf", &d) == 1) st.best = d;
                else if (sscanf(line, "clicks %llu", &ull) == 1) st.clicks = ull;
                else if (sscanf(line, "stars %d", &i) == 1) st.stars = i;
                else if (sscanf(line, "ach %d", &i) == 1) st.ach = i;
                else if (sscanf(line, "won %d", &i) == 1) st.won = i;
                else if (sscanf(line, "play %lf", &d) == 1) st.playtime = d;
                else if (strncmp(line, "lv", 2) == 0) {
                        char *p = line + 2;
                        for (int j = 0; j < N_ITEMS; j++) {
                                while (*p == ' ') p++;
                                st.lv[j] = (int)strtol(p, &p, 10);
                                if (st.lv[j] < 0) st.lv[j] = 0;
                                if (st.lv[j] > 99999) st.lv[j] = 99999;
                        }
                }
        }
        fclose(f);
        if (st.stars < 0 || st.stars > 100000) st.stars = 0;
        if (st.money < 0 || !isfinite(st.money)) st.money = 0;
        if (st.earned < 0 || !isfinite(st.earned)) st.earned = 0;
        if (!isfinite(st.best) || st.best < 0) st.best = 0;
}

/* ============================ DISPLAY (fb) ================================ */
#define LW 854
#define LH 480

typedef struct {
        int fd;
        unsigned char *mem;
        size_t mem_len;
        int fb_w, fb_h, pitch, bpp;
        struct fb_var_screeninfo vi;
        int lw, lh, rotation;
        uint32_t *canvas;
        int *lx_of, *ly_of;              /* fb->logical maps */
        uint32_t enc_r[256], enc_g[256], enc_b[256]; /* 255->field LUTs */
        uint32_t enc_a[256];                       /* alpha -> vi.transp LUT */
} Disp;

static Disp D;
static volatile sig_atomic_t sig_quit;
static void on_signal(int s) { (void)s; sig_quit = 1; }

static void read_rotation(int *rot) {
        *rot = 90;                        /* SF3000 default profile */
        FILE *f = fopen("/tmp/tfdevice.env", "r");
        if (!f) return;
        char line[128], key[64], val[64];
        while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%63[^=]=%63s", key, val) != 2) continue;
                if (!strcmp(key, "TF_ROTATE")) *rot = atoi(val);
        }
        fclose(f);
        if (*rot != 0 && *rot != 90 && *rot != 180 && *rot != 270) *rot = 90;
}

static void read_rot_override(int *rot);   /* defined below disp_open_fb */

/* Map construction - matches TreeFrogUI apps/image_viewer.c overlay_present
 * exactly (the proven on-device reference):
 *   rot  90: lx = fy * lw / fb_h        ly = lh-1 - fx * lh / fb_w
 *   rot 180: lx = lw-1 - fx * lw / fb_w ly = lh-1 - fy * lh / fb_h
 *   rot 270: lx = lw-1 - fy * lw / fb_h ly = fx * lh / fb_w
 *   rot   0: lx = fx * lw / fb_w        ly = fy * lh / fb_h
 * NOTE: for 90/270 lx is indexed by the FB ROW and ly by the FB COLUMN.
 * Getting this wrong transposes the image (mirror + sideways text). */
static void disp_rebuild_maps(void) {
        if (D.rotation == 90 || D.rotation == 270) {
                for (int fy = 0; fy < D.fb_h; fy++) {
                        int lx = (D.rotation == 90)
                                ? fy * D.lw / D.fb_h
                                : D.lw - 1 - fy * D.lw / D.fb_h;
                        D.lx_of[fy] = lx < 0 ? 0 : (lx >= D.lw ? D.lw - 1 : lx);
                }
                for (int fx = 0; fx < D.fb_w; fx++) {
                        int ly = (D.rotation == 90)
                                ? D.lh - 1 - fx * D.lh / D.fb_w
                                : fx * D.lh / D.fb_w;
                        D.ly_of[fx] = ly < 0 ? 0 : (ly >= D.lh ? D.lh - 1 : ly);
                }
        } else {
                for (int fx = 0; fx < D.fb_w; fx++) {
                        int lx = (D.rotation == 180)
                                ? D.lw - 1 - fx * D.lw / D.fb_w
                                : fx * D.lw / D.fb_w;
                        D.lx_of[fx] = lx < 0 ? 0 : (lx >= D.lw ? D.lw - 1 : lx);
                }
                for (int fy = 0; fy < D.fb_h; fy++) {
                        int ly = (D.rotation == 180)
                                ? D.lh - 1 - fy * D.lh / D.fb_h
                                : fy * D.lh / D.fb_h;
                        D.ly_of[fy] = ly < 0 ? 0 : (ly >= D.lh ? D.lh - 1 : ly);
                }
        }
}

static int disp_open_fb(const char *dev) {
        int fd = open(dev, O_RDWR);
        if (fd < 0) return -1;
        struct fb_fix_screeninfo fin;
        struct fb_var_screeninfo vi;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
            ioctl(fd, FBIOGET_FSCREENINFO, &fin) < 0 || !fin.smem_len) {
                close(fd); return -1;
        }
        int bpp = vi.bits_per_pixel / 8;
        if (bpp != 2 && bpp != 4) { close(fd); return -1; }
        D.fd = fd; D.vi = vi;
        D.fb_w = vi.xres; D.fb_h = vi.yres;
        D.pitch = fin.line_length; D.bpp = bpp; D.mem_len = fin.smem_len;
        D.mem = mmap(NULL, D.mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (D.mem == MAP_FAILED) { D.mem = NULL; close(fd); return -1; }
        return 0;
}

static int disp_init(void) {
        memset(&D, 0, sizeof(D));
        D.fd = -1;
        int rot;
        read_rotation(&rot);
        D.lw = LW; D.lh = LH;
        D.rotation = rot;
        if (disp_open_fb("/dev/fb1") != 0 && disp_open_fb("/dev/fb0") != 0) {
                D.fb_w = 0; D.fb_h = 0;           /* headless (test builds) */
        } else if (D.fb_w < D.fb_h && D.lw > D.lh) {
                /* portrait panel + landscape canvas: keep profile rotation */
        } else {
                D.rotation = 0;
        }
        read_rot_override(&D.rotation);           /* manual rot.txt fix */
        D.canvas = malloc((size_t)LW * LH * 4);
        if (!D.canvas) return -1;
        memset(D.canvas, 0, (size_t)LW * LH * 4);
        int map_n = D.fb_w > D.fb_h ? D.fb_w : D.fb_h;
        if (map_n < 1) map_n = 1;
        D.lx_of = malloc(sizeof(int) * (size_t)map_n);
        D.ly_of = malloc(sizeof(int) * (size_t)map_n);
        if (!D.lx_of || !D.ly_of) return -1;
        if (D.fb_w > 0) disp_rebuild_maps();
        /* fb1 is a hardware ARGB overlay: the compositor blends per-pixel
         * alpha, so every packed pixel MUST carry the canvas alpha. Some
         * firmware fb drivers leave var.transp zeroed even on ARGB layers,
         * so for 32bpp assume byte-3 alpha when it is not reported. */
        unsigned a_len = D.vi.transp.length, a_off = D.vi.transp.offset;
        if (D.bpp == 4 && a_len == 0) { a_len = 8; a_off = 24; }
        for (int v = 0; v < 256; v++) {
                unsigned r = (v * ((1u << D.vi.red.length) - 1u) + 127) / 255;
                unsigned g = (v * ((1u << D.vi.green.length) - 1u) + 127) / 255;
                unsigned b = (v * ((1u << D.vi.blue.length) - 1u) + 127) / 255;
                unsigned a = a_len ? (v * ((1u << a_len) - 1u) + 127) / 255 : 0;
                D.enc_r[v] = r << D.vi.red.offset;
                D.enc_g[v] = g << D.vi.green.offset;
                D.enc_b[v] = b << D.vi.blue.offset;
                D.enc_a[v] = a << a_off;
        }
        return 0;
}

static void disp_clear_device(void) {
        if (D.mem) memset(D.mem, 0, D.mem_len);
}

static void disp_close(void) {
        disp_clear_device();
        if (D.mem) munmap(D.mem, D.mem_len);
        if (D.fd >= 0) close(D.fd);
        free(D.canvas); free(D.lx_of); free(D.ly_of);
        memset(&D, 0, sizeof(D)); D.fd = -1;
}

/* optional manual orientation fix: a "rot.txt" file next to the binary
 * containing 0, 90, 180 or 270 overrides the device profile rotation */
static void read_rot_override(int *rot) {
        char ex[400];
        ssize_t n = readlink("/proc/self/exe", ex, sizeof(ex) - 1);
        if (n <= 0) return;
        ex[n] = 0;
        char *slash = strrchr(ex, '/');
        if (!slash) return;
        *slash = 0;
        char path[420];
        snprintf(path, sizeof(path), "%s/rot.txt", ex);
        FILE *f = fopen(path, "r");
        if (!f) return;
        int v = 0;
        if (fscanf(f, "%d", &v) == 1 &&
            (v == 0 || v == 90 || v == 180 || v == 270))
                *rot = v;
        fclose(f);
}

/* ------------------------------ present --------------------------------- */
typedef struct {
        int fb_w, fb_h, pitch, bpp, xo, yo;
        int lw, lh, rotation;
        const uint32_t *canvas;
        const int *lx_of, *ly_of;
        const uint32_t *enc_r, *enc_g, *enc_b, *enc_a;
} PresentJob;

static inline uint32_t pack_px(const PresentJob *j, uint32_t px) {
        return j->enc_r[(px >> 16) & 255] | j->enc_g[(px >> 8) & 255] |
               j->enc_b[px & 255] | j->enc_a[px >> 24];
}

static void present_generic(const PresentJob *j, unsigned char *mem) {
        int t = (j->rotation == 90 || j->rotation == 270);
        for (int fy = 0; fy < j->fb_h; fy++) {
                unsigned char *dst = mem + (size_t)(fy + j->yo) * j->pitch;
                int lxk = t ? j->lx_of[fy] : 0;     /* row-constant for 90/270 */
                int ly0 = t ? 0 : j->ly_of[fy];     /* col-constant for 0/180  */
                for (int fx = 0; fx < j->fb_w; fx++) {
                        int lx = t ? lxk : j->lx_of[fx];
                        int ly = t ? j->ly_of[fx] : ly0;
                        uint32_t v = pack_px(j,
                                j->canvas[(size_t)ly * j->lw + lx]);
                        if (j->bpp == 4)
                                ((uint32_t *)dst)[fx + j->xo] = v;
                        else
                                ((uint16_t *)dst)[fx + j->xo] = (uint16_t)v;
                }
        }
}

/* Cache-friendly fast path for the exact 1:1 rotated case (fb_w == lh and
 * fb_h == lw, i.e. the SF3000 profile). 8x8 tiles: canvas reads stay inside
 * one 32-byte line per row and each fb cache line is fully covered, which
 * cuts the strided-access misses about 4x versus the generic loop. */
static void present_rot_tiled(const PresentJob *j, unsigned char *mem) {
        bool r90 = (j->rotation == 90);
        for (int ty = 0; ty < j->lh; ty += 8) {
                int th = j->lh - ty; if (th > 8) th = 8;
                for (int tx = 0; tx < j->lw; tx += 8) {
                        int tw = j->lw - tx; if (tw > 8) tw = 8;
                        uint32_t stage[64];
                        for (int jj = 0; jj < th; jj++)
                                memcpy(&stage[jj * 8],
                                       &j->canvas[(size_t)(ty + jj) * j->lw + tx],
                                       (size_t)tw * sizeof(uint32_t));
                        for (int jj = 0; jj < th; jj++) {
                                int fx = r90 ? (j->fb_w - 1 - (ty + jj))
                                             : (ty + jj);
                                for (int ii = 0; ii < tw; ii++) {
                                        int fy = r90 ? (tx + ii)
                                                     : (j->fb_h - 1 - (tx + ii));
                                        uint32_t v =
                                                pack_px(j, stage[jj * 8 + ii]);
                                        unsigned char *dst = mem +
                                                (size_t)(fy + j->yo) * j->pitch;
                                        if (j->bpp == 4)
                                                ((uint32_t *)dst)[fx + j->xo] = v;
                                        else
                                                ((uint16_t *)dst)[fx + j->xo] =
                                                        (uint16_t)v;
                                }
                        }
                }
        }
}

/* verbatim image_viewer transform - selftest oracle */
static void present_reference(const PresentJob *j, unsigned char *mem) {
        for (int fy = 0; fy < j->fb_h; fy++) {
                unsigned char *row = mem + (size_t)(fy + j->yo) * j->pitch;
                for (int fx = 0; fx < j->fb_w; fx++) {
                        int lx, ly;
                        if (j->rotation == 90) {
                                lx = fy * j->lw / j->fb_h;
                                ly = j->lh - 1 - fx * j->lh / j->fb_w;
                        } else if (j->rotation == 180) {
                                lx = j->lw - 1 - fx * j->lw / j->fb_w;
                                ly = j->lh - 1 - fy * j->lh / j->fb_h;
                        } else if (j->rotation == 270) {
                                lx = j->lw - 1 - fy * j->lw / j->fb_h;
                                ly = fx * j->lh / j->fb_w;
                        } else {
                                lx = fx * j->lw / j->fb_w;
                                ly = fy * j->lh / j->fb_h;
                        }
                        uint32_t v = pack_px(j,
                                j->canvas[(size_t)ly * j->lw + lx]);
                        if (j->bpp == 4)
                                ((uint32_t *)row)[fx + j->xo] = v;
                        else
                                ((uint16_t *)row)[fx + j->xo] = (uint16_t)v;
                }
        }
}

static void disp_present(void) {
        if (!D.mem || D.fb_w <= 0) return;
        PresentJob j;
        j.fb_w = D.fb_w; j.fb_h = D.fb_h; j.pitch = D.pitch;
        j.bpp = D.bpp; j.xo = D.vi.xoffset; j.yo = D.vi.yoffset;
        j.lw = D.lw; j.lh = D.lh; j.rotation = D.rotation;
        j.canvas = D.canvas; j.lx_of = D.lx_of; j.ly_of = D.ly_of;
        j.enc_r = D.enc_r; j.enc_g = D.enc_g;
        j.enc_b = D.enc_b; j.enc_a = D.enc_a;
        if ((D.rotation == 90 || D.rotation == 270) &&
            D.fb_w == D.lh && D.fb_h == D.lw) {
                present_rot_tiled(&j, D.mem);
                return;
        }
        if (D.rotation == 0 && D.fb_w == D.lw && D.fb_h == D.lh &&
            D.bpp == 4 && D.pitch == D.fb_w * 4 &&
            D.vi.red.offset == 16 && D.vi.green.offset == 8 &&
            D.vi.blue.offset == 0) {
                for (int y = 0; y < D.fb_h; y++) {
                        uint32_t *src = D.canvas + (size_t)y * D.lw;
                        uint32_t *dst = (uint32_t *)(D.mem +
                                (size_t)(y + j.yo) * D.pitch) + j.xo;
                        memcpy(dst, src, (size_t)D.fb_w * 4);
                }
                return;
        }
        present_generic(&j, D.mem);
}

/* ============================== INPUT (shm) =============================== */
enum { K_LEFT, K_RIGHT, K_UP, K_DOWN, K_A, K_B, K_L1, K_R1, K_X, K_Y,
       K_START, K_SELECT, KCOUNT };
static int key_bits[KCOUNT] = {7, 5, 2, 3, 13, 14, 10, 11, 12, 15, 1, 0};
static const char *key_names[KCOUNT] = {"LEFT", "RIGHT", "UP", "DOWN", "A",
        "B", "L1", "R1", "X", "Y", "START", "SELECT"};

static volatile uint32_t *keys_shm;

#define KEYMAP_FILE "/mnt/sdcard/frogui/keymap.txt"
static void load_keymap(void) {
        FILE *f = fopen(KEYMAP_FILE, "r");
        if (!f) return;
        char line[64], name[32];
        int bit;
        while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%31[^=]=%d", name, &bit) != 2) continue;
                for (int i = 0; i < KCOUNT; i++)
                        if (!strcmp(name, key_names[i]) && bit >= 0 && bit < 32)
                                key_bits[i] = bit;
        }
        fclose(f);
}

static void input_init(void) {
#ifndef HOST_TEST
        key_t key = ftok("/tmp/joy_key", 'a');
        if (key == (key_t)-1) return;
        int id = shmget(key, 4, 0666);
        if (id < 0) return;
        void *m = shmat(id, NULL, 0);
        if (m == (void *)-1) return;
        keys_shm = (volatile uint32_t *)m;
#endif
}

static void input_close(void) {
#ifndef HOST_TEST
        if (keys_shm) shmdt((void *)keys_shm);
        keys_shm = NULL;
#endif
}

static uint32_t raw_mask(void) {
        if (!keys_shm) return 0;
        uint32_t in = *keys_shm & 0xFFFFu, out = 0;
        for (int i = 0; i < KCOUNT; i++)
                if (in & (1u << key_bits[i])) out |= 1u << i;
        return out;
}

/* edge detect + key repeat for the 4 directions */
typedef struct { float held; } Rep;
static Rep reps[KCOUNT];
static uint32_t prev_mask;

#define REPEAT_FIRST 0.34f
#define REPEAT_NEXT  0.10f

static uint32_t poll_keys(float dt) {
        uint32_t cur = raw_mask();
        uint32_t pressed = cur & ~prev_mask;
        for (int i = 0; i < KCOUNT; i++) {
                if (cur & (1u << i)) {
                        reps[i].held += dt;
                        bool dir = (i == K_LEFT || i == K_RIGHT ||
                                    i == K_UP || i == K_DOWN);
                        if (dir && reps[i].held >= REPEAT_FIRST) {
                                float over = reps[i].held - REPEAT_FIRST;
                                if (fmodf(over, REPEAT_NEXT) < dt * 1.5f)
                                        pressed |= (1u << i);
                        }
                } else {
                        reps[i].held = 0;
                }
        }
        prev_mask = cur;
        return pressed;
}

static bool key_down(int k) { return keys_shm && (raw_mask() & (1u << k)); }

/* ============================ DRAW PRIMITIVES ============================= */
static inline uint32_t mkcol(uint32_t a, uint32_t rgb) {
        return ((a & 255u) << 24) | (rgb & 0xFFFFFFu);
}

static void blend_px(int x, int y, uint32_t c) {
        if (x < 0 || y < 0 || x >= LW || y >= LH) return;
        uint32_t a = (c >> 24) & 255;
        if (!a) return;
        uint32_t *d = &D.canvas[(size_t)y * LW + x];
        if (a >= 255) { *d = 0xFF000000u | (c & 0xFFFFFFu); return; }
        uint32_t dr = (*d >> 16) & 255, dg = (*d >> 8) & 255, db = *d & 255;
        uint32_t sr = (c >> 16) & 255, sg = (c >> 8) & 255, sb = c & 255;
        *d = 0xFF000000u |
             ((((dr * (255 - a) + sr * a) / 255) & 255) << 16) |
             ((((dg * (255 - a) + sg * a) / 255) & 255) << 8) |
              ((db * (255 - a) + sb * a) / 255);
}

static void fill_rect(int x, int y, int w, int h, uint32_t c) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > LW) w = LW - x;
        if (y + h > LH) h = LH - y;
        if (w <= 0 || h <= 0) return;
        for (int yy = y; yy < y + h; yy++)
                for (int xx = x; xx < x + w; xx++)
                        blend_px(xx, yy, c);
}

/* vertical gradient inside a rounded rect; returns row insets via helper */
static void round_span(int x0, int y0, int w, int h, int r, int y,
                       int *sx, int *ex) {
        *sx = x0; *ex = x0 + w - 1;
        if (r <= 0) return;
        if (y < y0 + r) {
                int dy = (y0 + r - 1) - y;
                if (dy >= r) dy = r - 1;
                int in = r - (int)(sqrt((double)r * r - (double)dy * dy) + 0.5);
                *sx += in; *ex -= in;
        } else if (y >= y0 + h - r) {
                int dy = y - (y0 + h - r);
                if (dy >= r) dy = r - 1;
                int in = r - (int)(sqrt((double)r * r - (double)dy * dy) + 0.5);
                *sx += in; *ex -= in;
        }
        if (*ex < *sx) { int t = *sx; *sx = *ex; *ex = t; }
}

static void round_rect(int x, int y, int w, int h, int r, uint32_t c) {
        if (w <= 0 || h <= 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        for (int yy = y; yy < y + h; yy++) {
                int sx, ex;
                round_span(x, y, w, h, r, yy, &sx, &ex);
                for (int xx = sx; xx <= ex; xx++) blend_px(xx, yy, c);
        }
}

static void grad_round_rect(int x, int y, int w, int h, int r,
                            uint32_t c1, uint32_t c2) {
        if (w <= 0 || h <= 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        for (int yy = y; yy < y + h; yy++) {
                int sx, ex;
                round_span(x, y, w, h, r, yy, &sx, &ex);
                float t = (h > 1) ? (float)(yy - y) / (float)(h - 1) : 0.0f;
                uint32_t a1 = (c1 >> 24) & 255, a2 = (c2 >> 24) & 255;
                uint32_t ca = (uint32_t)(a1 + (a2 - a1) * t);
                uint32_t cr = (uint32_t)(((c1 >> 16) & 255) +
                        (((int)((c2 >> 16) & 255) - (int)((c1 >> 16) & 255)) * t));
                uint32_t cg = (uint32_t)(((c1 >> 8) & 255) +
                        (((int)((c2 >> 8) & 255) - (int)((c1 >> 8) & 255)) * t));
                uint32_t cb = (uint32_t)((c1 & 255) +
                        (((int)(c2 & 255) - (int)(c1 & 255)) * t));
                uint32_t c = (ca << 24) | (cr << 16) | (cg << 8) | cb;
                for (int xx = sx; xx <= ex; xx++) blend_px(xx, yy, c);
        }
}

/* --------------------------------- text ----------------------------------- */
static int glyph_index(unsigned char c) {
        if (c >= 0x20 && c <= 0x7E) return c - 0x20;
        if (c == 0x7F) return 95;
        if (c == 0x80) return 96;
        if (c == 0x81) return 97;
        return 0;
}

static const Glyph *font_for(int size) {
        if (size <= 16) return font_b14;
        if (size <= 24) return font_b20;
        if (size <= 36) return font_b30;
        return font_b42;
}

static int text_w(int size, const char *s) {
        const Glyph *f = font_for(size);
        int w = 0;
        for (; *s; s++) w += f[glyph_index((unsigned char)*s)].adv;
        return w;
}

static void draw_text(int size, const char *s, int x, int y, uint32_t c) {
        const Glyph *f = font_for(size);
        for (; *s; s++) {
                const Glyph *g = &f[glyph_index((unsigned char)*s)];
                if (g->w && g->bits) {
                        int gy = y + g->oy;
                        for (int ry = 0; ry < g->h; ry++) {
                                const unsigned char *row = g->bits +
                                        (size_t)ry * ((g->w + 1) / 2);
                                for (int rx = 0; rx < g->w; rx++) {
                                        unsigned a = (rx & 1) ?
                                                (row[rx / 2] & 15) :
                                                (row[rx / 2] >> 4);
                                        if (a)
                                                blend_px(x + g->ox + rx,
                                                         gy + ry,
                                                         mkcol(a * 17, c));
                                }
                        }
                }
                x += g->adv;
        }
}

static void text_c(int size, const char *s, int cx, int y, uint32_t c) {
        draw_text(size, s, cx - text_w(size, s) / 2, y, c);
}

static void text_r(int size, const char *s, int rx, int y, uint32_t c) {
        draw_text(size, s, rx - text_w(size, s), y, c);
}

/* truncate with ellipsis so text never overflows maxw */
static void text_fit(int size, const char *s, int x, int y, int maxw,
                     uint32_t c) {
        if (text_w(size, s) <= maxw) { draw_text(size, s, x, y, c); return; }
        char buf[128];
        int n = 0;
        const unsigned char *p = (const unsigned char *)s;
        while (*p && n < (int)sizeof(buf) - 4) {
                buf[n++] = (char)*p;
                buf[n] = 0;
                char tmp[132];
                snprintf(tmp, sizeof(tmp), "%s%c", buf, (char)0x80);
                if (text_w(size, tmp) > maxw) { n--; buf[n] = 0; break; }
                p++;
        }
        buf[n] = 0;
        if (n < 0) n = 0;
        snprintf(buf + n, sizeof(buf) - n, "%c", (char)0x80);
        draw_text(size, buf, x, y, c);
}

/* ------------------------- fx timers (kept minimal) ----------------------- */
static void update_fx(float dt) {
        if (press_t > 0) press_t -= dt;      /* button press-down animation   */
        if (shop_flash_t > 0) shop_flash_t -= dt;   /* denied-buy feedback    */
        for (int i = 0; i < MAX_TOAST; i++)
                if (toasts[i].t > 0) toasts[i].t -= dt;
}

static void draw_toasts(void) {
        int y = 84;
        for (int i = 0; i < MAX_TOAST; i++) {
                if (toasts[i].t <= 0) continue;
                int w1 = text_w(14, toasts[i].head);
                int w2 = text_w(20, toasts[i].txt);
                int pw = (w1 > w2 ? w1 : w2) + 44;
                if (pw > 500) pw = 500;
                int px = LW / 2 - pw / 2;
                round_rect(px, y, pw, 58, 14, mkcol(235, 0x10131C));
                fill_rect(px, y + 10, 4, 38, mkcol(255, toasts[i].col &
                                                   0xFFFFFFu));
                draw_text(14, toasts[i].head, px + 20, y + 9,
                          mkcol(255, toasts[i].col & 0xFFFFFFu));
                draw_text(20, toasts[i].txt, px + 20, y + 27, C_TEXT);
                y += 66;
        }
}

/* =============================== BACKDROP ================================= */
static void draw_bg(void) {
        for (int y = 0; y < LH; y++) {
                uint32_t t = (uint32_t)(y * 255 / LH);
                uint32_t r = (0x16 * (255 - t) + 0x0F * t) / 255;
                uint32_t g = (0x1B * (255 - t) + 0x12 * t) / 255;
                uint32_t b = (0x2C * (255 - t) + 0x20 * t) / 255;
                uint32_t c = 0xFF000000u | (r << 16) | (g << 8) | b;
                for (int x = 0; x < LW; x++) D.canvas[(size_t)y * LW + x] = c;
        }
}

/* ================================ LAYOUT ================================== */
#define BTN_X 70
#define BTN_Y 148
#define BTN_W 310
#define BTN_H 225
#define SHOP_W 520

static float shop_panel_x(void) {
        float e = 1.0f - (1.0f - shop_t) * (1.0f - shop_t) * (1.0f - shop_t);
        return (float)LW - e * (float)SHOP_W;
}

/* =============================== TOP / BOTTOM ============================= */
static void draw_hud(void) {
        char buf[48];
        fmt_num(st.money, true, buf, sizeof(buf));
        draw_text(30, buf, 28, 12, C_GREEN);
        fmt_num(cps(), false, buf, sizeof(buf));
        char rate[64];
        snprintf(rate, sizeof(rate), "+%s PER SECOND", buf);
        draw_text(14, rate, 30, 52, C_DIM);
        draw_text(14, "CLICK POWER", 30, 76, C_DIM);
        fmt_num(click_power(), true, buf, sizeof(buf));
        char pw[64];
        snprintf(pw, sizeof(pw), "+%s", buf);
        draw_text(20, pw, 30 + text_w(14, "CLICK POWER") + 8, 70, C_TEXT);

        /* goal panel */
        int gx = 566, gy = 12, gw = 272, gh = 64;
        round_rect(gx + 3, gy + 5, gw, gh, 16, mkcol(90, 0x000000));
        round_rect(gx, gy, gw, gh, 16, C_PANEL);
        draw_text(14, "GOAL", gx + 18, gy + 10, C_DIM);
        float k = goal_frac();
        if (st.won) {
                draw_text(30, "\x7f", gx + gw - 44, gy + 12, C_GOLD);
        } else {
                char pct[16];
                if (k >= 0.01f)
                        snprintf(pct, sizeof(pct), "%d%%",
                                 (int)(k * 100.0f + 0.5f));
                else if (k >= 0.0001f)
                        snprintf(pct, sizeof(pct), "%.2f%%", k * 100.0f);
                else
                        snprintf(pct, sizeof(pct), "0%%");
                text_r(14, pct, gx + gw - 18, gy + 12, C_DIM);
        }
        fmt_num(GOAL, true, buf, sizeof(buf));
        draw_text(20, buf, gx + 18, gy + 30, C_TEXT);
        round_rect(gx + 18, gy + gh - 16, gw - 36, 7, 3, mkcol(255, 0x2A3040));
        int fw = (int)((gw - 36) * k);
        if (fw > 0)
                round_rect(gx + 18, gy + gh - 16, fw, 7, 3, C_TEAL);

        /* bottom bar */
        int bx = 16, by = 422, bw = LW - 32, bh = 42;
        round_rect(bx, by, bw, bh, 14, C_PANEL);
        draw_text(14, "A CLICK \x81 HOLD A AUTO \x81 X SHOP \x81 "
                  "START MENU", bx + 18, by + 13, C_DIM);
        char tr[32];
        snprintf(tr, sizeof(tr), "TROPHIES %d/%d",
                 __builtin_popcount(st.ach), N_ACH);
        char stars[16];
        snprintf(stars, sizeof(stars), "\x7f%d", st.stars);
        int sw = text_w(20, stars);
        int tw = text_w(14, tr);
        draw_text(14, tr, bx + bw - 18 - sw - 18 - tw, by + 14, C_DIM);
        draw_text(20, stars, bx + bw - 18 - sw, by + 9, C_GOLD);
}

/* ================================ BUTTON ================================== */
static void draw_button(void) {
        int off = press_t > 0 ? 4 : 0;
        int x = BTN_X, y = BTN_Y + off;
        round_rect(x + 6, y + 10, BTN_W, BTN_H, 30, mkcol(80, 0x000000));
        grad_round_rect(x, y, BTN_W, BTN_H, 30, C_ORANGE2, C_ORANGE);
        /* darker rim */
        round_rect(x - 3, y - 3, BTN_W + 6, BTN_H + 6, 33,
                   mkcol(70, 0xB96A1F));
        draw_text(30, "CLICK!", x + BTN_W / 2 - text_w(30, "CLICK!") / 2,
                  y + BTN_H / 2 - 34, C_WHITE);
        char buf[32];
        fmt_num(click_power(), true, buf, sizeof(buf));
        char sub[48];
        snprintf(sub, sizeof(sub), "+%s", buf);
        draw_text(20, sub, x + BTN_W / 2 - text_w(20, sub) / 2,
                  y + BTN_H / 2 + 10, mkcol(220, 0xFFFFFF));
}

/* ================================= SHOP =================================== */
/* v1.2 fast path: the panel chrome (shadow, body, teal edge, titles,
 * dividers) never changes while the shop is open, so it is rendered once
 * into shop_cache[] and blitted every frame.  Per frame only the money
 * readout, the rows, the scrollbar and the prestige strip are drawn, and
 * the dark dim layer covers just the strip that stays visible (left of the
 * panel) instead of the whole screen.  On the C3100 this removes roughly
 * two thirds of the per-frame pixel work while shopping. */
#define SHOP_CACHE_W (SHOP_W + 24)        /* panel + its left shadow        */
#define SHOP_CACHE_X (LW - SHOP_CACHE_W)  /* snapshot origin in the canvas  */
static uint32_t *shop_cache;
static bool shop_cache_ok;

/* draws the static chrome into the canvas (also used by the live path) and
 * snapshots it into shop_cache[] when the buffer is available and the panel
 * is fully open at its resting position */
static void shop_cache_build(int px) {
        int w = SHOP_W, h = LH;
        if (px > -30)
                round_rect(px - 24, 8, w, h - 16, 24, mkcol(80, 0x000000));
        round_rect(px, 0, w, h, 0, C_PANEL);
        fill_rect(px, 0, 4, h, C_TEAL);
        draw_text(30, "SHOP", px + 24, 14, C_WHITE);
        draw_text(14, "A BUY \x81 B CLOSE", px + 24, 46, C_DIM);
        fill_rect(px, 62, w, 2, mkcol(70, 0x000000));
        fill_rect(px, 408, w, 2, mkcol(70, 0x000000));
        if (!shop_cache || px != LW - SHOP_W) return;
        for (int y = 0; y < LH; y++)
                memcpy(&shop_cache[(size_t)y * SHOP_CACHE_W],
                       &D.canvas[(size_t)y * LW + SHOP_CACHE_X],
                       (size_t)SHOP_CACHE_W * 4);
        shop_cache_ok = true;
}

static void shop_ensure_visible(void) {
        if (shop_sel >= N_ITEMS) return;      /* prestige strip is fixed */
        float ry = shop_sel * 54.0f;
        if (ry - shop_scroll < 0) shop_scroll = ry;
        if (ry + 54 - shop_scroll > 332) shop_scroll = ry + 54 - 332;
}

static void draw_shop(void) {
        if (shop_t <= 0.001f) return;
        int px = (int)shop_panel_x();
        int dim = (int)(110 * shop_t);
        if (shop_cache_ok && px != LW - SHOP_W) shop_cache_ok = false;
        if (!shop_cache_ok) {
                if (!shop_cache)
                        shop_cache = malloc(sizeof(uint32_t) *
                                            (size_t)SHOP_CACHE_W * LH);
                fill_rect(0, 0, LW, LH, mkcol((uint32_t)dim, 0x000000));
                shop_cache_build(px);     /* live frame; snapshots chrome */
        } else {
                /* fast path: dim only the visible strip + blit the panel */
                fill_rect(0, 0, SHOP_CACHE_X, LH,
                          mkcol((uint32_t)dim, 0x000000));
                for (int y = 0; y < LH; y++)
                        memcpy(&D.canvas[(size_t)y * LW + SHOP_CACHE_X],
                               &shop_cache[(size_t)y * SHOP_CACHE_W],
                               (size_t)SHOP_CACHE_W * 4);
        }
        if (px >= LW) return;
        int w = SHOP_W;

        char buf[48];
        fmt_num(st.money, true, buf, sizeof(buf));
        text_r(20, buf, px + w - 24, 20, C_GREEN);

        /* rows */
        int max_scroll = N_ITEMS * 54 - 332;
        if (max_scroll < 0) max_scroll = 0;
        if (shop_scroll > max_scroll) shop_scroll = (float)max_scroll;
        for (int i = 0; i < N_ITEMS; i++) {
                int ry = 70 + (int)((float)i * 54.0f - shop_scroll);
                if (ry < 64 || ry + 54 > 406) continue;   /* clipa na lista */
                bool sel = (shop_sel == i);
                bool flash = (shop_flash_t > 0 && shop_row_flash == i);
                if (sel) {
                        round_rect(px + 8, ry + 1, w - 16, 52, 12,
                                   mkcol(200, 0xFFFFFF));
                        round_rect(px + 11, ry + 4, w - 22, 46, 10,
                                   flash ? mkcol(220, 0x5A1A1A) : C_PANEL2);
                } else if (flash) {
                        round_rect(px + 11, ry + 4, w - 22, 46, 10,
                                   mkcol(160, 0x5A1A1A));
                }
                round_rect(px + 20, ry + 9, 36, 36, 10,
                           0xFF000000u | ITEM_COL[i]);
                char tag[4];
                snprintf(tag, sizeof(tag), "%s", ITEM_TAG[i]);
                text_c(14, tag, px + 38, ry + 20, 0xFF10131C);
                int name_max = w - 214;
                text_fit(20, ITEM_NAME[i], px + 64, ry + 8, name_max, C_TEXT);
                char sub[64];
                if (ITEM_ISCLICK[i]) {
                        char v[24];
                        fmt_num(ITEM_VLR[i] * star_mult(), true, v, sizeof(v));
                        snprintf(sub, sizeof(sub), "LV %d  +%s / CLICK",
                                 st.lv[i], v);
                } else {
                        char v[24];
                        fmt_num(ITEM_VLR[i] * star_mult(), true, v, sizeof(v));
                        snprintf(sub, sizeof(sub), "LV %d  +%s / SEC",
                                 st.lv[i], v);
                }
                draw_text(14, sub, px + 64, ry + 30, C_DIM);
                char cost[24];
                fmt_num(item_cost(i), true, cost, sizeof(cost));
                bool afford = st.money >= item_cost(i);
                text_r(20, cost, px + w - 24, ry + 16,
                       afford ? C_GREEN : mkcol(170, 0xFF5A5A));
        }
        /* scrollbar */
        if (max_scroll > 0) {
                int track_h = 332;
                int th = track_h * 332 / (N_ITEMS * 54);
                int ty = 70 + (int)(shop_scroll / max_scroll *
                                    (track_h - th));
                fill_rect(px + w - 8, ty, 4, th, mkcol(90, 0xFFFFFF));
        }

        /* prestige strip - divider line already lives in the cached chrome */
        bool sel = (shop_sel == N_ITEMS);
        bool unlocked = st.money >= GOAL || st.earned >= GOAL;
        if (sel) {
                round_rect(px + 8, 411, w - 16, 62, 12, mkcol(200, 0xFFFFFF));
                round_rect(px + 11, 414, w - 22, 56, 10, C_PANEL2);
        }
        draw_text(30, "\x7f", px + 20, 424, C_GOLD);
        draw_text(20, "PRESTIGE", px + 62, 420, C_TEXT);
        char sub[80];
        int gain = prestige_gain();
        if (unlocked) {
                snprintf(sub, sizeof(sub),
                         "RESET FOR +%d STAR(S) \x81 +25%% EACH", gain);
        } else {
                snprintf(sub, sizeof(sub), "REACH $1.00T TO UNLOCK");
        }
        draw_text(14, sub, px + 62, 446, C_DIM);
        if (unlocked) {
                char g[24];
                snprintf(g, sizeof(g), "+%d \x7f", gain);
                text_r(20, g, px + w - 24, 430, C_GOLD);
        } else {
                text_r(14, "LOCKED", px + w - 24, 436,
                       mkcol(170, 0xFF5A5A));
        }
}

/* ============================ OVERLAY SCREENS ============================= */
static void draw_pause(void) {
        fill_rect(0, 0, LW, LH, mkcol(140, 0x000000));
        int pw = 380, ph = 250;
        int px = LW / 2 - pw / 2, py = LH / 2 - ph / 2;
        round_rect(px + 5, py + 9, pw, ph, 20, mkcol(90, 0x000000));
        round_rect(px, py, pw, ph, 20, C_PANEL);
        text_c(30, "PAUSED", LW / 2, py + 22, C_WHITE);
        const char *items[2] = { "RESUME", "SAVE & QUIT" };
        for (int i = 0; i < 2; i++) {
                int iy = py + 92 + i * 52;
                if (menu_sel == i) {
                        int tw2 = text_w(20, items[i]);
                        round_rect(LW / 2 - tw2 / 2 - 26, iy - 9, tw2 + 52,
                                   42, 12, mkcol(70, 0x2BD9C8));
                        text_c(20, items[i], LW / 2, iy, C_WHITE);
                } else {
                        text_c(20, items[i], LW / 2, iy, C_DIM);
                }
        }
        text_c(14, "A SELECT \x81 B BACK", LW / 2, py + ph - 32, C_DIM);
}

static void draw_win(void) {
        fill_rect(0, 0, LW, LH, mkcol(150, 0x000000));
        int pw = 560, ph = 250;
        int px = LW / 2 - pw / 2, py = LH / 2 - ph / 2;
        round_rect(px + 5, py + 9, pw, ph, 20, mkcol(90, 0x000000));
        round_rect(px, py, pw, ph, 20, C_PANEL);
        text_c(42, "GOAL REACHED!", LW / 2, py + 24, C_GOLD);
        char buf[48];
        fmt_num(st.earned, true, buf, sizeof(buf));
        char line[80];
        snprintf(line, sizeof(line), "YOU EARNED %s IN TOTAL", buf);
        text_c(20, line, LW / 2, py + 96, C_WHITE);
        text_c(14, "PRESTIGE IN THE SHOP TO KEEP GROWING", LW / 2,
               py + 136, C_DIM);
        text_c(20, "PRESS A", LW / 2, py + 180, C_TEAL);
}

static void draw_prestige_confirm(void) {
        fill_rect(0, 0, LW, LH, mkcol(150, 0x000000));
        int pw = 480, ph = 230;
        int px = LW / 2 - pw / 2, py = LH / 2 - ph / 2;
        round_rect(px + 5, py + 9, pw, ph, 20, mkcol(90, 0x000000));
        round_rect(px, py, pw, ph, 20, C_PANEL);
        text_c(30, "PRESTIGE?", LW / 2, py + 22, C_WHITE);
        char sub[80];
        snprintf(sub, sizeof(sub), "GAIN +%d STAR(S) \x81 +25%% EACH",
                 prestige_gain());
        text_c(20, sub, LW / 2, py + 88, C_GOLD);
        text_c(14, "MONEY AND UPGRADES RESET \x81 TROPHIES STAY",
               LW / 2, py + 126, C_DIM);
        text_c(20, "A CONFIRM \x81 B CANCEL", LW / 2, py + 176, C_TEAL);
}

/* ================================= TITLE ================================== */
static void draw_title(void) {
        draw_text(42, GAME_NAME, LW / 2 - text_w(42, GAME_NAME) / 2, 96,
                  C_WHITE);
        draw_text(14, "A TINY CLICKING EMPIRE", LW / 2 -
                  text_w(14, "A TINY CLICKING EMPIRE") / 2, 156, C_DIM);
        int pw = 340, ph = 60;
        int px = LW / 2 - pw / 2, py = 216;
        round_rect(px, py, pw, ph, 16, C_PANEL);
        char buf[48];
        fmt_num(st.best, true, buf, sizeof(buf));
        char line[64];
        snprintf(line, sizeof(line), "BEST  %s", buf);
        text_c(20, line, LW / 2, py + 18, C_GREEN);
        draw_text(30, "PRESS A TO START",
                  LW / 2 - text_w(30, "PRESS A TO START") / 2, 330, C_TEAL);
        text_c(14, "SF3000 NATIVE \x81 START QUITS TO DESKTOP", LW / 2,
               442, C_DIM);
}

/* ================================ SELFTEST ================================ */
static int st_fail;
static void st_check(bool ok, const char *name) {
        printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) st_fail++;
}

static int run_selftest(void) {
        char buf[64];
        fmt_num(0, true, buf, sizeof(buf));
        st_check(!strcmp(buf, "$0"), "fmt $0");
        fmt_num(999, true, buf, sizeof(buf));
        st_check(!strcmp(buf, "$999"), "fmt $999");
        fmt_num(1000, true, buf, sizeof(buf));
        st_check(!strcmp(buf, "$1.00K"), "fmt $1.00K");
        fmt_num(1234, true, buf, sizeof(buf));
        st_check(!strcmp(buf, "$1.23K"), "fmt $1.23K");
        fmt_num(999999, true, buf, sizeof(buf));
        st_check(!strcmp(buf, "$1.00M"), "fmt $1.00M");
        fmt_num(1.2345e6, true, buf, sizeof(buf));
        st_check(!strcmp(buf, "$1.23M"), "fmt $1.23M");
        fmt_num(1e12, true, buf, sizeof(buf));
        st_check(!strcmp(buf, "$1.00T"), "fmt $1.00T");
        fmt_num(4e6, false, buf, sizeof(buf));
        st_check(!strcmp(buf, "4.00M"), "fmt 4.00M");

        bool mono = true;
        for (int i = 0; i < N_ITEMS; i++)
                for (int l = 0; l < 60; l++) {
                        st.lv[i] = l;
                        if (item_cost(i) >= item_cost(i) * ITEM_G[i] - 1e-9)
                                mono = false;
                }
        st_check(mono, "cost monotonic");
        for (int i = 0; i < N_ITEMS; i++) st.lv[i] = 0;
        st_check(fabs(item_cost(0) - 15.0) < 1e-9, "cost THUMB lv0 = 15");

        st_check(fabs(click_power() - 1.0) < 1e-9, "click power base 1");
        st.lv[0] = 2;
        st_check(fabs(click_power() - 3.0) < 1e-9, "click power +2 thumb");
        st.lv[0] = 0; st.stars = 1;
        st_check(fabs(click_power() - 1.25) < 1e-9, "star mult 1.25x");
        st.stars = 0;
        st.lv[3] = 2;
        st_check(fabs(cps() - 1.0) < 1e-9, "cps 2 mice = 1/s");
        st.lv[3] = 0;

        Save old = st;
        st.earned = 1e12; st_check(prestige_gain() == 1, "prestige 1T = 1");
        st.earned = 1e18; st_check(prestige_gain() == 2, "prestige 1e18 = 2");
        st.earned = 1e21; st_check(prestige_gain() == 3, "prestige 1e21 = 3");
        st.earned = 0;

        /* goal bar is LINEAR: $33 of $1T must be ~0 (hardware report) */
        st.earned = 33.0;
        st_check(goal_frac() < 1e-8f, "goal bar linear: 33 of 1T ~ 0");
        st.earned = GOAL * 0.5;
        st_check(fabs((double)goal_frac() - 0.5) < 1e-6, "goal bar half = 0.5");
        st.earned = 0;

        /* save/load roundtrip through a temp file */
        setenv("CLICKER3000_SAVE", "/tmp/c3k_test.sav", 1);
        find_save_path();
        default_state();
        st.money = 12345.678; st.earned = 999999.5; st.best = 1e6;
        st.clicks = 4242; st.stars = 3; st.ach = 0b10101010; st.won = 1;
        for (int i = 0; i < N_ITEMS; i++) st.lv[i] = i * 7 + 1;
        save_now();
        default_state();
        load_now();
        bool ok = fabs(st.money - 12345.678) < 0.01 &&
                  fabs(st.earned - 999999.5) < 0.01 &&
                  fabs(st.best - 1e6) < 0.01 && st.clicks == 4242 &&
                  st.stars == 3 && st.ach == 0b10101010 && st.won == 1;
        for (int i = 0; i < N_ITEMS; i++)
                if (st.lv[i] != i * 7 + 1) ok = false;
        st_check(ok, "save/load roundtrip");

        /* v1.2 migration: a legacy save next to the binary is adopted on
         * load and removed after the first save in the new location.
         * Skipped when a real legacy save already exists (never touch it). */
        {
                char lg[512];
                legacy_save_path(lg, sizeof(lg));
                struct stat sb0;
                bool can = lg[0] && stat(lg, &sb0) != 0;
                if (can) {
                        FILE *lf = fopen(lg, "w");
                        if (lf) {
                                fprintf(lf, "clicker3000 v1\nmoney 777\n"
                                            "earned 888\n");
                                fclose(lf);
                        } else can = false;
                }
                if (can) {
                        setenv("CLICKER3000_SAVE", "/tmp/c3k_migr.sav", 1);
                        find_save_path();
                        unlink("/tmp/c3k_migr.sav");
                        default_state();
                        load_now();
                        st_check(fabs(st.money - 777.0) < 1e-9 &&
                                 fabs(st.earned - 888.0) < 1e-9,
                                 "migration: legacy save adopted");
                        st.money = 999.0;
                        save_now();
                        struct stat sb;
                        bool gone = stat(lg, &sb) != 0;
                        FILE *nf = fopen("/tmp/c3k_migr.sav", "r");
                        bool moved = nf != NULL;
                        if (nf) fclose(nf);
                        st_check(gone && moved,
                                 "migration: new save written, legacy removed");
                        unlink("/tmp/c3k_migr.sav");
                } else {
                        st_check(true, "migration: skipped (no exe dir or "
                                       "legacy save already present)");
                }
                setenv("CLICKER3000_SAVE", "/tmp/c3k_test.sav", 1);
                find_save_path();
        }

        /* achievement thresholds */
        default_state();
        do_click();
        st_check((st.ach & 1) && !(st.ach & 2), "ach first click");
        st.clicks = 99;
        do_click();
        st_check(st.ach & 2, "ach 100 clicks");
        default_state();
        add_money(1.5e3);
        st_check((st.ach & 8) != 0, "ach 1K earned");
        add_money(1e12);
        st_check(st.won && (st.ach & 128), "win at goal");

        /* income simulation: 1 hour at 60fps with clicks + purchases */
        default_state();
        double money0 = 0;
        for (int f = 0; f < 3600 * 60; f++) {
                add_money(cps() / 60.0);
                if (f % 60 == 0) do_click();
                if (st.money >= item_cost(3) && st.lv[3] < 50) try_buy(3);
                if (!isfinite(st.money) || st.money < -1e-9) break;
        }
        money0 = st.money;
        st_check(isfinite(money0) && money0 > 1000.0, "sim income grows");
        st_check(st.lv[3] >= 20, "sim bought many mice");

        st = old;
        setenv("CLICKER3000_SAVE", "", 1);

        /* v1.2 shop fast path: a cached-chrome frame must be byte-identical
         * to the live-rendered frame (same state, deterministic drawing) */
        {
                static uint32_t ref[LW * LH];
                default_state();
                D.canvas = malloc((size_t)LW * LH * 4);
                bool can = D.canvas != NULL;
                st_check(can, "shop test canvas alloc");
                if (can) {
                        shop_open = true; shop_t = 1.0f; shop_sel = 3;
                        shop_scroll = 27.0f;
                        shop_flash_t = 0; shop_row_flash = 0; press_t = 0;
                        st.money = 4321.0; st.lv[2] = 3; st.lv[5] = 2;
                        draw_bg(); draw_hud(); draw_button();
                        draw_shop();              /* live path, builds cache */
                        memcpy(ref, D.canvas, sizeof(ref));
                        for (int i = 0; i < LW * LH; i++)
                                D.canvas[i] = 0x12345678;   /* poison */
                        draw_bg(); draw_hud(); draw_button();
                        draw_shop();              /* cached path */
                        st_check(memcmp(ref, D.canvas, sizeof(ref)) == 0,
                                 "shop cache frame == live frame");
                        free(D.canvas); D.canvas = NULL;
                        free(shop_cache); shop_cache = NULL;
                        shop_cache_ok = false;
                        default_state();
                }
        }

        /* display transform: generic vs tiled vs image_viewer oracle.
         * Catches transposed/rotated maps before they reach hardware. */
        {
                static uint32_t cv[LW * LH];
                for (int i = 0; i < LW * LH; i++) {
                        uint32_t x = (uint32_t)i * 2654435761u;
                        x ^= x >> 13;
                        cv[i] = 0x01020304u + x;   /* distinct per pixel */
                }
                D.lw = LW; D.lh = LH;
                D.lx_of = malloc(sizeof(int) * (size_t)(LW > LH ? LW : LH));
                D.ly_of = malloc(sizeof(int) * (size_t)(LW > LH ? LW : LH));
                bool maps_ok = D.lx_of && D.ly_of;
                st_check(maps_ok, "disp maps alloc");
                for (int bi = 0; bi < 2 && maps_ok; bi++) {
                        int bpp = bi == 0 ? 4 : 2;
                        uint32_t er[256], eg[256], eb[256], ea[256];
                        unsigned rl, ro, gl, go, bl, bo, al, ao;
                        if (bpp == 4) { rl = 8; ro = 16; gl = 8; go = 8;
                                        bl = 8; bo = 0;  al = 8; ao = 24; }
                        else          { rl = 5; ro = 11; gl = 6; go = 5;
                                        bl = 5; bo = 0;  al = 0; ao = 0; }
                        for (int v = 0; v < 256; v++) {
                                er[v] = ((v * ((1u << rl) - 1) + 127) / 255) << ro;
                                eg[v] = ((v * ((1u << gl) - 1) + 127) / 255) << go;
                                eb[v] = ((v * ((1u << bl) - 1) + 127) / 255) << bo;
                                ea[v] = al ? (((v * ((1u << al) - 1) + 127) /
                                               255) << ao) : 0;
                        }
                        for (int rot = 0; rot <= 270; rot += 90) {
                                bool port = (rot == 90 || rot == 270);
                                PresentJob j;
                                memset(&j, 0, sizeof(j));
                                j.fb_w = port ? LH : LW;
                                j.fb_h = port ? LW : LH;
                                j.pitch = j.fb_w * bpp;
                                j.bpp = bpp;
                                j.lw = LW; j.lh = LH; j.rotation = rot;
                                j.canvas = cv;
                                j.enc_r = er; j.enc_g = eg;
                                j.enc_b = eb; j.enc_a = ea;
                                D.fb_w = j.fb_w; D.fb_h = j.fb_h;
                                D.rotation = rot;
                                disp_rebuild_maps();
                                j.lx_of = D.lx_of; j.ly_of = D.ly_of;
                                size_t fbsz = (size_t)j.pitch * j.fb_h;
                                unsigned char *m1 = calloc(1, fbsz);
                                unsigned char *m3 = calloc(1, fbsz);
                                present_generic(&j, m1);
                                present_reference(&j, m3);
                                char nm[64];
                                snprintf(nm, sizeof(nm),
                                         "disp rot%d %dbpp generic==ref", rot, bpp);
                                st_check(memcmp(m1, m3, fbsz) == 0, nm);
                                if (port) {
                                        unsigned char *m2 = calloc(1, fbsz);
                                        present_rot_tiled(&j, m2);
                                        snprintf(nm, sizeof(nm),
                                                 "disp rot%d %dbpp tiled==ref",
                                                 rot, bpp);
                                        st_check(memcmp(m2, m3, fbsz) == 0, nm);
                                        free(m2);
                                }
                                free(m1); free(m3);
                        }
                }
                /* padded pitch + nonzero pan offsets, rot90 32bpp */
                if (maps_ok) {
                        PresentJob j;
                        memset(&j, 0, sizeof(j));
                        j.fb_w = LH; j.fb_h = LW;
                        j.pitch = LH * 4 + 64;
                        j.bpp = 4; j.xo = 2; j.yo = 1;
                        j.lw = LW; j.lh = LH; j.rotation = 90;
                        j.canvas = cv;
                        j.enc_r = D.enc_r; j.enc_g = D.enc_g;
                        j.enc_b = D.enc_b; j.enc_a = D.enc_a;
                        D.fb_w = j.fb_w; D.fb_h = j.fb_h; D.rotation = 90;
                        disp_rebuild_maps();
                        j.lx_of = D.lx_of; j.ly_of = D.ly_of;
                        size_t fbsz = (size_t)j.pitch * (j.fb_h + 2);
                        unsigned char *m1 = calloc(1, fbsz);
                        unsigned char *m2 = calloc(1, fbsz);
                        unsigned char *m3 = calloc(1, fbsz);
                        present_generic(&j, m1);
                        present_rot_tiled(&j, m2);
                        present_reference(&j, m3);
                        st_check(memcmp(m1, m3, fbsz) == 0,
                                 "disp rot90 padded generic==ref");
                        st_check(memcmp(m2, m3, fbsz) == 0,
                                 "disp rot90 padded tiled==ref");
                        free(m1); free(m2); free(m3);
                }
                free(D.lx_of); free(D.ly_of);
                D.lx_of = NULL; D.ly_of = NULL; D.fb_w = 0; D.fb_h = 0;
        }

        printf("selftest: %s (%d failures)\n", st_fail ? "FAILED" : "ALL OK",
               st_fail);
        return st_fail ? 1 : 0;
}

/* ============================== PPM / PREVIEW ============================= */
#ifdef HOST_TEST
static void ppm_write(const char *path) {
        FILE *f = fopen(path, "wb");
        if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", LW, LH);
        for (int i = 0; i < LW * LH; i++) {
                uint32_t c = D.canvas[i];
                unsigned char rgb[3] = { (unsigned char)(c >> 16),
                                         (unsigned char)(c >> 8),
                                         (unsigned char)c };
                fwrite(rgb, 1, 3, f);
        }
        fclose(f);
}

static void preview_setup(const char *what) {
        default_state();
        st.money = 2345.67; st.earned = 4.2e10; st.best = 12000;
        st.clicks = 213; st.stars = 2; st.ach = 0b01011111;
        st.lv[0] = 7; st.lv[1] = 3; st.lv[2] = 1; st.lv[3] = 9; st.lv[4] = 2;
        press_t = 0; time_acc = 0;
        if (!strcmp(what, "title")) {
                mode = 0; st.best = 15432; st.stars = 3;
        } else if (!strcmp(what, "game")) {
                mode = 1;
        } else if (!strcmp(what, "shop")) {
                mode = 1; shop_open = true; shop_t = 1.0f; shop_sel = 4;
                st.money = 8123.45; st.lv[5] = 4; st.lv[6] = 2; st.lv[9] = 1;
                st.stars = 5; st.ach = 0b01111111;
        } else if (!strcmp(what, "pause")) {
                mode = 2; menu_sel = 0;
        } else if (!strcmp(what, "win")) {
                mode = 1; st.won = 1; st.earned = 1.234e12; win_show = true;
                st.stars = 6;
        }
}
#endif

/* ================================= MAIN =================================== */
static void game_frame_input(uint32_t pressed) {
        if (mode == 0) {                                   /* title */
                if (pressed & (1u << K_A)) mode = 1;
                if (pressed & (1u << K_START)) g_quit = true;
                return;
        }
        if (mode == 2) {                                   /* pause */
                if (pressed & (1u << K_UP)) menu_sel = 0;
                if (pressed & (1u << K_DOWN)) menu_sel = 1;
                if (pressed & (1u << K_A)) {
                        if (menu_sel == 0) mode = 1;
                        else g_quit = true;
                }
                if (pressed & ((1u << K_B) | (1u << K_START))) mode = 1;
                return;
        }
        /* mode 1: game */
        if (win_show) {
                if (pressed & ((1u << K_A) | (1u << K_B))) win_show = false;
                return;
        }
        if (shop_open) {
                if (prestige_confirm) {
                        if (pressed & (1u << K_A)) {
                                prestige_confirm = false;
                                do_prestige();
                        }
                        if (pressed & ((1u << K_B) | (1u << K_X)))
                                prestige_confirm = false;
                        return;
                }
                if (pressed & (1u << K_UP)) {
                        shop_sel = (shop_sel + N_ITEMS) % (N_ITEMS + 1);
                        shop_ensure_visible();
                }
                if (pressed & (1u << K_DOWN)) {
                        shop_sel = (shop_sel + 1) % (N_ITEMS + 1);
                        shop_ensure_visible();
                }
                if (pressed & (1u << K_A)) {
                        if (shop_sel == N_ITEMS) {
                                if (st.money >= GOAL || st.earned >= GOAL)
                                        prestige_confirm = true;
                        } else {
                                try_buy(shop_sel);
                        }
                }
                if (pressed & ((1u << K_B) | (1u << K_X))) {
                        shop_open = false; shop_t = 0.0f;
                }
                return;
        }
        /* plain gameplay */
        static int hold_frames;
        if (pressed & (1u << K_X)) { shop_open = true; shop_t = 1.0f; return; }
        if (pressed & (1u << K_START)) { mode = 2; menu_sel = 0; return; }
        if (pressed & (1u << K_A)) do_click();
        if (key_down(K_A)) {
                if (++hold_frames >= AUTOCLICK_DIV) {
                        hold_frames = 0;
                        do_click();
                }
        } else {
                hold_frames = 0;
        }
}

static void frame_logic(float dt) {
        if (mode == 1 && !prestige_confirm)
                add_money(cps() * dt);
        update_fx(dt);
        if (mode == 1) {
                st.playtime += dt;
                time_acc += dt;
                if (time_acc >= 30.0f) { time_acc = 0; save_now(); }
        }
}

static void draw_frame(void) {
        if (mode == 0) {
                draw_bg();
                draw_title();
        } else if (mode == 2) {
                draw_bg();
                draw_hud();
                draw_button();
                draw_pause();
                draw_toasts();
        } else {
                draw_bg();
                draw_hud();
                draw_button();
                draw_shop();
                draw_toasts();
                if (prestige_confirm) draw_prestige_confirm();
                if (win_show) draw_win();
        }
        disp_present();
}

#ifndef HOST_TEST
int main(int argc, char **argv) {
        if (argc > 1 && !strcmp(argv[1], "--selftest")) {
                find_save_path();
                setenv("CLICKER3000_SAVE", "/tmp/c3k_selftest.sav", 1);
                find_save_path();
                return run_selftest();
        }
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        if (disp_init() != 0) {
                fprintf(stderr, "clicker3000: out of memory\n");
                return 1;
        }
        if (!D.mem) {
                fprintf(stderr, "clicker3000: no framebuffer available\n");
                free(D.canvas);
                return 1;
        }
        load_keymap();
        input_init();
        if (!keys_shm) {
                draw_bg();
                text_c(20, "NO INPUT FOUND", LW / 2, 200, C_RED);
                text_c(14, "KEY SHM /tmp/joy_key NOT AVAILABLE", LW / 2,
                       240, C_DIM);
                text_c(14, "RETURNING TO LAUNCHER...", LW / 2, 270, C_DIM);
                disp_present();
                sleep(3);
                free(D.canvas);
                if (D.mem) {
                        disp_close();
                }
                return 1;
        }
        find_save_path();
        load_now();
        double t_prev = now_sec();
        while (!sig_quit && !g_quit) {
                double t_now = now_sec();
                float dt = (float)(t_now - t_prev);
                if (dt > 0.05f) dt = 0.05f;
                if (dt <= 0) dt = 0.0001f;
                t_prev = t_now;
                uint32_t pressed = poll_keys(dt);
                game_frame_input(pressed);
                frame_logic(dt);
                draw_frame();
                double rest = t_now + 1.0 / 60.0 - now_sec();
                if (rest > 0) {
                        struct timespec ts = { (time_t)rest,
                                (long)((rest - (double)(time_t)rest) * 1e9) };
                        nanosleep(&ts, NULL);
                }
        }
        save_now();
        input_close();
        disp_close();
        return 0;
}
#else /* HOST_TEST */
int main(int argc, char **argv) {
        if (argc > 1 && !strcmp(argv[1], "--selftest")) {
                setenv("CLICKER3000_SAVE", "/tmp/c3k_selftest.sav", 1);
                find_save_path();
                return run_selftest();
        }
        if (argc > 3 && !strcmp(argv[1], "--preview")) {
                if (disp_init() != 0) return 1;
                preview_setup(argv[2]);
                draw_frame();
                ppm_write(argv[3]);
                disp_close();
                return 0;
        }
        fprintf(stderr, "usage: %s --selftest | --preview title|game|shop|"
                        "pause|win out.ppm\n", argv[0]);
        return 2;
}
#endif
