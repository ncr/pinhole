//! Sources of your own, out of process (docs/source-providers.md): a
//! plugin's `provider`, a program in its folder that speaks JSON lines on
//! its stdin and stdout. `ProcessKind` is its kind of source and
//! `ProcessSource` one source of it. The provider never writes the library:
//! it says what there is, and pinholed checks every line before it writes
//! anything. Nor does it pass bytes: its tickets name a file or a URL,
//! which pinholed reads itself.
//!
//! One process serves each source in use (the one shown), started when
//! first asked something and stopped when the source is no longer used, or
//! its plugin is turned off. Signing in, `discover` and the like each get a
//! process of their own for the request. A provider that says nothing for
//! two minutes while asked something, that sends what is not the protocol,
//! or that ends, is stopped: what it was asked fails `unavailable`, and the
//! next request starts another, at most once in ten seconds.

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::os::unix::fs::DirBuilderExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex, OnceLock, Weak};
use std::time::{Duration, Instant};

use anyhow::{anyhow, Result};
use serde_json::{json, Value};

use crate::add::{AddError, Added, Addition};
use crate::db::Index;
use crate::kinds::{Failure, Kind, SignedIn};
use crate::plugins::Provided;
use crate::source::{Asset, Body, Effect, FetchError, Health, Heard, Listing, Rendition, Source, Stored, Ticket, TokenExpired, Want, Watch};

/// What a provider may do before it is stopped or its line refused.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Limits {
    /// Saying nothing for this long while asked something.
    pub silence: Duration,
    /// Started again at most once in this long.
    pub restart_gap: Duration,
    /// The longest line, in bytes.
    pub line_max: usize,
    /// The most assets in one `put`.
    pub batch_max: usize,
}

impl Default for Limits {
    fn default() -> Limits {
        Limits { silence: Duration::from_secs(120), restart_gap: Duration::from_secs(10), line_max: 4 << 20, batch_max: 1000 }
    }
}

impl Limits {
    /// The defaults, with `PINHOLE_PROVIDER_SILENCE_SECS` for a test that
    /// cannot wait two minutes.
    pub fn from_env() -> Limits {
        let mut limits = Limits::default();
        if let Some(secs) = std::env::var("PINHOLE_PROVIDER_SILENCE_SECS").ok().and_then(|s| s.parse::<u64>().ok()) {
            limits.silence = Duration::from_secs(secs.max(1));
        }
        limits
    }
}

/// An error a provider answered with: its code (docs/source-providers.md)
/// and why. `unavailable` is also pinholed's own, for a provider stopped,
/// ended or not started.
#[derive(Debug, Clone, PartialEq)]
pub struct ProviderError {
    pub code: String,
    pub message: String,
}

impl ProviderError {
    fn new(code: &str, message: impl Into<String>) -> ProviderError {
        ProviderError { code: code.to_owned(), message: message.into() }
    }

    fn unavailable(message: impl Into<String>) -> ProviderError {
        ProviderError::new("unavailable", message)
    }
}

impl std::fmt::Display for ProviderError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} ({})", self.message, self.code)
    }
}

impl std::error::Error for ProviderError {}

/// The provider's error in a chain, if it is one.
fn provider_error(e: &anyhow::Error) -> Option<&ProviderError> {
    e.chain().find_map(|c| c.downcast_ref::<ProviderError>())
}

/// What the reader of a provider's stdout hands whoever asked.
enum Line {
    Note(Value),
    Answer(std::result::Result<Value, ProviderError>),
    /// It was stopped, or it ended.
    Gone(String),
}

/// One provider process, running.
struct Running {
    child: Arc<Mutex<Child>>,
    /// Lines to write, by a thread of its own: one that does not read its
    /// stdin holds up no one but itself.
    stdin: mpsc::Sender<String>,
    /// Who waits for which answer.
    pending: Arc<Mutex<HashMap<u64, mpsc::SyncSender<Line>>>>,
    /// When it last sent a line of the protocol.
    heard: Arc<Mutex<Instant>>,
    gone: Arc<AtomicBool>,
    generation: u64,
}

struct Slot {
    running: Option<Running>,
    /// When one was last started.
    started: Option<Instant>,
    generation: u64,
}

type Tell = Arc<dyn Fn(Heard) + Send + Sync>;

/// A provider, run as needed: started by the first request, again after it
/// ended (at most once in `restart_gap`), and stopped when dropped.
struct Provider {
    /// The plugin's id, which its lines in pinholed's log start with.
    plugin: String,
    program: PathBuf,
    state: PathBuf,
    limits: Limits,
    /// Its kind's switch: off, it is never started.
    on: Arc<AtomicBool>,
    slot: Mutex<Slot>,
    next_id: AtomicU64,
    /// The watch (`Source::watch`), asked again of each process started,
    /// and who hears what it says.
    watching: Mutex<Option<(Value, Tell, Arc<AtomicBool>)>>,
    /// Reads of its sources' files given up on, not back yet (`watched`).
    stuck: Arc<AtomicUsize>,
    weak: Weak<Provider>,
}

impl Provider {
    fn new(plugin: &str, program: &Path, state: &Path, limits: Limits, on: Arc<AtomicBool>) -> Arc<Provider> {
        Arc::new_cyclic(|weak| Provider {
            plugin: plugin.to_owned(),
            program: program.to_owned(),
            state: state.to_owned(),
            limits,
            on,
            slot: Mutex::new(Slot { running: None, started: None, generation: 0 }),
            next_id: AtomicU64::new(1),
            watching: Mutex::new(None),
            stuck: Arc::default(),
            weak: weak.clone(),
        })
    }

    /// Ask it something: `on_note` hears the notes it sends before its
    /// answer, and an error from it gives the request up (the provider
    /// goes on; what it says of this request after is not heard).
    fn call(
        &self,
        method: &str,
        params: Value,
        on_note: &mut dyn FnMut(&Value) -> Result<()>,
        silence: Option<Duration>,
    ) -> Result<Value> {
        let silence = silence.unwrap_or(self.limits.silence);
        let asked = Instant::now();
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let (tx, rx) = mpsc::sync_channel::<Line>(16);
        let (pending, heard, generation, fresh) = {
            let mut slot = self.slot.lock().unwrap();
            let fresh = self.ready(&mut slot)?;
            let running = slot.running.as_ref().expect("started");
            running.pending.lock().unwrap().insert(id, tx);
            // Gone as it was asked: whoever stopped it may have told those
            // waiting before this one was among them.
            if running.gone.load(Ordering::SeqCst) {
                running.pending.lock().unwrap().remove(&id);
                return Err(ProviderError::unavailable(format!("{} ended", self.plugin)).into());
            }
            let line = json!({"id": id, "method": method, "params": params}).to_string();
            if running.stdin.send(line + "\n").is_err() {
                return Err(ProviderError::unavailable(format!("{} ended", self.plugin)).into());
            }
            (running.pending.clone(), running.heard.clone(), running.generation, fresh)
        };
        if fresh && method != "watch" {
            self.watch_again();
        }
        let forget = || {
            pending.lock().unwrap().remove(&id);
        };
        loop {
            match rx.recv_timeout(Duration::from_millis(250)) {
                Ok(Line::Note(note)) => {
                    if let Err(e) = on_note(&note) {
                        forget();
                        return Err(e);
                    }
                }
                Ok(Line::Answer(answer)) => return answer.map_err(Into::into),
                Ok(Line::Gone(why)) => return Err(ProviderError::unavailable(why).into()),
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    // Since it last said anything, or was asked this.
                    let quiet = heard.lock().unwrap().elapsed().min(asked.elapsed());
                    if quiet >= silence {
                        let why = format!("{} said nothing for {} s while asked {method}: stopped", self.plugin, quiet.as_secs());
                        self.stop(generation, &why);
                        forget();
                        return Err(ProviderError::unavailable(why).into());
                    }
                }
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    return Err(ProviderError::unavailable(format!("{} ended", self.plugin)).into());
                }
            }
        }
    }

    /// A process to ask, started if there is none: whether it was started
    /// now. Not while its plugin is off, nor within `restart_gap` of the last.
    fn ready(&self, slot: &mut Slot) -> std::result::Result<bool, ProviderError> {
        if slot.running.as_ref().is_some_and(|r| r.gone.load(Ordering::SeqCst)) {
            if let Some(old) = slot.running.take() {
                reap(&old.child);
            }
        }
        if slot.running.is_some() {
            return Ok(false);
        }
        if !self.on.load(Ordering::SeqCst) {
            return Err(ProviderError::new("turned_off", format!("{} is off", self.plugin)));
        }
        if let Some(started) = slot.started.filter(|s| s.elapsed() < self.limits.restart_gap) {
            return Err(ProviderError::unavailable(format!(
                "{} stopped; it is started again at most once in {} s ({} s ago)",
                self.plugin,
                self.limits.restart_gap.as_secs(),
                started.elapsed().as_secs()
            )));
        }
        slot.started = Some(Instant::now());
        slot.generation += 1;
        let running = self.start(slot.generation).map_err(|e| ProviderError::unavailable(format!("{e:#}")))?;
        slot.running = Some(running);
        Ok(true)
    }

    fn start(&self, generation: u64) -> Result<Running> {
        // Its own folder, for it alone: it works in it, and keeps there
        // whatever it keeps.
        std::fs::DirBuilder::new().recursive(true).mode(0o700).create(&self.state)?;
        let mut child = Command::new(&self.program)
            .arg("--state-dir")
            .arg(&self.state)
            .current_dir(&self.state)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| anyhow!("starting {}: {e}", self.program.display()))?;
        eprintln!("pinholed: {}: started {} (pid {})", self.plugin, self.program.display(), child.id());
        let mut stdin = child.stdin.take().expect("piped stdin");
        let stdout = child.stdout.take().expect("piped stdout");
        let stderr = child.stderr.take().expect("piped stderr");
        let child = Arc::new(Mutex::new(child));
        let pending: Arc<Mutex<HashMap<u64, mpsc::SyncSender<Line>>>> = Arc::default();
        let heard = Arc::new(Mutex::new(Instant::now()));
        let gone = Arc::new(AtomicBool::new(false));
        let (to_stdin, lines) = mpsc::channel::<String>();
        std::thread::spawn(move || {
            for line in lines {
                if stdin.write_all(line.as_bytes()).and_then(|_| stdin.flush()).is_err() {
                    return;
                }
            }
        });
        let plugin = self.plugin.clone();
        std::thread::spawn(move || log_stderr(&plugin, stderr));
        let reader = Reader {
            plugin: self.plugin.clone(),
            child: child.clone(),
            pending: pending.clone(),
            heard: heard.clone(),
            gone: gone.clone(),
            line_max: self.limits.line_max,
            provider: self.weak.clone(),
        };
        std::thread::spawn(move || reader.run(stdout));
        Ok(Running { child, stdin: to_stdin, pending, heard, gone, generation })
    }

    /// Stop the process of this generation, if it is still the one: what it
    /// was asked fails.
    fn stop(&self, generation: u64, why: &str) {
        let slot = self.slot.lock().unwrap();
        if let Some(running) = slot.running.as_ref().filter(|r| r.generation == generation) {
            eprintln!("pinholed: {why}");
            end(running, why);
        }
    }

    /// Stop whatever runs now (its plugin turned off, or its source no
    /// longer used). Stopped so, not for anything it did, it may start
    /// again at once.
    fn stop_now(&self, why: &str) {
        let running = {
            let mut slot = self.slot.lock().unwrap();
            slot.started = None;
            slot.running.take()
        };
        if let Some(running) = running {
            retire(running, why);
        }
    }

    /// Ask a process just started to watch, if the one before it was.
    fn watch_again(&self) {
        let Some((params, _, stop)) = self.watching.lock().unwrap().clone() else { return };
        if stop.load(Ordering::SeqCst) {
            return;
        }
        let Some(provider) = self.weak.upgrade() else { return };
        std::thread::spawn(move || {
            if let Err(e) = provider.call("watch", params, &mut |_| Ok(()), None) {
                if provider_error(&e).is_none_or(|p| p.code != "unsupported") {
                    eprintln!("pinholed: {}: watch: {e:#}", provider.plugin);
                }
            }
        });
    }

    /// News from it, as it comes: to whoever watches.
    fn news(&self, note: &Value) {
        let Some((_, tell, stop)) = self.watching.lock().unwrap().clone() else { return };
        if stop.load(Ordering::SeqCst) {
            return;
        }
        match note["note"].as_str() {
            Some("changed") => tell(Heard::Changed),
            Some("listening") => tell(Heard::Listening(note["on"].as_bool().unwrap_or(false))),
            _ => {}
        }
    }
}

