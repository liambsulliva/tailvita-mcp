//! Screenshot port of the background service.
//!
//! A client connects to `capture_port` (1339), may send one request line,
//! and gets the frame now on screen as a JPEG:
//!
//! ```text
//! -> jpeg 80\n            optional; quality 1-100, default 80
//! <- OK jpeg 960 544\n    then the JPEG, until the connection closes
//! <- ERR <reason>\n       instead, when there is no picture
//! ```
//!
//! The picture goes straight from the screen to the socket: nothing is
//! written to the memory card. The system screenshot (PS + Start) is no use
//! here, because taken while an enlarged-memory game is loaded it makes the
//! system end background services.
//!
//! The frame comes from `tailnet_capture.skprx` (daemon/capture_kernel.c),
//! which copies it into a 2 MiB memory block this process holds only while
//! a screenshot is being sent. The block is outside the newlib heap, so the
//! heap stays small.

// Off the Vita only `serve` is built (for the tests); the listener half is not.
#![cfg_attr(not(target_os = "vita"), allow(dead_code, unused_imports))]

use std::cell::RefCell;
use std::io::{self, BufRead, BufReader, BufWriter, Read, Write};
use std::net::{Ipv4Addr, TcpListener, TcpStream};
use std::thread;
use std::time::Duration;

use jpeg_encoder::{rgb_to_ycbcr, Encoder, ImageBuffer, JpegColorType, SamplingFactor};
use vita_log::{info, warn};

/// The launcher writes this once the kernel helper is loaded.
pub const READY_PATH: &str = "ux0:data/tailnet-listener/capture2-ready";

const DEFAULT_QUALITY: u8 = 80;
/// Rows handed to the encoder per call: 16 rows of 960 pixels is 60 KiB.
const ROWS_PER_READ: u32 = 16;
const THREAD_STACK: usize = 128 * 1024;
const REQUEST_WAIT: Duration = Duration::from_millis(400);
const WRITE_WAIT: Duration = Duration::from_secs(20);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameInfo {
    pub width: u32,
    pub height: u32,
    pub owner_pid: u32,
}

/// Where frames come from: the kernel helper on the Vita, a fake in tests.
pub trait FrameSource {
    /// Take a snapshot of the screen and describe it.
    fn snapshot(&mut self) -> Result<FrameInfo, i32>;
    /// Copy `rows` rows starting at `first_row` into `dst` as R, G, B, A
    /// bytes, `width` pixels per row.
    fn read_rows(&mut self, first_row: u32, rows: u32, dst: &mut [u8]) -> Result<(), i32>;
    /// Drop the snapshot.
    fn release(&mut self);
}

/// Quality asked for by a request line. An empty line, or one that is not
/// `jpeg [quality]`, gets the default.
pub fn parse_quality(line: &str) -> u8 {
    let mut words = line.split_whitespace();
    if words.next() != Some("jpeg") {
        return DEFAULT_QUALITY;
    }
    words
        .next()
        .and_then(|w| w.parse::<u32>().ok())
        .map_or(DEFAULT_QUALITY, |q| q.clamp(1, 100) as u8)
}

/// Feeds the encoder one row at a time, fetching `ROWS_PER_READ` rows from
/// the source whenever it runs past the ones it holds.
struct Rows<'a, 'b, S: FrameSource> {
    info: FrameInfo,
    state: &'b RefCell<RowState<'a, S>>,
}

struct RowState<'a, S: FrameSource> {
    source: &'a mut S,
    rgba: Vec<u8>,
    first: u32,
    count: u32,
    error: Option<i32>,
}

impl<S: FrameSource> ImageBuffer for Rows<'_, '_, S> {
    fn get_jpeg_color_type(&self) -> JpegColorType {
        JpegColorType::Ycbcr
    }

    fn width(&self) -> u16 {
        self.info.width as u16
    }

    fn height(&self) -> u16 {
        self.info.height as u16
    }

