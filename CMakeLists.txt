# POD Gold - Static Recompilation
#
# Phase 2 (compile) is subsumed here: this builds the runtime + import bridges +
# all generated lifted code into a single host executable that maps PODMMX.EXE
# and runs it.
#
#   1. py -3.11 run_lift.py        # generate src/recomp/gen/   (Phase 3 lifter)
#   2. py -3.11 gen_imports.py     # generate src/runtime/imports_gen.c
#   3. cmake -B build -A Win32 && cmake --build build --config Debug
#   4. build/Debug/pod_recomp.exe analysis/PODMMX.EXE --run
cmake_minimum_required(VERSION 3.20)
project(pod_recomp C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Build as 32-bit x86: cmake -B build -A Win32")
endif()

set(CMAKE_C_STANDARD 11)

file(GLOB RECOMP_GENERATED_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/recomp_*.c")
if(NOT RECOMP_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated sources. Run: py -3.11 run_lift.py")
endif()

add_executable(pod_recomp
    src/runtime/recomp_runtime.c
    src/runtime/imports_gen.c
    src/runtime/shims_impl.c
    src/runtime/ddraw_shim.c
    src/runtime/thread_shim.c
    ${RECOMP_GENERATED_SOURCES}
)

target_include_directories(pod_recomp PRIVATE src/recomp src/recomp/gen src/runtime)

# Benign (0) dummy return address so a stray stack leak can't become a huge size
# argument; RECOMP_TRACE gives a function-entry backtrace on crash during bring-up.
target_compile_definitions(pod_recomp PRIVATE RECOMP_RETADDR=0u RECOMP_TRACE)

if(MSVC)
    # Generated code is intentionally not warning-clean; /bigobj for huge TUs.
    target_compile_options(pod_recomp PRIVATE /w /bigobj /Od)
    # The host process must leave the guest VA range [0x00400000 ..] free so the
    # loader can map it fixed (g_mem_base == 0): link at a high base, ASLR off.
    target_link_options(pod_recomp PRIVATE /BASE:0x70000000 /DYNAMICBASE:NO /FIXED)
    target_link_libraries(pod_recomp PRIVATE gdi32 user32 winmm)
endif()
