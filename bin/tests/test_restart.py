"""Exercise installer restarts with real processes, without opening GUI windows."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / 'bin/restart-installed'


class RestartTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiled = tempfile.TemporaryDirectory()
        cls.program = Path(cls.compiled.name) / 'program'
        source = cls.program.with_suffix('.c')
        source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
static volatile sig_atomic_t done = 0;
static void stop(int signum) { done = 1; }
int main(int argc, char **argv) {
    signal(SIGTERM, stop);
    char cwd[4096]; getcwd(cwd, sizeof(cwd));
    FILE *f = fopen(getenv("PINHOLE_TEST_EVENTS"), "a");
    fprintf(f, "start|%d|%s|%s", getpid(), cwd, getenv("PINHOLE_TEST_SESSION"));
    for (int i = 1; i < argc; ++i) fprintf(f, "|%s", argv[i]);
    fprintf(f, "\n"); fclose(f);
    while (!done) usleep(10000);
    f = fopen(getenv("PINHOLE_TEST_EVENTS"), "a");
    fprintf(f, "stop|%d\n", getpid()); fclose(f);
    return 0;
}
''')
        subprocess.run(['cc', '-O0', str(source), '-o', str(cls.program)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.compiled.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.checkout, self.prefix = self.root / 'checkout', self.root / 'prefix'
        self.binary = self.checkout / 'app/build/pinhole'
        self.daemon = self.checkout / 'core/target/release/pinholed'
        for path in [self.binary, self.daemon]:
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(self.program, path)
        launcher = self.prefix / 'bin/pinhole'
        launcher.parent.mkdir(parents=True)
        launcher.symlink_to(self.binary)
        self.events = self.root / 'events'
        self.socket = str(self.root / 'runtime/pinhole.sock')
        self.env = dict(os.environ, PINHOLE_TEST_EVENTS=str(self.events), PINHOLE_TEST_SESSION='original session',
                        PINHOLE_SOCKET=self.socket, XDG_STATE_HOME=str(self.root / 'state'))
        self.children = []
        self.addCleanup(self.cleanup_processes)

    def cleanup_processes(self):
        # Relaunched processes are detached; collect their recorded PIDs too.
        if self.events.exists():
            for line in self.events.read_text().splitlines():
                if line.startswith('start|'):
                    try:
                        os.kill(int(line.split('|')[1]), 15)
                    except ProcessLookupError:
                        pass
        for child in self.children:
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()

    def wait_for(self, predicate):
        deadline = time.monotonic() + 5
        while not predicate():
            if time.monotonic() >= deadline:
                self.fail('Process did not reach expected state')
            time.sleep(.02)

    def lines(self):
        return self.events.read_text().splitlines() if self.events.exists() else []

    def start(self, program, *args):
        p = subprocess.Popen([str(program), *args], cwd=self.root, env=self.env)
        self.children.append(p)
        self.wait_for(lambda: any(line.startswith(f'start|{p.pid}|') for line in self.lines()))
        return p

    def restart(self):
        return subprocess.run([str(HELPER), str(self.prefix), str(self.checkout)],
                              env=dict(self.env, PINHOLE_TEST_SESSION='installer session'),
                              capture_output=True, text=True, timeout=15)

    def test_reopens_window_after_daemon_stops_preserving_arguments_cwd_and_session(self):
        service = self.start(self.daemon, '--socket', self.socket)
        window = self.start(self.binary, '--example', 'family photos with spaces')
        # Activation/build may already have replaced a running executable.
        replacement = self.binary.with_suffix('.new')
        shutil.copy2(self.program, replacement)
        replacement.replace(self.binary)
        result = self.restart()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.wait_for(lambda: len([line for line in self.lines() if line.startswith('start|')]) == 3)
        lines = self.lines()
        restarted = [line for line in lines if line.startswith('start|')][-1]
        self.assertIn(f'|{self.root}|original session|--example|family photos with spaces', restarted)
        self.assertLess(lines.index(f'stop|{window.pid}'), lines.index(restarted))
        self.assertLess(lines.index(f'stop|{service.pid}'), lines.index(restarted))

    def test_closed_app_stays_closed_and_old_service_stops(self):
        service = self.start(self.daemon, '--socket', self.socket)
        result = self.restart()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f'stop|{service.pid}', self.lines())
        self.assertEqual(len([line for line in self.lines() if line.startswith('start|')]), 1)

    def test_other_installation_and_other_library_service_are_untouched(self):
        other = self.root / 'other/pinhole'
        other.parent.mkdir()
        shutil.copy2(self.program, other)
        window = self.start(other)
        service = self.start(self.daemon, '--socket', self.socket + '.other')
        result = self.restart()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(window.poll())
        self.assertIsNone(service.poll())
        self.assertEqual(len(self.lines()), 2)

    def installer(self, build):
        scripts = self.checkout / 'bin'
        scripts.mkdir()
        for name in ['install', 'restart-installed']:
            shutil.copy2(ROOT / 'bin' / name, scripts / name)
        for name, contents in {'build': build, 'install-dev': 'exit 0'}.items():
            path = scripts / name
            path.write_text('#!/bin/sh\n' + contents + '\n')
            path.chmod(0o755)
        for args in [['init', '-b', 'main'], ['config', 'user.name', 'Test'],
                     ['config', 'user.email', 'test@example.invalid'], ['add', '.'], ['commit', '-m', 'Test']]:
            subprocess.run(['git', *args], cwd=self.checkout, capture_output=True, check=True)
        return scripts / 'install'

    def test_failed_build_keeps_window_and_daemon_running(self):
        installer = self.installer('exit 1')
        service = self.start(self.daemon, '--socket', self.socket)
        window = self.start(self.binary)
        result = subprocess.run([str(installer), '--non-interactive'], capture_output=True,
                                env=dict(self.env, PREFIX=str(self.prefix), PINHOLE_RUST_BUILDER='cargo'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIsNone(window.poll())
        self.assertIsNone(service.poll())
        self.assertEqual(len(self.lines()), 2)

    def test_default_installs_source_and_restarts_but_updater_keeps_running(self):
        installer = self.installer('exit 0')
        window = self.start(self.binary)
        env = dict(self.env, PREFIX=str(self.prefix), PINHOLE_RUST_BUILDER='cargo')
        held = subprocess.run([str(installer), '--non-interactive'], capture_output=True, text=True,
                              env=dict(env, PINHOLE_KEEP_RUNNING='1'))
        self.assertEqual(held.returncode, 0, held.stderr)
        self.assertIsNone(window.poll())
        self.assertEqual(len(self.lines()), 1)
        result = subprocess.run([str(installer), '--non-interactive'], capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('source checkout', result.stdout)
        self.wait_for(lambda: len([line for line in self.lines() if line.startswith('start|')]) == 2)
        self.assertIn(f'stop|{window.pid}', self.lines())


if __name__ == '__main__':
    unittest.main()
