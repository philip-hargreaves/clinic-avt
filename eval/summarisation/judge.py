"""The note judge: one isolated LLM judge per note, with the frozen prompt (judge-prompt.md) and
output schema (judge-schema.json). Each judge sees one task file and nothing else, so the last
note is judged exactly like the first.

  tasks    one self-contained task per note under judge/tasks/<tag>/<cid>.md: the prompt,
           the transcript the note was written from, the note, and the consultation's checklist
           (empty for the 37 without one). Refusals are not judged.
  pending  tasks without a judge record yet: tag, consult, task file
  collect  validate each judge's raw JSON from judge/out/<tag>/<cid>.json and write the
           auditable record runs/<tag>/<cid>.json (note, claims, checklist, prompt names)

Running the judges: for every pending task, start a fresh judge (Claude Opus, no other context)
on the task file alone, constrained to judge-schema.json, and save its JSON reply to
judge/out/<tag>/<cid>.json. Then collect. A task with a record is skipped, so an interrupted
batch loses only the notes in flight.

    python eval/summarisation/judge.py tasks tier-default-standard --transcripts sealed
    python eval/summarisation/judge.py tasks qwen3.5-4b-safety
    python eval/summarisation/judge.py pending tier-default-standard
    python eval/summarisation/judge.py collect tier-default-standard --judge claude-opus-5-5
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import read_json, write_json  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
VERDICTS = ("supported", "contradicted", "unsupported")


def data():
    return config.path("summarisation")


def judge_prompt():
    text = open(os.path.join(HERE, "judge-prompt.md"), encoding="utf-8").read()
    return text.split("---", 1)[1].strip()  # the header above the rule is for readers


def transcript(cid, source, manifest):
    if source == "reference":
        return (data() / "prep" / manifest[cid]["transcript"]).read_text(encoding="utf-8")
    tag = config.section("summarisation")["transcript_tag"]
    path = config.path("perf_loop") / "transcripts" / f"{tag}-{cid}_mixed.txt"
    return path.read_text(encoding="utf-8") if path.exists() else None


def write_tasks(tag, source):
    manifest = {m["consult_id"]: m for m in read_json(data() / "prep" / "manifest.json")}
    prompt = judge_prompt()
    out = data() / "judge" / "tasks" / tag
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for path in sorted((data() / "notes" / tag).glob("*.md")):
        cid = path.stem
        note = path.read_text(encoding="utf-8").strip()
        source_text = transcript(cid, source, manifest) if cid in manifest else None
        if note.startswith("NOT A CONSULTATION") or source_text is None:
            continue
        checklist = manifest[cid]["checklist"]
        items = read_json(data() / "prep" / checklist) if checklist else []
        (out / f"{cid}.md").write_text(
            prompt + "\n\n---\n\nTRANSCRIPT:\n" + source_text + "\n\n---\n\nNOTE:\n" + note +
            "\n\n---\n\nCHECKLIST (JSON; may be empty):\n" + json.dumps(items, ensure_ascii=False, indent=1) + "\n",
            encoding="utf-8")
        count += 1
    print(f"{count} tasks under {out}")


def pending(tag):
    runs = data() / "runs" / tag
    return [(path.stem, path) for path in sorted((data() / "judge" / "tasks" / tag).glob("*.md"))
            if not (runs / f"{path.stem}.json").exists()]


def parse(raw):
    raw = raw.strip()
    if raw.startswith("```"):
        raw = raw[raw.find("{"):raw.rfind("}") + 1]
    output = json.loads(raw)
    assert isinstance(output.get("claims"), list) and isinstance(output.get("checklist"), list)
    for claim in output["claims"]:
        assert claim["verdict"] in VERDICTS, claim["verdict"]
    return output


def collect(tag, gen_prompt, judge_model):
    ok = bad = 0
    for path in sorted((data() / "judge" / "out" / tag).glob("*.json")):
        cid = path.stem
        try:
            output = parse(path.read_text(encoding="utf-8"))
        except (ValueError, AssertionError, KeyError) as e:
            bad += 1
            print(f"BAD {tag} {cid}: {e}")
            continue
        note = data() / "notes" / tag / f"{cid}.md"
        write_json(data() / "runs" / tag / f"{cid}.json", {
            "consult_id": cid, "model": tag, "gen_prompt": gen_prompt, "judge_prompt": "judge-prompt.md",
            "judge_model": judge_model, "note": note.read_text(encoding="utf-8") if note.exists() else None,
            "claims": output["claims"], "checklist": output["checklist"]})
        ok += 1
    print(f"collected {ok}, bad {bad}, still pending {len(pending(tag))}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tasks")
    t.add_argument("tag")
    t.add_argument("--transcripts", choices=["reference", "sealed"], default="reference",
                   help="what the notes were written from: PriMock reference or the app's sealed transcripts")
    p = sub.add_parser("pending")
    p.add_argument("tag")
    c = sub.add_parser("collect")
    c.add_argument("tag")
    c.add_argument("--gen-prompt", default="prompt-v1.md")
    c.add_argument("--judge", required=True, help="the judge model, recorded with every result")
    args = ap.parse_args()
    if args.cmd == "tasks":
        write_tasks(args.tag, args.transcripts)
    elif args.cmd == "pending":
        for cid, path in pending(args.tag):
            print(f"{args.tag}\t{cid}\t{path}")
    else:
        collect(args.tag, args.gen_prompt, args.judge)


if __name__ == "__main__":
    main()
