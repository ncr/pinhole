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
         "share/applications/pinhole.desktop", "share/licenses/pinhole/LICENSE", "bin/pinhole-auth",
         "bin/pinhole-update", "bin/pinholed", "bin/pinhole"]


class ActivationTests(unittest.TestCase):
    def prepare(self, root, installed=None):
        """A previous installation (recorded once `installed` has changed it,
        as an install would have made it) and a complete stage."""
        destination, stage = root / "installed", root / "stage"
        for tree, value in [(destination, "old"), (stage, "new")]:
            for relative in PATHS:
                path = tree / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(value)
            (tree / "share/pinhole/update.json").write_text(json.dumps({"mode": "local", "prefix": str(destination)}))
        if installed:
            installed(destination)
        for tree in [destination, stage]:
            subprocess.run([str(RECORD), str(tree)], check=True)
        return destination, stage

    def activate(self, stage, destination):
        return subprocess.run([str(ACTIVATE), str(stage), str(destination)], capture_output=True, text=True)

    def test_complete_install(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, stage = self.prepare(Path(temporary))
            self.assertEqual(self.activate(stage, destination).returncode, 0)
            self.assertTrue(all((destination / path).read_text() == "new" for path in PATHS
                                if path not in ["share/pinhole/update.json", "share/pinhole/install-manifest.json"]))

    def test_first_install_into_an_empty_prefix(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination, stage = self.prepare(root)
            shutil.rmtree(destination)
            self.assertEqual(self.activate(stage, destination).returncode, 0)
            self.assertEqual((destination / "bin/pinhole").read_text(), "new")

    def test_late_failure_restores_every_previous_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, stage = self.prepare(Path(temporary))
            before = {path: (destination / path).read_bytes() for path in PATHS}
            (stage / "bin/pinhole").unlink()
            self.assertNotEqual(self.activate(stage, destination).returncode, 0)
            self.assertEqual({path: (destination / path).read_bytes() for path in PATHS}, before)

    def test_replaces_developer_link_without_writing_checkout(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checkout = root / "checkout"
            checkout.mkdir()
            (checkout / "version").write_text("checkout")
            def linked(destination):
                (destination / "lib/pinhole/plugins/version").unlink()
                (destination / "lib/pinhole/plugins").rmdir()
                (destination / "lib/pinhole/plugins").symlink_to(checkout)
            destination, stage = self.prepare(root, linked)
            self.assertEqual(self.activate(stage, destination).returncode, 0)
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
            self.assertEqual(self.activate(stage, destination).returncode, 0)
            self.assertTrue(all(photo.read_bytes() == b"personal photo data" for photo in photos))

    def test_own_files_beside_the_installed_ones_are_kept(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, stage = self.prepare(Path(temporary))
            helper = destination / "lib/pinhole/my-helper"
            helper.write_text("mine")
            self.assertEqual(self.activate(stage, destination).returncode, 0)
            self.assertEqual(helper.read_text(), "mine")
            self.assertEqual((destination / "lib/pinhole/plugins/version").read_text(), "new")

    def test_a_replaced_launcher_stops_activation_and_nothing_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, stage = self.prepare(Path(temporary))
            (destination / "bin/pinhole").write_text("my own wrapper")
            result = self.activate(stage, destination)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(str(destination / "bin/pinhole"), result.stderr)
            self.assertIn("not installed by Pinhole", result.stderr)
            self.assertEqual((destination / "bin/pinhole").read_text(), "my own wrapper")
            self.assertEqual((destination / "lib/pinhole/plugins/version").read_text(), "old")

    def test_without_a_manifest_a_file_in_the_way_stops_activation(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination, stage = self.prepare(Path(temporary))
            (destination / "share/pinhole/install-manifest.json").unlink()
            result = self.activate(stage, destination)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not installed by Pinhole", result.stderr)
            self.assertEqual((destination / "bin/pinhole").read_text(), "old")

    def test_what_the_previous_install_had_and_this_one_does_not_goes(self):
        with tempfile.TemporaryDirectory() as temporary:
            def extra(destination):
                (destination / "lib/pinhole/retired").write_text("old")
            destination, stage = self.prepare(Path(temporary), extra)
            self.assertEqual(self.activate(stage, destination).returncode, 0)
            self.assertFalse((destination / "lib/pinhole/retired").exists())

    def test_a_retired_file_reached_through_a_link_is_left_where_it_is(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def extra(destination):
                (destination / "lib/pinhole/retired").mkdir()
                (destination / "lib/pinhole/retired/plugin.qml").write_text("same")
            destination, stage = self.prepare(root, extra)
            # Since: the folder made a link into a checkout holding the same file.
            checkout = root / "checkout"
            checkout.mkdir()
            (checkout / "plugin.qml").write_text("same")
            shutil.rmtree(destination / "lib/pinhole/retired")
            (destination / "lib/pinhole/retired").symlink_to(checkout)
            self.assertEqual(self.activate(stage, destination).returncode, 0)
            self.assertEqual((checkout / "plugin.qml").read_text(), "same")
            self.assertTrue((destination / "lib/pinhole/retired").is_symlink())

    def test_a_late_failure_puts_a_developer_link_back(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checkout = root / "checkout"
            checkout.mkdir()
            (checkout / "version").write_text("checkout")
            def linked(destination):
                shutil.rmtree(destination / "lib/pinhole/plugins")
                (destination / "lib/pinhole/plugins").symlink_to(checkout)
            destination, stage = self.prepare(root, linked)
            before = {path: (destination / path).read_bytes() for path in PATHS if "plugins" not in path}
            (stage / "bin/pinhole").unlink()
            self.assertNotEqual(self.activate(stage, destination).returncode, 0)
            self.assertTrue((destination / "lib/pinhole/plugins").is_symlink())
            self.assertEqual(os.readlink(destination / "lib/pinhole/plugins"), str(checkout))
            self.assertEqual({path: (destination / path).read_bytes() for path in before}, before)
            self.assertEqual((checkout / "version").read_text(), "checkout")


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


class InstallerTests(unittest.TestCase):
    def checkout(self, root):
        checkout = root / "checkout"
        scripts = checkout / "bin"
        scripts.mkdir(parents=True)
        for name in ["install", "restart-installed", "uninstall-files"]:
            shutil.copy2(ACTIVATE.with_name(name), scripts / name)
        install_dev = (
            'mkdir -p "$1/bin" "$1/share/pinhole"; printf x > "$1/bin/pinhole"\n'
            'printf \'{"version": 1, "mode": "developer", "prefix": "%s", "entries": []}\' "$1"'
            ' > "$1/share/pinhole/install-manifest.json"')
        for name, contents in {"build": "exit 0", "install-dev": install_dev, "install-depends": "exit 0"}.items():
            (scripts / name).write_text("#!/bin/sh\n" + contents + "\n")
            (scripts / name).chmod(0o755)
        for arguments in [["init", "-b", "main"], ["config", "user.name", "Install test"],
                          ["config", "user.email", "install@example.invalid"], ["add", "."], ["commit", "-m", "Fixture"]]:
            subprocess.run(["git", *arguments], cwd=checkout, check=True, capture_output=True)
        return scripts / "install"

    def run_installer(self, root, *args):
        env = {**os.environ, "PREFIX": str(root / "prefix"), "XDG_STATE_HOME": str(root / "state"),
               "XDG_CONFIG_HOME": str(root / "config"), "PINHOLE_KEEP_RUNNING": "1"}
        return subprocess.run([str(root / "checkout/bin/install"), "--non-interactive", *args], env=env,
                              capture_output=True, text=True, timeout=30)

    def test_refuses_root_before_building_or_writing_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.checkout(root)
            fake = root / "fakebin"
            fake.mkdir()
            (fake / "id").write_text("#!/bin/sh\necho 0\n")
            (fake / "id").chmod(0o755)
            env = {**os.environ, "PATH": f"{fake}:{os.environ['PATH']}", "PREFIX": str(root / "prefix"),
                   "XDG_STATE_HOME": str(root / "state")}
            for args in [[], ["--uninstall"]]:
                result = subprocess.run([str(root / "checkout/bin/install"), "--non-interactive", *args], env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn("not as root", result.stderr)
            self.assertFalse((root / "state").exists() or (root / "prefix").exists())

    def test_own_omarchy_widget_link_is_kept_and_a_previous_installs_is_moved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.checkout(root)
            plugins = root / "config/omarchy/plugins"
            plugins.mkdir(parents=True)
            widget = plugins / "io.github.ncr.pinhole"
            mine = root / "dotfiles/pinhole-bar"
            widget.symlink_to(mine)
            result = self.run_installer(root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Kept your own", result.stdout)
            self.assertEqual(os.readlink(widget), str(mine))
            launcher = (root / "state/pinhole/bar-launcher").read_text().strip()
            self.assertEqual(launcher, str((root / "prefix").resolve() / "bin/pinhole"))
            result = self.run_installer(root, "--uninstall")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(os.readlink(widget), str(mine))
            self.assertFalse((root / "state/pinhole/bar-launcher").exists())

            widget.unlink()
            widget.symlink_to(root / "older/lib/pinhole/omarchy-bar")
            self.assertEqual(self.run_installer(root).returncode, 0)
            made = str((root / "prefix").resolve() / "lib/pinhole/omarchy-bar")
            self.assertEqual(os.readlink(widget), made)
            self.assertEqual(self.run_installer(root, "--uninstall").returncode, 0)
            self.assertFalse(os.path.lexists(widget))


class MetadataTests(unittest.TestCase):
    def write(self, root, mode, remote="https://user:secret-token@example.invalid/pinhole.git", git=True):
        checkout = root / "checkout"
        (checkout / "bin").mkdir(parents=True)
        shutil.copy2(ACTIVATE.with_name("install-metadata"), checkout / "bin/install-metadata")
        if git:
            for arguments in [["init", "-b", "main"], ["config", "user.name", "Install test"],
                              ["config", "user.email", "install@example.invalid"], ["add", "."],
                              ["commit", "-m", "Fixture"]]:
                subprocess.run(["git", *arguments], cwd=checkout, check=True, capture_output=True)
            if remote:
                subprocess.run(["git", "remote", "add", "origin", remote], cwd=checkout, check=True)
        prefix = root / "stage"
        subprocess.run([str(checkout / "bin/install-metadata"), str(prefix)], check=True,
                       env={**os.environ, "PINHOLE_INSTALL_MODE": mode, "PINHOLE_RUNTIME_PREFIX": "/usr"})
        text = (prefix / "share/pinhole/update.json").read_text()
        return checkout, text, json.loads(text)

    def test_package_names_neither_remote_nor_build_folder(self):
        with tempfile.TemporaryDirectory() as temporary:
            checkout, text, metadata = self.write(Path(temporary), "package")
            self.assertNotIn("secret-token", text)
            self.assertNotIn(str(checkout), text)
            self.assertEqual(metadata["mode"], "package")
            self.assertEqual(metadata["prefix"], "/usr")
            self.assertRegex(metadata["commit"], "^[0-9a-f]{40}$")

    def test_developer_names_its_checkout_never_the_remote(self):
        with tempfile.TemporaryDirectory() as temporary:
            checkout, text, metadata = self.write(Path(temporary), "developer")
            self.assertNotIn("secret-token", text)
            self.assertNotIn("example.invalid", text)
            self.assertEqual(metadata["mode"], "developer")
            self.assertEqual(metadata["source"], str(checkout.resolve()))

    def test_source_archive_or_no_origin_installs_without_updates(self):
        for git, remote in [(False, None), (True, None)]:
            with self.subTest(git=git), tempfile.TemporaryDirectory() as temporary:
                _, _, metadata = self.write(Path(temporary), "developer", remote=remote, git=git)
                self.assertEqual(metadata["mode"], "local")
                self.assertNotIn("source", metadata)
                self.assertEqual("commit" in metadata, git)


class BuildGuardTests(unittest.TestCase):
    def test_failed_build_leaves_the_previous_installation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scripts = root / "bin"
            scripts.mkdir()
            shutil.copy2(ACTIVATE.with_name("install"), scripts / "install")
            build = scripts / "build"
            build.write_text("#!/bin/sh\necho 'build failed' >&2\nexit 66\n")
            (scripts / "install-depends").write_text("#!/bin/sh\nexit 0\n")
            (scripts / "install-depends").chmod(0o755)
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
            "install-depends": "#!/bin/sh\nexit 0\n",
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