impl Drop for Provider {
    fn drop(&mut self) {
        let running = self.slot.get_mut().map_or(None, |slot| slot.running.take());
        if let Some(running) = running {
            retire(running, "no longer used");
        }
    }
}

/// Stop it kindly: what it was asked fails, its stdin is closed, and it is
/// killed if it has not ended within three seconds. Waited for in the
/// background, so whoever stops it does not wait.
fn retire(running: Running, why: &str) {
    running.gone.store(true, Ordering::SeqCst);
    for (_, waiting) in running.pending.lock().unwrap().drain() {
        let _ = waiting.try_send(Line::Gone(why.to_owned()));
    }
    let Running { child, stdin, .. } = running;
    // Its writer ends, and with it the provider's stdin.
    drop(stdin);
    std::thread::spawn(move || {
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline {
            if !matches!(child.lock().unwrap().try_wait(), Ok(None)) {
                return;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        reap(&child);
    });
}

/// Mark it gone, tell everyone waiting on it, and kill it.
fn end(running: &Running, why: &str) {
    running.gone.store(true, Ordering::SeqCst);
    let _ = running.child.lock().unwrap().kill();
    for (_, waiting) in running.pending.lock().unwrap().drain() {
        let _ = waiting.try_send(Line::Gone(why.to_owned()));
    }
}

/// Wait for it, so it never lingers as a zombie: it has been killed, or
/// has ended.
fn reap(child: &Mutex<Child>) {
    let mut child = child.lock().unwrap();
    let _ = child.kill();
    let _ = child.wait();
}

/// Reads a provider's stdout: answers and notes to whoever asked, news to
/// whoever watches; anything else stops it.
struct Reader {
    plugin: String,
    child: Arc<Mutex<Child>>,
    pending: Arc<Mutex<HashMap<u64, mpsc::SyncSender<Line>>>>,
    heard: Arc<Mutex<Instant>>,
    gone: Arc<AtomicBool>,
    line_max: usize,
    provider: Weak<Provider>,
}

impl Reader {
    fn run(self, stdout: std::process::ChildStdout) {
        let mut stdout = BufReader::with_capacity(1 << 16, stdout);
        let mut buf = Vec::new();
        let why = loop {
            match read_line(&mut stdout, self.line_max, &mut buf) {
                Err(e) => break format!("{} could not be read: {e}", self.plugin),
                Ok(None) => break self.ended(),
                Ok(Some(false)) => {
                    break format!("{} sent a line of more than {} MB: stopped", self.plugin, self.line_max >> 20)
                }
                Ok(Some(true)) => {
                    if buf.iter().all(u8::is_ascii_whitespace) {
                        continue;
                    }
                    if let Err(why) = self.line(&buf) {
                        break format!("{} {why}: stopped", self.plugin);
                    }
                }
            }
        };
        if !self.gone.swap(true, Ordering::SeqCst) {
            eprintln!("pinholed: {why}");
        }
        let _ = self.child.lock().unwrap().kill();
        for (_, waiting) in self.pending.lock().unwrap().drain() {
            let _ = waiting.send(Line::Gone(why.clone()));
        }
    }

    /// It closed its stdout: how it ended.
    fn ended(&self) -> String {
        for _ in 0..20 {
            if let Ok(Some(status)) = self.child.lock().unwrap().try_wait() {
                return format!("{} ended ({status})", self.plugin);
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        format!("{} closed its stdout", self.plugin)
    }

    /// One line: an error says how it is not the protocol.
    fn line(&self, bytes: &[u8]) -> std::result::Result<(), String> {
        let Ok(Value::Object(line)) = serde_json::from_slice::<Value>(bytes) else {
            let start = String::from_utf8_lossy(&bytes[..bytes.len().min(80)]).into_owned();
            return Err(format!("sent a line that is not a JSON object ({start:?})"));
        };
        let line = Value::Object(line);
        let note = line.get("note").map(|n| n.as_str().ok_or("sent a note that is not a string"));
        let note = note.transpose()?;
        let id = match line.get("id") {
            None => None,
            Some(id) => Some(id.as_u64().ok_or("sent an id that is not a request's")?),
        };
        let Some(id) = id else {
            // News: what the provider has to say of itself.
            if note.is_none() {
                return Err("sent a line with neither an id nor a note".into());
            }
            *self.heard.lock().unwrap() = Instant::now();
            if let Some(provider) = self.provider.upgrade() {
                provider.news(&line);
            }
            return Ok(());
        };
        let said = if note.is_some() {
            Line::Note(line)
        } else {
            match line.get("ok").and_then(Value::as_bool) {
                Some(true) => Line::Answer(Ok(line.get("result").cloned().unwrap_or(Value::Null))),
                Some(false) => Line::Answer(Err(ProviderError::new(
                    short(line["error"]["code"].as_str().filter(|c| is_code(c)).unwrap_or("failing"), 64),
                    short(line["error"]["message"].as_str().unwrap_or(""), 2000),
                ))),
                None => return Err("answered with neither \"ok\": true nor false".into()),
            }
        };
        let answered = matches!(said, Line::Answer(_));
        // Someone gave up waiting, or it was never asked: nobody hears it,
        // and it is no sign of life.
        let waiting = {
            let mut pending = self.pending.lock().unwrap();
            if answered { pending.remove(&id) } else { pending.get(&id).cloned() }
        };
        if let Some(waiting) = waiting {
            *self.heard.lock().unwrap() = Instant::now();
            let _ = waiting.send(said);
        }
        Ok(())
    }
}

/// An error code: lower case, digits and `_`.
fn is_code(code: &str) -> bool {
    (1..=64).contains(&code.len()) && code.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
}

/// At most `max` bytes of it, cut at a character.
fn short(text: &str, max: usize) -> &str {
    if text.len() <= max {
        return text;
    }
    let mut end = max;
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

/// One line into `buf`, without its newline: Some(true), or Some(false)
/// when it is longer than `max` (then not all read); None at the end.
fn read_line(reader: &mut impl BufRead, max: usize, buf: &mut Vec<u8>) -> std::io::Result<Option<bool>> {
    buf.clear();
    loop {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            return Ok(if buf.is_empty() { None } else { Some(true) });
        }
        let (part, newline) = match available.iter().position(|&b| b == b'\n') {
            Some(i) => (&available[..i], true),
            None => (available, false),
        };
        if buf.len() + part.len() > max {
            // As much as fits, which a log keeps.
            let fits = max - buf.len();
            buf.extend_from_slice(&part[..fits]);
            reader.consume(fits);
            return Ok(Some(false));
        }
        buf.extend_from_slice(part);
        let used = part.len() + newline as usize;
        reader.consume(used);
        if newline {
            return Ok(Some(true));
        }
    }
}

/// The rest of a line, up to and with its newline, passed over as it
/// comes: never kept, however long it is.
fn skip_line(reader: &mut impl BufRead) -> std::io::Result<()> {
    loop {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            return Ok(());
        }
        let (used, newline) = match available.iter().position(|&b| b == b'\n') {
            Some(i) => (i + 1, true),
            None => (available.len(), false),
        };
        reader.consume(used);
        if newline {
            return Ok(());
        }
    }
}

/// Its stderr, into pinholed's log a line at a time, each after its
/// plugin's id: at most 200 lines in ten seconds, and 2,000 bytes a line.
fn log_stderr(plugin: &str, stderr: std::process::ChildStderr) {
    log_lines(plugin, stderr, &mut std::io::stderr());
}

pub(crate) fn log_lines(plugin: &str, from: impl Read, log: &mut impl Write) {
    log_lines_telling(plugin, from, log, |_| {});
}

/// As `log_lines`, but a line that is a JSON object with an `"event"` (a
/// worker's news for its plugin) goes to `tell` instead of the log.
pub(crate) fn log_lines_telling(plugin: &str, from: impl Read, log: &mut impl Write, mut tell: impl FnMut(serde_json::Value)) {
    let mut stderr = BufReader::new(from);
    let mut buf = Vec::new();
    let (mut since, mut said, mut left_out) = (Instant::now(), 0u32, 0u32);
    loop {
        let line = match read_line(&mut stderr, 2000, &mut buf) {
            Ok(None) | Err(_) => break,
            Ok(Some(true)) => String::from_utf8_lossy(&buf).into_owned(),
            Ok(Some(false)) => {
                // The rest of a long line, left out.
                let text = String::from_utf8_lossy(&buf).into_owned();
                if skip_line(&mut stderr).is_err() {
                    break;
                }
                format!("{text}…")
            }
        };
        if line.trim_start().starts_with('{') {
            if let Ok(said) = serde_json::from_str::<serde_json::Value>(line.trim()) {
                if said.get("event").is_some_and(serde_json::Value::is_string) {
                    tell(said);
                    continue;
                }
            }
        }
        if since.elapsed() >= Duration::from_secs(10) {
            if left_out > 0 {
                let _ = writeln!(log, "pinholed: {plugin}: {left_out} lines of its log left out");
            }
            (since, said, left_out) = (Instant::now(), 0, 0);
        }
        if said < 200 {
            said += 1;
            let _ = writeln!(log, "pinholed: {plugin}: {}", line.trim_end());
        } else {
            if left_out == 0 {
                let _ = writeln!(log, "pinholed: {plugin}: more than 200 lines of its log in 10 s: the rest of them left out");
            }
            left_out += 1;
        }
    }
    if left_out > 0 {
        let _ = writeln!(log, "pinholed: {plugin}: {left_out} lines of its log left out");
    }
}

/// Strings that live as long as pinholed, for what the `Source` and
/// `Kind` traits give as `'static`: one copy of each kind and list of
/// extensions there has been.
fn intern(text: &str) -> &'static str {
    static ALL: OnceLock<Mutex<HashSet<&'static str>>> = OnceLock::new();
    let mut all = ALL.get_or_init(Default::default).lock().unwrap();
    if let Some(known) = all.get(text) {
        return known;
    }
    let leaked: &'static str = Box::leak(text.to_owned().into_boxed_str());
    all.insert(leaked);
    leaked
}

fn intern_list(list: &[String]) -> &'static [&'static str] {
    static ALL: OnceLock<Mutex<HashMap<Vec<String>, &'static [&'static str]>>> = OnceLock::new();
    let mut all = ALL.get_or_init(Default::default).lock().unwrap();
    if let Some(known) = all.get(list) {
        return known;
    }
    let leaked: &'static [&'static str] = Box::leak(list.iter().map(|e| intern(e)).collect::<Vec<_>>().into_boxed_slice());
    all.insert(list.to_vec(), leaked);
    leaked
}

/// How long a provider's file (a path ticket) may take to be found and
/// opened, and then between two reads of it. A FIFO is refused before it
/// is opened; a network mount that no longer answers holds a download for
/// this long, not for good.
const FILE_GAP: Duration = Duration::from_secs(30);

