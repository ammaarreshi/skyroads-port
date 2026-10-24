#include "render.h"
#include "tables.h"
#include <stdlib.h>
#include <string.h>

/* ---- TREKDAT second-stage expansion (fn_3a7a) -------------------------- */

static uint8_t *expand_obj(const sr_trek_obj *o)
{
    uint8_t *out = malloc(o->raw_size);
    if (!out)
        return NULL;
    const uint8_t *src = o->data + o->data_ofs;
    const uint8_t *end = o->data + o->raw_size;
    uint8_t *dst = out;

    memcpy(dst, src, 0x270);              /* directory */
    src += 0x270;
    dst += 0x270;
    for (int rec = 0; rec < 0x410 && src < end; rec++) {
        *dst++ = *src++;                  /* color */
        *dst++ = *src++;                  /* anchor lo */
        *dst++ = *src++;                  /* anchor hi */
        for (;;) {
            uint8_t b = *src++;
            *dst++ = b;
            if (b == 0xFF)
                break;
            *dst++ = *src++;              /* length */
            *dst++ = 0;                   /* pad */
        }
    }
    return out;
}

bool sr_render_init(sr_render *r, const sr_assets *a)
{
    memset(r, 0, sizeof *r);
    if (a->n_trek < 8)
        return false;
    for (int i = 0; i < 8; i++) {
        r->exp[i] = expand_obj(&a->trek[i]);
        if (!r->exp[i])
            return false;
    }
    return true;
}

void sr_render_free(sr_render *r)
{
    for (int i = 0; i < 8; i++)
        free(r->exp[i]);
}

void sr_render_set_world(sr_render *r, const sr_assets *a)
{
    memset(r->pristine, 0, sizeof r->pristine);
    if (a->world.n_picts > 0) {
        const sr_pict *w = &a->world.picts[0];
        for (int y = 0; y < w->h && y < SR_SCREEN_H; y++)
            memcpy(r->pristine + y * 320, w->pixels + (size_t)y * w->w, w->w);
    }
    if (a->dashbrd.n_picts > 0) {
        const sr_pict *d = &a->dashbrd.picts[0];
        int y0 = d->screen_ofs / 320, x0 = d->screen_ofs % 320;
        for (int y = 0; y < d->h; y++)
            for (int x = 0; x < d->w; x++) {
                uint8_t c = d->pixels[(size_t)y * d->w + x];
                if (c)
                    r->pristine[(y0 + y) * 320 + x0 + x] = c;
            }
    }
}

/* ---- span fills (0x3137 left / 0x3174 right) --------------------------- */

typedef struct {
    uint8_t *vp;              /* fb pixels + 32*320 (viewport bias) */
    const uint8_t *obj;       /* current phase object */
    const uint8_t *si;        /* record cursor */
} fillctx;

/* One record: color k, anchor, spans. half=0 left, 1 right (mirrored).
 * color_override: 0 = use record color, else patched color. */
static void fill_record(fillctx *c, int half, int color_override)
{
    uint8_t k = *c->si++;
    if (color_override)
        k = (uint8_t)color_override;
    uint8_t fill = sr_quad[k < 74 ? k : 0][half];
    int anchor = c->si[0] | (c->si[1] << 8);
    c->si += 2;
    for (;;) {
        uint8_t off = *c->si++;
        if (off == 0xFF)
            break;
        uint8_t len = *c->si++;
        c->si++;                               /* pad */
        if (half == 0) {
            int di = anchor - off;
            for (int i = 0; i < len; i++)
                c->vp[di + i] = fill;
        } else {
            int di = anchor - 1 + off;
            for (int i = 0; i < len; i++)
                c->vp[di - i] = fill;
        }
        anchor += 320;
    }
}

static void skip_record(fillctx *c)            /* fn_31b5 */
{
    c->si += 3;
    while (*c->si != 0xFF)
        c->si += 3;
    c->si++;
}

