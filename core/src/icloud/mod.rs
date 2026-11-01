//! Everything that is Apple's: iCloud Photos as a source. Signing in goes
//! through a provider process (`auth`, auth/PROTOCOL.md); the library is
//! read with CloudKit's web API (`cloudkit`), whose records `records` pairs
//! into assets; `walk` reads them into the index, beside which iCloud keeps
//! tables of its own (`index`). Files are downloaded from signed links that
//! expire within the hour, looked up a hundred assets at a time.

pub mod auth;
pub mod cloudkit;
pub mod favorite;
pub mod index;
pub mod kind;
pub mod push;
pub mod records;
pub mod session;
pub mod upload;
pub mod walk;

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::{Arc, Mutex, MutexGuard};

use anyhow::{anyhow, Result};
use rusqlite::{params, Connection, OptionalExtension};

use crate::add::{Added, Addition};
use crate::db::Index;
use crate::source::{Body, FetchError, Health, Heard, Listing, Source, Stored, Ticket, Watch};
use crate::sync::now_secs;
use auth::{FixedSession, ProcessProvider, ProviderError};
use cloudkit::{Downloader, PhotosClient};
use records::{pair_records_with_leftovers, CloudAsset};

/// Download links stop working somewhere under 35 minutes; younger than
/// this the one in the index is used as is, older ones are looked up again.
const URL_FRESH_SECS: i64 = 20 * 60;
/// Assets to one lookup, and lookups at once for a long list.
const LOOKUP_ASSETS: usize = 100;
const LOOKUPS_AT_ONCE: usize = 4;

pub struct IcloudSource {
    /// The sign-in provider to run.
    auth: PathBuf,
    /// The Apple ID whose library this is; none until one is signed in to.
    account: Option<String>,
    /// Keep the records as iCloud sent them (pinhole-probe).
    pub keep_raw: bool,
    /// Pages of the list at once, and assets to a page, when walking it.
    pub parallel: usize,
    pub per_page: u32,
    /// Each signed in when first needed, and again after Apple rejected
    /// the session: one for the catalog, one for looking links up, so a
    /// long walk never holds up a photo being opened.
    catalog: Mutex<Option<PhotosClient>>,
    links: Mutex<Option<PhotosClient>>,
    /// The links client's session, for downloads on any thread, without a
    /// deadline for the whole download: a long video takes minutes.
    downloader: Mutex<Option<Downloader>>,
    /// Links that answered as expired, sooner than links usually do: never
    /// handed out again, however fresh the index says they are, so a retry
    /// looks the asset up.
    expired: Mutex<HashSet<String>>,
    /// Fresh links for an index only read (a library given to a window):
    /// by asset and rendition, the link and when it was got, kept here
    /// instead of in the index.
    fresh: Mutex<HashMap<(String, String), (String, i64)>>,
}

impl IcloudSource {
    pub fn new(auth: PathBuf, account: Option<String>) -> Self {
        IcloudSource {
            auth,
            account,
            keep_raw: false,
            parallel: 4,
            per_page: 100,
            catalog: Mutex::new(None),
            links: Mutex::new(None),
            downloader: Mutex::new(None),
            expired: Mutex::new(HashSet::new()),
            fresh: Mutex::new(HashMap::new()),
        }
    }