/// Reads of a source's files given up on (`FILE_GAP`) and not back yet:
/// while this many are, its files are not tried, so a mount that does not
/// answer ties up threads of its own, never every download's.
const STUCK_MAX: usize = 4;

/// The biggest original fetched by URL to make a photo's thumbnail or
/// medium from: one bigger is no photo to make one of.
const MAKE_MAX: u64 = 512 << 20;

/// The reads of a file done by a thread of its own, which may not come
/// back (a dead mount): given up on by whoever waits for them, or put down
/// by whoever read them while the thread is still out, it is counted in
/// `stuck` until the thread has ended.
struct Watched {
    /// 0 running, 1 given up on, 2 ended.
    phase: Mutex<u8>,
    stuck: Arc<AtomicUsize>,
}

impl Watched {
    fn give_up(&self) {
        let mut phase = self.phase.lock().unwrap();
        if *phase == 0 {
            *phase = 1;
            self.stuck.fetch_add(1, Ordering::SeqCst);
        }
    }

    /// Back from a call that may block: whether it was given up on
    /// meanwhile, and the thread had better end.
    fn given_up(&self) -> bool {
        *self.phase.lock().unwrap() == 1
    }
}

/// Held by the thread of a `Watched`, however it ends: it is no longer
/// counted, nor ever counted after.
struct Ending(Arc<Watched>);

impl Drop for Ending {
    fn drop(&mut self) {
        let mut phase = self.0.phase.lock().unwrap();
        if *phase == 1 {
            self.0.stuck.fetch_sub(1, Ordering::SeqCst);
        }
        *phase = 2;
    }
}

/// Whether this source's files may be tried: not while `STUCK_MAX` reads
/// of them have not come back.
fn room(what: &str, stuck: &Arc<AtomicUsize>) -> std::result::Result<(), FetchError> {
    // A body put down is counted until its thread is back from the read it
    // was on, which on a file that answers is at once: a moment for those.
    let settling = Instant::now();
    while stuck.load(Ordering::SeqCst) >= STUCK_MAX && settling.elapsed() < Duration::from_millis(100) {
        std::thread::sleep(Duration::from_millis(5));
    }
    let waiting = stuck.load(Ordering::SeqCst);
    if waiting >= STUCK_MAX {
        return Err(FetchError::Failed(anyhow!("{what}: {waiting} reads of this source's files have not come back; none tried until one does")));
    }
    Ok(())
}

/// A file opened and read by a thread of its own, `gap` at most waited for
/// its opening and for each read: whoever reads it is never held up for
/// good, however the file behaves. `open` gives the reader and its length.
fn watched<R: Read + Send + 'static>(
    what: String,
    open: impl FnOnce() -> std::result::Result<(R, u64), FetchError> + Send + 'static,
    gap: Duration,
    stuck: &Arc<AtomicUsize>,
) -> std::result::Result<Body, FetchError> {
    room(&what, stuck)?;
    let watch = Arc::new(Watched { phase: Mutex::new(0), stuck: stuck.clone() });
    let (opened_tx, opened_rx) = mpsc::sync_channel::<std::result::Result<u64, FetchError>>(1);
    let (tx, rx) = mpsc::sync_channel::<std::io::Result<Vec<u8>>>(2);
    let reading = watch.clone();
    std::thread::Builder::new()
        .name("pinholed-file".into())
        .spawn(move || {
            let _ending = Ending(reading.clone());
            let opened = open();
            if reading.given_up() {
                return;
            }
            let (mut reader, length) = match opened {
                Ok(opened) => opened,
                Err(e) => return drop(opened_tx.send(Err(e))),
            };
            if opened_tx.send(Ok(length)).is_err() {
                return;
            }
            loop {
                let mut buf = vec![0u8; 256 << 10];
                let read = reader.read(&mut buf);
                if reading.given_up() {
                    return;
                }
                match read {
                    Ok(0) => return,
                    Ok(n) => {
                        buf.truncate(n);
                        if tx.send(Ok(buf)).is_err() {
                            return;
                        }
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
                    Err(e) => return drop(tx.send(Err(e))),
                }
            }
        })
        .map_err(|e| FetchError::Failed(e.into()))?;
    match opened_rx.recv_timeout(gap) {
        Ok(Ok(length)) => Ok(Body { length: Some(length), reader: Box::new(Pumped { rx, chunk: Vec::new(), at: 0, gap, watch, what }) }),
        Ok(Err(e)) => Err(e),
        Err(mpsc::RecvTimeoutError::Timeout) => {
            watch.give_up();
            Err(FetchError::Failed(anyhow!("{what} did not answer within {} s", gap.as_secs())))
        }
        Err(mpsc::RecvTimeoutError::Disconnected) => Err(FetchError::Failed(anyhow!("{what} could not be opened"))),
    }
}

/// What the thread of a `watched` file reads, as it comes.
struct Pumped {
    rx: mpsc::Receiver<std::io::Result<Vec<u8>>>,
    chunk: Vec<u8>,
    at: usize,
    gap: Duration,
    watch: Arc<Watched>,
    what: String,
}

impl Read for Pumped {
    fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
        if self.at == self.chunk.len() {
            match self.rx.recv_timeout(self.gap) {
                Ok(Ok(chunk)) => (self.chunk, self.at) = (chunk, 0),
                Ok(Err(e)) => return Err(e),
                Err(mpsc::RecvTimeoutError::Disconnected) => return Ok(0),
                Err(mpsc::RecvTimeoutError::Timeout) => {
                    self.watch.give_up();
                    let why = format!("{} did not answer within {} s", self.what, self.gap.as_secs());
                    return Err(std::io::Error::new(std::io::ErrorKind::TimedOut, why));
                }
            }
        }
        let n = (self.chunk.len() - self.at).min(out.len());
        out[..n].copy_from_slice(&self.chunk[self.at..self.at + n]);
        self.at += n;
        Ok(n)
    }
}

impl Drop for Pumped {
    /// Put down while its thread may still be out on a read: counted until
    /// that comes back (nothing, when the thread has ended).
    fn drop(&mut self) {
        self.watch.give_up();
    }
}

/// A path ticket's file, opened: only a file (never a FIFO or a device,
/// which would hold whoever reads it), and found to be one once open.
fn open_checked(path: &Path) -> std::result::Result<File, FetchError> {
    let gone = |e: std::io::Error| match e.kind() {
        std::io::ErrorKind::NotFound => FetchError::NotFound(format!("{} is gone", path.display())),
        _ => FetchError::Failed(anyhow!("{}: {e}", path.display())),
    };
    let not_a_file = || FetchError::NotFound(format!("{} is not a file", path.display()));
    if !std::fs::metadata(path).map_err(gone)?.is_file() {
        return Err(not_a_file());
    }
    use std::os::unix::fs::OpenOptionsExt;
    let file = std::fs::OpenOptions::new().read(true).custom_flags(crate::render::O_NONBLOCK).open(path).map_err(gone)?;
    if !file.metadata().map_err(gone)?.is_file() {
        return Err(not_a_file());
    }
    Ok(file)
}

/// A file of a path ticket, or bytes `start..=end` of it, read as
/// `watched`.
fn open_path(path: PathBuf, range: Option<(u64, u64)>, gap: Duration, stuck: &Arc<AtomicUsize>) -> std::result::Result<Body, FetchError> {
    let what = path.display().to_string();
    watched(what, move || {
        let mut file = open_checked(&path)?;
        let failed = |e: std::io::Error| FetchError::Failed(anyhow!("{}: {e}", path.display()));
        let length = file.metadata().map_err(failed)?.len();
        let n = match range {
            Some((start, end)) => {
                file.seek(SeekFrom::Start(start)).map_err(failed)?;
                end.min(length.saturating_sub(1)).saturating_sub(start) + 1
            }
            None => length,
        };
        Ok((file.take(n), n))
    }, gap, stuck)
}

/// A path ticket's file to make a thumbnail or medium from, opened by a
/// thread of its own as `watched` opens one, and a photo's size read from
/// its start (render::size) there too: before a program is started on it,
/// holding a place among background work, which is given the file itself,
/// never its path again. The thread ends once it has handed the file over,
/// so it is counted in `stuck` only when it took longer than `gap`.
fn open_to_make(path: PathBuf, photo: bool, gap: Duration, stuck: &Arc<AtomicUsize>) -> std::result::Result<(File, Option<(u32, u32)>), FetchError> {
    let what = path.display().to_string();
    room(&what, stuck)?;
    let watch = Arc::new(Watched { phase: Mutex::new(0), stuck: stuck.clone() });
    let (opened_tx, opened_rx) = mpsc::sync_channel(1);
    let opening = watch.clone();
    std::thread::Builder::new()
        .name("pinholed-file".into())
        .spawn(move || {
            let _ending = Ending(opening.clone());
            let opened = open_checked(&path).map(|file| {
                let size = if photo { crate::render::size(&file) } else { None };
                (file, size)
            });
            if !opening.given_up() {
                let _ = opened_tx.send(opened);
            }
        })
        .map_err(|e| FetchError::Failed(e.into()))?;
    match opened_rx.recv_timeout(gap) {
        Ok(opened) => opened,
        Err(mpsc::RecvTimeoutError::Timeout) => {
            watch.give_up();
            Err(FetchError::Failed(anyhow!("{what} did not answer within {} s", gap.as_secs())))
        }
        Err(mpsc::RecvTimeoutError::Disconnected) => Err(FetchError::Failed(anyhow!("{what} could not be opened"))),
    }
}

/// Requests a `Loopback` serves at once: ffmpeg makes one at a time.
const LOOPBACK_MAX: usize = 8;

/// The longest head of a request to a `Loopback`: ffmpeg's are a few
/// hundred bytes.
const HEAD_MAX: u64 = 8 << 10;

/// A provider's URL on the loopback, for ffmpeg to make a video's
/// thumbnail from: it reads only what it needs, by ranges, and the
/// ticket's headers (a provider's sign-in) stay in pinholed, never on a
/// command line. Served until dropped, and only to this user's programs:
/// its URL's path (a token) is on ffmpeg's command line, which other users
/// can read.
struct Loopback {
    url: String,
    port: u16,
    stop: Arc<AtomicBool>,
    /// Whether the provider's server said its URL was stale (401, 403, 404
    /// or 410), to any request.
    expired: Arc<AtomicBool>,
}

impl Loopback {
    fn start(client: reqwest::blocking::Client, url: String, headers: serde_json::Map<String, Value>) -> std::io::Result<Loopback> {
        let listener = std::net::TcpListener::bind(("127.0.0.1", 0))?;
        let port = listener.local_addr()?.port();
        let token = crate::add::uuid();
        let (stop, expired) = (Arc::new(AtomicBool::new(false)), Arc::new(AtomicBool::new(false)));
        let (stopped, path) = (stop.clone(), format!("/{token}"));
        let passing = Arc::new(Passing { client, url, headers, path, expired: expired.clone() });
        let open = Arc::new(AtomicUsize::new(0));
        std::thread::spawn(move || {
            for conn in listener.incoming() {
                if stopped.load(Ordering::SeqCst) {
                    return;
                }
                let Ok(mut conn) = conn else { continue };
                if open.fetch_add(1, Ordering::SeqCst) >= LOOPBACK_MAX {
                    open.fetch_sub(1, Ordering::SeqCst);
                    let _ = conn.set_write_timeout(Some(Duration::from_secs(1)));
                    let _ = conn.write_all(b"HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                    continue;
                }
                let (passing, open) = (passing.clone(), open.clone());
                std::thread::spawn(move || {
                    let _ = pass_on(conn, &passing);
                    open.fetch_sub(1, Ordering::SeqCst);
                });
            }
        });
        Ok(Loopback { url: format!("http://127.0.0.1:{port}/{token}"), port, stop, expired })
    }
}

impl Drop for Loopback {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        // Wakes its listener, which then sees it is stopped.
        let _ = std::net::TcpStream::connect(("127.0.0.1", self.port));
    }
}

