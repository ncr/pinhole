//! iCloud as a kind of source (kinds.rs), `pinhole.icloud`'s: an entry is
//! an Apple ID (`{"kind": "icloud", "account": …}`), and the Apple IDs
//! signed in to here are sources without the settings naming them. Signing
//! in and out goes through the sign-in provider (`auth`,
//! auth/PROTOCOL.md), one provider process to an Apple ID while it waits
//! for the two-factor code.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use anyhow::Result;
use serde_json::{json, Value};

use super::auth::{self, ProcessProvider, ProviderError};
use crate::kinds::{Failure, Kind, SignedIn};
use crate::source::Source;

pub struct IcloudKind {
    /// The sign-in provider to run.
    auth: PathBuf,
    /// For a test: the Apple IDs taken as signed in to, in place of those
    /// with a session here.
    accounts: Option<Vec<String>>,
    /// Sign-ins waiting for their two-factor code, by Apple ID: the code
    /// has to reach the provider that started the sign-in.
    signing_in: Mutex<HashMap<String, ProcessProvider>>,
}

impl IcloudKind {
    pub fn new(auth: PathBuf) -> Self {
        IcloudKind { auth, accounts: None, signing_in: Mutex::new(HashMap::new()) }
    }

    /// With these Apple IDs taken as the ones signed in to (a test's).
    pub fn with_accounts(auth: PathBuf, accounts: Vec<String>) -> Self {
        IcloudKind { accounts: Some(accounts), ..IcloudKind::new(auth) }
    }

    fn accounts(&self) -> Vec<String> {
        self.accounts.clone().unwrap_or_else(auth::accounts)
    }

    fn waiting(&self) -> std::sync::MutexGuard<'_, HashMap<String, ProcessProvider>> {
        self.signing_in.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn signed_in(apple_id: &str) -> SignedIn {
        SignedIn::Entry(json!({"kind": "icloud", "account": apple_id}))
    }

    /// Sign in with the password, or without one by taking over
    /// icloudpd's session for the Apple ID.
    fn start_sign_in(&self, apple_id: &str, password: Option<&str>) -> Result<SignedIn, Failure> {
        let state_dir = auth::state_dir(apple_id);
        let known = state_dir.join("account.json").exists();
        // A sign-in that fails leaves no trace of an Apple ID that was not
        // signed in to before.
        let forget = |e: Failure| {
            if !known {
                let _ = std::fs::remove_dir_all(&state_dir);
            }
            e
        };
        self.waiting().remove(apple_id);
        let mut provider = ProcessProvider::spawn(self.auth.clone(), Some(state_dir.clone())).map_err(|e| forget(failure(&e)))?;
        match password {
            None => {
                provider.call("import_icloudpd", json!({"apple_id": apple_id})).map_err(|e| forget(failure(&e)))?;
                let status = provider.call("status", Value::Null).map_err(|e| forget(failure(&e)))?;
                if status["state"] != "signed_in" {
                    return Err(forget(Failure::new("needs_sign_in", format!("icloudpd's session for {apple_id} is no longer valid"))));
                }
            }
            Some(password) => {
                let state = provider
                    .call("sign_in", json!({"apple_id": apple_id, "password": password}))
                    .map_err(|e| forget(failure(&e)))?;
                if state["state"] == "needs_code" {
                    self.waiting().insert(apple_id.to_owned(), provider);
                    return Ok(SignedIn::NeedsCode);
                }
            }
        }
        Ok(Self::signed_in(apple_id))
    }

    fn finish_sign_in(&self, apple_id: &str, code: &str) -> Result<SignedIn, Failure> {
        let mut provider = self
            .waiting()
            .remove(apple_id)
            .ok_or_else(|| Failure::bad_request("no sign-in is waiting for a code for that Apple ID"))?;
        if let Err(e) = provider.call("submit_code", json!({"code": code})) {
            let f = failure(&e);
            // A wrong code can be tried again.
            if f.code == "wrong_code" {
                self.waiting().insert(apple_id.to_owned(), provider);
            }
            return Err(f);
        }
        Ok(Self::signed_in(apple_id))
    }
}

