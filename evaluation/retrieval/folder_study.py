"""Offline study of guidance retrieval over a clinician's own folder.

  python folder_study.py units          engine chunker over every PDF in the folder -> units.jsonl
  python folder_study.py embed          shipped embedder over the units (text, headed, titled) and the queries
  python folder_study.py embed queries  the queries alone, for a working directory that already holds the passages
  python folder_study.py run            every variant over every query -> runs.jsonl
  python folder_study.py pool [FILE]    every card any variant showed for an in-scope note -> pool.txt, to be judged
  python folder_study.py score [FILE]   variants against rag/gold/folder-notes/judgments.jsonl, and the abstention signal
  python folder_study.py check          reports gold that echoes a unit it expects or names one that is gone
  python folder_study.py models         embeds units and queries with each shortlisted embedder -> runs-models.jsonl
  python folder_study.py compare        the embedders against each other: ranking with the floor off, and the abstention signal
  python folder_study.py remap OLD      gold moved from an old units.jsonl's numbering to the current one, by text
  python folder_study.py parity PATH    the baseline variant against a folder_eval.py note-mode run

FILE names a run file inside the working directory and defaults to runs.jsonl. Working data goes
to build/retrieval/study, or to build/retrieval/$FOLDER_STUDY for the same study over a changed
folder or chunker. The splitter, the exclusion filter, the vote and the floor are ports of
engine/src/core/guidance/guidance_query.hpp and guidance_rank.hpp, and parity proves the port before any
variant is trusted. Run with the harness venv (openvino_genai).
"""

import json
import math
import os
import random
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import read_json, read_jsonl, write_jsonl  # noqa: E402
from common.stats import mean_interval  # noqa: E402
from harness import CANDIDATES, GOLD, REPO, load_shortlist  # noqa: E402
from embed import make_pipeline

# FOLDER_STUDY names a second working directory, for the same study over a changed folder
STUDY = REPO / "build" / "retrieval" / os.environ.get("FOLDER_STUDY", "study")
FOLDER = config.path("guidelines_folder")
UNITS_EXE = config.path("engine").parent / "clinicavt_units.exe"
MODEL = config.path("app_models") / "gte-large-int8"

# The first-stage finalists of the selection round, as shortlist.json configures them
EMBEDDERS = ["gte-large", "bge-large", "arctic-l-v2", "bge-base"]

FLOOR = 0.85
UNION = 50
RRF_K = 60.0
LIMIT = 3
DUPLICATE = 0.5
BOOTSTRAP = 5000
INSIDE = ("cases", "folder-notes")
OUTSIDE = ("gp-notes", "near-misses", "negatives")

ABBREVIATIONS = {"e.g", "i.e", "dr", "mr", "mrs", "ms", "prof", "vs", "approx", "etc", "no", "hx", "pt", "rx",
                 "mg", "mcg", "ml", "kg", "cm", "mm", "bd", "tds", "od", "prn", "st", "ca", "cf", "wk", "wks",
                 "yr", "yrs", "mth"}
OPENINGS = ("no ", "nil ", "not ", "denies", "denied", "never ", "without ", "negative for", "no history",
            "family history", "fh:", "fh ", "fhx", "if ", "unless ", "should she", "should he", "in case")
PHRASES = (" mother had", " father had", " mother has", " father has", " sister had", " brother had",
           "grandmother", "grandfather", "family history of", "no evidence of", "were to develop")


# ---- ports of the engine's query side

def split_sentences(note: str) -> list[str]:
    """A newline always ends a sentence. A full stop, exclamation or question mark ends one when
    whitespace and a capital, digit, quote or bracket follow and the word before is not an
    abbreviation. Fragments under three words are dropped."""
    out, start, i = [], 0, 0

    def flush(a, b):
        piece = note[a:b].strip()
        if len(piece.split()) >= 3:
            out.append(piece)

    while i < len(note):
        c = note[i]
        if c == "\n":
            flush(start, i)
            start = i + 1
        elif c in ".!?":
            j = i + 1
            while j < len(note) and note[j] in " \t\r":
                j += 1
            if j > i + 1 and j < len(note) and (note[j].isupper() or note[j].isdigit() or note[j] in "\"'("):
                token = note[:i].split()[-1].lower() if note[:i].split() else ""
                token = token.rstrip(".")
                abbreviation = c == "." and (token in ABBREVIATIONS or (len(token) == 1 and token.isalpha()))
                if not abbreviation:
                    flush(start, i + 1)
                    start = j
        i += 1
    flush(start, len(note))
    return out


