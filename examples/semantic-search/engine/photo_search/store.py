"""A rebuildable semantic index. This module never opens the photo library."""
import json
import os
from pathlib import Path
import sqlite3
import time

import numpy as np

KIND = "pinhole-semantic-index-v1"
CHUNK = 4096


class Store:
    def __init__(self, path, library_key, identity, dimension):
        if not library_key or not 1 <= dimension <= 4096:
            raise ValueError("invalid index identity")
        self.path = Path(path).resolve()
        expected = {"kind": KIND, "library_key": library_key, "identity": identity, "dimension": dimension}
        existed = self.path.exists()
        if existed:
            # Check ownership read-only before changing SQLite pragmas. In
            # particular, accidentally passing library.sqlite must be safe.
            check = sqlite3.connect(self.path.as_uri() + "?mode=ro", uri=True)
            try:
                tables = {row[0] for row in check.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                found = json.loads(check.execute("SELECT value FROM semantic_meta WHERE key='identity'").fetchone()[0]) if "semantic_meta" in tables else None
                if tables and found != expected:
                    raise ValueError("this file belongs to another index or application")
            finally:
                check.close()
        self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.db = sqlite3.connect(self.path, timeout=2)
        if not existed:
            os.chmod(self.path, 0o600)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=NORMAL")
        self.db.executescript('''
            CREATE TABLE IF NOT EXISTS semantic_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS catalog (
                asset_id TEXT PRIMARY KEY, path TEXT NOT NULL, fingerprint TEXT, active INTEGER NOT NULL DEFAULT 1);
            CREATE TABLE IF NOT EXISTS embeddings (
                asset_id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, vector BLOB NOT NULL, updated_ns INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS failures (
                asset_id TEXT PRIMARY KEY, reason TEXT NOT NULL, updated_ns INTEGER NOT NULL);
        ''')
        self.db.execute("INSERT OR IGNORE INTO semantic_meta VALUES ('identity',?)", (json.dumps(expected, sort_keys=True),))
        self.db.commit()
        self.dimension = dimension
        self.identity = expected
        self.generation = 0
        self.snapshot = None

    def close(self):
        self.db.close()

    def catalog(self, assets):
        if not isinstance(assets, list) or any(not isinstance(x, dict) or 'id' not in x or 'path' not in x for x in assets):
            raise ValueError('catalog must be a list of assets')
        if len(assets) > 100000 or len({x["id"] for x in assets}) != len(assets):
            raise ValueError("duplicate or excessive catalog assets")
        if any(not isinstance(x["id"], str) or not 0 < len(x["id"]) <= 1024 or
               not isinstance(x["path"], str) or len(x["path"]) > 8192 for x in assets):
            raise ValueError("invalid catalog asset")
        with self.db:
            self.db.execute("UPDATE catalog SET active=0")
            self.db.executemany('''INSERT INTO catalog(asset_id,path,active) VALUES (?,?,1)
                ON CONFLICT(asset_id) DO UPDATE SET
                fingerprint=CASE WHEN catalog.path=excluded.path THEN catalog.fingerprint ELSE NULL END,
                path=excluded.path,active=1''', [(x["id"], x["path"]) for x in assets])
            self.db.execute("DELETE FROM embeddings WHERE asset_id IN (SELECT asset_id FROM catalog WHERE active=0)")
            self.db.execute("DELETE FROM failures WHERE asset_id IN (SELECT asset_id FROM catalog WHERE active=0)")
            self.db.execute("DELETE FROM catalog WHERE active=0")
        self.invalidate()

    def invalidate(self):
        self.generation += 1
        self.snapshot = None

    def fresh(self, asset_id, fingerprint):
        row = self.db.execute("SELECT fingerprint FROM embeddings WHERE asset_id=?", (asset_id,)).fetchone()
        return row is not None and row[0] == fingerprint

    def fingerprint(self, asset_id, fingerprint):
        with self.db:
            self.db.execute("UPDATE catalog SET fingerprint=? WHERE asset_id=?", (fingerprint, asset_id))
            self.db.execute("DELETE FROM failures WHERE asset_id=?", (asset_id,))
        self.invalidate()

    def put(self, asset_id, fingerprint, vector):
        vector = np.asarray(vector, dtype="<f4")
        if (vector.shape != (self.dimension,) or not np.isfinite(vector).all() or
                abs(float(np.linalg.norm(vector)) - 1) > 1e-5):
            raise ValueError("invalid normalized embedding")
        with self.db:
            if self.db.execute("SELECT 1 FROM catalog WHERE asset_id=?", (asset_id,)).fetchone() is None:
                raise ValueError("embedding is not in this library catalog")
            self.db.execute("UPDATE catalog SET fingerprint=? WHERE asset_id=?", (fingerprint, asset_id))
            self.db.execute("INSERT OR REPLACE INTO embeddings VALUES (?,?,?,?)",
                            (asset_id, fingerprint, vector.tobytes(), time.time_ns()))
            self.db.execute("DELETE FROM failures WHERE asset_id=?", (asset_id,))
        self.invalidate()

    def fail(self, asset_id, reason):
        with self.db:
            self.db.execute("UPDATE catalog SET fingerprint=NULL WHERE asset_id=?", (asset_id,))
            self.db.execute("INSERT OR REPLACE INTO failures VALUES (?,?,?)", (asset_id, str(reason)[:2048], time.time_ns()))
        self.invalidate()

    def status(self):
        indexed = self.db.execute('''SELECT count(*) FROM embeddings e JOIN catalog c USING(asset_id)
            WHERE e.fingerprint=c.fingerprint AND c.active=1''').fetchone()[0]
        return {"indexed": indexed, "total": self.db.execute("SELECT count(*) FROM catalog").fetchone()[0],
                "errors": self.db.execute("SELECT count(*) FROM failures").fetchone()[0],
                "dimension": self.dimension}

    def vector_for(self, asset_id):
        row = self.db.execute('''SELECT e.vector FROM embeddings e JOIN catalog c USING(asset_id)
            WHERE e.asset_id=? AND e.fingerprint=c.fingerprint AND c.active=1''', (asset_id,)).fetchone()
        if row is None:
            return None
        vector = np.frombuffer(row[0], dtype="<f4")
        if (vector.shape != (self.dimension,) or not np.isfinite(vector).all() or
                abs(float(np.linalg.norm(vector)) - 1) > 1e-5):
            raise ValueError("invalid stored embedding")
        return vector

    def search(self, vector, limit=100):
        if not 1 <= limit <= 500:
            raise ValueError("invalid result limit")
        vector = np.asarray(vector, dtype=np.float32)
        if vector.shape != (self.dimension,) or not np.isfinite(vector).all() or abs(float(np.linalg.norm(vector)) - 1) > 1e-5:
            raise ValueError("invalid normalized query vector")
        ids, matrix = self.prepare()
        scores = np.empty(len(ids), dtype=np.float32)
        for start in range(0, len(ids), CHUNK):
            scores[start:start + CHUNK] = matrix[start:start + CHUNK].astype(np.float32) @ vector
        order = np.argsort(-scores, kind="stable")[:limit]
        return [{"id": ids[i], "score": float(scores[i])} for i in order]

    def prepare(self):
        """The index in memory, half its stored size: half-precision vectors
        rank as the stored ones do, read a chunk at a time."""
        if self.snapshot is not None:
            return self.snapshot
        rows = self.db.execute('''SELECT e.asset_id,e.vector FROM embeddings e JOIN catalog c USING(asset_id)
            WHERE e.fingerprint=c.fingerprint AND c.active=1 ORDER BY e.asset_id''')
        ids, chunks = [], []
        while block := rows.fetchmany(CHUNK):
            if len(ids) + len(block) > 100000 or any(len(row[1]) != self.dimension * 4 for row in block):
                raise ValueError("corrupt or excessive embedding index")
            matrix = np.frombuffer(b"".join(row[1] for row in block), dtype="<f4").reshape(len(block), self.dimension)
            if not np.isfinite(matrix).all() or np.any(np.abs(np.linalg.norm(matrix, axis=1) - 1) > 1e-5):
                raise ValueError("invalid stored embedding")
            ids.extend(row[0] for row in block)
            chunks.append(matrix.astype(np.float16))
        matrix = np.concatenate(chunks) if chunks else np.empty((0, self.dimension), dtype=np.float16)
        self.snapshot = ids, matrix
        return self.snapshot
