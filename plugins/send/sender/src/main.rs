//! pinhole-send: the Send to phone plugin's way to LocalSend (plugins/send),
//! with LocalSend's own protocol code. Each command writes a line for each
//! thing that happens, as it happens, for the plugin to show:
//!
//! `pinhole-send devices [<device> ...]`
//!     The LocalSend devices nearby, a JSON line for each as it answers
//!     (again from a nearer address), until stdin closes or it is stopped. The devices given, as those lines, are where devices were
//!     last: asked first, since a phone that is already open does not
//!     announce itself.
//!
//! `pinhole-send send <device> <path> <name> [<path> <name> ...]`
//!     The files to the device, each under its name: `asking` while its
//!     owner decides, `accepted <files> <bytes>`, `progress <sent> <total>`
//!     as it goes, and `sent <files>`; or `declined`, `busy`, `pin` or
//!     `failed <why>`, exiting 1. Stopped (SIGTERM, or stdin closed), it
//!     tells the device and says `cancelled`.
//!
//! `pinhole-send stage <folder> <path> <name> [<path> <name> ...]`
//!     The files under their names in the folder (linked where they can be,
//!     else copied), for LocalSend's own window: their paths, a line each.
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
use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::io::AsyncReadExt;
use tokio::sync::{mpsc, oneshot};
use tokio_stream::wrappers::ReceiverStream;
use tokio_util::sync::CancellationToken;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let runtime = tokio::runtime::Runtime::new().expect("a tokio runtime");
    let result = runtime.block_on(async {
        match args.first().map(String::as_str) {
            Some("devices") => devices(&args[1..]).await,
            Some("send") => send(&args[1..]).await,
            Some("stage") => stage(&args[1..]),
            _ => bail!("usage: pinhole-send devices [<device> ...] | send <device> <path> <name> ... | stage <folder> <path> <name> ..."),
        }
    });
    if let Err(err) = result {
        say(&format!("failed {err:#}"));
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

/// Cancelled when the plugin is done with it: SIGTERM, SIGINT, or stdin
/// closing (the window gone, however it went).
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
    let on_eof = token.clone();
    tokio::spawn(async move {
        let mut stdin = tokio::io::stdin();
        let mut buffer = [0u8; 256];
        while let Ok(n) = stdin.read(&mut buffer).await {
            if n == 0 {
                break;
            }
        }
        on_eof.cancel();
    });
    token
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
        eprintln!("multicast unavailable: {err:#}");
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
    let known: Vec<HttpChannel> = known
        .iter()
        .filter_map(|line| serde_json::from_str::<Value>(line).ok())
        .filter_map(|device| channel_of(&device).ok())
        .collect();
    tokio::spawn({
        let discovery = discovery.clone();
        async move {
            let networks = local_interface_addresses(&InterfaceFilter::default()).unwrap_or_default();
            let _ = discovery
                .discover_staged(known.clone(), networks, DEFAULT_PORT, ProtocolType::Https, Duration::from_secs(2))
                .await;
            loop {
                tokio::time::sleep(Duration::from_secs(10)).await;
                let _ = discovery.discover_known_http_channels(known.clone()).await;
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
                    // Again only from a nearer address: a phone answers on
                    // each network it is on (Wi-Fi, a VPN, IPv6).
                    let rank = device.channel.http().map(|http| nearness(&http.host)).unwrap_or(u8::MAX);
                    if said.get(&device.fingerprint).is_some_and(|&said| said <= rank) {
                        continue;
                    }
                    if let Some(line) = device_line(&device) {
                        said.insert(device.fingerprint.clone(), rank);
                        say(&line.to_string());
                    }
                }
                Some(DiscoveryEvent::MulticastFailed) => eprintln!("multicast stopped"),
                None => break,
            },
            _ = stop.cancelled() => break,
            _ = &mut deadline => break,
        }
    }
    Ok(())
}

