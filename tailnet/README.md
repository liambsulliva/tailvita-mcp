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
- The newlib heap size is left at the default, the same as the demo.
