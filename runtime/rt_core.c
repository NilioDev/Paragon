/* Paragon runtime core: memory, dispatch, guest heap, libgcc helpers. */
#include "rt.h"
#include "rt_jni.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/mman.h>

uint32_t R[16];
uint32_t FN, FZ, FC, FV;
uint32_t PEND;
uint8_t *G;
static int depth;

void rt_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    backend_log(buf);
    rt_diag_line(buf, 0);
}

static jmp_buf *fatal_jmp;

void rt_fatal(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    backend_log(buf);
    char regs[256];
    snprintf(regs, sizeof regs, "regs r0=%x r1=%x r2=%x r3=%x r4=%x sp=%x lr=%x", R[0], R[1], R[2], R[3], R[4], R[13], R[14]);
    backend_log(regs);
    rt_diag_line(buf, 1);
    rt_diag_line(regs, 1);
    rt_diag_dead();
    if (fatal_jmp) longjmp(*fatal_jmp, 1);
    abort();
}

void rt_unimpl(uint32_t pc, const char *what) { rt_fatal("unimplemented instruction at %08x: %s", pc, what); }

static void map_rw(uint32_t lo, uint32_t hi) {
    if (mprotect(G + lo, hi - lo, PROT_READ | PROT_WRITE) != 0) rt_fatal("mprotect %x-%x failed", lo, hi);
}

/* ---------------- guest heap: segregated power-of-two free lists ---------------- */
static uint32_t heap_top = GUEST_HEAP_LO;
static uint32_t free_lists[32];
#define HDR 8u   /* [size class][magic] header keeps 8-byte alignment */

static int size_class(uint32_t n) {
    int c = 4;
    while ((1u << c) < n + HDR) c++;
    return c;
}

uint32_t g_malloc(uint32_t n) {
    int c = size_class(n ? n : 1);
    if (c > 30) return 0;
    uint32_t blk = free_lists[c];
    if (blk) {
        free_lists[c] = RD32(blk + HDR);
    } else {
        uint32_t sz = 1u << c;
        if (heap_top + sz > GUEST_HEAP_HI) return 0;
        blk = heap_top;
        heap_top += sz;
    }
    WR32(blk, (uint32_t)c);
    WR32(blk + 4, 0x6865617Fu);
    return blk + HDR;
}

void g_free(uint32_t a) {
    if (!a) return;
    uint32_t blk = a - HDR;
    if (RD32(blk + 4) != 0x6865617Fu) { rt_log("g_free: bad pointer %08x", a); return; }
    uint32_t c = RD32(blk);
    WR32(blk + 4, 0);
    WR32(a, free_lists[c]);
    free_lists[c] = blk;
}

uint32_t g_strdup(const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = g_malloc(n);
    memcpy(HP(a), s, n);
    return a;
}

/* ---------------- init ---------------- */
int rt_init(const void *image, uint32_t image_size) {
    if (G) return 0;
    void *p = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    G = (uint8_t *)p;
    map_rw(rt_image_base, rt_image_base + ((rt_image_size + 0xfff) & ~0xfffu));
    memcpy(G + rt_image_base, image, image_size < rt_image_size ? image_size : rt_image_size);
    map_rw(GUEST_DATAIMP, GUEST_DATAIMP + 0x1000);
    WR32(GUEST_DATAIMP, 0x5A3C9E11u);  /* __stack_chk_guard */
    map_rw(GUEST_ENV, GUEST_ENV + 0x10000);
    map_rw(GUEST_HEAP_LO, GUEST_HEAP_HI);
    map_rw(GUEST_STACK_LO, GUEST_STACK_HI);
    /* JNIEnv: *env -> function table; JavaVM likewise */
    WR32(GUEST_ENV + 0x0, GUEST_ENV + 0x100);
    for (uint32_t i = 0; i < 240; i++) WR32(GUEST_ENV + 0x100 + 4 * i, GUEST_JNI_FN + 4 * i);
    WR32(GUEST_ENV + 0x10, GUEST_ENV + 0x800);
    for (uint32_t i = 0; i < 8; i++) WR32(GUEST_ENV + 0x800 + 4 * i, GUEST_VM_FN + 4 * i);
    return 0;
}

