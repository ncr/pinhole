//! A folder of photos on this computer (and the folders in it) as a source.
//! An asset's id is its path inside the folder. The originals are served as
//! they are, never copied; thumbnails and the 2048 px medium size are made
//! from them with ImageMagick (a video's from its most telling frame, with
//! ffmpeg: cover.rs). Changes are found by looking at the folder again: a
//! stat of every file, compared with what the index has. A folder has no
//! favourites of its own: they are kept in its library (`folder_favorites`).
//! See docs/sources.md. This is the Rust of the `pinhole.folder` plugin,
//! linked into pinholed: `FolderKind` is its kind of source.

pub mod copy;
pub mod discover;

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{BufRead, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::{Duration, Instant, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use rayon::prelude::*;
use rusqlite::Connection;

use pinhole_core::add::{Added, Addition};
use pinhole_core::db::Index;
use pinhole_core::render::{MEDIUM_EDGE, THUMB_EDGE};
use pinhole_core::source::{Asset, Body, FetchError, Health, Kind, Listing, Location, Rendition, Source, Stored, Ticket, Want};

/// Assets put into the index at a time while listing: few enough that
/// describing them (EXIF, ffprobe) takes well under a second, so a listing
/// stops soon when another source is shown.
const PUT_BATCH: usize = 128;
/// Whatever a look at the folder takes: photos and videos.
const ACCEPTS: &[&str] = &["jpg", "jpeg", "heic", "heif", "png", "gif", "tif", "tiff", "webp", "avif",
                           "mov", "mp4", "m4v", "mkv", "webm", "avi"];

pub struct FolderSource {
    root: PathBuf,
    /// For a test: files there that are taken for ones that could not be
    /// read.
    #[cfg(test)]
    unread_too: Vec<String>,
}

/// What a file is, by its extension.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Media {
    Photo,
    Video,
}

/// A file of the folder as a look at it finds it.
#[derive(Clone, Debug)]
struct Found {
    /// Inside the folder, with '/' between parts.
    rel: String,
    media: Media,
    size: u64,
    modified_ns: i128,
}

impl Found {
    /// Names its thumbnail and cached files; changes when the file does,
    /// and a video's when how its cover is made does (cover::VERSION), so
    /// that it is made again. Its folder's (`root`) too: the cache is
    /// shared, and another folder's file of this name, size and time is
    /// another file.
    fn key(&self, root: &Path) -> String {
        use std::os::unix::ffi::OsStrExt;
        let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
        let cover = if self.media == Media::Video { pinhole_core::cover::VERSION } else { "" };
        let root = root.as_os_str().as_bytes().iter().copied().chain([0]);
        for b in root.chain(self.rel.bytes()).chain(self.size.to_le_bytes()).chain(self.modified_ns.to_le_bytes()).chain(cover.bytes()) {
            hash = (hash ^ b as u64).wrapping_mul(0x0100_0000_01b3);
        }
        format!("f{hash:016x}")
    }
}

/// An asset as found: the file, and a Live Photo's motion beside it.
#[derive(Clone, Debug)]
struct Entry {
    file: Found,
    motion: Option<Found>,
}

impl Entry {
    /// What the index keeps to tell whether it changed since.
    fn signature(&self, root: &Path) -> (String, Option<String>) {
        (self.file.key(root), self.motion.as_ref().map(|m| m.key(root)))
    }
}

/// A photo and a video named as it that is not its motion (`is_motion`),
/// by both their keys (`Found::key`, `unpaired_key`), kept in the library
/// so they are not read again: either file changed, they are.
const UNPAIRED: &str = "folder_unpaired";

fn unpaired_key(root: &Path, photo: &Found, video: &Found) -> String {
    format!("{} {}", photo.key(root), video.key(root))
}

fn unpaired(index: &Connection) -> Result<HashSet<String>> {
    index.execute_batch(&format!("CREATE TABLE IF NOT EXISTS {UNPAIRED} (key TEXT PRIMARY KEY) WITHOUT ROWID"))?;
    let mut stmt = index.prepare(&format!("SELECT key FROM {UNPAIRED}"))?;
    let keys = stmt.query_map([], |r| r.get(0))?.collect::<rusqlite::Result<_>>()?;
    Ok(keys)
}

/// The assets made favourites here (`set_favorite`), none before the first.
fn favorites(index: &Connection) -> HashSet<String> {
    let Ok(mut stmt) = index.prepare("SELECT asset_id FROM folder_favorites") else { return HashSet::new() };
    stmt.query_map([], |r| r.get(0)).map(|rows| rows.flatten().collect()).unwrap_or_default()
}

fn media_of(path: &Path) -> Option<Media> {
    let ext = path.extension()?.to_str()?.to_ascii_lowercase();
    if pinhole_core::add::PHOTOS.contains(&ext.as_str()) {
        Some(Media::Photo)
    } else if pinhole_core::add::VIDEOS.contains(&ext.as_str()) {
        Some(Media::Video)
    } else {
        None
    }
}

/// The UTI the service has always answered with, by extension.
fn file_type(rel: &str) -> &'static str {
    let ext = rel.rsplit('.').next().unwrap_or("").to_ascii_lowercase();
    match ext.as_str() {
        "jpg" | "jpeg" => "public.jpeg",
        "heic" | "heif" => "public.heic",
        "png" => "public.png",
        "gif" => "com.compuserve.gif",
        "tif" | "tiff" => "public.tiff",
        "webp" => "org.webmproject.webp",
        "avif" => "public.avif",
        "mov" => "com.apple.quicktime-movie",
        "mp4" => "public.mpeg-4",
        "m4v" => "com.apple.m4v-video",
        "mkv" => "org.matroska.mkv",
        "webm" => "org.webmproject.webm",
        "avi" => "public.avi",
        _ => "public.data",
    }
}

/// What a walk through a folder found.
struct Walk {
    found: Vec<Found>,
    /// Inside the folder, what could not be read (a folder without
    /// permission, a failing disk): "" when the folder itself could not.
    /// What the index has there is unseen, not gone.
    unread: Vec<String>,
    /// It got through all of them before the deadline.
    complete: bool,
}

/// The photos and videos in a folder and the folders in it, hidden ones
/// skipped, and whatever could not be read. Only regular files: a link
/// (which may lead out of the folder) or a named pipe called .jpg is not a
/// photo of it, and links to folders are not followed.
fn walk(root: &Path, deadline: Option<Instant>) -> Walk {
    let rel_of = |path: &Path| path.strip_prefix(root).map_or_else(|_| String::new(), |r| r.to_string_lossy().into_owned());
    let mut walk = Walk { found: Vec::new(), unread: Vec::new(), complete: false };
    let mut dirs = vec![root.to_path_buf()];
    while let Some(dir) = dirs.pop() {
        if deadline.is_some_and(|d| Instant::now() >= d) {
            return walk;
        }
        let Ok(entries) = std::fs::read_dir(&dir) else {
            walk.unread.push(rel_of(&dir));
            continue;
        };
        for entry in entries {
            // A listing that failed partway: whatever else is in it is unseen.
            let Ok(entry) = entry else {
                walk.unread.push(rel_of(&dir));
                break;
            };
            let path = entry.path();
            if entry.file_name().to_string_lossy().starts_with('.') {
                continue;
            }
            let Ok(meta) = entry.metadata() else {
                walk.unread.push(rel_of(&path));
                continue;
            };
            if meta.is_dir() {
                dirs.push(path);
                continue;
            }
            if !meta.is_file() {
                continue;
            }
            let Some(media) = media_of(&path) else { continue };
            let Ok(rel) = path.strip_prefix(root) else { continue };
            let modified_ns = meta
                .modified()
                .ok()
                .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                .map_or(0, |d| d.as_nanos() as i128);
            walk.found.push(Found { rel: rel.to_string_lossy().into_owned(), media, size: meta.len(), modified_ns });
        }
    }
    walk.complete = true;
    walk
}

/// Whether an asset, by its path inside the folder, is in what a walk could
/// not read (`Walk::unread`), so its file may well still be there.
fn unread_holds(unread: &[String], id: &str) -> bool {
    unread.iter().any(|u| u.is_empty() || id == u || id.strip_prefix(u.as_str()).is_some_and(|rest| rest.starts_with('/')))
}

