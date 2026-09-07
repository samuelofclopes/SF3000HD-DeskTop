/* ============================================================
 * mywm.c — compositor MYWM v3.1 (+ taskbar + teclado de sistema)
 *
 * v3.1: FIX CRÍTICO do v3.0 — wnd_activate agora DEVOLVE o
 *       índice final (inseriu por baixo dos teclados, mas o
 *       arrastar/log ainda assumiam "ativa = última da lista" →
 *       arrastar o terminal movia o TECLADO, log mentia o título).
 *       REFLOW estilo GBoard: teclado abre → maximizadas encolhem
 *       para o espaço acima dele; teclado fecha → voltam a crescer.
 *       Maximizar respeita o teclado; apps novas nascem acima dele;
 *       □ no teclado ignorado.
 * v3.0: teclado de sistema (is_keyboard, MYWM_MSG_TEXT, foco).
 * v2.5: debounce; v2.4: PAD_L; v2.2: R1 max/restaurar, START+SELECT.
 *
 * START/≡ = menu | R1 = max/restaurar | START+SELECT sai
 * Log: /mnt/sdcard/mywm.log
 * ============================================================ */
#include "mywm.h"
#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* ===== bits ===== */
#define BIT_SELECT 0
#define BIT_START  3
#define BIT_UP     4
#define BIT_RIGHT  5
#define BIT_DOWN   6
#define BIT_LEFT   7
#define BIT_L      10
#define BIT_R      11
#define BIT_X      12
#define BIT_A      13
#define BIT_B      14
#define BIT_Y      15
#define KEY_MASK ((1u<<BIT_SELECT)|(1u<<BIT_START)|(1u<<BIT_UP)|(1u<<BIT_RIGHT)|\
                  (1u<<BIT_DOWN)|(1u<<BIT_LEFT)|(1u<<BIT_L)|(1u<<BIT_R)|\
                  (1u<<BIT_X)|(1u<<BIT_A)|(1u<<BIT_B)|(1u<<BIT_Y))

static FILE *lg;
#define LOG(...) do{ if(lg){fprintf(lg,__VA_ARGS__);fflush(lg);fsync(fileno(lg));} }while(0)

/* ===== tema ===== */
#define C_WIN_BODY        0x2104
#define C_WIN_TITLE       0x39E7
#define C_WIN_TITLE_INACT 0x4208
#define C_WIN_BORDER      0x7BEF
#define C_BTN_HOVER       0x6B5F
#define C_TEXT_DIM        0xB596
#define C_X_RED           0xF800
#define TITLE_H  30
#define WIN_R    9
#define TASKBAR_H 36
#define C_TASKBAR_BG      0x18E3
#define C_TASKBAR_BTN     0x2104
#define C_TASKBAR_BTN_HOV 0x39E7
#define C_TASKBAR_TXT     0xB596

/* margem esquerda: a moldura do painel tapa os primeiros px do
   lado esquerdo do ecrã — o conteúdo da taskbar recua PAD_L */
#define PAD_L 11

/* altura útil de maximização SEM teclado (com teclado: max_h_now()) */
#define MAX_H (H - 40 - TASKBAR_H)

/* ===== framebuffer ===== */
static int fb_fd, fb_w, fb_h, fb_stride;
static size_t fb_len;
static unsigned short *fb;
static int W, H;
static unsigned short *bb;

static int fb_init(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) < 0) return -1;
    v.xoffset = 0; v.yoffset = 0; v.activate = FB_ACTIVATE_NOW;
    ioctl(fb_fd, FBIOPUT_VSCREENINFO, &v);
    ioctl(fb_fd, FBIOPAN_DISPLAY, &v);
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &f) < 0) return -1;
    fb_w = v.xres; fb_h = v.yres;
    fb_stride = f.line_length / 2;
    fb_len = f.smem_len;
    fb = mmap(NULL, fb_len, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb == MAP_FAILED) return -1;
    if (fb_h > fb_w) { W = fb_h; H = fb_w; } else { W = fb_w; H = fb_h; }
    LOG("fb %dx%d -> %dx%d\n", fb_w, fb_h, W, H);
    return 0;
}

static void present(void)
{
    if (fb_h > fb_w) {
        const int T = 16;
        for (int ty = 0; ty < H; ty += T)
        for (int tx = 0; tx < W; tx += T) {
            int xe = (tx+T < W) ? tx+T : W;
            int ye = (ty+T < H) ? ty+T : H;
            for (int x = tx; x < xe; x++) {
                unsigned short *d = &fb[(size_t)x * fb_stride];
                const unsigned short *sp = &bb[(size_t)ty * W + x];
                int dy_idx = H-1-ty;
                for (int y = ty; y < ye; y++, sp += W, dy_idx--)
                    d[dy_idx] = *sp;
            }
        }
    } else {
        for (int y = 0; y < H; y++)
            memcpy(&fb[(size_t)y * fb_stride], &bb[(size_t)y * W], (size_t)W * 2);
    }
}

/* ===== input ===== */
static volatile uint32_t *shm_keys;
static uint32_t keys_now = 0;

static int input_init(void)
{
    key_t k = ftok("/tmp/joy_key", 'a');
    if (k == -1) return 0;
    int id = shmget(k, 0, 0666);
    if (id == -1) return 0;
    void *p = shmat(id, NULL, SHM_RDONLY);
    if (p == (void *)-1) return 0;
    shm_keys = p;
    usleep(300000);
    LOG("input ok\n");
    return 1;
}

/* ===== desenho base ===== */
static void px(int x, int y, unsigned short c)
{
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H)
        bb[(size_t)y * W + x] = c;
}

static void rect(int x, int y, int w, int h, unsigned short c)
{
    int x0 = x, x1 = x+w, y0 = y, y1 = y+h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int j = y0; j < y1; j++) {
        unsigned short *row = &bb[(size_t)j * W];
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}

static void rect_round_top(int x, int y, int w, int h, int r, unsigned short c)
{
    static int cut_r = -1;
    static int cut_tab[256];
    int use_tab = (r > 0 && r <= 256);
    if (use_tab && r != cut_r) {
        for (int dy = 0; dy < r; dy++) {
            int cc = r - (int)sqrt((double)(r*r - dy*dy)) - 1;
            cut_tab[dy] = cc < 0 ? 0 : cc;
        }
        cut_r = r;
    }
    for (int j = 0; j < h; j++) {
        int off = 0, row_w = w;
        if (j < r) {
            int dy = r - 1 - j;
            int cut;
            if (use_tab) cut = cut_tab[dy];
            else { cut = r - (int)sqrt((double)(r*r - dy*dy)) - 1; if (cut < 0) cut = 0; }
            off = cut; row_w = w - 2*cut;
        }
        int yy = y+j;
        if ((unsigned)yy >= (unsigned)H) continue;
        int x0 = x+off, x1 = x0+row_w;
        if (x0 < 0) x0 = 0;
        if (x1 > W) x1 = W;
        if (x0 >= x1) continue;
        unsigned short *row = &bb[(size_t)yy * W];
        for (int i = x0; i < x1; i++) row[i] = c;
    }
}

