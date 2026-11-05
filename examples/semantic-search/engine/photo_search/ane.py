"""Bounded resident libane backend for constant-weight vision projections.

H14 matmul format references: joshuaswarren/mil-hwx-compiler at
296d7ac6ebddc15967b071183dcf40bc41d3891b, H14Program.cpp; libane ABI:
joshuaswarren/omarchy-ane at 91ceff571e48aa1ad724ae0965be6edc96686d14.
No descriptor words or kernel registers are modified here.
"""
from collections import OrderedDict
import ctypes
import fcntl
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time

import numpy as np

ROWS, WIDTH = 64, 1024


def sha(data):
    return hashlib.sha256(data).hexdigest()


def hash_file(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for data in iter(lambda: stream.read(1 << 20), b""):
            h.update(data)
    return h.hexdigest()


def scales_for(weight):
    maxima = np.max(np.abs(weight), axis=1)
    exponents = np.zeros(len(weight), dtype=np.float32)
    np.log2(maxima, out=exponents, where=maxima > 0)
    result = np.exp2(np.clip(np.floor(-exponents), -16, 24)).astype(np.float32)
    result[maxima == 0] = 1
    return result


def unpack_matrix(binary, record):
    start = 0x1000 + record["constantOffset"]
    raw = binary[start:start + record["constantBytes"]]
    if len(raw) != WIDTH * WIDTH * 2:
        raise ValueError("wrong compiled constant allocation")
    packed = np.frombuffer(raw, dtype="<f2").reshape(WIDTH // 16, WIDTH, 16)
    columns = np.arange(WIDTH)
    plane = columns // 16
    destination = (plane % 16) * 4 + plane // 16
    return packed[destination, :, columns % 16].copy()


def compile_tile(weight, root, compiler, compiler_hash):
    """Only the hardware-qualified 64×1024×1024 geometry is emitted."""
    root.mkdir(parents=True, exist_ok=True)
    with (root / "compile.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return _compile_tile(weight, root, compiler, compiler_hash)


def _compile_tile(weight, root, compiler, compiler_hash):
    weight = np.ascontiguousarray(weight, dtype="<f2")
    if weight.shape != (WIDTH, WIDTH) or not np.isfinite(weight).all():
        raise ValueError("invalid matmul tile")
    payload = weight.tobytes()
    key = sha(b"pinhole-h14-resident-v1\0" + compiler_hash.encode() + payload)
    destination = root / key
    marker = destination / "verified.json"
    if marker.exists():
        cached = json.loads(marker.read_text())
        binary = destination / "compiled/program-0.anec"
        if (cached.get("key") == key and cached.get("anec_sha256") == hash_file(binary) and
                cached.get("manifest_sha256") == hash_file(destination / "compiled/manifest.json")):
            return binary, weight, key
        raise ValueError("compiled cache corruption; refusing reuse")
    if destination.exists():
        # Preserve diagnostics from an interrupted older prototype cache.
        destination.rename(root / ("incomplete-" + key + "-" + str(time.time_ns())))
    directory = Path(tempfile.mkdtemp(prefix=".compile-", dir=root))
    marker = directory / "verified.json"
    blob = bytearray(128) + payload
    struct.pack_into("<IIQQ", blob, 64, 0xDEADBEEF, 1, len(payload), 128)
    (directory / "weights.bin").write_bytes(blob)
    (directory / "model.mil").write_text('''program(1.3)
[buildInfo = dict<string, string>({})]
{
  func main<ios18>(tensor<fp16, [64,1024]> x) {
    tensor<fp16, [1024,1024]> W = const()[name = string("W"), val = tensor<fp16, [1024,1024]>(BLOBFILE(path = string("@model_path/weights.bin"), offset = uint64(64)))];
    tensor<fp16, [64,1024]> y = linear(x = x, weight = W)[name = string("linear")];
  } -> (y);
}
''')
    env = os.environ.copy()
    bundled = compiler.parent.parent / "lib/mil-hwx-compiler/lib"
    if bundled.is_dir():
        env["LD_LIBRARY_PATH"] = str(bundled) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    result = subprocess.run([str(compiler), "--target", "H14", "--mil", str(directory / "model.mil"),
                             "--model-root", str(directory), "--output", str(directory / "compiled")],
                            capture_output=True, text=True, timeout=30, env=env)
    (directory / "compiler.log").write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError("H14 compilation failed: " + result.stderr)
    manifest_path = directory / "compiled/manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if manifest["target"] != "H14" or len(manifest["programs"]) != 1:
        raise ValueError("unexpected compiler output")
    record = manifest["programs"][0]
    if record["file"] != "program-0.anec" or record["encoder"] != "apple-parity-matvec":
        raise ValueError("unexpected ANEC program")
    for port, index in [(record["inputs"][0], 5), (record["outputs"][0], 4)]:
        if (port["index"] != index or port["dtype"] != "float16" or port["shape"] != [ROWS, WIDTH] or
                port["nchw"] != [1, 1, ROWS, WIDTH, ROWS * WIDTH * 2, WIDTH * 2] or
                port["allocationBytes"] != ROWS * WIDTH * 2):
            raise ValueError("unsupported H14 port layout")
    binary = directory / "compiled/program-0.anec"
    if not np.array_equal(unpack_matrix(binary.read_bytes(), record).view(np.uint16), weight.view(np.uint16)):
        raise ValueError("compiled ANEC weights differ from the requested matrix")
    marker.write_text(json.dumps({"key": key, "anec_sha256": hash_file(binary),
                                  "manifest_sha256": hash_file(manifest_path)}, indent=2) + "\n")
    directory.rename(destination)
    return destination / "compiled/program-0.anec", weight, key


def assert_device_healthy(path, boot=None):
    boot = boot or Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    if path.exists():
        health = json.loads(path.read_text())
        if health.get('boot_id') == boot and health.get('quarantined'):
            raise RuntimeError('ANE failed earlier in this boot; reboot and repeat the smoke test before reuse')


class NativeRuntime:
    """Resident libane handles; finite outputs, per-tile checks and no fallback."""
    def __init__(self, library, root, smoke_receipt, *, max_programs=64, check="first"):
        receipt = json.loads(smoke_receipt.read_text())
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        self.health_file = root.parent / "ane-device-health.json"
        assert_device_healthy(self.health_file, boot)
        assert_device_healthy(smoke_receipt.parent / 'ane-device-health.json', boot)
        self.boot_id = boot
        if (receipt.get("boot_id") != boot or receipt.get("errors") != 0 or
                receipt.get("chip") != "t6020" or not receipt.get("available") or
                len(receipt.get("sha256", [])) != 20 or
                set(receipt["sha256"]) != {receipt.get("golden_sha256")} or
                Path("/sys/bus/platform/devices/284000000.ane/driver").resolve().name != "ane_t6021" or
                not Path("/dev/accel/accel0").exists()):
            raise ValueError("current-boot M2 Pro smoke and bound ANE driver required")
        if not 1 <= max_programs <= 128 or check not in ("first", "all"):
            raise ValueError("invalid resident runtime limits")
        root.mkdir(parents=True, exist_ok=True)
        self.lock = (root / "device.lock").open("a")
        fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.lib = ctypes.CDLL(str(library.resolve()))
        definitions = {
            "__ane_init": ([ctypes.c_char_p, ctypes.c_int], ctypes.c_void_p),
            "__ane_free": ([ctypes.c_void_p], None),
            "__ane_src_size": ([ctypes.c_void_p, ctypes.c_uint32], ctypes.c_uint64),
            "__ane_dst_size": ([ctypes.c_void_p, ctypes.c_uint32], ctypes.c_uint64),
            "__ane_send": ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32], None),
            "__ane_read": ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32], None),
            "ane_exec": ([ctypes.c_void_p], ctypes.c_int),
        }
        self.functions = {}
        for name, (arguments, result) in definitions.items():
            function = getattr(self.lib, name)
            function.argtypes, function.restype = arguments, result
            self.functions[name] = function
        self.handles, self.checked = OrderedDict(), set()
        self.root = root
        self.max_programs, self.check = max_programs, check
        self.failed = False
        self.closed = False
        self.metrics = {"calls": 0, "checked_calls": 0, "loads": 0, "exec_seconds": 0.,
                        "max_resident": 0, "library_sha256": hash_file(library), "check_mode": check}

    def call(self, binary, key, x, weight):
        if self.failed or self.closed:
            raise RuntimeError("ANE runtime is failed; no retry or CPU fallback")
        x = np.ascontiguousarray(x, dtype="<f2")
        if x.shape != (ROWS, WIDTH) or not np.isfinite(x).all():
            raise ValueError("invalid ANE input tile")
        try:
            if key not in self.handles:
                while len(self.handles) >= self.max_programs:
                    _, old = self.handles.popitem(last=False)
                    self.functions["__ane_free"](old)
                # Respect both the driver-wide cap and this experiment's
                # small budget before requesting another constant program.
                used = int(Path("/sys/module/ane_t6021/parameters/bo_total_bytes").read_text())
                cap = int(Path("/sys/module/ane_t6021/parameters/bo_total_max_mb").read_text()) << 20
                if used + (4 << 20) > min(cap, 2 << 30):
                    raise RuntimeError("ANE buffer budget exceeded")
                nn = self.functions["__ane_init"](str(binary.resolve()).encode(), 0)
                if not nn:
                    raise RuntimeError("ANE program load failed")
                if (self.functions["__ane_src_size"](nn, 0) != x.nbytes or
                        self.functions["__ane_dst_size"](nn, 0) != x.nbytes):
                    self.functions["__ane_free"](nn)
                    raise ValueError("libane allocations disagree with the validated manifest")
                self.handles[key] = nn
                self.metrics["loads"] += 1
                self.metrics["max_resident"] = max(self.metrics["max_resident"], len(self.handles))
            nn = self.handles.pop(key)
            self.handles[key] = nn
            y = np.zeros((ROWS, WIDTH), dtype="<f2")
            self.functions["__ane_send"](nn, x.ctypes.data, 0)
            started = time.perf_counter()
            if self.functions["ane_exec"](nn) < 0:
                self.health_file.write_text(json.dumps({"schema": 1, "boot_id": self.boot_id,
                                                        "quarantined": True, "reason": "ane_exec failed",
                                                        "time_ns": time.time_ns()}) + "\n")
                raise RuntimeError("ANE execution failed")
            self.metrics["exec_seconds"] += time.perf_counter() - started
            self.functions["__ane_read"](nn, y.ctypes.data, 0)
            self.metrics["calls"] += 1
            if not np.isfinite(y).all():
                raise ValueError("nonfinite ANE result")
            if self.check == "all" or key not in self.checked:
                # Same 2 ulp OR 4 condition-unit criterion as ane-run.
                a, b = x.astype(np.float64), weight.astype(np.float64)
                reference = a @ b.T
                absolute_terms = np.abs(a) @ np.abs(b).T
                rounded = reference.astype(np.float16)
                ulp = np.abs(np.spacing(np.abs(rounded))).astype(np.float64)
                error = np.abs(y.astype(np.float64) - reference)
                band = ((np.abs(y.astype(np.float64) - rounded) <= 2 * ulp) |
                        (error <= 4 * absolute_terms * 2**-11))
                band = np.where(absolute_terms == 0, error == 0, band)
                if not np.all(band):
                    failures = self.root / "failures"
                    failures.mkdir(mode=0o700, exist_ok=True)
                    evidence = failures / f"{time.time_ns()}-{key}.npz"
                    np.savez(evidence, input=x, weight=weight, actual=y, reference=reference, accepted=band)
                    raise ValueError(f"ANE numerical check failed in {int(np.count_nonzero(~band))} lanes; evidence: {evidence}")
                self.checked.add(key)
                self.metrics["checked_calls"] += 1
            return y.astype(np.float32)
        except BaseException:
            self.failed = True
            raise

    def close(self):
        if self.closed:
            return
        for nn in self.handles.values():
            self.functions["__ane_free"](nn)
        self.handles.clear()
        self.lock.close()
        self.closed = True


