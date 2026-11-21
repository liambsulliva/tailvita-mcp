//! Per-connection forwarding between a tailnet stream (netstack) and a
//! loopback stream (the companion, via sceNet).
//!
//! `netstack::tcp::TcpStream` can't be cloned and its `read` holds `&mut`
//! for the whole wait, so the classic "one copy thread per direction"
//! needs a lock that would stall writes behind reads. Instead each
//! connection has:
//!
//! - a **pump** thread that owns the tailnet stream. It polls the tailnet
//!   side with a short read timeout and writes chunks that arrive from…
//! - a **reader** thread that blocks on the loopback socket and hands
//!   chunks to the pump over a 2-slot channel (bounded memory).
//!
//! Either side's EOF or error, the idle timeout, or the session stopping
//! closes both sides. netstack has no half-close (its `shutdown` is a full
//! close and a read EOF makes the stream unwritable), so neither do we.

use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddr, TcpStream as StdTcpStream};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{sync_channel, TryRecvError};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use netstack::tcp::TcpStream;
use netstack::{StackHandle, TcpListener};
use vita_log::{info, warn};

use crate::ftp::{self, LineBuf};

/// Copy chunk size, per direction. The tailnet socket itself has fixed
/// 16 KiB rx + 16 KiB tx buffers inside netstack.
const CHUNK: usize = 4 * 1024;
/// Stack for the per-connection pump and FTP data threads. They hold no
/// large locals (buffers are on the heap).
pub const CONN_STACK: usize = 64 * 1024;
/// The loopback reader only loops over `read` + channel send.
const READER_STACK: usize = 32 * 1024;
/// Tailnet read wait when nothing is moving. Bounds the extra latency a
/// loopback chunk can see before the pump notices it.
const IDLE_POLL: Duration = Duration::from_millis(30);
/// Loopback read timeout, so the reader notices a stopped pump.
const LOCAL_READ_POLL: Duration = Duration::from_secs(1);
const WRITE_TIMEOUT: Duration = Duration::from_secs(30);
const LOCAL_CONNECT_TIMEOUT: Duration = Duration::from_secs(3);
/// How long a passive data port waits for the tailnet peer to connect.
const DATA_ACCEPT_TIMEOUT: Duration = Duration::from_secs(30);
const DATA_ACCEPT_SLICE: Duration = Duration::from_millis(250);
/// How long a new passive data connection waits for a free slot. A finished
/// transfer's slot is held until its bridge has seen both sides close, which
/// on a slow link trails the client by a second or two; a client sending
/// many small files asks for the next connection before that.
const DATA_SLOT_WAIT: Duration = Duration::from_secs(8);
const DATA_SLOT_POLL: Duration = Duration::from_millis(100);

pub static ACTIVE_CONNS: AtomicUsize = AtomicUsize::new(0);
pub static ACTIVE_DATA: AtomicUsize = AtomicUsize::new(0);

pub type IpSource = Arc<dyn Fn() -> Option<Ipv4Addr> + Send + Sync>;

/// State shared by every connection of one runtime incarnation.
pub struct Shared {
    pub stack: StackHandle,
    /// Set when the session is torn down (app exit or relogin).
    pub stop: Arc<AtomicBool>,
    pub tailnet_ip: IpSource,
    pub idle: Duration,
    pub data_idle: Duration,
    pub data_slots: Arc<AtomicUsize>,
    pub max_data: usize,
    /// Threads still running; the session waits for these on drop.
    pub live: Arc<AtomicUsize>,
}

/// A held slot in a bounded counter; released on drop.
pub struct Slot(Arc<AtomicUsize>);