def is_excluded(sentence: str) -> bool:
    """A sentence that must not retrieve: a negated finding, family history or a hypothetical.
    Guidance for that condition would be for a patient who does not have it."""
    s = sentence.strip().lower()
    return s.startswith(OPENINGS) or any(p in s for p in PHRASES)


def sub_queries(note: str) -> list[str]:
    out = [s for s in split_sentences(note) if not is_excluded(s)]
    whole = note.strip()
    if len(whole.split()) >= 3 and whole not in out:
        out.append(whole)
    return out


def content_words(text: str) -> set[str]:
    return {w for w in re.findall(r"[a-z0-9]+", text.lower()) if len(w) > 3}


def near_duplicate(a: str, b: str) -> bool:
    """Two passages saying the same thing, as a quality standard restates its guideline: half the
    shorter one's content words appear in the other."""
    wa, wb = content_words(a), content_words(b)
    smaller = min(len(wa), len(wb))
    return smaller >= 5 and len(wa & wb) / smaller >= DUPLICATE


# ---- data

def load_units() -> list[dict]:
    return read_jsonl(STUDY / "units.jsonl")


def load_runs(name: str = "runs.jsonl") -> list[dict]:
    return read_jsonl(STUDY / name)


def load_judgments() -> dict:
    path = GOLD / "folder-notes" / "judgments.jsonl"
    return {(j["qid"], j["unit"]): j["label"] for j in read_jsonl(path)} if path.exists() else {}


def load_queries() -> list[dict]:
    """In scope: the clinician's four cases. Out of scope: the app's own notes of general-practice
    consultations, which a rheumatology folder does not cover, and the harness negatives."""
    queries = []
    for row in read_jsonl(GOLD / "st-georges-cases" / "cases.jsonl"):
        queries.append({"qid": row["qid"], "set": "cases", "text": row["text"]})
    for row in read_jsonl(GOLD / "folder-notes" / "notes.jsonl"):
        expected = [f"{e['doc']}#{o}" for e in row["expected"] for o in e["ords"]]
        queries.append({"qid": row["qid"], "set": "folder-notes", "text": row["text"], "expected": expected})
    for path in sorted((REPO / "demo" / "reflections").glob("*.json")):
        queries.append({"qid": "gp-" + path.stem[:2], "set": "gp-notes", "text": read_json(path)["note"]})
    # Musculoskeletal problems close to the folder's topics that it does not cover
    for row in read_jsonl(GOLD / "folder-notes" / "near-misses.jsonl"):
        queries.append({"qid": row["qid"], "set": "near-misses", "text": row["text"]})
    for row in read_jsonl(GOLD / "negatives" / "negatives.jsonl"):
        queries.append({"qid": row.get("qid", f"neg-{len(queries)}"), "set": "negatives", "text": row["text"]})
    return queries


def group_by_query(index: list[dict]) -> dict:
    """qid -> [(row number in queries.npy, index entry)] for its sub-queries."""
    by_query = {}
    for i, entry in enumerate(index):
        by_query.setdefault(entry["qid"], []).append((i, entry))
    return by_query


# ---- steps

def units():
    rows = []
    for pdf in sorted(FOLDER.glob("*.pdf")):
        done = subprocess.run([str(UNITS_EXE), str(pdf)], capture_output=True)
        if done.returncode != 0:
            print(f"skipped {pdf.name}: {done.stderr.decode(errors='replace').strip()[:80]}")
            continue
        lines = done.stdout.decode("utf-8").splitlines()
        for ord_, line in enumerate(lines):
            rows.append({"id": f"{pdf.stem}#{ord_}", "doc": pdf.stem, "ord": ord_, **json.loads(line)})
        print(f"{pdf.stem}: {len(lines)} units")
    print(write_jsonl(STUDY / "units.jsonl", rows), "units")


def pipeline():
    # The model the app ships, read as the app reads it: mean pooling, no instruction
    return make_pipeline({"pooling": "mean", "max_length": 512}, MODEL)


