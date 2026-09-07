/* ============================================================
 * paint.c — app MYWM v2: desenhar com o rato
 *   B limpa | X muda cor | Y sai
 *   Log: /mnt/sdcard/mywm_paint.log
 * ============================================================ */
#include "mywm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *lg;
#define LOG(...) do{ if(lg){fprintf(lg,__VA_ARGS__);fflush(lg);fsync(fileno(lg));} }while(0)

int main(void)
{
    lg = fopen("/mnt/sdcard/mywm_paint.log", "w");
    int w = 300, h = 220;

    while (1) {
        LOG("=== paint %dx%d ===\n", w, h);
        MywmConn c;
        if (mywm_connect(&c, w, h, "Paint") != 0) {
            LOG("FATAL connect\n");
            return 1;
        }

        unsigned short *canvas = malloc((size_t)w * h * 2);
        memset(canvas, 0, (size_t)w * h * 2);

        int lastx = -1, lasty = -1, quit = 0;
        unsigned short cor = 0x07E0;   /* verde */
        int b_prev = 0, x_prev = 0, y_prev = 0;

        while (!quit) {
            /* input: ler as keys + estado do rato diretamente do frame */
            uint32_t k = mywm_get_keys(&c);
            int mx = (int)c.fr->mouse_x;
            int my = (int)c.fr->mouse_y;
            int btn = (int)c.fr->mouse_btn;
            int fw = (int)c.fr->w, fh = (int)c.fr->h;

            /* edges (1 clique = 1 ação) */
            int b_now  = (k >> 14) & 1;   /* B bit 14 */
            int x_now  = (k >> 12) & 1;   /* X bit 12 */
            int y_now  = (k >> 15) & 1;   /* Y bit 15 */

            /* desenhar quando o botão do rato (A) está premido */
            if (btn && mx >= 0 && mx < fw && my >= 0 && my < fh) {
                if (lastx >= 0) {
                    /* linha do ponto anterior ao atual */
                    int dx = mx - lastx, dy = my - lasty;
                    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                    if (n < 1) n = 1;
                    for (int i = 0; i <= n; i++)
                        canvas[(size_t)(lasty + dy*i/n) * fw + (lastx + dx*i/n)] = cor;
                }
                lastx = mx;
                lasty = my;
            } else {
                lastx = -1;
            }

            /* B (edge) = limpar */
            if (b_now && !b_prev)
                memset(canvas, 0, (size_t)w * h * 2);

            /* X (edge) = mudar cor */
            if (x_now && !x_prev)
                cor = cor == 0x07E0 ? 0xF800
                    : cor == 0xF800 ? 0x001F
                    : cor == 0x001F ? 0xFFE0
                    : 0x07E0;

            /* Y (edge) = sair */
            if (y_now && !y_prev)
                quit = 1;

            b_prev = b_now;
            x_prev = x_now;
            y_prev = y_now;

            /* compor o frame */
            memcpy(c.fr->pixels, canvas, (size_t)fw * fh * 2);
            mywm_end(&c);

            /* resize? */
            int nw, nh;
            if (mywm_poll_resize(&c, &nw, &nh)) {
                LOG("resize %dx%d -> %dx%d\n", w, h, nw, nh);
                w = nw; h = nh;
                free(canvas);
                mywm_disconnect(&c);
                break;    /* reconectar com o tamanho novo */
            }
            if (quit) { mywm_disconnect(&c); break; }
            usleep(20000);
        }
        if (quit) break;
    }

    LOG("=== paint fim ===\n");
    return 0;
}