    fn fill_buffers(&self, y: u16, buffers: &mut [Vec<u8>; 4]) {
        let st = &mut *self.state.borrow_mut();
        let y = u32::from(y);
        let width = self.info.width as usize;
        if st.error.is_none() && (y < st.first || y >= st.first + st.count) {
            let rows = ROWS_PER_READ.min(self.info.height - y);
            let bytes = rows as usize * width * 4;
            match st.source.read_rows(y, rows, &mut st.rgba[..bytes]) {
                Ok(()) => {
                    st.first = y;
                    st.count = rows;
                }
                Err(e) => st.error = Some(e),
            }
        }
        if st.error.is_some() {
            // The encoder cannot stop part-way; finish with grey rows and
            // let the caller report the error.
            for b in buffers.iter_mut().take(3) {
                b.extend(std::iter::repeat(128).take(width));
            }
            return;
        }
        let at = (y - st.first) as usize * width * 4;
        for px in st.rgba[at..at + width * 4].chunks_exact(4) {
            let (luma, cb, cr) = rgb_to_ycbcr(px[0], px[1], px[2]);
            buffers[0].push(luma);
            buffers[1].push(cb);
            buffers[2].push(cr);
        }
    }
}

/// Answer one client: read its optional request line, then send a JPEG of
/// the screen or an `ERR` line. Returns the frame that was sent.
pub fn serve<S: FrameSource, C: Read + Write>(source: &mut S, conn: C) -> io::Result<FrameInfo> {
    let mut reader = BufReader::with_capacity(64, conn);
    let mut line = String::new();
    // No request line (timeout, or the client only reads) means defaults.
    let _ = reader.by_ref().take(64).read_line(&mut line);
    let quality = parse_quality(&line);
    let mut out = BufWriter::with_capacity(8 * 1024, reader.into_inner());

    let info = match source.snapshot() {
        Ok(info) if info.width > 0 && info.height > 0 && info.width <= 0xFFFF && info.height <= 0xFFFF => info,
        Ok(_) => {
            source.release();
            out.write_all(b"ERR empty frame\n")?;
            out.flush()?;
            return Err(io::Error::new(io::ErrorKind::InvalidData, "empty frame"));
        }
        Err(code) => {
            writeln!(out, "ERR no frame ({:08X})", code as u32)?;
            out.flush()?;
            return Err(io::Error::new(io::ErrorKind::NotFound, format!("snapshot failed ({:08X})", code as u32)));
        }
    };
    writeln!(out, "OK jpeg {} {}", info.width, info.height)?;

    let state = RefCell::new(RowState {
        source,
        rgba: vec![0u8; ROWS_PER_READ as usize * info.width as usize * 4],
        first: 0,
        count: 0,
        error: None,
    });
    let mut encoder = Encoder::new(&mut out, quality);
    encoder.set_sampling_factor(SamplingFactor::F_2_2);
    let encoded = encoder.encode_image(Rows { info, state: &state });
    let st = state.into_inner();
    st.source.release();
    if let Some(code) = st.error {
        return Err(io::Error::new(io::ErrorKind::Other, format!("frame read failed ({code})")));
    }
    encoded.map_err(|e| io::Error::new(io::ErrorKind::Other, e.to_string()))?;
    out.flush()?;
    Ok(info)
}

/// The kernel helper (daemon/capture.h) and the memory block it fills.
#[cfg(target_os = "vita")]
mod kernel {
    use super::{FrameInfo, FrameSource};

    #[repr(C)]
    #[derive(Default)]
    struct TnCaptureInfo {
        width: u32,
        height: u32,
        pitch: u32,
        pixelformat: u32,
        pid: u32,
    }

