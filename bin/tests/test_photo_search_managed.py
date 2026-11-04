"""Read-only catalog updates for the daemon-owned search worker."""
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from photo_search.managed import Catalog


class ManagedCatalogTests(unittest.TestCase):
    def test_new_photos_pause_and_source_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            settings = root / 'plugins.json'
            writer = sqlite3.connect(root / 'library.sqlite')
            self.addCleanup(writer.close)
            writer.executescript("""
                CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);
                INSERT INTO meta VALUES ('source','folder:test');
                CREATE TABLE assets(id TEXT,thumb_path TEXT,is_video INTEGER);
                INSERT INTO assets VALUES ('a','thumbs/a.jpg',0),('video','thumbs/v.jpg',1);
            """)
            catalog = Catalog(SimpleNamespace(library=root, managed_plugins=settings, library_key='folder:test'))
            self.addCleanup(catalog.close)
            worker = Mock(running=False)
            def poll():
                catalog.next_poll = 0
                catalog.poll(worker)

            poll()
            worker.request.assert_called_once_with({'op': 'catalog', 'assets': [{'id': 'a', 'path': 'thumbs/a.jpg'}],
                                                    'auto_index': True})
            self.assertTrue(worker.catalog_ready)
            poll()
            worker.request.assert_called_once()
            with self.assertRaises(sqlite3.OperationalError):
                catalog.db.execute("DELETE FROM assets")

            with writer:
                writer.execute("INSERT INTO assets VALUES ('b','thumbs/b.jpg',0)")
            settings.write_text(json.dumps({'settings': {'me.semantic-search': {'indexingPaused': True}}}))
            poll()
            self.assertEqual(worker.request.call_args.args[0]['assets'][-1]['id'], 'b')
            self.assertFalse(worker.request.call_args.args[0]['auto_index'])
            with writer:
                writer.execute("UPDATE meta SET value='folder:other'")
            poll()
            self.assertFalse(worker.catalog_ready)
            worker.stop.assert_called_once()
            self.assertEqual(worker.request.call_count, 2)
