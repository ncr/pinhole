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


class JoiningSuggestions(unittest.TestCase):
    def joined(self, photos, angles, protected=(), wider=False):
        faces = [dict(key=str(i), id=photo) for i, photo in enumerate(photos)]
        theta = np.deg2rad(angles)
        vectors = np.column_stack((np.cos(theta), np.sin(theta)))
        suggestions = [(str(i // 2 * 2), str(i), 1.0) for i in range(len(faces))]
        return {member: anchor for anchor, member, _ in
                worker.join_groups(faces, vectors, suggestions, set(protected), 0.62, wider=wider)}

    def test_groups_need_two_matches_without_chaining_through_adopted_faces(self):
        self.assertEqual(self.joined(list("abcdef"), [0, 4, 45, 49, 90, 94]),
                         {"0": "0", "1": "0", "2": "0", "3": "0", "4": "4", "5": "4"})
        self.assertEqual(self.joined(list("abcd"), [0, 110, 20, 40]),
                         {"0": "0", "1": "0", "2": "2", "3": "2"})

    def test_wider_checks_matches_outside_the_six_original_samples(self):
        angles = [0, 0, 0, 10, 0, 0, 12, 0] + [60, 60, 60, 10, 60, 60, 12, 60]
        theta = np.deg2rad(angles)
        vectors = np.column_stack((np.cos(theta), np.sin(theta)))
        faces = [dict(key=str(i), id=str(i)) for i in range(len(angles))]
        suggestions = [(str(i // 8 * 8), str(i), 1.0) for i in range(len(angles))]
        careful = worker.join_groups(faces, vectors, suggestions, set(), 0.62)
        self.assertEqual({anchor for anchor, _, _ in careful}, {"0", "8"})
        wider = worker.join_groups(faces, vectors, suggestions, set(), 0.58, wider=True)
        self.assertEqual({anchor for anchor, _, _ in wider}, {"0"})

    def test_overlapping_photos_and_groups_with_saved_edits_stay_separate(self):
        separate = {"0": "0", "1": "0", "2": "2", "3": "2"}
        for wider in (False, True):
            self.assertEqual(self.joined(list("abac"), [0, 4, 20, 24], wider=wider), separate)
            self.assertEqual(self.joined(list("abcd"), [0, 4, 20, 24], protected=["2"], wider=wider), separate)


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


class Guesses(unittest.TestCase):
    def library(self, db):
        db.executescript("""
            CREATE TABLE photos (id TEXT PRIMARY KEY, path TEXT);
            CREATE TABLE faces (key TEXT PRIMARY KEY, photo TEXT, box TEXT, vector BLOB);
            CREATE TABLE suggestions (anchor TEXT, member TEXT, score REAL);
        """)
        # Anna looks one way, Ola at right angles to her; the groups lean
        # towards one, or (far) towards neither.
        angles = {"anna": 0, "anna2": 4, "ola": 90, "a1": 6, "a2": 10, "o1": 85, "far": None}
        for key, angle in angles.items():
            vector = np.zeros(128, dtype="<f4")
            if angle is None:
                vector[2] = 1
            else:
                vector[0], vector[1] = np.cos(np.deg2rad(angle)), np.sin(np.deg2rad(angle))
            db.execute("INSERT INTO photos VALUES (?, ?)", (key, key + ".jpg"))
            db.execute("INSERT INTO faces VALUES (?, ?, '[0, 0, 1, 1]', ?)", (key, key, vector.tobytes()))
        db.executemany("INSERT INTO suggestions VALUES (?,?,?)", [
            ("a1", "a1", 1.0), ("a1", "a2", 0.9), ("o1", "o1", 1.0), ("far", "far", 1.0)])

    def test_a_group_is_guessed_to_be_the_named_person_it_looks_like(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {"anna": {"person": "Anna"}, "anna2": {"person": "Anna"}, "ola": {"person": "Ola"}})
            rows = worker.with_guesses(db, worker.review_rows(db, "groups", None, 0))
            self.assertEqual({row["anchor"]: row["guess"] for row in rows}, {"a1": "Anna", "o1": "Ola", "far": None})
            self.assertEqual([row["anchor"] for row in worker.likely_rows(db, "Anna", 0)], ["a1"])
            people = worker.with_likely(db, worker.review_rows(db, "people", None, 0))
            self.assertEqual({row["name"]: row["likely"] for row in people}, {"Anna": 1, "Ola": 1})

    def test_a_group_said_to_be_someone_else_is_not_guessed_again(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {"anna": {"person": "Anna"}, "a1": {"not": ["Anna"]}})
            self.assertEqual(worker.likely_rows(db, "Anna", 0), [])

    def test_a_group_not_known_comes_apart_from_those_to_name(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {"a1": {"unknown": True}})
            rows = worker.review_rows(db, "groups", None, 0)
            self.assertEqual([(row["anchor"], row["unknown"]) for row in rows][-1], ("a1", True))
            self.assertEqual([row["anchor"] for row in worker.review_rows(db, "groups", None, 0, known=False)], ["a1"])
            self.assertNotIn("a1", [row["anchor"] for row in worker.review_rows(db, "groups", None, 0, known=True)])

    def test_a_photo_says_who_each_face_is_or_looks_like(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            db.execute("INSERT INTO faces VALUES ('a1-2', 'a1', '[0.5, 0, 0.2, 0.2]', ?)", (db.execute(
                "SELECT vector FROM faces WHERE key='o1'").fetchone()[0],))
            worker.review_labels(db, {"anna": {"person": "Anna"}, "anna2": {"person": "Anna"}, "ola": {"person": "Ola"}})
            faces = {face["key"]: face for face in worker.photo_rows(db, "a1")}
            # In a group that looks like Anna; alone, like Ola.
            self.assertEqual((faces["a1"]["name"], faces["a1"]["guess"], faces["a1"]["anchor"]), (None, "Anna", "a1"))
            self.assertEqual((faces["a1-2"]["guess"], faces["a1-2"]["anchor"]), ("Ola", None))
            self.assertEqual([(face["name"], face["guess"]) for face in worker.photo_rows(db, "anna")], [("Anna", None)])
            self.assertEqual(worker.photo_rows(db, "nowhere"), [])
            self.assertEqual(worker.known_names(db), ["Anna", "Ola"])

    def test_a_face_said_not_to_be_someone_is_not_guessed_to_be_them(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {"anna": {"person": "Anna"}, "a1": {"not": ["Anna"]}})
            self.assertEqual([face["guess"] for face in worker.photo_rows(db, "a1")], [None])

    def test_a_face_in_a_group_said_not_to_be_someone_is_not_guessed_to_be_them(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {"anna": {"person": "Anna"}, "a1": {"not": ["Anna"]}})
            self.assertEqual([face["guess"] for face in worker.photo_rows(db, "a2")], [None])

    def test_a_face_named_with_an_old_group_says_which_to_take_it_from(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {"a1": {"name": "Anna"}})
            self.assertEqual([(f["name"], f["legacy"]) for f in worker.photo_rows(db, "a2")], [("Anna", "a1")])
            self.assertEqual([(f["name"], f["legacy"]) for f in worker.photo_rows(db, "a1")], [("Anna", "a1")])
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {"a1": {"name": "Anna", "rejected": ["a2"]}})
            self.assertEqual([(f["name"], f["legacy"]) for f in worker.photo_rows(db, "a2")], [(None, None)])

    def test_no_named_people_gives_no_guesses(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {})
            self.assertEqual(worker.guesses(db, ["a1", "o1"]), {})


if __name__ == "__main__":
    unittest.main()
