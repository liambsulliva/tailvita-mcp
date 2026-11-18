#include <vitacompanion_kernel.h>

#include <psp2kern/display.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <stdint.h>

/*
 * Framebuffer capture for the `screenshot` command.
 *
 * The user module can only see SceShell's own framebuffer, so the kernel
 * module looks up whichever framebuffer is on the primary head (the
 * foreground app, or the shell) and copies rows out of the owning
 * process. Rows go through one small static buffer; the caller streams
 * them to a file, so a full frame never sits in memory.
 */

#define SCREEN_ROW_BYTES_MAX (VITACOMPANION_SCREEN_MAX_PITCH * 4U)

static uint8_t row_buffer[SCREEN_ROW_BYTES_MAX];

static int current_frame(SceDisplayFrameBufInfo *info)
{
    int head = ksceDisplayGetPrimaryHead();
    int index;

    if (head < 0)
        head = 0;

    /* pid -1: whatever is currently configured for this head. Index 0 is
     * the usual layer; fall back to 1 if nothing is bound there. */
    for (index = 0; index < 2; ++index) {
        info->size = sizeof(*info);
        if (ksceDisplayGetProcFrameBufInternal(-1, head, index, info) >= 0 &&
            info->framebuf.base != NULL &&
            info->framebuf.width > 0 && info->framebuf.height > 0)
            return 0;
    }
    return -1;
}

static int frame_is_supported(const SceDisplayFrameBufInfo *info)
{
    return info->framebuf.pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 &&
        info->framebuf.pitch >= info->framebuf.width &&
        info->framebuf.pitch <= VITACOMPANION_SCREEN_MAX_PITCH &&
        info->framebuf.height <= VITACOMPANION_SCREEN_MAX_HEIGHT;
}

int vitaCompanionKernelScreenInfo(vitacompanion_screen_info *user_info)
{
    SceDisplayFrameBufInfo info;
    vitacompanion_screen_info out;

    if (!user_info)
        return -1;
    if (current_frame(&info) < 0)
        return -2;
    if (!frame_is_supported(&info))
        return -3;

    out.width = info.framebuf.width;
    out.height = info.framebuf.height;
    out.pitch = info.framebuf.pitch;
    out.pixelformat = info.framebuf.pixelformat;
    out.pid = (uint32_t)info.pid;
    if (ksceKernelCopyToUser(user_info, &out, sizeof(out)) < 0)
        return -4;
    return 0;
}

/* Copy one row (width pixels, ABGR8888) of the current frame to user_dst.
 * The frame is looked up on every call, so a frame that changes between
 * rows tears rather than fails; that is fine for a debugging capture. */
int vitaCompanionKernelScreenReadRow(uint32_t row, void *user_dst,
    uint32_t dst_size)
{
    SceDisplayFrameBufInfo info;
    uint32_t bytes;
    const uint8_t *src;

    if (!user_dst)
        return -1;
    if (current_frame(&info) < 0)
        return -2;
    if (!frame_is_supported(&info))
        return -3;
    if (row >= info.framebuf.height)
        return -5;

    bytes = info.framebuf.width * 4U;
    if (dst_size < bytes || bytes > sizeof(row_buffer))
        return -6;

    src = (const uint8_t *)info.framebuf.base +
        (uintptr_t)row * info.framebuf.pitch * 4U;
    if (ksceKernelCopyFromUserProc(info.pid, row_buffer, src, bytes) < 0)
        return -7;
    if (ksceKernelCopyToUser(user_dst, row_buffer, bytes) < 0)
        return -4;
    return (int)bytes;
}