/// What a `Loopback` passes requests on to.
struct Passing {
    client: reqwest::blocking::Client,
    url: String,
    headers: serde_json::Map<String, Value>,
    /// Its URL's path.
    path: String,
    expired: Arc<AtomicBool>,
}

extern "C" {
    fn geteuid() -> u32;
}

/// The user whose socket the other end of a connection on the loopback
/// is, from the kernel's table of them (/proc/net/tcp): its local address
/// is this end's peer, and its remote this end's own.
fn peer_user(conn: &std::net::TcpStream) -> Option<u32> {
    let hex = |addr: std::net::SocketAddr| match addr {
        // As the kernel prints them: the address as a number in this
        // computer's byte order, the port as one.
        std::net::SocketAddr::V4(a) => Some(format!("{:08X}:{:04X}", u32::from_ne_bytes(a.ip().octets()), a.port())),
        std::net::SocketAddr::V6(_) => None,
    };
    let (theirs, ours) = (hex(conn.peer_addr().ok()?)?, hex(conn.local_addr().ok()?)?);
    let table = std::fs::read_to_string("/proc/net/tcp").ok()?;
    table.lines().skip(1).find_map(|line| {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.get(1) == Some(&theirs.as_str()) && fields.get(2) == Some(&ours.as_str()) {
            fields.get(7)?.parse().ok()
        } else {
            None
        }
    })
}

/// One request to a `Loopback`: from a program of this user's, its head no
/// longer than `HEAD_MAX`, for its path; passed on to the provider's URL
/// with its headers and the range asked, and the answer back as it comes.
fn pass_on(conn: std::net::TcpStream, passing: &Passing) -> Result<()> {
    let mut out = &conn;
    // SAFETY: a call with no arguments that cannot fail.
    if peer_user(&conn) != Some(unsafe { geteuid() }) {
        out.write_all(b"HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")?;
        return Ok(());
    }
    conn.set_read_timeout(Some(Duration::from_secs(30)))?;
    let mut reader = BufReader::new((&conn).take(HEAD_MAX));
    let mut lines = Vec::new();
    loop {
        let mut line = Vec::new();
        reader.read_until(b'\n', &mut line)?;
        if !line.ends_with(b"\n") {
            if reader.get_ref().limit() == 0 {
                out.write_all(b"HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")?;
            }
            // Else it hung up part way.
            return Ok(());
        }
        let line = String::from_utf8_lossy(&line).trim().to_owned();
        if line.is_empty() {
            break;
        }
        lines.push(line);
    }
    let first = lines.first().map(String::as_str).unwrap_or("");
    let mut parts = first.split_whitespace();
    let (method, target) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""));
    let mut range = None;
    for header in lines.iter().skip(1) {
        if let Some((name, value)) = header.split_once(':') {
            let value = value.trim();
            if name.eq_ignore_ascii_case("range") && value.starts_with("bytes=") && value[6..].chars().all(|c| c.is_ascii_digit() || c == '-') {
                range = Some(value.to_owned());
            }
        }
    }
    if target != passing.path || !matches!(method, "GET" | "HEAD") {
        out.write_all(b"HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")?;
        return Ok(());
    }
    let client = &passing.client;
    let mut request = if method == "HEAD" { client.head(&passing.url) } else { client.get(&passing.url) };
    for (name, value) in &passing.headers {
        request = request.header(name.as_str(), value.as_str().unwrap_or(""));
    }
    if let Some(range) = &range {
        request = request.header(reqwest::header::RANGE, range.as_str());
    }
    let Ok(mut response) = request.send() else {
        out.write_all(b"HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\nContent-Length: 0\r\n\r\n")?;
        return Ok(());
    };
    let status = response.status();
    if matches!(status.as_u16(), 401 | 403 | 404 | 410) {
        passing.expired.store(true, Ordering::SeqCst);
    }
    let mut head = format!("HTTP/1.1 {} {}\r\nConnection: close\r\n", status.as_u16(), status.canonical_reason().unwrap_or(""));
    for name in ["content-length", "content-range", "content-type", "accept-ranges"] {
        if let Some(value) = response.headers().get(name).and_then(|v| v.to_str().ok()) {
            head.push_str(&format!("{name}: {value}\r\n"));
        }
    }
    head.push_str("\r\n");
    out.write_all(head.as_bytes())?;
    if method == "GET" {
        std::io::copy(&mut response, &mut out)?;
    }
    Ok(())
}

/// A kind of source of a plugin's own, its provider out of process.
pub struct ProcessKind {
    provided: Provided,
    kind: &'static str,
    accepts: &'static [&'static str],
    /// `--state-dir`: `sources/<plugin id>`.
    state: PathBuf,
    limits: Limits,
    on: Arc<AtomicBool>,
    /// The provider of each source in use, by its id.
    running: Mutex<HashMap<String, Weak<Provider>>>,
    /// The one a sign-in that waits for a code is with.
    signing_in: Mutex<Option<Arc<Provider>>>,
}

impl ProcessKind {
    /// Off until its plugin's switch says otherwise (`set_on`).
    pub fn new(provided: Provided, state: &Path, limits: Limits) -> ProcessKind {
        ProcessKind {
            kind: intern(&provided.kind),
            accepts: intern_list(&provided.accepts),
            state: state.join(&provided.plugin),
            provided,
            limits,
            on: Arc::new(AtomicBool::new(false)),
            running: Mutex::new(HashMap::new()),
            signing_in: Mutex::new(None),
        }
    }

    pub fn provided(&self) -> &Provided {
        &self.provided
    }

    fn provider(&self) -> Arc<Provider> {
        Provider::new(&self.provided.plugin, &self.provided.program, &self.state, self.limits, self.on.clone())
    }

    /// The provider of a source, shared by every `ProcessSource` of it.
    fn provider_of(&self, id: &str) -> Arc<Provider> {
        let mut running = self.running.lock().unwrap();
        running.retain(|_, p| p.strong_count() > 0);
        if let Some(provider) = running.get(id).and_then(Weak::upgrade) {
            return provider;
        }
        let provider = self.provider();
        running.insert(id.to_owned(), Arc::downgrade(&provider));
        provider
    }

    /// Asked once, of a process of its own for the request.
    fn once(&self, method: &str, params: Value, silence: Option<Duration>) -> std::result::Result<Value, Failure> {
        self.provider().call(method, params, &mut |_| Ok(()), silence).map_err(failure)
    }

    /// An entry as a provider gave it, checked: of this kind (said or
    /// not), with a key.
    fn entry(&self, mut entry: Value) -> std::result::Result<Value, Failure> {
        if let Some(fields) = entry.as_object_mut() {
            fields.entry("kind").or_insert_with(|| json!(self.kind));
        }
        self.normal(&entry).ok_or_else(|| {
            Failure::new("failing", format!("{} gave no entry of {} with a key", self.provided.plugin, self.kind))
        })
    }

    /// How a sign-in went, as the provider said.
    fn signed(&self, said: Value, provider: Arc<Provider>) -> std::result::Result<SignedIn, Failure> {
        match said["state"].as_str() {
            Some("needs_code") => {
                *self.signing_in.lock().unwrap() = Some(provider);
                Ok(SignedIn::NeedsCode)
            }
            Some("signed_in") => self.entry(said["entry"].clone()).map(SignedIn::Entry),
            _ => Err(Failure::new("failing", format!("{} said neither signed_in nor needs_code", self.provided.plugin))),
        }
    }
}

/// A provider's error as a window's: its own code, but `unavailable` for
/// anything else.
fn failure(e: anyhow::Error) -> Failure {
    match provider_error(&e) {
        Some(p) => Failure::new(&p.code, p.message.clone()),
        None => Failure::new("unavailable", format!("{e:#}")),
    }
}

/// Its key: what tells its sources apart, 1 to 256 bytes of no control
/// characters.
fn key_of(entry: &Value) -> Option<&str> {
    entry["key"].as_str().filter(|k| (1..=256).contains(&k.len()) && !k.chars().any(char::is_control))
}

/// What a source is called: its entry's `name`, else its key.
fn name_of(entry: &Value) -> String {
    let named = entry["name"].as_str().map(|n| short(n.trim(), 200)).filter(|n| !n.is_empty() && !n.chars().any(char::is_control));
    named.or_else(|| key_of(entry)).unwrap_or("").to_owned()
}

impl Kind for ProcessKind {
    fn kind(&self) -> &str {
        self.kind
    }

    fn plugin(&self) -> &str {
        &self.provided.plugin
    }

    /// As it is, if it is of this kind and has a key, and is not huge.
    fn normal(&self, entry: &Value) -> Option<Value> {
        (entry.is_object() && entry["kind"] == self.kind && key_of(entry).is_some() && entry.to_string().len() <= 64 << 10)
            .then(|| entry.clone())
    }

    fn id_of(&self, entry: &Value) -> String {
        format!("{}:{}", self.kind, entry["key"].as_str().unwrap_or(""))
    }

    fn source(&self, entry: &Value) -> Result<Arc<dyn Source>> {
        let entry = self.normal(entry).ok_or_else(|| anyhow!("a {} source needs a key", self.kind))?;
        let id = self.id_of(&entry);
        Ok(Arc::new(ProcessSource {
            provider: self.provider_of(&id),
            prefix: format!("{:08x}", crate::storage::stable_hash(id.as_bytes()) as u32),
            id,
            entry,
            kind: self.kind,
            accepts: self.accepts,
            needs_sign_in: self.provided.needs_sign_in,
            rounds_every: Duration::from_secs(self.provided.rounds_every.unwrap_or(60)),
            batch_max: self.limits.batch_max,
            http: OnceLock::new(),
        }))
    }

    fn describe(&self, entry: &Value) -> Value {
        json!({"name": name_of(entry), "key": entry["key"]})
    }

    fn entry_for(&self, params: &Value) -> std::result::Result<Value, Failure> {
        let said = self.once("add_entry", params.clone(), None)?;
        self.entry(said["entry"].clone())
    }

    /// What it says, of those of its kind added; nothing when it fails or
    /// takes more than 15 s, as the window waits for it.
    fn discover(&self, added: &[Value]) -> Value {
        let added: Vec<&Value> = added.iter().filter(|e| e["kind"] == self.kind).collect();
        match self.once("discover", json!({"added": added}), Some(Duration::from_secs(15))) {
            Ok(found) => found,
            Err(f) => {
                if f.code != "unsupported" {
                    eprintln!("pinholed: {}: discover: {} ({})", self.provided.plugin, f.message, f.code);
                }
                Value::Null
            }
        }
    }

    fn needs_sign_in(&self) -> bool {
        self.provided.needs_sign_in
    }

