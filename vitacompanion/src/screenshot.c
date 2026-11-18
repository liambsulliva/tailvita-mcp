#include "screenshot.h"

#include <vitacompanion_kernel.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SCREENSHOT_DIR "ux0:data/vitacompanion"
#define SCREENSHOT_PATH_MAX 128
#define BMP_HEADER_SIZE 54U

/* One ABGR row in, one BGR row out. Static so the capture costs no heap
 * and no command-thread stack: 4 KiB + 3 KiB. */
static uint8_t abgr_row[VITACOMPANION_SCREEN_MAX_PITCH * 4U];
static uint8_t bgr_row[VITACOMPANION_SCREEN_MAX_PITCH * 3U + 3U];

static void put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t screenshot_bmp_row_bytes(uint32_t width)
{
    return (width * 3U + 3U) & ~3U;
}

void screenshot_bmp_header(uint8_t header[54], uint32_t width, uint32_t height)
{
    uint32_t image = screenshot_bmp_row_bytes(width) * height;

    memset(header, 0, BMP_HEADER_SIZE);
    header[0] = 'B';
    header[1] = 'M';
    put_le32(header + 2, BMP_HEADER_SIZE + image);
    put_le32(header + 10, BMP_HEADER_SIZE);
    put_le32(header + 14, 40);
    put_le32(header + 18, width);
    put_le32(header + 22, height); /* positive: rows stored bottom-up */
    put_le16(header + 26, 1);
    put_le16(header + 28, 24);
    put_le32(header + 34, image);
    put_le32(header + 38, 2835); /* 72 dpi */
    put_le32(header + 42, 2835);
}

void screenshot_abgr_to_bgr(const uint8_t *src, uint8_t *dst, uint32_t width)
{
    uint32_t x;

    /* SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 is R, G, B, A in memory order. */
    for (x = 0; x < width; ++x) {
        dst[x * 3U + 0U] = src[x * 4U + 2U];
        dst[x * 3U + 1U] = src[x * 4U + 1U];
        dst[x * 3U + 2U] = src[x * 4U + 0U];
    }
}

bool screenshot_path_is_allowed(const char *path)
{
    size_t length;

    if (!path)
        return false;
    length = strlen(path);
    if (length < 5 || length >= SCREENSHOT_PATH_MAX - 4)
        return false;
    if (strncmp(path, "ux0:data/", 9) != 0 && strncmp(path, "ux0:/data/", 10) != 0)
        return false;
    if (strstr(path, "..") != NULL)
        return false;
    return strcmp(path + length - 4, ".bmp") == 0;
}

void cmd_screenshot(char **arg_list, size_t arg_count, char *res_msg)
{
    const char *path = arg_count > 1 ? arg_list[1] : SCREENSHOT_DEFAULT_PATH;
    char tmp_path[SCREENSHOT_PATH_MAX];
    uint8_t header[BMP_HEADER_SIZE];
    vitacompanion_screen_info info;
    uint32_t row_bytes;
    uint32_t y;
    SceUID fd;
    int result;

    if (!screenshot_path_is_allowed(path)) {
        strcpy(res_msg, "Error: path must be a .bmp under ux0:data/\n");
        return;
    }
    result = vitaCompanionKernelScreenInfo(&info);
    if (result < 0) {
        snprintf(res_msg, 256, "Error: no capturable framebuffer (%d)\n", result);
        return;
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    sceIoMkdir(SCREENSHOT_DIR, 0777);
    fd = sceIoOpen(tmp_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        snprintf(res_msg, 256, "Error: cannot open %s (0x%08X)\n", tmp_path, (unsigned int)fd);
        return;
    }

    row_bytes = screenshot_bmp_row_bytes(info.width);
    memset(bgr_row, 0, sizeof(bgr_row));
    screenshot_bmp_header(header, info.width, info.height);
    result = sceIoWrite(fd, header, sizeof(header)) == (int)sizeof(header) ? 0 : -1;

    for (y = info.height; result == 0 && y > 0; --y) {
        result = vitaCompanionKernelScreenReadRow(y - 1U, abgr_row, sizeof(abgr_row));
        if (result < 0)
            break;
        screenshot_abgr_to_bgr(abgr_row, bgr_row, info.width);
        result = sceIoWrite(fd, bgr_row, row_bytes) == (int)row_bytes ? 0 : -1;
    }
    sceIoClose(fd);

    if (result < 0) {
        sceIoRemove(tmp_path);
        snprintf(res_msg, 256, "Error: capture failed at row %u (%d)\n", (unsigned int)y, result);
        return;
    }
    sceIoRemove(path);
    if (sceIoRename(tmp_path, path) < 0) {
        sceIoRemove(tmp_path);
        snprintf(res_msg, 256, "Error: cannot rename to %s\n", path);
        return;
    }
    snprintf(res_msg, 256, "Saved %s %ux%u pid=0x%08X\n", path,
        (unsigned int)info.width, (unsigned int)info.height, (unsigned int)info.pid);
}
