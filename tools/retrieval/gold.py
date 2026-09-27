"""Gold sets: map the UCL statements onto corpus recommendations, then build the query files.

  python gold.py map-ucl            -> rag/gold/ucl-triplets/mapping-draft.jsonl (hand-check, save as mapping.jsonl)
  python gold.py build              -> rag/results/queries/queries-<date>.jsonl (statements, cases, negatives)
  python gold.py build-notes        -> rag/results/queries/notes-<date>.jsonl (whole notes per labelled PriMock consultation, four sources)
  python gold.py build-transcripts  -> rag/results/queries/transcripts-<date>.jsonl (transcript, doctor turns, note plus doctor turns)

Gold inputs, each optional. A missing set is reported and skipped:
  rag/gold/st-georges-cases/cases.jsonl       {qid, text, expected_ids, expected_codes}
  rag/gold/ucl-triplets/triplets.csv + mapping.jsonl
  rag/gold/primock-statements/statements.jsonl {qid, text, expected_ids, expected_codes, mode}
  rag/gold/synthetic-statements/statements.jsonl {qid, text, expected_ids, expected_codes, mode, area, kind}
  rag/gold/negatives/negatives.jsonl           {qid, text, kind}

The build refuses a synthetic statement sharing a 4-gram with a recommendation it expects and logs lexical overlap per set.
"""

import argparse
import csv
import difflib
import json
import re
import time

from common import GOLD, RESULTS, latest_chunks, log, read_jsonl, write_jsonl

UCL = GOLD / "ucl-triplets"
NON_NICE = {"anaphylaxis"}  # Resuscitation Council UK text, absent from the corpus
GUIDELINE_CODES = [
    ("urinary", "ng109"), ("type1", "ng17"), ("type 1", "ng17"), ("type 2", "ng28"), ("thyroid", "ng145"),
    ("ovarian", "cg122"), ("hypertension", "ng136"), ("gastro", "cg184"), ("heart failure", "ng106"),
    ("kidney", "ng203"), ("anaphylaxis", "cg134"),
]


def code_for(name: str) -> str:
    name = name.lower()
    for key, code in GUIDELINE_CODES:
        if key in name:
            return code
    return ""


def norm_statement(s: str) -> str:
    s = re.sub(r"^\d+\.\s*", "", s.strip())
    s = re.sub(r"\s*[\(\[]\s*[\d\.]+(?:\s[^\)\]]*)?[\)\]]\s*$", "", s)
    return re.sub(r"\s+", " ", s).strip().lower()


def read_ucl():
    with open(UCL / "triplets.csv", encoding="utf-8-sig", newline="") as f:
        rows = list(csv.reader(f))[1:]
    out = []
    for r in rows:
        r = (r + [""] * 8)[:8]
        name = r[2].replace("_recommendation", "").strip()
        out.append({"guideline": name, "code": code_for(name), "statement": r[3].strip(),
                    "scenario": r[5].strip(), "question": r[6].strip()})
    return out


def map_ucl():
    chunks = read_jsonl(latest_chunks())
    by_code = {}
    for c in chunks:
        by_code.setdefault(c["code"], []).append(c)
    seen, drafts = set(), []
    for row in read_ucl():
        if row["statement"] in seen:
            continue
        seen.add(row["statement"])
        if any(k in row["guideline"].lower() for k in NON_NICE) or not by_code.get(row["code"]):
            drafts.append({**row, "best_id": "", "ratio": 0, "kind": "not-nice", "decision": "drop"})
            continue
        ns = norm_statement(row["statement"])
        best, ratio = None, 0.0
        for c in by_code[row["code"]]:
            r = difflib.SequenceMatcher(None, ns, c["text"].lower()).ratio()
            if r > ratio:
                best, ratio = c, r
        contained = len(ns) > 30 and ns[:60] in best["text"].lower()
        kind = "verbatim" if ratio >= 0.9 or contained else "near" if ratio >= 0.6 else "check"
        drafts.append({**row, "best_id": best["id"], "best_number": best["number"], "best_text": best["text"][:200],
                       "ratio": round(ratio, 3), "kind": kind,
                       "decision": "accept" if kind != "check" else "check"})
    out = UCL / "mapping-draft.jsonl"
    n = write_jsonl(out, drafts)
    kinds = {}
    for d in drafts:
        kinds[d["kind"]] = kinds.get(d["kind"], 0) + 1
    log(f"{n} statements -> {out}; {kinds}")