/* ---- composers (jump table ds:0xb7f) ----------------------------------- */

typedef struct {
    fillctx f;
    const uint8_t *dir;       /* directory cell (6 u16 record offsets) */
    uint16_t tile;            /* current grid tile */
    uint16_t nearer;          /* next row toward camera */
    uint16_t inner;           /* neighbor toward screen center */
    int half;
} compctx;

static void seek_kind(compctx *cc, int kind)
{
    uint16_t off = (uint16_t)(cc->dir[kind*2] | (cc->dir[kind*2+1] << 8));
    cc->f.si = cc->f.obj + off;
}

static int shape(uint16_t t) { return (t >> 8) & 0xf; }

static void compose_floor(compctx *cc)         /* fn_2e50 */
{
    int c = cc->tile & 0xf;
    if (c == 0)
        return;
    seek_kind(cc, 0);
    fill_record(&cc->f, cc->half, c);                       /* top */
    if ((cc->inner & 0xf) == 0)
        fill_record(&cc->f, cc->half, c + 0x1e);            /* side edge */
    else
        skip_record(&cc->f);
    if ((cc->nearer & 0xf) == 0)
        fill_record(&cc->f, cc->half, c + 0xf);             /* front edge */
}

static int blockcolor(uint16_t t)
{
    int c = (t >> 4) & 0xf;
    return c ? c : 0x3d;
}

static void compose_lowblock(compctx *cc)      /* 0x2e9f */
{
    compose_floor(cc);
    if (shape(cc->nearer) < 2) {
        seek_kind(cc, 3);
        fill_record(&cc->f, cc->half, 0);                   /* top faces */
    }
    seek_kind(cc, 2);
    fill_record(&cc->f, cc->half, blockcolor(cc->tile));    /* front */
    if (shape(cc->inner) < 2)
        fill_record(&cc->f, cc->half, 0);                   /* side */
}

static void compose_tun_low(compctx *cc)       /* 0x2ee1 */
{
    compose_floor(cc);
    if (shape(cc->nearer) < 2) {
        seek_kind(cc, 1);
        fill_record(&cc->f, cc->half, 0x41);                /* entrance */
    }
    seek_kind(cc, 2);
    fill_record(&cc->f, cc->half, blockcolor(cc->tile));
    if (shape(cc->inner) < 2)
        fill_record(&cc->f, cc->half, 0);
    if (shape(cc->nearer) < 2) {
        seek_kind(cc, 3);
        skip_record(&cc->f);                                /* plain top */
        fill_record(&cc->f, cc->half, 0);                   /* split tops */
        fill_record(&cc->f, cc->half, 0);
    }
}

static void compose_highblock(compctx *cc)     /* 0x2f3c */
{
    compose_floor(cc);
    if (shape(cc->nearer) < 2) {
        seek_kind(cc, 3);
        fill_record(&cc->f, cc->half, 0);
    }
    seek_kind(cc, 2);
    skip_record(&cc->f);                                    /* no lower front */
    if (shape(cc->inner) < 2)
        fill_record(&cc->f, cc->half, 0);                   /* lower side */
    seek_kind(cc, 5);
    fill_record(&cc->f, cc->half, blockcolor(cc->tile));    /* upper front */
    if (shape(cc->inner) < 4)
        fill_record(&cc->f, cc->half, 0);                   /* upper side */
    else
        skip_record(&cc->f);
    if (shape(cc->nearer) < 4)
        fill_record(&cc->f, cc->half, 0);
}

static void compose_tunnel(compctx *cc)        /* 0x303d */
{
    compose_floor(cc);
    if (shape(cc->nearer) < 1) {
        seek_kind(cc, 1);
        fill_record(&cc->f, cc->half, 0x43);                /* front wall */
    }
    seek_kind(cc, 4);
    for (int i = 0; i < 6; i++)
        fill_record(&cc->f, cc->half, 0);                   /* arch gradient */
    if (shape(cc->nearer) < 1) {
        fill_record(&cc->f, cc->half, 0);                   /* inner rim x2 */
        fill_record(&cc->f, cc->half, 0);
    }
}

