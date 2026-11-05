"""Offline numerical and download bounds for the experimental ANE lowering."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

loader = importlib.machinery.SourceFileLoader("photo_search_ane", str(Path(__file__).resolve().parents[1] / "photo-search-ane"))
spec = importlib.util.spec_from_loader(loader.name, loader)
ane = importlib.util.module_from_spec(spec)
loader.exec_module(ane)
NUMPY = importlib.util.find_spec("numpy") is not None
COMPILER = os.environ.get("PINHOLE_TEST_ANE_COMPILER")


class BoundedRangeTests(unittest.TestCase):
    def test_ignored_or_incorrect_ranges_fail_without_reading_checkpoint(self):
        from unittest.mock import MagicMock
        for status, header in [(200, ""), (206, "bytes 0-7/100"), (206, "broken")]:
            response = MagicMock(status=status, headers={"Content-Range": header})
            response.__enter__.return_value = response
            with patch.object(ane.urllib.request, "urlopen", return_value=response):
                with self.assertRaises(ValueError):
                    ane.range_read("https://example.invalid/weights", 8, 15)
            response.read.assert_not_called()

    def test_short_and_oversized_range_responses_fail(self):
        from unittest.mock import MagicMock
        for value in [bytes(7), bytes(9)]:
            response = MagicMock(status=206, headers={"Content-Range": "bytes 8-15/100"})
            response.__enter__.return_value = response
            response.read.return_value = value
            with patch.object(ane.urllib.request, "urlopen", return_value=response):
                with self.assertRaises(ValueError):
                    ane.range_read("https://example.invalid/weights", 8, 15)
        with self.assertRaises(ValueError):
            ane.range_read("https://example.invalid/weights", 0, 16 << 20)

    def test_rows_cover_every_position_once_with_supported_shapes(self):
        for count in range(258):
            blocks = ane.row_blocks(count)
            self.assertTrue(all(size in (1, 2, 8, 64) for _, size in blocks))
            self.assertEqual([i for offset, size in blocks for i in range(offset, offset + size)], list(range(count)))


@unittest.skipUnless(NUMPY, "optional ANE experiment needs NumPy")
class ProjectionInputTests(unittest.TestCase):
    def test_stale_smoke_receipt_prevents_any_device_dispatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt = root / "smoke.json"
            receipt.write_text(json.dumps({"boot_id": "a different boot"}))
            with patch.object(ane.subprocess, "run") as dispatch:
                with self.assertRaisesRegex(ValueError, "passing M2 Pro smoke"):
                    ane.verify(root, root, root / "report.json", runner="/usr/bin/true", smoke_receipt=receipt)
            dispatch.assert_not_called()

    def test_pixel_layout_retains_rgb_patch_and_grid_order(self):
        import numpy as np
        image = np.arange(3 * 224 * 224, dtype=np.float32).reshape(3, 224, 224)
        for size in [16, 32]:
            actual = ane.patch_rows(image, size)
            windows = np.lib.stride_tricks.sliding_window_view(image, (size, size), axis=(1, 2))[:, ::size, ::size]
            expected = windows.transpose(1, 2, 0, 3, 4).reshape(-1, 3 * size * size)
            np.testing.assert_array_equal(actual, expected)
        image[0, 0, 0] = np.nan
        with self.assertRaises(ValueError):
            ane.patch_rows(image, 16)


@unittest.skipUnless(NUMPY and COMPILER, "set PINHOLE_TEST_ANE_COMPILER for real compiler integration (no device)")
class CompilerIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import numpy as np
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.weights = cls.root / "weights"
        cls.weights.mkdir()
        rng = np.random.default_rng(24)
        # Very different filter magnitudes expose missing/wrong unscaling.
        cls.weight = (rng.normal(0, .05, (768, 3, 16, 16)) *
                      np.geomspace(1e-5, 1, 768)[:, None, None, None]).astype(np.float32)
        cls.weight[0] = 0
        cls.bias = rng.normal(0, .02, 768).astype(np.float32)
        np.savez(cls.weights / "projection.npz", weight=cls.weight, bias=cls.bias)
        metadata = ane.MODELS["siglip2"] | {"schema": 1, "model": "siglip2",
                     "weights_sha256": ane.sha((cls.weights / "projection.npz").read_bytes())}
        ane.write_json(cls.weights / "source.json", metadata)
        cls.plan_dir = cls.root / "compiled"
        cls.plan = ane.lower(cls.weights, COMPILER, cls.plan_dir)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_real_compiler_constants_scaling_bias_and_full_projection(self):
        report = ane.verify(self.weights, self.plan_dir, self.root / "verified.json")
        self.assertTrue(report["pass"])
        self.assertFalse(report["device_execution"])
        self.assertLess(report["results"][0]["relative_l2"], .002)
        self.assertTrue(all(x > 0 for x in self.plan["weight_scales"]))
        self.assertEqual(self.plan["ane_dispatches_per_image"], 5)

    def test_compiled_constant_corruption_is_rejected(self):
        p = self.plan["programs"][0]
        binary = self.plan_dir / p["path"] / "compiled/program-0.anec"
        original = binary.read_bytes()
        try:
            binary.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
            with self.assertRaises(ValueError):
                ane.compiled_matrix(self.plan_dir, p)
        finally:
            binary.write_bytes(original)

    def test_dispatch_failure_stops_without_cpu_fallback_and_keeps_evidence(self):
        import numpy as np
        p = self.plan["programs"][0]
        matrix = ane.compiled_matrix(self.plan_dir, p)
        failed = ane.subprocess.CompletedProcess([], 1, "numerical FAIL", "")
        with patch.object(ane.subprocess, "run", return_value=failed) as call:
            with self.assertRaisesRegex(ValueError, "stop without fallback"):
                ane.device_matmul(self.plan_dir, p, np.zeros((p["rows"], p["reduction_tile"]), dtype=np.float16),
                                  matrix, "unused-test-runner", [])
        self.assertEqual(call.call_count, 1)
        logs = list(self.plan_dir.glob("failed-dispatch-*/runner.log"))
        self.assertEqual(len(logs), 1)
        self.assertIn("numerical FAIL", logs[0].read_text())

    def test_wrong_model_identity_is_rejected_before_projection(self):
        wrong = self.root / "wrong-plan"
        wrong.mkdir()
        plan = json.loads(json.dumps(self.plan))
        plan["source"]["revision"] = "different"
        ane.write_json(wrong / "plan.json", plan)
        with self.assertRaises(ValueError):
            ane.verify(self.weights, wrong, self.root / "wrong-report.json")


if __name__ == "__main__":
    unittest.main()
