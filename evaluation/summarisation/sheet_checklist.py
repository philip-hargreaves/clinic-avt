"""Checklists for the patient sheet: what each sheet must carry across from the note it was written from.

  tasks    one self-contained extraction task per note under judge/tasks/sheet-checklist-<tag>/<cid>.md,
           using the frozen sheet-checklist-prompt.md
  collect  validate each extractor's raw JSON from judge/out/sheet-checklist-<tag>/<cid>.json and write the
           checklist to sheet-checklists/<tag>/<cid>.json in the judge's {id, text, criticality} form

The sheet judge reads them with `judge.py tasks <sheet tag> --against <note tag>`.

    python evaluation/summarisation/sheet_checklist.py tasks final19-constrained-concise
    python evaluation/summarisation/sheet_checklist.py collect final19-constrained-concise
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import write_json  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
CATEGORIES = ("diagnosis", "medicine", "test", "referral", "follow-up", "warning-sign", "advice")


def data():
    return config.path("summarisation")


def tasks(tag, consults):
    prompt = open(os.path.join(HERE, "sheet-checklist-prompt.md"), encoding="utf-8").read().split("---", 1)[1].strip()
    out = data() / "judge" / "tasks" / f"sheet-checklist-{tag}"
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for path in sorted((data() / "notes" / tag).glob("*.md")):
        note = path.read_text(encoding="utf-8").strip()
        if (consults and path.stem not in consults) or not note or note.startswith("NOT A CONSULTATION"):
            continue
        (out / path.name).write_text(prompt + "\n\n---\n\nCLINICAL NOTE:\n" + note + "\n", encoding="utf-8")
        count += 1
    print(f"{count} tasks under {out}")


def collect(tag):
    ok = bad = 0
    for path in sorted((data() / "judge" / "out" / f"sheet-checklist-{tag}").glob("*.json")):
        try:
            raw = path.read_text(encoding="utf-8").strip()
            items = json.loads(raw[raw.find("{"):raw.rfind("}") + 1])["items"]
            for item in items:
                assert item["category"] in CATEGORIES and item["criticality"] in ("critical", "minor")
                assert item["text"].strip()
        except (ValueError, KeyError, AssertionError) as e:
            bad += 1
            print(f"BAD {tag} {path.stem}: {e}")
            continue
        write_json(data() / "sheet-checklists" / tag / f"{path.stem}.json",
                   [{"id": i["id"], "text": i["text"], "criticality": i["criticality"], "category": i["category"]}
                    for i in items])
        ok += 1
    print(f"collected {ok}, bad {bad}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tasks")
    t.add_argument("tag")
    t.add_argument("--consults", help="comma-separated consultation ids")
    c = sub.add_parser("collect")
    c.add_argument("tag")
    args = ap.parse_args()
    if args.cmd == "tasks":
        tasks(args.tag, set(args.consults.split(",")) if args.consults else None)
    else:
        collect(args.tag)


if __name__ == "__main__":
    main()