static void blit_text(SDL_Surface *s, int dx, int dy)
{
    if (!s) return;
    SDL_LockSurface(s);
    Uint32 *src = (Uint32 *)s->pixels;
    int pitch4 = s->pitch / 4;
    int x0 = dx < 0 ? -dx : 0;
    int y0 = dy < 0 ? -dy : 0;
    int x1 = s->w; if (dx + x1 > W) x1 = W - dx;
    int y1 = s->h; if (dy + y1 > H) y1 = H - dy;
    for (int y = y0; y < y1; y++) {
        const Uint32 *row = src + (size_t)y * pitch4;
        unsigned short *drow = &bb[(size_t)(dy+y) * W];
        for (int x = x0; x < x1; x++) {
            Uint32 p = row[x];
            Uint8 r, g, b, a;
            SDL_GetRGBA(p, s->format, &r, &g, &b, &a);
            if (a > 128)
                drow[dx+x] = (unsigned short)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
        }
    }
    SDL_UnlockSurface(s);
}

/* blit limitado a largura máxima (para a taskbar truncar títulos) */
static void blit_text_clip(SDL_Surface *s, int dx, int dy, int max_w)
{
    if (!s) return;
    SDL_LockSurface(s);
    Uint32 *src = (Uint32 *)s->pixels;
    int pitch4 = s->pitch / 4;
    int x1 = s->w; if (x1 > max_w) x1 = max_w;
    for (int y = 0; y < s->h; y++) {
        const Uint32 *row = src + (size_t)y * pitch4;
        for (int x = 0; x < x1; x++) {
            Uint32 p = row[x];
            Uint8 r, g, b, a;
            SDL_GetRGBA(p, s->format, &r, &g, &b, &a);
            if (a > 128)
                px(dx+x, dy+y, (unsigned short)(((r>>3)<<11)|((g>>2)<<5)|(b>>3)));
        }
    }
    SDL_UnlockSurface(s);
}

/* ===== wallpaper ===== */
static SDL_Surface *g_wall = NULL;

static void draw_wall(void)
{
    if (!g_wall) {
        for (int y = 0; y < H; y++) {
            unsigned short c = (unsigned short)(((y*31/H)<<11) | (y*63/H));
            unsigned short *row = &bb[(size_t)y * W];
            for (int x = 0; x < W; x++) row[x] = c;
        }
        return;
    }
    static unsigned short *pre = NULL;
    if (!pre) {
        pre = malloc((size_t)W * H * 2);
        SDL_LockSurface(g_wall);
        Uint8 *src = (Uint8 *)g_wall->pixels;
        int sb = g_wall->format->BytesPerPixel;
        int pitch = g_wall->pitch;
        int sw = g_wall->w, sh = g_wall->h;
        for (int y = 0; y < H; y++) {
            int sy = y * sh / H;
            for (int x = 0; x < W; x++) {
                int sx = x * sw / W;
                Uint8 *p = src + (size_t)sy * pitch + sx * sb;
                pre[(size_t)y * W + x] =
                    (unsigned short)(((p[2]>>3)<<11)|((p[1]>>2)<<5)|(p[0]>>3));
            }
        }
        SDL_UnlockSurface(g_wall);
        LOG("wallpaper pré-convertida\n");
    }
    memcpy(bb, pre, (size_t)W * H * 2);
}

/* ===== rato ===== */
static int mx = 100, my = 100;
static float vel_x, vel_y;
static Uint32 hold_start;
static int was_moving;

#define ACC_RAMP_MS 130
#define ACC_START   0.9f
#define ACC_MAX     7.0f
#define FRICTION    0.45f

static void update_mouse(void)
{
    int ix = 0, iy = 0;
    if (keys_now & (1u<<BIT_LEFT))  ix--;
    if (keys_now & (1u<<BIT_RIGHT)) ix++;
    if (keys_now & (1u<<BIT_UP))    iy--;
    if (keys_now & (1u<<BIT_DOWN))  iy++;

    if (ix || iy) {
        Uint32 now = SDL_GetTicks();
        if (!was_moving) hold_start = now;
        Uint32 held = now - hold_start;
        float t = held >= ACC_RAMP_MS ? 1.0f : (float)held / ACC_RAMP_MS;
        float k = t * t * t;
        float speed = ACC_START + (ACC_MAX - ACC_START) * k;
        float len = (ix && iy) ? 1.41421356f : 1.0f;
        vel_x = (ix/len) * speed;
        vel_y = (iy/len) * speed;
        was_moving = 1;
    } else {
        vel_x *= FRICTION;
        vel_y *= FRICTION;
        if (vel_x > -0.3f && vel_x < 0.3f) vel_x = 0;
        if (vel_y > -0.3f && vel_y < 0.3f) vel_y = 0;
        was_moving = 0;
    }

    static float fx, fy;
    fx += vel_x; fy += vel_y;
    int dx = (int)fx, dy = (int)fy;
    fx -= dx; fy -= dy;
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (mx > W-4) mx = W-4;
    if (my < 0) my = 0;
    if (my > H-4) my = H-4;
}

static void draw_cursor(void)
{
    for (int i = 0; i < 14; i++)
        for (int j = 0; j <= i/2 && j < 6; j++) {
            px(mx+j,   my+i,   0xFFFF);
            px(mx+j+1, my+i,   0xFFFF);
        }
    for (int i = 0; i < 7; i++) {
        px(mx+i,   my+14-i, 0xFFFF);
        px(mx+i+1, my+14-i, 0xFFFF);
    }
    for (int i = 0; i < 13; i++) px(mx, my+i, 0x0000);
    for (int i = 0; i < 6; i++)  px(mx+i, my+i/2, 0x0000);
}

/* ===== janelas ===== */
#define MAX_WINS 8

typedef struct {
    int x, y, w, h;
    int orig_w, orig_h;
    char title[48];
    int visible, active;
    int is_app;
    int is_kb;                 /* v3: janela de teclado (nunca foco) */
    pid_t app_pid;
    int app_shmid;
    MywmFrame *app_fr;
    /* geometria pedida pelo mywm (max/restaurar/reflow) — aplicada
       quando a app reconecta com o frame novo */
    int pend_geo, pend_x, pend_y, pend_w, pend_h;
    /* app desligou — esperar 150ms por uma possível reconexão
       (resize) antes de remover a janela */
    int closing;
    Uint32 close_t;
    int _bx1, _bx2, _bx3, _bby, _bbw, _bbh;
    char _last_title[48];
    SDL_Surface *_t_title;
    char _last_body[128];
    SDL_Surface *_t_body;
} Wnd;

static Wnd wins[MAX_WINS];
static int n_wins = 0;
static TTF_Font *f_big, *f_med, *f_small;
static SDL_Color col_white = {255,255,255}, col_dim = {160,160,160};

