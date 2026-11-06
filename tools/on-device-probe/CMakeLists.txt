cmake_minimum_required(VERSION 3.25)
project(compiler_probe LANGUAGES C)
add_library(probe SHARED probe.c)
target_link_libraries(probe PRIVATE dl)
