"""Search engines and the local profiles that select them."""

import os
import platform
from .wemm import DIMENSION


class LegacyEngine:
    dimensions = {"clip": 512, "siglip2": 768}
    benchmark_import = True
    idle_seconds = None

    def __init__(self, args):
        self.args = args
        self.image_model = self.text_model = None

    def identity(self):
        from .ane import hash_file
        from .models import PINS

        name = self.args.model
        path = self.args.models / PINS[name]["directory"]
        return {"recipe": 1, "image": PINS[name], "text": PINS["multilingual" if name == "clip" else name],
                "processor_sha256": hash_file(path / "preprocessor_config.json"),
                "backend": self.args.backend, "patch_backend": "cpu"}

    def check_indexing(self):
        pass

    def can_index(self):
        return True

    def prepare_image_model(self, model):
        pass

    def encode_image(self, image):
        if self.image_model is None:
            from .models import ImageModel

            model = ImageModel(self.args.model, self.args.models)
            self.prepare_image_model(model)
            self.image_model = model
        return self.image_model.encode(self.image_model.pixels(image))[0]

    def encode_text(self, texts):
        if self.text_model is None:
            from .models import TextModel

            self.text_model = TextModel(self.args.model, self.args.models)
        return self.text_model.encode(texts)[0]

    def loaded(self):
        return self.text_model is not None

    def close(self):
        if self.image_model is not None:
            self.image_model.restore()
        self.image_model = self.text_model = None


class AneEngine(LegacyEngine):
    def __init__(self, args):
        super().__init__(args)
        if not all([args.ane_library, args.compiler, args.smoke_receipt]):
            raise ValueError("hybrid-ane requires library, compiler and current-boot smoke receipt")
        self.runtime = None

    def check_indexing(self):
        from .ane import assert_device_healthy

        assert_device_healthy(self.args.cache.parent / "ane-device-health.json")
        assert_device_healthy(self.args.smoke_receipt.parent / "ane-device-health.json")

    def prepare_image_model(self, model):
        from .ane import NativeRuntime

        if self.runtime is None:
            self.runtime = NativeRuntime(self.args.ane_library, self.args.cache, self.args.smoke_receipt)
        model.hybrid(self.runtime, self.args.cache, self.args.compiler)

    def close(self):
        try:
            super().close()
        finally:
            if self.runtime is not None:
                self.runtime.close()
                self.runtime = None


class CudaEngine:
    dimensions = {"wemm": DIMENSION}
    benchmark_import = False
    idle_seconds = 60

    def __init__(self, args):
        self.args = args
        self.model = None

    def identity(self):
        from .wemm import identity

        return identity()

    def check_indexing(self):
        import torch

        if not torch.cuda.is_available() or not torch.cuda.is_bf16_supported(including_emulation=False):
            raise RuntimeError("WeMM requires a CUDA GPU with native BF16 support")

    def can_index(self):
        if not getattr(self.args, "background", False):
            return True
        import torch

        free, _ = torch.cuda.mem_get_info()
        if self.model is not None:
            return free >= 768 * 1024 ** 2
        return free >= 11 * 1024 ** 3

    def encoder(self):
        if self.model is None:
            from .wemm import WeMM

            self.model = WeMM(self.args.models)
        return self.model

    def encode_image(self, image):
        return self.encoder().images([image])[0]

    def encode_text(self, texts):
        return self.encoder().texts(texts)

    def loaded(self):
        return self.model is not None

    def close(self):
        if self.model is not None:
            torch = self.model.torch
            self.model = None
            torch.cuda.empty_cache()


