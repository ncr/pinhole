//! Pinhole's core: what pinholed is, whatever the photos come from.
//!
//! `source` is what the rest of Pinhole asks for photos, whoever keeps them
//! (docs/sources.md), and `kinds` the kinds of source there are, each
//! owned by a plugin. The service and the index talk only to them. The
//! built-in kinds are crates of their plugins, linked into pinholed
//! (core/daemon): pinhole.icloud's in plugins/icloud/source, pinhole.folder's
//! in plugins/folder/source.

pub mod add;
pub mod background;
pub mod cover;
pub mod db;
pub mod effects;
pub mod jpeg;
pub mod kinds;
pub mod plugins;
pub mod service;
pub mod source;
pub mod storage;
pub mod stream;
pub mod sync;
