//! Tailnet listener for the PS Vita.
//!
//! Brings up the tailscale-vita runtime with the node identity the demo
//! (TVIT00010) registered, then forwards tailnet TCP connections to the
//! companion's loopback services: FTP on 1337 and the command port on
//! 1338. Runs while the app is open; there is no UI, progress goes to
//! `ux0:data/tailnet-listener/listener.log`.

use std::net::Ipv4Addr;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use netstack::TcpListener;
use tailscale_vita::{run_supervised, Runtime};
use vita_log::{error, info, warn, LogConfig, LogError};

mod bridge;
mod config;
mod ftp;
mod sys;

use bridge::{Shared, Slot};
use config::{ForwardKind, ListenerConfig};

#[cfg(target_os = "vita")]
const LOG_PATH: &str = "ux0:data/tailnet-listener/listener.log";
#[cfg(not(target_os = "vita"))]
const LOG_PATH: &str = "listener.log";

/// Same as the upstream demo's runtime and accept threads.
const RUNTIME_STACK: usize = 256 * 1024;
const ACCEPT_STACK: usize = 64 * 1024;
/// Per-listener accept wait inside the shared accept loop.
const ACCEPT_SLICE: Duration = Duration::from_millis(100);
/// Pre-allocated listening sockets per port (each holds 32 KiB of
/// netstack buffers). Capped well below max_conns.
const MAX_LISTEN_POOL: usize = 2;
const SESSION_DRAIN: Duration = Duration::from_secs(5);
const STATUS_EVERY: Duration = Duration::from_secs(60);

fn main() {
    let _ = vita_fs::create_dir_all(std::path::Path::new(config::CONFIG_DIR));
    // Read the config before starting the logger so `log_level` applies to
    // every line; errors are logged once the logger is up.
    let loaded = ListenerConfig::load_or_init();
    let filter = match &loaded {
        Ok((c, _)) => c.log_level.clone(),
        Err(_) => "info".into(),
    };
    match vita_log::init_with_config(LogConfig {
        path: LOG_PATH.into(),
        filter,
        rotate_bytes: 1024 * 1024,
        keep_files: 2,
        ..LogConfig::default()
    }) {
        Ok(()) | Err(LogError::AlreadyInitialized) => {}
        Err(e) => {
            eprintln!("vita-log init failed: {e}");
            return;
        }
    }
    info!(build = env!("BUILD_TIMESTAMP"), version = env!("CARGO_PKG_VERSION"), "listener.startup");
    run(loaded);
    vita_log::flush();
    thread::sleep(Duration::from_secs(1));
}

fn run(loaded: Result<(ListenerConfig, bool), config::LoadError>) {
    let (lcfg, created) = match loaded {
        Ok(v) => v,
        Err(e) => {
            error!(error = %e, "listener.config.invalid");
            return;
        }
    };
    if created {
        info!(path = config::CONFIG_PATH, "listener.config.template_written");
    }
    let rc = match lcfg.runtime_config() {
        Ok(rc) => rc,
        Err(e) => {
            error!(error = %e, "listener.runtime_config.unavailable; run the tailscale-vita demo once to join the tailnet");
            return;
        }
    };
    info!(
        control_url = %rc.control_url,
        hostname = %rc.hostname,
        state_dir = %rc.state_dir,
        has_auth_key = !rc.auth_key.is_empty(),
        forwards = lcfg.forwards.len(),
        "listener.config.loaded"
    );
    for f in &lcfg.forwards {
        info!(port = f.port, target = f.target_port(), kind = ?f.kind, max_conns = f.max_conns, "listener.forward");
    }

    let lcfg = Arc::new(lcfg);
    let runtime = {
        let lcfg = Arc::clone(&lcfg);
        thread::Builder::new()
            .name("ts-runtime".into())
            .stack_size(RUNTIME_STACK)
            .spawn(move || {
                // The app runs until the user closes it; the OS ends the
                // process then, so the supervisor never needs to stop.
                let r = run_supervised(rc, || false, |rt| Session::start(rt, &lcfg));
                match r {
                    Ok(s) => info!(?s, "listener.runtime.exit"),
                    Err(e) => error!(error = %e, "listener.runtime.exit.error"),
                }
            })
    };
    let runtime = match runtime {
        Ok(h) => h,
        Err(e) => {
            error!(error = %e, "listener.runtime.spawn_failed");
            return;
        }
    };

    let mut last_status = Instant::now() - STATUS_EVERY;
    while !runtime.is_finished() {
        if lcfg.keep_awake {
            sys::power_tick();
        }
        if last_status.elapsed() >= STATUS_EVERY {
            last_status = Instant::now();
            let m = sys::free_memory();
            info!(
                conns = bridge::ACTIVE_CONNS.load(Ordering::Acquire),
                data_conns = bridge::ACTIVE_DATA.load(Ordering::Acquire),
                free_user_kb = m.user_kb,
                free_cdram_kb = m.cdram_kb,
                free_phycont_kb = m.phycont_kb,
                "listener.status"
            );
        }
        thread::sleep(Duration::from_secs(1));
    }
    let _ = runtime.join();
    info!("listener.done");
}

