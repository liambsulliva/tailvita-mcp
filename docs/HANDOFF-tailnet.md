# Handoff: tailnet listener for the Vita

Last updated 2026-11-18. Read this top to bottom before acting. Repo: `github.com/liambsulliva/tailvita-mcp` (public, `main`).

## 1. Goal

Build a working tailnet listener for the PS Vita. Tailnet peers should reach the companion's FTP service (port 1337) and command service (port 1338) through the Vita's tailnet address, without being on the LAN.

Liam asked for a complete listener, not a config flag. Done means a tailnet peer gets real replies from both ports, and the steps are reproducible from this repo.

## 2. Current state (verified 2026-11-18)

### Repository

| Path | Contents | Status |
| --- | --- | --- |
| `vitacompanion/` | Companion firmware: FTP, command port, kernel and user plugins. History imported from the `liambsulliva/vitacompanion-tail` fork, which tracks `devnoname120/vitacompanion`. | Imported, 55 commits, unchanged from upstream 1.10 |
| `tailnet/` | The listener. | Not started. This is the work. |
| `ps-vita-mcp/` | devloop and resident MCP servers. | Pending import from the Mac |
| `vitacompanion-slim/` | Liam's memory-reduced companion mods and the vitacompanion MCP server. | Pending import from the Mac |
| `docs/` | This plan. | |

### Vita

- Companion modules are loaded from `ur0:/tai/` (per `ur0:/tai/config.txt`): `vitacompanion_kernel.skprx` and `vitacompanion.suprx`. The last reading was companion version 1.08.
- The tailscale-vita demo is installed as `TVIT00010` at `ux0:/app/TVIT00010`. It has joined the tailnet as host `vita` at `100.96.148.83`. Its state lives in `ux0:/data/tailscale-vita/`, which holds the node keys and `config.toml`.
- `config.toml` holds the Tailscale auth key in plain text. Never print or commit it.
- The demo runs only in the foreground. Nothing runs in the background yet.
- Plugins loaded by SceShell include QuickMenuReborn, shellsecbat, vita_resident and the VitaDB daemon. They share the memory budget.

### Mac

- Build tools are installed: rustup, `nightly-2026-06-01` with `rust-src`, `cargo-vita`, and VitaSDK packages freetype, libpng, zlib and bzip2.
- The Mac is a tailnet node at `100.97.211.40`.
- ps-vita-mcp and vitacompanion-slim exist only on the Mac, under `~/Developer/`. `mcp.json` (in the `dotagents` repo) references them by absolute path.

### Network

- The NAS reaches the Mac over Tailscale.
- The NAS can't reach the Vita's tailnet address on port 8080. Not diagnosed. The NAS may not be a tailnet node.

## 3. The listener

### Why no companion change is needed

The companion binds its services to `INADDR_ANY` (`vendor/libftpvita/ftpvita.c`, `src/net.c`). The Vita can reach them over loopback at `127.0.0.1:1337` and `127.0.0.1:1338`. A forwarder running on the Vita can therefore bridge tailnet connections to them without changing the companion.

### Design

- The listener is a Rust app in `tailnet/`. It uses the tailscale-vita crates: `tailscale-vita` for the runtime and `netstack` for TCP. Depend on them as a git dependency on `koalabuttz/tailscale-vita`, pinned to rev `88a992c`.
- Copy `rust-toolchain.toml` (`nightly-2026-06-01`) into `tailnet/`.
- The listener follows the demo's pattern. Read `tailscale-vita-demo/src/main.rs`: `run_supervised(config, should_stop, setup)`, `AcceptSession`, and `accept_loop`. The accept loop uses `netstack::TcpListener::bind(stack, port, pool)` and `accept_timeout`.
- For each accepted tailnet connection on port N, open `std::net::TcpStream` to `127.0.0.1:N`. Run two copy threads, one in each direction. Close both sides when either side hits EOF or an error.
- Limits, to protect the Vita's memory:
  - Cap concurrent connections per port. Suggested: 2 for 1337 and 4 for 1338.
  - Use 256 KiB thread stacks, as the demo does.
  - Keep socket buffers small. The bridge README reports that heavy socket use exhausted SceShell's network memory and dropped Wi-Fi for minutes.
  - Add an idle timeout on each connection.
- Reuse the existing node identity by pointing `state_dir` at `ux0:/data/tailscale-vita`. That way the listener needs no new auth key.
- Config: a separate file, e.g. `ux0:/data/tailnet-listener/config.toml`. It sets `state_dir`, `control_url`, and the port map.
- Package with a new title ID, e.g. `TVIT00011`, in `[package.metadata.vita]`.
- Note: tailscale-vita already has an FTP server on the tailnet (`[ftp] enabled = true`, which needs a password). Decide whether the 1337 forwarder is still worth it. The command port (1338) has no tailnet path otherwise.

### Build

Build on the Mac through `mac-run`, from inside the checkout:

```sh
~/.dotagents/bin/mac-run --get tailnet/target/armv7-sony-vita-newlibeabihf/release/<name>.vpk \
  'export PATH="$HOME/.cargo/bin:$PATH" && export TARGET_AR=arm-vita-eabi-ar && cd tailnet && cargo vita build vpk -p <name> --release'
```

`TARGET_AR` is required. Without it, ring's build script archives its C objects with Apple's `ar` and the link fails with undefined `ring_core_*` symbols.

