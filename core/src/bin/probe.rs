//! pinhole-probe: measure what the iCloud Photos web API can do for Pinhole
//! before the app is built on it. Read-only: nothing here writes to iCloud.

use std::io::Read;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use clap::{Parser, Subcommand};
use rand::seq::SliceRandom;
use rusqlite::Connection;

use pinhole_core::auth::{FixedSession, ProcessProvider};
use pinhole_core::cloudkit::{PhotosClient, Traffic, LIBRARY_COUNT};
use pinhole_core::db;
use pinhole_core::source::{pair_records, Asset, ICloudSource, Page, PhotoSource};

#[derive(Parser)]
struct Cli {
    /// The sign-in provider to run (see auth/PROTOCOL.md).
    #[arg(long, default_value = "auth/pyicloud/pinhole-auth")]
    auth: PathBuf,
    /// The SQLite index the probe writes.
    #[arg(long)]
    db: Option<PathBuf>,
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Show the session the provider hands over, without its secrets.
    Session,
    /// Library zones, indexing state and item counts.
    Count,
    /// How many assets one list request can return, and how long it takes.
    PageSize,
    /// Read the whole library's metadata into the index. Resumable.
    Walk {
        /// Assets per request.
        #[arg(long, default_value_t = 100)]
        per_page: u32,
        /// Keep the raw CloudKit records too, to inspect fields.
        #[arg(long)]
        raw: bool,
        /// Start over instead of resuming.
        #[arg(long)]
        restart: bool,
        /// Requests in flight at once. Above 1 the walk always starts over.
        #[arg(long, default_value_t = 1)]
        parallel: usize,
    },
    /// Ask CloudKit's change feed what changed since the walk began.
    Changes {
        /// Read the feed from the very beginning instead, for this many pages.
        #[arg(long)]
        from_start: Option<u32>,
    },
    /// Totals from the index: counts, sizes and dimensions per rendition.
    Stats,
    /// Download a random sample of one rendition and time it.
    Sample {
        #[arg(long, default_value = "JPEGThumb")]
        kind: String,
        #[arg(long, default_value_t = 200)]
        n: usize,
        #[arg(long, default_value_t = 8)]
        threads: usize,
        /// Keep the downloaded files here.
        #[arg(long)]
        out: Option<PathBuf>,
        /// shared: one connection pool for all threads; own: one per thread;
        /// own-h1: one per thread, HTTP/1.1 only.
        #[arg(long, default_value = "own")]
        pool: String,
    },
    /// Check whether videos can be streamed: ranged reads, latency, expiry.
    Stream {
        #[arg(long, default_value_t = 3)]
        n: usize,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let db_path = cli.db.clone().unwrap_or_else(default_db);
    if let Some(dir) = db_path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let provider = Box::new(ProcessProvider::spawn(cli.auth.clone(), None)?);
    let mut client = PhotosClient::new(provider)?;

    match cli.command {
        Cmd::Session => session(&client),
        Cmd::Count => count(&mut client),
        Cmd::PageSize => page_size(&mut client),
        Cmd::Walk { per_page, raw, parallel, .. } if parallel > 1 => walk_parallel(client, &db_path, per_page, raw, parallel),
        Cmd::Walk { per_page, raw, restart, .. } => walk(client, &db_path, per_page, raw, restart),
        Cmd::Changes { from_start } => changes(&mut client, &db_path, from_start),
        Cmd::Stats => stats(&db_path),
        Cmd::Sample { kind, n, threads, out, pool } => sample(&mut client, &db_path, &kind, n, threads, out, &pool),
        Cmd::Stream { n } => stream(&mut client, &db_path, n),
    }
}

fn default_db() -> PathBuf {
    let base = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/share"));
    base.join("pinhole/probe.sqlite")
}

fn now_secs() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0)
}

fn session(client: &PhotosClient) -> Result<()> {
    let s = client.session();
    let mut services: Vec<&String> = s.services.keys().collect();
    services.sort();
    println!("apple id      {}", s.apple_id);
    println!("validated at  {}", s.validated_at);
    println!("cookies       {}", s.cookies.len());
    println!("ckdatabasews  {}", s.service("ckdatabasews").unwrap_or("missing"));
    println!("services      {}", services.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(" "));
    Ok(())
}

