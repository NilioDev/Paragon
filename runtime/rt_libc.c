/* Host implementations of the C library functions the game imports.
 * Arguments come from guest registers/stack; pointers are guest addresses. */
#include "rt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define S(a) ((char *)HP(a))
#define A(i) rt_arg(i)

void I_malloc(void) { R[0] = g_malloc(R[0]); }
void I_free(void) { g_free(R[0]); }
void I_memcpy(void) { memmove(HP(R[0]), HP(R[1]), R[2]); }
void I_memmove(void) { memmove(HP(R[0]), HP(R[1]), R[2]); }
void I_memset(void) { memset(HP(R[0]), (int)R[1], R[2]); }
void I_strlen(void) { R[0] = (uint32_t)strlen(S(R[0])); }
void I_strcpy(void) { strcpy(S(R[0]), S(R[1])); }
void I_strncpy(void) { strncpy(S(R[0]), S(R[1]), R[2]); }
void I_strcat(void) { strcat(S(R[0]), S(R[1])); }
void I_strncat(void) { strncat(S(R[0]), S(R[1]), R[2]); }
void I_strcmp(void) { R[0] = (uint32_t)strcmp(S(R[0]), S(R[1])); }
void I_strncmp(void) { R[0] = (uint32_t)strncmp(S(R[0]), S(R[1]), R[2]); }
void I_strchr(void) { char *p = strchr(S(R[0]), (int)R[1]); R[0] = p ? (uint32_t)((uint8_t *)p - G) : 0; }
void I_strstr(void) { char *p = strstr(S(R[0]), S(R[1])); R[0] = p ? (uint32_t)((uint8_t *)p - G) : 0; }
void I_atoi(void) { R[0] = (uint32_t)atoi(S(R[0])); }
void I_abort(void) { rt_fatal("guest called abort()"); }
void I___stack_chk_fail(void) { rt_fatal("guest stack protector tripped"); }
void I_putchar(void) { }
void I_sqrt(void) {
    uint64_t b = ((uint64_t)R[1] << 32) | R[0];
    double d; memcpy(&d, &b, 8); d = sqrt(d); memcpy(&b, &d, 8);
    R[0] = (uint32_t)b; R[1] = (uint32_t)(b >> 32);
}

/* ---- formatted output ---- */
struct va_ctx { uint32_t p; };
static uint32_t va_next(void *c) { struct va_ctx *v = c; uint32_t x = RD32(v->p); v->p += 4; return x; }
struct reg_ctx { int i; };
static uint32_t reg_next(void *c) { struct reg_ctx *r = c; return rt_arg(r->i++); }

void I_vsprintf(void) {
    char buf[4096];
    struct va_ctx v = { R[2] };
    int n = rt_vformat(buf, sizeof buf, S(R[1]), va_next, &v);
    strcpy(S(R[0]), buf);
    R[0] = (uint32_t)n;
}
void I_printf(void) { R[0] = 0; }
void I_vprintf(void) { R[0] = 0; }
void I___android_log_print(void) {
    char buf[1024];
    struct reg_ctx r = { 3 };
    rt_vformat(buf, sizeof buf, S(R[2]), reg_next, &r);
    rt_log("[game] %s", buf);
    R[0] = 0;
}

/* ---- time ---- */
void I_gettimeofday(void) {
    uint64_t us = backend_time_us();
    if (R[0]) { WR32(R[0], (uint32_t)(us / 1000000)); WR32(R[0] + 4, (uint32_t)(us % 1000000)); }
    R[0] = 0;
}
void I_time(void) {
    uint32_t t = (uint32_t)(backend_time_us() / 1000000);
    if (R[0]) WR32(R[0], t);
    R[0] = t;
}
static uint32_t tm_buf;
void I_localtime(void) {
    time_t t = (time_t)RD32(R[0]);
    struct tm tm;
    localtime_r(&t, &tm);
    if (!tm_buf) tm_buf = g_malloc(64);
    uint32_t v[11] = { (uint32_t)tm.tm_sec, (uint32_t)tm.tm_min, (uint32_t)tm.tm_hour, (uint32_t)tm.tm_mday,
                       (uint32_t)tm.tm_mon, (uint32_t)tm.tm_year, (uint32_t)tm.tm_wday, (uint32_t)tm.tm_yday,
                       (uint32_t)tm.tm_isdst, 0, 0 };
    for (int i = 0; i < 11; i++) WR32(tm_buf + 4 * (uint32_t)i, v[i]);
    R[0] = tm_buf;
}

/* ---- files (the game only uses absolute paths it got from Java) ---- */
#define MAXF 32
static FILE *files[MAXF];
#define FHANDLE(i) (0x0F200000u + 16u * (uint32_t)(i))
static FILE *fget(uint32_t h) {
    uint32_t i = (h - 0x0F200000u) / 16;
    return i < MAXF ? files[i] : NULL;
}
void I_fopen(void) {
    char mode[8] = { 0 };
    strncpy(mode, S(R[1]), 6);
    if (!strchr(mode, 'b')) strcat(mode, "b");
    FILE *f = fopen(S(R[0]), mode);
    R[0] = 0;
    if (!f) return;
    for (int i = 1; i < MAXF; i++)
        if (!files[i]) { files[i] = f; R[0] = FHANDLE(i); return; }
    fclose(f);
}
void I_fclose(void) {
    uint32_t i = (R[0] - 0x0F200000u) / 16;
    if (i < MAXF && files[i]) { fclose(files[i]); files[i] = NULL; }
    R[0] = 0;
}
void I_fread(void) {
    FILE *f = fget(R[3]);
    R[0] = f ? (uint32_t)fread(HP(R[0]), R[1], R[2], f) : 0;
}
void I_fwrite(void) {
    FILE *f = fget(R[3]);
    R[0] = f ? (uint32_t)fwrite(HP(R[0]), R[1], R[2], f) : 0;
    if (f) fflush(f);
}
void I_access(void) { R[0] = (uint32_t)access(S(R[0]), (int)R[1]); }
void I_unlink(void) { R[0] = (uint32_t)unlink(S(R[0])); }
void I_rename(void) { R[0] = (uint32_t)rename(S(R[0]), S(R[1])); }
void I_stat(void) {
    struct stat st;
    int r = stat(S(R[0]), &st);
    if (r == 0 && R[1]) {
        /* 32-bit ARM bionic struct stat layout */
        memset(HP(R[1]), 0, 104);
        WR32(R[1] + 16, (uint32_t)st.st_mode);
        WR32(R[1] + 48, (uint32_t)st.st_size);
        WR32(R[1] + 52, (uint32_t)((uint64_t)st.st_size >> 32));
        WR32(R[1] + 64, (uint32_t)st.st_mtime);
    }
    R[0] = (uint32_t)r;
}

/* ---- C++ runtime ---- */
void I___cxa_guard_acquire(void) { R[0] = (RD32(R[0]) & 1) ? 0 : 1; }
void I___cxa_guard_release(void) { WR32(R[0], 1); }
void I___cxa_begin_cleanup(void) { rt_fatal("C++ exception cleanup is not supported"); }
void I___cxa_call_unexpected(void) { rt_fatal("C++ unexpected exception"); }
void I___cxa_type_match(void) { rt_fatal("C++ exception type match is not supported"); }
void I___gnu_Unwind_Find_exidx(void) { R[0] = 0; }
