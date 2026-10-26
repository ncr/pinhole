//! Told by Apple when the photos change, as icloud.com is: a WebSocket to
//! Apple's web push service gives a push token, registered for the photos
//! container with PDS (Apple's push registry, with a JWT the socket hands
//! out) and with CloudKit's device service. A change to the library then
//! comes down the socket within seconds. Nothing in it says what changed:
//! a sync round asks. See docs/service.md, Staying in sync.

use std::io::ErrorKind;
use std::net::TcpStream;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use base64::Engine;
use p256::ecdsa::signature::Signer;
use p256::ecdsa::{Signature, SigningKey};
use rand::RngCore;
use reqwest::blocking::Client;
use serde_json::{json, Value};
use tungstenite::client::IntoClientRequest;
use tungstenite::stream::MaybeTlsStream;
use tungstenite::{Message, WebSocket};

use crate::icloud::auth::{self, ProcessProvider, SessionProvider};
use crate::icloud::session::Session;
use crate::source::Heard;

const CONTAINER: &str = "com.apple.photos.cloud";
const TOPIC: &str = "com.apple.icloud-container.com.apple.photos.cloud";
const SOCKET: &str = "wss://websocket.push.apple.com/v2";
const PDS: &str = "https://gatewayws.icloud.com/pds/WebObjects/pushregistry.woa/webBatchRegister/batchRegisterToken";
/// How long a registration holds; it is made again a while before.
const REGISTERED_FOR: Duration = Duration::from_secs(3600);
/// Waits between tries after a failure: doubling from the first to the last.
const RETRY_FIRST: Duration = Duration::from_secs(5);
const RETRY_LAST: Duration = Duration::from_secs(300);
/// The order of P-256's field: icloud.com keeps only keys whose y is at most
/// half of it, and so does this.
const P256_P: [u8; 32] = [
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
];

type Socket = WebSocket<MaybeTlsStream<TcpStream>>;

/// Listen for the Apple ID's photo changes until `stop` is set, telling
/// `tell` of each and of whether it is listening. Signs in through the
/// provider by itself; on any failure it says it is not listening, waits
/// (longer each time) and tries again.
pub fn listen(provider: PathBuf, account: String, tell: Box<dyn Fn(Heard) + Send + Sync>, stop: Arc<AtomicBool>) {
    let mut wait = RETRY_FIRST;
    while !stop.load(Ordering::Relaxed) {
        let started = Instant::now();
        match connection(&provider, &account, &*tell, &stop) {
            Ok(()) => return,
            Err(e) => eprintln!("pinholed: push: {e:#}"),
        }
        tell(Heard::Listening(false));
        // A connection that held a while was not a failure to back off from.
        if started.elapsed() > RETRY_LAST {
            wait = RETRY_FIRST;
        }
        let until = Instant::now() + wait;
        while Instant::now() < until && !stop.load(Ordering::Relaxed) {
            std::thread::sleep(Duration::from_millis(250));
        }
        wait = (wait * 2).min(RETRY_LAST);
    }
}

/// One connection, from signing in to its end: Ok when stopped.
fn connection(provider: &PathBuf, account: &str, tell: &dyn Fn(Heard), stop: &AtomicBool) -> Result<()> {
    let session = ProcessProvider::spawn(provider.clone(), Some(auth::state_dir(account)))?.session()?;
    let http = Client::builder().timeout(Duration::from_secs(30)).build()?;
    let key = keypair();
    let (mut socket, token) = open(&key)?;
    send(&mut socket, &field(3, &field(1, TOPIC))).context("filtering the push topics")?; // WebFilterMessage
    let mut registered: Option<Instant> = None;
    let mut jwt_asked = false;
    loop {
        if stop.load(Ordering::Relaxed) {
            let _ = socket.close(None);
            return Ok(());
        }
        // A new JWT for each registration, a while before the last runs out.
        if !jwt_asked && registered.is_none_or(|t| t.elapsed() > REGISTERED_FOR * 4 / 5) {
            send(&mut socket, &field(7, &field(1, 600u64)))?; // WebJwtRequestMessage
            jwt_asked = true;
        }
        let message = match socket.read() {
            Ok(Message::Binary(data)) => data,
            Ok(Message::Close(frame)) => return Err(anyhow!("Apple closed the push connection: {frame:?}")),
            Ok(_) => continue,
            Err(tungstenite::Error::Io(e)) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => continue,
            Err(e) => return Err(anyhow!("push connection: {e}")),
        };
        let message = parse(&message)?;
        if let Some(reply) = message.bytes(8) {
            // WebJwtResponseMessage: register the token with it.
            jwt_asked = false;
            let reply = parse(reply)?;
            let jwt = reply.bytes(1).map(|j| String::from_utf8_lossy(j).into_owned()).unwrap_or_default();
            if jwt.is_empty() {
                return Err(anyhow!("Apple gave no JWT (status {:?})", reply.int(2)));
            }
            register(&http, &session, &key, &token, &jwt)?;
            if registered.is_none() {
                eprintln!("pinholed: push: Apple tells of changes to {account}'s photos");
                tell(Heard::Listening(true));
            }
            registered = Some(Instant::now());
        } else if let Some(push) = message.bytes(2) {
            // WebClientPushMessage: acknowledged, and passed on if it is
            // about the photos.
            let push = parse(push)?;
            let topic = push.bytes(1).unwrap_or_default().to_vec();
            let id = push.int(2).unwrap_or(0);
            send(&mut socket, &field(2, &[field(1, &topic[..]), field(2, id), field(3, 0u64)].concat()))?;
            let payload: Value = push.bytes(4).and_then(|p| serde_json::from_slice(p).ok()).unwrap_or(Value::Null);
            let about = payload["ck"]["cid"].as_str().unwrap_or("?");
            eprintln!("pinholed: push: a change to {about}");
            if about == "?" || about == CONTAINER {
                tell(Heard::Changed);
            }
        }
    }
}

