"""Export a self-contained, model-blind relevance review. No HTTP server."""
import base64
import hashlib
import json
import os
from pathlib import Path


def validate_judgments(value, corpus_id, ids):
    if value.get("schema") != 1 or value.get("corpus_id") != corpus_id:
        raise ValueError("judgments do not match this corpus")
    queries = value.get("queries")
    if not isinstance(queries, list) or not queries:
        raise ValueError("no queries to review")
    seen = set()
    for query in queries:
        key, text = query.get("id"), query.get("text")
        if not isinstance(key, str) or not key or key in seen or not isinstance(text, str) or not text.strip():
            raise ValueError("invalid or duplicate query")
        seen.add(key)
        relevance = query.get("relevance")
        if not isinstance(relevance, dict) or not set(relevance).issubset(ids):
            raise ValueError("unknown relevance ids")
        if any(type(grade) is not int or grade not in (0, 1, 2) for grade in relevance.values()):
            raise ValueError("relevance grades must be 0, 1 or 2")
        if type(query.get("complete")) is not bool:
            raise ValueError("query completeness must be boolean")


def export_review(corpus_path, judgments_path, output, validate_corpus):
    corpus, ids = validate_corpus(corpus_path)
    judgments = json.loads(Path(judgments_path).read_text(encoding="utf-8"))
    validate_judgments(judgments, corpus["corpus_id"], ids)
    images = {}
    root = Path(corpus_path).resolve().parent
    for item in corpus["items"]:
        data = (root / item["path"]).read_bytes()
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError("image changed during export")
        # The corpus consists of cached JPEG thumbnails; never include paths,
        # capture metadata, model names, scores or a model's ranking order.
        images[item["id"]] = "data:image/jpeg;base64," + base64.b64encode(data).decode("ascii")
    orders = {query["id"]: sorted(ids, key=lambda key: hashlib.sha256(
        (corpus["corpus_id"] + "\0" + query["id"] + "\0" + key).encode()).hexdigest())
        for query in judgments["queries"]}
    payload = json.dumps({"judgments": judgments, "images": images, "orders": orders},
                         ensure_ascii=False, allow_nan=False).replace("<", "\\u003c")
    assets = Path(__file__).parent
    script = (assets / "review.js").read_text(encoding="utf-8")
    script_hash = base64.b64encode(hashlib.sha256(script.encode()).digest()).decode()
    html = (assets / "review.html").read_text(encoding="utf-8")
    html = html.replace("__SCRIPT_HASH__", script_hash).replace("__SCRIPT__", script).replace("__PAYLOAD__", payload)
    # Refuse overwriting labels, corpus files or an earlier private review.
    fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(html)
    return {"photos": len(ids), "queries": len(judgments["queries"]),
            "output": str(output), "order": "seeded per query, independent of model rankings"}
