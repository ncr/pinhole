"""Offline publisher models with an explicit hybrid ANE projection backend."""
import io
import json
from pathlib import Path
import time

import numpy as np

from .ane import Linear, hash_file

PINS = {
    "clip": {"repo": "sentence-transformers/clip-ViT-B-32", "revision": "327ab6726d33c0e22f920c83f2ff9e4bd38ca37f", "directory": "clip/0_CLIPModel",
             "checkpoint_sha256": "99d28a652e6ec46629ab7047a0ac82c69b1fe11e0ce672c43af65d3a9a3fc05d"},
    "siglip2": {"repo": "google/siglip2-base-patch16-224", "revision": "75de2d55ec2d0b4efc50b3e9ad70dba96a7b2fa2", "directory": "siglip2",
                "checkpoint_sha256": "612923381c76ec5a9bed335d1c48827e3f2e506ac31b044b63b2031fadee6a0b"},
    "multilingual": {"repo": "sentence-transformers/clip-ViT-B-32-multilingual-v1", "revision": "58edf8cada9e398793dca955574a48cbb7f18be2", "directory": "multilingual",
                     "checkpoint_sha256": "7b968c09938603158413ea2c91af82e82158ebb6db901a6cf97c28ce7d7ebc6d"},
}


def verify_checkpoint(name, models_root):
    path = models_root / PINS[name]["directory"] / "model.safetensors"
    if hash_file(path) != PINS[name]["checkpoint_sha256"]:
        raise ValueError(f"{name} checkpoint differs from its pinned publisher hash")
    return path.parent


def normalize(value):
    value = np.asarray(value, dtype=np.float32)
    if not np.isfinite(value).all():
        raise ValueError("nonfinite model embedding")
    norms = np.linalg.norm(value, axis=-1, keepdims=True)
    if np.any(norms <= 0):
        raise ValueError("zero model embedding")
    return value / norms


def load_image(path):
    from PIL import Image, ImageCms, ImageOps
    with Image.open(path) as original:
        image = ImageOps.exif_transpose(original)
        profile = image.info.get("icc_profile")
        if profile:
            image = ImageCms.profileToProfile(image, ImageCms.ImageCmsProfile(io.BytesIO(profile)),
                                              ImageCms.createProfile("sRGB"), outputMode="RGB")
        else:
            image = image.convert("RGB")
        return image.copy()


