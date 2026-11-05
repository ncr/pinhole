"""Private, plugin-owned offline search worker. Never opens library.sqlite."""
import hashlib
from collections import deque
import json
import os
from pathlib import Path
import signal
import socket
import stat
import time

import numpy as np

from .ane import hash_file
from .engines import create_engine
from .store import Store

MAX_MESSAGE = 24 << 20

# Stable ids and Polish prompts for the prototype's fixed vocabulary.
TAGS = (
    ('animals', 'zwierzęta', 'zdjęcie zwierzęcia'),
    ('beach', 'plaża', 'plaża z piaskiem nad morzem'),
    ('bicycle', 'rower', 'zdjęcie z rowerem'),
    ('buildings', 'budynki', 'zdjęcie architektury i budynków'),
    ('celebration', 'uroczystość', 'uroczystość, przyjęcie lub świętowanie'),
    ('document', 'dokument', 'zdjęcie papierowego dokumentu z tekstem'),
    ('flowers', 'kwiaty', 'kwiaty lub bukiet'),
    ('food', 'jedzenie', 'zdjęcie potrawy lub jedzenia'),
    ('forest', 'las', 'drzewa i ścieżka w lesie'),
    ('group', 'zdjęcie grupowe', 'kilka osób na wspólnym zdjęciu'),
    ('interior', 'wnętrze', 'wnętrze mieszkania lub pokoju'),
    ('mountains', 'góry', 'górski krajobraz'),
    ('night', 'noc', 'zdjęcie na dworze nocą'),
    ('portrait', 'portret', 'portret osoby z bliska'),
    ('screenshot', 'zrzut ekranu', 'zrzut ekranu telefonu lub komputera'),
    ('snow', 'śnieg', 'śnieg w zimowym krajobrazie'),
    ('street', 'ulica', 'miejska ulica z chodnikiem lub samochodami'),
    ('sunset', 'zachód słońca', 'zachód słońca na horyzoncie'),
    ('water', 'woda', 'krajobraz z jeziorem, rzeką lub morzem'),
)


def thumbnail(root, relative):
    if not relative or Path(relative).is_absolute():
        raise ValueError('thumbnail must have a relative catalog path')
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError('thumbnail is missing or outside the library')
    if path.stat().st_size > 32 << 20:
        raise ValueError('thumbnail exceeds 32 MiB')
    return path


def receive(connection):
    data = bytearray()
    while b'\n' not in data:
        block = connection.recv(min(65536, MAX_MESSAGE + 1 - len(data)))
        if not block:
            raise ValueError('incomplete request')
        data.extend(block)
        if len(data) > MAX_MESSAGE:
            raise ValueError('request exceeds message limit')
    head, _, rest = data.partition(b'\n')
    if rest.strip():
        raise ValueError('one request per connection is required')
    request = json.loads(head)
    if not isinstance(request, dict):
        raise ValueError('request must be an object')
    return request


def send(connection, message):
    connection.sendall((json.dumps(message, ensure_ascii=False, allow_nan=False) + '\n').encode())


def client(path, request, timeout=120):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(timeout)
        connection.connect(str(path))
        send(connection, request)
        return receive(connection)


