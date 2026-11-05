"""Pinned Tencent WeMM 4B inference on a local CUDA device."""
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


def identity():
    return {"recipe": 1, "repo": REPO, "revision": REVISION, "files": FILES,
            "backend": "cuda", "dtype": "bfloat16", "max_pixels": MAX_PIXELS,
            "transformers": "5.2.0", "qwen_vl_utils": "0.0.14"}


def verify_snapshot(path):
    from .ane import hash_file

    for name, expected in FILES.items():
        if hash_file(path / name) != expected:
            raise ValueError(f"WeMM checkpoint file differs from its pinned hash: {name}")


class WeMM:
    def __init__(self, path):
        import torch
        from transformers import AutoProcessor, Qwen3_5ForConditionalGeneration, __version__

        if __version__ != "5.2.0":
            raise ValueError("WeMM requires Transformers 5.2.0")
        if not torch.cuda.is_available() or not torch.cuda.is_bf16_supported(including_emulation=False):
            raise RuntimeError("WeMM requires a CUDA GPU with native BF16 support")
        verify_snapshot(path)
        self.torch = torch
        self.processor = AutoProcessor.from_pretrained(path, local_files_only=True, trust_remote_code=False)
        self.processor.tokenizer.padding_side = "right"
        self.embedding_token = self.processor.tokenizer.convert_tokens_to_ids("<embedding>")
        if self.embedding_token is None:
            raise ValueError("WeMM tokenizer is missing its embedding token")
        self.model = Qwen3_5ForConditionalGeneration.from_pretrained(
            path, local_files_only=True, use_safetensors=True, dtype=torch.bfloat16,
            device_map={"": 0}, attn_implementation="sdpa",
        ).eval()
        if self.model.dtype != torch.bfloat16 or any(p.device.type != "cuda" for p in self.model.parameters()):
            raise RuntimeError("WeMM did not load entirely on CUDA in BF16")

    def images(self, images):
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
        inputs = inputs.to("cuda")
        with torch.inference_mode():
            # Transformers 5.2 can carry multimodal position offsets into the next request.
            self.model.model.rope_deltas = None
            hidden = self.model.model(**inputs, use_cache=False).last_hidden_state
            # The publisher pools this token and normalizes in the model dtype.
            pooled = hidden[rows.to("cuda"), last.to("cuda")]
            vectors = torch_normalize(pooled, dim=-1).float().cpu().numpy()
        return normalize(vectors)
