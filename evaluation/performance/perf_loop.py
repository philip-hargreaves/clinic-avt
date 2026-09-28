"""1x performance loop: drives the release engine over its pipe with replayed consultations at real
time and records per-phase timings, engine memory and every engine death.

    python evaluation/performance/perf_loop.py [hours=8] [track prefix]
"""
import ctypes
import ctypes.wintypes
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config  # noqa: E402
from common.engine_pipe import Engine as PipeEngine, EngineDied  # noqa: E402

# EVAL_ENGINE: run a copied binary so rebuilds don't fight a live sweep.
# PERF_ENGINE_ARGS: extra flags, e.g. "--asr-device NPU".
ENGINE = str(config.path("engine"))
ENGINE_ARGS = os.environ.get("PERF_ENGINE_ARGS", "").split()
MODELS = str(config.path("app_models"))
# Tracks, store, logs and results live under build/
HERE = str(config.path("perf_loop"))
STORE = os.path.join(HERE, "store")
LOGS = os.path.join(HERE, "logs")
TAG = os.environ.get("PERF_TAG", "")  # experiments keep their own result files
RESULTS = os.path.join(HERE, f"runs{TAG}.jsonl")
EVENTS = os.path.join(HERE, f"events{TAG}.jsonl")
STOP_FILE = os.path.join(HERE, "STOP")
PAUSE_FILE = os.path.join(HERE, "PAUSE")  # present: wait between runs; delete to continue
# PERF_SKIP_DONE=1: a track whose saved transcript already exists under this tag is skipped,
# so a killed sweep resumes where it stopped (needs PERF_SAVE; never for repeated-run tags)
SKIP_DONE = os.environ.get("PERF_SKIP_DONE", "") == "1"

TRACKS = [
    ("c02m_elbow.wav", 120),
    ("c05m_elbow.wav", 300),
    ("c10m_weight.wav", 600),
    ("c15m_elbow_chest.wav", 902),
    ("c20m_weight_allergy.wav", 1200),
    ("cfull_elbow.wav", 542),  # the whole consult, for reading transcripts before/after
    ("cfull_day1_consultation01.wav", 458),
    ("cfull_day1_consultation09.wav", 537),
    ("cfull_day2_consultation07.wav", 458),
    ("cfull_day3_consultation03.wav", 697),
]
SAVE_DIR = os.path.join(HERE, "transcripts") if os.environ.get("PERF_SAVE") else None
# Sweeps: every wav in a directory, one engine for the lot, optionally faster than real time
SPEED = float(os.environ.get("PERF_SPEED", "1.0"))
AUDIO_DIR = os.environ.get("PERF_AUDIO_DIR")
if AUDIO_DIR:
    import wave
    TRACKS = []
    for name in sorted(os.listdir(AUDIO_DIR)):
        if name.endswith(".wav") and not name.endswith(("_eq.wav", "_far.wav")):
            with wave.open(os.path.join(AUDIO_DIR, name)) as w:
                TRACKS.append((name, w.getnframes() / w.getframerate()))
NOTE_TIMEOUT = 600.0
STOP_TIMEOUT = 600.0


def now():
    return time.perf_counter()


def stamp():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def log(line):
    print(f"{datetime.now().strftime('%H:%M:%S')} {line}", flush=True)


def saved_base(track):
    return os.path.join(SAVE_DIR, f"{TAG.lstrip('-') or 'run'}-{track.rsplit('.', 1)[0]}")


def event(kind, **fields):
    with open(EVENTS, "a", encoding="utf-8") as f:
        f.write(json.dumps({"t": stamp(), "kind": kind, **fields}) + "\n")


# --- Windows memory readings (no psutil dependency) --------------------------

class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.wintypes.DWORD),
        ("PageFaultCount", ctypes.wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


class MemoryStatusEx(ctypes.Structure):
    _fields_ = [
        ("dwLength", ctypes.wintypes.DWORD),
        ("dwMemoryLoad", ctypes.wintypes.DWORD),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def process_memory_mb(pid):
    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED
    if not handle:
        return None
    try:
        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return None
        return {"ws_mb": round(counters.WorkingSetSize / 2**20),
                "private_mb": round(counters.PrivateUsage / 2**20)}
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def system_memory_mb():
    status = MemoryStatusEx()
    status.dwLength = ctypes.sizeof(status)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    return {"avail_phys_mb": round(status.ullAvailPhys / 2**20),
            "avail_commit_mb": round(status.ullAvailPageFile / 2**20)}


def note_host_pids():
    out = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq clinicavt_note_host.exe", "/FO", "CSV", "/NH"],
        capture_output=True, text=True).stdout
    pids = []
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0] == "clinicavt_note_host.exe":
            pids.append(int(parts[1]))
    return pids


