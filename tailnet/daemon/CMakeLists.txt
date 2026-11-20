cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(tailnet_daemon_launcher C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# The launcher bubble. Like BGFTP's, it is a system-mode app with a 4 MiB
# budget, so it can open beside a running game.
add_executable(launcher launcher.c)
target_compile_options(launcher PRIVATE -Wall -O2 -std=gnu11)
target_link_libraries(launcher
  taihen_stub
  SceBgAppUtil_stub
  SceAppMgr_stub
  SceSysmodule_stub)
vita_create_self(eboot.bin launcher UNSAFE MEMSIZE 4096)

# The SceShell hook. After the launcher's SELF: vita.cmake's CONFIG argument
# leaks into later vita_create_self calls.
add_executable(tailnet_lowmem lowmem.c)
target_compile_options(tailnet_lowmem PRIVATE -Wall -O2 -fno-pic -fno-PIC -std=gnu99)
target_link_options(tailnet_lowmem PRIVATE -Wl,-q -nostdlib)
target_link_libraries(tailnet_lowmem
  taihen_stub
  SceLibKernel_stub
  SceKernelModulemgr_stub
  SceIofilemgr_stub
  gcc)
vita_create_self(tailnet_lowmem.suprx tailnet_lowmem
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/tailnet_lowmem.yml
  UNSAFE)
