#!/usr/bin/env python3
"""Pinhole correctness gates. The container runner supplies pinned dependencies."""
import argparse
import hashlib
import json
import math
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def validate_handbook(api):
    required = {'plugin-quickstart', 'plugins', 'source-providers', 'plugin-agents'}
    documents = api.get('documents', [])
    found = {d.get('id') for d in documents if d.get('markdown', '').strip()}
    if api.get('api', 0) < 7 or found != required or len(documents) != len(required):
        raise RuntimeError('The installed plugin handbook is incomplete')


def validate_scroll(log):
    # The benchmark can exit successfully with no recorded swaps. Do not
    # turn that into a green frame-pacing check, or impose a hardware FPS
    # target on GitHub's software-rendered virtual display.
    rows = re.findall(r'^scroll 5,000 px/s\s+(.+)$', log, re.MULTILINE)
    if len(rows) != 1:
        raise RuntimeError('The scroll benchmark did not report exactly one measured phase')
    try:
        fps, p50, p95, p99, maximum = map(float, rows[0].split()[:5])
    except ValueError as error:
        raise RuntimeError('The scroll benchmark reported invalid frame metrics') from error
    if not all(math.isfinite(x) and x >= 0 for x in (fps, p50, p95, p99, maximum)) or fps <= 0 or p50 <= 0:
        raise RuntimeError('The scroll benchmark did not record rendered frames')
    if not p50 <= p95 <= p99 <= maximum:
        raise RuntimeError('The scroll benchmark reported inconsistent frame percentiles')


