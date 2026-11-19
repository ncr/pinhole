"""Read-only catalog updates for the daemon-owned search worker."""
import json
import os
import socket
import threading
from pathlib import Path
import sqlite3
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from photo_search.managed import Catalog, service_manages


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
                                                    'auto_index': True, 'page': 0, 'more': False})
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

    def test_a_library_larger_than_a_page_goes_in_pages_the_last_saying_so(self):
        import photo_search.managed as managed
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            writer = sqlite3.connect(root / 'library.sqlite')
            self.addCleanup(writer.close)
            writer.executescript("""
                CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);
                INSERT INTO meta VALUES ('source','folder:test');
                CREATE TABLE assets(id TEXT,thumb_path TEXT,is_video INTEGER);
            """)
            with writer:
                writer.executemany("INSERT INTO assets VALUES (?,?,0)",
                                   [(f'{n:03}', f'thumbs/{n}.jpg') for n in range(7)])
            catalog = Catalog(SimpleNamespace(library=root, managed_plugins=root / 'plugins.json',
                                              library_key='folder:test'))
            self.addCleanup(catalog.close)
            worker = Mock(running=False)
            old, managed.CATALOG_PAGE = managed.CATALOG_PAGE, 3
            self.addCleanup(setattr, managed, 'CATALOG_PAGE', old)
            catalog.poll(worker)
            sent = [call.args[0] for call in worker.request.call_args_list]
            self.assertEqual([(m['page'], m['more'], len(m['assets'])) for m in sent],
                             [(0, True, 3), (1, True, 3), (2, False, 1)])
            self.assertEqual([a['id'] for m in sent for a in m['assets']], [f'{n:03}' for n in range(7)])


class ServiceManagesTests(unittest.TestCase):
    """The worker is the service's when its `workers` say it runs for this library."""

    def serve(self, directory, workers):
        path = Path(directory) / 's.sock'
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(path))
        server.listen()
        self.addCleanup(server.close)
        asked = []

        def answer():
            connection, _ = server.accept()
            with connection, connection.makefile('rwb') as lines:
                lines.write(b'{"event": "hello", "protocol": 1}\n')
                lines.flush()
                request = json.loads(lines.readline())
                asked.append(request)
                lines.write((json.dumps({'id': request['id'], 'ok': True, 'result': {'workers': workers}}) + '\n').encode())
                lines.flush()

        thread = threading.Thread(target=answer, daemon=True)
        thread.start()
        old = os.environ.get('PINHOLE_SOCKET')
        os.environ['PINHOLE_SOCKET'] = str(path)
        self.addCleanup(lambda: os.environ.pop('PINHOLE_SOCKET') if old is None else os.environ.update(PINHOLE_SOCKET=old))
        return asked

    def test_only_its_own_worker_for_this_library_and_source(self):
        library = '/data/library/folder/Pictures'
        mine = {'plugin': 'me.semantic-search', 'name': 'index', 'library': library, 'key': 'folder:/p', 'running': False}
        cases = [([mine], True),
                 ([dict(mine, plugin='pinhole.people')], False),
                 ([dict(mine, key='folder:/other')], False),
                 ([dict(mine, library='/data/library/folder/Other')], False),
                 ([dict(mine, library=None, key=None)], False),
                 ([], False)]
        for workers, managed in cases:
            with self.subTest(workers=workers), tempfile.TemporaryDirectory() as directory:
                asked = self.serve(directory, workers)
                self.assertIs(service_manages(Path(library), 'folder:/p'), managed)
                self.assertEqual(asked[0]['method'], 'workers')

    def test_no_service_is_not_managed(self):
        with tempfile.TemporaryDirectory() as directory:
            old = os.environ.get('PINHOLE_SOCKET')
            os.environ['PINHOLE_SOCKET'] = str(Path(directory) / 'none.sock')
            try:
                self.assertFalse(service_manages(Path('/x'), 'folder:/p'))
            finally:
                os.environ.pop('PINHOLE_SOCKET') if old is None else os.environ.update(PINHOLE_SOCKET=old)
