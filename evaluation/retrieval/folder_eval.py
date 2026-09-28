"""Scores the guidance tab on the St George's cases against the clinician's own folder, through
the engine the app ships. Every card is checked against the documents the gold names as
`expected_documents`.

    python tools/retrieval/folder_eval.py [--tag before] [--mode note|query] [study]

note (default) is what the app does after a note is written: the case becomes the stored note
of a demo copy and the engine searches it sentence by sentence. query sends the case as one
typed search. Either writes build/retrieval/folder-<date>-<tag>.jsonl with every card and the
sentence that found it, and prints hit@1, hit@3 and abstentions.

study runs every query of folder_study.py, in scope and out, and writes its cards in that
tool's run format to the study working directory as runs-engine-<tag>.jsonl, so that
`folder_study.py score runs-engine-<tag>.jsonl` judges the engine itself.

Close the app first.
"""

import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "demo"))
sys.path.insert(0, os.path.join(ROOT, "tools", "retrieval"))
import record_masters as rm  # noqa: E402

GOLD = os.path.join(ROOT, "rag", "gold", "st-georges-cases", "cases.jsonl")
INDEX_MINUTES = 20


def wait_for_index(engine):
    """Waits until every document in the folder is indexed: a chunker or embedder change
    rebuilds the whole index at startup, and searching before that reads a partial one."""
    settled = 0
    for _ in range(INDEX_MINUTES * 12):
        docs = engine.request("guidance/documents", None, 30).get("documents", [])
        if docs and all(d.get("state") in ("ready", "failed", "unsupported") for d in docs):
            settled += 1
            if settled >= 3:
                return docs
        else:
            settled = 0
        time.sleep(5)
    raise TimeoutError("the folder index did not settle")


def demo_copy(engine):
    """A throwaway demo record to hold each case as its note, made the way demo mode makes one."""
    with open(rm.MASTERS, encoding="utf-8") as f:
        source = next(iter(json.load(f).values()))["id"]
    copy = engine.request("session/start", {"playback": {"id": source}})["sessionId"]
    engine.wait_for({"audio.level"}, 10)
    engine.request("session/stop", None, 60)
    engine.wait_for({"patient/ready", "patient/failed"}, 180)
    return copy


def search(engine, case, copy):
    engine.notifications.clear()
    if copy is None:
        engine.request("guidance/search", {"text": case["text"], "limit": 3})
    else:
        engine.request("note/update", {"id": copy, "text": case["text"]})
        engine.notifications.clear()
        engine.request("guidance/search", {"id": copy})
    msg = engine.wait_for({"guidance/ready", "guidance/failed"}, 120)
    return (msg or {}).get("params", {}).get("shown", [])


def study(engine, copy, tag):
    import folder_study as fs
    # The engine numbers its units itself, so a card is matched to the study's unit by its text
    by_text = {(u["doc"], " ".join(u["text"].split())): u["id"] for u in fs.load_units()}
    rows, unmatched = [], 0
    for q in fs.load_queries():
        shown = search(engine, q, copy)
        cards = []
        for c in shown:
            unit = by_text.get((c.get("title"), " ".join((c.get("text") or "").split())))
            unmatched += unit is None
            cards.append({"id": unit or f"{c.get('title')}#engine-{c.get('chunkId', '').rsplit('-', 1)[-1]}",
                          "doc": c.get("title"), "page": c.get("page", 0) + 1, "cos": c.get("score"),
                          "trigger": c.get("trigger"), "text": c.get("text")})
        rows.append({"qid": q["qid"], "set": q["set"], "variant": f"engine {tag}", "expected": q.get("expected", []),
                     "shown": cards, "note_best": 0, "any_best": 0})
        rm.log(f"{q['qid']}: {len(cards)} cards")
    out = str(fs.STUDY / f"runs-engine-{tag}.jsonl")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    rm.log(f"{out}; {unmatched} cards matched no study unit")


def main():
    tag = sys.argv[sys.argv.index("--tag") + 1] if "--tag" in sys.argv else "run"
    mode = sys.argv[sys.argv.index("--mode") + 1] if "--mode" in sys.argv else "note"
    rm.LOG = os.path.join(ROOT, "build", f"folder-eval-{tag}.log")
    cases = [json.loads(line) for line in open(GOLD, encoding="utf-8")]
    engine = rm.launch()
    rows = []
    copy = None
    try:
        # A minute for the engine to answer at all, then four for the embedder to load
        engine.wait_up()
        for _ in range(240):
            msg = engine.wait_for({"guidance/model"}, 1)
            if msg and msg["params"].get("phase") == "ready":
                break
        docs = wait_for_index(engine)
        rm.log(f"{mode} mode, {len(docs)} documents indexed, {sum(d.get('chunks', 0) for d in docs)} units")
        if mode == "note":
            copy = demo_copy(engine)
        if "study" in sys.argv:
            study(engine, copy, tag)
            return
        for case in cases:
            shown = search(engine, case, copy)
            expected = case.get("expected_documents", [])
            titles = [c.get("title", "") for c in shown]
            row = {
                "qid": case["qid"],
                "expected": expected,
                "cards": [{"title": c.get("title"), "page": c.get("page", 0) + 1, "score": c.get("score"),
                           "section": c.get("section"), "trigger": c.get("trigger"),
                           "words": len((c.get("text") or "").split()), "text": c.get("text")} for c in shown],
                "hit1": bool(titles) and titles[0] in expected,
                "hit3": any(t in expected for t in titles),
                "abstained": not shown,
            }
            rows.append(row)
            rm.log(f"{case['qid']}: hit@1 {row['hit1']}, hit@3 {row['hit3']}")
            for c in row["cards"]:
                rm.log(f"   {c['score']}  {c['title']} p{c['page']} [{c['section']}]  <- {(c['trigger'] or 'whole note')[:70]}")
    finally:
        if copy is not None:
            try:
                engine.request("session/close")
                engine.request("session/delete", {"id": copy})
            except (TimeoutError, RuntimeError):
                pass
        engine.close()
    out_dir = os.path.join(ROOT, "build", "retrieval")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"folder-{time.strftime('%Y%m%d')}-{tag}.jsonl")
    with open(out, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    scored = [r for r in rows if r["expected"]]
    rm.log(f"hit@1 {sum(r['hit1'] for r in scored)}/{len(scored)}, hit@3 {sum(r['hit3'] for r in scored)}/{len(scored)}, "
           f"abstained {sum(r['abstained'] for r in rows)}; {out}")


if __name__ == "__main__":
    main()
