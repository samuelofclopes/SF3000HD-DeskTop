/* ============================================================
 * myterm.c — o TERMINAL como app MYWM (v5.0)
 *
 * v5.0: SEM teclado embutido — o input de texto vem do
 *       MYKEYBOARD (teclado de sistema) via canal de texto do
 *       mywm (foco-based). A janela INTEIRA é terminal.
 *       Setas/HOME/END movem o cursor; UP/DN = histórico;
 *       PGUP/PGDN = scroll. O D-pad é SEMPRE o rato (o L faz
 *       scroll quando o cursor está sobre a janela).
 *
 * Isso mata: os 2 modos, o input fantasma, o auto-repeat do A
 * que dobrava teclas (UP/DN/BSP), e o teclado que comia 35% da
 * janela ("linhas de baixo" agora todas visíveis).
 *
 * Log: /mnt/sdcard/mywm_term.log
 * ============================================================ */
#include "mywm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>

#define BIT_L 10   /* scroll (com o rato sobre a janela) */

#define HISTF "/mnt/sdcard/term_history"
#define SHELL "/bin/sh"

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

/* ---------- terminal + ANSI ---------- */
#define MAXL 300
#define MAXC 90
static char tbuf[MAXL][MAXC];
static int cur_row, cur_col, trows, tcols, scroll_off;
static int need_render = 1;

static void term_newline(void)
{
    cur_row++; cur_col = 0;
    if (cur_row >= MAXL) {
        memmove(tbuf, tbuf+1, (size_t)(MAXL-1)*MAXC);
        memset(tbuf[MAXL-1], 0, MAXC);
        cur_row = MAXL-1;
    } else {
        memset(tbuf[cur_row], 0, MAXC);
    }
}

static int ansi_st, ansi_np, ansi_prm[8];

static void csi_exec(char b)
{
    int p0 = ansi_prm[0];
    if (p0 < 0) p0 = 0;
    switch (b) {
    case 'J':
        if (p0 >= 2) { memset(tbuf, 0, sizeof tbuf); cur_row = cur_col = 0; }
        else if (p0 == 0) {
            memset(&tbuf[cur_row][cur_col], 0, MAXC-cur_col);
            for (int r = cur_row+1; r < MAXL; r++) memset(tbuf[r], 0, MAXC);
        }
        break;
    case 'K':
        if (p0 == 0) memset(&tbuf[cur_row][cur_col], 0, MAXC-cur_col);
        else if (p0 == 1) memset(tbuf[cur_row], 0, cur_col+1);
        else memset(tbuf[cur_row], 0, MAXC);
        break;
    case 'H':
        { int p1 = ansi_prm[1]; if (p1 < 1) p1 = 1;
          cur_row = p0 ? p0-1 : 0; if (cur_row >= MAXL) cur_row = MAXL-1;
          cur_col = p1-1; if (cur_col >= tcols) cur_col = tcols-1; if (cur_col < 0) cur_col = 0; }
        break;
    case 'A': cur_row -= p0?p0:1; if (cur_row < 0) cur_row = 0; break;
    case 'B': cur_row += p0?p0:1; if (cur_row >= MAXL) cur_row = MAXL-1; break;
    case 'C': cur_col += p0?p0:1; if (cur_col >= tcols) cur_col = tcols-1; break;
    case 'D': cur_col -= p0?p0:1; if (cur_col < 0) cur_col = 0; break;
    }
}

