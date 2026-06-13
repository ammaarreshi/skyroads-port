/* SDL2 shell: works on macOS/Windows/Linux/iOS and (via Emscripten) web.
 * Renders the core's 320x200 8bpp framebuffer through a streaming texture
 * with aspect-correct integer-ish scaling, and drives exact 36.0036 Hz
 * ticks from a rational accumulator. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/game.h"
#include "../../core/audio.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

static char data_dir[1024] = ".";
static char pref_dir[1024] = "";     /* writable dir (iOS/Android sandbox) */

static void *read_at(const char *dir, const char *name, size_t *out_size)
{
    char path[1200];

    /* try exact name, then upper-case (retail files are upper-case) */
    for (int attempt = 0; attempt < 2; attempt++) {
        char fname[64];
        size_t n = strlen(name);
        if (n >= sizeof fname) return NULL;
        for (size_t i = 0; i <= n; i++) {
            char c = name[i];
            fname[i] = attempt ? (char)((c >= 'a' && c <= 'z') ? c - 32 : c) : c;
        }
        snprintf(path, sizeof path, "%s/%s", dir, fname);
        SDL_RWops *rw = SDL_RWFromFile(path, "rb");
        if (!rw) continue;
        Sint64 size = SDL_RWsize(rw);
        if (size <= 0) { SDL_RWclose(rw); return NULL; }
        void *buf = malloc((size_t)size);
        size_t got = SDL_RWread(rw, buf, 1, (size_t)size);
        SDL_RWclose(rw);
        if (got != (size_t)size) { free(buf); return NULL; }
        *out_size = (size_t)size;
        return buf;
    }
    return NULL;
}

static void *io_read_file(const char *name, size_t *out_size)
{
    /* sandboxed platforms: saved cfg lives in the pref dir, data in the
     * bundle; try the writable location first so saves win */
    if (pref_dir[0]) {
        void *buf = read_at(pref_dir, name, out_size);
        if (buf)
            return buf;
    }
    return read_at(data_dir, name, out_size);
}

static bool io_write_file(const char *name, const void *data, size_t size)
{
    const char *dirs[2] = { data_dir, pref_dir };
    for (int i = 0; i < 2; i++) {
        if (!dirs[i][0])
            continue;
        char path[1300];
        snprintf(path, sizeof path, "%s/%s", dirs[i], name);
        SDL_RWops *rw = SDL_RWFromFile(path, "wb");
        if (!rw)
            continue;
        size_t n = SDL_RWwrite(rw, data, 1, size);
        SDL_RWclose(rw);
        if (n == size)
            return true;
    }
    return false;
}

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    SDL_AudioDeviceID adev;
    sr_audio *audio;
    int cur_song;
    bool audio_enabled;        /* cached, follows cfg.sound_off */
    sr_game game;
    sr_input input;
    sr_input touch;           /* touch-derived input, OR-ed with keyboard */
    bool mouse_down;          /* desktop testing: mouse mirrors one finger */
    float mouse_x, mouse_y;
    uint64_t acc_num;          /* accumulated time in (counter ticks * TICK_NUM) */
    uint64_t last_counter;
    bool quit;
    bool show_touch;           /* draw on-screen touch controls */
} app_t;

#define SR_LOGW (SR_SCREEN_W * 6)
#define SR_LOGH (SR_SCREEN_H * 6 * 6 / 5)

static const struct { SDL_Scancode sc; int key; } keymap[] = {
    { SDL_SCANCODE_UP, SR_KEY_UP },       { SDL_SCANCODE_DOWN, SR_KEY_DOWN },
    { SDL_SCANCODE_LEFT, SR_KEY_LEFT },   { SDL_SCANCODE_RIGHT, SR_KEY_RIGHT },
    { SDL_SCANCODE_HOME, SR_KEY_HOME },   { SDL_SCANCODE_PAGEUP, SR_KEY_PGUP },
    { SDL_SCANCODE_END, SR_KEY_END },     { SDL_SCANCODE_PAGEDOWN, SR_KEY_PGDN },
    { SDL_SCANCODE_KP_7, SR_KEY_HOME },   { SDL_SCANCODE_KP_9, SR_KEY_PGUP },
    { SDL_SCANCODE_KP_1, SR_KEY_END },    { SDL_SCANCODE_KP_3, SR_KEY_PGDN },
    { SDL_SCANCODE_KP_8, SR_KEY_UP },     { SDL_SCANCODE_KP_2, SR_KEY_DOWN },
    { SDL_SCANCODE_KP_4, SR_KEY_LEFT },   { SDL_SCANCODE_KP_6, SR_KEY_RIGHT },
    { SDL_SCANCODE_ESCAPE, SR_KEY_ESC },  { SDL_SCANCODE_SPACE, SR_KEY_JUMP },
    { SDL_SCANCODE_P, SR_KEY_PAUSE },     { SDL_SCANCODE_RETURN, SR_KEY_ENTER },
};

