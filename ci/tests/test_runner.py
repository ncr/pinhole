"""Lightweight CI regression checks; no compiler, Docker or desktop needed."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('pinhole_ci', ROOT / 'ci/check.py')
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)


class RunnerTests(unittest.TestCase):
    def test_failure_stops_and_records_the_actual_command_status(self):
        with tempfile.TemporaryDirectory() as scratch:
            checks = ci.Checks(Path(scratch), 'blaze')
            with self.assertRaisesRegex(RuntimeError, r'failed \(7\)'):
                checks.run('failure', [sys.executable, '-c', 'raise SystemExit(7)'])
            results = json.loads((Path(scratch) / 'checks.json').read_text())
            self.assertEqual(results['checks'][0]['exit_code'], 7)

    def test_timeout_is_a_failure_with_a_log(self):
        with tempfile.TemporaryDirectory() as scratch:
            checks = ci.Checks(Path(scratch), 'blaze')
            with self.assertRaisesRegex(RuntimeError, r'failed \(124\)'):
                checks.run('timeout', [sys.executable, '-c', 'import time; time.sleep(5)'], timeout=.05)
            self.assertEqual(checks.results[0]['exit_code'], 124)
            self.assertIn('Timed out', (Path(scratch) / 'timeout.log').read_text())

    def test_missing_executable_is_a_recorded_failure(self):
        with tempfile.TemporaryDirectory() as scratch:
            checks = ci.Checks(Path(scratch), 'blaze')
            with self.assertRaisesRegex(RuntimeError, r'failed \(127\)'):
                checks.run('missing', [str(Path(scratch) / 'no-such-program')])
            self.assertEqual(checks.results[0]['exit_code'], 127)

    def test_empty_handbook_is_not_a_success(self):
        with self.assertRaises(RuntimeError):
            ci.validate_handbook({'api': 7, 'documents': []})
        ids = ['plugin-quickstart', 'plugins', 'source-providers', 'plugin-agents']
        valid = {'api': 7, 'documents': [{'id': name, 'markdown': '# text'} for name in ids]}
        ci.validate_handbook(valid)
        with self.assertRaises(RuntimeError):
            ci.validate_handbook(valid | {'api': 6})
        with self.assertRaises(RuntimeError):
            ci.validate_handbook(valid | {'documents': valid['documents'] + [{'id': 'hdr', 'markdown': '# Internal'}]})
        valid['documents'][0]['markdown'] = '  '
        with self.assertRaises(RuntimeError):
            ci.validate_handbook(valid)
        with self.assertRaises(RuntimeError):
            ci.validate_handbook(valid | {'api': 6})
        with self.assertRaises(RuntimeError):
            ci.validate_handbook(valid | {'documents': valid['documents'] + [{'id': 'hdr', 'markdown': '# Internal'}]})

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