fn count(client: &mut PhotosClient) -> Result<()> {
    for scope in ["private", "shared"] {
        let zones = client.zones(scope)?;
        let names: Vec<String> = zones["zones"]
            .as_array()
            .into_iter()
            .flatten()
            .map(|z| format!("{}{}", z["zoneID"]["zoneName"].as_str().unwrap_or("?"), if z["deleted"] == true { " (deleted)" } else { "" }))
            .collect();
        println!("{scope:8} zones: {}", names.join(", "));
    }
    println!("indexing state: {}", client.indexing_state()?);
    for index in [
        LIBRARY_COUNT,
        "CPLAssetInSmartAlbumByAssetDate:Favorite",
        "CPLAssetInSmartAlbumByAssetDate:Video",
        "CPLAssetInSmartAlbumByAssetDate:Live",
        "CPLAssetHiddenByAssetDate",
        "CPLAssetDeletedByExpungedDate",
    ] {
        match client.count(index) {
            Ok(n) => println!("{n:>8}  {index}"),
            Err(e) => println!("{:>8}  {index}: {e}", "?"),
        }
    }
    Ok(())
}

fn page_size(client: &mut PhotosClient) -> Result<()> {
    println!("{:>8} {:>8} {:>8} {:>9} {:>8}", "asked", "records", "assets", "bytes", "ms");
    for assets in [50u32, 100, 200, 500, 1000] {
        let before = client.traffic.clone();
        let started = Instant::now();
        let r = client.list_page(0, assets * 2)?;
        let ms = started.elapsed().as_millis();
        let records = r["records"].as_array().map_or(0, Vec::len);
        let paired = pair_records(r["records"].as_array().map(Vec::as_slice).unwrap_or(&[]), false).len();
        println!("{:>8} {:>8} {:>8} {:>9} {:>8}", assets, records, paired, client.traffic.bytes - before.bytes, ms);
    }
    Ok(())
}

fn walk(client: PhotosClient, db_path: &PathBuf, per_page: u32, raw: bool, restart: bool) -> Result<()> {
    let mut conn = db::open(db_path)?;
    let mut source = ICloudSource { client, keep_raw: raw };
    let total = source.count()?;
    let mut rank: u64 = if restart { 0 } else { db::meta(&conn, "walk_next_rank").and_then(|v| v.parse().ok()).unwrap_or(0) };
    if rank == 0 {
        db::set_meta(&conn, "walk_started_at", &now_secs().to_string())?;
    }
    eprintln!("library: {total} assets; starting at {rank}, {per_page} per request");

    let started = Instant::now();
    let start_rank = rank;
    let mut pages = 0u64;
    loop {
        let page = source.page(rank, per_page)?;
        if pages == 0 && rank == 0 {
            // The token from the walk's first page is where the change feed
            // picks up afterwards: everything after it is "new since".
            if let Some(token) = &page.sync_token {
                db::set_meta(&conn, "walk_sync_token", token)?;
            }
        }
        if page.records == 0 {
            break;
        }
        db::upsert_assets(&mut conn, &page.assets, Some(rank), now_secs())?;
        // A page whose records failed to pair still moves the cursor, or the
        // walk would ask for the same page forever.
        rank += (page.assets.len() as u64).max(1);
        pages += 1;
        db::set_meta(&conn, "walk_next_rank", &rank.to_string())?;
        if pages % 20 == 0 {
            let done = rank - start_rank;
            let rate = done as f64 / started.elapsed().as_secs_f64();
            let left = total.saturating_sub(rank) as f64 / rate.max(0.001);
            eprintln!("{rank:>7}/{total}  {rate:>6.0} assets/s  ~{left:.0}s left  ({} requests, {} retries)", source.client.traffic.requests, source.client.traffic.retries);
        }
    }
    let secs = started.elapsed().as_secs_f64();
    let t = &source.client.traffic;
    db::set_meta(&conn, "walk_done_at", &now_secs().to_string())?;
    println!("walked {} assets in {:.1}s ({:.0}/s)", rank - start_rank, secs, (rank - start_rank) as f64 / secs);
    println!("requests {}  avg {:.0} ms  received {:.1} MB  retries {}  session refreshes {}",
        t.requests, t.time.as_millis() as f64 / t.requests.max(1) as f64, t.bytes as f64 / 1e6, t.retries, t.session_refreshes);
    let indexed: i64 = conn.query_row("SELECT count(*) FROM assets", [], |r| r.get(0))?;
    println!("index now holds {indexed} assets; iCloud reports {total}");
    Ok(())
}

