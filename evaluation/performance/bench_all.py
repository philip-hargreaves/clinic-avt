"""Whole-system benchmark at 1x. Covers every note tier with speech recognition on the GPU and on
the NPU, each step after Stop, translation, memory, CPU, GPU and NPU load, and package power from
the processor's energy counters. Baselines are the machine idle and each arm with its models loaded.

    python evaluation/performance/bench_all.py                 # full matrix, about 3 h
    python evaluation/performance/bench_all.py --quick         # one short track per arm, smoke test
    python evaluation/performance/bench_all.py --arms 9b-gpu   # chosen arms only
    python evaluation/performance/bench_report.py <run dir>    # -> report.md and summary.json

Results go to build/perf-loop/bench/<machine>-<time>/. Leave the machine alone while it runs and note
whether it was on mains or battery. On battery the report adds whole-system drain.
"""
import argparse
import csv
import ctypes
import ctypes.wintypes
import json
import os
import platform
import shutil
import subprocess
import sys
import threading
import time
import wave
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
from common import config  # noqa: E402

ARMS = {
    "9b-gpu": ("default", "GPU"),
    "4b-gpu": ("constrained", "GPU"),
    "35b-gpu": ("accuracy", "GPU"),
    "9b-npu": ("default", "NPU"),
    "4b-npu": ("constrained", "NPU"),
    "35b-npu": ("accuracy", "NPU"),
}
# The default arm runs again at the end to show drift from heat or a busier machine
ORDER = ["9b-gpu", "4b-gpu", "35b-gpu", "9b-npu", "4b-npu", "35b-npu", "9b-gpu"]
TRACKS = ["day3_consultation08_mixed.wav", "day1_consultation01_mixed.wav",
          "day2_consultation02_mixed.wav"]
LANGUAGES = ["Polish", "Urdu"]
COUNTERS = [
    r"\Energy Meter(RAPL_Package0_PKG)\Power",
    r"\Energy Meter(RAPL_Package0_PP0)\Power",
    r"\Energy Meter(RAPL_Package0_PP1)\Power",
    r"\Energy Meter(RAPL_Package0_DRAM)\Power",
    r"\Power Meter(*)\Power",
    r"\Processor Information(_Total)\% Processor Utility",
    r"\Processor Information(_Total)\% Processor Performance",
    r"\Processor Information(_Total)\Processor Frequency",
    r"\Thermal Zone Information(*)\Temperature",
    r"\Memory\Available MBytes",
    r"\Memory\Committed Bytes",
    r"\GPU Engine(*)\Utilization Percentage",
    r"\GPU Adapter Memory(*)\Shared Usage",
    r"\GPU Adapter Memory(*)\Dedicated Usage",
]
SYSTEM_IDLE_S = 300
COOLDOWN_S = 300
LOADED_IDLE_S = 120
WARMUP_S = 60


def log(line):
    print(f"{datetime.now().strftime('%H:%M:%S')} {line}", flush=True)


# --- Windows power mode (Settings > System > Power), set per arm and restored at the end -----

POWER_MODES = {"efficiency": "961cc777-2547-4f9d-8174-7d86181b8a7a",
               "balanced": "00000000-0000-0000-0000-000000000000",
               "performance": "ded574b5-45a0-4f42-8737-46345c09c238"}


class _Guid(ctypes.Structure):
    _fields_ = [("b", ctypes.c_byte * 16)]


def power_mode():
    import uuid
    out = _Guid()
    ctypes.WinDLL("powrprof.dll").PowerGetEffectiveOverlayScheme(ctypes.byref(out))
    guid = str(uuid.UUID(bytes_le=bytes(out.b)))
    return next((k for k, v in POWER_MODES.items() if v == guid), guid)


def set_power_mode(mode):
    import uuid
    guid = _Guid()
    ctypes.memmove(guid.b, uuid.UUID(POWER_MODES.get(mode, mode)).bytes_le, 16)
    fn = ctypes.WinDLL("powrprof.dll").PowerSetActiveOverlayScheme
    fn.argtypes = [_Guid]
    fn(guid)
    return power_mode()


