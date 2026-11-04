"""A locally patched compiler must carry verified source and binary provenance."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader('pinhole_blaze_bootstrap', str(ROOT / 'bin/bootstrap-blaze'))
spec = importlib.util.spec_from_loader(loader.name, loader)
bootstrap = importlib.util.module_from_spec(spec)
loader.exec_module(bootstrap)


class CompilerBootstrapTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)

    def fixture_pin(self):
        (self.root / 'bin').mkdir()
        patch = self.root / 'bin/compiler.patch'
        patch.write_text('the reviewed patch\n')
        pin = {'schema': 1, 'version': '0.20261030.1+pinhole.1', 'upstream_version': '0.20261030.1',
               'patch': 'bin/compiler.patch', 'patch_sha256': bootstrap.digest(patch)}
        (self.root / 'bin/blaze-source.json').write_text(json.dumps(pin))
        (self.root / 'bin/blaze-version').write_text(pin['version'])
        return bootstrap.load_pin(self.root)

    def test_source_patch_is_verified_before_bootstrap(self):
        self.fixture_pin()
        (self.root / 'bin/compiler.patch').write_text('changed patch')
        with self.assertRaisesRegex(RuntimeError, 'SHA256'):
            bootstrap.load_pin(self.root)

    def test_active_pin_cannot_describe_an_unpatched_release(self):
        self.fixture_pin()
        (self.root / 'bin/blaze-version').write_text('0.20261030.1')
        with self.assertRaisesRegex(RuntimeError, 'does not match'):
            bootstrap.load_pin(self.root)

    def fixture_install(self, pin):
        tool = self.root / 'tool'
        (tool / 'bin').mkdir(parents=True)
        (tool / 'bin/blaze-engine').write_bytes(b'compiler built from the reviewed patch')
        program = tool / 'bin/blaze'
        program.write_text('#!/bin/sh\necho "blaze ' + pin['version'] + ' (tested)"\n')
        program.chmod(0o755)
        library = self.fixture_library(tool / 'share/blaze/library')
        expected = library.joinpath('DIGEST').read_text().strip()
        override = patch.object(bootstrap, 'LIBRARY_DIGEST', expected)
        override.start()
        self.addCleanup(override.stop)
        record = {'schema': 1, 'pin': pin['fingerprint'], 'version': pin['version'],
                  'engine_sha256': bootstrap.digest(tool / 'bin/blaze-engine'), 'library_digest': expected}
        (tool / 'pinhole-compiler.json').write_text(json.dumps(record))
        return tool

    def fixture_library(self, path):
        path.mkdir(parents=True)
        (path / 'core.rs').write_text('a pinned standard library\n')
        (path / 'alias.rs').symlink_to('core.rs')
        (path / 'DIGEST').write_text(bootstrap.library_digest(path) + '\n')
        return path

    def test_unchanged_patched_install_is_reusable(self):
        pin = self.fixture_pin()
        self.assertTrue(bootstrap.valid_install(self.fixture_install(pin), pin, os.environ))

    def test_replaced_engine_cannot_reuse_the_provenance_marker(self):
        pin = self.fixture_pin()
        tool = self.fixture_install(pin)
        (tool / 'bin/blaze-engine').write_bytes(b'an unpatched or replaced engine')
        self.assertFalse(bootstrap.valid_install(tool, pin, os.environ))

    def test_new_patch_identity_invalidates_old_artifacts(self):
        pin = self.fixture_pin()
        tool = self.fixture_install(pin)
        pin['fingerprint'] = 'new source and patch'
        self.assertFalse(bootstrap.valid_install(tool, pin, os.environ))

    def test_versions_share_only_the_immutable_rust_toolchain(self):
        a = bootstrap.prepare_state(self.root / 'state', 'upstream')
        b = bootstrap.prepare_state(self.root / 'state', 'patched')
        self.assertNotEqual(a, b)
        self.assertEqual((a / 'toolchains').resolve(), (b / 'toolchains').resolve())

    def test_large_page_bootstrap_does_not_execute_the_upstream_engine(self):
        pin = self.fixture_pin() | {'revision': 'tested-revision', 'date': '2026-10-30'}
        data = self.root / 'data'
        cache = self.root / 'cache'
        base = data / 'pinhole/tools' / pin['upstream_version']
        (base / 'bin').mkdir(parents=True)
        for name in ('blaze', 'blaze-rustc', 'blaze-proc-macro-srv'):
            (base / 'bin' / name).write_text('release launcher')
        work = cache / 'pinhole/blaze-bootstrap' / pin['version']
        source = work / 'source'
        source.mkdir(parents=True)
        (source / '.pinhole-patch').write_text(pin['fingerprint'])
        toolchain = self.root / 'nightly'
        built = work / 'target/release/blaze-engine'
        commands = []

        def run(arguments, *, env, cwd=None, capture=False):
            args = list(map(str, arguments))
            commands.append(args)
            self.assertNotEqual(args[0], str(base / 'bin/blaze'))
            if args[0] == str(toolchain / 'bin/rustc'):
                return 'host: aarch64-unknown-linux-gnu\ncommit-hash: expected'
            if args[0] == str(toolchain / 'bin/cargo'):
                self.assertEqual(env['JEMALLOC_SYS_WITH_LG_PAGE'], '16')
                built.parent.mkdir(parents=True)
                built.write_text('rebuilt engine')
            if args[1:] == ['--version']:
                return f"blaze {pin['version']} (tested)"
            return None

        with (patch.object(bootstrap, 'load_pin', return_value=pin),
              patch.object(bootstrap, 'valid_engine', return_value=False),
              patch.object(bootstrap, 'large_arm_pages', return_value=True),
              patch.object(bootstrap, 'rustup_toolchain', return_value=(toolchain, 'expected')),
              patch.object(bootstrap, 'ensure_library'),
              patch.object(bootstrap, 'run', side_effect=run),
              patch.object(bootstrap.subprocess, 'run'),
              patch.object(bootstrap.sys, 'argv', ['bootstrap-blaze']),
              patch.dict(os.environ, {'XDG_DATA_HOME': str(data), 'XDG_CACHE_HOME': str(cache),
                                      'BLAZE_HOME': str(self.root / 'state')})):
            self.assertEqual(bootstrap.main(), 0)
        self.assertTrue(any('jpeg-repro' in ' '.join(args) for args in commands))
        self.assertEqual((base.with_name(pin['version']) / 'bin/blaze-engine').read_text(), 'rebuilt engine')

    def test_standard_library_is_bundled_and_survives_relocation(self):
        import shutil
        source = self.fixture_library(self.root / 'verified-source')
        expected = (source / 'DIGEST').read_text().strip()
        tool = self.root / 'portable-tool'
        with patch.object(bootstrap, 'run') as network:
            bootstrap.ensure_library(tool, self.fixture_pin(), os.environ, [source], expected)
            network.assert_not_called()
        relocated = self.root / 'relocated'
        shutil.copytree(tool, relocated, symlinks=True)
        shutil.rmtree(source)
        library = relocated / 'share/blaze/library'
        self.assertTrue(bootstrap.valid_library(library, expected))
        self.assertEqual((library / 'alias.rs').read_text(), 'a pinned standard library\n')

    def test_modified_or_missing_library_invalidates_an_unchanged_engine(self):
        pin = self.fixture_pin()
        tool = self.fixture_install(pin)
        library = tool / 'share/blaze/library'
        (library / 'core.rs').write_text('corrupt source')
        self.assertTrue(bootstrap.valid_engine(tool, pin, os.environ))
        self.assertFalse(bootstrap.valid_install(tool, pin, os.environ))
        (library / 'DIGEST').unlink()
        self.assertFalse(bootstrap.valid_install(tool, pin, os.environ))

    def test_untrusted_library_download_cannot_replace_the_bundle(self):
        import tarfile
        source = self.fixture_library(self.root / 'bad-download')
        expected = (source / 'DIGEST').read_text().strip()
        (source / 'core.rs').write_text('tampered after checksum was written')
        tool = self.root / 'tool'
        original = tool / 'share/blaze/library'
        original.mkdir(parents=True)
        (original / 'keep').write_text('previous library')
        def download(arguments, **kwargs):
            self.assertIn('/v0.20261030.1/library-', arguments[-3])
            with tarfile.open(arguments[-1], 'w:xz') as archive:
                archive.add(source, arcname='library')
        with patch.object(bootstrap, 'run', side_effect=download):
            with self.assertRaisesRegex(RuntimeError, 'pinned digest'):
                bootstrap.ensure_library(tool, self.fixture_pin(), os.environ, expected=expected)
        self.assertEqual((original / 'keep').read_text(), 'previous library')


if __name__ == '__main__':
    unittest.main()
