from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import sqlite3
import unittest

import numpy as np


loader = SourceFileLoader("pinhole_faces", str(Path(__file__).resolve().parents[1] / "pinhole-faces"))
worker = module_from_spec(spec_from_loader(loader.name, loader))
loader.exec_module(worker)


class Suggestions(unittest.TestCase):
    def groups(self, photos, angles, names=(), rejected=None):
        faces = [dict(key=str(i), id=photo, box=[0, 0, 1, 1])
                 for i, photo in enumerate(photos)]
        theta = np.deg2rad(angles)
        vectors = np.column_stack((np.cos(theta), np.sin(theta)))
        return [(anchor, face) for anchor, face, _ in
                worker.group_faces(faces, vectors, names, rejected or {})]

    def test_a_match_does_not_pull_an_unrelated_face_into_its_group(self):
        self.assertEqual(self.groups(["a", "b", "c"], [0, 45, 90]),
                         [("0", "0"), ("0", "1")])

    def test_only_one_face_per_photo_is_suggested(self):
        self.assertEqual(self.groups(["a", "b", "b", "a"], [0, 0, 1, 2]),
                         [("0", "0"), ("0", "1"), ("2", "2"), ("2", "3")])

    def test_rejections_and_named_anchors_survive_regrouping(self):
        self.assertEqual(self.groups(["a", "b", "c"], [0, 1, 2],
                                     names=["0", "1"], rejected={"0": ["2"]}),
                         [("0", "0"), ("1", "1"), ("1", "2")])

    def test_zero_faces_gives_no_groups(self):
        self.assertEqual(self.groups([], []), [])


class Review(unittest.TestCase):
    def test_legacy_and_explicit_labels_share_one_person_without_hiding_pending_faces(self):
        with sqlite3.connect(":memory:") as db:
            db.executescript("""
                CREATE TABLE faces (key TEXT PRIMARY KEY, photo TEXT, box TEXT);
                CREATE TABLE photos (id TEXT PRIMARY KEY, path TEXT);
                CREATE TABLE suggestions (anchor TEXT, member TEXT, score REAL);
            """)
            faces = ["old", "old-match", "rejected", "new", "pending"]
            db.executemany("INSERT INTO faces VALUES (?, ?, '[0, 0, 1, 1]')", ((f, f) for f in faces))
            db.executemany("INSERT INTO photos VALUES (?, ?)", ((f, f) for f in faces))
            db.executemany("INSERT INTO suggestions VALUES (?,?,?)", [
                ("old", "old", 1.0), ("old", "old-match", 0.9), ("old", "rejected", 0.8),
                ("new", "new", 1.0), ("new", "pending", 0.9),
            ])
            worker.review_labels(db, {
                "old": {"name": "Anna", "rejected": ["rejected"]},
                "new": {"person": "Anna"},
                "old-match": {"person": "Ola"},
            })
            people = worker.review_rows(db, "people", None, 0)
            self.assertEqual([(row["name"], row["count"]) for row in people], [("Anna", 2), ("Ola", 1)])
            self.assertEqual({row["key"] for row in worker.review_rows(db, "person", "Anna", 0)}, {"old", "new"})
            groups = worker.review_rows(db, "groups", None, 0)
            self.assertEqual([(row["anchor"], row["key"]) for row in groups], [("new", "pending")])
            self.assertEqual(worker.assignment_keys(db, "new"), ["pending"])

    def test_assignment_covers_faces_beyond_the_visible_page(self):
        with sqlite3.connect(":memory:") as db:
            db.execute("CREATE TABLE suggestions (anchor TEXT, member TEXT, score REAL)")
            db.executemany("INSERT INTO suggestions VALUES ('group', ?, 0.9)", ((str(i),) for i in range(65)))
            worker.review_labels(db, {"0": {"person": "Anna"}})
            self.assertEqual(set(worker.assignment_keys(db, "group")), {str(i) for i in range(1, 65)})


if __name__ == "__main__":
    unittest.main()
