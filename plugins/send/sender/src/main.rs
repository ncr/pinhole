//! pinhole-send: the Send to phone plugin's way to LocalSend (plugins/send),
//! with LocalSend's own protocol code. Each command writes a line for each
//! thing that happens, as it happens, for the plugin to show. Lists of files
//! come on stdin, as JSON, so there can be any number of them.
//!
//! `pinhole-send devices [<device> ...]`
//!     The LocalSend devices nearby, a JSON line for each as it answers
//!     (again from a better address), until it is stopped. The devices
//!     given, as those lines, are where devices were last: asked first,
//!     since a phone that is already open does not announce itself. A device
//!     known over HTTPS is never taken over by one claiming its fingerprint
//!     over plain HTTP, which proves nothing.
//!
//! `pinhole-send send <device>`, stdin `{"files": [{"path", "name"}, ...], "pin"}`
//!     The files to the device, each under its name, with the PIN it asked
//!     for if there is one (on stdin, where no other process reads it):
//!     `asking` while its owner decides, `accepted <files> <bytes>`,
//!     `progress <sent> <total>` as it goes, and `sent <files>`. Else,
//!     exiting 1, one of: `declined`, `busy`, `pin` (it wants one),
//!     `wrongpin`, `blocked` (it takes nothing from here after three wrong
//!     PINs, until it is opened again), `nothing` (it took none), `stalled`
//!     (nothing went for 30 s), `unreachable` (nothing answered at its
//!     address in 16 s), `mismatch` (not the certificate it answered
//!     with before), `rejected <status>`, `failed`, or `cancelled` at once
//!     when stopped (SIGTERM), the device then told within 0.7 s.
//!
//! `pinhole-send stage <folder>`, stdin `[{"path", "name"}, ...]`
//!     The files under their names in the folder (linked where they can be,
//!     else copied), for LocalSend's own window: their paths, a line each.
//!
//! `pinhole-send jpeg <folder>`, stdin `["path", ...]`
//!     Each HEIC as a JPEG in the folder (ImageMagick, which keeps the date,
//!     place and camera), a few at once: `<k> <path>` for the k-th as it is
//!     ready, `<k> !` if it could not be. Kept by the original's path, size
//!     and inode, so another file of its name, or it changed, is made anew.
//!
//! `pinhole-send tidy <folder>`
//!     The plugin's cache kept small, the folder's and each other source's
//!     beside it: folders staged for LocalSend gone after a day, JPEGs after
//!     thirty, and the oldest of them past 2 GB.
//!
//! This computer is one LocalSend device to the others, whichever command:
//! its certificate is kept in ~/.config/pinhole/send, so a phone sees the
//! same device each time.

use anyhow::{anyhow, bail, Context, Result};
use bytes::Bytes;
use futures_util::StreamExt;
use localsend::discovery::{
    DeviceChannel, DeviceIdentity, DiscoveredDevice, DiscoveryConfig, DiscoveryEvent, HttpChannel,
    DEFAULT_DISCOVERY_TIMEOUT,
};
use localsend::http::client::ClientError;
use localsend::http::client::v2::LsHttpClientV2;
use localsend::http::dto_v2::{PrepareUploadRequestDtoV2, RegisterDtoV2};
use localsend::http::server::v2::{PrepareUploadDecisionV2, ServerEventV2};
use localsend::http::server::{start_with_port, ServerConfigV2, TlsConfig};
use localsend::http::state::ClientInfo;
use localsend::model::discovery::{DeviceType, ProtocolType, PROTOCOL_VERSION_V2};
use localsend::model::transfer::{FileContent, FileDto};
use localsend::multicast::{MulticastDevice, DEFAULT_MULTICAST_GROUP, DEFAULT_MULTICAST_GROUP_V6, DEFAULT_PORT};
use localsend::util::interface::{local_interface_addresses, InterfaceFilter};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime};
use tokio::sync::{mpsc, oneshot};
use tokio_stream::wrappers::ReceiverStream;
use tokio_util::sync::CancellationToken;

/// With nothing sent for this long, a receiver is taken to have stopped.
const STALL: Duration = Duration::from_secs(30);
/// How long the device is given to hear the send was given up on.
const CANCEL_WAIT: Duration = Duration::from_secs(2);
/// Stopped, less: the plugin's host kills a second after asking (SIGTERM),
/// and `cancelled` is said before.
const STOPPED_WAIT: Duration = Duration::from_millis(700);
/// How long a device's address is given to answer at all: one gone from
/// the network would otherwise be waited for as long as the system tries,
/// minutes. Past the fourth of Linux's tries again (at 1, 3, 7 and 15 s),
/// for a phone whose Wi-Fi wakes slowly.
const CONNECT: Duration = Duration::from_secs(16);
/// HEICs made JPEGs at once.
const CONVERSIONS: usize = 3;

