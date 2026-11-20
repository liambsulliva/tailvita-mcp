//! Listener config (`ux0:data/tailnet-listener/config.toml`).
//!
//! The listener reuses the node identity that the tailscale-vita demo
//! (TVIT00010) registered. It reads the demo's config for `control_url`,
//! the optional control key pin and the auth key, then applies its own
//! overrides. The auth key stays in memory and is never logged or written
//! anywhere by this crate.

use std::path::Path;

use serde::Deserialize;

/// Our own config file. sceIo writes only persist with the `ux0:data/...`
/// form (see vita-log's notes), so use it for the file we create.
pub const CONFIG_PATH: &str = "ux0:data/tailnet-listener/config.toml";
pub const CONFIG_DIR: &str = "ux0:data/tailnet-listener";

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum ForwardKind {
    /// Plain byte bridge.
    Tcp,
    /// Byte bridge plus an FTP control-channel helper that also forwards
    /// the passive data port the companion picks for each transfer.
    Ftp,
}

#[derive(Clone, Debug, Deserialize)]
pub struct Forward {
    /// Port on the Vita's tailnet address.
    pub port: u16,
    /// Port on 127.0.0.1. Defaults to `port`.
    #[serde(default)]
    pub target: Option<u16>,
    #[serde(default = "default_kind")]
    pub kind: ForwardKind,
    /// Concurrent tailnet connections allowed on this port.
    #[serde(default = "default_max_conns")]
    pub max_conns: usize,
    /// Listening sockets kept ready on this port: how many connections can
    /// arrive at the same moment. Each costs 32 KiB of netstack buffers.
    /// Defaults to `max_conns`, capped at `MAX_LISTEN_POOL`.
    #[serde(default)]
    pub listen_pool: Option<usize>,
}

/// Most listening sockets one port may keep ready (4 × 32 KiB).
pub const MAX_LISTEN_POOL: usize = 4;

impl Forward {
    pub fn target_port(&self) -> u16 {
        self.target.unwrap_or(self.port)
    }

    pub fn listen_pool(&self) -> usize {
        self.listen_pool.unwrap_or(self.max_conns).min(self.max_conns).min(MAX_LISTEN_POOL)
    }
}

#[derive(Clone, Debug, Deserialize)]
pub struct ListenerConfig {
    /// The tailscale-vita config to inherit control settings from.
    #[serde(default = "default_tailscale_config")]
    pub tailscale_config: String,
    /// Node identity directory. Shared with the demo by default.
    #[serde(default = "default_state_dir")]
    pub state_dir: String,
    /// Overrides the inherited control URL when set.
    #[serde(default)]
    pub control_url: Option<String>,
    /// Overrides the inherited hostname when set.
    #[serde(default)]
    pub hostname: Option<String>,
    #[serde(default = "default_log_level")]
    pub log_level: String,
    /// Keep the tailscale-vita FTP / Taildrop / egress-probe services the
    /// inherited config enables. Off by default to save memory.
    #[serde(default)]
    pub keep_tailscale_services: bool,
    /// Stop the Vita from auto-suspending (which drops Wi-Fi) while open.
    #[serde(default = "default_true")]
    pub keep_awake: bool,
    /// Close a forwarded connection after this long with no bytes either way.
    #[serde(default = "default_idle_timeout")]
    pub idle_timeout_secs: u64,
    /// Idle timeout for FTP passive data connections.
    #[serde(default = "default_data_idle_timeout")]
    pub data_idle_timeout_secs: u64,
    /// Concurrent FTP passive data connections, across all FTP sessions.
    #[serde(default = "default_max_data_conns")]
    pub max_data_conns: usize,
    /// Screenshot port of the background service (0 turns it off). The app
    /// build ignores it.
    #[serde(default = "default_capture_port")]
    #[cfg_attr(not(feature = "daemon"), allow(dead_code))]
    pub capture_port: u16,
    #[serde(default = "default_forwards", rename = "forward")]
    pub forwards: Vec<Forward>,
}

pub const TEMPLATE: &str = r#"# tailnet-listener config. No secrets here: the listener inherits the
# control settings and node identity of the tailscale-vita demo.

tailscale_config = "ux0:/data/tailscale-vita/config.toml"
state_dir        = "ux0:/data/tailscale-vita"
# control_url    = "https://controlplane.tailscale.com"
# hostname       = "vita"
log_level        = "info"

