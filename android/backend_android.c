/* Android host: exposes the game's JNI entry points from a native 64-bit library and
 * forwards the recompiled code's Java calls to the real JNIEnv. */
#include "../runtime/rt.h"
#include "../runtime/rt_jni.h"
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern const unsigned char zen_image[];
extern const unsigned int zen_image_size;

static JavaVM *jvm;
static __thread JNIEnv *cur_env;
static pthread_mutex_t lock;

void backend_log(const char *m) { __android_log_write(ANDROID_LOG_INFO, "Paragon", m); }
uint64_t backend_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

static JNIEnv *env(void) {
    if (!cur_env) (*jvm)->AttachCurrentThread(jvm, &cur_env, NULL);
    return cur_env;
}
static void check_exc(const char *what) {
    JNIEnv *e = env();
    if ((*e)->ExceptionCheck(e)) {
        jthrowable t = (*e)->ExceptionOccurred(e);
        (*e)->ExceptionClear(e);
        char line[200];
        snprintf(line, sizeof line, "Java exception in %s", what);
        jclass oc = (*e)->FindClass(e, "java/lang/Object");
        jmethodID ts = oc ? (*e)->GetMethodID(e, oc, "toString", "()Ljava/lang/String;") : NULL;
        jstring str = ts ? (jstring)(*e)->CallObjectMethod(e, t, ts) : NULL;
        if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
        if (str) {
            const char *c = (*e)->GetStringUTFChars(e, str, NULL);
            snprintf(line, sizeof line, "Java %s: %s", what, c ? c : "?");
            if (c) (*e)->ReleaseStringUTFChars(e, str, c);
        }
        backend_log(line);
        rt_diag_line(line, 1);
    }
}

void *jb_find_class(const char *name) { void *c = (*env())->FindClass(env(), name); check_exc(name); return c; }
void *jb_new_global(void *o) { return (*env())->NewGlobalRef(env(), (jobject)o); }
void jb_delete_ref(void *o, int global) {
    if (global == 1) (*env())->DeleteGlobalRef(env(), (jobject)o);
    else if (global == 0) (*env())->DeleteLocalRef(env(), (jobject)o);
}
void *jb_get_static_method(void *cls, const char *name, const char *sig) {
    void *m = (*env())->GetStaticMethodID(env(), (jclass)cls, name, sig);
    check_exc(name);
    return m;
}
void *jb_new_string(const char *utf) { return (*env())->NewStringUTF(env(), utf); }
char *jb_get_string(void *s) {
    if (!s) return strdup("");
    const char *c = (*env())->GetStringUTFChars(env(), (jstring)s, NULL);
    char *r = strdup(c ? c : "");
    if (c) (*env())->ReleaseStringUTFChars(env(), (jstring)s, c);
    return r;
}
int jb_array_length(void *a) { return a ? (*env())->GetArrayLength(env(), (jarray)a) : 0; }
void jb_array_read(void *a, int off, int len, void *dst) { (*env())->GetByteArrayRegion(env(), (jbyteArray)a, off, len, dst); }
void *jb_new_byte_array(const void *data, int len) {
    jbyteArray a = (*env())->NewByteArray(env(), len);
    (*env())->SetByteArrayRegion(env(), a, 0, len, data);
    return a;
}

void jb_call_static(void *cls, void *mid, const char *sig, const jb_value *args, int n, jb_value *ret) {
    JNIEnv *e = env();
    jvalue jv[16];
    const char *p = sig + 1;
    for (int i = 0; i < n && i < 16; i++) {
        switch (*p) {
        case 'Z': jv[i].z = (jboolean)args[i].i; break;
        case 'B': jv[i].b = (jbyte)args[i].i; break;
        case 'C': jv[i].c = (jchar)args[i].i; break;
        case 'S': jv[i].s = (jshort)args[i].i; break;
        case 'I': jv[i].i = args[i].i; break;
        case 'J': jv[i].j = args[i].j; break;
        case 'F': jv[i].f = args[i].f; break;
        case 'D': jv[i].d = args[i].d; break;
        default: jv[i].l = (jobject)args[i].l;
        }
        while (*p == '[') p++;
        if (*p == 'L') while (*p && *p != ';') p++;
        p++;
    }
    const char *rs = strchr(sig, ')') + 1;
    jclass c = (jclass)cls;
    jmethodID m = (jmethodID)mid;
    switch (rs[0]) {
    case 'V': (*e)->CallStaticVoidMethodA(e, c, m, jv); break;
    case 'Z': ret->i = (*e)->CallStaticBooleanMethodA(e, c, m, jv); break;
    case 'B': ret->i = (*e)->CallStaticByteMethodA(e, c, m, jv); break;
    case 'C': ret->i = (*e)->CallStaticCharMethodA(e, c, m, jv); break;
    case 'S': ret->i = (*e)->CallStaticShortMethodA(e, c, m, jv); break;
    case 'I': ret->i = (*e)->CallStaticIntMethodA(e, c, m, jv); break;
    case 'J': ret->j = (*e)->CallStaticLongMethodA(e, c, m, jv); break;
    case 'F': ret->f = (*e)->CallStaticFloatMethodA(e, c, m, jv); break;
    case 'D': ret->d = (*e)->CallStaticDoubleMethodA(e, c, m, jv); break;
    default: ret->l = (*e)->CallStaticObjectMethodA(e, c, m, jv);
    }
    check_exc(sig);
    /* objects created for arguments are local references; free byte[] copies we made */
    p = sig + 1;
    for (int i = 0; i < n && i < 16; i++) {
        if (*p == '[' && p[1] == 'B' && jv[i].l) (*e)->DeleteLocalRef(e, jv[i].l);
        while (*p == '[') p++;
        if (*p == 'L') while (*p && *p != ';') p++;
        p++;
    }
}