/// A failed sign-in step: the provider's own code passes through.
fn failure(e: &anyhow::Error) -> Failure {
    match e.downcast_ref::<ProviderError>() {
        Some(p) => Failure::new(&p.code, p.message.clone()),
        None => Failure::new("unavailable", format!("{e:#}")),
    }
}

/// Each step and how it ended, in the log: what went wrong a first time
/// is otherwise lost. Never the password or the code.
fn logged(method: &str, apple_id: &str, how: &str, result: Result<SignedIn, Failure>) -> Result<SignedIn, Failure> {
    match &result {
        Ok(SignedIn::Entry(_)) => eprintln!("pinholed: {method} {apple_id} with {how}: signed_in"),
        Ok(SignedIn::NeedsCode) => eprintln!("pinholed: {method} {apple_id} with {how}: needs_code"),
        Err(f) => eprintln!("pinholed: {method} {apple_id} with {how}: {}: {}", f.code, f.message),
    }
    result
}

impl Kind for IcloudKind {
    fn kind(&self) -> &str {
        "icloud"
    }

    fn plugin(&self) -> &str {
        "pinhole.icloud"
    }

    /// One naming no Apple ID is the only one signed in to.
    fn normal(&self, entry: &Value) -> Option<Value> {
        let account = match entry["account"].as_str() {
            Some(account) => account.to_owned(),
            None => {
                let accounts = self.accounts();
                (accounts.len() == 1).then(|| accounts[0].clone())?
            }
        };
        Some(json!({"kind": "icloud", "account": account}))
    }

    fn id_of(&self, entry: &Value) -> String {
        format!("icloud:{}", entry["account"].as_str().unwrap_or(""))
    }

    fn implicit(&self) -> Vec<Value> {
        self.accounts().iter().map(|a| json!({"kind": "icloud", "account": a})).collect()
    }

    fn implicit_only(&self) -> bool {
        true
    }

    fn source(&self, entry: &Value) -> Result<Arc<dyn Source>> {
        let account = entry["account"].as_str().map(str::to_owned);
        Ok(Arc::new(super::IcloudSource::new(self.auth.clone(), account)))
    }

    /// The Apple ID's owner as Apple gives it, the Apple ID until known.
    fn describe(&self, entry: &Value) -> Value {
        let account = entry["account"].as_str().unwrap_or("");
        json!({"account": account, "name": auth::account_name(account).unwrap_or_else(|| account.to_owned())})
    }

    /// Only an Apple ID signed in to here.
    fn entry_for(&self, params: &Value) -> Result<Value, Failure> {
        let account = params["account"].as_str().ok_or_else(|| Failure::bad_request("iCloud needs an Apple ID"))?;
        if !self.accounts().iter().any(|a| a == account) {
            return Err(Failure::new("needs_sign_in", format!("{account} is not signed in to")));
        }
        Ok(json!({"kind": "icloud", "account": account}))
    }

    /// The Apple IDs icloudpd is signed in to, as the names of its session
    /// files, leaving out those signed in to here already.
    fn discover(&self, _added: &[Value]) -> Value {
        let home = PathBuf::from(std::env::var_os("HOME").unwrap_or_default());
        json!({"icloudpd": crate::discover::icloudpd_stems(&home.join(".config/icloudpd"), &self.accounts())})
    }

    fn needs_sign_in(&self) -> bool {
        true
    }