Do not build on the NAS.

### Test

- From the Mac, through `mac-run`, which is a tailnet peer:
  - Command port: `printf 'version\n' | nc -w 5 100.96.148.83 1338`
  - FTP: `curl ftp://100.96.148.83:1337/ux0:/`
- Test from the NAS only after confirming the NAS is a tailnet node.
- The listener runs only while its app is open. Shared node identity means the demo and the listener can't run at the same time.

## 4. Blockers and decisions

- **vitacompanion-slim source.** Not on GitHub. It exists only on the Mac, so the slim-sync phase is blocked. Ask Liam for a repo URL, or for permission to bundle it from the Mac.
- **Memory target.** No runtime memory readout exists. Resident status has no memory fields, and `ur0:/tai` is the only tai path that FTP lets us read. Ask for a target in MB. A useful measurement needs a heap-stats command in the companion. That is a companion change, so it needs a reboot.
- **NAS on the tailnet.** Unverified. Test before promising NAS access.
- **Upstream packet filter denies all inbound (found 2026-11-18).** Phase 1 is done and the listener comes online as `100.96.148.83`, bound to 1337 and 1338. Tailnet TCP and ICMP still get no reply, from the demo as well as the listener. Tailscale sends this tailnet's allow-all rule with `SrcIPs` as IP ranges (`100.64.0.0-100.115.91.255`, `100.115.94.0-100.127.255.255`) plus an IPv6 prefix. `filter_rule_from_wire` in `ts-control/src/netmap.rs` at `88a992c` parses only `*`, single IPs and CIDRs, so it drops the rule. The result is `rule_count=0`, which is `Rules([])`, which denies everything. The denial is logged only at trace level. Fixing it means changing the pinned dependency, by a fork or a local `[patch]`. That is Liam's call.
- **Deleting `liambsulliva/vitacompanion-tail`.** Now that `tailvita-mcp` is on GitHub, the fork can go. That is Liam's call.

## 5. Phases

1. **Listener build.** Create `tailnet/`, build the VPK on the Mac, and fetch it back. Acceptance: the build passes on the Mac and the VPK is fetched into the checkout.
2. **Deploy and test.** Install the VPK with `vita_install_vpk`, using the Mac-side path from the run folder. Launch it with `vita_command` (`launch <TITLEID>`). Check its log. Test from the Mac as in §3. Acceptance: the command port returns a version and the FTP port lists `ux0:/`.
3. **Slim sync.** Blocked until the slim source is available. Then bring `vitacompanion/` in line with it, one commit per concern. Host tests run via `python3 -m unittest discover -s tests`.
4. **Memory and companion modules.** Measure with the demo and the FTP server running. Add a heap-stats command if needed. Push the new modules to `ur0:/tai/` with `vita_ftp_upload`, using Mac-side paths. Liam reboots, then re-test.
5. **Hand back.** Report commit SHAs, build outputs, test results, measured memory, and open issues. Leave no secrets in the repo or the scratch folder.

## 6. Rules

- Commit locally as Liam Sullivan `<75332524+liambsulliva@users.noreply.github.com>`. Never add `Co-Authored-By`, `Claude-Session`, or AI attribution.
- Liam has authorized pushing to `tailvita-mcp`. Pushing to any other repo needs his approval.
- Installs need Liam's OK first. Use `DOTAGENTS_MAC_OK=1` only after he says yes.
- Don't restart the MCP servers and don't use `mac-run` to reach their state. They run on the Mac.
- Don't reboot the Vita. Liam does that.
- Get tokens through `request_secret`, never in chat. A GitHub token with repo scope is already saved as a T3 secret for the repo-creation step. Never write any token into this repo.
- Vita tools (`vita_install_vpk`, `vita_ftp_upload`) take Mac-side paths, not NAS paths. The MCP runs on the Mac.

## 7. Background: the MCP setup

- The current setup is three MCP servers on the Mac, configured in `dotagents/claude/mcp.json`:
  - devloop: `ps-vita-mcp/bridge/server.py`
  - resident: `ps-vita-mcp/resident/bridge.py`
  - vitacompanion: `vitacompanion-slim/tools/mcp_server.py`
- The "MCP Loop: Metroid Prime Bugfixing" thread named as the source of truth was not found under that title. The `mcp.json` config and the tool lists were used instead.
- **vita-agent-bridge** (`hotdog-face/vita-agent-bridge`, MIT) was assessed as worse as a replacement. It duplicates screen capture and file access that devloop and resident already provide. It adds a kernel hook that can't be unloaded. Its protocol is plaintext behind a single token. Its only tested firmware is 3.65. Its real advantages are file access while a game runs, SHA-256 checked uploads, and reboot from SceShell. Trial it on a spare console, not the working one.

## 8. Pitfalls

- `ux0:/tai` is closed to FTP. `ur0:/tai` is readable and holds the loaded modules.
- `ux0:/data/vitacompanion/` holds unused copies of the modules.
- A replaced module stays resident until the Vita reboots.
- The NAS-to-Vita tailnet route is unverified.
- The Vita's node identity is shared between the demo and any listener, so only one can run at a time.
- The demo's FTP-over-tailnet is off by default. Its config is in `ux0:/data/tailscale-vita/config.toml`.
