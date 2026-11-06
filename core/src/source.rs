//! What the rest of Pinhole asks for photos, whoever keeps them: iCloud, or
//! a folder on this computer. A source has a catalog, which says what is in
//! the library and what changed, and a fetch, which gets a photo's files.
//! See docs/sources.md.

use std::io::Read;
use std::path::PathBuf;
use std::time::Duration;

use anyhow::Result;

use crate::db::Index;

/// What an asset is, as the window draws it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Photo,
    Video,
    /// A photo with a few seconds of motion.
    Live,
}

/// How a Live Photo's motion is shown, as set on the iPhone: played in a
/// loop, back and forth, or as one long exposure of all its frames. A Live
/// Photo without one plays its motion once when asked.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Effect {
    Loop,
    Bounce,
    LongExposure,
}

impl Effect {
    pub const ALL: [Effect; 3] = [Effect::Loop, Effect::Bounce, Effect::LongExposure];

    pub fn name(self) -> &'static str {
        self.want().name()
    }

    pub fn parse(name: &str) -> Option<Effect> {
        Effect::ALL.into_iter().find(|e| e.name() == name)
    }

    /// The file that shows it.
    pub fn want(self) -> Want {
        match self {
            Effect::Loop => Want::Loop,
            Effect::Bounce => Want::Bounce,
            Effect::LongExposure => Want::LongExposure,
        }
    }
}

/// The files a window can ask for, whatever the source calls them.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Want {
    /// About 480 px on the long edge, kept in the library.
    Thumb,
    /// About 2048 px.
    Medium,
    /// The photo as it looks now at full size: the edit, if it has one.
    Full,
    /// As taken, unedited.
    Original,
    /// A video to play.
    Video,
    /// A Live Photo's motion.
    Live,
    /// A Live Photo's motion made to loop, to play back and forth, and its
    /// frames as one long exposure. The source's own when the effect was
    /// set on the photo (iCloud's, rendered on the iPhone); otherwise a
    /// loop is the motion itself, and the others are made by the plugin
    /// whose manifest `makes` them, the Live Photos plugin's (service.rs).
    Loop,
    Bounce,
    LongExposure,
    /// `full` as a JPEG, for whatever reads no HEIC: `full` itself when it
    /// is one, else made from it (service.rs). Never a source's rendition.
    Jpeg,
}

impl Want {
    pub const ALL: [Want; 10] = [
        Want::Thumb, Want::Medium, Want::Full, Want::Original, Want::Video, Want::Live,
        Want::Loop, Want::Bounce, Want::LongExposure, Want::Jpeg,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Want::Thumb => "thumb",
            Want::Medium => "medium",
            Want::Full => "full",
            Want::Original => "original",
            Want::Video => "video",
            Want::Live => "live",
            Want::Loop => "loop",
            Want::Bounce => "bounce",
            Want::LongExposure => "long_exposure",
            Want::Jpeg => "jpeg",
        }
    }

    pub fn parse(name: &str) -> Option<Want> {
        Want::ALL.into_iter().find(|w| w.name() == name)
    }

    /// Played while it downloads, when a window asks to stream it.
    pub fn streamable(self) -> bool {
        matches!(self, Want::Video | Want::Live | Want::Loop | Want::Bounce)
    }

    /// The effects', which the service makes when the source has none.
    pub fn effect(self) -> Option<Effect> {
        Effect::ALL.into_iter().find(|e| e.want() == self)
    }
}

/// One file of an asset, for one of the kinds a window asks for.
#[derive(Debug, Clone, PartialEq)]
pub struct Rendition {
    pub want: Want,
    /// The source's own name for it: iCloud's JPEGMed, a folder's file.
    pub name: String,
    /// Names the file in the cache and the thumbnails, and changes when
    /// the file does: iCloud's checksum, a hash of a folder file's path,
    /// size and time.
    pub key: String,
    pub width: Option<i64>,
    pub height: Option<i64>,
    pub size: Option<i64>,
    /// A UTI: public.jpeg, public.heic, com.apple.quicktime-movie...
    pub file_type: Option<String>,
}

/// A validated pair of WGS 84 coordinates in signed decimal degrees.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Location {
    latitude: f64,
    longitude: f64,
}

impl Location {
    pub fn new(latitude: f64, longitude: f64) -> Option<Self> {
        ((-90.0..=90.0).contains(&latitude) && (-180.0..=180.0).contains(&longitude))
            .then_some(Self { latitude, longitude })
    }