fn main() {
    // Ended with the window, however it goes: SIGTERM, as when stopped.
    unsafe {
        libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM);
    }
    let args: Vec<String> = std::env::args().skip(1).collect();
    let runtime = tokio::runtime::Runtime::new().expect("a tokio runtime");
    let result = runtime.block_on(async {
        match args.first().map(String::as_str) {
            Some("devices") => devices(&args[1..]).await,
            Some("send") => send(&args[1..]).await,
            Some("stage") => stage(&args[1..]),
            Some("jpeg") => jpeg(&args[1..]).await,
            Some("tidy") => tidy(&args[1..]),
            _ => bail!("usage: pinhole-send devices [<device> ...] | send <device> | stage <folder> | jpeg <folder> | tidy <folder>"),
        }
    });
    if let Err(err) = result {
        // For the log: never a URL (it carries the session's token).
        eprintln!("pinhole-send: {err:#}");
        say("failed");
        std::process::exit(1);
    }
    // The discovery's and the server's tasks are not waited for.
    std::process::exit(0);
}

/// A line to the plugin, at once.
fn say(line: &str) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{line}");
    let _ = out.flush();
}

/// The last word, and out.
fn end(line: &str) -> ! {
    say(line);
    std::process::exit(1);
}

/// Cancelled when the plugin is done with it: SIGTERM or SIGINT.
fn stopped() -> CancellationToken {
    let token = CancellationToken::new();
    let on_signal = token.clone();
    tokio::spawn(async move {
        use tokio::signal::unix::{signal, SignalKind};
        let (Ok(mut term), Ok(mut int)) = (signal(SignalKind::terminate()), signal(SignalKind::interrupt())) else {
            return;
        };
        tokio::select! {
            _ = term.recv() => {}
            _ = int.recv() => {}
        }
        on_signal.cancel();
    });
    token
}

/// The JSON on stdin.
fn input() -> Result<Value> {
    let mut text = String::new();
    std::io::stdin().read_to_string(&mut text).context("reading stdin")?;
    serde_json::from_str(&text).context("stdin is not JSON")
}

/// `[{"path", "name"}, ...]` from stdin.
fn files_in() -> Result<Vec<(PathBuf, String)>> {
    files_of(&input()?)
}

fn files_of(list: &Value) -> Result<Vec<(PathBuf, String)>> {
    list.as_array()
        .context("stdin is not a list")?
        .iter()
        .map(|file| {
            let path = file["path"].as_str().context("a file without a path")?;
            let name = file["name"].as_str().context("a file without a name")?;
            // A name, not a path: nothing lands outside where it is put.
            let name = Path::new(name).file_name().context("a file without a name")?.to_string_lossy().into_owned();
            Ok((PathBuf::from(path), name))
        })
        .collect()
}

/// This computer as a LocalSend device: named as the computer is, a
/// desktop, its model Pinhole.
struct Identity {
    alias: String,
    cert_pem: String,
    key_pem: String,
    fingerprint: String,
}

impl Identity {
    fn load() -> Result<Self> {
        let dir = std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
            .ok_or_else(|| anyhow!("no home folder"))?
            .join("pinhole/send");
        let path = dir.join("identity.pem");
        let alias = gethostname::gethostname().to_string_lossy().into_owned();
        if let Ok(text) = std::fs::read_to_string(&path) {
            if let Ok(identity) = Self::from_pem(&text, alias.clone()) {
                return Ok(identity);
            }
        }
        let cert = localsend::crypto::cert::generate_self_signed()?;
        let identity = Self {
            alias,
            fingerprint: cert.fingerprint,
            cert_pem: cert.certificate_pem,
            key_pem: cert.private_key_pem,
        };
        identity.save(&dir, &path)?;
        Ok(identity)
    }

