/* Widescreen: the world is drawn into a framebuffer wider than 320 pixels (paragon_screen_w), but most
 * of the game's UI was laid out for exactly 320x240.  While a UI pass runs we hand the game a 320-wide
 * buffer holding the middle of the screen, then copy it back, so the UI lands centred over the wide
 * world without touching the game's layout code. */
#include "rt.h"
#include "funcs.h"   /* generated */

#define G_SCREEN_W           0x100bb908u
#define GUEST_MainScreenBuf  0x10040725u   /* GcxGetMainScreenBuffer() */
#define GUEST_GsGraphics     0x100d4a80u   /* CGsSingleton<CGsGraphics>::ms_pSingleton */
#define GUEST_MvMap          0x100d4b80u   /* CGsSingleton<CMvMap>::ms_pSingleton; +0xb4 = width in tiles */
#define UI_W 320
#define UI_H 240

int paragon_center_maps = 1;
static uint32_t uibuf;
static int depth;
static uint32_t s_w, s_vw, s_fb, s_pix, s_g, s_gfx, s_clip_g, s_clip_gfx;

static void copy_rows(uint32_t dst, int dstride, uint32_t src, int sstride, int w) {
    for (int y = 0; y < UI_H; y++) memcpy(HP(dst + (uint32_t)(y * dstride)), HP(src + (uint32_t)(y * sstride)), (size_t)w * 2);
}

/* Start a pass that sees a centred, vw-pixel-wide screen. */
static void pass_begin(uint32_t vw) {
    if (depth++) return;
    s_w = RD32(G_SCREEN_W);
    s_vw = vw;
    if (s_w <= vw || s_w > 1024) { s_w = 0; return; }
    uint32_t r[4] = { R[0], R[1], R[2], R[3] }, lr = R[14];
    rt_call(GUEST_MainScreenBuf);
    s_g = R[0];
    R[0] = r[0]; R[1] = r[1]; R[2] = r[2]; R[3] = r[3]; R[14] = lr;
    s_fb = RD32(s_g);
    if (!s_fb || RD32(s_fb) != s_w) { s_w = 0; return; }
    if (!uibuf) uibuf = g_malloc(1024 * UI_H * 2);
    s_pix = RD32(s_fb + 0x10);
    uint32_t xo = (s_w - vw) / 2 * 2;
    copy_rows(uibuf, (int)vw * 2, s_pix + xo, (int)s_w * 2, (int)vw);
    WR32(s_fb, vw);
    WR32(s_fb + 0x10, uibuf);
    WR32(G_SCREEN_W, vw);
    s_clip_g = RD16(s_g + 0x4c);
    if (s_clip_g > vw) WR16(s_g + 0x4c, vw);
    s_gfx = RD32(GUEST_GsGraphics);
    if (s_gfx) {
        s_clip_gfx = RD16(s_gfx + 0x44);
        if (s_clip_gfx > vw) WR16(s_gfx + 0x44, vw);
    }
}

static void pass_end(int clear_sides) {
    if (--depth || !s_w) return;
    uint32_t xo = (s_w - s_vw) / 2 * 2;
    copy_rows(s_pix + xo, (int)s_w * 2, uibuf, (int)s_vw * 2, (int)s_vw);
    if (clear_sides)
        for (int y = 0; y < UI_H; y++) {
            memset(HP(s_pix + (uint32_t)y * s_w * 2), 0, xo);
            memset(HP(s_pix + (uint32_t)y * s_w * 2 + xo + s_vw * 2), 0, (s_w - s_vw) * 2 - xo);
        }
    WR32(s_fb, s_w);
    WR32(s_fb + 0x10, s_pix);
    WR32(G_SCREEN_W, s_w);
    WR16(s_g + 0x4c, s_clip_g);
    if (s_gfx) WR16(s_gfx + 0x44, s_clip_gfx);
}

/* run the original guest function f inside a UI pass */
static void run_pass(rt_fn f, uint32_t vw, int clear_sides) {
    uint32_t lr = R[14];
    pass_begin(vw);
    f();
    uint32_t r0 = R[0];
    pass_end(clear_sides);
    R[0] = r0;
    R[14] = lr;
}
static void ui_pass(rt_fn f) { run_pass(f, UI_W, 0); }

/* Maps narrower than the screen (castle rooms, houses) were never possible at 320 pixels; the game
 * would pin them to the left edge.  Draw them in a pass exactly as wide as the map, centred. */
void H_ZN12CMvGameState7DrawMapEv(void) {
    uint32_t map = RD32(GUEST_MvMap);
    uint32_t mw = map ? RD32(map + 0xb4) * 16 : 0;
    if (paragon_center_maps && mw >= 16 && mw < RD32(G_SCREEN_W)) run_pass(F_098a4c, mw, 1);
    else F_098a4c();
}

void H_ZN12CMvGameState10DrawGameUIEv(void) { ui_pass(F_098a10); }
void H_ZN12CMvGameState15DrawGameUIIntroEv(void) { ui_pass(F_0989d8); }
void H_ZN13CMvTitleState4DrawEv(void) { ui_pass(F_08b800); }
void H_ZN12CMvMenuState4DrawEv(void) { ui_pass(F_084664); }
void H_ZN7CGsLogo4DrawEv(void) { ui_pass(F_0671e8); }
