//! Pinhole's core.
//!
//! `source` is what the rest of Pinhole asks for photos, whoever keeps them
//! (docs/sources.md). The service, the index and pinhole-sync talk only to
//! it. iCloud is one source, everything Apple's is in `icloud`: in it,
//! `auth` talks to a sign-in provider process and gets back a finished
//! `Session`, and nothing else knows how Apple's sign-in works.

pub mod db;
pub mod icloud;
pub mod service;
pub mod source;
pub mod storage;
pub mod stream;
pub mod sync;
