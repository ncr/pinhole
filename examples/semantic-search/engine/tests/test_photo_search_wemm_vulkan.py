"""Exercise the Vulkan adapter's multimodal wire format without a model."""
import base64
import sys
from pathlib import Path
import unittest
import tempfile
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from photo_search import wemm_vulkan
from photo_search.wemm_vulkan import EMBEDDING_TOKEN, WeMMVulkan


class WeMMVulkanTests(unittest.TestCase):
    def setUp(self):
        self.model = WeMMVulkan.__new__(WeMMVulkan)
        self.model.marker = "<image-marker>"
        self.model.call = Mock()
        self.model.vector = Mock()

    def test_text_embeds_explicit_reference_tokens(self):
        tokens = [248045, 846, 198, 55, 248046, EMBEDDING_TOKEN]
        self.model.call.return_value = {"tokens": tokens}
        self.model.text("pies")
        self.model.call.assert_called_once_with("/tokenize", {
            "content": "<|im_start|>user\npies<|im_end|><embedding>",
            "add_special": False, "parse_special": True,
        })
        self.model.vector.assert_called_once_with(tokens)

    def test_image_sends_pixels_and_media_marker_to_embedding(self):
        image = Mock()
        image.save.side_effect = lambda stream, format: stream.write(b"fixture-image")
        self.model.image(image)
        self.model.vector.assert_called_once_with({
            "prompt_string": "<|im_start|>user<image-marker><|im_end|><embedding>",
            "multimodal_data": [base64.b64encode(b"fixture-image").decode()],
        })


class WeightsTests(unittest.TestCase):
    def test_weights_are_hashed_once_until_they_change(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "model.gguf"
            path.write_bytes(b"weights")
            hashing = Mock(return_value="pinned")
            wemm_vulkan.verify(path, "pinned", hashing)
            wemm_vulkan.verify(path, "pinned", hashing)
            self.assertEqual(hashing.call_count, 1)
            path.write_bytes(b"other weights")
            hashing.return_value = "other"
            with self.assertRaisesRegex(ValueError, "pinned hash"):
                wemm_vulkan.verify(path, "pinned", hashing)
