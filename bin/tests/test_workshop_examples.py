"""Published recipes must be the complete, executable example plugins."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WorkshopExamplesTests(unittest.TestCase):
    def test_three_recipes_match_the_runnable_plugins_file_for_file(self):
        document = (ROOT / 'docs/plugin-examples.md').read_text()
        recipes = [('View: Years', 'years'), ('Tool: Swatch', 'ask'), ('Source: JPEG folder', 'jpegs')]
        for heading, folder in recipes:
            with self.subTest(example=heading):
                section = document.split('## ' + heading + '\n', 1)[1].split('\n## ', 1)[0]
                files = ['manifest.json', 'Main.qml', 'i18n/pl.json']
                if folder == 'jpegs':
                    files.append('pinhole-jpegs')
                for filename in files:
                    block = re.search(r'\*\*' + re.escape(filename) + r'\*\*\s+```\w+\n(.*?)\n```', section, re.S)
                    self.assertIsNotNone(block, filename)
                    expected = (ROOT / 'app/tests/examples' / folder / filename).read_text().rstrip()
                    self.assertEqual(block.group(1), expected, filename)
                manifest = json.loads((ROOT / 'app/tests/examples' / folder / 'manifest.json').read_text())
                self.assertIn('pinhole plugin check ' + manifest['id'], section)
                self.assertIn('/' + manifest['id'] + '/', section)
                self.assertIn('**Dependencies:**', section)



if __name__ == '__main__':
    unittest.main()
