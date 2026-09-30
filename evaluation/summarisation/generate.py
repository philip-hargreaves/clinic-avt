"""Generate notes with a model from evaluation/config.toml. Resumable: existing outputs are skipped.

  app    the app's own prompts over the sealed transcripts the pipeline produced
         (perf-loop tag [summarisation] transcript_tag), composed exactly as the engine
         composes them: style + one "SPEAKER: text" line per turn + detail file. Per consult,
         a note at each length, the patient sheet and the title from the concise note (the
         app's default), and optionally a SOAP note.
           notes/<tag>-<kind>/<cid>.md, notes/timings.jsonl
           tag: tier-<tier> for a shipped tier, else the model name

  study  a frozen research prompt (prompts/*.md, {transcript} placeholder) over the PriMock
         reference transcripts from prep.py, as the screening and prompt tuning ran.
           notes/<tag>/<cid>.md and notes/<tag>/_metrics.json

    python evaluation/summarisation/generate.py app --tier default
    python evaluation/summarisation/generate.py app --model gemma-4-e4b-it-int4-ov --limit 2 --details concise
    python evaluation/summarisation/generate.py study --model qwen3.5-4b-int4-ov --prompt prompt-4b-safety.md --tag qwen3.5-4b-safety
"""
import argparse
from pathlib import Path
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.io import append_jsonl, read_json  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DETAILS = ["concise", "detailed"]
REFUSAL = "NOT A CONSULTATION"


def data():
    return config.path("summarisation")


def notes_dir(name):
    d = data() / "notes" / name
    d.mkdir(parents=True, exist_ok=True)
    return d


PROMPT_OVERLAY = None  # --prompts: draft prompts that replace the app's file of the same name


def app_prompt(name):
    if PROMPT_OVERLAY and (PROMPT_OVERLAY / name).exists():
        return (PROMPT_OVERLAY / name).read_text(encoding="utf-8")
    return (config.path("prompts") / name).read_text(encoding="utf-8")


def transcript_block(turns):
    # note_prompt.hpp TranscriptBlock: one "ROLE: text" line per turn
    return "".join(f"{(t.get('speaker') or 'speaker').upper()}: {t.get('text', '')}\n" for t in turns)


def sealed_transcripts():
    tag = config.section("summarisation")["transcript_tag"]
    folder = config.path("perf_loop") / "transcripts"
    out = []
    for path in sorted(folder.glob(f"{tag}-*_mixed.json")):
        cid = path.name[len(tag) + 1:-len("_mixed.json")]
        out.append((cid, read_json(path)["turns"]))
    if not out:
        raise SystemExit(f"no sealed transcripts {tag}-*_mixed.json in {folder}")
    return out


def reference_transcripts():
    # PriMock reference lines are "Doctor: text" / "Patient: text"
    out = []
    for m in read_json(data() / "prep" / "manifest.json"):
        turns = []
        for line in (data() / "prep" / m["transcript"]).read_text(encoding="utf-8").splitlines():
            speaker, sep, text = line.partition(":")
            if sep:
                turns.append({"speaker": speaker.strip().lower(), "text": text.strip()})
        out.append((m["consult_id"], turns))
    return out


def resolve_model(args):
    tiers = config.section("summarisation")["tiers"]
    if args.tier:
        return tiers[args.tier], args.tag or f"tier-{args.tier}"
    if not args.model:
        raise SystemExit("give --tier or --model")
    tier = next((t for t, m in tiers.items() if m == args.model), None)
    return args.model, args.tag or (f"tier-{tier}" if tier else args.model)


