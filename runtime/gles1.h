/* Minimal OpenGL ES 1.x prototypes (ABI-compatible with libGLESv1_CM). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void glActiveTexture(uint32_t texture);
void glBindTexture(uint32_t target, uint32_t texture);
void glClear(uint32_t mask);
void glClearColorx(int32_t r, int32_t g, int32_t b, int32_t a);
void glColorPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr);
void glDisable(uint32_t cap);
void glDisableClientState(uint32_t array);
void glDrawArrays(uint32_t mode, int32_t first, int32_t count);
void glEnable(uint32_t cap);
void glEnableClientState(uint32_t array);
void glFrustumx(int32_t l, int32_t r, int32_t b, int32_t t, int32_t n, int32_t f);
void glGenTextures(int32_t n, uint32_t *textures);
void glHint(uint32_t target, uint32_t mode);
void glLoadIdentity(void);
void glMatrixMode(uint32_t mode);
void glMultMatrixx(const int32_t *m);
void glNormalPointer(uint32_t type, int32_t stride, const void *ptr);
void glTexCoordPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr);
void glTexEnvf(uint32_t target, uint32_t pname, float param);
void glTexImage2D(uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height,
                  int32_t border, uint32_t format, uint32_t type, const void *pixels);
void glTexParameterx(uint32_t target, uint32_t pname, int32_t param);
void glTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width,
                     int32_t height, uint32_t format, uint32_t type, const void *pixels);
void glTranslatex(int32_t x, int32_t y, int32_t z);
void glVertexPointer(int32_t size, uint32_t type, int32_t stride, const void *ptr);
void glViewport(int32_t x, int32_t y, int32_t width, int32_t height);
/* used by the runtime's own presentation code */
void glOrthof(float l, float r, float b, float t, float n, float f);
void glColor4f(float r, float g, float b, float a);
void glBlendFunc(uint32_t s, uint32_t d);
void glPixelStorei(uint32_t pname, int32_t param);
void glClearColor(float r, float g, float b, float a);
#ifdef __cplusplus
}
#endif
