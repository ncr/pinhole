"""Source changes must not reuse Blaze's live contextual type inference."""
import concurrent.futures
import importlib.machinery
import importlib.util
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader('pinhole_blaze_guard', str(ROOT / 'bin/run-blaze'))
spec = importlib.util.spec_from_loader(loader.name, loader)
guard = importlib.util.module_from_spec(spec)
loader.exec_module(guard)


class BlazeGuardTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.directory = Path(self.scratch.name)
        self.root = self.directory / 'checkout'
        (self.root / 'src').mkdir(parents=True)
        self.source = self.root / 'src/main.rs'
        self.source.write_text('fn main() {}\n')
        self.log = self.directory / 'calls'
        self.base = self.directory / 'state' / 'pinned'
        self.env = dict(os.environ, BLAZE_HOME=str(self.base), BLAZE_CACHE=str(self.base / 'store'),
                        GUARD_LOG=str(self.log), GUARD_SOURCE=str(self.source),
                        GUARD_ARTIFACT=str(self.directory / 'artifact'))
        self.program = self.directory / 'blaze'
        self.program.write_text('''#!/usr/bin/env python3
import json, os, pathlib, signal, sys, time
command=sys.argv[1]
def record(event):
 with open(os.environ['GUARD_LOG'],'a') as log:
  log.write(json.dumps([event,os.environ['BLAZE_HOME'],os.environ['BLAZE_CACHE'],os.getpid()])+'\\n')
record(command)
if command=='hold':
 time.sleep(0.1)
 record('end')
if command=='edit':
 with open(os.environ['GUARD_SOURCE'],'a') as source: source.write('// changed\\n')
if command=='aba':
 source=pathlib.Path(os.environ['GUARD_SOURCE']);old=source.read_text()
 source.write_text('B');pathlib.Path(os.environ['GUARD_ARTIFACT']).write_text(source.read_text())
 source.write_text(old)
if command=='cancel':
 def terminated(*_):
  record('terminated');time.sleep(0.2);sys.exit(143)
 signal.signal(signal.SIGTERM,terminated)
 record('ready');time.sleep(10)
sys.exit(101 if command=='fail' else 0)
''')
        self.program.chmod(0o755)

    def run_guard(self, command):
        return guard.guarded_run(self.root, 'pinned', str(self.program), [command], self.env)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_unchanged_content_keeps_worker_and_persistent_store(self):
        self.assertEqual(self.run_guard('build'), 0)
        self.source.touch()
        self.assertEqual(self.run_guard('test'), 0)
        calls = self.calls()
        self.assertEqual([call[0] for call in calls], ['stop', 'build', 'test'])
        self.assertEqual({call[2] for call in calls}, {self.env['BLAZE_CACHE']})
        self.assertTrue((Path(calls[0][1]) / 'toolchains').is_symlink())

    def test_changed_source_manifest_and_deleted_input_restart_worker(self):
        self.run_guard('build')
        self.source.write_text('fn main() { println!("new"); }')
        self.run_guard('build')
        lock = self.root / 'Cargo.lock'
        lock.write_text('new dependencies')
        self.run_guard('build')
        lock.unlink()
        self.run_guard('build')
        self.assertEqual([call[0] for call in self.calls()], ['stop', 'build'] * 4)

    def test_generated_outputs_do_not_restart_worker(self):
        self.run_guard('build')
        (self.root / 'target').mkdir()
        (self.root / 'target/generated.rs').write_text('generated')
        for name in ('build', '.tools', '.agents', '.codex', '.aws', '.claude', '.ci-results'):
            (self.root / name).mkdir()
            (self.root / name / 'Cargo.lock').write_text('external or generated')
            (self.root / name / 'lib.rs').write_text('external or generated')
        (self.root / 'Main.qml').write_text('changed UI')
        self.run_guard('build')
        self.assertEqual([call[0] for call in self.calls()], ['stop', 'build', 'build'])

    def test_nested_cargo_config_restarts_worker(self):
        self.run_guard('build')
        configuration = self.root / 'helper/.cargo'
        configuration.mkdir(parents=True)
        (configuration / 'config.toml').write_text('[build]\nrustflags=["--cfg=local"]\n')
        self.run_guard('build')
        self.assertEqual([call[0] for call in self.calls()], ['stop', 'build', 'stop', 'build'])

    def test_failure_preserves_exit_code(self):
        self.assertEqual(self.run_guard('fail'), 101)
        self.assertEqual([call[0] for call in self.calls()], ['stop', 'fail'])

    def test_edit_during_command_rejects_result_and_stops_worker(self):
        self.assertEqual(self.run_guard('edit'), 75)
        self.assertEqual([call[0] for call in self.calls()], ['stop', 'edit', 'stop'])
        home = Path(self.calls()[0][1])
        self.assertFalse((home / 'pinhole-inputs.json').exists())

    def test_parallel_commands_serialize_check_and_build(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(self.run_guard, 'hold') for _ in range(2)]
            self.assertEqual([future.result() for future in futures], [0, 0])
        self.assertEqual([call[0] for call in self.calls()], ['stop', 'hold', 'end', 'hold', 'end'])

    def test_transient_source_edit_is_rejected_even_when_contents_return(self):
        before = self.source.read_text()
        self.assertEqual(self.run_guard('aba'), 75)
        self.assertEqual(self.source.read_text(), before)
        self.assertEqual(Path(self.env['GUARD_ARTIFACT']).read_text(), 'B')
        self.assertEqual([call[0] for call in self.calls()], ['stop', 'aba', 'stop'])

    def test_post_build_inspection_failure_discards_the_worker(self):
        before = guard.snapshot(self.root)
        with mock.patch.object(guard, 'snapshot', side_effect=[before, RuntimeError('racing edit')]):
            with self.assertRaisesRegex(RuntimeError, 'racing edit'):
                self.run_guard('build')
        self.assertEqual([call[0] for call in self.calls()], ['stop', 'build', 'stop'])
        self.assertFalse((Path(self.calls()[0][1]) / 'pinhole-inputs.json').exists())

    def test_detected_mutation_discards_trust_even_when_worker_stop_fails(self):
        original_stop = guard.stop
        calls = 0
        def fail_second_stop(*args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise RuntimeError('worker did not stop')
            return original_stop(*args, **kwargs)
        with mock.patch.object(guard, 'stop', side_effect=fail_second_stop):
            with self.assertRaisesRegex(RuntimeError, 'worker did not stop'):
                self.run_guard('aba')
        self.assertFalse((Path(self.calls()[0][1]) / 'pinhole-inputs.json').exists())

    def test_sigterm_reaps_child_and_holds_lock_until_cleanup_finishes(self):
        arguments = [sys.executable, str(ROOT / 'bin/run-blaze'), str(self.root), 'pinned', str(self.program)]
        first = subprocess.Popen([*arguments, 'cancel'], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        second = None
        try:
            deadline = time.monotonic() + 5
            while not self.log.exists() or not any(call[0] == 'ready' for call in self.calls()):
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.01)
            first.send_signal(signal.SIGTERM)
            second = subprocess.Popen([*arguments, 'hold'], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(first.wait(timeout=5), 143)
            self.assertEqual(second.wait(timeout=5), 0)
            events = [call[0] for call in self.calls()]
            self.assertLess(events.index('terminated'), events.index('hold'))
            self.assertEqual(events.count('stop'), 3) # Initial, cancellation cleanup, next command.
            child_pid = next(call[3] for call in self.calls() if call[0] == 'ready')
            with self.assertRaises(ProcessLookupError):
                os.kill(child_pid, 0)
        finally:
            for process in (first, second):
                if process is not None:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=5)
            if self.log.exists():
                for call in self.calls():
                    if call[0] == 'ready':
                        try:
                            os.kill(call[3], signal.SIGKILL)
                        except ProcessLookupError:
                            pass

    def test_checkout_namespaces_are_short_and_share_only_toolchains(self):
        first = guard.checkout_home(self.base, self.root, 'pinned')
        second = guard.checkout_home(self.base, self.directory / 'another', 'pinned')
        self.assertNotEqual(first, second)
        self.assertEqual((first / 'toolchains').resolve(), (second / 'toolchains').resolve())
        self.assertLess(len(str(first / 'workers' / ('f'*16) / 'sock').encode()), 108)


if __name__ == '__main__':
    unittest.main()
