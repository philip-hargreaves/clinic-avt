"""Report for a bench_all.py run. Covers load times, each step after Stop, translation, power and
energy per phase, GPU, NPU and CPU load, memory, NPU against GPU recognition, drift, and a battery
estimate.

    python evaluation/performance/bench_report.py build/perf-loop/bench/<run>   # -> report.md, summary.json
"""
import csv
import json
import statistics
import sys
import time
from collections import defaultdict
from datetime import datetime
from pathlib import Path

# Note model folders, and the share of weights each token reads (all for dense, 3 of 35 B for the MoE)
NOTE_MODELS = {"4b": ("qwen3.5-4b-int4", 1.0), "9b": ("qwen3.5-9b-int4", 1.0),
               "35b": ("qwen3.6-35b-a3b-int4", 3 / 35)}
SHIFT_CONSULTS = 30          # a GP day of ten-minute appointments
SHIFT_AUDIO_MIN = 10.0
SHIFT_HOURS = 8.0


def rows_of(path):
    return rows(path)


def rows(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def mean(values):
    values = [v for v in values if v is not None]
    return statistics.mean(values) if values else None


def fmt(v, digits=1):
    if v is None:
        return "–"
    if isinstance(v, (int, float)):
        return f"{v:,.{digits}f}"
    return str(v)


METHODS = """## Method

- **Workload.** Each arm is a note tier with speech recognition on the GPU or the NPU. The engine is the
  release build, started with replay allowed and an empty guidelines folder so document indexing adds
  no load; recordings are replayed at real time (1x) through the same path as the microphone. One
  engine serves all of an arm's consultations; the machine was left alone throughout.
- **Heat.** The Balanced power plan, as a clinician would run it. Each arm starts after a 5-minute
  cool-down, and the default arm is repeated at the end to show drift; a hot machine lowers GPU power.
- **Times.** From the engine's own notifications: *finalise* is the Stop request's round trip (the
  engine replies once the transcript is sealed); *first note words*, *note done* and *sheet done* are
  measured from Stop to the first and last note text and the last sheet text. Finalise stages come
  from the engine log. Tokens per second are the engine's own figures on note/ready and patient/ready.
- **Loading.** Note model load time is the engine's own figure. The machine's default tier loads with
  the first consultation, as in the app, so its load time comes from the switching test, which loads
  every tier in turn in one engine.
- **Power.** The processor package's RAPL energy counters, sampled each second by typeperf: package,
  CPU cores (PP0) and integrated GPU (PP1); the rest of the package (memory controller, NPU, media,
  fabric) is package minus the two. The screen, storage and the rest of the board are outside it, so
  figures are the processor's share and the energy above idle is what ClinicAVT adds.
- **Load.** Windows GPU engine counters each second, per adapter, taking the busiest engine type, as
  Task Manager does. The NPU is the adapter with only compute engines. Counter capture restarts when a
  ClinicAVT process starts so its engines are included.
- **Memory.** Each ClinicAVT process sampled each second: resident (working set) and committed
  (private bytes). Resident is what must fit in RAM; committed also counts what Windows can page out.
- **Accuracy.** Each run's sealed transcript is scored against the PriMock57 reference with the
  evaluation suite's transcription scorer (word error rate, inserted words, and the clinical lens).
- **Network.** Every 30 s, the TCP connections and UDP endpoints owned by ClinicAVT's processes.
- **Provenance.** The repository commit, uncommitted file count, engine binary SHA-256 and model
  manifests are in machine.json, with the raw counters, process samples and engine logs beside it.
"""


# --- Counters --------------------------------------------------------------------------

def parse_stamp(text, near):
    best = None
    for fmt_ in ("%m/%d/%Y %H:%M:%S.%f", "%d/%m/%Y %H:%M:%S.%f", "%m/%d/%Y %H:%M:%S", "%d/%m/%Y %H:%M:%S"):
        try:
            t = datetime.strptime(text, fmt_).timestamp()
        except ValueError:
            continue
        if best is None or (near is not None and abs(t - near) < abs(best - near)):
            best = t
    return best


def read_counters(path, near=None):
    # -> list of (epoch, {counter path: value}) with machine names stripped
    if not path.exists():
        return []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header:
            return []
        names = ["\\" + h.split("\\", 3)[-1].lower() if h.startswith("\\\\") else h for h in header]
        out = []
        for row in reader:
            if not row or len(row) < 2:
                continue
            t = parse_stamp(row[0], near)
            if t is None:
                continue
            values = {}
            for name, cell in zip(names[1:], row[1:]):
                try:
                    values[name] = float(cell)
                except ValueError:
                    pass
            out.append((t, values))
        return out


def classify_adapters(samples):
    # Maps luid to engine types seen. The NPU has only compute engines and the dGPU has
    # video-encode extras
    engines = defaultdict(set)
    shared = defaultdict(float)
    for _, values in samples:
        for name, v in values.items():
            if name.startswith("\\gpu engine("):
                inst = name[len("\\gpu engine("):].split(")")[0]
                if "_luid_" in inst and "engtype_" in inst:
                    luid = inst.split("_luid_")[1].split("_phys")[0]
                    engines[luid].add(inst.split("engtype_")[1])
            if name.startswith("\\gpu adapter memory(") and name.endswith("shared usage"):
                luid = name.split("luid_")[1].split("_phys")[0]
                shared[luid] = max(shared[luid], v)
    kinds = {}
    for luid, types in engines.items():
        lowered = {t.lower() for t in types}
        if lowered and lowered <= {"compute"}:
            kinds[luid] = "npu"
        elif lowered & {"ofa_0", "vr", "security", "videoencode"}:
            kinds[luid] = "dgpu"
    candidates = [l for l, t in engines.items() if "3d" in {x.lower() for x in t} and l not in kinds]
    if candidates:
        kinds[max(candidates, key=lambda l: shared.get(l, 0))] = "igpu"
    return kinds


def phase_stats(samples, a, b, kinds):
    picked = [v for t, v in samples if a <= t < b]
    if not picked:
        return None

    def col(key):
        return [v[key] for v in picked if key in v]

    pkg = col("\\energy meter(rapl_package0_pkg)\\power")
    pp0 = col("\\energy meter(rapl_package0_pp0)\\power")
    pp1 = col("\\energy meter(rapl_package0_pp1)\\power")
    meter = col("\\power meter(power meter (0))\\power")
    util = defaultdict(list)
    shared_igpu = []
    for v in picked:
        per = defaultdict(lambda: defaultdict(float))
        for name, value in v.items():
            if name.startswith("\\gpu engine(") and "_luid_" in name and "engtype_" in name:
                inst = name[len("\\gpu engine("):].split(")")[0]
                luid = inst.split("_luid_")[1].split("_phys")[0]
                per[luid][inst.split("engtype_")[1]] += value
            if name.startswith("\\gpu adapter memory(") and name.endswith("shared usage"):
                luid = name.split("luid_")[1].split("_phys")[0]
                if kinds.get(luid) == "igpu":
                    shared_igpu.append(value)
        for luid, kind in kinds.items():
            util[kind].append(min(100.0, max(per[luid].values(), default=0.0)))
    w = lambda xs: mean(xs) / 1000 if xs else None  # noqa: E731  (mW to W)
    seconds = b - a
    return {
        "seconds": round(seconds, 1),
        "pkg_w": w(pkg), "cores_w": w(pp0), "gfx_w": w(pp1),
        "rest_w": (w(pkg) - w(pp0) - w(pp1)) if pkg and pp0 and pp1 else None,
        "system_w": w(meter) if meter and max(meter) > 0 else None,
        "pkg_j": (w(pkg) * seconds) if pkg else None,
        "cpu_pct": mean(col("\\processor information(_total)\\% processor utility")),
        "clock_pct": mean(col("\\processor information(_total)\\% processor performance")),
        "igpu_pct": mean(util.get("igpu", [])), "npu_pct": mean(util.get("npu", [])),
        "dgpu_pct": mean(util.get("dgpu", [])),
        "avail_mb_min": min(col("\\memory\\available mbytes"), default=None),
        "commit_gb_max": (max(col("\\memory\\committed bytes")) / 2**30) if col("\\memory\\committed bytes") else None,
        "igpu_shared_gb_max": (max(shared_igpu) / 2**30) if shared_igpu else None,
    }


def proc_stats(procs, a, b):
    picked = [r for r in procs if a <= r["t"] < b]
    by_name = defaultdict(lambda: {"cpu": [], "private": [], "ws": []})
    all_ws, all_private = [], []
    for r in picked:
        totals = defaultdict(lambda: [0.0, 0.0, 0.0])
        for key, p in r["procs"].items():
            name = key.split(":")[0].replace("clinicavt_", "").replace(".exe", "")
            totals[name][0] += p.get("cpu_pct") or 0.0
            totals[name][1] += p.get("private_mb") or 0.0
            totals[name][2] += p.get("ws_mb") or 0.0
        for name, (cpu, private, ws) in totals.items():
            by_name[name]["cpu"].append(cpu)
            by_name[name]["private"].append(private)
            by_name[name]["ws"].append(ws)
        all_private.append(sum(t[1] for t in totals.values()))
        all_ws.append(sum(t[2] for t in totals.values()))
    out = {name: {"cpu_pct": mean(v["cpu"]), "private_mb_max": max(v["private"], default=None),
                  "ws_mb_max": max(v["ws"], default=None)} for name, v in by_name.items()}
    out["all"] = {"private_mb_max": max(all_private, default=None), "ws_mb_max": max(all_ws, default=None)}
    return out


# --- LibreHardwareMonitor ------------------------------------------------------------------

# Matched by sensor kind and name, since LHM numbers its sensors per chip
LHM_KEYS = {
    "pkg_c": lambda i, n: i.startswith("/intelcpu/") and "/temperature/" in i and n == "CPU Package",
    "core_max_c": lambda i, n: i.startswith("/intelcpu/") and "/temperature/" in i and n == "Core Max",
    "platform_w": lambda i, n: i.startswith("/intelcpu/") and "/power/" in i and n == "CPU Platform",
    "igpu_mhz": lambda i, n: i.startswith("/gpu-intel-integrated/") and i.endswith("/clock/0"),
    "igpu_w_lhm": lambda i, n: i.startswith("/gpu-intel-integrated/") and i.endswith("/power/0"),
}


def read_lhm(folder, near, span=None):
    # Merges by time every log file that overlaps the run, since the logger starts a new file
    # after any gap
    out = {}
    if not folder or not Path(folder).exists():
        return []
    for path in sorted(Path(folder).glob("LibreHardwareMonitorLog-*.csv")):
        # Creation times change when logs are copied from another machine, so only the last write counts
        if span and path.stat().st_mtime < span[0]:
            continue
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            rows_ = list(csv.reader(f))
        if len(rows_) < 3:
            continue
        ids, names = rows_[0], rows_[1]
        cols = {k: next((c for c, (i, n) in enumerate(zip(ids, names)) if test(i, n)), None)
                for k, test in LHM_KEYS.items()}
        for row in rows_[2:]:
            t = parse_stamp(row[0], near) if row else None
            if t is None:
                continue
            values = {}
            for k, n in cols.items():
                if n is not None and n < len(row):
                    try:
                        values[k] = float(row[n])
                    except ValueError:
                        pass
            out[t] = values
    return sorted(out.items())


def lhm_stats(series, a, b):
    picked = [v for t, v in series if a <= t < b]
    if not picked:
        return None
    col = lambda k: [v[k] for v in picked if k in v]  # noqa: E731
    return {"pkg_c": mean(col("pkg_c")), "pkg_c_max": max(col("pkg_c"), default=None),
            "core_max_c": max(col("core_max_c"), default=None), "igpu_mhz": mean(col("igpu_mhz")),
            "platform_w": mean(col("platform_w"))}


# --- Figures ---------------------------------------------------------------------------

PKG = "\\energy meter(rapl_package0_pkg)\\power"
PP0 = "\\energy meter(rapl_package0_pp0)\\power"
PP1 = "\\energy meter(rapl_package0_pp1)\\power"
CPU = "\\processor information(_total)\\% processor utility"


def series(samples, kinds):
    out = []
    for t, v in samples:
        per = defaultdict(lambda: defaultdict(float))
        for name, value in v.items():
            if name.startswith("\\gpu engine(") and "_luid_" in name and "engtype_" in name:
                inst = name[len("\\gpu engine("):].split(")")[0]
                per[inst.split("_luid_")[1].split("_phys")[0]][inst.split("engtype_")[1]] += value
        busy = {kind: min(100.0, max(per[luid].values(), default=0.0)) for luid, kind in kinds.items()}
        out.append((t, v.get(PKG, 0) / 1000, v.get(PP0, 0) / 1000, v.get(PP1, 0) / 1000, v.get(CPU),
                    busy.get("igpu"), busy.get("npu")))
    return out


def figures(run, md, consults, per_window, kinds, summary, arms, energy):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        md("\n(matplotlib missing, no figures)")
        return
    fig_dir = run / "figures"
    fig_dir.mkdir(exist_ok=True)
    md("\n## Figures\n")
    # One consultation per arm, as Task Manager would show it
    for arm, pass_no in arms:
        cs = [c for c in consults if c["window"].get("arm") == arm and c["window"].get("pass_no") == pass_no
              and c.get("stop")]
        if not cs:
            continue
        c = cs[len(cs) // 2]
        s0 = c["window"]["session_t0"]
        data = series(per_window[c["window"]["index"]], kinds)
        if not data:
            continue
        x = [(d[0] - s0) / 60 for d in data]
        fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 5), sharex=True)
        a1.plot(x, [d[1] for d in data], label="package", lw=1)
        a1.plot(x, [d[2] for d in data], label="CPU cores", lw=1)
        a1.plot(x, [d[3] for d in data], label="GPU", lw=1)
        a1.set_ylabel("Power (W)")
        a1.legend(loc="upper left", fontsize=8)
        a2.plot(x, [d[4] for d in data], label="CPU", lw=1)
        a2.plot(x, [d[5] for d in data], label="iGPU", lw=1)
        a2.plot(x, [d[6] for d in data], label="NPU", lw=1)
        a2.set_ylabel("Busy (%)")
        a2.set_xlabel("Minutes from start of recording")
        a2.legend(loc="upper left", fontsize=8)
        for ax in (a1, a2):
            ax.axvline((c["stop"] - s0) / 60, color="k", ls="--", lw=0.8)
            ax.axvspan((c["stop"] - s0) / 60, (c["after_stop_end"] - s0) / 60, color="0.9")
        a1.set_title(f"{arm}, {c['window'].get('track')}: Stop dashed, after-Stop work shaded", fontsize=9)
        fig.tight_layout()
        name = f"timeline-{arm}-{pass_no}.png"
        fig.savefig(fig_dir / name, dpi=200)
        plt.close(fig)
        md(f"![{arm} timeline](figures/{name})")
    firsts = [(k, v) for k, v in summary["arms"].items() if k.endswith("#1") and v.get("stop_to_first_words_s")]
    if firsts:
        fig, ax = plt.subplots(figsize=(7, 3))
        labels = [k[:-2] for k, _ in firsts]
        ax.bar(labels, [v["stop_to_first_words_s"] for _, v in firsts], label="first note words")
        ax.bar(labels, [(v.get("sheet_done_s") or 0) - v["stop_to_first_words_s"] for _, v in firsts],
               bottom=[v["stop_to_first_words_s"] for _, v in firsts], alpha=0.4, label="to sheet done")
        ax.axhline(5, color="k", ls="--", lw=0.8)
        ax.set_ylabel("Seconds after Stop")
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(fig_dir / "latency.png", dpi=200)
        plt.close(fig)
        md("![latency](figures/latency.png)")
    bars = [(a, e) for (a, p), e in energy.items() if p == 1 and e.get("above_wh") is not None]
    if bars:
        fig, ax = plt.subplots(figsize=(7, 3))
        ax.bar([a for a, _ in bars], [e["above_wh"] for _, e in bars])
        ax.set_ylabel("Wh above idle per consultation")
        fig.tight_layout()
        fig.savefig(fig_dir / "energy.png", dpi=200)
        plt.close(fig)
        md("![energy](figures/energy.png)")


