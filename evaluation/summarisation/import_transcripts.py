"""Sealed transcripts from the app's import path. Each PriMock57 mixed recording is imported into
the release engine, as a clinician would load an audio file, and the stored transcript is saved
where generate.py and judge.py read it. The engine also writes a note and sheet, which are saved
alongside but not judged.

    python evaluation/summarisation/import_transcripts.py [--tag IMP] [--limit N]

Writes <perf_loop>/transcripts/<tag>-<consult>_mixed.json and a timings line per consult to
<tag>-import.jsonl. A consult already saved is skipped, so a killed run resumes.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.engine_pipe import Engine, EngineDied  # noqa: E402

STARTED_AT = "2026-09-01T09:00:00Z"  # the store needs a start time and any past one works
IMPORT_TIMEOUT = 1800.0
NOTE_TIMEOUT = 900.0


def start_engine(root, tier):
    os.makedirs(os.path.join(root, "logs"), exist_ok=True)
    pipe = f"LOCAL\\clinicavt-import-{os.getpid()}"
    engine = Engine([str(config.path("engine")), "--note-tier", tier, pipe,
                     os.path.join(root, "store"), str(config.path("app_models"))],
                    pipe, os.path.join(root, "logs", f"engine-{int(time.time())}.log"))
    engine.wait_up()
    for _ in range(1200):
        if engine.request("engine/readiness", None, 10).get("ready"):
            return engine
        time.sleep(0.5)
    raise RuntimeError("engine never became ready")


def import_one(engine, wav):
    t0 = time.perf_counter()
    sid = engine.request("session/import", {"path": wav, "startedAt": STARTED_AT,
                                            "retain": False}, 60)["sessionId"]
    msg = engine.wait_for({"session/imported", "session/importFailed"}, IMPORT_TIMEOUT)
    if msg is None or msg["method"] != "session/imported":
        raise RuntimeError(f"import failed: {msg and msg.get('params')}")
    imported = time.perf_counter() - t0
    # The engine writes the note and sheet after the import. Wait for both so the next import
    # has the GPU to itself
    note = patient = None
    deadline = time.perf_counter() + NOTE_TIMEOUT
    while time.perf_counter() < deadline and patient is None:
        msg = engine.wait_for({"note/ready", "note/failed", "note/refused", "patient/ready",
                               "patient/failed"}, deadline - time.perf_counter())
        if msg is None:
            break
        method = msg["method"]
        if method.startswith("note/"):
            note = method
            if method != "note/ready":
                break  # no sheet follows a failed or refused note
        else:
            patient = method
    turns = engine.request("session/transcript", {"id": sid}, 30).get("turns", [])
    stored_note = engine.request("session/note", {"id": sid}, 30).get("text")
    stored_patient = engine.request("session/patient", {"id": sid}, 30).get("text")
    engine.request("session/delete", {"id": sid}, 30)
    timing = {"import_s": round(imported, 1), "all_s": round(time.perf_counter() - t0, 1),
              "note": note, "patient": patient, "turns": len(turns)}
    return {"turns": turns, "note": stored_note, "patient": stored_patient}, timing


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", default=config.section("summarisation")["transcript_tag"])
    ap.add_argument("--tier", default="default", help="the note tier the engine writes with")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    root = str(config.path("perf_loop"))
    out = os.path.join(root, "transcripts")
    os.makedirs(out, exist_ok=True)
    audio = config.path("mixed_audio")
    wavs = sorted(p for p in audio.glob("*_mixed.wav"))
    if args.limit:
        wavs = wavs[:args.limit]
    engine = start_engine(root, args.tier)
    try:
        for i, wav in enumerate(wavs, 1):
            target = os.path.join(out, f"{args.tag}-{wav.stem}.json")
            if os.path.exists(target):
                continue
            try:
                saved, timing = import_one(engine, str(wav))
            except EngineDied:
                print(f"{wav.stem}: engine died, restarting", flush=True)
                engine = start_engine(root, args.tier)
                saved, timing = import_one(engine, str(wav))
            with open(target, "w", encoding="utf-8") as f:
                json.dump(saved, f, indent=1)
            with open(os.path.join(root, f"{args.tag}-import.jsonl"), "a", encoding="utf-8") as f:
                f.write(json.dumps({"consult": wav.stem, **timing}) + "\n")
            print(f"[{i}/{len(wavs)}] {wav.stem}: {timing}", flush=True)
    finally:
        engine.close()


if __name__ == "__main__":
    main()
