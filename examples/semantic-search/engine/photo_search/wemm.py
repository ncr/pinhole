"""Pinned Tencent WeMM 4B inference, on a local CUDA device or the processor.

The checkpoint is 9.6 GiB, but no question needs all of it on the card:
search reads the language layers (6.7 GiB), the vision tower (0.6 GiB) is
put there only to index photos, the token table (1.2 GiB) is read where it
lies in the mapped file, a row per word, and the head that predicts words
(1.2 GiB) is never read at all. Weights come straight from the mapped file,
so nothing of them is copied into the process's own memory.
"""
import json
import struct

REPO = "tencent/WeMM-Embedding-4B"
REVISION = "41a4fafdd26252ddff05c025413af7e61e26f08e"
DIMENSION = 2560
MAX_PIXELS = 1024 ** 2
FILES = {
    "config.json": "c7f7f7416997863d984010a7567bf1467108475d6982e132f99c067eb01e9a50",
    "processor_config.json": "d601e2fe0de1bc11852de3aa843f01a1677ca84f4dc743916c9ce4b8d30fb384",
    "chat_template.jinja": "273d8e0e683b885071fb17e08d71e5f2a5ddfb5309756181681de4f5a1822d80",
    "tokenizer.json": "40e444c744512f423da4c8443c47c21e22ff76056ba4e9796a81c04c13a9daf0",
    "tokenizer_config.json": "35db43580d8c551641a5fe1d1daafb10e73d0a1243db18ff7bc00413a6152ff5",
    "model.safetensors": "880c33762729584656223d1235bf13d035b2a842c28eac9ae6ea50bbc3d1ac45",
}
GIB = 1024 ** 3
# Graphics memory a search takes on the card: the language layers and what
# a question's few words take beside them (measured 2026-11-26 on an RTX
# 5080: 6.81 GiB allocated at most, 7.2 GiB for the process in nvidia-smi).
TEXT_CARD_BYTES = int(7.2 * GIB)
# Left free for the desktop and other programs: with less to spare, search
# runs on the processor, a quarter of a second a question, rather than
# taking what they may want next.
LEAVE_FREE_BYTES = 2 * GIB
# To index photos: the vision tower (0.6 GiB) and a full-size photo's
# tokens beside the language layers.
IMAGE_CARD_BYTES = int(8.6 * GIB)
VISION_CARD_BYTES = int(1.4 * GIB)
# Still free when indexing goes on with the model loaded.
KEEP_CARD_BYTES = 768 * 1024 ** 2
# Card reasons for which WeMM runs on the processor for good, indexing too.
NO_CARD = ("driver", "bf16", "small")


def identity():
    return {"recipe": 1, "repo": REPO, "revision": REVISION, "files": FILES,
            "backend": "cuda", "dtype": "bfloat16", "max_pixels": MAX_PIXELS,
            "transformers": "5.2.0", "qwen_vl_utils": "0.0.14"}


def verify_snapshot(path):
    from .ane import hash_file

    for name, expected in FILES.items():
        if hash_file(path / name) != expected:
            raise ValueError(f"WeMM checkpoint file differs from its pinned hash: {name}")


class Card:
    """What the NVIDIA card can do for WeMM now: `reason` None when a
    search fits on it, else why not; `detail` is for the log."""

    def __init__(self, reason=None, detail="", free=0, total=0):
        self.reason, self.detail, self.free, self.total = reason, detail, free, total


