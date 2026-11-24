/* Frame presentation and on-screen diagnostics.
 * Replaces the game's glDrawFrame: every frame sets the complete GL state it needs (the Java UI layer
 * draws with the same context afterwards), uploads the game framebuffer to our own texture and draws it
 * aspect-correct. If something went wrong, the last log lines are drawn on top so a screenshot is enough
 * to diagnose it. */
#include "rt.h"
#include "gles1.h"
#include "font6x10.h"
#include <stdio.h>

#define GL_TEXTURE_2D 0x0DE1
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_ALPHA_TEST 0x0BC0
#define GL_LIGHTING 0x0B50
#define GL_FOG 0x0B60
#define GL_CULL_FACE 0x0B44
#define GL_DITHER 0x0BD0
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_TEXTURE 0x1702
#define GL_FLOAT 0x1406
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_MODULATE 0x2100
#define GL_REPLACE 0x1E01
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_TEXTURE0 0x84C0

extern int paragon_screen_w;
int paragon_view_w = 800, paragon_view_h = 480;   /* surface size, from the game's glViewport */
int paragon_smooth = 1;                            /* linear filtering when scaling */
int paragon_keep_aspect = 0;                       /* letterbox instead of filling the surface */

/* ---------------- log ring for the overlay ---------------- */
#define LOG_LINES 12
static char log_ring[LOG_LINES][96];
static int log_next, log_alert;
static int dead;

void rt_diag_line(const char *msg, int alert) {
    snprintf(log_ring[log_next % LOG_LINES], sizeof log_ring[0], "%s", msg);
    log_next++;
    if (alert) log_alert = 1;
}
void rt_diag_dead(void) { dead = 1; log_alert = 1; }
int rt_is_dead(void) { return dead; }

/* ---------------- textures ---------------- */
static uint32_t game_tex, text_tex;
static int tex_ready;
static uint16_t text_px[512 * 128];

static void make_tex(uint32_t *t, int w, int h, uint32_t fmt, uint32_t type) {
    glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, (int32_t)fmt, w, h, 0, fmt, type, 0);
}

void rt_present_reset(void) { tex_ready = 0; }   /* GL context was recreated */

static void draw_quad(float x0, float y0, float x1, float y1, float u1, float v1) {
    float v[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    float t[8] = { 0, 0, u1, 0, 0, v1, u1, v1 };
    glVertexPointer(2, GL_FLOAT, 0, v);
    glTexCoordPointer(2, GL_FLOAT, 0, t);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void setup_state(void) {
    if (!tex_ready) {
        make_tex(&game_tex, 1024, 256, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);   /* up to 1024x256 (widescreen) */
        make_tex(&text_tex, 512, 128, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
        tex_ready = 1;
    }
    glViewport(0, 0, paragon_view_w, paragon_view_h);
    /* the Java UI layer draws its buttons afterwards with the projection the game set up in glResize,
     * so save the matrices and put them back when done */
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, (float)paragon_view_w, (float)paragon_view_h, 0, -1, 1);
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (float)GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void draw_text_overlay(void) {
    if (!log_alert) return;
    memset(text_px, 0, sizeof text_px);
    int lines = log_next < LOG_LINES ? log_next : LOG_LINES;
    for (int l = 0; l < lines; l++) {
        const char *s = log_ring[(log_next - lines + l) % LOG_LINES];
        for (int c = 0; s[c] && c < 85; c++) {
            unsigned ch = (unsigned char)s[c];
            if (ch < 32 || ch > 126) ch = '?';
            for (int y = 0; y < 10; y++) {
                unsigned bits = font6x10[ch - 32][y];
                for (int x = 0; x < 6; x++)
                    if (bits & (0x80u >> x)) text_px[(l * 10 + y) * 512 + c * 6 + x] = 0xFFE0;  /* yellow */
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, text_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 128, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, text_px);
    float scale = paragon_view_h / 300.0f;
    if (scale < 1) scale = 1;
    draw_quad(8, 8, 8 + 512 * scale, 8 + 128 * scale, 1.0f, 1.0f);
}

/* Draw the game framebuffer (guest address) scaled to fit the surface, then the diagnostics. */
void rt_present(uint32_t pixels, int w, int h) {
    setup_state();
    if (pixels && w > 0 && h > 0 && w <= 1024 && h <= 256) {
        glBindTexture(GL_TEXTURE_2D, game_tex);
        uint32_t f = paragon_smooth ? GL_LINEAR : GL_NEAREST;
        glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (int32_t)f);
        glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (int32_t)f);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, HP(pixels));
        /* The Java touch layer maps the whole surface onto the game screen, so the image must fill it
         * (as the original did); letterboxing would put the touch buttons off from what is drawn. */
        float qw = (float)paragon_view_w, qh = (float)paragon_view_h, x0 = 0, y0 = 0;
        if (paragon_keep_aspect) {
            float sx = (float)paragon_view_w / (float)w, sy = (float)paragon_view_h / (float)h;
            float s = sx < sy ? sx : sy;
            qw = w * s; qh = h * s;
            x0 = (paragon_view_w - qw) / 2; y0 = (paragon_view_h - qh) / 2;
        }
        draw_quad(x0, y0, x0 + qw, y0 + qh, (float)w / 1024.0f, (float)h / 256.0f);
    }
    draw_text_overlay();
    /* leave state the way the Java UI layer expects it */
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

/* ---------------- override of the guest's glDrawFrame ---------------- */
#define GUEST_getDeviceInfo 0x1003c575u
void HglDrawFrame(void) {
    uint32_t save_lr = R[14];
    rt_call(GUEST_getDeviceInfo);
    uint32_t info = R[0];
    uint32_t pixels = info ? RD32(info + 4) : 0;
    rt_present(pixels, paragon_screen_w, 240);
    R[14] = save_lr;
}

/* the game sets the viewport to the surface size in glResize */
void rt_note_viewport(int w, int h) { if (w > 0 && h > 0) { paragon_view_w = w; paragon_view_h = h; } }