def accuracy(run, md):
    folder = run / "transcripts"
    files = sorted(folder.glob("*.json")) if folder.exists() else []
    if not files:
        return {}
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
        from transcription.score import score
    except Exception as e:
        md(f"\n(transcripts saved but not scored: {e})")
        return {}
    import tempfile
    clinical = True
    try:
        # scispaCy's model stores one setting as the string "True", which current spaCy rejects
        import spacy
        if not getattr(spacy.load, "patched", False):
            load = spacy.load

            def patched(name, **kw):
                cfg = dict(kw.pop("config", {}) or {})
                for key in ("components.tok2vec.model.embed.include_static_vectors",
                            "components.ner.model.tok2vec.embed.include_static_vectors"):
                    cfg.setdefault(key, True)
                return load(name, config=cfg, **kw)
            patched.patched = True
            spacy.load = patched
        from transcription import clinical as lens  # noqa: F401
        lens.analyze("no cough", "no cough")
    except Exception:
        clinical = False
    by_arm = defaultdict(list)
    for f in files:
        tier, device, pass_no, stem = f.stem.split("-", 3)
        arm = f"{tier}-{device}"
        consult = stem.replace("_mixed", "")
        data = json.loads(f.read_text(encoding="utf-8"))
        turns = sorted(data.get("turns", []), key=lambda t: t.get("firstFrame", 0))
        text = " ".join(t.get("text", "") for t in turns)
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as tmp:
            json.dump({"text": text}, tmp)
        try:
            row = score(Path(tmp.name), arm, consult, "mixed", clinical)
        except Exception as e:
            md(f"\n(could not score {f.name}: {e})")
            continue
        finally:
            Path(tmp.name).unlink(missing_ok=True)
        by_arm[(arm, int(pass_no))].append(row)
    md("\n## Transcription accuracy per arm\n")
    md("The same consultations against the PriMock57 reference, so the GPU and NPU arms of a tier "
       "show whether recognition on the NPU is as accurate. Pooled over the tracks.\n")
    md("| Arm | Pass | WER (%) | Content WER (%) | Inserted per 1,000 words | Drug recall | Negations kept |")
    md("|---|---|---|---|---|---|---|")
    out = {}
    for (arm, pass_no), rs in sorted(by_arm.items()):
        n = sum(x["wer_detail"]["N"] for x in rs)
        errs = sum(x["wer_detail"]["S"] + x["wer_detail"]["D"] + x["wer_detail"]["I"] for x in rs)
        ins = sum(x["wer_detail"]["I"] for x in rs)
        content = mean([x["wer"]["content"] for x in rs])
        drugs = mean([x.get("clinical", {}).get("drug_recall") for x in rs]) if clinical else None
        negs = mean([x.get("clinical", {}).get("neg_preserved") for x in rs]) if clinical else None
        out[f"{arm}#{pass_no}"] = {"wer": 100 * errs / n if n else None, "inserted_per_k": 1000 * ins / n if n else None}
        md(f"| {arm} | {pass_no} | {fmt(100 * errs / n if n else None, 2)} | {fmt(content and 100 * content, 2)} "
           f"| {fmt(1000 * ins / n if n else None)} | {fmt(drugs, 3)} | {fmt(negs, 3)} |")
    return out


