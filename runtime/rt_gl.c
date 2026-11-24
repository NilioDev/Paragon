/* OpenGL ES 1.x imports: convert guest arguments and forward to the platform GL. */
#include "rt.h"
#include "gles1.h"

static float FBITS(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static const void *GPTR(uint32_t a) { return a ? HP(a) : (const void *)0; }

void I_glActiveTexture(void) { glActiveTexture(R[0]); }
void I_glBindTexture(void) { glBindTexture(R[0], R[1]); }
void I_glClear(void) { glClear(R[0]); }
void I_glClearColorx(void) { glClearColorx((int32_t)R[0], (int32_t)R[1], (int32_t)R[2], (int32_t)R[3]); }
void I_glColorPointer(void) { glColorPointer((int32_t)R[0], R[1], (int32_t)R[2], GPTR(R[3])); }
void I_glDisable(void) { glDisable(R[0]); }
void I_glDisableClientState(void) { glDisableClientState(R[0]); }
void I_glDrawArrays(void) { glDrawArrays(R[0], (int32_t)R[1], (int32_t)R[2]); }
void I_glEnable(void) { glEnable(R[0]); }
void I_glEnableClientState(void) { glEnableClientState(R[0]); }
void I_glFrustumx(void) {
    glFrustumx((int32_t)R[0], (int32_t)R[1], (int32_t)R[2], (int32_t)R[3], (int32_t)rt_arg(4), (int32_t)rt_arg(5));
}
void I_glGenTextures(void) {
    uint32_t n = R[0], tmp[16];
    if (n > 16) n = 16;
    glGenTextures((int32_t)n, tmp);
    for (uint32_t i = 0; i < n; i++) WR32(R[1] + 4 * i, tmp[i]);
}
void I_glHint(void) { glHint(R[0], R[1]); }
void I_glLoadIdentity(void) { glLoadIdentity(); }
void I_glMatrixMode(void) { glMatrixMode(R[0]); }
void I_glMultMatrixx(void) { glMultMatrixx((const int32_t *)GPTR(R[0])); }
void I_glNormalPointer(void) { glNormalPointer(R[0], (int32_t)R[1], GPTR(R[2])); }
void I_glTexCoordPointer(void) { glTexCoordPointer((int32_t)R[0], R[1], (int32_t)R[2], GPTR(R[3])); }
void I_glTexEnvf(void) { glTexEnvf(R[0], R[1], FBITS(R[2])); }
void I_glTexImage2D(void) {
    glTexImage2D(R[0], (int32_t)R[1], (int32_t)R[2], (int32_t)R[3], (int32_t)rt_arg(4), (int32_t)rt_arg(5),
                 rt_arg(6), rt_arg(7), GPTR(rt_arg(8)));
}
void I_glTexParameterx(void) { glTexParameterx(R[0], R[1], (int32_t)R[2]); }
void I_glTexSubImage2D(void) {
    glTexSubImage2D(R[0], (int32_t)R[1], (int32_t)R[2], (int32_t)R[3], (int32_t)rt_arg(4), (int32_t)rt_arg(5),
                    rt_arg(6), rt_arg(7), GPTR(rt_arg(8)));
}
void I_glTranslatex(void) { glTranslatex((int32_t)R[0], (int32_t)R[1], (int32_t)R[2]); }
void I_glVertexPointer(void) { glVertexPointer((int32_t)R[0], R[1], (int32_t)R[2], GPTR(R[3])); }
void I_glViewport(void) { glViewport((int32_t)R[0], (int32_t)R[1], (int32_t)R[2], (int32_t)R[3]); }