static void term_feed(const char *p, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (ansi_st) {
        case 0:
            if (c == 27) ansi_st = 1;
            else if (c == '\n') term_newline();
            else if (c == '\r') cur_col = 0;
            else if (c == '\b') { if (cur_col > 0) { cur_col--; tbuf[cur_row][cur_col] = 0; } }
            else if (c == '\t') {
                int t = (cur_col/8+1)*8;
                while (cur_col < t && cur_col < tcols) tbuf[cur_row][cur_col++] = ' ';
                if (cur_col >= tcols) term_newline();
            }
            else if (c >= 32 && c < 127) {
                tbuf[cur_row][cur_col++] = (char)c;
                if (cur_col >= tcols) term_newline();
            }
            break;
        case 1:
            if (c == '[') { ansi_st = 2; memset(ansi_prm, 0, sizeof ansi_prm); ansi_np = 0; }
            else if (c == ']') ansi_st = 3;
            else ansi_st = 0;
            break;
        case 2:
            if (c >= '0' && c <= '9') { if (ansi_np < 8) ansi_prm[ansi_np] = ansi_prm[ansi_np]*10 + (c-'0'); }
            else if (c == ';') { if (ansi_np < 7) { ansi_np++; ansi_prm[ansi_np] = 0; } }
            else if (c >= '@' && c <= '~') { csi_exec((char)c); ansi_st = 0; }
            else ansi_st = 0;
            break;
        case 3:
            if (c == 7 || c == 27) ansi_st = 0;
            break;
        }
    }
    need_render = 1;
}

static void term_puts(const char *s) { term_feed(s, (int)strlen(s)); }

/* ---------- shell por pipes ---------- */
static int io_rd = -1, io_wr = -1;
static pid_t shell_pid = -1;
static char line[256];
static int llen = 0, lpos = 0;
static char hist[32][128];
static int hist_n = 0, hist_view = -1, hist_loaded = 0;
static char saved_line[256];
static int saved_llen = 0, has_saved = 0;
static char cwd[256] = "/mnt/sdcard";
static int pwd_capture = 0, pwd_len = 0, pwd_clear = 0;
static char pwd_buf[256];

static void show_prompt_cwd(void)
{
    if (cur_col) term_puts("\n");
    char buf[256];
    const char *home = "/mnt/sdcard";
    int homelen = (int)strlen(home);
    if (strncmp(cwd, home, homelen) == 0) {
        if (cwd[homelen] == 0) snprintf(buf, sizeof buf, "$ ");
        else snprintf(buf, sizeof buf, "%s$ ", cwd+homelen+1);
    } else {
        snprintf(buf, sizeof buf, "%s$ ", cwd);
    }
    term_puts(buf);
}

static int spawn_pipe_shell(void)
{
    int inp[2], outp[2];
    if (pipe(inp) < 0 || pipe(outp) < 0) return -1;
    shell_pid = fork();
    if (shell_pid < 0) return -1;
    if (shell_pid == 0) {
        setsid();
        dup2(inp[0], 0); dup2(outp[1], 1); dup2(outp[1], 2);
        close(inp[0]); close(inp[1]); close(outp[0]); close(outp[1]);
        chdir(cwd);
        setenv("TERM", "dumb", 1);
        setenv("HOME", "/mnt/sdcard", 1);
        setenv("PATH", "/bin:/sbin:/usr/bin:/mnt/sdcard/cubegm/usr/bin:/mnt/sdcard/rootfs/usr/bin", 1);
        execl(SHELL, "sh", NULL);
        _exit(127);
    }
    close(inp[0]); close(outp[1]);
    io_wr = inp[1]; io_rd = outp[0];
    LOG("sh pid=%d\n", shell_pid);
    return 0;
}

static void kill_shell(void)
{
    if (shell_pid > 0) {
        kill(shell_pid, SIGTERM);
        usleep(50000);
        kill(shell_pid, SIGKILL);
        waitpid(shell_pid, NULL, 0);
        shell_pid = -1;
    }
    if (io_rd >= 0) close(io_rd);
    if (io_wr >= 0) close(io_wr);
    io_rd = io_wr = -1;
}

static void shell_respawn(void)
{
    kill_shell();
    pwd_capture = pwd_len = pwd_clear = 0;
    if (spawn_pipe_shell() == 0) {
        term_puts("\n[sh reiniciado]\n");
        llen = lpos = 0; hist_view = -1; has_saved = 0;
        show_prompt_cwd();
    } else {
        LOG("respawn falhou\n");
    }
}

