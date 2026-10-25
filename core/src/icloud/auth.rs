use std::fmt;
use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};

use crate::icloud::session::Session;

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

/// A sign-in provider running as a child process, speaking JSON lines.
pub struct ProcessProvider {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    next_id: u64,
}

impl ProcessProvider {
    pub fn spawn(program: PathBuf, state_dir: Option<PathBuf>) -> Result<Self> {
        let mut command = Command::new(&program);
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
        Ok(Self { child, stdin, stdout, next_id: 1 })
    }

    pub fn call(&mut self, method: &str, params: Value) -> Result<Value> {
        let id = self.next_id;
        self.next_id += 1;
        let request = json!({"id": id, "method": method, "params": params});
        writeln!(self.stdin, "{request}").context("writing to sign-in provider")?;
        self.stdin.flush()?;

        let mut line = String::new();
        if self.stdout.read_line(&mut line)? == 0 {
            return Err(anyhow!("sign-in provider exited"));
        }
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

/// The sign-in provider to run: $PINHOLE_AUTH, else the one in this
/// checkout when running from core/target/release/, else `pinhole-auth` on
/// PATH (as the package installs it).
/// Where the provider keeps each account's session, one folder to an Apple
/// ID: `~/.local/state/pinhole/auth/<apple-id>`.
pub fn state_dir(apple_id: &str) -> PathBuf {
    std::env::var_os("XDG_STATE_HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/state"))
        .join("pinhole/auth")
        .join(apple_id)
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

pub fn default_provider() -> PathBuf {
    if let Some(p) = std::env::var_os("PINHOLE_AUTH") {
        return PathBuf::from(p);
    }
    if let Ok(exe) = std::env::current_exe() {
        let checkout = exe.ancestors().nth(4).map(|root| root.join("auth/pyicloud/pinhole-auth"));
        if let Some(p) = checkout.filter(|p| p.exists()) {
            return p;
        }
    }
    PathBuf::from("pinhole-auth")
}