/// A file's folder and name without its extension, lower case: a photo and
/// its motion share it (the motion maybe with `_hevc` after).
fn stem(rel: &str) -> String {
    rel.rsplit_once('.').map_or(rel, |(s, _)| s).to_ascii_lowercase()
}

/// The assets among the files found: a video named as a photo beside it
/// (IMG_1234.HEIC with IMG_1234.MOV, or IMG_1234_HEVC.MOV as icloudpd names
/// it) is taken for that photo's motion, not an asset of its own, until
/// describing them finds it is not (`is_motion`): `unpaired` has such
/// pairs (`unpaired_key`). A photo has one motion at most, a QuickTime one
/// first; any other video of its name is a video of its own.
fn pair(root: &Path, mut found: Vec<Found>, unpaired: &HashSet<String>) -> HashMap<String, Entry> {
    let later = |f: &Found| f.media == Media::Video && !f.rel.rsplit('.').next().is_some_and(|e| e.eq_ignore_ascii_case("mov"));
    found.sort_by(|a, b| (later(a), &a.rel).cmp(&(later(b), &b.rel)));
    let photos: HashSet<String> = found.iter().filter(|f| f.media == Media::Photo).map(|f| stem(&f.rel)).collect();
    let mut motion: HashMap<String, Vec<Found>> = HashMap::new();
    let mut entries: HashMap<String, Entry> = HashMap::new();
    for f in found {
        if f.media == Media::Video {
            let s = stem(&f.rel);
            let photo = s.strip_suffix("_hevc").filter(|p| photos.contains(*p)).map(str::to_owned)
                .or_else(|| photos.contains(&s).then_some(s));
            if let Some(photo) = photo {
                motion.entry(photo).or_default().push(f);
                continue;
            }
        }
        entries.insert(f.rel.clone(), Entry { file: f, motion: None });
    }
    let mut photos: Vec<&mut Entry> = entries.values_mut().filter(|e| e.file.media == Media::Photo).collect();
    photos.sort_by(|a, b| a.file.rel.cmp(&b.file.rel));
    for entry in photos {
        let Some(videos) = motion.get_mut(&stem(&entry.file.rel)) else { continue };
        if let Some(i) = videos.iter().position(|v| !unpaired.contains(&unpaired_key(root, &entry.file, v))) {
            entry.motion = Some(videos.remove(i));
        }
    }
    for f in motion.into_values().flatten() {
        entries.insert(f.rel.clone(), Entry { file: f, motion: None });
    }
    entries
}

/// The longest a Live Photo's motion is, in seconds: about three, and a
/// little more (`is_motion`).
const LIVE_SECONDS: f64 = 6.0;

/// Whether a video named as a photo is its motion: when both carry
/// Apple's content identifier, the same one, as a Live Photo's two files
/// do; without it (a Live Photo out of Google Takeout), a video as short
/// as a motion, or one whose length could not be read.
fn is_motion(photo: &Details, video: &Details) -> bool {
    match (&photo.content_id, &video.content_id) {
        (Some(a), Some(b)) => a == b,
        _ => video.duration.is_none_or(|d| d <= LIVE_SECONDS),
    }
}

/// What a folder would bring, counted from its files' names and sizes
/// alone: photos and videos (a Live Photo's two files once), Live Photos,
/// bytes, and whether the count stopped at `deadline` before the end.
#[derive(Debug, Default, PartialEq)]
pub struct Count {
    pub assets: u64,
    pub live: u64,
    pub bytes: u64,
    pub partial: bool,
}

pub fn count(root: &Path, deadline: Option<Instant>) -> Count {
    let Walk { found, complete, .. } = walk(root, deadline);
    let bytes = found.iter().map(|f| f.size).sum();
    let entries = pair(root, found, &HashSet::new());
    Count {
        assets: entries.len() as u64,
        live: entries.values().filter(|e| e.motion.is_some()).count() as u64,
        bytes,
        partial: !complete,
    }
}

/// What to call a folder: its name with the folder it is in
/// (`Pictures/iCloud`, `T7/DCIM`), alone when that is the home folder or
/// the root.
pub fn display_name(path: &Path, home: &Path) -> String {
    let Some(name) = path.file_name().map(|n| n.to_string_lossy().into_owned()) else {
        return path.display().to_string();
    };
    match path.parent() {
        Some(parent) if parent != home && parent.parent().is_some() => match parent.file_name() {
            Some(p) => format!("{}/{name}", p.to_string_lossy()),
            None => name,
        },
        _ => name,
    }
}

/// Where the index keeps (`db::set_meta`) the device number (st_dev) of the
/// disk the folder was on when last looked at (`FolderSource::same_disk`).
const DEVICE: &str = "folder_device";

/// The folder found empty on another disk than its photos were on: its own
/// disk unmounted from a mount point that stays, empty, on the disk below.
#[derive(Debug)]
struct NotMounted(String);

impl std::fmt::Display for NotMounted {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "{} is empty and on another disk than its photos were: their disk is not mounted", self.0)
    }
}

impl std::error::Error for NotMounted {}

/// A look at the folder: its assets by id, and what could not be read
/// (`Walk::unread`), where the index's assets are kept as they are.
struct Look {
    entries: HashMap<String, Entry>,
    unread: Vec<String>,
}

impl Look {
    /// A Live Photo known with its motion, now seen without it because the
    /// motion's file could not be read: left as the index has it, not made
    /// a still photo.
    fn motion_unread(&self, entry: &Entry) -> bool {
        entry.file.media == Media::Photo
            && entry.motion.is_none()
            && self.unread.iter().any(|u| {
                let s = stem(u);
                media_of(Path::new(u)) == Some(Media::Video) && (s == stem(&entry.file.rel) || s == stem(&entry.file.rel) + "_hevc")
            })
    }

    /// For the listing's summary: nothing when all was read.
    fn unread_note(&self) -> String {
        match self.unread.len() {
            0 => String::new(),
            n => format!("; {n} files or folders could not be read, what the library has of them stays"),
        }
    }
}