def split_sentences(text: str) -> list[str]:
    # Harness-only splitter. The engine uses ICU with clinical suppressions
    parts = re.split(r"(?<=[.!?])\s+(?=[A-Z0-9\"'(])|\n+", text)
    return [p.strip() for p in parts if len(p.split()) >= 3]


def tokens(text: str) -> list[str]:
    return re.findall(r"[a-z0-9]+", text.lower())


def ngrams(text: str, n: int = 4) -> set[tuple]:
    t = tokens(text)
    return {tuple(t[i:i + n]) for i in range(len(t) - n + 1)}


def overlap_report(queries: list[dict], chunks: dict) -> list[str]:
    """Logs median and max token Jaccard per set. Returns the synthetic rows that share a 4-gram
    with a recommendation they expect."""
    per_set, offending = {}, []
    for q in queries:
        if not q["expected_ids"]:
            continue
        qt = set(tokens(q["text"]))
        best = 0.0
        for i in q["expected_ids"]:
            ct = set(tokens(chunks[i]["text"]))
            best = max(best, len(qt & ct) / len(qt | ct))
            if q["set"] == "synthetic":
                shared = ngrams(q["text"]) & ngrams(chunks[i]["text"])
                if shared:
                    offending.append(f"{q['qid']} shares with {i}: " + "; ".join(" ".join(g) for g in sorted(shared)))
        per_set.setdefault(q["set"], []).append(best)
    for name, vals in sorted(per_set.items()):
        vals.sort()
        log(f"overlap {name}: n={len(vals)} median jaccard {vals[len(vals) // 2]:.2f} max {vals[-1]:.2f}")
    return offending


def check_expected(queries: list[dict], chunks: dict) -> None:
    missing = [(q["qid"], i) for q in queries for i in q["expected_ids"] if i not in chunks]
    if missing:
        raise SystemExit(f"expected ids not in the corpus: {missing}")


def primock_by_consult() -> dict:
    by_consult = {}
    for r in read_jsonl(GOLD / "primock-statements" / "statements.jsonl"):
        by_consult.setdefault(r["consult"], []).append(r)
    return by_consult


