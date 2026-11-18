# tailvita-mcp

Tools for driving a PS Vita with AI agents, reachable over a Tailscale tailnet.

## Layout

| Path | What it is | Status |
| --- | --- | --- |
| `vitacompanion/` | Companion firmware: FTP server, command port, kernel and user plugins. Forked from [devnoname120/vitacompanion](https://github.com/devnoname120/vitacompanion). History is kept under this path. | Imported |
| `tailnet/` | Tailnet listener that exposes the Vita's services on the tailnet. | Not started |
| `ps-vita-mcp/` | devloop and resident MCP servers. | Pending import from Mac |
| `vitacompanion-slim/` | Memory-reduced companion mods and the vitacompanion MCP server. | Pending import from Mac |

## Conventions

- Each component keeps its own build tool at its root: CMake for `vitacompanion/`, Cargo for Rust crates, and `uv` for Python servers.
- Each subtree keeps its upstream license and notices.
- Secrets never go in the repo: auth keys, device tokens and `*.priv` files are ignored by `.gitignore`.
