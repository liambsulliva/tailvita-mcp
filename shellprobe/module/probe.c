/* shellprobe.suprx: logs SceShell's free memory over time.
 *
 * Loaded into SceShell at run time by the Shell Probe app (never from
 * ur0:tai/config.txt). One 16 KiB thread wakes every SAMPLE_SEC seconds and
 * appends a line to LOG_PATH:
 *
 *   t=14:02:10 up=312s free_kb user=1234 cdram=0 phycont=0 apps=PCSG00001
 *
 * Every LADDER_EVERY-th sample it also allocates and at once frees one block
 * of each memory type, largest size first, and logs the largest size that
 * succeeded ("max_mb"). That number says what a plugin could really get,
 * which the free counters alone do not.
 *
 * The thread stops after MAX_MINUTES, or as soon as STOP_PATH exists.
 * No libc: only SCE calls, so there is no heap and nothing to initialise.
 */
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <psp2/types.h>

#define DIR_PATH  "ux0:data/tailnet-probe"
#define LOG_PATH  DIR_PATH "/shellmem.log"
#define STOP_PATH DIR_PATH "/stop"

#define SAMPLE_SEC   10
#define LADDER_EVERY 3
#define MAX_MINUTES  90

static const unsigned LADDER_MB[] = { 32, 24, 16, 12, 8, 6, 4, 3, 2, 1 };
#define N_LADDER (sizeof LADDER_MB / sizeof LADDER_MB[0])

static void log_line(const char *msg)
{
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    sceIoWrite(fd, msg, sceClibStrnlen(msg, 400));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

/* Largest block of `type` that can be allocated right now, in MiB. The block
 * is freed straight away. */
static unsigned max_alloc_mb(SceKernelMemBlockType type)
{
    for (unsigned i = 0; i < N_LADDER; i++) {
        SceUID blk = sceKernelAllocMemBlock("tnprobe", type, LADDER_MB[i] << 20, NULL);
        if (blk >= 0) {
            sceKernelFreeMemBlock(blk);
            return LADDER_MB[i];
        }
    }
    return 0;
}

/* Title IDs of the running apps, comma separated. */
static void running_apps(char *out, unsigned cap)
{
    SceInt32 ids[8];
    unsigned used = 0;
    out[0] = '\0';
    int n = sceAppMgrGetRunningAppIdListForShell(ids, 8);
    if (n < 0) {
        sceClibSnprintf(out, cap, "err:%08X", (unsigned)n);
        return;
    }
    for (int i = 0; i < n && i < 8; i++) {
        char name[32];
        sceClibMemset(name, 0, sizeof name);
        SceUID pid = sceAppMgrGetProcessIdByAppIdForShell(ids[i]);
        if (pid < 0 || sceAppMgrGetNameById(pid, name) < 0)
            sceClibSnprintf(name, sizeof name, "?%X", (unsigned)ids[i]);
        name[sizeof name - 1] = '\0';
        int w = sceClibSnprintf(out + used, cap - used, "%s%s", used ? "," : "", name);
        if (w < 0 || (unsigned)w >= cap - used)
            break;
        used += (unsigned)w;
    }
    if (used == 0)
        sceClibSnprintf(out, cap, "-");
}

static int probe_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    char line[400];
    char apps[128];
    SceUInt64 start = sceKernelGetSystemTimeWide();

    sceIoMkdir(DIR_PATH, 0777);
    sceIoRemove(STOP_PATH);
    sceClibSnprintf(line, sizeof line,
                    "--- shellprobe start pid=%08X sample=%ds ladder=every %d",
                    (unsigned)sceKernelGetProcessId(), SAMPLE_SEC, LADDER_EVERY);
    log_line(line);

    for (unsigned n = 0;; n++) {
        SceIoStat st;
        if (sceIoGetstat(STOP_PATH, &st) >= 0) {
            log_line("--- shellprobe stop (stop file)");
            break;
        }
        SceUInt64 now = sceKernelGetSystemTimeWide();
        if (now - start > (SceUInt64)MAX_MINUTES * 60 * 1000000ULL) {
            log_line("--- shellprobe stop (time limit)");
            break;
        }

        SceDateTime t;
        sceClibMemset(&t, 0, sizeof t);
        sceRtcGetCurrentClockLocalTime(&t);

        SceKernelFreeMemorySizeInfo mi;
        sceClibMemset(&mi, 0, sizeof mi);
        mi.size = sizeof mi;
        int rc = sceKernelGetFreeMemorySize(&mi);

        running_apps(apps, sizeof apps);

        int w = sceClibSnprintf(line, sizeof line,
                                "t=%02d:%02d:%02d up=%us rc=%X free_kb user=%d cdram=%d phycont=%d",
                                t.hour, t.minute, t.second,
                                (unsigned)(now / 1000000ULL), (unsigned)rc,
                                mi.size_user / 1024, mi.size_cdram / 1024,
                                mi.size_phycont / 1024);
        if (n % LADDER_EVERY == 0 && w > 0 && (unsigned)w < sizeof line) {
            w += sceClibSnprintf(line + w, sizeof line - w,
                                 " max_mb user=%u cdram=%u phycont=%u",
                                 max_alloc_mb(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW),
                                 max_alloc_mb(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW),
                                 max_alloc_mb(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW));
        }
        if (w > 0 && (unsigned)w < sizeof line)
            sceClibSnprintf(line + w, sizeof line - w, " apps=%s", apps);
        log_line(line);

        sceKernelDelayThread(SAMPLE_SEC * 1000000);
    }
    return sceKernelExitDeleteThread(0);
}

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    /* Priority 0xA0: below the shell's own threads (default 0x10000100 base
     * maps to 0x60..0xBF for users; larger is lower). */
    SceUID thid = sceKernelCreateThread("tnprobe", probe_main, 0xA0, 16 * 1024, 0, 0, NULL);
    if (thid < 0)
        return SCE_KERNEL_START_FAILED;
    if (sceKernelStartThread(thid, 0, NULL) < 0) {
        sceKernelDeleteThread(thid);
        return SCE_KERNEL_START_FAILED;
    }
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    return SCE_KERNEL_STOP_SUCCESS;
}