    extern "C" {
        fn tnCaptureGetApiVersion() -> i32;
        fn tnCaptureFrame(info: *mut TnCaptureInfo, dst: *mut u8, dst_size: u32) -> i32;
        fn sceKernelAllocMemBlock(name: *const u8, kind: u32, size: u32, opt: *mut u8) -> i32;
        fn sceKernelGetMemBlockBase(uid: i32, base: *mut *mut u8) -> i32;
        fn sceKernelFreeMemBlock(uid: i32) -> i32;
    }

    pub const API_VERSION: i32 = 2;
    const SCE_KERNEL_MEMBLOCK_TYPE_USER_RW: u32 = 0x0C20_D060;
    /// The largest frame the helper copies: 1024 x 544 pixels, a whole
    /// number of 4 KiB pages.
    const FRAME_BYTES: u32 = 1024 * 544 * 4;

    /// The frame block, held from `snapshot` to `release`.
    pub struct KernelSource {
        block: i32,
        base: *mut u8,
        width: u32,
        height: u32,
    }

    // SAFETY: the block is owned by this value alone and only reached
    // through `&mut self`.
    unsafe impl Send for KernelSource {}

    impl KernelSource {
        pub fn new() -> KernelSource {
            KernelSource { block: -1, base: std::ptr::null_mut(), width: 0, height: 0 }
        }
    }

    /// Version of the loaded helper. Only call this when the launcher's
    /// ready file exists: with no helper loaded the import is unresolved.
    pub fn api_version() -> i32 {
        // SAFETY: plain syscall, no arguments.
        unsafe { tnCaptureGetApiVersion() }
    }

    impl FrameSource for KernelSource {
        fn snapshot(&mut self) -> Result<FrameInfo, i32> {
            self.release();
            // SAFETY: a null-terminated name, a valid type and size, no options.
            let block = unsafe {
                sceKernelAllocMemBlock(
                    b"tailnet_frame\0".as_ptr(),
                    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                    FRAME_BYTES,
                    std::ptr::null_mut(),
                )
            };
            if block < 0 {
                return Err(block);
            }
            let mut base = std::ptr::null_mut();
            // SAFETY: `block` is a live block id and `base` a valid out-pointer.
            let rc = unsafe { sceKernelGetMemBlockBase(block, &mut base) };
            if rc < 0 || base.is_null() {
                // SAFETY: `block` is a live block id.
                unsafe { sceKernelFreeMemBlock(block) };
                return Err(if rc < 0 { rc } else { -1 });
            }
            self.block = block;
            self.base = base;

            let mut raw = TnCaptureInfo::default();
            // SAFETY: `raw` has the helper's layout and `base` points at
            // FRAME_BYTES writable bytes.
            let rc = unsafe { tnCaptureFrame(&mut raw, base, FRAME_BYTES) };
            if rc < 0 {
                self.release();
                return Err(rc);
            }
            self.width = raw.width;
            self.height = raw.height;
            Ok(FrameInfo { width: raw.width, height: raw.height, owner_pid: raw.pid })
        }

        fn read_rows(&mut self, first_row: u32, rows: u32, dst: &mut [u8]) -> Result<(), i32> {
            let row_bytes = self.width as usize * 4;
            if self.base.is_null() || first_row + rows > self.height || dst.len() != rows as usize * row_bytes {
                return Err(-1);
            }
            // SAFETY: the helper filled `width * height * 4` bytes at `base`,
            // and the range read here was just checked to lie inside them.
            let src = unsafe { std::slice::from_raw_parts(self.base.add(first_row as usize * row_bytes), dst.len()) };
            dst.copy_from_slice(src);
            Ok(())
        }

        fn release(&mut self) {
            if self.block >= 0 {
                // SAFETY: `block` is a live block id; nothing points into it
                // once `base` is cleared.
                unsafe { sceKernelFreeMemBlock(self.block) };
            }
            self.block = -1;
            self.base = std::ptr::null_mut();
            self.width = 0;
            self.height = 0;
        }
    }
}