# --- Engine process ------------------------------------------------------------

class Engine(PipeEngine):
    # extra_args: positional arguments after the models root, e.g. a replay wav
    def __init__(self, index, extra_args=()):
        self.index = index
        pipe = f"LOCAL\\clinicavt-perf-{os.getpid()}-{index}"
        super().__init__([ENGINE, *ENGINE_ARGS, pipe, STORE, MODELS, *extra_args], pipe,
                         os.path.join(LOGS, f"engine-{index:03d}.log"))


def timed(engine, method, params, timeout):
    t0 = now()
    result = engine.request(method, params, timeout)
    return result, now() - t0


# --- One replayed consultation -----------------------------------------------

def run_session(engine, track, duration, cycle, run_index, tags=None, on_stop=None):
    path = os.path.join(AUDIO_DIR or os.path.join(HERE, "audio"), track)
    rec = {
        "t": stamp(), "cycle": cycle, "run": run_index, "engine": engine.index,
        **(tags or {}),
        "engine_uptime_s": round(now() - engine.launched, 1),
        "track": track, "audio_s": duration, "outcome": "ok", "error": None,
        "mem_before": process_memory_mb(engine.proc.pid), "sys_before": system_memory_mb(),
    }
    levels = 0
    interrupted = None
    note_first = note_ready = patient_first = patient_ready = None
    note_text = patient_text = ""
    note_failed = patient_failed = None
    note_refused = False

    def drain(until, stop_on=None):
        # Consume notifications until wall time `until` or a named method
        nonlocal levels, interrupted, note_first, note_ready, patient_first
        nonlocal patient_ready, note_text, patient_text, note_failed, patient_failed, note_refused
        while True:
            remaining = until - now()
            if remaining <= 0:
                return None
            item = engine.next_notification(min(remaining, 1.0))
            if item is None:
                continue
            t, msg = item
            method = msg.get("method")
            params = msg.get("params", {}) or {}
            if method == "audio.level":
                levels += 1
            elif method == "session/interrupted":
                interrupted = params
            elif method == "note/partial":
                note_first = note_first or t
                note_text = params.get("text", note_text)
            elif method == "note/ready":
                note_first = note_first or t
                note_ready = t
                note_text = params.get("text", note_text)
            elif method == "note/failed":
                note_failed = params.get("detail", "?")
                note_ready = t
            elif method == "note/refused":
                note_failed = "refused: " + params.get("reason", "?")
                note_refused = True
                note_ready = t
            elif method == "patient/partial":
                patient_first = patient_first or t
                patient_text = params.get("text", patient_text)
            elif method == "patient/ready":
                patient_first = patient_first or t
                patient_ready = t
                patient_text = params.get("text", patient_text)
            elif method == "patient/failed":
                patient_failed = params.get("detail", "?")
                patient_ready = t
            if stop_on and method in stop_on:
                return method

    try:
        t_start = now()
        result, rtt = timed(
            engine, "session/start", {"replay": {"path": path, "speed": SPEED, "monitor": False}}, 30)
        session_id = result.get("sessionId")
        rec["session_id"] = session_id
        rec["start_rtt_s"] = round(rtt, 3)
        rec["replay_speed"] = SPEED
        # Real time: the source completes on its own at the end of the file
        drain(t_start + duration / SPEED + 1.5)
        rec["mem_live_end"] = process_memory_mb(engine.proc.pid)
        if interrupted:
            rec["outcome"] = "interrupted"
            rec["error"] = json.dumps(interrupted)
        # Stop blocks through finalise: its round trip is the finalise time
        t_stop = now()
        stop_probe = on_stop() if on_stop else None  # e.g. a clock sampler
        _, stop_rtt = timed(engine, "session/stop", None, STOP_TIMEOUT)
        if stop_probe:
            rec["stop_probe"] = stop_probe()
        rec["stop_at_s"] = round(t_stop - t_start, 1)
        rec["finalise_s"] = round(stop_rtt, 2)
        # Note lane
        reached = drain(t_stop + NOTE_TIMEOUT, stop_on={"note/ready", "note/failed", "note/refused"})
        if reached is None:
            rec["outcome"] = "note_timeout"
        elif note_refused:
            rec["outcome"] = "note_refused"
        else:
            reached = drain(t_stop + NOTE_TIMEOUT, stop_on={"patient/ready", "patient/failed"})
            if reached is None:
                rec["outcome"] = "patient_timeout"
        t_end = now()
        rec["note_first_token_s"] = round(note_first - t_stop - stop_rtt, 2) if note_first else None
        rec["note_done_s"] = round(note_ready - t_stop - stop_rtt, 2) if note_ready else None
        rec["patient_first_token_s"] = (
            round(patient_first - note_ready, 2) if patient_first and note_ready else None)
        rec["patient_done_s"] = round(patient_ready - note_ready, 2) if patient_ready and note_ready else None
        rec["stop_to_all_done_s"] = round(t_end - t_stop, 2)
        rec["note_chars"] = len(note_text)
        rec["patient_chars"] = len(patient_text)
        rec["note_failed"] = note_failed
        rec["patient_failed"] = patient_failed
        if (note_failed or patient_failed) and not note_refused:
            rec["outcome"] = "note_failed" if note_failed else "patient_failed"
        # Engine-side numbers for this session (Take() resets them)
        try:
            metrics = engine.request("engine/metrics", None, 15)
            rec["metrics"] = {k: metrics.get(k) for k in (
                "stageSeconds", "loadSeconds", "asrRealtimeFactor", "audioSeconds",
                "lostFrames", "diarTicks", "turns", "clusters", "replaySpeed")}
        except Exception as e:
            rec["metrics_error"] = str(e)
        rec["mem_after"] = process_memory_mb(engine.proc.pid)
        rec["sys_after"] = system_memory_mb()
        rec["note_hosts"] = len(note_host_pids())
        # For reading before/after: the sealed transcript, note and sheet
        if SAVE_DIR:
            try:
                os.makedirs(SAVE_DIR, exist_ok=True)
                sealed = engine.request("session/transcript", {"id": session_id}, 30)
                stored_note = engine.request("session/note", {"id": session_id}, 30)
                stored_patient = engine.request("session/patient", {"id": session_id}, 30)
                base = saved_base(track)
                with open(base + ".json", "w", encoding="utf-8") as f:
                    json.dump({"turns": sealed.get("turns", []), "note": stored_note.get("text"),
                               "patient": stored_patient.get("text")}, f, indent=1)
                with open(base + ".txt", "w", encoding="utf-8") as f:
                    for t in sealed.get("turns", []):
                        f.write(f"[{t['firstFrame'] / 16000:7.1f}s] {t['speaker'] or '?':8s} {t['text']}\n")
                    f.write("\n=== NOTE ===\n" + (stored_note.get("text") or "") + "\n")
                    f.write("\n=== PATIENT ===\n" + (stored_patient.get("text") or "") + "\n")
                rec["saved"] = base
            except Exception as e:
                rec["save_error"] = str(e)
        # Keep the store small; delete exercises the path too
        try:
            engine.request("session/delete", {"id": session_id}, 30)
        except Exception as e:
            rec["delete_error"] = str(e)
    except EngineDied as e:
        rec["outcome"] = "engine_died"
        rec["error"] = str(e)
        rec["exit_code"] = engine.exit_code()
    except TimeoutError as e:
        rec["outcome"] = "rpc_timeout"
        rec["error"] = str(e)
    except Exception as e:
        rec["outcome"] = "error"
        rec["error"] = f"{type(e).__name__}: {e}"

    rec["levels"] = levels
    # The engine's own finalise stage lines for this run
    stages = {}
    extra = []
    for line in engine.new_log_lines():
        if "finalise " in line and " at " in line:
            try:
                name = line.split("finalise ", 1)[1].rsplit(" at ", 1)[0]
                stages[name] = float(line.rsplit(" at ", 1)[1].rstrip(" s"))
            except ValueError:
                pass
        elif "clinicavt-engine:" in line and ("ready in" not in line):
            extra.append(line.split("clinicavt-engine: ", 1)[-1][:160])
    rec["finalise_stages"] = stages
    rec["engine_log"] = extra[-12:]
    with open(RESULTS, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")
    log(f"  {track:24s} {rec['outcome']:12s} finalise {rec.get('finalise_s')} s  "
        f"note {rec.get('note_first_token_s')}/{rec.get('note_done_s')} s  "
        f"sheet {rec.get('patient_done_s')} s  "
        f"ws {rec.get('mem_after', {}) and rec['mem_after'].get('ws_mb')} MB")
    return rec["outcome"]


# --- Loop --------------------------------------------------------------------

def start_engine(index):
    for attempt in range(5):
        try:
            return _start_engine(index + attempt * 1000)
        except (EngineDied, TimeoutError, RuntimeError) as e:
            event("engine_start_failed", index=index, attempt=attempt, error=str(e))
            log(f"engine start failed ({e}); retrying")
            time.sleep(10)
    raise RuntimeError("engine would not start")


def _start_engine(index):
    t0 = now()
    engine = Engine(index)
    engine.wait_up()
    echo_at = now() - t0
    # PERF_NOTE_TIER: the note model role to run, as the shell would configure it on connect
    tier = os.environ.get("PERF_NOTE_TIER")
    if tier:
        result = engine.request("note/tier", {"tier": tier}, 30)
        event("note_tier", index=index, tier=tier, state=result.get("state"), model=result.get("id"))
        log(f"engine {index}: note tier {tier} -> {result.get('id')} ({result.get('state')})")
    # Readiness: the compile caches; loads still proceed in the background
    ready_at = None
    for _ in range(1200):
        result = engine.request("engine/readiness", None, 10)
        if result.get("ready"):
            ready_at = now() - t0
            break
        time.sleep(0.5)
    event("engine_started", index=index, pid=engine.proc.pid, echo_s=round(echo_at, 2),
          ready_s=round(ready_at, 2) if ready_at else None, mem=process_memory_mb(engine.proc.pid))
    log(f"engine {index} pid {engine.proc.pid}: echo {echo_at:.2f} s, ready {ready_at and round(ready_at, 1)} s")
    return engine


def main():
    max_hours = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
    only = sys.argv[2] if len(sys.argv) > 2 else None  # e.g. c02m to smoke-test
    os.makedirs(STORE, exist_ok=True)
    os.makedirs(LOGS, exist_ok=True)
    tracks = [t for t in TRACKS if only is None or t[0].startswith(only)]
    deadline = now() + max_hours * 3600
    event("loop_started", max_hours=max_hours, tracks=[t[0] for t in tracks])
    engine_index = 0
    engine = None
    cycle = 0
    run_index = 0
    crashes = 0
    try:
        max_cycles = int(os.environ.get("PERF_CYCLES", "0"))  # 0: until the deadline
        while now() < deadline and not os.path.exists(STOP_FILE) \
                and (max_cycles == 0 or cycle < max_cycles):
            cycle += 1
            # A fresh engine per cycle: cold start is measured every cycle and a
            # leaking engine cannot poison the whole night
            if engine is not None:
                engine.close()
            engine_index += 1
            engine = start_engine(engine_index)
            # No separate warm-up: as in the app, the note model loads during the first capture,
            # so each cycle's first run is cold
            log(f"cycle {cycle}")
            for track, duration in tracks:
                while os.path.exists(PAUSE_FILE) and not os.path.exists(STOP_FILE):
                    time.sleep(5)
                if now() >= deadline or os.path.exists(STOP_FILE):
                    break
                if SKIP_DONE and SAVE_DIR and os.path.exists(saved_base(track) + ".json"):
                    log(f"  {track:<24} skipped (transcript saved)")
                    continue
                run_index += 1
                outcome = run_session(engine, track, duration, cycle, run_index)
                if outcome == "engine_died" or engine.exit_code() is not None:
                    crashes += 1
                    code = engine.exit_code()
                    event("engine_died", index=engine.index, exit_code=code,
                          uptime_s=round(now() - engine.launched, 1), run=run_index, track=track,
                          log_tail=engine.new_log_lines()[-15:])
                    log(f"ENGINE DIED exit {code} (crash #{crashes}); relaunching")
                    engine.close()
                    engine_index += 1
                    engine = start_engine(engine_index)
                time.sleep(3)
    finally:
        if engine is not None:
            engine.close()
        event("loop_finished", cycles=cycle, runs=run_index, crashes=crashes)
        log(f"done: {cycle} cycles, {run_index} runs, {crashes} engine deaths")


if __name__ == "__main__":
    main()
