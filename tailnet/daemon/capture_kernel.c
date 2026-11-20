/* tailnet_capture.skprx: copies the frame on screen out to a user process.
 *
 * A user process can only read its own framebuffer. To take a screenshot of
 * whatever is in front (a game, or the shell), this kernel module asks the
 * display driver which framebuffer is on the primary head and copies it out
 * of the owning process into the caller's buffer. The system screenshot is
 * no use to the background service: taken while an enlarged-memory game is
 * loaded, it makes the system end the service.
 *
 * The whole frame is copied inside one call, a row at a time through a 4 KiB
 * buffer, so the module allocates nothing. (A first version kept a 2 MiB
 * snapshot in kernel memory; the kernel refused that allocation.) The copy
 * takes a few milliseconds, short enough that the picture does not tear in
 * practice.
 *
 * The launcher loads this module at run time with taiHEN. It is never listed
 * in ur0:tai/config.txt, hooks nothing, and is gone after a reboot.
 */
#include <psp2kern/display.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/threadmgr.h>
#include <stdint.h>

#include "capture.h"

static SceUID g_mutex = -1;
/* One row on its way from the owner to the caller. Guarded by g_mutex. */
static uint8_t g_row[TN_CAPTURE_MAX_PITCH * 4];

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

int tnCaptureGetApiVersion(void)
{
    return TN_CAPTURE_API_VERSION;
}

int tnCaptureFrame(tn_capture_info *user_info, void *user_dst, uint32_t dst_size)
{
    uint32_t state;
    SceDisplayFrameBufInfo fb;
    tn_capture_info info;
    int rc = 0;

    ENTER_SYSCALL(state);
    if (!user_info || !user_dst) {
        EXIT_SYSCALL(state);
        return TN_CAPTURE_ERR_ARGUMENT;
    }
    ksceKernelLockMutex(g_mutex, 1, NULL);

    if (current_frame(&fb) < 0) {
        rc = TN_CAPTURE_ERR_NO_FRAME;
        goto out;
    }
    info.width = fb.framebuf.width;
    info.height = fb.framebuf.height;
    info.pitch = fb.framebuf.pitch;
    info.pixelformat = fb.framebuf.pixelformat;
    info.pid = (uint32_t)fb.pid;
    rc = ksceKernelCopyToUser(user_info, &info, sizeof info);
    if (rc < 0)
        goto out;

    if (info.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 || info.pitch < info.width ||
        info.pitch > TN_CAPTURE_MAX_PITCH || info.height > TN_CAPTURE_MAX_HEIGHT) {
        rc = TN_CAPTURE_ERR_FORMAT;
        goto out;
    }
    uint32_t row_bytes = info.width * 4u;
    if (dst_size / row_bytes < info.height) {
        rc = TN_CAPTURE_ERR_TOO_SMALL;
        goto out;
    }
    for (uint32_t y = 0; y < info.height; y++) {
        const uint8_t *src = (const uint8_t *)fb.framebuf.base + (uintptr_t)y * info.pitch * 4u;
        rc = ksceKernelCopyFromUserProc(fb.pid, g_row, src, row_bytes);
        if (rc < 0)
            goto out;
        rc = ksceKernelCopyToUser((uint8_t *)user_dst + (uintptr_t)y * row_bytes, g_row, row_bytes);
        if (rc < 0)
            goto out;
    }
    rc = 0;

out:
    ksceKernelUnlockMutex(g_mutex, 1);
    EXIT_SYSCALL(state);
    return rc;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    g_mutex = ksceKernelCreateMutex("tailnet_capture2", 0, 0, NULL);
    if (g_mutex < 0)
        return SCE_KERNEL_START_FAILED;
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    return SCE_KERNEL_STOP_SUCCESS;
}
