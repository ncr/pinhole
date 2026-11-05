"""Whether the person is away from the screen, for indexing on the GPU that
draws it only then: a model's work on the same card makes the desktop drop
frames while it runs (an RTX 5080, a 4B model: a hitch every few seconds).

Asked of Hyprland, through its socket: away while every real monitor (not
Hyprland's FALLBACK, not a headless one) is asleep (DPMS) or gone, or the
screen is locked (hyprlock). A real monitor may have no make: a MacBook's
built-in panel under Asahi reports none. None where it cannot say (no
Hyprland), for the caller to decide.
"""
import glob
import json
import os
import socket
import time


def hyprland_socket():
    runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    signature = os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
    if signature:
        path = os.path.join(runtime, "hypr", signature, ".socket.sock")
        if os.path.exists(path):
            return path
    found = sorted(glob.glob(os.path.join(runtime, "hypr", "*", ".socket.sock")), key=os.path.getmtime)
    return found[-1] if found else None


def monitors(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(1)
        connection.connect(path)
        connection.sendall(b"j/monitors all")
        data = b""
        while chunk := connection.recv(65536):
            data += chunk
    return json.loads(data)


def real(monitor):
    name = monitor.get("name") or ""
    return name != "FALLBACK" and not name.startswith("HEADLESS")


def locked():
    for entry in os.listdir("/proc"):
        if entry.isdigit():
            try:
                with open(f"/proc/{entry}/comm") as comm:
                    if comm.read().strip() == "hyprlock":
                        return True
            except OSError:
                pass
    return False


def away():
    path = hyprland_socket()
    if path is None:
        return None
    try:
        shown = [m for m in monitors(path) if real(m) and not m.get("disabled")]
    except (OSError, ValueError):
        return None
    return locked() or not any(m.get("dpmsStatus") for m in shown)


class Away:
    """away(), asked at most every few seconds."""

    def __init__(self, every=5, ask=away):
        self.every, self.ask = every, ask
        self.checked, self.value = -every, None

    def __call__(self):
        now = time.monotonic()
        if now - self.checked >= self.every:
            self.checked, self.value = now, self.ask()
        return self.value
