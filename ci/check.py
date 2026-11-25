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
    required = {'plugin-quickstart', 'plugins', 'plugin-examples', 'source-providers', 'plugin-agents', 'plugin-kit'}
    documents = api.get('documents', [])
    found = {d.get('id') for d in documents if d.get('markdown', '').strip()}
    if api.get('api', 0) < 7 or found != required or len(documents) != len(required):
        raise RuntimeError('The installed plugin handbook is incomplete')


def package_expectations(root):
    """What the package must hold: the programs, and every runtime file of
    every plugin and of the bar widget as the checkout has them."""
    expected = {'usr/bin/pinhole', 'usr/bin/pinholed', 'usr/bin/pinhole-update', 'usr/bin/pinhole-auth',
                'usr/share/applications/pinhole.desktop', 'usr/share/icons/hicolor/scalable/apps/pinhole.svg',
                'usr/share/pinhole/sRGB.icc', 'usr/share/pinhole/update.json',
                'usr/share/pinhole/install-manifest.json', 'usr/share/licenses/pinhole/LICENSE',
                'usr/share/licenses/pinhole/THIRD-PARTY-NOTICES.txt',
                'usr/lib/pinhole/plugins/series/pinhole-series', 'usr/lib/pinhole/plugins/send/pinhole-send',
                'usr/lib/pinhole/plugins/info/qml/Pinhole/Internal/Info/qmldir',
                'usr/lib/pinhole/plugins/info/qml/Pinhole/Internal/Info/libpinholeinfo.so'}
    listed = subprocess.check_output(['git', 'ls-files', '-z', 'plugins', 'omarchy-bar'], cwd=root)
    plugins = {Path(os.fsdecode(raw)).parent.name for raw in listed.split(b'\0')
               if os.fsdecode(raw).count('/') == 2 and os.fsdecode(raw).endswith('/manifest.json')}
    for raw in set(listed.split(b'\0')) - {b''}:
        parts = Path(os.fsdecode(raw)).parts
        if parts[-1].endswith('.md') or parts[-1].startswith('.'):
            continue
        if parts[0] == 'omarchy-bar' and (len(parts) == 2 or (len(parts) == 3 and parts[1] == 'i18n')):
            expected.add('usr/lib/pinhole/' + '/'.join(parts))
        elif parts[0] == 'plugins' and len(parts) > 2 and parts[1] in plugins and (
                len(parts) == 3 or (len(parts) == 4 and parts[2] == 'i18n')):
            expected.add('usr/lib/pinhole/' + '/'.join(parts))
    return expected


