import pathlib
import struct
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]

FCNTL_H = r"""
#pragma once
typedef int SceUID;
typedef unsigned int SceSize;
#define SCE_O_WRONLY 0x0002
#define SCE_O_CREAT 0x0200
#define SCE_O_TRUNC 0x0400
SceUID sceIoOpen(const char *path, int flags, int mode);
int sceIoWrite(SceUID fd, const void *data, SceSize size);
int sceIoClose(SceUID fd);
int sceIoRemove(const char *path);
int sceIoRename(const char *from, const char *to);
"""

STAT_H = r"""
#pragma once
int sceIoMkdir(const char *path, int mode);
"""

# A 3x2 frame with pitch 4 (one padding pixel per row). Pixel bytes are
# R, G, B, A in memory, as SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 stores them.
HARNESS = r"""
#include "screenshot.h"
#include <vitacompanion_kernel.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>

static const char *out_dir;
static FILE *open_file;
static int fail_row = -1;
static int no_frame;

static void host_path(const char *path, char *out, size_t size)
{
    size_t i, n = 0;
    n = (size_t)snprintf(out, size, "%s/", out_dir);
    for (i = 0; path[i] && n + 1 < size; ++i)
        out[n++] = (path[i] == '/' || path[i] == ':') ? '_' : path[i];
    out[n] = '\0';
}

SceUID sceIoOpen(const char *path, int flags, int mode)
{
    char p[512];
    (void)flags; (void)mode;
    host_path(path, p, sizeof(p));
    open_file = fopen(p, "wb");
    return open_file ? 3 : -1;
}
int sceIoWrite(SceUID fd, const void *data, SceSize size)
{
    (void)fd;
    return (int)fwrite(data, 1, size, open_file);
}
int sceIoClose(SceUID fd) { (void)fd; fclose(open_file); open_file = NULL; return 0; }
int sceIoRemove(const char *path) { char p[512]; host_path(path, p, sizeof(p)); remove(p); return 0; }
int sceIoRename(const char *from, const char *to)
{
    char a[512], b[512];
    host_path(from, a, sizeof(a));
    host_path(to, b, sizeof(b));
    return rename(a, b);
}
int sceIoMkdir(const char *path, int mode) { (void)path; (void)mode; return 0; }

int vitaCompanionKernelScreenInfo(vitacompanion_screen_info *info)
{
    if (no_frame)
        return -2;
    info->width = 3;
    info->height = 2;
    info->pitch = 4;
    info->pixelformat = 0;
    info->pid = 0x10005;
    return 0;
}

int vitaCompanionKernelScreenReadRow(uint32_t row, void *dst, uint32_t dst_size)
{
    uint8_t *d = dst;
    uint32_t x;
    if ((int)row == fail_row)
        return -7;
    if (dst_size < 12)
        return -6;
    for (x = 0; x < 3; ++x) {
        d[x * 4 + 0] = (uint8_t)(0x10 + row * 0x10 + x); /* R */
        d[x * 4 + 1] = (uint8_t)(0x80 + x);              /* G */
        d[x * 4 + 2] = (uint8_t)(0xF0 - row);            /* B */
        d[x * 4 + 3] = 0xFF;                             /* A */
    }
    return 12;
}

int main(int argc, char **argv)
{
    char res[2048];
    char *ok_args[] = {"screenshot", "ux0:data/vitacompanion/shot.bmp"};
    char *bad_args[] = {"screenshot", "ux0:data/../tai/config.bmp"};
    char *txt_args[] = {"screenshot", "ux0:data/x.txt"};
    char *ur0_args[] = {"screenshot", "ur0:tai/x.bmp"};
    char *fail_args[] = {"screenshot", "ux0:data/vitacompanion/fail.bmp"};
    (void)argc;
    out_dir = argv[1];

    cmd_screenshot(ok_args, 2, res);
    printf("ok:%s", res);
    cmd_screenshot(bad_args, 2, res);
    printf("dotdot:%s", res);
    cmd_screenshot(txt_args, 2, res);
    printf("txt:%s", res);
    cmd_screenshot(ur0_args, 2, res);
    printf("ur0:%s", res);
    fail_row = 0;
    cmd_screenshot(fail_args, 2, res);
    printf("fail:%s", res);
    fail_row = -1;
    no_frame = 1;
    cmd_screenshot(ok_args, 1, res);
    printf("noframe:%s", res);
    return 0;
}
"""