/* ---------------- dispatch ---------------- */
void rt_call(uint32_t t) {
    if (t - GUEST_IMP_BASE < 4 * rt_import_count) { rt_imports[(t - GUEST_IMP_BASE) / 4](); return; }
    if (t - GUEST_JNI_FN < 4 * 240) { backend_jni((t - GUEST_JNI_FN) / 4); return; }
    if (t - GUEST_VM_FN < 4 * 8) { backend_vm((t - GUEST_VM_FN) / 4); return; }
    rt_fn f = rt_lookup(t & ~1u);
    if (!f) rt_fatal("call to unknown address %08x (lr=%08x)", t, R[14]);
    f();
}

void rt_jump(uint32_t t) {
    if (t - GUEST_IMP_BASE < 4 * rt_import_count || t - GUEST_JNI_FN < 4 * 240 || t - GUEST_VM_FN < 4 * 8) {
        /* tail call into a host routine: it "returns" to LR, which a caller up the chain resolves */
        rt_call(t);
        if (!PEND) PEND = R[14];
        return;
    }
    rt_fn f = rt_lookup(t & ~1u);
    if (f) { f(); return; }
    PEND = t;                /* let a caller up the chain resolve it */
}

uint32_t rt_enter(rt_fn fn, int nargs, const uint32_t *args) {
    if (rt_is_dead()) return 0;
    uint32_t save_sp = R[13], save_lr = R[14];
    jmp_buf jb, *outer = fatal_jmp;
    if (depth == 0) {
        R[13] = GUEST_STACK_HI - 64;
        if (setjmp(jb)) {       /* rt_fatal: the game is stopped, the overlay shows why */
            fatal_jmp = outer;
            depth = 0;
            PEND = 0;
            return 0;
        }
        fatal_jmp = &jb;
    }
    depth++;
    for (int i = 0; i < nargs && i < 4; i++) R[i] = args[i];
    if (nargs > 4) {
        uint32_t extra = (uint32_t)(nargs - 4);
        R[13] -= (extra * 4 + 7) & ~7u;
        for (uint32_t i = 0; i < extra; i++) WR32(R[13] + 4 * i, args[4 + i]);
    }
    R[14] = GUEST_MAGIC_RET;
    PEND = 0;
    fn();
    if (PEND && PEND != GUEST_MAGIC_RET) rt_fatal("unresolved jump to %08x", PEND);
    PEND = 0;
    depth--;
    if (depth == 0) { rt_jni_end_entry(); fatal_jmp = outer; }
    uint32_t r0 = R[0];
    R[13] = save_sp;
    R[14] = save_lr;
    return r0;
}

uint32_t rt_export_call(const char *name, int nargs, const uint32_t *args) {
    for (const struct rt_export *e = rt_exports; e->name; e++)
        if (!strcmp(e->name, name)) return rt_enter(e->fn, nargs, args);
    rt_fatal("no export %s", name);
}

uint32_t rt_arg(int i) { return i < 4 ? R[i] : RD32(R[13] + 4 * (uint32_t)(i - 4)); }

