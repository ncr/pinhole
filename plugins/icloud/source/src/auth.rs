use std::fmt;
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc;
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};

use crate::session::Session;

/// Where sessions come from. The core asks again whenever iCloud rejects the
/// one it has.
pub trait SessionProvider: Send {
    fn session(&mut self) -> Result<Session>;
}

/// A session handed over once and never refreshed, for extra clients that
/// run beside the one that owns the provider.
pub struct FixedSession(pub Session);

impl SessionProvider for FixedSession {
    fn session(&mut self) -> Result<Session> {
        Ok(self.0.clone())
    }
}

/// An error the provider reported, with its code from auth/PROTOCOL.md.
#[derive(Debug)]
pub struct ProviderError {
    pub code: String,
    pub message: String,
}

impl fmt::Display for ProviderError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} ({})", self.message, self.code)
    }
}

impl std::error::Error for ProviderError {}

/// How long the provider may take to answer: Apple's sign-in itself can take
/// a while, but a provider that says nothing for this long is stuck, and
/// whoever asked (a sync round, a window signing in) must not wait for ever.
const ANSWER_WITHIN: Duration = Duration::from_secs(120);

/// A sign-in provider running as a child process, speaking JSON lines.
pub struct ProcessProvider {
    child: Child,
    stdin: ChildStdin,
    /// Its lines, read on a thread of their own, so an answer can be
    /// waited for no longer than ANSWER_WITHIN.
    lines: mpsc::Receiver<std::io::Result<String>>,
    next_id: u64,
    /// How it was started, to start it again: one that stopped answering
    /// was stopped, and the next call starts another.
    program: PathBuf,
    state_dir: Option<PathBuf>,
    stopped: bool,
    answer_within: Duration,
}

impl ProcessProvider {
    pub fn spawn(program: PathBuf, state_dir: Option<PathBuf>) -> Result<Self> {
        let (child, stdin, lines) = Self::start(&program, state_dir.as_ref())?;
        Ok(Self { child, stdin, lines, next_id: 1, program, state_dir, stopped: false, answer_within: ANSWER_WITHIN })
    }

    #[allow(clippy::type_complexity)]
    fn start(program: &PathBuf, state_dir: Option<&PathBuf>) -> Result<(Child, ChildStdin, mpsc::Receiver<std::io::Result<String>>)> {
        let mut command = Command::new(program);
        if let Some(dir) = state_dir {
            command.arg("--state-dir").arg(dir);
        }
        let mut child = command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .with_context(|| format!("starting sign-in provider {}", program.display()))?;
        let stdin = child.stdin.take().expect("piped stdin");
        let stdout = BufReader::new(child.stdout.take().expect("piped stdout"));
        let (tx, lines) = mpsc::channel();
        // Ends as the provider does (its stdout closes).
        std::thread::spawn(move || {
            for line in stdout.lines() {
                if tx.send(line).is_err() {
                    return;
                }
            }
        });
        Ok((child, stdin, lines))
    }

    pub fn call(&mut self, method: &str, params: Value) -> Result<Value> {
        if std::mem::take(&mut self.stopped) {
            let _ = self.child.kill();
            let _ = self.child.wait();
            (self.child, self.stdin, self.lines) = Self::start(&self.program, self.state_dir.as_ref())?;
        }
        let id = self.next_id;
        self.next_id += 1;
        let request = json!({"id": id, "method": method, "params": params});
        writeln!(self.stdin, "{request}").context("writing to sign-in provider")?;
        self.stdin.flush()?;

        let line = match self.lines.recv_timeout(self.answer_within) {
            Ok(line) => line?,
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                self.stopped = true;
                return Err(anyhow!("sign-in provider exited"));
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {
                // Stuck: it goes, and is started again when next needed.
                let _ = self.child.kill();
                self.stopped = true;
                return Err(ProviderError {
                    code: "unavailable".into(),
                    message: format!("the sign-in provider did not answer within {} s", self.answer_within.as_secs()),
                }
                .into());
            }
        };
        let reply: Value = serde_json::from_str(&line).context("sign-in provider sent something other than JSON")?;
        if reply["id"].as_u64() != Some(id) {
            return Err(anyhow!("sign-in provider answered out of order"));
        }
        if reply["ok"].as_bool() == Some(true) {
            Ok(reply["result"].clone())
        } else {
            let error = &reply["error"];
            Err(ProviderError {
                code: error["code"].as_str().unwrap_or("unavailable").to_owned(),
                message: error["message"].as_str().unwrap_or("").to_owned(),
            }
            .into())
        }
    }
}