class ScreenshotTests(unittest.TestCase):
    def test_writes_bottom_up_bgr_bmp_and_guards_paths(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            include = tmp / "include"
            (include / "psp2" / "io").mkdir(parents=True)
            (include / "psp2" / "io" / "fcntl.h").write_text(FCNTL_H)
            (include / "psp2" / "io" / "stat.h").write_text(STAT_H)
            harness = tmp / "harness.c"
            harness.write_text(textwrap.dedent(HARNESS))
            exe = tmp / "harness"
            out = tmp / "out"
            out.mkdir()
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                 "-I", str(include), "-I", str(ROOT / "include"),
                 "-I", str(ROOT / "src"),
                 str(ROOT / "src" / "screenshot.c"), str(harness),
                 "-o", str(exe)],
                check=True, cwd=ROOT,
            )
            result = subprocess.run([str(exe), str(out)], check=True,
                                    capture_output=True, text=True)
            lines = dict(line.split(":", 1) for line in result.stdout.splitlines())

            self.assertTrue(lines["ok"].startswith("Saved ux0:data/vitacompanion/shot.bmp 3x2"))
            self.assertIn("path must be a .bmp under ux0:data/", lines["dotdot"])
            self.assertIn("path must be a .bmp under ux0:data/", lines["txt"])
            self.assertIn("path must be a .bmp under ux0:data/", lines["ur0"])
            self.assertIn("capture failed", lines["fail"])
            self.assertIn("no capturable framebuffer (-2)", lines["noframe"])

            files = sorted(p.name for p in out.iterdir())
            # The failed capture leaves neither its file nor its .tmp.
            self.assertEqual(files, ["ux0_data_vitacompanion_shot.bmp"])
            bmp = (out / files[0]).read_bytes()

            self.assertEqual(bmp[:2], b"BM")
            size, offset, dib, width, height, planes, bpp = struct.unpack_from(
                "<I4xIIiiHH", bmp, 2)
            row_bytes = 12  # 3 px * 3 B = 9, padded to 12
            self.assertEqual((offset, dib, width, height, planes, bpp), (54, 40, 3, 2, 1, 24))
            self.assertEqual(size, 54 + row_bytes * 2)
            self.assertEqual(len(bmp), size)

            def pixel(file_row, x):
                base = 54 + file_row * row_bytes + x * 3
                return tuple(bmp[base:base + 3])

            # File row 0 is the bottom screen row (row 1); bytes are B, G, R.
            self.assertEqual(pixel(0, 0), (0xF0 - 1, 0x80, 0x20))
            self.assertEqual(pixel(0, 2), (0xF0 - 1, 0x82, 0x22))
            self.assertEqual(pixel(1, 1), (0xF0, 0x81, 0x11))
            self.assertEqual(bmp[54 + 9:54 + 12], b"\0\0\0")

    def test_kernel_exports_and_command_are_wired(self):
        yml = (ROOT / "kernel" / "vitacompanion_kernel.yml").read_text()
        cmake = (ROOT / "CMakeLists.txt").read_text()
        cmds = (ROOT / "src" / "cmd_definitions.c").read_text()
        header = (ROOT / "include" / "vitacompanion_kernel.h").read_text()
        kernel = (ROOT / "kernel" / "screen.c").read_text()

        for name in ("vitaCompanionKernelScreenInfo", "vitaCompanionKernelScreenReadRow"):
            self.assertIn(name, yml)
            self.assertIn(name, header)
        self.assertIn("kernel/screen.c", cmake)
        self.assertIn("SceDisplayForDriver_stub", cmake)
        self.assertIn("src/screenshot.c", cmake)
        self.assertIn('.name = "screenshot"', cmds)
        self.assertIn("VITACOMPANION_KERNEL_ABI_VERSION 4", header)
        # Syscalls keep to four arguments (the fifth is lost on device).
        self.assertIn("int vitaCompanionKernelScreenReadRow(uint32_t row, void *user_dst,\n    uint32_t dst_size)", kernel)
        self.assertIn("ksceKernelCopyFromUserProc(info.pid", kernel)
        self.assertIn("ksceKernelCopyToUser(user_dst", kernel)


if __name__ == "__main__":
    unittest.main()
