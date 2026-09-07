/* ============================================================
 * myapp_lib.c — cliente MYWM v4 (rev.4)
 * Compilar junto com cada app: gcc ... app.c myapp_lib.c ...
 *
 * rev.4: protocolo v4 — mywm_set_keyboard / mywm_send_text /
 *        mywm_get_text (canal de texto do teclado de sistema).
 * rev.2: mywm_get_keys devolve o ÚLTIMO estado conhecido (cache).
 * ============================================================ */
#include "mywm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>

/* caches (1 ligação por processo) */
static uint32_t g_last_keys = 0;
static uint32_t g_last_text_seq = 0;

int mywm_connect(MywmConn *c, int w, int h, const char *title)
{
    memset(c, 0, sizeof *c);
    c->my_id = (int)getpid();
    c->slot  = c->my_id % MYWM_MAX_APPS;

    /* 1) abrir a área global */
    key_t gk = ftok(MYWM_SHM_PATH, 1);
    if (gk == -1) return -1;
    int gid = shmget(gk, 0, 0666);
    if (gid < 0) return -1;
    MywmGlobal *gg = shmat(gid, NULL, 0);
    if (gg == (void *)-1) return -1;
    if (gg->magic != MYWM_MAGIC || gg->proto != MYWM_PROTO_VER) {
        shmdt(gg);
        return -1;
    }

    /* 2) criar o frame shm próprio */
    char anchor[64];
    snprintf(anchor, sizeof anchor, "/tmp/mywm.%d", c->my_id);
    FILE *f = fopen(anchor, "w");
    if (!f) { shmdt(gg); return -1; }
    fclose(f);

    key_t fk = ftok(anchor, 2);
    size_t sz = sizeof(MywmFrame) + (size_t)w * h * 2;
    int sid = shmget(fk, (int)sz, IPC_CREAT | 0666);
    if (sid < 0) { unlink(anchor); shmdt(gg); return -1; }

    MywmFrame *fr = shmat(sid, NULL, 0);
    if (fr == (void *)-1) { unlink(anchor); shmdt(gg); return -1; }

    /* 3) inicializar o frame */
    fr->magic = MYWM_MAGIC;
    fr->proto = MYWM_PROTO_VER;
    fr->app_id = (uint32_t)c->my_id;
    fr->w = w;
    fr->h = h;
    fr->frame_seq = 0;
    fr->dirty = 0;
    fr->keys = 0;
    fr->mouse_x = 0;
    fr->mouse_y = 0;
    fr->mouse_btn = 0;
    fr->got_input = 0;
    fr->resize_w = 0;
    fr->resize_h = 0;
    fr->resize_pending = 0;
    fr->dpad_grab = 0;
    fr->is_keyboard = 0;
    fr->text_seq = 0;
    fr->text_len = 0;
    memset((void *)fr->text_buf, 0, sizeof fr->text_buf);

    /* 4) escrever CREATE no nosso slot (zero corridas):
       despublicar -> escrever tudo -> publicar por último */
    MywmMsg *m = &gg->slots[c->slot];
    m->used = 0;
    m->app_id = (uint32_t)c->my_id;
    m->msg = MYWM_MSG_CREATE;
    m->a = sid;
    strncpy(m->title, title, sizeof m->title - 1);
    m->title[sizeof m->title - 1] = 0;
    memset(m->text, 0, sizeof m->text);
    m->used = 1;

    shmdt(gg);
    c->fr = fr;
    c->shm_id = sid;
    g_last_keys = 0;
    g_last_text_seq = 0;
    return 0;
}

void mywm_disconnect(MywmConn *c)
{
    if (!c->fr) return;

    /* avisar o server (CLOSE no nosso slot) */
    key_t gk = ftok(MYWM_SHM_PATH, 1);
    if (gk != -1) {
        int gid = shmget(gk, 0, 0666);
        if (gid >= 0) {
            MywmGlobal *gg = shmat(gid, NULL, 0);
            if (gg != (void *)-1 && gg->magic == MYWM_MAGIC) {
                MywmMsg *m = &gg->slots[c->slot];
                m->used = 0;
                m->app_id = (uint32_t)c->my_id;
                m->msg = MYWM_MSG_CLOSE;
                m->a = c->shm_id;
                m->used = 1;
                shmdt(gg);
            }
        }
    }

    /* desmapear o nosso frame (o SERVER faz IPC_RMID) */
    shmdt(c->fr);
    c->fr = NULL;

    /* apagar a âncora */
    char anchor[64];
    snprintf(anchor, sizeof anchor, "/tmp/mywm.%d", c->my_id);
    unlink(anchor);
}

void mywm_end(MywmConn *c)
{
    if (!c->fr) return;
    c->fr->frame_seq++;
    c->fr->dirty = 1;
}

uint32_t mywm_get_keys(MywmConn *c)
{
    if (!c || !c->fr) return 0;
    if (c->fr->got_input) {
        c->fr->got_input = 0;
        g_last_keys = c->fr->keys;
    } else if (c->fr->keys == 0) {
        /* release visível no frame mesmo sem got_input (compat.) */
        g_last_keys = 0;
    }
    return g_last_keys;
}

int mywm_poll_resize(MywmConn *c, int *w, int *h)
{
    if (!c->fr || !c->fr->resize_pending) return 0;
    *w = (int)c->fr->resize_w;
    *h = (int)c->fr->resize_h;
    c->fr->resize_pending = 0;
    return 1;
}

/* ===== v4: teclado de sistema ===== */

void mywm_set_keyboard(MywmConn *c)
{
    if (c && c->fr) c->fr->is_keyboard = 1;
}

/* enviar texto ao mywm — ele entrega à janela ATIVA (não-teclado) */
void mywm_send_text(MywmConn *c, const char *txt)
{
    if (!c || !txt || !txt[0]) return;
    key_t gk = ftok(MYWM_SHM_PATH, 1);
    if (gk == -1) return;
    int gid = shmget(gk, 0, 0666);
    if (gid < 0) return;
    MywmGlobal *gg = shmat(gid, NULL, 0);
    if (gg == (void *)-1) return;
    if (gg->magic == MYWM_MAGIC) {
        MywmMsg *m = &gg->slots[c->slot];
        m->used = 0;
        m->app_id = (uint32_t)c->my_id;
        m->msg = MYWM_MSG_TEXT;
        m->a = 0;
        memset(m->text, 0, sizeof m->text);
        strncpy(m->text, txt, sizeof m->text - 1);
        m->used = 1;    /* publicar por último */
    }
    shmdt(gg);
}

/* consumir o texto recebido (0 se não há nada novo) */
int mywm_get_text(MywmConn *c, char *out, int max)
{
    if (!c || !c->fr || !out || max <= 0) return 0;
    MywmFrame *fr = c->fr;
    if (fr->text_seq == g_last_text_seq) return 0;
    int n = (int)fr->text_len;
    if (n > max) n = max;
    if (n > 0) memcpy(out, (const void *)fr->text_buf, (size_t)n);
    g_last_text_seq = fr->text_seq;
    fr->text_len = 0;   /* drenar — o mywm volta a encher */
    return n;
}