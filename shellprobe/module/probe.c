/* shellprobe.suprx: logs SceShell's free memory over time.
 *
 * Loaded into SceShell at run time by the Shell Probe app (never from
 * ur0:tai/config.txt). One 16 KiB thread wakes every SAMPLE_SEC seconds and
 * appends a line to LOG_PATH:
 *
 *   t=14:02:10 up=312s rc=0 free_kb user=1234 cdram=0 phycont=0 apps=PCSG00001
 *
 * Every LADDER_EVERY-th sample it also allocates and at once frees one block
 * of each memory type, largest size first, and logs the largest size that
 * succeeded ("max_kb") and the error of the last size that failed. That
 * number says what a plugin could really get, which the free counters alone
 * do not.
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

#define SAMPLE_SEC   5
#define LADDER_EVERY 2
#define MAX_MINUTES  90

/* Block sizes tried, largest first, in KiB. CDRAM blocks must be multiples
 * of 256 KiB and physically contiguous ones multiples of 1 MiB, so those
 * types stop at their own smallest step. */
static const unsigned LADDER_KB[] = {
    32768, 24576, 16384, 12288, 8192, 6144, 4096, 3072, 2048, 1024, 512, 256, 64,
};
#define N_LADDER (sizeof LADDER_KB / sizeof LADDER_KB[0])

static void log_line(const char *msg)
{
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    sceIoWrite(fd, msg, sceClibStrnlen(msg, 400));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

/* Largest block of `type` that can be allocated right now, in KiB, trying no
 * size below `min_kb`. The block is freed straight away. `*err` gets the
 * error of the last size that failed (0 if the first try worked). */
static unsigned max_alloc_kb(SceKernelMemBlockType type, unsigned min_kb, int *err)
{
    *err = 0;
    for (unsigned i = 0; i < N_LADDER && LADDER_KB[i] >= min_kb; i++) {
        SceUID blk = sceKernelAllocMemBlock("tnprobe", type, LADDER_KB[i] << 10, NULL);
        if (blk >= 0) {
            sceKernelFreeMemBlock(blk);
            return LADDER_KB[i];
        }
        *err = blk;
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
            int eu, ec, ep;
            unsigned ku = max_alloc_kb(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64, &eu);
            unsigned kc = max_alloc_kb(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256, &ec);
            unsigned kp = max_alloc_kb(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 1024, &ep);
            w += sceClibSnprintf(line + w, sizeof line - w,
                                 " max_kb user=%u cdram=%u phycont=%u err=%X/%X/%X",
                                 ku, kc, kp, (unsigned)eu, (unsigned)ec, (unsigned)ep);
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
