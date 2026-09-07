/* ============================================================
 * mykeyboard.c — TECLADO DE SISTEMA do MYWM (v1.0)
 *
 * O "GBoard" da consola: uma janela especial (is_keyboard) que:
 *   - vive SEMPRE por cima das outras janelas
 *   - NUNCA recebe foco (clicar aqui não rouba o foco à app)
 *   - nasce ancorada acima da taskbar
 *   - é clicada com o RATO (uma borda de clique por tecla —
 *     zero auto-repeat, zero teclas dobradas)
 *   - envia cada tecla ao mywm (MYWM_MSG_TEXT), que a entrega
 *     à janela ATIVA — ex: o myterm
 *
 * Páginas: ABC | 123/sym (botão "123"/"ABC")
 * Linha de navegação: ESC LT UP DN RT HOME END PGUP PGDN TAB CTL
 *
 * Log: /mnt/sdcard/mywm_kb.log
 * ============================================================ */
#include "mywm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

static FILE *lg;
#define LOG(...) do{ if(lg){fprintf(lg,__VA_ARGS__);fflush(lg);fsync(fileno(lg));} }while(0)

/* ---------- fonte 5x7 ---------- */
static const unsigned char FONT5x7[96][7] = {
{0,0,0,0,0,0,0},{0x04,0x04,0x04,0x04,0x04,0,0x04},{0x0A,0x0A,0x0A,0,0,0,0},
{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},{0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
{0x19,0x1A,0x02,0x04,0x08,0x13,0x03},{0x08,0x14,0x14,0x08,0x15,0x12,0x0D},
{0x04,0x04,0,0,0,0,0},{0x02,0x04,0x08,0x08,0x08,0x04,0x02},
{0x08,0x04,0x02,0x02,0x02,0x04,0x08},{0,0x04,0x15,0x0E,0x15,0x04,0},
{0,0x04,0x04,0x1F,0x04,0x04,0},{0,0,0,0,0x0C,0x04,0x08},{0,0,0,0x1F,0,0,0},
{0,0,0,0,0,0x0C,0x0C},{0,0x01,0x02,0x04,0x08,0x10,0},
{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
{0,0x0C,0x0C,0,0x0C,0x0C,0},{0,0x0C,0x0C,0,0x0C,0x04,0x08},
{0x02,0x04,0x08,0x10,0x08,0x04,0x02},{0,0,0x1F,0,0x1F,0,0},
{0x08,0x04,0x02,0x01,0x02,0x04,0x08},{0x0E,0x11,0x01,0x02,0x04,0,0x04},
{0x0E,0x11,0x15,0x17,0x15,0x10,0x0E},{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
{0x07,0x02,0x02,0x02,0x02,0x12,0x0C},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},
{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},{0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
{0x11,0x19,0x15,0x13,0x11,0x11,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
{0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},{0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
{0,0x10,0x08,0x04,0x02,0x01,0},{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
{0x04,0x0A,0x11,0,0,0,0},{0,0,0,0,0,0,0x1F},{0x08,0x04,0,0,0,0,0},
{0,0,0x0E,0x01,0x0F,0x11,0x0F},{0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},
{0,0,0x0F,0x10,0x10,0x10,0x0F},{0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},
{0,0,0x0E,0x11,0x1F,0x10,0x0F},{0x06,0x08,0x08,0x1E,0x08,0x08,0x08},
{0,0x0F,0x11,0x11,0x0F,0x01,0x1E},{0x10,0x10,0x1E,0x11,0x11,0x11,0x11},
{0x04,0,0x0C,0x04,0x04,0x04,0x0E},{0x02,0,0x06,0x02,0x02,0x12,0x0C},
{0x10,0x12,0x14,0x18,0x14,0x12,0x11},{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
{0,0,0x1A,0x15,0x15,0x15,0x15},{0,0,0x1E,0x11,0x11,0x11,0x11},
{0,0,0x0E,0x11,0x11,0x11,0x0E},{0,0,0x1E,0x11,0x11,0x1E,0x10},
{0,0,0x0F,0x11,0x11,0x0F,0x01},{0,0,0x16,0x19,0x10,0x10,0x10},
{0,0,0x0F,0x10,0x0E,0x01,0x1E},{0x08,0x08,0x1E,0x08,0x08,0x08,0x06},
{0,0,0x11,0x11,0x11,0x11,0x0F},{0,0,0x11,0x11,0x11,0x0A,0x04},
{0,0,0x11,0x15,0x15,0x15,0x0A},{0,0,0x11,0x0A,0x04,0x0A,0x11},
{0,0,0x11,0x11,0x11,0x0F,0x01},{0,0,0x1F,0x02,0x04,0x08,0x1F},
{0x06,0x08,0x08,0x10,0x08,0x08,0x06},{0x04,0x04,0x04,0x04,0x04,0x04,0x04},
{0x0C,0x02,0x02,0x01,0x02,0x02,0x0C},{0,0,0x08,0x15,0x02,0,0},
{0,0,0,0,0,0,0}
};

/* ---------- páginas ---------- */
static const char *PG0[30] = {   /* ABC */
    "q","w","e","r","t","y","u","i","o","p",
    "a","s","d","f","g","h","j","k","l",";",
    "z","x","c","v","b","n","m",",",".","?"
};
static const char *PG1[30] = {   /* 123 / sym */
    "`","~","{","}","[","]","<",">","*","&",
    "@","#","$","%","^","+","=","|","\\","_",
    "(",")",":",";","'","\"","!",".",",","-"
};

/* linha de navegação: rótulo + código MYWM_KC_* */
static const char *NAV_L[11] = {"ESC","LT","UP","DN","RT","HOME","END","PGUP","PGDN","TAB","CTL"};
static const char NAV_C[11] = {
    MYWM_KC_ESC, MYWM_KC_LEFT, MYWM_KC_UP, MYWM_KC_DOWN, MYWM_KC_RIGHT,
    MYWM_KC_HOME, MYWM_KC_END, MYWM_KC_PGUP, MYWM_KC_PGDN, MYWM_KC_TAB,
    MYWM_KC_CTL
};

#define COL_PANEL 0x1082
#define COL_KEY   0x39E7
#define COL_HOVER 0x6B5F
#define COL_SHIFT 0x07E0
#define COL_FG    0xFFFF

/* ---------- desenho ---------- */
static int FW, FH;
static unsigned short *PX;
#define CW 12

static void apx(int x, int y, unsigned short c)
{
    if ((unsigned)x < (unsigned)FW && (unsigned)y < (unsigned)FH)
        PX[(size_t)y * FW + x] = c;
}

static void arect(int x, int y, int w, int h, unsigned short c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            apx(x+i, y+j, c);
}

static void achar(int x, int y, char chr, unsigned short c)
{
    unsigned char u = (unsigned char)chr;
    if (u < 32 || u > 126) return;
    const unsigned char *g = FONT5x7[u-32];
    for (int r = 0; r < 7; r++)
        for (int col = 0; col < 5; col++)
            if (g[r] & (0x10>>col)) {
                apx(x+col*2,   y+r*2,   c);
                apx(x+col*2+1, y+r*2,   c);
                apx(x+col*2,   y+r*2+1, c);
                apx(x+col*2+1, y+r*2+1, c);
            }
}

static void atext(int x, int y, const char *s, unsigned short c)
{
    for (; *s; s++, x += CW) achar(x, y, *s, c);
}

/* ---------- grelha de teclas ---------- */
typedef struct {
    int x, y, w, h;
    const char *lbl;   /* rótulo */
    char ch;           /* char/código a enviar (0 se ação) */
    int act;           /* 0=enviar ch | 1=SHIFT | 2=trocar página */
} Key;

static Key keys[64];
static int n_keys = 0;
static int page = 0, shift = 0;
static int hover_idx = -1;
static int need_render = 1;

static void build_keys(void)
{
    n_keys = 0;
    int rh = FH/5;

    /* linhas 0-2: letras / símbolos — JUSTAPOSTAS (zero gap) */
    const char **pg = page ? PG1 : PG0;
    int kw = FW/10;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 10; c++) {
            keys[n_keys].x = c*kw;  keys[n_keys].y = r*rh;
            keys[n_keys].w = kw;    keys[n_keys].h = rh;
            keys[n_keys].lbl = pg[r*10+c];
            keys[n_keys].ch = pg[r*10+c][0];
            keys[n_keys].act = 0;
            n_keys++;
        }

    /* linha 3: página / SHIFT / espaço / BSP / ENT (justapostas) */
    int y3 = 3*rh;
    keys[n_keys].x = 0;       keys[n_keys].y = y3;
    keys[n_keys].w = FW/8;    keys[n_keys].h = rh;
    keys[n_keys].lbl = page ? "ABC" : "123";
    keys[n_keys].ch = 0;      keys[n_keys].act = 2; n_keys++;

    keys[n_keys].x = FW/8;    keys[n_keys].y = y3;
    keys[n_keys].w = FW/8;    keys[n_keys].h = rh;
    keys[n_keys].lbl = "SHF"; keys[n_keys].ch = 0;
    keys[n_keys].act = 1; n_keys++;

    keys[n_keys].x = FW/4;    keys[n_keys].y = y3;
    keys[n_keys].w = FW/2;    keys[n_keys].h = rh;
    keys[n_keys].lbl = "SPC"; keys[n_keys].ch = ' ';
    keys[n_keys].act = 0; n_keys++;

    keys[n_keys].x = 3*FW/4;  keys[n_keys].y = y3;
    keys[n_keys].w = FW/8;    keys[n_keys].h = rh;
    keys[n_keys].lbl = "BSP"; keys[n_keys].ch = MYWM_KC_BS;
    keys[n_keys].act = 0; n_keys++;

    keys[n_keys].x = 7*FW/8;  keys[n_keys].y = y3;
    keys[n_keys].w = FW - 7*FW/8;   /* última estica até à borda */
    keys[n_keys].h = rh;
    keys[n_keys].lbl = "ENT"; keys[n_keys].ch = MYWM_KC_ENTER;
    keys[n_keys].act = 0; n_keys++;

    /* linha 4: navegação (11 justapostas) */
    int y4 = 4*rh, kw4 = FW/11;
    for (int i = 0; i < 11; i++) {
        keys[n_keys].x = i*kw4; keys[n_keys].y = y4;
        keys[n_keys].w = (i == 10) ? FW - 10*kw4 : kw4;
        keys[n_keys].h = rh;
        keys[n_keys].lbl = NAV_L[i];
        keys[n_keys].ch = NAV_C[i];
        keys[n_keys].act = 0;
        n_keys++;
    }
}

static void render_frame(void)
{
    build_keys();
    arect(0, 0, FW, FH, COL_PANEL);
    for (int i = 0; i < n_keys; i++) {
        Key *k = &keys[i];
        unsigned short col = COL_KEY;
        if (i == hover_idx) col = COL_HOVER;
        if (k->act == 1 && shift) col = COL_SHIFT;
        arect(k->x, k->y, k->w, k->h, col);

        char lb[8]; strncpy(lb, k->lbl, 7); lb[7] = 0;
        if (shift && !k->act && lb[1] == 0 && lb[0] >= 'a' && lb[0] <= 'z')
            lb[0] -= 32;
        int len = (int)strlen(lb);
        atext(k->x+(k->w-len*CW)/2, k->y+(k->h-14)/2, lb,
              (k->act == 1 && shift) ? 0x0000 : COL_FG);
    }
    /* grelha 1px: teclas justapostas, sem ela as vizinhas da mesma
       cor fundem-se numa barra. 0x2104 = cor da moldura da janela */
    for (int i = 0; i < n_keys; i++) {
        arect(keys[i].x, keys[i].y, keys[i].w, 1, 0x2104);
        arect(keys[i].x, keys[i].y, 1, keys[i].h, 0x2104);
    }
}

/* ---------- main ---------- */
static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s) { (void)s; g_stop = 1; }

int main(void)
{
    lg = fopen("/mnt/sdcard/mywm_kb.log", "w");
    LOG("=== mykeyboard v1.0 ===\n");

    signal(SIGTERM, on_stop);
    signal(SIGINT,  on_stop);

    int w = 600, h = 220, first = 1;   /* justaposto: sem espaços entre teclas */

    while (!g_stop) {
        MywmConn c;
        if (mywm_connect(&c, w, h, "Teclado") != 0) {
            LOG("FATAL connect\n");
            return 1;
        }
        /* marcar como TECLADO: topo absoluto, nunca recebe foco */
        mywm_set_keyboard(&c);

        PX = c.fr->pixels;
        FW = w; FH = h;
        hover_idx = -1;
        need_render = 1;
        if (first) { first = 0; LOG("pronto %dx%d\n", w, h); }

        int reconnect = 0;
        static int bprev = 0;

        while (!g_stop && !reconnect) {
            usleep(15000);
            mywm_get_keys(&c);   /* limpar a flag (não usamos teclas) */

            int rw, rh;
            if (mywm_poll_resize(&c, &rw, &rh)) {
                LOG("resize -> %dx%d\n", rw, rh);
                w = rw; h = rh;
                reconnect = 1;
                break;
            }

            int amx = (int)c.fr->mouse_x, amy = (int)c.fr->mouse_y;
            int abtn = (int)c.fr->mouse_btn;

            /* hover (highlight da tecla sob o cursor) */
            int hv = -1;
            for (int i = 0; i < n_keys; i++)
                if (amx >= keys[i].x && amx < keys[i].x+keys[i].w &&
                    amy >= keys[i].y && amy < keys[i].y+keys[i].h) {
                    hv = i; break;
                }
            if (hv != hover_idx) { hover_idx = hv; need_render = 1; }

            /* clique: UMA borda de descida por tecla — zero repetição */
            if (abtn && !bprev && hover_idx >= 0) {
                Key *k = &keys[hover_idx];
                if (k->act == 1) {            /* SHIFT */
                    shift = !shift;
                    need_render = 1;
                } else if (k->act == 2) {     /* página */
                    page = !page;
                    shift = 0;
                    need_render = 1;
                    LOG("página %s\n", page ? "123" : "ABC");
                } else {                      /* enviar tecla */
                    char ch = k->ch;
                    if (shift && ch >= 'a' && ch <= 'z') ch -= 32;
                    char buf[2] = { ch, 0 };
                    mywm_send_text(&c, buf);
                    LOG("tecla 0x%02x\n", (unsigned char)ch);
                    shift = 0;   /* shift só dura uma tecla */
                    need_render = 1;
                }
            }
            bprev = abtn;

            if (need_render) {
                render_frame();
                mywm_end(&c);
                need_render = 0;
            }
        }

        mywm_disconnect(&c);
        PX = NULL;
    }

    LOG("=== fim ===\n");
    if (lg) fclose(lg);
    return 0;
}