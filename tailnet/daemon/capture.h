/* Calls exported by tailnet_capture.skprx (capture_kernel.c). */
#ifndef TAILNET_CAPTURE_H
#define TAILNET_CAPTURE_H

#include <stdint.h>

#define TN_CAPTURE_API_VERSION 1
#define TN_CAPTURE_MAX_PITCH   1024
#define TN_CAPTURE_MAX_HEIGHT  544

typedef struct tn_capture_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       /* pixels per stored row, >= width */
    uint32_t pixelformat; /* always SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 */
    uint32_t pid;         /* process that owns the frame */
} tn_capture_info;

/* TN_CAPTURE_API_VERSION of the loaded module. */
int tnCaptureGetApiVersion(void);

/* Copy the frame now on screen into the module's buffer and describe it.
 * 0 on success; < 0: -1 bad argument, -2 nothing on screen, -3 unsupported
 * format or size, -4 copy to caller failed, -7 no memory, -8 the frame's
 * memory could not be read. A snapshot stays until the next one or a
 * release. */
int tnCaptureSnapshot(tn_capture_info *info);

/* Copy `rows` rows of the snapshot, starting at `first_row`, to `dst`: each
 * row is `width` pixels of R, G, B, A bytes. Returns the bytes copied, or
 * < 0: -1 bad argument, -5 row out of range, -6 dst too small, -9 no
 * snapshot, -4 copy to caller failed. */
int tnCaptureRead(uint32_t first_row, uint32_t rows, void *dst, uint32_t dst_size);

/* Free the snapshot. */
int tnCaptureRelease(void);

#endif
