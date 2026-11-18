#pragma once

#include <stdint.h>

#define VITACOMPANION_KERNEL_ABI_VERSION 4
#define VITACOMPANION_KERNEL_MODULE_NAME "vitacompanion_kernel"
/* This syscall lost its fifth, stack-passed argument on device. */
#define VITACOMPANION_TOUCH_ACTIVE_FLAG 0x100

int vitaCompanionKernelGetApiVersion(void);
int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed);
int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active);
int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y);
int vitaCompanionKernelReset(void);
int vitaCompanionKernelRequestReboot(void);
int vitaCompanionKernelForceReboot(void);

/* Screenshot support (ABI 4). Pitch and height caps bound the kernel's
 * row buffer; the Vita's largest framebuffer is 960x544. */
#define VITACOMPANION_SCREEN_MAX_PITCH 1024
#define VITACOMPANION_SCREEN_MAX_HEIGHT 1088

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t pixelformat;
    uint32_t pid;
} vitacompanion_screen_info;

int vitaCompanionKernelScreenInfo(vitacompanion_screen_info *info);
int vitaCompanionKernelScreenReadRow(uint32_t row, void *dst,
    uint32_t dst_size);
