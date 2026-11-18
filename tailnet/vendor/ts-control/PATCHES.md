# ts-control patches

The source is `crates/ts-control` from `koalabuttz/tailscale-vita` at rev
`88a992c` (MIT). `tailnet/Cargo.toml` swaps it in for the git copy with
`[patch."https://github.com/koalabuttz/tailscale-vita"]`. Every other
tailscale-vita crate still comes from the pinned revision.

Changes from upstream:

1. **Packet-filter IP ranges** (`src/netmap.rs`). Tailscale's control plane
   sends the default ACL's sources as inclusive ranges, for example
   `100.64.0.0-100.115.91.255` and `100.115.94.0-100.127.255.255`. Upstream
   parsed only `*`, single IPv4 addresses and CIDRs, so it dropped the
   whole rule. That left `PacketFilter::Rules([])`, which denies all
   inbound traffic: tailnet peers got no TCP or ICMP replies, while disco
   pings still worked. `parse_filter_addrs` now turns each range into the
   minimal set of CIDRs, for sources and destinations alike. IPv6 and
   malformed entries are still ignored, so a malformed rule still can't
   become an allow.
2. **Manifest.** Workspace inheritance is replaced with upstream's
   workspace values. `examples/host_diagnostic.rs` and its tracing
   dev-dependencies are left out.

To refresh: copy `crates/ts-control/src` from the new upstream rev, re-apply
change 1 (or drop it if upstream has fixed range parsing), and update the
versions in `Cargo.toml` from upstream's `[workspace.dependencies]`.
