//! FTP control-channel helpers (pure, host-testable).
//!
//! The companion's FTP server picks an ephemeral passive data port per
//! transfer and announces it in `227` / `229` replies, with its LAN address
//! in `227`. A plain port forward cannot carry those data connections, so
//! the FTP forwarder watches the control channel line by line:
//!
//! - server `227` / `229`: forward the announced port on the tailnet too,
//!   and rewrite the `227` address to the Vita's tailnet address;
//! - client `PORT` / `EPRT`: refused locally, since the companion cannot
//!   dial a tailnet peer (the tailnet is a userspace stack in this app).

use std::net::Ipv4Addr;

/// Longest control line we buffer before passing bytes through unparsed.
pub const MAX_LINE: usize = 1024;

pub const REPLY_ACTIVE_REFUSED: &[u8] =
    b"502 Active mode is not available over the tailnet; use PASV or EPSV.\r\n";
pub const REPLY_PASV_FAILED: &[u8] = b"425 Cannot open passive connection on the tailnet.\r\n";
pub const REPLY_TOO_MANY: &[u8] = b"421 Too many tailnet connections; try again later.\r\n";

/// Splits a byte stream into lines (each including its `\n`).
#[derive(Default)]
pub struct LineBuf {
    buf: Vec<u8>,
}

impl LineBuf {
    /// Feed bytes; returns complete lines, or raw chunks when a line grows
    /// past [`MAX_LINE`] without a newline.
    pub fn push(&mut self, data: &[u8]) -> Vec<Vec<u8>> {
        self.buf.extend_from_slice(data);
        let mut out = Vec::new();
        loop {
            if let Some(i) = self.buf.iter().position(|&b| b == b'\n') {
                let rest = self.buf.split_off(i + 1);
                out.push(std::mem::replace(&mut self.buf, rest));
            } else if self.buf.len() > MAX_LINE {
                out.push(std::mem::take(&mut self.buf));
            } else {
                break;
            }
        }
        out
    }

    /// Bytes still waiting for a newline.
    pub fn take_rest(&mut self) -> Vec<u8> {
        std::mem::take(&mut self.buf)
    }
}

fn digits(s: &[u8]) -> Option<u32> {
    if s.is_empty() || s.len() > 5 || !s.iter().all(u8::is_ascii_digit) {
        return None;
    }
    std::str::from_utf8(s).ok()?.parse().ok()
}

/// Port from `227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)`.
pub fn parse_pasv_reply(line: &[u8]) -> Option<u16> {
    if !line.starts_with(b"227") {
        return None;
    }
    let open = line.iter().position(|&b| b == b'(')?;
    let close = open + line[open..].iter().position(|&b| b == b')')?;
    let nums: Vec<u32> = line[open + 1..close]
        .split(|&b| b == b',')
        .map(|p| digits(p.trim_ascii()))
        .collect::<Option<_>>()?;
    if nums.len() != 6 || nums.iter().any(|&n| n > 255) {
        return None;
    }
    let port = nums[4] * 256 + nums[5];
    (port != 0).then_some(port as u16)
}

/// Port from `229 Entering Extended Passive Mode (|||port|)`.
pub fn parse_epsv_reply(line: &[u8]) -> Option<u16> {
    if !line.starts_with(b"229") {
        return None;
    }
    let open = line.iter().position(|&b| b == b'(')?;
    let close = open + line[open..].iter().position(|&b| b == b')')?;
    let inner = &line[open + 1..close];
    // RFC 2428: the delimiter is the first character, repeated.
    let d = *inner.first()?;
    if inner.len() < 5 || inner[1] != d || inner[2] != d || *inner.last()? != d {
        return None;
    }
    let port = digits(&inner[3..inner.len() - 1])?;
    (1..=65535).contains(&port).then_some(port as u16)
}

pub fn format_pasv_reply(ip: Ipv4Addr, port: u16) -> Vec<u8> {
    let o = ip.octets();
    format!(
        "227 Entering Passive Mode ({},{},{},{},{},{})\r\n",
        o[0],
        o[1],
        o[2],
        o[3],
        port >> 8,
        port & 0xff
    )
    .into_bytes()
}

/// True for client commands that ask for an active-mode data connection.
pub fn is_active_mode_command(line: &[u8]) -> bool {
    let verb: Vec<u8> = line
        .iter()
        .copied()
        .take_while(|b| b.is_ascii_alphabetic())
        .map(|b| b.to_ascii_uppercase())
        .collect();
    let next = line.get(verb.len()).copied();
    let ends = matches!(next, None | Some(b' ') | Some(b'\r') | Some(b'\n'));
    ends && (verb == b"PORT" || verb == b"EPRT")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pasv_reply_from_companion() {
        let l = b"227 Entering Passive Mode (192,168,1,20,195,80)\r\n";
        assert_eq!(parse_pasv_reply(l), Some(195 * 256 + 80));
        assert_eq!(parse_pasv_reply(b"227 Entering Passive Mode (1,2,3)\r\n"), None);
        assert_eq!(parse_pasv_reply(b"227 Entering Passive Mode (1,2,3,4,300,1)\r\n"), None);
        assert_eq!(parse_pasv_reply(b"200 OK\r\n"), None);
    }

    #[test]
    fn epsv_reply_from_companion() {
        assert_eq!(parse_epsv_reply(b"229 Entering Extended Passive Mode (|||50001|)\r\n"), Some(50001));
        assert_eq!(parse_epsv_reply(b"229 Entering Extended Passive Mode (!!!21!)\r\n"), Some(21));
        assert_eq!(parse_epsv_reply(b"229 Entering Extended Passive Mode (|||0|)\r\n"), None);
        assert_eq!(parse_epsv_reply(b"229 x (|||70000|)\r\n"), None);
        assert_eq!(parse_epsv_reply(b"229 x (||1|)\r\n"), None);
    }

    #[test]
    fn pasv_rewrite_round_trips() {
        let l = format_pasv_reply(Ipv4Addr::new(100, 96, 148, 83), 50001);
        assert_eq!(l, b"227 Entering Passive Mode (100,96,148,83,195,81)\r\n".to_vec());
        assert_eq!(parse_pasv_reply(&l), Some(50001));
    }

    #[test]
    fn active_mode_detection() {
        assert!(is_active_mode_command(b"PORT 1,2,3,4,5,6\r\n"));
        assert!(is_active_mode_command(b"eprt |1|1.2.3.4|5000|\r\n"));
        assert!(!is_active_mode_command(b"PASV\r\n"));
        assert!(!is_active_mode_command(b"PORTX\r\n"));
        assert!(!is_active_mode_command(b"RETR PORT\r\n"));
    }

    #[test]
    fn line_buf_splits_and_holds_partials() {
        let mut lb = LineBuf::default();
        assert!(lb.push(b"220 hel").is_empty());
        let out = lb.push(b"lo\r\n230 ok\r\n33");
        assert_eq!(out, vec![b"220 hello\r\n".to_vec(), b"230 ok\r\n".to_vec()]);
        assert_eq!(lb.take_rest(), b"33".to_vec());
    }

    #[test]
    fn line_buf_flushes_overlong_lines() {
        let mut lb = LineBuf::default();
        let big = vec![b'a'; MAX_LINE + 10];
        let out = lb.push(&big);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].len(), MAX_LINE + 10);
    }
}
