"""Note model latency on the iGPU: cold and warm load, time to first token, decode rate, time to a
500-token note against the 90 s bar, peak RAM and decode bandwidth efficiency.

Inputs are the 25th-percentile, median and longest PriMock consultations by words, under
hpi-prompt.txt. Decode runs a fixed 500 tokens with EOS ignored so rates compare; one extra
generation per input keeps the real note. Rows append to build/evaluation/performance/latency.jsonl.

    python evaluation/performance/latency.py --prepare-only
    python evaluation/performance/latency.py qwen3.5-4b-int4 qwen3.5-9b-int4 [--reps 3]
"""
import argparse
import gc
import json
import os
import statistics
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import config, primock  # noqa: E402

SYSTEM = (Path(__file__).parent / "hpi-prompt.txt").read_text(encoding="utf-8").strip()
ARC_BW_GBS = 134.0  # Arc 140T peak memory bandwidth, the denominator of bandwidth efficiency
NOTE_BAR_S = 90.0   # time to note after a 15-minute consultation


def utterances(consult):
    # Raw interval text with its markup, so the inputs match earlier runs
    out = []
    for speaker in primock.SPEAKERS:
        for start, _, text in primock.intervals(primock.textgrid(consult, speaker)):
            text = text.strip()
            if text and text.lower() != "sil":
                out.append((start, speaker.capitalize(), text))
    return out


def select_inputs():
    words = {c: sum(len(t.split()) for _, _, t in utterances(c)) for c in primock.consults()}
    ranked = sorted(words.items(), key=lambda kv: kv[1])
    n = len(ranked)
    picks = {"short_p25": ranked[round(0.25 * (n - 1))], "median_p50": ranked[round(0.50 * (n - 1))],
             "worst_max": ranked[-1]}
    inputs = {}
    for point, (consult, count) in picks.items():
        turns = sorted(utterances(consult), key=lambda u: u[0])
        body = "\n".join(f"{role}: {text}" for _, role, text in turns)
        inputs[point] = {"consult": consult, "words": count, "body": body}
    return inputs


class PeakRss(threading.Thread):
    def __init__(self, proc, interval=0.1):
        super().__init__(daemon=True)
        self.proc, self.interval, self.peak, self.running = proc, interval, 0, True

    def run(self):
        while self.running:
            self.peak = max(self.peak, self.proc.memory_info().rss)
            time.sleep(self.interval)


def run_model(name, inputs, device, reps, max_new, out):
    import psutil
    from common.llm import NoteModel
    spec = config.model(name)
    weight_gb = round(sum(f.stat().st_size for f in spec["path"].rglob("*") if f.is_file()) / 1e9, 2)
    cold = NoteModel(name, device)
    cold_load_s = cold.load_s
    del cold
    gc.collect()
    model = NoteModel(name, device)
    print(f"\n=== {name} [{weight_gb} GB] {device}: load cold {cold_load_s:.1f} s, warm {model.load_s:.1f} s")
    proc = psutil.Process()
    for point, info in inputs.items():
        note, note_metrics = model.generate(info["body"], max_new, system=SYSTEM)
        runs, peak = [], 0
        for _ in range(reps):
            sampler = PeakRss(proc)
            sampler.start()
            _, m = model.generate(info["body"], max_new, system=SYSTEM, ignore_eos=True)
            sampler.running = False
            sampler.join()
            runs.append(m)
            peak = max(peak, sampler.peak)
        med = {k: statistics.median(r[k] for r in runs) for k in ("ttft_s", "decode_tok_s", "tpot_ms", "generate_s")}
        row = {"model": name, "weight_gb": weight_gb, "device": device, "point": point,
               "consult": info["consult"], "input_tokens": runs[0]["input_tokens"],
               "output_tokens": runs[0]["output_tokens"], "cold_load_s": round(cold_load_s, 2),
               "warm_load_s": round(model.load_s, 2), "ttft_s": round(med["ttft_s"], 3),
               "decode_tok_s": round(med["decode_tok_s"], 2), "tpot_ms": round(med["tpot_ms"], 2),
               "time_to_note_s": round(med["generate_s"], 2), "within_bar": med["generate_s"] < NOTE_BAR_S,
               "peak_ram_gb": round(peak / 1e9, 2), "reps": reps,
               "note_tokens": note_metrics["output_tokens"], "note": note}
        if spec.get("params_b"):
            active_gb = weight_gb * spec.get("active_b", spec["params_b"]) / spec["params_b"]
            row["bw_eff"] = round(row["decode_tok_s"] * active_gb / ARC_BW_GBS, 3)
        out.write(json.dumps(row) + "\n")
        out.flush()
        print(f"  {point:11s} in={row['input_tokens']:5d}  first token {row['ttft_s']:5.2f} s  "
              f"decode {row['decode_tok_s']:5.1f} tok/s  note {row['time_to_note_s']:5.1f} s  "
              f"RAM {row['peak_ram_gb']:.1f} GB")
    del model
    gc.collect()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("models", nargs="*", help="model names from evaluation/config.toml")
    ap.add_argument("--device", default="GPU.0")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--max-new", type=int, default=500)
    ap.add_argument("--prepare-only", action="store_true", help="print the three inputs and stop")
    args = ap.parse_args()

    inputs = select_inputs()
    for point, info in inputs.items():
        print(f"{point:11s} {info['consult']:24s} {info['words']:5d} words")
    if args.prepare_only or not args.models:
        return
    path = config.out("performance") / "latency.jsonl"
    with open(path, "a", encoding="utf-8") as out:
        for name in args.models:
            try:
                run_model(name, inputs, args.device, args.reps, args.max_new, out)
            except Exception as e:  # one failed model must not end the sweep
                out.write(json.dumps({"model": name, "error": repr(e)}) + "\n")
                print(f"  {name} FAILED: {e!r}")
                gc.collect()
    print(f"-> {path}")


if __name__ == "__main__":
    main()
