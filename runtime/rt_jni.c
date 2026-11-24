/* JNI bridge: the guest sees a fake 32-bit JNIEnv whose functions land here.
 * Objects are passed to the guest as small 32-bit handles; byte arrays are copied into
 * guest memory using the old Dalvik ArrayObject layout (length at +8, data at +16),
 * because the game reads array contents directly instead of calling JNI. */
#include "rt.h"
#include "rt_jni.h"
#include <stdio.h>
#include <stdlib.h>

#define HBASE 0x00100000u
#define MAXH 65536
static void *htab[MAXH];
static uint8_t hglobal[MAXH];
static uint32_t hnext = 1;

static uint32_t hput(void *o, int global) {
    if (!o) return 0;
    for (uint32_t n = 0; n < MAXH; n++) {
        uint32_t i = hnext++;
        if (hnext >= MAXH) hnext = 1;
        if (!htab[i]) { htab[i] = o; hglobal[i] = (uint8_t)global; return HBASE + 4 * i; }
    }
    rt_fatal("JNI handle table full");
}
static int is_handle(uint32_t h) { return h >= HBASE + 4 && h < HBASE + 4 * MAXH; }
static void *hget(uint32_t h) { return is_handle(h) ? htab[(h - HBASE) / 4] : NULL; }
static void hdel(uint32_t h) {
    if (!is_handle(h)) return;
    uint32_t i = (h - HBASE) / 4;
    if (hglobal[i] == 2) return;   /* cached method records stay */
    if (htab[i]) jb_delete_ref(htab[i], hglobal[i]);
    htab[i] = NULL;
}

/* byte arrays living in guest memory */
#define MAXA 256
static uint32_t arrays[MAXA];
static int is_garray(uint32_t a) { for (int i = 0; i < MAXA; i++) if (arrays[i] == a && a) return 1; return 0; }
static uint32_t garray_new(uint32_t len) {
    uint32_t a = g_malloc(16 + len + 8);
    WR32(a, 0xC1A55u); WR32(a + 4, 0); WR32(a + 8, len); WR32(a + 12, 0);
    for (int i = 0; i < MAXA; i++) if (!arrays[i]) { arrays[i] = a; return a; }
    rt_log("JNI: too many live byte arrays");
    return a;
}
static void garray_free(uint32_t a) {
    for (int i = 0; i < MAXA; i++) if (arrays[i] == a) { arrays[i] = 0; g_free(a); return; }
}
void rt_jni_end_entry(void) {
    for (int i = 0; i < MAXA; i++) if (arrays[i]) { g_free(arrays[i]); arrays[i] = 0; }
    for (uint32_t i = 1; i < MAXH; i++) if (htab[i] && !hglobal[i]) { jb_delete_ref(htab[i], 0); htab[i] = NULL; }
}

/* methods: remember the signature so arguments can be decoded */
struct method { void *cls; char *name; char *sig; void *mid; uint32_t handle; };
#define MAXM 256
static struct method *methods[MAXM];
static int nmethods;
/* the game looks methods up again on every call: cache them so handles do not leak */
static uint32_t get_method(void *cls, const char *name, const char *sig) {
    for (int k = 0; k < nmethods; k++)
        if (!strcmp(methods[k]->name, name) && !strcmp(methods[k]->sig, sig)) return methods[k]->handle;
    void *mid = jb_get_static_method(cls, name, sig);
    if (!mid) return 0;
    struct method *m = malloc(sizeof *m);
    m->cls = cls; m->name = strdup(name); m->sig = strdup(sig); m->mid = mid;
    m->handle = hput(m, 2);
    if (nmethods < MAXM) methods[nmethods++] = m;
    return m->handle;
}

/* argument readers */
struct argsrc { int kind; uint32_t p; int slot; };   /* kind 0: va_list memory, 1: register/stack slots, 2: jvalue[] */
static uint32_t src32(struct argsrc *s) {
    if (s->kind == 0) { uint32_t v = RD32(s->p); s->p += 4; return v; }
    if (s->kind == 1) return rt_arg(s->slot++);
    uint32_t v = RD32(s->p); s->p += 8; return v;
}
static uint64_t src64(struct argsrc *s) {
    uint32_t lo, hi;
    if (s->kind == 0) { s->p = (s->p + 7) & ~7u; lo = RD32(s->p); hi = RD32(s->p + 4); s->p += 8; }
    else if (s->kind == 1) { if (s->slot & 1) s->slot++; lo = rt_arg(s->slot++); hi = rt_arg(s->slot++); }
    else { lo = RD32(s->p); hi = RD32(s->p + 4); s->p += 8; }
    return ((uint64_t)hi << 32) | lo;
}

static void *obj_of(uint32_t h) {
    if (is_garray(h)) {
        /* guest-owned copy of a Java byte[]: create a real array for Java */
        uint32_t len = RD32(h + 8);
        return jb_new_byte_array(HP(h + 16), (int)len);
    }
    return hget(h);
}

static int decode_args(const char *sig, struct argsrc *s, jb_value *out, int max) {
    int n = 0;
    const char *p = sig + 1;
    while (*p && *p != ')' && n < max) {
        char c = *p;
        if (c == 'J') out[n].j = (int64_t)src64(s);
        else if (c == 'D') { uint64_t b = src64(s); memcpy(&out[n].d, &b, 8); }
        else if (c == 'F') {
            if (s->kind == 2) { uint32_t b = src32(s); memcpy(&out[n].f, &b, 4); }
            else { uint64_t b = src64(s); double d; memcpy(&d, &b, 8); out[n].f = (float)d; }
        } else if (c == 'L' || c == '[') out[n].l = obj_of(src32(s));
        else out[n].i = (int32_t)src32(s);
        /* advance past the type */
        while (*p == '[') p++;
        if (*p == 'L') { while (*p && *p != ';') p++; }
        p++;
        n++;
    }
    return n;
}

