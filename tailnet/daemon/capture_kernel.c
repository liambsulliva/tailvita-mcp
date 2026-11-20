/* tailnet_capture.skprx: copies the frame on screen out to a user process.
 *
 * A user process can only read its own framebuffer. To take a screenshot of
 * whatever is in front (a game, or the shell), this kernel module asks the
 * display driver which framebuffer is on the primary head and copies it out
 * of the owning process. The system screenshot is no use to the background
 * service: taken while an enlarged-memory game is loaded, it makes the
 * system end the service.
 *
 * tnCaptureSnapshot copies the whole frame into a kernel block in one go, so
 * the picture does not tear; tnCaptureRead then hands it out a few rows at a
 * time, so the caller never needs the 2 MiB frame in its own memory.
 *
 * The launcher loads this module at run time with taiHEN. It is never listed
 * in ur0:tai/config.txt, hooks nothing, and is gone after a reboot.
 */
#include <psp2kern/display.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr.h>
#include <stdint.h>

#include "capture.h"

static SceUID g_mutex = -1;
static SceUID g_block = -1;      /* the snapshot's memory block */
static uint8_t *g_pixels;        /* its base */
static uint32_t g_block_bytes;
static tn_capture_info g_info;   /* describes the snapshot; width 0 = none */

static int current_frame(SceDisplayFrameBufInfo *info)
{
    int head = ksceDisplayGetPrimaryHead();
    if (head < 0)
        head = 0;
    /* pid -1: whatever is shown on this head. Index 0 is the usual layer;
     * fall back to 1 if nothing is bound there. */
    for (int index = 0; index < 2; index++) {
        info->size = sizeof *info;
        if (ksceDisplayGetProcFrameBufInternal(-1, head, index, info) >= 0 &&
            info->framebuf.base != NULL && info->framebuf.width > 0 &&
            info->framebuf.height > 0)
            return 0;
    }
    return -1;
}

static void drop_snapshot(void)
{
    if (g_block >= 0)
        ksceKernelFreeMemBlock(g_block);
    g_block = -1;
    g_pixels = NULL;
    g_block_bytes = 0;
    g_info.width = 0;
}

static int snapshot_locked(tn_capture_info *out)
{
    SceDisplayFrameBufInfo fb;
    if (current_frame(&fb) < 0)
        return -2;
    if (fb.framebuf.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 ||
        fb.framebuf.pitch < fb.framebuf.width ||
        fb.framebuf.pitch > TN_CAPTURE_MAX_PITCH ||
        fb.framebuf.height > TN_CAPTURE_MAX_HEIGHT)
        return -3;

    uint32_t bytes = fb.framebuf.pitch * fb.framebuf.height * 4u;
    uint32_t need = (bytes + 0xFFFu) & ~0xFFFu;
    if (g_block < 0 || g_block_bytes < need) {
        drop_snapshot();
        SceUID blk = ksceKernelAllocMemBlock("tailnet_capture", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                             need, NULL);
        if (blk < 0)
            return -7;
        void *base = NULL;
        if (ksceKernelGetMemBlockBase(blk, &base) < 0 || base == NULL) {
            ksceKernelFreeMemBlock(blk);
            return -7;
        }
        g_block = blk;
        g_pixels = base;
        g_block_bytes = need;
    }
    g_info.width = 0;
    if (ksceKernelCopyFromUserProc(fb.pid, g_pixels, fb.framebuf.base, bytes) < 0)
        return -8;

    g_info.width = fb.framebuf.width;
    g_info.height = fb.framebuf.height;
    g_info.pitch = fb.framebuf.pitch;
    g_info.pixelformat = fb.framebuf.pixelformat;
    g_info.pid = (uint32_t)fb.pid;
    *out = g_info;
    return 0;
}

int tnCaptureGetApiVersion(void)
{
    return TN_CAPTURE_API_VERSION;
}

int tnCaptureSnapshot(tn_capture_info *user_info)
{
    uint32_t state;
    tn_capture_info out;
    int rc;

    ENTER_SYSCALL(state);
    if (!user_info) {
        rc = -1;
    } else {
        ksceKernelLockMutex(g_mutex, 1, NULL);
        rc = snapshot_locked(&out);
        ksceKernelUnlockMutex(g_mutex, 1);
        if (rc == 0 && ksceKernelCopyToUser(user_info, &out, sizeof out) < 0)
            rc = -4;
    }
    EXIT_SYSCALL(state);
    return rc;
}

int tnCaptureRead(uint32_t first_row, uint32_t rows, void *user_dst, uint32_t dst_size)
{
    uint32_t state;
    int rc;

    ENTER_SYSCALL(state);
    ksceKernelLockMutex(g_mutex, 1, NULL);
    if (!user_dst || rows == 0) {
        rc = -1;
    } else if (g_info.width == 0 || !g_pixels) {
        rc = -9;
    } else if (first_row >= g_info.height || rows > g_info.height - first_row) {
        rc = -5;
    } else if (dst_size / (g_info.width * 4u) < rows) {
        rc = -6;
    } else {
        uint32_t row_bytes = g_info.width * 4u;
        rc = (int)(row_bytes * rows);
        for (uint32_t r = 0; r < rows; r++) {
            const uint8_t *src = g_pixels + (uintptr_t)(first_row + r) * g_info.pitch * 4u;
            if (ksceKernelCopyToUser((uint8_t *)user_dst + (uintptr_t)r * row_bytes, src, row_bytes) < 0) {
                rc = -4;
                break;
            }
        }
    }
    ksceKernelUnlockMutex(g_mutex, 1);
    EXIT_SYSCALL(state);
    return rc;
}

int tnCaptureRelease(void)
{
    uint32_t state;
    ENTER_SYSCALL(state);
    ksceKernelLockMutex(g_mutex, 1, NULL);
    drop_snapshot();
    ksceKernelUnlockMutex(g_mutex, 1);
    EXIT_SYSCALL(state);
    return 0;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    g_mutex = ksceKernelCreateMutex("tailnet_capture", 0, 0, NULL);
    if (g_mutex < 0)
        return SCE_KERNEL_START_FAILED;
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    drop_snapshot();
    return SCE_KERNEL_STOP_SUCCESS;
}
