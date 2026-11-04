"""Exercise the Vulkan adapter's multimodal wire format without a model."""
import base64
import sys
from pathlib import Path
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
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