def run_app(args):
    from common.llm import NoteModel
    name, tag = resolve_model(args)
    details = args.details.split(",")
    global PROMPT_OVERLAY
    if args.prompts:
        PROMPT_OVERLAY = Path(args.prompts)
        if not PROMPT_OVERLAY.is_dir():
            raise SystemExit(f"no prompt folder {PROMPT_OVERLAY}")
    style, soap = app_prompt("note-narrative.md"), app_prompt("note-soap.md")
    detail = {d: app_prompt(f"detail-{d}.md") for d in DETAILS}
    soap_detail = app_prompt("detail-soap.md")
    patient, label = app_prompt("patient-info.md"), app_prompt("label.md")
    items = reference_transcripts() if args.transcripts == "reference" else sealed_transcripts()
    if args.consults:
        wanted = set(args.consults.split(","))
        items = [item for item in items if item[0] in wanted]
    items = items[: args.limit or None]
    timings = data() / "notes" / "timings.jsonl"
    model = None

    def run(kind, cid, prompt, cap=1024):
        nonlocal model
        path = notes_dir(f"{tag}-{kind}") / f"{cid}.md"
        if path.exists():
            return path.read_text(encoding="utf-8")
        if model is None:
            model = NoteModel(name, args.device)
            print(f"{name} ({model.spec['pipeline']}, {model.spec['template']}) loaded in {model.load_s:.1f} s", flush=True)
            append_jsonl(timings, {"tag": tag, "model": name, "event": "load", "seconds": round(model.load_s, 1)})
        text, m = model.generate(prompt, cap)
        path.write_text(text, encoding="utf-8")
        append_jsonl(timings, {"tag": tag, "model": name, "consult": cid, "kind": kind,
                               "refused": text.startswith(REFUSAL), "words": len(text.split()), **m})
        return text

    for i, (cid, turns) in enumerate(items, 1):
        block = transcript_block(turns)
        concise = None
        for d in details:
            text = run(d, cid, style + block + "\n" + detail[d])
            if d == "concise":
                concise = text
        if concise and concise.strip() and not concise.startswith(REFUSAL):
            run("sheet", cid, patient + concise + "\n")
            run("label", cid, label + concise + "\n", 16)
        if i <= args.soap:
            run("soap", cid, soap + block + "\n" + soap_detail)
        print(f"[{tag}] {i}/{len(items)} {cid}", flush=True)


def run_study(args):
    from common.llm import NoteModel
    template = open(os.path.join(HERE, "prompts", args.prompt), encoding="utf-8").read()
    manifest = read_json(data() / "prep" / "manifest.json")
    if args.consult:
        manifest = [m for m in manifest if m["consult_id"] == args.consult]
    elif args.checklisted:
        manifest = [m for m in manifest if m["has_checklist"]]
    manifest = manifest[: args.limit or None]
    tag = args.tag or args.model
    out = notes_dir(tag)
    model = NoteModel(args.model, args.device)
    print(f"{args.model} ({model.spec['pipeline']}, {model.spec['template']}) loaded in {model.load_s:.1f} s", flush=True)
    rows = []
    for m in manifest:
        cid = m["consult_id"]
        path = out / f"{cid}.md"
        if path.exists():
            continue
        transcript = (data() / "prep" / m["transcript"]).read_text(encoding="utf-8")
        text, metrics = model.generate(template.replace("{transcript}", transcript), args.max_new_tokens)
        if "===NOTE===" in text:  # extract-then-write arm: keep only the note
            text = text.split("===NOTE===")[-1].strip()
        path.write_text(text, encoding="utf-8")
        rows.append({"consult_id": cid, "words": len(text.split()), **metrics})
        print(f"  {cid}: {len(text.split())} words, {metrics['wall_s']} s, {metrics['decode_tok_s']} tok/s", flush=True)
    if rows:
        rates = [r["decode_tok_s"] for r in rows]
        secs = [r["wall_s"] for r in rows]
        (out / "_metrics.json").write_text(json.dumps({
            "tag": tag, "model": args.model, "prompt": args.prompt, "n": len(rows),
            "median_tok_s": round(statistics.median(rates), 2),
            "median_seconds": round(statistics.median(secs), 2), "per_consult": rows}, indent=2),
            encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)
    app = sub.add_parser("app")
    app.add_argument("--tier", choices=["constrained", "default", "accuracy"])
    app.add_argument("--model")
    app.add_argument("--tag")
    app.add_argument("--details", default=",".join(DETAILS))
    app.add_argument("--soap", type=int, default=0, help="SOAP notes for the first N consults")
    app.add_argument("--consults", help="comma-separated consultation ids, e.g. a tuning set")
    app.add_argument("--prompts", help="folder of draft prompts that replace the app's files of the same name")
    app.add_argument("--transcripts", choices=["sealed", "reference"], default="sealed",
                     help="the app's own transcripts, or PriMock's reference ones (the ceiling)")
    app.add_argument("--limit", type=int, default=0)
    app.add_argument("--device", default="GPU")
    study = sub.add_parser("study")
    study.add_argument("--model", required=True)
    study.add_argument("--prompt", default="prompt-v1.md")
    study.add_argument("--tag")
    study.add_argument("--consult")
    study.add_argument("--checklisted", action="store_true")
    study.add_argument("--limit", type=int, default=0)
    study.add_argument("--max-new-tokens", type=int, default=512)
    study.add_argument("--device", default="GPU")
    args = ap.parse_args()
    (run_app if args.mode == "app" else run_study)(args)


if __name__ == "__main__":
    main()
