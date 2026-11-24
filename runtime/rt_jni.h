/* Platform side of the JNI bridge. The test backend fakes Java; the Android backend uses the real JNIEnv. */
#pragma once
#include <stdint.h>

typedef union { int32_t i; int64_t j; float f; double d; void *l; } jb_value;

void *jb_find_class(const char *name);
void *jb_new_global(void *obj);
void jb_delete_ref(void *obj, int global);       /* global: 0 local, 1 global, 2 method record (ignore) */
void *jb_get_static_method(void *cls, const char *name, const char *sig);
void jb_call_static(void *cls, void *mid, const char *sig, const jb_value *args, int nargs, jb_value *ret);
void *jb_new_string(const char *utf);
char *jb_get_string(void *str);                   /* malloc'd UTF-8 copy */
int jb_array_length(void *arr);
void jb_array_read(void *arr, int off, int len, void *dst);
void *jb_new_byte_array(const void *data, int len);
void rt_jni_end_entry(void);
