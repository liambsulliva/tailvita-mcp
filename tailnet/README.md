# tailnet-listener (TVIT00011)

A Vita app that joins the tailnet with the node identity of the
tailscale-vita demo (TVIT00010) and forwards tailnet TCP connections to the
companion's loopback services:

| Tailnet port | Target | Kind | Concurrent connections |
| --- | --- | --- | --- |
| 1337 | `127.0.0.1:1337` (FTP) | `ftp` | 2, plus 2 passive data connections |
| 1338 | `127.0.0.1:1338` (command port) | `tcp` | 4 |

It runs only while the app is open, and it has no UI. The demo and the
listener share one node identity, so only one of them can run at a time.

Built on `koalabuttz/tailscale-vita` at rev `88a992c` (`run_supervised`, a
per-incarnation accept session, and `netstack::TcpListener`), following
`crates/tailscale-vita-demo/src/main.rs`.

## Build (on the Mac)

```sh
~/.dotagents/bin/mac-run --get tailnet/target/armv7-sony-vita-newlibeabihf/release/tailnet-listener.vpk \
  'export PATH="$HOME/.cargo/bin:$PATH" && export TARGET_AR=arm-vita-eabi-ar && cargo vita build vpk -p tailnet-listener --release'
```

Host unit tests: `~/.dotagents/bin/mac-run 'export PATH="$HOME/.cargo/bin:$PATH" && cargo test --release'`.

Two build details:

- `TARGET_AR=arm-vita-eabi-ar` is required. Without it, ring's C objects are archived with Apple's `ar` and the link fails.
- `build.rs` passes `--defsym,__sce_headroom=0x1000`. Without it, `vita-elf-create` can fail with "segment 1 overlaps" whenever the text segment happens to end within ~2 KiB of a 64 KiB boundary.

## Config

`ux0:data/tailnet-listener/config.toml` is written with defaults on first
launch. It holds no secrets: control settings and the auth key are read
from the demo's `ux0:/data/tailscale-vita/config.toml` and kept in memory.
The tailscale-vita FTP, Taildrop and egress-probe services are turned off
unless `keep_tailscale_services = true`. See `TEMPLATE` in
[`src/config.rs`](src/config.rs) for every key.

The log is `ux0:data/tailnet-listener/listener.log`: 1 MiB, with 2 rotated
files. A `listener.status` line every 60 s gives the active connection
counts and free user, CDRAM and phycont memory.

## How forwarding works

- Each connection uses two threads. A pump thread owns the netstack stream,
  polling it with a 30 ms read timeout. A reader thread blocks on the
  loopback socket and hands 4 KiB chunks to the pump over a 2-slot channel.
  netstack streams can't be cloned, and their `read` holds `&mut self` for
  the whole wait, so a lock shared by two copy threads would stall writes.
- netstack has no half-close, so EOF or an error on either side closes both.
  So do the idle timeout (300 s, or 60 s for FTP data) and a runtime relogin.
  An FTP control connection doesn't idle out while one of its data
  transfers is running, so slow multi-minute downloads keep their final reply.
- FTP: the companion picks an ephemeral passive port for each transfer, so a
  plain port forward can't carry the data connection. The 1337 forwarder
  reads the control channel line by line. For each `227`/`229` reply it binds
  the same port on the tailnet, accepting only the control peer's address,
  and rewrites the `227` address to the tailnet IP. `PORT`/`EPRT` are
  refused with `502`, because the companion can't dial a tailnet peer.

## Memory

- Thread stacks: 256 KiB for the runtime, 64 KiB for accept, 64 KiB for each connection pump, 32 KiB for each loopback reader.
- netstack fixes each tailnet socket at 16 KiB rx + 16 KiB tx (`netstack::buf`, not configurable at this rev).
- Each port keeps `listen_pool` sockets listening (default `max_conns`, at most 4): that many
  connections can arrive at the same moment. The defaults hold 2 on 1337 and 4 on 1338,
  6 × 32 KiB in all.
- The newlib heap size is left at the default, the same as the demo (app build).

## Background service (`daemon/`, TVIT00013 and TVIT00014)

The same listener, packaged so it keeps running with no app open and beside
any game. `daemon/build.sh` builds one VPK with one bubble, **Tailnet
Daemon**:

| Part | What it is |
| --- | --- |
| `eboot.bin` (TVIT00013) | `launcher.c`. Opened normally it starts the service and exits; it has no screen. Opened with **L held** it stops the service. |
| `eboot2.bin` (TVIT00014) | The listener built with `--features daemon`: a 2 MiB newlib heap inside a 15 MiB SELF budget. |
| `tailnet_shell.suprx` | `shell_module.c`, loaded into SceShell at run time by the launcher. |
| `tailnet_capture.skprx` | `capture_kernel.c`, a kernel helper loaded at run time by the launcher. It reads the frame on screen for the screenshot port. |

Nothing is written under `ur0:tai` and nothing loads at boot: after a
reboot, open the bubble once.

### Staying up beside a game

