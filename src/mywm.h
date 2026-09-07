/* ============================================================
 * mywm.h — protocolo MYWM v4
 *
 * v4: TECLADO DE SISTEMA.
 *   - MywmMsg_TEXT: a app de teclado envia chars ao mywm,
 *     que os entrega à janela ATIVA (não-teclado) via
 *     frame->text_buf (canal de texto, foco-based).
 *   - frame->is_keyboard: janela de teclado — sempre no topo,
 *     NUNCA recebe foco; clicar nela não rouba o foco.
 *   - códigos de controlo MYWM_KC_* partilhados por teclado
 *     e apps (setas, home/end, pgup/pgdn...).
 *
 * v3: dpad_grab (app agarra o D-pad/A).
 * v2: âncoras com ponto, slots fixos, server dono dos shms.
 *
 *   /tmp/mywm          âncora do SERVER (nunca apagar em runtime)
 *   /tmp/mywm.<pid>    âncora de cada APP (apagada no disconnect)
 * ============================================================ */
#ifndef MYWM_H
#define MYWM_H
#include <stdint.h>

#define MYWM_SHM_PATH   "/tmp/mywm"
#define MYWM_MAGIC      0x4D59574D   /* "MYWM" */
#define MYWM_PROTO_VER  4

/* ---- códigos de controlo do canal de texto ---- */
#define MYWM_KC_LEFT  0x01   /* cursor: esquerda  */
#define MYWM_KC_RIGHT 0x02   /* cursor: direita   */
#define MYWM_KC_UP    0x03   /* histórico: anterior */
#define MYWM_KC_DOWN  0x04   /* histórico: seguinte */
#define MYWM_KC_HOME  0x05   /* início da linha  */
#define MYWM_KC_END   0x06   /* fim da linha     */
#define MYWM_KC_PGUP  0x07   /* scroll: mais antigo */
#define MYWM_KC_PGDN  0x0B   /* scroll: mais recente */
#define MYWM_KC_BS    0x08   /* backspace (ASCII)  */
#define MYWM_KC_TAB   0x09   /* tab      (ASCII)  */
#define MYWM_KC_ENTER 0x0A   /* enter    (ASCII)  */
#define MYWM_KC_ESC   0x1B   /* escape   (ASCII)  */
#define MYWM_KC_DEL   0x7F   /* delete   (ASCII)  */
#define MYWM_KC_PGUP  0x07   /* scroll: mais antigo */
#define MYWM_KC_PGDN  0x0B   /* scroll: mais recente */
#define MYWM_KC_CTL   0x0C   /* ctrl (aplica à ação: ex CTL+C) */
#define MYWM_KC_BS    0x08   /* backspace (ASCII)  */
/* ---- frame de cada app (shm por app, RGB565) ---- */
typedef struct {
    uint32_t magic;
    uint32_t proto;
    uint32_t app_id;                /* pid da app */
    int32_t  w, h;                  /* dimensões do frame */
    volatile uint32_t frame_seq;    /* incrementa a cada frame */
    volatile uint32_t dirty;        /* 1 = redesenhar */
    /* input (server -> app) */
    volatile uint32_t keys;         /* bitmask dos botões */
    volatile uint32_t mouse_x, mouse_y;
    volatile uint32_t mouse_btn;    /* bit0 = A */
    volatile uint32_t got_input;    /* server põe 1; app limpa 0 */
    /* resize (server -> app) */
    volatile uint32_t resize_w, resize_h;
    volatile uint32_t resize_pending;
    /* v3: app agarra o D-pad/A (server congela o rato e não clica) */
    volatile uint32_t dpad_grab;
    /* v4: teclado de sistema */
    volatile uint32_t is_keyboard;  /* 1 = janela de teclado (nunca foco) */
    volatile uint32_t text_seq;     /* +1 quando text_buf recebe chars */
    volatile uint32_t text_len;     /* chars válidos em text_buf */
    volatile char     text_buf[16]; /* mywm escreve; a app drena */
    uint16_t pixels[1];             /* frame w*h RGB565 */
} MywmFrame;

/* ---- mensagens (app -> server): slot fixo por app ---- */
#define MYWM_MSG_NONE    0
#define MYWM_MSG_CREATE  1
#define MYWM_MSG_CLOSE   2
#define MYWM_MSG_TEXT    3   /* v4: texto -> janela ativa (não-teclado) */

typedef struct {
    volatile uint32_t used;         /* 1 = há mensagem; server limpa a 0 */
    uint32_t app_id;
    uint32_t msg;
    int32_t  a;                     /* CREATE: shmid do frame */
    char     title[40];             /* CREATE: título da janela */
    char     text[16];              /* v4: TEXT: chars a entregar */
} MywmMsg;

#define MYWM_MAX_APPS 8
typedef struct {
    uint32_t magic, proto;
    MywmMsg slots[MYWM_MAX_APPS];
} MywmGlobal;

/* ---- API do cliente ---- */
typedef struct {
    MywmFrame *fr;
    int shm_id;
    int my_id;
    int slot;
} MywmConn;

int  mywm_connect(MywmConn *c, int w, int h, const char *title);
void mywm_disconnect(MywmConn *c);
void mywm_end(MywmConn *c);          /* marca frame pronto */
uint32_t mywm_get_keys(MywmConn *c); /* estado das teclas (cache) */
int  mywm_poll_resize(MywmConn *c, int *w, int *h);

/* v4: teclado de sistema */
void mywm_set_keyboard(MywmConn *c);            /* marca a app como teclado */
void mywm_send_text(MywmConn *c, const char *txt); /* envia chars ao foco  */
int  mywm_get_text(MywmConn *c, char *out, int max); /* consome chars recebidos */

#endif