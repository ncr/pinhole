use std::collections::{BTreeMap, HashMap};
use std::fmt;
use std::time::{SystemTime, UNIX_EPOCH};

use serde::Deserialize;
use url::Url;

/// A signed-in iCloud session, as a sign-in provider hands it over. See
/// auth/PROTOCOL.md. It holds live credentials, so it is kept in memory only
/// and its Debug output leaves the secrets out.
#[derive(Clone, Deserialize)]
pub struct Session {
    pub apple_id: String,
    pub dsid: String,
    pub services: HashMap<String, String>,
    pub params: BTreeMap<String, String>,
    pub headers: BTreeMap<String, String>,
    pub cookies: Vec<Cookie>,
    pub validated_at: String,
}

#[derive(Clone, Deserialize)]
pub struct Cookie {
    pub name: String,
    pub value: String,
    pub domain: String,
    pub path: String,
    pub secure: bool,
    pub expires: Option<f64>,
}

impl Session {
    pub fn service(&self, name: &str) -> Option<&str> {
        self.services.get(name).map(String::as_str)
    }

    /// The Cookie header for a request to `url`, following the usual
    /// domain, path, secure and expiry rules.
    pub fn cookie_header(&self, url: &Url) -> Option<String> {
        let host = url.host_str()?;
        let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0);
        let pairs: Vec<String> = self
            .cookies
            .iter()
            .filter(|c| domain_matches(host, &c.domain))
            .filter(|c| url.path().starts_with(&c.path))
            .filter(|c| !c.secure || url.scheme() == "https")
            .filter(|c| c.expires.map_or(true, |e| e > now))
            .map(|c| format!("{}={}", c.name, c.value))
            .collect();
        (!pairs.is_empty()).then(|| pairs.join("; "))
    }
}

fn domain_matches(host: &str, domain: &str) -> bool {
    let domain = domain.trim_start_matches('.');
    host == domain || host.ends_with(&format!(".{domain}"))
}

impl fmt::Debug for Session {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Session")
            .field("apple_id", &self.apple_id)
            .field("services", &self.services.len())
            .field("cookies", &self.cookies.len())
            .field("validated_at", &self.validated_at)
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cookie(name: &str, domain: &str, secure: bool, expires: Option<f64>) -> Cookie {
        Cookie { name: name.into(), value: "v".into(), domain: domain.into(), path: "/".into(), secure, expires }
    }

    fn session(cookies: Vec<Cookie>) -> Session {
        Session {
            apple_id: "a@example.com".into(),
            dsid: "1".into(),
            services: HashMap::new(),
            params: BTreeMap::new(),
            headers: BTreeMap::new(),
            cookies,
            validated_at: String::new(),
        }
    }

    #[test]
    fn sends_matching_cookies_only() {
        let s = session(vec![
            cookie("wide", ".icloud.com", true, None),
            cookie("exact", "p00-ckdatabasews.icloud.com", false, None),
            cookie("other", ".apple.com", false, None),
            cookie("stale", ".icloud.com", false, Some(1.0)),
        ]);
        let url = Url::parse("https://p00-ckdatabasews.icloud.com/database").unwrap();
        assert_eq!(s.cookie_header(&url).unwrap(), "wide=v; exact=v");
        let plain = Url::parse("http://www.icloud.com/").unwrap();
        assert_eq!(s.cookie_header(&plain), None);
    }
}
