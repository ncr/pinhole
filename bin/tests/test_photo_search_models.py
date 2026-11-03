"""Full encoder adapter and resident-lowering contracts without a device."""
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
NUMPY = importlib.util.find_spec("numpy") is not None
TORCH = importlib.util.find_spec("torch") is not None
COMPILER = os.environ.get("PINHOLE_TEST_ANE_COMPILER")


@unittest.skipUnless(NUMPY, "optional models experiment requires NumPy")
class ComparisonTests(unittest.TestCase):
    def test_normalization_rejects_invalid_embeddings(self):
        import numpy as np
        from photo_search.models import normalize
        np.testing.assert_allclose(normalize([[3., 4.]]), [[.6, .8]], rtol=1e-6)
        for value in [[[0., 0.]], [[np.nan, 1.]], [[np.inf, 1.]]]:
            with self.assertRaises(ValueError):
                normalize(value)

    def test_overlap_is_agreement_and_rejects_changed_queries(self):
        from photo_search.compare import overlap
        a = {"corpus_id": "same", "queries": {"q": "pies"}, "rankings": {"q": ["a", "b", "c"]}}
        b = {"corpus_id": "same", "queries": {"q": "pies"}, "rankings": {"q": ["b", "c", "a"]}}
        result = overlap(a, b, 2)
        self.assertEqual(result["mean_shared"], 1)
        self.assertEqual(result["same_set_queries"], 0)
        b["queries"] = {"q": "kot"}
        with self.assertRaises(ValueError):
            overlap(a, b, 2)

    def test_resident_runtime_rejects_stale_smoke_before_loading_library(self):
        from photo_search.ane import NativeRuntime
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = root / "smoke.json"
            receipt.write_text(json.dumps({"boot_id": "old"}))
            with patch("photo_search.ane.ctypes.CDLL") as load:
                with self.assertRaises(ValueError):
                    NativeRuntime(root / "unused.so", root, receipt)
            load.assert_not_called()

    def test_closed_runtime_cannot_submit_again(self):
        from photo_search.ane import NativeRuntime
        runtime = NativeRuntime.__new__(NativeRuntime)
        runtime.closed, runtime.failed = True, False
        with self.assertRaises(RuntimeError):
            runtime.call(None, None, None, None)

    def test_quarantine_blocks_this_boot_before_loading_a_device_library(self):
        from photo_search.ane import NativeRuntime, assert_device_healthy
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
            receipt = root / 'smoke.json'
            receipt.write_text(json.dumps({'boot_id': boot}))
            health = root / 'ane-device-health.json'
            health.write_text(json.dumps({'boot_id': boot, 'quarantined': True}))
            with patch('photo_search.ane.ctypes.CDLL') as load:
                with self.assertRaisesRegex(RuntimeError, 'reboot'):
                    NativeRuntime(root / 'unused.so', root / 'tiles', receipt)
            load.assert_not_called()
            assert_device_healthy(health, 'another-boot')