def embed(queries_only: bool = False):
    pipe = pipeline()
    rows = load_units()

    def documents(texts, name):
        out = []
        for i in range(0, len(texts), 32):
            out.extend(pipe.embed_documents(texts[i:i + 32]))
            if i % 640 == 0:
                print(name, i, "/", len(texts), flush=True)
        np.save(STUDY / f"{name}.npy", np.asarray(out, dtype=np.float32))

    # `embed queries` leaves the passages alone, for a working directory that holds only docs-text
    if not queries_only and not (STUDY / "docs-titled.npy").exists():
        headed = [(r["section"] + ". " if r["section"] else "") + r["text"] for r in rows]
        documents([r["text"] for r in rows], "docs-text")
        documents(headed, "docs-headed")
        documents([r["doc"] + ". " + h for r, h in zip(rows, headed)], "docs-titled")

    texts, index = [], []
    for q in load_queries():
        for sub in sub_queries(q["text"]):
            index.append({"qid": q["qid"], "sub": sub, "whole": sub == q["text"].strip()})
            texts.append(sub)
    # The engine embeds a query and a passage the same way: no instruction, mean pooling
    documents(texts, "queries")
    with open(STUDY / "queries.json", "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=1)
    print(len(texts), "sub-queries")


def rank(variant: dict, lists: list[dict], unit_rows: list[dict], background=None) -> dict:
    """lists: one per sub-query, {'sub', 'whole', 'cos': cosines by unit index}."""
    floor = variant.get("floor", FLOOR)
    whole = next((e for e in lists if e["whole"]), lists[-1])
    best = {"note_best": round(float(whole["cos"].max()), 4),
            "any_best": round(max(float(e["cos"].max()) for e in lists), 4)}
    # The note as a whole decides whether the folder covers it at all
    if variant.get("note_gate") and best["note_best"] < variant.get("note_floor", floor):
        return {"considered": 0, "shown": [], **best}
    neighbourhood = None
    if variant.get("within_note"):
        neighbourhood = {int(u) for u in np.argsort(-whole["cos"])[:UNION]}

    tally = {}
    for entry in lists:
        if variant.get("whole_only") and not entry["whole"]:
            continue
        cos = entry["cos"]
        adjusted = cos - background if background is not None and variant.get("csls") else cos
        order = np.argsort(-adjusted)[:variant.get("union", UNION)]
        weight = variant.get("note_weight", 1) if entry["whole"] else 1
        for position, unit in enumerate(int(u) for u in order):
            if neighbourhood is not None and unit not in neighbourhood:
                continue  # sentences only reorder what the whole note already found
            if variant.get("gate") and cos[unit] < floor and not (entry["whole"] and variant.get("note_votes")):
                continue  # a sentence votes only for what it actually resembles
            tallied = tally.setdefault(unit, {"score": 0.0, "cos": -1.0, "best": 10 ** 9, "trigger": "", "votes": 0})
            if variant.get("order") == "vote":
                tallied["score"] += weight / (RRF_K + position + 1.0)
            tallied["votes"] += 1
            tallied["cos"] = max(tallied["cos"], float(cos[unit]))
            if position < tallied["best"]:
                tallied["best"], tallied["trigger"] = position, "" if entry["whole"] else entry["sub"]
    if variant.get("order") != "vote":
        for tallied in tally.values():
            tallied["score"] = tallied["cos"]

    ranked = sorted(tally.items(), key=lambda kv: (-kv[1]["score"], -kv[1]["cos"]))[:UNION]
    shown = []
    for unit, tallied in ranked:
        if len(shown) >= LIMIT:
            break
        # The floor is on the best cosine a card reached, whatever the vote made of it
        if tallied["cos"] < floor:
            continue
        if any(near_duplicate(unit_rows[s["unit"]]["text"], unit_rows[unit]["text"]) for s in shown):
            continue
        row = unit_rows[unit]
        shown.append({"unit": unit, "id": row["id"], "doc": row["doc"], "page": row["page"] + 1,
                      "section": row["section"], "cos": round(tallied["cos"], 4),
                      "score": round(tallied["score"], 5), "votes": tallied["votes"],
                      "trigger": tallied["trigger"]})
    return {"considered": len(ranked), "shown": shown, **best}


VARIANTS = [
    {"name": "V0 engine: vote over sentences and note", "order": "vote", "docs": "docs-text"},
    {"name": "V1 whole note only", "order": "cosine", "whole_only": True, "docs": "docs-text"},
    {"name": "V2 best similarity over sentences and note", "order": "cosine", "docs": "docs-text"},
    {"name": "V3 vote, gated at the floor", "order": "vote", "gate": True, "docs": "docs-text"},
    {"name": "V4 vote, union of 10", "order": "vote", "union": 10, "docs": "docs-text"},
    {"name": "V5 vote, heading embedded with passage", "order": "vote", "docs": "docs-headed"},
    {"name": "V6 vote, title and heading embedded", "order": "vote", "docs": "docs-titled"},
    {"name": "V7 vote, hub-corrected similarity", "order": "vote", "csls": True, "docs": "docs-text"},
    {"name": "V8 gated vote, heading embedded", "order": "vote", "gate": True, "docs": "docs-headed"},
    {"name": "V9 best similarity, heading embedded", "order": "cosine", "docs": "docs-headed"},
    {"name": "V10 vote, but silent unless the whole note clears the floor", "order": "vote", "note_gate": True, "docs": "docs-text"},
    {"name": "V11 vote within the whole note's top 50", "order": "vote", "within_note": True, "note_gate": True, "docs": "docs-text"},
    {"name": "V12 best similarity within the whole note's top 50", "order": "cosine", "within_note": True, "note_gate": True, "docs": "docs-text"},
    {"name": "V13 gated vote, silent unless the whole note clears the floor", "order": "vote", "gate": True, "note_gate": True, "docs": "docs-text"},
    {"name": "V14 as V13 with the note floor at 0.84", "order": "vote", "gate": True, "note_gate": True, "note_floor": 0.84, "docs": "docs-text"},
    {"name": "V15 as V10 with the note floor at 0.84", "order": "vote", "note_gate": True, "note_floor": 0.84, "docs": "docs-text"},
    {"name": "V16 as V13, the whole note always votes", "order": "vote", "gate": True, "note_votes": True, "note_gate": True, "docs": "docs-text"},
    {"name": "V17 as V16 with the note floor at 0.84", "order": "vote", "gate": True, "note_votes": True, "note_gate": True, "note_floor": 0.84, "docs": "docs-text"},
    {"name": "V18 as V16, note weight 3", "order": "vote", "gate": True, "note_votes": True, "note_gate": True, "note_weight": 3, "docs": "docs-text"},
    {"name": "V19 as V17, note weight 3", "order": "vote", "gate": True, "note_votes": True, "note_gate": True, "note_floor": 0.84, "note_weight": 3, "docs": "docs-text"},
]


def run():
    unit_rows = load_units()
    queries = load_queries()
    index = read_json(STUDY / "queries.json")
    qvec = np.load(STUDY / "queries.npy")
    variants = [v for v in VARIANTS if (STUDY / f"{v['docs']}.npy").exists()]
    docs = {name: np.load(STUDY / f"{name}.npy") for name in {v["docs"] for v in variants}}
    by_query = group_by_query(index)

    # Hub correction: a unit's mean similarity to every sub-query of every note
    background = {name: (qvec @ matrix.T).mean(axis=0) for name, matrix in docs.items()}

    rows = []
    occurrence = {name: np.zeros(len(unit_rows), dtype=int) for name in docs}
    for q in queries:
        for v in variants:
            matrix = docs[v["docs"]]
            # How often a unit reaches a sub-query's top ten, counted once per document field
            counts_hubs = v is VARIANTS[0] or (v["docs"] != "docs-text" and v["name"].startswith("V5"))
            lists = []
            for i, entry in by_query[q["qid"]]:
                cos = matrix @ qvec[i]
                lists.append({"sub": entry["sub"], "whole": entry["whole"], "cos": cos})
                if counts_hubs:
                    occurrence[v["docs"]][np.argsort(-cos)[:10]] += 1
            result = rank(v, lists, unit_rows, background[v["docs"]])
            rows.append({"qid": q["qid"], "set": q["set"], "variant": v["name"],
                         "expected": q.get("expected", []), **result})
    written = write_jsonl(STUDY / "runs.jsonl", rows)
    np.save(STUDY / "occurrence-text.npy", occurrence["docs-text"])
    if "docs-headed" in occurrence:
        np.save(STUDY / "occurrence-headed.npy", occurrence["docs-headed"])
    print(written, "runs over", len(queries), "queries and", len(variants), "variants")


def remap(old_units: str):
    """After a chunker change renumbers the units: moves the expected ordinals of notes.jsonl and
    the units of judgments.jsonl from the old numbering to the new by their text. A unit the
    chunker has stopped producing keeps its old id in the judgments, where nothing can show it,
    and is reported when a note expected it."""
    old = {u["id"]: u for u in read_jsonl(Path(old_units))}
    # by_head is the fallback for a unit the chunker reflowed: same opening, different tail
    by_text, by_head = {}, {}
    for u in load_units():
        by_text[(u["doc"], u["text"])] = u
        by_head.setdefault((u["doc"], u["text"][:60]), u)

    def moved(unit_id):
        was = old.get(unit_id)
        if was is None:
            return None
        now = by_text.get((was["doc"], was["text"])) or by_head.get((was["doc"], was["text"][:60]))
        return now["id"] if now else None

    notes_path = GOLD / "folder-notes" / "notes.jsonl"
    notes, lost = read_jsonl(notes_path), 0
    for note in notes:
        for entry in note["expected"]:
            ords = []
            for o in entry["ords"]:
                now = moved(f"{entry['doc']}#{o}")
                if now is None:
                    print(note["qid"], "expected unit is gone:", f"{entry['doc']}#{o}")
                    lost += 1
                else:
                    ords.append(int(now.rsplit("#", 1)[1]))
            entry["ords"] = ords
    write_jsonl(notes_path, notes)

    path = GOLD / "folder-notes" / "judgments.jsonl"
    rows, kept, gone = read_jsonl(path), 0, 0
    for row in rows:
        now = moved(row["unit"])
        if now is None and row["unit"] in old:
            gone += 1
            row["unit"] = "gone:" + row["unit"]
        elif now is not None:
            kept += 1
            row["unit"] = now
    write_jsonl(path, rows)
    print(f"judgments: {kept} moved, {gone} on units that are gone; expected units lost: {lost}")


def check():
    """Refuses a folder note that shares a four-word sequence with a unit it expects, and an
    expected unit that does not exist."""
    by_id = {u["id"]: u for u in load_units()}

    def grams(text):
        t = re.findall(r"[a-z0-9]+", text.lower())
        return {tuple(t[i:i + 4]) for i in range(len(t) - 3)}

    bad = 0
    for q in load_queries():
        for unit_id in q.get("expected", []):
            if unit_id not in by_id:
                print(q["qid"], "expects a unit that does not exist:", unit_id)
                bad += 1
                continue
            shared = grams(q["text"]) & grams(by_id[unit_id]["text"])
            if shared:
                print(q["qid"], "echoes", unit_id, ":", " ".join(sorted(shared)[0]))
                bad += 1
    print("clean" if not bad else f"{bad} problems")


def pool(name: str = "runs.jsonl"):
    """Cards not yet judged, pooled over every variant and shuffled within a note so the judge
    cannot tell which variant showed a card or where it ranked."""
    by_id = {u["id"]: u for u in load_units()}
    judged = load_judgments()
    cards = sorted({(r["qid"], c["id"]) for r in load_runs(name) if r["set"] in INSIDE
                    for c in r["shown"]} - set(judged))
    random.Random(7).shuffle(cards)
    cards.sort(key=lambda card: card[0])
    with open(STUDY / "pool.json", "w", encoding="utf-8") as f:
        json.dump([{"qid": q, "unit": u} for q, u in cards], f, indent=0)
    with open(STUDY / "pool.txt", "w", encoding="utf-8", newline="\n") as f:
        for i, (q, u) in enumerate(cards):
            f.write(f"{i} {q} | {by_id[u]['doc']} p{by_id[u]['page'] + 1} | {by_id[u]['text']}\n")
    print(len(cards), "cards to judge")


def auc(positive: list[float], negative: list[float]) -> float:
    """The chance an in-scope score beats an out-of-scope one, ties counting half."""
    wins = sum((p > n) + 0.5 * (p == n) for p in positive for n in negative)
    return wins / (len(positive) * len(negative))


def score(name: str = "runs.jsonl"):
    """Graded judgments: 2 the guidance the note calls for, 1 on the topic, 0 neither. nDCG@3 is
    against the best three judged cards for the note, so a silent variant scores zero on it."""
    judged = load_judgments()
    runs = load_runs(name)
    # The best a note could be shown is judged over this folder's units only
    present = {u["id"] for u in load_units()}
    ideal = {}
    for (qid, unit), label in judged.items():
        if unit in present:
            ideal.setdefault(qid, []).append(label)

    def dcg(labels):
        return sum((2 ** g - 1) / math.log2(i + 2) for i, g in enumerate(labels))

    print(f"{'variant':<62} ndcg3  p3   first2 first0 quiet | silent gp  near  neg | unjudged | ndcg3 - V0 (95%)")
    rng = np.random.default_rng(7)
    baseline = None
    for variant in dict.fromkeys(r["variant"] for r in runs):
        rows = [r for r in runs if r["variant"] == variant]
        scope = [r for r in rows if r["set"] in INSIDE]
        ndcg, precision, first2, first0, quiet, unjudged = [], [], 0, 0, 0, 0
        for r in scope:
            labels = [judged.get((r["qid"], c["id"])) for c in r["shown"]]
            unjudged += labels.count(None)
            labels = [g or 0 for g in labels]
            best = dcg(sorted(ideal.get(r["qid"], [0]), reverse=True)[:LIMIT])
            ndcg.append(dcg(labels) / best if best else 0.0)
            if labels:
                precision.append(sum(g > 0 for g in labels) / len(labels))
                first2 += labels[0] == 2
                first0 += labels[0] == 0
            else:
                quiet += 1
        silent_counts = []
        for s in OUTSIDE:
            of_set = [r for r in rows if r["set"] == s]
            silent_counts.append(f"{sum(not r['shown'] for r in of_set)}/{len(of_set)}")

        n = len(scope)
        ndcg = np.asarray(ndcg)
        if baseline is None:
            baseline = ndcg  # the first variant listed is what the others are compared against
        low, high = mean_interval(rng, ndcg - baseline, BOOTSTRAP)
        counts = f"{first2:>3}/{n} {first0:>3}/{n} {quiet:>3}/{n}"
        print(f"{variant[:62]:<62} {ndcg.mean():.3f}  {np.mean(precision):.2f} {counts}"
              f" |  {'  '.join(silent_counts)} | {unjudged} | {low:+.3f} to {high:+.3f}")

    base = [r for r in runs if r["variant"] == runs[0]["variant"]]
    if not any(r.get("note_best") for r in base):
        return  # an engine run carries cards only
    inside = [r for r in base if r["set"] in INSIDE]
    out_of_scope = [r for r in base if r["set"] in OUTSIDE]
    for key, label in (("note_best", "whole note"), ("any_best", "best sentence")):
        by_set = ", ".join(f"{s} {auc([r[key] for r in inside], [r[key] for r in out_of_scope if r['set'] == s]):.3f}"
                           for s in OUTSIDE)
        overall = auc([r[key] for r in inside], [r[key] for r in out_of_scope])
        print(f"\nabstention signal, {label}: AUC {overall:.3f} ({by_set})")
        for floor in (0.83, 0.84, 0.85, 0.86, 0.87, 0.88):
            answered = sum(r[key] >= floor for r in inside)
            silent = sum(r[key] < floor for r in out_of_scope)
            print(f"   floor {floor:.2f}: answers {answered}/{len(inside)} in scope, "
                  f"silent on {silent}/{len(out_of_scope)} out of scope")


def models():
    shortlist = {e["id"]: e for e in load_shortlist()}
    unit_rows = load_units()
    index = read_json(STUDY / "queries.json")
    for name in EMBEDDERS:
        docs_path, queries_path = STUDY / f"docs-{name}.npy", STUDY / f"queries-{name}.npy"
        if docs_path.exists() and queries_path.exists() and len(np.load(queries_path)) == len(index):
            continue
        pipe = make_pipeline(shortlist[name], CANDIDATES / f"{name}-int8", 512)
        # A finished passage matrix is kept, so a grown query set costs only the queries again
        if not docs_path.exists():
            texts, vectors = [r["text"] for r in unit_rows], []
            for i in range(0, len(texts), 32):
                vectors.extend(pipe.embed_documents(texts[i:i + 32]))
                if i % 640 == 0:
                    print(name, i, "/", len(texts), flush=True)
            if vectors:
                np.save(docs_path, np.asarray(vectors, dtype=np.float32))
        np.save(queries_path, np.asarray([pipe.embed_query(e["sub"]) for e in index], dtype=np.float32))
        del pipe  # the next model loads into the same memory

    # A floor belongs to one model's similarity scale, so the embedders are ranked with it off
    queries = load_queries()
    by_query = group_by_query(index)
    rows = []
    for name in EMBEDDERS:
        matrix, qvec = np.load(STUDY / f"docs-{name}.npy"), np.load(STUDY / f"queries-{name}.npy")
        for q in queries:
            lists = [{"sub": e["sub"], "whole": e["whole"], "cos": matrix @ qvec[i]} for i, e in by_query[q["qid"]]]
            whole = next((e for e in lists if e["whole"]), lists[-1])
            for label, variant in (("vote", {"order": "vote", "floor": -1.0}),
                                   ("whole note", {"order": "cosine", "whole_only": True, "floor": -1.0})):
                result = rank(variant, lists, unit_rows)
                result["top10"] = [unit_rows[int(u)]["id"] for u in np.argsort(-whole["cos"])[:10]]
                rows.append({"qid": q["qid"], "set": q["set"], "variant": f"{name}, {label}",
                             "expected": q.get("expected", []), **result})
    print(write_jsonl(STUDY / "runs-models.jsonl", rows), "runs")


def compare():
    runs = load_runs("runs-models.jsonl")
    print(f"{'embedder, ranking':<28} s@1   s@3   r@10 | AUC whole note  best sentence")
    for variant in dict.fromkeys(r["variant"] for r in runs):
        rows = [r for r in runs if r["variant"] == variant]
        notes = [r for r in rows if r["set"] == "folder-notes"]
        s1 = np.mean([bool(r["shown"]) and r["shown"][0]["id"] in r["expected"] for r in notes])
        s3 = np.mean([any(c["id"] in r["expected"] for c in r["shown"]) for r in notes])
        r10 = np.mean([any(u in r["expected"] for u in r["top10"]) for r in notes])
        inside = [r for r in rows if r["set"] in INSIDE]
        out_of_scope = [r for r in rows if r["set"] in OUTSIDE]
        note_auc = auc([r["note_best"] for r in inside], [r["note_best"] for r in out_of_scope])
        sentence_auc = auc([r["any_best"] for r in inside], [r["any_best"] for r in out_of_scope])
        print(f"{variant:<28} {s1:.2f}  {s3:.2f}  {r10:.2f} |     {note_auc:.3f}          {sentence_auc:.3f}")


def parity(path: str):
    """The baseline variant's cards against the ones a folder_eval.py run recorded, by document
    and page. A difference means the port and the engine have drifted apart."""
    ours = {r["qid"]: r for r in load_runs() if r["variant"] == VARIANTS[0]["name"]}
    agree = total = 0
    for row in read_jsonl(Path(path)):
        engine = [(c["title"], c["page"]) for c in row["cards"]]
        mine = [(s["doc"], s["page"]) for s in ours[row["qid"]]["shown"]]
        total += 1
        agree += engine == mine
        print(row["qid"], "MATCH" if engine == mine else "DIFFER", "\n   engine:", engine, "\n   study: ", mine)
    print(f"{agree}/{total} cases identical")


def main():
    step = sys.argv[1] if len(sys.argv) > 1 else ""
    argument = sys.argv[2] if len(sys.argv) > 2 else None
    no_argument = {"units": units, "run": run, "check": check, "models": models, "compare": compare}
    if step in no_argument:
        no_argument[step]()
    elif step == "embed":
        embed(queries_only=argument == "queries")
    elif step == "pool":
        pool(argument or "runs.jsonl")
    elif step == "score":
        score(argument or "runs.jsonl")
    elif step == "remap" and argument:
        remap(argument)
    elif step == "parity" and argument:
        parity(argument)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