    fn from_pem(text: &str, alias: String) -> Result<Self> {
        let blocks = pem::parse_many(text)?;
        let cert = blocks.iter().find(|b| b.tag() == "CERTIFICATE").context("no certificate")?;
        let key = blocks.iter().find(|b| b.tag().ends_with("PRIVATE KEY")).context("no key")?;
        Ok(Self {
            alias,
            fingerprint: localsend::crypto::cert::fingerprint_from_cert_der(cert.contents()),
            cert_pem: pem::encode(cert),
            key_pem: pem::encode(key),
        })
    }

    /// Written aside and renamed, readable by its owner only (it holds the
    /// private key).
    fn save(&self, dir: &Path, path: &Path) -> Result<()> {
        use std::os::unix::fs::OpenOptionsExt;
        std::fs::create_dir_all(dir).with_context(|| format!("making {}", dir.display()))?;
        let part = path.with_extension(format!("pem.{}.part", std::process::id()));
        let mut file = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&part)?;
        file.write_all(format!("{}{}", self.cert_pem, self.key_pem).as_bytes())?;
        drop(file);
        std::fs::rename(&part, path)?;
        Ok(())
    }

    fn register_dto(&self, port: u16) -> RegisterDtoV2 {
        RegisterDtoV2 {
            alias: self.alias.clone(),
            version: PROTOCOL_VERSION_V2.to_string(),
            device_model: Some("Pinhole".to_string()),
            device_type: Some(DeviceType::Desktop),
            fingerprint: self.fingerprint.clone(),
            port,
            protocol: ProtocolType::Https,
            download: false,
        }
    }

    fn multicast_device(&self, port: u16) -> MulticastDevice {
        let dto = self.register_dto(port);
        MulticastDevice {
            alias: dto.alias,
            version: dto.version,
            device_model: dto.device_model,
            device_type: dto.device_type,
            fingerprint: dto.fingerprint,
            port: dto.port,
            protocol: dto.protocol,
            download: dto.download,
        }
    }
}

/// A device as the plugin is told of it, and gives it back.
fn device_line(device: &DiscoveredDevice) -> Option<Value> {
    let http = device.channel.http()?;
    Some(json!({
        "fingerprint": device.fingerprint,
        "alias": device.alias,
        "model": device.device_model,
        "type": device.device_type.as_ref().map(|t| serde_json::to_value(t).unwrap_or(Value::Null)),
        "host": http.host,
        "port": http.port,
        "protocol": http.protocol.as_str(),
    }))
}

fn channel_of(device: &Value) -> Result<HttpChannel> {
    let host = device["host"].as_str().context("a device without a host")?.to_string();
    let port = device["port"].as_u64().context("a device without a port")? as u16;
    let protocol = match device["protocol"].as_str() {
        Some("http") => ProtocolType::Http,
        _ => ProtocolType::Https,
    };
    Ok(HttpChannel { host, port, protocol })
}

/// How good an address is, best 0: over HTTPS before plain HTTP; then the
/// local network's own, any other IPv4 (a VPN's), IPv6.
fn rank(http: &HttpChannel) -> u8 {
    let near = match http.host.parse::<std::net::IpAddr>() {
        Ok(std::net::IpAddr::V4(ip)) if ip.is_private() => 0,
        Ok(std::net::IpAddr::V4(_)) => 1,
        _ => 2,
    };
    near + if http.protocol == ProtocolType::Https { 0 } else { 10 }
}

