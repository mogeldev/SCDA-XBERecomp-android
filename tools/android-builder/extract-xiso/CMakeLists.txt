cmake_minimum_required(VERSION 3.25)
project(scda_extract_xiso LANGUAGES C)

include(FetchContent)
FetchContent_Declare(extract_xiso
    GIT_REPOSITORY https://github.com/xboxdev/extract-xiso.git
    GIT_TAG 3f5b62cfe68f000b0e3c8a30104973f3a297948e
    SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(extract_xiso)

add_executable(extract-xiso "${extract_xiso_SOURCE_DIR}/extract-xiso.c")
target_compile_definitions(extract-xiso PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
# No extra compiler runtime DLLs for players.
set_property(TARGET extract-xiso PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
configure_file("${extract_xiso_SOURCE_DIR}/LICENSE.TXT" "${CMAKE_CURRENT_BINARY_DIR}/extract-xiso-LICENSE.txt" COPYONLY)