/// The socket, connected: its push token.
fn open(key: &SigningKey) -> Result<(Socket, Vec<u8>)> {
    let (public, nonce, signature) = signed_nonce(key, None);
    // WebClientConnectionMessage { ConnectionMessageV1 { key, nonce,
    // signature, NewConnectionRequest { token for a day } } }, in the path.
    let v1 = [field(1, &public[..]), field(2, &nonce[..]), field(3, &signature[..]), field(5, &field(1, 86400u64))].concat();
    let url = format!("{SOCKET}/{}", hex(&field(1, &v1)));
    let mut request = url.into_client_request()?;
    request.headers_mut().insert("Origin", "https://www.icloud.com".parse()?);
    let (mut socket, _) = tungstenite::connect(request).context("connecting to Apple's push service")?;
    let tcp = match socket.get_mut() {
        MaybeTlsStream::Rustls(s) => s.get_mut(),
        MaybeTlsStream::Plain(s) => s,
        _ => return Err(anyhow!("an unexpected kind of push connection")),
    };
    // So a stop is seen within a second, between messages.
    tcp.set_read_timeout(Some(Duration::from_secs(1)))?;
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let data = match socket.read() {
            Ok(Message::Binary(data)) => data,
            Ok(_) => continue,
            Err(tungstenite::Error::Io(e)) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {
                if Instant::now() > deadline {
                    return Err(anyhow!("Apple's push service did not answer"));
                }
                continue;
            }
            Err(e) => return Err(anyhow!("push connection: {e}")),
        };
        // ServerToWebClientMessage { WebConnectionResponseMessage { token } }
        let Some(response) = parse(&data)?.bytes(1).map(parse).transpose()? else { continue };
        let token = response.bytes(1).unwrap_or_default();
        if token.is_empty() {
            return Err(anyhow!("Apple's push service gave no token (status {:?})", response.int(2)));
        }
        let token = base64::engine::general_purpose::STANDARD.decode(token).context("the push token")?;
        return Ok((socket, token));
    }
}

fn send(socket: &mut Socket, message: &[u8]) -> Result<()> {
    socket.send(Message::Binary(message.to_vec().into())).context("writing to the push connection")
}