impl FolderSource {
    pub fn new(root: PathBuf) -> Self {
        FolderSource {
            root,
            #[cfg(test)]
            unread_too: Vec::new(),
        }
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    /// Every photo and video in the folder, with Live Photos' motion paired
    /// to its photo (`pair`, but for the pairs in `unpaired`), and what
    /// could not be read. Hidden files and folders are skipped.
    fn look(&self, unpaired: &HashSet<String>) -> Result<Look> {
        if !self.root.is_dir() {
            // Never "everything was deleted": a disk not mounted, say.
            return Err(anyhow!("{} is not there", self.root.display()));
        }
        #[allow(unused_mut)]
        let mut walk = walk(&self.root, None);
        #[cfg(test)]
        {
            walk.found.retain(|f| !self.unread_too.contains(&f.rel));
            walk.unread.extend(self.unread_too.iter().cloned());
        }
        let mut unread = walk.unread;
        // The folder itself unread (no permission to it): failing, and
        // nothing of it seen. An error writes nothing, so its photos stay.
        if unread.iter().any(String::is_empty) {
            return Err(anyhow!("{} cannot be read", self.root.display()));
        }
        unread.sort();
        unread.dedup();
        Ok(Look { entries: pair(&self.root, walk.found, unpaired), unread })
    }

    /// A look that finds nothing where the index has photos, with the folder
    /// on another disk than when it was last looked at, is its disk
    /// unmounted (a removable disk or a network share at a fixed mount
    /// point, /mnt/photos), not every photo deleted: an error, which
    /// writes nothing, so the photos and their favourites stay. With photos
    /// in it, the folder is as it is found (a share mounted again has
    /// another number), and its disk is the one remembered from then on.
    fn same_disk(&self, conn: &Connection, look: &Look) -> Result<()> {
        use std::os::unix::fs::MetadataExt;
        let device = std::fs::metadata(&self.root)?.dev().to_string();
        let was = pinhole_core::db::meta(conn, DEVICE);
        if was.as_deref() == Some(device.as_str()) {
            return Ok(());
        }
        if look.entries.is_empty() && was.is_some() && conn.query_row("SELECT EXISTS (SELECT 1 FROM assets)", [], |r| r.get(0))? {
            return Err(NotMounted(self.root.display().to_string()).into());
        }
        pinhole_core::db::set_meta(conn, DEVICE, &device)
    }

    /// The asset for an entry: its dates and details read from the file.
    /// A photo and a video taken for its motion that turn out not to be one
    /// (`is_motion`) are two assets. An error only when the folder does not
    /// answer (`probe_video`).
    fn describe(&self, entry: &Entry) -> Result<Vec<Asset>> {
        let f = &entry.file;
        let path = self.root.join(&f.rel);
        if f.media == Media::Video {
            return Ok(vec![self.asset(f, None, probe_video(&path)?)]);
        }
        let exif = read_exif(&path);
        let Some(m) = &entry.motion else { return Ok(vec![self.asset(f, None, exif)]) };
        let probe = probe_video(&self.root.join(&m.rel))?;
        Ok(if is_motion(&exif, &probe) {
            vec![self.asset(f, Some(m), exif)]
        } else {
            vec![self.asset(f, None, exif), self.asset(m, None, probe)]
        })
    }

    /// The asset of a file, with its motion's.
    fn asset(&self, f: &Found, motion: Option<&Found>, details: Details) -> Asset {
        let Details { taken, duration, orientation, location, .. } = details;
        let modified_ms = (f.modified_ns / 1_000_000) as i64;
        let key = f.key(&self.root);
        let file_type = file_type(&f.rel);
        let made = |want: Want| Rendition {
            want,
            name: f.rel.clone(),
            key: key.clone(),
            width: None,
            height: None,
            size: None,
            file_type: Some("public.jpeg".into()),
        };
        let itself = |want: Want| Rendition {
            want,
            name: f.rel.clone(),
            key: key.clone(),
            width: None,
            height: None,
            size: Some(f.size as i64),
            file_type: Some(file_type.into()),
        };
        let mut renditions = vec![made(Want::Thumb), made(Want::Medium), itself(Want::Full), itself(Want::Original)];
        if f.media == Media::Video {
            renditions.push(itself(Want::Video));
        }
        if let Some(m) = motion {
            renditions.push(Rendition {
                want: Want::Live,
                name: m.rel.clone(),
                key: m.key(&self.root),
                width: None,
                height: None,
                size: Some(m.size as i64),
                file_type: Some(self::file_type(&m.rel).into()),
            });
        }
        Asset {
            id: f.rel.clone(),
            filename: Path::new(&f.rel).file_name().map(|n| n.to_string_lossy().into_owned()),
            item_type: Some(file_type.into()),
            taken: taken.or(Some(modified_ms)),
            added: Some(modified_ms),
            kind: match (f.media, motion) {
                (Media::Video, _) => Kind::Video,
                (Media::Photo, Some(_)) => Kind::Live,
                (Media::Photo, None) => Kind::Photo,
            },
            favorite: false,
            duration,
            orientation,
            has_location: location.is_some(),
            location,
            size: Some(f.size as i64),
            effect: None,
            renditions,
            edit: None,
            subtype: None,
        }
    }

    /// Describe entries (reading files takes a while: in parallel) and put
    /// them, a batch at a time, the favourites as the library keeps them,
    /// and which videos are not the motion they were taken for (`unpaired`).
    fn put(&self, index: &mut Index, entries: &[&Entry], progress: &(dyn Fn(u64, u64) + Sync), total: u64) -> Result<()> {
        let favorites = favorites(index.conn);
        for batch in entries.chunks(PUT_BATCH) {
            if index.stopping() {
                return Err(pinhole_core::source::Stopped.into());
            }
            let described: Vec<Vec<Asset>> = batch.par_iter().map(|e| self.describe(e)).collect::<Result<_>>()?;
            for (entry, assets) in batch.iter().zip(&described) {
                if let (Some(m), [_, _]) = (&entry.motion, assets.as_slice()) {
                    index.conn.execute(&format!("INSERT OR IGNORE INTO {UNPAIRED} (key) VALUES (?1)"),
                                       [unpaired_key(&self.root, &entry.file, m)])?;
                }
            }
            let mut assets: Vec<Asset> = described.into_iter().flatten().collect();
            for a in &mut assets {
                a.favorite = favorites.contains(&a.id);
            }
            index.put(&assets)?;
            progress(index.seen.len() as u64, total);
        }
        Ok(())
    }

    /// A JPEG of a photo or of a video's frame, at most `edge` on the long
    /// side, the right way up (pinhole-core's render.rs).
    fn render(&self, rel: &str, edge: u32) -> Result<Vec<u8>> {
        let path = self.file(rel)?;
        pinhole_core::render::render_path(&path, edge, media_of(&path) == Some(Media::Video))
            .with_context(|| format!("could not make an image of {rel}"))
    }

    /// An asset's file, by its path inside the folder, as a walk would find
    /// it: a regular file in the folder itself, not one that a link in the
    /// way leads to (out of the folder, maybe), nor a named pipe. Else
    /// NotFound: it is not one of this folder's.
    fn file(&self, rel: &str) -> std::io::Result<PathBuf> {
        // Joined to the root, an absolute name would replace it, and `..`
        // climb out of it: neither is a walk's.
        if rel.is_empty() || !Path::new(rel).components().all(|c| matches!(c, std::path::Component::Normal(_))) {
            return Err(std::io::Error::new(std::io::ErrorKind::NotFound, format!("{rel} is not a path inside {}", self.root.display())));
        }
        let real = self.root.join(rel).canonicalize()?;
        if real != self.root.canonicalize()?.join(rel) || !std::fs::symlink_metadata(&real)?.is_file() {
            return Err(std::io::Error::new(std::io::ErrorKind::NotFound, format!("{rel} is not a file of {}", self.root.display())));
        }
        Ok(real)
    }
}

/// Folders as a kind of source (pinhole-core's kinds.rs), `pinhole.folder`'s: an entry is
/// a folder's absolute path (`{"kind": "folder", "path": …}`).
pub struct FolderKind;

impl pinhole_core::kinds::Kind for FolderKind {
    fn kind(&self) -> &str {
        "folder"
    }

    fn plugin(&self) -> &str {
        "pinhole.folder"
    }

    fn normal(&self, entry: &serde_json::Value) -> Option<serde_json::Value> {
        Some(serde_json::json!({"kind": "folder", "path": entry["path"].as_str()?}))
    }

    fn id_of(&self, entry: &serde_json::Value) -> String {
        format!("folder:{}", entry["path"].as_str().unwrap_or(""))
    }

    fn source(&self, entry: &serde_json::Value) -> Result<std::sync::Arc<dyn Source>> {
        let path = entry["path"].as_str().ok_or_else(|| anyhow!("a folder source needs a path"))?;
        if !Path::new(path).is_absolute() {
            return Err(anyhow!("not an absolute path: {path}"));
        }
        Ok(std::sync::Arc::new(FolderSource::new(PathBuf::from(path))))
    }

    /// Its path, and its name as `display_name` gives it.
    fn describe(&self, entry: &serde_json::Value) -> serde_json::Value {
        let path = entry["path"].as_str().unwrap_or("");
        let home = PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
        serde_json::json!({"path": path, "name": display_name(Path::new(path), &home)})
    }

    /// `path`: an absolute folder, without a slash at the end.
    fn entry_for(&self, params: &serde_json::Value) -> Result<serde_json::Value, pinhole_core::kinds::Failure> {
        let bad = |m: &str| pinhole_core::kinds::Failure::bad_request(m);
        let path = params["path"].as_str().ok_or_else(|| bad("a folder needs its path"))?;
        let path = path.trim_end_matches('/');
        if !Path::new(path).is_absolute() || !Path::new(path).is_dir() {
            return Err(bad(&format!("{path} is not a folder")));
        }
        Ok(serde_json::json!({"kind": "folder", "path": if path.is_empty() { "/" } else { path }}))
    }

    /// Folders of photos not added yet (discover.rs): every DCIM folder on
    /// a disk under /run/media/$USER, the folders in Pictures, then
    /// Pictures itself.
    fn discover(&self, added: &[serde_json::Value]) -> serde_json::Value {
        let home = PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
        let config = std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from).unwrap_or_else(|| home.join(".config"));
        let user = std::env::var("USER")
            .unwrap_or_else(|_| home.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default());
        let added: HashSet<PathBuf> =
            added.iter().filter(|e| e["kind"] == "folder").filter_map(|e| e["path"].as_str()).map(PathBuf::from).collect();
        let pictures = crate::discover::pictures_dir(&home, &config);
        let candidates = crate::discover::candidates(&pictures, &Path::new("/run/media").join(user));
        serde_json::json!({"folders": crate::discover::folders(candidates, &added, &home, crate::discover::BUDGET)})
    }