/// The list is addressed by position, so pages can be fetched side by side:
/// workers take the next free position, the main thread writes the index.
fn walk_parallel(mut client: PhotosClient, db_path: &PathBuf, per_page: u32, raw: bool, parallel: usize) -> Result<()> {
    let mut conn = db::open(db_path)?;
    let total = client.count(LIBRARY_COUNT)?;
    db::set_meta(&conn, "walk_started_at", &now_secs().to_string())?;
    eprintln!("library: {total} assets; {parallel} requests at a time, {per_page} per request");

    let next = Arc::new(AtomicU64::new(0));
    let (tx, rx) = mpsc::channel::<Result<(u64, Page, Traffic)>>();
    let session = client.session().clone();
    let started = Instant::now();
    let workers: Vec<_> = (0..parallel)
        .map(|_| {
            let (next, tx, session) = (next.clone(), tx.clone(), session.clone());
            std::thread::spawn(move || {
                let client = match PhotosClient::new(Box::new(FixedSession(session))) {
                    Ok(c) => c,
                    Err(e) => return drop(tx.send(Err(e))),
                };
                let mut source = ICloudSource { client, keep_raw: raw };
                loop {
                    let rank = next.fetch_add(per_page as u64, Ordering::SeqCst);
                    if rank >= total {
                        break;
                    }
                    let before = source.client.traffic.clone();
                    let result = source.page(rank, per_page).map(|p| {
                        let t = &source.client.traffic;
                        let delta = Traffic {
                            requests: t.requests - before.requests,
                            bytes: t.bytes - before.bytes,
                            time: t.time - before.time,
                            retries: t.retries - before.retries,
                            session_refreshes: 0,
                        };
                        (rank, p, delta)
                    });
                    let failed = result.is_err();
                    if tx.send(result).is_err() || failed {
                        break;
                    }
                }
            })
        })
        .collect();
    drop(tx);

    let mut traffic = Traffic::default();
    let (mut assets, mut pages, mut short_pages) = (0u64, 0u64, 0u64);
    for message in rx {
        let (rank, page, t) = message?;
        if rank == 0 {
            if let Some(token) = &page.sync_token {
                db::set_meta(&conn, "walk_sync_token", token)?;
            }
        }
        if (page.assets.len() as u32) < per_page && rank + (per_page as u64) < total {
            short_pages += 1;
        }
        db::upsert_assets(&mut conn, &page.assets, Some(rank), now_secs())?;
        assets += page.assets.len() as u64;
        pages += 1;
        traffic.requests += t.requests;
        traffic.bytes += t.bytes;
        traffic.time += t.time;
        traffic.retries += t.retries;
        if pages % 40 == 0 {
            let rate = assets as f64 / started.elapsed().as_secs_f64();
            eprintln!("{assets:>7}/{total}  {rate:>6.0} assets/s  ~{:.0}s left  ({} retries)", total.saturating_sub(assets) as f64 / rate, traffic.retries);
        }
    }
    for w in workers {
        let _ = w.join();
    }
    let secs = started.elapsed().as_secs_f64();
    db::set_meta(&conn, "walk_done_at", &now_secs().to_string())?;
    println!("walked {assets} assets in {secs:.1}s ({:.0}/s) with {parallel} in flight", assets as f64 / secs);
    println!("requests {}  avg {:.0} ms  received {:.1} MB  retries {}  short pages {short_pages}",
        traffic.requests, traffic.time.as_millis() as f64 / traffic.requests.max(1) as f64, traffic.bytes as f64 / 1e6, traffic.retries);
    let indexed: i64 = conn.query_row("SELECT count(*) FROM assets", [], |r| r.get(0))?;
    println!("index now holds {indexed} assets; iCloud reports {total}");
    Ok(())
}