impl SessionProvider for ProcessProvider {
    fn session(&mut self) -> Result<Session> {
        let value = self.call("session", Value::Null)?;
        serde_json::from_value(value).context("sign-in provider sent a malformed session")
    }
}

impl Drop for ProcessProvider {
    fn drop(&mut self) {
        // The provider keeps nothing in memory that needs a clean shutdown;
        // its session store is written as it goes. Wait so it never lingers
        // as a zombie.
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Where the provider keeps each account's session, one folder to an Apple
/// ID: `~/.local/state/pinhole/auth/<apple-id>`.
pub fn state_dir(apple_id: &str) -> PathBuf {
    std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/state"))
        .join("pinhole/auth")
        .join(apple_id)
}

/// The Apple ID's owner as Apple gives it, if the provider has kept it.
pub fn account_name(apple_id: &str) -> Option<String> {
    let text = std::fs::read_to_string(state_dir(apple_id).join("account.json")).ok()?;
    let account: Value = serde_json::from_str(&text).ok()?;
    account["name"].as_str().map(str::trim).filter(|n| !n.is_empty()).map(str::to_owned)
}

/// The Apple IDs signed in to on this computer, whether or not their
/// session is still good.
pub fn accounts() -> Vec<String> {
    let Some(dir) = state_dir("x").parent().map(PathBuf::from) else { return Vec::new() };
    let Ok(entries) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut accounts: Vec<String> = entries
        .flatten()
        .filter(|e| e.path().join("account.json").exists())
        .filter_map(|e| e.file_name().into_string().ok())
        .collect();
    accounts.sort();
    accounts
}

/// The sign-in provider to run: $PINHOLE_AUTH, else the one in this
/// checkout (plugins/icloud/auth) when running from core/target/release/,
/// else the one installed beside pinholed, else `pinhole-auth` on PATH.
pub fn default_provider() -> PathBuf {
    if let Some(p) = std::env::var_os("PINHOLE_AUTH") {
        return PathBuf::from(p);
    }
    if let Ok(exe) = std::env::current_exe() {
        let checkout = exe.ancestors().nth(4).map(|root| root.join("plugins/icloud/auth/pyicloud/pinhole-auth"));
        if let Some(p) = checkout.filter(|p| p.exists()) {
            return p;
        }
        // Installed beside pinholed (/usr/bin, ~/.local/bin).
        if let Some(p) = exe.parent().map(|dir| dir.join("pinhole-auth")).filter(|p| p.exists()) {
            return p;
        }
    }
    PathBuf::from("pinhole-auth")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_provider_that_stops_answering_is_given_up_on_and_started_again() {
        let dir = std::env::temp_dir().join(format!("pinhole-provider-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        // Answers anything at once, but "hang" never.
        let program = dir.join("provider");
        std::fs::write(&program, r#"#!/bin/sh
while read -r line; do
  case "$line" in
    *'"hang"'*) sleep 30 ;;
    *) id=$(printf '%s' "$line" | sed 's/.*"id":\([0-9]*\).*/\1/'); echo "{\"id\":$id,\"ok\":true,\"result\":{\"pid\":$$}}" ;;
  esac
done
"#).unwrap();
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&program, std::fs::Permissions::from_mode(0o755)).unwrap();
        let mut provider = ProcessProvider::spawn(program, None).unwrap();
        provider.answer_within = Duration::from_millis(500);
        let first = provider.call("status", Value::Null).unwrap()["pid"].clone();
        let error = provider.call("hang", Value::Null).unwrap_err();
        assert_eq!(error.downcast_ref::<ProviderError>().map(|e| e.code.as_str()), Some("unavailable"), "{error:#}");
        // The next call has another one answer.
        let again = provider.call("status", Value::Null).unwrap()["pid"].clone();
        assert_ne!(first, again);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