/// How near an address is, nearest 0: the local network's own, then any
/// other IPv4 (a VPN's), then IPv6.
fn nearness(host: &str) -> u8 {
    match host.parse::<std::net::IpAddr>() {
        Ok(std::net::IpAddr::V4(ip)) if ip.is_private() => 0,
        Ok(std::net::IpAddr::V4(_)) => 1,
        _ => 2,
    }
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

async fn send(args: &[String]) -> Result<()> {
    let device: Value = serde_json::from_str(args.first().context("no device")?).context("the device")?;
    let pairs = &args[1..];
    if pairs.is_empty() || pairs.len() % 2 != 0 {
        bail!("files go as <path> <name> pairs");
    }
    let http = channel_of(&device)?;
    let identity = Identity::load()?;
    let stop = stopped();

    let mut files = HashMap::new();
    let mut paths = HashMap::new();
    let mut order = Vec::new();
    for (k, pair) in pairs.chunks(2).enumerate() {
        let path = PathBuf::from(&pair[0]);
        let size = std::fs::metadata(&path).with_context(|| format!("reading {}", path.display()))?.len();
        let id = format!("pinhole-{k}");
        files.insert(
            id.clone(),
            FileDto {
                id: id.clone(),
                file_name: pair[1].clone(),
                size,
                file_type: file_type(&pair[1]).to_string(),
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
    let payload = PrepareUploadRequestDtoV2 { info: identity.register_dto(DEFAULT_PORT), files: files.clone() };
    let prepared = match client
        .prepare_upload(http.protocol, &http.host, http.port, None, payload, None, stop.clone())
        .await
    {
        Ok(prepared) => prepared,
        Err(ClientError::Cancelled) => {
            say("cancelled");
            std::process::exit(1);
        }
        Err(ClientError::StatusCode(err)) => {
            say(match err.status {
                401 => "pin",
                403 => "declined",
                409 => "busy",
                _ => return Err(anyhow!("the device answered {}", err.status)),
            });
            std::process::exit(1);
        }
        Err(err) => return Err(err.into()),
    };
    let Some(response) = prepared.response else {
        say("declined");
        std::process::exit(1);
    };

    let accepted: Vec<&String> = order.iter().filter(|id| response.files.contains_key(*id)).collect();
    let total: u64 = accepted.iter().map(|id| files[*id].size).sum();
    say(&format!("accepted {} {total}", accepted.len()));

    // One file after another, saying how far it is ten times a second.
    let (progress_tx, mut progress_rx) = mpsc::unbounded_channel::<u64>();
    tokio::spawn(async move {
        let mut last = Instant::now() - Duration::from_secs(1);
        while let Some(sent) = progress_rx.recv().await {
            if last.elapsed() >= Duration::from_millis(100) || sent == total {
                last = Instant::now();
                say(&format!("progress {sent} {total}"));
            }
        }
    });
    let mut sent = 0u64;
    for id in &accepted {
        let token = &response.files[*id];
        let body = {
            let progress_tx = progress_tx.clone();
            let base = sent;
            let mut of_file = 0u64;
            let stream = ReceiverStream::new(FileContent::Path(paths[*id].clone()).into_receiver()).map(move |chunk| {
                of_file += chunk.len() as u64;
                let _ = progress_tx.send(base + of_file);
                Ok::<Bytes, anyhow::Error>(chunk)
            });
            localsend::reqwest::Body::wrap_stream(stream)
        };
        let result = client
            .upload(http.protocol, &http.host, http.port, None, &response.session_id, id, token, body, stop.clone())
            .await;
        if let Err(err) = result {
            let _ = client.cancel(http.protocol, &http.host, http.port, &response.session_id).await;
            if matches!(err, ClientError::Cancelled) {
                say("cancelled");
                std::process::exit(1);
            }
            return Err(anyhow!("{}: {err}", files[*id].file_name));
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
    let pairs = &args[1..];
    if pairs.len() % 2 != 0 {
        bail!("files go as <path> <name> pairs");
    }
    std::fs::create_dir_all(&folder).with_context(|| format!("making {}", folder.display()))?;
    for pair in pairs.chunks(2) {
        let name = Path::new(&pair[1]).file_name().context("a file without a name")?;
        // Two photos of one name (a camera's counter come round) both go.
        let mut to = folder.join(name);
        let mut n = 2;
        while to.exists() {
            let stem = Path::new(name).file_stem().unwrap_or(name).to_string_lossy();
            let extension = Path::new(name).extension().map(|e| format!(".{}", e.to_string_lossy())).unwrap_or_default();
            to = folder.join(format!("{stem} ({n}){extension}"));
            n += 1;
        }
        if std::fs::hard_link(&pair[0], &to).is_err() {
            std::fs::copy(&pair[0], &to).with_context(|| format!("copying {}", pair[0]))?;
        }
        say(&to.to_string_lossy());
    }
    Ok(())
}