/* ---------------- printf-style formatting over guest varargs ---------------- */
int rt_vformat(char *out, size_t cap, const char *fmt, uint32_t (*next)(void *), void *ctx) {
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < cap) out[o] = (ch); o++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        char spec[32];
        int sl = 0;
        spec[sl++] = '%';
        p++;
        while (*p && strchr("-+ #0", *p) && sl < 20) spec[sl++] = *p++;
        if (*p == '*') { sl += snprintf(spec + sl, 12, "%d", (int)next(ctx)); p++; }
        while (*p >= '0' && *p <= '9' && sl < 24) spec[sl++] = *p++;
        if (*p == '.') { spec[sl++] = *p++; while (*p >= '0' && *p <= '9' && sl < 28) spec[sl++] = *p++; }
        int longlong = 0;
        while (*p == 'l' || *p == 'h') { if (p[0] == 'l' && p[1] == 'l') { longlong = 1; p++; } p++; }
        char c = *p, tmp[512];
        if (!c) break;
        spec[sl++] = c;
        spec[sl] = 0;
        switch (c) {
        case '%': PUT('%'); continue;
        case 'd': case 'i':
            if (longlong) { uint32_t lo = next(ctx), hi = next(ctx); spec[sl - 1] = 0; strcat(spec, "lld");
                snprintf(tmp, sizeof tmp, spec, (long long)(((uint64_t)hi << 32) | lo)); }
            else snprintf(tmp, sizeof tmp, spec, (int)next(ctx));
            break;
        case 'u': case 'x': case 'X': case 'o': case 'c':
            snprintf(tmp, sizeof tmp, spec, next(ctx));
            break;
        case 'p': snprintf(tmp, sizeof tmp, "0x%x", next(ctx)); break;
        case 's': { uint32_t a = next(ctx); snprintf(tmp, sizeof tmp, spec, a ? (const char *)HP(a) : "(null)"); break; }
        case 'f': case 'g': case 'e': case 'F': case 'G': case 'E': {
            uint32_t lo = next(ctx), hi = next(ctx);
            uint64_t bits = ((uint64_t)hi << 32) | lo;
            double d; memcpy(&d, &bits, 8);
            snprintf(tmp, sizeof tmp, spec, d);
            break; }
        default: tmp[0] = 0;
        }
        for (char *t = tmp; *t; t++) PUT(*t);
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return (int)o;
#undef PUT
}

/* ---------------- libgcc replacements (soft-float ABI: values in core registers) ---------------- */
static double D(uint32_t lo, uint32_t hi) { uint64_t b = ((uint64_t)hi << 32) | lo; double d; memcpy(&d, &b, 8); return d; }
static void RETD(double d) { uint64_t b; memcpy(&b, &d, 8); R[0] = (uint32_t)b; R[1] = (uint32_t)(b >> 32); }
static float F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static void RETF(float f) { memcpy(&R[0], &f, 4); }
static uint64_t U64(uint32_t lo, uint32_t hi) { return ((uint64_t)hi << 32) | lo; }
static void RET64(uint64_t v) { R[0] = (uint32_t)v; R[1] = (uint32_t)(v >> 32); }
static int32_t f2i(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)d;
}

void H__aeabi_uidiv(void) { R[0] = R[1] ? R[0] / R[1] : 0; }
void H__aeabi_uidivmod(void) { uint32_t a = R[0], b = R[1]; R[0] = b ? a / b : 0; R[1] = b ? a % b : a; }
void H__aeabi_idiv(void) {
    int32_t a = (int32_t)R[0], b = (int32_t)R[1];
    R[0] = b == 0 ? 0 : (b == -1 ? (uint32_t)(-(int64_t)a) : (uint32_t)(a / b));
}
void H__aeabi_idivmod(void) {
    int32_t a = (int32_t)R[0], b = (int32_t)R[1];
    if (b == 0) { R[0] = 0; R[1] = (uint32_t)a; return; }
    if (b == -1) { R[0] = (uint32_t)(-(int64_t)a); R[1] = 0; return; }
    R[0] = (uint32_t)(a / b); R[1] = (uint32_t)(a % b);
}
void H__aeabi_ldivmod(void) {
    int64_t a = (int64_t)U64(R[0], R[1]), b = (int64_t)U64(R[2], R[3]);
    int64_t q = b ? (b == -1 ? (int64_t)(0 - (uint64_t)a) : a / b) : 0, r = b && b != -1 ? a % b : (b ? 0 : a);
    RET64((uint64_t)q); R[2] = (uint32_t)r; R[3] = (uint32_t)((uint64_t)r >> 32);
}
void H__aeabi_uldivmod(void) {
    uint64_t a = U64(R[0], R[1]), b = U64(R[2], R[3]);
    uint64_t q = b ? a / b : 0, r = b ? a % b : a;
    RET64(q); R[2] = (uint32_t)r; R[3] = (uint32_t)(r >> 32);
}
void H__divdi3(void) { H__aeabi_ldivmod(); }
void H__udivdi3(void) { H__aeabi_uldivmod(); }
void H__gnu_ldivmod_helper(void) {
    uint32_t remp = RD32(R[13]);
    H__aeabi_ldivmod();
    WR32(remp, R[2]); WR32(remp + 4, R[3]);
}
void H__gnu_uldivmod_helper(void) {
    uint32_t remp = RD32(R[13]);
    H__aeabi_uldivmod();
    WR32(remp, R[2]); WR32(remp + 4, R[3]);
}
void H__aeabi_ldiv0(void) { R[0] = 0; R[1] = 0; }
void H__aeabi_idiv0(void) { R[0] = 0; }