@unittest.skipUnless(NUMPY and COMPILER, "set PINHOLE_TEST_ANE_COMPILER for offline compiler integration")
class ResidentLoweringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from photo_search.ane import hash_file
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.compiler = Path(COMPILER).resolve()
        cls.compiler_hash = hash_file(cls.compiler)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_padding_tiling_scaling_and_bias_across_both_matrix_axes(self):
        import numpy as np
        from photo_search.ane import Linear, SimulatedRuntime
        rng = np.random.default_rng(2026)
        weight = rng.normal(0, .02, (1031, 1027)).astype(np.float32)
        weight *= np.geomspace(1e-5, 1, 1031).astype(np.float32)[:, None]
        weight[0] = 0
        bias = rng.normal(0, .01, 1031).astype(np.float32)
        value = rng.normal(0, .5, (2, 33, 1027)).astype(np.float32)
        layer = Linear(weight, bias, self.root, self.compiler, self.compiler_hash)
        runtime = SimulatedRuntime()
        actual, expected = layer(value, runtime), value @ weight.T + bias
        self.assertEqual(actual.shape, expected.shape)
        self.assertEqual(runtime.metrics["calls"], 8)
        self.assertLess(np.linalg.norm(actual - expected) / np.linalg.norm(expected), .002)
        np.testing.assert_array_equal(actual[..., 0], np.full((2, 33), bias[0]))

    def test_cache_corruption_is_rejected_without_silent_recompile(self):
        import numpy as np
        from photo_search.ane import compile_tile
        matrix = np.zeros((1024, 1024), dtype=np.float16)
        binary, _, _ = compile_tile(matrix, self.root, self.compiler, self.compiler_hash)
        original = binary.read_bytes()
        try:
            binary.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
            with self.assertRaisesRegex(ValueError, "corruption"):
                compile_tile(matrix, self.root, self.compiler, self.compiler_hash)
        finally:
            binary.write_bytes(original)

    def test_overflowing_inputs_are_rejected_before_the_runtime(self):
        import numpy as np
        from photo_search.ane import Linear, SimulatedRuntime
        layer = Linear(np.ones((1, 1), dtype=np.float32), None, self.root, self.compiler, self.compiler_hash)
        runtime = SimulatedRuntime()
        with np.errstate(over="ignore"):
            with self.assertRaisesRegex(ValueError, "overflows"):
                layer(np.full((1, 1), 1e10, dtype=np.float32), runtime)
        self.assertEqual(runtime.metrics["calls"], 0)


@unittest.skipUnless(NUMPY and TORCH and COMPILER, "full adapter tests need optional torch and the compiler")
class FullEncoderAdapterTests(unittest.TestCase):
    def test_tiny_clip_and_siglip_hooks_match_reference_and_restore(self):
        import numpy as np
        import torch
        from transformers import CLIPConfig, CLIPModel, SiglipConfig, SiglipModel
        from photo_search.ane import SimulatedRuntime
        from photo_search.models import ImageModel
        torch.set_num_threads(2)
        vision = {"hidden_size": 32, "intermediate_size": 64, "num_hidden_layers": 1,
                  "num_attention_heads": 4, "image_size": 224, "patch_size": 16}
        text = {"hidden_size": 32, "intermediate_size": 64, "num_hidden_layers": 1,
                "num_attention_heads": 4, "vocab_size": 128}
        for name, model_type, config_type in [("clip", CLIPModel, CLIPConfig), ("siglip2", SiglipModel, SiglipConfig)]:
            with self.subTest(model=name), tempfile.TemporaryDirectory() as temporary:
                torch.manual_seed(42)
                model = ImageModel.__new__(ImageModel)
                model.name = name
                model.model = model_type(config_type(vision_config=vision, text_config=text, projection_dim=16,
                                                      _attn_implementation="eager")).eval()
                model.model.requires_grad_(False)
                model.originals, model.prepared = [], []
                pixels = torch.randn(1, 3, 224, 224)
                reference = model.encode(pixels)
                runtime = SimulatedRuntime()
                model.hybrid(runtime, Path(temporary), Path(COMPILER).resolve(), patch_backend="ane")
                actual = model.encode(pixels)
                cosine = float(np.sum(reference * actual) / (np.linalg.norm(reference) * np.linalg.norm(actual)))
                self.assertGreater(cosine, .9999)
                self.assertGreater(runtime.metrics["calls"], 0)
                model.restore()
                np.testing.assert_array_equal(model.encode(pixels), reference)
                model.prepared = []
                cpu_patch_runtime = SimulatedRuntime()
                model.hybrid(cpu_patch_runtime, Path(temporary), Path(COMPILER).resolve(), patch_backend="cpu")
                actual_cpu_patch = model.encode(pixels)
                self.assertGreater(float(np.sum(reference * actual_cpu_patch) /
                                         (np.linalg.norm(reference) * np.linalg.norm(actual_cpu_patch))), .9999)
                self.assertFalse(any(x["name"].endswith("patch_embedding") for x in model.prepared))
                self.assertLess(cpu_patch_runtime.metrics["calls"], runtime.metrics["calls"])
                model.restore()


if __name__ == "__main__":
    unittest.main()