    /// A signed-in client in `slot`, signing in if there is none.
    fn client<'a>(&self, slot: &'a Mutex<Option<PhotosClient>>) -> Result<MutexGuard<'a, Option<PhotosClient>>> {
        let mut client = slot.lock().unwrap();
        if client.is_none() {
            let Some(account) = &self.account else {
                return Err(ProviderError { code: "needs_sign_in".into(), message: "no Apple ID signed in to".into() }.into());
            };
            let provider = ProcessProvider::spawn(self.auth.clone(), Some(auth::state_dir(account)))?;
            let c = PhotosClient::new(Box::new(provider))?;
            if std::ptr::eq(slot, &self.links) {
                eprintln!("pinholed: signed in as {}", c.session().apple_id);
                *self.downloader.lock().unwrap() = c.downloader().unhurried().ok();
            }
            *client = Some(c);
        }
        Ok(client)
    }

    /// Run `f` with the client in `slot`; a session Apple rejected is
    /// dropped, so the next call signs in again.
    fn with<T>(&self, slot: &Mutex<Option<PhotosClient>>, f: impl FnOnce(&mut PhotosClient) -> Result<T>) -> Result<T> {
        let mut client = self.client(slot)?;
        let result = f(client.as_mut().expect("signed in"));
        if let Err(e) = &result {
            if self.health(e) == Health::NeedsSignIn {
                *client = None;
            }
        }
        result
    }

    /// Fresh links for these (asset, master) pairs, all of each asset's
    /// renditions, `LOOKUP_ASSETS` to a lookup and up to `LOOKUPS_AT_ONCE`
    /// lookups at a time.
    fn look_up(&self, pairs: &[(String, String)]) -> Result<Vec<CloudAsset>> {
        let chunks: Vec<&[(String, String)]> = pairs.chunks(LOOKUP_ASSETS).collect();
        let lookup = |client: &mut PhotosClient, chunk: &[(String, String)]| -> Result<Vec<CloudAsset>> {
            let names: Vec<&str> = chunk.iter().flat_map(|(a, m)| [a.as_str(), m.as_str()]).collect();
            let r = client.lookup(&names)?;
            let records = r["records"].as_array().cloned().unwrap_or_default();
            Ok(pair_records_with_leftovers(&records, false).0)
        };
        if chunks.len() <= 1 {
            return self.with(&self.links, |client| chunks.first().map_or(Ok(Vec::new()), |c| lookup(client, c)));
        }
        // Many: side by side, on clients sharing the one session.
        let session = self.with(&self.links, |client| Ok(client.session().clone()))?;
        let queue = Arc::new(Mutex::new(chunks.into_iter().map(<[_]>::to_vec).collect::<Vec<_>>()));
        let workers: Vec<_> = (0..LOOKUPS_AT_ONCE)
            .map(|_| {
                let (queue, session) = (queue.clone(), session.clone());
                std::thread::spawn(move || -> Result<Vec<CloudAsset>> {
                    let mut client = PhotosClient::new(Box::new(FixedSession(session)))?;
                    let mut out = Vec::new();
                    loop {
                        let chunk = queue.lock().unwrap().pop();
                        let Some(chunk) = chunk else { return Ok(out) };
                        out.extend(lookup(&mut client, &chunk)?);
                    }
                })
            })
            .collect();
        let mut assets = Vec::new();
        for w in workers {
            assets.extend(w.join().map_err(|_| anyhow!("a lookup thread panicked"))??);
        }
        Ok(assets)
    }
}

impl Source for IcloudSource {
    fn name(&self) -> String {
        "iCloud".into()
    }