static void redraw_line(void)
{
    term_puts("\r\033[K$ ");
    if (llen > 0) {
        char save = line[llen]; line[llen] = 0;
        term_puts(line);
        line[llen] = save;
    }
    if (lpos < llen) {
        char buf[16];
        snprintf(buf, sizeof buf, "\033[%dD", llen-lpos);
        term_puts(buf);
    }
}

/* ---------- editor de linha ---------- */
static int line_max(void)
{
    int maxl = tcols-3;
    if (maxl > 250) maxl = 250;
    if (maxl < 8) maxl = 8;
    return maxl;
}

static void line_insert(char ch)
{
    if (llen >= line_max()) return;
    memmove(&line[lpos+1], &line[lpos], (size_t)(llen-lpos));
    line[lpos++] = ch; llen++;
    redraw_line();
}

static void line_backspace(void)
{
    if (lpos > 0) {
        memmove(&line[lpos-1], &line[lpos], (size_t)(llen-lpos));
        lpos--; llen--;
        redraw_line();
    }
}

static void submit_line(void)
{
    line[llen] = 0;
    term_puts("\n");
    if (llen) {
        if (hist_n == 0 || strcmp(hist[hist_n-1], line)) {
            if (hist_n < 32) strcpy(hist[hist_n++], line);
            else { memmove(hist, hist+1, 31*128); strcpy(hist[31], line); }
        }
        write(io_wr, line, llen);
        write(io_wr, "\n", 1);
        char *cmd = line;
        while (*cmd == ' ') cmd++;
        if (!strncmp(cmd, "cd", 2) && (cmd[2] == ' ' || cmd[2] == '\0' || cmd[2] == '/')) {
            /* pwd escondido: atualiza o cwd do prompt */
            write(io_wr, "pwd\n", 4);
            pwd_capture = 1; pwd_len = 0;
        }
    } else show_prompt_cwd();
    llen = lpos = 0; hist_view = -1; has_saved = 0;
}

static void hist_load(int dir)
{
    if (!hist_n) return;
    if (dir < 0) {
        if (hist_view < 0) {
            if (!has_saved) { memcpy(saved_line, line, 256); saved_llen = llen; has_saved = 1; }
            hist_view = hist_n-1;
        } else if (hist_view > 0) hist_view--;
    } else {
        if (hist_view < 0) return;
        hist_view++;
        if (hist_view >= hist_n) {
            hist_view = -1;
            if (has_saved) { memcpy(line, saved_line, 256); llen = saved_llen; has_saved = 0; }
            else { line[0] = 0; llen = 0; }
            lpos = llen; redraw_line();
            return;
        }
    }
    if (hist_view >= 0) {
        strncpy(line, hist[hist_view], 255); line[255] = 0;
        llen = (int)strlen(line);
    }
    lpos = llen;
    redraw_line();
}

static void load_history(void)
{
    if (hist_loaded) return;
    hist_loaded = 1;
    FILE *f = fopen(HISTF, "r");
    if (!f) return;
    char buf[256];
    while (fgets(buf, sizeof buf, f) && hist_n < 32) {
        char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
        strncpy(hist[hist_n], buf, 127); hist[hist_n++][127] = 0;
    }
    fclose(f);
    LOG("historia: %d\n", hist_n);
}

static void save_history(void)
{
    FILE *f = fopen(HISTF, "w");
    if (!f) return;
    for (int i = 0; i < hist_n; i++) fprintf(f, "%s\n", hist[i]);
    fflush(f); fsync(fileno(f)); fclose(f);
}

/* ---------- desenho ---------- */
static int FW, FH;
static unsigned short *PX;
#define CW 12
#define CHH 16

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

#define COL_BG 0x0000
#define COL_FG 0xFFFF
#define COL_CUR 0xFFE0

