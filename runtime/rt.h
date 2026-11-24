/* Paragon runtime: CPU state and guest memory for statically recompiled ARM code. */
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rt_fn)(void);

/* ---- CPU state (only one guest thread runs at a time; callers serialise) ---- */
extern uint32_t R[16];
extern uint32_t FN, FZ, FC, FV;
extern uint32_t PEND;            /* pending indirect jump target that the current function could not resolve */
extern uint8_t *G;               /* host address of guest address 0 */

/* ---- guest memory map ---- */
#define GUEST_IMP_BASE   0x0F000000u   /* imported functions: base + 4*index */
#define GUEST_DATAIMP    0x0F100000u   /* imported data objects */
#define GUEST_ENV        0x0E000000u   /* JNIEnv* / JavaVM* structs */
#define GUEST_JNI_FN     0x0E100000u   /* fake JNIEnv function addresses: base + 4*index */
#define GUEST_VM_FN      0x0E200000u   /* fake JavaVM function addresses */
#define GUEST_HEAP_LO    0x20000000u
#define GUEST_HEAP_HI    0x40000000u
#define GUEST_STACK_LO   0x7E000000u
#define GUEST_STACK_HI   0x7F000000u
#define GUEST_MAGIC_RET  0xFFFF0001u

typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;

#define HP(a) ((void *)(G + (uint32_t)(a)))
static inline uint32_t RD32(uint32_t a) { return *(u32u *)(G + a); }
static inline uint32_t RD16(uint32_t a) { return *(u16u *)(G + a); }
static inline uint32_t RD8(uint32_t a) { return G[a]; }
static inline uint32_t RDS16(uint32_t a) { return (uint32_t)(int32_t)(int16_t)*(u16u *)(G + a); }
static inline uint32_t RDS8(uint32_t a) { return (uint32_t)(int32_t)(int8_t)G[a]; }
static inline void WR32(uint32_t a, uint32_t v) { *(u32u *)(G + a) = v; }
static inline void WR16(uint32_t a, uint32_t v) { *(u16u *)(G + a) = (uint16_t)v; }
static inline void WR8(uint32_t a, uint32_t v) { G[a] = (uint8_t)v; }

/* ---- flag-setting arithmetic ---- */
static inline uint32_t ADDS(uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    FN = r >> 31; FZ = r == 0; FC = r < a; FV = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t ADCS(uint32_t a, uint32_t b) {
    uint64_t w = (uint64_t)a + b + FC;
    uint32_t r = (uint32_t)w;
    FN = r >> 31; FZ = r == 0; FC = (uint32_t)(w >> 32); FV = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t SUBS(uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    FN = r >> 31; FZ = r == 0; FC = a >= b; FV = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t SBCS(uint32_t a, uint32_t b) {
    uint32_t borrow = !FC;
    uint32_t r = a - b - borrow;
    FN = r >> 31; FZ = r == 0; FC = (uint64_t)a >= (uint64_t)b + borrow; FV = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t CLZ(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

/* ---- shifts (n is the full shift amount, already masked to 0..255 for register shifts) ---- */
static inline uint32_t SH_LSL(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v << n; }
static inline uint32_t SH_LSR(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v >> n; }
static inline uint32_t SH_ASR(uint32_t v, uint32_t n) { return (uint32_t)((int32_t)v >> (n >= 32 ? 31 : n)); }
static inline uint32_t SH_ROR(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint32_t SH_LSL_C(uint32_t v, uint32_t n, uint32_t *c) {
    if (n == 0) { *c = FC; return v; }
    if (n < 32) { *c = (v >> (32 - n)) & 1; return v << n; }
    *c = n == 32 ? (v & 1) : 0; return 0;
}
static inline uint32_t SH_LSR_C(uint32_t v, uint32_t n, uint32_t *c) {
    if (n == 0) { *c = FC; return v; }
    if (n < 32) { *c = (v >> (n - 1)) & 1; return v >> n; }
    *c = n == 32 ? (v >> 31) : 0; return 0;
}
static inline uint32_t SH_ASR_C(uint32_t v, uint32_t n, uint32_t *c) {
    if (n == 0) { *c = FC; return v; }
    if (n < 32) { *c = (v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
    *c = v >> 31; return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t SH_ROR_C(uint32_t v, uint32_t n, uint32_t *c) {
    if (n == 0) { *c = FC; return v; }
    n &= 31;
    uint32_t r = n ? (v >> n) | (v << (32 - n)) : v;
    *c = r >> 31; return r;
}

/* ---- control flow ---- */
void rt_call(uint32_t target);           /* call a guest/import/JNI address */
void rt_jump(uint32_t target);           /* unresolved indirect jump from a function's __ind block */
void rt_fatal(const char *fmt, ...) __attribute__((noreturn));
void rt_unimpl(uint32_t pc, const char *what);
rt_fn rt_lookup(uint32_t addr);          /* generated */

/* ---- generated tables ---- */
struct rt_export { const char *name; rt_fn fn; };
extern const struct rt_export rt_exports[];
extern const rt_fn rt_imports[];
extern const char *const rt_import_names[];
extern const uint32_t rt_import_count, rt_image_base, rt_image_size, rt_stack_chk_guard;

/* ---- runtime services ---- */
int rt_init(const void *image, uint32_t image_size);
uint32_t rt_enter(rt_fn fn, int nargs, const uint32_t *args);
uint32_t rt_export_call(const char *name, int nargs, const uint32_t *args);
uint32_t g_malloc(uint32_t n);
void g_free(uint32_t a);
uint32_t g_strdup(const char *s);
uint32_t rt_arg(int i);                  /* i-th 32-bit argument of the current call (r0-r3, then stack) */
void rt_log(const char *fmt, ...);
int rt_vformat(char *out, size_t cap, const char *fmt, uint32_t (*next)(void *), void *ctx);

/* ---- presentation / diagnostics (present.c) ---- */
void rt_diag_line(const char *msg, int alert);
void rt_diag_dead(void);
int rt_is_dead(void);
void rt_present(uint32_t pixels, int w, int h);
void rt_present_reset(void);

/* ---- backend (platform) hooks ---- */
void backend_jni(uint32_t index);        /* a guest call of JNIEnv function #index */
void backend_vm(uint32_t index);         /* a guest call of JavaVM function #index */
void backend_log(const char *msg);
uint64_t backend_time_us(void);           /* wall clock, microseconds */

#ifdef __cplusplus
}
#endif
