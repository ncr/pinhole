"""A failed activation must leave the previous installation usable."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ACTIVATE = Path(__file__).resolve().parents[1] / "activate-install"
RECORD = ACTIVATE.with_name("install-record")
UNINSTALL = ACTIVATE.with_name("uninstall-files")
PATHS = ["lib/pinhole/plugins/version", "share/pinhole/update.json", "share/pinhole/sRGB.icc",
         "share/pinhole/install-manifest.json",
         "share/icons/hicolor/scalable/apps/pinhole.svg",
         "share/applications/pinhole.desktop", "bin/pinhole-auth",
         "bin/pinhole-update", "bin/pinholed", "bin/pinhole"]


class ActivationTests(unittest.TestCase):
    def prepare(self, root):
        destination, stage = root / "installed", root / "stage"
        for tree, value in [(destination, "old"), (stage, "new")]:
            for relative in PATHS:
                path = tree / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(value)
        return destination, stage

    def test_complete_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, stage = self.prepare(Path(temporary))
            subprocess.run([str(ACTIVATE), str(stage), str(destination)], check=True)
            self.assertTrue(all((destination / path).read_text() == "new" for path in PATHS))

    def test_late_failure_restores_every_previous_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, stage = self.prepare(Path(temporary))
            (stage / "bin/pinhole").unlink()
            result = subprocess.run([str(ACTIVATE), str(stage), str(destination)], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(all((destination / path).read_text() == "old" for path in PATHS))

    def test_replaces_developer_link_without_writing_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination, stage = self.prepare(root)
            checkout = root / "checkout"
            checkout.mkdir()
            (checkout / "version").write_text("checkout")
            (destination / "lib/pinhole/plugins/version").unlink()
            (destination / "lib/pinhole/plugins").rmdir()
            (destination / "lib/pinhole/plugins").symlink_to(checkout)
            subprocess.run([str(ACTIVATE), str(stage), str(destination)], check=True)
            self.assertEqual((checkout / "version").read_text(), "checkout")
            self.assertEqual((destination / "lib/pinhole/plugins/version").read_text(), "new")

    def test_activation_preserves_photo_library_beside_installed_resources(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, stage = self.prepare(Path(temporary))
            photos = [destination / "share/pinhole/library.sqlite", destination / "share/pinhole/thumbs/photo.jpg",
                      destination / "share/pinhole/originals/family.heic", destination / "share/pinhole/tools/custom-tool",
                      destination / "share/pinhole/tools/custom.state"]
            for photo in photos:
                photo.parent.mkdir(parents=True, exist_ok=True)
                photo.write_bytes(b"personal photo data")
            subprocess.run([str(ACTIVATE), str(stage), str(destination)], check=True)
            self.assertTrue(all(photo.read_bytes() == b"personal photo data" for photo in photos))


class UninstallTests(unittest.TestCase):
    def record(self, prefix, mode="local"):
        metadata = prefix / "share/pinhole/update.json"
        metadata.parent.mkdir(parents=True, exist_ok=True)
        metadata.write_text(json.dumps({"mode": mode, "prefix": str(prefix)}))
        subprocess.run([str(RECORD), str(prefix)], check=True)

    def remove(self, prefix):
        return subprocess.run([str(UNINSTALL), str(prefix)], capture_output=True, text=True)

    def test_keeps_library_config_custom_plugins_and_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "local"
            binary = prefix / "bin/pinhole"
            binary.parent.mkdir(parents=True)
            binary.write_text("installed program")
            self.record(prefix)
            personal = [prefix / "share/pinhole/library/family.heic", root / "config/pinhole/plugins/custom.qml",
                        root / "repo/source.cpp", prefix / "lib/pinhole/plugins/custom.qml"]
            for path in personal:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("personal")
            self.assertEqual(self.remove(prefix).returncode, 0)
            self.assertFalse(binary.exists())
            self.assertTrue(all(path.read_text() == "personal" for path in personal))

    def test_modified_program_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)
            program = prefix / "pinhole"
            program.write_text("installed")
            self.record(prefix)
            program.write_text("user replacement")
            result = self.remove(prefix)
            self.assertEqual(result.returncode, 0)
            self.assertIn("Preserved", result.stdout)
            self.assertEqual(program.read_text(), "user replacement")

    def test_developer_link_removed_without_following_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo = root / "repo"
            repo.mkdir()
            (repo / "photo.heic").write_text("personal")
            prefix = root / "local"
            prefix.mkdir()
            link = prefix / "plugins"
            link.symlink_to(repo)
            self.record(prefix, "developer")
            self.assertEqual(self.remove(prefix).returncode, 0)
            self.assertFalse(link.is_symlink())
            self.assertEqual((repo / "photo.heic").read_text(), "personal")

    def test_package_removal_is_left_to_os(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)
            program = prefix / "pinhole"
            program.write_text("package")
            self.record(prefix, "package")
            result = self.remove(prefix)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("pacman", result.stderr)
            self.assertEqual(program.read_text(), "package")

    def test_replaced_parent_link_does_not_remove_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "local"
            plugin = prefix / "plugins/main.qml"
            plugin.parent.mkdir(parents=True)
            plugin.write_text("same content")
            self.record(prefix)
            plugin.unlink()
            plugin.parent.rmdir()
            personal = root / "personal"
            personal.mkdir()
            (personal / "main.qml").write_text("same content")
            plugin.parent.symlink_to(personal)
            self.assertEqual(self.remove(prefix).returncode, 0)
            self.assertEqual((personal / "main.qml").read_text(), "same content")


class BuildGuardTests(unittest.TestCase):
    def test_failed_build_leaves_the_previous_installation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scripts = root / "bin"
            scripts.mkdir()
            shutil.copy2(ACTIVATE.with_name("install"), scripts / "install")
            build = scripts / "build"
            build.write_text("#!/bin/sh\necho 'build failed' >&2\nexit 66\n")
            build.chmod(0o755)
            program = root / "prefix/bin/pinhole"
            program.parent.mkdir(parents=True)
            program.write_text("previous installed program")
            result = subprocess.run([str(scripts / "install"), "--non-interactive"], cwd=root,
                                    env={**os.environ, "PREFIX": str(root / "prefix"),
                                         "XDG_STATE_HOME": str(root / "state")}, capture_output=True, text=True)
            self.assertEqual(result.returncode, 66)
            self.assertIn("build failed", result.stderr)
            self.assertEqual(program.read_text(), "previous installed program")

    def prepare(self, root, during_build):
        checkout, prefix = root / "checkout", root / "prefix"
        scripts = checkout / "bin"
        scripts.mkdir(parents=True)
        shutil.copy2(ACTIVATE.with_name("install"), scripts / "install")
        (checkout / "source.txt").write_text("original source")
        for name, contents in {
            "build": "#!/bin/sh\n" + during_build + "\n",
            "install-dev": '#!/bin/sh\nprintf "new program" > "$1/bin/pinhole"\n',
        }.items():
            script = scripts / name
            script.write_text(contents)
            script.chmod(0o755)
        for arguments in [["init", "-b", "main"], ["config", "user.name", "Install test"],
                          ["config", "user.email", "install@example.invalid"], ["add", "."],
                          ["commit", "-m", "Fixture"]]:
            subprocess.run(["git", *arguments], cwd=checkout, check=True, capture_output=True)
        revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=checkout, text=True).strip()
        program = prefix / "bin/pinhole"
        program.parent.mkdir(parents=True)
        program.write_text("previous installed program")
        result = subprocess.run([str(scripts / "install"), "--dev", "--non-interactive"], cwd=checkout,
                                env={**os.environ, "PREFIX": str(prefix), "XDG_STATE_HOME": str(root / "state"),
                                     "PINHOLE_KEEP_RUNNING": "1",
                                     "PINHOLE_EXPECTED_COMMIT": revision}, capture_output=True, text=True)
        return result, checkout, program

    def test_source_edit_during_build_prevents_activation_and_is_kept(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, checkout, program = self.prepare(Path(temporary), "printf 'user edit' > source.txt")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no installation was activated", result.stderr)
            self.assertEqual(program.read_text(), "previous installed program")
            self.assertEqual((checkout / "source.txt").read_text(), "user edit")

    def test_head_change_during_build_prevents_activation(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, checkout, program = self.prepare(Path(temporary), "git commit --allow-empty -m 'User commit'")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("no installation was activated", result.stderr)
            self.assertEqual(program.read_text(), "previous installed program")
            subject = subprocess.check_output(["git", "log", "-1", "--format=%s"], cwd=checkout, text=True).strip()
            self.assertEqual(subject, "User commit")


if __name__ == "__main__":
    unittest.main()