    pub fn degrees(self) -> (f64, f64) {
        (self.latitude, self.longitude)
    }
}

/// One photo or video in the library, as every source describes it.
#[derive(Debug, Clone, PartialEq)]
pub struct Asset {
    /// The source's id for it, unique in the library and stable as long as
    /// the asset is there.
    pub id: String,
    pub filename: Option<String>,
    /// The original's UTI.
    pub item_type: Option<String>,
    /// When it was taken and when it came into the library, milliseconds
    /// since the epoch.
    pub taken: Option<i64>,
    pub added: Option<i64>,
    pub kind: Kind,
    pub favorite: bool,
    /// A video's, in seconds.
    pub duration: Option<f64>,
    /// EXIF's, 1 to 8.
    pub orientation: Option<i64>,
    pub has_location: bool,
    pub location: Option<Location>,
    /// The original's, in bytes.
    pub size: Option<i64>,
    /// A Live Photo's, as set on the iPhone.
    pub effect: Option<Effect>,
    pub renditions: Vec<Rendition>,
}

impl Asset {
    pub fn rendition(&self, want: Want) -> Option<&Rendition> {
        self.renditions.iter().find(|r| r.want == want)
    }
}

/// A rendition as the index keeps it, enough to fetch it by.
#[derive(Debug, Clone)]
pub struct Stored {
    pub asset: String,
    pub want: Want,
    pub name: String,
    pub key: String,
    pub file_type: String,
}

/// What a source needs to read one rendition: for iCloud a download link,
/// good for a while; for a folder, the file.
#[derive(Debug, Clone)]
pub struct Ticket(pub String);

/// A rendition being read.
pub struct Body {
    pub reader: Box<dyn Read + Send>,
    /// In bytes, when the source knows.
    pub length: Option<u64>,
}

/// The longest a download waits for the next of its bytes (or for the
/// answer to start): one that stalls (the network gone, a server that
/// hangs) fails, and a download thread is free again. Not a deadline for
/// the whole, which for a long video takes minutes.
pub const READ_GAP: Duration = Duration::from_secs(60);

/// An HTTP client for downloads: 30 s to connect, at most `gap` for the
/// answer to start and between two reads, and no deadline for the whole.
pub fn download_client(gap: Duration) -> reqwest::blocking::ClientBuilder {
    // The blocking client's `timeout` is a deadline for the answer's head
    // and for each read of its body, not for the whole of it: each read
    // gives back what has come, so it is the gap between two.
    reqwest::blocking::Client::builder().connect_timeout(Duration::from_secs(30)).timeout(gap)
}

#[derive(Debug)]
pub enum FetchError {
    /// The ticket has gone stale (an expired link): prepare it again.
    Expired,
    /// The source has no such file.
    NotFound(String),
    /// The file is there, but what makes an image of it turned it down
    /// (render::Rejected: damaged, cut short, a kind its decoder does not
    /// know). That is the file's alone, never the source's health; and it
    /// is not made again until it changes (`unreadable`).
    Unreadable(String),
    /// Anything else; `Source::health` tells what it says about the
    /// connection.
    Failed(anyhow::Error),
}

impl std::fmt::Display for FetchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FetchError::Expired => write!(f, "the link expired"),
            FetchError::NotFound(what) | FetchError::Unreadable(what) => write!(f, "{what}"),
            FetchError::Failed(e) => write!(f, "{e:#}"),
        }
    }
}

/// `Unreadable` when a program turned the file down, else `Failed`.
impl From<anyhow::Error> for FetchError {
    fn from(e: anyhow::Error) -> Self {
        if crate::render::rejected(&e) {
            FetchError::Unreadable(format!("{e:#}"))
        } else {
            FetchError::Failed(e)
        }
    }
}

/// How long a file turned down (`FetchError::Unreadable`) is not made
/// again, unless it changes first (and so its key does): a program
/// updated meanwhile may make something of it.
pub const UNREADABLE_FOR: Duration = Duration::from_secs(6 * 3600);

/// Files turned down at most, kept in mind: past it, none more is.
const UNREADABLE_MAX: usize = 100_000;

/// Files turned down, by source and key: why, and when.
static UNREADABLE: std::sync::Mutex<std::collections::BTreeMap<(String, String), (std::time::Instant, String)>> =
    std::sync::Mutex::new(std::collections::BTreeMap::new());