fn changes(client: &mut PhotosClient, db_path: &PathBuf, from_start: Option<u32>) -> Result<()> {
    let conn = db::open(db_path)?;
    let (mut token, pages) = match from_start {
        Some(p) => (None, p),
        None => (Some(db::meta(&conn, "walk_sync_token").ok_or_else(|| anyhow!("no walk_sync_token; run walk first"))?), u32::MAX),
    };
    let mut total_records = 0usize;
    for page in 0..pages {
        let started = Instant::now();
        let r = client.zone_changes(token.as_deref(), 400)?;
        let zone = &r["zones"][0];
        if let Some(code) = zone["serverErrorCode"].as_str() {
            println!("page {page}: server error {code}: {}", zone["reason"].as_str().unwrap_or(""));
            break;
        }
        let records = zone["records"].as_array().cloned().unwrap_or_default();
        total_records += records.len();
        let mut kinds = std::collections::BTreeMap::<String, usize>::new();
        for rec in &records {
            let k = if rec["deleted"] == true { "deleted".to_owned() } else { rec["recordType"].as_str().unwrap_or("?").to_owned() };
            *kinds.entry(k).or_default() += 1;
        }
        let paired = pair_records(&records, false);
        println!("page {page}: {} records {:?}, {} assets paired, moreComing={}, {} ms",
            records.len(), kinds, paired.len(), zone["moreComing"], started.elapsed().as_millis());
        for a in paired.iter().take(5) {
            println!("    {}  {}  {}", a.id, a.filename.as_deref().unwrap_or("?"), a.asset_date.unwrap_or(0));
        }
        token = zone["syncToken"].as_str().map(str::to_owned);
        if zone["moreComing"] != true {
            break;
        }
    }
    println!("{total_records} records in total; final token {}", if token.is_some() { "present" } else { "missing" });
    Ok(())
}

fn stats(db_path: &PathBuf) -> Result<()> {
    let conn = db::open(db_path)?;
    let q = |sql: &str| -> Result<i64> { Ok(conn.query_row(sql, [], |r| r.get::<_, Option<i64>>(0))?.unwrap_or(0)) };
    println!("assets {}  videos {}  live {}  favorites {}  with location {}",
        q("SELECT count(*) FROM assets")?, q("SELECT count(*) FROM assets WHERE is_video")?,
        q("SELECT count(*) FROM assets WHERE is_live")?, q("SELECT count(*) FROM assets WHERE favorite")?,
        q("SELECT count(*) FROM assets WHERE has_location")?);
    println!("captured from {} to {}",
        date(q("SELECT min(asset_date) FROM assets WHERE asset_date > 0")?), date(q("SELECT max(asset_date) FROM assets")?));

    println!("\n{:<18} {:>7} {:>10} {:>9} {:>14}", "rendition", "assets", "total GB", "avg KB", "long edge p5/50/95");
    let mut stmt = conn.prepare("SELECT kind, count(*), sum(size), avg(size) FROM resources GROUP BY kind ORDER BY sum(size) DESC")?;
    let rows: Vec<(String, i64, i64, f64)> = stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get::<_, Option<i64>>(2)?.unwrap_or(0), r.get::<_, Option<f64>>(3)?.unwrap_or(0.0))))?.collect::<Result<_, _>>()?;
    for (kind, n, sum, avg) in rows {
        let mut edges: Vec<i64> = conn
            .prepare("SELECT max(coalesce(width,0), coalesce(height,0)) FROM resources WHERE kind = ?1")?
            .query_map([&kind], |r| r.get(0))?
            .collect::<Result<_, _>>()?;
        edges.sort_unstable();
        let pct = |p: f64| edges.get(((edges.len() as f64 - 1.0) * p) as usize).copied().unwrap_or(0);
        println!("{:<18} {:>7} {:>10.2} {:>9.0} {:>6}/{}/{}", kind, n, sum as f64 / 1e9, avg / 1e3, pct(0.05), pct(0.5), pct(0.95));
    }

    println!("\nfile types (originals):");
    let mut stmt = conn.prepare("SELECT coalesce(file_type,'?'), count(*) FROM resources WHERE kind='Original' GROUP BY 1 ORDER BY 2 DESC LIMIT 12")?;
    for row in stmt.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?)))? {
        let (t, n) = row?;
        println!("  {n:>7}  {t}");
    }
    println!("\nassets per year:");
    let mut stmt = conn.prepare("SELECT strftime('%Y', asset_date/1000, 'unixepoch') y, count(*) FROM assets GROUP BY y ORDER BY y")?;
    for row in stmt.query_map([], |r| Ok((r.get::<_, Option<String>>(0)?, r.get::<_, i64>(1)?)))? {
        let (y, n) = row?;
        println!("  {}  {n}", y.unwrap_or_else(|| "?".into()));
    }
    let size = std::fs::metadata(db_path)?.len();
    println!("\nindex file {:.1} MB", size as f64 / 1e6);
    Ok(())
}