class Checks:
    def __init__(self, output, builder):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.builder = builder
        self.results = []
        self.env = os.environ | {'PINHOLE_RUST_BUILDER': builder, 'BLAZE_TESTS': 'all',
                                 'QT_QPA_PLATFORMTHEME': 'generic', 'QT_FORCE_STDERR_LOGGING': '1',
                                 'PINHOLE_SILENT': '1'}

    def run(self, name, command, *, env=None, timeout=1800):
        print(f'[{name}] running', flush=True)
        started = time.monotonic()
        log = self.output / f'{name}.log'
        with log.open('w') as stream:
            try:
                result = subprocess.run(command, cwd=ROOT, env=env or self.env, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=timeout)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
                stream.write(f'\nTimed out after {timeout}s\n')
            except OSError as error:
                code = 127
                stream.write(f'\nCould not execute the check: {error}\n')
        self.results.append({'name': name, 'command': list(map(str, command)), 'exit_code': code,
                             'seconds': round(time.monotonic() - started, 3)})
        self.save()
        if code:
            print(''.join(log.read_text(errors='replace').splitlines(keepends=True)[-45:]), file=sys.stderr)
            raise RuntimeError(f'{name} failed ({code}); see {log}')
        print(f'[{name}] passed', flush=True)

    def save(self):
        (self.output / 'checks.json').write_text(json.dumps({'builder': self.builder, 'checks': self.results}, indent=2) + '\n')

    def snapshot(self, filename='source-initial.json'):
        listed = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT)
        files = {}
        for raw in sorted(set(listed.split(b'\0')) - {b''}):
            relative = os.fsdecode(raw)
            path = ROOT / relative
            if path.is_symlink():
                files[relative] = {'symlink': os.readlink(path)}
            elif path.is_file():
                files[relative] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                                   'executable': bool(path.stat().st_mode & 0o111)}
            else:
                files[relative] = {'deleted': True}
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
        (self.output / filename).write_text(json.dumps({'commit': commit, 'files': files}, indent=2) + '\n')

    def prepare(self):
        self.run('ci-runner', [sys.executable, '-m', 'unittest', 'discover', '-s', 'ci/tests', '-v'])
        if self.builder == 'blaze':
            self.run('compiler-setup', ['bin/setup-blaze'], timeout=5400)
        self.run('compiler-version', ['bin/build-rust', '--version'])
        if Path('/opt/pinhole-ci-packages.txt').exists():
            shutil.copyfile('/opt/pinhole-ci-packages.txt', self.output / 'environment-packages.txt')

    def rust(self):
        with tempfile.TemporaryDirectory(prefix='pinhole-ci-rust-') as scratch:
            env = self.env.copy()
            if self.builder == 'cargo':
                # Reference compiler's output never replaces Blaze binaries.
                env['CARGO_TARGET_DIR'] = str(Path(scratch) / 'reference-target')
            for name, manifest in [('core', 'core/Cargo.toml'), ('series', 'plugins/series/finder/Cargo.toml'),
                                   ('send', 'plugins/send/sender/Cargo.toml')]:
                self.run(f'rust-{name}', ['bin/build-rust', 'test', '--release', '--locked', '--workspace', '--no-fail-fast',
                                         '--manifest-path', manifest], env=env)
            self.run('jpeg-compiler-parity', ['bin/build-rust', 'run', '--release', '--locked',
                                            '--manifest-path', 'docs/reviews/jpeg-repro/Cargo.toml'], env=env)
            if self.builder == 'blaze':
                self.run('jpeg-byte-parity', ['bin/check-rust-parity'], env=env)

    def build(self):
        self.run('build', ['bin/build'])

    def ui(self):
        with tempfile.TemporaryDirectory(prefix='pinhole-ci-ui-') as scratch:
            env = self.profile(Path(scratch))
            self.run('qt-full', ['app/bin/test'], env=env)
            self.run('frame-rates', ['app/bin/test-framestats'], env=env)
            self.run('query-budgets', ['app/bin/test-queries'], env=env)
            self.run('accessibility', ['app/bin/test-accessibility'], env=env)
            self.run('calendar', ['plugins/calendar/tests/run'], env=env)
            self.run('installer-updater', [sys.executable, '-m', 'unittest', 'discover', '-s', 'bin/tests', '-v'], env=env)
            for manifest in sorted((ROOT / 'plugins').glob('*/manifest.json')):
                self.run(f'plugin-{manifest.parent.name}', ['app/build/pinhole', 'plugin', 'check', str(manifest.parent)], env=env)
            self.run('offline-plugin-api', ['app/build/pinhole', 'plugin', 'docs', '--json'], env=env)
            api = json.loads((self.output / 'offline-plugin-api.log').read_text())
            validate_handbook(api)

    def profile(self, scratch):
        env = self.env.copy()
        for key, folder in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                            ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state'),
                            ('XDG_PICTURES_DIR', 'Pictures'), ('HOME', 'home')]:
            path = scratch / folder
            path.mkdir(parents=True, exist_ok=True)
            env[key] = str(path)
        env['PINHOLE_BUILTIN_PLUGINS'] = str(ROOT / 'plugins')
        env['PINHOLED'] = str(ROOT / 'core/target/release/pinholed')
        return env

    def fixture(self, scratch, count):
        library = scratch / 'library'
        self.run(f'fixture-{count}', ['core/target/release/pinhole-fake', '--count', str(count), '--out', str(library)])
        return library

    def visuals(self):
        with tempfile.TemporaryDirectory(prefix='pinhole-ci-visual-') as temporary:
            scratch = Path(temporary)
            library = self.fixture(scratch, 512)
            env = self.profile(scratch) | {'QT_QPA_PLATFORM': 'xcb', 'LIBGL_ALWAYS_SOFTWARE': '1',
                                           'QSG_RHI_BACKEND': 'opengl'}
            self.run('grid-render', ['xvfb-run', '-a', '-s', '-screen 0 1600x1000x24',
                                    'app/build/tests/pinhole_tests', 'anOversizedGridCellUpgradesAPreviewToTheFullPhoto'], env=env)
            for mode in ('guide', 'accessibility', 'lazyplugins', 'querylimits', 'wheelzoom'):
                out = self.output / mode
                out.mkdir(exist_ok=True)
                test_env = env | {'PINHOLE_SELFTEST': f'{mode}:{out}', 'PINHOLE_SOCKET': str(scratch / f'{mode}.sock')}
                self.run(f'visual-{mode}', ['xvfb-run', '-a', '-s', '-screen 0 1600x1000x24',
                                          'app/build/pinhole', '--library', str(library)], env=test_env, timeout=90)

    def native(self):
        with tempfile.TemporaryDirectory(prefix='pinhole-ci-wayland-') as temporary:
            scratch = Path(temporary)
            # Enough rows to scroll continuously through uncached tiles;
            # rendering 10k synthetic JPEGs would dominate a two-core CI job.
            # Large real-library startup distributions are measured separately.
            library = self.fixture(scratch, 2048)
            runtime = scratch / 'runtime'; runtime.mkdir(mode=0o700)
            env = self.profile(scratch) | {'XDG_RUNTIME_DIR': str(runtime), 'WAYLAND_DISPLAY': 'pinhole-ci',
                                           'QT_QPA_PLATFORM': 'wayland', 'LIBGL_ALWAYS_SOFTWARE': '1',
                                           'QSG_RHI_BACKEND': 'opengl'}
            # A compositor of our own, with no DRM/input devices or host
            # Wayland socket. CI can never put a window on a user's desktop.
            with (self.output / 'weston.log').open('w') as log:
                compositor = subprocess.Popen(['weston', '--backend=headless', '--renderer=gl', '--width=1400',
                                                '--height=900', '--socket=pinhole-ci', '--no-config', '--idle-time=0'],
                                               env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while not (runtime / 'pinhole-ci').exists():
                    if compositor.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('Headless Weston failed; see weston.log')
                    time.sleep(.05)
                self.run('wayland-startup', ['app/bin/measure-startup', '--native-hidden', '--library', str(library),
                                            '--runs', '5', '--output', str(self.output / 'startup')], env=env)
                metrics = json.loads((self.output / 'startup/results.json').read_text())
                if metrics['summary']['first_frame']['median_ms'] > 3000:
                    raise RuntimeError('Warm first frame exceeds CI software-renderer safety budget (3s)')
                self.run('wayland-scroll', ['app/build/pinhole', '--bench', '--library', str(library)],
                         env=env | {'PINHOLE_BENCH_ONLY': 'scroll 5,000', 'PINHOLE_SOCKET': str(scratch / 'bench.sock')}, timeout=90)
                validate_scroll((self.output / 'wayland-scroll.log').read_text())
            finally:
                compositor.terminate()
                try: compositor.wait(timeout=3)
                except subprocess.TimeoutExpired: compositor.kill(); compositor.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['compiler', 'rust', 'ui', 'visual', 'native', 'all'])
    parser.add_argument('--builder', choices=['blaze', 'cargo'], default='blaze')
    parser.add_argument('--output', type=Path, default=ROOT / '.ci-results/local')
    args = parser.parse_args()
    check = Checks(args.output, args.builder)
    try:
        check.snapshot()
        check.prepare()
        if args.mode in ('rust', 'all'): check.rust()
        if args.mode in ('ui', 'visual', 'native', 'all'):
            check.build()
        if args.mode in ('ui', 'all'): check.ui()
        if args.mode in ('visual', 'all'):
            if not (ROOT / 'app/build/tests/pinhole_tests').exists():
                check.run('qt-render-build', ['app/bin/test', 'bundledPluginDocumentsAreReadableAndTheirLinksResolve'])
            check.visuals()
        if args.mode in ('native', 'all'): check.native()
        return 0
    except (RuntimeError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        check.save()


if __name__ == '__main__':
    raise SystemExit(main())
