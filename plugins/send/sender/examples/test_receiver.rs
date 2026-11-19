// A LocalSend receiver for testing pinhole-send: test_receiver <port> <folder> [decline] [quiet] [pin=<pin>] [stall] [none] [lan]
// Accepts (or declines) everything from this computer, saves into <folder>, which must not exist yet, logs to stdout.
// "lan": also takes files from other devices on the network. LocalSend's server listens on every interface,
// so without it an offer from any address that is not this computer's own is declined.
// A file is saved only under a plain name of its own and never over an existing one.
// "quiet": does not announce itself (a phone whose LocalSend has been open a while).
// "pin=1234": asks for that PIN. "stall": takes the files but stops reading them.
// "none": accepts none of them (204).
use localsend::discovery::{DeviceIdentity, DiscoveryConfig, DEFAULT_DISCOVERY_TIMEOUT};
use localsend::http::server::common::save::FileUploadTarget;
use localsend::http::server::v2::{PrepareUploadDecisionV2, ServerEventV2};
use localsend::http::server::{start_with_port, ServerConfigV2, TlsConfig};
use localsend::http::state::ClientInfo;
use localsend::model::discovery::{DeviceType, ProtocolType, PROTOCOL_VERSION_V2};
use localsend::multicast::{MulticastDevice, DEFAULT_MULTICAST_GROUP, DEFAULT_MULTICAST_GROUP_V6, DEFAULT_PORT};
use localsend::util::interface::InterfaceFilter;
use std::net::{SocketAddr, SocketAddrV6, UdpSocket};
use std::path::{Component, Path, PathBuf};
use std::time::Duration;
use tokio::sync::{mpsc, oneshot};

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    let port: u16 = args[1].parse().unwrap();
    let folder = PathBuf::from(&args[2]);
    let decline = args.iter().any(|a| a == "decline");
    let quiet = args.iter().any(|a| a == "quiet");
    let stall = args.iter().any(|a| a == "stall");
    let none = args.iter().any(|a| a == "none");
    let lan = args.iter().any(|a| a == "lan");
    let pin = args.iter().find_map(|a| a.strip_prefix("pin=").map(str::to_string));
    std::fs::create_dir(&folder).unwrap_or_else(|e| panic!("{} must be a new folder: {e}", folder.display()));
    let cert = localsend::crypto::cert::generate_self_signed().unwrap();
    let (tx, mut rx) = mpsc::channel(16);
    let (_stop, stop_rx) = oneshot::channel();
    let _server = start_with_port(
        port,
        Some(TlsConfig { cert: cert.certificate_pem.clone(), private_key: cert.private_key_pem.clone() }),
        ClientInfo {
            alias: "Test phone".into(),
            version: PROTOCOL_VERSION_V2.into(),
            device_model: Some("Testphone".into()),
            device_type: Some(DeviceType::Mobile),
            token: cert.fingerprint.clone(),
        },
        None,
        Some(ServerConfigV2 { pin, verify_checksums: false, event_tx: tx }),
        None,
        stop_rx,
    )
    .await
    .unwrap();
    let (_dstop, dstop_rx) = oneshot::channel();
    let discovery = localsend::discovery::start(
        DiscoveryConfig {
            group: DEFAULT_MULTICAST_GROUP,
            group_v6: Some(DEFAULT_MULTICAST_GROUP_V6),
            port: DEFAULT_PORT,
            interface_filter: InterfaceFilter::default(),
            device: MulticastDevice {
                alias: "Test phone".into(),
                version: PROTOCOL_VERSION_V2.into(),
                device_model: Some("Testphone".into()),
                device_type: Some(DeviceType::Mobile),
                fingerprint: cert.fingerprint.clone(),
                port,
                protocol: ProtocolType::Https,
                download: false,
            },
            identity: DeviceIdentity { cert_pem: cert.certificate_pem.clone(), private_key_pem: cert.private_key_pem.clone() },
            timeout: DEFAULT_DISCOVERY_TIMEOUT,
            event_tx: None,
        },
        dstop_rx,
    )
    .await;
    if !quiet {
        tokio::spawn(async move {
            loop {
                discovery.announce().await;
                tokio::time::sleep(Duration::from_secs(3)).await;
            }
        });
    } else {
        std::mem::forget(discovery);
    }
    println!("listening on {port}");
    while let Some(event) = rx.recv().await {
        match event {
            ServerEventV2::Register { ip, info } => println!("register {} {ip}", info.alias),
            ServerEventV2::PrepareUpload { ip, info, decision_tx, .. } if !lan && !is_this_computer(ip.ip, ip.scope_id) => {
                println!("declined {} from {ip}: not this computer (pass lan)", info.alias);
                let _ = decision_tx.send(PrepareUploadDecisionV2::Decline);
            }
            ServerEventV2::PrepareUpload { info, files, decision_tx, .. } => {
                println!("prepare from {} ({} {:?}): {:?}", info.alias, info.device_model.clone().unwrap_or_default(), info.device_type,
                    files.values().map(|f| (f.file_name.clone(), f.size, f.file_type.clone())).collect::<Vec<_>>());
                tokio::time::sleep(Duration::from_secs(1)).await;
                let _ = decision_tx.send(if decline {
                    PrepareUploadDecisionV2::Decline
                } else {
                    PrepareUploadDecisionV2::Accept(if none { Default::default() } else { files.keys().cloned().collect() })
                });
            }
            ServerEventV2::FileUpload { file, target_tx, .. } if stall => {
                println!("upload {} (not read)", file.file_name);
                let (binary_tx, binary_rx) = mpsc::channel(1);
                let (result_tx, result_rx) = oneshot::channel();
                let _ = target_tx.send(FileUploadTarget::Stream { binary_tx, result_rx });
                std::mem::forget((binary_rx, result_tx));
            }
            ServerEventV2::FileUpload { file, target_tx, .. } => {
                // Dropping target_tx answers the sender with an error.
                let Some(path) = new_file_in(&folder, &file.file_name) else {
                    println!("refused {:?}", file.file_name);
                    continue;
                };
                let (result_tx, result_rx) = oneshot::channel();
                println!("upload {}", path.display());
                let _ = target_tx.send(FileUploadTarget::Path { path, result_tx, progress_tx: None });
                tokio::spawn(async move { println!("saved {:?}", result_rx.await) });
            }
            ServerEventV2::SessionEnd { reason, .. } => println!("end {reason:?}"),
            ServerEventV2::CancelReceived { .. } => println!("cancel received"),
            _ => {}
        }
    }
}

// A sender's address that this computer can bind to is one of its own.
fn is_this_computer(ip: std::net::IpAddr, scope_id: Option<u32>) -> bool {
    let address = match ip {
        std::net::IpAddr::V6(v6) => SocketAddr::V6(SocketAddrV6::new(v6, 0, 0, scope_id.unwrap_or(0))),
        v4 => SocketAddr::new(v4, 0),
    };
    ip.is_loopback() || UdpSocket::bind(address).is_ok()
}

// The sender's name as one plain file in folder, created here so that nothing already there is written over.
fn new_file_in(folder: &Path, name: &str) -> Option<PathBuf> {
    let mut components = Path::new(name).components();
    let (Some(Component::Normal(plain)), None) = (components.next(), components.next()) else { return None };
    if plain != name {
        return None;
    }
    let path = folder.join(plain);
    std::fs::OpenOptions::new().write(true).create_new(true).open(&path).ok()?;
    Some(path)
}
