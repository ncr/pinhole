"""Lightweight CI regression checks; no compiler, Docker or desktop needed."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pinhole_ci', ROOT / 'ci/check.py')
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)
identity_spec = importlib.util.spec_from_file_location('pinhole_ci_identity', ROOT / 'ci/identity.py')
identity = importlib.util.module_from_spec(identity_spec)
identity_spec.loader.exec_module(identity)


class RunnerTests(unittest.TestCase):
    def test_runner_identity_preserves_image_accounts_and_maps_a_missing_uid(self):
        passwd = 'root:x:0:0:root:/root:/bin/bash\nbuild:x:1000:1000::/home/build:/bin/bash\ndbus:x:81:81:D-Bus:/:/usr/bin/nologin\n'
        group = 'root:x:0:\nbuild:x:1000:\ndbus:x:81:\n'
        users, groups = identity.account_files(passwd, group, 1001, 1001)
        self.assertTrue(users.startswith(passwd))
        self.assertTrue(groups.startswith(group))
        self.assertIn('pinhole-ci:x:1001:1001:Pinhole CI:/tmp/pinhole-ci-home:/bin/bash', users)
        self.assertIn('pinhole-ci:x:1001:pinhole-ci', groups)

    def test_existing_image_uid_gets_one_private_home_without_duplicate_accounts(self):
        passwd = 'root:x:0:0:root:/root:/bin/bash\nbuild:x:1000:1000::/home/build:/bin/bash\n'
        group = 'root:x:0:\nbuild:x:1000:\n'
        users, groups = identity.account_files(passwd, group, 1000, 1000)
        self.assertEqual(users.count('build:'), 1)
        self.assertIn('build:x:1000:1000::/tmp/pinhole-ci-home:/bin/bash', users)
        self.assertEqual(group, groups)
        with self.assertRaises(ValueError):
            identity.account_files(passwd, group, 0, 0)

    def test_ci_entrypoint_cleans_private_identity_after_success_and_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            fake = base / 'docker'
            fake.write_text('#!' + sys.executable + '\n' + r"""
import json,os,pathlib,sys
if sys.argv[1]=='build': raise SystemExit(0)
if '/source/ci/in-container' not in sys.argv:
    print(json.dumps({'passwd':'root:x:0:0:root:/root:/bin/bash\nbuild:x:1000:1000::/home/build:/bin/bash\n',
                      'group':'root:x:0:\nbuild:x:1000:\n'}))
    raise SystemExit(0)
mounts=[arg for arg in sys.argv if 'target=/etc/' in arg]
paths=[pathlib.Path(arg.split('source=',1)[1].split(',target=',1)[0]) for arg in mounts]
proof={'mounts':mounts,'paths':[str(p) for p in paths],
       'files':[p.read_text() for p in paths],
       'directory_mode':paths[0].parent.stat().st_mode & 0o777,
       'modes':[p.stat().st_mode & 0o777 for p in paths]}
