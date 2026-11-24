"""bin/install-depends installs what a source installation lacks and keeps what it could lose."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "install-depends"
RECIPE = (Path(__file__).resolve().parents[2] / "pkgbuild/PKGBUILD").read_text()

# A pacman of a system lacking libheif, with sqlite installed only for
# another package and wayland provided by a package of another name. What
# it is asked to change goes to $LOG.
PACMAN = """#!/bin/sh
case "$1" in
  -T) shift; for n in "$@"; do [ "$n" = libheif ] && echo libheif; done; echo "$@" | grep -qw libheif && exit 127; exit 0 ;;
  -Qqd) [ "$2" = sqlite ] && { echo sqlite; exit 0; }; [ "$2" = wayland ] && { echo "error: package 'wayland' was not found" >&2; exit 1; }; exit 1 ;;
  -S|-D) echo "pacman $*" >> "$LOG"; exit 0 ;;
esac
exit 2
"""
# A sudo that may run without a password, and says what it ran.
SUDO = """#!/bin/sh
[ "$1" = -n ] && exit 0
echo "sudo" >> "$LOG"
exec "$@"
"""


class InstallDependsTests(unittest.TestCase):
    def run_with(self, pacman, *arguments, sudo=None):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            log = folder / "log"
            log.touch()
            env = {**os.environ, "LOG": str(log), "PINHOLE_SUDO": ""}
            for name, script in [("pacman", pacman), ("sudo", sudo)]:
                if script is None:
                    continue
                (folder / name).write_text(script)
                (folder / name).chmod(0o755)
                env["PINHOLE_" + name.upper()] = str(folder / name)
            result = subprocess.run([str(SCRIPT), *arguments], capture_output=True, text=True,
                                    env=env, stdin=subprocess.DEVNULL)
            return result, log.read_text().splitlines()

    def test_installs_what_is_missing_and_keeps_what_could_go(self):
        self.assertIn("'libheif'", RECIPE)
        result, ran = self.run_with(PACMAN, sudo=SUDO)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(ran, ["sudo", "pacman -S --needed --asexplicit --noconfirm libheif",
                               "sudo", "pacman -D --asexplicit sqlite"])
        self.assertIn("Installing what Pinhole needs: libheif", result.stdout)

    def test_says_the_commands_without_sudo_and_fails_on_what_is_missing(self):
        result, ran = self.run_with(PACMAN)
        self.assertEqual((result.returncode, ran), (1, []))
        lines = result.stderr.splitlines()
        self.assertEqual(len(lines), 2, result.stderr)
        self.assertTrue(lines[0].startswith("Warning: Pinhole needs libheif, not installed; "
                                            "pacman holds sqlite only"), lines[0])
        self.assertEqual(lines[1], "  Install and keep them: sudo pacman -S --needed --asexplicit libheif"
                                   " && sudo pacman -D --asexplicit sqlite")

    def test_check_only_says(self):
        result, ran = self.run_with(PACMAN, "--check", sudo=SUDO)
        self.assertEqual((result.returncode, ran), (0, []))
        self.assertIn("Pinhole needs libheif", result.stderr)

    def test_only_loose_is_no_failure(self):
        result, ran = self.run_with(PACMAN.replace("[ \"$n\" = libheif ] && echo libheif", "true")
                                    .replace("echo \"$@\" | grep -qw libheif && exit 127", "true"))
        self.assertEqual((result.returncode, ran), (0, []))
        self.assertIn("pacman holds sqlite only", result.stderr)

    def test_a_build_tool_on_path_counts(self):
        # cargo from rustup: pacman knows no cargo, PATH has one.
        pacman = PACMAN.replace('[ "$n" = libheif ] && echo libheif', '[ "$n" = cargo ] && echo cargo')
        pacman = pacman.replace('grep -qw libheif', 'grep -qw cargo')
        with tempfile.TemporaryDirectory() as temporary:
            (Path(temporary) / "cargo").write_text("#!/bin/sh\n")
            (Path(temporary) / "cargo").chmod(0o755)
            os.environ["PATH"], saved = f"{temporary}:{os.environ['PATH']}", os.environ["PATH"]
            try:
                result, ran = self.run_with(pacman, sudo=SUDO)
            finally:
                os.environ["PATH"] = saved
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(ran, ["sudo", "pacman -D --asexplicit sqlite"])

    def test_silent_when_all_is_kept(self):
        result, ran = self.run_with("#!/bin/sh\nexit 1\n", sudo=SUDO)
        self.assertEqual((result.returncode, result.stderr, ran), (0, "", []))

    def test_silent_without_pacman(self):
        result = subprocess.run([str(SCRIPT)], capture_output=True, text=True,
                                env={**os.environ, "PINHOLE_PACMAN": ""})
        self.assertEqual((result.returncode, result.stderr), (0, ""))


if __name__ == "__main__":
    unittest.main()