/// Keeps in mind that the file of `source` (its id) under `key` was turned
/// down, and `why`.
pub fn note_unreadable(source: &str, key: &str, why: &str) {
    let mut all = UNREADABLE.lock().unwrap_or_else(|e| e.into_inner());
    if all.len() >= UNREADABLE_MAX {
        all.retain(|_, (at, _)| at.elapsed() < UNREADABLE_FOR);
    }
    if all.len() < UNREADABLE_MAX {
        all.insert((source.to_owned(), key.to_owned()), (std::time::Instant::now(), why.to_owned()));
    }
}

/// Why the file of `source` under `key` was turned down, if it was, less
/// than `UNREADABLE_FOR` ago.
pub fn unreadable(source: &str, key: &str) -> Option<String> {
    let mut all = UNREADABLE.lock().unwrap_or_else(|e| e.into_inner());
    let at = (source.to_owned(), key.to_owned());
    match all.get(&at) {
        Some((when, why)) if when.elapsed() < UNREADABLE_FOR => Some(why.clone()),
        Some(_) => {
            all.remove(&at);
            None
        }
        None => None,
    }
}

/// What a window should tell the person about the source.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Health {
    Fine,
    /// No session, or it was rejected: sign in again.
    NeedsSignIn,
    /// Out of reach: the network, or a disk that is not mounted.
    Offline,
    /// It answers, but with errors.
    Failing,
}

impl Health {
    pub fn name(self) -> &'static str {
        match self {
            Health::Fine => "fine",
            Health::NeedsSignIn => "needs_sign_in",
            Health::Offline => "offline",
            Health::Failing => "failing",
        }
    }
}

/// The change token is no use any more: only listing everything again
/// brings the index back in step.
#[derive(Debug)]
pub struct TokenExpired(pub String);

impl std::fmt::Display for TokenExpired {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "the change token is no longer accepted: {}", self.0)
    }
}

impl std::error::Error for TokenExpired {}

/// A listing or the making of thumbnails given up part way, because
/// another source is shown now. What was written stays: the next round
/// for this source lists again and makes only the thumbnails missing.
#[derive(Debug)]
pub struct Stopped;

impl std::fmt::Display for Stopped {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "stopped: another source is shown now")
    }
}

impl std::error::Error for Stopped {}

/// Something this source cannot do at all, such as keeping favourites:
/// the window is told `unsupported`.
#[derive(Debug)]
pub struct Unsupported(pub String);

impl std::fmt::Display for Unsupported {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl std::error::Error for Unsupported {}

/// What listing the library or following its changes came to, beyond the
/// assets themselves.
#[derive(Debug, Default)]
pub struct Listing {
    /// Where `changes` picks up from next.
    pub token: Option<String>,
    /// A line about how it went, for the service's log.
    pub summary: String,
}

/// What a source watching for changes says (`Source::watch`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Heard {
    /// Something changed: a round finds out what.
    Changed,
    /// Whether it is being told of changes now.
    Listening(bool),
}

/// Watching a source for changes, until dropped.
pub struct Watch {
    stop: std::sync::Arc<std::sync::atomic::AtomicBool>,
}

impl Watch {
    pub fn new(stop: std::sync::Arc<std::sync::atomic::AtomicBool>) -> Self {
        Watch { stop }
    }
}

impl Drop for Watch {
    fn drop(&mut self) {
        self.stop.store(true, std::sync::atomic::Ordering::Relaxed);
    }
}

pub trait Source: Send + Sync {
    /// "iCloud", or the folder's name: what the window calls it.
    fn name(&self) -> String;

    /// Its kind, as the settings name it: "folder", or pinhole.icloud's.
    fn kind(&self) -> &'static str;

    /// Which library this is, written into the index it fills, so it never
    /// writes into another's: by default its kind; a folder's says its path.
    fn id(&self) -> String {
        self.kind().to_owned()
    }

    /// What says which of its kind it is, for the window: `{"path":
    /// <folder>}`, or pinhole.icloud's `{"account": …}`; nothing when it is
    /// not yet known.
    fn details(&self) -> serde_json::Value {
        serde_json::json!({})
    }

    /// The folder its library is in, under `library/<kind>/`: unique among
    /// sources of its kind (a folder's path, pinhole.icloud's account).
    fn library_name(&self) -> String;

    /// Names its library went by in earlier versions, kept to while a
    /// library of its own is there under one of them.
    fn former_library_names(&self) -> Vec<String> {
        Vec::new()
    }

    /// How often to look for changes.
    fn rounds_every(&self) -> Duration {
        Duration::from_secs(60)
    }