static void draw_x(int cx, int cy)
{ for (int i = 0; i < 11; i++) { px(cx+i, cy+i, 0xFFFF); px(cx+10-i, cy+i, 0xFFFF); } }

static void draw_square(int cx, int cy)
{ for (int i = 0; i < 11; i++) { px(cx+i,cy,0xFFFF); px(cx+i,cy+10,0xFFFF); px(cx,cy+i,0xFFFF); px(cx+10,cy+i,0xFFFF); } }

static void draw_minus(int cx, int cy)
{ for (int i = 0; i < 13; i++) { px(cx+i, cy, 0xFFFF); px(cx+i, cy+1, 0xFFFF); } }

static void draw_window(Wnd *wn)
{
    if (!wn->visible) return;
    int x = wn->x, y = wn->y, w = wn->w, h = wn->h;

    rect_round_top(x+5, y+6, w, h, WIN_R, 0x0000);
    rect_round_top(x, y, w, h, WIN_R, C_WIN_BODY);
    rect_round_top(x, y, w, TITLE_H, WIN_R, wn->active ? C_WIN_TITLE : C_WIN_TITLE_INACT);
    rect(x, y+TITLE_H-1, w, 1, C_WIN_BORDER);

    if (strcmp(wn->title, wn->_last_title) != 0) {
        if (wn->_t_title) SDL_FreeSurface(wn->_t_title);
        wn->_t_title = TTF_RenderText_Blended(f_small, wn->title, col_white);
        strcpy(wn->_last_title, wn->title);
    }
    if (wn->_t_title) blit_text(wn->_t_title, x+12, y+(TITLE_H-14)/2);

    int bw = 28, bh = TITLE_H-10, by = y+5;
    int bx3 = x+w-bw-6, bx2 = bx3-bw-2, bx1 = bx2-bw-2;
    wn->_bx1 = bx1; wn->_bx2 = bx2; wn->_bx3 = bx3;
    wn->_bby = by;  wn->_bbw = bw;  wn->_bbh = bh;

    unsigned short c3 = C_WIN_TITLE;
    if (wn->active && mx >= bx3 && mx < bx3+bw && my >= by && my < by+bh) c3 = C_X_RED;
    rect(bx3, by, bw, bh, c3);
    draw_x(bx3+bw/2-5, by+bh/2-5);

    if (wn->active) {
        unsigned short c2 = (mx>=bx2 && mx<bx2+bw && my>=by && my<by+bh) ? C_BTN_HOVER : C_WIN_TITLE;
        unsigned short c1 = (mx>=bx1 && mx<bx1+bw && my>=by && my<by+bh) ? C_BTN_HOVER : C_WIN_TITLE;
        rect(bx2, by, bw, bh, c2); draw_square(bx2+bw/2-5, by+bh/2-5);
        rect(bx1, by, bw, bh, c1); draw_minus(bx1+bw/2-6, by+bh/2-1);
    } else {
        rect(bx2, by, bw, bh, C_WIN_TITLE_INACT);
        rect(bx1, by, bw, bh, C_WIN_TITLE_INACT);
    }

    if (wn->is_app && wn->app_fr && wn->app_fr->magic == MYWM_MAGIC
        && wn->app_fr->w > 0 && wn->app_fr->h > 0
        && wn->app_fr->w <= W && wn->app_fr->h <= H) {
        int fw = wn->app_fr->w, fh = wn->app_fr->h;
        int cw = w - 8, ch = h - TITLE_H - 8;

        if (fw == cw && fh == ch) {
            for (int yy = 0; yy < ch; yy++)
                memcpy(&bb[(size_t)(y+TITLE_H+4+yy) * W + x+4],
                       &wn->app_fr->pixels[(size_t)yy * fw],
                       (size_t)fw * 2);
        } else {
            int step_x = (fw << 16) / cw;
            int step_y = (fh << 16) / ch;
            int base_y = 0;
            for (int yy = 0; yy < ch; yy++) {
                const unsigned short *s = &wn->app_fr->pixels[(size_t)(base_y>>16) * fw];
                unsigned short *d = &bb[(size_t)(y+TITLE_H+4+yy) * W + x+4];
                int fxx = 0;
                for (int xx = 0; xx < cw; xx++) {
                    d[xx] = s[fxx >> 16];
                    fxx += step_x;
                }
                base_y += step_y;
            }
        }
        wn->app_fr->dirty = 0;
    }

    if (!wn->is_app && wn->_t_body)
        blit_text(wn->_t_body, x+14, y+TITLE_H+14);
}

static int wnd_create(int x, int y, int w, int h, const char *title, const char *body)
{
    if (n_wins >= MAX_WINS) return -1;
    Wnd *wn = &wins[n_wins];
    memset(wn, 0, sizeof *wn);
    wn->x = x; wn->y = y; wn->w = w; wn->h = h;
    wn->orig_w = w; wn->orig_h = h;
    strncpy(wn->title, title, 47);
    wn->title[47] = 0;
    if (body) {
        wn->_t_body = TTF_RenderText_Blended(f_small, body, col_dim);
    }
    wn->visible = 1;
    wn->active = 0;
    return n_wins++;
}

/* ativar = subir ao TOPO das janelas normais, mas SEMPRE por baixo
   das janelas de teclado (o teclado fica visível por cima de tudo).
   DEVOLVE o índice final — o array é REORDENADO, o índice antigo
   deixa de ser válido! (bug v3.0: quem chamava continuava a usar
   o índice velho e agarrava a janela errada) */
static int wnd_activate(int wi)
{
    if (wi < 0 || wi >= n_wins) return -1;
    Wnd tmp = wins[wi];
    for (int k = wi; k < n_wins-1; k++) wins[k] = wins[k+1];
    int dst = n_wins-1;
    while (dst > 0 && wins[dst-1].is_kb) dst--;
    for (int k = n_wins-1; k > dst; k--) wins[k] = wins[k-1];
    wins[dst] = tmp;
    for (int i = 0; i < n_wins; i++) wins[i].active = 0;
    wins[dst].active = 1;
    return dst;
}

static void wnd_remove_app(int wi)
{
    if (wi < 0 || wi >= n_wins) return;
    if (wins[wi].app_fr) shmdt(wins[wi].app_fr);
    if (wins[wi].app_shmid) shmctl(wins[wi].app_shmid, IPC_RMID, NULL);
    for (int k = wi; k < n_wins-1; k++) wins[k] = wins[k+1];
    n_wins--;
}

/* ===== teclado de sistema: região ocupada ===== */
static int kb_visible(void)
{
    for (int i = 0; i < n_wins; i++)
        if (wins[i].is_kb && wins[i].visible && !wins[i].closing) return 1;
    return 0;
}