static void compose_tun_high(compctx *cc)      /* 0x2fb0 */
{
    compose_floor(cc);
    if (shape(cc->nearer) < 2) {
        seek_kind(cc, 1);
        fill_record(&cc->f, cc->half, 0x41);
    }
    seek_kind(cc, 2);
    skip_record(&cc->f);
    if (shape(cc->inner) < 2)
        fill_record(&cc->f, cc->half, 0);
    seek_kind(cc, 4);
    for (int i = 0; i < 6; i++)
        fill_record(&cc->f, cc->half, 0);
    seek_kind(cc, 5);
    fill_record(&cc->f, cc->half, blockcolor(cc->tile));
    if (shape(cc->inner) < 4)
        fill_record(&cc->f, cc->half, 0);
    else
        skip_record(&cc->f);
    if (shape(cc->nearer) < 4)
        fill_record(&cc->f, cc->half, 0);
}

static void compose_tile(compctx *cc)
{
    switch (shape(cc->tile)) {
    case 0: compose_floor(cc); break;
    case 1: compose_tunnel(cc); break;
    case 2: compose_lowblock(cc); break;
    case 3: compose_tun_low(cc); break;
    case 4: compose_highblock(cc); break;
    case 5: compose_tun_high(cc); break;
    default: break;                            /* 6..15: nothing */
    }
}

/* ---- ship sprite + shadow ---------------------------------------------- */

static int cowl_hidden(int x, int y)
{
    if (y < 0 || y > 137)
        return 1;
    uint16_t hw = sr_cowl[y];
    if (!hw)
        return 0;
    return x > 160 - (int)hw && x < 160 + (int)hw;
}

static void draw_ship(sr_fb *fb, const uint8_t *cell, int left, int top)
{
    for (int j = 0; j < 29; j++)               /* screen columns */
        for (int i = 0; i < 24; i++) {         /* screen rows */
            uint8_t px = cell[j * 24 + i];
            if (!px)
                continue;
            int x = left + j, y = top + i;
            if (x < 0 || x >= 320 || cowl_hidden(x, y))
                continue;
            fb->px[y * 320 + x] = px;
        }
}

static void draw_shadow(sr_fb *fb, int left, int top, int clearance)
{
    int map = clearance / 5;
    if (map < 0 || map >= 5)
        return;
    const uint8_t *stencil = sr_shadow[map];
    for (int j = 0; j < 29; j++)
        for (int i = 0; i < 9; i++) {
            if (!stencil[i * 29 + j])
                continue;
            int x = left + j, y = top + i;
            if (x < 0 || x >= 320 || cowl_hidden(x, y))
                continue;
            uint8_t *p = &fb->px[y * 320 + x];
            if (*p >= 1 && *p <= 15)
                *p = (uint8_t)(*p + 0x2d);
            else if (*p == 0x3d)
                *p = 0x40;
        }
}

/* ground support height for the shadow (fn_0b71) */
static uint16_t support(const sr_play *p, uint32_t z, uint16_t x, int in_tun)
{
    uint16_t t = sr_tile_at(p, z, x);
    int sh = (t >> 8) & 0xf;
    if (sh >= 2 && sh <= 5)
        return sr_blocktop[sh];
    if (sh == 1 && !in_tun)
        return 0;
    return (t & 0xf) ? 0x2800 : 0;
}

/* ---- frame -------------------------------------------------------------- */