impl Slot {
    pub fn try_acquire(counter: &Arc<AtomicUsize>, max: usize) -> Option<Slot> {
        counter
            .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| (n < max).then_some(n + 1))
            .ok()
            .map(|_| Slot(Arc::clone(counter)))
    }

    /// Like `try_acquire`, but keeps trying for up to `wait`, and gives up
    /// early when `stop` is set.
    pub fn acquire_within(
        counter: &Arc<AtomicUsize>,
        max: usize,
        wait: Duration,
        poll: Duration,
        stop: &AtomicBool,
    ) -> Option<Slot> {
        let deadline = Instant::now() + wait;
        loop {
            if let Some(slot) = Slot::try_acquire(counter, max) {
                return Some(slot);
            }
            if stop.load(Ordering::Acquire) || Instant::now() >= deadline {
                return None;
            }
            thread::sleep(poll);
        }
    }
}

impl Drop for Slot {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::AcqRel);
    }
}

struct Live(Arc<AtomicUsize>, &'static AtomicUsize);

impl Live {
    fn new(sh: &Shared, stat: &'static AtomicUsize) -> Live {
        sh.live.fetch_add(1, Ordering::AcqRel);
        stat.fetch_add(1, Ordering::AcqRel);
        Live(Arc::clone(&sh.live), stat)
    }
}

impl Drop for Live {
    fn drop(&mut self) {
        self.1.fetch_sub(1, Ordering::AcqRel);
        self.0.fetch_sub(1, Ordering::AcqRel);
    }
}

fn is_timeout(e: &std::io::Error) -> bool {
    matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut)
}

fn connect_local(port: u16) -> std::io::Result<StdTcpStream> {
    let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, port));
    StdTcpStream::connect_timeout(&addr, LOCAL_CONNECT_TIMEOUT)
}

/// Serve one accepted tailnet connection on its own thread.
pub fn spawn_conn(
    sh: Arc<Shared>,
    mut ts: TcpStream,
    peer: SocketAddr,
    port: u16,
    target: u16,
    is_ftp: bool,
    slot: Slot,
) {
    let live = Live::new(&sh, &ACTIVE_CONNS);
    let spawned = thread::Builder::new()
        .name(format!("fwd-{port}"))
        .stack_size(CONN_STACK)
        .spawn(move || {
            let _slot = slot;
            let _live = live;
            let local = match connect_local(target) {
                Ok(s) => s,
                Err(e) => {
                    warn!(%peer, port, target, error = %e, "fwd.local_connect_failed");
                    if is_ftp {
                        let _ = ts.write_all(b"421 Companion FTP service is not reachable.\r\n");
                    }
                    return;
                }
            };
            info!(%peer, port, target, "fwd.open");
            let ftp = is_ftp.then(|| FtpSession::new(Arc::clone(&sh), peer));
            let (reason, up, down) = bridge(&sh, ts, local, sh.idle, ftp);
            info!(%peer, port, reason, bytes_in = up, bytes_out = down, "fwd.close");
        });
    if let Err(e) = spawned {
        // The closure (and with it the stream, slot and live guard) was
        // dropped, which closes the connection.
        warn!(%peer, port, error = %e, "fwd.spawn_failed");
    }
}