static int kb_top(void)
{
    int top = H - TASKBAR_H;
    for (int i = 0; i < n_wins; i++)
        if (wins[i].is_kb && wins[i].visible && !wins[i].closing)
            if (wins[i].y < top) top = wins[i].y;
    return top;
}

/* altura de maximização AGORA (respeita o teclado aberto) */
static int max_h_now(void)
{
    if (kb_visible()) {
        int mh = kb_top() - 12 - 20;   /* gap 12 sobre o teclado */
        if (mh < 120) mh = 120;
        return mh;
    }
    return H - 40 - TASKBAR_H;
}

/* ===== maximizar/restaurar (R1 e botão □) ===== */

/* o cursor está sobre a ÁREA DE CLIENTE de alguma app NORMAL?
   (o teclado não conta — as teclas da consola lá não fazem nada) */
static int cursor_over_app_client(void)
{
    for (int wi = n_wins-1; wi >= 0; wi--) {
        Wnd *wn = &wins[wi];
        if (!wn->visible || wn->closing || !wn->is_app || !wn->app_fr) continue;
        if (wn->is_kb) continue;
        if (mx >= wn->x+4 && mx < wn->x+wn->w-4 &&
            my >= wn->y+TITLE_H+4 && my < wn->y+wn->h-4)
            return 1;
    }
    return 0;
}

/* aplicar maximização (à altura disponível AGORA) */
static void app_apply_max(Wnd *wn)
{
    int mh = max_h_now();
    wn->pend_x = 20; wn->pend_y = 20;
    wn->pend_w = W-40; wn->pend_h = mh;
    wn->app_fr->resize_w = (uint32_t)(W - 40 - 8);
    wn->app_fr->resize_h = (uint32_t)(mh - TITLE_H - 8);
    wn->pend_geo = 1;
    wn->app_fr->resize_pending = 1;
}

/* pedir à app que redimensione; a geometria final fica PENDENTE
   e é aplicada quando a app reconectar com o frame novo */
static void app_request_toggle_max(Wnd *wn)
{
    if (wn->w >= W-40) {
        /* restaurar */
        wn->pend_x = 20; wn->pend_y = 20;
        wn->pend_w = wn->orig_w; wn->pend_h = wn->orig_h;
        wn->app_fr->resize_w = (uint32_t)(wn->orig_w - 8);
        wn->app_fr->resize_h = (uint32_t)(wn->orig_h - TITLE_H - 8);
        wn->pend_geo = 1;
        wn->app_fr->resize_pending = 1;
    } else {
        app_apply_max(wn);
    }
    LOG("resize pedido '%s' -> %dx%d\n",
        wn->title, (int)wn->app_fr->resize_w, (int)wn->app_fr->resize_h);
}

static void toggle_maximize_win(int wi)
{
    if (wi < 0 || wi >= n_wins) return;
    Wnd *wn = &wins[wi];
    if (wn->is_kb) return;          /* teclado não se maximiza */
    if (wn->is_app) {
        if (wn->app_fr) app_request_toggle_max(wn);
    } else {
        if (wn->w >= W-40) {
            wn->w = wn->orig_w; wn->h = wn->orig_h;
            wn->x = 80; wn->y = 70;
        } else {
            wn->w = W-40; wn->h = max_h_now();
            wn->x = 20; wn->y = 20;
        }
        LOG("janela '%s' -> %dx%d\n", wn->title, wn->w, wn->h);
    }
    wnd_activate(wi);
}

/* re-aplicar a maximização às janelas maximizadas — chamado quando
   um teclado aparece/desaparece (as janelas "refluem" para o
   espaço visível, como o Android faz com o GBoard) */
static void reflow_maximized(void)
{
    for (int i = 0; i < n_wins; i++) {
        Wnd *wn = &wins[i];
        if (wn->is_kb || !wn->visible || wn->closing) continue;
        if (wn->w < W-40) continue;              /* só maximizadas */
        if (wn->is_app && wn->app_fr) {
            int mh = max_h_now();
            int tw = W-40-8, th = mh - TITLE_H - 8;
            if (wn->app_fr->w != tw || wn->app_fr->h != th)
                app_apply_max(wn);
            else { wn->x = 20; wn->y = 20; wn->h = mh; }
        } else {
            wn->w = W-40; wn->h = max_h_now();
            wn->x = 20; wn->y = 20;
        }
    }
}

/* ===== flags das apps: grab + teclado ===== */
static pid_t g_grab_pid = 0;

static void compute_grab(void)
{
    g_grab_pid = 0;

    /* atualizar a flag is_kb (lida do frame — a app marca-se) e
       garantir que uma janela de teclado NUNCA fica com o foco */
    int kb_focused = 0;
    for (int i = 0; i < n_wins; i++) {
        Wnd *wn = &wins[i];
        if (wn->is_app && wn->app_fr)
            wn->is_kb = (wn->app_fr->is_keyboard != 0);
        else
            wn->is_kb = 0;
        if (wn->is_kb && wn->active) kb_focused = 1;
    }
    if (kb_focused) {
        /* devolver o foco à janela normal do topo */
        for (int i = n_wins-1; i >= 0; i--)
            if (wins[i].visible && !wins[i].is_kb) { wnd_activate(i); break; }
    }

    for (int i = 0; i < n_wins; i++) {
        Wnd *wn = &wins[i];
        if (wn->visible && !wn->closing && wn->active && wn->is_app
            && wn->app_pid > 0 && wn->app_fr && wn->app_fr->dpad_grab) {
            g_grab_pid = wn->app_pid;
            return;
        }
    }
}

/* ===== servidor ===== */
static MywmGlobal *g_glob = NULL;
static int g_glob_shmid = -1;

static int server_init(void)
{
    FILE *f = fopen(MYWM_SHM_PATH, "w");
    if (!f) { LOG("server: fopen %s: %s\n", MYWM_SHM_PATH, strerror(errno)); return -1; }
    fclose(f);

    key_t k = ftok(MYWM_SHM_PATH, 1);
    g_glob_shmid = shmget(k, sizeof(MywmGlobal), IPC_CREAT | 0666);
    if (g_glob_shmid < 0) { LOG("shmget: %s\n", strerror(errno)); return -1; }
    g_glob = shmat(g_glob_shmid, NULL, 0);
    if (g_glob == (void *)-1) { LOG("shmat\n"); return -1; }

    g_glob->magic = MYWM_MAGIC;
    g_glob->proto = MYWM_PROTO_VER;
    memset(g_glob->slots, 0, sizeof g_glob->slots);

    LOG("server ativo (shmid=%d)\n", g_glob_shmid);
    return 0;
}

static void server_cleanup(void)
{
    for (int i = 0; i < n_wins; i++)
        if (wins[i].is_app && wins[i].app_pid > 0)
            kill(wins[i].app_pid, SIGTERM);
    usleep(200000);
    while (waitpid(-1, NULL, WNOHANG) > 0);

    for (int i = 0; i < n_wins; i++)
        if (wins[i].is_app) {
            if (wins[i].app_fr) shmdt(wins[i].app_fr);
            if (wins[i].app_shmid) shmctl(wins[i].app_shmid, IPC_RMID, NULL);
        }

    if (g_glob) shmdt(g_glob);
    if (g_glob_shmid >= 0) shmctl(g_glob_shmid, IPC_RMID, NULL);
    unlink(MYWM_SHM_PATH);
}

