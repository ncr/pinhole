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
# Pinhole open asks for the model every 20 seconds (warm); it is held this
# long past each ask, so a window that is busy a moment does not lose it.
HOLD_SECONDS = 90
# A search holds indexing off this long past each question: one photo
# indexed takes the GPU about a second, which a question would wait for.
ASKING = 10

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
        self.held_until = 0
        self.tag_vectors = None
        self.pending = []
        self.running = False
        self.waiting = False
        # Waiting for the person to step away from the screen: indexing on
        # the GPU that draws it makes the desktop stutter (away.py).
        self.yielding = False
        self.yielded_at = None
        self.catalog_ready = False
        self.last_error = ''
        self.processed = self.skipped = 0
        self.stamps = {}
        # The indexed photos still to be checked in the background, a few
        # at a time while it has nothing else to do (sweep).
        self.unswept = None
        # When someone last asked something: indexing holds off a while
        # after, so the next question of a search being typed never waits
        # for a photo being indexed on the GPU.
        self.asked_at = None

    def close(self):
        try:
            self.engine.close()
        finally:
            self.store.close()

    def held(self):
        """Pinhole is open: search must answer at once, so the model stays
        loaded whatever else wants the memory."""
        return time.monotonic() < self.held_until

    def release(self):
        """The model let go, its memory the desktop's, unless Pinhole holds it."""
        if self.held() or self.engine.idle_seconds is None:
            return False
        self.engine.close()
        self.last_inference = None
        return True

    def yield_to_person(self, here):
        """Background indexing waits while someone uses the screen, the
        model let go after a minute of that unless Pinhole holds it."""
        if not here:
            self.yielding, self.yielded_at = False, None
            return False
        if not self.yielding:
            self.yielding, self.yielded_at = True, time.monotonic()
        elif (self.engine.idle_seconds is not None and self.last_inference is not None
                and time.monotonic() - max(self.yielded_at, self.last_inference) >= self.engine.idle_seconds):
            self.release()
        return True

    def release_idle(self):
        delay = self.engine.idle_seconds
        if (delay is not None and not self.running and self.last_inference is not None
                and time.monotonic() - self.last_inference >= delay):
            self.release()

    def status(self):
        return self.store.status() | {"running": self.running, "waiting": self.waiting, "pending": len(self.pending),
                                      "yielding": self.yielding and self.running,
                                      "catalog_ready": self.catalog_ready, "managed": bool(getattr(self.args, "managed_plugins", None)),
                                      "processed": self.processed, "skipped": self.skipped,
                                      "error": self.last_error, "backend": self.args.backend}

    def refresh(self, assets=None):
        """Check indexed local files (all, or those of `assets`, ids);
        same-path replacements must not rank.

        Hash once after a worker restart, then only after stat changes. The
        stat cache is ephemeral; vectors remain bound to content SHA-256.
        """
        sql = '''SELECT c.asset_id,c.path,e.fingerprint FROM catalog c
            JOIN embeddings e USING(asset_id) WHERE c.fingerprint=e.fingerprint AND c.active=1'''
        if assets is None:
            rows = self.store.db.execute(sql).fetchall()
        else:
            rows = []
            for start in range(0, len(assets), 500):
                chunk = list(assets[start:start + 500])
                rows += self.store.db.execute(sql + f' AND c.asset_id IN ({",".join("?" * len(chunk))})', chunk).fetchall()
        for asset, relative, fingerprint in rows:
            # The common case, every query: one stat of the file checked
            # before, the same as then (any other file has another inode).
            known = self.stamps.get(asset)
            if known is not None and known[0] == relative:
                try:
                    info = os.stat(known[1])
                    if known[2:] == (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino, info.st_dev):
                        continue
                except OSError:
                    pass
            try:
                path = thumbnail(self.args.library, relative)
                info = path.stat()
                stamp = (relative, str(path), info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino, info.st_dev)
                if self.stamps.get(asset) != stamp:
                    if hash_file(path) != fingerprint:
                        raise ValueError('thumbnail changed; index it again')
                    self.stamps[asset] = stamp
            except (OSError, ValueError) as error:
                self.store.fail(asset, str(error))
                self.stamps.pop(asset, None)

    def sweep(self, count=100):
        """The next `count` indexed photos checked (refresh), between other
        work: all of them in turn after a start or a new catalog, without
        a question ever waiting for 35,000 stats or a first hash of each.
        True while some are left."""
        if self.unswept is None:
            self.unswept = deque(asset for (asset,) in self.store.db.execute(
                'SELECT asset_id FROM embeddings ORDER BY asset_id'))
        if not self.unswept:
            return False
        chunk = [self.unswept.popleft() for _ in range(min(count, len(self.unswept)))]
        self.refresh(chunk)
        return bool(self.unswept)

    def encode_text(self, texts):
        try:
            return self.engine.encode_text(texts)
        finally:
            self.last_inference = time.monotonic()

    def suggest_tags(self, asset):
        if not isinstance(asset, str) or not 0 < len(asset) <= 1024:
            raise ValueError('invalid photo id')
        self.refresh([asset])
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
        self.release()

    def asking(self):
        """Someone searched in the last ASKING seconds."""
        return self.asked_at is not None and time.monotonic() - self.asked_at < ASKING

    def request(self, message):
        op = message.get('op')
        if op in ('query', 'suggest_tags'):
            self.asked_at = time.monotonic()
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
            before = self.store.generation
            self.store.catalog(assets)
            active = {x['id'] for x in assets}
            self.stamps = {key: value for key, value in self.stamps.items() if key in active}
            if self.store.generation != before:
                # Every indexed photo checked again, in the background: a
                # question meanwhile checks what it found itself.
                self.unswept = None
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
            if not self.store.status()['indexed']:
                return {"results": [], "seconds": 0, "status": self.status()}
            started = time.perf_counter()
            vector = self.encode_text([text])[0]
            # Only what was found checked, not every photo indexed (35,000
            # stats and a join, most of a question's time): one that
            # changed is failed, and the search asked again without it.
            for _ in range(5):
                results = self.store.search(vector, limit)
                generation = self.store.generation
                self.refresh([result["id"] for result in results])
                if self.store.generation == generation:
                    break
            return {"results": results, "seconds": time.perf_counter() - started, "status": self.status()}
        if op == 'warm':
            # Pinhole open: the model loaded and held while it asks again,
            # and the index read into memory, so a question waits for
            # neither. A model that cannot start is said, not raised.
            self.held_until = time.monotonic() + HOLD_SECONDS
            if self.store.status()['indexed']:
                self.store.prepare()
                if self.engine.loaded():
                    self.last_inference = time.monotonic()
                else:
                    try:
                        self.encode_text(['warm'])
                    except (OSError, RuntimeError, ValueError) as error:
                        return self.status() | {"warm": False, "reason": str(error)}
            return self.status() | {"warm": True}
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
                # Short of memory: given back, unless Pinhole holds it.
                self.release()
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
            # Broken, not idle: let go even if held, to start afresh.
            if self.engine.idle_seconds is not None:
                self.engine.close()
                self.last_inference = None
            raise
        finally:
            if not self.pending:
                self.running = False