pathlib.Path(os.environ['PINHOLE_IDENTITY_PROOF']).write_text(json.dumps(proof))
raise SystemExit(int(os.environ['PINHOLE_IDENTITY_EXIT']))
""")
            fake.chmod(0o755)
            for exit_code in [0, 23]:
                with self.subTest(exit_code=exit_code):
                    proof = base / f'proof-{exit_code}.json'
                    env = os.environ | {'PATH': str(base) + os.pathsep + os.environ['PATH'],
                                        'PINHOLE_CI_CACHE': str(base / 'cache'),
                                        'PINHOLE_CI_OUTPUT': str(base / 'output'),
                                        'PINHOLE_IDENTITY_PROOF': str(proof),
                                        'PINHOLE_IDENTITY_EXIT': str(exit_code)}
                    result = subprocess.run([str(ROOT / 'ci/run'), 'rust'], env=env,
                                            text=True, capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, exit_code, result.stderr)
                    record = json.loads(proof.read_text())
                    self.assertEqual(record['directory_mode'], 0o700)
                    self.assertEqual(record['modes'], [0o644, 0o644])
                    self.assertTrue(all(mount.endswith(',readonly') for mount in record['mounts']))
                    self.assertIn('/tmp/pinhole-ci-home', record['files'][0])
                    self.assertTrue(all(not Path(path).parent.exists() for path in record['paths']))

    def test_failure_stops_and_records_the_actual_command_status(self):
        with tempfile.TemporaryDirectory() as scratch:
            checks = ci.Checks(Path(scratch))
            with self.assertRaisesRegex(RuntimeError, r'failed \(7\)'):
                checks.run('failure', [sys.executable, '-c', 'raise SystemExit(7)'])
            results = json.loads((Path(scratch) / 'checks.json').read_text())
            self.assertEqual(results['checks'][0]['exit_code'], 7)

    def test_timeout_is_a_failure_with_a_log(self):
        with tempfile.TemporaryDirectory() as scratch:
            checks = ci.Checks(Path(scratch))
            with self.assertRaisesRegex(RuntimeError, r'failed \(124\)'):
                checks.run('timeout', [sys.executable, '-c', 'import time; time.sleep(5)'], timeout=.05)
            self.assertEqual(checks.results[0]['exit_code'], 124)
            self.assertIn('Timed out', (Path(scratch) / 'timeout.log').read_text())

    def test_missing_executable_is_a_recorded_failure(self):
        with tempfile.TemporaryDirectory() as scratch:
            checks = ci.Checks(Path(scratch))
            with self.assertRaisesRegex(RuntimeError, r'failed \(127\)'):
                checks.run('missing', [str(Path(scratch) / 'no-such-program')])
            self.assertEqual(checks.results[0]['exit_code'], 127)

    def test_empty_handbook_is_not_a_success(self):
        with self.assertRaises(RuntimeError):
            ci.validate_handbook({'api': 7, 'documents': []})
        ids = ['plugin-quickstart', 'plugins', 'plugin-examples', 'source-providers', 'plugin-agents']
        valid = {'api': 7, 'documents': [{'id': name, 'markdown': '# text'} for name in ids]}
        ci.validate_handbook(valid)
        with self.assertRaises(RuntimeError):
            ci.validate_handbook(valid | {'api': 6})
        with self.assertRaises(RuntimeError):
            ci.validate_handbook(valid | {'documents': valid['documents'] + [{'id': 'hdr', 'markdown': '# Internal'}]})
        valid['documents'][0]['markdown'] = '  '
        with self.assertRaises(RuntimeError):
            ci.validate_handbook(valid)

    def test_scroll_requires_measured_finite_consistent_frames(self):
        for body in ['', 'scroll 5,000 px/s 0 0 0 0 0', 'scroll 5,000 px/s nan 1 2 3 4',
                     'scroll 5,000 px/s 60 3 2 1 4']:
            with self.subTest(body=body), self.assertRaises(RuntimeError):
                ci.validate_scroll(body)
        ci.validate_scroll('scroll 5,000 px/s                 60 16.7 17.0 18.5 21.0 0/0% 0.1\n')

    def test_source_overlay_keeps_deletions_and_untracked_code_but_no_ignored_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); source = base / 'source'; source.mkdir(); dest = base / 'copy'; dest.mkdir()
            subprocess.run(['git', 'init', '-q', str(source)], check=True)
            (source / '.gitignore').write_text('private/\n')
            (source / 'removed.cpp').write_text('old')
            (source / 'changed.cpp').write_text('before')
            subprocess.run(['git', '-C', str(source), 'add', '.'], check=True)
            (source / 'removed.cpp').unlink(); (dest / 'removed.cpp').write_text('old')
            (source / 'changed.cpp').write_text('after')
            (source / 'new.cpp').write_text('new')
            (source / 'private').mkdir(); (source / 'private/secret').write_text('not exported')
            (source / 'relative-link').symlink_to('new.cpp')
            subprocess.run([sys.executable, str(ROOT / 'ci/export-source.py'), str(source), str(dest)], check=True)
            self.assertFalse((dest / 'removed.cpp').exists())
            self.assertEqual((dest / 'changed.cpp').read_text(), 'after')
            self.assertEqual((dest / 'new.cpp').read_text(), 'new')
            self.assertTrue((dest / 'relative-link').is_symlink())
            self.assertFalse((dest / 'private').exists())
            self.assertFalse((dest / '.git').exists())


if __name__ == '__main__':
    unittest.main()