static void layout_calc(void)
{
    /* a janela INTEIRA é terminal */
    tcols = (FW - 8) / CW;
    if (tcols > MAXC) tcols = MAXC;
    if (tcols < 8) tcols = 8;
    trows = (FH - 8) / CHH;
    if (trows < 4) trows = 4;
    if (trows > MAXL) trows = MAXL;
}

static void scroll_by(int delta)   /* + = ver mais antigo */
{
    scroll_off += delta;
    if (scroll_off < 0) scroll_off = 0;
    if (scroll_off > MAXL-trows) scroll_off = MAXL-trows;
    need_render = 1;
}

static void render_frame(void)
{
    layout_calc();
    arect(0, 0, FW, FH, COL_BG);

    int base = cur_row - (trows-1);
    if (base < 0) base = 0;
    if (scroll_off > 0) { base -= scroll_off; if (base < 0) base = 0; }
    if (base > MAXL-trows) base = MAXL-trows;
    for (int r = 0; r < trows && base+r < MAXL; r++) {
        char *ln = tbuf[base+r];
        for (int c = 0; c < tcols && c < MAXC; c++)
            if (ln[c]) achar(4+c*CW, 4+r*CHH, ln[c], COL_FG);
    }
    {
        static int blink = 0;
        blink = !blink;
        if (blink && scroll_off == 0) {
            int vy = cur_row - base;
            if (vy >= 0 && vy < trows && cur_col < tcols)
                arect(4+cur_col*CW, 4+vy*CHH, CW-1, CHH-1, COL_CUR);
        }
    }

    /* indicador de scroll (canto sup. direito) */
    if (scroll_off)
        atext(FW-8*12, 4, "[SCROLL]", COL_CUR);
}

/* ---------- input ---------- */
static int held[2], hcnt[2];

static int fire(int i, uint32_t k, int bit, int d0, int dr)
{
    int d = (k >> bit) & 1, f = 0;
    if (d && !held[i]) { f = 1; hcnt[i] = 0; }
    else if (d && ++hcnt[i] >= d0) { f = 1; hcnt[i] = d0 - dr; }
    held[i] = d;
    return f;
}

static void reset_input_state(void)
{
    memset(held, 0, sizeof held);
    memset(hcnt, 0, sizeof hcnt);
    hist_view = -1;
    has_saved = 0;
}

/* ---------- main ---------- */
static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s) { (void)s; g_stop = 1; }

static unsigned now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
}

