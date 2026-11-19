//! Plays videos while they download. pinholed writes a video into its cache
//! as usual, and meanwhile serves the part already written over HTTP on the
//! loopback, so a player can start at once. A seek far past what has arrived
//! (a QuickTime file that keeps its index at the end, say) is read from the
//! source directly for that range; the download carries on regardless.

use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::fs::FileExt;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use anyhow::Result;
use rand::RngCore;

/// Reads bytes `start..=end` of the file from its source.
pub type Ranged = Box<dyn Fn(u64, u64) -> Result<Box<dyn Read + Send>> + Send + Sync>;

/// A read this far past the download waits for it rather than going to
/// the source.
const AHEAD: u64 = 4 << 20;
/// A stream nobody has read for this long is forgotten.
const IDLE: Duration = Duration::from_secs(3600);
/// Connections served at once: a player opens a few.
const CONNECTIONS_MAX: usize = 32;
/// The longest head of a request: a player's are a few hundred bytes.
const HEAD_MAX: usize = 8 << 10;
/// How long the whole of a request's head may take to come.
const HEAD_WAIT: Duration = Duration::from_secs(10);

#[derive(Clone, Copy, PartialEq)]
enum State {
    Running,
    Done,
    Failed,
}

/// One file being downloaded, readable while it grows.
pub struct Stream {
    file: File,
    total: Option<u64>,
    content_type: &'static str,
    ranged: Ranged,
    progress: Mutex<(u64, State)>,
    grew: Condvar,
    used: Mutex<Instant>,
}

impl Stream {
    /// `n` more bytes are on disk.
    pub fn advance(&self, n: u64) {
        self.progress.lock().unwrap().0 += n;
        self.grew.notify_all();
    }

    pub fn finish(&self, ok: bool) {
        self.progress.lock().unwrap().1 = if ok { State::Done } else { State::Failed };
        self.grew.notify_all();
    }

    fn failed(&self) -> bool {
        self.progress.lock().unwrap().1 == State::Failed
    }
}

pub struct Streams {
    port: u16,
    by_token: Mutex<HashMap<String, Arc<Stream>>>,
}

impl Streams {
    /// Listen on a free loopback port.
    pub fn start() -> Result<Arc<Streams>> {
        let listener = TcpListener::bind(("127.0.0.1", 0))?;
        let streams = Arc::new(Streams { port: listener.local_addr()?.port(), by_token: Mutex::new(HashMap::new()) });
        let serving = streams.clone();
        let open = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        std::thread::spawn(move || {
            for mut conn in listener.incoming().flatten() {
                // Any program can connect, a token or not: past the most at
                // once, turned away before a thread is spent on it.
                if open.fetch_add(1, Ordering::SeqCst) >= CONNECTIONS_MAX {
                    open.fetch_sub(1, Ordering::SeqCst);
                    let _ = conn.set_write_timeout(Some(Duration::from_secs(1)));
                    let _ = conn.write_all(b"HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                    continue;
                }
                let (streams, open) = (serving.clone(), open.clone());
                std::thread::spawn(move || {
                    let _ = streams.serve(conn);
                    open.fetch_sub(1, Ordering::SeqCst);
                });
            }
        });
        Ok(streams)
    }

    /// Make a file being downloaded readable. `file` is a handle on the part
    /// file, which keeps working after it is renamed into the cache. Returns
    /// the URL to play it from.
    pub fn add(&self, file: File, total: Option<u64>, file_type: &str, ranged: Ranged) -> (String, Arc<Stream>) {
        let mut bytes = [0u8; 16];
        rand::thread_rng().fill_bytes(&mut bytes);
        let token: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
        let stream = Arc::new(Stream {
            file,
            total,
            content_type: content_type(file_type),
            ranged,
            progress: Mutex::new((0, State::Running)),
            grew: Condvar::new(),
            used: Mutex::new(Instant::now()),
        });
        let mut map = self.by_token.lock().unwrap();
        map.retain(|_, s| s.progress.lock().unwrap().1 == State::Running || s.used.lock().unwrap().elapsed() < IDLE);
        map.insert(token.clone(), stream.clone());
        (format!("http://127.0.0.1:{}/{token}", self.port), stream)
    }

    fn serve(&self, conn: TcpStream) -> Result<()> {
        let mut out = conn;
        let Some(head) = read_head(&mut out)? else {
            return respond(&mut out, "431 Request Header Fields Too Large", &[]);
        };
        let head = String::from_utf8_lossy(&head);
        let mut lines = head.lines();
        let mut parts = lines.next().unwrap_or("").split_whitespace();
        let (method, target) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""));
        let mut range = None;
        for header in lines {
            if let Some((name, value)) = header.split_once(':') {
                if name.eq_ignore_ascii_case("range") {
                    range = parse_range(value.trim());
                }
            }
        }