    fn accepts(&self) -> &'static [&'static str] {
        self.accepts
    }

    fn sign_in(&self, params: &Value) -> std::result::Result<SignedIn, Failure> {
        // A sign-in started again starts afresh.
        drop(self.signing_in.lock().unwrap().take());
        let provider = self.provider();
        let said = provider.call("sign_in", params.clone(), &mut |_| Ok(()), None).map_err(failure)?;
        self.signed(said, provider)
    }

    fn submit_code(&self, params: &Value) -> std::result::Result<SignedIn, Failure> {
        let provider = self.signing_in.lock().unwrap().take().unwrap_or_else(|| self.provider());
        let said = provider.call("submit_code", params.clone(), &mut |_| Ok(()), None);
        let said = match said {
            Ok(said) => said,
            Err(e) => {
                // A wrong code may be tried again, with the same one.
                let f = failure(e);
                if f.code == "wrong_code" {
                    *self.signing_in.lock().unwrap() = Some(provider);
                }
                return Err(f);
            }
        };
        self.signed(said, provider)
    }

    fn sign_out(&self, entry: &Value) -> std::result::Result<(), Failure> {
        self.once("sign_out", json!({"entry": entry}), None).map(drop)
    }

    /// Asked of it in the background: a provider that hangs holds up no
    /// removal.
    fn removed(&self, entry: &Value) {
        if let Some(provider) = self.running.lock().unwrap().remove(&self.id_of(entry)).and_then(|p| p.upgrade()) {
            provider.stop_now("its source was removed");
        }
        let (provider, entry, plugin) = (self.provider(), entry.clone(), self.provided.plugin.clone());
        std::thread::spawn(move || {
            if let Err(e) = provider.call("removed", json!({"entry": entry}), &mut |_| Ok(()), None) {
                eprintln!("pinholed: {plugin}: removed: {e:#}");
            }
        });
    }

    fn set_on(&self, on: bool) {
        if self.on.swap(on, Ordering::SeqCst) == on || on {
            return;
        }
        let running: Vec<Arc<Provider>> = self.running.lock().unwrap().values().filter_map(Weak::upgrade).collect();
        for provider in running {
            provider.stop_now("its plugin was turned off");
        }
        drop(self.signing_in.lock().unwrap().take());
        eprintln!("pinholed: {}: turned off: its providers stopped", self.provided.plugin);
    }
}

/// One source of a provider's kind.
pub struct ProcessSource {
    provider: Arc<Provider>,
    entry: Value,
    id: String,
    kind: &'static str,
    accepts: &'static [&'static str],
    needs_sign_in: bool,
    rounds_every: Duration,
    batch_max: usize,
    /// Its keys in the library and the cache start with this (a hash of
    /// its id) and `~`: a provider's keys need only be its own, never
    /// another source's.
    prefix: String,
    http: OnceLock<reqwest::blocking::Client>,
}

/// What `put` and `removed` notes came to, beyond what the index has.
#[derive(Default)]
struct Took {
    refused: usize,
    said: Vec<String>,
}

impl Took {
    fn refuse(&mut self, why: String) {
        self.refused += 1;
        if self.said.len() < 5 {
            self.said.push(why);
        }
    }

    fn log(&self, plugin: &str, method: &str) {
        for why in &self.said {
            eprintln!("pinholed: {plugin}: {method}: {why}");
        }
        if self.refused > self.said.len() {
            eprintln!("pinholed: {plugin}: {method}: {} more refused", self.refused - self.said.len());
        }
    }
}

impl ProcessSource {
    fn params(&self) -> Value {
        json!({"entry": self.entry})
    }

    fn with(&self, fields: Value) -> Value {
        let mut params = self.params();
        if let (Some(params), Some(fields)) = (params.as_object_mut(), fields.as_object()) {
            params.extend(fields.clone());
        }
        params
    }

    /// Notes `put` (and in `changes`, `removed`) into the index, checked.
    fn take(&self, index: &mut Index, note: &Value, took: &mut Took, removals: bool) -> Result<()> {
        match note["note"].as_str() {
            Some("put") => {
                let assets = note["assets"].as_array().ok_or_else(|| ProviderError::new("failing", "a put with no assets"))?;
                if assets.len() > self.batch_max {
                    return Err(ProviderError::new("failing", format!("a put of {} assets, more than {}", assets.len(), self.batch_max)).into());
                }
                let mut good = Vec::with_capacity(assets.len());
                for asset in assets {
                    match asset_of(asset, &self.prefix) {
                        Ok(asset) => good.push(asset),
                        Err((id, why)) => {
                            took.refuse(format!("{}: {why}", id.as_deref().unwrap_or("an asset")));
                            // What it had of one it now says wrong stays,
                            // as it was: a listing does not take it away.
                            if let Some(id) = id {
                                index.seen.insert(id);
                            }
                        }
                    }
                }
                index.put(&good)
            }
            Some("removed") if removals => {
                let ids = note["ids"].as_array().ok_or_else(|| ProviderError::new("failing", "removed with no ids"))?;
                if ids.len() > self.batch_max {
                    return Err(ProviderError::new("failing", format!("{} removed at once, more than {}", ids.len(), self.batch_max)).into());
                }
                let ids: Vec<String> = ids.iter().filter_map(|id| id.as_str().filter(|id| is_id(id))).map(str::to_owned).collect();
                index.remove(&ids)
            }
            _ => Ok(()),
        }
    }

    fn listing(&self, said: &Value) -> Listing {
        Listing {
            token: said["token"].as_str().filter(|t| t.len() <= 4096).map(str::to_owned),
            summary: short(said["summary"].as_str().unwrap_or(""), 1000).to_owned(),
        }
    }

    fn http(&self) -> std::result::Result<&reqwest::blocking::Client, FetchError> {
        if let Some(client) = self.http.get() {
            return Ok(client);
        }
        let client = crate::source::download_client(crate::source::READ_GAP).build().map_err(|e| FetchError::Failed(e.into()))?;
        Ok(self.http.get_or_init(|| client))
    }

    fn get(&self, url: &str, headers: &serde_json::Map<String, Value>, range: Option<(u64, u64)>) -> std::result::Result<Body, FetchError> {
        let mut request = self.http()?.get(url);
        for (name, value) in headers {
            request = request.header(name.as_str(), value.as_str().unwrap_or(""));
        }
        if let Some((start, end)) = range {
            request = request.header(reqwest::header::RANGE, format!("bytes={start}-{end}"));
        }
        let response = request.send().map_err(|e| FetchError::Failed(e.into()))?;
        let status = response.status();
        if matches!(status.as_u16(), 401 | 403 | 404 | 410) {
            return Err(FetchError::Expired);
        }
        if !status.is_success() || (range.is_some() && status.as_u16() != 206) {
            return Err(FetchError::Failed(anyhow!("{url} answered {status}")));
        }
        Ok(Body { length: response.content_length(), reader: Box::new(response) })
    }

    /// A thumbnail or medium made from the original, as a folder's are. A
    /// stale ticket of the original's is `Expired`, so that it is prepared
    /// again, as any other.
    fn make(&self, from: &Value, edge: u32, video: bool) -> std::result::Result<Body, FetchError> {
        // Unreadable when the program making it turned the file down.
        let cannot = |e: anyhow::Error| FetchError::from(e.context("could not make an image of it"));
        let bytes = match (from["path"].as_str(), from["url"].as_str()) {
            (Some(path), _) => {
                // A file that answers, opened before a place among
                // background work is taken for it.
                let (file, size) = open_to_make(PathBuf::from(path), !video, FILE_GAP, &self.provider.stuck)?;
                // The file's alone while it is still there: a disk gone
                // from under the program makes it fail too.
                crate::render::render(&file, size, edge, video).map_err(|e| match cannot(e) {
                    FetchError::Unreadable(why) if !Path::new(path).is_file() => FetchError::Failed(anyhow!("{why}")),
                    e => e,
                })?
            }
            (None, Some(url)) if video => {
                // ffmpeg reads what it needs of it, by ranges.
                let headers = from["headers"].as_object().cloned().unwrap_or_default();
                let loopback = Loopback::start(self.http()?.clone(), url.to_owned(), headers).map_err(|e| FetchError::Failed(e.into()))?;
                match crate::render::render_video_url(&loopback.url, edge) {
                    Ok(bytes) => bytes,
                    Err(_) if loopback.expired.load(Ordering::SeqCst) => return Err(FetchError::Expired),
                    Err(e) => return Err(cannot(e)),
                }
            }
            _ => self.make_fetched(from, edge)?,
        };
        Ok(Body { length: Some(bytes.len() as u64), reader: Box::new(std::io::Cursor::new(bytes)) })
    }

    /// A photo's made from its original fetched by URL: into a part file in
    /// the cache's `making/` (on disk, where the photos are), at most
    /// `MAKE_MAX` of it, and gone once made.
    fn make_fetched(&self, from: &Value, edge: u32) -> std::result::Result<Vec<u8>, FetchError> {
        let mut body = self.open(&Ticket(from.to_string()), None)?;
        let too_big = |bytes: u64| anyhow!("its original is {} MB, more than {} MB", bytes >> 20, MAKE_MAX >> 20);
        let made = (|| -> Result<Vec<u8>> {
            if let Some(length) = body.length.filter(|l| *l > MAKE_MAX) {
                return Err(too_big(length));
            }
            let dir = crate::storage::making();
            std::fs::create_dir_all(&dir)?;
            let part = dir.join(format!("{}.part", crate::add::uuid()));
            let made = (|| -> Result<Vec<u8>> {
                let mut file = File::options().read(true).write(true).create_new(true).open(&part)?;
                let copied = std::io::copy(&mut (&mut body.reader).take(MAKE_MAX + 1), &mut file)?;
                if copied > MAKE_MAX {
                    return Err(too_big(copied));
                }
                crate::render::render(&file, crate::render::size(&file), edge, false)
            })();
            let _ = std::fs::remove_file(&part);
            made
        })();
        made.map_err(|e| FetchError::from(e.context("could not make an image of it")))
    }
}

/// An asset's id: 1 to 512 bytes, no control characters.
fn is_id(id: &str) -> bool {
    (1..=512).contains(&id.len()) && !id.chars().any(char::is_control)
}

/// A rendition's key: 1 to 256 bytes of `[A-Za-z0-9._~+/=-]`.
fn is_key(key: &str) -> bool {
    (1..=256).contains(&key.len()) && key.chars().all(|c| c.is_ascii_alphanumeric() || "._~+/=-".contains(c))
}

/// A string field: absent or null is None, else at most `max` bytes of no
/// control characters.
fn text(v: &Value, field: &str, max: usize) -> std::result::Result<Option<String>, String> {
    match &v[field] {
        Value::Null => Ok(None),
        Value::String(s) if s.len() <= max && !s.chars().any(char::is_control) => Ok(Some(s.clone())),
        _ => Err(format!("{field} is not a string of at most {max} bytes")),
    }
}

fn whole(v: &Value, field: &str, range: std::ops::RangeInclusive<i64>) -> std::result::Result<Option<i64>, String> {
    match &v[field] {
        Value::Null => Ok(None),
        n => n.as_i64().filter(|n| range.contains(n)).map(Some).ok_or_else(|| format!("{field} is not a whole number in {range:?}")),
    }
}

fn flag(v: &Value, field: &str) -> std::result::Result<bool, String> {
    match &v[field] {
        Value::Null => Ok(false),
        b => b.as_bool().ok_or_else(|| format!("{field} is not true or false")),
    }
}