async fn devices(known: &[String]) -> Result<()> {
    let identity = Identity::load()?;
    let stop = stopped();

    // A server of its own, on any free port, for the devices that answer an
    // announcement by registering with it. It only ever takes
    // registrations: anything offered to it is declined. (LocalSend's own
    // window, if it runs, has 53317; a phone answering there would be
    // LocalSend's to know of, not ours.)
    let (server_tx, mut server_rx) = mpsc::channel::<ServerEventV2>(16);
    let (_server_stop, server_stop_rx) = oneshot::channel();
    let server = start_with_port(
        0,
        Some(TlsConfig { cert: identity.cert_pem.clone(), private_key: identity.key_pem.clone() }),
        ClientInfo {
            alias: identity.alias.clone(),
            version: PROTOCOL_VERSION_V2.to_string(),
            device_model: Some("Pinhole".to_string()),
            device_type: Some(DeviceType::Desktop),
            token: identity.fingerprint.clone(),
        },
        None,
        Some(ServerConfigV2 { pin: None, verify_checksums: false, event_tx: server_tx }),
        None,
        server_stop_rx,
    )
    .await
    .context("starting the server devices answer")?;
    let port = server.port();

    let (events_tx, mut events_rx) = mpsc::channel::<DiscoveryEvent>(64);
    let (_discovery_stop, discovery_stop_rx) = oneshot::channel();
    let discovery = Arc::new(
        localsend::discovery::start(
            DiscoveryConfig {
                group: DEFAULT_MULTICAST_GROUP,
                group_v6: Some(DEFAULT_MULTICAST_GROUP_V6),
                port: DEFAULT_PORT,
                interface_filter: InterfaceFilter::default(),
                device: identity.multicast_device(port),
                identity: DeviceIdentity {
                    cert_pem: identity.cert_pem.clone(),
                    private_key_pem: identity.key_pem.clone(),
                },
                timeout: DEFAULT_DISCOVERY_TIMEOUT,
                event_tx: Some(events_tx),
            },
            discovery_stop_rx,
        )
        .await,
    );
    if let Some(err) = discovery.multicast_error() {
        eprintln!("pinhole-send: multicast unavailable: {err:#}");
    }

    tokio::spawn({
        let discovery = discovery.clone();
        let own = identity.fingerprint.clone();
        async move {
            while let Some(event) = server_rx.recv().await {
                match event {
                    ServerEventV2::Register { ip, info } if info.fingerprint != own => {
                        discovery
                            .add_device(DiscoveredDevice {
                                alias: info.alias,
                                version: info.version,
                                device_model: info.device_model,
                                device_type: info.device_type,
                                fingerprint: info.fingerprint,
                                channel: DeviceChannel::Http(HttpChannel {
                                    host: ip.to_string(),
                                    port: info.port,
                                    protocol: info.protocol,
                                }),
                                download: info.download,
                            })
                            .await;
                    }
                    ServerEventV2::PrepareUpload { decision_tx, .. } => {
                        let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
                    }
                    _ => {}
                }
            }
        }
    });

    // Announce, ask where devices were, and scan the local networks if
    // nothing answers; then announce again now and then, for a phone whose
    // LocalSend opens in the meantime.
    let known: Vec<Value> = known.iter().filter_map(|line| serde_json::from_str::<Value>(line).ok()).collect();
    // Fingerprints proven over HTTPS: a plain HTTP claim to one is not
    // believed.
    let mut secure: HashSet<String> = known
        .iter()
        .filter(|device| device["protocol"].as_str() != Some("http"))
        .filter_map(|device| device["fingerprint"].as_str().map(str::to_string))
        .collect();
    let channels: Vec<HttpChannel> = known.iter().filter_map(|device| channel_of(device).ok()).collect();
    tokio::spawn({
        let discovery = discovery.clone();
        async move {
            let networks = local_interface_addresses(&InterfaceFilter::default()).unwrap_or_default();
            let _ = discovery
                .discover_staged(channels.clone(), networks, DEFAULT_PORT, ProtocolType::Https, Duration::from_secs(2))
                .await;
            loop {
                tokio::time::sleep(Duration::from_secs(10)).await;
                let _ = discovery.discover_known_http_channels(channels.clone()).await;
                discovery.announce().await;
            }
        }
    });

    let mut said: HashMap<String, u8> = HashMap::new();
    // Ten minutes at most, should nothing stop it.
    let deadline = tokio::time::sleep(Duration::from_secs(600));
    tokio::pin!(deadline);
    loop {
        tokio::select! {
            event = events_rx.recv() => match event {
                Some(DiscoveryEvent::Discovered { device } | DiscoveryEvent::Updated { device }) => {
                    let Some(http) = device.channel.http() else { continue };
                    if http.protocol == ProtocolType::Https {
                        secure.insert(device.fingerprint.clone());
                    } else if secure.contains(&device.fingerprint) {
                        continue;
                    }
                    // Again only from a better address: a phone answers on
                    // each network it is on (Wi-Fi, a VPN, IPv6).
                    let rank = rank(http);
                    if said.get(&device.fingerprint).is_some_and(|&said| said <= rank) {
                        continue;
                    }
                    if let Some(line) = device_line(&device) {
                        said.insert(device.fingerprint.clone(), rank);
                        say(&line.to_string());
                    }
                }
                Some(DiscoveryEvent::MulticastFailed) => eprintln!("pinhole-send: multicast stopped"),
                None => break,
            },
            _ = stop.cancelled() => break,
            _ = &mut deadline => break,
        }
    }
    Ok(())
}

