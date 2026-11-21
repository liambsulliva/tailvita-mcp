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

# The launcher bubble. It is a system-mode app with a 4 MiB
# budget, so it can open beside a running game.
add_executable(launcher launcher.c)
target_compile_options(launcher PRIVATE -Wall -O2 -std=gnu11)
target_link_libraries(launcher
  taihen_stub
  SceBgAppUtil_stub
  SceAppMgr_stub
  SceCtrl_stub
  SceSysmodule_stub)
vita_create_self(eboot.bin launcher UNSAFE MEMSIZE 4096)

# The SceShell hook. After the launcher's SELF: vita.cmake's CONFIG argument
# leaks into later vita_create_self calls.
add_executable(tailnet_shell shell_module.c)
target_compile_options(tailnet_shell PRIVATE -Wall -O2 -fno-pic -fno-PIC -std=gnu99)
target_link_options(tailnet_shell PRIVATE -Wl,-q -nostdlib)
target_link_libraries(tailnet_shell
  taihen_stub
  SceLibKernel_stub
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceIofilemgr_stub
  SceAppMgr_stub
  gcc)
vita_create_self(tailnet_shell.suprx tailnet_shell
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/tailnet_shell.yml
  UNSAFE)

# The kernel helper that reads the frame on screen (capture_kernel.c), and the
# stub library the service links.
# The 2 in the module and library names is the interface generation. A kernel
# module cannot be replaced without a reboot, so an incompatible interface
# gets new names and can be loaded beside an old copy that is still resident.
# (The yml cannot carry this note: vita.cmake does not accept comments there.)
add_executable(tailnet_capture2 capture_kernel.c)
target_compile_definitions(tailnet_capture2 PRIVATE __VITA_KERNEL__)
# -mgeneral-regs-only: no NEON or VFP registers in kernel code; the compiler
# otherwise uses them for plain struct copies.
target_compile_options(tailnet_capture2 PRIVATE -Wall -O2 -fno-pic -fno-PIC -std=gnu99 -mgeneral-regs-only)
target_link_options(tailnet_capture2 PRIVATE -Wl,-q -nostdlib)
target_link_libraries(tailnet_capture2
  SceDisplayForDriver_stub
  SceSysmemForDriver_stub
  SceThreadmgrForDriver_stub
  SceCpuForDriver_stub
  gcc)
vita_create_self(tailnet_capture.skprx tailnet_capture2
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/tailnet_capture.yml
  UNSAFE)
vita_create_stubs(tailnet_capture_stubs tailnet_capture2
  ${CMAKE_CURRENT_SOURCE_DIR}/tailnet_capture.yml KERNEL)