def bandwidth(md, summary):
    # utilisation = tokens/s x weight size / peak bandwidth
    machine = summary["machine"]
    try:
        peak_gbs = float(machine.get("memory_mts")) * float(machine.get("memory_bus_bits")) / 8 / 1000
    except (TypeError, ValueError):
        peak_gbs = None
    models = Path(machine.get("models") or "")
    md("\n## What limits decode speed\n")
    md(f"Each generated token reads the active weights once, so decode is bound by memory bandwidth: "
       f"ceiling = peak bandwidth / active weight size, and utilisation = measured / ceiling. Peak "
       f"bandwidth here is {fmt(peak_gbs)} GB/s ({machine.get('memory_mts')} MT/s on a "
       f"{machine.get('memory_bus_bits')}-bit bus). The mixture-of-experts 35B reads about 3 of its 35 "
       f"billion parameters per token, which is why it outruns the dense 9B.\n")
    md("| Arm | Active weights (GB) | Note tokens/s | Ceiling (tokens/s) | Weights read (GB/s) | Utilisation |")
    md("|---|---|---|---|---|---|")
    for key, a in summary["arms"].items():
        tps = a.get("note_tokens_per_s")
        tier = key.split("-")[0]
        if not tps or tier not in NOTE_MODELS:
            continue
        folder, share = NOTE_MODELS[tier]
        bins = [f for f in (models / folder).glob("*.bin")
                if "tokenizer" not in f.name and "vision" not in f.name]
        size_gb = sum(f.stat().st_size for f in bins) * share / 1e9 if bins else None
        ceiling = peak_gbs / size_gb if size_gb and peak_gbs else None
        a["bandwidth"] = {"active_gb": size_gb, "ceiling_tps": ceiling,
                          "utilisation": tps / ceiling if ceiling else None}
        md(f"| {key.split('#')[0]}{'' if key.endswith('#1') else ' (repeat)'} | {fmt(size_gb, 2)} | {fmt(tps)} "
           f"| {fmt(ceiling)} | {fmt(tps * size_gb if size_gb else None)} "
           f"| {fmt(100 * tps / ceiling if ceiling else None, 0)}% |")