/// An asset as a provider says it, checked, its keys given the source's
/// prefix: or its id, if that much is right, and what is wrong.
fn asset_of(v: &Value, prefix: &str) -> std::result::Result<Asset, (Option<String>, String)> {
    let id = v["id"].as_str().filter(|id| is_id(id)).ok_or((None, "no id of 1 to 512 bytes".to_owned()))?;
    let wrong = |why: String| (Some(id.to_owned()), why);
    let kind = match v["kind"].as_str() {
        Some("photo") => crate::source::Kind::Photo,
        Some("video") => crate::source::Kind::Video,
        Some("live") => crate::source::Kind::Live,
        _ => return Err(wrong("kind is not photo, video or live".into())),
    };
    let effect = match &v["effect"] {
        Value::Null => None,
        e => Some(e.as_str().and_then(Effect::parse).ok_or_else(|| wrong("effect is not loop, bounce or long_exposure".into()))?),
    };
    let duration = match &v["duration"] {
        Value::Null => None,
        d => Some(d.as_f64().filter(|d| d.is_finite() && *d >= 0.0).ok_or_else(|| wrong("duration is not seconds".into()))?),
    };
    let renditions = v["renditions"].as_array().map(Vec::as_slice).unwrap_or_default();
    if renditions.len() > 16 {
        return Err(wrong("more than 16 renditions".into()));
    }
    let mut seen = HashSet::new();
    let mut made = Vec::new();
    for r in renditions {
        let want = r["kind"].as_str().and_then(Want::parse).filter(|w| *w != Want::Jpeg);
        let want = want.ok_or_else(|| wrong(format!("a rendition of no kind known ({})", r["kind"])))?;
        if !seen.insert(want) {
            return Err(wrong(format!("two {} renditions", want.name())));
        }
        let key = r["key"].as_str().filter(|k| is_key(k)).ok_or_else(|| wrong(format!("the {} key is not 1 to 256 of [A-Za-z0-9._~+/=-]", want.name())))?;
        let name = text(r, "name", 1024).map_err(wrong)?.filter(|n| !n.is_empty()).ok_or_else(|| wrong(format!("the {} has no name", want.name())))?;
        let make = flag(r, "make").map_err(wrong)?;
        if make && !matches!(want, Want::Thumb | Want::Medium) {
            return Err(wrong(format!("a {} cannot be made", want.name())));
        }
        made.push(Rendition {
            want,
            name,
            // Made ones say so in their key: prepared from the original.
            key: format!("{prefix}{}~{key}", if make { "m" } else { "" }),
            width: whole(r, "width", 0..=1 << 20).map_err(wrong)?,
            height: whole(r, "height", 0..=1 << 20).map_err(wrong)?,
            size: whole(r, "size", 0..=i64::MAX).map_err(wrong)?,
            file_type: if make { Some("public.jpeg".into()) } else { text(r, "file_type", 256).map_err(wrong)? },
        });
    }
    if made.iter().any(|r| r.key.starts_with(&format!("{prefix}m~"))) && !seen.contains(&Want::Original) {
        return Err(wrong("one made needs an original to be made from".into()));
    }
    let location = match (&v["latitude"], &v["longitude"]) {
        (Value::Null, Value::Null) => None,
        (lat, lon) => Some(lat.as_f64().zip(lon.as_f64())
            .and_then(|(lat, lon)| crate::source::Location::new(lat, lon))
            .ok_or_else(|| wrong("latitude and longitude must be a pair in decimal degrees".into()))?),
    };
    Ok(Asset {
        id: id.to_owned(),
        filename: text(v, "filename", 1024).map_err(wrong)?,
        item_type: text(v, "item_type", 256).map_err(wrong)?,
        taken: whole(v, "taken", i64::MIN..=i64::MAX).map_err(wrong)?,
        added: whole(v, "added", i64::MIN..=i64::MAX).map_err(wrong)?,
        kind,
        favorite: flag(v, "favorite").map_err(wrong)?,
        duration,
        orientation: whole(v, "orientation", 1..=8).map_err(wrong)?,
        has_location: flag(v, "has_location").map_err(wrong)?,
        location,
        size: whole(v, "size", 0..=i64::MAX).map_err(wrong)?,
        effect,
        renditions: made,
        edit: None,
        subtype: None,
    })
}

/// A ticket as a provider gave it, checked.
fn ticket_of(t: &Value) -> std::result::Result<Ticket, FetchError> {
    if let Some(path) = t["path"].as_str() {
        if path.len() > 4096 || !Path::new(path).is_absolute() || path.contains('\0') {
            return Err(FetchError::Failed(anyhow!("a ticket's path is not an absolute path")));
        }
        return Ok(Ticket(json!({"path": path}).to_string()));
    }
    if let Some(url) = t["url"].as_str() {
        let parsed = reqwest::Url::parse(url).ok().filter(|u| matches!(u.scheme(), "http" | "https") && url.len() <= 8192);
        if parsed.is_none() {
            return Err(FetchError::Failed(anyhow!("a ticket's url is not an http or https URL")));
        }
        let headers = match &t["headers"] {
            Value::Null => serde_json::Map::new(),
            Value::Object(h) if h.len() <= 32 => h.clone(),
            _ => return Err(FetchError::Failed(anyhow!("a ticket's headers are not a map of at most 32"))),
        };
        for (name, value) in &headers {
            let good = reqwest::header::HeaderName::from_bytes(name.as_bytes()).is_ok()
                && value.as_str().is_some_and(|v| v.len() <= 8192 && reqwest::header::HeaderValue::from_str(v).is_ok());
            if !good {
                return Err(FetchError::Failed(anyhow!("a ticket's header {name:?} is not one")));
            }
        }
        return Ok(Ticket(json!({"url": url, "headers": headers}).to_string()));
    }
    if t["error"].is_object() {
        let code = t["error"]["code"].as_str().unwrap_or("");
        let message = short(t["error"]["message"].as_str().unwrap_or(""), 2000).to_owned();
        return Err(match code {
            "not_found" => FetchError::NotFound(message),
            _ => FetchError::Failed(ProviderError::new(if is_code(code) { code } else { "failing" }, message).into()),
        });
    }
    Err(FetchError::Failed(anyhow!("a ticket with neither path, url nor error")))
}

impl Source for ProcessSource {
    fn name(&self) -> String {
        name_of(&self.entry)
    }

