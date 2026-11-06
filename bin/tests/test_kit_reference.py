"""docs/plugin-kit.md says what the kit's QML files have now."""
from pathlib import Path
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "kit-reference"


class KitReferenceTests(unittest.TestCase):
    def test_up_to_date(self):
        result = subprocess.run([str(SCRIPT), "--check"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