def validate_package(names, metadata, built_in, expected):
    """A package's file list and its update.json, against what it must hold
    and must not tell: where it was built, or a Git remote."""
    names = {name.rstrip('/') for name in names}
    missing = sorted(expected - names)
    if missing:
        raise RuntimeError('The package lacks ' + ', '.join(missing))
    stray = sorted(name for name in names if name != 'usr' and not name.startswith(('usr/', '.')))
    if stray:
        raise RuntimeError('The package has files outside /usr: ' + ', '.join(stray))
    recorded = json.loads(metadata)
    if recorded.get('mode') != 'package' or recorded.get('prefix') != '/usr':
        raise RuntimeError('The package does not say it is a package installed in /usr')
    if {'source', 'repository'} & recorded.keys() or built_in in metadata:
        raise RuntimeError('The package tells where it was built or its Git remote')


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
    def __init__(self, output):
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.results = []
        self.env = os.environ | {'QT_QPA_PLATFORMTHEME': 'generic', 'QT_FORCE_STDERR_LOGGING': '1',
                                 'PINHOLE_SILENT': '1'}

    def run(self, name, command, *, env=None, timeout=1800, cwd=ROOT):
        print(f'[{name}] running', flush=True)
        started = time.monotonic()
        log = self.output / f'{name}.log'
        with log.open('w') as stream:
            try:
                result = subprocess.run(command, cwd=cwd, env=env or self.env, stdout=stream,
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
        (self.output / 'checks.json').write_text(json.dumps({'builder': 'cargo', 'checks': self.results}, indent=2) + '\n')

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
        self.run('compiler-version', ['cargo', '--version', '--verbose'])
        if Path('/opt/pinhole-ci-packages.txt').exists():
            shutil.copyfile('/opt/pinhole-ci-packages.txt', self.output / 'environment-packages.txt')

    def rust(self):
        for name, manifest in [('core', 'core/Cargo.toml'), ('series', 'plugins/series/finder/Cargo.toml'),
                               ('send', 'plugins/send/sender/Cargo.toml')]:
            self.run(f'rust-{name}', ['cargo', 'test', '--release', '--locked', '--workspace', '--no-fail-fast',
                                     '--manifest-path', manifest])
        with tempfile.TemporaryDirectory(prefix='pinhole-ci-jpeg-') as scratch:
            for mode, flags in [('simd', []), ('scalar', ['--no-default-features'])]:
                self.run(f'jpeg-{mode}', ['cargo', 'run', '--release', '--locked',
                                         '--manifest-path', 'docs/reviews/jpeg-repro/Cargo.toml',
                                         *flags, '--', str(Path(scratch) / mode)])

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
            self.run('live-effects', ['plugins/live/tests/run'], env=env)
            self.run('installer-updater', [sys.executable, '-m', 'unittest', 'discover', '-s', 'bin/tests', '-v'], env=env)
            self.run('photo-search-engine', [sys.executable, '-m', 'unittest', 'discover', '-s',
                                             'examples/semantic-search/engine/tests', '-v'], env=env)
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
            # The plugins that start off are on, as app/bin/test-selftests has
            # them: lazyplugins looks into Calendar and Frame.
            manifests = [json.loads(m.read_text()) for m in (ROOT / 'plugins').glob('*/manifest.json')]
            (scratch / 'config/pinhole').mkdir(parents=True, exist_ok=True)
            (scratch / 'config/pinhole/plugins.json').write_text(json.dumps(
                {'enabled': {m['id']: True for m in manifests if m.get('startsOff')}}))
            self.run('grid-render', ['xvfb-run', '-a', '-s', '-screen 0 1600x1000x24',
                                    'app/build/tests/pinhole_tests', 'anOversizedGridCellUpgradesAPreviewToTheFullPhoto'], env=env)
            places = self.output / 'places'
            places.mkdir(exist_ok=True)
            self.run('visual-places', ['app/bin/test-places', str(places)], env=env, timeout=45)
            for mode in ('guide', 'accessibility', 'lazyplugins', 'querylimits', 'wheelzoom'):
                out = self.output / mode
                out.mkdir(exist_ok=True)
                test_env = env | {'PINHOLE_SELFTEST': f'{mode}:{out}', 'PINHOLE_SOCKET': str(scratch / f'{mode}.sock')}
                self.run(f'visual-{mode}', ['xvfb-run', '-a', '-s', '-screen 0 1600x1000x24',
                                          'app/build/pinhole', '--library', str(library)], env=test_env, timeout=90)

    def package(self):
        """The package the recipe in pkgbuild/ makes, of this source as it is
        (its edits too), built as the AUR's helpers build it and looked into."""
        with tempfile.TemporaryDirectory(prefix='pinhole-ci-package-') as temporary:
            scratch = Path(temporary)
            source, recipe = scratch / 'source', scratch / 'recipe'
            git = ['git', '-c', 'user.name=Pinhole CI', '-c', 'user.email=ci@localhost', '-c', 'safe.directory=*']
            subprocess.run([*git, 'clone', '--quiet', '--no-hardlinks', str(ROOT), str(source)], check=True)
            subprocess.run([sys.executable, str(ROOT / 'ci/export-source.py'), str(ROOT), str(source)], check=True)
            subprocess.run([*git, '-C', str(source), 'add', '-A'], check=True)
            subprocess.run([*git, '-C', str(source), 'commit', '--quiet', '--allow-empty', '-m', 'Checked'], check=True)
            recipe.mkdir()
            text = (ROOT / 'pkgbuild/PKGBUILD').read_text()
            published = 'git+https://github.com/ncr/pinhole.git'
            if published not in text:
                raise RuntimeError('The recipe no longer builds from ' + published)
            (recipe / 'PKGBUILD').write_text(text.replace(published, f'git+file://{source}'))
            self.run('package-build', ['makepkg', '--nodeps', '--noconfirm', '--noprogressbar'],
                     cwd=recipe, timeout=3600)
            built = [path for path in recipe.glob('*.pkg.tar.*') if '-debug-' not in path.name]
            if len(built) != 1:
                raise RuntimeError(f'makepkg made {len(built)} packages, not one')
            names = subprocess.check_output(['bsdtar', '-tf', str(built[0])], text=True).splitlines()
            (self.output / 'package-files.txt').write_text('\n'.join(sorted(names)) + '\n')
            metadata = subprocess.check_output(['bsdtar', '-xOf', str(built[0]), 'usr/share/pinhole/update.json'],
                                               text=True)
            validate_package(names, metadata, str(scratch), package_expectations(ROOT))
            print('[package] looked into: everything there, nothing told', flush=True)

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
                # What should be at once still is, with a library and plugin
                # data of years (People's names of 20,000 faces): a cost that
                # grows with what a person has never shows with an empty
                # profile. Budgets for two cores drawing in software, far
                # above the desktop's; the 34-second start it was made for
                # fails them by far.
                self.run('speed', ['app/bin/test-speed', '--display', 'weston', '--photos', '10000', '--faces', '20000',
                                   '--runs', '3', '--output', str(self.output / 'speed'),
                                   '--max-first-frame-ms', '3000', '--max-plugin-ms', '400',
                                   '--max-plugins-ms', '1200', '--max-still-ms', '600'], env=env, timeout=1200)
            finally:
                compositor.terminate()
                try: compositor.wait(timeout=3)
                except subprocess.TimeoutExpired: compositor.kill(); compositor.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['rust', 'ui', 'visual', 'native', 'package', 'all'])
    parser.add_argument('--output', type=Path, default=ROOT / '.ci-results/local')
    args = parser.parse_args()
    check = Checks(args.output)
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
        if args.mode in ('package', 'all'): check.package()
        return 0
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        check.save()


if __name__ == '__main__':
    raise SystemExit(main())