    fn accepts(&self) -> &'static [&'static str] {
        ACCEPTS
    }
}

impl Source for FolderSource {
    fn name(&self) -> String {
        display_name(&self.root, Path::new(&std::env::var_os("HOME").unwrap_or_default()))
    }

    fn kind(&self) -> &'static str {
        "folder"
    }

    fn id(&self) -> String {
        format!("folder:{}", self.root.display())
    }

    fn details(&self) -> serde_json::Value {
        serde_json::json!({"path": self.root.display().to_string()})
    }

    /// Its path, readable: /home/me/Pictures is home-me-Pictures. A path
    /// with a dash or a tilde of its own is told apart by a hash of it after
    /// a tilde (/a-b/c and /a/b-c are not both a-b-c); without one, each
    /// dash is a slash, there is no tilde, and no two paths share a name.
    fn library_name(&self) -> String {
        let path = self.root.to_string_lossy();
        let name = path.trim_matches('/').replace('/', "-");
        if name.is_empty() {
            "-".into()
        } else if path.contains(['-', '~']) {
            format!("{name}~{:016x}", pinhole_core::storage::stable_hash(path.as_bytes()))
        } else {
            name
        }
    }

    fn health(&self, error: &anyhow::Error) -> Health {
        if !self.root.is_dir() || error.is::<NotMounted>() {
            return Health::Offline;
        }
        Health::Failing
    }

    fn rounds_every(&self) -> Duration {
        Duration::from_secs(10)
    }

    fn list(&self, index: &mut Index, progress: &(dyn Fn(u64, u64) + Sync)) -> Result<Listing> {
        let look = self.look(&unpaired(index.conn)?)?;
        self.same_disk(index.conn, &look)?;
        let entries = &look.entries;
        // What could not be read is not gone: the listing leaves it be, and
        // a Live Photo whose motion it is as the index has it.
        let mut held = HashSet::new();
        if !look.unread.is_empty() {
            let mut stmt = index.conn.prepare("SELECT id FROM assets")?;
            let ids = stmt.query_map([], |r| r.get::<_, String>(0))?.collect::<rusqlite::Result<HashSet<String>>>()?;
            drop(stmt);
            held = entries
                .values()
                .filter(|e| ids.contains(&e.file.rel) && look.motion_unread(e))
                .map(|e| e.file.rel.clone())
                .collect();
            index.seen.extend(ids.into_iter().filter(|id| unread_holds(&look.unread, id) || held.contains(id)));
        }
        let mut all: Vec<&Entry> = entries.values().filter(|e| !held.contains(&e.file.rel)).collect();
        all.sort_by(|a, b| a.file.rel.cmp(&b.file.rel));
        self.put(index, &all, progress, all.len() as u64)?;
        let lives = entries.values().filter(|e| e.motion.is_some()).count();
        Ok(Listing {
            token: Some(pinhole_core::sync::now_secs().to_string()),
            summary: format!("{} photos and videos in {}, {lives} of them Live Photos{}", entries.len(),
                             self.root.display(), look.unread_note()),
        })
    }

    fn changes(&self, index: &mut Index, _token: &str) -> Result<Listing> {
        let look = self.look(&unpaired(index.conn)?)?;
        self.same_disk(index.conn, &look)?;
        let entries = &look.entries;
        // What the index has, by the keys of each asset's file and motion.
        let mut known: HashMap<String, (String, Option<String>)> = HashMap::new();
        {
            let mut stmt = index.conn.prepare(
                "SELECT a.id, o.key, l.key FROM assets a
                 LEFT JOIN renditions o ON o.asset_id = a.id AND o.kind = 'original'
                 LEFT JOIN renditions l ON l.asset_id = a.id AND l.kind = 'live'",
            )?;
            let rows = stmt.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?, r.get(2)?)))?;
            for row in rows {
                let (id, key, live) = row?;
                known.insert(id, (key.unwrap_or_default(), live));
            }
        }
        let changed: Vec<&Entry> = entries
            .values()
            .filter(|e| known.get(&e.file.rel) != Some(&e.signature(&self.root)))
            .filter(|e| !(known.contains_key(&e.file.rel) && look.motion_unread(e)))
            .collect();
        let gone: Vec<String> = known
            .keys()
            .filter(|id| !entries.contains_key(*id) && !unread_holds(&look.unread, id))
            .cloned()
            .collect();
        self.put(index, &changed, &|_, _| {}, changed.len() as u64)?;
        // A video taken for a new photo's motion that is not one is put
        // back as it was (`describe`): it stays, and its favourite with it.
        let gone: Vec<String> = gone.into_iter().filter(|id| !index.seen.contains(id)).collect();
        index.remove(&gone)?;
        Ok(Listing {
            token: Some(pinhole_core::sync::now_secs().to_string()),
            summary: format!("{} files looked at{}", entries.len(), look.unread_note()),
        })
    }

    fn local(&self, rendition: &Stored) -> Option<PathBuf> {
        matches!(rendition.want, Want::Full | Want::Original | Want::Video | Want::Live)
            .then(|| self.file(&rendition.name).ok())
            .flatten()
    }

    fn prepare(&self, _index: &Connection, wanted: &[Stored]) -> Result<Vec<Result<Ticket, FetchError>>> {
        Ok(wanted
            .iter()
            .map(|w| match self.file(&w.name) {
                Ok(_) => Ok(Ticket(format!("{}\t{}", w.want.name(), w.name))),
                // Not that it is not there: the folder's (a folder without
                // permission, a failing disk), which its health says.
                Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
                    Err(FetchError::Failed(anyhow!("{}: {e}", self.root.join(&w.name).display())))
                }
                _ => Err(FetchError::NotFound(format!("{} is not in {}", w.name, self.root.display()))),
            })
            .collect())
    }

    fn accepts(&self) -> &'static [&'static str] {
        ACCEPTS
    }

    /// Copied into the folder itself, beside what is there, never over it
    /// (`copy::copy_into`); the next look at the folder finds it.
    fn add(&self, addition: &Addition, sent: &(dyn Fn(u64, u64) + Sync)) -> Result<Added> {
        if !self.root.is_dir() {
            return Err(anyhow!("{} is not there", self.root.display()));
        }
        let names = copy::copy_into(&self.root, addition, sent)?;
        Ok(Added { id: names.into_iter().next(), ..Added::default() })
    }

    /// Kept in the library alone: the files stay as they are.
    fn set_favorite(&self, index: &Connection, ids: &[String], favorite: bool) -> Result<()> {
        index.execute_batch(
            "CREATE TABLE IF NOT EXISTS folder_favorites (asset_id TEXT PRIMARY KEY) WITHOUT ROWID;
             CREATE TRIGGER IF NOT EXISTS folder_forget AFTER DELETE ON assets BEGIN
                 DELETE FROM folder_favorites WHERE asset_id = old.id;
             END;",
        )?;
        let tx = index.unchecked_transaction()?;
        for id in ids {
            if favorite {
                tx.execute("INSERT OR IGNORE INTO folder_favorites (asset_id) VALUES (?1)", [id])?;
            } else {
                tx.execute("DELETE FROM folder_favorites WHERE asset_id = ?1", [id])?;
            }
        }
        tx.commit()?;
        Ok(())
    }

    fn open(&self, ticket: &Ticket, range: Option<(u64, u64)>) -> Result<Body, FetchError> {
        let (want, rel) = ticket.0.split_once('\t').ok_or_else(|| anyhow!("not a ticket of this folder"))?;
        let edge = match Want::parse(want) {
            Some(Want::Thumb) => Some(THUMB_EDGE),
            Some(Want::Medium) => Some(MEDIUM_EDGE),
            _ => None,
        };
        if let Some(edge) = edge {
            // Turned down by the program that makes it, the file's alone:
            // while the folder and the file are still there, as a disk
            // gone from under a program makes it fail too.
            let bytes = self.render(rel, edge).map_err(|e| match FetchError::from(e) {
                FetchError::Unreadable(why) if !(self.root.is_dir() && self.file(rel).is_ok()) => {
                    FetchError::Failed(anyhow!("{why}"))
                }
                e => e,
            })?;
            return Ok(Body { length: Some(bytes.len() as u64), reader: Box::new(std::io::Cursor::new(bytes)) });
        }
        let path = self.root.join(rel);
        let mut file = self.file(rel).and_then(|real| open_regular(&real)).map_err(|e| match e.kind() {
            std::io::ErrorKind::NotFound => FetchError::NotFound(format!("{} is gone", path.display())),
            _ => FetchError::Failed(e.into()),
        })?;
        let length = file.metadata().map_err(|e| FetchError::Failed(e.into()))?.len();
        match range {
            Some((start, end)) => {
                file.seek(SeekFrom::Start(start)).map_err(|e| FetchError::Failed(e.into()))?;
                let n = end.min(length.saturating_sub(1)).saturating_sub(start) + 1;
                Ok(Body { length: Some(n), reader: Box::new(file.take(n)) })
            }
            None => Ok(Body { length: Some(length), reader: Box::new(file) }),
        }
    }
}

