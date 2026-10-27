cmake_minimum_required(VERSION 3.16)
project(rr2 CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
add_subdirectory(externals/dynarmic EXCLUDE_FROM_ALL)

set(SOURCES src/loader.cpp src/cpu.cpp src/table.cpp src/libc.cpp src/patches.cpp src/input.cpp src/main.cpp src/save.cpp src/obb.cpp src/lab.cpp)
if(ANDROID)
    add_library(RecklessRacing2 SHARED ${SOURCES} src/jni.cpp src/gl.cpp src/overlay.cpp)
    target_link_libraries(RecklessRacing2 PRIVATE dynarmic log GLESv2)
    target_link_options(RecklessRacing2 PRIVATE -s -Wl,-z,max-page-size=16384)  # 16 KB-page phones
else()
    add_executable(rr2_host ${SOURCES})  # PC test harness
    target_link_libraries(rr2_host PRIVATE dynarmic)
endif()