    /// Be told of changes as they happen, if the source can tell: `tell`
    /// hears of each, and of whether it is listening, from a thread of the
    /// source's own until the Watch is dropped. Rounds go on meanwhile,
    /// in case a change is missed.
    fn watch(&self, _tell: Box<dyn Fn(Heard) + Send + Sync>) -> Option<Watch> {
        None
    }

    /// Whether it needs signing in (`sign_in`, docs/service.md).
    fn needs_sign_in(&self) -> bool {
        false
    }

    /// What an error from this source says about reaching it.
    fn health(&self, _error: &anyhow::Error) -> Health {
        Health::Failing
    }

    /// Get ready to answer, so the first request need not wait (iCloud
    /// checks its session, which takes a few seconds).
    fn start(&self) -> Result<()> {
        Ok(())
    }

    /// The catalog: every asset there is, put into `index` as they come.
    /// Assets the index had that were not put are removed afterwards.
    fn list(&self, index: &mut Index, progress: &(dyn Fn(u64, u64) + Sync)) -> Result<Listing>;

    /// What changed since `token`, put into and removed from `index`, and
    /// the token to pick up from next time. `TokenExpired` when the token
    /// is no use any more.
    fn changes(&self, index: &mut Index, token: &str) -> Result<Listing>;

    /// The fetch: the file itself, when it is on this computer and is
    /// served as it is, not copied into the cache.
    fn local(&self, _rendition: &Stored) -> Option<PathBuf> {
        None
    }

    /// Get these renditions ready to read, as many at once as the source
    /// can: one ticket each, in order. `index` is the library's, for what
    /// the source keeps there. An error for all of them is the outer one.
    fn prepare(&self, index: &rusqlite::Connection, wanted: &[Stored]) -> Result<Vec<Result<Ticket, FetchError>>>;

    /// Start reading one rendition: all of it, or bytes `start..=end`.
    fn open(&self, ticket: &Ticket, range: Option<(u64, u64)>) -> Result<Body, FetchError>;

    /// The extensions of the files `add` takes, in lower case; none when
    /// nothing can be added to this library.
    fn accepts(&self) -> &'static [&'static str] {
        &[]
    }

    /// Add one photo or video to the library (a Live Photo's two files
    /// together), from files on this computer, which stay as they are.
    /// `sent` hears how many of its bytes have gone, of how many. The
    /// library learns of it the usual way, at the next round. An
    /// `AddError` says why one could not be added; any other error is the
    /// source's, as `health` tells.
    fn add(&self, _addition: &crate::add::Addition, _sent: &(dyn Fn(u64, u64) + Sync)) -> Result<crate::add::Added> {
        Err(crate::add::AddError::new("unsupported", format!("{} cannot be added to", self.name())).into())
    }

    /// Make these assets favourites, or not, where the source keeps that:
    /// iCloud in its records, a folder in its library alone. `index` is
    /// the library's, for what the source keeps there; `assets.favorite`
    /// is the core's to set once this has worked. An error changes
    /// nothing the core keeps, and the next round shows what the source
    /// has.
    fn set_favorite(&self, _index: &rusqlite::Connection, _ids: &[String], _favorite: bool) -> Result<()> {
        Err(Unsupported(format!("{} keeps no favourites", self.name())).into())
    }
}

/// Nothing to show photos from yet, or any more: every source removed.
/// Its library is an empty one (`storage::library_for`), and a sync round
/// has nothing to do.
pub struct NoSource;

impl Source for NoSource {
    fn name(&self) -> String {
        String::new()
    }

    fn kind(&self) -> &'static str {
        "none"
    }

    fn id(&self) -> String {
        "none".into()
    }

    fn library_name(&self) -> String {
        String::new()
    }

    /// Only the settings to look at: a source added from a terminal shows
    /// within seconds.
    fn rounds_every(&self) -> Duration {
        Duration::from_secs(5)
    }

    fn list(&self, _index: &mut Index, _progress: &(dyn Fn(u64, u64) + Sync)) -> Result<Listing> {
        Ok(Listing::default())
    }

    fn changes(&self, _index: &mut Index, _token: &str) -> Result<Listing> {
        Ok(Listing::default())
    }

    fn prepare(&self, _index: &rusqlite::Connection, wanted: &[Stored]) -> Result<Vec<Result<Ticket, FetchError>>> {
        Ok(wanted.iter().map(|_| Err(FetchError::NotFound("there is no source".into()))).collect())
    }

    fn open(&self, _ticket: &Ticket, _range: Option<(u64, u64)>) -> Result<Body, FetchError> {
        Err(FetchError::NotFound("there is no source".into()))
    }
}