/// The token registered for the photos container: with PDS, signed with
/// the key and carrying the socket's JWT and the session's cookies, and
/// with CloudKit's device service, as icloud.com does.
fn register(http: &Client, session: &Session, key: &SigningKey, token: &[u8], jwt: &str) -> Result<()> {
    let (public, nonce, signature) = signed_nonce(key, Some(2));
    let web_auth = [field(1, &nonce[..]), field(2, &public[..]), field(3, &signature[..]), field(4, jwt)].concat();
    let push_token = [
        field(1, session.dsid.as_str()),
        field(2, &field(2, &[][..])), // UserAuth { ICloudAuth {} }: the cookies
        field(3, hex(token).to_uppercase().as_str()),
        field(4, &field(1, &web_auth)),
    ]
    .concat();
    let topic = [field(1, TOPIC), field(2, format!("{CONTAINER}_P").as_str()), field(4, 5u64)].concat(); // client ICLOUDCOM
    let body = [field(1, &[field(1, &push_token), field(4, &topic)].concat()), field(2, REGISTERED_FOR.as_secs())].concat();
    let url = url::Url::parse(PDS)?;
    let mut request = http
        .post(url.clone())
        .header("x-protocol-version", "1")
        .header("x-pds-client-version", "2")
        .header("x-pds-client-type", "iCloudCom")
        .header("x-registration-reason", "1")
        .header("Content-Type", "application/protobuf")
        .header("Origin", "https://www.icloud.com")
        .header("Referer", "https://www.icloud.com/");
    if let Some(agent) = session.headers.get("User-Agent") {
        request = request.header("User-Agent", agent);
    }
    if let Some(cookie) = session.cookie_header(&url) {
        request = request.header("Cookie", cookie);
    }
    let response = request.body(body).send().context("registering for pushes with PDS")?;
    let status = response.status();
    let bytes = response.bytes()?;
    let answer = parse(&bytes).unwrap_or_default();
    // BatchRegisterResp { status: 0 is done }
    if !status.is_success() || answer.int(1) != Some(0) {
        let message = answer.bytes(2).map(|m| String::from_utf8_lossy(m).into_owned()).unwrap_or_default();
        return Err(anyhow!("PDS answered {status}, status {:?} {message}", answer.int(1)));
    }

    let device = session.service("ckdeviceservice").ok_or_else(|| anyhow!("the session has no ckdeviceservice"))?;
    let mut url = url::Url::parse(&format!("{device}/device/1/{CONTAINER}/production/tokens/register"))?;
    url.query_pairs_mut().extend_pairs(&session.params);
    let mut request = http.post(url.clone()).header("Content-Type", "text/plain");
    for (k, v) in &session.headers {
        request = request.header(k, v);
    }
    if let Some(cookie) = session.cookie_header(&url) {
        request = request.header("Cookie", cookie);
    }
    let client_id = session.params.get("clientId").cloned().unwrap_or_default();
    let body = json!({"apnsEnvironment": "production", "apnsToken": hex(token), "clientID": client_id});
    let response = request.body(body.to_string()).send().context("registering for pushes with CloudKit")?;
    if !response.status().is_success() {
        return Err(anyhow!("CloudKit's tokens/register answered {}", response.status()));
    }
    Ok(())
}

/// A P-256 key of the kind icloud.com makes: its y at most p - y.
fn keypair() -> SigningKey {
    loop {
        let key = SigningKey::random(&mut rand::rngs::OsRng);
        let point = key.verifying_key().to_encoded_point(false);
        let y: [u8; 32] = point.y().expect("an uncompressed point").as_slice().try_into().expect("32 bytes");
        if y <= minus(&P256_P, &y) {
            return key;
        }
    }
}

/// a - b, both big-endian and a >= b.
fn minus(a: &[u8; 32], b: &[u8; 32]) -> [u8; 32] {
    let mut out = [0u8; 32];
    let mut borrow = 0i16;
    for i in (0..32).rev() {
        let mut d = a[i] as i16 - b[i] as i16 - borrow;
        borrow = (d < 0) as i16;
        if d < 0 {
            d += 256;
        }
        out[i] = d as u8;
    }
    out
}

/// The public key (uncompressed), a nonce (the time and random bytes;
/// with `purpose` for a registration) and its signature, as icloud.com
/// signs them: ECDSA over SHA-256, in DER behind a version prefix.
fn signed_nonce(key: &SigningKey, purpose: Option<u8>) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
    let millis = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_millis() as u64);
    let mut random = [0u8; 8];
    rand::rngs::OsRng.fill_bytes(&mut random);
    let mut nonce = Vec::with_capacity(18);
    nonce.push(purpose.is_some() as u8);
    nonce.extend_from_slice(&millis.to_be_bytes());
    if let Some(p) = purpose {
        nonce.push(p);
    }
    nonce.extend_from_slice(&random);
    let signature: Signature = key.sign(&nonce);
    let (r, s) = signature.split_bytes();
    let integer = |n: &[u8]| -> Vec<u8> {
        let mut v = vec![0x02, (n.len() + (n[0] > 127) as usize) as u8];
        if n[0] > 127 {
            v.push(0);
        }
        v.extend_from_slice(n);
        v
    };
    let body = [integer(&r), integer(&s)].concat();
    let signed = [&[0x01, 0x03, 0x30, body.len() as u8][..], &body].concat();
    let public = key.verifying_key().to_encoded_point(false).as_bytes().to_vec();
    (public, nonce, signed)
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

// Protobuf, as little of it as this needs: varints and length-delimited
// fields, written and read by number.

trait Wire {
    fn put(&self, number: u32, out: &mut Vec<u8>);
}

impl Wire for u64 {
    fn put(&self, number: u32, out: &mut Vec<u8>) {
        varint((number as u64) << 3, out);
        varint(*self, out);
    }
}

