cmake_minimum_required(VERSION 3.25)
project(scda_android_xiso LANGUAGES C)
include(FetchContent)
FetchContent_Declare(extract_xiso
    GIT_REPOSITORY https://github.com/xboxdev/extract-xiso.git
    GIT_TAG 3f5b62cfe68f000b0e3c8a30104973f3a297948e
    SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(extract_xiso)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/harden.py"
    "${extract_xiso_SOURCE_DIR}/extract-xiso.c" "${CMAKE_CURRENT_BINARY_DIR}/extract-xiso.c"
    COMMAND_ERROR_IS_FATAL ANY)
configure_file(wrapper.c "${CMAKE_CURRENT_BINARY_DIR}/wrapper.c" COPYONLY)
configure_file("${extract_xiso_SOURCE_DIR}/LICENSE.TXT" "${CMAKE_CURRENT_BINARY_DIR}/extract-xiso.txt" COPYONLY)
add_executable(extract_xiso "${CMAKE_CURRENT_BINARY_DIR}/wrapper.c")
set_target_properties(extract_xiso PROPERTIES OUTPUT_NAME libscda_xiso.so)
target_compile_definitions(extract_xiso PRIVATE _FILE_OFFSET_BITS=64 _GNU_SOURCE __LINUX__)
target_compile_options(extract_xiso PRIVATE -fstack-protector-strong)
target_link_options(extract_xiso PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,now -Wl,-z,relro)

# Static readers for ZIP, 7z, RAR/RAR5 and tar; never use external filter programs.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
foreach(option XZ_NLS XZ_TOOL_XZ XZ_TOOL_XZDEC XZ_TOOL_LZMADEC XZ_TOOL_LZMAINFO XZ_DOC)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(xz GIT_REPOSITORY https://github.com/tukaani-project/xz.git
    GIT_TAG 3d078b52adbff566ccfc51067dfbf742ecf3ef86)
FetchContent_MakeAvailable(xz)
# FindLibLZMA's link checks cannot link an in-tree target until build time.
set(LIBLZMA_INCLUDE_DIR "${xz_SOURCE_DIR}/src/liblzma/api" CACHE PATH "" FORCE)
set(LIBLZMA_LIBRARY liblzma CACHE STRING "" FORCE)
foreach(check LIBLZMA_HAS_AUTO_DECODER LIBLZMA_HAS_EASY_ENCODER LIBLZMA_HAS_LZMA_PRESET)
    set(${check} TRUE CACHE BOOL "" FORCE)
endforeach()
foreach(option ENABLE_OPENSSL ENABLE_LIBB2 ENABLE_LZ4 ENABLE_LZO ENABLE_ZSTD ENABLE_LIBXML2
        ENABLE_EXPAT ENABLE_PCREPOSIX ENABLE_PCRE2POSIX ENABLE_CNG ENABLE_TAR ENABLE_CPIO
        ENABLE_CAT ENABLE_UNZIP ENABLE_XATTR ENABLE_ACL ENABLE_ICONV ENABLE_TEST ENABLE_INSTALL)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(ENABLE_LZMA ON CACHE BOOL "" FORCE)
set(ENABLE_ZLIB ON CACHE BOOL "" FORCE)
FetchContent_Declare(bzip2 GIT_REPOSITORY https://gitlab.com/bzip2/bzip2.git
    GIT_TAG 6a8690fc8d26c815e798c588f796eabe9d684cf0 SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(bzip2)
add_library(bz2 STATIC "${bzip2_SOURCE_DIR}/blocksort.c" "${bzip2_SOURCE_DIR}/huffman.c"
    "${bzip2_SOURCE_DIR}/crctable.c" "${bzip2_SOURCE_DIR}/randtable.c" "${bzip2_SOURCE_DIR}/compress.c"
    "${bzip2_SOURCE_DIR}/decompress.c" "${bzip2_SOURCE_DIR}/bzlib.c")
set(BZIP2_INCLUDE_DIR "${bzip2_SOURCE_DIR}" CACHE PATH "" FORCE)
set(BZIP2_LIBRARY_RELEASE bz2 CACHE STRING "" FORCE)
set(BZIP2_NEED_PREFIX TRUE CACHE BOOL "" FORCE)
set(ENABLE_BZip2 ON CACHE BOOL "" FORCE)
FetchContent_Declare(libarchive GIT_REPOSITORY https://github.com/libarchive/libarchive.git
    GIT_TAG 27cbc7827172698143e440801fc0ba39ccb4f1f5)
FetchContent_MakeAvailable(libarchive)
add_executable(game_archive archive.c)
set_target_properties(game_archive PROPERTIES OUTPUT_NAME libscda_archive.so)
target_include_directories(game_archive PRIVATE "${libarchive_SOURCE_DIR}/libarchive")
target_link_libraries(game_archive PRIVATE archive_static liblzma bz2 z)
target_compile_options(game_archive PRIVATE -fstack-protector-strong)
target_link_options(game_archive PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,now -Wl,-z,relro)
configure_file("${libarchive_SOURCE_DIR}/COPYING" "${CMAKE_CURRENT_BINARY_DIR}/libarchive.txt" COPYONLY)
configure_file("${xz_SOURCE_DIR}/COPYING" "${CMAKE_CURRENT_BINARY_DIR}/xz.txt" COPYONLY)
configure_file("${xz_SOURCE_DIR}/COPYING.0BSD" "${CMAKE_CURRENT_BINARY_DIR}/xz-0BSD.txt" COPYONLY)
configure_file("${bzip2_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/bzip2.txt" COPYONLY)
