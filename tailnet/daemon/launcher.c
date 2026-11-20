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
 * it. The module is loaded at run time with taiHEN; nothing is written under
 * ur0:tai.
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
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include <taihen.h>

#define DIR_PATH "ux0:data/tailnet-listener"
#define LOG_PATH DIR_PATH "/launcher.log"
#define MOD_SRC  "app0:tailnet_shell.suprx"
#define MOD_DST  DIR_PATH "/tailnet_shell.suprx"
/* Leave SceShell alone when this file exists. */
#define NO_HOOK  DIR_PATH "/no-shell-module"
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

static void load_shell_module(void)
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
    SceUID shell = -1;
    char name[32] = { 0 };
    int rc = sceAppMgrGetIdByName(&shell, "NPXS19999");
    if (rc < 0 || shell <= 0 || sceAppMgrGetNameById(shell, name) < 0 ||
        sceClibStrncmp(name, "NPXS19999", 10) != 0) {
        say("launcher: SceShell pid not found (rc=%08X pid=%08X name=%s)", (unsigned)rc,
            (unsigned)shell, name);
        return;
    }
    SceUID mod = taiLoadStartModuleForPid(shell, MOD_DST, 0, NULL, 0);
    say("launcher: shell module into SceShell pid %08X -> %08X", (unsigned)shell, (unsigned)mod);
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
    load_shell_module();

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
