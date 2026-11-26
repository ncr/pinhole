"""WeMM on the NVIDIA card where it fits, else on the processor, and the
plain line the panels say about it. No model or card is used."""
from contextlib import closing
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from photo_search import wemm  # noqa: E402
from photo_search.wemm import Card  # noqa: E402

NUMPY = importlib.util.find_spec('numpy') is not None
PLUGIN = Path(__file__).resolve().parents[2]
GIB = 1 << 30


def smi(stdout='', returncode=0, stderr=''):
    return Mock(return_value=subprocess.CompletedProcess([], returncode, stdout, stderr))


class OutOfMemoryError(RuntimeError):
    """As torch.cuda.OutOfMemoryError is named."""


class CardTests(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop('CUDA_VISIBLE_DEVICES', None)

    def test_what_nvidia_smi_says_of_the_card(self):
        line = 'NVIDIA GeForce RTX 5080, 12.0, 16303, {free}\n'
        cases = [
            (line.format(free=12000), None),
            # The card on 2026-11-26: Superwhisper, browsers and the desktop
            # holding half of it.
            (line.format(free=7979), 'busy'),
            ('NVIDIA GeForce RTX 2080, 7.5, 8192, 8000\n', 'bf16'),
            ('NVIDIA GeForce RTX 4060, 8.9, 8188, 8000\n', 'small'),
        ]
        for stdout, reason in cases:
            with self.subTest(stdout=stdout):
                found = wemm.card(smi(stdout))
                self.assertEqual(found.reason, reason)
                self.assertIn('NVIDIA GeForce', found.detail)

    def test_no_nvidia_card_or_driver_is_said_as_such(self):
        missing = Mock(side_effect=FileNotFoundError(2, 'No such file or directory', 'nvidia-smi'))
        failing = smi(returncode=9, stdout="NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver.")
        for run in (missing, failing, smi(''), smi('garbage\n')):
            with self.subTest(run=run):
                self.assertEqual(wemm.card(run).reason, 'driver')
        os.environ['CUDA_VISIBLE_DEVICES'] = ''
        self.assertEqual(wemm.card(smi('NVIDIA GeForce RTX 5080, 12.0, 16303, 15000\n')).reason, 'driver')

    def test_out_of_memory_is_recognised_without_torch(self):
        self.assertTrue(wemm.out_of_memory(OutOfMemoryError('CUDA out of memory. Tried to allocate 9.64 GiB.')))
        self.assertTrue(wemm.out_of_memory(RuntimeError('CUDA error: out of memory')))
        self.assertFalse(wemm.out_of_memory(ValueError('thumbnail changed during inference')))

    def test_each_reason_has_a_plain_line_in_both_languages(self):
        indexer = (PLUGIN / 'SemanticIndexer.qml').read_text()
        polish = json.loads((PLUGIN / 'i18n/pl.json').read_text())
        for reason in wemm.NO_CARD + ('busy',):
            self.assertIn(f'status.card === "{reason}"', indexer)
        lines = [line for line in indexer.split('deviceNote:')[1].split('function cancel')[0].splitlines()
                 if 'qsTr("' in line]
        self.assertEqual(len(lines), 4)
        for line in lines:
            english = line.split('qsTr("')[1].split('")')[0]
            self.assertIn('processor', english)
            self.assertNotRegex(english, 'CUDA|PyTorch|GiB|Traceback')
            self.assertIn(english, polish)


@unittest.skipUnless(NUMPY, 'requires NumPy')
class EngineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.args = SimpleNamespace(model='wemm', backend='cuda', models=root, index=root / 'wemm.sqlite',
                                    library_key='source-a', library=root, background=True, threads=2)
        quiet = patch('photo_search.engines.log')
        self.log = quiet.start()
        self.addCleanup(quiet.stop)

    def engine(self, card):
        from photo_search.engines import CudaEngine
        with patch('photo_search.wemm.card', return_value=card):
            return CudaEngine(self.args)

    def model(self, device):
        import numpy as np
        model = Mock(device=device, vision=False)
        model.texts.return_value = np.ones((1, wemm.DIMENSION), dtype=np.float32)
        model.images.return_value = np.ones((1, wemm.DIMENSION), dtype=np.float32)
        return model

    def test_a_busy_card_is_left_to_other_programs_and_search_runs_on_the_processor(self):
        busy = Card('busy', 'RTX 5080, 7.8 of 15.9 GiB free', int(7.8 * GIB), 16 * GIB)
        engine = self.engine(busy)
        self.assertEqual(engine.describe(), {'search_on': 'processor', 'card': 'busy'})
        with patch('photo_search.wemm.card', return_value=busy), \
                patch('photo_search.wemm.WeMM', side_effect=lambda path, device: self.model(device)) as load:
            engine.encode_text(['car'])
            load.assert_called_once_with(self.args.models, 'cpu')
            # Indexing waits for room on the card rather than taking it.
            self.assertFalse(engine.can_index())
        self.assertEqual(engine.describe(), {'search_on': 'processor', 'card': 'busy'})

    def test_a_card_with_room_answers_and_one_that_fills_meanwhile_hands_over_to_the_processor(self):
        fits = Card(None, 'RTX 5080, 12 of 15.9 GiB free', 12 * GIB, 16 * GIB)
        engine = self.engine(fits)
        self.assertEqual(engine.describe(), {'search_on': 'card', 'card': None})
        card = self.model('cuda')
        card.texts.side_effect = OutOfMemoryError('CUDA out of memory. Tried to allocate 20.00 MiB.')
        processor = self.model('cpu')
        with patch('photo_search.wemm.card', return_value=fits), \
                patch('photo_search.wemm.WeMM', side_effect=[card, processor]) as load:
            vectors = engine.encode_text(['car'])
        self.assertEqual(vectors.shape, (1, wemm.DIMENSION))
        self.assertEqual([c.args[1] for c in load.call_args_list], ['cuda', 'cpu'])
        self.assertEqual(engine.describe(), {'search_on': 'processor', 'card': 'busy'})
        self.assertIn('out of memory', str(self.log.call_args_list))

    def test_a_card_that_cannot_load_the_model_leaves_search_on_the_processor(self):
        fits = Card(None, 'RTX 5080', 12 * GIB, 16 * GIB)
        engine = self.engine(fits)
        processor = self.model('cpu')
        with patch('photo_search.wemm.card', return_value=fits), \
                patch('photo_search.wemm.WeMM', side_effect=[RuntimeError('CUDA driver version is insufficient'),
                                                             processor]):
            engine.encode_text(['car'])
        self.assertEqual(engine.describe(), {'search_on': 'processor', 'card': 'driver'})

    def test_without_a_suitable_card_photos_are_indexed_on_the_processor(self):
        engine = self.engine(Card('driver', 'nvidia-smi: not found'))
        self.assertEqual(engine.describe(), {'search_on': 'processor', 'card': 'driver'})
        with patch('photo_search.wemm.card', return_value=Card('driver', 'nvidia-smi: not found')), \
                patch('photo_search.wemm_vulkan.available_memory_kib', return_value=8 << 20), \
                patch('photo_search.wemm.WeMM', side_effect=lambda path, device: self.model(device)) as load:
            self.assertTrue(engine.can_index())
            engine.encode_image(object())
            engine.encode_text(['car'])
        load.assert_called_once_with(self.args.models, 'cpu')
        with patch('photo_search.wemm.card', return_value=Card('small', 'RTX 4060, 8 GiB')), \
                patch('photo_search.wemm_vulkan.available_memory_kib', return_value=1 << 20):
            self.assertFalse(engine.can_index())


@unittest.skipUnless(NUMPY, 'requires NumPy')
class WorkerTests(unittest.TestCase):
    def setUp(self):
        from photo_search.worker import Worker
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        (root / 'a.jpg').write_bytes(b'local thumbnail a')
        args = SimpleNamespace(model='wemm', backend='cuda', models=root, index=root / 'wemm.sqlite',
                               library_key='source-a', library=root, background=False, threads=2)
        quiet = patch('photo_search.engines.log')
        quiet.start()
        self.addCleanup(quiet.stop)
        with patch('photo_search.wemm.card', return_value=Card('driver', 'nvidia-smi: not found')):
            self.worker = Worker(args)
        self.addCleanup(self.worker.close)
        self.worker.request({'op': 'catalog', 'assets': [{'id': 'a', 'path': 'a.jpg'}]})

    def test_the_status_says_where_search_runs_before_anything_is_asked(self):
        status = self.worker.request({'op': 'status'})
        self.assertEqual((status['search_on'], status['card'], status['error']), ('processor', 'driver', ''))

    def test_a_card_that_fills_while_indexing_is_waited_for_not_an_error(self):
        model = Mock()
        model.images.side_effect = OutOfMemoryError('CUDA out of memory. Tried to allocate 9.64 GiB.')
        self.worker.request({'op': 'start'})
        with patch('photo_search.wemm.WeMM', return_value=model), \
                patch('photo_search.models.load_image', return_value=object()), \
                patch('sys.stderr') as log:
            self.worker.step()
        status = self.worker.status()
        self.assertEqual((status['error'], status['waiting'], status['running'], status['pending']), ('', True, True, 1))
        self.assertIn('out of memory', str(log.write.call_args_list))
        self.assertFalse(self.worker.engine.loaded())


if __name__ == '__main__':
    unittest.main()
