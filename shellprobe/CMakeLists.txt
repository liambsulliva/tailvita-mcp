cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(shellprobe C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(SAMPLES "${VITASDK}/share/gcc-arm-vita-eabi/samples/common")

# --- the module that runs inside SceShell: no libc, non-weak imports so a
# missing import fails the load cleanly instead of crashing the shell later.
add_executable(shellprobe module/probe.c)
target_compile_options(shellprobe PRIVATE -Wall -O2 -fno-pic -fno-PIC -std=gnu99)
target_link_options(shellprobe PRIVATE -Wl,-q -nostdlib)
target_link_libraries(shellprobe
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceIofilemgr_stub
  SceSysmem_stub
  SceAppMgr_stub
  SceRtc_stub
  SceProcessmgr_stub
  gcc)

# --- the app that loads it (unsafe homebrew: taiHEN's user API needs it).
add_executable(shellprobe_app app/main.c ${SAMPLES}/debugScreen.c)
target_include_directories(shellprobe_app PRIVATE ${SAMPLES})
target_compile_options(shellprobe_app PRIVATE -Wall -O2 -std=gnu11)
target_link_libraries(shellprobe_app
  taihen_stub
  SceDisplay_stub
  SceCtrl_stub
  SceAppMgr_stub)
vita_create_self(shellprobe_app.self shellprobe_app UNSAFE)
# After the app's: vita.cmake's CONFIG argument leaks into later calls.
vita_create_self(shellprobe.suprx shellprobe
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/module/shellprobe.yml
  UNSAFE)
add_dependencies(shellprobe_app.self-self shellprobe.suprx-self)
vita_create_vpk(shellprobe.vpk "TVIT00012" shellprobe_app.self
  VERSION "01.00"
  NAME "Shell Probe"
  FILE ${CMAKE_CURRENT_BINARY_DIR}/shellprobe.suprx shellprobe.suprx)