# --- Keep the machine awake ------------------------------------------------------

def stay_awake(on):
    ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
    ctypes.windll.kernel32.SetThreadExecutionState(
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED if on else ES_CONTINUOUS)


# --- System counters through typeperf ------------------------------------------------

def usable_counters():
    keep = []
    for counter in COUNTERS:
        r = subprocess.run(["typeperf", counter, "-sc", "1"], capture_output=True, text=True)
        if r.returncode == 0 and "Error" not in r.stdout:
            keep.append(counter)
    return keep


class Typeperf:
    # typeperf expands its wildcards once, at start, so it restarts into a new part whenever a
    # ClinicAVT process appears (a note model that loads with the first consultation)
    def __init__(self, counters, out_csv):
        self.counters, self.base = counters, Path(out_csv)
        self.parts = []
        self.running = True
        self._start()
        self.pids = set(process_pids(Sampler.NAMES))
        self.watch = threading.Thread(target=self._watch, daemon=True)
        self.watch.start()

    def _start(self):
        path = self.base if not self.parts else self.base.with_name(f"{self.base.stem}-p{len(self.parts) + 1}.csv")
        self.parts.append(path.name)
        self.proc = subprocess.Popen(
            ["typeperf", *self.counters, "-si", "1", "-f", "CSV", "-o", str(path), "-y"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def _end(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            pass

    def _watch(self):
        due = []
        while self.running:
            time.sleep(2)
            now_ = time.time()
            pids = set(process_pids(Sampler.NAMES))
            if pids - self.pids:
                due = [now_ + d for d in (0, 20, 60, 120)]
            self.pids = pids
            if self.running and due and due[0] <= now_:
                due = [d for d in due if d > now_]
                self._end()
                self._start()

    def stop(self):
        self.running = False
        self.watch.join(5)
        self._end()


# --- Per-process CPU and memory, and the battery ------------------------------------

class FileTime(ctypes.Structure):
    _fields_ = [("low", ctypes.wintypes.DWORD), ("high", ctypes.wintypes.DWORD)]


class PowerStatus(ctypes.Structure):
    _fields_ = [("ac", ctypes.c_byte), ("flag", ctypes.c_byte), ("percent", ctypes.c_byte),
                ("saver", ctypes.c_byte), ("life", ctypes.wintypes.DWORD),
                ("full", ctypes.wintypes.DWORD)]


def cpu_seconds(pid):
    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        c, e, k, u = FileTime(), FileTime(), FileTime(), FileTime()
        if not ctypes.windll.kernel32.GetProcessTimes(handle, ctypes.byref(c), ctypes.byref(e),
                                                      ctypes.byref(k), ctypes.byref(u)):
            return None
        return ((k.high << 32 | k.low) + (u.high << 32 | u.low)) / 1e7
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def process_pids(names):
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    pids = {}
    for row in csv.reader(out.splitlines()):
        if len(row) > 1 and row[0].lower() in names:
            pids[int(row[1])] = row[0].lower()
    return pids


def battery_wmi():
    script = ("Get-CimInstance -Namespace root\\wmi -ClassName BatteryStatus | "
              "Select-Object PowerOnline,Discharging,DischargeRate,RemainingCapacity | ConvertTo-Json")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                           capture_output=True, text=True, timeout=20)
        data = json.loads(r.stdout) if r.stdout.strip() else None
        return data[0] if isinstance(data, list) else data
    except Exception:
        return None


def network(pids):
    if not pids:
        return {"tcp": [], "udp": []}
    ids = ",".join(str(p) for p in pids)
    script = (f"$p=@({ids}); "
              "$t=@(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $p -contains $_.OwningProcess } "
              "| ForEach-Object { \"$($_.OwningProcess) $($_.LocalAddress):$($_.LocalPort)->$($_.RemoteAddress):$($_.RemotePort) $($_.State)\" }); "
              "$u=@(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { $p -contains $_.OwningProcess } "
              "| ForEach-Object { \"$($_.OwningProcess) $($_.LocalAddress):$($_.LocalPort)\" }); "
              "@{tcp=$t; udp=$u} | ConvertTo-Json -Compress")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                           capture_output=True, text=True, timeout=30)
        data = json.loads(r.stdout) if r.stdout.strip() else {}
        return {"tcp": data.get("tcp") or [], "udp": data.get("udp") or []}
    except Exception as e:
        return {"error": str(e)}


class Sampler(threading.Thread):
    NAMES = {"clinicavt_engine.exe", "clinicavt_note_host.exe", "clinicavt_ingest_host.exe"}

    def __init__(self, out_path):
        super().__init__(daemon=True)
        self.out = open(out_path, "a", encoding="utf-8")
        self.running = True
        self.cpus = os.cpu_count() or 1

    def run(self):
        import perf_loop
        last = {}
        pids = {}
        tick = 0
        while self.running:
            t = time.time()
            if tick % 5 == 0:
                pids = process_pids(self.NAMES)
            row = {"t": round(t, 3), "procs": {}}
            for pid, name in pids.items():
                cpu = cpu_seconds(pid)
                mem = perf_loop.process_memory_mb(pid)
                if cpu is None or mem is None:
                    continue
                share = None
                if pid in last:
                    dt = t - last[pid][0]
                    share = round(100 * (cpu - last[pid][1]) / dt / self.cpus, 2) if dt > 0 else None
                last[pid] = (t, cpu)
                row["procs"][f"{name}:{pid}"] = {"cpu_pct": share, **mem}
            status = PowerStatus()
            ctypes.windll.kernel32.GetSystemPowerStatus(ctypes.byref(status))
            row["ac"] = status.ac
            row["battery_pct"] = status.percent
            if tick % 30 == 0:
                row["battery"] = battery_wmi()
            if tick % 30 == 15:
                row["network"] = network(list(pids))
            row["sys"] = perf_loop.system_memory_mb()
            self.out.write(json.dumps(row) + "\n")
            self.out.flush()
            tick += 1
            time.sleep(max(0.0, 1.0 - (time.time() - t)))

    def stop(self):
        self.running = False
        self.join(5)
        self.out.close()


# --- Machine description ---------------------------------------------------------------

def provenance():
    import hashlib

    def git(*a):
        # Without git on PATH the provenance has no commit
        try:
            r = subprocess.run(["git", "-C", str(config.REPO), *a], capture_output=True, text=True)
        except OSError:
            return None
        return r.stdout.strip()

    def sha256(path):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()

    models = config.path("app_models")
    manifests = {}
    for m in sorted(models.glob("*/manifest.json")):
        try:
            data = json.loads(m.read_text(encoding="utf-8"))
            manifests[m.parent.name] = {k: data.get(k) for k in ("id", "version", "source", "precision") if k in data}
        except Exception:
            manifests[m.parent.name] = {}
    engine = config.path("engine")
    return {
        "commit": git("rev-parse", "HEAD"), "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "uncommitted_files": len([l for l in (git("status", "--porcelain") or "").splitlines() if l.strip()]),
        "engine_sha256": sha256(engine) if engine.exists() else None,
        "engine_built": datetime.fromtimestamp(engine.stat().st_mtime).isoformat(timespec="seconds")
        if engine.exists() else None,
        "models": manifests,
        "python": sys.version.split()[0],
    }


def machine():
    def ps(cmd):
        r = subprocess.run(["powershell", "-NoProfile", "-Command", cmd],
                           capture_output=True, text=True, timeout=60)
        return r.stdout.strip()
    return {
        "host": platform.node(),
        "os": platform.platform(),
        "cpu": ps("(Get-CimInstance Win32_Processor).Name"),
        "logical_cpus": os.cpu_count(),
        "ram_gb": ps("[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,1)"),
        "model": ps("(Get-CimInstance Win32_ComputerSystem).Model"),
        "gpus": ps("(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name + ' ' + "
                   "$_.DriverVersion }) -join '; '"),
        "npu": ps("(Get-PnpDevice -PresentOnly | Where-Object { $_.FriendlyName -match "
                  "'AI Boost|NPU' } | Select-Object -First 1).FriendlyName"),
        "memory_mts": ps("(Get-CimInstance Win32_PhysicalMemory | Measure-Object ConfiguredClockSpeed "
                         "-Maximum).Maximum"),
        "memory_bus_bits": ps("(Get-CimInstance Win32_PhysicalMemory | Measure-Object DataWidth -Sum).Sum"),
        "power_plan": ps("powercfg /getactivescheme"),
        "battery": battery_wmi(),
        "battery_full_mwh": ps("(Get-CimInstance -Namespace root\\wmi -ClassName "
                               "BatteryFullChargedCapacity).FullChargedCapacity"),
        "battery_design_mwh": ps("(Get-CimInstance Win32_Battery).DesignCapacity"),
    }


# --- The benchmark ------------------------------------------------------------------------

class Bench:
    def __init__(self, out, counters, tracks):
        self.out = out
        self.counters = counters
        self.tracks = tracks
        self.windows = out / "windows.jsonl"
        self.translations = out / "translations.jsonl"
        self.loads = out / "loads.jsonl"
        done = [json.loads(line) for line in self.windows.read_text(encoding="utf-8").splitlines()
                if line.strip()] if self.windows.exists() else []
        self.index = max((w["index"] for w in done), default=0)
        self.passes = {}
        for w in done:
            if w.get("arm"):
                self.passes[w["arm"]] = max(self.passes.get(w["arm"], 0), w.get("pass_no", 0))

    def window(self, kind, seconds=None, body=None, **tags):
        self.index += 1
        csv_path = self.out / "counters" / f"{self.index:03d}-{kind}.csv"
        perf = Typeperf(self.counters, csv_path)
        time.sleep(3)
        t0 = time.time()
        result = body() if body else time.sleep(seconds)
        t1 = time.time()
        time.sleep(2)
        perf.stop()
        record = {"index": self.index, "kind": kind, "t0": t0, "t1": t1, "power_mode": power_mode(),
                  "csv": csv_path.name, "csv_parts": perf.parts, **tags}
        if isinstance(result, dict):
            record.update(result)
        with open(self.windows, "a", encoding="utf-8") as f:
            f.write(json.dumps(record) + "\n")
        return result

    def start_arm(self, arm, index):
        import perf_loop
        tier, device = ARMS[arm.split("@")[0]]
        # Empty guidelines folder so document indexing adds no load
        empty = self.out / "guidelines"
        empty.mkdir(exist_ok=True)
        perf_loop.ENGINE_ARGS[:] = (["--asr-device", device] if device != "GPU" else []) + \
            ["--guidelines", str(empty)]
        t0 = time.time()
        engine = perf_loop.Engine(index)
        engine.wait_up()
        echo_s = time.time() - t0
        tier_reply = engine.request("note/tier", {"tier": tier}, 30)
        ready_s = None
        for _ in range(2400):
            if engine.request("engine/readiness", None, 10).get("ready"):
                ready_s = time.time() - t0
                break
            time.sleep(0.5)
        # The note model may load now or with the first consultation, so the wait is bounded
        note_load = None
        asr_state = None
        # "idle" is the machine default, which loads with the first consultation as in the app
        if (tier_reply or {}).get("state") not in ("ready", "idle"):
            deadline = time.perf_counter() + (600 if tier == "accuracy" else 240)
            while time.perf_counter() < deadline:
                item = engine.next_notification(1.0)
                if item is None:
                    continue
                msg = item[1]
                if msg.get("method") == "asr/device":
                    asr_state = msg.get("params")
                if msg.get("method") == "note/model" and \
                        msg.get("params", {}).get("state") in ("ready", "failed"):
                    note_load = msg.get("params")
                    break
        record = {"arm": arm, "tier": tier, "asr_device": device, "echo_s": round(echo_s, 2),
                  "engine_ready_s": ready_s and round(ready_s, 1), "tier_reply": tier_reply,
                  "note_model": note_load, "note_loaded_after_s": round(time.time() - t0, 1),
                  "asr": asr_state, "t": time.time()}
        try:
            record["metrics"] = engine.request("engine/metrics", None, 15)
        except Exception as e:
            record["metrics_error"] = str(e)
        with open(self.loads, "a", encoding="utf-8") as f:
            f.write(json.dumps(record) + "\n")
        log(f"{arm}: ready {record['engine_ready_s']} s, note model "
            f"{(note_load or tier_reply or {}).get('state')} in {note_load and note_load.get('seconds')} s")
        return engine

    def translate_before_delete(self, engine, arm, track):
        # Translation needs the stored sheet, so it is requested right before the run deletes it
        original = engine.request
        bench = self

        def request(method, params=None, timeout=30.0):
            if method == "session/delete" and params:
                for language in LANGUAGES:
                    row = {"arm": arm, "track": track, "language": language,
                           "session": params.get("id"), "t0": time.time()}
                    t = time.perf_counter()
                    try:
                        original("patient/translate", {"id": params["id"], "language": language}, 30)
                        first = None
                        while time.perf_counter() - t < 300:
                            item = engine.next_notification(1.0)
                            if item is None:
                                continue
                            m = item[1].get("method")
                            if m == "translate/partial" and first is None:
                                first = item[0] - t
                            if m in ("translate/ready", "translate/failed"):
                                row["outcome"] = m.split("/")[1]
                                row["seconds"] = round(item[0] - t, 2)
                                row["first_s"] = round(first, 2) if first is not None else None
                                p = item[1].get("params", {})
                                row["tokens_per_s"] = p.get("tokensPerSecond")
                                row["chars"] = len(p.get("text") or "")
                                break
                    except Exception as e:
                        row["outcome"] = "error"
                        row["error"] = str(e)
                    row["t1"] = time.time()
                    with open(bench.translations, "a", encoding="utf-8") as f:
                        f.write(json.dumps(row) + "\n")
            return original(method, params, timeout)

        engine.request = request
        return lambda: setattr(engine, "request", original)

    def sniff_rates(self, engine):
        # Decode rates come on the note and sheet ready notifications. Wrap once per engine
        if not hasattr(engine, "rates"):
            original = engine.next_notification
            engine.rates = {}

            def next_notification(timeout):
                item = original(timeout)
                if item is not None:
                    method = item[1].get("method")
                    if method in ("note/ready", "patient/ready"):
                        key = method.split("/")[0] + "_tokens_per_s"
                        engine.rates[key] = item[1].get("params", {}).get("tokensPerSecond")
                return item

            engine.next_notification = next_notification
        engine.rates.clear()
        return engine.rates

    def switches(self):
        import perf_loop
        rows = []

        def note_host_mb():
            return sum((perf_loop.process_memory_mb(p) or {}).get("private_mb", 0)
                       for p in perf_loop.note_host_pids())

        def wait(method, done, timeout):
            deadline = time.perf_counter() + timeout
            while time.perf_counter() < deadline:
                item = engine.next_notification(1.0)
                if item and item[1].get("method") == method and done(item[1].get("params", {})):
                    return item[1].get("params", {})
            return None

        empty = self.out / "guidelines"
        empty.mkdir(exist_ok=True)
        perf_loop.ENGINE_ARGS[:] = ["--guidelines", str(empty)]
        sys_before = perf_loop.system_memory_mb()
        t0 = time.time()
        engine = perf_loop.Engine(self.index + 900)
        engine.wait_up()
        rows.append({"step": "engine start (echo)", "seconds": round(time.time() - t0, 2)})
        for tier in ("constrained", "accuracy", "default"):
            before = note_host_mb()
            t = time.time()
            reply = engine.request("note/tier", {"tier": tier}, 30)
            params = reply if (reply or {}).get("state") in ("ready", "idle") else wait(
                "note/model", lambda p, tier=tier: p.get("tier") == tier and p.get("state") in ("ready", "failed"),
                900)
            time.sleep(5)
            rows.append({"step": f"note tier to {tier}", "seconds": round(time.time() - t - 5, 2),
                         "model_seconds": (params or {}).get("seconds"), "state": (params or {}).get("state"),
                         "note_host_mb_before": before, "note_host_mb_after": note_host_mb(),
                         "engine_mb": (perf_loop.process_memory_mb(engine.proc.pid) or {}).get("private_mb")})
            log(f"switch: {rows[-1]}")
        for device in ("NPU", "GPU"):
            before = (perf_loop.process_memory_mb(engine.proc.pid) or {}).get("private_mb")
            t = time.time()
            engine.request("asr/device", {"device": device}, 30)
            params = wait("asr/device", lambda p, d=device: p.get("device") == d and p.get("state") in ("ready", "failed"), 900)
            rows.append({"step": f"speech recognition to {device}", "seconds": round(time.time() - t, 2),
                         "state": (params or {}).get("state"), "engine_mb_before": before,
                         "engine_mb_after": (perf_loop.process_memory_mb(engine.proc.pid) or {}).get("private_mb")})
            log(f"switch: {rows[-1]}")
        loaded = perf_loop.system_memory_mb()
        t = time.time()
        engine.close()
        rows.append({"step": "engine shutdown", "seconds": round(time.time() - t, 2)})
        time.sleep(10)
        after = perf_loop.system_memory_mb()
        rows.append({"step": "memory given back (MB)", "freed_mb": after["avail_phys_mb"] - loaded["avail_phys_mb"],
                     "before_start_mb": sys_before["avail_phys_mb"], "loaded_mb": loaded["avail_phys_mb"],
                     "after_mb": after["avail_phys_mb"]})
        with open(self.out / "switches.jsonl", "a", encoding="utf-8") as f:
            for row in rows:
                f.write(json.dumps(row) + "\n")

    def imports(self, device):
        import perf_loop
        empty = self.out / "guidelines"
        empty.mkdir(exist_ok=True)
        perf_loop.ENGINE_ARGS[:] = (["--asr-device", device] if device != "GPU" else []) + \
            ["--guidelines", str(empty)]
        t0 = time.time()
        engine = perf_loop.Engine(self.index + 800)
        engine.wait_up()
        for _ in range(2400):
            if engine.request("engine/readiness", None, 10).get("ready"):
                break
            time.sleep(0.5)
        log(f"imports on {device}: engine ready in {time.time() - t0:.1f} s")
        try:
            for track, duration in self.tracks:
                path = str(Path(os.environ["PERF_AUDIO_DIR"]) / track)

                def body(path=path, duration=duration):
                    t = time.perf_counter()
                    sid = engine.request("session/import", {"path": path, "startedAt": "2026-09-01T09:00:00Z",
                                                            "retain": False}, 60)["sessionId"]
                    msg = engine.wait_for({"session/imported", "session/importFailed"}, 1800)
                    imported = time.perf_counter() - t
                    result = {"outcome": (msg or {}).get("method", "timeout"), "import_s": round(imported, 2),
                              "x_real_time": round(duration / imported, 2) if imported else None}
                    note_s = sheet_s = None
                    deadline = time.perf_counter() + 900
                    while time.perf_counter() < deadline and sheet_s is None and result["outcome"] == "session/imported":
                        m = engine.wait_for({"note/ready", "note/failed", "note/refused", "patient/ready",
                                             "patient/failed"}, deadline - time.perf_counter())
                        if m is None:
                            break
                        if m["method"].startswith("note/"):
                            note_s = round(time.perf_counter() - t - imported, 2)
                            if m["method"] != "note/ready":
                                break
                        else:
                            sheet_s = round(time.perf_counter() - t - imported, 2)
                    result.update({"note_after_s": note_s, "sheet_after_s": sheet_s,
                                   "all_s": round(time.perf_counter() - t, 2)})
                    try:
                        result["turns"] = len(engine.request("session/transcript", {"id": sid}, 30).get("turns", []))
                        engine.request("session/delete", {"id": sid}, 30)
                    except Exception as e:
                        result["error"] = str(e)
                    log(f"  import {track} on {device}: {result['import_s']} s "
                        f"({result['x_real_time']}x), note +{note_s} s, sheet +{sheet_s} s")
                    return result

                self.window("import", body=body, device=device, track=track, audio_s=duration)
                time.sleep(5)
        finally:
            engine.close()

    def run_arm(self, arm, pass_no):
        import perf_loop
        mode = arm.split("@")[1] if "@" in arm else self.default_mode
        log(f"power mode {set_power_mode(mode)}")
        perf_loop.saved_base = lambda track: os.path.join(
            perf_loop.SAVE_DIR, f"{arm}-{pass_no}-{track.rsplit('.', 1)[0]}")
        engine = self.start_arm(arm, self.index + 1)
        try:
            self.window("warmup", WARMUP_S, arm=arm, pass_no=pass_no)
            for track, duration in self.tracks:
                restore = self.translate_before_delete(engine, arm, track)

                def body(track=track, duration=duration):
                    rates = self.sniff_rates(engine)
                    t0 = time.time()
                    outcome = perf_loop.run_session(engine, track, duration, pass_no, self.index,
                                                    tags={"arm": arm, "pass_no": pass_no})
                    return {"outcome": outcome, "session_t0": t0, **dict(rates)}

                try:
                    self.window("consultation", body=body, arm=arm, pass_no=pass_no, track=track,
                                audio_s=duration)
                finally:
                    restore()
                if engine.exit_code() is not None:
                    log(f"{arm}: engine died; restarting")
                    engine.close()
                    engine = self.start_arm(arm, self.index + 1)
                time.sleep(5)
            self.window("settle", WARMUP_S, arm=arm, pass_no=pass_no)
            self.window("loaded-idle", LOADED_IDLE_S, arm=arm, pass_no=pass_no)
        finally:
            engine.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--arms", nargs="*", default=None)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--switches-only", action="store_true")
    parser.add_argument("--imports", action="store_true", help="time the upload path on the GPU and the NPU")
    parser.add_argument("--imports-only", action="store_true")
    parser.add_argument("--import-devices", nargs="+", default=["GPU", "NPU"])
    parser.add_argument("--power-mode", default="balanced", choices=list(POWER_MODES),
                        help="Windows power mode for arms without an @mode suffix")
    parser.add_argument("--skip-idle", action="store_true", help="no system idle windows (later parts of one run)")
    parser.add_argument("--light", action="store_true", help="no per-engine GPU counters, to measure monitoring overhead")
    parser.add_argument("--switches", action="store_true", help="time loading, switching and unloading at the end")
    parser.add_argument("--audio-dir", default=str(config.REPO / "demo"))
    parser.add_argument("--out", default=None)
    args = parser.parse_args()

    order = [] if (args.switches_only or args.imports_only) else (args.arms or ORDER)
    if args.imports_only:
        args.imports = True
    if args.switches_only:
        args.switches = True
    if args.quick:
        global LOADED_IDLE_S
        LOADED_IDLE_S = 30
    stamp = datetime.now().strftime("%Y%m%d-%H%M")
    out = Path(args.out) if args.out else config.path("perf_loop") / "bench" / f"{platform.node()}-{stamp}"
    (out / "counters").mkdir(parents=True, exist_ok=True)

    # perf_loop reads these when imported
    os.environ["PERF_AUDIO_DIR"] = str(out / "audio")
    os.environ["PERF_TAG"] = "-bench"
    os.environ.setdefault("PERF_SPEED", "1")
    (out / "audio").mkdir(exist_ok=True)
    chosen = TRACKS[:1] if args.quick else TRACKS
    tracks = []
    for name in chosen:
        src = Path(args.audio_dir) / name
        shutil.copy2(src, out / "audio" / name)
        with wave.open(str(src)) as w:
            tracks.append((name, w.getnframes() / w.getframerate()))
    import perf_loop
    perf_loop.HERE = str(out)
    perf_loop.STORE = str(out / "store")
    perf_loop.LOGS = str(out / "logs")
    perf_loop.RESULTS = str(out / "runs.jsonl")
    perf_loop.EVENTS = str(out / "events.jsonl")
    perf_loop.SAVE_DIR = str(out / "transcripts")
    os.makedirs(perf_loop.STORE, exist_ok=True)
    os.makedirs(perf_loop.LOGS, exist_ok=True)

    counters = usable_counters()
    if args.light:
        counters = [c for c in counters if "GPU Engine" not in c]
    info = machine()
    info["provenance"] = provenance()
    # LibreHardwareMonitor, run as administrator, logs temperatures, the iGPU clock and platform power
    lhm = Path(os.environ.get("BENCH_LHM_DIR", "lhm"))
    info["lhm_dir"] = str(lhm) if any(lhm.glob("LibreHardwareMonitorLog-*.csv")) else None
    info["lhm_running"] = "librehardwaremonitor.exe" in set(process_pids({"librehardwaremonitor.exe"}).values())
    info.update({"started": datetime.now().isoformat(timespec="seconds"), "order": order,
                 "tracks": tracks, "languages": LANGUAGES, "counters": counters,
                 "engine": str(config.path("engine")), "models": str(config.path("app_models"))})
    if (out / "machine.json").exists():
        first = json.loads((out / "machine.json").read_text(encoding="utf-8"))
        info["started"] = first.get("started", info["started"])
        info["order"] = first.get("order", []) + order
    (out / "machine.json").write_text(json.dumps(info, indent=1), encoding="utf-8")
    log(f"out {out}")
    log(f"{info['cpu']}, {info['ram_gb']} GB, on {'mains' if (info.get('battery') or {}).get('PowerOnline') else 'battery'}")
    log(f"counters: {len(counters)} of {len(COUNTERS)}")

    original_mode = power_mode()
    info["power_mode_before"] = original_mode
    (out / "machine.json").write_text(json.dumps(info, indent=1), encoding="utf-8")
    stay_awake(True)
    sampler = Sampler(out / "procs.jsonl")
    sampler.start()
    bench = Bench(out, counters, tracks)
    bench.default_mode = args.power_mode
    set_power_mode(args.power_mode)
    try:
        if not args.skip_idle:
            log("system idle")
            bench.window("system-idle", 60 if args.quick else SYSTEM_IDLE_S)
        seen = dict(bench.passes)

        def guarded(label, fn):
            # A step that fails (a model too big for the machine, an engine that will not restart) is
            # recorded and the run carries on with the next one
            try:
                fn()
            except Exception as e:
                log(f"{label} FAILED: {type(e).__name__}: {e}")
                with open(out / "failures.jsonl", "a", encoding="utf-8") as f:
                    f.write(json.dumps({"t": time.time(), "step": label, "error": f"{type(e).__name__}: {e}"}) + "\n")

        def arms(group):
            for arm in group:
                if (out / "STOP").exists():
                    return
                seen[arm] = seen.get(arm, 0) + 1
                if bench.index > 1:
                    log("cool-down")
                    bench.window("cooldown", 60 if args.quick else COOLDOWN_S)
                log(f"=== {arm} (pass {seen[arm]})")
                guarded(arm, lambda arm=arm: bench.run_arm(arm, seen[arm]))

        # The largest tier runs last so a machine too small for it still records every other step
        arms([a for a in order if not a.startswith("35b")])
        if args.imports and not (out / "STOP").exists():
            for device in args.import_devices:
                log(f"cool-down before imports on {device}")
                bench.window("cooldown", 60 if args.quick else COOLDOWN_S)
                guarded(f"imports {device}", lambda device=device: bench.imports(device))
        arms([a for a in order if a.startswith("35b")])
        if args.switches and not (out / "STOP").exists():
            log("loading, switching and unloading")
            guarded("switches", lambda: bench.window("switches", body=bench.switches))
        if not args.skip_idle:
            log("system idle, end")
            bench.window("system-idle-end", 60 if args.quick else SYSTEM_IDLE_S)
    finally:
        sampler.stop()
        stay_awake(False)
        log(f"power mode restored to {set_power_mode(original_mode)}")
        (out / "DONE").write_text(datetime.now().isoformat(timespec="seconds"), encoding="utf-8")
        log("done")


if __name__ == "__main__":
    main()