/* Touch layout (documented in docs/PORTING.md):
 * D-PAD on the lower left — left/right steer, up = accelerate, down =
 * brake, diagonals combine (steer while accelerating). One big JUMP
 * button under the right thumb. The same controls drive the menus
 * (d-pad = arrows, button = Enter). Tiny top-left corner = ESC,
 * top-right corner = pause. */

typedef struct {
    float dx, dy, dr;          /* d-pad center + radius, in pixels */
    float jx, jy, jr;          /* jump button center + radius */
} touch_geom;

static touch_geom get_touch_geom(app_t *a)
{
    int w, h;
    SDL_GetWindowSize(a->win, &w, &h);
    touch_geom g;
    float m = (float)(w < h ? w : h);
    g.dr = m * 0.20f;
    g.dx = g.dr * 1.35f;
    g.dy = (float)h - g.dr * 1.35f;
    g.jr = m * 0.13f;
    g.jx = (float)w - g.jr * 1.7f;
    g.jy = (float)h - g.jr * 1.7f;
    return g;
}

/* Classify a finger position into key flags (writes into out[]). */
static void touch_classify(app_t *a, float nx, float ny, uint8_t *out)
{
    int w, h;
    SDL_GetWindowSize(a->win, &w, &h);
    float px = nx * (float)w, py = ny * (float)h;
    touch_geom g = get_touch_geom(a);

    /* corner taps */
    if (py < 0.16f * (float)h) {
        if (px < 0.12f * (float)w) { out[SR_KEY_ESC] = 1; return; }
        if (px > 0.88f * (float)w) { out[SR_KEY_PAUSE] = 1; return; }
    }
    /* jump button (generous halo) */
    float jdx = px - g.jx, jdy = py - g.jy;
    if (jdx * jdx + jdy * jdy <= (g.jr * 1.6f) * (g.jr * 1.6f)) {
        out[SR_KEY_JUMP] = 1;
        out[SR_KEY_ENTER] = 1;            /* doubles as select in menus */
        return;
    }
    /* d-pad: independent axes -> 8-way with diagonals */
    float ddx = px - g.dx, ddy = py - g.dy;
    if (ddx * ddx + ddy * ddy <= (g.dr * 1.8f) * (g.dr * 1.8f)) {
        float dead = g.dr * 0.28f;
        if (ddx < -dead) out[SR_KEY_LEFT] = 1;
        if (ddx >  dead) out[SR_KEY_RIGHT] = 1;
        if (ddy < -dead) out[SR_KEY_UP] = 1;
        if (ddy >  dead) out[SR_KEY_DOWN] = 1;
    }
}

/* Rebuild held-key state from SDL's authoritative finger list each frame
 * (event-based tracking can wedge when the OS also synthesizes mouse
 * events from touches). */
static void touch_update_held(app_t *a)
{
    memset(a->touch.held, 0, sizeof a->touch.held);
    int ndev = SDL_GetNumTouchDevices();
    for (int d = 0; d < ndev; d++) {
        SDL_TouchID tid = SDL_GetTouchDevice(d);
        int nf = SDL_GetNumTouchFingers(tid);
        for (int i = 0; i < nf; i++) {
            SDL_Finger *f = SDL_GetTouchFinger(tid, i);
            if (f)
                touch_classify(a, f->x, f->y, a->touch.held);
        }
    }
    if (a->mouse_down)
        touch_classify(a, a->mouse_x, a->mouse_y, a->touch.held);
}

