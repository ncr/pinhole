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
    def test_named_faces_make_one_person_without_hiding_pending_faces(self):
        with sqlite3.connect(":memory:") as db:
            db.executescript("""
                CREATE TABLE faces (key TEXT PRIMARY KEY, photo TEXT, box TEXT);
                CREATE TABLE photos (id TEXT PRIMARY KEY, path TEXT);
                CREATE TABLE suggestions (anchor TEXT, member TEXT, score REAL);
            """)
            faces = ["old", "old-match", "new", "pending"]
            db.executemany("INSERT INTO faces VALUES (?, ?, '[0, 0, 1, 1]')", ((f, f) for f in faces))
            db.executemany("INSERT INTO photos VALUES (?, ?)", ((f, f) for f in faces))
            db.executemany("INSERT INTO suggestions VALUES (?,?,?)", [
                ("old", "old", 1.0), ("old", "old-match", 0.9),
                ("new", "new", 1.0), ("new", "pending", 0.9),
            ])
            worker.review_labels(db, {
                "old": {"person": "Anna"},
                "new": {"person": "Anna"},
                "old-match": {"person": "Ola"},
            })
            people = worker.review_rows(db, "people", None, 0)
            self.assertEqual([(row["name"], row["count"]) for row in people], [("Anna", 2), ("Ola", 1)])
            self.assertEqual({row["key"] for row in worker.review_rows(db, "person", "Anna", 0)}, {"old", "new"})
            groups = worker.review_rows(db, "groups", None, 0)
            self.assertEqual([(row["anchor"], row["key"]) for row in groups], [("new", "pending")])
            self.assertEqual(worker.assignment_keys(db, "new"), ["pending"])

    def test_faces_taken_out_of_one_group_on_two_computers_are_both_out(self):
        with sqlite3.connect(":memory:") as db:
            db.execute("CREATE TABLE suggestions (anchor TEXT, member TEXT, score REAL)")
            db.executemany("INSERT INTO suggestions VALUES ('group', ?, 0.9)", ((str(i),) for i in range(4)))
            # Each computer's under a key of its own, so both are in what
            # is synced once the two have heard of each other.
            here, there = {worker.reject_key("group", "1"): True}, {worker.reject_key("group", "2"): True}
            worker.review_labels(db, {**here, **there, worker.reject_key("group", "3"): None})
            self.assertEqual(worker.assignment_keys(db, "group"), ["0", "3"])

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

    def test_a_face_found_again_in_a_new_thumbnail_keeps_what_was_said_of_it(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            db.execute("CREATE TABLE aliases (old TEXT PRIMARY KEY, new TEXT NOT NULL)")

            def vector(angle):
                v = np.zeros(128, dtype="<f4")
                v[0], v[1] = np.cos(np.deg2rad(angle)), np.sin(np.deg2rad(angle))
                return v.tobytes()
            # The photo turned in iCloud: its face again, a little apart, under
            # a new key, and a face it had not before.
            before = [("a1", vector(6))]
            after = [("a1-turned", vector(8)), ("stranger", vector(70))]
            worker.carry(db, np, before, after)
            self.assertEqual(worker.aliases(db), {"a1": "a1-turned"})
            # Turned again: the first key follows it too.
            worker.carry(db, np, [("a1-turned", vector(8))], [("a1-again", vector(7))])
            self.assertEqual(worker.aliases(db), {"a1": "a1-again", "a1-turned": "a1-again"})
            edits, moved = worker.translated(worker.aliases(db), {
                "a1": {"person": "Anna"}, "reject\tanna\ta1-turned": True})
            self.assertEqual(edits, {"a1-again": {"person": "Anna"}, "reject\tanna\ta1-again": True})
            self.assertEqual(moved, {"a1": "a1-again", "a1-turned": "a1-again"})
            # An edit on the new key wins over the old one's.
            edits, _ = worker.translated(worker.aliases(db), {"a1": {"person": "Anna"}, "a1-again": {"person": "Ola"}})
            self.assertEqual(edits, {"a1-again": {"person": "Ola"}})
            self.assertEqual(worker.with_aliases(db, (["a1"], {"a1-turned": ["a1"]})),
                             (["a1-again"], {"a1-again": ["a1-again"]}))

    def test_a_named_face_found_again_is_still_named(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            db.execute("CREATE TABLE aliases (old TEXT PRIMARY KEY, new TEXT NOT NULL)")
            db.execute("UPDATE faces SET key='a1-turned' WHERE key='a1'")
            db.execute("UPDATE suggestions SET anchor='a1-turned' WHERE anchor='a1'")
            db.execute("UPDATE suggestions SET member='a1-turned' WHERE member='a1'")
            db.execute("INSERT INTO aliases VALUES ('a1', 'a1-turned')")
            worker.review_labels(db, {"a1": {"person": "Anna"}})
            self.assertEqual([(f["key"], f["name"]) for f in worker.photo_rows(db, "a1")], [("a1-turned", "Anna")])

    def test_no_named_people_gives_no_guesses(self):
        with sqlite3.connect(":memory:") as db:
            self.library(db)
            worker.review_labels(db, {})
            self.assertEqual(worker.guesses(db, ["a1", "o1"]), {})


class Edits(unittest.TestCase):
    def test_what_is_said_comes_from_the_synced_file(self):
        import json
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            synced = Path(folder, "pinhole.people.json")
            synced.write_text(json.dumps({"values": {"both": {"not": ["Anna"]}, "reject\tnew\tx": True,
                                                     "reject\tnew\ty": True, "named": {"person": "Ola"}},
                                          "clock": {}}))
            names, excluded = worker.load_edits(synced, None)
            self.assertEqual(names, ["both"])
            self.assertEqual(excluded, {"new": ["x", "y"]})
            # Not there yet: nothing said; a file that cannot be read keeps
            # what was read before.
            self.assertEqual(worker.load_edits(Path(folder, "none"), None), ([], {}))
            synced.write_text("{not json")
            self.assertEqual(worker.load_edits(synced, "before"), "before")

    def test_names_kept_on_this_computer_come_from_its_state(self):
        import json
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            state = Path(folder, "state.json")
            self.assertFalse(worker.load_local(state, "lib", None))
            state.write_text(json.dumps({"local\tlib": True, "local\tlib\tboth": {"not": ["Anna"]},
                                         "local\tlib\treject\tg\tx": True,
                                         "local\tother\treject\tg\ty": True, "shown": True}))
            # Chosen for this library, not for another.
            self.assertTrue(worker.load_local(state, "lib", None))
            self.assertFalse(worker.load_local(state, "other", None))
            self.assertEqual(worker.load_edits(state, None, "local\tlib\t"), (["both"], {"g": ["x"]}))

if __name__ == "__main__":
    unittest.main()
