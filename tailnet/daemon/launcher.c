/* Tailnet Daemon launcher (eboot.bin, the visible bubble).
 *
 * Opened normally, it starts the background service (eboot2.bin in this
 * package: the tailnet listener built with the `daemon` feature) and exits.
 * The service keeps running with no app open.
 *
 * Opened with L held, it stops the service and tells the watchdog to leave
 * it stopped.
 *
 * Before starting the service it loads tailnet_shell.suprx into SceShell
 * (shell_module.c). That module lets the service start while an
 * enlarged-memory game is running, and starts it again when the system ends
 * it. It also loads tailnet_capture.skprx (capture_kernel.c), the kernel
 * helper the service's screenshot port reads the screen through. Both are
 * loaded at run time with taiHEN; nothing is written under ur0:tai, and both
 * are gone after a reboot.
 *
 * There is no screen output. Each step goes to
 * ux0:data/tailnet-listener/launcher.log.
 */
#include <psp2/appmgr.h>
#include <psp2/bgapputil.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include <taihen.h>

#include "capture.h"

#define DIR_PATH "ux0:data/tailnet-listener"
#define LOG_PATH DIR_PATH "/launcher.log"
#define MOD_SRC  "app0:tailnet_shell.suprx"
#define MOD_DST  DIR_PATH "/tailnet_shell.suprx"
/* Leave SceShell alone when this file exists. */
#define NO_HOOK  DIR_PATH "/no-shell-module"
#define CAP_SRC  "app0:tailnet_capture.skprx"
#define CAP_DST  DIR_PATH "/tailnet_capture.skprx"
/* Written once the kernel helper is loaded: "<boot time> <SceShell pid>".
 * The service offers screenshots only while it exists. */
#define CAP_READY DIR_PATH "/capture-ready"
/* Leave the kernel alone when this file exists. */
#define NO_CAPTURE DIR_PATH "/no-capture"
/* While this file exists the SceShell module restarts a service that died. */
#define KEEP_PATH DIR_PATH "/keepalive"
/* The service exits when it sees this file. */
#define STOP_PATH DIR_PATH "/stop"

static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    sceClibVsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    sceIoWrite(fd, line, sceClibStrnlen(line, sizeof line));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

static int copy_file(const char *src, const char *dst)
{
    static char buf[8 * 1024];
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (in < 0)
        return in;
    SceUID out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) {
        sceIoClose(in);
        return out;
    }
    int total = 0, n;
    while ((n = sceIoRead(in, buf, sizeof buf)) > 0) {
        if (sceIoWrite(out, buf, n) != n) {
            n = -1;
            break;
        }
        total += n;
    }
    sceIoClose(in);
    sceIoClose(out);
    return n < 0 ? n : total;
}

/* SceShell's pid, or < 0. Checked by asking for its title back. */
static SceUID find_shell(void)
{
    SceUID shell = -1;
    char name[32] = { 0 };
    int rc = sceAppMgrGetIdByName(&shell, "NPXS19999");
    if (rc < 0 || shell <= 0 || sceAppMgrGetNameById(shell, name) < 0 ||
        sceClibStrncmp(name, "NPXS19999", 10) != 0) {
        say("launcher: SceShell pid not found (rc=%08X pid=%08X name=%s)", (unsigned)rc,
            (unsigned)shell, name);
        return -1;
    }
    return shell;
}

static void load_shell_module(SceUID shell)
{
    SceIoStat st;
    if (sceIoGetstat(NO_HOOK, &st) >= 0) {
        say("launcher: %s exists; SceShell left alone", NO_HOOK);
        return;
    }
    int n = copy_file(MOD_SRC, MOD_DST);
    if (n <= 0) {
        say("launcher: copying the shell module failed (%08X)", (unsigned)n);
        return;
    }
    SceUID mod = taiLoadStartModuleForPid(shell, MOD_DST, 0, NULL, 0);
    say("launcher: shell module into SceShell pid %08X -> %08X", (unsigned)shell, (unsigned)mod);
}

/* When this boot started, in seconds on the RTC's scale. */
static unsigned long long boot_seconds(void)
{
    SceRtcTick now = { 0 };
    sceRtcGetCurrentTick(&now);
    return (now.tick - sceKernelGetSystemTimeWide()) / 1000000ULL;
}

/* True if CAP_READY was written during this boot: the same SceShell pid and
 * a boot time within a few seconds. A kernel module must not be loaded
 * twice, and nothing else says whether ours is already there. */