fn date(ms: i64) -> String {
    let days = ms / 86_400_000;
    // Civil date from days since 1970, Howard Hinnant's algorithm, to avoid a
    // date crate for one line of output.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!("{y:04}-{m:02}-{d:02}")
}

/// The given assets with fresh download URLs, looked up by record name 100
/// at a time. Stored URLs expire, so anything that downloads asks again.
fn fresh_assets(client: &mut PhotosClient, ids: &[(String, String)]) -> Result<Vec<Asset>> {
    let mut out = Vec::new();
    for chunk in ids.chunks(100) {
        let names: Vec<&str> = chunk.iter().flat_map(|(master, asset)| [master.as_str(), asset.as_str()]).collect();
        let started = Instant::now();
        let r = client.lookup(&names)?;
        let assets = pair_records(r["records"].as_array().map(Vec::as_slice).unwrap_or(&[]), false);
        eprintln!("looked up {} assets in {} ms", assets.len(), started.elapsed().as_millis());
        out.extend(assets);
    }
    Ok(out)
}

/// Random (master, asset) record names matching an SQL filter on `assets a`.
fn random_ids(conn: &Connection, filter: &str, n: usize) -> Result<Vec<(String, String)>> {
    let mut ids: Vec<(String, String)> = conn
        .prepare(&format!("SELECT id, asset_record FROM assets a WHERE {filter}"))?
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?
        .collect::<Result<_, _>>()?;
    ids.shuffle(&mut rand::thread_rng());
    ids.truncate(n);
    Ok(ids)
}

fn sample(client: &mut PhotosClient, db_path: &PathBuf, kind: &str, n: usize, threads: usize, out: Option<PathBuf>, pool: &str) -> Result<()> {
    let conn = db::open(db_path)?;

    // Do stored URLs survive? Try the oldest one we have.
    if let Ok((url, fetched)) = conn.query_row(
        "SELECT url, url_fetched_at FROM resources WHERE kind = ?1 AND url IS NOT NULL ORDER BY url_fetched_at LIMIT 1",
        [kind], |r| Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?)),
    ) {
        let status = client.downloader().fetch(&url, Some("bytes=0-0"), false).map(|r| r.status().as_u16()).unwrap_or(0);
        println!("stored URL fetched {} min ago answers {status}", (now_secs() - fetched) / 60);
    }

    let ids = random_ids(&conn, &format!("EXISTS (SELECT 1 FROM resources r WHERE r.asset_id = a.id AND r.kind = '{kind}')"), n)?;
    let started = Instant::now();
    let assets = fresh_assets(client, &ids)?;
    let jobs: Vec<(String, String, i64)> = assets
        .iter()
        .filter_map(|a| a.resource(kind).and_then(|r| Some((a.id.clone(), r.url.clone()?, r.size.unwrap_or(0)))))
        .collect();
    println!("refreshed {} URLs in {:.1}s", jobs.len(), started.elapsed().as_secs_f64());
    let Some(first) = jobs.first() else { return Err(anyhow!("no {kind} renditions to sample")) };

    let downloader = client.downloader();
    let no_cookies = downloader.fetch(&first.1, None, false)?.status();
    println!("download without cookies answers {no_cookies}");
    let with_cookies = !no_cookies.is_success();
    if let Some(dir) = &out {
        std::fs::create_dir_all(dir)?;
    }

    let queue = Arc::new(Mutex::new(jobs.clone()));
    let results = Arc::new(Mutex::new(Vec::<(u128, u128, usize)>::new()));
    let wall = Instant::now();
    let workers: Vec<_> = (0..threads)
        .map(|_| {
            let (queue, results, out) = (queue.clone(), results.clone(), out.clone());
            let downloader = match pool {
                "own" => downloader.separate(false),
                "own-h1" => downloader.separate(true),
                _ => Ok(downloader.clone()),
            };
            std::thread::spawn(move || {
                let Ok(downloader) = downloader else { return };
                loop {
                    // Taken in its own statement: in a `while let` the lock
                    // guard would live through the download and the threads
                    // would take turns.
                    let job = queue.lock().unwrap().pop();
                    let Some((id, url, _)) = job else { break };
                    let t = Instant::now();
                    let Ok(mut response) = downloader.fetch(&url, None, with_cookies) else { continue };
                    let ttfb = t.elapsed().as_millis();
                    let mut body = Vec::new();
                    if response.read_to_end(&mut body).is_err() || !response.status().is_success() {
                        continue;
                    }
                    if let Some(dir) = &out {
                        let _ = std::fs::write(dir.join(format!("{}.jpg", id.replace('/', "_"))), &body);
                    }
                    results.lock().unwrap().push((ttfb, t.elapsed().as_millis(), body.len()));
                }
            })
        })
        .collect();
    for w in workers {
        let _ = w.join();
    }
    let wall = wall.elapsed().as_secs_f64();
    let mut r = results.lock().unwrap().clone();
    if r.is_empty() {
        return Err(anyhow!("every download failed"));
    }
    let bytes: usize = r.iter().map(|x| x.2).sum();
    r.sort_by_key(|x| x.1);
    let pct = |p: f64| r[((r.len() - 1) as f64 * p) as usize].1;
    let mut ttfb: Vec<u128> = r.iter().map(|x| x.0).collect();
    ttfb.sort_unstable();
    println!("{kind}: {}/{} downloaded with {threads} threads in {wall:.1}s = {:.0} files/s, {:.1} MB/s",
        r.len(), jobs.len(), r.len() as f64 / wall, bytes as f64 / 1e6 / wall);
    println!("  avg size {:.0} KB; time to first byte p50 {} ms; full file p50 {} ms, p95 {} ms",
        bytes as f64 / r.len() as f64 / 1e3, ttfb[ttfb.len() / 2], pct(0.5), pct(0.95));
    Ok(())
}

