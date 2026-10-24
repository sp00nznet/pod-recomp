# =============================================================================
# POD Gold - static recompilation host (32-bit, MSVC x86)
#
#   py -3 tools/catalog.py   # -> work/functions.json   (pcrecomp disasm32)
#   py -3 run_lift.py        # -> src/recomp/gen/
#   build.cmd                # vcvarsall amd64_x86 + CMake + Ninja -> build/pod.exe
#   build\pod.exe --run
# =============================================================================
cmake_minimum_required(VERSION 3.20)
# The C runtime linked in (/MT): pod.exe runs on a PC without the x86 Visual
# C++ redistributable (netlab's test VMs have none).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(pod C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "the host must be 32-bit: use build.cmd (vcvarsall amd64_x86)")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "runtime/native32 is MSVC x86 inline assembly")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The toolkit: this checkout's own worktree (work/pcrecomp), else a sibling
# ../tools, which is where netlab puts the DEPS=pcrecomp=tools copy.
# tools/setup.ps1 passes -DPCRECOMP explicitly.
if(EXISTS "${CMAKE_SOURCE_DIR}/work/pcrecomp/runtime")
  set(_PCRECOMP_DEFAULT "${CMAKE_SOURCE_DIR}/work/pcrecomp")
else()
  set(_PCRECOMP_DEFAULT "${CMAKE_SOURCE_DIR}/../tools")
endif()
set(PCRECOMP "${_PCRECOMP_DEFAULT}" CACHE PATH "pcrecomp checkout")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty -- run `py -3 run_lift.py` first")
endif()

set(HOST_SOURCES src/runtime/host.c src/runtime/ddraw.c src/runtime/present.c
                 src/runtime/dinput.c src/runtime/indeo.c src/runtime/oracle.c)
add_executable(pod
  ${HOST_SOURCES}
  ${PCRECOMP}/runtime/native32/native32.c
  ${PCRECOMP}/runtime/recomp32/image_loader.c
  ${PCRECOMP}/runtime/recomp32/recomp_trace.c
  ${GEN_SOURCES}
)
target_include_directories(pod PRIVATE
  src/runtime src/recomp/gen ${PCRECOMP}/runtime/native32 ${PCRECOMP}/runtime/recomp32)
# RECOMP_RETADDR=0u: a stack imbalance during bring-up leaks a harmless 0, not
# 0xDEAD0000 into a size argument.
target_compile_definitions(pod PRIVATE RECOMP_RETADDR=0u _CRT_SECURE_NO_WARNINGS
                                       RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
# Linked high and not relocatable, so PODMMX.EXE's 0x00400000..0x0078D000 is
# free for the guest image.
target_link_options(pod PRIVATE /BASE:0x60000000 /FIXED:NO /DYNAMICBASE:NO
                                /NXCOMPAT /STACK:0x1000000 /MANIFESTUAC:NO
                                /MAP)   # build/pod.map: names a host address in a fault or watchdog report
set(GEN_OPT "/O2" CACHE STRING "optimization for the generated C")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;${GEN_OPT}")
set_source_files_properties(${HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "/W3")
target_link_libraries(pod PRIVATE user32 gdi32 kernel32 advapi32 winmm dwmapi
                                  d3d11 dxgi dxguid vfw32)

option(POD_TRACE "record a ring buffer of lifted function entries" OFF)
if(POD_TRACE)
  target_compile_definitions(pod PRIVATE RECOMP_TRACE=1)
endif()
