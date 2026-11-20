/* Calls exported by tailnet_capture.skprx (capture_kernel.c). */
#ifndef TAILNET_CAPTURE_H
#define TAILNET_CAPTURE_H

#include <stdint.h>

#define TN_CAPTURE_API_VERSION 2
#define TN_CAPTURE_MAX_PITCH   1024
#define TN_CAPTURE_MAX_HEIGHT  544

/* Errors. They are shaped like SCE error codes on purpose: a syscall's
 * small negative return value (-7, say) reaches the caller with bit 30
 * cleared (0xBFFFFFF9), so plain negative numbers cannot be told apart
 * reliably. */
#define TN_CAPTURE_ERR_ARGUMENT  ((int)0x80F70001) /* null pointer */
#define TN_CAPTURE_ERR_NO_FRAME  ((int)0x80F70002) /* nothing on screen */
#define TN_CAPTURE_ERR_FORMAT    ((int)0x80F70003) /* unsupported format or size */
#define TN_CAPTURE_ERR_TOO_SMALL ((int)0x80F70004) /* dst smaller than the frame */
/* Any other negative value is the SCE error of the memory copy that failed. */

typedef struct tn_capture_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       /* pixels per row as stored by the owner, >= width */
    uint32_t pixelformat; /* SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 when supported */
    uint32_t pid;         /* process that owns the frame */
} tn_capture_info;

/* TN_CAPTURE_API_VERSION of the loaded module. */
int tnCaptureGetApiVersion(void);

/* Describe the frame now on screen and copy it to `dst` as `height` rows of
 * `width` pixels, each R, G, B, A (so width * height * 4 bytes, without the
 * owner's row padding). `info` is filled in whenever a frame was found, also
 * when the call then fails with TN_CAPTURE_ERR_FORMAT or
 * TN_CAPTURE_ERR_TOO_SMALL, so a caller can size its buffer from it.
 * Returns 0 on success. */
int tnCaptureFrame(tn_capture_info *info, void *dst, uint32_t dst_size);

#endif