void H__aeabi_dadd(void) { RETD(D(R[0], R[1]) + D(R[2], R[3])); }
void H__aeabi_dsub(void) { RETD(D(R[0], R[1]) - D(R[2], R[3])); }
void H__aeabi_drsub(void) { RETD(D(R[2], R[3]) - D(R[0], R[1])); }
void H__aeabi_dmul(void) { RETD(D(R[0], R[1]) * D(R[2], R[3])); }
void H__aeabi_ddiv(void) { RETD(D(R[0], R[1]) / D(R[2], R[3])); }
void H__floatunsidf(void) { RETD((double)R[0]); }
void H__aeabi_i2d(void) { RETD((double)(int32_t)R[0]); }
void H__extendsfdf2(void) { RETD((double)F(R[0])); }
void H__aeabi_ul2d(void) { RETD((double)U64(R[0], R[1])); }
void H__aeabi_l2d(void) { RETD((double)(int64_t)U64(R[0], R[1])); }
void H__aeabi_d2iz(void) { R[0] = (uint32_t)f2i(D(R[0], R[1])); }
void H__truncdfsf2(void) { RETF((float)D(R[0], R[1])); }

void H__aeabi_fsub(void) { RETF(F(R[0]) - F(R[1])); }
void H__aeabi_frsub(void) { RETF(F(R[1]) - F(R[0])); }
void H__addsf3(void) { RETF(F(R[0]) + F(R[1])); }
void H__mulsf3(void) { RETF(F(R[0]) * F(R[1])); }
void H__divsf3(void) { RETF(F(R[0]) / F(R[1])); }
void H__floatunsisf(void) { RETF((float)R[0]); }
void H__aeabi_i2f(void) { RETF((float)(int32_t)R[0]); }
void H__floatundisf(void) { RETF((float)U64(R[0], R[1])); }
void H__floatdisf(void) { RETF((float)(int64_t)U64(R[0], R[1])); }
void H__aeabi_f2iz(void) { R[0] = (uint32_t)f2i((double)F(R[0])); }
void H__gtsf2(void) { float a = F(R[0]), b = F(R[1]); R[0] = (a != a || b != b) ? (uint32_t)-1 : (a > b ? 1 : (a < b ? (uint32_t)-1 : 0)); }
void H__ltsf2(void) { float a = F(R[0]), b = F(R[1]); R[0] = (a != a || b != b) ? 1 : (a < b ? (uint32_t)-1 : (a > b ? 1 : 0)); }
void H__nesf2(void) { float a = F(R[0]), b = F(R[1]); R[0] = (a != a || b != b) ? 1 : (a != b); }
void H__aeabi_fcmpeq(void) { R[0] = F(R[0]) == F(R[1]); }
void H__aeabi_fcmplt(void) { R[0] = F(R[0]) < F(R[1]); }
void H__aeabi_fcmple(void) { R[0] = F(R[0]) <= F(R[1]); }
void H__aeabi_fcmpge(void) { R[0] = F(R[0]) >= F(R[1]); }
void H__aeabi_fcmpgt(void) { R[0] = F(R[0]) > F(R[1]); }
static void cfcmp(float a, float b) {
    FN = 0; FV = 0;
    if (a != a || b != b) { FZ = 0; FC = 1; return; }
    FZ = a == b; FC = !(a < b);
}
void H__aeabi_cfcmpeq(void) { cfcmp(F(R[0]), F(R[1])); }
void H__aeabi_cfrcmple(void) { cfcmp(F(R[1]), F(R[0])); }