fn stream(client: &mut PhotosClient, db_path: &PathBuf, n: usize) -> Result<()> {
    let conn = db::open(db_path)?;
    let ids = random_ids(&conn, "is_video AND duration > 20", n)?;
    let assets = fresh_assets(client, &ids)?;
    let downloader = client.downloader();
    for a in assets.iter().filter(|a| a.is_video()) {
        println!("{}  {:.0}s", a.filename.as_deref().unwrap_or("?"), a.duration.unwrap_or(0.0));
        for kind in ["VidMed", "VidFull", "Original"] {
            let Some(res) = a.resource(kind) else { continue };
            let (Some(url), Some(size)) = (&res.url, res.size) else { continue };
            let mid = size / 2;
            for (label, range) in [("start", "bytes=0-65535".to_owned()), ("middle", format!("bytes={mid}-{}", mid + 65_535))] {
                let t = Instant::now();
                let response = downloader.fetch(url, Some(&range), false)?;
                let ttfb = t.elapsed().as_millis();
                let status = response.status();
                let headers = response.headers().clone();
                let got = response.bytes()?.len();
                println!("  {kind:<9} {:>6.1} MB {}x{} {:<22} {label:<6} {status} {got:>6} B  first byte {ttfb} ms  accept-ranges={} type={}",
                    size as f64 / 1e6, res.width.unwrap_or(0), res.height.unwrap_or(0), res.file_type.as_deref().unwrap_or("?"),
                    headers.get("accept-ranges").and_then(|v| v.to_str().ok()).unwrap_or("-"),
                    headers.get("content-type").and_then(|v| v.to_str().ok()).unwrap_or("-"));
            }
            // A few MB in one go: is it fast enough to play while it arrives?
            let t = Instant::now();
            let mut response = downloader.fetch(url, Some("bytes=0-8388607"), false)?;
            let mut buf = Vec::new();
            response.read_to_end(&mut buf).context("reading video")?;
            let secs = t.elapsed().as_secs_f64();
            let bitrate = size as f64 * 8.0 / a.duration.unwrap_or(1.0).max(1.0) / 1e6;
            println!("  {kind:<9} 8 MB read at {:.1} MB/s; the file plays at {bitrate:.1} Mbit/s", buf.len() as f64 / 1e6 / secs);
        }
    }
    Ok(())
}