def switches(run, md):
    rows = rows_of(run / "switches.jsonl")
    if not rows:
        return
    md("\n## Loading, switching and unloading\n")
    md("In one engine: each note tier loaded in turn (the previous one is unloaded first), speech "
       "recognition moved to the NPU and back, then shutdown and the memory it gives back.\n")
    md("| Step | Seconds | Model's own load (s) | Note host before / after (GB) | Engine (GB) |")
    md("|---|---|---|---|---|")
    for r in rows:
        if "freed_mb" in r:
            md(f"\nShutdown gave back {fmt(r['freed_mb'] / 1024, 1)} GB (free RAM {fmt(r['loaded_mb'] / 1024, 1)} GB "
               f"loaded, {fmt(r['after_mb'] / 1024, 1)} GB after; {fmt(r['before_start_mb'] / 1024, 1)} GB before start).")
            continue
        nb, na = r.get("note_host_mb_before"), r.get("note_host_mb_after")
        eng = r.get("engine_mb") or r.get("engine_mb_after")
        md(f"| {r['step']} | {fmt(r.get('seconds'), 2)} | {fmt(r.get('model_seconds'))} "
           f"| {fmt(nb and nb / 1024, 2)} / {fmt(na and na / 1024, 2)} | {fmt(eng and eng / 1024, 2)} |")


# RAM for Windows and the clinician's other applications on top of ClinicAVT
OS_ALLOWANCE_GB = 6.0
RAM_CLASSES = [8, 16, 32, 64, 128]
NFR5_TPS = 15.0
SHARED = ["whisper-turbo-int8", "silero-vad", "pyannote-seg3", "eres2netv2-int8", "nllb-200-600m-int8",
          "gte-large-int8"]


def folder_gb(path):
    return sum(f.stat().st_size for f in path.rglob("*") if f.is_file()) / 1e9 if path.exists() else None


def min_specs(md, summary):
    machine = summary["machine"]
    models = Path(machine.get("models") or "")
    shared_gb = sum(folder_gb(models / m) or 0 for m in SHARED)
    md("\n## Minimum specification per tier\n")
    md(f"RAM is ClinicAVT's peak resident memory plus {OS_ALLOWANCE_GB:.0f} GB for Windows "
       f"and other applications, rounded up to a standard size. Bandwidth is what reaches {NFR5_TPS:.0f} "
       "tokens/s (NFR-5) at the utilisation measured here, given as LPDDR5x speed on a 128-bit bus. "
       "Speech recognition needs to run faster than real time with margin; disk is the installed models.\n")
    md("| Tier | Resident (GB) | Committed (GB) | Left on a 16 GB laptop (GB) | RAM class | Bandwidth for 15 tokens/s (GB/s) | LPDDR5x on 128-bit (MT/s) "
       "| Speech x real time, GPU / NPU | Disk (GB) |")
    md("|---|---|---|---|---|---|---|---|---|")
    out = {}
    for tier, (folder, _) in NOTE_MODELS.items():
        gpu = summary["arms"].get(f"{tier}-gpu#1") or {}
        npu = summary["arms"].get(f"{tier}-npu#1") or {}
        foot = max([((a.get("memory") or {}).get("resident_mb") or 0) / 1024 for a in (gpu, npu)], default=0)
        committed = max([((a.get("memory") or {}).get("committed_mb") or 0) / 1024 for a in (gpu, npu)], default=0)
        ram = next((c for c in RAM_CLASSES if c >= foot + OS_ALLOWANCE_GB), None) if foot else None
        bw = gpu.get("bandwidth") or {}
        need = None
        if bw.get("utilisation") and bw.get("active_gb"):
            need = NFR5_TPS * bw["active_gb"] / bw["utilisation"]
        mts = need * 1000 * 8 / 128 if need else None
        disk = shared_gb + (folder_gb(models / folder) or 0)
        out[tier] = {"resident_gb": foot or None, "committed_gb": committed or None, "ram_class_gb": ram, "bandwidth_gbs": need, "lpddr_mts": mts,
                     "disk_gb": disk}
        md(f"| {tier} | {fmt(foot or None, 1)} | {fmt(committed or None, 1)} | {fmt(16 - foot if foot else None, 1)} "
           f"| {ram or '–'} GB | {fmt(need)} | {fmt(mts, 0)} "
           f"| {fmt(gpu.get('asr_rtf'))} / {fmt(npu.get('asr_rtf'))} | {fmt(disk, 1)} |")
    md("\nA tier whose bandwidth need is below the machine's peak streams fast enough; one above it still "
       "works, more slowly. The note model always runs on the GPU, so an Intel GPU with OpenVINO support is "
       "required; the NPU is optional and only moves speech recognition.")
    summary["min_specs"] = out


# --- Report ------------------------------------------------------------------------------