def card(run=None):
    """The first NVIDIA card, asked of nvidia-smi: no model loaded, and no
    CUDA context made, which would itself keep 0.3 GiB of the card.
    Reasons: 'driver' (no NVIDIA card, or its driver does not answer),
    'bf16' (older than Ampere, without the BF16 WeMM is made in), 'small'
    (too little graphics memory in all), 'busy' (enough in all, but other
    programs hold it now)."""
    import os
    import subprocess

    if os.environ.get("CUDA_VISIBLE_DEVICES") == "":
        return Card("driver", "CUDA_VISIBLE_DEVICES is empty")
    try:
        done = (run or subprocess.run)(
            ["nvidia-smi", "--query-gpu=name,compute_cap,memory.total,memory.free",
             "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as error:
        return Card("driver", f"nvidia-smi: {error}")
    lines = done.stdout.strip().splitlines()
    try:
        if done.returncode or not lines:
            raise ValueError((done.stderr or done.stdout).strip()[:300] or f"exit {done.returncode}")
        name, capability, total, free = (part.strip() for part in lines[0].split(","))
        total, free = int(total) * 1024 ** 2, int(free) * 1024 ** 2
        major = int(capability.split(".")[0])
    except ValueError as error:
        return Card("driver", f"nvidia-smi: {error}")
    if major < 8:
        return Card("bf16", f"{name}, compute capability {capability}", free, total)
    if total < TEXT_CARD_BYTES + LEAVE_FREE_BYTES:
        return Card("small", f"{name}, {total / GIB:.1f} GiB in all", free, total)
    if free < TEXT_CARD_BYTES + LEAVE_FREE_BYTES:
        return Card("busy", f"{name}, {free / GIB:.1f} of {total / GIB:.1f} GiB free", free, total)
    return Card(None, f"{name}, {free / GIB:.1f} of {total / GIB:.1f} GiB free", free, total)


def out_of_memory(error):
    """The card had no room for what was asked of it."""
    return type(error).__name__ == "OutOfMemoryError" or "out of memory" in str(error).lower()


class Weights:
    """The checkpoint's tensors as views of the mapped file."""

    DTYPES = {"BF16": "bfloat16", "F16": "float16", "F32": "float32"}

    def __init__(self, path):
        import numpy as np

        with open(path, "rb") as file:
            size = struct.unpack("<Q", file.read(8))[0]
            self.header = json.loads(file.read(size))
        self.header.pop("__metadata__", None)
        self.base = 8 + size
        # Copy-on-write: writable views for torch, the file never changed.
        self.data = np.memmap(path, dtype=np.uint8, mode="c")

    def tensor(self, name):
        import numpy as np
        import torch

        info = self.header[name]
        start, end = info["data_offsets"]
        raw = self.data[self.base + start:self.base + end]
        dtype = getattr(torch, self.DTYPES[info["dtype"]])
        width = {torch.bfloat16: np.uint16, torch.float16: np.uint16, torch.float32: np.uint32}[dtype]
        return torch.from_numpy(raw.view(width)).view(dtype).reshape(info["shape"])


def mapped_embedding(weight, device):
    import torch

    class MappedEmbedding(torch.nn.Module):
        """The token table read where it lies, on the processor: a
        question's few rows go to the device, not 1.2 GiB of table."""

        def __init__(self):
            super().__init__()
            # Not a parameter: moving the model must not move it.
            self.table = weight
            self.device = torch.device(device)

        @property
        def weight(self):
            return self.table

        def forward(self, ids):
            return torch.nn.functional.embedding(ids.to("cpu"), self.table).to(self.device)

    return MappedEmbedding()


class WeMM:
    def __init__(self, path, device="cuda"):
        import torch
        from accelerate import init_empty_weights
        from accelerate.utils import set_module_tensor_to_device
        from transformers import AutoConfig, AutoProcessor, Qwen3_5ForConditionalGeneration, __version__

        if __version__ != "5.2.0":
            raise ValueError("WeMM requires Transformers 5.2.0")
        if device == "cuda" and (not torch.cuda.is_available() or
                                 not torch.cuda.is_bf16_supported(including_emulation=False)):
            raise RuntimeError("WeMM requires a CUDA GPU with native BF16 support")
        verify_snapshot(path)
        self.torch, self.device, self.place = torch, device, set_module_tensor_to_device
        self.processor = AutoProcessor.from_pretrained(path, local_files_only=True, trust_remote_code=False)
        self.processor.tokenizer.padding_side = "right"
        self.embedding_token = self.processor.tokenizer.convert_tokens_to_ids("<embedding>")
        if self.embedding_token is None:
            raise ValueError("WeMM tokenizer is missing its embedding token")
        config = AutoConfig.from_pretrained(path, local_files_only=True)
        # Built empty: parameters are filled from the file, part by part.
        with init_empty_weights(include_buffers=False):
            whole = Qwen3_5ForConditionalGeneration._from_config(config, attn_implementation="sdpa",
                                                                 dtype=torch.bfloat16)
        self.weights = Weights(path / "model.safetensors")
        self.model = whole.model.eval()  # without lm_head, which an embedding never uses
        text = self.model.language_model
        text.embed_tokens = mapped_embedding(self.weights.tensor("model.language_model.embed_tokens.weight"),
                                             device)
        self.load(text, "model.language_model.")
        self.vision = False

    def load(self, module, prefix):
        for name, _ in list(module.named_parameters()):
            self.place(module, name, self.device, value=self.weights.tensor(prefix + name))
        for name, buffer in list(module.named_buffers()):
            self.place(module, name, self.device, value=buffer)

    def images(self, images):
        if not self.vision:
            self.load(self.model.visual, "model.visual.")
            self.vision = True
        return self.encode([{"type": "image", "image": image, "max_pixels": MAX_PIXELS} for image in images])

    def texts(self, texts):
        return self.encode([{"type": "text", "text": text} for text in texts])

    def encode(self, content):
        from qwen_vl_utils import process_vision_info
        from torch.nn.functional import normalize as torch_normalize
        from .models import normalize

        torch = self.torch
        conversations = [[{"role": "user", "content": [item]}] for item in content]
        prompts = [self.processor.apply_chat_template(sample, tokenize=False, add_generation_prompt=False)
                   for sample in conversations]
        images, _ = process_vision_info(conversations, image_patch_size=16)
        inputs = self.processor(text=prompts, images=images, padding=True, return_tensors="pt")
        last = inputs.attention_mask.sum(dim=1) - 1
        rows = torch.arange(len(conversations))
        if not (inputs.input_ids[rows, last] == self.embedding_token).all():
            raise ValueError("WeMM did not receive the expected embedding token")
        inputs = inputs.to(self.device)
        with torch.inference_mode():
            # Transformers 5.2 can carry multimodal position offsets into the next request.
            self.model.rope_deltas = None
            hidden = self.model(**inputs, use_cache=False).last_hidden_state
            # The publisher pools this token and normalizes in the model dtype.
            pooled = hidden[rows.to(self.device), last.to(self.device)]
            vectors = torch_normalize(pooled, dim=-1).float().cpu().numpy()
        return normalize(vectors)