/// A file's type as LocalSend gives it, by its name: a phone saves images
/// and videos to its gallery by it.
fn file_type(name: &str) -> &'static str {
    let extension = Path::new(name).extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase();
    match extension.as_str() {
        "jpg" | "jpeg" => "image/jpeg",
        "heic" => "image/heic",
        "heif" => "image/heif",
        "png" => "image/png",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "avif" => "image/avif",
        "tif" | "tiff" => "image/tiff",
        "dng" => "image/x-adobe-dng",
        "mov" => "video/quicktime",
        "mp4" | "m4v" => "video/mp4",
        _ => "application/octet-stream",
    }
}

/// A value for a URL's query, percent-encoded but for what never needs it
/// (RFC 3986's unreserved): LocalSend puts it in as it is given.
fn query_value(text: &str) -> String {
    text.bytes()
        .map(|b| match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => (b as char).to_string(),
            b => format!("%{b:02X}"),
        })
        .collect()
}

/// Whether anything takes a connection at the device's address within
/// CONNECT. An address that is not one (a name) is left to the request.
async fn reachable(http: &HttpChannel) -> bool {
    let address = if http.host.contains(':') {
        format!("[{}]:{}", http.host, http.port)
    } else {
        format!("{}:{}", http.host, http.port)
    };
    let Ok(address) = address.parse::<std::net::SocketAddr>() else { return true };
    matches!(tokio::time::timeout(CONNECT, tokio::net::TcpStream::connect(address)).await, Ok(Ok(_)))
}

/// What went wrong talking to a device, as a word for the plugin; never
/// the error itself, whose URL carries the session's token.
fn trouble(err: &ClientError) -> String {
    let mut source: Option<&dyn std::error::Error> = Some(err);
    while let Some(e) = source {
        if e.to_string().contains("fingerprint mismatch") {
            return "mismatch".into();
        }
        source = e.source();
    }
    match err {
        ClientError::StatusCode(err) => format!("rejected {}", err.status),
        ClientError::Reqwest(e) if e.is_connect() || e.is_timeout() => "unreachable".into(),
        _ => "failed".into(),
    }
}

/// A device's no to the files, by its HTTP status, as a word for the
/// plugin.
fn refusal(status: u16, with_pin: bool) -> String {
    match status {
        401 if with_pin => "wrongpin".into(),
        401 => "pin".into(),
        403 => "declined".into(),
        409 => "busy".into(),
        // LocalSend's answer to this address after three wrong PINs, until
        // it is opened again.
        429 => "blocked".into(),
        status => format!("rejected {status}"),
    }
}

