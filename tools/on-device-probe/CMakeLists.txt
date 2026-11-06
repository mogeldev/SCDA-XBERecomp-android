cmake_minimum_required(VERSION 3.25)
project(compiler_probe LANGUAGES C)
add_library(probe SHARED probe.c)
target_link_libraries(probe PRIVATE dl)
add_executable(load_check load_check.c)
target_link_libraries(load_check PRIVATE dl)
set_target_properties(load_check PROPERTIES OUTPUT_NAME libprobe_load SUFFIX .so
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../out/on-device-probe/jniLibs/${ANDROID_ABI}")
