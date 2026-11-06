"""WeMM 4B Q8_0 with a private, pinned llama.cpp Vulkan runtime."""
import base64
import http.client
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

REPO = "seamon67/WeMM-Embedding-4B-GGUF"
REVISION = "1e3e1b0d7fdb3c3661315b338b3e3b5b055e0121"
RUNTIME_COMMIT = "960b1d51b73176ac970be51ef8ef22c9e2b9113c"
MODEL = "WeMM-4B-Q8_0.gguf"
PROJECTOR = "mmproj-WeMM-4B-BF16.gguf"
FILES = {
    MODEL: "7def6777d811e61c3c16a2a820dd6607b208f2f01d80d52d77df8b39ca50acb0",
    PROJECTOR: "ebf6749204de105ad5acd8df7b181f2517a7c750dab43f30e8efa7578ef76871",
}
ICD = Path("/usr/share/vulkan/icd.d/asahi_icd.aarch64.json")
EMBEDDING_TOKEN = 248077
IMAGE_MAX_TOKENS = 768
START_MEMORY_KIB = 7 * 1024 ** 2
KEEP_MEMORY_KIB = 2 * 1024 ** 2


def identity():
    return {"recipe": 1, "backend": "vulkan", "repo": REPO, "revision": REVISION,
            "runtime": RUNTIME_COMMIT, "files": FILES, "image_max_tokens": IMAGE_MAX_TOKENS}


def available_memory_kib():
    with open("/proc/meminfo") as info:
        for line in info:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1])
    raise RuntimeError("Could not check available memory for WeMM Vulkan")


def check_installation(directory):
    if not ICD.is_file():
        raise RuntimeError("WeMM Vulkan requires the Asahi Vulkan driver")
    if not shutil.which("setpriv"):
        raise RuntimeError("WeMM Vulkan requires setpriv to tie the model process to the worker")
    if not os.access(directory / "bin/llama-server", os.X_OK):
        raise RuntimeError("The pinned WeMM Vulkan runtime is missing")
    if not all((directory / "weights" / name).is_file() for name in FILES):
        raise RuntimeError("The pinned WeMM Vulkan weights are missing")


# Weights hashed once per worker: 5 GB read again at every load made each
# search after an idle minute wait seconds. A changed file has a new stamp.
VERIFIED = {}


def verify(path, expected, hash_file):
    info = path.stat()
    stamp = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino, info.st_dev)
    if VERIFIED.get(str(path)) == (stamp, expected):
        return
    if hash_file(path) != expected:
        raise ValueError(f"WeMM Vulkan file differs from its pinned hash: {path.name}")
    VERIFIED[str(path)] = stamp, expected


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path, timeout):
        super().__init__("localhost", timeout=timeout)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


class WeMMVulkan:
    def __init__(self, directory, threads):
        from .ane import hash_file

        check_installation(directory)
        server = directory / "bin/llama-server"
        weights = directory / "weights"
        for name, expected in FILES.items():
            verify(weights / name, expected, hash_file)

        env = os.environ | {"VK_DRIVER_FILES": str(ICD)}
        version = subprocess.run([str(server), "--version"], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, timeout=10)
        if version.returncode or f"commit {RUNTIME_COMMIT[:7]})" not in version.stdout:
            raise RuntimeError("Unexpected llama.cpp build for WeMM Vulkan")

        self.runtime = tempfile.TemporaryDirectory(prefix="pinhole-wemm-", dir="/tmp")
        self.socket = Path(self.runtime.name) / "model.sock"
        self.process = None
        try:
            with (Path(self.runtime.name) / "model.log").open("wb") as log:
                self.process = subprocess.Popen([
                    "setpriv", "--pdeathsig", "KILL", str(server),
                    "--model", str(weights / MODEL), "--mmproj", str(weights / PROJECTOR),
                    "--device", "Vulkan0", "--n-gpu-layers", "all", "--parallel", "1",
                    "--threads", str(threads), "--threads-batch", str(threads),
                    # A 2048-character query can take up to 8192 UTF-8 bytes, plus the template.
                    "--ctx-size", "9216", "--batch-size", "1024", "--ubatch-size", "1024",
                    "--embedding", "--pooling", "last", "--image-max-tokens", str(IMAGE_MAX_TOKENS),
                    "--host", str(self.socket), "--offline", "--no-webui", "--no-warmup", "--log-verbosity", "1",
                ], env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
            self.marker = self.wait_ready()
        except BaseException:
            self.close()
            raise

    def call(self, path, value=None, timeout=45):
        connection = UnixHTTP(self.socket, timeout)
        try:
            data = None if value is None else json.dumps(value, ensure_ascii=False).encode()
            connection.request("GET" if data is None else "POST", path, data, {"Content-Type": "application/json"})
            response = connection.getresponse()
            if response.status != 200:
                raise RuntimeError(f"WeMM Vulkan {path} returned HTTP {response.status}")
            return json.loads(response.read())
        finally:
            connection.close()

    def wait_ready(self):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline and self.process.poll() is None:
            try:
                info = self.call("/props", timeout=1)
            except (OSError, RuntimeError, http.client.HTTPException):
                time.sleep(.15)
                continue
            if info.get("modalities", {}).get("vision") and info.get("media_marker"):
                return info["media_marker"]
            raise RuntimeError("WeMM Vulkan loaded without an image projector")
        details = (Path(self.runtime.name) / "model.log").read_text(errors="replace").strip().splitlines()
        raise RuntimeError("WeMM Vulkan could not start" + (f": {details[-1]}" if details else ""))

    def vector(self, content):
        from .models import normalize
        from .wemm import DIMENSION

        result = self.call("/embedding", {"content": content, "embd_normalize": 2})
        vector, = result[0]["embedding"]
        vector = normalize(vector)
        if vector.shape != (DIMENSION,):
            raise ValueError("Unexpected WeMM Vulkan embedding dimensions")
        return vector

    def image(self, image):
        stream = io.BytesIO()
        image.save(stream, format="PNG")
        return self.vector({
            "prompt_string": f"<|im_start|>user{self.marker}<|im_end|><embedding>",
            "multimodal_data": [base64.b64encode(stream.getvalue()).decode()],
        })

    def text(self, text):
        # Match Tencent's tokenizer post-processing and pool the special
        # final token. Explicit token IDs avoid an implicit BOS/EOS.
        tokens = self.call("/tokenize", {
            "content": f"<|im_start|>user\n{text}<|im_end|><embedding>",
            "add_special": False, "parse_special": True,
        })["tokens"]
        if not tokens or tokens[-1] != EMBEDDING_TOKEN:
            raise ValueError("WeMM Vulkan is missing its final embedding token")
        return self.vector(tokens)

    def close(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.runtime.cleanup()