static uint32_t to_guest_object(void *o, const char *retsig) {
    if (!o) return 0;
    if (retsig[0] == '[' && retsig[1] == 'B') {
        int len = jb_array_length(o);
        uint32_t a = garray_new((uint32_t)len);
        jb_array_read(o, 0, len, HP(a + 16));
        jb_delete_ref(o, 0);
        return a;
    }
    return hput(o, 0);
}

static void call_static(int kind) {
    void *cls = hget(R[1]);
    struct method *m = hget(R[2]);
    if (!m) rt_fatal("JNI CallStatic with bad method handle %08x", R[2]);
    struct argsrc s;
    if (kind == 0) { s.kind = 1; s.slot = 3; }            /* ... */
    else if (kind == 1) { s.kind = 0; s.p = R[3]; }       /* va_list */
    else { s.kind = 2; s.p = R[3]; }                       /* jvalue* */
    jb_value args[16], ret;
    memset(&ret, 0, sizeof ret);
    int n = decode_args(m->sig, &s, args, 16);
    const char *rs = strchr(m->sig, ')') + 1;
    jb_call_static(cls, m->mid, m->sig, args, n, &ret);
    switch (rs[0]) {
    case 'V': break;
    case 'L': case '[': R[0] = to_guest_object(ret.l, rs); break;
    case 'J': R[0] = (uint32_t)ret.j; R[1] = (uint32_t)((uint64_t)ret.j >> 32); break;
    case 'D': { uint64_t b; memcpy(&b, &ret.d, 8); R[0] = (uint32_t)b; R[1] = (uint32_t)(b >> 32); break; }
    case 'F': memcpy(&R[0], &ret.f, 4); break;
    case 'Z': R[0] = (uint32_t)(ret.i & 0xff); break;
    default: R[0] = (uint32_t)ret.i;
    }
}

static uint32_t strings_held[64];

void backend_jni(uint32_t i) {
    switch (i) {
    case 4: R[0] = 0x00010004; return;                                     /* GetVersion */
    case 6: R[0] = hput(jb_find_class((const char *)HP(R[1])), 0); return;  /* FindClass */
    case 15: R[0] = 0; return;                                              /* ExceptionOccurred */
    case 16: case 17: return;                                               /* ExceptionDescribe/Clear */
    case 21: {                                                              /* NewGlobalRef */
        void *o = hget(R[1]);
        R[0] = o ? hput(jb_new_global(o), 1) : (is_garray(R[1]) ? R[1] : 0);
        return; }
    case 22: case 23:                                                       /* Delete{Global,Local}Ref */
        if (is_garray(R[1])) garray_free(R[1]); else hdel(R[1]);
        return;
    case 113: {                                                             /* GetStaticMethodID */
        R[0] = get_method(hget(R[1]), (const char *)HP(R[2]), (const char *)HP(R[3]));
        return; }
    case 228: R[0] = 0; return;                                             /* ExceptionCheck */
    case 167: R[0] = hput(jb_new_string((const char *)HP(R[1])), 0); return; /* NewStringUTF */
    case 168: { char *s = jb_get_string(hget(R[1])); R[0] = s ? (uint32_t)strlen(s) : 0; free(s); return; }
    case 169: {                                                             /* GetStringUTFChars */
        char *s = jb_get_string(hget(R[1]));
        R[0] = g_strdup(s ? s : "");
        free(s);
        if (R[2]) WR8(R[2], 1);
        for (int k = 0; k < 64; k++) if (!strings_held[k]) { strings_held[k] = R[0]; break; }
        return; }
    case 170:                                                               /* ReleaseStringUTFChars */
        for (int k = 0; k < 64; k++) if (strings_held[k] == R[2]) { strings_held[k] = 0; g_free(R[2]); break; }
        return;
    case 171:                                                               /* GetArrayLength */
        R[0] = is_garray(R[1]) ? RD32(R[1] + 8) : (uint32_t)jb_array_length(hget(R[1]));
        return;
    case 176: R[0] = garray_new(R[1]); memset(HP(R[0] + 16), 0, R[1]); return; /* NewByteArray */
    case 184:                                                               /* GetByteArrayElements */
        if (!is_garray(R[1])) rt_fatal("GetByteArrayElements on non-guest array");
        if (R[2]) WR8(R[2], 0);
        R[0] = R[1] + 16;
        return;
    case 192: return;                                                       /* ReleaseByteArrayElements */
    case 200:                                                               /* GetByteArrayRegion */
        if (is_garray(R[1])) memcpy(HP(rt_arg(4)), HP(R[1] + 16 + R[2]), R[3]);
        else jb_array_read(hget(R[1]), (int)R[2], (int)R[3], HP(rt_arg(4)));
        return;
    case 208:                                                               /* SetByteArrayRegion */
        if (is_garray(R[1])) memcpy(HP(R[1] + 16 + R[2]), HP(rt_arg(4)), R[3]);
        return;
    }
    if (i >= 114 && i <= 143) { call_static((int)((i - 114) % 3)); return; }
    rt_fatal("JNI function #%u is not implemented", i);
}

void backend_vm(uint32_t i) {
    switch (i) {
    case 4: case 6: case 7:                     /* AttachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon */
        WR32(R[1], GUEST_ENV);
        R[0] = 0;
        return;
    case 5: R[0] = 0; return;                   /* DetachCurrentThread */
    }
    rt_fatal("JavaVM function #%u is not implemented", i);
}