struct PortListener {
    listener: TcpListener,
    port: u16,
    target: u16,
    is_ftp: bool,
    max_conns: usize,
    active: Arc<AtomicUsize>,
}

/// Listeners and connections bound to one runtime incarnation.
/// `run_supervised` drops it before tearing that runtime down.
struct Session {
    shared: Arc<Shared>,
    accept: Option<thread::JoinHandle<()>>,
}

impl Session {
    fn start(rt: &Runtime, cfg: &ListenerConfig) -> Session {
        let snapshot = rt.snapshot();
        let tailnet_ip: bridge::IpSource = Arc::new(move || -> Option<Ipv4Addr> {
            snapshot.read().our_addrs.first().map(|a| a.addr)
        });
        let shared = Arc::new(Shared {
            stack: rt.netstack().handle(),
            stop: Arc::new(AtomicBool::new(false)),
            tailnet_ip,
            idle: Duration::from_secs(cfg.idle_timeout_secs),
            data_idle: Duration::from_secs(cfg.data_idle_timeout_secs),
            data_slots: Arc::new(AtomicUsize::new(0)),
            max_data: cfg.max_data_conns,
            live: Arc::new(AtomicUsize::new(0)),
        });

        let mut listeners = Vec::new();
        for f in &cfg.forwards {
            let pool = f.max_conns.min(MAX_LISTEN_POOL);
            match TcpListener::bind(rt.netstack(), f.port, pool) {
                Ok(listener) => {
                    info!(port = f.port, pool, "listener.listen");
                    listeners.push(PortListener {
                        listener,
                        port: f.port,
                        target: f.target_port(),
                        is_ftp: f.kind == ForwardKind::Ftp,
                        max_conns: f.max_conns,
                        active: Arc::new(AtomicUsize::new(0)),
                    });
                }
                Err(e) => warn!(port = f.port, error = %e, "listener.bind_failed"),
            }
        }

        let accept = if listeners.is_empty() {
            None
        } else {
            let sh = Arc::clone(&shared);
            match thread::Builder::new()
                .name("accept".into())
                .stack_size(ACCEPT_STACK)
                .spawn(move || accept_loop(sh, listeners))
            {
                Ok(h) => Some(h),
                Err(e) => {
                    error!(error = %e, "listener.accept.spawn_failed");
                    None
                }
            }
        };
        if let Some(ip) = (shared.tailnet_ip)() {
            info!(%ip, "listener.tailnet_ip");
        }
        Session { shared, accept }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Release);
        if let Some(h) = self.accept.take() {
            let _ = h.join();
        }
        let deadline = Instant::now() + SESSION_DRAIN;
        while self.shared.live.load(Ordering::Acquire) > 0 && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(50));
        }
        let left = self.shared.live.load(Ordering::Acquire);
        if left > 0 {
            warn!(left, "listener.session.drain_timeout");
        }
        info!("listener.session.stopped");
    }
}

fn accept_loop(sh: Arc<Shared>, listeners: Vec<PortListener>) {
    let mut accepted = 0u32;
    while !sh.stop.load(Ordering::Acquire) {
        for l in &listeners {
            match l.listener.accept_timeout(ACCEPT_SLICE) {
                Ok((mut stream, peer)) => {
                    accepted += 1;
                    match Slot::try_acquire(&l.active, l.max_conns) {
                        Some(slot) => {
                            info!(%peer, port = l.port, count = accepted, "listener.accept");
                            bridge::spawn_conn(Arc::clone(&sh), stream, peer, l.port, l.target, l.is_ftp, slot);
                        }
                        None => {
                            warn!(%peer, port = l.port, max = l.max_conns, "listener.accept.over_cap");
                            if l.is_ftp {
                                use std::io::Write;
                                let _ = stream.write_all(ftp::REPLY_TOO_MANY);
                            }
                        }
                    }
                }
                Err(e)
                    if matches!(e.kind(), std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock) => {}
                Err(e) => {
                    warn!(port = l.port, error = %e, "listener.accept.error");
                    thread::sleep(Duration::from_millis(500));
                }
            }
        }
    }
    info!(accepted, "listener.accept.done");
}