class VulkanEngine:
    dimensions = {"wemm": DIMENSION}
    benchmark_import = False
    idle_seconds = 60

    def __init__(self, args):
        self.args = args
        self.model = None

    def identity(self):
        from .wemm_vulkan import identity

        return identity()

    def check_indexing(self):
        from .wemm_vulkan import check_installation

        check_installation(self.args.models)

    def can_index(self):
        from .wemm_vulkan import KEEP_MEMORY_KIB, START_MEMORY_KIB, available_memory_kib

        if self.model is not None and self.model.process.poll() is not None:
            self.close()
        available = available_memory_kib()
        if self.model is None:
            return available >= START_MEMORY_KIB
        return available >= KEEP_MEMORY_KIB

    # Indexing starts the model only with room for it (can_index); search
    # starts it whatever the memory, as search must always answer.
    def encoder(self, indexing=False):
        if self.model is not None and self.model.process.poll() is not None:
            self.close()
        if self.model is None:
            from .wemm_vulkan import START_MEMORY_KIB, WeMMVulkan, available_memory_kib

            if indexing and available_memory_kib() < START_MEMORY_KIB:
                raise RuntimeError("WeMM Vulkan is waiting for 7 GiB of available memory")
            self.model = WeMMVulkan(self.args.models, self.args.threads)
        return self.model

    def encode_image(self, image):
        return self.encoder(indexing=True).image(image)

    def encode_text(self, texts):
        import numpy as np

        model = self.encoder()
        return np.stack([model.text(text) for text in texts])

    def loaded(self):
        return self.model is not None and self.model.process.poll() is None

    def close(self):
        if self.model is not None:
            self.model.close()
            self.model = None


ENGINES = {"cpu": LegacyEngine, "hybrid-ane": AneEngine, "cuda": CudaEngine, "vulkan": VulkanEngine}


def create_engine(args):
    engine = ENGINES.get(args.backend)
    if engine is None or args.model not in engine.dimensions:
        raise ValueError(f"model {args.model!r} is unavailable on backend {args.backend!r}")
    return engine(args)


def local_profile(checkout):
    """Return launch arguments without importing a model or probing the accelerator."""
    base = checkout / ".tools/photo-search"
    machine = platform.machine()
    if machine in ("x86_64", "AMD64"):
        environment = base / "wemm-cuda"
        python = environment / ".venv/bin/python"
        if os.access(python, os.X_OK) and (environment / "weights/model.safetensors").is_file():
            return {"name": "WeMM 4B · NVIDIA CUDA", "python": str(python),
                    "indexFile": "wemm-4b-cuda-v1.sqlite", "benchmark": None,
                    "args": ["--model", "wemm", "--backend", "cuda", "--models", str(environment / "weights"),
                             "--threads", "2"]}
    if machine in ("aarch64", "arm64"):
        python = base / "venv/bin/python"
        vulkan = base / "wemm-vulkan"
        if (vulkan / "enabled").is_file() and os.access(python, os.X_OK):
            return {"name": "WeMM 4B · Apple GPU Vulkan", "python": str(python),
                    "indexFile": "wemm-4b-vulkan-v1.sqlite", "benchmark": None,
                    "args": ["--model", "wemm", "--backend", "vulkan", "--models", str(vulkan),
                             "--threads", "2"]}
        models = base / "models"
        library = checkout / ".tools/omarchy-ane/libane/libane.so"
        compiler = checkout / ".tools/ane-compiler-package/usr/bin/mil-hwxc"
        smoke = base / "ane-smoke.json"
        if os.access(python, os.X_OK) and all(p.is_file() for p in (library, compiler, models / "clip/0_CLIPModel/preprocessor_config.json")):
            return {"name": "CLIP · Apple Neural Engine", "python": str(python),
                    "indexFile": "clip-hybrid-v1.sqlite",
                    "benchmark": {"run": str(base / "full-v2/clip-hybrid-ane"), "corpus": str(base / "icloud-pl-v1/corpus.json")},
                    "args": ["--model", "clip", "--backend", "hybrid-ane", "--models", str(models),
                             "--cache", str(base / "resident-tiles"), "--compiler", str(compiler),
                             "--ane-library", str(library), "--smoke-receipt", str(smoke)]}
    raise ValueError("No local photo-search engine is installed for this computer")