def answer(worker, line):
    """One of pinholed's questions, {"ask": n, "request": {...}}, answered
    on stdout with its number: {"answer": n, "result": ...} or "error"."""
    try:
        question = json.loads(line)
        number = question["ask"]
    except (ValueError, KeyError, TypeError):
        return
    try:
        request = question.get("request")
        if not isinstance(request, dict):
            raise ValueError("a request is an object")
        if request.get("op") == "catalog":
            raise ValueError("pinholed manages this photo catalog")
        said = {"answer": number, "result": worker.request(request)}
    except Exception as error:
        said = {"answer": number, "error": str(error)}
    print(json.dumps(said, ensure_ascii=False), flush=True)


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
                from .away import Away
                away = Away()
                asked = bytearray()
                while not stopping:
                    if catalog is not None:
                        # pinholed's questions (pinhole.ask), a line each;
                        # its stdin closed, the end.
                        if select.select([0], [], [], 0)[0]:
                            block = os.read(0, 1 << 16)
                            if not block:
                                break
                            asked.extend(block)
                            while b'\n' in asked:
                                line, _, rest = bytes(asked).partition(b'\n')
                                asked[:] = rest
                                answer(worker, line)
                        catalog.poll(worker)
                    worker.release_idle()
                    try:
                        connection, _ = listener.accept()
                    except socket.timeout:
                        started = time.monotonic()
                        if (worker.running and started >= next_step and args.background
                                and worker.yield_to_person(away() is False)):
                            next_step = started + 5
                        elif not worker.running and worker.sweep():
                            pass
                        elif worker.running and started >= next_step and not worker.asking():
                            try:
                                worker.step()
                                # Pinhole tells the plugin (workerSaid), which
                                # asks for the status then, not on a timer.
                                print(json.dumps({"event": "progress"}), flush=True)
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