static void server_poll(void)
{
    if (!g_glob) return;

    for (int s = 0; s < MYWM_MAX_APPS; s++) {
        MywmMsg *m = &g_glob->slots[s];
        if (!m->used) continue;
        m->used = 0;

        if (m->msg == MYWM_MSG_CREATE) {
            int existing = -1, was_max = 0;
            int pend = 0, pex = 0, pey = 0, pew = 0, peh = 0;
            int saved_orig_w = 0, saved_orig_h = 0;
            int old_x = 20, old_y = 20;
            for (int wi = 0; wi < n_wins; wi++)
                if (wins[wi].is_app && wins[wi].app_pid == (pid_t)m->app_id) {
                    existing = wi;
                    was_max = (wins[wi].w >= W-40);
                    saved_orig_w = wins[wi].orig_w;
                    saved_orig_h = wins[wi].orig_h;
                    pend = wins[wi].pend_geo;
                    pex = wins[wi].pend_x; pey = wins[wi].pend_y;
                    pew = wins[wi].pend_w; peh = wins[wi].pend_h;
                    old_x = wins[wi].x; old_y = wins[wi].y;
                    break;
                }
            if (existing >= 0) {
                wnd_remove_app(existing);
                LOG("app substituída pid=%d\n", (int)m->app_id);
            }

            MywmFrame *fr = shmat(m->a, NULL, 0);
            if (fr == (void *)-1) {
                LOG("shmat falhou (shmid=%d)\n", m->a);
                continue;
            }
            if (fr->magic != MYWM_MAGIC || fr->proto != MYWM_PROTO_VER
                || fr->app_id != m->app_id
                || fr->w <= 0 || fr->h <= 0 || fr->w > W || fr->h > H) {
                LOG("frame inválida pid=%d (%dx%d)\n", (int)m->app_id, fr->w, fr->h);
                shmdt(fr);
                continue;
            }

            /* área de cliente máxima cabível (ecrã - margens - taskbar) */
            int max_cw = W - 40 - 8;
            int max_ch = MAX_H - TITLE_H - 8;
            int cw = fr->w, ch = fr->h;
            if (!fr->is_keyboard) {
                if (cw > max_cw) cw = max_cw;
                if (ch > max_ch) ch = max_ch;
            }

            int ww = cw + 8, wh = ch + TITLE_H + 8;
            int wx, wy;
            if (existing >= 0) {
                wx = old_x; wy = old_y;
            } else if (fr->is_keyboard) {
                /* teclado: ancorado em baixo, acima da taskbar */
                wx = 4 + PAD_L;
                wy = H - TASKBAR_H - wh - 6;
                if (wy < 0) wy = 0;
            } else {
                wx = 120 + n_wins*30; wy = 60 + n_wins*24;
            }
            if (wx + ww > W) wx = W - ww;
            if (wx < 0) wx = 0;
            if (!fr->is_keyboard) {
                /* nunca nascer por baixo da taskbar NEM do teclado */
                int limit = H - TASKBAR_H;
                if (kb_visible()) {
                    int kt = kb_top() - 4;
                    if (kt < limit) limit = kt;
                }
                if (wy + wh > limit) wy = limit - wh;
                if (wy < 0) wy = 0;
            }

            int wi = wnd_create(wx, wy, ww, wh, m->title, NULL);
            if (wi < 0) {
                LOG("máx janelas — rejeitada\n");
                shmdt(fr);
                continue;
            }
            wins[wi].is_app = 1;
            wins[wi].is_kb = (fr->is_keyboard != 0);
            wins[wi].app_pid = (pid_t)m->app_id;
            wins[wi].app_shmid = m->a;
            wins[wi].app_fr = fr;
            if (existing < 0) {
                wins[wi].orig_w = wins[wi].w;
                wins[wi].orig_h = wins[wi].h;
            } else {
                wins[wi].orig_w = saved_orig_w;
                wins[wi].orig_h = saved_orig_h;
            }

            /* geometria PEDIDA pelo mywm (max/restaurar/reflow) */
            if (existing >= 0 && pend) {
                wins[wi].x = pex; wins[wi].y = pey;
                wins[wi].w = pew; wins[wi].h = peh;
                LOG("geo pedida aplicada %dx%d\n", pew, peh);
            }
            /* senão: herdar maximização se o frame novo for grande */
            else if (existing >= 0 && was_max && fr->w > (W-40-8)/2) {
                wins[wi].x = 20; wins[wi].y = 20;
                wins[wi].w = W-40; wins[wi].h = max_h_now();
            }

            /* app demasiado grande? convidar a encolher */
            if (!pend && !wins[wi].is_kb && (cw != fr->w || ch != fr->h)) {
                fr->resize_w = (uint32_t)cw;
                fr->resize_h = (uint32_t)ch;
                fr->resize_pending = 1;
                LOG("frame %dx%d excede o máximo — pedido %dx%d\n",
                    fr->w, fr->h, cw, ch);
            }
            LOG("app ligou pid=%d '%s' %dx%d%s\n",
                (int)m->app_id, wins[wi].title, fr->w, fr->h,
                wins[wi].is_kb ? " [teclado]" : "");

            /* o teclado NUNCA fica com o foco. ATIVAR NO FIM:
               wnd_activate REORDENA o array — depois disto o
               índice wi deixa de ser válido! */
            if (!wins[wi].is_kb) wnd_activate(wi);
        }
        else if (m->msg == MYWM_MSG_CLOSE) {
            for (int wi = 0; wi < n_wins; wi++)
                if (wins[wi].is_app && wins[wi].app_pid == (pid_t)m->app_id
                    && !wins[wi].closing) {
                    /* zombie: esperar 150ms por uma possível reconexão
                       (resize) — evita perder a geometria pendente */
                    wins[wi].closing = 1;
                    wins[wi].close_t = SDL_GetTicks();
                    LOG("app desligou pid=%d (à espera)\n", (int)m->app_id);
                    break;
                }
        }
        else if (m->msg == MYWM_MSG_TEXT) {
            /* texto do teclado -> janela ativa (não-teclado) */
            char txt[16];
            int n = 0;
            while (n < 15 && m->text[n]) { txt[n] = m->text[n]; n++; }
            if (!n) continue;

            int dst = -1;
            for (int i = n_wins-1; i >= 0; i--) {
                Wnd *wn = &wins[i];
                if (!wn->is_app || !wn->app_fr || wn->is_kb
                    || wn->closing || !wn->visible) continue;
                if (wn->active) { dst = i; break; }
            }
            if (dst < 0) {
                /* nenhuma ativa: a janela normal do topo */
                for (int i = n_wins-1; i >= 0; i--) {
                    Wnd *wn = &wins[i];
                    if (!wn->is_app || !wn->app_fr || wn->is_kb
                        || wn->closing || !wn->visible) continue;
                    dst = i; break;
                }
            }
            if (dst >= 0) {
                MywmFrame *fr = wins[dst].app_fr;
                if (fr->text_len + n <= 16) {
                    memcpy((void *)&fr->text_buf[fr->text_len], txt, (size_t)n);
                    fr->text_len += n;
                    fr->text_seq++;
                } else {
                    LOG("texto descartado (buffer cheio)\n");
                }
            } else {
                LOG("texto sem destino\n");
            }
        }
    }
}