static void pump_events(app_t *a)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            a->quit = true;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (e.key.repeat)
                break;
            for (size_t i = 0; i < sizeof keymap / sizeof *keymap; i++)
                if (e.key.keysym.scancode == keymap[i].sc) {
                    a->input.held[keymap[i].key] = (e.type == SDL_KEYDOWN);
                    if (e.type == SDL_KEYDOWN)
                        a->input.pressed[keymap[i].key] = 1;
                }
            break;
        case SDL_FINGERDOWN:
            /* edge-trigger: menu navigation, ESC/pause, demo-exit taps */
            touch_classify(a, e.tfinger.x, e.tfinger.y, a->touch.pressed);
            break;
        /* real mouse mirrors one finger for desktop testing (touch-derived
         * synthetic mouse events are disabled via SDL hint) */
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.which != SDL_TOUCH_MOUSEID) {
                int w, h;
                SDL_GetWindowSize(a->win, &w, &h);
                a->mouse_down = true;
                a->mouse_x = (float)e.button.x / (w ? w : 1);
                a->mouse_y = (float)e.button.y / (h ? h : 1);
                touch_classify(a, a->mouse_x, a->mouse_y, a->touch.pressed);
            }
            break;
        case SDL_MOUSEMOTION:
            if (a->mouse_down && e.motion.which != SDL_TOUCH_MOUSEID) {
                int w, h;
                SDL_GetWindowSize(a->win, &w, &h);
                a->mouse_x = (float)e.motion.x / (w ? w : 1);
                a->mouse_y = (float)e.motion.y / (h ? h : 1);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.which != SDL_TOUCH_MOUSEID)
                a->mouse_down = false;
            break;
        }
    }
    touch_update_held(a);
}

static void draw_touch_overlay(app_t *a);

static void present(app_t *a)
{
    uint32_t pal[256];
    sr_palette_rgba(a->game.out_pal, pal);

    void *pixels;
    int pitch;
    SDL_LockTexture(a->tex, NULL, &pixels, &pitch);
    for (int y = 0; y < SR_SCREEN_H; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
        const uint8_t *src = a->game.fb.px + y * SR_SCREEN_W;
        for (int x = 0; x < SR_SCREEN_W; x++)
            row[x] = pal[src[x]];
    }
    SDL_UnlockTexture(a->tex);
    SDL_RenderSetLogicalSize(a->ren, SR_LOGW, SR_LOGH);
    SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 255);
    SDL_RenderClear(a->ren);
    SDL_RenderCopy(a->ren, a->tex, NULL, NULL);
    draw_touch_overlay(a);            /* disables logical size internally */
    SDL_RenderPresent(a->ren);
}

/* ---- on-screen touch controls ---------------------------------------- */
/* Drawn in raw output-pixel space (logical scaling disabled) so the zones
 * line up 1:1 with the normalized finger zones in pump_events(). */

