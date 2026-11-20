# wg-engine patches

The source is `crates/wg-engine` from `koalabuttz/tailscale-vita` at rev
`88a992c` (MIT). `tailnet/Cargo.toml` swaps it in for the git copy with
`[patch."https://github.com/koalabuttz/tailscale-vita"]`.

Changes from upstream:

1. **Per-packet log lines are `debug!`, not `info!`** (`src/pump.rs`:
   `wg.net.rx`, `wg.tun.rx`, `wg.net.tx`). At `info` they wrote one line per
   packet, about 1 MiB to the memory card every 5 minutes of traffic.
   vita-log has one global level, so the only way to keep the listener's own
   `info` lines and lose these is to lower them here. Set `log_level =
   "debug"` in the listener config to see them again.
2. **Manifest.** Workspace inheritance is replaced with upstream's
   workspace values.

To refresh: copy `crates/wg-engine/src` from the new upstream rev and
re-apply change 1.