static void reap_dead_apps(void)
{
    for (int wi = n_wins-1; wi >= 0; wi--) {
        if (!wins[wi].is_app || wins[wi].app_pid <= 0) continue;
        if (kill(wins[wi].app_pid, 0) == -1 && errno == ESRCH) {
            LOG("app morta pid=%d — limpar\n", (int)wins[wi].app_pid);
            wnd_remove_app(wi);
        }
    }
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* ===== input routing ===== */
/* Os botões da consola são ESPACIAIS: vão para a janela sob o
   cursor (o teclado incluído — ele precisa do rato para cliques).
   O texto do teclado virtual é de FOCO: vai para a janela ativa. */
static void route_input(void)
{
    /* 1) app ativa com dpad_grab: o input é TODO dela */
    if (g_grab_pid) {
        for (int i = 0; i < n_wins; i++) {
            Wnd *wn = &wins[i];
            if (!wn->is_app || !wn->app_fr) continue;
            if (wn->app_pid == g_grab_pid) {
                MywmFrame *fr = wn->app_fr;
                int cw = wn->w-8, ch = wn->h-TITLE_H-8;
                if (cw < 1) cw = 1;
                if (ch < 1) ch = 1;
                int lx = mx-(wn->x+4), ly = my-(wn->y+TITLE_H+4);
                if (lx < 0) lx = 0; else if (lx >= cw) lx = cw-1;
                if (ly < 0) ly = 0; else if (ly >= ch) ly = ch-1;
                if (fr->w > 0 && fr->h > 0) {
                    fr->mouse_x = (uint32_t)(lx * (int)fr->w / cw);
                    fr->mouse_y = (uint32_t)(ly * (int)fr->h / ch);
                }
                fr->mouse_btn = (keys_now >> BIT_A) & 1;
                fr->keys = keys_now;
                fr->got_input = 1;
            } else {
                wn->app_fr->keys = 0;
                wn->app_fr->mouse_btn = 0;
            }
        }
        return;
    }

    /* 2) sem grab: roteamento pela posição do cursor */
    for (int wi = n_wins-1; wi >= 0; wi--) {
        Wnd *wn = &wins[wi];
        if (!wn->is_app || !wn->app_fr) continue;
        if (!wn->visible || wn->closing) {
            wn->app_fr->keys = 0;
            wn->app_fr->mouse_btn = 0;
            continue;
        }
        int inside = mx >= wn->x+4 && mx < wn->x+wn->w-4 &&
                     my >= wn->y+TITLE_H+4 && my < wn->y+wn->h-4;
        if (inside) {
            int cw = wn->w-8, ch = wn->h-TITLE_H-8;
            wn->app_fr->mouse_x = (uint32_t)((mx-wn->x-4) * wn->app_fr->w / cw);
            wn->app_fr->mouse_y = (uint32_t)((my-wn->y-TITLE_H-4) * wn->app_fr->h / ch);
            wn->app_fr->mouse_btn = (keys_now >> BIT_A) & 1;
            wn->app_fr->keys = keys_now;
            wn->app_fr->got_input = 1;
            return;
        }
        wn->app_fr->keys = 0;
        wn->app_fr->mouse_btn = 0;
    }
}

/* ===== taskbar ===== */
static int menu_open = 0;
static char menu_apps[8][32];
static int menu_n = 0;

static void scan_apps(void)
{
    menu_n = 0;
    DIR *d = opendir("/mnt/sdcard/cubegm/wm");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && menu_n < 8) {
        if (e->d_name[0] == '.') continue;
        strncpy(menu_apps[menu_n], e->d_name, 31);
        menu_apps[menu_n][31] = 0;
        menu_n++;
    }
    closedir(d);
}

static void draw_taskbar(void)
{
    int ty = H - TASKBAR_H;
    rect(0, ty, W, TASKBAR_H, C_TASKBAR_BG);
    rect(0, ty, W, 1, C_WIN_BORDER);

    /* botão Iniciar (recuado PAD_L) */
    int s_hover = (mx >= 2+PAD_L && mx < 46+PAD_L && my >= ty+3 && my < H-3);
    rect(2+PAD_L, ty+3, 44, TASKBAR_H-6, s_hover ? C_TASKBAR_BTN_HOV : C_TASKBAR_BTN);
    for (int i = 0; i < 3; i++)
        rect(10+PAD_L, ty+11+i*5, 18, 2, C_TASKBAR_TXT);

    /* botões das janelas */
    int bx = 50+PAD_L;
    for (int i = 0; i < n_wins; i++) {
        int bw2 = 90;
        if (bx + bw2 > W - 120) break;
        int hov = (mx >= bx && mx < bx+bw2 && my >= ty+3 && my < H-3);
        int act = wins[i].visible && wins[i].active;
        unsigned short bg = (act || hov) ? C_TASKBAR_BTN_HOV : C_TASKBAR_BTN;
        rect(bx, ty+3, bw2, TASKBAR_H-6, bg);
        if (wins[i]._t_title)
            blit_text_clip(wins[i]._t_title, bx+4, ty+11, bw2-8);
        if (!wins[i].visible) {
            int cx = bx+bw2/2;
            for (int k = 0; k < 4; k++)
                rect(cx-k, ty+TASKBAR_H-9+k, k*2+1, 1, C_TASKBAR_TXT);
        }
        bx += bw2 + 2;
    }

    /* relógio */
    {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char b[8];
        snprintf(b, sizeof b, "%02d:%02d", tm->tm_hour, tm->tm_min);
        static SDL_Surface *t_clock = NULL;
        static int last_min = -1;
        if (tm->tm_min != last_min) {
            if (t_clock) SDL_FreeSurface(t_clock);
            t_clock = TTF_RenderText_Blended(f_small, b, col_dim);
            last_min = tm->tm_min;
        }
        if (t_clock) blit_text(t_clock, W-52, ty+11);
    }

    /* menu Iniciar (recuado PAD_L, alinhado com o botão) */
    if (menu_open) {
        int mw = 200, mh = menu_n * 28 + 40;
        int my0 = ty - mh;
        rect(4+PAD_L, my0, mw, mh, C_TASKBAR_BG);
        rect(4+PAD_L, my0, mw, 1, C_WIN_BORDER);
        for (int i = 0; i < menu_n; i++) {
            int iy = my0+5+i*28;
            int hov = (mx >= 6+PAD_L && mx < 6+PAD_L+mw-4 && my >= iy && my < iy+28);
            if (hov) rect(6+PAD_L, iy, mw-4, 28, C_TASKBAR_BTN_HOV);
            static SDL_Surface *t_app[8] = {0};
            if (!t_app[i])
                t_app[i] = TTF_RenderText_Blended(f_small, menu_apps[i], col_dim);
            if (t_app[i]) blit_text_clip(t_app[i], 12+PAD_L, iy+8, mw-16);
        }
        int fy = my0+5+menu_n*28;
        rect(6+PAD_L, fy, mw-4, 1, C_WIN_BORDER);
        int c_fy = fy+6;
        int hov_close = (mx >= 6+PAD_L && mx < 6+PAD_L+mw-4 && my >= c_fy && my < c_fy+24);
        if (hov_close) rect(6+PAD_L, c_fy, mw-4, 24, C_TASKBAR_BTN_HOV);
        static SDL_Surface *t_close = NULL;
        if (!t_close)
            t_close = TTF_RenderText_Blended(f_small, "Fechar tudo", col_dim);
        if (t_close) blit_text(t_close, 12+PAD_L, c_fy+5);
    }
}