void sr_render_frame(sr_render *r, sr_fb *fb, const sr_assets *a,
                     const sr_play *p, uint32_t tick, int on_sticky)
{
    /* full repaint from pristine (pixel-equal to restore+incremental) */
    memcpy(fb->px, r->pristine, sizeof fb->px);

    int in_tun = sr_in_tunnel(p, p->z, p->x, p->y);

    /* sprite selection (fn_0be3) */
    int tilt = (p->x / 0x80 - 95) / 46;
    if (tilt < 0) tilt = 0;
    if (tilt > 6) tilt = 6;
    int sprite;
    if (p->expl_ctr) {
        sprite = p->expl_ctr / 3;
        if (sprite >= 14)
            sprite = -1;
    } else {
        int pitch = 0;
        if (!in_tun) {
            if (p->yvel <= -0x163 || p->y < 0x2800) pitch = 2;
            else if (p->yvel >= 0x163) pitch = 1;
        }
        static const int flame_tab[4] = { 0, 1, 2, 1 };
        int flame = (p->end_state == SR_RES_NO_FUEL)
                    ? 0 : flame_tab[(tick / 2) & 3];
        sprite = 14 + 9 * tilt + 3 * pitch + flame;
    }

    uint16_t pos = (uint16_t)(p->z / 0x2000);
    int alt = ((on_sticky ? p->y - 0x80 : p->y)) / 0x80;
    int ship_x = p->x / 0x80 + sr_pan[tilt];

    /* composer pass: 10 rows far->near, dir rows 0..9 */
    uint8_t *vp = fb->px + 32 * 320;
    const uint8_t *obj = r->exp[pos & 7];
    int baserow = pos >> 3;

    for (int dr = 0; dr < 10; dr++) {
        int grow = baserow + 7 - dr;           /* grid row for this band */
        int dirrow = dr;
        int ship_row = (dr == 7);
        int dirbases[2];
        int nbases = 1;
        dirbases[0] = dirrow * 0x30;
        if (ship_row) {
            dirbases[0] = 0x210;               /* pre-ship */
            dirbases[1] = 0x240;               /* post-ship */
            nbases = 2;
        }
        for (int b = 0; b < nbases; b++) {
            if (ship_row && b == 1 && sprite >= 0) {
                /* ship drawn between pre- and post-ship geometry */
                const sr_pict *cars = &a->cars.picts[0];
                const uint8_t *cell = cars->pixels + (size_t)sprite * 0x2d0;
                int top = 0x9d - alt;
                int left = ship_x - 0x6e;
                draw_ship(fb, cell, left, top);
                if (!p->expl_ctr) {
                    uint16_t g1 = support(p, p->z, (uint16_t)(p->x - 0x380), in_tun);
                    uint16_t g2 = support(p, p->z, (uint16_t)(p->x + 0x380), in_tun);
                    uint16_t g = g1 > g2 ? g1 : g2;
                    if (g) {
                        int clearance = (p->y - g) / 0x80;
                        if (clearance >= 0)
                            draw_shadow(fb, left, top + 0x10 + clearance,
                                        clearance);
                    }
                }
            }
            for (int half = 0; half < 2; half++) {
                for (int ci = 0; ci < 4; ci++) {
                    int gcol = half == 0 ? ci : 6 - ci;
                    uint16_t tile = 0, nearer = 0, inner = 0;
                    if (grow >= 0 && grow < 500) {
                        const uint16_t *row = p->road->cells + grow * 7;
                        tile = row[gcol];
                        int in_col = half == 0 ? gcol + 1 : gcol - 1;
                        if (in_col >= 0 && in_col < 7)
                            inner = row[in_col];
                        if (grow - 1 >= 0)
                            nearer = p->road->cells[(grow - 1) * 7 + gcol];
                    }
                    compctx cc;
                    cc.f.vp = vp;
                    cc.f.obj = obj;
                    cc.f.si = NULL;
                    cc.dir = obj + dirbases[b] + ci * 0xc;
                    cc.tile = tile;
                    cc.nearer = nearer;
                    cc.inner = inner;
                    cc.half = half;
                    compose_tile(&cc);
                }
            }
        }
    }
}
