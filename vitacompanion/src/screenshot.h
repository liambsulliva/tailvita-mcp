#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCREENSHOT_DEFAULT_PATH "ux0:data/vitacompanion/screenshot.bmp"

void cmd_screenshot(char **arg_list, size_t arg_count, char *res_msg);

/* Exposed for host tests. */
uint32_t screenshot_bmp_row_bytes(uint32_t width);
void screenshot_bmp_header(uint8_t header[54], uint32_t width, uint32_t height);
void screenshot_abgr_to_bgr(const uint8_t *src, uint8_t *dst, uint32_t width);
bool screenshot_path_is_allowed(const char *path);
