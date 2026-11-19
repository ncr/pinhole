#!/usr/bin/env python3
"""Developer updater integration tests with real, local-only Git repos."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
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
        installer = self.remote / "bin/install"
        installer.write_text('''#!/usr/bin/env python3
import json, os, subprocess, sys
from pathlib import Path
p=Path(os.environ["PREFIX"])
p.mkdir(parents=True, exist_ok=True)
(p/"installed").write_text(Path("version").read_text() + os.environ["PINHOLE_KEEP_RUNNING"])
(p/"install-args").write_text(" ".join(sys.argv[1:]))
(p/"built-apart").write_text(os.environ.get("PINHOLE_BUILT", ""))
(p/"program").write_text(Path("app/build/pinhole").read_text())
''')
        installer.chmod(0o755)
        # A build that takes a while, says what the checkout held as it
        # ran, and makes the programs and a plugin's module of this version.
        build = self.remote / "bin/build"
        build.write_text('''#!/usr/bin/env python3
import os, time
from pathlib import Path
time.sleep(float(os.environ.get("PINHOLE_TEST_BUILD_WAIT", "0")))
if os.environ.get("PINHOLE_TEST_SEEN"):
    Path(os.environ["PINHOLE_TEST_SEEN"]).write_text(Path(os.environ["PINHOLE_TEST_LIVE"], "version").read_text())
version = Path("version").read_text()
for output in ["app/build/pinhole", "core/target/release/pinholed", "plugins/info/qml/Module/libmodule.so"]:
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    Path(output).write_text("built " + version)
''')
        build.chmod(0o755)
        (self.remote / ".gitignore").write_text("build/\ntarget/\n/plugins/*/qml/\n")
        (self.remote / "version").write_text("one\n")
        self.commit("initial")
        self.source = self.root / "working"
        self.git("clone", str(self.remote), str(self.source))
        self.git("config", "user.email", "tests@localhost", cwd=self.source)
        self.git("config", "user.name", "Update test", cwd=self.source)
        self.metadata = {"repository": str(self.remote), "source": str(self.source),
                         "prefix": str(self.root / "prefix"), "mode": "developer",
                         "commit": self.git("rev-parse", "HEAD", cwd=self.source)}
        self.env = patch.dict(os.environ, {"XDG_STATE_HOME": str(self.root / "state"),
                                           "XDG_CACHE_HOME": str(self.root / "cache"),
                                           "PINHOLE_TEST_LIVE": str(self.source)})
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

    def binary(self, revision):
        binary = self.source / "app/build/pinhole"
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_text("#!/bin/sh\nprintf '%s\\n' '" + revision + "'\n")
        binary.chmod(0o755)
        return binary

    def test_check_fetches_objects_without_changing_checkout_or_fetch_head(self):
        self.git("fetch", "origin", "main", cwd=self.source)
        fetch_head = (self.source / ".git/FETCH_HEAD").read_bytes()
        self.newer()
        result = update.check(self.metadata)
        self.assertEqual(result["state"], "available")
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), self.metadata["commit"])
        self.assertEqual((self.source / "version").read_text(), "one\n")
        self.assertEqual((self.source / ".git/FETCH_HEAD").read_bytes(), fetch_head)

    def test_developer_update_builds_apart_then_moves_checkout_and_programs_together(self):
        target = self.newer()
        seen = self.root / "seen-during-build"
        with patch.dict(os.environ, {"PINHOLE_TEST_SEEN": str(seen)}):
            self.apply(target)
        # The checkout was still the old one while the new one was built.
        self.assertEqual(seen.read_text(), "one\n")
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), target)
        for output in ["app/build/pinhole", "core/target/release/pinholed", "plugins/info/qml/Module/libmodule.so"]:
            self.assertEqual((self.source / output).read_text(), "built two\n")
        self.assertEqual((self.root / "prefix/installed").read_text(), "two\n1")
        self.assertEqual((self.root / "prefix/program").read_text(), "built two\n")
        self.assertEqual((self.root / "prefix/install-args").read_text(), "--non-interactive")
        self.assertEqual((self.root / "prefix/built-apart").read_text(), "1")
        self.assertEqual(list(self.source.glob("**/*.update")), [])

    def test_a_switch_stopped_part_way_is_offered_again_and_finished(self):
        self.binary(self.metadata["commit"])
        target = self.newer()
        replace = Path.replace
        def stopped(path, other):
            if Path(other).name == "pinholed":
                raise KeyboardInterrupt("killed between the programs")
            return replace(path, other)
        with patch.object(Path, "replace", stopped), self.assertRaises(KeyboardInterrupt):
            self.apply(target)
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), target)
        self.assertNotEqual((self.source / "app/build/pinhole").read_text(), "built two\n")
        # Its marker stays, so every window holds its plugins' QML from
        # being loaded again by the binary from before (Updater::switching).
        self.assertTrue(update.switch_path(self.metadata).exists())
        # Whatever the window says it runs, not current until it is done.
        self.assertEqual(update.check(self.metadata, running=target)["state"], "available")
        self.apply(target)
        for output in ["app/build/pinhole", "core/target/release/pinholed"]:
            self.assertEqual((self.source / output).read_text(), "built two\n")
        self.assertEqual(update.check(self.metadata, running=target)["state"], "current")
        self.assertFalse(update.switch_path(self.metadata).exists())

    def test_the_build_tree_is_kept_for_the_next_update(self):
        self.apply(self.newer())
        tree = update.build_tree(self.source.resolve())
        (tree / "app/build/kept-object").write_text("compiled")
        (self.remote / "version").write_text("three\n")
        self.commit("third")
        target = update.remote_revision(self.metadata)
        self.apply(target)
        self.assertEqual((tree / "app/build/kept-object").read_text(), "compiled")
        self.assertEqual((self.source / "app/build/pinhole").read_text(), "built three\n")
        # A tree whose worktree record is gone is made again.
        self.git("worktree", "remove", "--force", str(tree), cwd=self.source)
        tree.mkdir(parents=True)
        (self.remote / "version").write_text("four\n")
        self.commit("fourth")
        self.apply(update.remote_revision(self.metadata))
        self.assertEqual((self.source / "app/build/pinhole").read_text(), "built four\n")

    def test_dirty_checkout_is_never_changed(self):
        target = self.newer()
        (self.source / "version").write_text("unsaved edits\n")
        with self.assertRaisesRegex(update.UpdateError, "dirty"):
            self.apply(target)
        self.assertEqual((self.source / "version").read_text(), "unsaved edits\n")
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), self.metadata["commit"])

    def test_diverged_commits_are_never_discarded(self):
        target = self.newer()
        (self.source / "my-work").write_text("mine")
        self.commit("local work", self.source)
        local = self.git("rev-parse", "HEAD", cwd=self.source)
        with self.assertRaisesRegex(update.UpdateError, "diverged"):
            self.apply(target)
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), local)

    def test_local_ahead_commits_are_not_reset_or_rebuilt(self):
        target = update.remote_revision(self.metadata)
        (self.source / "my-work").write_text("mine")
        self.commit("ahead", self.source)
        with self.assertRaisesRegex(update.UpdateError, "ahead"):
            self.apply(target)
        self.assertFalse((self.root / "prefix/installed").exists())

    def test_other_branch_and_detached_head_are_never_switched(self):
        target = self.newer()
        self.git("switch", "-c", "my-feature", cwd=self.source)
        with self.assertRaisesRegex(update.UpdateError, "branch"):
            self.apply(target)
        self.assertEqual(self.git("branch", "--show-current", cwd=self.source), "my-feature")
        self.git("checkout", "--detach", cwd=self.source)
        with self.assertRaisesRegex(update.UpdateError, "branch"):
            self.apply(target)

    def test_packages_and_copied_installs_cannot_check_or_apply(self):
        for mode in ("package", "local", "unknown"):
            self.metadata["mode"] = mode
            with patch.object(update, "git", side_effect=AssertionError("must not invoke Git")):
                with self.assertRaisesRegex(update.UpdateError, "managed"):
                    update.check(self.metadata)
                with self.assertRaisesRegex(update.UpdateError, "managed"):
                    self.apply("a" * 40)
        self.assertFalse((self.root / "prefix").exists())
        self.assertFalse((self.root / "state").exists())

    def test_changed_remote_is_not_built(self):
        target = self.newer()
        (self.remote / "version").write_text("three\n")
        self.commit("newer again")
        with self.assertRaisesRegex(update.UpdateError, "changed"):
            self.apply(target)
        self.assertFalse((self.root / "prefix/installed").exists())

    def test_manual_pull_without_rebuild_offers_update(self):
        self.newer()
        self.git("pull", "--ff-only", cwd=self.source)
        self.binary(self.metadata["commit"])
        self.assertEqual(update.check(self.metadata)["state"], "available")

    def test_manual_rebuild_offers_restart_to_old_window(self):
        target = self.newer()
        self.git("pull", "--ff-only", cwd=self.source)
        self.binary(target)
        self.assertEqual(update.check(self.metadata, running=self.metadata["commit"])["state"], "ready")
        self.assertEqual(update.check(self.metadata, running=target)["state"], "current")

    def test_running_current_developer_is_not_bothered_by_own_edits(self):
        target = update.remote_revision(self.metadata)
        self.git("switch", "-c", "feature", cwd=self.source)
        (self.source / "version").write_text("unfinished")
        self.assertEqual(update.check(self.metadata, running=target)["state"], "current")

    def test_build_failure_leaves_checkout_programs_and_installation_as_they_were(self):
        old_binary = self.binary(self.metadata["commit"])
        old_bytes = old_binary.read_bytes()
        build = self.remote / "bin/build"
        build.write_text('''#!/usr/bin/env python3
from pathlib import Path
Path("app/build").mkdir(parents=True, exist_ok=True)
Path("app/build/pinhole").write_text("broken link output")
raise SystemExit(1)
''')
        (self.remote / "version").write_text("two\n")
        self.commit("broken build")
        target = update.remote_revision(self.metadata)
        with self.assertRaisesRegex(update.UpdateError, "build"):
            self.apply(target)
        self.assertEqual(old_binary.read_bytes(), old_bytes)
        self.assertTrue(os.access(old_binary, os.X_OK))
        self.assertEqual(self.git("rev-parse", "HEAD", cwd=self.source), self.metadata["commit"])
        self.assertEqual((self.source / "version").read_text(), "one\n")
        self.assertFalse((self.root / "prefix").exists())
        self.assertEqual(update.check(self.metadata)["state"], "available")

    def test_updaters_on_same_repository_are_serialized(self):
        with update.update_lock(self.metadata):
            with self.assertRaisesRegex(update.UpdateError, "busy"):
                with update.update_lock(self.metadata):
                    self.fail("second lock acquired")

    def test_detached_worker_finishes_after_launcher_exits_and_holds_lock(self):
        target = self.newer()
        metadata_path = self.root / "metadata.json"
        metadata_path.write_text(json.dumps(self.metadata))
        env = dict(os.environ, PINHOLE_TEST_BUILD_WAIT="0.4")
        started = subprocess.run([str(ROOT / "bin/update"), "--launch", "--metadata", str(metadata_path),
                                  "--revision", target], env=env, capture_output=True, text=True)
        self.assertEqual(started.returncode, 0, started.stdout + started.stderr)
        job = Path(json.loads(started.stdout)["job"])
        # Launcher is gone; the detached worker still owns the checkout.
        with self.assertRaisesRegex(update.UpdateError, "busy"):
            with update.update_lock(self.metadata):
                self.fail("worker lost its inherited lock")
        attached = subprocess.run([str(ROOT / "bin/update"), "--check", "--metadata", str(metadata_path)],
                                  capture_output=True, text=True)
        self.assertEqual(attached.returncode, 0, attached.stdout)
        self.assertEqual(json.loads(attached.stdout)["state"], "updating")
        for _ in range(100):
            result = json.loads(job.read_text())
            if result["state"] != "updating":
                break
            time.sleep(.02)
        self.assertEqual(result["state"], "ready", result)
        self.assertEqual((self.root / "prefix/installed").read_text(), "two\n1")
        with update.update_lock(self.metadata):
            pass

    def test_restart_failure_leaves_existing_window_and_service_alone(self):
        with patch.object(update.os, "kill", side_effect=AssertionError("must not kill")):
            with self.assertRaisesRegex(update.UpdateError, "restart"):
                update.restart(self.metadata, os.getpid(), [])


if __name__ == "__main__":
    unittest.main()
