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
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include <taihen.h>

#define DIR_PATH "ux0:data/tailnet-listener"
#define LOG_PATH DIR_PATH "/launcher.log"
#define MOD_SRC  "app0:tailnet_shell.suprx"
#define MOD_DST  DIR_PATH "/tailnet_shell.suprx"
/* Leave SceShell alone when this file exists. */
#define NO_HOOK  DIR_PATH "/no-shell-module"
#define CAP_SRC  "app0:tailnet_capture.skprx"
#define CAP_DST  DIR_PATH "/tailnet_capture.skprx"
/* Written once the kernel helper is loaded; holds SceShell's pid. The 2
 * matches the helper's interface generation (tailnet_capture.yml). The
 * service offers screenshots only while it exists. The SceShell module
 * removes it on its first start of a boot. */
#define CAP_READY DIR_PATH "/capture2-ready"
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

/* True only when the module was loaded by this call and stayed: the first
 * time the bubble is opened after a boot. A copy that finds itself already
 * there answers 80024501, as does one that could not install its hook. */
static int load_shell_module(SceUID shell)
{
    SceIoStat st;
    /* Without the shell module nothing clears a marker left by an earlier boot, and
     * the service would call a helper that is not there. Dropping it only turns
     * screenshots off. */
    if (sceIoGetstat(NO_HOOK, &st) >= 0) {
        say("launcher: %s exists; SceShell left alone", NO_HOOK);
        sceIoRemove(CAP_READY);
        return 0;
    }
    int n = copy_file(MOD_SRC, MOD_DST);
    if (n <= 0) {
        say("launcher: copying the shell module failed (%08X)", (unsigned)n);
        sceIoRemove(CAP_READY);
        return 0;
    }
    SceUID mod = taiLoadStartModuleForPid(shell, MOD_DST, 0, NULL, 0);
    say("launcher: shell module into SceShell pid %08X -> %08X", (unsigned)shell, (unsigned)mod);
    return mod >= 0;
}

/* Loads the kernel helper, once per boot.
 *
 * A kernel module must not be loaded twice. On 2026-11-21 a second load was
 * refused with 8002D013 and the service then failed to start; a third was
 * followed by the Vita shutting down. Nothing in user mode says whether the
 * helper is already there, so the load is tied to the one event that happens
 * once per boot: the SceShell module being loaded for the first time
 * (`first_open`). An earlier version compared boot times instead, which
 * drifts by however long the Vita has slept. */
static void load_capture_module(SceUID shell, int first_open)
{
    SceIoStat st;
    if (sceIoGetstat(NO_CAPTURE, &st) >= 0) {
        say("launcher: %s exists; kernel helper not loaded", NO_CAPTURE);
        return;
    }
    if (!first_open) {
        say("launcher: not the first open of this boot; kernel helper left as it is (%s)",
            sceIoGetstat(CAP_READY, &st) >= 0 ? "loaded" : "not loaded");
        return;
    }
    /* A second guard that does not rest on taiLoadStartModuleForPid's answer for a
     * copy that returned NO_RESIDENT (never checked on the Vita). The shell module
     * removes the marker on its first load of a boot, before this runs, so a marker
     * still here means the helper was loaded earlier in this boot. */
    if (sceIoGetstat(CAP_READY, &st) >= 0) {
        say("launcher: %s exists, so the helper is already loaded this boot; not loading it again",
            CAP_READY);
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

    /* Record it at once. This process cannot call the helper itself, because
     * its imports were bound when it started, before the helper existed; the
     * service starts afterwards and checks the helper's version before
     * offering screenshots. */
    char buf[32];
    int len = sceClibSnprintf(buf, sizeof buf, "%u\n", (unsigned)shell);
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
    if (shell >= 0)
        load_capture_module(shell, load_shell_module(shell));

    int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_BG_APP_UTIL);
    say("launcher: load BG_APP_UTIL -> %08X", (unsigned)rc);
    /* VitaSDK's header says the mode must be 1, but 0 is what works on
     * firmware 3.65; 1 is tried only if 0 is refused. */
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