class Worker:
    def __init__(self, args):
        self.args = args
        self.engine = create_engine(args)
        self.identity = self.engine.identity()
        self.store = Store(args.index, args.library_key, self.identity, self.engine.dimensions[args.model])
        self.last_inference = None
        self.tag_vectors = None
        self.pending = []
        self.running = False
        self.waiting = False
        self.catalog_ready = False
        self.last_error = ''
        self.processed = self.skipped = 0
        self.stamps = {}

    def close(self):
        try:
            self.engine.close()
        finally:
            self.store.close()

    def release_idle(self):
        delay = self.engine.idle_seconds
        if (delay is not None and not self.running and self.last_inference is not None
                and time.monotonic() - self.last_inference >= delay):
            self.engine.close()
            self.last_inference = None

    def status(self):
        return self.store.status() | {"running": self.running, "waiting": self.waiting, "pending": len(self.pending),
                                      "catalog_ready": self.catalog_ready, "managed": bool(getattr(self.args, "managed_plugins", None)),
                                      "processed": self.processed, "skipped": self.skipped,
                                      "error": self.last_error, "backend": self.args.backend}

    def refresh(self, asset_id=None):
        """Check indexed local files; same-path replacements must not rank.

        Hash once after a worker restart, then only after stat changes. The
        stat cache is ephemeral; vectors remain bound to content SHA-256.
        """
        sql = '''SELECT c.asset_id,c.path,e.fingerprint FROM catalog c
            JOIN embeddings e USING(asset_id) WHERE c.fingerprint=e.fingerprint AND c.active=1'''
        if asset_id is None:
            rows = self.store.db.execute(sql).fetchall()
        else:
            rows = self.store.db.execute(sql + ' AND c.asset_id=?', (asset_id,)).fetchall()
        for asset, relative, fingerprint in rows:
            try:
                path = thumbnail(self.args.library, relative)
                info = path.stat()
                stamp = (str(path), info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
                if self.stamps.get(asset) != stamp:
                    if hash_file(path) != fingerprint:
                        raise ValueError('thumbnail changed; index it again')
                    self.stamps[asset] = stamp
            except (OSError, ValueError) as error:
                self.store.fail(asset, str(error))
                self.stamps.pop(asset, None)

    def encode_text(self, texts):
        try:
            return self.engine.encode_text(texts)
        finally:
            self.last_inference = time.monotonic()

    def suggest_tags(self, asset):
        if not isinstance(asset, str) or not 0 < len(asset) <= 1024:
            raise ValueError('invalid photo id')
        self.refresh(asset)
        image = self.store.vector_for(asset)
        if image is None:
            return []
        if self.tag_vectors is None:
            self.tag_vectors = self.encode_text([prompt for _, _, prompt in TAGS])
        order = np.argsort(-(self.tag_vectors @ image), kind='stable')[:5]
        return [{'id': TAGS[i][0], 'label': TAGS[i][1]} for i in order]

    def missing(self):
        return self.store.db.execute('''SELECT c.asset_id,c.path FROM catalog c LEFT JOIN embeddings e
            USING(asset_id) WHERE c.fingerprint IS NULL OR e.asset_id IS NULL OR c.fingerprint<>e.fingerprint
            ORDER BY c.asset_id''').fetchall()

    def start(self, pending):
        if pending:
            self.engine.check_indexing()
        self.pending = deque(pending)
        self.waiting = False
        self.processed = self.skipped = 0
        self.running, self.last_error = bool(self.pending), ''

    def stop(self):
        self.pending, self.running, self.waiting = [], False, False
        if self.engine.idle_seconds is not None:
            self.engine.close()
            self.last_inference = None

    def request(self, message):
        op = message.get('op')
        if op == 'status':
            return self.status()
        if op == 'catalog':
            assets = message['assets']
            auto = message.get('auto_index')
            if 'auto_index' in message and type(auto) is not bool:
                raise ValueError('auto_index must be boolean')
            if not isinstance(assets, list) or len(assets) > 100000:
                raise ValueError('invalid or excessive catalog')
            # Validate all paths before changing the previous catalog. A
            # missing thumbnail is a per-photo indexing error, not fatal here.
            for asset in assets:
                if not isinstance(asset, dict) or not isinstance(asset.get('path'), str):
                    raise ValueError('invalid catalog entry')
                relative = asset['path']
                if not relative or Path(relative).is_absolute() or not (self.args.library / relative).resolve().is_relative_to(self.args.library.resolve()):
                    raise ValueError('catalog path escapes the library')
            self.store.catalog(assets)
            active = {x['id'] for x in assets}
            self.stamps = {key: value for key, value in self.stamps.items() if key in active}
            self.refresh()
            if auto is False and self.running:
                self.stop()
            elif self.running:
                catalog = {x['id']: x['path'] for x in assets}
                retained = [(asset, catalog[asset]) for asset, _ in self.pending if asset in catalog]
                queued = {asset for asset, _ in retained}
                self.pending = deque(retained + [(asset, path) for asset, path in self.missing() if asset not in queued])
                self.running = bool(self.pending)
                if not self.running:
                    self.waiting = False
            elif auto and not self.last_error:
                try:
                    self.start(self.missing())
                except (OSError, RuntimeError, ValueError) as error:
                    self.last_error = str(error)
            return self.status()
        if op == 'start':
            if self.running:
                return self.status()
            pending = self.missing() if message.get('missing_only') is True else self.store.db.execute(
                'SELECT asset_id,path FROM catalog ORDER BY asset_id').fetchall()
            self.start(pending)
            return self.status()
        if op == 'stop':
            self.stop()
            return self.status()
        if op == 'query':
            text = message.get('text', '')
            if not isinstance(text, str) or not text.strip() or len(text) > 2048:
                raise ValueError('query must contain 1–2048 characters')
            limit = message.get('limit', 100)
            if type(limit) is not int or not 1 <= limit <= 500:
                raise ValueError('result limit must be 1–500')
            self.refresh()
            if not self.store.status()['indexed']:
                return {"results": [], "seconds": 0, "status": self.status()}
            started = time.perf_counter()
            vector = self.encode_text([text])[0]
            return {"results": self.store.search(vector, limit), "seconds": time.perf_counter() - started,
                    "status": self.status()}
        if op == 'suggest_tags':
            return self.suggest_tags(message.get('id'))
        if op == 'import':
            return self.import_run(Path(message['run']), Path(message['corpus']))
        raise ValueError('unknown worker operation')

    def import_run(self, directory, corpus_path):
        """Map hashed benchmark ids back through the plugin's public catalog."""
        if not self.engine.benchmark_import:
            raise ValueError('this engine does not support benchmark import')
        from .compare import read_run
        report, ids, vectors = read_run(directory)
        corpus = json.loads(corpus_path.read_text())
        expected_backend = self.args.backend
        if (report['model'] != self.args.model or report['backend'] != expected_backend or
                report.get('patch_backend') != 'cpu' or report['corpus_id'] != corpus['corpus_id'] or
                report['source']['checkpoint_sha256'] != self.identity['image']['checkpoint_sha256'] or
                report['source']['processor_sha256'] != self.identity['processor_sha256']):
            raise ValueError('benchmark run has a different model or inference recipe')
        corpus_items = {item['id']: item for item in corpus['items']}
        mapping = {}
        for asset, relative in self.store.db.execute('SELECT asset_id,path FROM catalog'):
            opaque = hashlib.sha256(f"{corpus['library_key']}\0{asset}".encode()).hexdigest()
            mapping[opaque] = asset, relative
        if set(ids) - set(corpus_items) or set(ids) - set(mapping):
            raise ValueError('benchmark photos do not belong to the supplied catalog')
        # Verify every current thumbnail before importing any vector.
        staged = []
        for opaque, vector in zip(ids, vectors):
            asset, relative = mapping[opaque]
            digest = hash_file(thumbnail(self.args.library, relative))
            if digest != corpus_items[opaque]['sha256']:
                raise ValueError('thumbnail changed since the benchmark')
            if vector.shape != (self.store.dimension,):
                raise ValueError('benchmark dimension differs from the index')
            staged.append((asset, digest, vector))
        for asset, digest, vector in staged:
            self.store.put(asset, digest, vector)
        return self.status() | {"imported": len(staged)}

    def step(self):
        self.waiting = False
        if not self.running or not self.pending:
            self.running = False
            return
        asset, relative = self.pending.popleft()
        try:
            path = thumbnail(self.args.library, relative)
            digest = hash_file(path)
            if self.store.fresh(asset, digest):
                self.store.fingerprint(asset, digest)
                self.skipped += 1
                if not self.pending:
                    self.running = False
                return
            from .models import load_image
            image = load_image(path)
        except (OSError, ValueError) as error:
            self.store.fail(asset, str(error))
            self.processed += 1
            if not self.pending:
                self.running = False
            return
        try:
            if not self.engine.can_index():
                self.pending.appendleft((asset, relative))
                self.waiting = True
                return
            vector = self.engine.encode_image(image)
            self.last_inference = time.monotonic()
            if hash_file(path) != digest:
                raise ValueError('thumbnail changed during inference')
            self.store.put(asset, digest, vector)
            self.processed += 1
        except Exception as error:
            # A model/device failure ends the whole job, never silently
            # substituting a CPU backend or labelling a partial run complete.
            self.last_error = str(error)
            self.stop()
            raise
        finally:
            if not self.pending:
                self.running = False


def serve(args):
    import fcntl
    import select
    import torch
    torch.set_num_threads(args.threads)
    torch.set_num_interop_threads(1)
    args.socket.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    # A second process must not unlink a live worker's socket or open its DB.
    with args.socket.with_suffix('.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.socket.exists():
            info = args.socket.lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise ValueError('refusing to replace a foreign socket path')
            args.socket.unlink()
        worker = Worker(args)
        catalog = None
        if args.managed_plugins:
            from .managed import Catalog
            catalog = Catalog(args)
        stopping = False

        def stop(*_):
            nonlocal stopping
            stopping = True

        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(args.socket))
                args.socket.chmod(0o600)
                listener.listen(4)
                listener.settimeout(.025)
                print(json.dumps({"event": "ready", "status": worker.status()}), flush=True)
                next_step = 0
                while not stopping:
                    if catalog is not None:
                        if select.select([0], [], [], 0)[0] and not os.read(0, 1):
                            break
                        catalog.poll(worker)
                    worker.release_idle()
                    try:
                        connection, _ = listener.accept()
                    except socket.timeout:
                        started = time.monotonic()
                        if worker.running and started >= next_step:
                            try:
                                worker.step()
                            except Exception as error:
                                print(json.dumps({"event": "error", "error": str(error)}), flush=True)
                            if args.background:
                                finished = time.monotonic()
                                delay = 3 if worker.waiting else min(2, max(.15, finished - started))
                                next_step = finished + delay
                        continue
                    with connection:
                        connection.settimeout(5)
                        try:
                            request = receive(connection)
                            if catalog is not None and request.get("op") == "catalog":
                                raise ValueError("pinholed manages this photo catalog")
                            response = {"ok": True, "result": worker.request(request)}
                        except Exception as error:
                            response = {"ok": False, "error": str(error)}
                        try:
                            send(connection, response)
                        except (BrokenPipeError, ConnectionResetError, socket.timeout):
                            pass
        finally:
            if catalog is not None:
                catalog.close()
            worker.close()
            args.socket.unlink(missing_ok=True)
