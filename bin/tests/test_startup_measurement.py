"""Host-only timing must never substitute or launch a real photo service."""
import json
import os
from pathlib import Path
import subprocess
import runpy
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StartupMeasurementTests(unittest.TestCase):
    def test_a_plugin_over_its_budget_fails_the_measurement_and_synced_data_reaches_the_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / 'fake' / 'speed'
            library.mkdir(parents=True)
            (library / 'library.sqlite').touch()
            synced = root / 'synced'
            synced.mkdir()
            (synced / 'pinhole.people.json').write_text('{"values": {}, "clock": {}}')
            binary = root / 'slow-plugin-host'
            binary.write_text('''#!/usr/bin/python3
import os,sys,json
seen = os.path.isfile(os.path.join(os.environ['XDG_STATE_HOME'], 'pinhole/synced/fake-speed/pinhole.people.json'))
for event, ms in [('index_read',5),('grid_ready',10),('plugins_ready',40 if seen else 1),('first_frame',50),('media_ready',60)]:
 print('pinhole startup: '+json.dumps({'event':event,'ms':ms}),file=sys.stderr,flush=True)
print('pinhole plugin startup: '+json.dumps({'id':'pinhole.people','ms':30 if seen else 0}),file=sys.stderr,flush=True)
''')
            binary.chmod(0o700)
            command = ['/usr/bin/python3', str(ROOT / 'app/bin/measure-startup'), '--library', str(library),
                       '--runs', '1', '--warmup', '0', '--native-hidden', '--host-only', '--binary', str(binary),
                       '--synced', str(synced)]
            within = subprocess.run(command + ['--output', str(root / 'within'), '--max-plugin-ms', '50',
                                               '--max-plugins-ms', '50', '--max-first-frame-ms', '100'],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(within.returncode, 0, within.stdout + within.stderr)
            over = subprocess.run(command + ['--output', str(root / 'over'), '--max-plugin-ms', '20'],
                                  capture_output=True, text=True, timeout=10)
            self.assertEqual(over.returncode, 1, over.stdout + over.stderr)
            self.assertIn('plugin pinhole.people 30 ms > 20 ms', over.stderr)
            report = json.loads((root / 'over' / 'results.json').read_text())
            self.assertEqual(report['plugins_median_ms'], {'pinhole.people': 30})
            self.assertEqual(report['over_budget'], ['plugin pinhole.people 30 ms > 20 ms'])

    @unittest.skipUnless(hasattr(os, 'sched_getaffinity') and shutil.which('taskset'), 'requires Linux taskset')
    def test_explicit_affinity_reaches_only_the_measured_child_and_is_recorded(self):
        allowed = os.sched_getaffinity(0)
        cpu = min(allowed)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'library.sqlite').touch()
            binary = root / 'affinity-host'
            binary.write_text(f'''#!/usr/bin/python3
import os,sys,json
assert os.sched_getaffinity(0) == {{{cpu}}}
for event in ['index_read','plugins_ready','first_frame','media_ready']:
 print('pinhole startup: '+json.dumps({{'event':event,'ms':10}}),file=sys.stderr,flush=True)
''')
            binary.chmod(0o700)
            output = root / 'results'
            result = subprocess.run(['/usr/bin/python3', str(ROOT / 'app/bin/measure-startup'),
                '--library', str(root), '--output', str(output), '--runs', '1', '--warmup', '0',
                '--native-hidden', '--host-only', '--binary', str(binary), '--cpu-affinity', str(cpu)],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((output / 'results.json').read_text())
            self.assertEqual(report['cpu_affinity'], [cpu])
            self.assertEqual(report['runs'][0]['cpu_affinity_requested'], [cpu])
            self.assertEqual(os.sched_getaffinity(0), allowed)
            parse = runpy.run_path(str(ROOT / 'app/bin/measure-startup'))['cpu_affinity']
            for invalid in ['', '-1', '0-', '2-1', str(max(allowed) + 1), '0;true']:
                with self.assertRaises(ValueError):
                    parse(invalid)

    def test_host_only_has_no_daemon_and_records_the_missing_service(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            library = root / 'library'
            library.mkdir()
            (library / 'library.sqlite').write_bytes(b'fixture: the fake Qt host never opens this')
            binary = root / 'fake-qt-host'
            binary.write_text('''#!/usr/bin/python3
import json,os,sys
from pathlib import Path
assert Path(os.environ['PINHOLED']).name == 'false'
assert not Path(os.environ['PINHOLE_SOCKET']).exists()
assert os.environ['PINHOLE_STARTUP_FRAME_ONLY'] == '1'
for event,ms in [('index_read',10),('plugins_ready',20),('first_frame',30),('media_ready',40)]:
    print('pinhole startup: '+json.dumps({'event':event,'ms':ms}),file=sys.stderr,flush=True)
''')
            binary.chmod(0o700)
            output = root / 'measurements'
            result = subprocess.run(['/usr/bin/python3', str(ROOT / 'app/bin/measure-startup'),
                '--library', str(library), '--output', str(output), '--runs', '1', '--warmup', '0',
                '--native-hidden', '--host-only', '--binary', str(binary)],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((output / 'results.json').read_text())
            self.assertEqual(report['service_mode'], 'unavailable')
            self.assertIsNone(report['runs'][0]['service_boot_ms'])
            self.assertEqual(report['runs'][0]['service_mode'], 'unavailable')
            self.assertEqual(report['summary']['first_frame']['median_ms'], 30)
            self.assertFalse((output / '0-daemon.log').exists())


if __name__ == '__main__':
    unittest.main()