class ImageModel:
    def __init__(self, name, models_root):
        import torch
        from transformers import AutoImageProcessor, CLIPModel, SiglipModel
        self.name, self.path = name, verify_checkpoint(name, models_root)
        self.processor = AutoImageProcessor.from_pretrained(str(self.path), local_files_only=True, use_fast=False)
        model_type = CLIPModel if name == "clip" else SiglipModel
        self.model = model_type.from_pretrained(str(self.path), local_files_only=True, use_safetensors=True,
                                               attn_implementation="eager").eval()
        self.model.requires_grad_(False)
        self.originals = []
        self.prepared = []
        self.identity = PINS[name] | {"checkpoint_sha256": hash_file(self.path / "model.safetensors"),
                                      "processor_sha256": hash_file(self.path / "preprocessor_config.json"),
                                      "processor": self.processor.to_dict()}

    def pixels(self, image):
        return self.processor(images=image, return_tensors="pt")["pixel_values"]

    def encode(self, pixels):
        import torch
        with torch.inference_mode():
            value = self.model.get_image_features(pixel_values=pixels)
        return normalize(value.detach().cpu().numpy())

    def hybrid(self, runtime, cache, compiler, *, patch_backend="cpu", diagnostics=None):
        """Attention projections + final CLIP projection use ANE.

        Patch convolution, MLP, normalization, dynamic attention products/
        softmax and SigLIP pooling remain on CPU. An explicit diagnostic
        option can offload the patch convolution too. This boundary is
        fixed, reported and never chosen after a failed ANE operation.
        """
        import torch
        compiler_hash = hash_file(compiler)
        self.runtime = runtime

        def compare(name, actual, reference):
            if diagnostics is None:
                return
            a, b = np.asarray(actual, dtype=np.float64).ravel(), np.asarray(reference, dtype=np.float64).ravel()
            diagnostics.append({"name": name, "relative_l2": float(np.linalg.norm(a - b) / max(np.linalg.norm(b), 1e-30)),
                                "cosine": float(a @ b / max(np.linalg.norm(a) * np.linalg.norm(b), 1e-30)),
                                "max_absolute_error": float(np.max(np.abs(a - b)))})

        def replace(module, name):
            weight = module.weight.detach().numpy()
            bias = None if module.bias is None else module.bias.detach().numpy()
            layer = Linear(weight, bias, cache, compiler, compiler_hash)
            self.prepared.append({"name": name, "shape": list(weight.shape),
                                  "tiles": len(layer.tiles), "keys": [x[-1] for x in layer.tiles]})
            self.originals.append((module, module.forward))

            def forward(value):
                if value.device.type != "cpu":
                    raise ValueError("hybrid input must be on CPU")
                output = layer(value.detach().float().numpy(), runtime)
                if diagnostics is not None:
                    reference = torch.nn.functional.linear(value, module.weight, module.bias).detach().numpy()
                    compare(name, output, reference)
                return torch.from_numpy(output).to(dtype=value.dtype)

            module.forward = forward

        for name, module in self.model.vision_model.named_modules():
            if isinstance(module, torch.nn.Linear) and ".self_attn." in name:
                replace(module, "vision_model." + name)
        if self.name == "clip":
            replace(self.model.visual_projection, "visual_projection")
        if patch_backend == "cpu":
            return self.prepared
        if patch_backend != "ane":
            raise ValueError("unknown patch backend")
        convolution = self.model.vision_model.embeddings.patch_embedding
        if (convolution.kernel_size != convolution.stride or convolution.padding not in ((0, 0), "valid") or
                convolution.dilation != (1, 1) or convolution.groups != 1):
            raise ValueError("unsupported patch projection convolution")
        patch = convolution.kernel_size[0]
        weight = convolution.weight.detach().numpy().reshape(convolution.out_channels, -1)
        bias = None if convolution.bias is None else convolution.bias.detach().numpy()
        layer = Linear(weight, bias, cache, compiler, compiler_hash)
        self.prepared.append({"name": "vision_model.embeddings.patch_embedding", "shape": list(weight.shape),
                              "tiles": len(layer.tiles), "keys": [x[-1] for x in layer.tiles]})
        self.originals.append((convolution, convolution.forward))

        def patch_forward(value):
            if value.device.type != "cpu" or value.shape[-2:] != (224, 224):
                raise ValueError("unsupported patch projection input")
            patches = torch.nn.functional.unfold(value, kernel_size=patch, stride=patch).transpose(1, 2)
            result = layer(patches.detach().float().numpy(), runtime)
            output = torch.from_numpy(result).transpose(1, 2).reshape(value.shape[0], convolution.out_channels,
                                                                      224 // patch, 224 // patch).to(dtype=value.dtype)
            if diagnostics is not None:
                reference = torch.nn.functional.conv2d(value, convolution.weight, convolution.bias,
                                                       stride=patch, padding=0).detach().numpy()
                compare("vision_model.embeddings.patch_embedding", output.numpy(), reference)
            return output

        convolution.forward = patch_forward
        return self.prepared

    def restore(self):
        for module, forward in reversed(self.originals):
            module.forward = forward
        self.originals.clear()


class TextModel:
    """Keep a local text tower resident across interactive queries."""
    def __init__(self, name, models_root):
        self.name = name
        if name == "clip":
            from sentence_transformers import SentenceTransformer
            path = verify_checkpoint("multilingual", models_root)
            if hash_file(path / "2_Dense/model.safetensors") != "d12568dc7300970a4d3dbb49068ad16cd89b99840b74b026f8e48071e9414f74":
                raise ValueError("multilingual projection differs from its publisher hash")
            self.model = SentenceTransformer(str(path), device="cpu", local_files_only=True,
                                            trust_remote_code=False)
        elif name == "siglip2":
            from transformers import AutoTokenizer, SiglipModel
            path = verify_checkpoint(name, models_root)
            self.tokenizer = AutoTokenizer.from_pretrained(str(path), local_files_only=True)
            self.model = SiglipModel.from_pretrained(str(path), local_files_only=True, use_safetensors=True,
                                                   attn_implementation="eager").eval()
            self.model.requires_grad_(False)
        else:
            raise ValueError("unknown text model")

    def encode(self, texts):
        import torch
        if not texts or any(not isinstance(t, str) or not t.strip() or len(t) > 2048 for t in texts):
            raise ValueError("empty or excessive text query")
        started = time.perf_counter()
        info = {"backend": "cpu", "source": PINS["multilingual" if self.name == "clip" else "siglip2"]}
        if self.name == "clip":
            values = self.model.encode(texts, batch_size=8, normalize_embeddings=True, convert_to_numpy=True,
                                       show_progress_bar=False)
        else:
            # SigLIP 2 publisher's fixed-length training text recipe.
            tokens = self.tokenizer([text.lower() for text in texts], padding="max_length", max_length=64,
                                    truncation=True, return_tensors="pt")
            with torch.inference_mode():
                values = self.model.get_text_features(**tokens).detach().numpy()
            info.update(lowercase=True, max_length=64)
        info["batch_seconds"] = time.perf_counter() - started
        return normalize(values), info


def text_embeddings(name, models_root, texts):
    return TextModel(name, models_root).encode(texts)
