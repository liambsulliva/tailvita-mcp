/* Tailnet Daemon launcher (eboot.bin, the visible bubble).
 *
 * Starts the background service (eboot2.bin in this package: the tailnet
 * listener built with the `daemon` feature) and exits. The service keeps
 * running with no app open, until its LiveArea card is peeled off.
 *
 * Before that it loads tailnet_lowmem.suprx into SceShell, so the service
 * can also be started while an enlarged-memory game is running: starting
 * such a game ends every background service, and without the hook SceShell
 * refuses to start one again beside it. The module is loaded at run time
 * with taiHEN; nothing is written under ur0:tai.
 *
 * There is no screen output. Each step goes to
 * ux0:data/tailnet-listener/launcher.log.
 */
#include <psp2/appmgr.h>
#include <psp2/bgapputil.h>
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
#define MOD_SRC  "app0:tailnet_lowmem.suprx"
#define MOD_DST  DIR_PATH "/tailnet_lowmem.suprx"
/* Leave SceShell alone when this file exists. */
#define NO_HOOK  DIR_PATH "/no-lowmem"

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

static void load_lowmem_hook(void)
{
    SceIoStat st;
    if (sceIoGetstat(NO_HOOK, &st) >= 0) {
        say("launcher: %s exists; SceShell left alone", NO_HOOK);
        return;
    }
    int n = copy_file(MOD_SRC, MOD_DST);
    if (n <= 0) {
        say("launcher: copying the hook module failed (%08X)", (unsigned)n);
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
    say("launcher: hook module into SceShell pid %08X -> %08X", (unsigned)shell, (unsigned)mod);
}

int main(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DIR_PATH, 0777);
    say("launcher: start (uptime %us)", (unsigned)(sceKernelGetSystemTimeWide() / 1000000ULL));

    load_lowmem_hook();

    int rc = sceSysmoduleLoadModule(SCE_SYSMODULE_BG_APP_UTIL);
    say("launcher: load BG_APP_UTIL -> %08X", (unsigned)rc);
    /* BGFTP passes 0; VitaSDK's header says 1. Upstream's spike found 0 works. */
    rc = sceBgAppUtilStartBgApp(0);
    say("launcher: StartBgApp(0) -> %08X", (unsigned)rc);
    if (rc < 0) {
        rc = sceBgAppUtilStartBgApp(1);
        say("launcher: StartBgApp(1) -> %08X", (unsigned)rc);
    }
    say("launcher: exit");
    sceKernelDelayThread(500 * 1000);
    sceKernelExitProcess(0);
    return 0;
}
