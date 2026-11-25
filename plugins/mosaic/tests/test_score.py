from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import random
import unittest

loader = SourceFileLoader("pinhole_mosaic", str(Path(__file__).resolve().parents[1] / "pinhole-mosaic"))
worker = module_from_spec(spec_from_loader(loader.name, loader))
loader.exec_module(worker)

FINE = worker.FINE


def picture(colour):
    """FINE × FINE pixels, each what colour(x, y) says: (r, g, b)."""
    return bytes(value for y in range(FINE) for x in range(FINE) for value in colour(x, y))


class Score(unittest.TestCase):
    def test_shapes_in_colour_beat_a_horizon_fine_detail_and_grey(self):
        blocks = picture(lambda x, y: (230, 40, 30) if (x // 12 + y // 12) % 2 else (20, 60, 220))
        # Wherever it lies, not only between two cells.
        horizon = picture(lambda x, y: (235, 235, 235) if y < 19 else (20, 20, 20))
        random.seed(1)
        noise = picture(lambda x, y: (random.randrange(256), random.randrange(256), random.randrange(256)))
        grey = picture(lambda x, y: (220, 220, 220) if (x // 12 + y // 12) % 2 else (40, 40, 40))
        flat = picture(lambda x, y: (128, 128, 128))
        scores = {name: worker.score(pixels) for name, pixels in
                  dict(blocks=blocks, horizon=horizon, noise=noise, grey=grey, flat=flat).items()}
        self.assertGreater(scores["blocks"], 0.2)
        self.assertGreater(scores["blocks"], 2 * scores["grey"])
        self.assertGreater(scores["grey"], 100 * max(scores["horizon"], scores["noise"]))
        self.assertEqual(scores["flat"], 0)

    def test_the_best_are_one_a_moment(self):
        scores = {"a": ["t", 0.9], "b": ["t", 0.8], "c": ["t", 0.7], "d": ["t", 0]}
        dates = {"a": 1_000_000_000, "b": 1_000_060_000, "c": 2_000_000_000, "d": 3_000_000_000}
        self.assertEqual(worker.best(scores, dates), ["a", "c"])


if __name__ == "__main__":
    unittest.main()