/* ===== gestão de janelas ===== */
static int dragging_wnd = -1, drag_dx, drag_dy, a_prev = 0;

static void process_wins(void)
{
    int a_now = (keys_now >> BIT_A) & 1;
    /* com grab, o A pertence à app — o compositor não clica */
    int click = (a_now && !a_prev && !g_grab_pid && dragging_wnd < 0);

    /* arrastar em curso */
    if (dragging_wnd >= 0) {
        if (a_now) {
            Wnd *dw = &wins[dragging_wnd];
            dw->x = mx - drag_dx;
            dw->y = my - drag_dy;
            if (dw->x < 0)         dw->x = 0;
            if (dw->x > W - dw->w) dw->x = W - dw->w;
            if (dw->y < 0)         dw->y = 0;
            /* nunca arrastar para baixo da taskbar */
            if (dw->y > H - TASKBAR_H - dw->h) dw->y = H - TASKBAR_H - dw->h;
            if (dw->y < 0)         dw->y = 0;
        } else {
            dragging_wnd = -1;
        }
    }

    if (click) {
        /* ===== TASKBAR ===== */
        int ty = H - TASKBAR_H;
        if (my >= ty) {
            if (mx >= 2+PAD_L && mx < 46+PAD_L) {
                menu_open = !menu_open;
                if (menu_open) scan_apps();
                LOG("menu %s\n", menu_open ? "aberto" : "fechado");
            } else if (mx >= 50+PAD_L) {
                int bx = 50+PAD_L;
                for (int i = 0; i < n_wins; i++) {
                    int bw2 = 90;
                    if (bx + bw2 > W - 120) break;
                    if (mx >= bx && mx < bx+bw2) {
                        if (wins[i].visible) {
                            wins[i].visible = 0;
                            LOG("minimizar '%s' (taskbar)\n", wins[i].title);
                        } else {
                            wins[i].visible = 1;
                            if (!wins[i].is_kb) wnd_activate(i);
                            LOG("restaurar '%s' (taskbar)\n", wins[i].title);
                        }
                        break;
                    }
                    bx += bw2 + 2;
                }
            }
            a_prev = a_now;
            return;
        }
        /* menu Iniciar aberto? */
        if (menu_open) {
            int mw = 200, mh = menu_n * 28 + 40;
            int my0 = H - TASKBAR_H - mh;
            if (mx >= 4+PAD_L && mx < 4+PAD_L+mw && my >= my0 && my < H-TASKBAR_H) {
                int rel = my - my0 - 5;
                int item = rel / 28;
                if (item >= 0 && item < menu_n && rel % 28 < 28) {
                    char path[128];
                    snprintf(path, sizeof path,
                             "/mnt/sdcard/cubegm/wm/%s", menu_apps[item]);
                    pid_t p = fork();
                    if (p == 0) {
                        execl(path, menu_apps[item], (char*)NULL);
                        _exit(127);
                    }
                    if (p > 0) LOG("lançar '%s' pid=%d\n", menu_apps[item], (int)p);
                    menu_open = 0;
                } else if (my >= my0+5+menu_n*28+6) {
                    for (int i = 0; i < n_wins; i++) {
                        if (wins[i].is_app) kill(wins[i].app_pid, SIGTERM);
                        wins[i].visible = 0;
                    }
                    LOG("fechar tudo\n");
                    menu_open = 0;
                }
                a_prev = a_now;
                return;
            } else {
                menu_open = 0;   /* clique fora fecha */
            }
        }

        /* ===== JANELAS ===== */
        int hit = -1, hit_btn = 0;
        for (int wi = n_wins-1; wi >= 0; wi--) {
            Wnd *wn = &wins[wi];
            if (!wn->visible) continue;
            if (mx >= wn->x && mx < wn->x+wn->w && my >= wn->y && my < wn->y+wn->h) {
                hit = wi;
                if (my >= wn->_bby && my < wn->_bby+wn->_bbh) {
                    if      (mx >= wn->_bx3 && mx < wn->_bx3+wn->_bbw) hit_btn = 4;
                    else if (mx >= wn->_bx2 && mx < wn->_bx2+wn->_bbw) hit_btn = 3;
                    else if (mx >= wn->_bx1 && mx < wn->_bx1+wn->_bbw) hit_btn = 2;
                }
                if (!hit_btn && my < wn->y+TITLE_H) hit_btn = 1;
                break;
            }
        }

        if (hit >= 0) {
            if (hit_btn == 4) {
                Wnd *wn = &wins[hit];
                LOG("fechar '%s'\n", wn->title);
                if (wn->is_app) kill(wn->app_pid, SIGTERM);
                wn->visible = 0;
                for (int i = n_wins-1; i >= 0; i--)
                    if (wins[i].visible && !wins[i].is_kb) { wnd_activate(i); break; }
            }
            else if (hit_btn == 3) {
                if (wins[hit].is_kb) {
                    LOG("□ no teclado — ignorado\n");
                } else {
                    toggle_maximize_win(hit);
                }
            }
            else if (hit_btn == 2) {
                wins[hit].visible = 0;
                LOG("minimizar '%s'\n", wins[hit].title);
                for (int i = n_wins-1; i >= 0; i--)
                    if (wins[i].visible && !wins[i].is_kb) { wnd_activate(i); break; }
            }
            else if (hit_btn == 1) {
                if (wins[hit].is_kb) {
                    /* arrastar o teclado SEM lhe dar o foco */
                    dragging_wnd = hit;
                    drag_dx = mx - wins[hit].x;
                    drag_dy = my - wins[hit].y;
                } else {
                    /* FIX v3.1: usar o índice DEVOLVIDO pelo
                       wnd_activate (a lista é reordenada!) */
                    int ni = wnd_activate(hit);
                    Wnd *wn = &wins[ni];
                    if (wn->is_app) {
                        if (wn->app_fr && wn->w >= W-40) {
                            app_request_toggle_max(wn);   /* ramo restaurar */
                            LOG("arrastar maximizada — restaurar '%s'\n", wn->title);
                        }
                    } else if (wn->w >= W-40) {
                        wn->w = wn->orig_w; wn->h = wn->orig_h;
                    }
                    dragging_wnd = ni;
                    drag_dx = mx - wn->x;
                    drag_dy = my - wn->y;
                }
            }
            /* clique na área de cliente: ativa a janela — exceto o
               teclado (clicar nele NUNCA rouba o foco à app) */
            else if (!wins[hit].active && !wins[hit].is_kb) {
                wnd_activate(hit);
            }
        }
    }
    a_prev = a_now;
}

