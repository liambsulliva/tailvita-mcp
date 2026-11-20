/* Helpers shared by the probe modules. No libc: SCE calls only. */
#ifndef TNPROBE_COMMON_H
#define TNPROBE_COMMON_H

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/types.h>

#define DIR_PATH  "ux0:data/tailnet-probe"
#define STOP_PATH DIR_PATH "/stop"

/* Block sizes tried, largest first, in KiB. CDRAM blocks must be multiples
 * of 256 KiB and physically contiguous ones multiples of 1 MiB, so those
 * types stop at their own smallest step. */
static const unsigned LADDER_KB[] = {
    32768, 24576, 16384, 12288, 8192, 6144, 4096, 3072, 2048, 1024, 512, 256, 64,
};
#define N_LADDER (sizeof LADDER_KB / sizeof LADDER_KB[0])

static void log_to(const char *path, const char *msg)
{
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0)
        return;
    sceIoWrite(fd, msg, sceClibStrnlen(msg, 400));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
}

/* Largest block of `type` that can be allocated right now, in KiB, trying
 * sizes from `max_kb` down to `min_kb`. The block is freed straight away.
 * `*err` gets the error of the last size that failed (0 if none did). */
static unsigned max_alloc_kb(SceKernelMemBlockType type, unsigned min_kb,
                             unsigned max_kb, int *err)
{
    *err = 0;
    for (unsigned i = 0; i < N_LADDER && LADDER_KB[i] >= min_kb; i++) {
        if (LADDER_KB[i] > max_kb)
            continue;
        SceUID blk = sceKernelAllocMemBlock("tnprobe", type, LADDER_KB[i] << 10, NULL);
        if (blk >= 0) {
            sceKernelFreeMemBlock(blk);
            return LADDER_KB[i];
        }
        *err = blk;
    }
    return 0;
}

static int stop_requested(void)
{
    SceIoStat st;
    return sceIoGetstat(STOP_PATH, &st) >= 0;
}

#endif
