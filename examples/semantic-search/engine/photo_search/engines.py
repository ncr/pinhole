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


def log(*words):
    """What went wrong in full, for Pinhole's log: the person reads a line."""
    import sys

    print("photo search:", *words, file=sys.stderr, flush=True)


class CudaEngine:
    """WeMM on the NVIDIA card where it fits, else on the processor.

    A search loads only the language layers (6.8 GiB of the card) and only
    with 2 GiB to spare for everything else; with less, or no suitable card,
    it runs on the processor, a quarter of a second a question, its weights
    read from the mapped checkpoint. Photos are indexed on the card, waiting
    for room on it, or on the processor where there is no suitable card.
    """
    dimensions = {"wemm": DIMENSION}
    benchmark_import = False
    idle_seconds = 60

    def __init__(self, args):
        self.args = args
        self.model = None
        # Where the model loaded is: "cuda" or "cpu".
        self.device = None
        # The card as last looked at (wemm.card), and the reason said last.
        self.card = None
        self.said = None
        self.look()

    def identity(self):
        from .wemm import identity

        return identity()

    def look(self):
        from .wemm import card

        self.card = card()
        if self.card.reason != self.said:
            self.said = self.card.reason
            log(f"graphics card: {self.card.reason or 'fits'} ({self.card.detail})")
        return self.card

    def no_card(self):
        from .wemm import NO_CARD

        return self.card.reason in NO_CARD

    def where(self):
        """Where the next question is answered: "card" or "processor"."""
        if self.model is not None:
            return "card" if self.device == "cuda" else "processor"
        return "card" if self.card.reason is None else "processor"

    def describe(self):
        return {"search_on": self.where(), "card": self.card.reason}

    def check_indexing(self):
        pass

    def can_index(self):
        from .wemm import GIB, IMAGE_CARD_BYTES, KEEP_CARD_BYTES, VISION_CARD_BYTES

        if not getattr(self.args, "background", False):
            return True
        if self.device == "cuda":
            import torch

            free, _ = torch.cuda.mem_get_info()
            return free >= KEEP_CARD_BYTES + (0 if self.model.vision else VISION_CARD_BYTES)
        self.look()
        if self.no_card():
            from .wemm_vulkan import available_memory_kib

            # The weights are read from the mapped file; what the process
            # itself takes is under a GiB.
            return available_memory_kib() * 1024 >= 2 * GIB
        return self.card.free >= IMAGE_CARD_BYTES

    def load(self, device):
        from .wemm import WeMM

        self.close()
        self.model, self.device = WeMM(self.args.models, device), device
        log(f"WeMM loaded on the {'card' if device == 'cuda' else 'processor'}")
        return self.model

    def encoder(self, images=False):
        from .wemm import out_of_memory

        if images:
            device = "cpu" if self.no_card() else "cuda"
            if self.model is not None and self.device == device:
                return self.model
        elif self.model is not None:
            return self.model
        else:
            device = "cuda" if self.look().reason is None else "cpu"
        try:
            return self.load(device)
        except Exception as error:
            if device != "cuda" or images:
                raise
            # Taken by another program since, or a driver that does not
            # start: the question is answered on the processor.
            log(f"WeMM could not load on the card, so search runs on the processor: {error!r}")
            self.close()
            if not out_of_memory(error):
                self.card.reason = self.said = "driver"
            return self.load("cpu")

    def encode_image(self, image):
        return self.encoder(images=True).images([image])[0]

    def encode_text(self, texts):
        from .wemm import out_of_memory

        model = self.encoder()
        try:
            return model.texts(texts)
        except Exception as error:
            if self.device != "cuda" or not out_of_memory(error):
                raise
            log(f"the card ran out of memory for a question, so search runs on the processor: {error!r}")
            self.card.reason = "busy"
            return self.load("cpu").texts(texts)

    def loaded(self):
        return self.model is not None

    def close(self):
        if self.model is not None:
            torch, device = self.model.torch, self.device
            self.model = self.device = None
            if device == "cuda":
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
            return {"name": "WeMM 4B · NVIDIA card or processor", "python": str(python),
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
