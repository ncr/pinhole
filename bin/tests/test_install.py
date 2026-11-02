"""A failed activation must leave the previous installation usable."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ACTIVATE = Path(__file__).resolve().parents[1] / "activate-install"
PATHS = ["lib/pinhole/plugins/version", "share/pinhole/update.json",
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


if __name__ == "__main__":
    unittest.main()