/// Copy until either side closes. Returns (reason, tailnet->local bytes,
/// local->tailnet bytes).
fn bridge(
    sh: &Arc<Shared>,
    mut ts: TcpStream,
    local: StdTcpStream,
    idle: Duration,
    mut ftp: Option<FtpSession>,
) -> (&'static str, u64, u64) {
    let local = Arc::new(local);
    let _ = local.set_read_timeout(Some(LOCAL_READ_POLL));
    let _ = local.set_write_timeout(Some(WRITE_TIMEOUT));
    let _ = local.set_nodelay(true);
    let _ = ts.set_write_timeout(Some(WRITE_TIMEOUT));

    let (tx, rx) = sync_channel::<Vec<u8>>(2);
    let done = Arc::new(AtomicBool::new(false));
    let reader = {
        let local = Arc::clone(&local);
        let done = Arc::clone(&done);
        let stop = Arc::clone(&sh.stop);
        thread::Builder::new()
            .name("fwd-rd".into())
            .stack_size(READER_STACK)
            .spawn(move || {
                let mut buf = vec![0u8; CHUNK];
                while !done.load(Ordering::Acquire) && !stop.load(Ordering::Acquire) {
                    match (&*local).read(&mut buf) {
                        Ok(0) => break,
                        Ok(n) => {
                            if tx.send(buf[..n].to_vec()).is_err() {
                                break;
                            }
                        }
                        Err(e) if is_timeout(&e) => {}
                        Err(_) => break,
                    }
                }
            })
    };
    let reader = match reader {
        Ok(h) => h,
        Err(_) => return ("reader-spawn-failed", 0, 0),
    };

    let mut buf = vec![0u8; CHUNK];
    let (mut up, mut down) = (0u64, 0u64);
    let mut last = Instant::now();
    let reason = loop {
        if sh.stop.load(Ordering::Acquire) {
            break "session-stop";
        }
        let mut busy = false;
        match rx.try_recv() {
            Ok(chunk) => {
                busy = true;
                last = Instant::now();
                down += chunk.len() as u64;
                let r = match ftp.as_mut() {
                    Some(f) => f.from_server(&chunk, &mut ts),
                    None => ts.write_all(&chunk),
                };
                if r.is_err() {
                    break "tailnet-write";
                }
            }
            Err(TryRecvError::Empty) => {}
            Err(TryRecvError::Disconnected) => {
                if let Some(f) = ftp.as_mut() {
                    let _ = f.flush_server(&mut ts);
                }
                break "local-closed";
            }
        }
        let _ = ts.set_read_timeout(Some(if busy { Duration::ZERO } else { IDLE_POLL }));
        match ts.read(&mut buf) {
            Ok(0) => break "tailnet-closed",
            Ok(n) => {
                busy = true;
                last = Instant::now();
                up += n as u64;
                let r = match ftp.as_mut() {
                    Some(f) => f.from_client(&buf[..n], &mut ts, &local),
                    None => (&*local).write_all(&buf[..n]),
                };
                if r.is_err() {
                    break "local-write";
                }
            }
            Err(e) if is_timeout(&e) => {}
            Err(_) => break "tailnet-read",
        }
        if !busy && last.elapsed() >= idle {
            // An FTP control channel is silent while its data connection
            // carries a long transfer; it stays up as long as one runs.
            if ftp.as_ref().is_some_and(FtpSession::transferring) {
                last = Instant::now();
            } else {
                break "idle-timeout";
            }
        }
    };

    done.store(true, Ordering::Release);
    let _ = local.shutdown(Shutdown::Both);
    drop(rx);
    let _ = reader.join();
    drop(ftp);
    // Dropping `ts` sends FIN and waits (bounded) for the peer to ACK it.
    drop(ts);
    (reason, up, down)
}

/// FTP control-channel state for one tailnet session.
struct FtpSession {
    sh: Arc<Shared>,
    peer: SocketAddr,
    client: LineBuf,
    server: LineBuf,
    pending: Option<DataWaiter>,
    /// Data connections of this session that are moving bytes now.
    transfers: Arc<AtomicUsize>,
}

/// Counts one running data transfer for its control session.
struct Transfer(Arc<AtomicUsize>);

impl Transfer {
    fn start(count: &Arc<AtomicUsize>) -> Transfer {
        count.fetch_add(1, Ordering::AcqRel);
        Transfer(Arc::clone(count))
    }
}

impl Drop for Transfer {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::AcqRel);
    }
}

struct DataWaiter {
    cancel: Arc<AtomicBool>,
    accepted: Arc<AtomicBool>,
    handle: Option<thread::JoinHandle<()>>,
}

impl DataWaiter {
    /// Stop waiting for a data connection. A transfer already under way is
    /// left to finish.
    fn cancel(mut self) {
        self.cancel.store(true, Ordering::Release);
        if !self.accepted.load(Ordering::Acquire) {
            if let Some(h) = self.handle.take() {
                let _ = h.join();
            }
        }
    }
}

