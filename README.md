# tailvita-mcp

Tools for driving a PS Vita with AI agents, reachable over a Tailscale tailnet.

## How it fits together

```
agent on liams-nas ── dotagents bin/vita ──┬── LAN 10.0.0.151 ─────────────┐
                                           └── tailnet 100.96.148.83 ──────┤
                                               (tailnet listener TVIT00011) │
                                                                            ▼
                                      VitaCompanion 1.08 on the Vita: FTP :1337, commands :1338
```

- **VitaCompanion** is a taiHEN plugin pair loaded at boot from `ur0:tai/` (`vitacompanion_kernel.skprx` under `*KERNEL`, `vitacompanion.suprx` under `*main`). It serves FTP on port 1337 and a line-based command port on 1338 (`launch`, `quit`, `press`/`release`/`wait` synthetic input, `promote`, `screen`, `nosleep`, `reboot`, `version`).
- **The tailnet listener** (`tailnet/`, title ID `TVIT00011`) joins the tailnet as host `vita` and forwards tailnet ports 1337 and 1338 to the companion on loopback. It works only while it is the foreground app, and it shares its node identity with the tailscale-vita demo (`TVIT00010`), so only one of them runs at a time.
- **The agent side** lives in [dotagents](https://github.com/liambsulliva/dotagents): `bin/vita` (status, commands, FTP, install, launch, screenshot, logs), `bin/vita-loop`, and the `vita-install`, `vita-launch`, `vita-ss` and `vita-log` skills. `bin/vita` tries the LAN first and falls back to the tailnet.

## Current state

| Path | What it is | State |
| --- | --- | --- |
| `vitacompanion/` | Companion plugins: FTP server, command port, kernel and user modules. Forked from [devnoname120/vitacompanion](https://github.com/devnoname120/vitacompanion), with history kept under this path. | `main` holds 1.10 plus the slim memory knobs. **The Vita runs 1.08** (commit `a997d7d`); 1.09 and later have never run on it. |
| `tailnet/` | Tailnet listener `TVIT00011`. See [tailnet/README.md](tailnet/README.md). | Working from the Mac and the NAS. |
| `tailnet/vendor/ts-control/` | ts-control from tailscale-vita `88a992c`, patched so packet-filter source ranges like `100.64.0.0-100.115.91.255` (Tailscale's default ACL) parse. Without it every tailnet packet is dropped. See its `PATCHES.md`. | Vendored. |
| `vitacompanion-slim/` | Paul's memory-reduced companion and its MCP server, imported from the Mac. | Reference; its memory knobs are ported to `vitacompanion/`. |
| `ps-vita-mcp/` | devloop and resident MCP servers. | Not imported yet (still on the Mac). |

### The device

- Companion **1.08**, built from `a997d7d`. Rebuilding that commit on the Mac gives a kernel module byte-identical to the one on the Vita. Its user module also needs VitaSDK's `taipool` library (`vdpm taipool`), which isn't on the Mac yet.
- The Vita reaches the tailnet through the listener only. Over the tailnet, ping takes 25–400 ms and FTP downloads run at about 25 KB/s; the LAN gives about 500 KB/s.
- A backup of the running 1.08 pair is in `ux0:data/tmp/companion-1.08/`.

### Screenshots

Screenshots are the Vita's own: `bin/vita screenshot` fires hold-PS-press-Start through the companion's synthetic input, waits for the new image under `ux0:/picture/SCREENSHOT/<app>/`, pulls it and deletes it on the Vita. This needs no plugin change. An app that blocks system screenshots gives no image.

## Rules for deploying companion modules

On 2026-11-18 a 1.10 build with a kernel screenshot command was deployed over 1.08 and bootlooped the Vita (spinning wheel, freeze, power off). It was recovered by booting without plugins and restoring the 1.08 pair through VitaShell's FTP. Until the cause is known:

1. Build on the version the Vita runs. Check `vita cmd version` first, and stop if it differs from the source you built.
2. Back up the running pair to `ux0:data/tmp/` and check the hashes before writing to `ur0:tai/`.
3. Deploy the kernel and user modules together; the user module requires an exact kernel ABI version.
4. Liam approves every `ur0:tai/` write and does the reboot.

To recover from a bootloop: boot without plugins, open VitaShell, and copy the backup pair over `ur0:tai/` (by hand, or through VitaShell's FTP server).

## Branches

| Branch | Contents | State |
| --- | --- | --- |
| `main` | Everything above. | |
| `companion-screenshot` | Kernel screenshot command on 1.10 (kernel ABI 4). | **Do not deploy**: this pair bootlooped the Vita. Kept for reference. |
| `companion-screenshot-1.08` | The same command ported onto 1.08 (`a997d7d`). 43 host tests pass and the kernel module builds. | Not deployed. The user module needs `taipool`; the kernel side carries the same new code as the bootlooping build. |

## Roadmap

- **Kernel-level screenshots.** Capture the frame buffer from the companion's kernel module (`ksceDisplayGetProcFrameBufInternal` and a copy into user memory) and return it through a `screenshot` command, instead of the system combo. That works in apps that block system screenshots and leaves the photo folder alone. Start from `companion-screenshot-1.08`, find the bootloop's cause first, and test it without a boot-time load: load the kernel module once with taiHEN from a test app before anything goes in `ur0:tai/config.txt`.
- **Listener burst connections.** Each port accepts at most 2 new connections at once, so a burst of parallel connections to 1338 sees refusals. Raising 1338 to 4 costs about 64 KiB.
- **Tailnet throughput.** About 25 KB/s against 500 KB/s on the LAN; the limit looks like the Vita's userspace network stack, not the forwarding code.
- **Import `ps-vita-mcp/`** from the Mac.

## Conventions

- Each component keeps its own build tool at its root: CMake for `vitacompanion/`, Cargo for Rust crates, and `uv` for Python servers.
- Builds run on the Mac with `~/.dotagents/bin/mac-run`; the NAS takes only short builds when the Mac is unavailable. The Mac's VitaSDK has `libk` installed (needed by 1.10's user module).
- Each subtree keeps its upstream license and notices.
- Secrets never go in the repo: auth keys, device tokens and `*.priv` files are ignored by `.gitignore`. Never read or print `ux0:/data/tailscale-vita/config.toml`; it holds the Tailscale auth key.
