/* Shell Probe (TVIT00012): loads shellprobe.suprx into SceShell at run time.
 *
 * Nothing here touches ur0:tai. The module is copied from the package to
 * ux0:data/tailnet-probe/ (SceShell cannot see this app's app0:) and loaded
 * with taiHEN's taiLoadStartModuleForPid, which HENkaku allows for unsafe
 * homebrew. The module lives until the next reboot.
 *
 *   CROSS     load the probe into SceShell
 *   TRIANGLE  ask a running probe to stop (writes the stop file)
 *   CIRCLE    exit
 *
 * Every step is also appended to ux0:data/tailnet-probe/app.log.
 */
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <taihen.h>

#include "debugScreen.h"

#define DIR_PATH   "ux0:data/tailnet-probe"
#define MOD_SRC    "app0:shellprobe.suprx"
#define MOD_DST    DIR_PATH "/shellprobe.suprx"
#define STOP_PATH  DIR_PATH "/stop"
#define APPLOG     DIR_PATH "/app.log"

static void say(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    psvDebugScreenPrintf("%s\n", buf);
    SceUID fd = sceIoOpen(APPLOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, buf, strlen(buf));
        sceIoWrite(fd, "\n", 1);
        sceIoClose(fd);
    }
}

static int copy_file(const char *src, const char *dst)
{
    static char buf[16 * 1024];
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
            total = -1;
            break;
        }
        total += n;
    }
    sceIoClose(in);
    sceIoClose(out);
    return n < 0 ? n : total;
}

/* SceShell's pid, or < 0. The answer is checked by asking for its title. */
static SceUID find_shell_pid(void)
{
    static const char *names[] = { "NPXS19999", "main" };
    for (unsigned i = 0; i < 2; i++) {
        SceUID pid = -1;
        int rc = sceAppMgrGetIdByName(&pid, names[i]);
        char back[32] = { 0 };
        int rc2 = rc >= 0 ? sceAppMgrGetNameById(pid, back) : -1;
        say("GetIdByName(%s) rc=%08X pid=%08X name=%s (rc=%08X)", names[i],
            (unsigned)rc, (unsigned)pid, back, (unsigned)rc2);
        if (rc >= 0 && pid > 0 && pid != sceKernelGetProcessId())
            return pid;
    }
    return -1;
}

int main(void)
{
    psvDebugScreenInit();
    sceIoMkdir(DIR_PATH, 0777);
    say("--- Shell Probe (own pid %08X)", (unsigned)sceKernelGetProcessId());

    int n = copy_file(MOD_SRC, MOD_DST);
    say("copy module -> %s: %d bytes", MOD_DST, n);

    SceUID shell = find_shell_pid();
    if (shell < 0)
        say("could not find SceShell's pid; CROSS is disabled");

    say("");
    say("CROSS = load probe into SceShell   TRIANGLE = stop probe   CIRCLE = exit");

    int loaded = 0;
    unsigned prev = ~0u;
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof pad);
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = pad.buttons & ~prev;
        prev = pad.buttons;

        if (pressed & SCE_CTRL_CIRCLE)
            break;
        if (pressed & SCE_CTRL_TRIANGLE) {
            SceUID fd = sceIoOpen(STOP_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            if (fd >= 0)
                sceIoClose(fd);
            say("stop file written (%08X); the probe stops within 5 s", (unsigned)fd);
            loaded = 0;
        }
        if ((pressed & SCE_CTRL_CROSS) && shell >= 0 && n > 0) {
            if (loaded) {
                say("already loaded in this session; not loading twice");
            } else {
                sceIoRemove(STOP_PATH);
                say("loading into pid %08X ...", (unsigned)shell);
                SceUID mod = taiLoadStartModuleForPid(shell, MOD_DST, 0, NULL, 0);
                say("taiLoadStartModuleForPid = %08X (%s)", (unsigned)mod,
                    mod >= 0 ? "loaded; log: " DIR_PATH "/shellmem.log" : "FAILED");
                loaded = mod >= 0;
            }
        }
        sceKernelDelayThread(50 * 1000);
    }
    sceKernelExitProcess(0);
    return 0;
}
