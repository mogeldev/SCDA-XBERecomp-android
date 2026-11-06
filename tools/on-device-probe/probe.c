#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>

JNIEXPORT jint JNICALL
Java_io_github_mogeldev_scda_compilerprobe_ProbeActivity_checkLibrary(
    JNIEnv* env, jclass cls, jstring path) {
    (void)cls;
    const char* name = (*env)->GetStringUTFChars(env, path, NULL);
    if (!name) return -1;
    void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    (*env)->ReleaseStringUTFChars(env, path, name);
    if (!lib) {
        (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/IllegalStateException"), dlerror());
        return -1;
    }
    dlerror();
    int (*answer)(int) = (int (*)(int))dlsym(lib, "probe_answer");
    const char* error = dlerror();
    if (error) {
        (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/IllegalStateException"), error);
        dlclose(lib);
        return -1;
    }
    int result = answer(35);
    dlclose(lib);
    return result;
}
