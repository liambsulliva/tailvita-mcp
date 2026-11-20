/* tailnet_shell.suprx: the part of the Tailnet Daemon that lives in SceShell.
 *
 * 1. Launch hook. SceShell refuses to launch another app while a game that
 *    asked for extra memory (+29, +77 or +109 MiB) is running. The hook makes
 *    that budget check answer "allowed". It is the hook GrapheneCt's
 *    LowMemMode plugin installs: the same function offsets, chosen by
 *    SceShell's module NID, and the same replacement (run the original, then
 *    return 0). An unknown SceShell build is left alone.
 *
 * 2. Watchdog. Starting such a game ends every background service, and the
 *    system may end one again when memory runs short. While KEEP_PATH exists,
 *    one small thread checks every POLL_SEC seconds whether the service is
 *    running and starts it again after it has been gone for GONE_POLLS
 *    checks. It gives up after MAX_FAILS failed starts in a row, or after
 *    MAX_BLIND starts with the service never once seen running (in case the
 *    check itself does not work on this firmware).
 *
 * The launcher loads this module into SceShell at run time, so nothing is
 * listed in ur0:tai/config.txt and it is gone after a reboot. A second copy
 * in the same shell refuses to start. Every step goes to LOG_PATH.
 */
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/types.h>
#include <taihen.h>

#define SELF_NAME  "tailnet_shell"
#define DIR_PATH   "ux0:data/tailnet-listener"
#define LOG_PATH   DIR_PATH "/launcher.log"
/* Written by the launcher on start, removed when it is opened with L held. */
#define KEEP_PATH  DIR_PATH "/keepalive"
#define SERVICE_ID "TVIT00014"

#define POLL_SEC   5
#define GONE_POLLS 3
#define MAX_FAILS  3
#define MAX_BLIND  4

static SceUID g_hook = -1;
static tai_hook_ref_t g_ref;

static void say(const char *msg)
{
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    sceIoWrite(fd, msg, sceClibStrnlen(msg, 200));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

static int budget_check_patched(void *a1, int a2, int a3)
{
    TAI_CONTINUE(int, g_ref, a1, a2, a3);
    return 0;
}

/* How many modules named SELF_NAME this process holds (this one included). */
static int copies_loaded(void)
{
    static SceUID ids[256];
    SceSize num = 256;
    int count = 0;
    if (sceKernelGetModuleList(0xFF, ids, &num) < 0)
        return 1;
    for (SceSize i = 0; i < num; i++) {
        SceKernelModuleInfo mi;
        sceClibMemset(&mi, 0, sizeof mi);
        mi.size = sizeof mi;
        if (sceKernelGetModuleInfo(ids[i], &mi) >= 0 &&
            sceClibStrncmp(mi.module_name, SELF_NAME, sizeof SELF_NAME) == 0)
            count++;
    }
    return count;
}

static int service_running(int *rc_out)
{
    SceUID pid = -1;
    int rc = sceAppMgrGetIdByName(&pid, SERVICE_ID);
    *rc_out = rc;
    return rc >= 0 && pid > 0;
}

static int watchdog_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    char line[160];
    int last = -1;          /* last logged state: 1 running, 0 gone */
    int ever_seen = 0;
    unsigned gone = 0, fails = 0, blind = 0;
    int gave_up = 0;

    for (;;) {
        sceKernelDelayThread(POLL_SEC * 1000000);
        SceIoStat st;
        if (sceIoGetstat(KEEP_PATH, &st) < 0) {
            /* Not asked to keep it up: forget everything and wait. */
            gone = fails = blind = 0;
            gave_up = 0;
            last = -1;
            continue;
        }
        int rc;
        int running = service_running(&rc);
        if (running != last) {
            sceClibSnprintf(line, sizeof line, "shell: service %s (rc %08X)",
                            running ? "running" : "not running", (unsigned)rc);
            say(line);
            last = running;
        }
        if (running) {
            ever_seen = 1;
            gone = fails = blind = 0;
            gave_up = 0;
            continue;
        }
        if (gave_up || ++gone < GONE_POLLS)
            continue;
        gone = 0;
        if (fails >= MAX_FAILS || (!ever_seen && blind >= MAX_BLIND)) {
            say("shell: giving up on restarts until the service is seen running again");
            gave_up = 1;
            continue;
        }
        SceUID app = sceAppMgrLaunchAppByName2ForShell(SERVICE_ID, NULL, NULL);
        sceClibSnprintf(line, sizeof line, "shell: start service -> %08X", (unsigned)app);
        say(line);
        if (app < 0)
            fails++;
        blind++;
    }
    return 0;
}

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    char line[160];

    if (copies_loaded() > 1) {
        say("shell: already loaded in this shell; not loading twice");
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    tai_module_info_t info;
    sceClibMemset(&info, 0, sizeof info);
    info.size = sizeof info;
    int rc = taiGetModuleInfo("SceShell", &info);
    if (rc < 0) {
        sceClibSnprintf(line, sizeof line, "shell: SceShell not found (%08X)", (unsigned)rc);
        say(line);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    unsigned offset;
    switch (info.module_nid) {
    case 0x0552F692: /* retail 3.60 */
        offset = 0x27246E;
        break;
    case 0x5549BF1F: /* retail 3.65 */
    case 0x34B4D82E: /* retail 3.67 */
    case 0x12DAC0F3: /* retail 3.68 */
        offset = 0x272526;
        break;
    default:
        sceClibSnprintf(line, sizeof line, "shell: unknown SceShell nid %08X; no hook",
                        (unsigned)info.module_nid);
        say(line);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    g_hook = taiHookFunctionOffset(&g_ref, info.modid, 0, offset, 1, budget_check_patched);
    sceClibSnprintf(line, sizeof line, "shell: SceShell nid %08X offset %06X hook %08X (%s)",
                    (unsigned)info.module_nid, offset, (unsigned)g_hook,
                    g_hook >= 0 ? "installed" : "FAILED");
    say(line);
    if (g_hook < 0)
        return SCE_KERNEL_START_NO_RESIDENT;

    SceUID thid = sceKernelCreateThread("tailnet_watchdog", watchdog_main, 0xA0, 16 * 1024, 0, 0, NULL);
    if (thid < 0 || sceKernelStartThread(thid, 0, NULL) < 0)
        say("shell: watchdog thread failed to start; no automatic restarts");
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    if (g_hook >= 0)
        taiHookRelease(g_hook, g_ref);
    return SCE_KERNEL_STOP_SUCCESS;
}
