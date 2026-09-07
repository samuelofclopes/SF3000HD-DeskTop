/* demo.c — app de teste MYWM v2
   Gradiente pré-computado (zero floats), círculo do rato, resize.
   Log: /mnt/sdcard/mywm_demo.log */
#include "mywm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *lg;
#define LOG(...) do{ if(lg){fprintf(lg,__VA_ARGS__);fflush(lg);fsync(fileno(lg));} }while(0)

int main(void)
{
    lg = fopen("/mnt/sdcard/mywm_demo.log", "w");
    int w = 320, h = 200;

    while (1) {
        LOG("=== demo %dx%d ===\n", w, h);
        MywmConn c;
        if (mywm_connect(&c, w, h, "Demo") != 0) {
            LOG("FATAL connect\n");
            return 1;
        }

        /* gradiente pré-computado — zero floats, zero FPU */
        unsigned short *grad = malloc((size_t)w * h * 2);
        for (int y = 0; y < h; y++) {
            unsigned short g = (unsigned short)((y * 63 / h) << 5);
            for (int x = 0; x < w; x++)
                grad[(size_t)y * w + x] = (unsigned short)((x * 31 / w) | g);
        }

        uint32_t t = 0;
        while (1) {
            mywm_get_keys(&c);    /* consumir input (o círculo usa mouse_x/y) */

            int fw = (int)c.fr->w, fh = (int)c.fr->h;

            /* compor frame: gradiente + onda azul (inteiros) */
            int phase = (int)(t & 63);
            for (int y = 0; y < fh; y++) {
                int blue = ((y + phase) & 63) * 31 / 63;
                unsigned short b = (unsigned short)(blue << 11);
                const unsigned short *s = &grad[(size_t)y * fw];
                unsigned short *d = &c.fr->pixels[(size_t)y * fw];
                for (int x = 0; x < fw; x++)
                    d[x] = (unsigned short)(s[x] | b);
            }

            /* círculo no rato (prova do routing) */
            int ax = (int)c.fr->mouse_x, ay = (int)c.fr->mouse_y;
            for (int dy = -8; dy <= 8; dy++)
                for (int dx = -8; dx <= 8; dx++)
                    if (dx*dx + dy*dy <= 64 && ax+dx >= 0 && ax+dx < fw
                                               && ay+dy >= 0 && ay+dy < fh)
                        c.fr->pixels[(size_t)(ay+dy)*fw + (ax+dx)] = 0xFFFF;

            mywm_end(&c);
            t++;

            int nw, nh;
            if (mywm_poll_resize(&c, &nw, &nh)) {
                LOG("resize %dx%d -> %dx%d\n", w, h, nw, nh);
                w = nw; h = nh;
                free(grad);
                mywm_disconnect(&c);
                break;    /* reconectar com o tamanho novo */
            }
            usleep(20000);
        }
    }
    return 0;
}