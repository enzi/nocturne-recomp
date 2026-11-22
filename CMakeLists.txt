cmake_minimum_required(VERSION 3.28)
project(nocturne_recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
if(WIN32)
    find_package(OpenGL REQUIRED)
    set(OPENGL_TARGET OpenGL::GL)
else()
    find_package(OpenGL REQUIRED COMPONENTS OpenGL)
    set(OPENGL_TARGET OpenGL::OpenGL)
endif()

set(NOCTURNE_EXE "${CMAKE_SOURCE_DIR}/data/game/nocturne.exe" CACHE FILEPATH "Original nocturne.exe")
set(NOCTURNE_FUNCTIONS "${CMAKE_SOURCE_DIR}/recompiler/functions.txt" CACHE FILEPATH "Function list exported by tools/ghidra/ExportRecompInfo.java")
set(NOCTURNE_RECOMPILER "" CACHE FILEPATH "Recompiler built for the host, used instead of building one (for cross builds)")
set(RECOMP_REPLACED "${CMAKE_SOURCE_DIR}/patches/replaced.txt")
set(RECOMP_CHUNKS 16)

add_library(pe STATIC common/pe.cpp)
target_include_directories(pe PUBLIC common)

if(NOCTURNE_RECOMPILER)
    set(RECOMPILER "${NOCTURNE_RECOMPILER}")
else()
    find_package(zydis REQUIRED)
    add_executable(recompiler recompiler/main.cpp recompiler/translate.cpp)
    target_link_libraries(recompiler PRIVATE pe Zydis::Zydis)
    set(RECOMPILER recompiler)
endif()

set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(GEN_SOURCES "${GEN_DIR}/func_table.cpp")
math(EXPR LAST_CHUNK "${RECOMP_CHUNKS} - 1")
foreach(i RANGE 0 ${LAST_CHUNK})
    list(APPEND GEN_SOURCES "${GEN_DIR}/chunk_${i}.cpp")
endforeach()

add_custom_command(
    OUTPUT ${GEN_SOURCES} "${GEN_DIR}/funcs.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
    COMMAND ${RECOMPILER} "${NOCTURNE_EXE}" "${NOCTURNE_FUNCTIONS}" "${RECOMP_REPLACED}" "${GEN_DIR}" ${RECOMP_CHUNKS}
    DEPENDS ${RECOMPILER} "${NOCTURNE_EXE}" "${NOCTURNE_FUNCTIONS}" "${RECOMP_REPLACED}"
    COMMENT "Recompiling nocturne.exe"
    VERBATIM)

set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable;$<$<CXX_COMPILER_ID:Clang>:-Wno-gnu-anonymous-struct;-Wno-nested-anon-types>")

add_library(scenebake STATIC
    runtime/bake.cpp
    runtime/gl.cpp
    runtime/projection.cpp
    runtime/scene.cpp
    third_party/xatlas/xatlas.cpp)
target_include_directories(scenebake PUBLIC runtime third_party)
target_link_libraries(scenebake PUBLIC Threads::Threads SDL3::SDL3 ${OPENGL_TARGET})

add_executable(nocbake tools/nocbake.cpp)
target_link_libraries(nocbake PRIVATE scenebake)

add_executable(nocturne
    runtime/advapi32.cpp
    runtime/ddraw.cpp
    runtime/display.cpp
    runtime/dsound.cpp
    runtime/gdi32.cpp
    runtime/guest.cpp
    runtime/hle.cpp
    runtime/kernel32.cpp
    runtime/main.cpp
    runtime/options.cpp
    runtime/patches.cpp
    runtime/renderer.cpp
    runtime/runtime.cpp
    runtime/user32.cpp
    runtime/win32.cpp
    runtime/winmm.cpp
    runtime/world3d.cpp
    runtime/wsock32.cpp
    ${GEN_SOURCES})
target_include_directories(nocturne PRIVATE runtime third_party "${GEN_DIR}")
target_link_libraries(nocturne PRIVATE pe scenebake)

if(MINGW)
    target_link_options(nocturne PRIVATE -static -Wl,--stack,8388608)
    target_link_options(nocbake PRIVATE -static)
    add_custom_command(TARGET nocturne POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:nocturne> $<TARGET_FILE_DIR:nocturne>
        COMMAND_EXPAND_LISTS)
endif()
