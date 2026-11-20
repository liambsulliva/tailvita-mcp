/* tailnet_lowmem.suprx: lets the background service start while an
 * enlarged-memory game is running.
 *
 * SceShell refuses to launch another app while a game that asked for extra
 * memory (+29, +77 or +109 MiB) is running. This module hooks that budget
 * check and makes it answer "allowed". The hook is the one GrapheneCt's
 * LowMemMode plugin installs: the same function offsets, chosen by
 * SceShell's module NID, and the same replacement (run the original, then
 * return 0). An unknown SceShell build is left alone.
 *
 * The launcher loads it into SceShell at run time, so nothing is listed in
 * ur0:tai/config.txt and it is gone after a reboot. A second copy in the same
 * shell refuses to start.
 */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>
#include <taihen.h>

#define SELF_NAME "tailnet_lowmem"
#define LOG_PATH  "ux0:data/tailnet-listener/launcher.log"

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

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    char line[160];

    if (copies_loaded() > 1) {
        say("lowmem: already loaded in this shell; not loading twice");
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    tai_module_info_t info;
    sceClibMemset(&info, 0, sizeof info);
    info.size = sizeof info;
    int rc = taiGetModuleInfo("SceShell", &info);
    if (rc < 0) {
        sceClibSnprintf(line, sizeof line, "lowmem: SceShell not found (%08X)", (unsigned)rc);
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
        sceClibSnprintf(line, sizeof line, "lowmem: unknown SceShell nid %08X; no hook",
                        (unsigned)info.module_nid);
        say(line);
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    g_hook = taiHookFunctionOffset(&g_ref, info.modid, 0, offset, 1, budget_check_patched);
    sceClibSnprintf(line, sizeof line, "lowmem: SceShell nid %08X offset %06X hook %08X (%s)",
                    (unsigned)info.module_nid, offset, (unsigned)g_hook,
                    g_hook >= 0 ? "installed" : "FAILED");
    say(line);
    return g_hook >= 0 ? SCE_KERNEL_START_SUCCESS : SCE_KERNEL_START_NO_RESIDENT;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    if (g_hook >= 0)
        taiHookRelease(g_hook, g_ref);
    return SCE_KERNEL_STOP_SUCCESS;
}
