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
 *    checks. A start can report success and the service still never appear
 *    (seen on 2026-11-20 and 21), so each start is followed for FOLLOW_MS and
 *    the wait before the next one doubles, up to MAX_WAIT_POLLS checks. The
 *    wait goes back to GONE_POLLS once the service has stayed up for
 *    STEADY_POLLS checks. Before each start the titles that are running are
 *    logged, to show what the service could not start beside.
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
/* Says the kernel capture helper was loaded during this boot. This module
 * starts once per boot, so a marker it finds on a real start is a stale one. */
#define CAP_READY  DIR_PATH "/capture2-ready"
#define SERVICE_ID "TVIT00014"

#define POLL_SEC       5
#define GONE_POLLS     3
#define MAX_WAIT_POLLS 60
#define STEADY_POLLS   12
#define FOLLOW_MS      5000
#define FOLLOW_STEP_MS 250

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

/* One line naming every running title, e.g. "shell: running: GHTR00001". */
static void log_running_apps(void)
{
    char line[160];
    SceInt32 ids[12];
    int n = sceAppMgrGetRunningAppIdListForShell(ids, 12);
    int len = sceClibSnprintf(line, sizeof line, "shell: running (%d):", n);
    for (int i = 0; i < n && i < 12 && len < (int)sizeof line - 16; i++) {
        char name[32];
        sceClibMemset(name, 0, sizeof name);
        SceUID pid = sceAppMgrGetProcessIdByAppIdForShell(ids[i]);
        if (pid < 0 || sceAppMgrGetNameById(pid, name) < 0)
            sceClibSnprintf(name, sizeof name, "?%08X", (unsigned)ids[i]);
        name[12] = 0;
        len += sceClibSnprintf(line + len, sizeof line - len, " %s", name);
    }
    say(line);
}

/* Watch a start for FOLLOW_MS and log what became of it. */
static void follow_start(void)
{
    char line[160];
    int rc, seen_at = -1;
    for (int ms = FOLLOW_STEP_MS; ms <= FOLLOW_MS; ms += FOLLOW_STEP_MS) {
        sceKernelDelayThread(FOLLOW_STEP_MS * 1000);
        int running = service_running(&rc);
        if (running && seen_at < 0) {
            seen_at = ms;
        } else if (!running && seen_at >= 0) {
            sceClibSnprintf(line, sizeof line, "shell: service appeared after %d ms and was gone at %d ms (rc %08X)",
                            seen_at, ms, (unsigned)rc);
            say(line);
            return;
        }
    }
    if (seen_at >= 0)
        sceClibSnprintf(line, sizeof line, "shell: service appeared after %d ms", seen_at);
    else
        sceClibSnprintf(line, sizeof line, "shell: service did not appear within %d ms (rc %08X)",
                        FOLLOW_MS, (unsigned)rc);
    say(line);
}

static int watchdog_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    char line[160];
    int last = -1;          /* last logged state: 1 running, 0 gone */
    unsigned gone = 0, steady = 0, wait = GONE_POLLS;

    for (;;) {
        sceKernelDelayThread(POLL_SEC * 1000000);
        SceIoStat st;
        if (sceIoGetstat(KEEP_PATH, &st) < 0) {
            /* Not asked to keep it up: forget everything and wait. */
            gone = steady = 0;
            wait = GONE_POLLS;
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
            gone = 0;
            if (++steady >= STEADY_POLLS)
                wait = GONE_POLLS;
            continue;
        }
        steady = 0;
        if (++gone < wait)
            continue;
        gone = 0;
        log_running_apps();
        SceUID app = sceAppMgrLaunchAppByName2ForShell(SERVICE_ID, NULL, NULL);
        sceClibSnprintf(line, sizeof line, "shell: start service -> %08X (next try in %u s if it does not stay up)",
                        (unsigned)app, (wait * 2 > MAX_WAIT_POLLS ? MAX_WAIT_POLLS : wait * 2) * POLL_SEC);
        say(line);
        if (app >= 0)
            follow_start();
        wait = wait * 2 > MAX_WAIT_POLLS ? MAX_WAIT_POLLS : wait * 2;
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
    /* First start in this SceShell: whatever the marker says is from an
     * earlier boot. The launcher writes it again after loading the helper. */
    sceIoRemove(CAP_READY);

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
