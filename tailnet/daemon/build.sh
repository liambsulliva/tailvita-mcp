#!/bin/bash
# Build the Tailnet Daemon package: one bubble, two programs.
#
#   eboot.bin   launcher (launcher.c): loads the SceShell module and the
#               kernel capture helper, starts the service, exits.
#   eboot2.bin  the service: tailnet-listener built with `--features daemon`,
#               in a SELF that carries a memory budget.
#
# The layout and the SFO pair follow GrapheneCt's BGFTP, by way of the
# tailscale-vita M21 spike (sfo_make.py, reference/, assets/ are from there).
#
# Run on the NAS:  nice -n 15 ./build.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TAILNET="$(dirname "$HERE")"
export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:$HOME/.cargo/bin:$PATH"
export TARGET_AR="${TARGET_AR:-arm-vita-eabi-ar}"
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-2}"

OUT="$HERE/build"
mkdir -p "$OUT"

LAUNCHER_ID="TVIT00013"; LAUNCHER_TITLE="Tailnet Daemon"
SERVICE_ID="TVIT00014";  SERVICE_TITLE="Tailnet Service"
CONTENT_TAIL="TAILNETDAEMON000"

# The service's budget, in KiB. Measured on the Vita with a 6 MiB heap and a
# 16 MiB budget: 3.0 MiB free at start, 2.0 MiB free once every thread was
# up, so everything but the heap takes about 8.2 MiB (image, thread stacks,
# network memory), plus 96 KiB per open connection. With the 2 MiB heap
# (src/main.rs) that is about 11 MiB at full load; 12 MiB leaves 1 MiB spare.
# A background service is the first thing the system ends when memory runs
# short beside an enlarged-memory game, so smaller is safer.
MEMSIZE_KB=12288
ATTRIBUTE=3

echo "==> launcher, SceShell module, kernel capture helper"
cmake -S "$HERE" -B "$OUT/cmake" >/dev/null
cmake --build "$OUT/cmake" -j2 >/dev/null

echo "==> service: cargo vita build (features: daemon)"
# Its own target dir, so the app build's artifacts are not overwritten.
(cd "$TAILNET" && TAILNET_DAEMON_STUBS="$OUT/cmake" cargo vita build velf -p tailnet-listener --release \
    --features daemon --target-dir "$TAILNET/target/daemon")
VELF="$TAILNET/target/daemon/armv7-sony-vita-newlibeabihf/release/tailnet-listener.velf"
vita-make-fself -c -m "$MEMSIZE_KB" -at "$ATTRIBUTE" "$VELF" "$OUT/eboot2.bin"

echo "==> check the budget landed in the SELF"
python3 - "$OUT/eboot2.bin" "$ATTRIBUTE" "$MEMSIZE_KB" <<'PY'
import struct, sys
path, attr, memsize = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
data = open(path, "rb").read()
at = data.find(struct.pack("<4I", 1, attr, 0, memsize))
if at < 0:
    sys.exit(f"{path}: control info 6 lacks ATTRIBUTE={attr} MEMSIZE={memsize} KiB")
print(f"    ATTRIBUTE={attr} MEMSIZE={memsize} KiB at {at:#x}")
PY

echo "==> param.sfo / param2.sfo"
python3 "$HERE/sfo_make.py" --ref "$HERE/reference" --out "$OUT" \
  --gdc-id "$LAUNCHER_ID" --gdc-title "$LAUNCHER_TITLE" \
  --gdd-id "$SERVICE_ID" --gdd-title "$SERVICE_TITLE" \
  --tail "$CONTENT_TAIL"

echo "==> vpk"
rm -f "$OUT/tailnet-daemon.vpk"
vita-pack-vpk \
  -s "$OUT/param.sfo" \
  -b "$OUT/cmake/eboot.bin" \
  -a "$OUT/eboot2.bin=eboot2.bin" \
  -a "$OUT/param2.sfo=sce_sys/param2.sfo" \
  -a "$OUT/cmake/tailnet_shell.suprx=tailnet_shell.suprx" \
  -a "$OUT/cmake/tailnet_capture.skprx=tailnet_capture.skprx" \
  -a "$HERE/assets/icon0.png=sce_sys/icon0.png" \
  -a "$HERE/assets/bg0.png=sce_sys/livearea/contents/bg0.png" \
  -a "$HERE/assets/startup.png=sce_sys/livearea/contents/startup.png" \
  -a "$HERE/assets/template.xml=sce_sys/livearea/contents/template.xml" \
  "$OUT/tailnet-daemon.vpk"
ls -l "$OUT/tailnet-daemon.vpk"