# Keep the demo's own tailnet FTP / Taildrop services if they are enabled
# in its config. Off saves memory.
keep_tailscale_services = false

# Block auto-suspend while the listener is open (suspend drops Wi-Fi).
keep_awake = true

idle_timeout_secs      = 300
data_idle_timeout_secs = 60
max_data_conns         = 2

# Background service only: screenshots as JPEG on this port, on the LAN and
# the tailnet. 0 turns it off.
capture_port = 1339

# Each [[forward]] maps a tailnet port to 127.0.0.1:<target>. listen_pool
# (optional, 1-4, at most max_conns, default max_conns) is how many
# connections can arrive at once; each costs 32 KiB.
[[forward]]
port      = 1337
kind      = "ftp"
max_conns = 2

[[forward]]
port      = 1338
kind      = "tcp"
max_conns = 4
"#;

#[derive(Debug)]
pub enum LoadError {
    Io(String, std::io::Error),
    Parse(String, toml::de::Error),
    Invalid(String),
}

impl std::fmt::Display for LoadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LoadError::Io(p, e) => write!(f, "{p}: {e}"),
            // toml's message can quote the offending line, which for the
            // inherited file could be the auth key. Report position only.
            LoadError::Parse(p, e) => match e.span() {
                Some(s) => write!(f, "{p}: parse error at byte {}", s.start),
                None => write!(f, "{p}: parse error"),
            },
            LoadError::Invalid(m) => write!(f, "{m}"),
        }
    }
}

impl ListenerConfig {
    pub fn parse(s: &str) -> Result<Self, LoadError> {
        let cfg: ListenerConfig =
            toml::from_str(s).map_err(|e| LoadError::Parse(CONFIG_PATH.into(), e))?;
        cfg.validate()?;
        Ok(cfg)
    }

    /// Load our config, writing the template first if it is missing.
    pub fn load_or_init() -> Result<(Self, bool), LoadError> {
        match vita_fs::read_to_string(Path::new(CONFIG_PATH)) {
            Ok(s) => Ok((Self::parse(&s)?, false)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                let _ = vita_fs::create_dir_all(Path::new(CONFIG_DIR));
                vita_fs::write(Path::new(CONFIG_PATH), TEMPLATE.as_bytes())
                    .map_err(|e| LoadError::Io(CONFIG_PATH.into(), e))?;
                Ok((Self::parse(TEMPLATE)?, true))
            }
            Err(e) => Err(LoadError::Io(CONFIG_PATH.into(), e)),
        }
    }

    fn validate(&self) -> Result<(), LoadError> {
        if self.forwards.is_empty() {
            return Err(LoadError::Invalid("no [[forward]] entries".into()));
        }
        for (i, f) in self.forwards.iter().enumerate() {
            if f.port == 0 || f.target_port() == 0 {
                return Err(LoadError::Invalid(format!("forward {i}: port must be non-zero")));
            }
            if f.max_conns == 0 {
                return Err(LoadError::Invalid(format!("forward {i}: max_conns must be >= 1")));
            }
            if let Some(p) = f.listen_pool {
                if p == 0 || p > f.max_conns || p > MAX_LISTEN_POOL {
                    return Err(LoadError::Invalid(format!(
                        "forward {i}: listen_pool must be 1..={}",
                        f.max_conns.min(MAX_LISTEN_POOL)
                    )));
                }
            }
            if self.forwards[..i].iter().any(|g| g.port == f.port) {
                return Err(LoadError::Invalid(format!("forward {i}: duplicate port {}", f.port)));
            }
        }
        if self.idle_timeout_secs == 0 || self.data_idle_timeout_secs == 0 {
            return Err(LoadError::Invalid("idle timeouts must be >= 1 s".into()));
        }
        Ok(())
    }

    /// Build the runtime config from the inherited tailscale-vita config.
    /// Does not write a template there if it is missing: the demo owns it.
    pub fn runtime_config(&self) -> Result<tailscale_vita::Config, LoadError> {
        let path = self.tailscale_config.clone();
        let text = vita_fs::read_to_string(Path::new(&path))
            .map_err(|e| LoadError::Io(path.clone(), e))?;
        let mut rc: tailscale_vita::Config =
            toml::from_str(&text).map_err(|e| LoadError::Parse(path, e))?;
        self.apply(&mut rc);
        Ok(rc)
    }