/* ===== main ===== */
int main(void)
{
    lg = fopen("/mnt/sdcard/mywm.log", "w");
    LOG("=== mywm v3.1 ===\n");
    LOG("R1=max | START=menu | sair=START+SELECT | teclado=is_keyboard+reflow\n");

    if (fb_init() != 0) { LOG("FATAL fb\n"); return 1; }
    bb = malloc((size_t)W * H * 2);
    input_init();

    SDL_Init(SDL_INIT_VIDEO);
    TTF_Init();
    f_big   = TTF_OpenFont("/mnt/sdcard/osdata/font.ttf", 48);
    f_med   = TTF_OpenFont("/mnt/sdcard/osdata/font.ttf", 22);
    f_small = TTF_OpenFont("/mnt/sdcard/osdata/font.ttf", 14);
    if (!f_small) { LOG("FATAL fonte\n"); return 1; }
    LOG("fontes ok\n");

    SDL_Surface *raw = IMG_Load("/mnt/sdcard/osdata/wall.bmp");
    if (raw && raw->w >= 4 && raw->h >= 4) {
        g_wall = SDL_CreateRGBSurface(SDL_SWSURFACE, raw->w, raw->h, 32,
            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        SDL_BlitSurface(raw, NULL, g_wall, NULL);
        SDL_FreeSurface(raw);
    }

    {
        FILE *p = popen("killall demo paint myterm mykeyboard 2>/dev/null", "r");
        if (p) pclose(p);
        usleep(200000);
        p = popen("rm -f /tmp/mywm.* 2>/dev/null", "r");
        if (p) pclose(p);
    }

    if (server_init() != 0) { LOG("FATAL server\n"); return 1; }


    unsigned frames = 0;
    Uint32 fps_t = SDL_GetTicks();
    int fps = 0, quit = 0, start_prev = 0, r1_prev = 0;
    SDL_Surface *t_fps = NULL;
    int last_fps = -1;
    Uint32 reap_t = 0;

    while (!quit) {
        Uint32 t = SDL_GetTicks();

        /* DEBOUNCE: só aceitar o estado quando está igual em 2
           amostras seguidas — filtra o bounce elétrico dos botões */
        {
            static uint32_t k_raw = 0, k_stable = 0;
            uint32_t raw = shm_keys ? (*shm_keys & KEY_MASK) : 0;
            if (raw == k_raw) k_stable = raw;
            k_raw = raw;
            keys_now = k_stable;
        }

        /* grab: a app ativa agarrou o D-pad/A */
        compute_grab();
        if (g_grab_pid) {
            vel_x = 0; vel_y = 0; was_moving = 0;
        } else {
            update_mouse();
        }

        process_wins();
        server_poll();

        /* re-avaliar o grab DEPOIS do server_poll */
        compute_grab();

        /* varrer apps que fecharam de verdade (sem reconexão) */
        for (int wi = n_wins-1; wi >= 0; wi--)
            if (wins[wi].closing && t - wins[wi].close_t > 150) {
                LOG("app fechou pid=%d — limpar\n", (int)wins[wi].app_pid);
                wnd_remove_app(wi);
            }

        /* REFLOW: quando um teclado aparece/desaparece (contagem
           de teclados VISIBLES muda — arrastá-lo não conta) */
        {
            static int last_kb_cnt = -1;
            int kb_cnt = 0;
            for (int i = 0; i < n_wins; i++)
                if (wins[i].is_kb && wins[i].visible && !wins[i].closing) kb_cnt++;
            if (kb_cnt != last_kb_cnt) {
                if (last_kb_cnt >= 0) {
                    LOG("reflow (teclados visíveis: %d)\n", kb_cnt);
                    reflow_maximized();
                }
                last_kb_cnt = kb_cnt;
            }
        }

        route_input();

        /* START = menu Iniciar — mas só se NINGUÉM tiver o input */
        int st = (keys_now >> BIT_START) & 1;
        if (st && !start_prev && !g_grab_pid && !cursor_over_app_client()) {
            menu_open = !menu_open;
            if (menu_open) scan_apps();
            LOG("menu %s\n", menu_open ? "aberto" : "fechado");
        }
        start_prev = st;

        /* R1 = maximizar/restaurar a janela ATIVA */
        int r1 = (keys_now >> BIT_R) & 1;
        if (r1 && !r1_prev) {
            for (int i = n_wins-1; i >= 0; i--)
                if (wins[i].visible && wins[i].active && !wins[i].is_kb) {
                    toggle_maximize_win(i);
                    break;
                }
        }
        r1_prev = r1;

        if (t - reap_t > 2000) {
            reap_t = t;
            reap_dead_apps();
        }

        draw_wall();
        for (int i = 0; i < n_wins; i++) draw_window(&wins[i]);

        if (fps != last_fps) {
            if (t_fps) SDL_FreeSurface(t_fps);
            char b[16];
            snprintf(b, sizeof b, "FPS %d", fps);
            t_fps = TTF_RenderText_Blended(f_small, b, col_white);
            last_fps = fps;
        }
        blit_text(t_fps, W-80, H-TASKBAR_H-20);
        draw_taskbar();
        if (!g_grab_pid) draw_cursor();
        present();

        /* sair = START+SELECT juntos (segurar ambos) */
        if ((keys_now & ((1u<<BIT_SELECT)|(1u<<BIT_START)))
              == ((1u<<BIT_SELECT)|(1u<<BIT_START)))
            quit = 1;
        frames++;
        Uint32 now = SDL_GetTicks();
        if (now - fps_t >= 1000) {
            fps = (int)(frames * 1000 / (now - fps_t));
            frames = 0;
            fps_t = now;
            LOG("fps=%d\n", fps);
        }
        Uint32 el = SDL_GetTicks() - t;
        if (el < 16) SDL_Delay(16 - el);
    }

    server_cleanup();
    LOG("=== fim ===\n");
    return 0;
}