        let stream = self.by_token.lock().unwrap().get(target.trim_start_matches('/')).cloned();
        let Some(stream) = stream.filter(|s| !s.failed()) else {
            return respond(&mut out, "404 Not Found", &[]);
        };
        if method != "GET" && method != "HEAD" {
            return respond(&mut out, "405 Method Not Allowed", &[]);
        }
        *stream.used.lock().unwrap() = Instant::now();

        let Some(total) = stream.total else {
            // No length from the source: the whole file, start to end, unseekable.
            respond(&mut out, "200 OK", &[("Content-Type", stream.content_type.into())])?;
            return if method == "GET" { copy_growing(&stream, 0, u64::MAX, &mut out) } else { Ok(()) };
        };
        let (start, end) = match range {
            Some((s, e)) => (s, e.unwrap_or(total - 1).min(total - 1)),
            None => (0, total.saturating_sub(1)),
        };
        if total == 0 || start > end {
            return respond(&mut out, "416 Range Not Satisfiable", &[("Content-Range", format!("bytes */{total}"))]);
        }
        let status = if range.is_some() { "206 Partial Content" } else { "200 OK" };
        let mut headers = vec![
            ("Content-Type", stream.content_type.to_string()),
            ("Content-Length", (end - start + 1).to_string()),
            ("Accept-Ranges", "bytes".to_string()),
        ];
        if range.is_some() {
            headers.push(("Content-Range", format!("bytes {start}-{end}/{total}")));
        }

        let (have, state) = *stream.progress.lock().unwrap();
        if state == State::Running && start > have + AHEAD {
            // Far ahead of the download: from the source, for this range
            // only.
            let Ok(mut response) = (stream.ranged)(start, end) else {
                return respond(&mut out, "502 Bad Gateway", &[]);
            };
            respond(&mut out, status, &headers)?;
            if method == "GET" {
                std::io::copy(&mut response, &mut out)?;
            }
            return Ok(());
        }
        respond(&mut out, status, &headers)?;
        if method == "GET" {
            copy_growing(&stream, start, end, &mut out)?;
        }
        Ok(())
    }
}

/// A request's head, up to the blank line after it, read within `HEAD_WAIT`
/// in all: none (`None`) if it is longer than `HEAD_MAX`. A request for a
/// file has no body, so nothing after it is wanted.
fn read_head(conn: &mut TcpStream) -> Result<Option<Vec<u8>>> {
    let started = Instant::now();
    let mut head = Vec::new();
    let mut buf = [0u8; 1024];
    while !head.windows(4).any(|w| w == b"\r\n\r\n") && !head.windows(2).any(|w| w == b"\n\n") {
        if head.len() > HEAD_MAX {
            return Ok(None);
        }
        let left = HEAD_WAIT.checked_sub(started.elapsed()).filter(|d| !d.is_zero());
        let Some(left) = left else { anyhow::bail!("no request within {} s", HEAD_WAIT.as_secs()) };
        conn.set_read_timeout(Some(left))?;
        let n = conn.read(&mut buf)?;
        if n == 0 {
            anyhow::bail!("hung up part way");
        }
        head.extend_from_slice(&buf[..n]);
    }
    conn.set_read_timeout(Some(Duration::from_secs(30)))?;
    Ok(Some(head))
}

/// Bytes `start..=end` of the file, waiting for the download where they
/// have not arrived yet.
fn copy_growing(stream: &Stream, start: u64, end: u64, out: &mut TcpStream) -> Result<()> {
    let mut buf = vec![0u8; 256 << 10];
    let mut at = start;
    while at <= end {
        let have = {
            let mut progress = stream.progress.lock().unwrap();
            while progress.0 <= at && progress.1 == State::Running {
                progress = stream.grew.wait_timeout(progress, Duration::from_secs(1)).unwrap().0;
            }
            if progress.0 <= at {
                // Finished (the end of a file of unknown length) or failed.
                return Ok(());
            }
            progress.0
        };
        let want = (have.min(end.saturating_add(1)) - at).min(buf.len() as u64) as usize;
        let n = stream.file.read_at(&mut buf[..want], at)?;
        if n == 0 {
            return Ok(());
        }
        out.write_all(&buf[..n])?;
        at += n as u64;
        *stream.used.lock().unwrap() = Instant::now();
    }
    Ok(())
}

fn respond(out: &mut TcpStream, status: &str, headers: &[(&str, String)]) -> Result<()> {
    let mut head = format!("HTTP/1.1 {status}\r\nConnection: close\r\n");
    for (k, v) in headers {
        head.push_str(&format!("{k}: {v}\r\n"));
    }
    if !headers.iter().any(|(k, _)| *k == "Content-Length") && !status.starts_with("200") {
        head.push_str("Content-Length: 0\r\n");
    }
    head.push_str("\r\n");
    out.write_all(head.as_bytes())?;
    Ok(())
}

