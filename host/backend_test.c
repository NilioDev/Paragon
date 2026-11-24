/* Linux test host: runs the recompiled game headless with a fake Java side and captures frames.
 * Usage: zen_test ASSETS_DIR SAVE_DIR FRAMES OUT_DIR [EVERY] [INPUT_SCRIPT] */
#include "../runtime/rt.h"
#include "../runtime/rt_jni.h"
#include "../runtime/gles1.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

extern const unsigned char zen_image[];
extern const unsigned int zen_image_size;

static const char *assets, *savedir;
static uint64_t now_us = 1700000000ull * 1000000ull;
static int verbose;

void backend_log(const char *m) { fprintf(stderr, "%s\n", m); }
uint64_t backend_time_us(void) { now_us += 50; return now_us; }

/* ---------------- fake Java ---------------- */
enum { O_CLASS = 1, O_STRING, O_BYTES, O_METHOD };
struct fobj { int type; char *s; uint8_t *data; int len; };
static struct fobj *mk(int type, const char *s) { struct fobj *o = calloc(1, sizeof *o); o->type = type; o->s = s ? strdup(s) : NULL; return o; }

void *jb_find_class(const char *name) { return mk(O_CLASS, name); }
void *jb_new_global(void *o) { return o; }
void jb_delete_ref(void *o, int global) { (void)o; (void)global; }
void *jb_get_static_method(void *cls, const char *name, const char *sig) { (void)cls; (void)sig; return mk(O_METHOD, name); }
void *jb_new_string(const char *utf) { return mk(O_STRING, utf); }
char *jb_get_string(void *s) { return strdup(s ? ((struct fobj *)s)->s : ""); }
int jb_array_length(void *a) { return ((struct fobj *)a)->len; }
void jb_array_read(void *a, int off, int len, void *dst) { memcpy(dst, ((struct fobj *)a)->data + off, (size_t)len); }
void *jb_new_byte_array(const void *data, int len) {
    struct fobj *o = mk(O_BYTES, NULL); o->data = malloc((size_t)len + 1); memcpy(o->data, data, (size_t)len); o->len = len; return o;
}

static long fsize(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1; }

void jb_call_static(void *cls, void *mid, const char *sig, const jb_value *a, int n, jb_value *ret) {
    (void)cls; (void)n;
    const char *m = ((struct fobj *)mid)->s;
    char path[1024];
    if (!strcmp(m, "isAssetExist")) {
        snprintf(path, sizeof path, "%s/%s", assets, ((struct fobj *)a[0].l)->s);
        long s = fsize(path);
        ret->i = s < 0 ? 0 : (int)s;
    } else if (!strcmp(m, "readAssete")) {
        snprintf(path, sizeof path, "%s/%s", assets, ((struct fobj *)a[0].l)->s);
        FILE *f = fopen(path, "rb");
        if (!f) { ret->l = NULL; return; }
        struct fobj *o = mk(O_BYTES, NULL);
        fseek(f, 0, SEEK_END); o->len = (int)ftell(f); fseek(f, 0, SEEK_SET);
        o->data = malloc((size_t)o->len + 1);
        if (fread(o->data, 1, (size_t)o->len, f) != (size_t)o->len) o->len = 0;
        fclose(f);
        ret->l = o;
    } else if (!strcmp(m, "getAbsolueFilePath")) {
        ret->l = mk(O_STRING, savedir);
    } else {
        if (verbose) fprintf(stderr, "  java %s%s %d %d %d @%llu\n", m, sig, n > 0 ? a[0].i : 0, n > 1 ? a[1].i : 0, n > 2 ? a[2].i : 0, (unsigned long long)(now_us / 1000));
        memset(ret, 0, sizeof *ret);
    }
}

