"""Opt-in real Chromium check of offline grading, import and export."""
import base64
import hashlib
import html
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


@unittest.skipUnless(os.environ.get("PINHOLE_TEST_CHROMIUM"), "set PINHOLE_TEST_CHROMIUM to a local Chromium executable")
class ReviewBrowserTests(unittest.TestCase):
    def test_grading_completion_keyboard_and_round_trip(self):
        from photo_search.review import export_review
        loader = importlib.machinery.SourceFileLoader("review_benchmark", str(Path(__file__).resolve().parents[1] / "photo-search-benchmark"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        benchmark = importlib.util.module_from_spec(spec); loader.exec_module(benchmark)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # Synthetic bytes suffice here: image decoding belongs to the
            # image suite, and no private thumbnails enter browser test logs.
            image = b"synthetic local thumbnail"
            (root / "image.jpg").write_bytes(image)
            corpus = {"schema": 1, "items": [{"id": str(i), "path": "image.jpg", "sha256": benchmark.digest(image)} for i in range(26)]}
            corpus["corpus_id"] = benchmark.identity(corpus)
            benchmark.write_json(root / "corpus.json", corpus)
            judgments = {"schema": 1, "corpus_id": corpus["corpus_id"], "queries": [
                {"id": "q", "text": "pies", "complete": False, "relevance": {}},
                {"id": "q2", "text": "kot", "complete": False, "relevance": {}}]}
            benchmark.write_json(root / "judgments.json", judgments)
            output = root / "review.html"
            export_review(root / "corpus.json", root / "judgments.json", output, benchmark.validate_corpus)
            script = r'''(async () => {
                const $ = id => document.getElementById(id);
                const assert = (value, message) => { if (!value) throw Error(message); };
                const first = () => document.querySelector('.photo');
                const key = name => document.activeElement.dispatchEvent(new KeyboardEvent('keydown', {key:name,bubbles:true}));
                const read = () => JSON.parse(document.getElementById('payload').textContent).judgments;
                let exported;
                URL.createObjectURL = blob => { exported = blob; return 'blob:test'; };
                HTMLAnchorElement.prototype.click = () => {};
                const save = async () => { $('download').click(); return JSON.parse(await exported.text()); };
                const load = async value => {
                    Object.defineProperty($('import'), 'files', {configurable:true,value:[new File([JSON.stringify(value)], 'judgments.json')]});
                    await $('import').onchange({target:$('import')});
                };
                try {
                    assert($('complete').disabled, 'initial completion must be disabled');
                    assert(document.querySelectorAll('.photo').length === 24, 'paged thumbnails');
                    first().focus(); key('2'); key('ArrowRight'); key('0');
                    let value = await save();
                    assert(Object.values(value.queries[0].relevance).includes(2), 'positive grade saved');
                    assert(Object.values(value.queries[0].relevance).includes(0), 'explicit zero saved');
                    assert(!value.queries[0].complete && Object.keys(value.queries[0].relevance).length === 2, 'unreviewed items stay absent');
                    // Return to the first card, grade every photo and cross the page boundary.
                    first().focus();
                    for (let i = 0; i < 26; i++) { key('0'); if (i < 25) key('ArrowRight'); }
                    assert(!$('complete').disabled && $('page').textContent.includes('2 z 2'), 'completion requires full review across pages');
                    $('complete').click(); value = await save();
                    assert(value.queries[0].complete && Object.keys(value.queries[0].relevance).length === 26, 'full review exported');
                    key('Delete'); assert($('complete').disabled, 'clearing a grade invalidates completion');
                    await save(); await load(value);
                    assert($('progress').textContent.includes('Zapytanie zatwierdzone.'), 'import restores completion');
                    const foreign = read(); foreign.corpus_id = 'other'; await load(foreign);
                    assert($('status').className === 'error', 'foreign import rejected');
                    value = await save(); assert(value.queries[0].complete, 'rejected import preserves previous state');
                    $('nextQuery').click(); assert($('progress').textContent.includes('Oceniono 0 z 26'), 'query state isolated');
                    document.body.dataset.testResult = 'PASS';
                } catch (error) { document.body.dataset.testResult = 'FAIL: ' + error.message; }
            })();'''
            script_hash = base64.b64encode(hashlib.sha256(script.encode()).digest()).decode()
            text = re.sub(r"script-src [^;]+", lambda match: match.group(0) + f" 'sha256-{script_hash}'", output.read_text())
            output.write_text(text.replace("</html>", f"<script>{script}</script></html>"))
            result = subprocess.run([os.environ["PINHOLE_TEST_CHROMIUM"], "--headless", "--disable-gpu", "--disable-background-networking", "--no-first-run",
                                     f"--user-data-dir={root / 'profile'}", "--dump-dom", "--virtual-time-budget=3000", output.as_uri()],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr[-2000:])
            match = re.search(r'data-test-result="([^"]+)"', result.stdout)
            self.assertIsNotNone(match, result.stderr[-2000:])
            self.assertEqual(html.unescape(match.group(1)), "PASS")
