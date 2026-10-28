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
    /// set on the photo (iCloud's, rendered on the iPhone); otherwise the
    /// service makes them from the motion (service.rs).
    Loop,
    Bounce,
    LongExposure,
}

impl Want {
    pub const ALL: [Want; 9] = [
        Want::Thumb, Want::Medium, Want::Full, Want::Original, Want::Video, Want::Live,
        Want::Loop, Want::Bounce, Want::LongExposure,
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

#[derive(Debug)]
pub enum FetchError {
    /// The ticket has gone stale (an expired link): prepare it again.
    Expired,
    /// The source has no such file.
    NotFound(String),
    /// Anything else; `Source::health` tells what it says about the
    /// connection.
    Failed(anyhow::Error),
}

impl std::fmt::Display for FetchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FetchError::Expired => write!(f, "the link expired"),
            FetchError::NotFound(what) => write!(f, "{what}"),
            FetchError::Failed(e) => write!(f, "{e:#}"),
        }
    }
}

impl From<anyhow::Error> for FetchError {
    fn from(e: anyhow::Error) -> Self {
        FetchError::Failed(e)
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

/// What listing the library or following its changes came to, beyond the
/// assets themselves.
#[derive(Debug, Default)]
pub struct Listing {
    /// Where `changes` picks up from next.
    pub token: Option<String>,
    /// A line about how it went, for pinhole-sync to print.
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

    /// "icloud" or "folder", as the settings name it.
    fn kind(&self) -> &'static str;

    /// Which library this is, written into the index it fills, so it never
    /// writes into another's: "icloud", or the folder's path.
    fn id(&self) -> String {
        self.kind().to_owned()
    }

    /// The folder its library is in, under `library/<kind>/`: unique among
    /// sources of its kind (an Apple ID, a folder's path).
    fn library_name(&self) -> String;

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

    /// Whether it needs signing in (pinhole-sync sign-in).
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