    fn kind(&self) -> &'static str {
        self.kind
    }

    fn id(&self) -> String {
        self.id.clone()
    }

    fn details(&self) -> Value {
        json!({"key": self.entry["key"]})
    }

    /// Its key, made a folder's name: what is not `[A-Za-z0-9_-]` (or a
    /// dot after the first) becomes `_`, with a hash of the key after a
    /// tilde when anything did.
    fn library_name(&self) -> String {
        let key = self.entry["key"].as_str().unwrap_or("");
        let safe: String = key
            .chars()
            .enumerate()
            .map(|(i, c)| if c.is_ascii_alphanumeric() || c == '_' || c == '-' || (c == '.' && i > 0) { c } else { '_' })
            .take(100)
            .collect();
        if safe == key {
            safe
        } else {
            format!("{safe}~{:016x}", crate::storage::stable_hash(key.as_bytes()))
        }
    }

    fn rounds_every(&self) -> Duration {
        self.rounds_every
    }

    /// News of changes as they come, if it can tell: asked in the
    /// background, and again of each process started after.
    fn watch(&self, tell: Box<dyn Fn(Heard) + Send + Sync>) -> Option<Watch> {
        let stop = Arc::new(AtomicBool::new(false));
        *self.provider.watching.lock().unwrap() = Some((self.params(), Arc::from(tell), stop.clone()));
        let provider = self.provider.clone();
        let params = self.params();
        std::thread::spawn(move || {
            if let Err(e) = provider.call("watch", params, &mut |_| Ok(()), None) {
                if provider_error(&e).is_none_or(|p| p.code != "unsupported") {
                    eprintln!("pinholed: {}: watch: {e:#}", provider.plugin);
                }
            }
        });
        Some(Watch::new(stop))
    }

    fn needs_sign_in(&self) -> bool {
        self.needs_sign_in
    }

    fn health(&self, error: &anyhow::Error) -> Health {
        match provider_error(error).map(|p| p.code.as_str()) {
            Some("needs_sign_in") => Health::NeedsSignIn,
            Some("offline") => Health::Offline,
            _ => Health::Failing,
        }
    }

    /// Ready, or `unsupported`: one with nothing to get ready is.
    fn start(&self) -> Result<()> {
        match self.provider.call("start", self.params(), &mut |_| Ok(()), None) {
            Err(e) if provider_error(&e).is_some_and(|p| p.code == "unsupported") => Ok(()),
            said => said.map(drop),
        }
    }

    fn list(&self, index: &mut Index, progress: &(dyn Fn(u64, u64) + Sync)) -> Result<Listing> {
        let mut took = Took::default();
        let said = self.provider.call("list", self.params(), &mut |note| {
            if note["note"] == "progress" {
                progress(note["done"].as_u64().unwrap_or(0), note["total"].as_u64().unwrap_or(0));
                return Ok(());
            }
            self.take(index, note, &mut took, false)
        }, None);
        took.log(&self.provider.plugin, "list");
        let mut listing = self.listing(&said?);
        if took.refused > 0 {
            listing.summary = format!("{} ({} assets refused)", listing.summary, took.refused);
        }
        Ok(listing)
    }

    fn changes(&self, index: &mut Index, token: &str) -> Result<Listing> {
        let mut took = Took::default();
        let said = self.provider.call("changes", self.with(json!({"token": token})), &mut |note| self.take(index, note, &mut took, true), None);
        took.log(&self.provider.plugin, "changes");
        // One that follows no changes lists again.
        match said {
            Err(e) if provider_error(&e).is_some_and(|p| p.code == "token_expired" || p.code == "unsupported") => {
                Err(TokenExpired(format!("{e}")).into())
            }
            said => Ok(self.listing(&said?)),
        }
    }

    /// Tickets from the provider, one for each, in order. One made is
    /// prepared as its original, and made from that when opened.
    fn prepare(&self, index: &rusqlite::Connection, wanted: &[Stored]) -> Result<Vec<std::result::Result<Ticket, FetchError>>> {
        let mut asked = Vec::with_capacity(wanted.len());
        // For each wanted: whether it is made, and its edge.
        let mut making = Vec::with_capacity(wanted.len());
        for w in wanted {
            let (flag, key) = w.key.strip_prefix(&self.prefix).and_then(|k| k.split_once('~')).unwrap_or(("", &w.key));
            if flag == "m" {
                let original: Option<(String, String)> = index
                    .query_row("SELECT name, key FROM renditions WHERE asset_id = ?1 AND kind = 'original'", [&w.asset], |r| {
                        Ok((r.get(0)?, r.get(1)?))
                    })
                    .ok();
                let Some((name, okey)) = original else {
                    asked.push(None);
                    making.push(None);
                    continue;
                };
                let okey = okey.strip_prefix(&self.prefix).and_then(|k| k.split_once('~')).map_or(okey.clone(), |(_, k)| k.to_owned());
                let edge = if w.want == Want::Thumb { crate::render::THUMB_EDGE } else { crate::render::MEDIUM_EDGE };
                let video: bool = index.query_row("SELECT is_video FROM assets WHERE id = ?1", [&w.asset], |r| r.get(0)).unwrap_or(false);
                asked.push(Some(json!({"asset": w.asset, "kind": "original", "name": name, "key": okey})));
                making.push(Some((edge, video)));
            } else {
                asked.push(Some(json!({"asset": w.asset, "kind": w.want.name(), "name": w.name, "key": key})));
                making.push(None);
            }
        }
        let wanted_now: Vec<&Value> = asked.iter().flatten().collect();
        let said = if wanted_now.is_empty() {
            json!({"tickets": []})
        } else {
            self.provider.call("prepare", self.with(json!({"wanted": wanted_now})), &mut |_| Ok(()), None)?
        };
        let tickets = said["tickets"].as_array().filter(|t| t.len() == wanted_now.len()).ok_or_else(|| {
            ProviderError::new("failing", format!("prepare gave no ticket list of {} for as many asked", wanted_now.len()))
        })?;
        let mut tickets = tickets.iter();
        Ok(asked
            .iter()
            .zip(making)
            .map(|(asked, making)| {
                if asked.is_none() {
                    return Err(FetchError::NotFound("no original to make it from".into()));
                }
                let ticket = ticket_of(tickets.next().expect("as many"))?;
                Ok(match making {
                    Some((edge, video)) => {
                        let from: Value = serde_json::from_str(&ticket.0).expect("a ticket of ours");
                        Ticket(json!({"make": edge, "video": video, "from": from}).to_string())
                    }
                    None => ticket,
                })
            })
            .collect())
    }

    fn open(&self, ticket: &Ticket, range: Option<(u64, u64)>) -> std::result::Result<Body, FetchError> {
        let ticket: Value = serde_json::from_str(&ticket.0).map_err(|_| FetchError::Failed(anyhow!("not a ticket of a provider")))?;
        if let Some(edge) = ticket["make"].as_u64() {
            return self.make(&ticket["from"], edge as u32, ticket["video"].as_bool().unwrap_or(false));
        }
        if let Some(url) = ticket["url"].as_str() {
            return self.get(url, ticket["headers"].as_object().unwrap_or(&serde_json::Map::new()), range);
        }
        open_path(PathBuf::from(ticket["path"].as_str().unwrap_or_default()), range, FILE_GAP, &self.provider.stuck)
    }

    fn accepts(&self) -> &'static [&'static str] {
        self.accepts
    }

    fn add(&self, addition: &Addition, sent: &(dyn Fn(u64, u64) + Sync)) -> Result<Added> {
        let params = self.with(json!({"file": addition.file, "motion": addition.motion, "files": addition.files().collect::<Vec<_>>()}));
        let said = self.provider.call("add", params, &mut |note| {
            if note["note"] == "sent" {
                sent(note["sent"].as_u64().unwrap_or(0), note["bytes"].as_u64().unwrap_or(0));
            }
            Ok(())
        }, None);
        match said {
            Ok(said) => Ok(Added {
                id: said["id"].as_str().filter(|id| is_id(id)).map(str::to_owned),
                duplicate: said["duplicate"].as_bool().unwrap_or(false),
                note: said["note"].as_str().map(|n| short(n, 500).to_owned()),
            }),
            Err(e) => {
                let code = provider_error(&e).map(|p| p.code.as_str());
                let known = ["unsupported", "too_large", "over_quota", "protected", "failed"];
                match code.and_then(|c| known.iter().find(|k| **k == c)) {
                    Some(code) => Err(AddError::new(code, provider_error(&e).map(|p| p.message.clone()).unwrap_or_default()).into()),
                    None => Err(e),
                }
            }
        }
    }

    fn set_favorite(&self, _index: &rusqlite::Connection, ids: &[String], favorite: bool) -> Result<()> {
        match self.provider.call("set_favorite", self.with(json!({"ids": ids, "favorite": favorite})), &mut |_| Ok(()), None) {
            Err(e) if provider_error(&e).is_some_and(|p| p.code == "unsupported") => {
                Err(crate::source::Unsupported(format!("{} keeps no favourites", self.name())).into())
            }
            said => said.map(drop),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lines_longer_than_the_limit_are_told_apart() {
        let mut reader = BufReader::with_capacity(4, &b"short\n\nexactly10!\nmuch longer than ten\nend"[..]);
        let mut buf = Vec::new();
        assert_eq!(read_line(&mut reader, 10, &mut buf).unwrap(), Some(true));
        assert_eq!(buf, b"short");
        assert_eq!(read_line(&mut reader, 10, &mut buf).unwrap(), Some(true));
        assert_eq!(buf, b"");
        assert_eq!(read_line(&mut reader, 10, &mut buf).unwrap(), Some(true));
        assert_eq!(buf, b"exactly10!");
        assert_eq!(read_line(&mut reader, 10, &mut buf).unwrap(), Some(false));
    }

    #[test]
    fn a_long_line_of_its_log_is_cut_and_the_rest_of_it_passed_over() {
        let mut text = b"short\n".to_vec();
        text.extend(std::iter::repeat_n(b'y', 5000));
        text.extend_from_slice(b"\nnext\n");
        let mut log = Vec::new();
        log_lines("p", BufReader::with_capacity(64, &text[..]), &mut log);
        let log = String::from_utf8(log).unwrap();
        let lines: Vec<&str> = log.lines().collect();
        assert_eq!(lines.len(), 3, "{log}");
        assert_eq!(lines[0], "pinholed: p: short");
        assert_eq!(lines[1], format!("pinholed: p: {}…", "y".repeat(2000)));
        assert_eq!(lines[2], "pinholed: p: next");
        // The rest of a line, passed over in pieces, never kept.
        let mut reader = BufReader::with_capacity(8, &b"0123456789abcdef\nafter"[..]);
        skip_line(&mut reader).unwrap();
        let mut rest = String::new();
        reader.read_to_string(&mut rest).unwrap();
        assert_eq!(rest, "after");
    }

    /// A file that answers when told to: its reads wait until then.
    struct Hanging(std::os::unix::net::UnixStream);

    impl Read for Hanging {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            self.0.read(buf)
        }
    }

    #[test]
    fn a_file_that_does_not_answer_holds_no_one_up_and_a_fifo_is_not_opened() {
        let gap = Duration::from_millis(300);
        let stuck: Arc<AtomicUsize> = Arc::default();
        let dir = std::env::temp_dir().join(format!("pinhole-watched-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();

        // A FIFO is no file: refused at once, never opened.
        let fifo = dir.join("fifo");
        assert!(Command::new("mkfifo").arg(&fifo).status().unwrap().success());
        let started = Instant::now();
        assert!(matches!(open_path(fifo.clone(), None, gap, &stuck), Err(FetchError::NotFound(_))));
        assert!(matches!(open_to_make(fifo.clone(), true, gap, &stuck), Err(FetchError::NotFound(_))));
        assert!(started.elapsed() < gap);
        assert!(matches!(open_path(dir.join("gone"), None, gap, &stuck), Err(FetchError::NotFound(_))));

        // A file, whole or a range of it.
        let file = dir.join("a.bin");
        let bytes: Vec<u8> = (0..1_000_000u32).map(|i| (i % 251) as u8).collect();
        std::fs::write(&file, &bytes).unwrap();
        let mut body = open_path(file.clone(), None, gap, &stuck).unwrap();
        assert_eq!(body.length, Some(1_000_000));
        let mut read = Vec::new();
        body.reader.read_to_end(&mut read).unwrap();
        assert_eq!(read, bytes);
        let mut body = open_path(file.clone(), Some((10, 19)), gap, &stuck).unwrap();
        let mut read = Vec::new();
        body.reader.read_to_end(&mut read).unwrap();
        assert_eq!((body.length, &read[..]), (Some(10), &bytes[10..20]));

        // One whose opening never comes back: given up on after the gap.
        let mut answers = Vec::new();
        for _ in 0..STUCK_MAX {
            let (ours, theirs) = std::os::unix::net::UnixStream::pair().unwrap();
            let started = Instant::now();
            let opened = watched("a dead mount".into(), move || {
                let mut hanging = Hanging(theirs);
                let mut first = [0u8; 1];
                hanging.read_exact(&mut first).map_err(anyhow::Error::from)?;
                Ok((hanging, 100))
            }, gap, &stuck);
            let Err(FetchError::Failed(e)) = opened else { panic!("opened") };
            assert!(e.to_string().contains("did not answer within"), "{e}");
            assert!(started.elapsed() < gap * 3);
            answers.push(ours);
        }
        assert_eq!(stuck.load(Ordering::SeqCst), STUCK_MAX);
        // So many not back: the next is not tried at all.
        let started = Instant::now();
        let Err(FetchError::Failed(e)) = open_path(file.clone(), None, gap, &stuck) else { panic!("tried") };
        assert!(e.to_string().contains("have not come back"), "{e}");
        assert!(started.elapsed() < Duration::from_millis(300));
        // One comes back: tried again.
        answers.pop().unwrap().write_all(b"x").unwrap();
        let back = Instant::now();
        while stuck.load(Ordering::SeqCst) == STUCK_MAX && back.elapsed() < Duration::from_secs(5) {
            std::thread::sleep(Duration::from_millis(10));
        }
        assert_eq!(stuck.load(Ordering::SeqCst), STUCK_MAX - 1);
        assert!(open_path(file.clone(), None, gap, &stuck).is_ok());

        // One that stalls part way: its reader fails after the gap.
        let (mut ours, theirs) = std::os::unix::net::UnixStream::pair().unwrap();
        ours.write_all(b"some").unwrap();
        let mut body = watched("a mount gone".into(), move || Ok((Hanging(theirs), 100)), gap, &stuck).unwrap();
        let mut some = [0u8; 4];
        body.reader.read_exact(&mut some).unwrap();
        let started = Instant::now();
        let e = body.reader.read(&mut some).unwrap_err();
        assert_eq!(e.kind(), std::io::ErrorKind::TimedOut);
        assert!(started.elapsed() >= gap && started.elapsed() < gap * 3);
        assert_eq!(stuck.load(Ordering::SeqCst), STUCK_MAX);
        drop(answers);
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// Reads given up on just as they come back, by the thousand: every
    /// one counted while it is out is no longer once it is back, however
    /// the two cross.
    #[test]
    fn a_read_given_up_on_as_it_comes_back_is_counted_out() {
        let left: Vec<usize> = std::thread::scope(|scope| {
            let racing: Vec<_> = (0..8)
                .map(|_| {
                    scope.spawn(|| {
                        let stuck: Arc<AtomicUsize> = Arc::default();
                        let gap = Duration::from_millis(1);
                        let started = Instant::now();
                        while started.elapsed() < Duration::from_secs(4) {
                            if stuck.load(Ordering::SeqCst) >= STUCK_MAX {
                                std::thread::sleep(Duration::from_millis(20));
                                continue;
                            }
                            // Opened about when it is given up on.
                            let _ = watched("race".into(), move || {
                                std::thread::sleep(gap);
                                Ok((std::io::empty(), 0))
                            }, gap, &stuck);
                        }
                        let back = Instant::now();
                        while stuck.load(Ordering::SeqCst) > 0 && back.elapsed() < Duration::from_secs(5) {
                            std::thread::sleep(Duration::from_millis(10));
                        }
                        stuck.load(Ordering::SeqCst)
                    })
                })
                .collect();
            racing.into_iter().map(|r| r.join().unwrap()).collect()
        });
        assert!(left.iter().all(|l| *l == 0), "counted though back: {left:?}");
    }

    /// A file opened to make an image of, by the thousand: none is ever
    /// counted as a read not back, so many at once never keep the next
    /// from being tried.
    #[test]
    fn a_file_opened_to_make_from_is_never_counted_as_stuck() {
        let dir = std::env::temp_dir().join(format!("pinhole-to-make-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let photo = dir.join("a.jpg");
        image::RgbImage::from_pixel(64, 48, image::Rgb([200, 40, 40])).save(&photo).unwrap();
        let stuck: Arc<AtomicUsize> = Arc::default();
        let done = AtomicBool::new(false);
        let most = std::thread::scope(|scope| {
            let watching = scope.spawn(|| {
                let mut most = 0;
                while !done.load(Ordering::SeqCst) {
                    most = most.max(stuck.load(Ordering::SeqCst));
                }
                most
            });
            let opening: Vec<_> = (0..4)
                .map(|_| {
                    scope.spawn(|| {
                        for _ in 0..500 {
                            let (file, size) = open_to_make(photo.clone(), true, Duration::from_secs(30), &stuck).unwrap();
                            assert_eq!(size, Some((64, 48)));
                            assert!(file.metadata().unwrap().is_file());
                        }
                    })
                })
                .collect();
            opening.into_iter().for_each(|o| o.join().unwrap());
            done.store(true, Ordering::SeqCst);
            watching.join().unwrap()
        });
        assert_eq!(most, 0);
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// A body put down while its thread still reads: counted until that
    /// read comes back.
    #[test]
    fn a_body_put_down_while_its_read_is_out_is_counted_until_it_is_back() {
        let stuck: Arc<AtomicUsize> = Arc::default();
        let (mut ours, theirs) = std::os::unix::net::UnixStream::pair().unwrap();
        ours.write_all(b"some").unwrap();
        let mut body = watched("a mount gone".into(), move || Ok((Hanging(theirs), 100)), Duration::from_secs(30), &stuck).unwrap();
        let mut some = [0u8; 4];
        body.reader.read_exact(&mut some).unwrap();
        // Its thread now waits on the next read.
        std::thread::sleep(Duration::from_millis(100));
        drop(body);
        assert_eq!(stuck.load(Ordering::SeqCst), 1);
        ours.write_all(b"more").unwrap();
        let back = Instant::now();
        while stuck.load(Ordering::SeqCst) > 0 && back.elapsed() < Duration::from_secs(5) {
            std::thread::sleep(Duration::from_millis(10));
        }
        assert_eq!(stuck.load(Ordering::SeqCst), 0);
    }

    /// A request to the loopback: its head, and what came back.
    fn ask_loopback(port: u16, head: &[u8]) -> String {
        let mut conn = std::net::TcpStream::connect(("127.0.0.1", port)).unwrap();
        conn.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let _ = conn.write_all(head);
        let mut out = Vec::new();
        let _ = conn.read_to_end(&mut out);
        String::from_utf8_lossy(&out).into_owned()
    }

    /// A server on 127.0.0.1 that answers anything with `body`, to whoever
    /// sends `x-secret: s3cret`.
    fn upstream(body: &'static str) -> u16 {
        let up = std::net::TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = up.local_addr().unwrap().port();
        std::thread::spawn(move || {
            for conn in up.incoming() {
                let Ok(mut conn) = conn else { continue };
                std::thread::spawn(move || {
                    let mut head = Vec::new();
                    let mut byte = [0u8; 1];
                    while !head.ends_with(b"\r\n\r\n") && conn.read(&mut byte).unwrap_or(0) == 1 {
                        head.push(byte[0]);
                    }
                    let ok = String::from_utf8_lossy(&head).to_ascii_lowercase().contains("x-secret: s3cret");
                    let (status, body) = if ok { (200, body) } else { (403, "") };
                    let _ = conn.write_all(format!("HTTP/1.1 {status} X\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len()).as_bytes());
                });
            }
        });
        port
    }

    #[test]
    fn whose_end_of_a_connection_on_the_loopback_is_found() {
        let listener = std::net::TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let theirs = std::net::TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (ours, _) = listener.accept().unwrap();
        assert_eq!(peer_user(&ours), Some(unsafe { geteuid() }));
        drop(theirs);
    }

    #[test]
    fn the_loopback_passes_on_its_one_url_and_takes_no_long_head_nor_many_at_once() {
        let up = upstream("SECRET-BYTES");
        let client = crate::source::download_client(Duration::from_secs(5)).build().unwrap();
        let mut headers = serde_json::Map::new();
        headers.insert("x-secret".into(), json!("s3cret"));
        let lb = Loopback::start(client, format!("http://127.0.0.1:{up}/v.mp4"), headers).unwrap();
        let token = lb.url.rsplit_once('/').unwrap().1.to_owned();
        let answer = ask_loopback(lb.port, format!("GET /{token} HTTP/1.1\r\nHost: x\r\n\r\n").as_bytes());
        assert!(answer.starts_with("HTTP/1.1 200") && answer.ends_with("SECRET-BYTES"), "{answer}");
        // Not its URL: nothing passed on.
        let answer = ask_loopback(lb.port, b"GET /http://example.com/ HTTP/1.1\r\n\r\n");
        assert!(answer.starts_with("HTTP/1.1 404") && !answer.contains("SECRET"), "{answer}");
        // A head longer than any ffmpeg sends: refused at once, never read
        // to its end.
        let started = Instant::now();
        let mut long = format!("GET /{token} HTTP/1.1\r\n").into_bytes();
        long.extend(std::iter::repeat_n(b'a', 64 << 10));
        let answer = ask_loopback(lb.port, &long);
        assert!(answer.starts_with("HTTP/1.1 431"), "{answer}");
        assert!(started.elapsed() < Duration::from_secs(2), "{:?}", started.elapsed());
        // Many at once, saying nothing: past the most, closed at once.
        let quiet: Vec<std::net::TcpStream> =
            (0..LOOPBACK_MAX).map(|_| std::net::TcpStream::connect(("127.0.0.1", lb.port)).unwrap()).collect();
        std::thread::sleep(Duration::from_millis(200));
        let started = Instant::now();
        let answer = ask_loopback(lb.port, format!("GET /{token} HTTP/1.1\r\n\r\n").as_bytes());
        assert!(answer.starts_with("HTTP/1.1 503"), "{answer}");
        assert!(started.elapsed() < Duration::from_secs(2), "{:?}", started.elapsed());
        drop(quiet);
        let back = Instant::now();
        let answer = loop {
            let answer = ask_loopback(lb.port, format!("GET /{token} HTTP/1.1\r\n\r\n").as_bytes());
            if !answer.starts_with("HTTP/1.1 503") || back.elapsed() > Duration::from_secs(5) {
                break answer;
            }
            std::thread::sleep(Duration::from_millis(50));
        };
        assert!(answer.ends_with("SECRET-BYTES"), "{answer}");
        // Gone with it.
        let port = lb.port;
        drop(lb);
        std::thread::sleep(Duration::from_millis(200));
        assert!(std::net::TcpStream::connect(("127.0.0.1", port)).is_err());
    }

    #[test]
    fn a_download_that_stalls_fails_after_the_read_gap_and_a_slow_one_does_not() {
        use std::net::TcpListener;
        let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            for (i, conn) in listener.incoming().enumerate() {
                let mut conn = conn.unwrap();
                std::thread::spawn(move || {
                    let mut head = [0u8; 1024];
                    let _ = conn.read(&mut head);
                    let _ = conn.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n\r\n");
                    // The first stalls after a byte; the second sends a
                    // byte every half a gap, slower than a gap in all.
                    for _ in 0..10 {
                        let _ = conn.write_all(b"x");
                        std::thread::sleep(Duration::from_millis(if i == 0 { 5000 } else { 250 }));
                    }
                });
            }
        });
        let client = crate::source::download_client(Duration::from_millis(500)).build().unwrap();
        let url = format!("http://127.0.0.1:{port}/");
        let mut stalled = client.get(&url).send().unwrap();
        let started = Instant::now();
        let mut all = Vec::new();
        assert!(stalled.read_to_end(&mut all).is_err());
        assert!(started.elapsed() < Duration::from_secs(3), "{:?}", started.elapsed());
        let mut slow = client.get(&url).send().unwrap();
        let mut all = Vec::new();
        slow.read_to_end(&mut all).unwrap();
        assert_eq!(all, b"xxxxxxxxxx");
    }

    #[test]
    fn assets_are_checked_before_anything_is_written() {
        let good = json!({"id": "a/1.jpg", "kind": "live", "taken": 1, "favorite": true, "effect": "bounce", "orientation": 6,
                          "renditions": [{"kind": "thumb", "name": "a/1.jpg", "key": "k1", "make": true},
                                         {"kind": "original", "name": "a/1.jpg", "key": "k+/=~.-_1", "size": 10, "file_type": "public.jpeg"},
                                         {"kind": "live", "name": "a/1.mov", "key": "m1"}],
                          "something_new": [1, 2]});
        let asset = asset_of(&good, "0000abcd").unwrap();
        assert_eq!(asset.id, "a/1.jpg");
        assert_eq!((asset.kind, asset.effect, asset.favorite, asset.orientation), (crate::source::Kind::Live, Some(Effect::Bounce), true, Some(6)));
        assert_eq!(asset.rendition(Want::Thumb).unwrap().key, "0000abcdm~k1");
        assert_eq!(asset.rendition(Want::Thumb).unwrap().file_type.as_deref(), Some("public.jpeg"));
        assert_eq!(asset.rendition(Want::Original).unwrap().key, "0000abcd~k+/=~.-_1");

        let wrong = |change: Value| {
            let mut asset = good.clone();
            for (k, v) in change.as_object().unwrap() {
                asset[k] = v.clone();
            }
            asset_of(&asset, "0000abcd").unwrap_err()
        };
        let rendition = |r: Value| json!({"renditions": [r]});
        assert_eq!(asset_of(&json!({"kind": "photo"}), "p").unwrap_err().0, None);
        assert_eq!(asset_of(&json!({"id": "", "kind": "photo"}), "p").unwrap_err().0, None);
        assert_eq!(asset_of(&json!({"id": "x".repeat(513), "kind": "photo"}), "p").unwrap_err().0, None);
        assert_eq!(asset_of(&json!({"id": "a\nb", "kind": "photo"}), "p").unwrap_err().0, None);
        for change in [
            json!({"kind": "gif"}),
            json!({"effect": "spin"}),
            json!({"orientation": 9}),
            json!({"taken": "yesterday"}),
            json!({"favorite": "yes"}),
            json!({"duration": -1}),
            json!({"filename": "a\u{0}b"}),
            json!({"renditions": [{"kind": "thumb", "name": "t", "key": "a"}, {"kind": "thumb", "name": "t", "key": "b"}]}),
            rendition(json!({"kind": "jpeg", "name": "t", "key": "a"})),
            rendition(json!({"kind": "poster", "name": "t", "key": "a"})),
            rendition(json!({"kind": "thumb", "name": "t", "key": "a b"})),
            rendition(json!({"kind": "thumb", "name": "t", "key": "../../x\u{0}"})),
            rendition(json!({"kind": "thumb", "name": "t", "key": "k".repeat(257)})),
            rendition(json!({"kind": "thumb", "name": "t"})),
            rendition(json!({"kind": "thumb", "key": "a"})),
            rendition(json!({"kind": "thumb", "name": "t", "key": "a", "make": true})),
            rendition(json!({"kind": "full", "name": "t", "key": "a", "make": true})),
            rendition(json!({"kind": "thumb", "name": "t", "key": "a", "width": -2})),
            json!({"renditions": (0..17).map(|i| json!({"kind": "thumb", "name": "t", "key": i.to_string()})).collect::<Vec<_>>()}),
        ] {
            let (id, why) = wrong(change.clone());
            assert_eq!(id.as_deref(), Some("a/1.jpg"), "{change}: {why}");
        }
    }

    #[test]
    fn tickets_are_a_path_a_url_or_an_error() {
        assert!(ticket_of(&json!({"path": "/photos/a.jpg"})).is_ok());
        assert!(ticket_of(&json!({"url": "https://nas.local/a.jpg", "headers": {"Authorization": "Bearer x"}})).is_ok());
        for bad in [
            json!({"path": "photos/a.jpg"}),
            json!({"url": "file:///etc/passwd"}),
            json!({"url": "https://x/a", "headers": {"Bad Header": "x"}}),
            json!({"url": "https://x/a", "headers": {"X": "a\nb"}}),
            json!({"url": "https://x/a", "headers": ["x"]}),
            json!({"what": 1}),
        ] {
            assert!(matches!(ticket_of(&bad), Err(FetchError::Failed(_))), "{bad}");
        }
        assert!(matches!(ticket_of(&json!({"error": {"code": "not_found", "message": "gone"}})), Err(FetchError::NotFound(_))));
        let Err(FetchError::Failed(e)) = ticket_of(&json!({"error": {"code": "needs_sign_in", "message": "x"}})) else { panic!() };
        assert_eq!(provider_error(&e).unwrap().code, "needs_sign_in");
    }
}
