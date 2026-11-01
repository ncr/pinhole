//! pinholed's own part: the core (pinhole-core) with the built-in kinds of
//! source linked in, each the Rust of a plugin (docs/core-and-plugins.md):
//! pinhole.icloud's from plugins/icloud/source, pinhole.folder's from
//! plugins/folder/source.

use std::path::PathBuf;
use std::sync::Arc;

use pinhole_core::kinds::Kinds;
use pinhole_folder::FolderKind;
use pinhole_icloud::kind::IcloudKind;

/// The kinds compiled into pinholed: iCloud first, as Apple IDs have
/// always been the first sources, then folders. `auth` is iCloud's sign-in
/// provider.
pub fn builtin_kinds(auth: PathBuf) -> Kinds {
    Kinds::new(vec![Arc::new(IcloudKind::new(auth)), Arc::new(FolderKind)])
}
