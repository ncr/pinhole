"""Derived-index, resumption and source-boundary tests; no accelerator."""
import importlib.util
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
NUMPY = importlib.util.find_spec('numpy') is not None


@unittest.skipUnless(NUMPY, 'optional search prototype requires NumPy')
class StoreTests(unittest.TestCase):
    def setUp(self):
        import numpy as np
        from photo_search.store import Store
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.store = Store(self.root / 'index.sqlite', 'source-a', {'model': 'test'}, 2)
        self.a, self.b = np.array([1., 0.]), np.array([0., 1.])
        self.store.catalog([{'id': 'a', 'path': 'a.jpg'}, {'id': 'b', 'path': 'b.jpg'}])

    def tearDown(self):
        self.store.close()
        self.temporary.cleanup()

    def test_restart_retains_vectors_and_source_identity_is_required(self):
        from photo_search.store import Store
        self.store.put('a', 'hash', self.a)
        self.store.close()
        self.store = Store(self.root / 'index.sqlite', 'source-a', {'model': 'test'}, 2)
        self.assertTrue(self.store.fresh('a', 'hash'))
        self.assertEqual(self.store.search(self.a)[0]['id'], 'a')
        with self.assertRaises(ValueError):
            Store(self.root / 'index.sqlite', 'source-b', {'model': 'test'}, 2)
        self.assertEqual((self.root / 'index.sqlite').stat().st_mode & 0o777, 0o600)

    def test_foreign_library_is_not_changed_even_by_pragmas(self):
        from photo_search.store import Store
        path = self.root / 'library.sqlite'
        with closing(sqlite3.connect(path)) as db:
            db.execute('CREATE TABLE assets(id TEXT)')
            db.commit()
        before = path.read_bytes()
        with self.assertRaises(ValueError):
            Store(path, 'source-a', {'model': 'test'}, 2)
        self.assertEqual(path.read_bytes(), before)
        self.assertFalse(path.with_name('library.sqlite-wal').exists())

    def test_catalog_changes_and_failure_hide_stale_vectors(self):
        self.store.put('a', 'hash', self.a)
        self.store.put('b', 'hash-b', self.b)
        self.assertEqual(len(self.store.search(self.a)), 2)
        self.store.catalog([{'id': 'a', 'path': 'new.jpg'}])
        self.assertEqual(self.store.status()['indexed'], 0)
        self.assertEqual(self.store.search(self.a), [])
        self.store.fingerprint('a', 'hash')
        self.assertEqual(self.store.search(self.a)[0]['id'], 'a')
        self.store.fail('a', 'missing')
        self.assertEqual(self.store.search(self.a), [])
        self.store.fingerprint('a', 'hash')
        self.assertEqual(self.store.status()['errors'], 0)
        self.assertEqual(self.store.status()['indexed'], 1)

    def test_validation_and_stable_ties(self):
        import numpy as np
        for asset in ['b', 'a']:
            self.store.put(asset, asset, self.a)
        self.assertEqual([x['id'] for x in self.store.search(self.a)], ['a', 'b'])
        for value in [[0, 0], [2, 0], [np.nan, 0], [1, 0, 0]]:
            with self.assertRaises(ValueError):
                self.store.put('a', 'bad', value)
            with self.assertRaises(ValueError):
                self.store.search(value)
        with self.assertRaises(ValueError):
            self.store.put('foreign', 'hash', self.a)
        with self.assertRaises(ValueError):
            self.store.catalog([{'id': 'a', 'path': 'a'}, {'id': 'a', 'path': 'b'}])
        self.assertEqual(self.store.status()['total'], 2)