/* ---------------- fake GL: keep the last uploaded framebuffer ---------------- */
static uint16_t fb[512 * 512];
static int fbw, fbh;
void glActiveTexture(uint32_t t) { (void)t; }
void glBindTexture(uint32_t a, uint32_t b) { (void)a; (void)b; }
void glClear(uint32_t m) { (void)m; }
void glClearColorx(int32_t r, int32_t g, int32_t b, int32_t a) { (void)r; (void)g; (void)b; (void)a; }
void glColorPointer(int32_t s, uint32_t t, int32_t st, const void *p) { (void)s; (void)t; (void)st; (void)p; }
void glDisable(uint32_t c) { (void)c; }
void glDisableClientState(uint32_t c) { (void)c; }
void glDrawArrays(uint32_t m, int32_t f, int32_t c) { (void)m; (void)f; (void)c; }
void glEnable(uint32_t c) { (void)c; }
void glEnableClientState(uint32_t c) { (void)c; }
void glFrustumx(int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f) { (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
void glGenTextures(int32_t n, uint32_t *t) { for (int i = 0; i < n; i++) t[i] = (uint32_t)i + 1; }
void glHint(uint32_t a, uint32_t b) { (void)a; (void)b; }
void glLoadIdentity(void) { }
void glMatrixMode(uint32_t m) { (void)m; }
void glMultMatrixx(const int32_t *m) { (void)m; }
void glNormalPointer(uint32_t t, int32_t s, const void *p) { (void)t; (void)s; (void)p; }
void glTexCoordPointer(int32_t s, uint32_t t, int32_t st, const void *p) { (void)s; (void)t; (void)st; (void)p; }
void glTexEnvf(uint32_t a, uint32_t b, float c) { (void)a; (void)b; (void)c; }
void glTexImage2D(uint32_t t, int32_t l, int32_t i, int32_t w, int32_t h, int32_t b, uint32_t f, uint32_t ty, const void *p) {
    (void)t; (void)l; (void)i; (void)b; (void)f; (void)ty; (void)p; (void)w; (void)h;
}
void glTexParameterx(uint32_t a, uint32_t b, int32_t c) { (void)a; (void)b; (void)c; }
void glTexSubImage2D(uint32_t t, int32_t l, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t f, uint32_t ty, const void *p) {
    (void)t; (void)l; (void)f;
    if (ty != 0x8363 || !p || w > 512 || h != 240) return;
    for (int r = 0; r < h; r++) memcpy(fb + (y + r) * 512 + x, (const uint16_t *)p + r * w, (size_t)w * 2);
    if (x + w > fbw) fbw = x + w;
    if (y + h > fbh) fbh = y + h;
}
void glTranslatex(int32_t x, int32_t y, int32_t z) { (void)x; (void)y; (void)z; }
void glVertexPointer(int32_t s, uint32_t t, int32_t st, const void *p) { (void)s; (void)t; (void)st; (void)p; }
void glViewport(int32_t x, int32_t y, int32_t w, int32_t h) { (void)x; (void)y; (void)w; (void)h; }
void glOrthof(float l, float r, float b, float t, float n, float f) { (void)l; (void)r; (void)b; (void)t; (void)n; (void)f; }
void glColor4f(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }
void glBlendFunc(uint32_t s, uint32_t d) { (void)s; (void)d; }
void glPixelStorei(uint32_t p, int32_t v) { (void)p; (void)v; }
void glClearColor(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; }

static void dump(const char *dir, int frame) {
    char p[1024];
    snprintf(p, sizeof p, "%s/frame%05d.ppm", dir, frame);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", fbw, fbh);
    for (int y = 0; y < fbh; y++)
        for (int x = 0; x < fbw; x++) {
            uint16_t v = fb[y * 512 + x];
            unsigned char px[3] = { (unsigned char)(((v >> 11) & 31) * 255 / 31), (unsigned char)(((v >> 5) & 63) * 255 / 63),
                                    (unsigned char)((v & 31) * 255 / 31) };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

#define CLS 0   /* jclass argument: the game never uses it */
static void call(const char *name, int n, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t args[5] = { GUEST_ENV, CLS, a, b, c };
    if (verbose && !strstr(name, "NativeRender")) fprintf(stderr, "> %s(%d,%d,%d)\n", name + 32, (int)a, (int)b, (int)c);
    rt_export_call(name, n, args);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s ASSETS SAVEDIR FRAMES OUTDIR [EVERY] [INPUT]\n", argv[0]); return 2; }
    assets = argv[1]; savedir = argv[2];
    int frames = atoi(argv[3]), every = argc > 5 ? atoi(argv[5]) : 30;
    verbose = getenv("VERBOSE") != NULL;
    extern int paragon_screen_w;
    if (getenv("SCREENW")) paragon_screen_w = atoi(getenv("SCREENW"));
    if (rt_init(zen_image, zen_image_size)) { fprintf(stderr, "rt_init failed\n"); return 1; }
    uint32_t vm[2] = { GUEST_ENV + 0x10, 0 };
    rt_export_call("JNI_OnLoad", 2, vm);
    call("Java_com_gamevil_nexus2_Natives_InitializeJNIGlobalRef", 2, 0, 0, 0);
    call("Java_com_gamevil_nexus2_Natives_NativeInit", 2, 0, 0, 0);
    call("Java_com_gamevil_nexus2_Natives_NativeResize", 4, 800, 480, 0);
    FILE *in = argc > 6 ? fopen(argv[6], "r") : NULL;
    int next_ev = -1, ev_a = 0, ev_b = 0, ev_c = 0;
    char kind[32] = { 0 };
    if (in && fscanf(in, "%d %31s %d %d %d", &next_ev, kind, &ev_a, &ev_b, &ev_c) != 5) next_ev = -1;
    for (int f = 0; f < frames; f++) {
        while (next_ev == f) {
            if (!strcmp(kind, "input")) call("Java_com_gamevil_nexus2_Natives_setInputEvent", 5, (uint32_t)ev_a, (uint32_t)ev_b, (uint32_t)ev_c);
            else if (!strcmp(kind, "clet")) call("Java_com_gamevil_nexus2_Natives_handleCletEvent", 5, (uint32_t)ev_a, (uint32_t)ev_b, (uint32_t)ev_c);
            else if (!strcmp(kind, "pause")) call("Java_com_gamevil_nexus2_Natives_NativePauseClet", 2, 0, 0, 0);
            else if (!strcmp(kind, "resume")) call("Java_com_gamevil_nexus2_Natives_NativeResumeClet", 2, 0, 0, 0);
            else if (!strcmp(kind, "surface")) {   /* GL context lost: onSurfaceCreated + onSurfaceChanged */
                call("Java_com_gamevil_nexus2_Natives_NativeInit", 2, 0, 0, 0);
                call("Java_com_gamevil_nexus2_Natives_NativeResize", 4, (uint32_t)ev_a, (uint32_t)ev_b, 0);
            }
            if (fscanf(in, "%d %31s %d %d %d", &next_ev, kind, &ev_a, &ev_b, &ev_c) != 5) next_ev = -1;
        }
        call("Java_com_gamevil_nexus2_Natives_NativeRender", 2, 0, 0, 0);
        now_us += 33333;
        if (f % every == 0) dump(argv[4], f);
    }
    fprintf(stderr, "ran %d frames, framebuffer %dx%d\n", frames, fbw, fbh);
    return 0;
}
