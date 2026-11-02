#!/usr/bin/env python3
"""Real git fixtures; no network, application install, or photo library."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader("pinhole_update", str(ROOT / "bin/update"))
spec = importlib.util.spec_from_loader(loader.name, loader)
update = importlib.util.module_from_spec(spec)
loader.exec_module(update)


class Updates(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.remote = self.root / "remote"
        self.remote.mkdir()
        self.git("init", "-b", "main", cwd=self.remote)
        self.git("config", "user.email", "tests@localhost", cwd=self.remote)
        self.git("config", "user.name", "Update test", cwd=self.remote)
        (self.remote / "bin").mkdir()
        installer = self.remote / "bin/install-local"
        installer.write_text('''#!/usr/bin/env python3
import json, os
from pathlib import Path
p=Path(os.environ["PREFIX"])
p.mkdir(parents=True, exist_ok=True)
(p/"installed").write_text(Path("version").read_text() + os.environ["PINHOLE_KEEP_RUNNING"])
(p/"install-args").write_text(" ".join(__import__("sys").argv[1:]))
''')
        installer.chmod(0o755)
        (self.remote / "version").write_text("one\n")
        self.commit("initial")
        self.source = self.root / "working"
        self.git("clone", str(self.remote), str(self.source))
        self.git("config", "user.email", "tests@localhost", cwd=self.source)
        self.git("config", "user.name", "Update test", cwd=self.source)
        self.metadata = {"repository": str(self.remote), "source": str(self.source),
                         "prefix": str(self.root / "prefix"), "mode": "developer",
                         "commit": self.git("rev-parse", "HEAD", cwd=self.source)}
        self.env = patch.dict(os.environ, {"XDG_CACHE_HOME": str(self.root / "cache")})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.tmp.cleanup()

    def git(self, *args, cwd=None):
        return subprocess.check_output(["git", *args], cwd=cwd, stderr=subprocess.DEVNULL, text=True).strip()

    def commit(self, message, cwd=None):
        cwd = cwd or self.remote
        self.git("add", ".", cwd=cwd)
        self.git("commit", "-m", message, cwd=cwd)

    def newer(self):
        (self.remote / "version").write_text("two\n")
        self.commit("new main")
        return update.remote_revision(self.metadata)

    def apply(self, target):
        with (self.root / "build.log").open("w") as log:
            return update.apply(self.metadata, target, log)

    def test_check_does_not_change_development_checkout(self):
        target = self.newer()
        self.assertNotEqual(target, self.metadata["commit"])
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), self.metadata["commit"])
        self.assertEqual((self.source / "version").read_text(), "one\n")

    def test_developer_update_fast_forwards_builds_and_keeps_running(self):
        target = self.newer()
        self.apply(target)
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), target)
        self.assertEqual((self.root / "prefix/installed").read_text(), "two\n1")
        self.assertEqual((self.root / "prefix/install-args").read_text(), "--dev")

    def test_dirty_checkout_is_never_changed(self):
        target = self.newer()
        (self.source / "version").write_text("unsaved edits\n")
        with self.assertRaisesRegex(update.UpdateError, "dirty"):
            self.apply(target)
        self.assertEqual((self.source / "version").read_text(), "unsaved edits\n")
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), self.metadata["commit"])

    def test_local_commits_are_never_discarded(self):
        target = self.newer()
        (self.source / "my-work").write_text("mine")
        self.commit("local work", self.source)
        local = self.git("rev-parse", "HEAD", cwd=self.source)
        with self.assertRaisesRegex(update.UpdateError, "diverged"):
            self.apply(target)
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), local)

    def test_other_branch_is_never_switched(self):
        target = self.newer()
        self.git("switch", "-c", "my-feature", cwd=self.source)
        with self.assertRaisesRegex(update.UpdateError, "branch"):
            self.apply(target)
        self.assertEqual(self.git("branch", "--show-current", cwd=self.source), "my-feature")

    def test_local_install_uses_isolated_checkout_and_updates_incrementally(self):
        self.metadata["mode"] = "local"
        target = self.newer()
        (self.source / "version").write_text("unfinished local work")
        self.apply(target)
        self.assertEqual((self.root / "prefix/installed").read_text(), "two\n1")
        self.assertEqual((self.source / "version").read_text(), "unfinished local work")
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), self.metadata["commit"])
        (self.remote / "version").write_text("three\n")
        self.commit("another update")
        self.apply(update.remote_revision(self.metadata))
        self.assertEqual((self.root / "prefix/installed").read_text(), "three\n1")

    def test_system_package_updates_in_user_prefix_without_modifying_usr(self):
        self.metadata.update(mode="package", prefix="/usr")
        with patch.object(Path, "home", return_value=self.root / "home"):
            installed = self.apply(self.newer())
        self.assertEqual(installed, self.root / "home/.local/share/pinhole/update.json")
        self.assertEqual((self.root / "home/.local/installed").read_text(), "two\n1")

    def test_changed_remote_is_not_built(self):
        target = self.newer()
        (self.remote / "version").write_text("three\n")
        self.commit("newer again")
        with self.assertRaisesRegex(update.UpdateError, "changed"):
            self.apply(target)
        self.assertFalse((self.root / "prefix/installed").exists())


if __name__ == "__main__":
    unittest.main()
