#ifndef JNI_PATCH_H
#define JNI_PATCH_H

extern char fake_vm[0x1000];
extern char fake_env[0x1000];

void  jni_init(void);
void  jni_load(void);
void *NVThreadGetCurrentJNIEnv(void);

#endif /* JNI_PATCH_H */