int main(void)
{
    lg = fopen("/mnt/sdcard/mywm_term.log", "w");
    LOG("=== myterm v5.0 ===\n");

    signal(SIGTERM, on_stop);
    signal(SIGINT,  on_stop);

    int w = 660, h = 360, first = 1;

    while (!g_stop) {
        LOG("connect %dx%d\n", w, h);
        MywmConn c;
        if (mywm_connect(&c, w, h, "Terminal") != 0) {
            LOG("FATAL connect\n");
            kill_shell();
            if (hist_loaded) save_history();
            return 1;
        }
        PX = c.fr->pixels;
        FW = w; FH = h;
        reset_input_state();
        layout_calc();
        need_render = 1;

        if (first) {
            first = 0;
            load_history();
            if (spawn_pipe_shell() != 0) {
                LOG("FATAL shell\n");
                mywm_disconnect(&c);
                return 1;
            }
            pwd_clear = 1;
            term_puts("Terminal MYWM v5\n");
            term_puts("Texto: mykeyboard (menu)\n");
            term_puts("PGUP/PGDN=scroll  L=scroll\n\n");
            write(io_wr, "pwd\n", 4);
            pwd_capture = 1; pwd_len = 0;
            show_prompt_cwd();
        } else {
            LOG("reconectado %dx%d\n", w, h);
        }

        int reconnect = 0;
        unsigned blink_t = now_ms();

        while (!g_stop && !reconnect) {
            /* ---- output do shell ---- */
            if (io_rd >= 0) {
                fd_set rf;
                FD_ZERO(&rf); FD_SET(io_rd, &rf);
                struct timeval tv = {0, 20000};
                int rv = select(io_rd+1, &rf, NULL, NULL, &tv);
                if (rv > 0 && FD_ISSET(io_rd, &rf)) {
                    char ob[2048];
                    int n = read(io_rd, ob, sizeof ob);
                    if (n > 0) {
                        if (pwd_capture) {
                            for (int i = 0; i < n; i++) {
                                if (ob[i] == '\n') {
                                    pwd_buf[pwd_len] = 0; pwd_len = 0;
                                    if (pwd_buf[0] == '/') {
                                        strncpy(cwd, pwd_buf, 255); cwd[255] = 0;
                                        pwd_capture = 0;
                                        if (pwd_clear) {
                                            pwd_clear = 0;
                                            memset(tbuf, 0, sizeof tbuf);
                                            cur_row = cur_col = 0;
                                        }
                                        show_prompt_cwd();
                                        if (i+1 < n) term_feed(ob+i+1, n-i-1);
                                        break;
                                    }
                                    term_puts(pwd_buf); term_puts("\n");
                                } else if (pwd_len < 255) pwd_buf[pwd_len++] = ob[i];
                            }
                        } else {
                            term_feed(ob, n);
                            scroll_off = 0;
                        }
                    } else if (n == 0) {
                        shell_respawn();
                        continue;
                    }
                }
            } else {
                usleep(20000);
            }

            /* ---- resize pedido pelo compositor (R1/□) ---- */
            int rw, rh;
            if (mywm_poll_resize(&c, &rw, &rh)) {
                LOG("resize -> %dx%d\n", rw, rh);
                w = rw; h = rh;
                reconnect = 1;
                break;
            }

            /* ---- gamepad: L = scroll (rato sobre a janela) ---- */
            uint32_t k = mywm_get_keys(&c);
            if (fire(0, k, BIT_L, 1<<29, 0)) scroll_by(trows/2);

            /* ---- texto do MYKEYBOARD (foco-based) ---- */
            char txt[16];
            int tn = mywm_get_text(&c, txt, sizeof txt);
            for (int i = 0; i < tn; i++) {
                unsigned char ch = (unsigned char)txt[i];
                if (ch >= 32 && ch < 127) {
                    line_insert((char)ch);
                } else switch (ch) {
                case MYWM_KC_ENTER: submit_line(); break;
                case MYWM_KC_BS:
                case MYWM_KC_DEL:   line_backspace(); break;
                case MYWM_KC_TAB:
                    line_insert(' '); line_insert(' ');
                    line_insert(' '); line_insert(' ');
                    break;
                case MYWM_KC_LEFT:  if (lpos > 0)   { lpos--; redraw_line(); } break;
                case MYWM_KC_RIGHT: if (lpos < llen){ lpos++; redraw_line(); } break;
                case MYWM_KC_UP:    hist_load(-1); break;
                case MYWM_KC_DOWN:  hist_load(1);  break;
                case MYWM_KC_HOME:  lpos = 0;   redraw_line(); break;
                case MYWM_KC_END:   lpos = llen; redraw_line(); break;
                case MYWM_KC_PGUP:  scroll_by(trows/2);   break;
                case MYWM_KC_PGDN:  scroll_by(-(trows/2)); break;
                case MYWM_KC_CTL:   if (shell_pid > 0) kill(-shell_pid, SIGINT); break;
                default: break;   /* ESC e outros: ignorados */
                }
            }

            /* ---- blink do cursor (2x/s) ---- */
            unsigned nw = now_ms();
            if ((int)(nw - blink_t) >= 500) {
                blink_t = nw;
                need_render = 1;
            }

            /* ---- render condicional ---- */
            if (need_render) {
                render_frame();
                mywm_end(&c);
                need_render = 0;
            }
        }

        mywm_disconnect(&c);
        PX = NULL;
        /* reconectar (resize) ou sair — o shell e o conteúdo sobrevivem */
    }

    kill_shell();
    save_history();
    LOG("=== fim ===\n");
    if (lg) fclose(lg);
    return 0;
}