def main():
    run = Path(sys.argv[1])
    machine = json.loads((run / "machine.json").read_text(encoding="utf-8"))
    windows = rows(run / "windows.jsonl")
    runs = rows(run / "runs.jsonl")
    translations = rows(run / "translations.jsonl")
    loads = rows(run / "loads.jsonl")
    procs = rows(run / "procs.jsonl")

    all_samples = []
    per_window = {}
    for w in windows:
        samples = []
        for part in w.get("csv_parts") or [w["csv"]]:
            samples.extend(read_counters(run / "counters" / part, w["t0"]))
        samples.sort(key=lambda s: s[0])
        per_window[w["index"]] = samples
        all_samples.extend(samples)
    kinds = classify_adapters(all_samples)
    span = (min(w["t0"] for w in windows) - 60, max(w["t1"] for w in windows) + 60) if windows else None

    rest_by_mode = defaultdict(list)
    idle, cool = [], []
    for w in windows:
        if w["kind"].startswith("system-idle"):
            s = phase_stats(per_window[w["index"]], w["t0"], w["t1"], kinds)
            if s:
                idle.append(s)
        elif w["kind"] == "cooldown":
            # The last minute of the cool-down, once the machine has settled
            s = phase_stats(per_window[w["index"]], w["t1"] - 60, w["t1"], kinds)
            if s:
                cool.append(s)
        else:
            continue
        if s and s.get("pkg_w") is not None:
            rest_by_mode[w.get("power_mode") or "balanced"].append(s["pkg_w"])
    # Idle baseline is the median of the rest windows in the same power mode, so one disturbed
    # window cannot skew it. A mode with no rest window falls back to Balanced
    baseline = {m: statistics.median(v) for m, v in rest_by_mode.items()}
    idle_pkg = baseline.get("balanced") or (statistics.median([x for v in rest_by_mode.values() for x in v])
                                            if rest_by_mode else None)

    def arm_idle(arm):
        mode = arm.split("@")[1] if "@" in arm else "balanced"
        return baseline.get(mode, idle_pkg)

    consults = []
    for w in windows:
        if w["kind"] != "consultation":
            continue
        rec = next((r for r in runs if r.get("arm") == w.get("arm") and r.get("pass_no") == w.get("pass_no")
                    and r.get("track") == w.get("track")), None)
        if rec is None or rec.get("stop_at_s") is None:
            consults.append({"window": w, "rec": rec, "phases": {}})
            continue
        s0 = w["session_t0"]
        stop = s0 + rec["stop_at_s"]
        fin_end = stop + (rec.get("finalise_s") or 0)
        note_end = fin_end + (rec.get("note_done_s") or 0)
        sheet_end = note_end + (rec.get("patient_done_s") or 0)
        trans = [t for t in translations if t.get("arm") == w.get("arm") and t.get("track") == w.get("track")
                 and w["t0"] <= t["t0"] <= w["t1"]]
        samples = per_window[w["index"]]
        phases = {
            "capture": phase_stats(samples, s0, stop, kinds),
            "finalise": phase_stats(samples, stop, fin_end, kinds),
            "note": phase_stats(samples, fin_end, note_end, kinds),
            "sheet": phase_stats(samples, note_end, sheet_end, kinds),
        }
        if trans:
            phases["translation"] = phase_stats(samples, min(t["t0"] for t in trans),
                                                max(t["t1"] for t in trans), kinds)
        phases_p = {"capture": proc_stats(procs, s0, stop), "after_stop": proc_stats(procs, stop, sheet_end)}
        consults.append({"window": w, "rec": rec, "phases": phases, "procs": phases_p, "trans": trans,
                         "after_stop_end": sheet_end, "stop": stop})

    warmup = defaultdict(list)
    for w in windows:
        if w["kind"] == "warmup":
            s = phase_stats(per_window[w["index"]], w["t0"], w["t1"], kinds)
            if s:
                warmup[(w["arm"], w["pass_no"])].append(s)
    loaded_idle = defaultdict(list)
    for w in windows:
        if w["kind"] == "loaded-idle":
            s = phase_stats(per_window[w["index"]], w["t0"], w["t1"], kinds)
            if s:
                loaded_idle[(w["arm"], w["pass_no"])].append(s)

    arms = []
    for w in windows:
        key = (w.get("arm"), w.get("pass_no"))
        if w["kind"] == "consultation" and key not in arms:
            arms.append(key)

    out = []
    md = out.append
    md(f"# Benchmark: {machine.get('host')}, {machine.get('started')}\n")
    batt = machine.get("battery") or {}
    md(f"- **Machine:** {machine.get('model')}; {machine.get('cpu')}; {machine.get('ram_gb')} GB; "
       f"GPUs: {machine.get('gpus')}; NPU: {machine.get('npu')}")
    md(f"- **Power:** {'mains' if batt.get('PowerOnline') else 'battery'}; plan: {machine.get('power_plan')}; "
       f"battery full {fmt((float(machine.get('battery_full_mwh') or 0)) / 1000, 1)} Wh")
    md(f"- **Tracks:** " + ", ".join(f"{n} ({d / 60:.1f} min)" for n, d in machine.get("tracks", [])))
    md(f"- **Adapters found:** " + ", ".join(f"{k}={v}" for k, v in kinds.items()))
    prov = machine.get("provenance") or {}
    md(f"- **Code:** commit {str(prov.get('commit'))[:10]} on {prov.get('branch')}, "
       f"{prov.get('uncommitted_files')} uncommitted files; engine {str(prov.get('engine_sha256'))[:12]} "
       f"built {prov.get('engine_built')}")
    md("- **Rest baselines by power mode:** " + ", ".join(
        f"{m} {fmt(v, 2)} W (n={len(rest_by_mode[m])})" for m, v in sorted(baseline.items())))
    md(f"- **System idle package power:** {fmt(idle_pkg, 2)} W "
       f"(every power figure is the processor package from its RAPL counter; the screen, SSD and the "
       f"rest of the board are outside it)\n")

    md(METHODS)
    md("## Load times\n")
    md("| Arm | Engine ready (s) | Note model state | Note model load (s) | Loaded after (s) | Speech load (s) |")
    md("|---|---|---|---|---|---|")
    for l in loads:
        nm = l.get("note_model") or {}
        ls = (l.get("metrics") or {}).get("loadSeconds") or {}
        md(f"| {l['arm']} | {fmt(l.get('engine_ready_s'))} | {nm.get('state') or (l.get('tier_reply') or {}).get('state')} "
           f"| {fmt(nm.get('seconds'))} | {fmt(l.get('note_loaded_after_s'))} | {fmt(ls.get('asr'))} |")

    md("\n## After Stop, per arm (mean over tracks)\n")
    md("| Arm | Pass | Runs ok | Finalise (s) | Stop to first note words (s) | Note done (s) | Sheet done (s) "
       "| Stop to all done (s) | Note tokens/s | Sheet tokens/s | Speech x real time | Lost frames |")
    md("|---|---|---|---|---|---|---|---|---|---|---|---|")
    summary = {"machine": machine, "kinds": kinds, "idle_pkg_w": idle_pkg, "arms": {}}
    for arm, pass_no in arms:
        cs = [c for c in consults if c["window"].get("arm") == arm and c["window"].get("pass_no") == pass_no]
        recs = [c["rec"] for c in cs if c["rec"]]
        ok = sum(1 for r in recs if r.get("outcome") == "ok")
        fin = mean([r.get("finalise_s") for r in recs])
        first = mean([(r.get("finalise_s") or 0) + r["note_first_token_s"] for r in recs
                      if r.get("note_first_token_s") is not None])
        note = mean([(r.get("finalise_s") or 0) + r["note_done_s"] for r in recs if r.get("note_done_s") is not None])
        sheet = mean([(r.get("finalise_s") or 0) + (r.get("note_done_s") or 0) + r["patient_done_s"]
                      for r in recs if r.get("patient_done_s") is not None])
        alld = mean([r.get("stop_to_all_done_s") for r in recs])
        rtf = mean([(r.get("metrics") or {}).get("asrRealtimeFactor") for r in recs])
        lost = sum(((r.get("metrics") or {}).get("lostFrames") or 0) for r in recs)
        note_tps = mean([c["window"].get("note_tokens_per_s") for c in cs])
        sheet_tps = mean([c["window"].get("patient_tokens_per_s") for c in cs])
        md(f"| {arm} | {pass_no} | {ok}/{len(cs)} | {fmt(fin, 2)} | {fmt(first, 2)} | {fmt(note)} | {fmt(sheet)} "
           f"| {fmt(alld)} | {fmt(note_tps)} | {fmt(sheet_tps)} | {fmt(rtf)} | {lost} |")
        summary["arms"][f"{arm}#{pass_no}"] = {"ok": ok, "runs": len(cs), "finalise_s": fin,
                                               "stop_to_first_words_s": first, "note_done_s": note,
                                               "sheet_done_s": sheet, "stop_to_all_s": alld, "asr_rtf": rtf,
                                               "note_tokens_per_s": note_tps, "sheet_tokens_per_s": sheet_tps,
                                               "lost_frames": lost}

    md("\n### Per run\n")
    md("All times from Stop.\n")
    md("| Arm | Pass | Track | Audio (min) | Outcome | Finalise (s) | First words (s) | Note done (s) "
       "| Sheet done (s) | Note chars | Finalise stages (s) |")
    md("|---|---|---|---|---|---|---|---|---|---|---|")
    for c in consults:
        r, w = c["rec"] or {}, c["window"]
        fin = r.get("finalise_s") or 0
        first = (fin + r["note_first_token_s"]) if r.get("note_first_token_s") is not None else None
        note_done = (fin + r["note_done_s"]) if r.get("note_done_s") is not None else None
        sheet_done = (note_done + r["patient_done_s"]) if note_done is not None and r.get("patient_done_s") is not None else None
        stages = ", ".join(f"{k} {v:.1f}" for k, v in (r.get("finalise_stages") or {}).items())
        md(f"| {w.get('arm')} | {w.get('pass_no')} | {w.get('track', '')[:20]} | {fmt(w.get('audio_s', 0) / 60)} "
           f"| {r.get('outcome')} | {fmt(r.get('finalise_s'), 2)} | {fmt(first, 2)} | {fmt(note_done)} "
           f"| {fmt(sheet_done)} | {r.get('note_chars')} | {stages} |")

    md("\n## Translation of the sheet (CPU)\n")
    md("The translation model unloads when recording starts, so the first language of each "
       "consultation includes loading it and the second does not.\n")
    md("| Arm | Language | Runs | Seconds | First text (s) | Tokens/s |")
    md("|---|---|---|---|---|---|")
    groups = defaultdict(list)
    for t in translations:
        groups[(t["arm"], t["language"])].append(t)
    for (arm, lang), ts in groups.items():
        md(f"| {arm} | {lang} | {sum(1 for t in ts if t.get('outcome') == 'ready')}/{len(ts)} "
           f"| {fmt(mean([t.get('seconds') for t in ts]), 2)} | {fmt(mean([t.get('first_s') for t in ts]), 2)} "
           f"| {fmt(mean([t.get('tokens_per_s') for t in ts]))} |")

    md("\n## Power and load per phase (mean over tracks)\n")
    md("Package power splits into the CPU cores, the integrated GPU, and the rest of the package "
       "(memory controller, NPU, media and fabric). Load is the busiest engine of each device.\n")
    md("| Arm | Pass | Phase | Package (W) | Cores (W) | GPU (W) | Rest incl. NPU (W) | CPU % | iGPU % | NPU % | CPU clock (% of base) |")
    md("|---|---|---|---|---|---|---|---|---|---|---|")
    if cool:
        md(f"| system | – | end of cool-downs | {fmt(mean([s['pkg_w'] for s in cool]), 2)} | "
           f"{fmt(mean([s['cores_w'] for s in cool]), 2)} | {fmt(mean([s['gfx_w'] for s in cool]), 2)} | "
           f"{fmt(mean([s['rest_w'] for s in cool]), 2)} | {fmt(mean([s['cpu_pct'] for s in cool]))} | "
           f"{fmt(mean([s['igpu_pct'] for s in cool]))} | {fmt(mean([s['npu_pct'] for s in cool]))} | {fmt(mean([s['clock_pct'] for s in cool]))} |")
    md(f"| system | – | idle | {fmt(idle_pkg, 2)} | {fmt(mean([s['cores_w'] for s in idle]), 2)} | "
       f"{fmt(mean([s['gfx_w'] for s in idle]), 2)} | {fmt(mean([s['rest_w'] for s in idle]), 2)} | "
       f"{fmt(mean([s['cpu_pct'] for s in idle]))} | {fmt(mean([s['igpu_pct'] for s in idle]))} | "
       f"{fmt(mean([s['npu_pct'] for s in idle]))} | {fmt(mean([s['clock_pct'] for s in idle]))} |")
    for arm, pass_no in arms:
        wu = warmup.get((arm, pass_no), [])
        if wu:
            md(f"| {arm} | {pass_no} | first minute after load | {fmt(mean([s['pkg_w'] for s in wu]), 2)} | "
               f"{fmt(mean([s['cores_w'] for s in wu]), 2)} | {fmt(mean([s['gfx_w'] for s in wu]), 2)} | "
               f"{fmt(mean([s['rest_w'] for s in wu]), 2)} | {fmt(mean([s['cpu_pct'] for s in wu]))} | "
               f"{fmt(mean([s['igpu_pct'] for s in wu]))} | {fmt(mean([s['npu_pct'] for s in wu]))} | {fmt(mean([s['clock_pct'] for s in wu]))} |")
        li = loaded_idle.get((arm, pass_no), [])
        if li:
            md(f"| {arm} | {pass_no} | models loaded, idle | {fmt(mean([s['pkg_w'] for s in li]), 2)} | "
               f"{fmt(mean([s['cores_w'] for s in li]), 2)} | {fmt(mean([s['gfx_w'] for s in li]), 2)} | "
               f"{fmt(mean([s['rest_w'] for s in li]), 2)} | {fmt(mean([s['cpu_pct'] for s in li]))} | "
               f"{fmt(mean([s['igpu_pct'] for s in li]))} | {fmt(mean([s['npu_pct'] for s in li]))} | {fmt(mean([s['clock_pct'] for s in li]))} |")
        cs = [c for c in consults if c["window"].get("arm") == arm and c["window"].get("pass_no") == pass_no]
        for phase in ("capture", "finalise", "note", "sheet", "translation"):
            ps = [c["phases"].get(phase) for c in cs if c["phases"].get(phase)]
            if not ps:
                continue
            md(f"| {arm} | {pass_no} | {phase} | {fmt(mean([p['pkg_w'] for p in ps]), 2)} | "
               f"{fmt(mean([p['cores_w'] for p in ps]), 2)} | {fmt(mean([p['gfx_w'] for p in ps]), 2)} | "
               f"{fmt(mean([p['rest_w'] for p in ps]), 2)} | {fmt(mean([p['cpu_pct'] for p in ps]))} | "
               f"{fmt(mean([p['igpu_pct'] for p in ps]))} | {fmt(mean([p['npu_pct'] for p in ps]))} | {fmt(mean([p['clock_pct'] for p in ps]))} |")

    series = read_lhm(machine.get("lhm_dir"), windows[0]["t0"] if windows else None, span)
    if series:
        md("\n## Temperature, iGPU clock and platform power per phase\n")
        md("From LibreHardwareMonitor, run as administrator alongside: CPU package temperature, the "
           "hottest core, the integrated GPU's clock, and platform power (PSys), the processor's reading "
           "of what the whole board draws.\n")
        md("| Arm | Pass | Phase | Package (°C) | Package max (°C) | Hottest core (°C) | iGPU clock (MHz) "
           "| Platform (W) |")
        md("|---|---|---|---|---|---|---|---|")
        for w in windows:
            if w["kind"] in ("system-idle", "system-idle-end", "cooldown"):
                st = lhm_stats(series, w["t0"], w["t1"])
                if st:
                    md(f"| system | – | {w['kind']} | {fmt(st['pkg_c'])} | {fmt(st['pkg_c_max'])} | "
                       f"{fmt(st['core_max_c'])} | {fmt(st['igpu_mhz'], 0)} | {fmt(st['platform_w'])} |")
        for arm, pass_no in arms:
            cs = [c for c in consults if c["window"].get("arm") == arm and c["window"].get("pass_no") == pass_no
                  and c.get("stop")]
            for phase in ("capture", "after Stop"):
                sts = []
                for c in cs:
                    s0 = c["window"]["session_t0"]
                    a, b = (s0, c["stop"]) if phase == "capture" else (c["stop"], c["after_stop_end"])
                    st = lhm_stats(series, a, b)
                    if st:
                        sts.append(st)
                if sts:
                    md(f"| {arm} | {pass_no} | {phase} | {fmt(mean([s['pkg_c'] for s in sts]))} | "
                       f"{fmt(max([s['pkg_c_max'] or 0 for s in sts]))} | {fmt(max([s['core_max_c'] or 0 for s in sts]))} | "
                       f"{fmt(mean([s['igpu_mhz'] for s in sts]), 0)} | {fmt(mean([s['platform_w'] for s in sts]))} |")
                    summary["arms"].setdefault(f"{arm}#{pass_no}", {}).setdefault("thermal", {})[phase] = {
                        "pkg_c": mean([s["pkg_c"] for s in sts]), "igpu_mhz": mean([s["igpu_mhz"] for s in sts]),
                        "platform_w": mean([s["platform_w"] for s in sts])}
    imports_ = [w for w in windows if w["kind"] == "import"]
    if imports_:
        md("\n## Upload path: importing a recording, GPU against NPU\n")
        md("Each recording imported as a file through the app's import path, as fast as the engine can go "
           "(diarisation, then speech recognition on the device shown); the default note tier then writes "
           "the note and sheet. Package power is over the import itself; energy runs from the import to the "
           "written sheet, the same span as a live consultation's, so the two compare per audio minute. "
           "An import that ended without a sheet is shown but left out of the summary.\n")
        md("| Device | Track | Audio (min) | Import (s) | x real time | Note after (s) | Sheet after (s) "
           "| Package (W) | Energy to sheet (Wh) | Above idle (Wh) | Above idle per audio minute (Wh) "
           "| Package (°C) |")
        md("|---|---|---|---|---|---|---|---|---|---|---|---|")
        series_ = read_lhm(machine.get("lhm_dir"), imports_[0]["t0"], span)
        by_device = defaultdict(list)
        for w in imports_:
            imported = w.get("import_s") or 0
            whole = w.get("all_s") or imported
            st = phase_stats(per_window[w["index"]], w["t0"], w["t0"] + imported, kinds)
            all_ = phase_stats(per_window[w["index"]], w["t0"], w["t0"] + whole, kinds)
            th = lhm_stats(series_, w["t0"], w["t0"] + imported) if series_ else None
            wh = all_["pkg_j"] / 3600 if all_ and all_.get("pkg_j") else None
            above = max(0.0, wh - idle_pkg * whole / 3600) if wh is not None and idle_pkg is not None else None
            per_min = above / (w["audio_s"] / 60) if above is not None else None
            if w.get("sheet_after_s") is not None:
                by_device[w["device"]].append({"x": w.get("x_real_time"), "import_s": imported,
                                               "wh_min": wh / (w["audio_s"] / 60) if wh else None,
                                               "above_min": per_min})
            md(f"| {w['device']} | {w['track'][:20]} | {fmt(w['audio_s'] / 60)} | {fmt(imported)} "
               f"| {fmt(w.get('x_real_time'))} | {fmt(w.get('note_after_s'))} | {fmt(w.get('sheet_after_s'))} "
               f"| {fmt(st and st['pkg_w'], 1)} | {fmt(wh, 3)} | {fmt(above, 3)} | {fmt(per_min, 3)} "
               f"| {fmt(th and th['pkg_c'])} |")
        summary["imports"] = {d: {"x_real_time": mean([r["x"] for r in v]),
                                  "import_s": mean([r["import_s"] for r in v]),
                                  "wh_per_audio_min": mean([r["wh_min"] for r in v if r["wh_min"]]),
                                  "above_idle_wh_per_audio_min": mean([r["above_min"] for r in v
                                                                       if r["above_min"] is not None])}
                              for d, v in by_device.items()}
        md("\n" + "; ".join(f"**{d}**: {fmt(v['x_real_time'])}x real time, "
                             f"{fmt(v['above_idle_wh_per_audio_min'], 3)} Wh above idle per audio minute"
                             for d, v in summary["imports"].items()))

    md("\n## Energy per consultation\n")
    md("Package energy from the start of recording until the sheet is written, and the part above the "
       "machine's own idle draw over the same time.\n")
    md("| Arm | Pass | Wh per consultation | Above idle (Wh) | Above idle per audio minute (Wh) "
       "| Capture power above idle (W) |")
    md("|---|---|---|---|---|---|")
    energy = {}
    for arm, pass_no in arms:
        cs = [c for c in consults if c["window"].get("arm") == arm and c["window"].get("pass_no") == pass_no
              and c["phases"].get("capture")]
        whs, above, per_min, cap, after = [], [], [], [], []
        base = arm_idle(arm)
        for c in cs:
            ps = [p for k, p in c["phases"].items() if p and k != "translation"]
            j = sum(p["pkg_j"] or 0 for p in ps)
            secs = sum(p["seconds"] for p in ps)
            whs.append(j / 3600)
            if base is not None:
                extra = (j - base * secs) / 3600
                above.append(extra)
                per_min.append(extra / (c["window"]["audio_s"] / 60))
                cap.append(c["phases"]["capture"]["pkg_w"] - base)
                post = [p for k, p in c["phases"].items() if p and k not in ("capture", "translation")]
                after.append(sum((p["pkg_j"] or 0) - base * p["seconds"] for p in post) / 3600)
        li = loaded_idle.get((arm, pass_no), [])
        energy[(arm, pass_no)] = {"wh": mean(whs), "above_wh": mean(above), "per_min": mean(per_min),
                                  "capture_above_w": mean(cap), "after_stop_above_wh": mean(after),
                                  "loaded_idle_above_w": (mean([s["pkg_w"] for s in li]) - base)
                                  if li and base is not None else None, "baseline_w": base}
        md(f"| {arm} | {pass_no} | {fmt(mean(whs), 2)} | {fmt(mean(above), 2)} | {fmt(mean(per_min), 3)} "
           f"| {fmt(mean(cap), 2)} |")
        summary["arms"].setdefault(f"{arm}#{pass_no}", {})["energy"] = energy[(arm, pass_no)]

    md("\n## Speech recognition on the NPU against the GPU\n")
    md("| Tier | Capture power GPU / NPU (W) | Energy per consultation GPU / NPU (Wh) "
       "| Stop to first words GPU / NPU (s) | Finalise GPU / NPU (s) |")
    md("|---|---|---|---|---|")
    for tier in ("4b", "9b", "35b"):
        g, n = (f"{tier}-gpu", 1), (f"{tier}-npu", 1)
        sg, sn = summary["arms"].get(f"{g[0]}#1"), summary["arms"].get(f"{n[0]}#1")
        if not sg or not sn:
            continue

        def cap(key):
            cs = [c["phases"]["capture"]["pkg_w"] for c in consults if c["window"].get("arm") == key
                  and c["window"].get("pass_no") == 1 and c["phases"].get("capture")]
            return mean(cs)
        md(f"| {tier} | {fmt(cap(g[0]), 2)} / {fmt(cap(n[0]), 2)} | {fmt(energy.get(g, {}).get('wh'), 2)} / "
           f"{fmt(energy.get(n, {}).get('wh'), 2)} | {fmt(sg.get('stop_to_first_words_s'), 2)} / "
           f"{fmt(sn.get('stop_to_first_words_s'), 2)} | {fmt(sg.get('finalise_s'), 2)} / {fmt(sn.get('finalise_s'), 2)} |")

    md("\n## Memory per arm (peak)\n")
    md("Resident is what must fit in RAM at once; committed also counts memory Windows can page out.\n")
    md("| Arm | Pass | Engine private (GB) | Note host private (GB) | ClinicAVT resident (GB) "
       "| ClinicAVT committed (GB) | iGPU shared (GB) "
       "| Lowest free RAM (GB) | Highest commit (GB) | Commit above idle system (GB) |")
    md("|---|---|---|---|---|---|---|---|---|---|")
    idle_commit = mean([s["commit_gb_max"] for s in idle])
    for arm, pass_no in arms:
        cs = [c for c in consults if c["window"].get("arm") == arm and c["window"].get("pass_no") == pass_no
              and c.get("stop")]
        if not cs:
            continue
        a, b = min(c["window"]["t0"] for c in cs), max(c["window"]["t1"] for c in cs)
        ps = proc_stats(procs, a, b)
        eng = (ps.get("engine") or {}).get("private_mb_max")
        host = (ps.get("note_host") or {}).get("private_mb_max")
        ph = [p for c in cs for p in c["phases"].values() if p]
        peak_commit = max([p["commit_gb_max"] or 0 for p in ph], default=0)
        res = (ps.get("all") or {}).get("ws_mb_max")
        com = (ps.get("all") or {}).get("private_mb_max")
        md(f"| {arm} | {pass_no} | {fmt(eng and eng / 1024, 2)} | {fmt(host and host / 1024, 2)} | "
           f"{fmt(res and res / 1024, 2)} | {fmt(com and com / 1024, 2)} | {fmt(max([p['igpu_shared_gb_max'] or 0 for p in ph], default=None), 2)} "
           f"| {fmt(min([p['avail_mb_min'] for p in ph if p['avail_mb_min'] is not None], default=0) / 1024, 2)} "
           f"| {fmt(max([p['commit_gb_max'] or 0 for p in ph], default=None), 1)} "
           f"| {fmt(peak_commit - idle_commit if idle_commit else None, 1)} |")
        summary["arms"].setdefault(f"{arm}#{pass_no}", {})["memory"] = {
            "engine_mb": eng, "note_host_mb": host, "resident_mb": res, "committed_mb": com,
            "commit_above_idle_gb": peak_commit - idle_commit if idle_commit else None}
    md("\nCommit above the idle system is what ClinicAVT itself needs, the figure to size a smaller "
       "device by: add the operating system's own baseline on that device.")

    md("\n## Process CPU share (mean, % of all cores)\n")
    md("| Arm | Pass | Engine during capture | Note host during capture | Engine after Stop | Note host after Stop |")
    md("|---|---|---|---|---|---|")
    for arm, pass_no in arms:
        cs = [c for c in consults if c["window"].get("arm") == arm and c["window"].get("pass_no") == pass_no
              and c.get("procs")]
        get = lambda phase, name: mean([(c["procs"][phase].get(name) or {}).get("cpu_pct") for c in cs])  # noqa: E731
        md(f"| {arm} | {pass_no} | {fmt(get('capture', 'engine'))} | {fmt(get('capture', 'note_host'))} | "
           f"{fmt(get('after_stop', 'engine'))} | {fmt(get('after_stop', 'note_host'))} |")

    checks = [p for p in procs if "network" in p and p["procs"]]
    seen_tcp = sorted({c for p in checks for c in (p["network"].get("tcp") or [])})
    seen_udp = sorted({c for p in checks for c in (p["network"].get("udp") or [])})
    md("\n## Network (NFR-6)\n")
    md(f"Every 30 s while ClinicAVT ran, {len(checks)} checks in all, the TCP connections and UDP endpoints "
       f"owned by its processes were listed: {len(seen_tcp)} TCP and {len(seen_udp)} UDP."
       + (" " + "; ".join(seen_tcp + seen_udp) if seen_tcp or seen_udp else ""))
    summary["network"] = {"checks": len(checks), "tcp": seen_tcp, "udp": seen_udp}

    md("\n## Battery\n")
    full_wh = float(machine.get("battery_full_mwh") or 0) / 1000
    # On mains Windows can report discharging with an invalid rate, so only off mains and real rates count
    discharging = [r for r in procs if (r.get("battery") or {}).get("Discharging")
                   and not r["battery"].get("PowerOnline") and 0 < (r["battery"].get("DischargeRate") or 0) < 10 ** 6]
    if discharging:
        rate = mean([r["battery"].get("DischargeRate") for r in discharging])
        md(f"On battery: whole-system drain averaged {fmt(rate / 1000, 1)} W over {len(discharging)} readings, "
           f"which would empty a {fmt(full_wh)} Wh battery in {fmt(full_wh / (rate / 1000), 1)} h.")
    else:
        md("Run on mains, so whole-system drain was not measured; the estimate below adds only what the "
           "processor package draws above its idle, on top of whatever the laptop draws anyway.")
    md(f"\nA day of {SHIFT_CONSULTS} consultations of {SHIFT_AUDIO_MIN:.0f} minutes over {SHIFT_HOURS:.0f} hours: "
       "recording at the measured capture power, the work after Stop once each, and the loaded models "
       "between consultations.\n")
    md("| Arm | Recording (Wh) | After Stop (Wh) | Between consultations (Wh) | Day (Wh) | Share of battery |")
    md("|---|---|---|---|---|---|")
    for arm, pass_no in arms:
        e = energy.get((arm, pass_no)) or {}
        if e.get("capture_above_w") is None or not full_wh:
            continue
        rec_wh = e["capture_above_w"] * SHIFT_AUDIO_MIN * 60 * SHIFT_CONSULTS / 3600
        post_wh = (e.get("after_stop_above_wh") or 0) * SHIFT_CONSULTS
        gap_h = max(0.0, SHIFT_HOURS - SHIFT_CONSULTS * SHIFT_AUDIO_MIN / 60)
        idle_wh = max(0.0, e.get("loaded_idle_above_w") or 0) * gap_h
        day = rec_wh + post_wh + idle_wh
        e["day_wh"] = day
        md(f"| {arm}{'' if pass_no == 1 else f' ({pass_no})'} | {fmt(rec_wh, 1)} | {fmt(post_wh, 1)} | "
           f"{fmt(idle_wh, 1)} | {fmt(day, 1)} | {fmt(100 * day / full_wh, 0)}% |")

    drift = [k for k in summary["arms"] if k.startswith("9b-gpu#")]
    if len(drift) > 1:
        a, b = summary["arms"].get("9b-gpu#1", {}), summary["arms"].get("9b-gpu#2", {})
        md(f"\n## Drift\n\nThe default arm again at the end: stop to first words {fmt(a.get('stop_to_first_words_s'), 2)} "
           f"then {fmt(b.get('stop_to_first_words_s'), 2)} s; note done {fmt(a.get('note_done_s'))} then "
           f"{fmt(b.get('note_done_s'))} s.")

    summary["accuracy"] = accuracy(run, md)
    bandwidth(md, summary)
    switches(run, md)
    min_specs(md, summary)
    figures(run, md, consults, per_window, kinds, summary, arms, energy)
    (run / "report.md").write_text("\n".join(out) + "\n", encoding="utf-8")
    (run / "summary.json").write_text(json.dumps(summary, indent=1, default=str), encoding="utf-8")
    print(run / "report.md")


if __name__ == "__main__":
    main()