impl FtpSession {
    fn new(sh: Arc<Shared>, peer: SocketAddr) -> FtpSession {
        FtpSession {
            sh,
            peer,
            client: LineBuf::default(),
            server: LineBuf::default(),
            pending: None,
            transfers: Arc::new(AtomicUsize::new(0)),
        }
    }

    fn transferring(&self) -> bool {
        self.transfers.load(Ordering::Acquire) > 0
    }

    /// Client -> companion. Active-mode commands are answered here.
    fn from_client(&mut self, data: &[u8], ts: &mut TcpStream, local: &StdTcpStream) -> std::io::Result<()> {
        let mut out = Vec::with_capacity(data.len());
        for line in self.client.push(data) {
            if ftp::is_active_mode_command(&line) {
                if !out.is_empty() {
                    (&*local).write_all(&out)?;
                    out.clear();
                }
                ts.write_all(ftp::REPLY_ACTIVE_REFUSED)?;
            } else {
                out.extend_from_slice(&line);
            }
        }
        if !out.is_empty() {
            (&*local).write_all(&out)?;
        }
        Ok(())
    }

    /// Companion -> client. Passive replies open a matching tailnet port
    /// before the client sees them.
    fn from_server(&mut self, data: &[u8], ts: &mut TcpStream) -> std::io::Result<()> {
        let mut out = Vec::with_capacity(data.len() + 16);
        for line in self.server.push(data) {
            let pasv = ftp::parse_pasv_reply(&line);
            let port = pasv.or_else(|| ftp::parse_epsv_reply(&line));
            match port {
                None => out.extend_from_slice(&line),
                Some(p) => match self.open_data(p) {
                    Ok(()) => match (pasv, (self.sh.tailnet_ip)()) {
                        (Some(_), Some(ip)) => out.extend_from_slice(&ftp::format_pasv_reply(ip, p)),
                        _ => out.extend_from_slice(&line),
                    },
                    Err(e) => {
                        warn!(peer = %self.peer, port = p, error = %e, "ftp.data.open_failed");
                        out.extend_from_slice(ftp::REPLY_PASV_FAILED);
                    }
                },
            }
        }
        if !out.is_empty() {
            ts.write_all(&out)?;
        }
        Ok(())
    }

    fn flush_server(&mut self, ts: &mut TcpStream) -> std::io::Result<()> {
        let rest = self.server.take_rest();
        if rest.is_empty() {
            Ok(())
        } else {
            ts.write_all(&rest)
        }
    }

    fn open_data(&mut self, port: u16) -> Result<(), String> {
        if let Some(old) = self.pending.take() {
            old.cancel();
        }
        let slot = Slot::acquire_within(
            &self.sh.data_slots,
            self.sh.max_data,
            DATA_SLOT_WAIT,
            DATA_SLOT_POLL,
            &self.sh.stop,
        )
        .ok_or_else(|| "too many data connections".to_string())?;
        let listener = TcpListener::bind_handle(&self.sh.stack, port, 1).map_err(|e| e.to_string())?;
        let cancel = Arc::new(AtomicBool::new(false));
        let accepted = Arc::new(AtomicBool::new(false));
        let live = Live::new(&self.sh, &ACTIVE_DATA);
        let sh = Arc::clone(&self.sh);
        let peer_ip = self.peer.ip();
        let (c, a) = (Arc::clone(&cancel), Arc::clone(&accepted));
        let transfers = Arc::clone(&self.transfers);
        let handle = thread::Builder::new()
            .name("ftp-data".into())
            .stack_size(CONN_STACK)
            .spawn(move || {
                let _slot = slot;
                let _live = live;
                data_conn(sh, listener, port, peer_ip, c, a, transfers);
            })
            .map_err(|e| e.to_string())?;
        self.pending = Some(DataWaiter { cancel, accepted, handle: Some(handle) });
        Ok(())
    }
}

