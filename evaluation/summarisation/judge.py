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

    python evaluation/summarisation/judge.py tasks tier-default-concise --transcripts sealed
    python evaluation/summarisation/judge.py tasks qwen3.5-4b-safety
    python evaluation/summarisation/judge.py pending tier-default-concise
    python evaluation/summarisation/judge.py collect tier-default-concise --judge claude-opus-5-5
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
    path = config.path("perf_loop") / "transcripts" / f"{tag}-{cid}_mixed.json"
    if not path.exists():
        return None
    # The dialogue only, in the reading copy's line format. The .txt copy appends the note and sheet
    # written during the run, which a judge could take as evidence
    return "".join(f"[{t['firstFrame'] / 16000:7.1f}s] {t['speaker'] or '?':8s} {t['text']}\n"
                   for t in read_json(path)["turns"])


def source_note(cid, note_tag):
    path = data() / "notes" / note_tag / f"{cid}.md"
    if not path.exists():
        return None
    return ("(This source is the clinical note the patient sheet was written from, not a transcript. "
            "Judge the sheet's claims against this note.)\n\n" + path.read_text(encoding="utf-8").strip() + "\n")


def write_tasks(tag, source, against=None):
    # With a source note, the sheet is judged against the note it was written from, under <tag>-vsnote
    manifest = {m["consult_id"]: m for m in read_json(data() / "prep" / "manifest.json")}
    prompt = judge_prompt()
    out = data() / "judge" / "tasks" / (f"{tag}-vsnote" if against else tag)
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for path in sorted((data() / "notes" / tag).glob("*.md")):
        cid = path.stem
        note = path.read_text(encoding="utf-8").strip()
        if against:
            source_text = source_note(cid, against)
        else:
            source_text = transcript(cid, source, manifest) if cid in manifest else None
        if note.startswith("NOT A CONSULTATION") or source_text is None:
            continue
        if against:
            sheet_list = data() / "sheet-checklists" / against / f"{cid}.json"
            items = [{k: i[k] for k in ("id", "text", "criticality")} for i in read_json(sheet_list)] \
                if sheet_list.exists() else []
        else:
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
    t.add_argument("--against", metavar="NOTE_TAG",
                   help="judge patient sheets against the notes they were written from (tasks go to <tag>-vsnote)")
    p = sub.add_parser("pending")
    p.add_argument("tag")
    c = sub.add_parser("collect")
    c.add_argument("tag")
    c.add_argument("--gen-prompt", default="prompt-v1.md")
    c.add_argument("--judge", required=True, help="the judge model, recorded with every result")
    args = ap.parse_args()
    if args.cmd == "tasks":
        write_tasks(args.tag, args.transcripts, args.against)
    elif args.cmd == "pending":
        for cid, path in pending(args.tag):
            print(f"{args.tag}\t{cid}\t{path}")
    else:
        collect(args.tag, args.gen_prompt, args.judge)


if __name__ == "__main__":
    main()