def build():
    queries = []

    cases = GOLD / "st-georges-cases" / "cases.jsonl"
    if cases.exists():
        for r in read_jsonl(cases):
            # The PMR case cites no NICE recommendation: against this corpus it must retrieve nothing
            queries.append({"qid": r["qid"], "set": "st-georges", "text": r["text"], "mode": "note",
                            "expected_ids": r["expected_ids"], "expected_codes": r.get("expected_codes", []),
                            "negative": not r["expected_ids"]})
    else:
        log("st-georges cases missing")

    mapping = UCL / "mapping.jsonl"
    if mapping.exists() and (UCL / "triplets.csv").exists():
        by_statement = {m["statement"]: m for m in read_jsonl(mapping)}
        kept = dropped = 0
        for i, row in enumerate(read_ucl()):
            m = by_statement.get(row["statement"])
            if not m or m["decision"] == "drop" or not m.get("expected_ids"):
                dropped += 1
                continue
            kept += 1
            queries.append({"qid": f"ucl-{i:03d}", "set": "ucl", "text": row["scenario"], "mode": "sentence",
                            "question": row["question"], "expected_ids": m["expected_ids"],
                            "expected_codes": [row["code"]], "negative": False})
        log(f"ucl: {kept} kept, {dropped} dropped")
    else:
        log("ucl mapping missing (run map-ucl, hand-check, save as mapping.jsonl)")

    primock = GOLD / "primock-statements" / "statements.jsonl"
    if primock.exists():
        for r in read_jsonl(primock):
            queries.append({"qid": r["qid"], "set": "primock", "text": r["text"], "mode": r.get("mode", "sentence"),
                            "expected_ids": r["expected_ids"], "expected_codes": r.get("expected_codes", []),
                            "negative": not r["expected_ids"]})
    else:
        log("primock statements missing")

    synthetic = GOLD / "synthetic-statements" / "statements.jsonl"
    if synthetic.exists():
        for r in read_jsonl(synthetic):
            queries.append({"qid": r["qid"], "set": "synthetic", "text": r["text"], "mode": r.get("mode", "sentence"),
                            "area": r.get("area", ""), "kind": r.get("kind", ""),
                            "expected_ids": r["expected_ids"], "expected_codes": r.get("expected_codes", []),
                            "negative": not r["expected_ids"]})
    else:
        log("synthetic statements missing")

    negatives = GOLD / "negatives" / "negatives.jsonl"
    if negatives.exists():
        for r in read_jsonl(negatives):
            queries.append({"qid": r["qid"], "set": "negatives", "text": r["text"], "mode": r.get("mode", "note"),
                            "kind": r.get("kind", ""), "expected_ids": [], "expected_codes": [], "negative": True})
    else:
        log("negatives missing")

    chunks = {c["id"]: c for c in read_jsonl(latest_chunks())}
    check_expected(queries, chunks)
    offending = overlap_report(queries, chunks)
    if offending:
        raise SystemExit("synthetic statements share wording with their recommendation:\n  " + "\n  ".join(offending))

    out = RESULTS / "queries" / f"queries-{time.strftime('%Y%m%d')}.jsonl"
    n = write_jsonl(out, queries)
    sets = {}
    for q in queries:
        sets[q["set"]] = sets.get(q["set"], 0) + 1
    log(f"{n} queries -> {out}; {sets}")


VAULT = GOLD.parents[2] / "intelliscribe"  # the docs and data vault beside the repo
NOTE_SOURCES = {
    # Whole notes for the PriMock consultations that carry statement labels, pooled per consultation
    "notes-human": (VAULT / "data" / "primock57" / "notes", "json"),
    "notes-4b": (VAULT / "bench" / "summarisation" / "notes" / "tier-constrained-standard", "md"),
    "notes-9b": (VAULT / "bench" / "summarisation" / "notes" / "tier-default-standard", "md"),
    "notes-35b": (VAULT / "bench" / "summarisation" / "notes" / "tier-accuracy-standard", "md"),
}


def build_notes():
    by_consult = primock_by_consult()
    chunks = {c["id"]: c for c in read_jsonl(latest_chunks())}
    queries = []
    for set_name, (folder, ext) in NOTE_SOURCES.items():
        if not folder.exists():
            log(f"{set_name}: {folder} missing, skipped")
            continue
        for consult, rows in sorted(by_consult.items()):
            path = folder / f"{consult}.{ext}"
            if not path.exists():
                raise SystemExit(f"{set_name}: no note for {consult} at {path}")
            text = json.loads(path.read_text(encoding="utf-8"))["note"] if ext == "json" else path.read_text(encoding="utf-8")
            if len(text.split()) < 3:
                log(f"{set_name}: {consult} note is empty, excluded")
                continue
            ids = sorted({i for r in rows for i in r["expected_ids"]})
            codes = sorted({c for r in rows for c in r.get("expected_codes", [])})
            queries.append({"qid": f"{set_name}-{consult}", "set": set_name, "consult": consult, "text": text.strip(),
                            "mode": "note", "expected_ids": ids, "expected_codes": codes, "negative": not ids,
                            "statements": [r["qid"] for r in rows]})
    check_expected(queries, chunks)
    out = RESULTS / "queries" / f"notes-{time.strftime('%Y%m%d')}.jsonl"
    n = write_jsonl(out, queries)
    sets = {}
    for q in queries:
        sets.setdefault(q["set"], [0, 0])[1 if q["negative"] else 0] += 1
    log(f"{n} note queries -> {out}; per set [positives, negatives]: {sets}")
    words = [len(q["text"].split()) for q in queries]
    log(f"note length in words: min {min(words)}, median {sorted(words)[len(words) // 2]}, max {max(words)}")


