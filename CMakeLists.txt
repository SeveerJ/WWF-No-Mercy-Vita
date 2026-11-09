# WWF No Mercy: Recompiled - PS Vita port.
#
# Vita:   cmake -S . -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
# Linux:  cmake -S . -B build-host -G Ninja -DNM_KHR_DIR=<EGL/GLES2 headers and libs>   (test build, Mesa)
#
# NM_GAME_DIR    WWFNoMercyRecomp checkout with RecompiledFuncs/ generated and fixed up, and rsp/nomercy_audio.cpp
# NM_RUNTIME_DIR its lib/N64ModernRuntime (wcw branch) with runtime/runtime-vita-wcw.patch applied
cmake_minimum_required(VERSION 3.20)
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK} AND NOT NM_HOST)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "")
endif()
project(NoMercyVita C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(NM_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../WWFNoMercyRecomp" CACHE PATH "WWFNoMercyRecomp checkout")
set(NM_RUNTIME_DIR "${NM_GAME_DIR}/lib/N64ModernRuntime" CACHE PATH "N64ModernRuntime (wcw) with the Vita patch")

if (VITA)
    include("${VITASDK}/share/vita.cmake" REQUIRED)
    set(VITA_APP_NAME "WWF No Mercy")
    set(VITA_TITLEID  "WWFNM0001")
    set(VITA_VERSION  "00.01")
    set(NM_VITA_APP_DIR "ux0:data/NoMercyRecompiled")
    add_compile_definitions(NM_VITA_APP_DIR="${NM_VITA_APP_DIR}")
    add_compile_options("SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/vita/vita_compat.h")
    add_compile_options(-mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon)
endif()

# ---- Runtime (ultramodern + librecomp) ----
add_subdirectory("${NM_RUNTIME_DIR}" runtime EXCLUDE_FROM_ALL)

# ---- Recompiled game code ----
file(GLOB NM_FUNC_SOURCES "${NM_GAME_DIR}/RecompiledFuncs/*.c")
add_library(NoMercyFuncs STATIC ${NM_FUNC_SOURCES})
target_include_directories(NoMercyFuncs PRIVATE
    "${NM_RUNTIME_DIR}/N64Recomp/include"
    "${NM_RUNTIME_DIR}/ultramodern/include"
    "${NM_RUNTIME_DIR}/librecomp/include")
# The generated code calls a few runtime functions without prototypes (the PC build uses clang-cl).
target_compile_options(NoMercyFuncs PRIVATE -O3 -fno-strict-aliasing -w
    -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types)

# ---- App ----
set(NM_APP_SOURCES
    src/nm_main.cpp
    src/nm_settings.cpp
    src/nm_stubs.cpp
    src/nm_perf.cpp
    src/nm_hle_audio.cpp
    src/hle_audio/alist.c
    src/hle_audio/alist_naudio.c
    src/hle_audio/audio.c
    src/hle_audio/memory.c
    src/gles/n64_renderer.cpp
    "${NM_GAME_DIR}/src/main/register_overlays.cpp"
    "${NM_GAME_DIR}/rsp/nomercy_audio.cpp")
if (VITA)
    list(APPEND NM_APP_SOURCES vita/vita_backend.cpp vita/vita_mod_stubs.cpp)
    set(NM_EXE NoMercyRecompiled)
else()
    list(APPEND NM_APP_SOURCES headless/headless_backend.cpp)
    set(NM_EXE nm_headless)
    # The RSP vector unit uses SSE4.1 on x86 (NEON through sse2neon on the Vita).
    set_source_files_properties("${NM_GAME_DIR}/rsp/nomercy_audio.cpp" PROPERTIES COMPILE_OPTIONS "-msse4.1")
endif()
add_executable(${NM_EXE} ${NM_APP_SOURCES})
target_include_directories(${NM_EXE} PRIVATE
    src
    "${NM_GAME_DIR}/include"
    "${NM_GAME_DIR}"
    "${NM_RUNTIME_DIR}/librecomp/include/librecomp"
    "${NM_RUNTIME_DIR}/librecomp/src"
    "${NM_RUNTIME_DIR}/N64Recomp/include"
    "${NM_RUNTIME_DIR}/thirdparty/xxHash")
target_compile_options(${NM_EXE} PRIVATE -O3 -fno-strict-aliasing)

if (VITA)
    option(NM_VITA_LOG_TO_TTY "Send printf to the debug TTY instead of log.txt" OFF)
    if (NM_VITA_LOG_TO_TTY)
        set_source_files_properties(vita/vita_backend.cpp PROPERTIES COMPILE_DEFINITIONS NM_VITA_LOG_TO_TTY)
    endif()
    target_link_libraries(${NM_EXE} PRIVATE
        NoMercyFuncs librecomp ultramodern
        vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub SceGxm_stub
        SceCommonDialog_stub SceRazorCapture_stub
        SceDisplay_stub SceCtrl_stub SceTouch_stub SceAudio_stub ScePower_stub SceSysmem_stub SceAppMgr_stub SceKernelThreadMgr_stub
        -Wl,--whole-archive pthread -Wl,--no-whole-archive m stdc++)
    vita_create_self(eboot.bin ${NM_EXE} UNSAFE)
    vita_create_vpk(NoMercyRecompiled.vpk ${VITA_TITLEID} eboot.bin
        VERSION ${VITA_VERSION}
        NAME ${VITA_APP_NAME}
        FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/icon0.png sce_sys/icon0.png
        FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
        FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        FILE ${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
else()
    set(NM_KHR_DIR "" CACHE PATH "Folder with include/{EGL,KHR,GLES2} and lib/{libEGL,libGLESv2}.so")
    target_include_directories(${NM_EXE} PRIVATE "${NM_KHR_DIR}/include")
    target_link_directories(${NM_EXE} PRIVATE "${NM_KHR_DIR}/lib")
    target_link_options(${NM_EXE} PRIVATE -rdynamic)
    target_link_libraries(${NM_EXE} PRIVATE NoMercyFuncs librecomp ultramodern EGL GLESv2 pthread)
endif()
