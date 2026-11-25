from contextlib import closing
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest
import zlib

import numpy as np


loader = SourceFileLoader("pinhole_screens", str(Path(__file__).resolve().parents[1] / "pinhole-screens"))
worker = module_from_spec(spec_from_loader(loader.name, loader))
loader.exec_module(worker)
D = worker.DIMENSION


def head(threshold=0.5):
    """A head of the real one's shape that says 'screen' by the first
    number alone: likely when it is high."""
    weights = np.zeros(D, np.float32)
    weights[0] = 4
    return dict(size=320, mean=np.full(D, 3, np.float32), scale=np.ones(D, np.float32),
                weights=weights, bias=0.0, threshold=threshold)


def row(first, pattern=None):
    """A photo's numbers as kept (bytes): its first number `first` (0–255),
    the rest a pattern that says what it looks like."""
    rng = np.random.default_rng(pattern)
    values = rng.integers(0, 256, D).astype(np.uint8) if pattern is not None else np.full(D, 128, np.uint8)
    values[0] = first
    return values


class Head(unittest.TestCase):
    def test_the_head_in_the_plugin_is_of_this_recipe_and_sane(self):
        real = worker.load_head()
        self.assertEqual(real["weights"].shape, (D,))
        self.assertTrue(0 < real["threshold"] < 1)
        self.assertGreater(real["size"], 0)
        self.assertTrue((real["scale"] > 0).all())


class Judged(unittest.TestCase):
    def test_those_the_head_finds_likely_are_found(self):
        # 255 * 6/255 = 6, less the mean 3: likely; 0: not.
        matrix = np.stack([row(255), row(0)])
        self.assertEqual(worker.judged(head(), ["a", "b"], matrix, {}), ["a"])

    def test_what_was_said_is_so_either_way(self):
        matrix = np.stack([row(255), row(0)])
        said = {"a": False, "b": True}
        answered = [(matrix[0], False), (matrix[1], True)]
        self.assertEqual(worker.judged(head(), ["a", "b"], matrix, said, answered), ["b"])

    def test_a_photo_alike_one_said_of_goes_its_way_and_others_stay(self):
        # b is a twin of a but for its first number; c looks like nothing else.
        a, b, c = row(40, 1), row(60, 1), row(60, 2)
        matrix = np.stack([a, b, c])
        self.assertEqual(worker.judged(head(), ["a", "b", "c"], matrix, {}), [])
        found = worker.judged(head(), ["a", "b", "c"], matrix, {"a": True}, [(a, True)])
        self.assertEqual(found, ["a", "b"])

    def test_said_of_a_photo_not_in_the_library_does_no_harm(self):
        self.assertEqual(worker.judged(head(), [], np.empty((0, D), np.uint8), {"x": True, "y": False}), ["x"])


class Said(unittest.TestCase):
    def test_only_is_keys_with_a_yes_or_a_no_are_read(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "synced.json"
            path.write_text(json.dumps({"values": {"is\ta": True, "is\tb": False, "is\tc": "yes",
                                                   "name\td": True}, "clock": {}}))
            self.assertEqual(worker.load_said(path, {}), {"a": True, "b": False})

    def test_no_file_yet_is_nothing_said_and_a_broken_one_keeps_what_was(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "synced.json"
            self.assertEqual(worker.load_said(path, {"a": True}), {})
            path.write_text("{")
            self.assertEqual(worker.load_said(path, {"a": True}), {"a": True})


class Scan(unittest.TestCase):
    """The worker's round over a library, with a stand-in for the model."""

    class Engine:
        def __init__(self):
            self.seen = []

        def vector(self, data):
            self.seen.append(data)
            if data == b"broken":
                raise ValueError("Unreadable thumbnail")
            # Each thumbnail a look of its own: none is another's twin.
            return row(255 if data.startswith(b"screen") else 0, zlib.crc32(data)).tobytes()

    def library(self, folder, photos):
        (folder / "thumbs").mkdir(exist_ok=True)
        for asset, data in photos.items():
            (folder / "thumbs" / f"{asset}.jpg").write_bytes(data)
        return [(asset, f"thumbs/{asset}.jpg") for asset in sorted(photos)]

    def test_a_round_keeps_what_it_made_and_finds_the_screens(self):
        with tempfile.TemporaryDirectory() as name:
            folder = Path(name)
            rows = self.library(folder, {"a": b"screen 1", "b": b"a cat", "c": b"broken"})
            engine = self.Engine()
            with closing(worker.connect(folder / "cache")) as db:
                errors = worker.scan(db, folder.resolve(), rows, lambda: engine, lambda done, total: None)
                self.assertEqual(errors, 1)
                self.assertEqual(worker.judge(db, head(), {}), ["a"])
                # Nothing changed: nothing looked at again, the broken one
                # neither.
                engine.seen.clear()
                worker.scan(db, folder.resolve(), rows, lambda: engine, lambda done, total: None)
                self.assertEqual(engine.seen, [])
                # A photo gone is dropped; one changed is looked at again.
                (folder / "thumbs/c.jpg").unlink()
                self.library(folder, {"b": b"screen now"})
                rows = [r for r in rows if r[0] != "c"]
                worker.scan(db, folder.resolve(), rows, lambda: engine, lambda done, total: None)
                self.assertEqual(engine.seen, [b"screen now"])
                self.assertEqual(worker.judge(db, head(), {"a": False}), ["b"])
                self.assertEqual(db.execute("SELECT count(*) FROM photos").fetchone(), (2,))

    def test_found_is_written_only_when_it_changes(self):
        with tempfile.TemporaryDirectory() as name:
            cache = Path(name)
            self.assertTrue(worker.write_found(cache, ["a"]))
            self.assertFalse(worker.write_found(cache, ["a"]))
            self.assertEqual(json.loads((cache / "found.json").read_text())["ids"], ["a"])

    def test_another_recipes_numbers_go(self):
        with tempfile.TemporaryDirectory() as name:
            cache = Path(name)
            (cache / "older-v0.sqlite").write_bytes(b"")
            with closing(worker.connect(cache)):
                pass
            self.assertEqual(sorted(p.name for p in cache.glob("*.sqlite")), [worker.RECIPE + ".sqlite"])

    def test_another_librarys_catalog_is_not_read(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "library.sqlite"
            with closing(sqlite3.connect(path)) as db, db:
                db.execute("CREATE TABLE meta (key TEXT, value TEXT)")
                db.execute("CREATE TABLE assets (id TEXT, thumb_path TEXT, is_video INTEGER)")
                db.execute("INSERT INTO meta VALUES ('source', 'icloud:someone')")
                db.execute("INSERT INTO assets VALUES ('a', 'thumbs/a.jpg', 0), ('v', 'thumbs/v.jpg', 1)")
            with closing(worker.readonly(path)) as catalog:
                self.assertIsNone(worker.catalog_rows(catalog, "icloud:me"))
                self.assertEqual(worker.catalog_rows(catalog, "icloud:someone"), [("a", "thumbs/a.jpg")])


if __name__ == "__main__":
    unittest.main()
