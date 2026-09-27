"""Records the demo masters: every bundled track replayed at 1x into the app's own store, with
the note tier, style and detail the app is set to, and names each one in masters.json beside the
app's preferences, so demo mode can play them back from the record button.

    python tools/demo/record_masters.py [track name ...]

Runs the engine the app ships (its bin folder: models, corpora and store all resolve as in the
app), so close the app first; one model-loading job at a time.
"""

import glob
import json
import os
import sys
import time
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "common"))
from engine_pipe import Engine  # noqa: E402

APP_BIN = glob.glob(os.path.join(ROOT, "app", "ClinicAVT.App", "bin", "x64", "Debug", "net*", "win-x64"))
ENGINE = os.path.join(APP_BIN[0], "clinicavt_engine.exe") if APP_BIN else ""
TRACKS = os.path.join(ROOT, "demo", "tracks.json")
LOG = os.path.join(ROOT, "build", "demo-masters.log")
PREFERENCES = os.path.join(os.environ["LOCALAPPDATA"], "clinicavt", "preferences.json")
MASTERS = os.path.join(os.environ["LOCALAPPDATA"], "clinicavt", "masters.json")


def log(line):
    stamp = time.strftime("%H:%M:%S")
    print(f"{stamp} {line}", flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{stamp} {line}\n")


def launch():
    # The shipped layout: the engine finds its models, corpora and store beside itself
    pipe = f"LOCAL\\clinicavt-masters-{os.getpid()}"
    return Engine([ENGINE, pipe], pipe, os.path.join(ROOT, "build", "demo-masters-engine.log"),
                  cwd=os.path.dirname(ENGINE))


def wav_seconds(path):
    with wave.open(path) as w:
        return w.getnframes() / w.getframerate()


def record(engine, name, path):
    seconds = wav_seconds(path)
    log(f"{name}: replaying {seconds:.0f} s at 1x")
    started = engine.request("session/start", {"replay": {"path": path, "speed": 1, "monitor": False}})
    if engine.wait_for({"session/interrupted"}, seconds + 3) is not None:
        log(f"{name}: INTERRUPTED")
        return False
    stopped = engine.request("session/stop", None, 600)
    session = stopped.get("sessionId") or started.get("sessionId")
    log(f"{name}: finalised {session}")
    note = engine.wait_for({"note/ready", "note/failed", "note/refused"}, 900)
    if note is None or note["method"] != "note/ready":
        log(f"{name}: NO NOTE ({note and note['method']})")
        return False
    # The guidance search lands while the patient sheet streams
    outcomes = {}
    deadline = time.monotonic() + 600
    while len(outcomes) < 2 and time.monotonic() < deadline:
        msg = engine.wait_for({"patient/ready", "patient/failed", "guidance/ready", "guidance/failed"},
                              deadline - time.monotonic())
        if msg is None:
            break
        outcomes[msg["method"].split("/")[0]] = msg["method"]
    # The title is written last, after the sheet; the next track must not cut it off
    row = {}
    for _ in range(120):
        rows = engine.request("session/list")["sessions"]
        row = next((s for s in rows if s["id"] == session), {})
        if row.get("label"):
            break
        time.sleep(1)
    log(f"{name}: note ok, {outcomes.get('patient')}, {outcomes.get('guidance')}, label '{row.get('label')}'")
    return row if row.get("label") else None


def remember(name, row):
    # Demo mode plays the master this file names for the chosen track
    masters = {}
    if os.path.exists(MASTERS):
        with open(MASTERS, encoding="utf-8") as f:
            masters = json.load(f)
    masters[name] = {"id": row["id"], "audioSeconds": row["audioSeconds"], "label": row["label"]}
    with open(MASTERS, "w", encoding="utf-8") as f:
        json.dump(masters, f, indent=2)


def main():
    if not ENGINE or not os.path.exists(ENGINE):
        sys.exit("build the app first: no engine beside ClinicAVT.App")
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(TRACKS, encoding="utf-8") as f:
        tracks = json.load(f)["tracks"]
    wanted = set(sys.argv[1:])
    if wanted:
        tracks = [t for t in tracks if t["name"] in wanted]
    engine = launch()
    try:
        engine.wait_up()
        # The masters are written the way the app is set up to write
        prefs = {}
        if os.path.exists(PREFERENCES):
            with open(PREFERENCES, encoding="utf-8") as f:
                prefs = json.load(f)
        tier = prefs.get("NoteTier") or "accuracy"
        style = prefs.get("NoteStyle") or "prose"
        detail = prefs.get("NoteDetail") or "standard"
        loaded = engine.request("note/tier", {"tier": tier}, 60)
        engine.request("note/options", {"style": style, "detail": detail})
        log(f"note tier {tier}: {loaded.get('id')} ({loaded.get('state')}), {style} {detail}")
        for _ in range(1200):
            if engine.request("engine/readiness", None, 10).get("ready"):
                break
            time.sleep(0.5)
        results = {}
        for track in tracks:
            path = os.path.join(os.path.dirname(TRACKS), track["file"])
            row = record(engine, track["name"], path)
            results[track["name"]] = row is not None
            if row:
                remember(track["name"], row)
        log("done: " + ", ".join(f"{k}={'ok' if v else 'FAILED'}" for k, v in results.items()))
    finally:
        engine.close()


if __name__ == "__main__":
    main()
