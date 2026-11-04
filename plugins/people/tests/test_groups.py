from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
