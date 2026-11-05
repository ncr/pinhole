"""Corpus isolation and retrieval metrics, without models or private photos."""
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

loader = importlib.machinery.SourceFileLoader("photo_search_benchmark", str(Path(__file__).resolve().parents[1] / "photo-search-benchmark"))
spec = importlib.util.spec_from_loader(loader.name, loader)
benchmark = importlib.util.module_from_spec(spec)
loader.exec_module(benchmark)
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


class PhotoSearchBenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.library = self.root / "library"
        self.library.mkdir()
        self.db = self.library / "library.sqlite"
        con = sqlite3.connect(self.db)
        con.execute("CREATE TABLE assets (id TEXT, thumb_path TEXT, asset_date INTEGER, item_type TEXT, thumb_w INTEGER, thumb_h INTEGER, is_video INTEGER)")
        for i in range(12):
            (self.library / f"{i}.jpg").write_bytes(f"synthetic thumbnail {i}".encode())
            con.execute("INSERT INTO assets VALUES (?,?,?,?,?,?,?)",
                        (str(i), f"{i}.jpg", 1704067200000, "public.jpeg", 480, 320, i == 11))
        con.execute("INSERT INTO assets VALUES ('missing','gone.jpg',0,'public.jpeg',1,1,0)")
        con.execute("INSERT INTO assets VALUES ('escape','../outside.jpg',0,'public.jpeg',1,1,0)")
        con.commit()
        con.close()
        (self.root / "outside.jpg").write_bytes(b"not in the library")
        self.output = self.root / "corpus"

    def prepare(self):
        return benchmark.prepare(self.library, self.output, 10, "seed")

    def ranking_fixture(self):
        self.prepare()
        corpus, ids = benchmark.validate_corpus(self.output / "corpus.json")
        ids = sorted(ids)
        judgments = {"schema": 1, "corpus_id": corpus["corpus_id"], "queries": [
            {"id": "q", "text": "pies", "complete": True, "relevance": {ids[0]: 2, ids[2]: 1}}]}
        run = {"schema": 1, "corpus_id": corpus["corpus_id"], "queries": {"q": "pies"}, "rankings": {"q": ids}}
        benchmark.write_json(self.output / "judgments.json", judgments)
        benchmark.write_json(self.output / "run.json", run)
        return judgments, run, ids

    def score(self, k=2):
        return benchmark.evaluate(self.output / "corpus.json", self.output / "judgments.json", self.output / "run.json", k)

    def test_sample_is_deterministic_isolated_and_excludes_videos_and_unsafe_paths(self):
        before = self.db.read_bytes()
        result = self.prepare()
        other = self.root / "second"
        benchmark.prepare(self.library, other, 10, "seed")
        self.assertEqual((self.output / "corpus.json").read_bytes(), (other / "corpus.json").read_bytes())
        self.assertEqual(result["photos"], 10)
        self.assertEqual(result["eligible"], 11)
        self.assertEqual(result["missing"], 2)
        self.assertEqual(self.db.read_bytes(), before)
        corpus, _ = benchmark.validate_corpus(self.output / "corpus.json")
        self.assertEqual({x["year"] for x in corpus["items"]}, {2024})
        self.assertNotIn(str(self.library), json.dumps(corpus))
        with self.assertRaises(ValueError):
            self.prepare()
        with self.assertRaises(ValueError):
            benchmark.prepare(self.library, self.library / "bad", 10, "seed")

    def test_live_wal_is_read(self):
        con = sqlite3.connect(self.db)
        self.addCleanup(con.close)
        con.execute("PRAGMA journal_mode=WAL")
        con.execute("UPDATE assets SET is_video=1")
        con.execute("UPDATE assets SET is_video=0 WHERE id='0'")
        con.commit()
        self.assertEqual(self.prepare()["photos"], 1)

    def test_metrics_have_known_denominators_and_graded_ndcg(self):
        self.ranking_fixture()
        result = self.score()
        self.assertEqual(result["macro"]["precision"], .5)
        self.assertEqual(result["macro"]["recall"], .5)
        self.assertEqual(result["macro"]["reciprocal_rank"], 1)
        import math
        self.assertAlmostEqual(result["macro"]["ndcg"], 3 / (3 + 1 / math.log2(3)))

    def test_review_keeps_unjudged_photos_and_escapes_query_html(self):
        from photo_search.review import export_review
        import re
        judgments, _, ids = self.ranking_fixture()
        judgments["queries"][0].update(text='</script><script>bad()</script> __SCRIPT__',
                                      complete=False, relevance={ids[0]: 0, ids[1]: 2})
        benchmark.write_json(self.output / "judgments.json", judgments)
        output = self.root / "review.html"
        export_review(self.output / "corpus.json", self.output / "judgments.json", output, benchmark.validate_corpus)
        html = output.read_text()
        payload = json.loads(re.search(r'<script type="application/json" id="payload">(.*?)</script>', html, re.S).group(1))
        self.assertEqual(payload["judgments"], judgments)
        self.assertEqual(set(payload["orders"]["q"]), set(ids))
        self.assertEqual(len(payload["images"]), len(ids))
        self.assertNotIn("<script>bad()", html)
        self.assertNotIn(str(self.library), html)
        self.assertIn("connect-src 'none'", html)
        self.assertEqual(output.stat().st_mode & 0o777, 0o600)
        self.assertNotIn("rankings", payload)
        before = output.read_bytes()
        with self.assertRaises(FileExistsError):
            export_review(self.output / "corpus.json", self.output / "judgments.json", output, benchmark.validate_corpus)
        self.assertEqual(output.read_bytes(), before)

    def test_review_rejects_foreign_or_invalid_judgments_before_writing(self):
        from photo_search.review import export_review
        judgments, _, _ = self.ranking_fixture()
        for change in [lambda x: x.update(corpus_id="foreign"),
                       lambda x: x["queries"][0].update(relevance={"other": 2}),
                       lambda x: x["queries"][0].update(complete=1),
                       lambda x: x["queries"].append(x["queries"][0])]:
            value = json.loads(json.dumps(judgments)); change(value)
            benchmark.write_json(self.output / "judgments.json", value)
            output = self.root / "rejected.html"
            with self.assertRaises(ValueError):
                export_review(self.output / "corpus.json", self.output / "judgments.json", output, benchmark.validate_corpus)
            self.assertFalse(output.exists())

    def test_unjudged_and_no_positive_queries_do_not_invent_quality(self):
        judgments, _, _ = self.ranking_fixture()
        judgments["queries"][0]["complete"] = False
        benchmark.write_json(self.output / "judgments.json", judgments)
        result = self.score()
        self.assertIsNone(result["macro"])
        self.assertEqual(result["incomplete_queries"], ["q"])
        judgments["queries"][0].update(complete=True, relevance={})
        benchmark.write_json(self.output / "judgments.json", judgments)
        self.assertEqual(self.score()["no_positive_queries"], ["q"])

    def test_changed_query_corpus_or_duplicate_results_are_rejected(self):
        judgments, run, ids = self.ranking_fixture()
        for change in [lambda x: x.update(corpus_id="other"),
                       lambda x: x["queries"].update(q="kot"),
                       lambda x: x["rankings"].update(q=[ids[0]] * len(ids)),
                       lambda x: x["rankings"].update(q=ids[:2])]:
            copy = json.loads(json.dumps(run))
            change(copy)
            benchmark.write_json(self.output / "run.json", copy)
            with self.assertRaises(ValueError):
                self.score()
        benchmark.write_json(self.output / "run.json", run)
        item = benchmark.read_json(self.output / "corpus.json")["items"][0]
        (self.output / item["path"]).write_bytes(b"changed after sampling")
        with self.assertRaises(ValueError):
            self.score()

    def test_unknown_ids_invalid_grades_and_invalid_k_are_rejected(self):
        judgments, _, _ = self.ranking_fixture()
        for invalid in [{"unknown": 1}, {next(iter(judgments["queries"][0]["relevance"])): True}]:
            judgments["queries"][0]["relevance"] = invalid
            benchmark.write_json(self.output / "judgments.json", judgments)
            with self.assertRaises(ValueError):
                self.score()
        for k in (0, 11):
            with self.assertRaises(ValueError):
                self.score(k)


if __name__ == "__main__":
    unittest.main()