@unittest.skipUnless(NUMPY, 'optional search prototype requires NumPy')
class WorkerTests(unittest.TestCase):
    def setUp(self):
        from photo_search.worker import Worker
        from photo_search.models import PINS
        self.temporary = tempfile.TemporaryDirectory()
        root = self.root = Path(self.temporary.name)
        self.args = SimpleNamespace(model='clip', models=root, backend='cpu', index=root / 'vectors.sqlite',
                                    library_key='source-a', library=root, cache=root / 'tiles')
        with patch('photo_search.worker.identity', return_value={'image': PINS['clip'], 'processor_sha256': 'processor'}):
            self.worker = Worker(self.args)
        self.assets = [{'id': 'a', 'path': 'a.jpg'}, {'id': 'b', 'path': 'b.jpg'}]
        (root / 'a.jpg').write_bytes(b'local thumbnail a')
        (root / 'b.jpg').write_bytes(b'local thumbnail b')
        self.worker.request({'op': 'catalog', 'assets': self.assets})

    def tearDown(self):
        self.worker.close()
        self.temporary.cleanup()

    def vector(self):
        import numpy as np
        result = np.zeros(512, dtype=np.float32)
        result[0] = 1
        return result

    def test_resume_skips_same_content_and_stop_preserves_finished_work(self):
        from photo_search.ane import hash_file
        self.worker.store.put('a', hash_file(self.root / 'a.jpg'), self.vector())
        self.worker.request({'op': 'start'})
        self.worker.step()
        self.assertEqual(self.worker.skipped, 1)
        self.assertIsNone(self.worker.image_model)
        self.worker.request({'op': 'stop'})
        self.assertEqual(self.worker.status()['pending'], 0)
        self.assertEqual(self.worker.status()['indexed'], 1)

    def test_missing_and_escaping_thumbnails_never_load_models(self):
        from photo_search.worker import thumbnail
        outside = self.root / 'sub'
        outside.mkdir()
        (outside / 'escape').symlink_to(self.root / 'a.jpg')
        with self.assertRaises(ValueError):
            thumbnail(outside, 'escape')
        for path in ['../escape', '/absolute']:
            with self.assertRaises(ValueError):
                self.worker.request({'op': 'catalog', 'assets': [{'id': 'x', 'path': path}]})
        self.worker.request({'op': 'catalog', 'assets': [{'id': 'x', 'path': 'missing.jpg'}]})
        self.worker.request({'op': 'start'})
        self.worker.step()
        self.assertEqual(self.worker.status()['errors'], 1)
        self.assertIsNone(self.worker.image_model)

    def test_catalog_refresh_preserves_running_job_and_adds_new_photos_once(self):
        from photo_search.ane import hash_file
        self.worker.store.put('a', hash_file(self.root / 'a.jpg'), self.vector())
        self.worker.request({'op': 'start'})
        self.worker.step()
        assets = self.assets + [{'id': 'c', 'path': 'new.jpg'}]
        for _ in range(2):
            self.worker.request({'op': 'catalog', 'assets': assets})
            self.assertTrue(self.worker.running)
            self.assertEqual(list(self.worker.pending), [('b', 'b.jpg'), ('c', 'new.jpg')])
        self.worker.request({'op': 'stop'})
        self.worker.request({'op': 'catalog', 'assets': assets})
        self.assertFalse(self.worker.running)

    def test_bad_thumbnail_is_per_photo_error_before_model_loading(self):
        with patch('photo_search.models.load_image', side_effect=ValueError('damaged image')):
            self.worker.request({'op': 'start'})
            self.worker.step()
            self.assertTrue(self.worker.running)
            self.worker.step()
        self.assertFalse(self.worker.running)
        self.assertEqual(self.worker.status()['errors'], 2)
        self.assertIsNone(self.worker.image_model)

    def test_device_failure_is_terminal_and_never_falls_back(self):
        from unittest.mock import Mock
        self.worker.image_model = Mock()
        self.worker.image_model.encode.side_effect = RuntimeError('ANE timeout')
        with patch('photo_search.models.load_image', return_value=object()):
            self.worker.request({'op': 'start'})
            with self.assertRaisesRegex(RuntimeError, 'ANE timeout'):
                self.worker.step()
        self.assertFalse(self.worker.running)
        self.assertEqual(self.worker.status()['indexed'], 0)
        self.assertEqual(self.worker.status()['error'], 'ANE timeout')
        self.worker.step()
        self.worker.image_model.encode.assert_called_once()

    def test_quarantined_boot_rejects_start_but_keeps_queries_available(self):
        self.args.backend = 'hybrid-ane'
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        (self.root / 'ane-device-health.json').write_text(json.dumps({'boot_id': boot, 'quarantined': True}))
        with self.assertRaisesRegex(RuntimeError, 'reboot'):
            self.worker.request({'op': 'start'})
        self.assertFalse(self.worker.running)
        self.assertIsNone(self.worker.image_model)
        self.assertEqual(self.worker.request({'op': 'query', 'text': 'pies'})['results'], [])

    def test_import_maps_opaque_ids_and_rejects_changed_thumbnail(self):
        import numpy as np
        from photo_search.ane import hash_file
        root = self.root
        opaque = hashlib.sha256(b'corpus-key\0a').hexdigest()
        run = root / 'run'
        run.mkdir()
        (run / 'run.json').write_text(json.dumps({'model': 'clip', 'backend': 'cpu', 'patch_backend': 'cpu',
            'corpus_id': 'frozen', 'source': self.worker.identity['image'] | {'processor_sha256': 'processor'}}))
        (run / 'image-ids.json').write_text(json.dumps([opaque]))
        np.save(run / 'images.npy', self.vector()[None, :])
        frozen = root / 'corpus.json'
        frozen.write_text(json.dumps({'corpus_id': 'frozen', 'library_key': 'corpus-key',
            'items': [{'id': opaque, 'sha256': hash_file(root / 'a.jpg')}]}))
        result = self.worker.request({'op': 'import', 'run': str(run), 'corpus': str(frozen)})
        self.assertEqual(result['imported'], 1)
        self.assertEqual(self.worker.store.search(self.vector())[0]['id'], 'a')
        (root / 'a.jpg').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'changed'):
            self.worker.import_run(run, frozen)

    def test_queries_reuse_text_encoder_and_empty_index_does_not_load_it(self):
        from unittest.mock import Mock
        self.assertEqual(self.worker.request({'op': 'query', 'text': 'pies'})['results'], [])
        self.assertIsNone(self.worker.text_model)
        from photo_search.ane import hash_file
        self.worker.store.put('a', hash_file(self.root / 'a.jpg'), self.vector())
        model = Mock()
        model.encode.return_value = self.vector()[None, :], {}
        with patch('photo_search.models.TextModel', return_value=model) as load:
            for query in ['pies', 'kot']:
                self.assertEqual(self.worker.request({'op': 'query', 'text': query})['results'][0]['id'], 'a')
        load.assert_called_once()
        self.assertEqual(model.encode.call_count, 2)

    def test_wemm_reuses_one_encoder_for_images_and_both_query_languages(self):
        import numpy as np
        from unittest.mock import Mock, call
        from photo_search.worker import Worker
        from photo_search.wemm import DIMENSION

        args = SimpleNamespace(**(vars(self.args) | {
            'model': 'wemm', 'backend': 'cuda', 'index': self.root / 'wemm.sqlite',
        }))
        encoder = Mock()
        vector = np.zeros((1, DIMENSION), dtype=np.float32)
        vector[0, 0] = 1
        encoder.images.return_value = encoder.texts.return_value = vector
        image = object()
        with closing(Worker(args)) as worker, \
                patch('photo_search.wemm.WeMM', return_value=encoder) as load, \
                patch('photo_search.models.load_image', return_value=image):
            worker.request({'op': 'catalog', 'assets': self.assets[:1]})
            self.assertEqual(worker.request({'op': 'query', 'text': 'pies'})['results'], [])
            load.assert_not_called()
            worker.request({'op': 'start'})
            worker.step()
            self.assertEqual(worker.status()['dimension'], DIMENSION)
            for query in ('pies na trawie', 'dog on the grass'):
                self.assertEqual(worker.request({'op': 'query', 'text': query})['results'][0]['id'], 'a')
            load.assert_called_once_with(args.models)
            encoder.images.assert_called_once_with([image])
            self.assertEqual(encoder.texts.call_args_list, [call(['pies na trawie']), call(['dog on the grass'])])
        args.index = self.args.index
        with self.assertRaisesRegex(ValueError, 'another index'):
            Worker(args)

    def test_same_path_thumbnail_replacement_is_excluded_before_query(self):
        from photo_search.ane import hash_file
        self.worker.store.put('a', hash_file(self.root / 'a.jpg'), self.vector())
        self.worker.refresh()
        (self.root / 'a.jpg').write_bytes(b'replaced in place')
        result = self.worker.request({'op': 'query', 'text': 'pies'})
        self.assertEqual(result['results'], [])
        self.assertEqual(self.worker.status()['errors'], 1)
        self.assertIsNone(self.worker.text_model)

    def test_tag_suggestions_rank_cached_vectors_and_recheck_the_photo(self):
        import numpy as np
        from unittest.mock import Mock
        from photo_search.ane import hash_file
        from photo_search.worker import TAGS

        request = {'op': 'suggest_tags', 'id': 'a'}
        self.assertEqual(self.worker.request(request), [])
        self.assertIsNone(self.worker.text_model)
        self.worker.store.put('a', hash_file(self.root / 'a.jpg'), self.vector())
        self.args.backend = 'hybrid-ane'
        boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
        (self.root / 'ane-device-health.json').write_text(json.dumps({'boot_id': boot, 'quarantined': True}))
        vectors = np.tile(-self.vector(), (len(TAGS), 1))
        vectors[-1] = self.vector()
        self.worker.text_model = Mock()
        self.worker.text_model.encode.return_value = vectors, {}
        for _ in range(2):
            tags = self.worker.request(request)
            self.assertEqual(tags[0], {'id': TAGS[-1][0], 'label': TAGS[-1][1]})
            self.assertEqual(len(tags), 5)
        self.worker.text_model.encode.assert_called_once_with([prompt for _, _, prompt in TAGS])
        self.assertIsNone(self.worker.image_model)
        self.assertIsNone(self.worker.runtime)
        (self.root / 'a.jpg').write_bytes(b'replaced in place')
        self.assertEqual(self.worker.request(request), [])
        self.assertEqual(self.worker.status()['errors'], 1)


if __name__ == '__main__':
    unittest.main()