    fn apply(&self, rc: &mut tailscale_vita::Config) {
        rc.state_dir = self.state_dir.clone();
        if let Some(u) = &self.control_url {
            rc.control_url = u.clone();
        }
        if let Some(h) = &self.hostname {
            rc.hostname = h.clone();
        }
        rc.log_level = self.log_level.clone();
        // The listener is the runtime owner and runs until the app closes.
        rc.suprx_host_only = false;
        rc.run_window_secs = None;
        rc.tailnet.want_running = true;
        if !self.keep_tailscale_services {
            rc.ftp.enabled = false;
            rc.taildrop.enabled = false;
            rc.egress_probe.enabled = false;
        }
    }
}

fn default_kind() -> ForwardKind {
    ForwardKind::Tcp
}
fn default_max_conns() -> usize {
    2
}
fn default_tailscale_config() -> String {
    "ux0:/data/tailscale-vita/config.toml".into()
}
fn default_state_dir() -> String {
    "ux0:/data/tailscale-vita".into()
}
fn default_log_level() -> String {
    "info".into()
}
fn default_true() -> bool {
    true
}
fn default_idle_timeout() -> u64 {
    300
}
fn default_data_idle_timeout() -> u64 {
    60
}
fn default_max_data_conns() -> usize {
    2
}
fn default_capture_port() -> u16 {
    1339
}

fn default_forwards() -> Vec<Forward> {
    vec![
        Forward { port: 1337, target: None, kind: ForwardKind::Ftp, max_conns: 2, listen_pool: None },
        Forward { port: 1338, target: None, kind: ForwardKind::Tcp, max_conns: 4, listen_pool: None },
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn template_parses_to_defaults() {
        let c = ListenerConfig::parse(TEMPLATE).unwrap();
        assert_eq!(c.forwards.len(), 2);
        assert_eq!(c.forwards[0].port, 1337);
        assert_eq!(c.forwards[0].kind, ForwardKind::Ftp);
        assert_eq!(c.forwards[0].max_conns, 2);
        assert_eq!(c.forwards[1].target_port(), 1338);
        assert_eq!(c.forwards[1].max_conns, 4);
        assert_eq!(c.forwards[0].listen_pool(), 2);
        assert_eq!(c.forwards[1].listen_pool(), 4);
        assert_eq!(c.state_dir, "ux0:/data/tailscale-vita");
        assert!(c.keep_awake);
        assert!(!c.keep_tailscale_services);
    }

    #[test]
    fn empty_file_uses_defaults() {
        let c = ListenerConfig::parse("").unwrap();
        assert_eq!(c.forwards.len(), 2);
        assert_eq!(c.idle_timeout_secs, 300);
    }

    #[test]
    fn rejects_duplicate_ports() {
        let s = "[[forward]]\nport = 1338\n[[forward]]\nport = 1338\n";
        assert!(ListenerConfig::parse(s).is_err());
    }

    #[test]
    fn overrides_inherited_runtime_config() {
        let inherited = r#"
            control_url = "https://controlplane.tailscale.com"
            auth_key = "secret"
            state_dir = "elsewhere"
            [ftp]
            enabled = true
            password = "x"
            [tailnet]
            want_running = false
        "#;
        let mut rc: tailscale_vita::Config = toml::from_str(inherited).unwrap();
        let c = ListenerConfig::parse("").unwrap();
        c.apply(&mut rc);
        assert_eq!(rc.state_dir, "ux0:/data/tailscale-vita");
        assert_eq!(rc.auth_key, "secret");
        assert!(!rc.ftp.enabled);
        assert!(rc.tailnet.want_running);
    }

    #[test]
    fn listen_pool_is_capped_and_checked() {
        let one = |extra: &str| {
            ListenerConfig::parse(&format!("[[forward]]\nport = 1338\nmax_conns = 8\n{extra}"))
        };
        assert_eq!(one("").unwrap().forwards[0].listen_pool(), MAX_LISTEN_POOL);
        assert_eq!(one("listen_pool = 3").unwrap().forwards[0].listen_pool(), 3);
        assert!(one("listen_pool = 0").is_err());
        assert!(one("listen_pool = 5").is_err());
        let small = ListenerConfig::parse("[[forward]]\nport = 9\nmax_conns = 1\nlisten_pool = 2");
        assert!(small.is_err());
    }
}