    fn kind(&self) -> &'static str {
        "icloud"
    }

    fn id(&self) -> String {
        format!("icloud:{}", self.account.as_deref().unwrap_or(""))
    }

    fn details(&self) -> serde_json::Value {
        match self.account.as_deref().filter(|a| !a.is_empty()) {
            Some(account) => serde_json::json!({"account": account}),
            None => serde_json::json!({}),
        }
    }

    fn library_name(&self) -> String {
        self.account.clone().unwrap_or_default()
    }

    fn needs_sign_in(&self) -> bool {
        true
    }

    fn health(&self, error: &anyhow::Error) -> Health {
        if let Some(p) = error.downcast_ref::<ProviderError>() {
            return match p.code.as_str() {
                "needs_sign_in" | "needs_code" | "wrong_code" => Health::NeedsSignIn,
                _ => Health::Offline,
            };
        }
        for cause in error.chain() {
            if let Some(e) = cause.downcast_ref::<reqwest::Error>() {
                if e.is_connect() || e.is_timeout() {
                    return Health::Offline;
                }
            }
        }
        let text = format!("{error:#}");
        if text.contains("answered 401") || text.contains("answered 421") || text.contains("answered 403") {
            return Health::NeedsSignIn;
        }
        Health::Failing
    }

    fn start(&self) -> Result<()> {
        self.with(&self.links, |_| Ok(()))
    }

    fn watch(&self, tell: Box<dyn Fn(Heard) + Send + Sync>) -> Option<Watch> {
        let account = self.account.clone()?;
        let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let (auth, stopping) = (self.auth.clone(), stop.clone());
        std::thread::spawn(move || push::listen(auth, account, tell, stopping));
        Some(Watch::new(stop))
    }

    fn list(&self, index: &mut Index, progress: &(dyn Fn(u64, u64) + Sync)) -> Result<Listing> {
        let r = self.with(&self.catalog, |client| {
            walk::walk(client, index, self.per_page, self.parallel, self.keep_raw, progress)
        })?;
        Ok(Listing {
            token: r.token,
            summary: format!(
                "{} split pairs repaired, {} unpaired, {} short pages; iCloud reports {}",
                r.repaired, r.unpaired, r.short_pages, r.reported
            ),
        })
    }

    fn changes(&self, index: &mut Index, token: &str) -> Result<Listing> {
        let r = self.with(&self.catalog, |client| walk::changes(client, index, token))?;
        Ok(Listing {
            token: Some(r.token),
            summary: format!("{} records, {} skipped", r.records, r.skipped),
        })
    }

    fn prepare(&self, index: &Connection, wanted: &[Stored]) -> Result<Vec<Result<Ticket, FetchError>>> {
        // A link in the index younger than 20 minutes is used as is.
        let now = now_secs();
        let expired = self.expired.lock().unwrap().clone();
        // An index only read keeps no links: those got since are here.
        let read_only = index.is_readonly(rusqlite::DatabaseName::Main).unwrap_or(true);
        let fresh = self.fresh.lock().unwrap();
        let mut tickets: Vec<Option<Result<Ticket, FetchError>>> = Vec::with_capacity(wanted.len());
        let mut stale: Vec<(String, String)> = Vec::new();
        for w in wanted {
            let row: Option<(String, Option<String>, Option<i64>)> = index
                .query_row(
                    "SELECT m.master_id, r.url, r.url_fetched_at FROM icloud_assets m
                     LEFT JOIN icloud_resources r ON r.asset_id = m.asset_id AND r.kind = ?2
                     WHERE m.asset_id = ?1",
                    params![w.asset, w.name],
                    |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
                )
                .optional()?;
            let row = row.map(|(master, url, at)| match fresh.get(&(w.asset.clone(), w.name.clone())) {
                Some((url, at)) => (master, Some(url.clone()), Some(*at)),
                None => (master, url, at),
            });
            match row {
                Some((_, Some(url), Some(at))) if now - at < URL_FRESH_SECS && !expired.contains(&url) => {
                    tickets.push(Some(Ok(Ticket(url))))
                }
                Some((master, _, _)) => {
                    if !stale.iter().any(|(a, _)| *a == w.asset) {
                        stale.push((w.asset.clone(), master));
                    }
                    tickets.push(None);
                }
                None => tickets.push(Some(Err(FetchError::NotFound(format!("iCloud has no record of {}", w.asset))))),
            }
        }
        drop(fresh);
        if stale.is_empty() {
            return Ok(tickets.into_iter().map(Option::unwrap).collect());
        }

        let assets = self.look_up(&stale)?;
        let by_id: HashMap<&str, &CloudAsset> = assets.iter().map(|a| (a.id.as_str(), a)).collect();
        // Fresh links go back into the index, so the next request for any
        // rendition of these assets needs no lookup. Not being able to write
        // them down only costs a lookup later. An index only read is left
        // as it is: they are kept here, until they are stale.
        if read_only {
            let mut kept = self.fresh.lock().unwrap();
            kept.retain(|_, (_, at)| now - *at < URL_FRESH_SECS);
            for asset in &assets {
                for r in &asset.resources {
                    if let Some(url) = &r.url {
                        kept.insert((asset.id.clone(), r.kind.clone()), (url.clone(), now));
                    }
                }
            }
        } else {
            let tx = index.unchecked_transaction();
            for asset in &assets {
                for r in &asset.resources {
                    let _ = index.execute(
                        "UPDATE icloud_resources SET url = ?3, url_fetched_at = ?4 WHERE asset_id = ?1 AND kind = ?2",
                        params![asset.id, r.kind, r.url, now],
                    );
                }
            }
            if let Ok(tx) = tx {
                let _ = tx.commit();
            }
        }
        Ok(wanted
            .iter()
            .zip(tickets)
            .map(|(w, ticket)| {
                ticket.unwrap_or_else(|| {
                    by_id
                        .get(w.asset.as_str())
                        .and_then(|a| a.resource(&w.name))
                        .and_then(|r| r.url.clone())
                        .map(Ticket)
                        .ok_or_else(|| FetchError::NotFound(format!("iCloud has no {} for {}", w.name, w.asset)))
                })
            })
            .collect())
    }

    fn accepts(&self) -> &'static [&'static str] {
        upload::ACCEPTS
    }

    /// Uploaded as icloud.com does (`upload`), with the session the links
    /// client has; one Apple rejected is dropped, as in `with`.
    fn add(&self, addition: &Addition, sent: &(dyn Fn(u64, u64) + Sync)) -> Result<Added> {
        let session = self.with(&self.links, |client| Ok(client.session().clone()))?;
        let result = upload::Uploader::new(session)?.upload(addition, sent);
        if let Err(e) = &result {
            if self.health(e) == Health::NeedsSignIn {
                *self.links.lock().unwrap() = None;
            }
        }
        result
    }

    /// In iCloud itself, as icloud.com sets them (`favorite`), with the
    /// change tags the index has; the new ones go back into it, so the next
    /// change needs no lookup.
    fn set_favorite(&self, index: &Connection, ids: &[String], favorite: bool) -> Result<()> {
        let mut assets = Vec::with_capacity(ids.len());
        for id in ids {
            let tag: Option<Option<String>> = index
                .query_row("SELECT change_tag FROM icloud_assets WHERE asset_id = ?1", [id], |r| r.get(0))
                .optional()?;
            let Some(tag) = tag else { return Err(anyhow!("iCloud has no record of {id}")) };
            assets.push((id.clone(), tag));
        }
        let tags = self.with(&self.links, |client| favorite::set(client, &assets, favorite))?;
        let tx = index.unchecked_transaction()?;
        for (id, tag) in &tags {
            tx.execute("UPDATE icloud_assets SET change_tag = ?2 WHERE asset_id = ?1", params![id, tag])?;
        }
        tx.commit()?;
        Ok(())
    }

    fn open(&self, ticket: &Ticket, range: Option<(u64, u64)>) -> Result<Body, FetchError> {
        let downloader = self.downloader.lock().unwrap().clone();
        let downloader = match downloader {
            Some(d) => d,
            None => {
                drop(self.client(&self.links)?);
                self.downloader.lock().unwrap().clone().ok_or_else(|| anyhow!("no iCloud session"))?
            }
        };
        let range_header = range.map(|(start, end)| format!("bytes={start}-{end}"));
        let response = downloader.fetch(&ticket.0, range_header.as_deref(), false)?;
        let status = response.status();
        if matches!(status.as_u16(), 401 | 403 | 404 | 410) {
            let mut expired = self.expired.lock().unwrap();
            if expired.len() > 10_000 {
                expired.clear();
            }
            expired.insert(ticket.0.clone());
            return Err(FetchError::Expired);
        }
        if !status.is_success() || (range.is_some() && status.as_u16() != 206) {
            return Err(FetchError::Failed(anyhow!("iCloud answered {status}")));
        }
        Ok(Body { length: response.content_length(), reader: Box::new(response) })
    }
}