/// Start the screenshot port on every interface (the LAN directly; the
/// tailnet through a forward to 127.0.0.1). Returns false when the kernel
/// helper is not loaded or the port cannot be bound.
#[cfg(target_os = "vita")]
pub fn start(port: u16) -> bool {
    if vita_fs::read(std::path::Path::new(READY_PATH)).is_err() {
        info!(path = READY_PATH, "capture.unavailable: kernel helper not loaded");
        return false;
    }
    let version = kernel::api_version();
    if version != kernel::API_VERSION {
        warn!(version, expected = kernel::API_VERSION, "capture.unavailable: helper version mismatch");
        return false;
    }
    let listener = match TcpListener::bind((Ipv4Addr::UNSPECIFIED, port)) {
        Ok(l) => l,
        Err(e) => {
            warn!(port, error = %e, "capture.bind_failed");
            return false;
        }
    };
    let spawned = thread::Builder::new()
        .name("capture".into())
        .stack_size(THREAD_STACK)
        .spawn(move || accept_loop(listener, kernel::KernelSource::new()));
    match spawned {
        Ok(_) => {
            info!(port, "capture.listen");
            true
        }
        Err(e) => {
            warn!(error = %e, "capture.spawn_failed");
            false
        }
    }
}

#[cfg(not(target_os = "vita"))]
pub fn start(_port: u16) -> bool {
    false
}

