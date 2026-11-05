"""Catalog and settings for the worker owned by the shared pinholed service."""
import json
import os
from pathlib import Path
import socket
import sqlite3
import time

PLUGIN = "me.semantic-search"


def service_manages(library, key):
    """Whether the shared service runs this plugin's worker for this library and source."""
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR") or Path.home() / ".cache")
    path = os.environ.get("PINHOLE_SOCKET") or runtime / "pinhole.sock"
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(1)
            connection.connect(str(path))
            request = {"id": 1, "method": "workers", "params": {}}
            connection.sendall((json.dumps(request) + "\n").encode())
            with connection.makefile("rb") as lines:
                for _ in range(32):
                    line = lines.readline(1 << 20)
                    if not line:
                        return False
                    reply = json.loads(line)
                    if reply.get("id") == 1:
                        workers = (reply.get("result") or {}).get("workers") or []
                        return any(isinstance(worker, dict) and worker.get("plugin") == PLUGIN
                                   and worker.get("key") == key and isinstance(worker.get("library"), str)
                                   and Path(worker["library"]) == Path(library) for worker in workers)
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    return False


def index_path(library, filename):
    cache = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
    return cache / "pinhole/plugins/me.semantic-search" / f"{library.parent.name}-{library.name}" / filename


class Catalog:
    def __init__(self, args):
        self.path = args.library / "library.sqlite"
        self.settings = args.managed_plugins
        self.library_key = args.library_key
        self.db = None
        self.inode = None
        self.version = None
        self.paused = False
        self.last_error = None
        self.next_poll = 0

    def close(self):
        if self.db is not None:
            self.db.close()
            self.db = None

    def settings_paused(self):
        try:
            data = json.loads(self.settings.read_text())
            return data.get("settings", {}).get("me.semantic-search", {}).get("indexingPaused") is True
        except (OSError, ValueError, AttributeError):
            return self.paused

    def poll(self, worker):
        now = time.monotonic()
        if now < self.next_poll:
            return
        self.next_poll = now + 5
        try:
            info = self.path.stat()
            inode = (info.st_dev, info.st_ino)
            if self.db is None or inode != self.inode:
                self.close()
                self.db = sqlite3.connect(self.path.resolve().as_uri() + "?mode=ro", uri=True,
                                          timeout=1, isolation_level=None)
                self.db.execute("PRAGMA query_only=ON")
                self.inode, self.version = inode, None
            version = self.db.execute("PRAGMA data_version").fetchone()[0]
            paused = self.settings_paused()
            if version == self.version and paused == self.paused:
                return
            self.db.execute("BEGIN")
            try:
                owner = self.db.execute("SELECT value FROM meta WHERE key='source'").fetchone()
                if not owner or owner[0] != self.library_key:
                    worker.stop()
                    worker.catalog_ready = False
                    return
                rows = self.db.execute("""SELECT id,thumb_path FROM assets WHERE is_video=0
                    AND thumb_path IS NOT NULL AND thumb_path<>'' ORDER BY id LIMIT 100001""").fetchall()
            finally:
                self.db.execute("ROLLBACK")
            worker.request({"op": "catalog", "assets": [{"id": asset, "path": path} for asset, path in rows],
                            "auto_index": not paused})
            worker.catalog_ready = True
            self.version, self.paused = version, paused
            self.last_error = None
        except (OSError, sqlite3.Error, ValueError) as error:
            # A rebuild/move can temporarily remove the catalog. Keep the
            # saved vectors and retry the read, without indexing stale paths.
            self.close()
            self.version = None
            if worker.running:
                worker.stop()
            worker.catalog_ready = False
            if str(error) != self.last_error:
                self.last_error = str(error)
                print(json.dumps({"event": "error", "error": self.last_error}), flush=True)
