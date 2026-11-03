"""Host-only timing must never substitute or launch a real photo service."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StartupMeasurementTests(unittest.TestCase):
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