async fn send(args: &[String]) -> Result<()> {
    let device: Value = serde_json::from_str(args.first().context("no device")?).context("the device")?;
    let given = input()?;
    // As a URL's query has it: a PIN of `&`, `#`, `+` or `%` is itself.
    let pin = given["pin"].as_str().filter(|pin| !pin.is_empty()).map(query_value);
    let http = channel_of(&device)?;
    let identity = Identity::load()?;
    let chosen = files_of(&given["files"])?;
    if chosen.is_empty() {
        bail!("no files");
    }
    let stop = stopped();

    let mut files = HashMap::new();
    let mut paths = HashMap::new();
    let mut order = Vec::new();
    for (k, (path, name)) in chosen.into_iter().enumerate() {
        let size = std::fs::metadata(&path).with_context(|| format!("reading {}", path.display()))?.len();
        let id = format!("pinhole-{k}");
        files.insert(
            id.clone(),
            FileDto {
                id: id.clone(),
                file_type: file_type(&name).to_string(),
                file_name: name,
                size,
                sha256: None,
                preview: None,
                metadata: None,
            },
        );
        paths.insert(id.clone(), path);
        order.push(id);
    }

    // Pinned to the certificate the device answered with, so nothing goes
    // to another device that took its address since.
    let fingerprint = device["fingerprint"].as_str().map(str::to_string);
    let client = LsHttpClientV2::try_new(
        &identity.key_pem,
        &identity.cert_pem,
        if http.protocol == ProtocolType::Https { fingerprint } else { None },
        None,
    )?;

    say("asking");
    tokio::select! {
        answers = reachable(&http) => if !answers {
            eprintln!("pinhole-send: asking: unreachable");
            end("unreachable");
        },
        _ = stop.cancelled() => end("cancelled"),
    }
    let payload = PrepareUploadRequestDtoV2 { info: identity.register_dto(DEFAULT_PORT), files: files.clone() };
    let prepared = match client
        .prepare_upload(http.protocol, &http.host, http.port, None, payload, pin.as_deref(), stop.clone())
        .await
    {
        Ok(prepared) => prepared,
        Err(ClientError::Cancelled) => end("cancelled"),
        Err(ClientError::StatusCode(err)) => end(&refusal(err.status, pin.is_some())),
        Err(err) => {
            let word = trouble(&err);
            eprintln!("pinhole-send: asking: {word}");
            end(&word)
        }
    };
    // 204: it needs none of them.
    let Some(response) = prepared.response else { end("nothing") };

    let accepted: Vec<&String> = order.iter().filter(|id| response.files.contains_key(*id)).collect();
    if accepted.is_empty() {
        end("nothing");
    }
    let total: u64 = accepted.iter().map(|id| files[*id].size).sum();
    say(&format!("accepted {} {total}", accepted.len()));

    // Stopped, or given up on: the uploads end either way.
    let abort = stop.child_token();
    let started = Instant::now();
    let last = Arc::new(AtomicU64::new(0));
    tokio::spawn({
        let abort = abort.clone();
        let last = last.clone();
        async move {
            loop {
                tokio::time::sleep(Duration::from_secs(1)).await;
                let since = (started.elapsed().as_millis() as u64).saturating_sub(last.load(Ordering::Relaxed));
                if Duration::from_millis(since) >= STALL {
                    abort.cancel();
                    return;
                }
            }
        }
    });

    // One file after another, saying how far it is ten times a second.
    let (progress_tx, mut progress_rx) = mpsc::unbounded_channel::<u64>();
    tokio::spawn(async move {
        let mut said = Instant::now() - Duration::from_secs(1);
        while let Some(sent) = progress_rx.recv().await {
            if said.elapsed() >= Duration::from_millis(100) || sent == total {
                said = Instant::now();
                say(&format!("progress {sent} {total}"));
            }
        }
    });
    let mut sent = 0u64;
    for id in &accepted {
        let token = &response.files[*id];
        let body = {
            let progress_tx = progress_tx.clone();
            let last = last.clone();
            let base = sent;
            let mut of_file = 0u64;
            let stream = ReceiverStream::new(FileContent::Path(paths[*id].clone()).into_receiver()).map(move |chunk| {
                of_file += chunk.len() as u64;
                last.store(started.elapsed().as_millis() as u64, Ordering::Relaxed);
                let _ = progress_tx.send(base + of_file);
                Ok::<Bytes, anyhow::Error>(chunk)
            });
            localsend::reqwest::Body::wrap_stream(stream)
        };
        last.store(started.elapsed().as_millis() as u64, Ordering::Relaxed);
        let result = client
            .upload(http.protocol, &http.host, http.port, None, &response.session_id, id, token, body, abort.clone())
            .await;
        if let Err(err) = result {
            let stopped = stop.is_cancelled();
            let word = match err {
                ClientError::Cancelled if stopped => "cancelled".to_string(),
                ClientError::Cancelled => "stalled".to_string(),
                err => trouble(&err),
            };
            eprintln!("pinhole-send: sending: {word}");
            // Stopped: said at once, before the host's kill; the device is
            // told in what is left. Else told, if it will hear it in a
            // moment, and not waited for longer.
            if stopped {
                say(&word);
            }
            let _ = tokio::time::timeout(
                if stopped { STOPPED_WAIT } else { CANCEL_WAIT },
                client.cancel(http.protocol, &http.host, http.port, &response.session_id),
            )
            .await;
            if stopped {
                std::process::exit(1);
            }
            end(&word);
        }
        sent += files[*id].size;
    }
    drop(progress_tx);
    // The last progress line out before the end.
    tokio::time::sleep(Duration::from_millis(20)).await;
    say(&format!("sent {}", accepted.len()));
    Ok(())
}

fn stage(args: &[String]) -> Result<()> {
    let folder = PathBuf::from(args.first().context("no folder")?);
    let files = files_in()?;
    std::fs::create_dir_all(&folder).with_context(|| format!("making {}", folder.display()))?;
    for (from, name) in files {
        // Two photos of one name (a camera's counter come round) both go.
        let mut to = folder.join(&name);
        let mut n = 2;
        while to.exists() {
            let stem = Path::new(&name).file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
            let extension = Path::new(&name).extension().map(|e| format!(".{}", e.to_string_lossy())).unwrap_or_default();
            to = folder.join(format!("{stem} ({n}){extension}"));
            n += 1;
        }
        if std::fs::hard_link(&from, &to).is_err() {
            std::fs::copy(&from, &to).with_context(|| format!("copying {}", from.display()))?;
        }
        say(&to.to_string_lossy());
    }
    Ok(())
}