/* ---------------- entry points ---------------- */
static uint32_t cls_handle;

static uint32_t enter(JNIEnv *e, const char *name, int n, uint32_t a, uint32_t b, uint32_t c) {
    pthread_mutex_lock(&lock);
    JNIEnv *saved = cur_env;
    cur_env = e;
    uint32_t args[5] = { GUEST_ENV, cls_handle, a, b, c };
    uint32_t r = rt_export_call(name, n, args);
    cur_env = saved;
    pthread_mutex_unlock(&lock);
    return r;
}

#define N(x) "Java_com_gamevil_nexus2_Natives_" #x
#define VOID0(x) JNIEXPORT void JNICALL Java_com_gamevil_nexus2_Natives_##x(JNIEnv *e, jclass c) { (void)c; enter(e, N(x), 2, 0, 0, 0); }
#define VOIDZ(x) JNIEXPORT void JNICALL Java_com_gamevil_nexus2_Natives_##x(JNIEnv *e, jclass c, jboolean v) { (void)c; enter(e, N(x), 3, v, 0, 0); }

VOID0(InitializeJNIGlobalRef)
VOID0(NativeDestroyClet)
/* NativeInit runs the game's initPlatformSize: take the widescreen width the Java side chose first */
extern int paragon_screen_w;
JNIEXPORT void JNICALL Java_com_gamevil_nexus2_Natives_NativeInit(JNIEnv *e, jclass c) {
    (void)c;
    static int have_width;
    if (!have_width) {
        have_width = 1;
        jclass wc = (*e)->FindClass(e, "com/paragon/Wide");
        jfieldID f = wc ? (*e)->GetStaticFieldID(e, wc, "gameW", "I") : NULL;
        if (f) {
            int w = (*e)->GetStaticIntField(e, wc, f);
            if (w >= 320 && w <= 1024) paragon_screen_w = w & ~1;
        }
        if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
        char m[64];
        snprintf(m, sizeof m, "game width %d", paragon_screen_w);
        backend_log(m);
    }
    enter(e, N(NativeInit), 2, 0, 0, 0);
}
VOID0(NativePauseClet)
VOID0(NativeResumeClet)
VOIDZ(SetDpadDrawingBlock)
VOIDZ(SetShowCancelButton)
VOIDZ(SetShowDirectionButton)
VOIDZ(SetShowGameMenuButton)
VOIDZ(SetShowMapButton)
VOIDZ(SetShowSelectButton)
VOIDZ(SetShowSlotSwapButton)

JNIEXPORT void JNICALL Java_com_gamevil_nexus2_Natives_NativeResize(JNIEnv *e, jclass c, jint w, jint h) {
    (void)c;
    rt_present_reset();            /* a new surface may mean a new GL context */
    enter(e, N(NativeResize), 4, (uint32_t)w, (uint32_t)h, 0);
}
JNIEXPORT void JNICALL Java_com_gamevil_nexus2_Natives_NativeRender(JNIEnv *e, jclass c) {
    (void)c;
    if (rt_is_dead()) { pthread_mutex_lock(&lock); rt_present(0, 0, 0); pthread_mutex_unlock(&lock); return; }
    enter(e, N(NativeRender), 2, 0, 0, 0);
}
JNIEXPORT void JNICALL Java_com_gamevil_nexus2_Natives_handleCletEvent(JNIEnv *e, jclass c, jint a, jint b, jint d) {
    (void)c; enter(e, N(handleCletEvent), 5, (uint32_t)a, (uint32_t)b, (uint32_t)d);
}
JNIEXPORT void JNICALL Java_com_gamevil_nexus2_Natives_setInputEvent(JNIEnv *e, jclass c, jint a, jint b, jint d) {
    (void)c; enter(e, N(setInputEvent), 5, (uint32_t)a, (uint32_t)b, (uint32_t)d);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    jvm = vm;
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &at);
    if (rt_init(zen_image, zen_image_size) != 0) {
        backend_log("could not reserve guest address space");
        return JNI_ERR;
    }
    JNIEnv *e;
    (*vm)->GetEnv(vm, (void **)&e, JNI_VERSION_1_4);
    pthread_mutex_lock(&lock);
    cur_env = e;
    uint32_t args[2] = { GUEST_ENV + 0x10, 0 };
    rt_export_call("JNI_OnLoad", 2, args);
    cur_env = NULL;
    pthread_mutex_unlock(&lock);
    return JNI_VERSION_1_4;
}