/// Opened for reading, and a regular file: never waiting on a named pipe
/// with no one writing to it, nor following a link at its end.
fn open_regular(path: &Path) -> std::io::Result<File> {
    use std::os::unix::fs::OpenOptionsExt;
    let file = std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NONBLOCK | libc::O_NOFOLLOW).open(path)?;
    if !file.metadata()?.is_file() {
        return Err(std::io::Error::new(std::io::ErrorKind::NotFound, format!("{} is not a file", path.display())));
    }
    Ok(file)
}

/// What of a TIFF its tags are looked for in (`read_exif`).
const EXIF_BUDGET: u64 = 8 << 20;

/// What is read of a photo's or a video's file.
#[derive(Default)]
struct Details {
    taken: Option<i64>,
    duration: Option<f64>,
    orientation: Option<i64>,
    location: Option<Location>,
    /// Apple's, the same in a Live Photo's still and its motion.
    content_id: Option<String>,
}

/// When a photo was taken (the camera's local time, with its offset when
/// it wrote one, else this computer's), which way up it is, and whether it
/// has a place, and Apple's content identifier.
fn read_exif(path: &Path) -> Details {
    let Ok(file) = open_regular(path) else { return Details::default() };
    let mut reader = std::io::BufReader::new(file);
    // A TIFF is read whole by kamadak-exif, image and all, for its few
    // tags: of a large one, only as much as `EXIF_BUDGET`, where they
    // nearly always are (at the start). Several are read at once.
    let tiff = reader.fill_buf().is_ok_and(|b| b.starts_with(b"II*\0") || b.starts_with(b"MM\0*"));
    let read = if tiff {
        let mut head = Vec::new();
        match reader.take(EXIF_BUDGET).read_to_end(&mut head) {
            Ok(_) => exif::Reader::new().read_raw(head),
            Err(e) => Err(e.into()),
        }
    } else {
        exif::Reader::new().read_from_container(&mut reader)
    };
    let Ok(exif) = read else { return Details::default() };
    let text = |tag| {
        exif.get_field(tag, exif::In::PRIMARY).map(|f| f.display_value().to_string().trim_matches('"').to_owned())
    };
    let taken = text(exif::Tag::DateTimeOriginal)
        .or_else(|| text(exif::Tag::DateTimeDigitized))
        .or_else(|| text(exif::Tag::DateTime))
        .and_then(|t| exif_time(&t, text(exif::Tag::OffsetTimeOriginal).as_deref()));
    let orientation = exif.get_field(exif::Tag::Orientation, exif::In::PRIMARY).and_then(|f| f.value.get_uint(0)).map(i64::from);
    let axis = |tag, direction, positive, negative| {
        let exif::Value::Rational(dms) = &exif.get_field(tag, exif::In::PRIMARY)?.value else {
            return None;
        };
        let [degrees, minutes, seconds] = dms.as_slice() else { return None };
        let sign = match text(direction)?.as_str() {
            value if value == positive => 1.0,
            value if value == negative => -1.0,
            _ => return None,
        };
        Some(sign * (degrees.to_f64() + minutes.to_f64() / 60.0 + seconds.to_f64() / 3600.0))
    };
    let location = axis(exif::Tag::GPSLatitude, exif::Tag::GPSLatitudeRef, "N", "S")
        .zip(axis(exif::Tag::GPSLongitude, exif::Tag::GPSLongitudeRef, "E", "W"))
        .and_then(|(lat, lon)| Location::new(lat, lon));
    let content_id = exif.get_field(exif::Tag::MakerNote, exif::In::PRIMARY).and_then(|f| match &f.value {
        exif::Value::Undefined(note, _) => apple_content_id(note),
        _ => None,
    });
    Details { taken, orientation, location, content_id, ..Details::default() }
}

/// The ContentIdentifier (tag 0x11) in Apple's maker note: "Apple iOS\0",
/// a version, "MM", then a big-endian IFD whose offsets count from the
/// note's start.
fn apple_content_id(note: &[u8]) -> Option<String> {
    if !note.starts_with(b"Apple iOS\0") || note.get(12..14)? != b"MM" {
        return None;
    }
    let u16_at = |i: usize| note.get(i..i + 2).map(|b| u16::from_be_bytes([b[0], b[1]]));
    let u32_at = |i: usize| note.get(i..i + 4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]));
    (0..u16_at(14)? as usize).map(|n| 16 + 12 * n).find_map(|e| {
        if u16_at(e)? != 0x11 || u16_at(e + 2)? != 2 {
            return None;
        }
        let len = u32_at(e + 4)? as usize;
        let at = if len <= 4 { e + 8 } else { u32_at(e + 8)? as usize };
        let text = std::str::from_utf8(note.get(at..at.checked_add(len)?)?).ok()?.trim_end_matches('\0');
        (!text.is_empty()).then(|| text.to_owned())
    })
}

/// "2024-05-01 12:34:56" (as kamadak-exif shows EXIF's 2024:05:01
/// 12:34:56) in milliseconds since the epoch.
fn exif_time(text: &str, offset: Option<&str>) -> Option<i64> {
    use chrono::{DateTime, Local, NaiveDateTime, TimeZone};
    let naive = NaiveDateTime::parse_from_str(text, "%Y-%m-%d %H:%M:%S")
        .or_else(|_| NaiveDateTime::parse_from_str(text, "%Y:%m:%d %H:%M:%S"))
        .ok()?;
    if let Some(offset) = offset {
        if let Ok(t) = DateTime::parse_from_str(&format!("{} {offset}", naive.format("%Y-%m-%d %H:%M:%S")), "%Y-%m-%d %H:%M:%S %:z") {
            return Some(t.timestamp_millis());
        }
    }
    Local.from_local_datetime(&naive).earliest().map(|t| t.timestamp_millis())
}

/// A video's length and when it was recorded, from ffprobe: Apple's own
/// creation date (with the place's offset) when there is one, else the
/// container's; and Apple's content identifier. Nothing of a video it cannot read, or spins on (a damaged
/// one); an error when it hangs, on a folder that does not answer (a mount
/// gone), so that the listing fails rather than keep the video without.
fn probe_video(path: &Path) -> Result<Details> {
    let _slot = pinhole_core::background::slot();
    let mut ffprobe = pinhole_core::background::command("ffprobe");
    ffprobe
        .args(["-v", "quiet", "-print_format", "json", "-show_entries", "format=duration:format_tags"])
        .arg(path)
        .stdin(Stdio::null());
    let output = match pinhole_core::background::output(&mut ffprobe) {
        Ok(output) => output,
        Err(e) if e.kind() == std::io::ErrorKind::TimedOut && !pinhole_core::background::spun(&e) => {
            return Err(anyhow!("reading {}: {e}", path.display()));
        }
        Err(_) => return Ok(Details::default()),
    };
    let Ok(v) = serde_json::from_slice::<serde_json::Value>(&output.stdout) else { return Ok(Details::default()) };
    let format = &v["format"];
    let duration = format["duration"].as_str().and_then(|d| d.parse().ok());
    let tags = &format["tags"];
    let taken = ["com.apple.quicktime.creationdate", "creation_time"]
        .iter()
        .filter_map(|k| tags[*k].as_str())
        .find_map(|t| chrono::DateTime::parse_from_rfc3339(t).ok().or_else(|| {
            chrono::DateTime::parse_from_str(t, "%Y-%m-%dT%H:%M:%S%z").ok()
        }))
        .map(|t| t.timestamp_millis());
    let location = ["com.apple.quicktime.location.ISO6709", "location"]
        .iter().filter_map(|k| tags[*k].as_str()).find_map(video_location);
    let content_id = tags["com.apple.quicktime.content.identifier"].as_str().filter(|c| !c.is_empty()).map(str::to_owned);
    Ok(Details { taken, duration, location, content_id, ..Details::default() })
}

