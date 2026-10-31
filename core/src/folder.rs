//! A folder of photos on this computer (and the folders in it) as a source.
//! An asset's id is its path inside the folder. The originals are served as
//! they are, never copied; thumbnails and the 2048 px medium size are made
//! from them with ImageMagick (a video's from its most telling frame, with
//! ffmpeg: cover.rs). Changes are found by looking at the folder again: a
//! stat of every file, compared with what the index has. A folder has no
//! favourites of its own: they are kept in its library (`folder_favorites`).
//! See docs/sources.md.

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::{Duration, Instant, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use rayon::prelude::*;
use rusqlite::Connection;

use crate::add::{Added, Addition};
use crate::db::Index;
use crate::source::{Asset, Body, FetchError, Health, Kind, Listing, Rendition, Source, Stored, Ticket, Want};

/// Long edges of the thumbnail (kept in the library, like iCloud's) and of
/// the medium size (made into the cache when first asked for).
const THUMB_EDGE: u32 = 480;
const MEDIUM_EDGE: u32 = 2048;
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
    /// Names its thumbnail and cached files; changes when the file does.
    fn key(&self) -> String {
        let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
        for b in self.rel.bytes().chain(self.size.to_le_bytes()).chain(self.modified_ns.to_le_bytes()) {
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
    fn signature(&self) -> (String, Option<String>) {
        (self.file.key(), self.motion.as_ref().map(Found::key))
    }
}

/// The assets made favourites here (`set_favorite`), none before the first.
fn favorites(index: &Connection) -> HashSet<String> {
    let Ok(mut stmt) = index.prepare("SELECT asset_id FROM folder_favorites") else { return HashSet::new() };
    stmt.query_map([], |r| r.get(0)).map(|rows| rows.flatten().collect()).unwrap_or_default()
}

fn media_of(path: &Path) -> Option<Media> {
    let ext = path.extension()?.to_str()?.to_ascii_lowercase();
    if crate::add::PHOTOS.contains(&ext.as_str()) {
        Some(Media::Photo)
    } else if crate::add::VIDEOS.contains(&ext.as_str()) {
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
/// skipped, and whatever could not be read.
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

/// The assets among the files found: a video named as a photo beside it
/// (IMG_1234.HEIC with IMG_1234.MOV, or IMG_1234_HEVC.MOV as icloudpd names
/// it) is that photo's motion, not an asset of its own.
/// A file's folder and name without its extension, lower case: a photo and
/// its motion share it (the motion maybe with `_hevc` after).
fn stem(rel: &str) -> String {
    rel.rsplit_once('.').map_or(rel, |(s, _)| s).to_ascii_lowercase()
}

fn pair(found: Vec<Found>) -> HashMap<String, Entry> {
    let photos: HashSet<String> = found.iter().filter(|f| f.media == Media::Photo).map(|f| stem(&f.rel)).collect();
    let mut motion: HashMap<String, Found> = HashMap::new();
    let mut entries: HashMap<String, Entry> = HashMap::new();
    for f in found {
        if f.media == Media::Video {
            let s = stem(&f.rel);
            let photo = s.strip_suffix("_hevc").filter(|p| photos.contains(*p)).map(str::to_owned)
                .or_else(|| photos.contains(&s).then_some(s));
            if let Some(photo) = photo {
                motion.insert(photo, f);
                continue;
            }
        }
        entries.insert(f.rel.clone(), Entry { file: f, motion: None });
    }
    for entry in entries.values_mut() {
        if entry.file.media == Media::Photo {
            entry.motion = motion.remove(&stem(&entry.file.rel));
        }
    }
    entries
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
    let entries = pair(found);
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
    /// to its photo (`pair`), and what could not be read. Hidden files and
    /// folders are skipped.
    fn look(&self) -> Result<Look> {
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
        unread.sort();
        unread.dedup();
        Ok(Look { entries: pair(walk.found), unread })
    }

    /// The asset for an entry: its dates and details read from the file.
    fn describe(&self, entry: &Entry) -> Asset {
        let f = &entry.file;
        let path = self.root.join(&f.rel);
        let modified_ms = (f.modified_ns / 1_000_000) as i64;
        let (taken, duration, orientation, has_location) = match f.media {
            Media::Photo => {
                let exif = read_exif(&path);
                (exif.taken, None, exif.orientation, exif.has_location)
            }
            Media::Video => {
                let probe = probe_video(&path);
                (probe.taken, probe.duration, None, false)
            }
        };
        let key = f.key();
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
        if let Some(m) = &entry.motion {
            renditions.push(Rendition {
                want: Want::Live,
                name: m.rel.clone(),
                key: m.key(),
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
            kind: match (f.media, &entry.motion) {
                (Media::Video, _) => Kind::Video,
                (Media::Photo, Some(_)) => Kind::Live,
                (Media::Photo, None) => Kind::Photo,
            },
            favorite: false,
            duration,
            orientation,
            has_location,
            size: Some(f.size as i64),
            effect: None,
            renditions,
        }
    }

    /// Describe entries (reading files takes a while: in parallel) and put
    /// them, a batch at a time, the favourites as the library keeps them.
    fn put(&self, index: &mut Index, entries: &[&Entry], progress: &(dyn Fn(u64, u64) + Sync), total: u64) -> Result<()> {
        let favorites = favorites(index.conn);
        for batch in entries.chunks(PUT_BATCH) {
            if index.stopping() {
                return Err(crate::source::Stopped.into());
            }
            let mut assets: Vec<Asset> = batch.par_iter().map(|e| self.describe(e)).collect();
            for a in &mut assets {
                a.favorite = favorites.contains(&a.id);
            }
            index.put(&assets)?;
            progress(index.seen.len() as u64, total);
        }
        Ok(())
    }

    /// A JPEG of a photo or of a video's frame, at most `edge` on the long
    /// side, the right way up.
    fn render(&self, rel: &str, edge: u32) -> Result<Vec<u8>> {
        let path = self.root.join(rel);
        // Never more at once than background work may take (background.rs).
        let _slot = crate::background::slot();
        // A video's from its most telling frame (cover.rs).
        if media_of(&path) == Some(Media::Video) {
            return crate::cover::pick(&path, edge).with_context(|| format!("could not make an image of {rel}"));
        }
        let fit = format!("{edge}x{edge}>");
        let mut magick = crate::background::command("magick");
        // Decoding a big JPEG at a fraction of its size is much quicker.
        // Only a big one: the hint lets the decoder enlarge a small one too.
        let big = image::image_dimensions(&path).is_ok_and(|(w, h)| w.max(h) > edge * 2);
        if big {
            magick.args(["-define", &format!("jpeg:size={}x{}", edge * 2, edge * 2)]);
        }
        let output = magick
            .arg(format!("{}[0]", path.display()))
            .args(["-auto-orient", "-thumbnail", &fit, "-quality", "82", "jpg:-"])
            .stdin(Stdio::null())
            .output()
            .context("running magick")?;
        if output.stdout.is_empty() {
            return Err(anyhow!("could not make an image of {rel}: {}", String::from_utf8_lossy(&output.stderr).trim()));
        }
        Ok(output.stdout)
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
            format!("{name}~{:016x}", crate::storage::stable_hash(path.as_bytes()))
        } else {
            name
        }
    }

    /// Until October 2026 a dash in the path was not told apart from a slash.
    fn former_library_names(&self) -> Vec<String> {
        let name = self.root.to_string_lossy().trim_matches('/').replace('/', "-");
        vec![if name.is_empty() { "root".into() } else { name }]
    }

    fn health(&self, error: &anyhow::Error) -> Health {
        if !self.root.is_dir() {
            return Health::Offline;
        }
        let _ = error;
        Health::Failing
    }

    fn rounds_every(&self) -> Duration {
        Duration::from_secs(10)
    }

    fn list(&self, index: &mut Index, progress: &(dyn Fn(u64, u64) + Sync)) -> Result<Listing> {
        let look = self.look()?;
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
            token: Some(crate::sync::now_secs().to_string()),
            summary: format!("{} photos and videos in {}, {lives} of them Live Photos{}", entries.len(),
                             self.root.display(), look.unread_note()),
        })
    }

    fn changes(&self, index: &mut Index, _token: &str) -> Result<Listing> {
        let look = self.look()?;
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
            .filter(|e| known.get(&e.file.rel) != Some(&e.signature()))
            .filter(|e| !(known.contains_key(&e.file.rel) && look.motion_unread(e)))
            .collect();
        let gone: Vec<String> = known
            .keys()
            .filter(|id| !entries.contains_key(*id) && !unread_holds(&look.unread, id))
            .cloned()
            .collect();
        self.put(index, &changed, &|_, _| {}, changed.len() as u64)?;
        index.remove(&gone)?;
        Ok(Listing {
            token: Some(crate::sync::now_secs().to_string()),
            summary: format!("{} files looked at{}", entries.len(), look.unread_note()),
        })
    }

    fn local(&self, rendition: &Stored) -> Option<PathBuf> {
        matches!(rendition.want, Want::Full | Want::Original | Want::Video | Want::Live)
            .then(|| self.root.join(&rendition.name))
    }

    fn prepare(&self, _index: &Connection, wanted: &[Stored]) -> Result<Vec<Result<Ticket, FetchError>>> {
        Ok(wanted
            .iter()
            .map(|w| {
                if self.root.join(&w.name).is_file() {
                    Ok(Ticket(format!("{}\t{}", w.want.name(), w.name)))
                } else {
                    Err(FetchError::NotFound(format!("{} is not in {}", w.name, self.root.display())))
                }
            })
            .collect())
    }

    fn accepts(&self) -> &'static [&'static str] {
        ACCEPTS
    }

    /// Copied into the folder itself, beside what is there, never over it
    /// (`add::copy_into`); the next look at the folder finds it.
    fn add(&self, addition: &Addition, sent: &(dyn Fn(u64, u64) + Sync)) -> Result<Added> {
        if !self.root.is_dir() {
            return Err(anyhow!("{} is not there", self.root.display()));
        }
        let names = crate::add::copy_into(&self.root, addition, sent)?;
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
            let bytes = self.render(rel, edge)?;
            return Ok(Body { length: Some(bytes.len() as u64), reader: Box::new(std::io::Cursor::new(bytes)) });
        }
        let path = self.root.join(rel);
        let mut file = File::open(&path).map_err(|e| match e.kind() {
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

#[derive(Default)]
struct Exif {
    taken: Option<i64>,
    orientation: Option<i64>,
    has_location: bool,
}

/// When a photo was taken (the camera's local time, with its offset when
/// it wrote one, else this computer's), which way up it is, and whether it
/// has a place.
fn read_exif(path: &Path) -> Exif {
    let Ok(file) = File::open(path) else { return Exif::default() };
    let Ok(exif) = exif::Reader::new().read_from_container(&mut std::io::BufReader::new(file)) else {
        return Exif::default();
    };
    let text = |tag| {
        exif.get_field(tag, exif::In::PRIMARY).map(|f| f.display_value().to_string().trim_matches('"').to_owned())
    };
    let taken = text(exif::Tag::DateTimeOriginal)
        .or_else(|| text(exif::Tag::DateTimeDigitized))
        .or_else(|| text(exif::Tag::DateTime))
        .and_then(|t| exif_time(&t, text(exif::Tag::OffsetTimeOriginal).as_deref()));
    let orientation = exif.get_field(exif::Tag::Orientation, exif::In::PRIMARY).and_then(|f| f.value.get_uint(0)).map(i64::from);
    let has_location = exif.get_field(exif::Tag::GPSLatitude, exif::In::PRIMARY).is_some();
    Exif { taken, orientation, has_location }
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

#[derive(Default)]
struct Probe {
    taken: Option<i64>,
    duration: Option<f64>,
}

/// A video's length and when it was recorded, from ffprobe: Apple's own
/// creation date (with the place's offset) when there is one, else the
/// container's.
fn probe_video(path: &Path) -> Probe {
    let _slot = crate::background::slot();
    let Ok(output) = crate::background::command("ffprobe")
        .args(["-v", "quiet", "-print_format", "json", "-show_entries", "format=duration:format_tags"])
        .arg(path)
        .stdin(Stdio::null())
        .output()
    else {
        return Probe::default();
    };
    let Ok(v) = serde_json::from_slice::<serde_json::Value>(&output.stdout) else { return Probe::default() };
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
    Probe { taken, duration }
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn live_photos_pair_with_their_motion_and_the_rest_stand_alone() {
        let dir = temp("look");
        for f in ["2026/IMG_1.HEIC", "2026/IMG_1_HEVC.MOV", "2026/IMG_2.JPG", "2026/IMG_2.MOV", "2026/clip.mp4",
                  "2026/notes.txt", ".hidden/IMG_9.JPG", "other/IMG_1.MOV"] {
            touch(&dir.join(f));
        }
        let entries = FolderSource::new(dir.clone()).look().unwrap().entries;
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
    fn what_is_added_is_copied_in_and_found_by_the_next_look() {
        let (from, dir) = (temp("add-from"), temp("add-to"));
        touch(&from.join("IMG_1.HEIC"));
        touch(&from.join("IMG_1.MOV"));
        touch(&dir.join("IMG_1.HEIC"));
        let source = FolderSource::new(dir.clone());
        let (additions, _) = crate::add::gather(&[from.join("IMG_1.HEIC"), from.join("IMG_1.MOV")]);
        let added = source.add(&additions[0], &|_, _| {}).unwrap();
        assert_eq!(added.id.as_deref(), Some("IMG_1 (2).HEIC"));
        let entries = source.look().unwrap().entries;
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
        let mut conn = crate::db::open(&dir.join(".library.sqlite")).unwrap();
        crate::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        source.set_favorite(&conn, &["IMG_1.JPG".into()], true).unwrap();
        conn.execute("UPDATE assets SET favorite = 1 WHERE id = 'IMG_1.JPG'", []).unwrap();
        let favourite = |conn: &Connection, id: &str| -> bool {
            conn.query_row("SELECT favorite FROM assets WHERE id = ?1", [id], |r| r.get(0)).unwrap()
        };
        // The file edited: put again, still a favourite.
        std::fs::write(dir.join("IMG_1.JPG"), b"xy").unwrap();
        let report = crate::sync::follow(&source, &mut conn).unwrap();
        assert_eq!(report.changes.updated, ["IMG_1.JPG"]);
        assert!(favourite(&conn, "IMG_1.JPG"));
        // Listed afresh.
        crate::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert!(favourite(&conn, "IMG_1.JPG"));
        assert!(!favourite(&conn, "IMG_2.JPG"));
        // Gone, and back: not a favourite any more.
        std::fs::remove_file(dir.join("IMG_1.JPG")).unwrap();
        crate::sync::follow(&source, &mut conn).unwrap();
        assert!(favorites(&conn).is_empty());
        touch(&dir.join("IMG_1.JPG"));
        crate::sync::follow(&source, &mut conn).unwrap();
        assert!(!favourite(&conn, "IMG_1.JPG"));
        // And not one again once taken back.
        source.set_favorite(&conn, &["IMG_2.JPG".into()], true).unwrap();
        source.set_favorite(&conn, &["IMG_2.JPG".into()], false).unwrap();
        assert!(favorites(&conn).is_empty());
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
        let mut conn = crate::db::open(&dir.join(".library.sqlite")).unwrap();
        crate::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
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
        let report = crate::sync::follow(&source, &mut conn).unwrap();
        assert_eq!(report.changes.removed, ["IMG_1.JPG"]);
        assert!(report.summary.contains("could not be read"), "{}", report.summary);
        crate::sync::list(&source, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert_eq!(ids(&conn), ["2026/IMG_2.JPG", "2026/deeper/IMG_3.JPG", "2026b/IMG_4.JPG"]);
        assert!(favorites(&conn).contains("2026/deeper/IMG_3.JPG"));
        // Readable again, and the file in it gone: now that is seen.
        std::fs::set_permissions(dir.join("2026"), std::fs::Permissions::from_mode(0o755)).unwrap();
        std::fs::remove_file(dir.join("2026/IMG_2.JPG")).unwrap();
        crate::sync::follow(&source, &mut conn).unwrap();
        assert_eq!(ids(&conn), ["2026/deeper/IMG_3.JPG", "2026b/IMG_4.JPG"]);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_live_photo_whose_motion_cannot_be_read_stays_as_it_is() {
        let found = |rel: &str, media| Found { rel: rel.into(), media, size: 1, modified_ns: 0 };
        let look = |unread: &[&str], files: Vec<Found>| Look {
            entries: pair(files),
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
        let mut conn = crate::db::open(&dir.join(".library.sqlite")).unwrap();
        crate::sync::list(&FolderSource::new(dir.clone()), &mut conn, &|_, _| {}, &|| false).unwrap();
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
        crate::sync::follow(&unread, &mut conn).unwrap();
        assert_eq!(live(&conn), before);
        crate::sync::list(&unread, &mut conn, &|_, _| {}, &|| false).unwrap();
        assert_eq!(live(&conn), before);
        // Gone for good: a photo alone.
        std::fs::remove_file(dir.join("IMG_1.MOV")).unwrap();
        crate::sync::follow(&FolderSource::new(dir.clone()), &mut conn).unwrap();
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
    fn it_accepts_whatever_a_look_finds() {
        let both: Vec<&str> = crate::add::PHOTOS.iter().chain(crate::add::VIDEOS).copied().collect();
        assert_eq!(ACCEPTS, both.as_slice());
    }

    #[test]
    fn a_folder_that_is_not_there_is_an_error_not_an_empty_library() {
        assert!(FolderSource::new(PathBuf::from("/nonexistent/pinhole")).look().is_err());
    }

    #[test]
    fn exif_times_with_and_without_an_offset() {
        assert_eq!(exif_time("2024-05-01 12:00:00", Some("+02:00")), Some(1_714_557_600_000));
        assert!(exif_time("2024:05:01 12:00:00", None).is_some());
        assert_eq!(exif_time("not a date", None), None);
    }
}