impl Drop for FtpSession {
    fn drop(&mut self) {
        if let Some(w) = self.pending.take() {
            w.cancel();
        }
    }
}

/// Wait for the control peer to open the passive data connection, then
/// bridge it to the companion's passive port.
fn data_conn(
    sh: Arc<Shared>,
    listener: TcpListener,
    port: u16,
    peer_ip: std::net::IpAddr,
    cancel: Arc<AtomicBool>,
    accepted: Arc<AtomicBool>,
    transfers: Arc<AtomicUsize>,
) {
    let deadline = Instant::now() + DATA_ACCEPT_TIMEOUT;
    let (ts, peer) = loop {
        if cancel.load(Ordering::Acquire) || sh.stop.load(Ordering::Acquire) {
            return;
        }
        match listener.accept_timeout(DATA_ACCEPT_SLICE) {
            // Only the peer that owns the control session may use the port.
            Ok((s, p)) if p.ip() == peer_ip => break (s, p),
            Ok((_, p)) => warn!(peer = %p, port, "ftp.data.foreign_peer_rejected"),
            Err(e) if is_timeout(&e) => {
                if Instant::now() >= deadline {
                    warn!(port, "ftp.data.accept_timeout");
                    return;
                }
            }
            Err(e) => {
                warn!(port, error = %e, "ftp.data.accept_error");
                return;
            }
        }
    };
    accepted.store(true, Ordering::Release);
    // Free the listening socket's buffers before the transfer starts.
    drop(listener);
    let local = match connect_local(port) {
        Ok(s) => s,
        Err(e) => {
            warn!(%peer, port, error = %e, "ftp.data.local_connect_failed");
            return;
        }
    };
    let _transfer = Transfer::start(&transfers);
    let (reason, up, down) = bridge(&sh, ts, local, sh.data_idle, None);
    info!(%peer, port, reason, bytes_in = up, bytes_out = down, "ftp.data.close");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_slot_freed_while_waiting_is_taken() {
        let count = Arc::new(AtomicUsize::new(0));
        let stop = AtomicBool::new(false);
        let held = Slot::try_acquire(&count, 1).unwrap();
        assert!(Slot::try_acquire(&count, 1).is_none(), "the only slot is held");
        let releaser = thread::spawn(move || {
            thread::sleep(Duration::from_millis(60));
            drop(held);
        });
        let got = Slot::acquire_within(&count, 1, Duration::from_secs(5), Duration::from_millis(5), &stop);
        assert!(got.is_some(), "the waiter takes the slot once it is released");
        releaser.join().unwrap();
        assert_eq!(count.load(Ordering::Acquire), 1);
    }

    #[test]
    fn waiting_for_a_slot_gives_up_at_the_deadline_or_on_stop() {
        let count = Arc::new(AtomicUsize::new(0));
        let _held = Slot::try_acquire(&count, 1).unwrap();
        let stop = AtomicBool::new(false);
        let started = Instant::now();
        assert!(Slot::acquire_within(&count, 1, Duration::from_millis(40), Duration::from_millis(5), &stop).is_none());
        assert!(started.elapsed() >= Duration::from_millis(40));

        stop.store(true, Ordering::Release);
        let started = Instant::now();
        assert!(Slot::acquire_within(&count, 1, Duration::from_secs(5), Duration::from_millis(5), &stop).is_none());
        assert!(started.elapsed() < Duration::from_secs(1), "stop ends the wait at once");
        assert_eq!(count.load(Ordering::Acquire), 1, "a failed wait takes nothing");
    }

    #[test]
    fn transfer_guard_counts_running_transfers() {
        let count = Arc::new(AtomicUsize::new(0));
        let a = Transfer::start(&count);
        let b = Transfer::start(&count);
        assert_eq!(count.load(Ordering::Acquire), 2);
        drop(a);
        assert_eq!(count.load(Ordering::Acquire), 1);
        drop(b);
        assert_eq!(count.load(Ordering::Acquire), 0);
    }
}
