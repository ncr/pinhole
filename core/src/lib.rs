//! Pinhole's core.
//!
//! Two boundaries matter here. `auth` talks to a sign-in provider process and
//! gets back a finished `Session`; nothing else in the core knows how Apple's
//! sign-in works. `source` is what the rest of the app asks for photos; the
//! iCloud implementation sits behind it, so a fake library can stand in for
//! tests and stress runs.

pub mod auth;
pub mod cloudkit;
pub mod db;
pub mod session;
pub mod source;
pub mod sync;