impl Wire for &[u8] {
    fn put(&self, number: u32, out: &mut Vec<u8>) {
        varint((number as u64) << 3 | 2, out);
        varint(self.len() as u64, out);
        out.extend_from_slice(self);
    }
}

impl Wire for &Vec<u8> {
    fn put(&self, number: u32, out: &mut Vec<u8>) {
        self.as_slice().put(number, out)
    }
}

impl Wire for &str {
    fn put(&self, number: u32, out: &mut Vec<u8>) {
        self.as_bytes().put(number, out)
    }
}

fn field(number: u32, value: impl Wire) -> Vec<u8> {
    let mut out = Vec::new();
    value.put(number, &mut out);
    out
}

fn varint(mut n: u64, out: &mut Vec<u8>) {
    loop {
        let byte = (n & 0x7f) as u8;
        n >>= 7;
        if n == 0 {
            out.push(byte);
            return;
        }
        out.push(byte | 0x80);
    }
}

/// A message's fields by number: varints and length-delimited ones (the
/// last of each number).
#[derive(Default)]
struct Fields<'a>(Vec<(u32, Field<'a>)>);

enum Field<'a> {
    Int(u64),
    Bytes(&'a [u8]),
}

impl<'a> Fields<'a> {
    fn int(&self, number: u32) -> Option<u64> {
        self.0.iter().rev().find_map(|(n, f)| match f {
            Field::Int(v) if *n == number => Some(*v),
            _ => None,
        })
    }

    fn bytes(&self, number: u32) -> Option<&'a [u8]> {
        self.0.iter().rev().find_map(|(n, f)| match f {
            Field::Bytes(v) if *n == number => Some(*v),
            _ => None,
        })
    }
}

fn parse(data: &[u8]) -> Result<Fields<'_>> {
    let mut fields = Vec::new();
    let mut at = 0;
    let read = |at: &mut usize| -> Result<u64> {
        let (mut n, mut shift) = (0u64, 0);
        loop {
            let byte = *data.get(*at).ok_or_else(|| anyhow!("a push message cut short"))?;
            *at += 1;
            n |= ((byte & 0x7f) as u64) << shift;
            if byte & 0x80 == 0 {
                return Ok(n);
            }
            shift += 7;
            if shift > 63 {
                return Err(anyhow!("a push message with a bad number"));
            }
        }
    };
    while at < data.len() {
        let key = read(&mut at)?;
        let number = (key >> 3) as u32;
        match key & 7 {
            0 => fields.push((number, Field::Int(read(&mut at)?))),
            2 => {
                let len = read(&mut at)? as usize;
                let bytes = data.get(at..at + len).ok_or_else(|| anyhow!("a push message cut short"))?;
                at += len;
                fields.push((number, Field::Bytes(bytes)));
            }
            1 => at += 8,
            5 => at += 4,
            other => return Err(anyhow!("a push message with wire type {other}")),
        }
    }
    Ok(Fields(fields))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fields_go_out_and_come_back() {
        let message = [field(1, "token"), field(2, 300u64), field(4, &field(1, &[1u8, 2, 3][..]))].concat();
        let read = parse(&message).unwrap();
        assert_eq!(read.bytes(1), Some(&b"token"[..]));
        assert_eq!(read.int(2), Some(300));
        assert_eq!(parse(read.bytes(4).unwrap()).unwrap().bytes(1), Some(&[1u8, 2, 3][..]));
        assert!(parse(&[0x0a, 5, 1]).is_err());
    }

    #[test]
    fn a_nonce_is_signed_as_icloud_com_signs_it() {
        let key = keypair();
        let (public, nonce, signature) = signed_nonce(&key, Some(2));
        assert_eq!((public.len(), public[0]), (65, 4));
        assert_eq!((nonce.len(), nonce[0], nonce[9]), (18, 1, 2));
        assert_eq!(signed_nonce(&key, None).1.len(), 17);
        assert_eq!(&signature[..3], &[1, 3, 0x30]);
        assert_eq!(signature[3] as usize, signature.len() - 4);
        // Without the prefix, the DER signature verifies.
        use p256::ecdsa::signature::Verifier;
        let der = Signature::from_der(&signature[2..]).unwrap();
        key.verifying_key().verify(&nonce, &der).unwrap();
    }

    #[test]
    fn subtraction_borrows() {
        let mut one = [0u8; 32];
        one[31] = 1;
        let mut b = [0u8; 32];
        b[30] = 1;
        let r = minus(&b, &one);
        assert_eq!((r[30], r[31]), (0, 255));
    }
}