The Vita ends every background service when an enlarged-memory game starts
(Ghost Trick is one), and SceShell then refuses to start one beside the game.
The SceShell module does two things about that. Its launch hook (the one
GrapheneCt's LowMemMode installs) makes the refused start pass. Its watchdog
checks every 5 s and starts the service again once it has been gone for
15 s, while `ux0:data/tailnet-listener/keepalive` exists; the launcher writes
that file on a normal start and removes it on an L-held stop. Measured: the
tailnet is back about 25 s after a game launch, with nobody touching the
Vita.

The app (TVIT00011) and the service share one node key and one log, so only
one may run: each binds `127.0.0.1:41338` and the second one exits with
`listener.already_running`. Close the app before starting the service. The
launcher's and the SceShell module's steps go to
`ux0:data/tailnet-listener/launcher.log`.

### Screenshots (port 1339)

The system screenshot (PS + Start) is no use beside a game: taken while an
enlarged-memory game is loaded, in front or suspended, it makes the system
end the service every time. PS alone does not, and neither does a system
screenshot with no game loaded.

So the service has its own screenshot port, `capture_port` (1339), on the
LAN and the tailnet. Connect, optionally send `jpeg <quality>` (1-100,
default 80), and read `OK jpeg <width> <height>` followed by the JPEG until
the connection closes, or `ERR <reason>`. The frame goes from the screen to
the socket; nothing is stored on the Vita. `~/.dotagents/bin/vita screenshot`
uses this port first.

How it works: `tailnet_capture.skprx` asks the display driver which
framebuffer is on screen and copies it out of the owning process into a
2 MiB block the service holds only while a picture is being sent
(`src/capture.rs`). The service encodes it 16 rows at a time.

Two things learned the hard way:

- A process can only call the helper if it started after the helper was
  loaded. The launcher loads it and must not call it; it writes
  `capture2-ready` (boot time and SceShell's pid) so it loads the helper
  once per boot, and the service offers the port only when that file exists.
- A small negative return value from a syscall reaches the caller with bit
  30 cleared (-7 arrived as `0xBFFFFFF9`), so the helper returns SCE-style
  `0x80F7xxxx` codes. Its first version also asked the kernel for a 2 MiB
  block and was refused; the current one allocates nothing.

A kernel module cannot be replaced without a reboot. If the helper's
interface changes, bump the `2` in `tailnet_capture.yml` and in
`capture2-ready`, so the new one loads beside the old.

### Install and stop

`vita install tailnet/daemon/build/tailnet-daemon.vpk` works on the LAN (6 s)
and over the tailnet (about 7 minutes at a hotspot's 5 KB/s). The Vita
refuses to replace the package while its service runs, and over the tailnet
that service is the link itself, so `vita` sends `quit TVIT00014; wait
3000ms; promote <folder>` as one command: the Vita finishes it alone and the
watchdog starts the new service. Reinstall after every rebuild; the system
binds the service's SELF at install time.

The SceShell module and the kernel helper are only replaced by a reboot
followed by opening the bubble. `vita launch TVIT00013` opens it too, but
only from the home screen (with a game in front the launch is ignored).

Files that switch things off, in `ux0:data/tailnet-listener/`:
`no-shell-module` (the launcher leaves SceShell alone), `no-capture` (the
kernel helper is not loaded), `stop` (the service exits; the launcher
removes it on a normal start).

If something goes wrong: the bubble's parts live only in memory, so a
restart of the Vita removes the SceShell module and the kernel helper, and
nothing starts again until the bubble is opened. To keep it from starting,
do not open the bubble, or delete the bubble.

### Measured on the Vita (2026-11-20)

- On LiveArea with no app open, the tailnet answers 12-20 s after the bubble
  is opened.
- With Ghost Trick in front, over the tailnet: `status`, a command, a file
  pull and a screenshot all work, and the service stays up. A screenshot is
  88-128 KB and takes 0.5 s on the LAN and 1.3-3 s over the tailnet.
- Inside a 16 MiB budget with a 6 MiB heap the service had 3.0 MiB free at
  start and 2.0 MiB once every thread was up; the heap peaked at 1.06 MiB
  used. Ghost Trick's own log showed 170 MB of its main memory free with the
  service running.
- On a phone hotspot with Ghost Trick in front, the same checks pass at
  about 5 KB/s: a screenshot takes 14 s (71 KB at quality 70) or 23 s
  (103 KB at 85), and a 200 KB upload and download each take about 40 s and
  come back identical.
- A soak on the home network ran 15 minutes clean (a check every minute, two
  identical screenshots); the heap stayed at 0.97 MiB used with a 1.16 MiB
  peak. It was then cut short when the Vita changed networks, so the
  one-hour soak is still to do.
- One start took 110 s to come online instead of 20: Tailscale's control
  server answered the map request after 82 s. Not seen again.

Open:

- Twice the service ended and then every start by the watchdog reported
  success but the service never came up (no log line, no crash dump), 6
  times the first time and 35 the second, until the bubble was opened
  again. Both followed something being done on the Vita by hand (a crash
  dialog the first time, a change of Wi-Fi network the second). The likely
  cause is that the service only runs while the launcher's LiveArea card is
  open: closing that card is the system's own way to end a background
  service. Not confirmed. The watchdog now stops after 4 such starts (that
  module is replaced at the next reboot).
- Whether the service reconnects by itself after a change of network is
  untested: it was not running during the one change so far.
- The first time the bubble was opened after a game had ended the service,
  the Vita said its saved data was corrupt and offered to delete it.
  Answering yes let it start. Not seen since.
- Not yet tested: sleep and wake, and a full one-hour soak.