    fn accepts(&self) -> &'static [&'static str] {
        super::upload::ACCEPTS
    }

    /// `apple_id`, and `password` unless icloudpd is signed in to it.
    fn sign_in(&self, params: &Value) -> Result<SignedIn, Failure> {
        let apple_id = params["apple_id"]
            .as_str()
            .map(str::trim)
            .filter(|a| !a.is_empty())
            .ok_or_else(|| Failure::bad_request("sign_in needs an apple_id"))?;
        let password = params["password"].as_str().filter(|p| !p.is_empty());
        let how = if password.is_some() { "a password" } else { "icloudpd's session" };
        eprintln!("pinholed: sign_in {apple_id} with {how}");
        logged("sign_in", apple_id, how, self.start_sign_in(apple_id, password))
    }

    /// `apple_id` and `code`, for a sign-in waiting for it.
    fn submit_code(&self, params: &Value) -> Result<SignedIn, Failure> {
        let apple_id = params["apple_id"].as_str().ok_or_else(|| Failure::bad_request("submit_code needs an apple_id"))?;
        let code = params["code"]
            .as_str()
            .map(str::to_owned)
            .or_else(|| params["code"].as_u64().map(|c| format!("{c:06}")))
            .ok_or_else(|| Failure::bad_request("submit_code needs the code"))?;
        eprintln!("pinholed: submit_code {apple_id} with the code");
        logged("submit_code", apple_id, "the code", self.finish_sign_in(apple_id, &code))
    }

    /// Its session deleted, nothing else: it stays among the sources with
    /// its library (and its owner's name). Without a session the provider
    /// says so without asking Apple.
    fn sign_out(&self, entry: &Value) -> Result<(), Failure> {
        let account = entry["account"].as_str().ok_or_else(|| Failure::bad_request(format!("{} is not an Apple ID", self.id_of(entry))))?;
        self.waiting().remove(account);
        // The provider keeps the whole session there (auth/PROTOCOL.md).
        remove(&auth::state_dir(account).join("cookies")).map_err(|e| Failure::new("unavailable", e))?;
        eprintln!("pinholed: signed out of {account}");
        Ok(())
    }

    /// Signed out of too: its whole state folder goes.
    fn removed(&self, entry: &Value) {
        if let Some(account) = entry["account"].as_str() {
            self.waiting().remove(account);
            let _ = std::fs::remove_dir_all(auth::state_dir(account));
        }
    }
}

fn remove(dir: &Path) -> std::result::Result<(), String> {
    match std::fs::remove_dir_all(dir) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(format!("removing {}: {e}", dir.display())),
        _ => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_entry_is_an_apple_id_signed_in_to_here() {
        let kind = IcloudKind::with_accounts(PathBuf::from("/nonexistent/pinhole-auth"), vec!["a@x.com".into()]);
        assert_eq!(kind.normal(&json!({"kind": "icloud"})), Some(json!({"kind": "icloud", "account": "a@x.com"})));
        assert_eq!(kind.normal(&json!({"kind": "icloud", "account": "b@x.com"})), Some(json!({"kind": "icloud", "account": "b@x.com"})));
        assert_eq!(kind.id_of(&json!({"kind": "icloud", "account": "b@x.com"})), "icloud:b@x.com");
        assert_eq!(kind.entry_for(&json!({"account": "a@x.com"})), Ok(json!({"kind": "icloud", "account": "a@x.com"})));
        assert_eq!(kind.entry_for(&json!({"account": "b@x.com"})).unwrap_err().code, "needs_sign_in");
        assert_eq!(kind.entry_for(&json!({})).unwrap_err().code, "bad_request");
        assert_eq!(kind.sign_in(&json!({"apple_id": " "})).unwrap_err().code, "bad_request");
        assert_eq!(kind.submit_code(&json!({"apple_id": "a@x.com", "code": 123})).unwrap_err().code, "bad_request");
        assert_eq!(kind.sign_out(&json!({"kind": "icloud"})).unwrap_err().code, "bad_request");
        let source = kind.source(&json!({"kind": "icloud", "account": "a@x.com"})).unwrap();
        assert_eq!(source.id(), "icloud:a@x.com");
        assert_eq!(source.details(), json!({"account": "a@x.com"}));
        // Two signed in to: an entry naming none names nothing.
        let two = IcloudKind::with_accounts(PathBuf::new(), vec!["a@x.com".into(), "b@x.com".into()]);
        assert_eq!(two.normal(&json!({"kind": "icloud"})), None);
        assert_eq!(two.implicit().len(), 2);
    }
}
