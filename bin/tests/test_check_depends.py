"""bin/check-depends says what a source installation lacks or could lose."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

CHECK = Path(__file__).resolve().parents[1] / "check-depends"
NAMES = (Path(__file__).resolve().parents[2] / "pkgbuild/PKGBUILD").read_text()

# A pacman of a system lacking libheif, with sqlite installed only for
# another package and wayland provided by a package of another name.
FAKE = """#!/bin/sh
case "$1" in
  -T) shift; for n in "$@"; do [ "$n" = libheif ] && echo libheif; done; [ -n "$(echo "$@" | grep -w libheif)" ] && exit 127; exit 0 ;;
  -Qqd) [ "$2" = sqlite ] && { echo sqlite; exit 0; }; [ "$2" = wayland ] && { echo "error: package 'wayland' was not found" >&2; exit 1; }; exit 1 ;;
esac
exit 2
"""


class CheckDependsTests(unittest.TestCase):
    def run_with(self, script):
        with tempfile.TemporaryDirectory() as temporary:
            pacman = Path(temporary) / "pacman"
            pacman.write_text(script)
            pacman.chmod(0o755)
            return subprocess.run([str(CHECK)], capture_output=True, text=True,
                                  env={**os.environ, "PINHOLE_PACMAN": str(pacman)})

    def test_names_what_is_missing_and_what_could_go(self):
        self.assertIn("'libheif'", NAMES)
        result = self.run_with(FAKE)
        self.assertEqual(result.returncode, 0)
        lines = result.stderr.splitlines()
        self.assertEqual(len(lines), 2, result.stderr)
        self.assertTrue(lines[0].startswith("Warning: Pinhole needs libheif, which is not installed; "
                                            "pacman holds sqlite only"), lines[0])
        self.assertEqual(lines[1], "  Install and keep them: sudo pacman -S --asexplicit libheif && sudo pacman -D --asexplicit sqlite")

    def test_silent_when_all_is_kept(self):
        result = self.run_with("#!/bin/sh\nexit 1\n")
        self.assertEqual((result.returncode, result.stderr), (0, ""))

    def test_silent_without_pacman(self):
        result = subprocess.run([sys.executable, str(CHECK)], capture_output=True, text=True,
                                env={**os.environ, "PATH": "/nonexistent", "PINHOLE_PACMAN": ""})
        self.assertEqual((result.returncode, result.stderr), (0, ""))


if __name__ == "__main__":
    unittest.main()