static int capture_ready_this_boot(SceUID shell)
{
    char buf[64] = { 0 };
    SceUID fd = sceIoOpen(CAP_READY, SCE_O_RDONLY, 0);
    if (fd < 0)
        return 0;
    int n = sceIoRead(fd, buf, sizeof buf - 1);
    sceIoClose(fd);
    if (n <= 0)
        return 0;
    unsigned long long boot = 0;
    unsigned pid = 0;
    const char *p = buf;
    while (*p >= '0' && *p <= '9')
        boot = boot * 10 + (unsigned)(*p++ - '0');
    while (*p == ' ')
        p++;
    while (*p >= '0' && *p <= '9')
        pid = pid * 10 + (unsigned)(*p++ - '0');
    unsigned long long now = boot_seconds();
    unsigned long long apart = now > boot ? now - boot : boot - now;
    return pid == (unsigned)shell && apart <= 5;
}

static void load_capture_module(SceUID shell)
{
    SceIoStat st;
    if (sceIoGetstat(NO_CAPTURE, &st) >= 0) {
        sceIoRemove(CAP_READY);
        say("launcher: %s exists; kernel helper not loaded", NO_CAPTURE);
        return;
    }
    if (capture_ready_this_boot(shell)) {
        say("launcher: kernel helper already loaded this boot");
        return;
    }
    sceIoRemove(CAP_READY);
    int n = copy_file(CAP_SRC, CAP_DST);
    if (n <= 0) {
        say("launcher: copying the kernel helper failed (%08X)", (unsigned)n);
        return;
    }
    SceUID mod = taiLoadStartKernelModule(CAP_DST, 0, NULL, 0);
    say("launcher: kernel helper -> %08X", (unsigned)mod);
    if (mod < 0)
        return;

    /* Self-test before anyone relies on it: one snapshot of this screen. */
    int ver = tnCaptureGetApiVersion();
    tn_capture_info info = { 0 };
    int rc = ver == TN_CAPTURE_API_VERSION ? tnCaptureSnapshot(&info) : -100;
    static unsigned char row[TN_CAPTURE_MAX_PITCH * 4];
    int got = rc == 0 ? tnCaptureRead(info.height / 2, 1, row, sizeof row) : -100;
    tnCaptureRelease();
    say("launcher: capture self-test version=%d snapshot=%d %ux%u pitch=%u owner=%08X row=%d "
        "pixel=%02X%02X%02X",
        ver, rc, (unsigned)info.width, (unsigned)info.height, (unsigned)info.pitch,
        (unsigned)info.pid, got, row[0], row[1], row[2]);
    if (ver != TN_CAPTURE_API_VERSION)
        return;

    /* "<boot seconds> <pid>\n"; the boot time is written digit by digit
     * because it does not fit the 32-bit formats. */
    char digits[24];
    int nd = 0;
    unsigned long long boot = boot_seconds();
    do {
        digits[nd++] = (char)('0' + boot % 10);
        boot /= 10;
    } while (boot && nd < (int)sizeof digits);
    char buf[64];
    int len = 0;
    while (nd > 0)
        buf[len++] = digits[--nd];
    len += sceClibSnprintf(buf + len, sizeof buf - len, " %u\n", (unsigned)shell);
    SceUID fd = sceIoOpen(CAP_READY, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
}

static void touch(const char *path)
{
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0)
        sceIoClose(fd);
}

static int l_held(void)
{
    SceCtrlData pad;
    /* The first reads after start can be empty; sample for a quarter second. */
    for (int i = 0; i < 5; i++) {
        sceClibMemset(&pad, 0, sizeof pad);
        if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 && (pad.buttons & SCE_CTRL_LTRIGGER))
            return 1;
        sceKernelDelayThread(50 * 1000);
    }
    return 0;
}

int main(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DIR_PATH, 0777);
    say("launcher: start (uptime %us)", (unsigned)(sceKernelGetSystemTimeWide() / 1000000ULL));

    if (l_held()) {
        sceIoRemove(KEEP_PATH);
        touch(STOP_PATH);
        say("launcher: L held; service told to stop, restarts off");
        sceKernelDelayThread(500 * 1000);
        sceKernelExitProcess(0);
        return 0;
    }

    sceIoRemove(STOP_PATH);
    SceUID shell = find_shell();
    if (shell >= 0) {
        load_shell_module(shell);
        load_capture_module(shell);
    }

    int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_BG_APP_UTIL);
    say("launcher: load BG_APP_UTIL -> %08X", (unsigned)rc);
    /* BGFTP passes 0; VitaSDK's header says 1. 0 works on firmware 3.65. */
    rc = sceBgAppUtilStartBgApp(0);
    say("launcher: StartBgApp(0) -> %08X", (unsigned)rc);
    if (rc < 0) {
        rc = sceBgAppUtilStartBgApp(1);
        say("launcher: StartBgApp(1) -> %08X", (unsigned)rc);
    }
    touch(KEEP_PATH);
    say("launcher: exit");
    sceKernelDelayThread(500 * 1000);
    sceKernelExitProcess(0);
    return 0;
}