// The QuickTime/MP4 tags produced by ffprobe, e.g. "+52.2319+021.0067+100/".
fn video_location(tag: &str) -> Option<Location> {
    let signed = |s: &str| matches!(s.as_bytes().first(), Some(b'+' | b'-'));
    if !signed(tag) {
        return None;
    }
    let end = |s: &str| s.char_indices().skip(1)
        .find(|&(_, c)| matches!(c, '+' | '-' | '/')).map_or(s.len(), |(i, _)| i);
    let (lat, rest) = tag.split_at(end(tag));
    if !signed(rest) {
        return None;
    }
    Location::new(lat.parse().ok()?, rest[..end(rest)].parse().ok()?)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_video_coordinates_with_signs_and_optional_altitude() {
        assert_eq!(video_location("-33.86+151.21+32/").map(Location::degrees), Some((-33.86, 151.21)));
        assert_eq!(video_location("+52.23-021.00/").map(Location::degrees), Some((52.23, -21.0)));
        assert!(video_location("+92.0+23.0/").is_none());
        assert!(video_location("+52.23/").is_none());
    }

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("pinhole-folder-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn touch(path: &Path) {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, b"x").unwrap();
    }

    #[test]
    fn thumbnails_are_made_whatever_the_files_are_called() {
        let has = |program: &str| std::process::Command::new(program).arg("-version").stdout(Stdio::null()).status().is_ok_and(|s| s.success());
        if !has("magick") || !has("ffmpeg") {
            return;
        }
        // Names ImageMagick reads as its own syntax when given them as
        // they are: `%d`/`%o` as a frame's number, `[1]` as which frame,
        // `*` as many files, `heic:` as a format. In a folder of such a
        // name too.
        let top = temp("names");
        let dir = top.join("Sale 50%d [1]");
        std::fs::create_dir_all(&dir).unwrap();
        let photos = ["50%off.jpg", "IMG [1]*.png", "x%03d.JPG", "heic:odd.jpg", "@list.jpg", "100% done.jpeg"];
        for name in photos {
            let format = if name.ends_with(".png") { "png:-" } else { "jpg:-" };
            let made = std::process::Command::new("magick").args(["-size", "640x480", "xc:#c82828", format]).output().unwrap();
            std::fs::write(dir.join(name), made.stdout).unwrap();
        }
        // Another file a pattern of them would also be.
        image::RgbImage::from_pixel(64, 64, image::Rgb([40, 200, 40])).save(dir.join("500ff.jpg")).unwrap();
        let video = "clip 50%d [1].mov";
        let made = std::process::Command::new("ffmpeg")
            .args(["-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=d=1:s=320x240:r=10", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-f", "mov"])
            .arg(dir.join(video))
            .status()
            .unwrap();
        assert!(made.success());
        let source = FolderSource::new(dir.clone());
        for name in photos.iter().chain([&video]) {
            let jpeg = source.render(name, 64).unwrap_or_else(|e| panic!("{name}: {e:#}"));
            let picture = image::load_from_memory(&jpeg).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert_eq!(picture.width().max(picture.height()), 64, "{name}");
            if *name != video {
                // Its own pixels, not another file's.
                let p = picture.to_rgb8().get_pixel(32, 24).0;
                assert!(p[0] > 150 && p[1] < 100, "{name}: {p:?}");
            }
        }
        let _ = std::fs::remove_dir_all(&top);
    }

    #[test]
    fn live_photos_pair_with_their_motion_and_the_rest_stand_alone() {
        let dir = temp("look");
        for f in ["2026/IMG_1.HEIC", "2026/IMG_1_HEVC.MOV", "2026/IMG_2.JPG", "2026/IMG_2.MOV", "2026/clip.mp4",
                  "2026/notes.txt", ".hidden/IMG_9.JPG", "other/IMG_1.MOV"] {
            touch(&dir.join(f));
        }
        let entries = FolderSource::new(dir.clone()).look(&HashSet::new()).unwrap().entries;
        let mut ids: Vec<(&str, Option<&str>)> =
            entries.values().map(|e| (e.file.rel.as_str(), e.motion.as_ref().map(|m| m.rel.as_str()))).collect();
        ids.sort();
        assert_eq!(ids, [
            ("2026/IMG_1.HEIC", Some("2026/IMG_1_HEVC.MOV")),
            ("2026/IMG_2.JPG", Some("2026/IMG_2.MOV")),
            ("2026/clip.mp4", None),
            // A video named as a photo in another folder is a video.
            ("other/IMG_1.MOV", None),
        ]);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_photo_has_one_motion_and_every_other_video_of_its_name_stands_alone() {
        let found = |rel: &str| Found { rel: rel.into(), media: media_of(Path::new(rel)).unwrap(), size: 1, modified_ns: 0 };
        let files = || vec![found("trip.mp4"), found("trip.jpg"), found("trip.mov"), found("TRIP.MOV")];
        let pairs = |entries: HashMap<String, Entry>| {
            let mut pairs: Vec<(String, Option<String>)> =
                entries.into_values().map(|e| (e.file.rel, e.motion.map(|m| m.rel))).collect();
            pairs.sort();
            pairs
        };
        let some = |s: &str| Some(s.to_owned());
        assert_eq!(pairs(pair(Path::new("/r"), files(), &HashSet::new())), [
            ("trip.jpg".into(), some("TRIP.MOV")),
            ("trip.mov".into(), None),
            ("trip.mp4".into(), None),
        ]);
        // The QuickTime ones found not to be its motion: the next is tried.
        let root = Path::new("/r");
        let not = |video: &str| unpaired_key(root, &found("trip.jpg"), &found(video));
        assert_eq!(pairs(pair(root, files(), &[not("trip.mov"), not("TRIP.MOV")].into())), [
            ("TRIP.MOV".into(), None),
            ("trip.jpg".into(), some("trip.mp4")),
            ("trip.mov".into(), None),
        ]);
        // Another photo of that name: tried with them again.
        let mut other = found("trip.jpg");
        other.size = 2;
        let files = vec![other, found("trip.mov"), found("TRIP.MOV")];
        assert_eq!(pairs(pair(root, files, &[not("trip.mov"), not("TRIP.MOV")].into())), [
            ("trip.jpg".into(), some("TRIP.MOV")),
            ("trip.mov".into(), None),
        ]);
    }

    #[test]
    fn a_video_is_a_photos_motion_by_apples_identifier_else_by_its_length() {
        let details = |id: Option<&str>, duration| Details { content_id: id.map(str::to_owned), duration, ..Details::default() };
        let (a, b) = (Some("8E82EB40"), Some("574219FF"));
        assert!(is_motion(&details(a, None), &details(a, Some(2.2))));
        assert!(!is_motion(&details(a, None), &details(b, Some(2.2))));
        // Without both identifiers: short enough, or of a length unknown.
        assert!(is_motion(&details(None, None), &details(a, Some(2.9))));
        assert!(is_motion(&details(a, None), &details(None, None)));
        assert!(!is_motion(&details(a, None), &details(None, Some(95.0))));
        assert!(!is_motion(&details(None, None), &details(None, Some(12.0))));
        // Apple's maker note, as an iPhone writes it, the identifier past
        // another entry.
        let id = b"8E82EB40-AAB9-4D44-B96D-DFA7BC925D60\0";
        let mut note = b"Apple iOS\0\0\x01MM".to_vec();
        note.extend(2u16.to_be_bytes());
        note.extend([0, 1, 0, 9, 0, 0, 0, 1, 0, 0, 0, 7]);
        note.extend([0, 0x11, 0, 2]);
        note.extend((id.len() as u32).to_be_bytes());
        note.extend(((16 + 24 + 4) as u32).to_be_bytes());
        note.extend([0; 4]);
        note.extend(id);
        assert_eq!(apple_content_id(&note).as_deref(), Some("8E82EB40-AAB9-4D44-B96D-DFA7BC925D60"));
        assert_eq!(apple_content_id(&note[..50]), None);
        assert_eq!(apple_content_id(b"Nikon\0\x02\x10\0\0MM*\0"), None);
    }

    #[test]
    fn a_long_video_named_as_a_photo_is_a_video_of_its_own() {
        if !std::process::Command::new("ffmpeg").arg("-version").stdout(Stdio::null()).status().is_ok_and(|s| s.success()) {
            return;
        }
        let dir = temp("unpaired");
        let made = std::process::Command::new("ffmpeg")
            .args(["-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=d=10:s=64x48:r=2", "-c:v", "libx264", "-pix_fmt", "yuv420p"])
            .arg(dir.join("presentation.mp4"))
            .status()
            .unwrap();
        assert!(made.success());
        let source = FolderSource::new(dir.clone());
        let mut conn = pinhole_core::db::open(&dir.join(".library.sqlite")).unwrap();
        // The video first, a favourite; then a photo of its name beside it.
        pinhole_core::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        source.set_favorite(&conn, &["presentation.mp4".into()], true).unwrap();
        image::RgbImage::from_pixel(64, 48, image::Rgb([200, 40, 40])).save(dir.join("presentation.jpg")).unwrap();
        let report = pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert!(report.changes.removed.is_empty(), "{:?}", report.changes);
        assert!(favorites(&conn).contains("presentation.mp4"));
        let kinds = |conn: &Connection| -> Vec<(String, bool, bool)> {
            let mut stmt = conn.prepare("SELECT id, is_live, is_video FROM assets ORDER BY id").unwrap();
            stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?))).unwrap().map(|r| r.unwrap()).collect()
        };
        let both = vec![("presentation.jpg".to_string(), false, false), ("presentation.mp4".to_string(), false, true)];
        assert_eq!(kinds(&conn), both);
        // Listed afresh, the same.
        pinhole_core::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert_eq!(kinds(&conn), both);
        assert!(favorites(&conn).contains("presentation.mp4"));
        // And it stays so, read no more.
        let report = pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert!(report.changes.updated.is_empty() && report.changes.added.is_empty(), "{:?}", report.changes);
        assert_eq!(kinds(&conn), both);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn what_is_added_is_copied_in_and_found_by_the_next_look() {
        let (from, dir) = (temp("add-from"), temp("add-to"));
        touch(&from.join("IMG_1.HEIC"));
        touch(&from.join("IMG_1.MOV"));
        touch(&dir.join("IMG_1.HEIC"));
        let source = FolderSource::new(dir.clone());
        let (additions, _) = pinhole_core::add::gather(&[from.join("IMG_1.HEIC"), from.join("IMG_1.MOV")]);
        let added = source.add(&additions[0], &|_, _| {}).unwrap();
        assert_eq!(added.id.as_deref(), Some("IMG_1 (2).HEIC"));
        let entries = source.look(&HashSet::new()).unwrap().entries;
        assert_eq!(entries["IMG_1 (2).HEIC"].motion.as_ref().map(|m| m.rel.as_str()), Some("IMG_1 (2).MOV"));
        assert!(entries["IMG_1.HEIC"].motion.is_none());
        // The originals stay where they were.
        assert!(from.join("IMG_1.MOV").is_file());
        assert!(FolderSource::new(PathBuf::from("/nonexistent/pinhole")).add(&additions[0], &|_, _| {}).is_err());
        let _ = std::fs::remove_dir_all(&from);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_favourite_stays_through_an_edit_and_a_listing_and_goes_with_its_file() {
        let dir = temp("favourite");
        for f in ["IMG_1.JPG", "IMG_2.JPG"] {
            touch(&dir.join(f));
        }
        let source = FolderSource::new(dir.clone());
        let mut conn = pinhole_core::db::open(&dir.join(".library.sqlite")).unwrap();
        pinhole_core::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        source.set_favorite(&conn, &["IMG_1.JPG".into()], true).unwrap();
        conn.execute("UPDATE assets SET favorite = 1 WHERE id = 'IMG_1.JPG'", []).unwrap();
        let favourite = |conn: &Connection, id: &str| -> bool {
            conn.query_row("SELECT favorite FROM assets WHERE id = ?1", [id], |r| r.get(0)).unwrap()
        };
        // The file edited: put again, still a favourite.
        std::fs::write(dir.join("IMG_1.JPG"), b"xy").unwrap();
        let report = pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert_eq!(report.changes.updated, ["IMG_1.JPG"]);
        assert!(favourite(&conn, "IMG_1.JPG"));
        // Listed afresh.
        pinhole_core::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert!(favourite(&conn, "IMG_1.JPG"));
        assert!(!favourite(&conn, "IMG_2.JPG"));
        // Gone, and back: not a favourite any more.
        std::fs::remove_file(dir.join("IMG_1.JPG")).unwrap();
        pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert!(favorites(&conn).is_empty());
        touch(&dir.join("IMG_1.JPG"));
        pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert!(!favourite(&conn, "IMG_1.JPG"));
        // And not one again once taken back.
        source.set_favorite(&conn, &["IMG_2.JPG".into()], true).unwrap();
        source.set_favorite(&conn, &["IMG_2.JPG".into()], false).unwrap();
        assert!(favorites(&conn).is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_folder_left_empty_by_its_disk_unmounted_keeps_its_photos_and_favourites() {
        let dir = temp("unmounted");
        for f in ["IMG_1.JPG", "2026/IMG_2.JPG"] {
            touch(&dir.join(f));
        }
        let source = FolderSource::new(dir.clone());
        let mut conn = pinhole_core::db::open(&std::env::temp_dir().join(format!("pinhole-folder-unmounted-{}.sqlite", std::process::id()))).unwrap();
        pinhole_core::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        source.set_favorite(&conn, &["IMG_1.JPG".into()], true).unwrap();
        let count = |conn: &Connection| -> i64 { conn.query_row("SELECT COUNT(*) FROM assets", [], |r| r.get(0)).unwrap() };
        // Its disk unmounted: the mount point empty, on the disk below
        // (another device number than the one remembered).
        pinhole_core::db::set_meta(&conn, DEVICE, "1").unwrap();
        std::fs::remove_dir_all(&dir).unwrap();
        std::fs::create_dir_all(&dir).unwrap();
        let error = pinhole_core::sync::follow(&source, &mut conn).unwrap_err();
        assert!(matches!(source.health(&error), Health::Offline), "{error:#}");
        assert!(pinhole_core::sync::list(&source, &mut conn, &|_, _| {}, &|| false).is_err());
        assert_eq!(count(&conn), 2);
        assert!(favorites(&conn).contains("IMG_1.JPG"));
        // Mounted again: as it was, favourite and all.
        touch(&dir.join("IMG_1.JPG"));
        pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert!(favorites(&conn).contains("IMG_1.JPG"));
        assert_eq!(count(&conn), 1);
        // Emptied on the disk it is on: its photos went.
        std::fs::remove_file(dir.join("IMG_1.JPG")).unwrap();
        pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert_eq!(count(&conn), 0);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn what_cannot_be_read_stays_in_the_library_with_its_favourites() {
        use std::os::unix::fs::PermissionsExt;
        let dir = temp("unread");
        for f in ["IMG_1.JPG", "2026/IMG_2.JPG", "2026/deeper/IMG_3.JPG", "2026b/IMG_4.JPG"] {
            touch(&dir.join(f));
        }
        let source = FolderSource::new(dir.clone());
        let mut conn = pinhole_core::db::open(&dir.join(".library.sqlite")).unwrap();
        pinhole_core::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        source.set_favorite(&conn, &["2026/deeper/IMG_3.JPG".into()], true).unwrap();
        let ids = |conn: &Connection| -> Vec<String> {
            let mut stmt = conn.prepare("SELECT id FROM assets ORDER BY id").unwrap();
            stmt.query_map([], |r| r.get(0)).unwrap().map(|r| r.unwrap()).collect()
        };
        let all = ids(&conn);
        assert_eq!(all.len(), 4);
        // 2026 cannot be read now: a round and a listing both leave what is in
        // it (2026b only shares the start of its name), and a file gone
        // elsewhere still goes.
        std::fs::set_permissions(dir.join("2026"), std::fs::Permissions::from_mode(0o000)).unwrap();
        std::fs::remove_file(dir.join("IMG_1.JPG")).unwrap();
        let report = pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert_eq!(report.changes.removed, ["IMG_1.JPG"]);
        assert!(report.summary.contains("could not be read"), "{}", report.summary);
        pinhole_core::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert_eq!(ids(&conn), ["2026/IMG_2.JPG", "2026/deeper/IMG_3.JPG", "2026b/IMG_4.JPG"]);
        assert!(favorites(&conn).contains("2026/deeper/IMG_3.JPG"));
        // Readable again, and the file in it gone: now that is seen.
        std::fs::set_permissions(dir.join("2026"), std::fs::Permissions::from_mode(0o755)).unwrap();
        std::fs::remove_file(dir.join("2026/IMG_2.JPG")).unwrap();
        pinhole_core::sync::follow(&source, &mut conn).unwrap();
        assert_eq!(ids(&conn), ["2026/deeper/IMG_3.JPG", "2026b/IMG_4.JPG"]);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn two_folders_files_of_one_name_size_and_time_have_their_own_keys() {
        let file = Found { rel: "2026/IMG_0001.JPG".into(), media: Media::Photo, size: 10, modified_ns: 5 };
        let (a, b) = (Path::new("/home/me/Pictures"), Path::new("/media/disk/Pictures"));
        assert_ne!(file.key(a), file.key(b));
        assert_eq!(file.key(a), file.key(a));
    }

    #[test]
    fn a_live_photo_whose_motion_cannot_be_read_stays_as_it_is() {
        let found = |rel: &str, media| Found { rel: rel.into(), media, size: 1, modified_ns: 0 };
        let look = |unread: &[&str], files: Vec<Found>| Look {
            entries: pair(Path::new("/r"), files, &HashSet::new()),
            unread: unread.iter().map(|u| u.to_string()).collect(),
        };
        let photo = || found("2026/IMG_1.HEIC", Media::Photo);
        // Its motion's file is there but could not be looked at.
        for unread in ["2026/IMG_1.MOV", "2026/img_1_hevc.mov"] {
            let l = look(&[unread], vec![photo()]);
            assert!(l.motion_unread(&l.entries["2026/IMG_1.HEIC"]), "{unread}");
        }
        // Another photo's, a picture, or the motion read after all: no.
        for (unread, files) in [
            ("2026/IMG_2.MOV", vec![photo()]),
            ("2026/IMG_1.JPG", vec![photo()]),
            ("2026/other.MOV", vec![photo(), found("2026/IMG_1.MOV", Media::Video)]),
        ] {
            let l = look(&[unread], files);
            assert!(!l.motion_unread(&l.entries["2026/IMG_1.HEIC"]), "{unread}");
        }
    }

    #[test]
    fn a_live_photo_whose_motion_cannot_be_read_stays_one_in_the_library() {
        let dir = temp("live");
        image::RgbImage::from_pixel(64, 48, image::Rgb([200, 40, 40])).save(dir.join("IMG_1.JPG")).unwrap();
        touch(&dir.join("IMG_1.MOV"));
        let mut conn = pinhole_core::db::open(&dir.join(".library.sqlite")).unwrap();
        pinhole_core::sync::list(&FolderSource::new(dir.clone()), &mut conn, &|_, _| {}, &|| false).unwrap();
        let live = |conn: &Connection| -> (bool, Option<String>) {
            conn.query_row(
                "SELECT a.is_live, r.key FROM assets a LEFT JOIN renditions r ON r.asset_id = a.id AND r.kind = 'live'
                 WHERE a.id = 'IMG_1.JPG'",
                [],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )
            .unwrap()
        };
        let before = live(&conn);
        assert!(before.0 && before.1.is_some(), "{before:?}");
        // Its motion there but not read: a round, then a whole listing,
        // and it is a Live Photo still, its motion where it was.
        let mut unread = FolderSource::new(dir.clone());
        unread.unread_too = vec!["IMG_1.MOV".into()];
        pinhole_core::sync::follow(&unread, &mut conn).unwrap();
        assert_eq!(live(&conn), before);
        pinhole_core::sync::list(&unread, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert_eq!(live(&conn), before);
        // Gone for good: a photo alone.
        std::fs::remove_file(dir.join("IMG_1.MOV")).unwrap();
        pinhole_core::sync::follow(&FolderSource::new(dir.clone()), &mut conn).unwrap();
        assert_eq!(live(&conn), (false, None));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn unread_paths_hold_what_is_in_them_and_nothing_beside() {
        let unread = ["2026".to_string(), "a/b.JPG".to_string()];
        assert!(unread_holds(&unread, "2026/IMG_1.JPG"));
        assert!(unread_holds(&unread, "2026/x/IMG_1.JPG"));
        assert!(unread_holds(&unread, "a/b.JPG"));
        assert!(!unread_holds(&unread, "2026b/IMG_1.JPG"));
        assert!(!unread_holds(&unread, "a/b.JPG.MOV"));
        assert!(unread_holds(&["".to_string()], "anything.JPG"));
        assert!(!unread_holds(&[], "anything.JPG"));
    }

    #[test]
    fn only_regular_files_in_the_folder_are_its_photos() {
        let (dir, outside) = (temp("regular"), temp("regular-outside"));
        touch(&dir.join("IMG_1.JPG"));
        touch(&outside.join("private.jpg"));
        std::os::unix::fs::symlink(outside.join("private.jpg"), dir.join("link.jpg")).unwrap();
        std::os::unix::fs::symlink(&outside, dir.join("linked")).unwrap();
        let fifo = std::ffi::CString::new(dir.join("pipe.jpg").into_os_string().into_encoded_bytes()).unwrap();
        assert_eq!(unsafe { libc::mkfifo(fifo.as_ptr(), 0o644) }, 0);
        let source = FolderSource::new(dir.clone());
        let entries = source.look(&HashSet::new()).unwrap().entries;
        assert_eq!(entries.keys().collect::<Vec<_>>(), ["IMG_1.JPG"]);
        // Nor handed out, should the index name them: a link, a file
        // through a linked folder, a pipe (at once, not waiting on it), a
        // path out of the folder.
        let absolute = outside.join("private.jpg").display().to_string();
        for rel in ["link.jpg", "linked/private.jpg", "pipe.jpg", "../regular-outside/private.jpg", absolute.as_str()] {
            let rel = rel.replace("regular-outside", outside.file_name().unwrap().to_str().unwrap());
            assert!(source.file(&rel).is_err(), "{rel}");
            assert!(source.open(&Ticket(format!("original\t{rel}")), None).is_err(), "{rel}");
            let stored = Stored { asset: rel.clone(), want: Want::Original, name: rel.clone(), key: String::new(), file_type: String::new() };
            assert!(source.local(&stored).is_none(), "{rel}");
        }
        assert_eq!(read_exif(&dir.join("pipe.jpg")).taken, None);
        assert!(source.file("IMG_1.JPG").is_ok());
        let _ = std::fs::remove_dir_all(&dir);
        let _ = std::fs::remove_dir_all(&outside);
    }

    #[test]
    fn a_large_tiff_keeps_its_date_read_from_its_start() {
        // A TIFF of one IFD, its date in it, then 64 MB of image.
        let date = b"2024:05:01 12:00:00\0";
        let mut tiff = b"II*\0".to_vec();
        tiff.extend(8u32.to_le_bytes());
        tiff.extend(1u16.to_le_bytes());
        tiff.extend(0x0132u16.to_le_bytes()); // DateTime
        tiff.extend(2u16.to_le_bytes());
        tiff.extend((date.len() as u32).to_le_bytes());
        tiff.extend(26u32.to_le_bytes());
        tiff.extend(0u32.to_le_bytes());
        tiff.extend(date);
        tiff.resize(64 << 20, 0);
        let dir = temp("tiff");
        std::fs::write(dir.join("scan.tif"), &tiff).unwrap();
        assert_eq!(read_exif(&dir.join("scan.tif")).taken, exif_time("2024:05:01 12:00:00", None));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn it_accepts_whatever_a_look_finds() {
        let both: Vec<&str> = pinhole_core::add::PHOTOS.iter().chain(pinhole_core::add::VIDEOS).copied().collect();
        assert_eq!(ACCEPTS, both.as_slice());
    }

    #[test]
    fn a_folder_that_is_not_there_is_an_error_not_an_empty_library() {
        assert!(FolderSource::new(PathBuf::from("/nonexistent/pinhole")).look(&HashSet::new()).is_err());
    }

    #[test]
    fn exif_times_with_and_without_an_offset() {
        assert_eq!(exif_time("2024-05-01 12:00:00", Some("+02:00")), Some(1_714_557_600_000));
        assert!(exif_time("2024:05:01 12:00:00", None).is_some());
        assert_eq!(exif_time("not a date", None), None);
    }
}
