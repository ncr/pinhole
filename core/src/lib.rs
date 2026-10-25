//! Pinhole's core.
//!
//! Two boundaries matter here. `auth` talks to a sign-in provider process and
//! gets back a finished `Session`; nothing else in the core knows how Apple's
//! sign-in works. `source` is what the rest of the app asks for photos,
//! whoever keeps them (docs/sources.md); iCloud is one source, in `icloud`.

pub mod auth;
pub mod cloudkit;
pub mod db;
pub mod icloud;
pub mod service;
pub mod session;
pub mod source;
pub mod storage;
pub mod stream;
pub mod sync;