/// The name a file's JPEG is kept under: its path, size, inode and birth,
/// hashed (as Export's, Outgoing::compatibleName). Not its time: pinholed
/// touches what it serves from its cache, each time. A file replaced, or
/// written over, is another.
fn jpeg_name(path: &Path) -> Result<String> {
    use std::os::unix::fs::MetadataExt;
    let meta = std::fs::metadata(path).with_context(|| format!("reading {}", path.display()))?;
    let born = meta.created().ok().and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok()).unwrap_or_default().as_millis();
    let path = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    let mut hash = Sha256::new();
    hash.update(path.to_string_lossy().as_bytes());
    hash.update(format!("\0{}\0{}\0{born}", meta.len(), meta.ino()).as_bytes());
    let digest = hash.finalize();
    Ok(digest[..16].iter().map(|b| format!("{b:02x}")).collect::<String>() + ".jpg")
}

async fn jpeg(args: &[String]) -> Result<()> {
    let folder = PathBuf::from(args.first().context("no folder")?);
    let list = input()?;
    let paths: Vec<PathBuf> = list
        .as_array()
        .context("stdin is not a list")?
        .iter()
        .map(|path| path.as_str().map(PathBuf::from).context("a path that is not text"))
        .collect::<Result<_>>()?;
    std::fs::create_dir_all(&folder).with_context(|| format!("making {}", folder.display()))?;
    let stop = stopped();
    let room = Arc::new(tokio::sync::Semaphore::new(CONVERSIONS));
    let mut tasks = Vec::new();
    for (k, path) in paths.into_iter().enumerate() {
        let folder = folder.clone();
        let room = room.clone();
        tasks.push(tokio::spawn(async move {
            let made = async {
                let out = folder.join(jpeg_name(&path)?);
                if out.exists() {
                    // Recently used, for tidy.
                    if let Ok(file) = std::fs::File::options().write(true).open(&out) {
                        let _ = file.set_modified(SystemTime::now());
                    }
                    return Ok::<PathBuf, anyhow::Error>(out);
                }
                let _turn = room.acquire().await?;
                // Written aside, under a name of its own, and moved: two at
                // once never clash, and a half-made one is never taken.
                let part = out.with_extension(format!("{}.{k}.part", std::process::id()));
                let mut magick = tokio::process::Command::new("magick");
                magick.arg(&path).args(["-quality", "92"]).arg(format!("jpg:{}", part.display()));
                // Gone with this, should it be stopped.
                unsafe {
                    magick.pre_exec(|| {
                        libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGTERM);
                        Ok(())
                    });
                }
                let status = magick.kill_on_drop(true).status().await?;
                if !status.success() {
                    let _ = std::fs::remove_file(&part);
                    bail!("magick could not convert {}", path.display());
                }
                std::fs::rename(&part, &out)?;
                Ok(out)
            };
            match made.await {
                Ok(out) => say(&format!("{k} {}", out.display())),
                Err(err) => {
                    eprintln!("pinhole-send: {err:#}");
                    say(&format!("{k} !"));
                }
            }
        }));
    }
    tokio::select! {
        _ = futures_util::future::join_all(tasks) => {}
        _ = stop.cancelled() => {
            // The parts being written go with this; tidy takes any left.
            for entry in std::fs::read_dir(&folder)?.flatten() {
                if entry.file_name().to_string_lossy().contains(&format!(".{}.", std::process::id())) {
                    let _ = std::fs::remove_file(entry.path());
                }
            }
            end("cancelled");
        }
    }
    Ok(())
}

fn tidy(args: &[String]) -> Result<()> {
    let folder = PathBuf::from(args.first().context("no folder")?);
    // Each source's library has a folder of its own, beside this one: the
    // others' are kept small too, though not shown now.
    let mut folders = vec![folder.clone()];
    if let Some(entries) = folder.parent().and_then(|parent| std::fs::read_dir(parent).ok()) {
        for entry in entries.flatten() {
            if entry.file_type().is_ok_and(|t| t.is_dir()) && entry.path() != folder {
                folders.push(entry.path());
            }
        }
    }
    for folder in folders {
        tidy_one(&folder);
    }
    Ok(())
}

