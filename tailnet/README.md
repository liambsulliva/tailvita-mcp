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
a game. `daemon/build.sh` builds one VPK with one bubble, **Tailnet Daemon**:

| Part | What it is |
| --- | --- |
| `eboot.bin` (TVIT00013) | `launcher.c`. Opened normally it starts the service and exits; it has no screen. Opened with **L held** it stops the service. |
| `eboot2.bin` (TVIT00014) | The listener built with `--features daemon`: a 2 MiB newlib heap inside a 12 MiB SELF budget. |
| `tailnet_shell.suprx` | `shell_module.c`, loaded into SceShell at run time by the launcher. Nothing is written under `ur0:tai`; it is gone after a reboot. |

The Vita ends every background service when an enlarged-memory game starts
(Ghost Trick is one), and SceShell then refuses to start one beside the game.
The SceShell module does two things about that. Its launch hook (the one
GrapheneCt's LowMemMode installs) makes the refused start pass. Its watchdog
starts the service again once it has been gone for 15 s, while
`ux0:data/tailnet-listener/keepalive` exists; the launcher writes that file
on a normal start and removes it on an L-held stop.

The app (TVIT00011) and the service share one node key and one log, so only
one may run: each binds `127.0.0.1:41338` and the second one exits with
`listener.already_running`. Close the app before starting the service. The
launcher's steps go to `ux0:data/tailnet-listener/launcher.log`.

Install it from VitaShell (`ux0:/vpk/tailnet-daemon.vpk`), and reinstall
after every rebuild: the system binds the service's SELF at install time.

Measured on the Vita on 2026-11-20, first build (16 MiB budget, 6 MiB heap,
started by hand, no watchdog):

- On LiveArea with no app open, the tailnet answered 20 s after the bubble
  was opened.
- Starting Ghost Trick ended the service (its log stops there). Opening the
  bubble again with the game suspended started it beside the game, and with
  the game in front `status`, a command and a file pull all worked. Ghost
  Trick's own log showed 170 MB of main memory free at the time.
- Inside its budget the service had 3.0 MiB free at start and 2.0 MiB once
  every thread was up; the heap peaked at 1.06 MiB used.
- The system screenshot (PS + Start, sent through the command port) fired,
  and the service died within a second of it. No crash dump was written, so
  the system ended it. Cause not yet known; the smaller budget and the
  watchdog in the current build are the first two things to try.
- The first time the bubble was opened after the game had ended the
  service, the Vita said its saved data was corrupt and offered to delete
  it. Answering yes let it start. Cause not yet known.

Not yet run on the Vita: the 12 MiB budget, the watchdog, and the L-held
stop.

