"""A pinned compiler must never accidentally reuse an older daemon or Cargo."""
import os
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
VERSION = (ROOT / 'bin/blaze-version').read_text().strip()

class CompilerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = dict(os.environ, XDG_DATA_HOME=str(self.root / 'data'),
                        BLAZE_HOME=str(self.root / 'state'), PINHOLE_RUST_BUILDER='blaze')
        self.env.pop('PINHOLE_BLAZE', None)
        self.env.pop('BLAZE_CACHE', None)
        self.compiler = self.root / 'data/pinhole/tools' / VERSION / 'bin/blaze'
        self.compiler.parent.mkdir(parents=True)
        self.compiler.write_text('#!/bin/sh\nif [ "$1" = --version ]; then echo "blaze ' + VERSION + ' (tested)"; exit; fi\nif [ "$1" = stop ]; then exit 0; fi\nprintf "%s\\n" "$BLAZE_HOME" "$BLAZE_CACHE" "$@"\nexit 101\n')
        self.compiler.chmod(0o755)

    def run_build(self, *args):
        return subprocess.run([str(ROOT / 'bin/build-rust'), *args], env=self.env, text=True, capture_output=True)

    def test_pinned_compiler_uses_its_own_daemon_and_preserves_failure(self):
        result = self.run_build('build', '--release')
        self.assertEqual(result.returncode, 101)
        self.assertEqual(result.stdout.splitlines(), [str(self.root / 'state' / 'd' / hashlib.sha256((str(ROOT) + '\0' + VERSION).encode()).hexdigest()[:12]), str(self.root / 'state' / VERSION / 'store'), 'build', '--release'])
        self.assertNotIn('retry', result.stderr.lower())

    def test_missing_pinned_compiler_does_not_use_system_cargo(self):
        self.compiler.unlink()
        result = self.run_build('build')
        self.assertEqual(result.returncode, 127)
        self.assertIn('bin/setup-blaze', result.stderr)

    def test_wrong_override_version_is_refused(self):
        self.compiler.write_text('#!/bin/sh\necho "blaze 0.20261027.0 (old)"\n')
        self.env['PINHOLE_BLAZE'] = str(self.compiler)
        result = self.run_build('build')
        self.assertEqual(result.returncode, 2)
        self.assertIn('found: blaze 0.20261027.0', result.stderr)

    def test_cargo_is_only_selected_explicitly(self):
        cargo = self.root / 'cargo'
        cargo.write_text('#!/bin/sh\nprintf "explicit cargo: %s\\n" "$*"\n')
        cargo.chmod(0o755)
        self.env.update(PINHOLE_RUST_BUILDER='cargo', PATH=str(self.root) + os.pathsep + self.env['PATH'])
        result = self.run_build('check', '--locked')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), 'explicit cargo: check --locked')

if __name__ == '__main__':
    unittest.main()
