// libscda_gpuprobe.so: the launcher's Turnip eligibility check, the same one the game uses (turnip_detect.h).
#include <jni.h>

#include "android/turnip_detect.h"

extern "C" JNIEXPORT jboolean JNICALL Java_io_github_mogeldev_scda_DriverChoice_nativeSupported(JNIEnv*, jclass) {
    return turnip::Supported();
}

extern "C" JNIEXPORT jint JNICALL Java_io_github_mogeldev_scda_DriverChoice_nativeChipId(JNIEnv*, jclass) {
    return jint(turnip::ChipId());
}