class Linear:
    def __init__(self, weight, bias, root, compiler, compiler_hash):
        self.weight = np.ascontiguousarray(weight, dtype=np.float32)
        if self.weight.ndim != 2 or not np.isfinite(self.weight).all():
            raise ValueError("invalid linear weight")
        self.bias = None if bias is None else np.array(bias, dtype=np.float32, copy=True)
        self.columns, self.reduction = self.weight.shape
        if self.bias is not None and (self.bias.shape != (self.columns,) or not np.isfinite(self.bias).all()):
            raise ValueError("invalid linear bias")
        self.scales = scales_for(self.weight)
        self.tiles = []
        for n in range(0, self.columns, WIDTH):
            for k in range(0, self.reduction, WIDTH):
                matrix = np.zeros((WIDTH, WIDTH), dtype="<f2")
                nc, kc = min(WIDTH, self.columns - n), min(WIDTH, self.reduction - k)
                matrix[:nc, :kc] = self.weight[n:n + nc, k:k + kc] * self.scales[n:n + nc, None]
                binary, compiled, key = compile_tile(matrix, root, compiler, compiler_hash)
                self.tiles.append((n, k, nc, kc, binary, compiled, key))

    def __call__(self, value, runtime):
        value = np.ascontiguousarray(value, dtype=np.float32)
        if value.shape[-1] != self.reduction or not np.isfinite(value).all():
            raise ValueError("invalid linear input")
        original = value.shape
        flat = value.reshape(-1, self.reduction)
        output = np.zeros((len(flat), self.columns), dtype=np.float32)
        for start in range(0, len(flat), ROWS):
            count = min(ROWS, len(flat) - start)
            for n, k, nc, kc, binary, matrix, key in self.tiles:
                x = np.zeros((ROWS, WIDTH), dtype=np.float16)
                x[:count, :kc] = flat[start:start + count, k:k + kc]
                if not np.isfinite(x).all():
                    raise ValueError("linear input overflows fp16")
                partial = runtime.call(binary, key, x, matrix)
                if np.any(partial[:, nc:] != 0) or np.any(partial[count:] != 0):
                    raise ValueError("ANE wrote nonzero padded rows/columns")
                output[start:start + count, n:n + nc] += partial[:count, :nc]
        output /= self.scales
        if self.bias is not None:
            output += self.bias
        return output.reshape(*original[:-1], self.columns)


class SimulatedRuntime:
    """Explicit CPU reference for lowering tests; never described as ANE."""
    def __init__(self):
        self.metrics = {"calls": 0}

    def call(self, binary, key, x, weight):
        self.metrics["calls"] += 1
        return (x.astype(np.float32) @ weight.astype(np.float32).T).astype(np.float16).astype(np.float32)
