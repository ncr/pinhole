"""Compare frozen-corpus encoder/ranking runs without inventing quality labels."""
import json
from pathlib import Path

import numpy as np

from .ane import hash_file


def distribution(values):
    values = np.asarray(values, dtype=np.float64)
    if not len(values) or not np.isfinite(values).all():
        raise ValueError("invalid measurement distribution")
    return {"count": len(values), "median": float(np.median(values)), "p95": float(np.percentile(values, 95)),
            "min": float(np.min(values)), "max": float(np.max(values)), "sum": float(np.sum(values))}


def read_run(directory):
    report = json.loads((directory / "run.json").read_text())
    ids = json.loads((directory / "image-ids.json").read_text())
    vectors = np.load(directory / "images.npy", allow_pickle=False).astype(np.float64)
    if (vectors.ndim != 2 or len(vectors) != len(ids) or len(set(ids)) != len(ids) or
            not np.isfinite(vectors).all() or np.any(np.abs(np.linalg.norm(vectors, axis=1) - 1) > 1e-5)):
        raise ValueError("invalid or non-normalized image embedding run")
    return report, ids, vectors


def overlap(a, b, k):
    if a["corpus_id"] != b["corpus_id"] or a["queries"] != b["queries"]:
        raise ValueError("rankings have different corpora or queries")
    if set(a["rankings"]) != set(b["rankings"]):
        raise ValueError("incomplete query rankings")
    rows = []
    for key in a["queries"]:
        first, second = a["rankings"][key][:k], b["rankings"][key][:k]
        if len(first) != k or len(second) != k or len(set(first)) != k or len(set(second)) != k:
            raise ValueError("rankings contain too few or duplicate top results")
        rows.append({"query": key, "shared": len(set(first) & set(second)), "same_order": first == second,
                     "same_set": set(first) == set(second)})
    return {"k": k, "mean_shared": float(np.mean([x["shared"] for x in rows])),
            "same_set_queries": sum(x["same_set"] for x in rows),
            "same_order_queries": sum(x["same_order"] for x in rows), "queries": rows}


def compare(root):
    result = {"schema": 1, "models": {}, "quality": "unjudged; ranking overlap is not retrieval quality"}
    for name in ["clip", "siglip2"]:
        cpu, ids, first = read_run(root / f"{name}-cpu")
        ane, second_ids, second = read_run(root / f"{name}-hybrid-ane")
        if (cpu["corpus_id"] != ane["corpus_id"] or ids != second_ids or first.shape != second.shape or
                cpu["source"]["checkpoint_sha256"] != ane["source"]["checkpoint_sha256"] or
                cpu["source"]["processor_sha256"] != ane["source"]["processor_sha256"]):
            raise ValueError("CPU and hybrid runs are not comparable")
        cosines = np.sum(first * second, axis=1) / (np.linalg.norm(first, axis=1) * np.linalg.norm(second, axis=1))
        relative = np.linalg.norm(first - second, axis=1) / np.linalg.norm(first, axis=1)
        a = json.loads((root / f"{name}-cpu-rankings.json").read_text())
        b = json.loads((root / f"{name}-hybrid-ane-rankings.json").read_text())
        result["models"][name] = {
            "photos": len(ids), "dimension": first.shape[1], "corpus_id": cpu["corpus_id"],
            "embedding_cosine": distribution(cosines), "embedding_relative_l2": distribution(relative),
            "cosine_below_0_99999": int(np.count_nonzero(cosines < .99999)),
            "top10_cpu_vs_hybrid": overlap(a, b, 10),
            "cpu_seconds_per_image": distribution([x["total_seconds"] for x in cpu["timings"]]),
            "hybrid_seconds_per_image": distribution([x["total_seconds"] for x in ane["timings"]]),
            "hybrid_ane": ane["ane"],
            "report_hashes": {"cpu": hash_file(root / f"{name}-cpu/run.json"),
                              "hybrid": hash_file(root / f"{name}-hybrid-ane/run.json")},
        }
    first = json.loads((root / "clip-hybrid-ane-rankings.json").read_text())
    second = json.loads((root / "siglip2-hybrid-ane-rankings.json").read_text())
    result["model_top10_overlap"] = overlap(first, second, 10)
    return result