/// One client at a time: there is one frame block.
fn accept_loop<S: FrameSource>(listener: TcpListener, mut source: S) {
    for conn in listener.incoming() {
        let conn: TcpStream = match conn {
            Ok(c) => c,
            Err(e) => {
                warn!(error = %e, "capture.accept_failed");
                thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let peer = conn.peer_addr().ok();
        let _ = conn.set_read_timeout(Some(REQUEST_WAIT));
        let _ = conn.set_write_timeout(Some(WRITE_WAIT));
        let started = std::time::Instant::now();
        match serve(&mut source, conn) {
            Ok(f) => info!(
                ?peer,
                width = f.width,
                height = f.height,
                owner = format!("{:08X}", f.owner_pid),
                ms = started.elapsed().as_millis() as u64,
                "capture.sent"
            ),
            Err(e) => warn!(?peer, error = %e, "capture.failed"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    /// A frame whose colour depends on the position, so a mixed-up row or
    /// column would show.
    struct Fake {
        info: FrameInfo,
        taken: bool,
        released: bool,
        fail_snapshot: Option<i32>,
        fail_read_at: Option<u32>,
        largest_read: u32,
    }

    impl Fake {
        fn new(width: u32, height: u32) -> Fake {
            Fake {
                info: FrameInfo { width, height, owner_pid: 0x1234 },
                taken: false,
                released: false,
                fail_snapshot: None,
                fail_read_at: None,
                largest_read: 0,
            }
        }
    }

    impl FrameSource for Fake {
        fn snapshot(&mut self) -> Result<FrameInfo, i32> {
            if let Some(code) = self.fail_snapshot {
                return Err(code);
            }
            self.taken = true;
            Ok(self.info)
        }

        fn read_rows(&mut self, first_row: u32, rows: u32, dst: &mut [u8]) -> Result<(), i32> {
            assert!(self.taken, "read before snapshot");
            assert!(first_row + rows <= self.info.height, "read past the frame");
            assert_eq!(dst.len(), (rows * self.info.width * 4) as usize);
            self.largest_read = self.largest_read.max(rows);
            if self.fail_read_at.is_some_and(|at| first_row >= at) {
                return Err(-9);
            }
            for (i, px) in dst.chunks_exact_mut(4).enumerate() {
                let x = i as u32 % self.info.width;
                let y = first_row + i as u32 / self.info.width;
                px.copy_from_slice(&[(x * 255 / self.info.width) as u8, (y * 255 / self.info.height) as u8, 64, 255]);
            }
            Ok(())
        }

        fn release(&mut self) {
            self.released = true;
        }
    }

    /// A connection with a request to read and a reply to write.
    struct Conn {
        request: Cursor<Vec<u8>>,
        reply: Vec<u8>,
    }

    impl Conn {
        fn new(request: &str) -> Conn {
            Conn { request: Cursor::new(request.as_bytes().to_vec()), reply: Vec::new() }
        }
    }

    impl Read for &mut Conn {
        fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
            self.request.read(buf)
        }
    }

    impl Write for &mut Conn {
        fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
            self.reply.extend_from_slice(buf);
            Ok(buf.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    fn split_reply(reply: &[u8]) -> (&str, &[u8]) {
        let nl = reply.iter().position(|&b| b == b'\n').expect("header line");
        (std::str::from_utf8(&reply[..nl]).unwrap(), &reply[nl + 1..])
    }

    #[test]
    fn request_line_sets_the_quality_within_bounds() {
        assert_eq!(parse_quality(""), 80);
        assert_eq!(parse_quality("jpeg\n"), 80);
        assert_eq!(parse_quality("jpeg 55\r\n"), 55);
        assert_eq!(parse_quality("jpeg 0"), 1);
        assert_eq!(parse_quality("jpeg 400"), 100);
        assert_eq!(parse_quality("jpeg high"), 80);
        assert_eq!(parse_quality("png 10"), 80);
    }

    #[test]
    fn a_client_gets_a_header_line_and_a_complete_jpeg() {
        let mut fake = Fake::new(96, 40);
        let mut conn = Conn::new("jpeg 70\n");
        let sent = serve(&mut fake, &mut conn).unwrap();
        assert_eq!(sent, FrameInfo { width: 96, height: 40, owner_pid: 0x1234 });

        let (header, jpeg) = split_reply(&conn.reply);
        assert_eq!(header, "OK jpeg 96 40");
        assert_eq!(&jpeg[..2], [0xFF, 0xD8], "starts with the JPEG start marker");
        assert_eq!(&jpeg[jpeg.len() - 2..], [0xFF, 0xD9], "ends with the JPEG end marker");
        // The frame header (marker FFC0) carries the size: precision, height, width.
        let sof = jpeg.windows(2).position(|w| w == [0xFF, 0xC0]).expect("frame header");
        assert_eq!(&jpeg[sof + 5..sof + 9], [0, 40, 0, 96]);
        assert!(fake.released, "the snapshot is dropped afterwards");
        assert!(fake.largest_read <= ROWS_PER_READ, "rows are fetched in small batches");
    }

    #[test]
    fn a_client_that_sends_nothing_still_gets_a_picture() {
        let mut fake = Fake::new(32, 24);
        let mut conn = Conn::new("");
        serve(&mut fake, &mut conn).unwrap();
        assert!(conn.reply.starts_with(b"OK jpeg 32 24\n"));
    }

    #[test]
    fn a_higher_quality_gives_a_larger_picture() {
        let size = |request: &str| {
            let mut fake = Fake::new(96, 64);
            let mut conn = Conn::new(request);
            serve(&mut fake, &mut conn).unwrap();
            conn.reply.len()
        };
        assert!(size("jpeg 95\n") > size("jpeg 20\n"));
    }

    #[test]
    fn no_frame_gives_an_error_line_and_no_picture() {
        let mut fake = Fake::new(32, 24);
        fake.fail_snapshot = Some(-2);
        let mut conn = Conn::new("");
        assert!(serve(&mut fake, &mut conn).is_err());
        assert_eq!(conn.reply, b"ERR no frame (FFFFFFFE)\n");
    }

    #[test]
    fn a_read_failure_part_way_is_reported_and_the_snapshot_is_dropped() {
        let mut fake = Fake::new(32, 64);
        fake.fail_read_at = Some(32);
        let mut conn = Conn::new("");
        let err = serve(&mut fake, &mut conn).unwrap_err();
        assert!(err.to_string().contains("frame read failed (-9)"));
        assert!(fake.released);
    }
}