fn tidy_one(folder: &Path) {
    const DAY: Duration = Duration::from_secs(24 * 60 * 60);
    let age = |path: &Path| -> Duration {
        std::fs::symlink_metadata(path)
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| t.elapsed().ok())
            .unwrap_or_default()
    };
    // Staged for LocalSend: links to the originals, which would keep them
    // on disk past the storage limit.
    if let Ok(entries) = std::fs::read_dir(folder.join("localsend")) {
        for entry in entries.flatten() {
            if age(&entry.path()) > DAY {
                let _ = std::fs::remove_dir_all(entry.path());
            }
        }
    }
    // JPEGs: not used for thirty days, then the oldest past 2 GB; parts
    // left by a stop an hour on.
    let mut kept = Vec::new();
    if let Ok(entries) = std::fs::read_dir(folder.join("jpeg")) {
        for entry in entries.flatten() {
            let path = entry.path();
            let old = age(&path);
            let part = path.extension().is_some_and(|e| e == "part");
            if (part && old > Duration::from_secs(3600)) || (!part && old > 30 * DAY) {
                let _ = std::fs::remove_file(&path);
            } else if !part {
                kept.push((old, entry.metadata().map(|m| m.len()).unwrap_or(0), path));
            }
        }
    }
    kept.sort_by(|a, b| b.0.cmp(&a.0));
    let mut size: u64 = kept.iter().map(|k| k.1).sum();
    for (_, bytes, path) in kept {
        if size <= 2 << 30 {
            break;
        }
        if std::fs::remove_file(&path).is_ok() {
            size -= bytes;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_pin_is_itself_in_the_query() {
        assert_eq!(query_value("1234"), "1234");
        assert_eq!(query_value("a&b"), "a%26b");
        assert_eq!(query_value("x#y"), "x%23y");
        assert_eq!(query_value("1+2"), "1%2B2");
        assert_eq!(query_value("50%"), "50%25");
        assert_eq!(query_value("a b"), "a%20b");
        assert_eq!(query_value("ż"), "%C5%BC");
    }

    #[test]
    fn three_wrong_pins_are_not_a_busy_device() {
        assert_eq!(refusal(401, false), "pin");
        assert_eq!(refusal(401, true), "wrongpin");
        assert_eq!(refusal(403, false), "declined");
        assert_eq!(refusal(409, false), "busy");
        assert_eq!(refusal(429, true), "blocked");
        assert_eq!(refusal(500, false), "rejected 500");
    }

    #[test]
    fn the_pin_comes_with_the_files() {
        let given: Value = serde_json::from_str(r#"{"files": [{"path": "/a/IMG_1.HEIC", "name": "../IMG_1.HEIC"}], "pin": "1+2"}"#).unwrap();
        assert_eq!(files_of(&given["files"]).unwrap(), vec![(PathBuf::from("/a/IMG_1.HEIC"), "IMG_1.HEIC".to_string())]);
        assert_eq!(given["pin"].as_str().map(query_value).as_deref(), Some("1%2B2"));
    }

    #[test]
    fn tidy_keeps_each_sources_cache_small() {
        let root = std::env::temp_dir().join(format!("pinhole-send-tidy-{}", std::process::id()));
        let old = SystemTime::now() - Duration::from_secs(40 * 24 * 60 * 60);
        for source in ["icloud-a", "folder-b"] {
            std::fs::create_dir_all(root.join(source).join("jpeg")).unwrap();
            std::fs::create_dir_all(root.join(source).join("localsend/1")).unwrap();
            for name in ["old.jpg", "new.jpg"] {
                std::fs::write(root.join(source).join("jpeg").join(name), b"x").unwrap();
            }
            let file = std::fs::File::options().write(true).open(root.join(source).join("jpeg/old.jpg")).unwrap();
            file.set_modified(old).unwrap();
            let staged = std::fs::File::open(root.join(source).join("localsend/1")).unwrap();
            staged.set_modified(old).unwrap();
        }
        tidy(&[root.join("icloud-a").to_string_lossy().into_owned()]).unwrap();
        for source in ["icloud-a", "folder-b"] {
            assert!(!root.join(source).join("jpeg/old.jpg").exists(), "{source}");
            assert!(root.join(source).join("jpeg/new.jpg").exists(), "{source}");
            assert!(!root.join(source).join("localsend/1").exists(), "{source}");
        }
        std::fs::remove_dir_all(&root).unwrap();
    }
}