/// `bytes=a-` or `bytes=a-b`; suffix ranges and lists are not used by
/// players and are served as the whole file.
fn parse_range(value: &str) -> Option<(u64, Option<u64>)> {
    let spec = value.strip_prefix("bytes=")?;
    if spec.contains(',') {
        return None;
    }
    let (a, b) = spec.split_once('-')?;
    let start = a.trim().parse().ok()?;
    let end = if b.trim().is_empty() { None } else { Some(b.trim().parse().ok()?) };
    Some((start, end))
}

fn content_type(file_type: &str) -> &'static str {
    match file_type {
        "com.apple.quicktime-movie" => "video/quicktime",
        "public.mpeg-4" => "video/mp4",
        "com.apple.m4v-video" => "video/x-m4v",
        "public.avi" => "video/x-msvideo",
        _ => "application/octet-stream",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ranges() {
        assert_eq!(parse_range("bytes=0-"), Some((0, None)));
        assert_eq!(parse_range("bytes=100-199"), Some((100, Some(199))));
        assert_eq!(parse_range("bytes=-500"), None);
        assert_eq!(parse_range("bytes=0-1,5-9"), None);
    }

    /// A reader waiting on bytes the download has not written yet gets them
    /// as they arrive.
    #[test]
    fn reads_while_writing() {
        let dir = std::env::temp_dir().join(format!("pinhole-stream-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("v.part");
        let mut writer = File::options().read(true).write(true).create(true).truncate(true).open(&path).unwrap();
        let streams = Streams::start().unwrap();
        let ranged: Ranged = Box::new(|_, _| Err(anyhow::anyhow!("not in this test")));
        let (url, stream) = streams.add(writer.try_clone().unwrap(), Some(10), "public.mpeg-4", ranged);

        let port_and_token = url.trim_start_matches("http://127.0.0.1:").to_owned();
        let (port, token) = port_and_token.split_once('/').unwrap();
        let client = {
            let (port, token) = (port.to_owned(), token.to_owned());
            std::thread::spawn(move || {
                let mut conn = TcpStream::connect(("127.0.0.1", port.parse::<u16>().unwrap())).unwrap();
                write!(conn, "GET /{token} HTTP/1.1\r\nRange: bytes=2-\r\n\r\n").unwrap();
                let mut reply = Vec::new();
                conn.read_to_end(&mut reply).unwrap();
                String::from_utf8(reply).unwrap()
            })
        };
        writer.write_all(b"0123").unwrap();
        stream.advance(4);
        std::thread::sleep(Duration::from_millis(50));
        writer.write_all(b"456789").unwrap();
        stream.advance(6);
        stream.finish(true);

        let reply = client.join().unwrap();
        assert!(reply.starts_with("HTTP/1.1 206"), "{reply}");
        assert!(reply.contains("Content-Range: bytes 2-9/10"), "{reply}");
        assert!(reply.ends_with("\r\n\r\n23456789"), "{reply}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    fn ask(port: u16, head: &[u8]) -> String {
        let mut conn = TcpStream::connect(("127.0.0.1", port)).unwrap();
        conn.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let _ = conn.write_all(head);
        let mut reply = Vec::new();
        let _ = conn.read_to_end(&mut reply);
        String::from_utf8_lossy(&reply).into_owned()
    }

    /// Before a token is looked at: no long head, nor many at once.
    #[test]
    fn takes_no_long_head_nor_many_at_once() {
        let streams = Streams::start().unwrap();
        let mut long = b"GET /x HTTP/1.1\r\n".to_vec();
        long.extend(std::iter::repeat_n(b'a', 64 << 10));
        assert!(ask(streams.port, &long).starts_with("HTTP/1.1 431"));
        let quiet: Vec<TcpStream> = (0..CONNECTIONS_MAX).map(|_| TcpStream::connect(("127.0.0.1", streams.port)).unwrap()).collect();
        std::thread::sleep(Duration::from_millis(200));
        let started = Instant::now();
        assert!(ask(streams.port, b"GET /x HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 503"));
        assert!(started.elapsed() < Duration::from_secs(2));
        drop(quiet);
        let back = Instant::now();
        let answer = loop {
            let answer = ask(streams.port, b"GET /x HTTP/1.1\r\n\r\n");
            if !answer.starts_with("HTTP/1.1 503") || back.elapsed() > Duration::from_secs(5) {
                break answer;
            }
            std::thread::sleep(Duration::from_millis(50));
        };
        assert!(answer.starts_with("HTTP/1.1 404"), "{answer}");
    }
}