/* Filled chevron (direction: 0 up,1 down,2 left,3 right) centered at cx,cy. */
static void chevron(SDL_Renderer *r, int cx, int cy, int s, int dir, Uint8 a)
{
    SDL_Vertex v[3];
    SDL_Color c = { 255, 255, 255, a };
    SDL_FPoint p[3];
    switch (dir) {
    case 0: p[0]=(SDL_FPoint){cx,cy-s}; p[1]=(SDL_FPoint){cx-s,cy+s}; p[2]=(SDL_FPoint){cx+s,cy+s}; break;
    case 1: p[0]=(SDL_FPoint){cx,cy+s}; p[1]=(SDL_FPoint){cx-s,cy-s}; p[2]=(SDL_FPoint){cx+s,cy-s}; break;
    case 2: p[0]=(SDL_FPoint){cx-s,cy}; p[1]=(SDL_FPoint){cx+s,cy-s}; p[2]=(SDL_FPoint){cx+s,cy+s}; break;
    default:p[0]=(SDL_FPoint){cx+s,cy}; p[1]=(SDL_FPoint){cx-s,cy-s}; p[2]=(SDL_FPoint){cx-s,cy+s}; break;
    }
    for (int i = 0; i < 3; i++) { v[i].position = p[i]; v[i].color = c; v[i].tex_coord=(SDL_FPoint){0,0}; }
    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

/* Filled circle via horizontal spans. */
static void fill_circle(SDL_Renderer *r, float cx, float cy, float rad,
                        Uint8 rr, Uint8 gg, Uint8 bb, Uint8 aa)
{
    SDL_SetRenderDrawColor(r, rr, gg, bb, aa);
    int irad = (int)rad;
    for (int dy = -irad; dy <= irad; dy++) {
        float half = SDL_sqrtf(rad * rad - (float)dy * dy);
        SDL_RenderDrawLineF(r, cx - half, cy + dy, cx + half, cy + dy);
    }
}

static void draw_touch_overlay(app_t *a)
{
    if (!a->show_touch)
        return;
    /* the simulator/window may be scaled vs window coords; keep both in
     * window space */
    SDL_RenderSetLogicalSize(a->ren, 0, 0);
    int w, h, ow, oh;
    SDL_GetWindowSize(a->win, &w, &h);
    SDL_GetRendererOutputSize(a->ren, &ow, &oh);
    float sx = w ? (float)ow / w : 1, sy = h ? (float)oh / h : 1;
    SDL_RenderSetScale(a->ren, sx, sy);
    SDL_SetRenderDrawBlendMode(a->ren, SDL_BLENDMODE_BLEND);

    touch_geom g = get_touch_geom(a);
    const uint8_t *held = a->touch.held;
    float arm = g.dr * 0.62f;          /* half-width of the d-pad cross */

    /* d-pad cross: two rounded bars */
    SDL_FRect hbar = { g.dx - g.dr, g.dy - arm * 0.55f, g.dr * 2, arm * 1.1f };
    SDL_FRect vbar = { g.dx - arm * 0.55f, g.dy - g.dr, arm * 1.1f, g.dr * 2 };
    SDL_SetRenderDrawColor(a->ren, 30, 30, 40, 90);
    SDL_RenderFillRectF(a->ren, &hbar);
    SDL_RenderFillRectF(a->ren, &vbar);

    /* arm chevrons, brighter while held */
    float tip = g.dr * 0.68f, cs = g.dr * 0.22f;
    chevron(a->ren, (int)(g.dx - tip), (int)g.dy, (int)cs, 2,
            held[SR_KEY_LEFT] ? 255 : 140);
    chevron(a->ren, (int)(g.dx + tip), (int)g.dy, (int)cs, 3,
            held[SR_KEY_RIGHT] ? 255 : 140);
    chevron(a->ren, (int)g.dx, (int)(g.dy - tip), (int)cs, 0,
            held[SR_KEY_UP] ? 255 : 140);
    chevron(a->ren, (int)g.dx, (int)(g.dy + tip), (int)cs, 1,
            held[SR_KEY_DOWN] ? 255 : 140);

    /* jump button */
    int jheld = held[SR_KEY_JUMP];
    fill_circle(a->ren, g.jx, g.jy, g.jr, 255, 200, 50, jheld ? 150 : 70);
    fill_circle(a->ren, g.jx, g.jy, g.jr * 0.86f, 255, 220, 90,
                jheld ? 120 : 50);
    chevron(a->ren, (int)g.jx, (int)g.jy, (int)(g.jr * 0.45f), 0,
            jheld ? 255 : 180);

    SDL_RenderSetScale(a->ren, 1.0f, 1.0f);
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    app_t *a = ud;
    sr_audio_render(a->audio, (int16_t *)stream, len / 4);
}

static void apply_audio(app_t *a)
{
    if (!a->adev)
        return;
    /* honor the Settings "Sound Off" toggle — the menu writes cfg.sound_off but
     * nothing was muting the OPL output; sync it here on change. */
    bool want_enabled = !a->game.cfg.sound_off;
    if (want_enabled != a->audio_enabled) {
        SDL_LockAudioDevice(a->adev);
        sr_audio_set_enabled(a->audio, want_enabled);
        a->audio_enabled = want_enabled;
        if (want_enabled)
            a->cur_song = -1;      /* force the current track to restart */
        SDL_UnlockAudioDevice(a->adev);
    }
    if (a->game.want_song != a->cur_song || a->game.sfx_request ||
        a->game.want_intro_snd) {
        SDL_LockAudioDevice(a->adev);
        if (a->game.want_song != a->cur_song) {
            sr_audio_music(a->audio, &a->game.assets, a->game.want_song);
            a->cur_song = a->game.want_song;
        }
        if (a->game.sfx_request) {
            sr_audio_sfx(a->audio, &a->game.assets, a->game.sfx_request - 1);
            a->game.sfx_request = 0;
        }
        if (a->game.want_intro_snd) {
            sr_audio_pcm(a->audio, a->game.assets.intro_snd,
                         a->game.assets.intro_snd_size, 0x5a);
            a->game.want_intro_snd = 0;
        }
        SDL_UnlockAudioDevice(a->adev);
    }
}

static void main_loop(void *ud)
{
    app_t *a = ud;
    pump_events(a);

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t dt = now - a->last_counter;
    a->last_counter = now;
    if (dt > freq) dt = freq;                 /* clamp pauses to 1s */
    a->acc_num += dt * SR_TICK_NUM;

    uint64_t per_tick = freq * (uint64_t)SR_TICK_DEN;
    int guard = 0;
    while (a->acc_num >= per_tick && guard++ < 8) {
        a->acc_num -= per_tick;
        /* compose keyboard + touch into a transient input; never write
         * the OR back into the persistent keyboard state (that latches) */
        sr_input eff;
        for (int k = 0; k < SR_KEY_COUNT; k++) {
            eff.held[k] = (uint8_t)(a->input.held[k] || a->touch.held[k]);
            eff.pressed[k] = (uint8_t)(a->input.pressed[k] ||
                                       a->touch.pressed[k]);
        }
        sr_game_tick(&a->game, &eff);
        memset(a->input.pressed, 0, sizeof a->input.pressed);
        memset(a->touch.pressed, 0, sizeof a->touch.pressed);
        apply_audio(a);
    }
    present(a);

    if (!sr_game_running(&a->game))
        a->quit = true;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        snprintf(data_dir, sizeof data_dir, "%s", argv[1]);
#ifdef __EMSCRIPTEN__
    snprintf(data_dir, sizeof data_dir, "/data");
#endif

    /* SkyRoads is a landscape game (320x200); lock orientation so phones
     * present it the right way up. Must be set before video init. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    /* touches are handled natively — don't synthesize mouse events from
     * them (they corrupt held-state tracking) */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* Sandboxed platforms (iOS/Android): data files ship in the app bundle
     * (base path), saves go to the per-app pref dir. Desktop macOS keeps
     * the argv data dir, so gate on the iOS target macro, not __APPLE__. */
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
    {
        char *base = SDL_GetBasePath();
        if (base) {
            snprintf(data_dir, sizeof data_dir, "%s", base);
            SDL_free(base);
        }
        char *pref = SDL_GetPrefPath("skyroads", "skyroads");
        if (pref) {
            snprintf(pref_dir, sizeof pref_dir, "%s", pref);
            SDL_free(pref);
        }
    }
#endif
    /* heap-allocate: app_t embeds the framebuffer and asset tables, far
     * larger than small platform stacks (Emscripten: 64KB) */
    app_t *a = calloc(1, sizeof *a);
#if (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__ANDROID__)
    a->show_touch = true;                /* always on for touch devices */
    Uint32 win_flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI;
    int win_w = SR_SCREEN_W * 3, win_h = SR_SCREEN_H * 3 * 6 / 5;
#else
    a->show_touch = SDL_GetHintBoolean("SR_TOUCH_UI", SDL_FALSE);
    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    int win_w = SR_SCREEN_W * 3, win_h = SR_SCREEN_H * 3 * 6 / 5;
#endif
    a->win = SDL_CreateWindow("SkyRoads",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              win_w, win_h, win_flags);
    a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(a->ren, SR_SCREEN_W * 6, SR_SCREEN_H * 6 * 6 / 5);
    a->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_ARGB8888,
                               SDL_TEXTUREACCESS_STREAMING,
                               SR_SCREEN_W, SR_SCREEN_H);

    char err[256] = "";
    if (!sr_game_init(&a->game, (sr_io){ io_read_file, io_write_file }, err, sizeof err)) {
        fprintf(stderr,
            "SkyRoads data files not found (first missing: %s).\n\n"
            "This engine needs the original game's data files (*.LZS, *.DAT,\n"
            "*.SND, DEMO.REC). SkyRoads was released as FREEWARE by Bluemoon\n"
            "Interactive - download it (e.g. from bluemoon.ee) and either run\n"
            "this program from that folder or pass the folder as the first\n"
            "argument:  skyroads /path/to/skyroads\n", err);
        return 1;
    }
    a->audio = sr_audio_create();
    a->cur_song = -1;
    a->audio_enabled = true;       /* matches sr_audio_create default */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = SR_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    want.userdata = a;
    a->adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (a->adev)
        SDL_PauseAudioDevice(a->adev, 0);
    a->last_counter = SDL_GetPerformanceCounter();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(main_loop, a, 0, 1);
#else
    while (!a->quit)
        main_loop(a);
#endif
    SDL_Quit();
    return 0;
}
