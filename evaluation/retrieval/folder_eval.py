"""The St George's cases, or every folder_study.py query, through the engine the app ships.

    python folder_eval.py [--tag before] [study]

Sends each case as one typed search. Writes build/retrieval/folder-<date>-<tag>.jsonl and prints
hit@1, hit@3 and abstentions against the gold's `expected_documents`. study writes every study
query's cards to the study working directory as runs-engine-<tag>.jsonl, for
`folder_study.py score`. Runs the engine from the app's Debug bin folder, so close the app first.
"""

import glob
import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "evaluation"))
from common.engine_pipe import Engine  # noqa: E402

APP_BIN = glob.glob(os.path.join(ROOT, "app", "ClinicAVT.App", "bin", "x64", "Debug", "net*", "win-x64"))
ENGINE = os.path.join(APP_BIN[0], "clinicavt_engine.exe") if APP_BIN else ""
GOLD = os.path.join(ROOT, "rag", "gold", "st-georges-cases", "cases.jsonl")
INDEX_MINUTES = 20
LOG = os.path.join(ROOT, "build", "folder-eval.log")


def log(line):
    stamp = time.strftime("%H:%M:%S")
    print(f"{stamp} {line}", flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{stamp} {line}\n")


def launch():
    # The shipped layout: the engine finds its models, corpora and store beside itself
    pipe = f"LOCAL\\clinicavt-folder-eval-{os.getpid()}"
    return Engine([ENGINE, pipe], pipe, os.path.join(ROOT, "build", "folder-eval-engine.log"),
                  cwd=os.path.dirname(ENGINE))


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


def search(engine, case):
    engine.notifications.clear()
    engine.request("guidance/search", {"text": case["text"], "limit": 3})
    msg = engine.wait_for({"guidance/ready", "guidance/failed"}, 120)
    return (msg or {}).get("params", {}).get("shown", [])


def study(engine, tag):
    import folder_study as fs
    # The engine numbers its units itself, so a card is matched to the study's unit by its text
    by_text = {(u["doc"], " ".join(u["text"].split())): u["id"] for u in fs.load_units()}
    rows, unmatched = [], 0
    for q in fs.load_queries():
        shown = search(engine, q)
        cards = []
        for c in shown:
            unit = by_text.get((c.get("title"), " ".join((c.get("text") or "").split())))
            unmatched += unit is None
            cards.append({"id": unit or f"{c.get('title')}#engine-{c.get('chunkId', '').rsplit('-', 1)[-1]}",
                          "doc": c.get("title"), "page": c.get("page", 0) + 1, "cos": c.get("score"),
                          "trigger": c.get("trigger"), "text": c.get("text")})
        rows.append({"qid": q["qid"], "set": q["set"], "variant": f"engine {tag}", "expected": q.get("expected", []),
                     "shown": cards, "note_best": 0, "any_best": 0})
        log(f"{q['qid']}: {len(cards)} cards")
    out = str(fs.STUDY / f"runs-engine-{tag}.jsonl")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    log(f"{out}; {unmatched} cards matched no study unit")


def main():
    global LOG
    if not ENGINE or not os.path.exists(ENGINE):
        sys.exit("build the app first: no engine beside ClinicAVT.App")
    tag = sys.argv[sys.argv.index("--tag") + 1] if "--tag" in sys.argv else "run"
    LOG = os.path.join(ROOT, "build", f"folder-eval-{tag}.log")
    cases = [json.loads(line) for line in open(GOLD, encoding="utf-8")]
    engine = launch()
    rows = []
    try:
        # A minute for the engine to answer at all, then four for the embedder to load
        engine.wait_up()
        for _ in range(240):
            msg = engine.wait_for({"guidance/model"}, 1)
            if msg and msg["params"].get("phase") == "ready":
                break
        docs = wait_for_index(engine)
        log(f"{len(docs)} documents indexed, {sum(d.get('chunks', 0) for d in docs)} units")
        if "study" in sys.argv:
            study(engine, tag)
            return
        for case in cases:
            shown = search(engine, case)
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
            log(f"{case['qid']}: hit@1 {row['hit1']}, hit@3 {row['hit3']}")
            for c in row["cards"]:
                log(f"   {c['score']}  {c['title']} p{c['page']} [{c['section']}]  <- {(c['trigger'] or 'whole note')[:70]}")
    finally:
        engine.close()
    out_dir = os.path.join(ROOT, "build", "retrieval")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"folder-{time.strftime('%Y%m%d')}-{tag}.jsonl")
    with open(out, "w", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    scored = [r for r in rows if r["expected"]]
    log(f"hit@1 {sum(r['hit1'] for r in scored)}/{len(scored)}, hit@3 {sum(r['hit3'] for r in scored)}/{len(scored)}, "
           f"abstained {sum(r['abstained'] for r in rows)}; {out}")


if __name__ == "__main__":
    main()