TRANSCRIPTS = VAULT / "data" / "primock57" / "transcripts"


def read_textgrid(path) -> list[tuple[float, str]]:
    """(start, text) per spoken interval, with annotation tags such as <UNIN/> removed."""
    out = []
    for m in re.finditer(r'xmin = ([\d.]+)\s+xmax = [\d.]+\s+text = "((?:[^"]|"")*)"', path.read_text(encoding="utf-8", errors="replace")):
        t = re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", m.group(2).replace('""', '"'))).strip()
        if t:
            out.append((float(m.group(1)), t))
    return out


def build_transcripts():
    """Transcript-derived queries for the labelled PriMock consultations: the full transcript, the doctor's
    turns only, and the clinician's note with the doctor's sentences as extra sub-queries."""
    by_consult = primock_by_consult()
    chunks = {c["id"]: c for c in read_jsonl(latest_chunks())}
    notes_dir = NOTE_SOURCES["notes-human"][0]
    queries = []
    for consult, rows in sorted(by_consult.items()):
        turns = {}
        for who in ("doctor", "patient"):
            path = TRANSCRIPTS / f"{consult}_{who}.TextGrid"
            if not path.exists():
                raise SystemExit(f"no transcript at {path}")
            turns[who] = read_textgrid(path)
        full = [t for _, t in sorted(turns["doctor"] + turns["patient"])]
        doctor = [t for _, t in sorted(turns["doctor"])]
        note = json.loads((notes_dir / f"{consult}.json").read_text(encoding="utf-8"))["note"].strip()
        ids = sorted({i for r in rows for i in r["expected_ids"]})
        base = {"consult": consult, "mode": "note", "expected_ids": ids,
                "expected_codes": sorted({c for r in rows for c in r.get("expected_codes", [])}), "negative": not ids}
        queries.append({"qid": f"transcript-full-{consult}", "set": "transcript-full", "text": "\n".join(full), **base})
        queries.append({"qid": f"transcript-doctor-{consult}", "set": "transcript-doctor", "text": "\n".join(doctor), **base})
        queries.append({"qid": f"note-plus-doctor-{consult}", "set": "note-plus-doctor", "text": note,
                        "extra_sentences": [s for t in doctor for s in split_sentences(t)], **base})
    check_expected(queries, chunks)
    out = RESULTS / "queries" / f"transcripts-{time.strftime('%Y%m%d')}.jsonl"
    n = write_jsonl(out, queries)
    for s in ("transcript-full", "transcript-doctor", "note-plus-doctor"):
        qs = [q for q in queries if q["set"] == s]
        words = sorted(len(q["text"].split()) + sum(len(x.split()) for x in q.get("extra_sentences", [])) for q in qs)
        sents = sorted(len(split_sentences(q["text"])) + len(q.get("extra_sentences", [])) for q in qs)
        log(f"{s}: {len(qs)} queries, words median {words[len(words) // 2]} max {words[-1]}, sub-queries median {sents[len(sents) // 2]} max {sents[-1]}")
    log(f"{n} transcript queries -> {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["map-ucl", "build", "build-notes", "build-transcripts"])
    args = ap.parse_args()
    {"map-ucl": map_ucl, "build": build, "build-notes": build_notes, "build-transcripts": build_transcripts}[args.cmd]()


if __name__ == "__main__":
    main()
